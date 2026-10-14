// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! pipeline: write many rows asynchronously. A number of queries are in
//! flight at once, each a transaction of one write; when one completes,
//! its callback lets its transaction go and starts the next write with
//! the same query, so the pipeline stays full until every row is
//! written. The main loop only sends and polls.
//!
//! A callback is a plain function with its `user_ref` for context, so
//! what it shares with `main` is in statics: the table, set once, and
//! counters.
//!
//! ```text
//! cargo run -p ic_examples --example pipeline -- \
//!   [connectstring] [rows] [in flight]
//! ```

use std::sync::atomic::AtomicI32;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::sync::OnceLock;
use std::time::Instant;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::query::QueryId;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::transaction::TransactionHint;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::Values;
use ic_examples::T1;
use ic_port::err;
use ic_port::IcError;

/// The rows get ids from here up, clear of the other examples' rows.
const FIRST_ID: u32 = 1000;
/// The most queries in flight.
const MAX_IN_FLIGHT: u32 = 256;

/// The table, set once before the first write.
static TABLE: OnceLock<T1> = OnceLock::new();
/// The next id to write, and one past the last.
static NEXT: AtomicU32 = AtomicU32::new(0);
static END: AtomicU32 = AtomicU32::new(0);
/// Writes completed, and how many of them failed, with the first error.
static DONE: AtomicU32 = AtomicU32::new(0);
static FAILED: AtomicU32 = AtomicU32::new(0);
static FIRST_ERROR: AtomicI32 = AtomicI32::new(0);

fn note_failure(code: i32) {
  if FAILED.fetch_add(1, Ordering::Relaxed) == 0 {
    FIRST_ERROR.store(code, Ordering::Relaxed);
  }
}

/// Start the write of row `id` with query `qid`: fill its rows, start a
/// transaction where the row lives, define the write, ask to commit. It
/// goes with the next send.
fn start_write(
  conn: &mut ApidConnection,
  qid: QueryId,
  id: u32,
) -> Result<(), IcError> {
  let t1 = match TABLE.get() {
    Some(t1) => t1,
    None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  };
  let values = Values {
    id,
    val: Some(id as i32 * 2),
    code: id,
    name: Some("piped"),
  };
  match conn.query_mut(qid) {
    Some(query) => {
      t1.fill(&t1.rec, query.key_row_mut(), &values)?;
      t1.fill(&t1.rec, query.attr_row_mut(), &values)?;
    }
    None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
  }
  let hint = match conn.query(qid) {
    Some(query) => conn.hint_for_key(&t1.table, &t1.rec, query.key_row()),
    None => TransactionHint::Any,
  };
  let trans = conn.start_transaction(hint, Some(&t1.table))?;
  let args = WriteKeyArgs {
    // Insert, or overwrite what an earlier run wrote.
    kind: WriteKind::Write,
    callback: Some(on_written),
    ..WriteKeyArgs::default()
  };
  conn.write_key(qid, trans, &args)?;
  conn.commit_transaction(trans)
}

/// Called from inside a poll when a write completes. Its transaction is
/// done: the commit went with the write.
fn on_written(conn: &mut ApidConnection, qid: QueryId, _user_ref: usize) {
  let (error, trans) = match conn.query(qid) {
    Some(query) => (query.error(), query.transaction()),
    None => return,
  };
  DONE.fetch_add(1, Ordering::Relaxed);
  if let Some(e) = error {
    note_failure(e.code);
  }
  if let Some(trans) = trans {
    let _ = conn.close_transaction(trans);
  }
  loop {
    let id = NEXT.fetch_add(1, Ordering::Relaxed);
    if id >= END.load(Ordering::Relaxed) {
      return;
    }
    match start_write(conn, qid, id) {
      Ok(()) => return,
      Err(e) => {
        // Not started: counted as done, and failed.
        DONE.fetch_add(1, Ordering::Relaxed);
        note_failure(e.code);
      }
    }
  }
}

/// A number from the arguments, or the default.
fn number(args: &[String], at: usize, default: u32) -> u32 {
  match args.get(at) {
    Some(text) => text.parse().unwrap_or(default),
    None => default,
  }
}

fn run(args: &[String]) -> Result<(), IcError> {
  let rows = number(args, 2, 10_000);
  let mut width = number(args, 3, 32);
  if width == 0 || width > MAX_IN_FLIGHT {
    width = MAX_IN_FLIGHT;
  }
  if width > rows {
    width = rows;
  }
  let (_global, mut conn) = connect(args, "ex_pipeline")?;
  let bound = T1::bind(&mut conn)?;
  let _ = TABLE.set(bound);
  let t1 = match TABLE.get() {
    Some(t1) => t1,
    None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  };
  NEXT.store(FIRST_ID, Ordering::Relaxed);
  END.store(FIRST_ID + rows, Ordering::Relaxed);

  let start = Instant::now();
  let mut queries: Vec<QueryId> = Vec::with_capacity(width as usize);
  let mut i: u32 = 0;
  while i < width {
    let qid = conn.create_query(&t1.table, &t1.rec, &t1.rec)?;
    queries.push(qid);
    let id = NEXT.fetch_add(1, Ordering::Relaxed);
    start_write(&mut conn, qid, id)?;
    i += 1;
  }
  // Send what is defined and take the replies; the callbacks, run
  // inside, keep the pipeline full. Without force, a send may wait a
  // moment for company, never longer than the adaptive send allows.
  while DONE.load(Ordering::Relaxed) < rows {
    conn.flush(10, false)?;
  }
  let seconds = start.elapsed().as_secs_f64();
  let mut rate = 0.0;
  if seconds > 0.0 {
    rate = rows as f64 / seconds;
  }
  println!(
    "{} rows written, {} at a time, in {:.3} s: {:.0} rows/s",
    rows, width, seconds, rate
  );
  if FAILED.load(Ordering::Relaxed) > 0 {
    return Err(IcError::new(FIRST_ERROR.load(Ordering::Relaxed)));
  }
  for qid in queries {
    conn.free_query(qid)?;
  }
  Ok(())
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
