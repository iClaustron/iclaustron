// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! AUTO_INCREMENT values (chapter 04, "AUTO_INCREMENT"; chapter 05
//! §16.5).
//!
//! **Asked for, not filled in.** The application asks for a value with
//! [`ApidConnection::auto_increment`], or for a block of them, and
//! puts it in its row; after an insert with a value of its own it says
//! so with [`ApidConnection::auto_increment_seen`], which moves the
//! counter past it, only ever up. Defining an insert stays as it was.
//! A [`Submitter`] has the same calls, for a thread with no connection
//! of its own.
//!
//! **The counter** is one row per table in the counter table, stored
//! as `sys/def/SYSTAB_0`, keyed by the table id (column 0) and holding
//! the next value no client has reserved (column 1). Every client of
//! the cluster takes its values from there, MySQL servers included, so
//! values are unique across them though not in order.
//!
//! **One range per table for the whole process**, on the table's shared
//! description under a mutex: the values reserved and not yet handed
//! out. Taking a value is a step under the lock. The thread that finds
//! the range used up refills it, one interpreted update adding the
//! values reserved to the counter and reading back what it became,
//! holding the lock for that round trip while other threads wait.
//!
//! **The counter's own connection.** The updates go on a connection the
//! process keeps for them, made at first use, one operation at a time
//! under its mutex, which is taken under a table's range and never the
//! other way round. A refill on the caller's connection would send the
//! queries it had defined and not yet sent, and its poll would run
//! callbacks that may themselves ask for values; on a connection of
//! its own a refill does neither, and a callback may ask for a value.
//!
//! **Retried inside the call.** A temporary error is tried again, the
//! first pause 100 µs and doubled after each failure; once 3.6 s have
//! passed since the first attempt the call fails with the last error.
//! An update whose outcome is unknown at worst leaves a gap, never a
//! duplicate. A table's lock is held throughout, the counter's
//! connection only for each attempt.
//!
//! **Step and offset** are the process's, as MySQL's
//! `auto_increment_increment` and `auto_increment_offset`: a value is
//! the least k × step + offset above the last handed out, and a refill
//! reserves prefetch × step. The hidden key of a table without a
//! primary key takes 1 and 1, as MySQL servers do.
//!
//! Verify: chapter 05 §16.5.

use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_port::err;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_AUTO_INCREMENT;
use ic_port::time;
use ic_port::IcError;
use ic_signals::table_props;
use ic_signals::table_props::TableProps;

use crate::apid_conn::ApidConnection;
use crate::apid_global::ApidShared;
use crate::apid_global::GlobalOptions;
use crate::dict_cache::TableDef;
use crate::executor::Submitter;
use crate::program::Cmp;
use crate::program::Program;
use crate::program::ProgramBuilder;
use crate::query::QueryId;
use crate::query::ReadKeyArgs;
use crate::query::WriteKeyArgs;
use crate::record::Record;
use crate::transaction::CommitState;

/// The counter table's database and name, stored as
/// `sys/def/SYSTAB_0`.
const IC_COUNTER_DATABASE: &str = "sys";
const IC_COUNTER_TABLE: &str = "SYSTAB_0";
/// Its key, the table id, 32 bits unsigned.
const IC_COUNTER_KEY: u32 = 0;
/// The next value not yet reserved, 64 bits unsigned.
const IC_COUNTER_NEXT: u32 = 1;

/// The name a table without a primary key gives its hidden key.
const IC_HIDDEN_KEY_NAME: &str = "$PK";

/// The first pause before a temporary error is tried again, in
/// microseconds; doubled after each failure.
const IC_COUNTER_FIRST_PAUSE_US: u64 = 100;
/// How long a counter operation is tried for, in microseconds, from
/// the first attempt.
const IC_COUNTER_RETRY_US: u64 = 3_600_000;

// ---- What a table holds ----

/// The values reserved for a table and not yet handed out, as the
/// reference's client keeps them (chapter 05 §16.5).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Range {
  /// The last value handed out, or one below the next an explicit value
  /// left; `u64::MAX` before the first refill.
  given: u64,
  /// The last value reserved. The range holds values while it is above
  /// `given`.
  reserved: u64,
  /// The highest the counter was read back as after an explicit value.
  highest_seen: u64,
}

impl Range {
  /// A range that holds nothing, before the first refill.
  pub(crate) const fn empty() -> Range {
    Range {
      given: u64::MAX,
      reserved: u64::MAX,
      highest_seen: 0,
    }
  }

  fn holds_some(&self) -> bool {
    self.given != self.reserved
  }

  /// The first of `n` values a step apart, taken from what the range
  /// holds, if it holds them all.
  fn take(&mut self, n: u64, step: u64, offset: u64) -> Option<u64> {
    if !self.holds_some() {
      return None;
    }
    let first = next_above(self.given, step, offset)?;
    let last = first.checked_add((n - 1).checked_mul(step)?)?;
    if last > self.reserved {
      return None;
    }
    self.given = last;
    Some(first)
  }

