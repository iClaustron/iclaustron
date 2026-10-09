// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The dictionary cache (`legacy-c/api/ic_apid_table.ic`): the table
//! and index descriptions every thread of the process shares.
//!
//! **One object per table version, shared.** Binding a table gives a
//! [`TableDef`] behind an `Arc`. The count the `Arc` keeps is the C's
//! reference count: an object the cache has let go of lives on for as
//! long as a thread still holds it, which also does the work of the C's
//! list of old objects kept for clean-up.
//!
//! **One fetch per name.** The first thread to ask for a name marks it
//! as being fetched and asks the dictionary without holding the lock.
//! Threads that ask for it meanwhile wait for that fetch rather than
//! sending one of their own, as the C does.
//!
//! **A connection keeps what it has bound**, so binding the same table
//! again takes no lock: it only checks that the object is still valid.
//!
//! An object stops being valid, and is let go of by the cache, when:
//!
//! 1. **The dictionary says the table changed.** Every data node sends
//!    `TABLE_CHANGED_REP` when a table is altered, and the master sends it
//!    when one is dropped, naming the table and the version that no
//!    longer holds. The receive thread executes it here. An alteration
//!    arrives once per data node, and only the first finds anything. A
//!    notice that arrives while the name is being fetched is kept, and
//!    the fetch is repeated if it brought back the version named.
//! 2. **No data node is connected.** Everything goes, as in the
//!    reference: after a cluster restart the same ids may name other
//!    tables. Fetches under way are left to finish.
//! 3. **An operation failed because the table changed.** The caller says
//!    so, through the connection.
//!
//! A thread holding an object that is no longer valid may go on using
//! it. After an online change, operations with the old version still
//! work; after an offline change or a drop, the data nodes refuse them.
//!
//! **An index belongs to the version of its table it was bound for, and
//! holds it.** That version lives for as long as the index does, since
//! an operation through the index reads that version's columns. When
//! the cache lets go of a table version it lets go of the indexes bound
//! for it too, so the cache itself never keeps an old version alive;
//! whoever holds such an index keeps both. Its internal name is
//! `sys/def/<table id>/<index>`, or `<database>/def/<table id>/<index>`
//! for one made by an older version. Binding it for another version of
//! the table fetches it again.
//!
//! **Hash maps are shared by id and version.** A hash map never
//! changes; a table that is reorganised names a new one.
//!
//! The lock is the dictionary level, the lowest. It is never held while
//! asking the dictionary. The receive thread takes it for a notice and
//! when the last link goes, holding nothing else.
//!
//! Verify: chapter 05 §17.13.

use std::collections::BTreeMap;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_port::err;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_DICT;
use ic_port::IcError;
use ic_signals::table_props;
use ic_signals::table_props::ColumnProps;
use ic_signals::table_props::HashMapProps;
use ic_signals::table_props::TableProps;

use crate::apid_conn::ApidConnection;
use crate::dict_client;

/// How many times a bind fetches again because what it fetched was
/// already out of date, before it gives up.
pub const IC_DICT_REFETCHES: u32 = 3;

/// A table's description, shared by every thread that binds it.
pub struct TableDef {
  info: TableProps,
  hash_map: Option<Arc<HashMapProps>>,
  valid: AtomicBool,
  /// By column id, the column's place in `info.attributes`, or
  /// `u32::MAX`: a column is found by index, not by a search, on paths
  /// taken for every column of every row.
  attr_index: Vec<u32>,
  /// Whether any column is a blob, asked on every delete.
  has_blob: bool,
}

/// The place of each column in `attributes`, by column id.
fn index_attributes(info: &TableProps) -> Vec<u32> {
  let mut index: Vec<u32> = Vec::new();
  for (place, attr) in info.attributes.iter().enumerate() {
    let id = attr.column_id as usize;
    if id >= index.len() {
      index.resize(id + 1, u32::MAX);
    }
    index[id] = place as u32;
  }
  index
}

impl std::fmt::Debug for TableDef {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "TableDef({}, id {}, version {:#x}{})",
      self.info.name,
      self.info.table_id,
      self.info.table_version,
      if self.is_valid() { "" } else { ", invalid" }
    )
  }
}

impl TableDef {
  pub(crate) fn new(
    info: TableProps,
    hash_map: Option<Arc<HashMapProps>>,
  ) -> TableDef {
    let attr_index = index_attributes(&info);
    let has_blob = info.attributes.iter().any(|attr| attr.is_blob());
    TableDef {
      info,
      hash_map,
      valid: AtomicBool::new(true),
      attr_index,
      has_blob,
    }
  }

