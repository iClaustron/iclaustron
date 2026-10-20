// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! conditional_update: programs that run in the data nodes, on the row
//! an operation holds there (doc/user-guide.md, section 6).
//!
//! - An update of a row's name that happens only while its val is still
//!   the value the application read, as an optimistic lock: once with
//!   val as it is, which commits, and once with a val that is out of
//!   date, which the data node refuses with 6000.
//! - A counter: val plus an input, with nothing read first, and the new
//!   value read back after the update, all in one round trip.
//! - A scan whose filter passes over, on the data nodes, every row but
//!   this example's with val above an input and a name.
//!
//! ```text
//! cargo run -p ic_examples --example conditional_update -- [connectstring]
//! ```

use std::sync::Arc;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::key_op;
use ic_apid::program::Cmp;
use ic_apid::program::Program;
use ic_apid::program::ProgramBuilder;
use ic_apid::query::AbortOption;
use ic_apid::query::QueryId;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::scan::ScanArgs;
use ic_apid::transaction::CommitState;
use ic_apid::transaction::TransactionHint;
use ic_examples::check;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::finish;
use ic_examples::get_i32;
use ic_examples::get_u32;
use ic_examples::get_varchar;
use ic_examples::put_u32;
use ic_examples::put_varchar;
use ic_examples::Values;
use ic_examples::T1;
use ic_port::err;
use ic_port::IcError;
use ic_signals::interp::IC_REFUSE_CONDITION;
use ic_signals::key_ops;

/// This example's rows: ids from here up, clear of the other examples'
/// rows, code the same as id, val ten times the row's place, and a name
/// on every other row.
const EX_COND_FIRST: u32 = 300;
/// How many rows it writes.
const EX_COND_ROWS: u32 = 10;
/// The filter's bound on val: rows 5 to 9 are above it, and of those
/// 5, 7 and 9 have a name.
const EX_COND_VAL_ABOVE: i64 = 40;

/// Write this example's rows, over whatever an earlier run left.
fn write_rows(conn: &mut ApidConnection, t1: &T1) -> Result<(), IcError> {
  let mut row = t1.row();
  let mut place: u32 = 0;
  while place < EX_COND_ROWS {
    let name = format!("row {}", place);
    let values = Values {
      id: EX_COND_FIRST + place,
      val: Some((place * 10) as i32),
      code: EX_COND_FIRST + place,
      name: if place % 2 == 1 {
        Some(name.as_str())
      } else {
        None
      },
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
    place += 1;
  }
  Ok(())
}

/// The optimistic update's condition: val is still input 0.
fn val_still(t1: &T1) -> Result<Arc<Program>, IcError> {
  let mut b = ProgramBuilder::new(&t1.table);
  let val = b.column(t1.val);
  let read = b.input(0);
  let same = b.compare(Cmp::Eq, val, read);
  b.finish(Some(same))
}

/// The counter: val becomes val plus input 0. No condition, only the
/// assignment.
fn val_plus(t1: &T1) -> Result<Arc<Program>, IcError> {
  let mut b = ProgramBuilder::new(&t1.table);
  let val = b.column(t1.val);
  let step = b.input(0);
  let sum = b.add(val, step);
  b.assign(t1.val, sum);
  b.finish(None)
}

/// The scan's filter: id from input 1 up to below input 2, which are
/// this example's rows, val above input 0, and a name.
fn filter(t1: &T1) -> Result<Arc<Program>, IcError> {
  let mut b = ProgramBuilder::new(&t1.table);
  let id = b.column(t1.id);
  let from = b.input(1);
  let low = b.compare(Cmp::Ge, id, from);
  let id = b.column(t1.id);
  let to = b.input(2);
  let high = b.compare(Cmp::Lt, id, to);
  let ours = b.and(low, high);
  let val = b.column(t1.val);
  let limit = b.input(0);
  let above = b.compare(Cmp::Gt, val, limit);
  let name = b.column(t1.name);
  let named = b.is_not_null(name);
  let wanted = b.and(above, named);
  let both = b.and(ours, wanted);
  b.finish(Some(both))
}

/// Define an update on `query` as a transaction of its own, ask to
/// commit and wait: `None` if it committed, else the error it ended
/// with, the query's or the transaction's. The query is taken back from
/// the executed list, idle for its next use.
fn update_and_wait(
  conn: &mut ApidConnection,
  t1: &T1,
  query: QueryId,
  abort_option: AbortOption,
) -> Result<Option<IcError>, IcError> {
  let trans = conn.start_transaction(TransactionHint::Any, Some(&t1.table))?;
  let args = WriteKeyArgs {
    kind: WriteKind::Update,
    abort_option,
    ..WriteKeyArgs::default()
  };
  conn.write_key(query, trans, &args)?;
  conn.commit_transaction(trans)?;
  let (state, trans_error) = finish(conn, trans)?;
  let query_error = conn.query(query).and_then(|done| done.error());
  while conn.get_next_executed_query().is_some() {}
  if state == CommitState::Committed {
    return Ok(None);
  }
  match query_error.or(trans_error) {
    Some(e) => Ok(Some(e)),
    None => Ok(Some(IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK))),
  }
}