  /// A refill added `amount` to the counter, which became `counter`:
  /// the values below it from `counter − amount` are the range now, and
  /// the first of `n` values a step apart is taken from it.
  fn refilled(
    &mut self,
    counter: u64,
    amount: u64,
    n: u64,
    step: u64,
    offset: u64,
  ) -> Option<u64> {
    let old = counter.checked_sub(amount)?;
    let first = at_or_above(old, step, offset)?;
    let last = first.checked_add((n - 1).checked_mul(step)?)?;
    let reserved = counter.checked_sub(1)?;
    if last > reserved {
      return None;
    }
    self.given = last;
    self.reserved = reserved;
    Some(first)
  }

  /// An insert used a value of its own, `next` − 1. True if the counter
  /// must be moved past it; otherwise what the range holds was enough,
  /// moved past it here if the value fell inside.
  fn seen(&mut self, next: u64) -> bool {
    // Above what this process handed out or read back already.
    if self.given != u64::MAX && self.given > next {
      return false;
    }
    if self.highest_seen > next {
      return false;
    }
    if self.holds_some() {
      if next <= self.given.saturating_add(1) {
        return false;
      }
      if next <= self.reserved {
        self.given = next - 1;
        return false;
      }
    }
    true
  }

  /// The counter was moved up to at least `next` and read back as
  /// `counter`; the range holds nothing until the next refill.
  fn raised(&mut self, next: u64, counter: u64) {
    self.highest_seen = counter;
    self.given = next - 1;
    self.reserved = next - 1;
  }
}

/// The least k × step + offset above `after`.
fn next_above(after: u64, step: u64, offset: u64) -> Option<u64> {
  if after < offset {
    return Some(offset);
  }
  let steps = (after - offset) / step + 1;
  steps.checked_mul(step)?.checked_add(offset)
}

/// The least k × step + offset at or above `value`.
fn at_or_above(value: u64, step: u64, offset: u64) -> Option<u64> {
  if value <= offset {
    return Some(offset);
  }
  next_above(value - 1, step, offset)
}

/// A table's AUTO_INCREMENT column and the values reserved for it, on
/// its shared description.
pub(crate) struct TableCounter {
  /// The column's id.
  column: u32,
  /// True for the hidden key of a table without a primary key.
  hidden: bool,
  /// How many values a refill reserves for this table, or zero for the
  /// process's prefetch.
  prefetch: AtomicU32,
  range: IcMutex<Range>,
}

impl TableCounter {
  /// The counter of a table with an AUTO_INCREMENT column; the
  /// dictionary allows one.
  pub(crate) fn of(info: &TableProps) -> Option<TableCounter> {
    let mut found = None;
    let mut keys: u32 = 0;
    for attr in &info.attributes {
      if attr.primary_key {
        keys += 1;
      }
      if attr.auto_increment && found.is_none() {
        found = Some(attr);
      }
    }
    let attr = found?;
    let hidden =
      attr.name == IC_HIDDEN_KEY_NAME && attr.primary_key && keys == 1;
    Some(TableCounter {
      column: attr.column_id,
      hidden,
      prefetch: AtomicU32::new(0),
      range: IcMutex::new(IC_MUTEX_LEVEL_AUTO_INCREMENT, Range::empty()),
    })
  }
}

impl TableDef {
  /// The id of the table's AUTO_INCREMENT column, a hidden key
  /// included, if it has one.
  pub fn auto_increment_column(&self) -> Option<u32> {
    let counter = self.counter.as_ref()?;
    Some(counter.column)
  }

  /// The id of the hidden key, `$PK`, if this is a table without a
  /// primary key (chapter 04, "AUTO_INCREMENT"): an ordinary last
  /// column, BIGINT UNSIGNED, the primary key and the AUTO_INCREMENT
  /// column, which tools listing columns may leave out.
  pub fn hidden_key(&self) -> Option<u32> {
    let counter = self.counter.as_ref()?;
    if !counter.hidden {
      return None;
    }
    Some(counter.column)
  }

  /// How many AUTO_INCREMENT values a refill reserves for this table
  /// from now on, in every thread that binds it; zero goes back to the
  /// process's, [`GlobalOptions::auto_increment_prefetch`]. A table
  /// without an AUTO_INCREMENT column has nothing to set.
  pub fn set_auto_increment_prefetch(&self, n: u32) {
    if let Some(counter) = &self.counter {
      counter.prefetch.store(n, Ordering::Relaxed);
    }
  }
}

// ---- The process's choices ----

/// The prefetch, step and offset of the process, from its options.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct CounterOptions {
  prefetch: u32,
  step: u64,
  offset: u64,
}

