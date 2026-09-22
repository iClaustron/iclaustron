// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Rows to and from the words of a key operation: the key a request
//! carries, the columns a read asks for, and the row a read gets back.
//! New: the C built none of these.
//!
//! **The key** is each primary key column's value in attribute order,
//! as the record holds it, padded to whole words: a fixed-size value at
//! its full size, a variable-sized one with its length bytes and only
//! the bytes it has. Verify: `NdbOperationExec.cpp`,
//! `buildSignalsNdbRecord`, and `NdbDictionaryImpl.cpp`,
//! `computeAggregates`, where key positions follow attribute order.
//!
//! **A read asks for its columns packed,** as the reference's record
//! path does: `READ_ALL` with the column count when the record has
//! every column, otherwise `READ_PACKED` followed by a bitmask of the
//! attribute ids wanted. Verify: `buildSignalsNdbRecord`.
//!
//! **The row comes back packed** behind one `READ_PACKED` header whose
//! size is the length of a result bitmap in bytes:
//!
//! - The bitmap has one bit per attribute id, set if the column was
//!   read, and after each nullable column read one more bit, set if it
//!   is NULL.
//! - The values follow in attribute order, NULLs taking no room. A
//!   byte-sized column, such as a character or 16-bit type, starts at
//!   the next byte; a 32- or 64-bit one at the next word. Bit columns
//!   are packed together bit by bit, starting on a word, and whatever
//!   follows them starts after the last word they touched.
//! - A fixed-size value has its full size; a variable-sized one says
//!   its length in its first one or two bytes.
//!
//! Verify: `DbtupRoutines.cpp`, `read_packed`; `NdbReceiver.cpp`,
//! `unpackNdbRecord`, `pad_pos` and `handle_packed_bit`.

use ic_ndb_signals::attr_header;
use ic_ndb_signals::dict_tab_info;
use ic_ndb_signals::dict_tab_info::AttributeInfo;
use ic_port::err;
use ic_port::IcError;

use crate::dict_cache::IndexDef;
use crate::node_connect::words_as_bytes;
use crate::record::Record;
use crate::record::RecordField;

/// Pseudo column: read every column, the count in the size field.
pub const IC_ATTR_READ_ALL: u32 = 0xFFF0;
/// Pseudo column: read the columns a bitmask names, or the packed row
/// that comes back.
pub const IC_ATTR_READ_PACKED: u32 = 0xFFF3;
/// Pseudo column: the partition the row is in, as one word.
pub const IC_ATTR_FRAGMENT: u32 = 0xFFFE;

