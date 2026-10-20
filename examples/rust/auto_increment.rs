// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! auto_increment: values of an AUTO_INCREMENT column asked for and put
//! in the row, from the counter MySQL servers take theirs from
//! (doc/user-guide.md, section 7).
//!
//! - Three rows of `ic_ex.t2`, each value asked for alone: the first
//!   refills the range the process holds for the table, 4096 values in
//!   one round trip, and the others take from it.
//! - Five rows whose ids are one block, reserved together.
//! - A row with an id of the application's own, past the counter, which
//!   it then says it used, so that the counter moves past it for every
//!   client.
//! - `ic_ex.t3`, a table without a primary key: its hidden key, `$PK`,
//!   is its AUTO_INCREMENT column and takes its values the same way.
//!   Its rows are read back by a scan, which returns the key, and
//!   deleted by it.
//!
//! ```text
//! cargo run -p ic_examples --example auto_increment -- [connectstring]
//! ```

use std::sync::Arc;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::dict_cache::TableDef;
use ic_apid::key_op;
use ic_apid::record::Record;
use ic_apid::scan::ScanArgs;
use ic_examples::check;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::get_i32;
use ic_examples::get_u64;
use ic_examples::put_i32;
use ic_examples::put_u64;
use ic_examples::EX_DB;
use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;

/// A table with an AUTO_INCREMENT column, bound, with a record over
/// every field and the ids of the two the example uses.
struct Counted {
  table: Arc<TableDef>,
  rec: Record,
  /// The AUTO_INCREMENT column: t2's id, t3's hidden key.
  key: u32,
  /// `val INT`.
  val: u32,
}

impl Counted {
  fn bind(conn: &mut ApidConnection, name: &str) -> Result<Counted, IcError> {
    let table = conn.table_bind(EX_DB, name)?;
    let key = match table.auto_increment_column() {
      Some(key) => key,
      None => return Err(IcError::new(err::IC_ERROR_NO_AUTO_INCREMENT)),
    };
    let rec = Record::default_for(&table, None)?;
    Ok(Counted {
      key,
      val: table.field_id("val")?,
      rec,
      table,
    })
  }

  /// Write a row of `key` and `val`, as `operation` says: an `IC_OP_*`
  /// value.
  fn write(
    &self,
    conn: &mut ApidConnection,
    operation: u32,
    key: u64,
    val: i32,
  ) -> Result<(), IcError> {
    let mut row = vec![0; self.rec.row_size() as usize];
    put_u64(&self.rec, &mut row, self.key, key)?;
    put_i32(&self.rec, &mut row, self.val, Some(val))?;
    key_op::write_key(conn, operation, &self.rec, &row, &self.rec, &row)
  }

