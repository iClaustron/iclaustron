// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Values as text, in and out of a row a record lays out: what a
//! command line gives and what a listing shows. New: for the tools,
//! examples and tests; an application works in the row's own bytes.
//!
//! A value is kept as the data node keeps it, so text is turned into
//! those bytes and back: an integer little-endian, as the machines
//! RonDB runs on order their words; a binary string as its bytes; a
//! character string in its column's character set. A fixed-size column
//! is padded, a character one with its set's spaces and a binary one
//! with zeros, and a variable-sized one has its length in front. The
//! word `NULL` stands for no value.
//!
//! **Character sets** are converted through tables compiled in from
//! public standards (chapter 09): UTF-8 for utf8mb4, and for utf8mb3
//! the characters of the Basic Multilingual Plane; UTF-16 big-endian
//! for utf16 and little-endian for utf16le; UCS-2 and UTF-32,
//! big-endian; and for latin1 the Windows code page 1252, as Unicode
//! publishes it in
//! <https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1252.TXT>,
//! with the five bytes it leaves unassigned taken as the control
//! characters of the same numbers, as MySQL's latin1 takes them
//! (chapter 05 §6.3). Which set a column is in comes from the data
//! nodes: binding a table describes each character column's collation,
//! once for the process (chapter 06 §8.6). A column of another set is
//! shown in hexadecimal and given the same way, `0x` and two digits a
//! byte; a fixed-size one is padded with the byte the data node names.
//! A column whose collation could not be described is shown in
//! hexadecimal and takes no text. A character a set cannot hold is
//! refused, and bytes that are not a string of the set are shown in
//! hexadecimal.
//!
//! The types a record can hold that are neither numbers nor strings,
//! such as the temporal types and decimals, are shown as bytes and
//! cannot be given as text here; a record carries them to and from an
//! application unchanged.

use ic_signals::collation_desc;
use ic_signals::collation_desc::CollationDescConf;
use ic_signals::table_props;
use ic_signals::table_props::ColumnProps;

use crate::record::Record;
use crate::record::RecordField;

/// The text that stands for no value.
pub const IC_NULL_TEXT: &str = "NULL";

/// What CP1252 makes of the bytes 0x80 to 0x9F; from 0xA0 on, and
/// below 0x80, a byte is the code point of the same number. The five
/// it leaves unassigned, 0x81, 0x8D, 0x8F, 0x90 and 0x9D, stand for
/// themselves.
const IC_CP1252_HIGH: [u16; 32] = [
  0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6,
  0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018,
  0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161,
  0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
];

/// How a character column's bytes are turned into text and back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextEncoding {
  /// UTF-8: utf8mb4.
  Utf8,
  /// UTF-8 of the Basic Multilingual Plane only: utf8mb3.
  Utf8Bmp,
  /// UTF-16, big-endian: utf16.
  Utf16Big,
  /// UTF-16, little-endian: utf16le.
  Utf16Little,
  /// Two bytes a character, big-endian, the Basic Multilingual Plane
  /// only: ucs2.
  Ucs2,
  /// Four bytes a character, big-endian: utf32.
  Utf32,
  /// The Windows code page 1252: latin1.
  Cp1252,
  /// A set with no table here: its bytes in hexadecimal, a fixed-size
  /// value padded with `pad`.
  Bytes {
    /// The byte the data node pads a fixed-size value with.
    pad: u8,
  },
}

impl TextEncoding {
  /// The encoding of a space, which a fixed-size value is padded with.
  fn space(&self) -> Vec<u8> {
    match self {
      TextEncoding::Utf16Big | TextEncoding::Ucs2 => vec![0x00, 0x20],
      TextEncoding::Utf16Little => vec![0x20, 0x00],
      TextEncoding::Utf32 => vec![0x00, 0x00, 0x00, 0x20],
      TextEncoding::Bytes { pad } => vec![*pad],
      _ => vec![0x20],
    }
  }
}

