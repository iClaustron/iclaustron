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
//! Verify: `Ndb.cpp`, `computeHash` with an `NdbRecord`, where the key
//! is built and the second hash word taken; `rondb_hash.cpp`,
//! `rondb_calc_hash`; `NdbDictionary.cpp`, where the hash map is looked
//! up; `DbtupRoutines.cpp`, `read_pseudo`, `FRAGMENT`.

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
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let mut bytes: Vec<u8> = Vec::new();
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
    bytes.extend_from_slice(&value[..len]);
    while bytes.len() % 4 != 0 {
      bytes.push(0);
    }
  }
  Ok(bytes)
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
  let key = distribution_key(key_rec, key_row)?;
  Ok(map.fragment_of(partition_hash(table, &key)))
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