impl CounterOptions {
  /// The options as the counter takes them: a prefetch or step of zero
  /// counts as one, and so does an offset of zero or above the step.
  pub(crate) fn from_options(options: &GlobalOptions) -> CounterOptions {
    let step = options.auto_increment_step.max(1) as u64;
    let mut offset = options.auto_increment_offset as u64;
    if offset == 0 || offset > step {
      offset = 1;
    }
    CounterOptions {
      prefetch: options.auto_increment_prefetch.max(1),
      step,
      offset,
    }
  }

  /// The step and offset of a table's values.
  fn step_and_offset(&self, counter: &TableCounter) -> (u64, u64) {
    if counter.hidden {
      return (1, 1);
    }
    (self.step, self.offset)
  }

  /// How many values a refill reserves for a table.
  fn prefetch_of(&self, counter: &TableCounter) -> u32 {
    match counter.prefetch.load(Ordering::Relaxed) {
      0 => self.prefetch,
      own => own,
    }
  }
}

// ---- The counter's connection ----

/// What a counter operation does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CounterOp {
  /// Add this many to the counter and read it back.
  Add(u64),
  /// Set the counter to this unless it is there already, and read it
  /// back.
  Raise(u64),
  /// Read the counter.
  Peek,
}

/// The counter table, and the queries its operations run as.
struct CounterTable {
  table: Arc<TableDef>,
  key_rec: Record,
  /// Where the key row holds the table id, and where a row read back
  /// holds the counter.
  key_at: usize,
  next_at: usize,
  /// counter = counter + input 0, the counter read after.
  add: QueryId,
  /// counter = input 0 if counter < input 0, the counter read after.
  raise: QueryId,
  /// A read of the counter.
  peek: QueryId,
}

/// The connection the counter operations go on, and the counter table
/// once bound.
pub(crate) struct CounterLink {
  conn: ApidConnection,
  bound: Option<CounterTable>,
  /// Set when an attempt's transaction did not end in time, so that
  /// the connection is let go of rather than used again.
  broken: bool,
}

/// The process's counter connection, and whether the process has
/// stopped.
pub(crate) struct CounterSlot {
  link: Option<CounterLink>,
  closed: bool,
}

impl CounterSlot {
  pub(crate) const fn new() -> CounterSlot {
    CounterSlot {
      link: None,
      closed: false,
    }
  }
}

impl ApidShared {
  /// Let go of the counter's connection, for good: the global is going.
  pub(crate) fn drop_counter_link(&self) {
    let link = {
      let mut slot = self.counter.lock();
      slot.closed = true;
      slot.link.take()
    };
    drop(link);
  }
}

impl CounterLink {
  fn new(shared: &Arc<ApidShared>) -> Result<CounterLink, IcError> {
    Ok(CounterLink {
      conn: ApidConnection::new(Arc::clone(shared))?,
      bound: None,
      broken: false,
    })
  }

  /// The counter table bound, and its queries made, for as long as the
  /// table's description holds.
  fn bind(&mut self) -> Result<(), IcError> {
    let table = self
      .conn
      .table_bind(IC_COUNTER_DATABASE, IC_COUNTER_TABLE)?;
    if let Some(bound) = &self.bound {
      if Arc::ptr_eq(&bound.table, &table) {
        return Ok(());
      }
    }
    if let Some(old) = self.bound.take() {
      let _ = self.conn.free_query(old.add);
      let _ = self.conn.free_query(old.raise);
      let _ = self.conn.free_query(old.peek);
    }
    let is = |id: u32, type_code: u32| match table.field(id) {
      Some(attr) => attr.type_code == type_code,
      None => false,
    };
    if !is(IC_COUNTER_KEY, table_props::IC_TYPE_UNSIGNED)
      || !is(IC_COUNTER_NEXT, table_props::IC_TYPE_BIGUNSIGNED)
    {
      return Err(IcError::new(err::IC_ERROR_BAD_TABLE_DESCRIPTION));
    }
    let key_rec = Record::default_for(&table, Some(&[IC_COUNTER_KEY]))?;
    let next_rec = Record::default_for(&table, Some(&[IC_COUNTER_NEXT]))?;
    let key_at = field_offset(&key_rec)?;
    let next_at = field_offset(&next_rec)?;

    let (add_program, raise_program) = counter_programs(&table)?;
    let add = self.conn.create_query(&table, &key_rec, &key_rec)?;
    let raise = self.conn.create_query(&table, &key_rec, &key_rec)?;
    let peek = self.conn.create_query(&table, &key_rec, &next_rec)?;
    let made = CounterTable {
      table,
      key_rec,
      key_at,
      next_at,
      add,
      raise,
      peek,
    };
    let set = self.set_programs(&made, &add_program, &raise_program, &next_rec);
    if let Err(e) = set {
      let _ = self.conn.free_query(made.add);
      let _ = self.conn.free_query(made.raise);
      let _ = self.conn.free_query(made.peek);
      return Err(e);
    }
    self.bound = Some(made);
    Ok(())
  }