/// How the text of a column in the collation `desc` describes is
/// converted: by the name of its character set, through a table
/// compiled in, or as bytes.
pub fn encoding_of(desc: &CollationDescConf) -> TextEncoding {
  let name = desc.charset_name.as_str();
  if (desc.flags & collation_desc::IC_COLLATION_UNICODE) != 0 {
    match name {
      "utf8mb4" => return TextEncoding::Utf8,
      "utf8mb3" | "utf8" => return TextEncoding::Utf8Bmp,
      "utf16" => return TextEncoding::Utf16Big,
      "utf16le" => return TextEncoding::Utf16Little,
      "ucs2" => return TextEncoding::Ucs2,
      "utf32" => return TextEncoding::Utf32,
      _ => {}
    }
  } else if name == "latin1" {
    return TextEncoding::Cp1252;
  }
  TextEncoding::Bytes {
    pad: desc.pad_byte as u8,
  }
}

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
  let encoding = rec.table().text_encoding(field.field_id());
  put_bytes(attr, field, encoding, text, row)
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
  let encoding = rec.table().text_encoding(field.field_id());
  format_value(attr, encoding, &row[at..at + field.size() as usize])
}

/// True for a column this module can take text for.
pub fn takes_text(attr: &ColumnProps) -> bool {
  is_integer(attr) || is_string(attr)
}

fn is_integer(attr: &ColumnProps) -> bool {
  let integers =
    table_props::IC_TYPE_TINYINT..=table_props::IC_TYPE_BIGUNSIGNED;
  integers.contains(&attr.type_code)
}

fn is_signed(attr: &ColumnProps) -> bool {
  matches!(
    attr.type_code,
    table_props::IC_TYPE_TINYINT
      | table_props::IC_TYPE_SMALLINT
      | table_props::IC_TYPE_MEDIUMINT
      | table_props::IC_TYPE_INT
      | table_props::IC_TYPE_BIGINT
  )
}

fn is_string(attr: &ColumnProps) -> bool {
  matches!(
    attr.type_code,
    table_props::IC_TYPE_CHAR
      | table_props::IC_TYPE_BINARY
      | table_props::IC_TYPE_VARCHAR
      | table_props::IC_TYPE_VARBINARY
      | table_props::IC_TYPE_LONGVARCHAR
      | table_props::IC_TYPE_LONGVARBINARY
  )
}

/// The bytes of a value, into its place in the row.
fn put_bytes(
  attr: &ColumnProps,
  field: &RecordField,
  encoding: Option<TextEncoding>,
  text: &str,
  row: &mut [u8],
) -> Result<(), String> {
  let at = field.offset() as usize;
  let size = field.size() as usize;
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
      table_props::type_name(attr.type_code)
    ));
  }
  // A binary string is its bytes, padded with zeros.
  let mut space: Vec<u8> = vec![0];
  let mut encoded: Vec<u8> = Vec::new();
  let mut bytes = text.as_bytes();
  if attr.is_char_type() {
    let known = match encoding {
      Some(known) => known,
      None => {
        return Err(format!(
          "{} is in a character set not known: its collation was not \
           described",
          attr.name
        ))
      }
    };
    encode(attr, known, text, &mut encoded)?;
    bytes = &encoded;
    space = known.space();
  }
  let head = match attr.array_kind {
    table_props::IC_ARRAY_KIND_SHORT_VARSIZE => 1,
    table_props::IC_ARRAY_KIND_LONG_VARSIZE => 2,
    _ => 0,
  };
  if bytes.len() + head > size {
    return Err(format!("{} is too long for {}", text, attr.name));
  }
  if head == 0 {
    // Spaces from the field's start line up with the value's end, as
    // the field and the value are each a whole number of the set's
    // units.
    for (i, byte) in row[at..at + size].iter_mut().enumerate() {
      *byte = space[i % space.len()];
    }
  } else if head == 1 {
    row[at] = bytes.len() as u8;
  } else {
    let len = (bytes.len() as u16).to_le_bytes();
    row[at..at + 2].copy_from_slice(&len);
  }
  row[at + head..at + head + bytes.len()].copy_from_slice(bytes);
  Ok(())
}

