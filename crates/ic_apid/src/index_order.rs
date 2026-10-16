// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Rows in an ordered index's order, for the merge of an ordered range
//! scan (chapter 06 §8.7). New: the C had no range scans.
//!
//! Each fragment returns its rows in the index's order, and the client
//! merges them. To compare two rows, each is given an **order key**:
//! bytes made of the row's index columns, in the index's order, that
//! compare byte by byte, the shorter first, as the index orders the
//! rows. A key is made once for each row the merge looks at, so that
//! choosing the next row compares bytes alone.
//!
//! A column's part of the key is a byte, 0 for NULL and 1 for a value,
//! since NULL comes before every value, and then the value:
//!
//! - integers, and the old temporal types kept as integers (DATE, TIME,
//!   DATETIME, YEAR, TIMESTAMP), big-endian, a signed one's top bit
//!   flipped so that the negative come first;
//! - FLOAT and DOUBLE by their bits made to compare as the numbers do,
//!   the negative ones' bits all flipped and the others' top bit set;
//!   minus zero as zero;
//! - DECIMAL, BINARY and the new temporal types (TIME2, DATETIME2,
//!   TIMESTAMP2) as their bytes, which compare as the values do;
//! - BIT in 32-bit words, each big-endian, the last cut to the bytes
//!   the column has;
//! - VARBINARY as its bytes, a character column as its collation's sort
//!   key ([`CollationMap::sort_key`]): either of these escaped, each 0
//!   followed by 0xFF, and ended by two zeros, so that a value that is
//!   the start of another comes first and the columns after it do not
//!   count first.
//!
//! The comparisons are the data nodes' own for each type (chapter 05
//! §6.3). An old decimal, the text form of MySQL 4, has no key here,
//! nor has a character column whose collation was not described or
//! weighs a character by its neighbours; an ordered scan over such an
//! index is refused.

use std::sync::Arc;

use ic_port::err;
use ic_port::IcError;
use ic_signals::table_props;

use crate::collation_map::CollationMap;
use crate::dict_cache::IndexDef;
use crate::record::Record;

/// One column of the index, as the order key takes it.
#[derive(Debug)]
struct OrderColumn {
  /// Its place in the scan's record.
  position: u32,
  type_code: u32,
  array_kind: u32,
  /// For a character column, its collation.
  collation: Option<Arc<CollationMap>>,
}

/// The order of an ordered index's columns, for rows laid out by one
/// record.
#[derive(Debug, Default)]
pub struct IndexOrder {
  columns: Vec<OrderColumn>,
}

impl IndexOrder {
  /// The order of `index`'s columns in rows laid out by `rec`, a record
  /// over the index's table, which must hold every one of them
  /// (`IC_ERROR_KEY_RECORD`). A column whose order is not known here
  /// is refused (`IC_ERROR_NOT_SUPPORTED`).
  pub fn new(index: &IndexDef, rec: &Record) -> Result<IndexOrder, IcError> {
    let table = rec.table();
    let own = index.as_table();
    let mut columns: Vec<OrderColumn> = Vec::new();
    for (place, column) in own.info().attributes.iter().enumerate() {
      // The index's hidden column is no column of the table.
      let field_id = match table.field_id(&column.name) {
        Ok(field_id) => field_id,
        Err(_) => continue,
      };
      let position = match rec.position_of(field_id) {
        Some(position) => position,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      };
      let attr = match table.field(field_id) {
        Some(attr) => attr,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      };
      let mut collation: Option<Arc<CollationMap>> = None;
      if attr.is_char_type() {
        collation = own.collation_at(place).cloned();
        if collation.is_none() {
          return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
        }
      }
      if !has_order_key(attr.type_code) {
        return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
      }
      columns.push(OrderColumn {
        position,
        type_code: attr.type_code,
        array_kind: attr.array_kind,
        collation,
      });
    }
    Ok(IndexOrder { columns })
  }