  fn set_programs(
    &mut self,
    made: &CounterTable,
    add_program: &Arc<Program>,
    raise_program: &Arc<Program>,
    next_rec: &Record,
  ) -> Result<(), IcError> {
    let pairs = [(made.add, add_program), (made.raise, raise_program)];
    for (qid, program) in pairs {
      let query = match self.conn.query_mut(qid) {
        Some(query) => query,
        None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
      };
      query.set_program(Some(program))?;
      query.set_reads_after(Some(next_rec))?;
    }
    Ok(())
  }

  /// One counter operation for the table `table_id`, in a transaction
  /// of its own placed by the counter row's key: the counter as it
  /// stands after it.
  fn run(&mut self, table_id: u32, op: CounterOp) -> Result<u64, IcError> {
    self.bind()?;
    let bound = match &self.bound {
      Some(bound) => bound,
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    };
    let (qid, input) = match op {
      CounterOp::Add(amount) => (bound.add, Some(amount)),
      CounterOp::Raise(value) => (bound.raise, Some(value)),
      CounterOp::Peek => (bound.peek, None),
    };
    let query = match self.conn.query_mut(qid) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    };
    let at = bound.key_at;
    match query.key_row_mut().get_mut(at..at + 4) {
      Some(key) => key.copy_from_slice(&table_id.to_le_bytes()),
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    }
    if let Some(value) = input {
      // An input is a signed integer.
      let value = match i64::try_from(value) {
        Ok(value) => value,
        Err(_) => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
      };
      query.set_input(0, value)?;
    }
    let hint = match self.conn.query(qid) {
      Some(query) => {
        self
          .conn
          .hint_for_key(&bound.table, &bound.key_rec, query.key_row())
      }
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    };
    let tid = self.conn.start_transaction(hint, Some(&bound.table))?;
    let defined = match op {
      CounterOp::Peek => self.conn.read_key(qid, tid, &ReadKeyArgs::default()),
      _ => self.conn.write_key(qid, tid, &WriteKeyArgs::default()),
    };
    let asked = match defined {
      Ok(()) => self.conn.commit_transaction(tid),
      Err(e) => {
        let _ = self.conn.rollback_transaction(tid);
        Err(e)
      }
    };
    let next_at = bound.next_at;
    // Waited for a little longer than the reply timeout takes to end a
    // transaction whose coordinator does not answer.
    let state = match self.conn.wait_for_end(tid) {
      Some(state) => state,
      None => {
        // The transaction goes with the connection, given up and made
        // again.
        self.broken = true;
        return Err(IcError::new(err::IC_ERROR_TIMEOUT));
      }
    };
    // The commit's acknowledgement goes now, not with a send that may
    // be long in coming.
    let _ = self.conn.send_queued(true);
    let (trans_error, query_error) =
      match (self.conn.transaction(tid), self.conn.query(qid)) {
        (Some(trans), Some(query)) => (trans.error(), query.error()),
        _ => (None, None),
      };
    let _ = self.conn.close_transaction(tid);
    // Only this connection's own queries are on its executed list.
    while self.conn.get_next_executed_query().is_some() {}
    asked?;
    if let Some(e) = trans_error.or(query_error) {
      return Err(e);
    }
    if state != CommitState::Committed {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK));
    }
    let query = match self.conn.query(qid) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    };
    let row = match op {
      CounterOp::Peek => query.attr_row(),
      _ => query.after_row(),
    };
    match row.get(next_at..next_at + 8) {
      Some(bytes) => {
        let mut le = [0u8; 8];
        le.copy_from_slice(bytes);
        Ok(u64::from_le_bytes(le))
      }
      None => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    }
  }
}

/// The counter's two programs: counter = counter + input 0, as MySQL's
/// update adds its constant (chapter 05 §16.5); and counter = input 0
/// where the counter is below it, as MySQL's moves it up only.
fn counter_programs(
  table: &Arc<TableDef>,
) -> Result<(Arc<Program>, Arc<Program>), IcError> {
  let mut b = ProgramBuilder::new(table);
  let next = b.column(IC_COUNTER_NEXT);
  let amount = b.input(0);
  let sum = b.add(next, amount);
  b.assign(IC_COUNTER_NEXT, sum);
  let add = b.finish(None)?;
  let mut b = ProgramBuilder::new(table);
  let next = b.column(IC_COUNTER_NEXT);
  let value = b.input(0);
  let below = b.compare(Cmp::Lt, next, value);
  b.assign_if(IC_COUNTER_NEXT, value, below);
  let raise = b.finish(None)?;
  Ok((add, raise))
}

/// Where a record of one field holds it in its row.
fn field_offset(rec: &Record) -> Result<usize, IcError> {
  match rec.field(0) {
    Some(field) => Ok(field.offset() as usize),
    None => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  }
}