/// A character string's text in its set's bytes, into `out`.
fn encode(
  attr: &ColumnProps,
  encoding: TextEncoding,
  text: &str,
  out: &mut Vec<u8>,
) -> Result<(), String> {
  if let TextEncoding::Bytes { .. } = encoding {
    return match bytes_of_hex(text) {
      Some(bytes) => {
        out.extend_from_slice(&bytes);
        Ok(())
      }
      None => Err(format!(
        "{} is in a character set with no table here: give its bytes as \
         0x and two hexadecimal digits a byte",
        attr.name
      )),
    };
  }
  if encoding == TextEncoding::Utf8 {
    out.extend_from_slice(text.as_bytes());
    return Ok(());
  }
  let mut units = [0u16; 2];
  for c in text.chars() {
    let code = c as u32;
    let held = match encoding {
      TextEncoding::Utf8Bmp | TextEncoding::Ucs2 if code > 0xFFFF => false,
      TextEncoding::Utf8Bmp => {
        let mut utf8 = [0u8; 4];
        out.extend_from_slice(c.encode_utf8(&mut utf8).as_bytes());
        true
      }
      TextEncoding::Utf16Big | TextEncoding::Ucs2 => {
        for unit in c.encode_utf16(&mut units).iter() {
          out.extend_from_slice(&unit.to_be_bytes());
        }
        true
      }
      TextEncoding::Utf16Little => {
        for unit in c.encode_utf16(&mut units).iter() {
          out.extend_from_slice(&unit.to_le_bytes());
        }
        true
      }
      TextEncoding::Utf32 => {
        out.extend_from_slice(&code.to_be_bytes());
        true
      }
      _ => match cp1252_byte(code) {
        Some(byte) => {
          out.push(byte);
          true
        }
        None => false,
      },
    };
    if !held {
      return Err(format!(
        "{} cannot hold {:?}: its character set has no such character",
        attr.name, c
      ));
    }
  }
  Ok(())
}

/// The CP1252 byte of a code point, if it has one.
fn cp1252_byte(code: u32) -> Option<u8> {
  if code < 0x80 || (0xA0..=0xFF).contains(&code) {
    return Some(code as u8);
  }
  let place = IC_CP1252_HIGH
    .iter()
    .position(|high| *high as u32 == code)?;
  Some(0x80 + place as u8)
}

/// A character string's bytes as text, or `None` for bytes that are
/// not a string of the set.
fn decode(encoding: TextEncoding, bytes: &[u8]) -> Option<String> {
  match encoding {
    TextEncoding::Utf8 | TextEncoding::Utf8Bmp => {
      let text = std::str::from_utf8(bytes).ok()?;
      let bmp = encoding == TextEncoding::Utf8Bmp;
      if bmp && text.chars().any(|c| c as u32 > 0xFFFF) {
        return None;
      }
      Some(text.to_string())
    }
    TextEncoding::Bytes { .. } => None,
    _ => decode_units(encoding, bytes),
  }
}

/// The text of a string in units of one size: UTF-16 and UCS-2 of two
/// bytes, UTF-32 of four, CP1252 of one. A pair of UTF-16's surrogates
/// is one character; a surrogate alone is none.
fn decode_units(encoding: TextEncoding, bytes: &[u8]) -> Option<String> {
  let unit = match encoding {
    TextEncoding::Utf32 => 4,
    TextEncoding::Cp1252 => 1,
    _ => 2,
  };
  if bytes.len() % unit != 0 {
    return None;
  }
  let pairs =
    encoding == TextEncoding::Utf16Big || encoding == TextEncoding::Utf16Little;
  let mut text = String::with_capacity(bytes.len());
  let mut at: usize = 0;
  while at < bytes.len() {
    let mut code = unit_at(encoding, bytes, at);
    at += unit;
    if pairs && (0xD800..0xDC00).contains(&code) && at < bytes.len() {
      let low = unit_at(encoding, bytes, at);
      if (0xDC00..0xE000).contains(&low) {
        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        at += unit;
      }
    }
    text.push(char::from_u32(code)?);
  }
  Some(text)
}

