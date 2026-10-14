// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! key_ops: insert, read, update and delete by primary key, one
//! synchronous call each: each is a transaction of its own, sent and
//! waited for before the call returns.
//!
//! ```text
//! cargo run -p ic_examples --example key_ops -- [connectstring]
//! ```

use ic_apid::apid_conn::ApidConnection;
use ic_apid::key_op;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::put_u32;
use ic_examples::Values;
use ic_examples::T1;
use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;

/// Read row `id` and print it, or say it is not there.
fn read_and_print(
  conn: &mut ApidConnection,
  t1: &T1,
  id: u32,
  what: &str,
) -> Result<(), IcError> {
  let mut key = t1.row();
  put_u32(&t1.rec, &mut key, t1.id, id)?;
  let mut row = t1.row();
  if key_op::read_committed(conn, &t1.rec, &key, &t1.rec, &mut row)? {
    println!("{}: {}", what, t1.show(&row));
  } else {
    println!("{}: no row {}", what, id);
  }
  Ok(())
}

/// Write a row as the operation says: an `IC_OP_*` value.
fn write(
  conn: &mut ApidConnection,
  t1: &T1,
  operation: u32,
  values: &Values<'_>,
) -> Result<(), IcError> {
  let mut row = t1.row();
  t1.fill(&t1.rec, &mut row, values)?;
  key_op::write_key(conn, operation, &t1.rec, &row, &t1.rec, &row)
}

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_key_ops")?;
  let t1 = T1::bind(&mut conn)?;
  let mut values = Values {
    id: 1,
    val: Some(10),
    code: 100,
    name: Some("one"),
  };

  // Left from an earlier run, the row goes first; its absence is no
  // failure.
  match write(&mut conn, &t1, key_ops::IC_OP_DELETE, &values) {
    Ok(()) => {}
    Err(e) if e.code as u32 == key_op::IC_NDB_ERROR_NO_SUCH_ROW => {}
    Err(e) => return Err(e),
  }

  write(&mut conn, &t1, key_ops::IC_OP_INSERT, &values)?;
  read_and_print(&mut conn, &t1, 1, "inserted")?;

  // A second insert of the same key is refused, and the error says so.
  match write(&mut conn, &t1, key_ops::IC_OP_INSERT, &values) {
    Err(e) if e.code as u32 == key_op::IC_NDB_ERROR_ROW_EXISTS => {
      println!("a second insert: error {}, as it should", e.code)
    }
    Err(e) => return Err(e),
    Ok(()) => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  }

  values.val = None;
  values.name = Some("uno");
  write(&mut conn, &t1, key_ops::IC_OP_UPDATE, &values)?;
  read_and_print(&mut conn, &t1, 1, "updated, val NULL")?;

  write(&mut conn, &t1, key_ops::IC_OP_DELETE, &values)?;
  read_and_print(&mut conn, &t1, 1, "deleted")
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
