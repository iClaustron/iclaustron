// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Where a key belongs: the distribution key hashed the way the data
//! nodes hash it, and the partition the hash map sends it to. New: the
//! C sent every operation wherever it liked.
//!
//! **The bytes hashed** are the distribution key columns in attribute
//! order, each as the record holds it and padded to a word: a fixed-size
//! value in full, a variable-sized one behind its one or two length
//! bytes with only the bytes it has. A character column is first
//! transformed by its collation, which needs the MySQL collation tables
//! this library does not carry, so such a key is refused for now; a
//! table whose distribution key is integers or binary strings is placed
//! exactly.
//!
//! **The hash** is the table's: MD5 for a table made before RonDB
//! 22.10.1, XXH3 otherwise, as `HashFunctionFlag` says. What places the
//! key is the second word of the MD5 digest, or the high half of the
//! XXH3 value. A table with RonDB's fanout partitioning hashes a base
//! part and a detail part apart and combines them; that is not done
//! here yet, and such a table is refused.
//!
//! **The partition** is the hash map's bucket at the hash modulo the
//! map's length. The data nodes place a row the same way, and the
//! `FRAGMENT` pseudo column reads back the partition a row landed in,
//! which is how this was checked.
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
//! Verify: `Ndb.cpp`, `computeHash` with an `NdbRecord`, where the key
//! is built and the second hash word taken, and `startTransaction` with
//! a key, which goes on to `NdbImpl::select_node`; `rondb_hash.cpp`,
//! `rondb_calc_hash`; `NdbDictionary.cpp`, where the hash map is looked
//! up; `NdbDictionaryImpl.cpp`, `get_nodes` and
//! `calculate_primary_replicas`; `ndb_cluster_connection.cpp`,
//! `select_node`; `DbtupRoutines.cpp`, `read_pseudo`, `FRAGMENT`.

use ic_ndb_signals::dict_tab_info;
use ic_port::err;
use ic_port::IcError;

use crate::dict_cache::TableDef;
use crate::record::Record;

/// The distribution key of the row `key_rec` lays out, as the bytes to
/// hash.
pub fn distribution_key(
  key_rec: &Record,
  key_row: &[u8],
) -> Result<Vec<u8>, IcError> {
  let mut bytes: Vec<u8> = Vec::new();
  distribution_fields(key_rec, key_row, &mut |value: &[u8]| {
    bytes.extend_from_slice(value);
    while bytes.len() % 4 != 0 {
      bytes.push(0);
    }
  })?;
  Ok(bytes)
}

/// A distribution key that fits here is hashed from the stack: a key
/// of a few integers, which is most of them, costs no allocation.
const IC_KEY_ON_STACK: usize = 128;

/// The distribution key into `out` if it fits, and how many bytes it
/// took; `None` if it does not fit, for the caller to allocate.
fn distribution_key_into(
  key_rec: &Record,
  key_row: &[u8],
  out: &mut [u8],
) -> Result<Option<usize>, IcError> {
  let mut at: usize = 0;
  distribution_fields(key_rec, key_row, &mut |value: &[u8]| {
    let padded = (value.len() + 3) & !3;
    if at + padded <= out.len() {
      out[at..at + value.len()].copy_from_slice(value);
      out[at + value.len()..at + padded].fill(0);
    }
    at += padded;
  })?;
  if at > out.len() {
    return Ok(None);
  }
  Ok(Some(at))
}