  /// Everything the dictionary said about the table.
  pub fn info(&self) -> &TableProps {
    &self.info
  }

  /// The internal name, `database/def/table`.
  pub fn name(&self) -> &str {
    &self.info.name
  }

  /// The id every operation on the table names.
  pub fn table_id(&self) -> u32 {
    self.info.table_id
  }

  /// The version every operation on the table names.
  pub fn table_version(&self) -> u32 {
    self.info.table_version
  }

  /// How many fields the table has.
  pub fn num_fields(&self) -> u32 {
    self.info.attributes.len() as u32
  }

  /// A field's id, by its name (`ic_table_def_get_field_id`). The id is
  /// the attribute id signals name the column by.
  pub fn field_id(&self, name: &str) -> Result<u32, IcError> {
    match self.info.attribute(name) {
      Some(attr) => Ok(attr.column_id),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    }
  }

  /// A field, by its id.
  pub fn field(&self, field_id: u32) -> Option<&ColumnProps> {
    let place = *self.attr_index.get(field_id as usize)?;
    self.info.attributes.get(place as usize)
  }

  /// True if any column is a blob (BLOB, TEXT, and a MySQL server's
  /// JSON and GEOMETRY). Rows of such a table are not deleted until
  /// blobs are supported: the blob's parts would be left behind.
  pub fn has_blob_columns(&self) -> bool {
    self.has_blob
  }

  /// Which fragment each key hash goes to, if the table is placed by a
  /// hash map.
  pub fn hash_map(&self) -> Option<&HashMapProps> {
    self.hash_map.as_deref()
  }

  /// False once the cache has let go of this object; see the module
  /// note. Binding the table again gives the current one.
  pub fn is_valid(&self) -> bool {
    self.valid.load(Ordering::Acquire)
  }

  fn invalidate(&self) {
    self.valid.store(false, Ordering::Release);
  }
}

/// An index's description, for one version of its table, which it
/// holds.
pub struct IndexDef {
  /// The index's own table, as the data nodes keep it: its key is the
  /// indexed columns in the index's order, and its own replicas and
  /// hash map place its rows.
  own: Arc<TableDef>,
  table: Arc<TableDef>,
  valid: AtomicBool,
}

impl std::fmt::Debug for IndexDef {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "IndexDef({}, id {}, for table {} version {:#x}{})",
      self.own.name(),
      self.own.table_id(),
      self.table.table_id(),
      self.table.table_version(),
      if self.is_valid() { "" } else { ", invalid" }
    )
  }
}

impl IndexDef {
  /// An index over one version of its table, as the dictionary
  /// described it, with the hash map that places its own rows.
  pub(crate) fn new(
    info: TableProps,
    table: Arc<TableDef>,
    hash_map: Option<Arc<HashMapProps>>,
  ) -> IndexDef {
    IndexDef {
      own: Arc::new(TableDef::new(info, hash_map)),
      table,
      valid: AtomicBool::new(true),
    }
  }

  /// Everything the dictionary said about the index, which it describes
  /// as a table of its own.
  pub fn info(&self) -> &TableProps {
    self.own.info()
  }

  /// The index's own table, which a transaction whose first operation
  /// goes through the index is placed by (see
  /// [`ApidConnection::hint_for_unique_key`]).
  pub fn as_table(&self) -> &Arc<TableDef> {
    &self.own
  }

  /// The internal name, `sys/def/<table id>/<index>`.
  pub fn name(&self) -> &str {
    self.own.name()
  }

  /// The index's own id.
  pub fn index_id(&self) -> u32 {
    self.own.table_id()
  }

  /// The index's own version.
  pub fn index_version(&self) -> u32 {
    self.own.table_version()
  }

  /// The table it indexes, at the version it was bound for, whether or
  /// not a newer one has come since.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// The table it indexes.
  pub fn table_id(&self) -> u32 {
    self.table.table_id()
  }

  /// The version of the table it was bound for.
  pub fn table_version(&self) -> u32 {
    self.table.table_version()
  }

  /// A unique hash index, which a key operation can go through.
  pub fn is_unique(&self) -> bool {
    self.info().table_kind == table_props::IC_OBJECT_KIND_UNIQUE_INDEX
  }