  /// The order key of `row`, laid out by `rec`, the record the order
  /// was made for, into `out`, cleared first.
  pub fn key_into(
    &self,
    rec: &Record,
    row: &[u8],
    out: &mut Vec<u8>,
  ) -> Result<(), IcError> {
    out.clear();
    for column in &self.columns {
      if rec.is_null(row, column.position) {
        out.push(0);
        continue;
      }
      out.push(1);
      let field = match rec.field(column.position) {
        Some(field) => field,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      };
      let start = field.offset() as usize;
      let size = field.size() as usize;
      if start + size > row.len() {
        return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
      }
      value_key(column, &row[start..start + size], out)?;
    }
    Ok(())
  }
}

/// True for a type an order key can be made of.
fn has_order_key(type_code: u32) -> bool {
  !matches!(
    type_code,
    table_props::IC_TYPE_UNDEFINED
      | table_props::IC_TYPE_OLDDECIMAL
      | table_props::IC_TYPE_OLDDECIMALUNSIGNED
      | table_props::IC_TYPE_BLOB
      | table_props::IC_TYPE_TEXT
  )
}

/// A value's part of the key, from the field's bytes in the row.
fn value_key(
  column: &OrderColumn,
  field: &[u8],
  out: &mut Vec<u8>,
) -> Result<(), IcError> {
  match column.type_code {
    table_props::IC_TYPE_TINYINT
    | table_props::IC_TYPE_SMALLINT
    | table_props::IC_TYPE_MEDIUMINT
    | table_props::IC_TYPE_INT
    | table_props::IC_TYPE_BIGINT
    | table_props::IC_TYPE_TIME
    | table_props::IC_TYPE_DATETIME => {
      push_big_endian(field, true, out);
      Ok(())
    }
    table_props::IC_TYPE_TINYUNSIGNED
    | table_props::IC_TYPE_SMALLUNSIGNED
    | table_props::IC_TYPE_MEDIUMUNSIGNED
    | table_props::IC_TYPE_UNSIGNED
    | table_props::IC_TYPE_BIGUNSIGNED
    | table_props::IC_TYPE_DATE
    | table_props::IC_TYPE_YEAR
    | table_props::IC_TYPE_TIMESTAMP => {
      push_big_endian(field, false, out);
      Ok(())
    }
    table_props::IC_TYPE_FLOAT => push_float(field, out),
    table_props::IC_TYPE_DOUBLE => push_double(field, out),
    table_props::IC_TYPE_BIT => {
      push_bits(field, out);
      Ok(())
    }
    table_props::IC_TYPE_CHAR
    | table_props::IC_TYPE_VARCHAR
    | table_props::IC_TYPE_LONGVARCHAR => {
      let (head, bytes) = var_value(column.array_kind, field)?;
      let map = match &column.collation {
        Some(map) => map,
        None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
      };
      let mut sort: Vec<u8> = Vec::new();
      map.sort_key(bytes, field.len() - head, head == 0, &mut sort)?;
      push_escaped(&sort, out);
      Ok(())
    }
    table_props::IC_TYPE_VARBINARY | table_props::IC_TYPE_LONGVARBINARY => {
      let (_, bytes) = var_value(column.array_kind, field)?;
      push_escaped(bytes, out);
      Ok(())
    }
    // DECIMAL, BINARY, TIME2, DATETIME2 and TIMESTAMP2 compare as their
    // bytes.
    _ => {
      out.extend_from_slice(field);
      Ok(())
    }
  }
}

/// A value's length bytes, one, two or none, and its bytes after them.
fn var_value(array_kind: u32, field: &[u8]) -> Result<(usize, &[u8]), IcError> {
  let too_long = IcError::new(err::IC_ERROR_VALUE_TOO_LONG);
  match array_kind {
    table_props::IC_ARRAY_KIND_SHORT_VARSIZE => {
      if field.is_empty() || 1 + field[0] as usize > field.len() {
        return Err(too_long);
      }
      Ok((1, &field[1..1 + field[0] as usize]))
    }
    table_props::IC_ARRAY_KIND_LONG_VARSIZE => {
      if field.len() < 2 {
        return Err(too_long);
      }
      let len = field[0] as usize + 256 * field[1] as usize;
      if 2 + len > field.len() {
        return Err(too_long);
      }
      Ok((2, &field[2..2 + len]))
    }
    _ => Ok((0, field)),
  }
}

