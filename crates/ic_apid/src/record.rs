// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Records: where the fields of a table lie in a row the application
//! owns (`IC_RECORD` in `ic_apid.h`; doc/rust/04-api-design.md, "Row
//! binding: records"). New: the C bound fields through an array of
//! 64-bit slots instead.
//!
//! A record gives each field a byte offset in the row, and a nullable
//! field a null bit. The value at the offset is in the form the data
//! node stores it, so reading or writing a field is one copy:
//!
//! ```text
//!   fixed-size types           inline, at the size the data node uses
//!   VARCHAR, VARBINARY         one length byte, then the bytes
//!   LONGVARCHAR, LONGVARBINARY two length bytes, little-endian, then
//!                              the bytes
//!   DECIMAL                    packed binary
//!   BIT(n)                     n bits in whole 32-bit words
//!   nullable field             one bit in a null byte; set means NULL
//!   BLOB, TEXT                 not in a record, for now
//! ```
//!
//! A variable-sized field takes its largest size in the row.
//!
//! **Checked when made, as the reference checks a record:** every field
//! exists and is not a blob, no field comes twice, every value and null
//! bit lies inside the row, and no two of them share a bit. A nullable
//! field must have a null bit. A NOT NULL field's null bit is not used,
//! so it is not checked. Verify: `NdbDictionaryImpl.cpp`,
//! `validateRecordSpec`; `NdbDictionary.cpp`,
//! `Column::getSizeInBytesForRecord`.
//!
//! **The default record** puts the null bits first, eight to a byte in
//! attribute order, then every field in attribute order, each starting
//! on a 4-byte boundary, and rounds the row up to eight bytes. That is
//! how the data node keeps each column, and how each value arrives in a
//! signal: in whole words, its length rounded up. So a value is copied
//! from word to word. Eight-byte integers and doubles go on an 8-byte
//! boundary, where a C compiler puts them, so that the row can be
//! written as a C struct. The reference's default record packs the
//! fields with no padding (`createDefaultNdbRecord`). Verify:
//! `AttributeHeader.hpp`, `getDataSize`; `DbtupRoutines.cpp`, where a
//! fixed-size column is read at a word offset.
//!
//! A record holds the version of the table it was made for, whose
//! columns it describes.

use std::sync::Arc;

use ic_ndb_signals::dict_tab_info;
use ic_ndb_signals::dict_tab_info::AttributeInfo;
use ic_port::err;
use ic_port::IcError;

use crate::dict_cache::TableDef;

/// In [`FieldSpec::null_byte_offset`]: the field has no null bit.
pub const IC_NO_NULL_BIT: u32 = 0xFFFF_FFFF;

/// Where one field lies in the row (`IC_FIELD_SPEC`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FieldSpec {
  /// The field, by the id the table gives it.
  pub field_id: u32,
  /// Byte offset of the value in the row.
  pub offset: u32,
  /// Byte offset of the null bit, or [`IC_NO_NULL_BIT`].
  pub null_byte_offset: u32,
  /// The null bit within that byte, 0 to 7.
  pub null_bit: u32,
}

impl FieldSpec {
  /// A field without a null bit.
  pub fn not_null(field_id: u32, offset: u32) -> FieldSpec {
    FieldSpec {
      field_id,
      offset,
      null_byte_offset: IC_NO_NULL_BIT,
      null_bit: 0,
    }
  }

  /// A field whose null bit is `null_bit` of the byte at
  /// `null_byte_offset`.
  pub fn nullable(
    field_id: u32,
    offset: u32,
    null_byte_offset: u32,
    null_bit: u32,
  ) -> FieldSpec {
    FieldSpec {
      field_id,
      offset,
      null_byte_offset,
      null_bit,
    }
  }
}

/// One field of a record, as checked.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecordField {
  field_id: u32,
  field_type: u32,
  offset: u32,
  size: u32,
  null_byte_offset: u32,
  null_bit: u32,
}

impl RecordField {
  /// The field, by the id the table gives it.
  pub fn field_id(&self) -> u32 {
    self.field_id
  }

  /// Its type, one of the `IC_NDB_TYPE_*` values.
  pub fn field_type(&self) -> u32 {
    self.field_type
  }

  /// Byte offset of the value in the row.
  pub fn offset(&self) -> u32 {
    self.offset
  }

