// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! scan_update: rows locked by a scan and updated through it, as
//! `UPDATE ... WHERE` does. An exclusive scan of a range of the index on
//! val runs on a transaction; each row's lock is taken over by an update
//! of its name, a query on that transaction; one commit ends them all.
//! A batch's receiver goes on only once the batch's takeovers are done.
//!
//! The name is updated, not val: changing the column a scan goes by
//! would move the row within the index being scanned.
//!
//! ```text
//! cargo run -p ic_examples --example scan_update -- [connectstring]
//! ```

use ic_apid::index_scan::ScanRange;
use ic_apid::key_op;
use ic_apid::query::QueryId;
use ic_apid::scan::ScanArgs;
use ic_apid::scan::ScanLock;
use ic_apid::scan::TakeoverArgs;
use ic_apid::transaction::TransactionHint;
use ic_examples::committed;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::get_u32;
use ic_examples::get_varchar;
use ic_examples::put_u32;
use ic_examples::put_varchar;
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

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_scan_update")?;
  let t1 = T1::bind(&mut conn)?;
  write_scan_rows(&mut conn, &t1)?;
  let index = conn.index_bind(EX_DB, EX_VAL_INDEX, EX_TABLE)?;
  let val_rec = t1.record_over(&[t1.val])?;
  let scan = conn.create_index_scan(&index, &val_rec, &t1.rec)?;

  // A query for each row: its key comes from the scanned row, and its
  // attribute row holds the new name.
  let id_rec = t1.record_over(&[t1.id])?;
  let name_rec = t1.record_over(&[t1.name])?;
  let mut queries: Vec<QueryId> = Vec::with_capacity(EX_SCAN_ROWS as usize);
  while queries.len() < EX_SCAN_ROWS as usize {
    queries.push(conn.create_query(&t1.table, &id_rec, &name_rec)?);
  }

  // Every scan example row: val from the lowest id's to the highest's.
  let lowest = -((EX_SCAN_FIRST + EX_SCAN_ROWS - 1) as i32);
  let highest = -(EX_SCAN_FIRST as i32);
  let low = val_row(&t1, &val_rec, lowest)?;
  let high = val_row(&t1, &val_rec, highest)?;
  let range = ScanRange {
    low_columns: 1,
    low_inclusive: true,
    high_columns: 1,
    high_inclusive: true,
    range_no: 0,
  };
  conn.add_scan_range(scan, &range, &low, &high)?;
  let trans = conn.start_transaction(TransactionHint::Any, Some(&t1.table))?;
  let args = ScanArgs {
    lock: ScanLock::Exclusive,
    // The transaction the locks are taken over into.
    trans: Some(trans),
    batch_rows: 32,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args)?;
  let mut used: usize = 0;
  while conn.next_row(scan)? {
    let id = get_u32(&t1.rec, conn.scan_row(scan), t1.id).unwrap_or(0);
    let qid = match queries.get(used) {
      Some(qid) => *qid,
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    };
    used += 1;
    let name = format!("updated {}", id);
    match conn.query_mut(qid) {
      Some(query) => {
        put_varchar(&name_rec, query.attr_row_mut(), t1.name, Some(&name))?
      }
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    }
    // The update goes with the next send; next_row sends it before it
    // waits for the next batch.
    conn.update_scan_row(qid, scan, &TakeoverArgs::default())?;
  }
  if let Some(e) = conn.scan(scan).and_then(|done| done.error()) {
    return Err(e);
  }
  conn.commit_transaction(trans)?;
  committed(&mut conn, trans, queries[used.saturating_sub(1)])?;
  println!("{} rows updated by one scan and one commit", used);

  // One of them, read back.
  let mut key = t1.row();
  let first = EX_SCAN_FIRST;
  put_u32(&t1.rec, &mut key, t1.id, first)?;
  let mut row = t1.row();
  if key_op::read_committed(&mut conn, &t1.rec, &key, &t1.rec, &mut row)? {
    let name = get_varchar(&t1.rec, &row, t1.name).unwrap_or_default();
    println!("row {} now: name '{}'", first, name);
  }
  while conn.get_next_executed_query().is_some() {}
  for qid in queries {
    conn.free_query(qid)?;
  }
  conn.free_scan(scan)
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