/// One attempt at a counter operation on the process's connection for
/// them, made first if need be.
fn attempt(
  shared: &Arc<ApidShared>,
  table_id: u32,
  op: CounterOp,
) -> Result<u64, IcError> {
  let mut slot = shared.counter.lock();
  if slot.closed {
    return Err(IcError::new(err::IC_ERROR_APPLICATION_STOPPED));
  }
  if slot.link.is_none() {
    slot.link = Some(CounterLink::new(shared)?);
  }
  let link = match slot.link.as_mut() {
    Some(link) => link,
    None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  };
  let outcome = link.run(table_id, op);
  if link.broken {
    slot.link = None;
  }
  outcome
}

/// A counter operation, a temporary error tried again from a pause of
/// 100 µs, doubled each time, until 3.6 s have passed since the first
/// attempt; a pause that would carry past that ends there, for a last
/// attempt.
fn counter_op(
  shared: &Arc<ApidShared>,
  table_id: u32,
  op: CounterOp,
) -> Result<u64, IcError> {
  let start = time::gethrtime();
  let mut pause_us = IC_COUNTER_FIRST_PAUSE_US;
  loop {
    let e = match attempt(shared, table_id, op) {
      Ok(value) => return Ok(value),
      Err(e) => e,
    };
    if !e.is_temporary() {
      return Err(e);
    }
    let waited = time::micros_elapsed(start, time::gethrtime());
    if waited >= IC_COUNTER_RETRY_US {
      return Err(e);
    }
    let pause = pause_us.min(IC_COUNTER_RETRY_US - waited);
    time::microsleep(pause as u32);
    pause_us = pause_us.saturating_mul(2);
  }
}

fn counter_of(table: &TableDef) -> Result<&TableCounter, IcError> {
  match &table.counter {
    Some(counter) => Ok(counter),
    None => Err(IcError::new(err::IC_ERROR_NO_AUTO_INCREMENT)),
  }
}

// ---- What the connection and the submitter call ----

/// The first of `n` values a step apart, zero counting as one.
pub(crate) fn take_values(
  shared: &Arc<ApidShared>,
  table: &TableDef,
  n: u32,
) -> Result<u64, IcError> {
  let counter = counter_of(table)?;
  let n = n.max(1) as u64;
  let options = &shared.auto_increment;
  let (step, offset) = options.step_and_offset(counter);
  let mut range = counter.range.lock();
  if let Some(first) = range.take(n, step, offset) {
    return Ok(first);
  }
  let size = n.max(options.prefetch_of(counter) as u64);
  let amount = match size.checked_mul(step) {
    Some(amount) => amount,
    None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
  };
  let next = counter_op(shared, table.table_id(), CounterOp::Add(amount))?;
  match range.refilled(next, amount, n, step, offset) {
    Some(first) => Ok(first),
    // A counter that ran past the end of 64 bits.
    None => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  }
}

/// An insert used `value` of its own: the counter moved past it, if
/// what this process holds or has seen is not past it already.
pub(crate) fn report_seen(
  shared: &Arc<ApidShared>,
  table: &TableDef,
  value: u64,
) -> Result<(), IcError> {
  let counter = counter_of(table)?;
  let next = value.saturating_add(1);
  let mut range = counter.range.lock();
  if !range.seen(next) {
    return Ok(());
  }
  let read = counter_op(shared, table.table_id(), CounterOp::Raise(next))?;
  range.raised(next, read);
  Ok(())
}

/// The counter as it stands, the range neither locked nor touched.
pub(crate) fn peek(
  shared: &Arc<ApidShared>,
  table: &TableDef,
) -> Result<u64, IcError> {
  counter_of(table)?;
  counter_op(shared, table.table_id(), CounterOp::Peek)
}

impl ApidConnection {
  /// The next value of the table's AUTO_INCREMENT column
  /// (`ic_apid_conn_auto_increment`), for the application to put in its
  /// row: from the range the process holds for the table, or, when that
  /// is used up, from a refill, a round trip on the counter's own
  /// connection. Unique across every client of the cluster, MySQL
  /// servers included, not in order across them; values not used are
  /// gaps. Not checked against the column's type: a column narrower
  /// than 64 bits is the schema's error (chapter 04, "AUTO_INCREMENT").
  ///
  /// Errors: `IC_ERROR_NO_AUTO_INCREMENT` for a table without such a
  /// column; a temporary error still there after 3.6 s of trying.
  pub fn auto_increment(&self, table: &TableDef) -> Result<u64, IcError> {
    take_values(&self.shared, table, 1)
  }