/// A little-endian integer of the field's size, big-endian, its top bit
/// flipped if it is signed.
fn push_big_endian(field: &[u8], signed: bool, out: &mut Vec<u8>) {
  let start = out.len();
  let mut i = field.len();
  while i > 0 {
    i -= 1;
    out.push(field[i]);
  }
  if signed && out.len() > start {
    out[start] ^= 0x80;
  }
}

/// A FLOAT's bits, made to compare as the numbers do.
fn push_float(field: &[u8], out: &mut Vec<u8>) -> Result<(), IcError> {
  if field.len() != 4 {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let value = f32::from_le_bytes([field[0], field[1], field[2], field[3]]);
  let mut bits = value.to_bits();
  if value == 0.0 {
    bits = 0;
  }
  if bits & 0x8000_0000 != 0 {
    bits = !bits;
  } else {
    bits |= 0x8000_0000;
  }
  out.extend_from_slice(&bits.to_be_bytes());
  Ok(())
}

/// A DOUBLE's bits, made to compare as the numbers do.
fn push_double(field: &[u8], out: &mut Vec<u8>) -> Result<(), IcError> {
  if field.len() != 8 {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let mut eight = [0u8; 8];
  eight.copy_from_slice(field);
  let value = f64::from_le_bytes(eight);
  let mut bits = value.to_bits();
  if value == 0.0 {
    bits = 0;
  }
  if bits & 0x8000_0000_0000_0000 != 0 {
    bits = !bits;
  } else {
    bits |= 0x8000_0000_0000_0000;
  }
  out.extend_from_slice(&bits.to_be_bytes());
  Ok(())
}

/// A BIT column's 32-bit words, each big-endian, the last cut to the
/// bytes the column has: the data nodes compare the words as numbers.
fn push_bits(field: &[u8], out: &mut Vec<u8>) {
  let mut i: usize = 0;
  while i < field.len() {
    let mut word = [0u8; 4];
    let take = (field.len() - i).min(4);
    word[..take].copy_from_slice(&field[i..i + take]);
    out.extend_from_slice(&u32::from_le_bytes(word).to_be_bytes());
    i += 4;
  }
}

/// Bytes of no fixed length, escaped and ended so that they compare
/// alone: each 0 is followed by 0xFF, and two zeros end them.
fn push_escaped(bytes: &[u8], out: &mut Vec<u8>) {
  for byte in bytes {
    out.push(*byte);
    if *byte == 0 {
      out.push(0xFF);
    }
  }
  out.push(0);
  out.push(0);
}

#[cfg(test)]
mod tests {
  use super::*;

  fn column(type_code: u32, array_kind: u32) -> OrderColumn {
    OrderColumn {
      position: 0,
      type_code,
      array_kind,
      collation: None,
    }
  }

  fn key(type_code: u32, array_kind: u32, field: &[u8]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    value_key(&column(type_code, array_kind), field, &mut out).expect("key");
    out
  }

  #[test]
  fn integers_compare_as_numbers() {
    let ints: [i32; 6] = [i32::MIN, -300, -1, 0, 1, i32::MAX];
    let mut last: Vec<u8> = Vec::new();
    for value in ints {
      let now = key(table_props::IC_TYPE_INT, 0, &value.to_le_bytes());
      assert!(last < now, "{}", value);
      last = now;
    }
    // A MEDIUMINT of three bytes, -1 and 1.
    let minus = key(table_props::IC_TYPE_MEDIUMINT, 0, &[0xFF, 0xFF, 0xFF]);
    let plus = key(table_props::IC_TYPE_MEDIUMINT, 0, &[1, 0, 0]);
    assert!(minus < plus);
    let small = key(table_props::IC_TYPE_UNSIGNED, 0, &1u32.to_le_bytes());
    let big = key(table_props::IC_TYPE_UNSIGNED, 0, &u32::MAX.to_le_bytes());
    assert!(small < big);
  }

  #[test]
  fn floating_point_compares_as_numbers() {
    let doubles: [f64; 7] = [f64::MIN, -2.5, -0.5, 0.0, 0.25, 3.0, f64::MAX];
    let mut last: Vec<u8> = Vec::new();
    for value in doubles {
      let now = key(table_props::IC_TYPE_DOUBLE, 0, &value.to_le_bytes());
      assert!(last < now, "{}", value);
      last = now;
    }
    let zero = key(table_props::IC_TYPE_FLOAT, 0, &0.0f32.to_le_bytes());
    let minus_zero =
      key(table_props::IC_TYPE_FLOAT, 0, &(-0.0f32).to_le_bytes());
    assert_eq!(zero, minus_zero);
    let minus = key(table_props::IC_TYPE_FLOAT, 0, &(-1.0f32).to_le_bytes());
    assert!(minus < zero);
  }

  #[test]
  fn binary_strings_compare_shorter_first() {
    let short = table_props::IC_ARRAY_KIND_SHORT_VARSIZE;
    let a = key(table_props::IC_TYPE_VARBINARY, short, &[1, b'a', 0]);
    let a0 = key(table_props::IC_TYPE_VARBINARY, short, &[2, b'a', 0]);
    let ab = key(table_props::IC_TYPE_VARBINARY, short, &[2, b'a', b'b']);
    let empty = key(table_props::IC_TYPE_VARBINARY, short, &[0, 0, 0]);
    assert!(empty < a);
    assert!(a < a0);
    assert!(a0 < ab);
    assert_eq!(a0, vec![b'a', 0, 0xFF, 0, 0]);
    // A length beyond the field is refused.
    let mut out: Vec<u8> = Vec::new();
    let column = column(table_props::IC_TYPE_VARBINARY, short);
    assert!(value_key(&column, &[3, b'a'], &mut out).is_err());
  }

  #[test]
  fn the_old_temporal_types_compare_as_numbers() {
    // TIME is three bytes, signed: -1 second before 0.
    let minus = key(table_props::IC_TYPE_TIME, 0, &[0xFF, 0xFF, 0xFF]);
    let zero = key(table_props::IC_TYPE_TIME, 0, &[0, 0, 0]);
    assert!(minus < zero);
    // DATE is three bytes, unsigned.
    let early = key(table_props::IC_TYPE_DATE, 0, &[0x21, 0x0F, 0x00]);
    let late = key(table_props::IC_TYPE_DATE, 0, &[0x01, 0x00, 0x0F]);
    assert!(early < late);
  }

  #[test]
  fn bits_compare_word_by_word() {
    // Two words: the second word decides when the first ties.
    let low = key(table_props::IC_TYPE_BIT, 0, &[1, 0, 0, 0, 1, 0, 0, 0]);
    let high = key(table_props::IC_TYPE_BIT, 0, &[1, 0, 0, 0, 0, 1, 0, 0]);
    assert!(low < high);
    // Within a word, its high byte counts most.
    let a = key(table_props::IC_TYPE_BIT, 0, &[0xFF, 0x00]);
    let b = key(table_props::IC_TYPE_BIT, 0, &[0x00, 0x01]);
    assert!(a < b);
  }

  #[test]
  fn types_with_no_order_here_are_known() {
    assert!(!has_order_key(table_props::IC_TYPE_OLDDECIMAL));
    assert!(!has_order_key(table_props::IC_TYPE_BLOB));
    assert!(has_order_key(table_props::IC_TYPE_DECIMAL));
  }
}