/// Each distribution key value, in attribute order, given to `take`:
/// the value's bytes as they lie in the row, a variable-length one with
/// its length in front, as the reference feeds them to the hash.
fn distribution_fields(
  key_rec: &Record,
  key_row: &[u8],
  take: &mut dyn FnMut(&[u8]),
) -> Result<(), IcError> {
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  for attr in &key_rec.table().info().attributes {
    if !attr.distribution_key {
      continue;
    }
    if attr.is_char_type() {
      // Its collation's transform is needed first.
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let field = match key_rec.position_of(attr.attribute_id) {
      Some(position) => match key_rec.field(position) {
        Some(field) => field,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      },
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    let start = field.offset() as usize;
    let size = field.size() as usize;
    let value = &key_row[start..start + size];
    let len = match attr.array_type {
      dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR => 1 + value[0] as usize,
      dict_tab_info::IC_ARRAY_TYPE_MEDIUM_VAR => {
        2 + value[0] as usize + 256 * value[1] as usize
      }
      _ => size,
    };
    if len > size {
      return Err(IcError::new(err::IC_ERROR_VALUE_TOO_LONG));
    }
    take(&value[..len]);
  }
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
/// known: a table with no hash map, or with fanout partitioning, or a
/// key that needs a collation.
pub fn partition_of(
  table: &TableDef,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<u32, IcError> {
  // A fanout of one, the default, is no fanout at all.
  if table.info().partition_hash_fanout > 1 {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  let map = match table.hash_map() {
    Some(map) => map,
    None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
  };
  let mut buf = [0u8; IC_KEY_ON_STACK];
  if let Some(len) = distribution_key_into(key_rec, key_row, &mut buf)? {
    return Ok(map.fragment_of(partition_hash(table, &buf[..len])));
  }
  let key = distribution_key(key_rec, key_row)?;
  Ok(map.fragment_of(partition_hash(table, &key)))
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
    let mut nodes: Vec<u32> = Vec::new();
    for node in &info.fragment_nodes {
      let node = *node as u32;
      if started.contains(&node) && !nodes.contains(&node) {
        nodes.push(node);
      }
    }
    return pick_in_turn(&nodes, turn);
  }
  let replicas = info.nodes_of_fragment(partition);
  let mut alive: Vec<u32> = Vec::new();
  for node in replicas {
    if started.contains(&(*node as u32)) {
      alive.push(*node as u32);
    }
  }
  if info.read_backup {
    return pick_in_turn(&alive, turn);
  }
  if let Some(primary) = primary_of(table, partition, started) {
    return Some(primary);
  }
  alive.first().copied()
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
  let mut alive: Vec<u32> = Vec::new();
  for node in replicas {
    let node = *node as u32;
    if started.contains(&node) {
      alive.push(node);
    }
  }
  alive.sort_unstable();
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
  use ic_ndb_signals::dict_tab_info::parse_table_info;
  use ic_ndb_signals::dict_tab_info::HashMapInfo;
  use ic_ndb_signals::dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_ARRAY_TYPE;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_DKEY;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_END;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_LENGTH;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_PRECISION;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_TYPE;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_ID;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_KEY;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_NAME;
  use ic_ndb_signals::dict_tab_info::IC_DTI_HASH_FUNCTION;
  use ic_ndb_signals::dict_tab_info::IC_DTI_NO_OF_ATTRIBUTES;
  use ic_ndb_signals::dict_tab_info::IC_DTI_TABLE_NAME;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_INT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_VARBINARY;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_VARCHAR;
  use ic_ndb_signals::simple_properties::PropertyWriter;
  use std::sync::Arc;

  /// `a INT` key and distribution key, `b VARBINARY(8)` key, `c INT`;
  /// with `dkey_all` both key columns are the distribution key, and
  /// with `new_hash` the table hashes by XXH3. Its fanout is the
  /// default, one, which is no fanout.
  fn table(dkey_all: bool, new_hash: bool, charset: u32) -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_DTI_TABLE_NAME, "db/def/t");
    w.add_u32(IC_DTI_NO_OF_ATTRIBUTES, 3);
    if new_hash {
      w.add_u32(IC_DTI_HASH_FUNCTION, 1);
    }
    w.add_string(IC_DTI_ATTRIBUTE_NAME, "a");
    w.add_u32(IC_DTI_ATTRIBUTE_ID, 0);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, IC_NDB_TYPE_INT);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_KEY, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_DKEY, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
    w.add_string(IC_DTI_ATTRIBUTE_NAME, "b");
    w.add_u32(IC_DTI_ATTRIBUTE_ID, 1);
    let mut kind = IC_NDB_TYPE_VARBINARY;
    if charset != 0 {
      kind = IC_NDB_TYPE_VARCHAR;
    }
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, kind);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, 8);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_PRECISION, charset << 16);
    w.add_u32(IC_DTI_ATTRIBUTE_ARRAY_TYPE, IC_ARRAY_TYPE_SHORT_VAR);
    w.add_u32(IC_DTI_ATTRIBUTE_KEY, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_DKEY, dkey_all as u32);
    w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
    w.add_string(IC_DTI_ATTRIBUTE_NAME, "c");
    w.add_u32(IC_DTI_ATTRIBUTE_ID, 2);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, IC_NDB_TYPE_INT);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
    let info = parse_table_info(w.words()).expect("parsed");
    let map = HashMapInfo {
      name: "test map".to_string(),
      object_id: 1,
      version: 1,
      fragments: vec![0, 1, 2, 3, 4, 5, 6, 7],
    };
    Arc::new(TableDef::new(info, Some(Arc::new(map))))
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
  fn the_key_on_the_stack_is_the_key_on_the_heap() {
    let t = table(true, true, 0);
    let rec = Record::default_for(&t, None).expect("record");
    let row = row_of(&rec, 7, b"hi");
    let mut buf = [0xFFu8; IC_KEY_ON_STACK];
    let len = distribution_key_into(&rec, &row, &mut buf)
      .expect("key")
      .expect("fits");
    assert_eq!(&buf[..len], &distribution_key(&rec, &row).expect("key")[..]);
    // A key that does not fit is left to the heap.
    let mut small = [0u8; 4];
    let got = distribution_key_into(&rec, &row, &mut small).expect("key");
    assert_eq!(got, None);
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
}