  /// Bytes the value takes in the row, length bytes included.
  pub fn size(&self) -> u32 {
    self.size
  }

  /// True if the field has a null bit, which it has exactly when the
  /// column may be NULL.
  pub fn is_nullable(&self) -> bool {
    self.null_byte_offset != IC_NO_NULL_BIT
  }

  /// Byte offset of the null bit, or [`IC_NO_NULL_BIT`].
  pub fn null_byte_offset(&self) -> u32 {
    self.null_byte_offset
  }

  /// The null bit within its byte.
  pub fn null_bit(&self) -> u32 {
    self.null_bit
  }
}

/// Where a chosen set of a table's fields lie in a row. Fields are
/// numbered by their position in the record, which is what a field
/// mask counts.
#[derive(Clone)]
pub struct Record {
  table: Arc<TableDef>,
  fields: Vec<RecordField>,
  row_size: u32,
  /// By column id, the column's position in `fields`, or `u32::MAX`:
  /// found by index when a row is packed or unpacked.
  position_by_id: Vec<u32>,
}

/// The position of each field, by its column id.
fn index_positions(fields: &[RecordField]) -> Vec<u32> {
  let mut index: Vec<u32> = Vec::new();
  for (position, field) in fields.iter().enumerate() {
    let id = field.field_id as usize;
    if id >= index.len() {
      index.resize(id + 1, u32::MAX);
    }
    index[id] = position as u32;
  }
  index
}

impl std::fmt::Debug for Record {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "Record({}, {} field(s), {} bytes)",
      self.table.name(),
      self.fields.len(),
      self.row_size
    )
  }
}

impl Record {
  /// A record as the application lays out its row
  /// (`ic_table_def_create_record`).
  pub fn new(
    table: &Arc<TableDef>,
    spec: &[FieldSpec],
    row_size: u32,
  ) -> Result<Record, IcError> {
    if spec.len() > table.num_fields() as usize {
      return Err(IcError::new(err::IC_ERROR_TOO_MANY_FIELDS));
    }
    let mut fields: Vec<RecordField> = Vec::with_capacity(spec.len());
    for one in spec {
      let attr = match table.field(one.field_id) {
        Some(attr) => attr,
        None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
      };
      if is_blob(attr) {
        return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
      }
      for field in &fields {
        if field.field_id == one.field_id {
          return Err(IcError::new(err::IC_ERROR_DUPLICATE_FIELD_IDS));
        }
      }
      let mut null_byte_offset = IC_NO_NULL_BIT;
      let mut null_bit: u32 = 0;
      if attr.nullable {
        if one.null_byte_offset == IC_NO_NULL_BIT {
          return Err(IcError::new(err::IC_ERROR_NO_NULL_BIT));
        }
        if one.null_bit > 7 {
          return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
        }
        null_byte_offset = one.null_byte_offset;
        null_bit = one.null_bit;
      }
      fields.push(RecordField {
        field_id: one.field_id,
        field_type: attr.ext_type,
        offset: one.offset,
        size: attr.max_byte_size(),
        null_byte_offset,
        null_bit,
      });
    }
    check_layout(&fields, row_size)?;
    let position_by_id = index_positions(&fields);
    Ok(Record {
      table: Arc::clone(table),
      fields,
      row_size,
      position_by_id,
    })
  }

  /// A record with every field word-aligned, over the fields named,
  /// or over every field but the blobs if none are named
  /// (`ic_table_def_default_record`). See the module note for the
  /// layout; [`field`](Self::field) tells where each field went.
  pub fn default_for(
    table: &Arc<TableDef>,
    field_ids: Option<&[u32]>,
  ) -> Result<Record, IcError> {
    let mut attrs: Vec<&AttributeInfo> = Vec::new();
    match field_ids {
      Some(ids) => {
        for id in ids {
          match table.field(*id) {
            Some(attr) => attrs.push(attr),
            None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
          }
        }
      }
      None => {
        for attr in &table.info().attributes {
          if !is_blob(attr) {
            attrs.push(attr);
          }
        }
      }
    }
    let mut num_nullable: u32 = 0;
    for attr in &attrs {
      if attr.nullable {
        num_nullable += 1;
      }
    }
    let mut offset = num_nullable.div_ceil(8);
    let mut next_null: u32 = 0;
    let mut spec: Vec<FieldSpec> = Vec::with_capacity(attrs.len());
    for attr in &attrs {
      offset = offset.next_multiple_of(field_alignment(attr));
      let mut one = FieldSpec::not_null(attr.attribute_id, offset);
      if attr.nullable {
        one.null_byte_offset = next_null / 8;
        one.null_bit = next_null % 8;
        next_null += 1;
      }
      spec.push(one);
      offset += attr.max_byte_size();
    }
    Record::new(table, &spec, offset.next_multiple_of(8))
  }

