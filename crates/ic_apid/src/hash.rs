// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Where a key belongs: the distribution key hashed the way the data
//! nodes hash it, and the partition the hash map sends it to. New: the
//! C sent every operation wherever it liked.
//!
//! **The bytes hashed** are the distribution key columns in attribute
//! order, each padded to a word: a fixed-size value in full, a
//! variable-sized one behind its one or two length bytes with only the
//! bytes it has, as the record holds them; a character column as its
//! collation puts it (`collation_map`), from the weights the data nodes
//! described, fetched when the table was bound. A character column
//! whose collation is not known or cannot be placed leaves its key
//! unplaced, and the transaction goes to the next node in turn.
//!
//! **The hash** is the table's: MD5 for a table made before RonDB
//! 22.10.1, XXH3 otherwise, as `IC_TP_HASH_FUNCTION` says. What places the
//! key is the second word of the MD5 digest, or the high half of the
//! XXH3 value.
//!
//! **A fanout table** (RonDB's `PARTITION_HASH=base:detail:fanout`)
//! is hashed in two steps (phase 10; the author, 2026-10-21): its
//! primary key's columns in key order, the first `base` of them, table
//! property 166, apart from the next `detail`, 167, each part as a
//! distribution key's bytes are. The row is placed by the base's hash
//! rounded down to a multiple of the fanout, 168, plus the detail's
//! modulo the fanout, so that every row of one base is in the fanout's
//! partitions from the base's own; a scan whose ranges fix the base
//! names that first hash value and reads those partitions alone
//! (chapter 05 §6.3, step 3, §7.2).
//!
//! **The partition** is the hash map's bucket at the hash modulo the
//! map's length. The data nodes place a row the same way, and the
//! `IC_COL_PARTITION` pseudo column reads back the partition a row
//! landed in, which is how this was checked.
//!
//! **The node** comes from the table's replica data: the nodes holding
//! each fragment. A read-backup table, which every table made by a
//! RonDB server is, may be read at any replica, so the started ones are
//! taken in turn; a fully replicated one at any node holding any
//! fragment. Otherwise the primary replica is wanted, and which replica
//! is primary is not fixed: with several replicas alive the data nodes
//! spread the primaries of a node group's fragments over its alive
//! nodes in order, in batches of the fragment count divided by the
//! alive count rounded up, and the reference works the same rule out
//! for itself, so this does too. The reference also prefers a node in
//! its own location domain or on its own host; that is not done here
//! yet. A wrong choice costs a hop inside the cluster, never a wrong
//! answer: the coordinator forwards to wherever the row is.
//!
//! Verify: chapter 05 §17.14.

use ic_port::err;
use ic_port::IcError;
use ic_signals::table_props;
use ic_signals::table_props::ColumnProps;

use crate::collation_map::CollationMap;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::record::Record;
use crate::row_codec;

/// The distribution key of the row `key_rec` lays out, as the bytes to
/// hash.
pub fn distribution_key(
  key_rec: &Record,
  key_row: &[u8],
) -> Result<Vec<u8>, IcError> {
  let mut bytes: Vec<u8> = Vec::new();
  distribution_fields(key_rec, key_row, &mut |value: &[u8]| {
    push_padded(&mut bytes, value);
  })?;
  Ok(bytes)
}

/// A key that fits here is hashed from the stack: a key of a few
/// integers, which is most of them, costs no allocation.
const IC_KEY_ON_STACK: usize = 128;

/// A value onto the key, padded with zeros to a word.
fn push_padded(bytes: &mut Vec<u8>, value: &[u8]) {
  bytes.extend_from_slice(value);
  while bytes.len() % 4 != 0 {
    bytes.push(0);
  }
}

/// A walk over a key's columns, giving each column's part to a taker.
type KeyWalk<'a> = dyn Fn(&mut dyn FnMut(&[u8])) -> Result<(), IcError> + 'a;

