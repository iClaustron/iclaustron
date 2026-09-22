// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Values as text, in and out of a row a record lays out: what a
//! command line gives and what a listing shows. New: for the tools,
//! examples and tests; an application works in the row's own bytes.
//!
//! A value is kept as the data node keeps it, so text is turned into
//! those bytes and back: an integer little-endian, as the machines
//! RonDB runs on order their words; a character or binary string as its
//! bytes, padded for a fixed-size column and behind its length for a
//! variable-sized one. The word `NULL` stands for no value.
//!
//! The types a record can hold that are neither numbers nor strings,
//! such as the temporal types and decimals, are shown as bytes and
//! cannot be given as text here; a record carries them to and from an
//! application unchanged.

use ic_ndb_signals::dict_tab_info;
use ic_ndb_signals::dict_tab_info::AttributeInfo;

use crate::record::Record;
use crate::record::RecordField;

/// The text that stands for no value.
pub const IC_NULL_TEXT: &str = "NULL";

/// Put `text` into the field at `position` of `row`. The error is a
/// sentence naming the column, for a command line to print.
pub fn put_value(
  rec: &Record,
  position: u32,
  text: &str,
  row: &mut [u8],
) -> Result<(), String> {
  let field = match rec.field(position) {
    Some(field) => field,
    None => return Err("No such field in the record".to_string()),
  };
  let attr = match rec.table().field(field.field_id()) {
    Some(attr) => attr,
    None => return Err("No such column in the table".to_string()),
  };
  if text == IC_NULL_TEXT {
    if let Err(e) = rec.set_null(row, position, true) {
      return Err(format!("{} cannot be NULL ({})", attr.name, e.code));
    }
    return Ok(());
  }
  if field.is_nullable() {
    let _ = rec.set_null(row, position, false);
  }
  put_bytes(attr, field, text, row)
}

/// The value of the field at `position` of `row`, as text.
pub fn value_text(rec: &Record, position: u32, row: &[u8]) -> String {
  let field = match rec.field(position) {
    Some(field) => field,
    None => return String::new(),
  };
  if rec.is_null(row, position) {
    return IC_NULL_TEXT.to_string();
  }
  let attr = match rec.table().field(field.field_id()) {
    Some(attr) => attr,
    None => return String::new(),
  };
  let at = field.offset() as usize;
  format_value(attr, &row[at..at + field.size() as usize])
}

/// True for a column this module can take text for.
pub fn takes_text(attr: &AttributeInfo) -> bool {
  is_integer(attr) || is_string(attr)
}

fn is_integer(attr: &AttributeInfo) -> bool {
  let integers =
    dict_tab_info::IC_NDB_TYPE_TINYINT..=dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED;
  integers.contains(&attr.ext_type)
}

fn is_signed(attr: &AttributeInfo) -> bool {
  matches!(
    attr.ext_type,
    dict_tab_info::IC_NDB_TYPE_TINYINT
      | dict_tab_info::IC_NDB_TYPE_SMALLINT
      | dict_tab_info::IC_NDB_TYPE_MEDIUMINT
      | dict_tab_info::IC_NDB_TYPE_INT
      | dict_tab_info::IC_NDB_TYPE_BIGINT
  )
}

fn is_string(attr: &AttributeInfo) -> bool {
  matches!(
    attr.ext_type,
    dict_tab_info::IC_NDB_TYPE_CHAR
      | dict_tab_info::IC_NDB_TYPE_BINARY
      | dict_tab_info::IC_NDB_TYPE_VARCHAR
      | dict_tab_info::IC_NDB_TYPE_VARBINARY
      | dict_tab_info::IC_NDB_TYPE_LONGVARCHAR
      | dict_tab_info::IC_NDB_TYPE_LONGVARBINARY
  )
}

