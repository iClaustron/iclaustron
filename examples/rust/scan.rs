// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! scan: every row of the table, read by a scan of every fragment at
//! once. The scan is made once and run twice: row by row, then a batch
//! at a time, each batch handed back once its rows are read, which is
//! what lets that fragment's receiver go on.
//!
//! ```text
//! cargo run -p ic_examples --example scan -- [connectstring]
//! ```

use ic_apid::scan::ScanArgs;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::get_i32;
use ic_examples::write_scan_rows;
use ic_examples::EX_SCAN_FIRST;
use ic_examples::EX_SCAN_ROWS;
use ic_examples::T1;
use ic_port::IcError;

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_scan")?;
  let t1 = T1::bind(&mut conn)?;
  write_scan_rows(&mut conn, &t1)?;
  let scan = conn.create_scan(&t1.table, &t1.rec)?;

  // Row by row: committed reads, with no lock, of every fragment at
  // once. next_row waits for batches and hands each back as it goes.
  conn.start_scan(scan, &ScanArgs::default())?;
  let mut rows: u64 = 0;
  let mut ours: u64 = 0;
  let lowest = -((EX_SCAN_FIRST + EX_SCAN_ROWS - 1) as i32);
  let highest = -(EX_SCAN_FIRST as i32);
  while conn.next_row(scan)? {
    rows += 1;
    if let Some(val) = get_i32(&t1.rec, conn.scan_row(scan), t1.val) {
      if (lowest..=highest).contains(&val) {
        ours += 1;
      }
    }
  }
  println!("{} rows, {} of them the scan examples'", rows, ours);

  // A batch at a time, at most 16 rows each: the program holds each
  // batch for as long as it likes, and hands it back.
  let args = ScanArgs {
    batch_rows: 16,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args)?;
  let mut batches: u32 = 0;
  let mut again: u64 = 0;
  while let Some(batch) = conn.next_batch(scan)? {
    batches += 1;
    while conn.next_scan_row(batch)? {
      again += 1;
    }
    conn.release_scan_batch(batch)?;
  }
  println!("again: {} rows in {} batches", again, batches);
  conn.free_scan(scan)
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