  /// The first of `n` values a step apart, reserved together, for an
  /// insert of many rows (`ic_apid_conn_auto_increment_block`): from
  /// the range if it holds them all, otherwise from a refill of at
  /// least `n`, what the range held then left as a gap. Zero counts as
  /// one. Errors: as [`auto_increment`](Self::auto_increment).
  pub fn auto_increment_block(
    &self,
    table: &TableDef,
    n: u32,
  ) -> Result<u64, IcError> {
    take_values(&self.shared, table, n)
  }

  /// After an insert with a value of the application's own in the
  /// AUTO_INCREMENT column (`ic_apid_conn_auto_increment_seen`): the
  /// counter is moved past it, only ever up, so that no client hands
  /// it out. A value within the range this process holds moves only
  /// the range; one below what it handed out or saw moves nothing.
  ///
  /// Errors: as [`auto_increment`](Self::auto_increment);
  /// `IC_ERROR_NOT_SUPPORTED` for a value of 2^63 − 1 or more that
  /// would move the counter, as the interpreter's inputs are signed.
  pub fn auto_increment_seen(
    &self,
    table: &TableDef,
    value: u64,
  ) -> Result<(), IcError> {
    report_seen(&self.shared, table, value)
  }

  /// The counter as it stands (`ic_apid_conn_auto_increment_peek`): the
  /// next value no client has reserved, by one read of the counter row,
  /// nothing reserved and the range not touched. Only advisory: not the
  /// next value this process hands out from its range, and out of date
  /// once any client refills. Errors: as
  /// [`auto_increment`](Self::auto_increment).
  pub fn auto_increment_peek(&self, table: &TableDef) -> Result<u64, IcError> {
    peek(&self.shared, table)
  }
}

/// The same calls for a thread with no connection of its own. They are
/// the connection's: the calling thread takes from the range the
/// process holds for the table, shared with every connection and
/// executor, and a refill goes on the counter's own connection, never
/// through the executor. So they may be made from a callback on the
/// executor's own thread, where a refill holds the executor for its
/// round trip. A stopped executor's submitter refuses them, as it
/// refuses everything, with `IC_ERROR_EXECUTOR_STOPPED`.
impl Submitter {
  /// As [`ApidConnection::auto_increment`]
  /// (`ic_apid_submitter_auto_increment`).
  pub fn auto_increment(&self, table: &TableDef) -> Result<u64, IcError> {
    take_values(self.apid()?, table, 1)
  }

  /// As [`ApidConnection::auto_increment_block`]
  /// (`ic_apid_submitter_auto_increment_block`).
  pub fn auto_increment_block(
    &self,
    table: &TableDef,
    n: u32,
  ) -> Result<u64, IcError> {
    take_values(self.apid()?, table, n)
  }

  /// As [`ApidConnection::auto_increment_seen`]
  /// (`ic_apid_submitter_auto_increment_seen`).
  pub fn auto_increment_seen(
    &self,
    table: &TableDef,
    value: u64,
  ) -> Result<(), IcError> {
    report_seen(self.apid()?, table, value)
  }

  /// As [`ApidConnection::auto_increment_peek`]
  /// (`ic_apid_submitter_auto_increment_peek`).
  pub fn auto_increment_peek(&self, table: &TableDef) -> Result<u64, IcError> {
    peek(self.apid()?, table)
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::program::run_model;
  use ic_signals::prop_stream::PropertyWriter;
  use ic_signals::table_props::parse_table_info;
  use ic_signals::table_props::IC_TP_COLUMN_AUTO_INC;
  use ic_signals::table_props::IC_TP_COLUMN_COUNT;
  use ic_signals::table_props::IC_TP_COLUMN_END;
  use ic_signals::table_props::IC_TP_COLUMN_ID;
  use ic_signals::table_props::IC_TP_COLUMN_IN_KEY;
  use ic_signals::table_props::IC_TP_COLUMN_LENGTH;
  use ic_signals::table_props::IC_TP_COLUMN_NAME;
  use ic_signals::table_props::IC_TP_COLUMN_TYPE;
  use ic_signals::table_props::IC_TP_TABLE_NAME;
  use ic_signals::table_props::IC_TYPE_BIGUNSIGNED;
  use ic_signals::table_props::IC_TYPE_INT;
  use ic_signals::table_props::IC_TYPE_UNSIGNED;

  /// A column: its name, type, and whether it is in the key and is the
  /// AUTO_INCREMENT column.
  type Column = (&'static str, u32, bool, bool);

  /// A table as the dictionary describes one, its columns numbered in
  /// order.
  fn table_of(name: &str, columns: &[Column]) -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, name);
    w.add_u32(IC_TP_COLUMN_COUNT, columns.len() as u32);
    let mut id: usize = 0;
    while id < columns.len() {
      let (column, type_code, key, auto) = columns[id];
      w.add_string(IC_TP_COLUMN_NAME, column);
      w.add_u32(IC_TP_COLUMN_ID, id as u32);
      w.add_u32(IC_TP_COLUMN_TYPE, type_code);
      w.add_u32(IC_TP_COLUMN_LENGTH, 1);
      w.add_u32(IC_TP_COLUMN_IN_KEY, key as u32);
      w.add_u32(IC_TP_COLUMN_AUTO_INC, auto as u32);
      w.add_u32(IC_TP_COLUMN_END, 0);
      id += 1;
    }
    let info = parse_table_info(w.words()).expect("parsed");
    Arc::new(TableDef::new(info, None))
  }

