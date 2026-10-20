// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! What the Rust examples share (`examples/README.md`): connecting, the
//! table `ic_ex.t1` with a record the library lays out, values put into
//! a row and taken out of it, waiting for a transaction, and the rows
//! the scan examples read.
//!
//! A value in a row is kept as the data nodes keep it, so these helpers
//! show the format: an integer in four or eight little-endian bytes, a
//! VARCHAR as its length byte and then its bytes, and a null bit per
//! nullable field.

use std::sync::Arc;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::apid_global::GlobalOptions;
use ic_apid::dict_cache::TableDef;
use ic_apid::key_op;
use ic_apid::query::QueryId;
use ic_apid::query::TransId;
use ic_apid::record::Record;
use ic_apid::transaction::CommitState;
use ic_apid::transaction::IC_NDB_ERROR_NO_REPLY;
use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;

/// The database of the examples' table.
pub const EX_DB: &str = "ic_ex";
/// The examples' table.
pub const EX_TABLE: &str = "t1";
/// The hash index a MySQL server makes for `UNIQUE KEY t1_code`.
pub const EX_INDEX: &str = "t1_code$unique";
/// The ordered index a MySQL server makes for `KEY t1_val (val)`.
pub const EX_VAL_INDEX: &str = "t1_val";
/// The scan examples' rows: ids from here up, clear of the other
/// examples' rows, code the same as id, and val the id negated, which
/// no other example writes, so that a range of val finds these alone.
pub const EX_SCAN_FIRST: u32 = 200;
/// How many rows the scan examples write.
pub const EX_SCAN_ROWS: u32 = 100;
/// How long to wait for the data nodes to start, in milliseconds.
pub const EX_START_WAIT_MS: u32 = 30_000;

/// The connectstring: the first argument, or `localhost:1186`.
pub fn connect_string(args: &[String]) -> &str {
  match args.get(1) {
    Some(text) => text.as_str(),
    None => "localhost:1186",
  }
}

/// Start the Data API with the connectstring in the arguments, wait for
/// the data nodes, and make a connection for this thread. Keep the
/// global for as long as the connection is used.
pub fn connect(
  args: &[String],
  name: &str,
) -> Result<(ApidGlobal, ApidConnection), IcError> {
  let global = ApidGlobal::connect(
    connect_string(args),
    Some(name),
    &GlobalOptions::default(),
  )?;
  if global.wait_for_all_started(EX_START_WAIT_MS) == 0 {
    return Err(IcError::new(err::IC_ERROR_NO_STARTED_DATA_NODE));
  }
  let conn = global.create_connection()?;
  Ok((global, conn))
}

/// For `main`: say what went wrong, and exit with 1.
pub fn exit_on(result: Result<(), IcError>) {
  if let Err(e) = result {
    eprintln!("error {}: {}", e.code, e.message());
    std::process::exit(1);
  }
}

/// A check of a result the example knows to expect: on a mismatch, say
/// what was wrong and fail, so that [`exit_on`] exits with 1. A run
/// whose checks all held ends with "all checks passed".
pub fn check(holds: bool, what: &str) -> Result<(), IcError> {
  if holds {
    return Ok(());
  }
  eprintln!("check failed: {}", what);
  Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA))
}

/// The values of a row of `ic_ex.t1`; `None` for NULL.
#[derive(Clone, Copy, Debug)]
pub struct Values<'a> {
  /// `id INT UNSIGNED NOT NULL`, the primary key.
  pub id: u32,
  /// `val INT`.
  pub val: Option<i32>,
  /// `code INT UNSIGNED NOT NULL`, the unique key.
  pub code: u32,
  /// `name VARCHAR(32) CHARACTER SET latin1`.
  pub name: Option<&'a str>,
}

/// The table `ic_ex.t1`, bound, with a record over every field and the
/// fields' ids.
pub struct T1 {
  /// The table's description.
  pub table: Arc<TableDef>,
  /// A record over every field, laid out by the library.
  pub rec: Record,
  /// The id of `id`.
  pub id: u32,
  /// The id of `val`.
  pub val: u32,
  /// The id of `code`.
  pub code: u32,
  /// The id of `name`.
  pub name: u32,
}

impl T1 {
  /// Bind the table and lay out its record.
  pub fn bind(conn: &mut ApidConnection) -> Result<T1, IcError> {
    let table = conn.table_bind(EX_DB, EX_TABLE)?;
    let rec = Record::default_for(&table, None)?;
    Ok(T1 {
      id: table.field_id("id")?,
      val: table.field_id("val")?,
      code: table.field_id("code")?,
      name: table.field_id("name")?,
      table,
      rec,
    })
  }