  /// The table version the record was made for.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// How many fields.
  pub fn num_fields(&self) -> u32 {
    self.fields.len() as u32
  }

  /// Bytes in a row.
  pub fn row_size(&self) -> u32 {
    self.row_size
  }

  /// The field at a position (`ic_record_get_field_layout`).
  pub fn field(&self, position: u32) -> Option<&RecordField> {
    self.fields.get(position as usize)
  }

  /// The position of a field, by the id the table gives it
  /// (`ic_record_get_position`).
  pub fn position_of(&self, field_id: u32) -> Option<u32> {
    let position = *self.position_by_id.get(field_id as usize)?;
    if position == u32::MAX {
      return None;
    }
    Some(position)
  }

  /// True if every primary key field is in the record, as a key record
  /// needs.
  pub fn covers_primary_key(&self) -> bool {
    for attr in &self.table.info().attributes {
      if attr.primary_key && self.position_of(attr.attribute_id).is_none() {
        return false;
      }
    }
    true
  }

  /// True if the field at `position` is NULL in `row`
  /// (`ic_record_is_null`). A field that cannot be NULL never is.
  pub fn is_null(&self, row: &[u8], position: u32) -> bool {
    let field = match self.field(position) {
      Some(field) => field,
      None => return false,
    };
    if !field.is_nullable() {
      return false;
    }
    match row.get(field.null_byte_offset as usize) {
      Some(byte) => (byte >> field.null_bit) & 1 != 0,
      None => false,
    }
  }