/// The key section of a request: each primary key column of the key
/// record's row, in attribute order, padded to words.
pub fn key_info(key_rec: &Record, key_row: &[u8]) -> Result<Vec<u32>, IcError> {
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let mut bytes: Vec<u8> = Vec::new();
  for attr in &key_rec.table().info().attributes {
    if !attr.primary_key {
      continue;
    }
    let field = match key_rec.position_of(attr.attribute_id) {
      Some(position) => match key_rec.field(position) {
        Some(field) => field,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      },
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    let start = field.offset() as usize;
    let len = value_len(attr, field, &key_row[start..])?;
    bytes.extend_from_slice(&key_row[start..start + len]);
    while bytes.len() % 4 != 0 {
      bytes.push(0);
    }
  }
  Ok(words_of(&bytes))
}

/// The key section of a request through a unique index: each key
/// column of the index, in the index's order, taken from the base
/// table's row by name and padded to words. The index describes itself
/// as a table whose primary key is the indexed columns, with a hidden
/// column after them. Verify: `NdbIndexOperation.cpp`, `indxInit`,
/// where the operation's access table becomes the index's own.
pub fn index_key_info(
  index: &IndexDef,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<Vec<u32>, IcError> {
  if key_row.len() < key_rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let mut bytes: Vec<u8> = Vec::new();
  for column in &index.info().attributes {
    if !column.primary_key {
      continue;
    }
    let attr_id = key_rec.table().field_id(&column.name)?;
    let (attr, field) = match key_rec.position_of(attr_id) {
      Some(position) => match field_at(key_rec, position) {
        Some(found) => found,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      },
      None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
    };
    let start = field.offset() as usize;
    let len = value_len(attr, field, &key_row[start..])?;
    bytes.extend_from_slice(&key_row[start..start + len]);
    while bytes.len() % 4 != 0 {
      bytes.push(0);
    }
  }
  Ok(words_of(&bytes))
}

/// The attribute information of a read of every column the record has.
pub fn read_attr_info(rec: &Record) -> Vec<u32> {
  let table = rec.table();
  let count = rec.num_fields();
  if count == table.num_fields() {
    return vec![attr_header::attr_header(IC_ATTR_READ_ALL, count)];
  }
  let mut max_id: u32 = 0;
  let mut position: u32 = 0;
  while position < count {
    if let Some(field) = rec.field(position) {
      if field.field_id() > max_id {
        max_id = field.field_id();
      }
    }
    position += 1;
  }
  let mask_words = (max_id / 32 + 1) as usize;
  let mut words: Vec<u32> = vec![0; 1 + mask_words];
  words[0] =
    attr_header::attr_header(IC_ATTR_READ_PACKED, 4 * mask_words as u32);
  position = 0;
  while position < count {
    if let Some(field) = rec.field(position) {
      let id = field.field_id() as usize;
      words[1 + id / 32] |= 1 << (id % 32);
    }
    position += 1;
  }
  words
}

/// The attribute information of a write: for each field of the record
/// its column's header, with the value's length in bytes, and the value
/// padded to words. A length of zero means NULL, and no value follows.
///
/// With `skip_keys` the primary key columns are left out, as an update
/// may not change the key. An insert sends them, and then the row must
/// hold the same key as the key section does, as the reference makes
/// sure of by taking them from the key row.
pub fn write_attr_info(
  rec: &Record,
  row: &[u8],
  skip_keys: bool,
) -> Result<Vec<u32>, IcError> {
  if row.len() < rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let mut words: Vec<u32> = Vec::new();
  let mut position: u32 = 0;
  while position < rec.num_fields() {
    let (attr, field) = match field_at(rec, position) {
      Some(found) => found,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let is_null = rec.is_null(row, position);
    position += 1;
    if skip_keys && attr.primary_key {
      continue;
    }
    let id = field.field_id();
    if is_null {
      words.push(attr_header::attr_header(id, 0));
      continue;
    }
    let start = field.offset() as usize;
    let len = value_len(attr, field, &row[start..])?;
    words.push(attr_header::attr_header(id, len as u32));
    let mut value = row[start..start + len].to_vec();
    while value.len() % 4 != 0 {
      value.push(0);
    }
    words.extend_from_slice(&words_of(&value));
  }
  Ok(words)
}

/// Put a packed row into `row` as the record lays it out, and set or
/// clear each field's null bit. Returns how many words it took.
pub fn unpack_row(
  rec: &Record,
  words: &[u32],
  row: &mut [u8],
) -> Result<usize, IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  if row.len() < rec.row_size() as usize {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let header = match words.first() {
    Some(header) => *header,
    None => return Err(bad),
  };
  if attr_header::attribute_id(header) != IC_ATTR_READ_PACKED {
    return Err(bad);
  }
  let mask_words = (attr_header::byte_size(header) / 4) as usize;
  if words.len() < 1 + mask_words {
    return Err(bad);
  }
  let mask = &words[1..1 + mask_words];
  let data = words_as_bytes(&words[1 + mask_words..]);
  let total_bits = 32 * mask_words;
  // Where the next value starts: a byte, and a bit within the word that
  // bit columns are being packed into.
  let mut pos: usize = 0;
  let mut bit_pos: usize = 0;
  let mut bit: usize = 0;
  let mut attr_id: u32 = 0;
  while bit < total_bits {
    let read = mask_bit(mask, bit);
    bit += 1;
    let this_id = attr_id;
    attr_id += 1;
    if !read {
      continue;
    }
    let (attr, field, position) = match field_of(rec, this_id) {
      Some(found) => found,
      None => return Err(bad),
    };
    if attr.nullable {
      let is_null = bit < total_bits && mask_bit(mask, bit);
      bit += 1;
      rec.set_null(row, position, is_null)?;
      if is_null {
        continue;
      }
    }
    let start = field.offset() as usize;
    let size = field.size() as usize;
    if attr.size_bits_log2 == dict_tab_info::IC_ATTR_SIZE_BIT {
      pos = next_word(pos);
      let len = attr.array_size as usize;
      copy_bits(data, pos, bit_pos, len, &mut row[start..start + size])?;
      pos += 4 * ((bit_pos + len) / 32);
      bit_pos = (bit_pos + len) % 32;
      continue;
    }
    // Past the words bit columns have filled, then to a word for the
    // wider types.
    if attr.size_bits_log2 >= dict_tab_info::IC_ATTR_SIZE_32 {
      pos = next_word(pos);
    }
    pos += 4 * bit_pos.div_ceil(32);
    bit_pos = 0;
    if pos > data.len() {
      return Err(bad);
    }
    let len = value_len(attr, field, &data[pos..])?;
    if pos + len > data.len() {
      return Err(bad);
    }
    row[start..start + len].copy_from_slice(&data[pos..pos + len]);
    pos += len;
  }
  let used = next_word(pos) + 4 * bit_pos.div_ceil(32);
  Ok(1 + mask_words + used / 4)
}

/// How many bytes of `value` belong to the field: its full size if
/// fixed, or its length bytes and the length they say.
fn value_len(
  attr: &AttributeInfo,
  field: &RecordField,
  value: &[u8],
) -> Result<usize, IcError> {
  let size = field.size() as usize;
  let short = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  let len = match attr.array_type {
    dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR => {
      if value.is_empty() {
        return Err(short);
      }
      1 + value[0] as usize
    }
    dict_tab_info::IC_ARRAY_TYPE_MEDIUM_VAR => {
      if value.len() < 2 {
        return Err(short);
      }
      2 + value[0] as usize + 256 * value[1] as usize
    }
    _ => size,
  };
  if len > size {
    return Err(IcError::new(err::IC_ERROR_VALUE_TOO_LONG));
  }
  Ok(len)
}

/// The column and the record's field at a position.
fn field_at(
  rec: &Record,
  position: u32,
) -> Option<(&AttributeInfo, &RecordField)> {
  let field = rec.field(position)?;
  let attr = rec.table().field(field.field_id())?;
  Some((attr, field))
}

/// The column, the record's field and its position for an attribute id.
fn field_of(
  rec: &Record,
  attr_id: u32,
) -> Option<(&AttributeInfo, &RecordField, u32)> {
  let position = rec.position_of(attr_id)?;
  let field = rec.field(position)?;
  let attr = rec.table().field(attr_id)?;
  Some((attr, field, position))
}

fn mask_bit(mask: &[u32], bit: usize) -> bool {
  (mask[bit / 32] >> (bit % 32)) & 1 != 0
}

fn next_word(pos: usize) -> usize {
  pos.next_multiple_of(4)
}

/// Copy `len` bits, starting at bit `bit_pos` of the word at byte `pos`
/// of `data`, into `dest` from its first bit, clearing the rest of it.
/// The words are the data node's, in its own byte order, as the field in
/// the row is kept.
fn copy_bits(
  data: &[u8],
  pos: usize,
  bit_pos: usize,
  len: usize,
  dest: &mut [u8],
) -> Result<(), IcError> {
  let words_touched = (bit_pos + len).div_ceil(32);
  if pos + 4 * words_touched > data.len() || len.div_ceil(8) > dest.len() {
    return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
  }
  dest.fill(0);
  let mut i: usize = 0;
  while i < len {
    let src_bit = bit_pos + i;
    let at = pos + 4 * (src_bit / 32);
    let word =
      u32::from_ne_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]]);
    if (word >> (src_bit % 32)) & 1 != 0 {
      // Bit i of the value, in a word kept in native order.
      let dest_word = i / 32;
      let in_word = 1u32 << (i % 32);
      let bytes = in_word.to_ne_bytes();
      let mut b: usize = 0;
      while b < 4 {
        let at_dest = 4 * dest_word + b;
        if at_dest < dest.len() {
          dest[at_dest] |= bytes[b];
        }
        b += 1;
      }
    }
    i += 1;
  }
  Ok(())
}