  /// A record over the fields named only: the unique key's, or the
  /// values an update through the index may set.
  pub fn record_over(&self, fields: &[u32]) -> Result<Record, IcError> {
    Record::default_for(&self.table, Some(fields))
  }

  /// An empty row for the record over every field.
  pub fn row(&self) -> Vec<u8> {
    vec![0; self.rec.row_size() as usize]
  }

  /// Put the values into a row of `rec`, those of the fields it holds.
  pub fn fill(
    &self,
    rec: &Record,
    row: &mut [u8],
    values: &Values<'_>,
  ) -> Result<(), IcError> {
    if holds(rec, self.id) {
      put_u32(rec, row, self.id, values.id)?;
    }
    if holds(rec, self.val) {
      put_i32(rec, row, self.val, values.val)?;
    }
    if holds(rec, self.code) {
      put_u32(rec, row, self.code, values.code)?;
    }
    if holds(rec, self.name) {
      put_varchar(rec, row, self.name, values.name)?;
    }
    Ok(())
  }

  /// A row of the record over every field, as text.
  pub fn show(&self, row: &[u8]) -> String {
    let mut text = String::new();
    match get_u32(&self.rec, row, self.id) {
      Some(id) => text.push_str(&format!("id {}", id)),
      None => text.push_str("id NULL"),
    }
    match get_u32(&self.rec, row, self.code) {
      Some(code) => text.push_str(&format!(", code {}", code)),
      None => text.push_str(", code NULL"),
    }
    match get_i32(&self.rec, row, self.val) {
      Some(val) => text.push_str(&format!(", val {}", val)),
      None => text.push_str(", val NULL"),
    }
    match get_varchar(&self.rec, row, self.name) {
      Some(name) => text.push_str(&format!(", name '{}'", name)),
      None => text.push_str(", name NULL"),
    }
    text
  }
}

/// True if the record holds the field.
pub fn holds(rec: &Record, field_id: u32) -> bool {
  rec.position_of(field_id).is_some()
}

/// Where a field lies in a record's row: its offset, its size, and its
/// position in the record.
fn place(rec: &Record, field_id: u32) -> Result<(usize, usize, u32), IcError> {
  let position = match rec.position_of(field_id) {
    Some(position) => position,
    None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
  };
  match rec.field(position) {
    Some(field) => {
      Ok((field.offset() as usize, field.size() as usize, position))
    }
    None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
  }
}

/// Clear a field's null bit, if it has one.
fn not_null(rec: &Record, row: &mut [u8], position: u32) {
  if let Some(field) = rec.field(position) {
    if field.is_nullable() {
      let _ = rec.set_null(row, position, false);
    }
  }
}

