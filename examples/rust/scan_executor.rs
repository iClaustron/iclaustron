// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! scan_executor: scans for threads with no connection of their own. An
//! executor runs them on its connection, and each thread reads its rows
//! through a reader of its own; a batch moves to the reader without a
//! copy, and the receiver goes on once the reader has read it. Four
//! threads each read a quarter of the scan examples' rows at the same
//! time, by a range of the index on val, in the index's order, which
//! the reader merges.
//!
//! ```text
//! cargo run -p ic_examples --example scan_executor -- [connectstring]
//! ```

use std::sync::Arc;

use ic_apid::executor::Executor;
use ic_apid::executor::ExecutorOptions;
use ic_apid::executor::Submitter;
use ic_apid::index_scan::ScanRange;
use ic_apid::scan::ScanArgs;
use ic_apid::scan_reader::ScanReader;
use ic_apid::scan_reader::ScanShape;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::get_i32;
use ic_examples::val_row;
use ic_examples::write_scan_rows;
use ic_examples::EX_DB;
use ic_examples::EX_SCAN_FIRST;
use ic_examples::EX_SCAN_ROWS;
use ic_examples::EX_TABLE;
use ic_examples::EX_VAL_INDEX;
use ic_examples::T1;
use ic_port::err;
use ic_port::IcError;

/// The threads, each reading a quarter.
const THREADS: u32 = 4;

/// What one thread does: read the vals from `from` to `to` through a
/// reader of `shape`, and give how many rows it read, and whether they
/// came in order.
fn read_quarter(
  submitter: Submitter,
  shape: Arc<ScanShape>,
  t1: Arc<T1>,
  from: i32,
  to: i32,
) -> Result<(u64, bool), IcError> {
  let val_rec = t1.record_over(&[t1.val])?;
  let low = val_row(&t1, &val_rec, from)?;
  let high = val_row(&t1, &val_rec, to)?;
  let range = ScanRange {
    low_columns: 1,
    low_inclusive: true,
    high_columns: 1,
    high_inclusive: true,
    range_no: 0,
  };
  let mut reader = ScanReader::new(&shape);
  reader.add_range(&range, &low, &high)?;
  let args = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  submitter.scan(&mut reader, &args)?;
  let mut rows: u64 = 0;
  let mut last = i32::MIN;
  let mut in_order = true;
  while reader.next_row()? {
    let val = get_i32(&t1.rec, reader.row(), t1.val).unwrap_or(0);
    if val < last {
      in_order = false;
    }
    last = val;
    rows += 1;
  }
  Ok((rows, in_order))
}

fn run(args: &[String]) -> Result<(), IcError> {
  let (global, mut conn) = connect(args, "ex_scan_executor")?;
  let t1 = Arc::new(T1::bind(&mut conn)?);
  write_scan_rows(&mut conn, &t1)?;
  let executor = Executor::start(&global, &ExecutorOptions::default())?;
  let submitter = executor.submitter();
  let index = submitter.index_bind(EX_DB, EX_VAL_INDEX, EX_TABLE)?;
  let val_rec = t1.record_over(&[t1.val])?;
  // A shape is made once and shared by every reader that uses it.
  let shape = ScanShape::new_index(&index, &val_rec, &t1.rec)?;

  let quarter = EX_SCAN_ROWS / THREADS;
  let mut threads = Vec::new();
  let mut part: u32 = 0;
  while part < THREADS {
    let first = EX_SCAN_FIRST + part * quarter;
    let to = -(first as i32);
    let from = -((first + quarter - 1) as i32);
    // Each thread its own submitter, the shape and table shared.
    let mine = submitter.clone();
    let shared = Arc::clone(&shape);
    let table = Arc::clone(&t1);
    let handle =
      std::thread::spawn(move || read_quarter(mine, shared, table, from, to));
    // Each thread kept with its number, for what it found.
    threads.push((part, handle));
    part += 1;
  }
  let mut total: u64 = 0;
  for (number, thread) in threads {
    let (rows, in_order) = match thread.join() {
      Ok(result) => result?,
      Err(_) => return Err(IcError::new(err::IC_ERROR_INTERNAL_PANIC)),
    };
    println!("thread {}: {} rows, in order: {}", number, rows, in_order);
    total += rows;
  }
  println!(
    "{} rows read by {} threads through one executor",
    total, THREADS
  );
  Ok(())
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