  /// An ordered index, for range scans.
  pub fn is_ordered(&self) -> bool {
    self.info().table_kind == table_props::IC_OBJECT_KIND_ORDERED_INDEX
  }

  /// False once the cache has let go of this object.
  pub fn is_valid(&self) -> bool {
    self.valid.load(Ordering::Acquire)
  }

  fn invalidate(&self) {
    self.valid.store(false, Ordering::Release);
  }
}

/// What the cache holds under one internal name.
enum Entry {
  /// One thread is asking the dictionary, and the others wait. Holds
  /// the table id and version of every notice that arrived meanwhile.
  Fetching(Vec<(u32, u32)>),
  Table(Arc<TableDef>),
  Index(Arc<IndexDef>),
}

impl Entry {
  /// The id and version a notice would name, if there is an object.
  fn id_and_version(&self) -> Option<(u32, u32)> {
    match self {
      Entry::Fetching(_) => None,
      Entry::Table(def) => Some((def.table_id(), def.table_version())),
      Entry::Index(def) => Some((def.index_id(), def.index_version())),
    }
  }

  fn invalidate(&self) {
    match self {
      Entry::Fetching(_) => {}
      Entry::Table(def) => def.invalidate(),
      Entry::Index(def) => def.invalidate(),
    }
  }
}

#[derive(Default)]
struct CacheState {
  entries: BTreeMap<String, Entry>,
  hash_maps: Vec<Arc<HashMapProps>>,
}

/// What a thread found when it looked a name up.
enum Lookup {
  Table(Arc<TableDef>),
  Index(Arc<IndexDef>),
  /// Nothing there. The name is now marked as being fetched by this
  /// thread, which must end with `fetched` or `fetch_failed`.
  Fetch,
}

/// The cache every thread shares. One per process, in the shared state.
pub(crate) struct DictCache {
  state: IcMutex<CacheState>,
  cond: IcCond,
}

impl DictCache {
  pub(crate) fn new() -> DictCache {
    DictCache {
      state: IcMutex::new(IC_MUTEX_LEVEL_DICT, CacheState::default()),
      cond: IcCond::new(),
    }
  }

  /// Look a name up, waiting while another thread fetches it.
  fn lookup(&self, name: &str) -> Lookup {
    let mut state = self.state.lock();
    loop {
      match state.entries.get(name) {
        None => break,
        Some(Entry::Table(def)) => return Lookup::Table(Arc::clone(def)),
        Some(Entry::Index(def)) => return Lookup::Index(Arc::clone(def)),
        Some(Entry::Fetching(_)) => {}
      }
      state = self.cond.wait(state);
    }
    state
      .entries
      .insert(name.to_string(), Entry::Fetching(Vec::new()));
    Lookup::Fetch
  }

  /// Put what was fetched in place of the mark, unless a notice that
  /// arrived meanwhile names its version. Then the mark goes, nothing is
  /// kept, and the caller fetches again: false.
  fn fetched(&self, name: &str, entry: Entry) -> bool {
    let mut state = self.state.lock();
    let mut out_of_date = false;
    if let Some(Entry::Fetching(noticed)) = state.entries.get(name) {
      if let Some(id_version) = entry.id_and_version() {
        out_of_date = noticed.contains(&id_version);
      }
    }
    if out_of_date {
      state.entries.remove(name);
    } else {
      state.entries.insert(name.to_string(), entry);
    }
    drop(state);
    self.cond.broadcast();
    !out_of_date
  }

  /// Take the mark away after a failed fetch. A thread that was waiting
  /// then fetches for itself.
  fn fetch_failed(&self, name: &str) {
    let mut state = self.state.lock();
    if let Some(Entry::Fetching(_)) = state.entries.get(name) {
      state.entries.remove(name);
    }
    drop(state);
    self.cond.broadcast();
  }

  fn hash_map(
    &self,
    object_id: u32,
    version: u32,
  ) -> Option<Arc<HashMapProps>> {
    let state = self.state.lock();
    for map in &state.hash_maps {
      if map.object_id == object_id && map.version == version {
        return Some(Arc::clone(map));
      }
    }
    None
  }

  /// Keep a hash map, or the one another thread kept first.
  fn add_hash_map(&self, map: HashMapProps) -> Arc<HashMapProps> {
    let mut state = self.state.lock();
    for kept in &state.hash_maps {
      if kept.object_id == map.object_id && kept.version == map.version {
        return Arc::clone(kept);
      }
    }
    let map = Arc::new(map);
    state.hash_maps.push(Arc::clone(&map));
    map
  }