/// Bytes, padded to whole words, as words in native order.
fn words_of(bytes: &[u8]) -> Vec<u32> {
  let mut words: Vec<u32> = Vec::with_capacity(bytes.len() / 4);
  let mut i: usize = 0;
  while i + 4 <= bytes.len() {
    words.push(u32::from_ne_bytes([
      bytes[i],
      bytes[i + 1],
      bytes[i + 2],
      bytes[i + 3],
    ]));
    i += 4;
  }
  words
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::dict_cache::TableDef;
  use ic_ndb_signals::dict_tab_info::parse_table_info;
  use ic_ndb_signals::dict_tab_info::IC_ARRAY_TYPE_FIXED;
  use ic_ndb_signals::dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_ARRAY_TYPE;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_END;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_LENGTH;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_PRECISION;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_TYPE;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_ID;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_KEY;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_NAME;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_NULLABLE;
  use ic_ndb_signals::dict_tab_info::IC_DTI_NO_OF_ATTRIBUTES;
  use ic_ndb_signals::dict_tab_info::IC_DTI_TABLE_NAME;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_BIGINT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_BIT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_INT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_TINYINT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_VARCHAR;
  use ic_ndb_signals::simple_properties::PropertyWriter;
  use std::sync::Arc;

  struct Column {
    name: &'static str,
    ext_type: u32,
    length: u32,
    key: bool,
    nullable: bool,
    charset: u32,
    // Sent by the dictionary for every variable-sized column; it is
    // what makes a value carry its length.
    array_type: u32,
  }

  /// `id INT` key, `name VARCHAR(20)` key, `t TINYINT NULL`,
  /// `big BIGINT NULL`, `b BIT(10) NULL`, `n INT`.
  const COLUMNS: [Column; 6] = [
    Column {
      name: "id",
      ext_type: IC_NDB_TYPE_INT,
      length: 1,
      key: true,
      nullable: false,
      array_type: IC_ARRAY_TYPE_FIXED,
      charset: 0,
    },
    Column {
      name: "name",
      ext_type: IC_NDB_TYPE_VARCHAR,
      length: 20,
      key: true,
      nullable: false,
      array_type: IC_ARRAY_TYPE_SHORT_VAR,
      charset: 8,
    },
    Column {
      name: "t",
      ext_type: IC_NDB_TYPE_TINYINT,
      length: 1,
      key: false,
      nullable: true,
      array_type: IC_ARRAY_TYPE_FIXED,
      charset: 0,
    },
    Column {
      name: "big",
      ext_type: IC_NDB_TYPE_BIGINT,
      length: 1,
      key: false,
      nullable: true,
      array_type: IC_ARRAY_TYPE_FIXED,
      charset: 0,
    },
    Column {
      name: "b",
      ext_type: IC_NDB_TYPE_BIT,
      length: 10,
      key: false,
      nullable: true,
      array_type: IC_ARRAY_TYPE_FIXED,
      charset: 0,
    },
    Column {
      name: "n",
      ext_type: IC_NDB_TYPE_INT,
      length: 1,
      key: false,
      nullable: false,
      array_type: IC_ARRAY_TYPE_FIXED,
      charset: 0,
    },
  ];

  fn table() -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_DTI_TABLE_NAME, "db/def/t");
    w.add_u32(IC_DTI_NO_OF_ATTRIBUTES, COLUMNS.len() as u32);
    let mut id: usize = 0;
    while id < COLUMNS.len() {
      let column = &COLUMNS[id];
      w.add_string(IC_DTI_ATTRIBUTE_NAME, column.name);
      w.add_u32(IC_DTI_ATTRIBUTE_ID, id as u32);
      w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, column.ext_type);
      w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, column.length);
      w.add_u32(IC_DTI_ATTRIBUTE_KEY, column.key as u32);
      w.add_u32(IC_DTI_ATTRIBUTE_NULLABLE, column.nullable as u32);
      w.add_u32(IC_DTI_ATTRIBUTE_EXT_PRECISION, column.charset << 16);
      w.add_u32(IC_DTI_ATTRIBUTE_ARRAY_TYPE, column.array_type);
      w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
      id += 1;
    }
    let info = parse_table_info(w.words()).expect("parsed");
    Arc::new(TableDef::new(info, None))
  }

  /// An attribute header, as the words of a write carry it.
  fn head(attr_id: u32, size: u32) -> u32 {
    attr_header::attr_header(attr_id, size)
  }

  /// Words from bytes, padding the last word with zeros.
  fn padded(bytes: &[u8]) -> Vec<u32> {
    let mut all = bytes.to_vec();
    while all.len() % 4 != 0 {
      all.push(0);
    }
    words_of(&all)
  }

  #[test]
  fn the_key_is_each_key_column_padded_to_words() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let id = rec.field(0).expect("id");
    let at = id.offset() as usize;
    row[at..at + 4].copy_from_slice(&7u32.to_le_bytes());
    // "abc" behind its length byte.
    let name = rec.field(1).expect("name");
    let at = name.offset() as usize;
    row[at..at + 4].copy_from_slice(&[3, b'a', b'b', b'c']);
    let key = key_info(&rec, &row).expect("key");
    // Four bytes of id, then four of name: the length and three bytes.
    assert_eq!(key, padded(&[7, 0, 0, 0, 3, b'a', b'b', b'c']));
  }

  #[test]
  fn an_index_key_is_the_indexed_columns_in_index_order() {
    let t = table();
    // A unique index over name: the index's own table has name as its
    // key and a hidden column after it.
    let mut w = PropertyWriter::new();
    w.add_string(IC_DTI_TABLE_NAME, "sys/def/1/uk$unique");
    w.add_u32(IC_DTI_NO_OF_ATTRIBUTES, 2);
    w.add_string(IC_DTI_ATTRIBUTE_NAME, "name");
    w.add_u32(IC_DTI_ATTRIBUTE_ID, 0);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, IC_NDB_TYPE_VARCHAR);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, 20);
    w.add_u32(IC_DTI_ATTRIBUTE_KEY, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_PRECISION, 8 << 16);
    w.add_u32(IC_DTI_ATTRIBUTE_ARRAY_TYPE, IC_ARRAY_TYPE_SHORT_VAR);
    w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
    w.add_string(IC_DTI_ATTRIBUTE_NAME, "NDB$PK");
    w.add_u32(IC_DTI_ATTRIBUTE_ID, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, IC_NDB_TYPE_BIGINT);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
    let info = parse_table_info(w.words()).expect("parsed");
    let index = IndexDef::new(info, Arc::clone(&t));
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let at = rec.field(1).expect("name").offset() as usize;
    row[at..at + 3].copy_from_slice(&[2, b'h', b'i']);
    let key = index_key_info(&index, &rec, &row).expect("key");
    assert_eq!(key, padded(&[2, b'h', b'i']));
    // A record without the indexed column cannot key the index.
    let without = Record::default_for(&t, Some(&[0])).expect("record");
    let row = vec![0u8; without.row_size() as usize];
    assert!(index_key_info(&index, &without, &row).is_err());
  }

  #[test]
  fn a_key_record_without_every_key_column_is_refused() {
    let t = table();
    let rec = Record::default_for(&t, Some(&[0])).expect("record");
    let row = vec![0u8; rec.row_size() as usize];
    let e = key_info(&rec, &row).expect_err("refused");
    assert_eq!(e.code, err::IC_ERROR_KEY_RECORD);
  }

  #[test]
  fn a_write_sends_a_header_and_value_for_each_column() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let at = |p: u32| rec.field(p).expect("field").offset() as usize;
    row[at(0)..at(0) + 4].copy_from_slice(&7u32.to_ne_bytes());
    row[at(1)..at(1) + 3].copy_from_slice(&[2, b'h', b'i']);
    rec.set_null(&mut row, 2, true).expect("t is NULL");
    row[at(3)..at(3) + 8].copy_from_slice(&5u64.to_ne_bytes());
    row[at(5)..at(5) + 4].copy_from_slice(&42u32.to_ne_bytes());
    let words = write_attr_info(&rec, &row, false).expect("values");
    let mut want: Vec<u32> = vec![head(0, 4)];
    want.extend_from_slice(&padded(&7u32.to_ne_bytes()));
    want.push(head(1, 3));
    want.extend_from_slice(&padded(&[2, b'h', b'i']));
    // A NULL column is a header of length zero, with no value.
    want.push(head(2, 0));
    want.push(head(3, 8));
    want.extend_from_slice(&padded(&5u64.to_ne_bytes()));
    want.push(head(4, 4));
    want.extend_from_slice(&padded(&[0, 0, 0, 0]));
    want.push(head(5, 4));
    want.extend_from_slice(&padded(&42u32.to_ne_bytes()));
    assert_eq!(words, want);
  }

  #[test]
  fn an_update_leaves_the_key_columns_out() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    rec.set_null(&mut row, 2, true).expect("t is NULL");
    rec.set_null(&mut row, 3, true).expect("big is NULL");
    rec.set_null(&mut row, 4, true).expect("b is NULL");
    let words = write_attr_info(&rec, &row, true).expect("values");
    // t, big and b are NULL; n is a zero of four bytes.
    let want = vec![head(2, 0), head(3, 0), head(4, 0), head(5, 4), 0];
    assert_eq!(words, want);
  }

  #[test]
  fn a_read_of_every_column_asks_for_all() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    assert_eq!(read_attr_info(&rec), vec![(0xFFF0 << 16) | 6]);
  }

  #[test]
  fn a_read_of_some_columns_asks_by_bitmask() {
    let t = table();
    let rec = Record::default_for(&t, Some(&[5, 2])).expect("record");
    assert_eq!(read_attr_info(&rec), vec![(0xFFF3 << 16) | 4, 0b100100]);
  }

  #[test]
  fn a_packed_row_is_put_where_the_record_says() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    // From bit 0 up: id read; name read; t read, then its NULL bit
    // clear; big read, then its NULL bit set; b read, then its NULL bit
    // clear; n read.
    let mask: u32 = 0b1_0111_0111;
    let mut values: Vec<u8> = Vec::new();
    // id, on a word.
    values.extend_from_slice(&7u32.to_ne_bytes());
    // name, where it falls.
    values.extend_from_slice(&[2, b'h', b'i']);
    // t, the next byte: -10.
    values.push(0xF6);
    // big is NULL and takes no room. b starts on a word, which t's byte
    // has just reached.
    values.extend_from_slice(&0b10_0000_0101u32.to_ne_bytes());
    // n goes past the word b's ten bits touched, on a word.
    values.extend_from_slice(&42u32.to_ne_bytes());
    let mut words = vec![(0xFFF3 << 16) | 4, mask];
    words.extend_from_slice(&padded(&values));
    let mut row = vec![0u8; rec.row_size() as usize];
    let used = unpack_row(&rec, &words, &mut row).expect("unpacked");
    assert_eq!(used, words.len());
    let at = |p: u32| rec.field(p).expect("field").offset() as usize;
    assert_eq!(&row[at(0)..at(0) + 4], &7u32.to_ne_bytes());
    assert_eq!(&row[at(1)..at(1) + 3], &[2, b'h', b'i']);
    assert_eq!(row[at(2)], 0xF6);
    assert!(!rec.is_null(&row, 2));
    assert!(rec.is_null(&row, 3));
    assert_eq!(&row[at(4)..at(4) + 4], &0b10_0000_0101u32.to_ne_bytes());
    assert!(!rec.is_null(&row, 4));
    assert_eq!(&row[at(5)..at(5) + 4], &42u32.to_ne_bytes());
  }

  #[test]
  fn a_row_that_is_not_packed_is_refused() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let words = [(3 << 16) | 4, 42];
    assert!(unpack_row(&rec, &words, &mut row).is_err());
  }

  #[test]
  fn a_value_longer_than_its_field_is_refused() {
    let t = table();
    let rec = Record::default_for(&t, Some(&[1])).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    // name read, with a length byte saying 30 for a 20-byte field.
    let mut values = vec![30u8];
    values.extend_from_slice(&[b'x'; 30]);
    let mut words = vec![(0xFFF3 << 16) | 4, 0b10];
    words.extend_from_slice(&padded(&values));
    let e = unpack_row(&rec, &words, &mut row).expect_err("refused");
    assert_eq!(e.code, err::IC_ERROR_VALUE_TOO_LONG);
  }
}
