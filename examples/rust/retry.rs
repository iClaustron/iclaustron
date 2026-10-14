// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! retry: a transaction tried again after a temporary error.
//!
//! One transaction holds a row with an exclusive read and does not end.
//! Another updates the same row: it waits for the lock, and when the
//! data node's lock wait limit runs out it is rolled back with 266, a
//! temporary error. The program then lets the first transaction go, as
//! a conflict usually goes away by itself, and tries the update again,
//! in a new transaction, which commits.
//!
//! ```text
//! cargo run -p ic_examples --example retry -- [connectstring]
//! ```

use std::time::Duration;

use ic_apid::key_op;
use ic_apid::query::QueryState;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_examples::committed;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::finish;
use ic_examples::Values;
use ic_examples::T1;
use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;

/// How many times the update is tried.
const ATTEMPTS: u64 = 5;

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_retry")?;
  let t1 = T1::bind(&mut conn)?;
  let no_query = IcError::new(err::IC_ERROR_NO_SUCH_FIELD);
  let mut row = t1.row();
  let values = Values {
    id: 9,
    val: Some(90),
    code: 900,
    name: Some("nine"),
  };
  t1.fill(&t1.rec, &mut row, &values)?;
  key_op::write_key(
    &mut conn,
    key_ops::IC_OP_WRITE,
    &t1.rec,
    &row,
    &t1.rec,
    &row,
  )?;
  let hint = conn.hint_for_key(&t1.table, &t1.rec, &row);

  // The holder: an exclusive read, sent and completed, and the
  // transaction left open, so that it keeps the row's lock.
  let holder = conn.create_query(&t1.table, &t1.rec, &t1.rec)?;
  match conn.query_mut(holder) {
    Some(query) => query.key_row_mut().copy_from_slice(&row),
    None => return Err(no_query),
  }
  let holding = conn.start_transaction(hint, Some(&t1.table))?;
  let read = ReadKeyArgs {
    kind: ReadKind::Exclusive,
    ..ReadKeyArgs::default()
  };
  conn.read_key(holder, holding, &read)?;
  loop {
    let state = match conn.query(holder) {
      Some(query) => query.state(),
      None => return Err(no_query),
    };
    if state == QueryState::Completed || state == QueryState::Idle {
      break;
    }
    conn.flush(100, true)?;
  }
  if let Some(query) = conn.query(holder) {
    if let Some(e) = query.error() {
      return Err(e);
    }
  }
  println!("row 9 is held by another transaction");

  // The update, tried until it commits or fails for good.
  let updater = conn.create_query(&t1.table, &t1.rec, &t1.rec)?;
  let again = Values {
    id: 9,
    val: Some(91),
    code: 900,
    name: Some("nine, again"),
  };
  t1.fill(&t1.rec, &mut row, &again)?;
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  let mut holding_open = true;
  let mut attempt: u64 = 1;
  loop {
    match conn.query_mut(updater) {
      Some(query) => {
        query.key_row_mut().copy_from_slice(&row);
        query.attr_row_mut().copy_from_slice(&row);
      }
      None => return Err(no_query),
    }
    let trans = conn.start_transaction(hint, Some(&t1.table))?;
    conn.write_key(updater, trans, &update)?;
    conn.commit_transaction(trans)?;
    match committed(&mut conn, trans, updater) {
      Ok(()) => {
        println!("attempt {}: committed", attempt);
        break;
      }
      Err(e) if e.is_temporary() && attempt < ATTEMPTS => {
        println!(
          "attempt {}: error {}, {}; temporary, so again",
          attempt,
          e.code,
          e.message()
        );
      }
      Err(e) => return Err(e),
    }
    if holding_open {
      // The conflict goes away: the holder rolls back.
      conn.rollback_transaction(holding)?;
      finish(&mut conn, holding)?;
      holding_open = false;
    }
    // A little longer before each attempt.
    std::thread::sleep(Duration::from_millis(100 * attempt));
    attempt += 1;
  }
  conn.free_query(holder)?;
  conn.free_query(updater)
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