  /// The val of row `key`: `None` if there is no such row, `Some(None)`
  /// if its val is NULL.
  fn val_of(
    &self,
    conn: &mut ApidConnection,
    key: u64,
  ) -> Result<Option<Option<i32>>, IcError> {
    let mut key_row = vec![0; self.rec.row_size() as usize];
    put_u64(&self.rec, &mut key_row, self.key, key)?;
    let mut row = vec![0; self.rec.row_size() as usize];
    if !key_op::read_committed(conn, &self.rec, &key_row, &self.rec, &mut row)?
    {
      return Ok(None);
    }
    Ok(Some(get_i32(&self.rec, &row, self.val)))
  }
}

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_auto_increment")?;
  let t2 = Counted::bind(&mut conn, "t2")?;

  // Each value asked for alone, and put in the row.
  let before = conn.auto_increment_peek(&t2.table)?;
  let mut ids: Vec<u64> = Vec::new();
  while ids.len() < 3 {
    let id = conn.auto_increment(&t2.table)?;
    t2.write(&mut conn, key_ops::IC_OP_INSERT, id, ids.len() as i32)?;
    println!("inserted id {}", id);
    ids.push(id);
  }
  let after = conn.auto_increment_peek(&t2.table)?;
  println!(
    "the counter went from {} to {}: {} values reserved by one refill",
    before,
    after,
    after - before
  );
  check(
    ids[0] >= before && ids[1] == ids[0] + 1 && ids[2] == ids[1] + 1,
    "the three ids follow on from the counter",
  )?;
  check(after >= before + 4096, "a refill reserves 4096 values")?;

  // A block of five, for an insert of many rows.
  let first = conn.auto_increment_block(&t2.table, 5)?;
  let mut next: u64 = 0;
  while next < 5 {
    t2.write(&mut conn, key_ops::IC_OP_INSERT, first + next, 10)?;
    next += 1;
  }
  println!("a block of five: ids {} to {}", first, first + 4);
  check(first == ids[2] + 1, "the block follows on from the range")?;

  // An id of the application's own, past the counter. Said, it moves
  // the counter past it, and what was left of this process's range is
  // a gap.
  let own = conn.auto_increment_peek(&t2.table)? + 1000;
  t2.write(&mut conn, key_ops::IC_OP_INSERT, own, 99)?;
  conn.auto_increment_seen(&t2.table, own)?;
  let past = conn.auto_increment(&t2.table)?;
  t2.write(&mut conn, key_ops::IC_OP_INSERT, past, 100)?;
  println!(
    "an id of our own, {}; the next value, {}, is past it",
    own, past
  );
  check(past > own, "the next value is past the id of our own")?;
  check(
    t2.val_of(&mut conn, own)? == Some(Some(99)),
    "the row of our own id is there",
  )?;

  // A table without a primary key: the hidden key is the AUTO_INCREMENT
  // column, the last.
  let t3 = Counted::bind(&mut conn, "t3")?;
  check(
    t3.table.hidden_key() == Some(t3.key),
    "t3's AUTO_INCREMENT column is its hidden key",
  )?;
  let name = match t3.table.field(t3.key) {
    Some(attr) => attr.name.clone(),
    None => String::new(),
  };
  let mut keys: Vec<u64> = Vec::new();
  while keys.len() < 2 {
    let key = conn.auto_increment(&t3.table)?;
    t3.write(&mut conn, key_ops::IC_OP_INSERT, key, keys.len() as i32 + 1)?;
    keys.push(key);
  }
  println!(
    "t3 has no primary key; two rows, {} {} and {}",
    name, keys[0], keys[1]
  );
  check(keys[1] == keys[0] + 1, "the hidden keys follow on")?;

  // A scan returns the hidden key with each row, so a row it finds can
  // be read, updated or deleted by it.
  let scan = conn.create_scan(&t3.table, &t3.rec)?;
  conn.start_scan(scan, &ScanArgs::default())?;
  let mut ours: usize = 0;
  while conn.next_row(scan)? {
    let row = conn.scan_row(scan);
    match (get_u64(&t3.rec, row, t3.key), get_i32(&t3.rec, row, t3.val)) {
      (Some(key), Some(val)) => {
        println!("scanned: {} {}, val {}", name, key, val);
        if (key == keys[0] && val == 1) || (key == keys[1] && val == 2) {
          ours += 1;
        }
      }
      (Some(key), None) => println!("scanned: {} {}, val NULL", name, key),
      (None, _) => check(false, "every scanned row has its hidden key")?,
    }
  }
  if let Some(e) = conn.scan(scan).and_then(|done| done.error()) {
    return Err(e);
  }
  conn.free_scan(scan)?;
  for key in &keys {
    t3.write(&mut conn, key_ops::IC_OP_DELETE, *key, 0)?;
  }
  check(ours == 2, "the scan finds both rows, with their values")?;
  for key in &keys {
    check(
      t3.val_of(&mut conn, *key)?.is_none(),
      "the rows are deleted",
    )?;
  }
  println!("both deleted by their hidden keys");
  Ok(())
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
  println!("auto_increment: all checks passed");
}