/// The fields `walk` gives, each padded to a word, hashed by `table`'s
/// function and placed by its hash map: on the stack when they fit,
/// walked again into a vector when they do not.
fn partition_by(table: &TableDef, walk: &KeyWalk<'_>) -> Result<u32, IcError> {
  // A fanout table is hashed in two parts (`fanout_hashes`).
  if table.info().fanout > 1 {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  let map = match table.hash_map() {
    Some(map) => map,
    None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
  };
  let mut buf = [0u8; IC_KEY_ON_STACK];
  let mut at: usize = 0;
  walk(&mut |value: &[u8]| {
    let padded = (value.len() + 3) & !3;
    if at + padded <= buf.len() {
      buf[at..at + value.len()].copy_from_slice(value);
      buf[at + value.len()..at + padded].fill(0);
    }
    at += padded;
  })?;
  if at <= buf.len() {
    return Ok(map.fragment_of(partition_hash(table, &buf[..at])));
  }
  let mut bytes: Vec<u8> = Vec::with_capacity(at);
  walk(&mut |value: &[u8]| push_padded(&mut bytes, value))?;
  Ok(map.fragment_of(partition_hash(table, &bytes)))
}

/// The first `count` columns of the table's primary key, in key order,
/// which is their attribute order, each column's part given to `take`
/// (see `take_column`).
fn primary_fields(
  key_rec: &Record,
  key_row: &[u8],
  count: u32,
  take: &mut dyn FnMut(&[u8]),
) -> Result<(), IcError> {
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let table = key_rec.table();
  let mut taken: u32 = 0;
  for (place, attr) in table.info().attributes.iter().enumerate() {
    if taken == count {
      break;
    }
    if !attr.primary_key {
      continue;
    }
    let field = match key_rec.position_of(attr.column_id) {
      Some(position) => match key_rec.field(position) {
        Some(field) => field,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      },
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    let start = field.offset() as usize;
    let size = field.size() as usize;
    let collation = table.collation_at(place).map(|map| map.as_ref());
    take_column(attr, collation, &key_row[start..start + size], take)?;
    taken += 1;
  }
  Ok(())
}

/// A fanout table's key hashed in its two parts: the base, the first
/// columns of its primary key as many as table property 166 says, and
/// the detail, the next as many as 167 says, each column padded to a
/// word. The base's hash, and the detail's when `detail` is wanted,
/// else 0, when the row need hold the base alone. On the stack when the
/// key fits, as `partition_by` does.
fn fanout_hashes(
  table: &TableDef,
  key_rec: &Record,
  key_row: &[u8],
  detail: bool,
) -> Result<(u32, u32), IcError> {
  let info = table.info();
  let base = info.fanout_base_keys;
  let mut count = base;
  if detail {
    count += info.fanout_detail_keys;
  }
  let mut buf = [0u8; IC_KEY_ON_STACK];
  let mut at: usize = 0;
  let mut split: usize = 0;
  let mut taken: u32 = 0;
  primary_fields(key_rec, key_row, count, &mut |value: &[u8]| {
    let padded = (value.len() + 3) & !3;
    if at + padded <= buf.len() {
      buf[at..at + value.len()].copy_from_slice(value);
      buf[at + value.len()..at + padded].fill(0);
    }
    at += padded;
    taken += 1;
    if taken == base {
      split = at;
    }
  })?;
  if at <= buf.len() {
    return Ok(two_hashes(table, &buf[..split], &buf[split..at], detail));
  }
  let mut bytes: Vec<u8> = Vec::with_capacity(at);
  primary_fields(key_rec, key_row, count, &mut |value: &[u8]| {
    push_padded(&mut bytes, value)
  })?;
  Ok(two_hashes(table, &bytes[..split], &bytes[split..], detail))
}

/// The hashes of a fanout key's two parts, the detail's 0 unless asked
/// for.
fn two_hashes(
  table: &TableDef,
  base: &[u8],
  detail: &[u8],
  with_detail: bool,
) -> (u32, u32) {
  let mut detail_hash: u32 = 0;
  if with_detail {
    detail_hash = partition_hash(table, detail);
  }
  (partition_hash(table, base), detail_hash)
}

/// The first of the fanout's hash values a base's rows are placed by:
/// its hash rounded down to a multiple of the fanout.
fn fanout_first(base_hash: u32, fanout: u32) -> u32 {
  base_hash / fanout * fanout
}

/// The hash a fanout table's row is placed by: the base's first hash
/// value plus the detail's hash modulo the fanout, wrapping as the data
/// nodes' 32-bit sum does.
fn fanout_value(base_hash: u32, detail_hash: u32, fanout: u32) -> u32 {
  fanout_first(base_hash, fanout).wrapping_add(detail_hash % fanout)
}

/// Each distribution key column's part of the key, in attribute order,
/// given to `take` (see `take_column`).
fn distribution_fields(
  key_rec: &Record,
  key_row: &[u8],
  take: &mut dyn FnMut(&[u8]),
) -> Result<(), IcError> {
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let table = key_rec.table();
  for (place, attr) in table.info().attributes.iter().enumerate() {
    if !attr.distribution_key {
      continue;
    }
    let field = match key_rec.position_of(attr.column_id) {
      Some(position) => match key_rec.field(position) {
        Some(field) => field,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      },
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    let start = field.offset() as usize;
    let size = field.size() as usize;
    let collation = table.collation_at(place).map(|map| map.as_ref());
    take_column(attr, collation, &key_row[start..start + size], take)?;
  }
  Ok(())
}

/// Each column of a unique index's own key, in the index's order, given
/// to `take` as a table's distribution key is: found by name in
/// `key_rec`, which is over the indexed table. A NULL is refused, as a
/// row with one is not in the index.
fn index_fields(
  index: &IndexDef,
  key_rec: &Record,
  key_row: &[u8],
  take: &mut dyn FnMut(&[u8]),
) -> Result<(), IcError> {
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let own = index.as_table();
  for (place, column) in own.info().attributes.iter().enumerate() {
    if !column.primary_key {
      continue;
    }
    let field_id = key_rec.table().field_id(&column.name)?;
    let position = match key_rec.position_of(field_id) {
      Some(position) => position,
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    if key_rec.is_null(key_row, position) {
      return Err(IcError::new(row_codec::IC_NDB_ERROR_NULL_KEY));
    }
    let field = match key_rec.field(position) {
      Some(field) => field,
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    let start = field.offset() as usize;
    let size = field.size() as usize;
    let collation = own.collation_at(place).map(|map| map.as_ref());
    take_column(column, collation, &key_row[start..start + size], take)?;
  }
  Ok(())
}

/// One column's part of the key, given to `take`, which pads it to a
/// word. A value as it lies in the row: a fixed-size one in full, a
/// variable-sized one behind its one or two length bytes with only the
/// bytes it has. A character value as its collation puts it, from a
/// CHAR's whole field or a VARCHAR's bytes after its length (chapter 05
/// §6.3, §16.2); without a known collation it cannot be placed.
fn take_column(
  attr: &ColumnProps,
  collation: Option<&CollationMap>,
  value: &[u8],
  take: &mut dyn FnMut(&[u8]),
) -> Result<(), IcError> {
  let size = value.len();
  let (head, len) = match attr.array_kind {
    table_props::IC_ARRAY_KIND_SHORT_VARSIZE => (1, 1 + value[0] as usize),
    table_props::IC_ARRAY_KIND_LONG_VARSIZE => {
      (2, 2 + value[0] as usize + 256 * value[1] as usize)
    }
    _ => (0, size),
  };
  if len > size {
    return Err(IcError::new(err::IC_ERROR_VALUE_TOO_LONG));
  }
  if !attr.is_char_type() {
    take(&value[..len]);
    return Ok(());
  }
  let map = match collation {
    Some(map) => map,
    None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
  };
  let mut part: Vec<u8> = Vec::new();
  map.key_part(&value[head..len], size - head, head == 0, &mut part)?;
  take(&part);
  Ok(())
}

/// The word that places a key: the second of the MD5 digest, or the
/// high half of the XXH3 value, whichever the table hashes by.
pub fn partition_hash(table: &TableDef, key: &[u8]) -> u32 {
  if table.info().hash_function == 0 {
    return ic_util::md5::digest(key)[1];
  }
  (ic_util::xxh3::hash(key) >> 32) as u32
}

/// The partition the row with this key is in, or why it cannot be
/// known: a table with no hash map, or a character key column whose
/// collation is not known or cannot be placed. A fanout table's key is
/// hashed in its two parts.
pub fn partition_of(
  table: &TableDef,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<u32, IcError> {
  let fanout = table.info().fanout;
  if fanout > 1 {
    let map = match table.hash_map() {
      Some(map) => map,
      None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    };
    let (base, detail) = fanout_hashes(table, key_rec, key_row, true)?;
    return Ok(map.fragment_of(fanout_value(base, detail, fanout)));
  }
  partition_by(table, &|take| distribution_fields(key_rec, key_row, take))
}

/// For a fanout table, the first of the hash values that rows with this
/// row's base columns are placed by, which a scan pruned to the
/// fanout's partitions names (chapter 05 §7.2, word 5's bit 31); the
/// row need hold the base columns alone. Refused for a table without a
/// fanout (`IC_ERROR_NOT_SUPPORTED`), and as `partition_of` is.
pub fn fanout_first_of(
  table: &TableDef,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<u32, IcError> {
  let fanout = table.info().fanout;
  if fanout <= 1 || table.hash_map().is_none() {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  let (base, _) = fanout_hashes(table, key_rec, key_row, false)?;
  Ok(fanout_first(base, fanout))
}

/// The partition of the index's own table that a unique key is in, or
/// why it cannot be known, as for a table. `key_row` is laid out by
/// `key_rec` over the indexed table. The bytes hashed are the indexed
/// columns in the index's order, each as a table's distribution key
/// column would be, character ones through their collations, which is
/// what the coordinator hashes over the index's table. Verify: chapter
/// 05 §6.8, §16.2.
pub fn index_partition_of(
  index: &IndexDef,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<u32, IcError> {
  partition_by(index.as_table(), &|take| {
    index_fields(index, key_rec, key_row, take)
  })
}

/// Node ids a node choice collects, kept inline up to this many: a
/// partition has at most four replicas. More, which only a fully
/// replicated table in a large cluster lists, go to the heap.
const IC_INLINE_NODES: usize = 8;

/// A short list of node ids that allocates nothing until it outgrows
/// [`IC_INLINE_NODES`], when it moves to a vector rather than refusing:
/// choosing a node is done for every transaction.
struct NodeList {
  inline: [u32; IC_INLINE_NODES],
  len: usize,
  more: Vec<u32>,
}

impl NodeList {
  fn new() -> NodeList {
    NodeList {
      inline: [0; IC_INLINE_NODES],
      len: 0,
      more: Vec::new(),
    }
  }

  fn push(&mut self, node: u32) {
    if self.more.is_empty() && self.len < IC_INLINE_NODES {
      self.inline[self.len] = node;
      self.len += 1;
      return;
    }
    if self.more.is_empty() {
      self.more.extend_from_slice(&self.inline[..self.len]);
    }
    self.more.push(node);
  }

  fn as_slice(&self) -> &[u32] {
    if !self.more.is_empty() {
      return &self.more;
    }
    &self.inline[..self.len]
  }

  fn as_mut_slice(&mut self) -> &mut [u32] {
    if !self.more.is_empty() {
      return &mut self.more;
    }
    &mut self.inline[..self.len]
  }
}

/// The node to send an operation on `partition` to, given the nodes
/// that are started, or `None` if no node holding it is. `turn` spreads
/// the choice where any replica will do; any changing number serves.
pub fn choose_node(
  table: &TableDef,
  partition: u32,
  started: &[u32],
  turn: u32,
) -> Option<u32> {
  let info = table.info();
  if info.fully_replicated {
    // Any node holding any fragment.
    let mut nodes = NodeList::new();
    for node in &info.fragment_nodes {
      let node = *node as u32;
      if started.contains(&node) && !nodes.as_slice().contains(&node) {
        nodes.push(node);
      }
    }
    return pick_in_turn(nodes.as_slice(), turn);
  }
  let replicas = info.nodes_of_fragment(partition);
  let mut alive = NodeList::new();
  for node in replicas {
    if started.contains(&(*node as u32)) {
      alive.push(*node as u32);
    }
  }
  if info.read_backup {
    return pick_in_turn(alive.as_slice(), turn);
  }
  if let Some(primary) = primary_of(table, partition, started) {
    return Some(primary);
  }
  alive.as_slice().first().copied()
}

fn pick_in_turn(nodes: &[u32], turn: u32) -> Option<u32> {
  if nodes.is_empty() {
    return None;
  }
  Some(nodes[turn as usize % nodes.len()])
}

/// The primary replica of a partition among the nodes that are
/// started, as the data nodes assign primaries: with one replica, or
/// only one of them alive, that one; otherwise the fragments of each
/// node group, in order, are dealt to the group's alive nodes in node
/// id order, so many fragments to each that every alive node gets an
/// equal share. `None` if no replica is alive or the table has no
/// replica data.
pub fn primary_of(
  table: &TableDef,
  partition: u32,
  started: &[u32],
) -> Option<u32> {
  let info = table.info();
  let replicas = info.nodes_of_fragment(partition);
  if replicas.is_empty() {
    return None;
  }
  if replicas.len() == 1 {
    let node = replicas[0] as u32;
    if started.contains(&node) {
      return Some(node);
    }
    return None;
  }
  // The group is the fragment's nodes; its alive nodes, in id order.
  let mut group = NodeList::new();
  for node in replicas {
    let node = *node as u32;
    if started.contains(&node) {
      group.push(node);
    }
  }
  group.as_mut_slice().sort_unstable();
  let alive = group.as_slice();
  if alive.is_empty() {
    return None;
  }
  if alive.len() == 1 {
    return Some(alive[0]);
  }
  // Which of the group's fragments this is, and how many the group has:
  // a fragment is in the group when its first node is one of ours.
  let first = replicas[0];
  let mut place: u32 = 0;
  let mut count: u32 = 0;
  let mut f: u32 = 0;
  while f < info.fragments_with_nodes() {
    let nodes = info.nodes_of_fragment(f);
    if nodes.contains(&first) {
      if f == partition {
        place = count;
      }
      count += 1;
    }
    f += 1;
  }
  let per_batch = count.div_ceil(alive.len() as u32);
  let batch = (place / per_batch) as usize;
  Some(alive[batch.min(alive.len() - 1)])
}

#[cfg(test)]
mod tests {
  use super::*;
  use ic_signals::prop_stream::PropertyWriter;
  use ic_signals::table_props::parse_table_info;
  use ic_signals::table_props::HashMapProps;
  use ic_signals::table_props::IC_ARRAY_KIND_SHORT_VARSIZE;
  use ic_signals::table_props::IC_TP_COLUMN_ARRAY_KIND;
  use ic_signals::table_props::IC_TP_COLUMN_COUNT;
  use ic_signals::table_props::IC_TP_COLUMN_END;
  use ic_signals::table_props::IC_TP_COLUMN_ID;
  use ic_signals::table_props::IC_TP_COLUMN_IN_DIST_KEY;
  use ic_signals::table_props::IC_TP_COLUMN_IN_KEY;
  use ic_signals::table_props::IC_TP_COLUMN_LENGTH;
  use ic_signals::table_props::IC_TP_COLUMN_NAME;
  use ic_signals::table_props::IC_TP_COLUMN_PRECISION;
  use ic_signals::table_props::IC_TP_COLUMN_TYPE;
  use ic_signals::table_props::IC_TP_FANOUT;
  use ic_signals::table_props::IC_TP_FANOUT_BASE_KEYS;
  use ic_signals::table_props::IC_TP_FANOUT_DETAIL_KEYS;
  use ic_signals::table_props::IC_TP_HASH_FUNCTION;
  use ic_signals::table_props::IC_TP_TABLE_NAME;
  use ic_signals::table_props::IC_TYPE_INT;
  use ic_signals::table_props::IC_TYPE_VARBINARY;
  use ic_signals::table_props::IC_TYPE_VARCHAR;
  use std::sync::Arc;

  /// `a INT` key and distribution key, `b VARBINARY(8)` key, `c INT`;
  /// with `dkey_all` both key columns are the distribution key, and
  /// with `new_hash` the table hashes by XXH3. Its fanout is the
  /// default, one, which is no fanout.
  fn table(dkey_all: bool, new_hash: bool, charset: u32) -> Arc<TableDef> {
    table_with_fanout(dkey_all, new_hash, charset, 1)
  }

  /// As `table`, with a fanout above one making it `a` the base and `b`
  /// the detail: `PARTITION_HASH=1:1:fanout`.
  fn table_with_fanout(
    dkey_all: bool,
    new_hash: bool,
    charset: u32,
    fanout: u32,
  ) -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "db/def/t");
    w.add_u32(IC_TP_COLUMN_COUNT, 3);
    if new_hash {
      w.add_u32(IC_TP_HASH_FUNCTION, 1);
    }
    if fanout > 1 {
      w.add_u32(IC_TP_FANOUT_BASE_KEYS, 1);
      w.add_u32(IC_TP_FANOUT_DETAIL_KEYS, 1);
      w.add_u32(IC_TP_FANOUT, fanout);
    }
    w.add_string(IC_TP_COLUMN_NAME, "a");
    w.add_u32(IC_TP_COLUMN_ID, 0);
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 1);
    w.add_u32(IC_TP_COLUMN_IN_KEY, 1);
    w.add_u32(IC_TP_COLUMN_IN_DIST_KEY, 1);
    w.add_u32(IC_TP_COLUMN_END, 0);
    w.add_string(IC_TP_COLUMN_NAME, "b");
    w.add_u32(IC_TP_COLUMN_ID, 1);
    let mut kind = IC_TYPE_VARBINARY;
    if charset != 0 {
      kind = IC_TYPE_VARCHAR;
    }
    w.add_u32(IC_TP_COLUMN_TYPE, kind);
    w.add_u32(IC_TP_COLUMN_LENGTH, 8);
    w.add_u32(IC_TP_COLUMN_PRECISION, charset << 16);
    w.add_u32(IC_TP_COLUMN_ARRAY_KIND, IC_ARRAY_KIND_SHORT_VARSIZE);
    w.add_u32(IC_TP_COLUMN_IN_KEY, 1);
    w.add_u32(IC_TP_COLUMN_IN_DIST_KEY, dkey_all as u32);
    w.add_u32(IC_TP_COLUMN_END, 0);
    w.add_string(IC_TP_COLUMN_NAME, "c");
    w.add_u32(IC_TP_COLUMN_ID, 2);
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 1);
    w.add_u32(IC_TP_COLUMN_END, 0);
    let info = parse_table_info(w.words()).expect("parsed");
    let map = HashMapProps {
      name: "test map".to_string(),
      object_id: 1,
      version: 1,
      fragments: vec![0, 1, 2, 3, 4, 5, 6, 7],
    };
    Arc::new(TableDef::new(info, Some(Arc::new(map))))
  }

  /// A unique index over the given columns of `table`, its own table
  /// placed by an eight-fragment map, or by none.
  fn index_over(
    t: &Arc<TableDef>,
    columns: &[(&str, u32, u32)],
    with_map: bool,
  ) -> IndexDef {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "sys/def/1/u$unique");
    w.add_u32(IC_TP_COLUMN_COUNT, columns.len() as u32 + 1);
    w.add_u32(IC_TP_HASH_FUNCTION, 1);
    let mut id: u32 = 0;
    for (name, type_code, length) in columns {
      w.add_string(IC_TP_COLUMN_NAME, name);
      w.add_u32(IC_TP_COLUMN_ID, id);
      w.add_u32(IC_TP_COLUMN_TYPE, *type_code);
      w.add_u32(IC_TP_COLUMN_LENGTH, *length);
      if *type_code != IC_TYPE_INT {
        w.add_u32(IC_TP_COLUMN_PRECISION, 8 << 16);
        w.add_u32(IC_TP_COLUMN_ARRAY_KIND, IC_ARRAY_KIND_SHORT_VARSIZE);
      }
      w.add_u32(IC_TP_COLUMN_IN_KEY, 1);
      w.add_u32(IC_TP_COLUMN_END, 0);
      id += 1;
    }
    w.add_string(IC_TP_COLUMN_NAME, "NDB$PK");
    w.add_u32(IC_TP_COLUMN_ID, id);
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 2);
    w.add_u32(IC_TP_COLUMN_END, 0);
    let info = parse_table_info(w.words()).expect("parsed");
    let mut map: Option<Arc<HashMapProps>> = None;
    if with_map {
      map = Some(Arc::new(HashMapProps {
        name: "index map".to_string(),
        object_id: 2,
        version: 1,
        fragments: vec![7, 6, 5, 4, 3, 2, 1, 0],
      }));
    }
    IndexDef::new(info, Arc::clone(t), map)
  }

  #[test]
  fn a_unique_key_is_placed_by_the_index_table() {
    let t = table(true, true, 0);
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 0x0102_0304, b"xy");
    // The index's key is c and a, in that order, not the table's.
    let index =
      index_over(&t, &[("c", IC_TYPE_INT, 1), ("a", IC_TYPE_INT, 1)], true);
    let placed = index_partition_of(&index, &rec, &row).expect("placed");
    let at_c = rec.field(2).expect("c").offset() as usize;
    let mut key: Vec<u8> = row[at_c..at_c + 4].to_vec();
    key.extend_from_slice(&0x0102_0304u32.to_le_bytes());
    let map = index.as_table().hash_map().expect("map");
    let want = map.fragment_of(partition_hash(index.as_table(), &key));
    assert_eq!(placed, want);
    // A character column without a known collation, or an index
    // without a hash map, is not placed.
    let by_name = index_over(&t, &[("b", IC_TYPE_VARCHAR, 8)], true);
    assert!(index_partition_of(&by_name, &rec, &row).is_err());
    let unmapped = index_over(&t, &[("a", IC_TYPE_INT, 1)], false);
    assert!(index_partition_of(&unmapped, &rec, &row).is_err());
  }

  fn row_of(rec: &Record, a: u32, b: &[u8]) -> Vec<u8> {
    let mut row = vec![0u8; rec.row_size() as usize];
    let at = rec.field(0).expect("a").offset() as usize;
    row[at..at + 4].copy_from_slice(&a.to_le_bytes());
    let at = rec.field(1).expect("b").offset() as usize;
    row[at] = b.len() as u8;
    row[at + 1..at + 1 + b.len()].copy_from_slice(b);
    row
  }

  /// A table over the fragments given, each held by the nodes listed,
  /// with the flags asked for.
  fn placed(
    fragments: &[&[u16]],
    read_backup: bool,
    fully_replicated: bool,
  ) -> Arc<TableDef> {
    let t = table(false, true, 0);
    let mut info = t.info().clone();
    info.read_backup = read_backup;
    info.fully_replicated = fully_replicated;
    info.replica_count = fragments[0].len() as u32;
    info.fragment_nodes = Vec::new();
    for nodes in fragments {
      info.fragment_nodes.extend_from_slice(nodes);
    }
    Arc::new(TableDef::new(info, None))
  }

  #[test]
  fn the_primary_is_dealt_in_batches_over_the_alive_nodes() {
    // One node group of nodes 1 and 2 holding four fragments, the
    // distribution handler's primaries alternating.
    let group: [&[u16]; 4] = [&[1, 2], &[2, 1], &[1, 2], &[2, 1]];
    let t = placed(&group, false, false);
    // Both alive: two fragments each, in fragment order.
    let both = [1, 2];
    assert_eq!(primary_of(&t, 0, &both), Some(1));
    assert_eq!(primary_of(&t, 1, &both), Some(1));
    assert_eq!(primary_of(&t, 2, &both), Some(2));
    assert_eq!(primary_of(&t, 3, &both), Some(2));
    // Only node 2 alive: it has them all.
    assert_eq!(primary_of(&t, 1, &[2]), Some(2));
    assert_eq!(primary_of(&t, 1, &[]), None);
  }

  #[test]
  fn two_node_groups_are_dealt_apart() {
    let groups: [&[u16]; 4] = [&[1, 2], &[3, 4], &[2, 1], &[4, 3]];
    let t = placed(&groups, false, false);
    let all = [1, 2, 3, 4];
    assert_eq!(primary_of(&t, 0, &all), Some(1));
    assert_eq!(primary_of(&t, 1, &all), Some(3));
    assert_eq!(primary_of(&t, 2, &all), Some(2));
    assert_eq!(primary_of(&t, 3, &all), Some(4));
  }

  #[test]
  fn a_read_backup_table_takes_its_replicas_in_turn() {
    let group: [&[u16]; 2] = [&[1, 2], &[2, 1]];
    let t = placed(&group, true, false);
    assert_eq!(choose_node(&t, 0, &[1, 2], 0), Some(1));
    assert_eq!(choose_node(&t, 0, &[1, 2], 1), Some(2));
    // A replica that is down is passed over.
    assert_eq!(choose_node(&t, 0, &[2], 0), Some(2));
    assert_eq!(choose_node(&t, 0, &[3], 0), None);
  }

  #[test]
  fn a_plain_table_goes_to_its_primary() {
    let group: [&[u16]; 2] = [&[1, 2], &[2, 1]];
    let t = placed(&group, false, false);
    assert_eq!(choose_node(&t, 0, &[1, 2], 5), Some(1));
    assert_eq!(choose_node(&t, 1, &[1, 2], 5), Some(2));
  }

  #[test]
  fn a_fully_replicated_table_goes_anywhere_it_is_held() {
    let groups: [&[u16]; 2] = [&[1, 2], &[3, 4]];
    let t = placed(&groups, true, true);
    assert_eq!(choose_node(&t, 0, &[1, 2, 3, 4], 2), Some(3));
    assert_eq!(choose_node(&t, 0, &[4], 0), Some(4));
  }

  #[test]
  fn only_the_distribution_key_columns_are_hashed() {
    let t = table(false, true, 0);
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 7, b"hi");
    assert_eq!(
      distribution_key(&rec, &row).expect("key"),
      7u32.to_le_bytes()
    );
  }

  #[test]
  fn a_variable_sized_column_keeps_its_length_byte_and_is_padded() {
    let t = table(true, true, 0);
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 7, b"hi");
    let key = distribution_key(&rec, &row).expect("key");
    assert_eq!(key, [7, 0, 0, 0, 2, b'h', b'i', 0]);
  }

  #[test]
  fn a_node_list_moves_to_the_heap_past_its_inline_room() {
    let mut list = NodeList::new();
    let mut i: u32 = 0;
    while i < IC_INLINE_NODES as u32 {
      list.push(i + 1);
      i += 1;
    }
    assert!(list.more.is_empty(), "still inline");
    list.push(100);
    list.push(50);
    assert_eq!(list.as_slice().len(), IC_INLINE_NODES + 2);
    assert_eq!(list.as_slice()[0], 1);
    assert_eq!(list.as_slice()[IC_INLINE_NODES], 100);
    list.as_mut_slice().sort_unstable();
    assert_eq!(list.as_slice()[IC_INLINE_NODES], 50);
    assert_eq!(list.as_slice()[IC_INLINE_NODES + 1], 100);
  }

  #[test]
  fn the_key_on_the_stack_is_the_key_on_the_heap() {
    let t = table(true, true, 0);
    let map = t.hash_map().expect("map");
    // A short key, on the stack, as distribution_key makes it.
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 7, b"hi");
    let key = distribution_key(&rec, &row).expect("key");
    assert_eq!(
      partition_of(&t, &rec, &row).expect("placed"),
      map.fragment_of(partition_hash(&t, &key))
    );
    // A key too long for the stack, walked again onto the heap.
    let long = [7u8; 2 * IC_KEY_ON_STACK + 1];
    let placed = partition_by(&t, &|take| {
      take(&long[..]);
      Ok(())
    })
    .expect("placed");
    let mut bytes = long.to_vec();
    while bytes.len() % 4 != 0 {
      bytes.push(0);
    }
    assert_eq!(placed, map.fragment_of(partition_hash(&t, &bytes)));
  }

  #[test]
  fn a_character_column_needs_its_collation() {
    let t = table(true, true, 8);
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 7, b"hi");
    let e = distribution_key(&rec, &row).expect_err("refused");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  }

  #[test]
  fn the_table_decides_which_hash_places_the_key() {
    let key = 7u32.to_le_bytes();
    let old = table(false, false, 0);
    let new = table(false, true, 0);
    assert_eq!(partition_hash(&old, &key), ic_util::md5::digest(&key)[1]);
    let xxh = ic_util::xxh3::hash(&key);
    assert_eq!(partition_hash(&new, &key), (xxh >> 32) as u32);
    assert_ne!(partition_hash(&old, &key), partition_hash(&new, &key));
  }

  #[test]
  fn the_partition_is_the_hash_maps_bucket() {
    let t = table(false, true, 0);
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 7, b"");
    let hash = partition_hash(&t, &7u32.to_le_bytes());
    let want = t.hash_map().expect("map").fragment_of(hash);
    assert_eq!(partition_of(&t, &rec, &row).expect("partition"), want);
    assert!(want < 8);
  }

  #[test]
  fn a_fanout_tables_key_is_hashed_in_its_base_and_its_detail() {
    // PARTITION_HASH=1:1:4 over (a, b), whichever the distribution key
    // flags say: a is the base, b the detail, each hashed apart.
    for dkey_all in [false, true] {
      let t = table_with_fanout(dkey_all, true, 0, 4);
      let rec = Record::default_for(&t, None).expect("record");
      let row = row_of(&rec, 7, b"hi");
      let base = partition_hash(&t, &7u32.to_le_bytes());
      let detail = partition_hash(&t, &[2, b'h', b'i', 0]);
      let value = (base / 4 * 4).wrapping_add(detail % 4);
      let want = t.hash_map().expect("map").fragment_of(value);
      assert_eq!(partition_of(&t, &rec, &row).expect("placed"), want);
      // A scan by the base alone names the first of its four values.
      assert_eq!(fanout_first_of(&t, &rec, &row).expect("base"), base / 4 * 4);
      // Rows of one base, whatever their detail, are in the base's four
      // partitions: a run of four buckets of the map.
      let first = base / 4 * 4;
      let mut ours: Vec<u32> = Vec::new();
      let mut k: u32 = 0;
      while k < 4 {
        ours.push(
          t.hash_map()
            .expect("map")
            .fragment_of(first.wrapping_add(k)),
        );
        k += 1;
      }
      for detail_bytes in [&b""[..], b"x", b"hello", b"12345678"] {
        let other = row_of(&rec, 7, detail_bytes);
        let placed = partition_of(&t, &rec, &other).expect("placed");
        assert!(ours.contains(&placed), "{:?} in {:?}", placed, ours);
      }
    }
    // Without a fanout there is no first value to name.
    let plain = table(true, true, 0);
    let rec = Record::default_for(&plain, None).expect("record");
    let e = fanout_first_of(&plain, &rec, &row_of(&rec, 7, b""))
      .expect_err("no fanout");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  }

  #[test]
  fn a_fanout_value_wraps_as_the_data_nodes_sum_does() {
    assert_eq!(fanout_first(13, 4), 12);
    assert_eq!(fanout_value(13, 7, 4), 15);
    // 2^32 - 1 is a multiple of 3: the detail's part wraps past it.
    assert_eq!(fanout_value(u32::MAX, 2, 3), 1);
    assert_eq!(fanout_value(u32::MAX, 5, 3), 1);
  }
}