  /// Set or clear the null bit of the field at `position` in `row`
  /// (`ic_record_set_null`).
  pub fn set_null(
    &self,
    row: &mut [u8],
    position: u32,
    is_null: bool,
  ) -> Result<(), IcError> {
    let field = match self.field(position) {
      Some(field) => field,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if !field.is_nullable() {
      return Err(IcError::new(err::IC_ERROR_NO_NULL_BIT));
    }
    let index = field.null_byte_offset as usize;
    if index >= row.len() {
      return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
    }
    let mask: u8 = 1 << field.null_bit;
    if is_null {
      row[index] |= mask;
    } else {
      row[index] &= !mask;
    }
    Ok(())
  }
}

fn is_blob(attr: &AttributeInfo) -> bool {
  attr.ext_type == dict_tab_info::IC_NDB_TYPE_BLOB
    || attr.ext_type == dict_tab_info::IC_NDB_TYPE_TEXT
}

/// Where a field of the default record may start: on a word, as the
/// data node keeps it, or on eight bytes for the eight-byte integers and
/// doubles a C compiler puts there.
fn field_alignment(attr: &AttributeInfo) -> u32 {
  match attr.ext_type {
    dict_tab_info::IC_NDB_TYPE_BIGINT
    | dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_DOUBLE
    | dict_tab_info::IC_NDB_TYPE_DATETIME => 8,
    _ => 4,
  }
}

/// No value or null bit may lie outside the row, and no two may share a
/// bit. As the reference does: every value and null bit is a range of
/// bits, and sorted by where they start, each must begin after the one
/// before it ends.
fn check_layout(fields: &[RecordField], row_size: u32) -> Result<(), IcError> {
  let bad = IcError::new(err::IC_ERROR_RECORD_LAYOUT);
  let row_bits = row_size as u64 * 8;
  // First and last bit of each value and null bit.
  let mut ranges: Vec<(u64, u64)> = Vec::with_capacity(2 * fields.len());
  for field in fields {
    if field.size > 0 {
      let first = field.offset as u64 * 8;
      let last = first + field.size as u64 * 8 - 1;
      if last >= row_bits {
        return Err(bad);
      }
      ranges.push((first, last));
    }
    if field.is_nullable() {
      let bit = field.null_byte_offset as u64 * 8 + field.null_bit as u64;
      if bit >= row_bits {
        return Err(bad);
      }
      ranges.push((bit, bit));
    }
  }
  ranges.sort_unstable();
  let mut i: usize = 1;
  while i < ranges.len() {
    if ranges[i].0 <= ranges[i - 1].1 {
      return Err(bad);
    }
    i += 1;
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;
  use ic_ndb_signals::dict_tab_info::parse_table_info;
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
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_BLOB;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_CHAR;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_INT;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_VARCHAR;
  use ic_ndb_signals::simple_properties::PropertyWriter;

  struct Column {
    name: &'static str,
    ext_type: u32,
    length: u32,
    key: bool,
    nullable: bool,
    charset: u32,
  }

  const COLUMNS: [Column; 6] = [
    Column {
      name: "id",
      ext_type: IC_NDB_TYPE_INT,
      length: 1,
      key: true,
      nullable: false,
      charset: 0,
    },
    Column {
      name: "b",
      ext_type: IC_NDB_TYPE_INT,
      length: 1,
      key: false,
      nullable: true,
      charset: 0,
    },
    Column {
      name: "name",
      ext_type: IC_NDB_TYPE_VARCHAR,
      length: 20,
      key: false,
      nullable: false,
      charset: 8,
    },
    Column {
      name: "big",
      ext_type: IC_NDB_TYPE_BIGINT,
      length: 1,
      key: false,
      nullable: true,
      charset: 0,
    },
    Column {
      name: "doc",
      ext_type: IC_NDB_TYPE_BLOB,
      length: 1,
      key: false,
      nullable: true,
      charset: 0,
    },
    Column {
      name: "tag",
      ext_type: IC_NDB_TYPE_CHAR,
      length: 3,
      key: false,
      nullable: false,
      charset: 8,
    },
  ];

  /// `id INT` key, `b INT NULL`, `name VARCHAR(20)`, `big BIGINT NULL`,
  /// `doc BLOB`, `tag CHAR(3)`.
  fn table() -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_DTI_TABLE_NAME, "db/def/t1");
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
      w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
      id += 1;
    }
    let info = parse_table_info(w.words()).expect("parsed");
    Arc::new(TableDef::new(info, None))
  }

  fn code_of(result: Result<Record, IcError>) -> i32 {
    result.expect_err("refused").code
  }

  #[test]
  fn fields_are_found_by_name() {
    let t1 = table();
    assert_eq!(t1.field_id("name").expect("name"), 2);
    assert_eq!(
      t1.field_id("nope").expect_err("none").code,
      err::IC_ERROR_NO_SUCH_FIELD
    );
  }

  #[test]
  fn the_default_record_puts_every_field_on_a_word() {
    let t1 = table();
    let rec = Record::default_for(&t1, None).expect("record");
    // The blob is left out.
    assert_eq!(rec.num_fields(), 5);
    // One null byte first, for b and big.
    let id = rec.field(0).expect("id");
    assert_eq!((id.offset(), id.size()), (4, 4));
    assert!(!id.is_nullable());
    let b = rec.field(1).expect("b");
    assert_eq!(b.offset(), 8);
    assert_eq!((b.null_byte_offset(), b.null_bit()), (0, 0));
    // A length byte and twenty bytes.
    let name = rec.field(2).expect("name");
    assert_eq!((name.offset(), name.size()), (12, 21));
    // 33 rounded up to eight, where a C compiler puts a 64-bit integer.
    let big = rec.field(3).expect("big");
    assert_eq!((big.offset(), big.size()), (40, 8));
    assert_eq!((big.null_byte_offset(), big.null_bit()), (0, 1));
    let tag = rec.field(4).expect("tag");
    assert_eq!((tag.offset(), tag.size()), (48, 3));
    // 51 rounded up to eight.
    assert_eq!(rec.row_size(), 56);
    assert!(rec.covers_primary_key());
  }

  #[test]
  fn a_field_after_an_odd_sized_one_starts_on_the_next_word() {
    let t1 = table();
    // name ends at byte 21; tag starts at 24, not 21.
    let rec = Record::default_for(&t1, Some(&[2, 5])).expect("record");
    assert_eq!(rec.field(0).expect("name").offset(), 0);
    assert_eq!(rec.field(1).expect("tag").offset(), 24);
    assert_eq!(rec.row_size(), 32);
  }

  #[test]
  fn a_default_record_over_chosen_fields_keeps_their_order() {
    let t1 = table();
    let rec = Record::default_for(&t1, Some(&[3, 0])).expect("record");
    assert_eq!(rec.field(0).expect("big").offset(), 8);
    assert_eq!(rec.field(1).expect("id").offset(), 16);
    assert_eq!(rec.position_of(0), Some(1));
    assert_eq!(rec.position_of(1), None);
  }

  #[test]
  fn a_record_made_by_hand_is_checked() {
    let t1 = table();
    let good = [
      FieldSpec::not_null(0, 0),
      FieldSpec::nullable(1, 4, 8, 0),
      FieldSpec::nullable(3, 16, 8, 1),
    ];
    let rec = Record::new(&t1, &good, 24).expect("record");
    assert!(rec.covers_primary_key());
    // id and b overlap.
    let overlap = [FieldSpec::not_null(0, 0), FieldSpec::nullable(1, 2, 8, 0)];
    assert_eq!(
      code_of(Record::new(&t1, &overlap, 24)),
      err::IC_ERROR_RECORD_LAYOUT
    );
    // big runs past the end of the row.
    let past = [FieldSpec::nullable(3, 20, 8, 1)];
    assert_eq!(
      code_of(Record::new(&t1, &past, 24)),
      err::IC_ERROR_RECORD_LAYOUT
    );
    // Two fields on one null bit.
    let shared = [
      FieldSpec::nullable(1, 0, 8, 0),
      FieldSpec::nullable(3, 16, 8, 0),
    ];
    assert_eq!(
      code_of(Record::new(&t1, &shared, 24)),
      err::IC_ERROR_RECORD_LAYOUT
    );
    // A null bit inside a value.
    let inside = [FieldSpec::nullable(1, 0, 2, 0)];
    assert_eq!(
      code_of(Record::new(&t1, &inside, 24)),
      err::IC_ERROR_RECORD_LAYOUT
    );
  }

  #[test]
  fn fields_that_cannot_be_in_a_record_are_refused() {
    let t1 = table();
    let nullable_without_bit = [FieldSpec::not_null(1, 0)];
    assert_eq!(
      code_of(Record::new(&t1, &nullable_without_bit, 8)),
      err::IC_ERROR_NO_NULL_BIT
    );
    let blob = [FieldSpec::nullable(4, 0, 8, 0)];
    assert_eq!(
      code_of(Record::new(&t1, &blob, 16)),
      err::IC_ERROR_NOT_SUPPORTED
    );
    let unknown = [FieldSpec::not_null(9, 0)];
    assert_eq!(
      code_of(Record::new(&t1, &unknown, 8)),
      err::IC_ERROR_NO_SUCH_FIELD
    );
    let twice = [FieldSpec::not_null(0, 0), FieldSpec::not_null(0, 4)];
    assert_eq!(
      code_of(Record::new(&t1, &twice, 8)),
      err::IC_ERROR_DUPLICATE_FIELD_IDS
    );
  }

  #[test]
  fn a_not_null_fields_null_bit_is_not_used() {
    // The reference passes over it; so a bit on top of a value is fine.
    let t1 = table();
    let spec = [FieldSpec::nullable(0, 0, 0, 0)];
    let rec = Record::new(&t1, &spec, 4).expect("record");
    assert!(!rec.field(0).expect("id").is_nullable());
    let key_only =
      Record::new(&t1, &[FieldSpec::nullable(1, 0, 4, 0)], 8).expect("record");
    assert!(!key_only.covers_primary_key());
  }

  #[test]
  fn null_bits_are_set_and_read() {
    let t1 = table();
    let rec = Record::default_for(&t1, None).expect("record");
    let mut row = vec![0u8; rec.row_size() as usize];
    assert!(!rec.is_null(&row, 3));
    rec.set_null(&mut row, 3, true).expect("set");
    assert!(rec.is_null(&row, 3));
    assert!(!rec.is_null(&row, 1));
    assert_eq!(row[0], 0b10);
    rec.set_null(&mut row, 3, false).expect("cleared");
    assert_eq!(row[0], 0);
    let e = rec.set_null(&mut row, 0, true).expect_err("not nullable");
    assert_eq!(e.code, err::IC_ERROR_NO_NULL_BIT);
  }
}
