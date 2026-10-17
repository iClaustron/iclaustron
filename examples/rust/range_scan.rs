// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! range_scan: rows found by ranges of the ordered index on val, in the
//! index's order. A range's bounds are rows of a record over the
//! index's columns, here val alone: from -250 up to but not -240, then
//! the same from the high end; then three ranges at once, each row
//! coming with the number of the range it is in.
//!
//! ```text
//! cargo run -p ic_examples --example range_scan -- [connectstring]
//! ```

use ic_apid::apid_conn::ApidConnection;
use ic_apid::index_scan::ScanRange;
use ic_apid::scan::ScanArgs;
use ic_apid::scan::ScanId;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::get_i32;
use ic_examples::val_row;
use ic_examples::write_scan_rows;
use ic_examples::EX_DB;
use ic_examples::EX_TABLE;
use ic_examples::EX_VAL_INDEX;
use ic_examples::T1;
use ic_port::IcError;

/// Read a run to its end, printing each row's val after `what`, with
/// its range's number when `numbered`.
fn print_run(
  conn: &mut ApidConnection,
  t1: &T1,
  scan: ScanId,
  what: &str,
  numbered: bool,
) -> Result<(), IcError> {
  let mut line = format!("{}:", what);
  while conn.next_row(scan)? {
    let val = get_i32(&t1.rec, conn.scan_row(scan), t1.val).unwrap_or(0);
    if numbered {
      line.push_str(&format!(" {}/{}", conn.scan_range_no(scan), val));
    } else {
      line.push_str(&format!(" {}", val));
    }
  }
  println!("{}", line);
  Ok(())
}

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_range_scan")?;
  let t1 = T1::bind(&mut conn)?;
  write_scan_rows(&mut conn, &t1)?;
  // A MySQL server names the ordered index of KEY t1_val t1_val.
  let index = conn.index_bind(EX_DB, EX_VAL_INDEX, EX_TABLE)?;
  let val_rec = t1.record_over(&[t1.val])?;
  let scan = conn.create_index_scan(&index, &val_rec, &t1.rec)?;

  // val from -250, included, up to -240, not included; each run takes
  // the ranges added before it.
  let low = val_row(&t1, &val_rec, -250)?;
  let high = val_row(&t1, &val_rec, -240)?;
  let range = ScanRange {
    low_columns: 1,
    low_inclusive: true,
    high_columns: 1,
    high_inclusive: false,
    range_no: 0,
  };
  for descending in [false, true] {
    conn.add_scan_range(scan, &range, &low, &high)?;
    let args = ScanArgs {
      // Merged from every fragment in the index's order.
      ordered: true,
      descending,
      ..ScanArgs::default()
    };
    conn.start_scan(scan, &args)?;
    let mut what = "from -250 to -240";
    if descending {
      what = "the same from the high end";
    }
    print_run(&mut conn, &t1, scan, what, false)?;
  }

  // Three ranges, numbered in the order added: val = -299; val from
  // -210 to -205; val from -201 to -200. The rows come range by range.
  let ranges: [(u32, i32, i32); 3] =
    [(0, -299, -299), (1, -210, -205), (2, -201, -200)];
  for (range_no, from, to) in ranges {
    let both = ScanRange {
      low_columns: 1,
      low_inclusive: true,
      high_columns: 1,
      high_inclusive: true,
      range_no,
    };
    let from_row = val_row(&t1, &val_rec, from)?;
    let to_row = val_row(&t1, &val_rec, to)?;
    conn.add_scan_range(scan, &both, &from_row, &to_row)?;
  }
  let ordered = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &ordered)?;
  print_run(&mut conn, &t1, scan, "range/val", true)?;
  conn.free_scan(scan)
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