/// Read row `id`, which must be there, and print it.
fn read_and_print(
  conn: &mut ApidConnection,
  t1: &T1,
  id: u32,
  what: &str,
) -> Result<Vec<u8>, IcError> {
  let mut key = t1.row();
  put_u32(&t1.rec, &mut key, t1.id, id)?;
  let mut row = t1.row();
  let found = key_op::read_committed(conn, &t1.rec, &key, &t1.rec, &mut row)?;
  check(found, "the row is there")?;
  println!("{}: {}", what, t1.show(&row));
  Ok(row)
}

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_conditional_update")?;
  let t1 = T1::bind(&mut conn)?;
  write_rows(&mut conn, &t1)?;
  let id_rec = t1.record_over(&[t1.id])?;
  let name_rec = t1.record_over(&[t1.name])?;
  let val_rec = t1.record_over(&[t1.val])?;

  // 1. The optimistic update of row 300's name, whose val is 0. Its
  // program and records are made once; each use sets the input.
  let still = val_still(&t1)?;
  let guarded = conn.create_query(&t1.table, &id_rec, &name_rec)?;
  for (expected, name) in [(0, "checked"), (5, "too late")] {
    let query = match conn.query_mut(guarded) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    query.set_program(Some(&still))?;
    query.set_input(0, expected)?;
    put_u32(&id_rec, query.key_row_mut(), t1.id, EX_COND_FIRST)?;
    put_varchar(&name_rec, query.attr_row_mut(), t1.name, Some(name))?;
    // A missed condition rolls the transaction back with 6000 where the
    // update aborts on error, and fails this update alone with 626
    // where it ignores errors.
    let ended =
      update_and_wait(&mut conn, &t1, guarded, AbortOption::AbortOnError)?;
    match (expected, ended) {
      (0, None) => println!("val still {}: the update committed", expected),
      (5, Some(e)) if e.code == i32::from(IC_REFUSE_CONDITION) => println!(
        "val not {} any more: refused with {}, the row as it was",
        expected, e.code
      ),
      (_, Some(e)) => return Err(e),
      (_, None) => check(false, "an update expecting val 5 is refused")?,
    }
  }
  let row = read_and_print(&mut conn, &t1, EX_COND_FIRST, "row 300")?;
  check(
    get_i32(&t1.rec, &row, t1.val) == Some(0)
      && get_varchar(&t1.rec, &row, t1.name).as_deref() == Some("checked"),
    "row 300 is val 0, name 'checked': the refused update changed nothing",
  )?;

  // 2. The counter on row 301, whose val is 10: the data node adds, and
  // the update reads the new value back. Its attribute row holds the key
  // alone, which leaves val to the program.
  let plus = val_plus(&t1)?;
  let counter = conn.create_query(&t1.table, &id_rec, &id_rec)?;
  match conn.query_mut(counter) {
    Some(query) => {
      query.set_program(Some(&plus))?;
      query.set_reads_after(Some(&val_rec))?;
      query.set_input(0, 5)?;
      put_u32(&id_rec, query.key_row_mut(), t1.id, EX_COND_FIRST + 1)?;
      put_u32(&id_rec, query.attr_row_mut(), t1.id, EX_COND_FIRST + 1)?;
    }
    None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
  }
  let mut times: u32 = 0;
  while times < 2 {
    let ended = update_and_wait(&mut conn, &t1, counter, AbortOption::Default)?;
    if let Some(e) = ended {
      return Err(e);
    }
    let after = conn
      .query(counter)
      .and_then(|done| get_i32(&val_rec, done.after_row(), t1.val));
    let expected = 10 + 5 * (times as i32 + 1);
    check(
      after == Some(expected),
      "the counter reads back 15, then 20",
    )?;
    println!("row 301: val plus 5, read back after: {}", expected);
    times += 1;
  }

  // 3. A scan of the whole table, the filter run on every row in the data
  // nodes: only rows 305, 307 and 309 come back.
  let wanted = filter(&t1)?;
  let scan = conn.create_scan(&t1.table, &t1.rec)?;
  conn.set_scan_program(scan, Some(&wanted))?;
  conn.set_scan_input(scan, 0, EX_COND_VAL_ABOVE)?;
  conn.set_scan_input(scan, 1, i64::from(EX_COND_FIRST))?;
  conn.set_scan_input(scan, 2, i64::from(EX_COND_FIRST + EX_COND_ROWS))?;
  conn.start_scan(scan, &ScanArgs::default())?;
  let mut ids: Vec<u32> = Vec::new();
  while conn.next_row(scan)? {
    ids.push(get_u32(&t1.rec, conn.scan_row(scan), t1.id).unwrap_or(0));
  }
  if let Some(e) = conn.scan(scan).and_then(|done| done.error()) {
    return Err(e);
  }
  ids.sort_unstable();
  println!(
    "the filter, val above {} and a name, let through rows {:?}",
    EX_COND_VAL_ABOVE, ids
  );
  check(
    ids == [305, 307, 309],
    "the filter lets through 305, 307, 309",
  )?;
  conn.free_scan(scan)?;
  conn.free_query(guarded)?;
  conn.free_query(counter)
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
  println!("conditional_update: all checks passed");
}