/// The unit at `at` as a number: a code point, or half a surrogate
/// pair.
fn unit_at(encoding: TextEncoding, bytes: &[u8], at: usize) -> u32 {
  match encoding {
    TextEncoding::Utf16Little => {
      u16::from_le_bytes([bytes[at], bytes[at + 1]]) as u32
    }
    TextEncoding::Utf32 => u32::from_be_bytes([
      bytes[at],
      bytes[at + 1],
      bytes[at + 2],
      bytes[at + 3],
    ]),
    TextEncoding::Cp1252 => {
      let byte = bytes[at] as u32;
      if (0x80..0xA0).contains(&byte) {
        return IC_CP1252_HIGH[(byte - 0x80) as usize] as u32;
      }
      byte
    }
    _ => u16::from_be_bytes([bytes[at], bytes[at + 1]]) as u32,
  }
}

/// Bytes given as `0x` and two hexadecimal digits a byte.
fn bytes_of_hex(text: &str) -> Option<Vec<u8>> {
  let digits = text.strip_prefix("0x")?.as_bytes();
  if digits.len() % 2 != 0 {
    return None;
  }
  let mut bytes: Vec<u8> = Vec::with_capacity(digits.len() / 2);
  for pair in digits.chunks_exact(2) {
    let high = (pair[0] as char).to_digit(16)?;
    let low = (pair[1] as char).to_digit(16)?;
    bytes.push((high * 16 + low) as u8);
  }
  Some(bytes)
}

/// An integer that fits the column, as the bits to store.
fn parse_integer(
  attr: &ColumnProps,
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
fn format_value(
  attr: &ColumnProps,
  encoding: Option<TextEncoding>,
  bytes: &[u8],
) -> String {
  if is_integer(attr) {
    if is_signed(attr) {
      return signed_of(bytes).to_string();
    }
    return unsigned_of(bytes).to_string();
  }
  match attr.type_code {
    table_props::IC_TYPE_YEAR => unsigned_of(bytes).to_string(),
    table_props::IC_TYPE_FLOAT if bytes.len() == 4 => {
      f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]).to_string()
    }
    table_props::IC_TYPE_DOUBLE if bytes.len() == 8 => {
      let mut eight = [0u8; 8];
      eight.copy_from_slice(bytes);
      f64::from_le_bytes(eight).to_string()
    }
    _ => string_or_bytes(attr, encoding, bytes),
  }
}