  /// A notice from the dictionary: the object of that name, id and
  /// version has been altered or dropped. Returns how many cached
  /// objects were let go of: the object, and for a table the indexes
  /// bound for that version of it.
  pub(crate) fn table_changed(
    &self,
    name: &str,
    table_id: u32,
    table_version: u32,
  ) -> usize {
    let mut state = self.state.lock();
    let id_version = (table_id, table_version);
    let named = match state.entries.get_mut(name) {
      None => false,
      Some(Entry::Fetching(noticed)) => {
        if !noticed.contains(&id_version) {
          noticed.push(id_version);
        }
        false
      }
      Some(entry) => entry.id_and_version() == Some(id_version),
    };
    let mut dropped: usize = 0;
    if named {
      if let Some(entry) = state.entries.remove(name) {
        entry.invalidate();
        dropped += 1;
      }
    }
    // Even if the table itself had already gone, its indexes may not.
    dropped + let_go_of_indexes(&mut state, table_id, table_version)
  }

  /// Let go of everything but what is being fetched. Returns how many
  /// objects that was.
  pub(crate) fn invalidate_all(&self) -> usize {
    let mut state = self.state.lock();
    let mut names: Vec<String> = Vec::new();
    for (name, entry) in &state.entries {
      if entry.id_and_version().is_some() {
        names.push(name.clone());
      }
    }
    for name in &names {
      if let Some(entry) = state.entries.remove(name) {
        entry.invalidate();
      }
    }
    state.hash_maps.clear();
    names.len()
  }

  /// Let go of a table an operation found out of date, and of the
  /// indexes bound for that version of it.
  pub(crate) fn forget_table(&self, table: &TableDef) {
    table.invalidate();
    let id_version = (table.table_id(), table.table_version());
    let mut state = self.state.lock();
    let named = match state.entries.get(table.name()) {
      Some(Entry::Table(def)) => {
        (def.table_id(), def.table_version()) == id_version
      }
      _ => false,
    };
    if named {
      state.entries.remove(table.name());
    }
    let_go_of_indexes(&mut state, table.table_id(), table.table_version());
  }

  /// Let go of an index that no longer fits its table.
  pub(crate) fn forget_index(&self, index: &IndexDef) {
    index.invalidate();
    let mut state = self.state.lock();
    let named = match state.entries.get(index.name()) {
      Some(Entry::Index(def)) => {
        def.index_id() == index.index_id()
          && def.table_version() == index.table_version()
      }
      _ => false,
    };
    if named {
      state.entries.remove(index.name());
    }
  }
}

/// Let go of the indexes bound for one version of a table, so that the
/// cache does not keep that version alive through them. Returns how
/// many there were.
fn let_go_of_indexes(
  state: &mut CacheState,
  table_id: u32,
  table_version: u32,
) -> usize {
  let mut names: Vec<String> = Vec::new();
  for (name, entry) in &state.entries {
    if let Entry::Index(def) = entry {
      if def.table_id() == table_id && def.table_version() == table_version {
        names.push(name.clone());
      }
    }
  }
  for name in &names {
    if let Some(entry) = state.entries.remove(name) {
      entry.invalidate();
    }
  }
  names.len()
}

/// The internal name of an index, as the reference makes it now.
pub fn index_name(table_id: u32, index: &str) -> String {
  format!("sys/def/{}/{}", table_id, index)
}

/// The internal name of an index made by an older version.
pub fn old_index_name(database: &str, table_id: u32, index: &str) -> String {
  format!(
    "{}/{}/{}/{}",
    database,
    dict_client::IC_DICT_SCHEMA,
    table_id,
    index
  )
}

/// Bind a table by its internal name: from the cache, or from the
/// dictionary, with its hash map.
pub(crate) fn bind_table(
  cache: &DictCache,
  conn: &mut ApidConnection,
  name: &str,
) -> Result<Arc<TableDef>, IcError> {
  let mut refetches: u32 = 0;
  loop {
    match cache.lookup(name) {
      Lookup::Table(def) => return Ok(def),
      // Only an index has an index's name.
      Lookup::Index(_) => {
        return Err(IcError::new(err::IC_ERROR_NO_SUCH_TABLE))
      }
      Lookup::Fetch => {}
    }
    let def = match fetch_table(cache, conn, name) {
      Ok(def) => Arc::new(def),
      Err(e) => {
        cache.fetch_failed(name);
        return Err(e);
      }
    };
    if cache.fetched(name, Entry::Table(Arc::clone(&def))) {
      return Ok(def);
    }
    refetches += 1;
    if refetches >= IC_DICT_REFETCHES {
      return Err(IcError::new(err::IC_ERROR_TABLE_KEEPS_CHANGING));
    }
  }
}