  /// The counter table's shape, under names of our own.
  fn counter_table() -> Arc<TableDef> {
    table_of(
      "sys/def/counter",
      &[
        ("table", IC_TYPE_UNSIGNED, true, false),
        ("next", IC_TYPE_BIGUNSIGNED, false, false),
      ],
    )
  }

  #[test]
  fn the_column_and_the_hidden_key_are_found() {
    let t = table_of(
      "db/def/t",
      &[
        ("id", IC_TYPE_BIGUNSIGNED, true, true),
        ("v", IC_TYPE_INT, false, false),
      ],
    );
    assert_eq!(t.auto_increment_column(), Some(0));
    assert_eq!(t.hidden_key(), None);
    let h = table_of(
      "db/def/h",
      &[
        ("v", IC_TYPE_INT, false, false),
        ("$PK", IC_TYPE_BIGUNSIGNED, true, true),
      ],
    );
    assert_eq!(h.auto_increment_column(), Some(1));
    assert_eq!(h.hidden_key(), Some(1));
    let plain = table_of("db/def/p", &[("id", IC_TYPE_INT, true, false)]);
    assert_eq!(plain.auto_increment_column(), None);
    assert_eq!(plain.hidden_key(), None);
    plain.set_auto_increment_prefetch(10);
    assert_eq!(
      counter_of(&plain).map(|_| ()),
      Err(IcError::new(err::IC_ERROR_NO_AUTO_INCREMENT))
    );

    // The hidden key takes 1 and 1 whatever the process's step and
    // offset; a table's own prefetch stands before the process's.
    let options = GlobalOptions {
      auto_increment_step: 10,
      auto_increment_offset: 3,
      ..GlobalOptions::default()
    };
    let taken = CounterOptions::from_options(&options);
    let t_counter = counter_of(&t).expect("t");
    let h_counter = counter_of(&h).expect("h");
    assert_eq!(taken.step_and_offset(t_counter), (10, 3));
    assert_eq!(taken.step_and_offset(h_counter), (1, 1));
    assert_eq!(taken.prefetch_of(t_counter), 4096);
    t.set_auto_increment_prefetch(100);
    assert_eq!(taken.prefetch_of(t_counter), 100);
    t.set_auto_increment_prefetch(0);
    assert_eq!(taken.prefetch_of(t_counter), 4096);
  }

  #[test]
  fn a_refill_adds_an_input_as_mysqls_adds_a_constant() {
    let t = counter_table();
    let (add, raise) = counter_programs(&t).expect("the programs");
    // MySQL's (interp.pcap, frame 26536): read the counter, set 512,
    // add, write the counter, accept. Ours takes the amount from input
    // 0 where MySQL's sets it.
    assert_eq!(
      add.words(),
      [0x0001_0001, 0x0000_8079, 0x0000_0207, 0x0001_0002, 0x12]
    );
    assert_eq!(add.inputs(), 1);
    assert!(add.writes());
    let mut inputs = [0i64; 16];
    inputs[0] = 4096;
    let row = [Some(7), Some(100)];
    let (accepted, writes) =
      run_model(add.words(), &row, &inputs).expect("the add runs");
    assert!(accepted);
    assert_eq!(writes, [(1, Some(4196))]);

    // Moved up only: written where the counter is below the input.
    assert_eq!(raise.inputs(), 1);
    for (input, written) in [(150, Some(150)), (100, None), (50, None)] {
      inputs[0] = input;
      let (accepted, writes) =
        run_model(raise.words(), &row, &inputs).expect("the raise runs");
      assert!(accepted, "input {}", input);
      let mut expected: Vec<(u32, Option<i64>)> = Vec::new();
      if let Some(value) = written {
        expected.push((1, Some(value)));
      }
      assert_eq!(writes, expected, "input {}", input);
    }
  }

  #[test]
  fn values_follow_on_without_step_or_offset() {
    let mut range = Range::empty();
    assert_eq!(range.take(1, 1, 1), None, "nothing before a refill");
    // The counter stood at 1; 4 added, it is 5: values 1 to 4.
    assert_eq!(range.refilled(5, 4, 1, 1, 1), Some(1));
    assert_eq!(range.take(1, 1, 1), Some(2));
    assert_eq!(range.take(1, 1, 1), Some(3));
    assert_eq!(range.take(1, 1, 1), Some(4));
    assert_eq!(range.take(1, 1, 1), None, "used up");
  }