/// A character string as text, a binary one as bytes.
fn string_or_bytes(
  attr: &ColumnProps,
  encoding: Option<TextEncoding>,
  bytes: &[u8],
) -> String {
  if !is_string(attr) {
    return hex_of(bytes);
  }
  let head = match attr.array_kind {
    table_props::IC_ARRAY_KIND_SHORT_VARSIZE => 1,
    table_props::IC_ARRAY_KIND_LONG_VARSIZE => 2,
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
  let mut value = &bytes[head..head + len];
  let known = match encoding {
    Some(known) if attr.is_char_type() => known,
    _ => return hex_of(value),
  };
  if head == 0 {
    // A fixed-size character column is kept padded with spaces.
    let space = known.space();
    while value.len() >= space.len() && value.ends_with(&space) {
      value = &value[..value.len() - space.len()];
    }
  }
  match decode(known, value) {
    Some(text) => format!("'{}'", text),
    None => hex_of(value),
  }
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
  use ic_signals::prop_stream::PropertyWriter;
  use ic_signals::table_props::parse_table_info;
  use ic_signals::table_props::IC_ARRAY_KIND_SHORT_VARSIZE;
  use ic_signals::table_props::IC_TP_COLUMN_ARRAY_KIND;
  use ic_signals::table_props::IC_TP_COLUMN_COUNT;
  use ic_signals::table_props::IC_TP_COLUMN_END;
  use ic_signals::table_props::IC_TP_COLUMN_ID;
  use ic_signals::table_props::IC_TP_COLUMN_IN_KEY;
  use ic_signals::table_props::IC_TP_COLUMN_LENGTH;
  use ic_signals::table_props::IC_TP_COLUMN_NAME;
  use ic_signals::table_props::IC_TP_COLUMN_NULLABLE;
  use ic_signals::table_props::IC_TP_COLUMN_PRECISION;
  use ic_signals::table_props::IC_TP_COLUMN_TYPE;
  use ic_signals::table_props::IC_TP_TABLE_NAME;
  use ic_signals::table_props::IC_TYPE_BINARY;
  use ic_signals::table_props::IC_TYPE_CHAR;
  use ic_signals::table_props::IC_TYPE_DATE;
  use ic_signals::table_props::IC_TYPE_INT;
  use ic_signals::table_props::IC_TYPE_TINYINT;
  use ic_signals::table_props::IC_TYPE_VARCHAR;
  use std::sync::Arc;

  /// A column: its name, type, length in bytes, whether in the key,
  /// whether nullable, its collation number and its array kind.
  type Column = (&'static str, u32, u32, bool, bool, u32, u32);

  /// A table of `columns`, its character columns in `encoding`.
  fn described(
    columns: &[Column],
    encoding: Option<TextEncoding>,
  ) -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "db/def/t");
    w.add_u32(IC_TP_COLUMN_COUNT, columns.len() as u32);
    let mut encodings: Vec<Option<TextEncoding>> = Vec::new();
    for (i, column) in columns.iter().enumerate() {
      let (name, type_code, length, key, nullable, charset, array_kind) =
        *column;
      w.add_string(IC_TP_COLUMN_NAME, name);
      w.add_u32(IC_TP_COLUMN_ID, i as u32);
      w.add_u32(IC_TP_COLUMN_TYPE, type_code);
      w.add_u32(IC_TP_COLUMN_LENGTH, length);
      w.add_u32(IC_TP_COLUMN_IN_KEY, key as u32);
      w.add_u32(IC_TP_COLUMN_NULLABLE, nullable as u32);
      w.add_u32(IC_TP_COLUMN_PRECISION, charset << 16);
      w.add_u32(IC_TP_COLUMN_ARRAY_KIND, array_kind);
      w.add_u32(IC_TP_COLUMN_END, 0);
      encodings.push(if charset == 0 { None } else { encoding });
    }
    let info = parse_table_info(w.words()).expect("parsed");
    Arc::new(TableDef::new(info, None).with_text_encodings(encodings))
  }

  /// `id INT` key, `t TINYINT NULL`, `c CHAR(4)`, `v VARCHAR(8) NULL`,
  /// `d DATE NULL`, the strings in latin1.
  fn table() -> Arc<TableDef> {
    let short = IC_ARRAY_KIND_SHORT_VARSIZE;
    let columns: [Column; 5] = [
      ("id", IC_TYPE_INT, 1, true, false, 0, 0),
      ("t", IC_TYPE_TINYINT, 1, false, true, 0, 0),
      ("c", IC_TYPE_CHAR, 4, false, false, 8, 0),
      ("v", IC_TYPE_VARCHAR, 8, false, true, 8, short),
      ("d", IC_TYPE_DATE, 1, false, true, 0, 0),
    ];
    described(&columns, Some(TextEncoding::Cp1252))
  }

  /// `id INT` key, `c CHAR` of 16 bytes, `v VARCHAR` of 32, `b BINARY`
  /// of 4, the strings in `encoding`.
  fn strings_in(encoding: Option<TextEncoding>) -> Arc<TableDef> {
    let short = IC_ARRAY_KIND_SHORT_VARSIZE;
    let columns: [Column; 4] = [
      ("id", IC_TYPE_INT, 1, true, false, 0, 0),
      ("c", IC_TYPE_CHAR, 16, false, false, 45, 0),
      ("v", IC_TYPE_VARCHAR, 32, false, false, 45, short),
      ("b", IC_TYPE_BINARY, 4, false, false, 0, 0),
    ];
    described(&columns, encoding)
  }

  /// A field's bytes in `row`.
  fn bytes_at<'a>(rec: &Record, position: u32, row: &'a [u8]) -> &'a [u8] {
    let field = rec.field(position).expect("the field");
    let at = field.offset() as usize;
    &row[at..at + field.size() as usize]
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

  #[test]
  fn latin1_is_code_page_1252_every_byte() {
    let t = table();
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let mut byte: u32 = 1;
    while byte < 256 {
      let text = decode(TextEncoding::Cp1252, &[byte as u8]).expect("a char");
      let mut back: Vec<u8> = Vec::new();
      encode(
        t.field(3).expect("v"),
        TextEncoding::Cp1252,
        &text,
        &mut back,
      )
      .expect("every character goes back");
      assert_eq!(back, vec![byte as u8], "byte {:#x}", byte);
      byte += 1;
    }
    for (byte, code) in [
      (0x41u8, 0x41u32),
      (0x80, 0x20AC),
      (0x81, 0x81),
      (0x8A, 0x160),
      (0x9D, 0x9D),
      (0x9F, 0x178),
      (0xA0, 0xA0),
      (0xE9, 0xE9),
      (0xFF, 0xFF),
    ] {
      let text = decode(TextEncoding::Cp1252, &[byte]).expect("a char");
      assert_eq!(text.chars().next().map(|c| c as u32), Some(code));
    }
    put_value(&rec, 3, "café €", &mut row).expect("v");
    assert_eq!(&bytes_at(&rec, 3, &row)[..7], b"\x06caf\xe9 \x80");
    assert_eq!(value_text(&rec, 3, &row), "'café €'");
    let e = put_value(&rec, 3, "Łódź", &mut row).expect_err("not latin1");
    assert!(e.contains("cannot hold"), "{}", e);
  }

  #[test]
  fn unicode_sets_go_in_and_come_back() {
    let all = "aé漢😀";
    let cases: [(TextEncoding, &[u8]); 4] = [
      (TextEncoding::Utf8, "aé漢😀".as_bytes()),
      (
        TextEncoding::Utf16Big,
        b"\x00\x61\x00\xe9\x6f\x22\xd8\x3d\xde\x00",
      ),
      (
        TextEncoding::Utf16Little,
        b"\x61\x00\xe9\x00\x22\x6f\x3d\xd8\x00\xde",
      ),
      (
        TextEncoding::Utf32,
        b"\x00\x00\x00\x61\x00\x00\x00\xe9\x00\x00\x6f\x22\x00\x01\xf6\x00",
      ),
    ];
    for (encoding, bytes) in cases {
      let t = strings_in(Some(encoding));
      let rec = Record::default_for(&t, None).expect("record");
      let mut row = vec![0u8; rec.row_size() as usize];
      put_value(&rec, 2, all, &mut row).expect("v");
      let v = bytes_at(&rec, 2, &row);
      assert_eq!(v[0] as usize, bytes.len(), "{:?}", encoding);
      assert_eq!(&v[1..1 + bytes.len()], bytes, "{:?}", encoding);
      assert_eq!(value_text(&rec, 2, &row), format!("'{}'", all));
    }
    // The sets of the Basic Multilingual Plane hold the rest.
    for encoding in [TextEncoding::Utf8Bmp, TextEncoding::Ucs2] {
      let t = strings_in(Some(encoding));
      let rec = Record::default_for(&t, None).expect("record");
      let mut row = vec![0u8; rec.row_size() as usize];
      let e = put_value(&rec, 2, all, &mut row).expect_err("no 😀");
      assert!(e.contains("cannot hold"), "{}", e);
      put_value(&rec, 2, "aé漢", &mut row).expect("v");
      assert_eq!(value_text(&rec, 2, &row), "'aé漢'");
    }
    // A lone surrogate is no character of UCS-2, nor of UTF-16.
    let t = strings_in(Some(TextEncoding::Ucs2));
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    put_value(&rec, 2, "a", &mut row).expect("v");
    let at = rec.field(2).expect("v").offset() as usize;
    row[at..at + 3].copy_from_slice(&[2, 0xD8, 0x3D]);
    assert_eq!(value_text(&rec, 2, &row), "0xd83d");
  }

  #[test]
  fn a_fixed_size_value_is_padded_with_its_sets_spaces() {
    let spaces: [(TextEncoding, &[u8]); 4] = [
      (TextEncoding::Utf8, b"\x20"),
      (TextEncoding::Utf16Big, b"\x00\x20"),
      (TextEncoding::Utf16Little, b"\x20\x00"),
      (TextEncoding::Utf32, b"\x00\x00\x00\x20"),
    ];
    for (encoding, space) in spaces {
      let t = strings_in(Some(encoding));
      let rec = Record::default_for(&t, None).expect("record");
      let mut row = vec![0u8; rec.row_size() as usize];
      put_value(&rec, 1, "ab", &mut row).expect("c");
      let c = bytes_at(&rec, 1, &row);
      let tail = &c[c.len() - space.len()..];
      assert_eq!(tail, space, "{:?}", encoding);
      assert_eq!(value_text(&rec, 1, &row), "'ab'", "{:?}", encoding);
    }
    // A variable-sized value keeps its trailing spaces; a binary one is
    // padded with zeros.
    let t = strings_in(Some(TextEncoding::Utf8));
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    put_value(&rec, 2, "a  ", &mut row).expect("v");
    assert_eq!(value_text(&rec, 2, &row), "'a  '");
    put_value(&rec, 3, "x", &mut row).expect("b");
    assert_eq!(bytes_at(&rec, 3, &row), b"x\0\0\0");
    assert_eq!(value_text(&rec, 3, &row), "0x78000000");
  }

  #[test]
  fn a_set_with_no_table_is_given_and_shown_in_hex() {
    let t = strings_in(Some(TextEncoding::Bytes { pad: 0x20 }));
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    put_value(&rec, 1, "0xc4E0", &mut row).expect("c");
    let c = bytes_at(&rec, 1, &row);
    assert_eq!(&c[..3], b"\xc4\xe0\x20");
    assert_eq!(value_text(&rec, 1, &row), "0xc4e0");
    put_value(&rec, 2, "0x", &mut row).expect("an empty v");
    assert_eq!(value_text(&rec, 2, &row), "0x");
    for text in ["Да", "0xc4e", "0xzz"] {
      let e = put_value(&rec, 2, text, &mut row).expect_err("not hex");
      assert!(e.contains("hexadecimal"), "{}", e);
    }
    // A column whose collation was not described takes no text, and is
    // shown as its bytes.
    let t = strings_in(None);
    let rec = Record::default_for(&t, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    let e = put_value(&rec, 2, "a", &mut row).expect_err("not known");
    assert!(e.contains("not known"), "{}", e);
    let at = rec.field(2).expect("v").offset() as usize;
    row[at..at + 2].copy_from_slice(&[1, b'a']);
    assert_eq!(value_text(&rec, 2, &row), "0x61");
  }

  #[test]
  fn a_description_names_the_encoding() {
    let mut desc = CollationDescConf {
      flags: collation_desc::IC_COLLATION_UNICODE,
      pad_byte: 0x20,
      ..CollationDescConf::default()
    };
    for (name, encoding) in [
      ("utf8mb4", TextEncoding::Utf8),
      ("utf8mb3", TextEncoding::Utf8Bmp),
      ("utf16", TextEncoding::Utf16Big),
      ("utf16le", TextEncoding::Utf16Little),
      ("ucs2", TextEncoding::Ucs2),
      ("utf32", TextEncoding::Utf32),
    ] {
      desc.charset_name = name.to_string();
      assert_eq!(encoding_of(&desc), encoding, "{}", name);
    }
    desc.flags = 0;
    desc.charset_name = "latin1".to_string();
    assert_eq!(encoding_of(&desc), TextEncoding::Cp1252);
    desc.charset_name = "cp1251".to_string();
    assert_eq!(encoding_of(&desc), TextEncoding::Bytes { pad: 0x20 });
  }
}