fn fetch_table(
  cache: &DictCache,
  conn: &mut ApidConnection,
  name: &str,
) -> Result<TableDef, IcError> {
  let info = dict_client::get_table_named(conn, name)?;
  let hash_map = hash_map_of(cache, conn, &info)?;
  Ok(TableDef::new(info, hash_map))
}

/// The hash map a table or index is placed by, from the cache or the
/// dictionary, if it has one.
fn hash_map_of(
  cache: &DictCache,
  conn: &mut ApidConnection,
  info: &TableProps,
) -> Result<Option<Arc<HashMapProps>>, IcError> {
  if info.hash_map_id == table_props::IC_NULL_PTR {
    return Ok(None);
  }
  let id = info.hash_map_id;
  if let Some(map) = cache.hash_map(id, info.hash_map_version) {
    return Ok(Some(map));
  }
  let map = dict_client::get_hash_map(conn, id)?;
  Ok(Some(cache.add_hash_map(map)))
}

/// Bind an index of a table already bound: by its current name, or by
/// the older form if there is none such.
pub(crate) fn bind_index(
  cache: &DictCache,
  conn: &mut ApidConnection,
  database: &str,
  index: &str,
  table: &Arc<TableDef>,
) -> Result<Arc<IndexDef>, IcError> {
  let name = index_name(table.table_id(), index);
  match bind_index_named(cache, conn, &name, table) {
    Err(e) if e.code == err::IC_ERROR_NO_SUCH_TABLE => {}
    other => return other,
  }
  let old = old_index_name(database, table.table_id(), index);
  match bind_index_named(cache, conn, &old, table) {
    Err(e) if e.code == err::IC_ERROR_NO_SUCH_TABLE => {
      Err(IcError::new(err::IC_ERROR_NO_SUCH_INDEX))
    }
    other => other,
  }
}

fn bind_index_named(
  cache: &DictCache,
  conn: &mut ApidConnection,
  name: &str,
  table: &Arc<TableDef>,
) -> Result<Arc<IndexDef>, IcError> {
  let mut refetches: u32 = 0;
  loop {
    match cache.lookup(name) {
      Lookup::Index(def) => {
        if def.table_id() == table.table_id()
          && def.table_version() == table.table_version()
        {
          return Ok(def);
        }
        // Bound for another version of the table. It may be one a
        // thread still holding the older table needs, which will then
        // fetch it again in turn; the count stops that going on.
        cache.forget_index(&def);
      }
      Lookup::Table(_) => {
        return Err(IcError::new(err::IC_ERROR_NO_SUCH_TABLE))
      }
      Lookup::Fetch => {
        let def = match fetch_index(cache, conn, name, table) {
          Ok(def) => Arc::new(def),
          Err(e) => {
            cache.fetch_failed(name);
            return Err(e);
          }
        };
        if cache.fetched(name, Entry::Index(Arc::clone(&def))) {
          return Ok(def);
        }
      }
    }
    refetches += 1;
    if refetches >= IC_DICT_REFETCHES {
      return Err(IcError::new(err::IC_ERROR_TABLE_KEEPS_CHANGING));
    }
  }
}

fn fetch_index(
  cache: &DictCache,
  conn: &mut ApidConnection,
  name: &str,
  table: &Arc<TableDef>,
) -> Result<IndexDef, IcError> {
  let info = dict_client::get_table_named(conn, name)?;
  let is_index = info.table_kind == table_props::IC_OBJECT_KIND_UNIQUE_INDEX
    || info.table_kind == table_props::IC_OBJECT_KIND_ORDERED_INDEX;
  if !is_index || info.base_table_id != table.table_id() {
    return Err(IcError::new(err::IC_ERROR_NO_SUCH_TABLE));
  }
  index_is_online(&info)?;
  // The index's own replicas and hash map, which it inherits from its
  // table or has of its own, place a transaction that goes through it.
  let hash_map = hash_map_of(cache, conn, &info)?;
  Ok(IndexDef::new(info, Arc::clone(table), hash_map))
}