  #[test]
  fn values_keep_to_the_step_and_offset() {
    // Step 10, offset 5, the counter at 1: 5, 15, 25, as the reference
    // says of its own.
    let mut range = Range::empty();
    assert_eq!(range.refilled(31, 30, 1, 10, 5), Some(5));
    assert_eq!(range.take(1, 10, 5), Some(15));
    assert_eq!(range.take(1, 10, 5), Some(25));
    assert_eq!(range.take(1, 10, 5), None, "35 was not reserved");
    // The counter at 37 after another client's refill: 30 more give
    // 37 to 66, of which 45, 55 and 65.
    assert_eq!(range.refilled(67, 30, 1, 10, 5), Some(45));
    assert_eq!(range.take(1, 10, 5), Some(55));
    assert_eq!(range.take(1, 10, 5), Some(65));
    assert_eq!(range.take(1, 10, 5), None);
  }

  #[test]
  fn least_values_at_and_above() {
    assert_eq!(next_above(0, 1, 1), Some(1));
    assert_eq!(next_above(7, 1, 1), Some(8));
    assert_eq!(next_above(4, 10, 5), Some(5));
    assert_eq!(next_above(5, 10, 5), Some(15));
    assert_eq!(next_above(14, 10, 5), Some(15));
    assert_eq!(next_above(15, 10, 5), Some(25));
    assert_eq!(next_above(u64::MAX - 1, 1, 1), Some(u64::MAX));
    assert_eq!(next_above(u64::MAX, 1, 1), None);
    assert_eq!(at_or_above(1, 10, 5), Some(5));
    assert_eq!(at_or_above(5, 10, 5), Some(5));
    assert_eq!(at_or_above(6, 10, 5), Some(15));
    assert_eq!(at_or_above(15, 10, 5), Some(15));
    assert_eq!(at_or_above(16, 10, 5), Some(25));
  }

  #[test]
  fn a_block_is_taken_whole_or_refilled() {
    let mut range = Range::empty();
    // Eight values reserved, 1 to 8; a block of three, then of four.
    assert_eq!(range.refilled(9, 8, 3, 1, 1), Some(1));
    assert_eq!(range.take(4, 1, 1), Some(4));
    // 8 is left; a block of two does not fit and takes a refill.
    assert_eq!(range.take(2, 1, 1), None);
    // At least two reserved, here the prefetch of eight: 9 to 16.
    assert_eq!(range.refilled(17, 8, 2, 1, 1), Some(9));
    assert_eq!(range.take(1, 1, 1), Some(11));
    // With step 3, offset 2: a block of four is 2, 5, 8, 11.
    let mut stepped = Range::empty();
    assert_eq!(stepped.refilled(13, 12, 4, 3, 2), Some(2));
    assert_eq!(stepped.take(1, 3, 2), None, "12 is the last reserved");
  }

  #[test]
  fn an_explicit_value_moves_the_range_or_the_counter() {
    let mut range = Range::empty();
    // Nothing held: any value goes to the counter.
    assert!(range.seen(51));
    range.raised(51, 51);
    // The counter read back as 51: below it nothing moves.
    assert!(!range.seen(40));
    assert_eq!(range.take(1, 1, 1), None, "the range holds nothing");
    // A refill of ten from 51: 51 to 60.
    assert_eq!(range.refilled(61, 10, 1, 1, 1), Some(51));
    // 55 used: the range goes past it, the counter does not move.
    assert!(!range.seen(56));
    assert_eq!(range.take(1, 1, 1), Some(56));
    // A value below what was handed out moves nothing.
    assert!(!range.seen(53));
    assert_eq!(range.take(1, 1, 1), Some(57));
    // One beyond the range goes to the counter, and empties the range.
    assert!(range.seen(101));
    range.raised(101, 120);
    assert_eq!(range.take(1, 1, 1), None);
    // Read back as 120: values below need not move it again.
    assert!(!range.seen(110));
  }

  #[test]
  fn the_options_count_zero_as_one() {
    let mut options = GlobalOptions::default();
    let taken = CounterOptions::from_options(&options);
    assert_eq!(
      taken,
      CounterOptions {
        prefetch: 4096,
        step: 1,
        offset: 1
      }
    );
    options.auto_increment_prefetch = 0;
    options.auto_increment_step = 0;
    options.auto_increment_offset = 0;
    let taken = CounterOptions::from_options(&options);
    assert_eq!((taken.prefetch, taken.step, taken.offset), (1, 1, 1));
    options.auto_increment_step = 10;
    options.auto_increment_offset = 11;
    let taken = CounterOptions::from_options(&options);
    assert_eq!((taken.step, taken.offset), (10, 1), "above the step");
    options.auto_increment_offset = 10;
    let taken = CounterOptions::from_options(&options);
    assert_eq!(taken.offset, 10);
  }
}