/// The bytes of a value, into its place in the row.
fn put_bytes(
  attr: &AttributeInfo,
  field: &RecordField,
  text: &str,
  row: &mut [u8],
) -> Result<(), String> {
  let at = field.offset() as usize;
  let size = field.size() as usize;
  let bytes = text.as_bytes();
  if is_integer(attr) {
    if size == 0 || size > 8 {
      return Err(format!("{} is an array of integers", attr.name));
    }
    let value = parse_integer(attr, text, size)?;
    row[at..at + size].copy_from_slice(&value.to_le_bytes()[..size]);
    return Ok(());
  }
  if !is_string(attr) {
    return Err(format!(
      "{} is of type {}, which is not given as text",
      attr.name,
      dict_tab_info::type_name(attr.ext_type)
    ));
  }
  let head = match attr.array_type {
    dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR => 1,
    dict_tab_info::IC_ARRAY_TYPE_MEDIUM_VAR => 2,
    _ => 0,
  };
  if bytes.len() + head > size {
    return Err(format!("{} is too long for {}", text, attr.name));
  }
  if head == 0 {
    // A fixed-size column is kept padded: characters with spaces,
    // bytes with zeros.
    let mut pad: u8 = 0;
    if attr.ext_type == dict_tab_info::IC_NDB_TYPE_CHAR {
      pad = b' ';
    }
    row[at..at + size].fill(pad);
  } else if head == 1 {
    row[at] = bytes.len() as u8;
  } else {
    let len = (bytes.len() as u16).to_le_bytes();
    row[at..at + 2].copy_from_slice(&len);
  }
  row[at + head..at + head + bytes.len()].copy_from_slice(bytes);
  Ok(())
}

/// An integer that fits the column, as the bits to store.
fn parse_integer(
  attr: &AttributeInfo,
  text: &str,
  size: usize,
) -> Result<u64, String> {
  if !is_signed(attr) {
    let value: u64 = match text.parse() {
      Ok(value) => value,
      Err(_) => return Err(format!("{} is not a whole number", text)),
    };
    if size < 8 && value >> (8 * size) != 0 {
      return Err(format!("{} does not fit {}", text, attr.name));
    }
    return Ok(value);
  }
  let value: i64 = match text.parse() {
    Ok(value) => value,
    Err(_) => return Err(format!("{} is not a whole number", text)),
  };
  // Above the column's sign bit there must be nothing but sign.
  let top = value >> (8 * size - 1);
  if size < 8 && top != 0 && top != -1 {
    return Err(format!("{} does not fit {}", text, attr.name));
  }
  Ok(value as u64)
}

/// A value's bytes as text.
fn format_value(attr: &AttributeInfo, bytes: &[u8]) -> String {
  if is_integer(attr) {
    if is_signed(attr) {
      return signed_of(bytes).to_string();
    }
    return unsigned_of(bytes).to_string();
  }
  match attr.ext_type {
    dict_tab_info::IC_NDB_TYPE_YEAR => unsigned_of(bytes).to_string(),
    dict_tab_info::IC_NDB_TYPE_FLOAT if bytes.len() == 4 => {
      f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]).to_string()
    }
    dict_tab_info::IC_NDB_TYPE_DOUBLE if bytes.len() == 8 => {
      let mut eight = [0u8; 8];
      eight.copy_from_slice(bytes);
      f64::from_le_bytes(eight).to_string()
    }
    _ => string_or_bytes(attr, bytes),
  }
}

/// A character string as text, a binary one as bytes.
fn string_or_bytes(attr: &AttributeInfo, bytes: &[u8]) -> String {
  if !is_string(attr) {
    return hex_of(bytes);
  }
  let head = match attr.array_type {
    dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR => 1,
    dict_tab_info::IC_ARRAY_TYPE_MEDIUM_VAR => 2,
    _ => 0,
  };
  if bytes.len() < head {
    return hex_of(bytes);
  }
  let len = match head {
    1 => bytes[0] as usize,
    2 => bytes[0] as usize + 256 * bytes[1] as usize,
    _ => bytes.len(),
  };
  if head + len > bytes.len() {
    return hex_of(bytes);
  }
  let value = &bytes[head..head + len];
  if !attr.is_char_type() {
    return hex_of(value);
  }
  let text = String::from_utf8_lossy(value);
  // A fixed-size character column is kept padded with spaces.
  format!("'{}'", text.trim_end_matches(' '))
}

/// A little-endian integer of up to eight bytes, sign extended.
fn signed_of(bytes: &[u8]) -> i64 {
  let unsigned = unsigned_of(bytes);
  let bits = 8 * bytes.len().min(8);
  if bits == 0 || bits == 64 {
    return unsigned as i64;
  }
  let shift = 64 - bits;
  ((unsigned << shift) as i64) >> shift
}

/// A little-endian unsigned integer of up to eight bytes.
fn unsigned_of(bytes: &[u8]) -> u64 {
  let mut value: u64 = 0;
  let mut i = bytes.len().min(8);
  while i > 0 {
    i -= 1;
    value = (value << 8) | bytes[i] as u64;
  }
  value
}