/// Refuse an index that is not online. The coordinator does not check,
/// and a lookup through an index still being built finds nothing. A
/// description that does not say, the field left at all ones, is taken
/// as online, as the reference takes every index.
fn index_is_online(info: &TableProps) -> Result<(), IcError> {
  let state = info.index_state;
  if state != !0 && state != table_props::IC_INDEX_STATE_ONLINE {
    return Err(IcError::new(err::IC_ERROR_INDEX_NOT_ONLINE));
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  fn table(name: &str, id: u32, version: u32) -> TableDef {
    let mut info = test_info();
    info.name = name.to_string();
    info.table_id = id;
    info.table_version = version;
    TableDef::new(info, None)
  }

  /// A description with every field at its default.
  fn test_info() -> TableProps {
    let mut w = ic_signals::prop_stream::PropertyWriter::new();
    w.add_string(table_props::IC_TP_TABLE_NAME, "x/def/x");
    table_props::parse_table_info(w.words()).expect("parsed")
  }

  /// Fill a name as a fetching thread would.
  fn put(cache: &DictCache, def: TableDef) -> Arc<TableDef> {
    let name = def.name().to_string();
    assert!(matches!(cache.lookup(&name), Lookup::Fetch));
    let def = Arc::new(def);
    assert!(cache.fetched(&name, Entry::Table(Arc::clone(&def))));
    def
  }

  #[test]
  fn only_an_online_index_is_used() {
    let mut info = test_info();
    assert!(index_is_online(&info).is_ok(), "a state not given");
    info.index_state = table_props::IC_INDEX_STATE_ONLINE;
    assert!(index_is_online(&info).is_ok());
    for building_or_dropping in [1, 2, 3, 9] {
      info.index_state = building_or_dropping;
      assert_eq!(
        index_is_online(&info).expect_err("refused").code,
        err::IC_ERROR_INDEX_NOT_ONLINE
      );
    }
  }

  #[test]
  fn a_table_without_blob_columns_says_so() {
    assert!(!table("x/def/x", 1, 1).has_blob_columns());
  }

  #[test]
  fn a_fetched_table_is_found_again() {
    let cache = DictCache::new();
    let def = put(&cache, table("db/def/t1", 13, 1));
    match cache.lookup("db/def/t1") {
      Lookup::Table(found) => assert!(Arc::ptr_eq(&found, &def)),
      _ => panic!("expected the table"),
    }
  }

  #[test]
  fn a_notice_for_its_version_lets_it_go() {
    let cache = DictCache::new();
    let def = put(&cache, table("db/def/t1", 13, 1));
    // The same notice from a second data node finds nothing more.
    assert_eq!(cache.table_changed("db/def/t1", 13, 1), 1);
    assert_eq!(cache.table_changed("db/def/t1", 13, 1), 0);
    assert!(!def.is_valid());
    assert!(matches!(cache.lookup("db/def/t1"), Lookup::Fetch));
  }

  #[test]
  fn a_notice_for_another_version_or_id_is_passed_over() {
    let cache = DictCache::new();
    let def = put(&cache, table("db/def/t1", 13, 2));
    assert_eq!(cache.table_changed("db/def/t1", 13, 1), 0);
    assert_eq!(cache.table_changed("db/def/t1", 14, 2), 0);
    assert!(def.is_valid());
  }

  #[test]
  fn a_notice_during_a_fetch_of_that_version_means_fetch_again() {
    let cache = DictCache::new();
    assert!(matches!(cache.lookup("db/def/t1"), Lookup::Fetch));
    assert_eq!(cache.table_changed("db/def/t1", 13, 1), 0);
    // The fetch brought back the version the notice named.
    let old = Arc::new(table("db/def/t1", 13, 1));
    assert!(!cache.fetched("db/def/t1", Entry::Table(old)));
    // So nothing is kept, and the next to ask fetches.
    assert!(matches!(cache.lookup("db/def/t1"), Lookup::Fetch));
    // This time it is the new version, which no notice named.
    let new = Arc::new(table("db/def/t1", 13, 2));
    assert!(cache.fetched("db/def/t1", Entry::Table(new)));
  }

  #[test]
  fn losing_every_link_lets_go_of_everything_but_fetches() {
    let cache = DictCache::new();
    let t1 = put(&cache, table("db/def/t1", 13, 1));
    let t2 = put(&cache, table("db/def/t2", 14, 1));
    assert!(matches!(cache.lookup("db/def/t3"), Lookup::Fetch));
    assert_eq!(cache.invalidate_all(), 2);
    assert!(!t1.is_valid() && !t2.is_valid());
    // The fetch under way can still finish.
    let t3 = Arc::new(table("db/def/t3", 15, 1));
    assert!(cache.fetched("db/def/t3", Entry::Table(t3)));
  }

  #[test]
  fn a_failed_fetch_leaves_the_name_free() {
    let cache = DictCache::new();
    assert!(matches!(cache.lookup("db/def/t1"), Lookup::Fetch));
    cache.fetch_failed("db/def/t1");
    assert!(matches!(cache.lookup("db/def/t1"), Lookup::Fetch));
  }

  #[test]
  fn forgetting_an_old_table_leaves_the_new_one() {
    let cache = DictCache::new();
    let old = table("db/def/t1", 13, 1);
    let new = put(&cache, table("db/def/t1", 13, 2));
    cache.forget_table(&old);
    assert!(!old.is_valid());
    assert!(new.is_valid());
    assert!(matches!(cache.lookup("db/def/t1"), Lookup::Table(_)));
  }

  fn index(name: &str, id: u32, table: &Arc<TableDef>) -> IndexDef {
    let mut info = test_info();
    info.name = name.to_string();
    info.table_id = id;
    info.table_version = 1;
    info.table_kind = table_props::IC_OBJECT_KIND_UNIQUE_INDEX;
    IndexDef::new(info, Arc::clone(table), None)
  }

  fn put_index(cache: &DictCache, def: IndexDef) -> Arc<IndexDef> {
    let name = def.name().to_string();
    assert!(matches!(cache.lookup(&name), Lookup::Fetch));
    let def = Arc::new(def);
    assert!(cache.fetched(&name, Entry::Index(Arc::clone(&def))));
    def
  }

  #[test]
  fn an_index_keeps_the_table_version_it_was_bound_for() {
    let cache = DictCache::new();
    let t1 = put(&cache, table("db/def/t1", 13, 1));
    let uk = put_index(&cache, index("sys/def/13/uk$unique", 14, &t1));
    // Nobody but the index holds the table object now, apart from the
    // cache.
    drop(t1);
    // The table is altered. The notice lets go of it, and of the index
    // bound for its old version.
    assert_eq!(cache.table_changed("db/def/t1", 13, 1), 2);
    assert!(!uk.is_valid());
    assert!(!uk.table().is_valid());
    // Both still whole, for whoever holds the index.
    assert_eq!(uk.table().table_version(), 1);
    assert_eq!(uk.table().name(), "db/def/t1");
    assert!(matches!(
      cache.lookup("sys/def/13/uk$unique"),
      Lookup::Fetch
    ));
  }

  #[test]
  fn forgetting_a_table_lets_go_of_its_indexes_only() {
    let cache = DictCache::new();
    let t1 = put(&cache, table("db/def/t1", 13, 1));
    let t2 = put(&cache, table("db/def/t2", 15, 1));
    let uk1 = put_index(&cache, index("sys/def/13/uk$unique", 14, &t1));
    let uk2 = put_index(&cache, index("sys/def/15/uk$unique", 16, &t2));
    cache.forget_table(&t1);
    assert!(!uk1.is_valid());
    assert!(uk2.is_valid());
    assert!(matches!(
      cache.lookup("sys/def/15/uk$unique"),
      Lookup::Index(_)
    ));
  }

  #[test]
  fn index_names_follow_the_reference() {
    assert_eq!(index_name(13, "idx_amount"), "sys/def/13/idx_amount");
    assert_eq!(old_index_name("ictest", 13, "uk"), "ictest/def/13/uk");
  }

  #[test]
  fn hash_maps_are_shared_by_id_and_version() {
    let cache = DictCache::new();
    let map = HashMapProps {
      name: "DEFAULT-HASHMAP-3840-8".to_string(),
      object_id: 2,
      version: 1,
      fragments: vec![0, 1],
    };
    let first = cache.add_hash_map(map.clone());
    let second = cache.add_hash_map(map);
    assert!(Arc::ptr_eq(&first, &second));
    assert!(cache.hash_map(2, 1).is_some());
    assert!(cache.hash_map(2, 2).is_none());
  }
}