/// Put an unsigned integer into a field of four bytes.
pub fn put_u32(
  rec: &Record,
  row: &mut [u8],
  field_id: u32,
  value: u32,
) -> Result<(), IcError> {
  let (at, size, position) = place(rec, field_id)?;
  if size != 4 || at + 4 > row.len() {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  row[at..at + 4].copy_from_slice(&value.to_le_bytes());
  not_null(rec, row, position);
  Ok(())
}

/// Put an unsigned integer into a field of eight bytes, such as an
/// AUTO_INCREMENT key.
pub fn put_u64(
  rec: &Record,
  row: &mut [u8],
  field_id: u32,
  value: u64,
) -> Result<(), IcError> {
  let (at, size, position) = place(rec, field_id)?;
  if size != 8 || at + 8 > row.len() {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  row[at..at + 8].copy_from_slice(&value.to_le_bytes());
  not_null(rec, row, position);
  Ok(())
}

/// Put a signed integer, or NULL, into a field of four bytes.
pub fn put_i32(
  rec: &Record,
  row: &mut [u8],
  field_id: u32,
  value: Option<i32>,
) -> Result<(), IcError> {
  let (at, size, position) = place(rec, field_id)?;
  if size != 4 || at + 4 > row.len() {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  match value {
    Some(value) => {
      row[at..at + 4].copy_from_slice(&value.to_le_bytes());
      not_null(rec, row, position);
      Ok(())
    }
    None => rec.set_null(row, position, true),
  }
}

/// Put a string, or NULL, into a VARCHAR of at most 255 bytes: its
/// length byte, then its bytes.
pub fn put_varchar(
  rec: &Record,
  row: &mut [u8],
  field_id: u32,
  value: Option<&str>,
) -> Result<(), IcError> {
  let (at, size, position) = place(rec, field_id)?;
  if at + size > row.len() {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let text = match value {
    Some(text) => text,
    None => return rec.set_null(row, position, true),
  };
  let bytes = text.as_bytes();
  if bytes.len() + 1 > size || bytes.len() > 255 {
    return Err(IcError::new(err::IC_ERROR_VALUE_TOO_LONG));
  }
  row[at] = bytes.len() as u8;
  row[at + 1..at + 1 + bytes.len()].copy_from_slice(bytes);
  not_null(rec, row, position);
  Ok(())
}

/// An unsigned integer from a field of four bytes; `None` if NULL.
pub fn get_u32(rec: &Record, row: &[u8], field_id: u32) -> Option<u32> {
  let (at, size, position) = place(rec, field_id).ok()?;
  if size != 4 || at + 4 > row.len() || rec.is_null(row, position) {
    return None;
  }
  let mut bytes = [0u8; 4];
  bytes.copy_from_slice(&row[at..at + 4]);
  Some(u32::from_le_bytes(bytes))
}

/// An unsigned integer from a field of eight bytes; `None` if NULL.
pub fn get_u64(rec: &Record, row: &[u8], field_id: u32) -> Option<u64> {
  let (at, size, position) = place(rec, field_id).ok()?;
  if size != 8 || at + 8 > row.len() || rec.is_null(row, position) {
    return None;
  }
  let mut bytes = [0u8; 8];
  bytes.copy_from_slice(&row[at..at + 8]);
  Some(u64::from_le_bytes(bytes))
}

/// A signed integer from a field of four bytes; `None` if NULL.
pub fn get_i32(rec: &Record, row: &[u8], field_id: u32) -> Option<i32> {
  let (at, size, position) = place(rec, field_id).ok()?;
  if size != 4 || at + 4 > row.len() || rec.is_null(row, position) {
    return None;
  }
  let mut bytes = [0u8; 4];
  bytes.copy_from_slice(&row[at..at + 4]);
  Some(i32::from_le_bytes(bytes))
}

/// A string from a VARCHAR of at most 255 bytes; `None` if NULL. The
/// column is latin1, whose characters are those of Unicode's first 256.
pub fn get_varchar(rec: &Record, row: &[u8], field_id: u32) -> Option<String> {
  let (at, size, position) = place(rec, field_id).ok()?;
  if at + size > row.len() || rec.is_null(row, position) {
    return None;
  }
  let len = row[at] as usize;
  if 1 + len > size {
    return None;
  }
  let mut text = String::with_capacity(len);
  for byte in &row[at + 1..at + 1 + len] {
    text.push(*byte as char);
  }
  Some(text)
}

/// Send and poll until the transaction is done, then let it go: how it
/// ended, and the error it ended with, if any.
pub fn finish(
  conn: &mut ApidConnection,
  trans: TransId,
) -> Result<(CommitState, Option<IcError>), IcError> {
  loop {
    let ended = match conn.transaction(trans) {
      Some(t) if t.is_done() => Some((t.commit_state(), t.error())),
      Some(_) => None,
      None => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
    };
    if let Some(ended) = ended {
      conn.close_transaction(trans)?;
      return Ok(ended);
    }
    conn.flush(100, true)?;
  }
}

/// As [`finish`], for a transaction meant to commit: the error that
/// ended it otherwise, the transaction's or else its query's.
pub fn committed(
  conn: &mut ApidConnection,
  trans: TransId,
  query: QueryId,
) -> Result<(), IcError> {
  let (state, error) = finish(conn, trans)?;
  if state == CommitState::Committed {
    return Ok(());
  }
  if let Some(e) = error {
    return Err(e);
  }
  if let Some(q) = conn.query(query) {
    if let Some(e) = q.error() {
      return Err(e);
    }
  }
  Err(IcError::new(IC_NDB_ERROR_NO_REPLY))
}

/// Write the scan examples' rows, over whatever an earlier run left:
/// ids `EX_SCAN_FIRST` up, code the id, val the id negated, and name
/// `row` and the id's last digit.
pub fn write_scan_rows(
  conn: &mut ApidConnection,
  t1: &T1,
) -> Result<(), IcError> {
  let mut row = t1.row();
  let mut id = EX_SCAN_FIRST;
  while id < EX_SCAN_FIRST + EX_SCAN_ROWS {
    let name = format!("row {}", id % 10);
    let values = Values {
      id,
      val: Some(-(id as i32)),
      code: id,
      name: Some(&name),
    };
    row.fill(0);
    t1.fill(&t1.rec, &mut row, &values)?;
    key_op::write_key(
      conn,
      key_ops::IC_OP_WRITE,
      &t1.rec,
      &row,
      &t1.rec,
      &row,
    )?;
    id += 1;
  }
  Ok(())
}

/// A row of `val_rec`, a record over val alone, holding `val`: a bound
/// of a range of the index on val.
pub fn val_row(
  t1: &T1,
  val_rec: &Record,
  val: i32,
) -> Result<Vec<u8>, IcError> {
  let mut row = vec![0; val_rec.row_size() as usize];
  put_i32(val_rec, &mut row, t1.val, Some(val))?;
  Ok(row)
}