fn hex_of(bytes: &[u8]) -> String {
  let mut text = String::from("0x");
  for byte in bytes {
    text.push_str(&format!("{:02x}", byte));
  }
  text
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::dict_cache::TableDef;
  use ic_ndb_signals::dict_tab_info::parse_table_info;
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
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_CHAR;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_DATE;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_INT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_TINYINT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_VARCHAR;
  use ic_ndb_signals::simple_properties::PropertyWriter;
  use std::sync::Arc;

  /// `id INT` key, `t TINYINT NULL`, `c CHAR(4)`, `v VARCHAR(8) NULL`,
  /// `d DATE NULL`.
  fn table() -> Arc<TableDef> {
    let types = [
      (IC_NDB_TYPE_INT, 1, true, false, 0, 0),
      (IC_NDB_TYPE_TINYINT, 1, false, true, 0, 0),
      (IC_NDB_TYPE_CHAR, 4, false, false, 8, 0),
      (
        IC_NDB_TYPE_VARCHAR,
        8,
        false,
        true,
        8,
        IC_ARRAY_TYPE_SHORT_VAR,
      ),
      (IC_NDB_TYPE_DATE, 1, false, true, 0, 0),
    ];
    let names = ["id", "t", "c", "v", "d"];
    let mut w = PropertyWriter::new();
    w.add_string(IC_DTI_TABLE_NAME, "db/def/t");
    w.add_u32(IC_DTI_NO_OF_ATTRIBUTES, types.len() as u32);
    let mut i: usize = 0;
    while i < types.len() {
      let (ext_type, length, key, nullable, charset, array_type) = types[i];
      w.add_string(IC_DTI_ATTRIBUTE_NAME, names[i]);
      w.add_u32(IC_DTI_ATTRIBUTE_ID, i as u32);
      w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, ext_type);
      w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, length);
      w.add_u32(IC_DTI_ATTRIBUTE_KEY, key as u32);
      w.add_u32(IC_DTI_ATTRIBUTE_NULLABLE, nullable as u32);
      w.add_u32(IC_DTI_ATTRIBUTE_EXT_PRECISION, charset << 16);
      w.add_u32(IC_DTI_ATTRIBUTE_ARRAY_TYPE, array_type);
      w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
      i += 1;
    }
    let info = parse_table_info(w.words()).expect("parsed");
    Arc::new(TableDef::new(info, None))
  }

  #[test]
  fn numbers_go_in_and_come_back() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    put_value(&rec, 0, "-7", &mut row).expect("id");
    put_value(&rec, 1, "-128", &mut row).expect("t");
    assert_eq!(value_text(&rec, 0, &row), "-7");
    assert_eq!(value_text(&rec, 1, &row), "-128");
  }

  #[test]
  fn a_number_too_big_for_its_column_is_refused() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    assert!(put_value(&rec, 1, "128", &mut row).is_err());
    assert!(put_value(&rec, 1, "-129", &mut row).is_err());
    assert!(put_value(&rec, 0, "seven", &mut row).is_err());
  }

  #[test]
  fn strings_keep_their_padding_and_length() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    put_value(&rec, 2, "ab", &mut row).expect("c");
    put_value(&rec, 3, "hello", &mut row).expect("v");
    let c = rec.field(2).expect("c").offset() as usize;
    assert_eq!(&row[c..c + 4], b"ab  ");
    let v = rec.field(3).expect("v").offset() as usize;
    assert_eq!(row[v], 5);
    assert_eq!(&row[v + 1..v + 6], b"hello");
    assert_eq!(value_text(&rec, 2, &row), "'ab'");
    assert_eq!(value_text(&rec, 3, &row), "'hello'");
    // Eight bytes and a length byte are all the field holds.
    put_value(&rec, 3, "12345678", &mut row).expect("just fits");
    assert!(put_value(&rec, 3, "123456789", &mut row).is_err());
  }

  #[test]
  fn null_goes_in_and_comes_back() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    put_value(&rec, 3, "hello", &mut row).expect("v");
    assert_eq!(value_text(&rec, 3, &row), "'hello'");
    put_value(&rec, 3, IC_NULL_TEXT, &mut row).expect("null");
    assert_eq!(value_text(&rec, 3, &row), "NULL");
    // A column that cannot be NULL says so.
    assert!(put_value(&rec, 0, IC_NULL_TEXT, &mut row).is_err());
  }

  #[test]
  fn a_type_without_text_is_shown_as_bytes() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let d = rec.field(4).expect("d").offset() as usize;
    row[d..d + 3].copy_from_slice(&[1, 2, 3]);
    let _ = rec.set_null(&mut row, 4, false);
    assert_eq!(value_text(&rec, 4, &row), "0x010203");
    assert!(put_value(&rec, 4, "2026-09-22", &mut row).is_err());
    let date = t.field(4).expect("date column");
    assert!(!takes_text(date));
  }
}
