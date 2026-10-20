// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Executors: user threads that run key operations for other threads
//! (doc/rust/architecture.html, sections 3 and 4; chapter 04,
//! "Executors").
//!
//! An application whose requests arrive on threads of its own, such as
//! a server's handlers, cannot give every one of them a connection: a
//! connection is a user thread's, with a block of its own, and there are
//! at most 4 711. And a thread that sends one operation at a time pays
//! the costs of a whole batch, a write, a read and a round trip, for
//! each. An executor is a user thread that takes requests from any
//! number of application threads and runs them together: everything it
//! admits in one pass of its loop goes in one write per data node, so
//! under load batching comes on its own.
//!
//! **Two inputs, one wait.** The executor waits for replies, which the
//! receive threads post, and for requests, which application threads
//! submit. Both arrive in its inbox, under one mutex, and either wakes
//! it by the same rule ([`thread_conn`](crate::thread_conn), "Requests").
//! Its loop:
//!
//! 1. take the posted signals and the submitted requests, in one lock,
//!    sleeping only when there is neither;
//! 2. execute the replies, which complete operations and free records;
//! 3. hand each finished request back to its application thread;
//! 4. admit waiting requests while fewer than the in-flight limit are
//!    out, each a transaction of its own, defined and committed;
//! 5. send what was admitted: one write per data node.
//!
//! **A request** ([`Request`]) is a transaction of one or more key
//! operations ([`KeyOp`]), each over an [`OpShape`]: a table, or a
//! unique index of it, and the records its rows are laid out by. The
//! request owns its rows, as a query does; it moves to the executor,
//! and back, with the rows read in it. Nothing is allocated for it on
//! the way: the executor runs each operation as one of the queries it
//! keeps for that shape, and copies the rows in and out.
//!
//! **Completion** ([`Done`]): the request is left in a [`Slot`], for an
//! application thread waiting on it, or handed to a function, on the
//! executor's thread. [`Submitter::execute`] is the blocking form: it
//! submits and sleeps on its submitter's slot until the request comes
//! back (the synchronous model's second form, section 4).
//!
//! **Back-pressure** is in two places. The inbox holds at most
//! [`ExecutorOptions::queue`] requests: a submit beyond refuses with
//! `IC_ERROR_EXECUTOR_BUSY`, or waits for room. And the executor has at
//! most [`ExecutorOptions::in_flight`] requests out; the rest wait in
//! its own queue, in order.
//!
//! **Scans** ([`Submitter::scan`]) run here too, read on the
//! application thread through a [`ScanReader`]
//! ([`scan_reader`](crate::scan_reader)). The executor moves each
//! complete batch to the reader, and asks its receiver for more once
//! the reader hands the batch back, which reaches the loop as a nudge
//! (step 1). A slow reader stalls only its own scan. Scans have a limit
//! of their own, [`ExecutorOptions::scans`], and a queue of their own,
//! so that neither scans nor transactions hold the other back. A scan
//! of an ordered index takes the reader's ranges from its feed, and an
//! ordered one runs here unordered over every fragment, each batch
//! going with its receiver's place, for the reader to merge.
//!
//! **Nothing is lost silently.** A request whose deadline has passed
//! when its turn comes fails with `IC_ERROR_TIMEOUT`, unsent. One with
//! no data node to go to fails with that error. A node failure fails
//! the transactions it affects, as on any connection, and their
//! requests come back with the error. A stopping executor finishes what
//! it has in flight and fails what waits with
//! `IC_ERROR_EXECUTOR_STOPPED`.

use std::collections::HashMap;
use std::collections::VecDeque;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::sync::OnceLock;
use std::sync::Weak;
use std::thread::JoinHandle;
use std::thread::ThreadId;

use ic_port::err;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_COMPLETION;
use ic_port::IcError;

use crate::apid_conn::ApidConnection;
use crate::apid_global::ApidGlobal;
use crate::apid_global::ApidShared;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::index_scan::RangeSet;
use crate::program;
use crate::program::Program;
use crate::program::IC_PROGRAM_INPUTS;
use crate::query;
use crate::query::AbortOption;
use crate::query::BatchHint;
use crate::query::QueryId;
use crate::query::ReadKeyArgs;
use crate::query::ReadKind;
use crate::query::TransId;
use crate::query::WriteKeyArgs;
use crate::query::WriteKind;
use crate::record::Record;
use crate::scan::ScanArgs;
use crate::scan::ScanId;
use crate::scan::ScanLock;
use crate::scan_reader::FeedBatch;
use crate::scan_reader::ScanReader;
use crate::scan_reader::ScanShape;
use crate::scan_reader::ScanWork;
use crate::thread_conn::ThreadConnection;
use crate::transaction::CommitState;
use crate::transaction::TransactionHint;

/// Requests an executor has out at most, by default: each a transaction
/// with a record at its coordinator. As many as `ic_bench`'s default
/// pipeline keeps out, two batches of 200.
pub const IC_EXECUTOR_IN_FLIGHT: u32 = 400;
/// Requests an executor's inbox holds at most, by default, before a
/// submit refuses or waits.
pub const IC_EXECUTOR_QUEUE: u32 = 4000;
/// Scans an executor runs at once at most, by default, each with a
/// record of its own at its coordinator.
pub const IC_EXECUTOR_SCANS: u32 = 64;
/// How long an executor with nothing to do sleeps at a time, in
/// milliseconds. A request or a reply wakes it at once; this is how
/// often it looks at its stop flag and its pools otherwise.
const IC_EXECUTOR_IDLE_MS: u32 = 100;
/// How often an executor lets go of the queries it keeps for shapes
/// nobody holds any more, in nanoseconds.
const IC_EXECUTOR_SWEEP_NANOS: u64 = 1_000_000_000;

/// Where the next shape's id comes from.
static NEXT_SHAPE_ID: AtomicU64 = AtomicU64::new(1);

/// A new shape's id, of a key operation's shape or a scan's.
pub(crate) fn next_shape_id() -> u64 {
  NEXT_SHAPE_ID.fetch_add(1, Ordering::Relaxed)
}

// ---- Shapes and operations ----

/// The shape of a key operation, made once and shared by every request
/// that uses it: a table, or a unique index of it, and the records its
/// key row and attribute row are laid out by (the executor's form of a
/// query, `ApidQuery`). An executor keeps queries of each shape it has
/// run, and lets them go once nobody holds the shape.
pub struct OpShape {
  id: u64,
  table: Arc<TableDef>,
  index: Option<Arc<IndexDef>>,
  key_rec: Record,
  attr_rec: Record,
  /// The program its operations run, and the columns an update reads
  /// after it (chapter 04, "Conditions and assignments").
  program: Option<Arc<Program>>,
  after_rec: Option<Record>,
}

impl std::fmt::Debug for OpShape {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match &self.index {
      Some(index) => {
        write!(f, "OpShape({} through {})", self.table.name(), index.name())
      }
      None => write!(f, "OpShape({})", self.table.name()),
    }
  }
}

impl OpShape {
  /// Operations by primary key on a table. Both records must be over
  /// this version of the table, and the key record must hold every
  /// primary key column, as for a query.
  pub fn new(
    table: &Arc<TableDef>,
    key_rec: &Record,
    attr_rec: &Record,
  ) -> Result<Arc<OpShape>, IcError> {
    query::check_records(table, key_rec, attr_rec)?;
    Ok(Arc::new(OpShape {
      id: next_shape_id(),
      table: Arc::clone(table),
      index: None,
      key_rec: key_rec.clone(),
      attr_rec: attr_rec.clone(),
      program: None,
      after_rec: None,
    }))
  }

  /// Operations through a unique index, with records over the index's
  /// table, as for a unique query.
  pub fn new_unique(
    index: &Arc<IndexDef>,
    key_rec: &Record,
    attr_rec: &Record,
  ) -> Result<Arc<OpShape>, IcError> {
    query::check_unique_records(index, key_rec, attr_rec)?;
    Ok(Arc::new(OpShape {
      id: next_shape_id(),
      table: Arc::clone(index.table()),
      index: Some(Arc::clone(index)),
      key_rec: key_rec.clone(),
      attr_rec: attr_rec.clone(),
      program: None,
      after_rec: None,
    }))
  }

  /// A shape of the same records whose operations run `program`, which
  /// must be for its table (`IC_ERROR_BAD_PROGRAM`). Which operations
  /// may run it is checked as each is defined: reads, updates and
  /// deletes, and only updates one that assigns. A program goes by
  /// primary key only: a unique shape refuses it
  /// (`IC_ERROR_NOT_SUPPORTED`).
  pub fn with_program(
    &self,
    program: &Arc<Program>,
  ) -> Result<Arc<OpShape>, IcError> {
    if self.index.is_some() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    program::check_program(program, &self.table, true)?;
    let mut shape = self.copy();
    shape.program = Some(Arc::clone(program));
    Ok(Arc::new(shape))
  }

  /// A shape of the same records whose updates read the columns of
  /// `after_rec` after their program, into each operation's
  /// [`KeyOp::after_row`]. The record must be over the shape's table
  /// (`IC_ERROR_KEY_RECORD`); a unique shape refuses it
  /// (`IC_ERROR_NOT_SUPPORTED`).
  pub fn reading_after(
    &self,
    after_rec: &Record,
  ) -> Result<Arc<OpShape>, IcError> {
    if self.index.is_some() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    if !query::over_table(after_rec, &self.table) {
      return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
    }
    let mut shape = self.copy();
    shape.after_rec = Some(after_rec.clone());
    Ok(Arc::new(shape))
  }

  /// The program its operations run.
  pub fn program(&self) -> Option<&Arc<Program>> {
    self.program.as_ref()
  }

  /// The record of the columns its updates read after their program.
  pub fn after_record(&self) -> Option<&Record> {
    self.after_rec.as_ref()
  }

  /// The same, as a shape of its own.
  fn copy(&self) -> OpShape {
    OpShape {
      id: next_shape_id(),
      table: Arc::clone(&self.table),
      index: self.index.clone(),
      key_rec: self.key_rec.clone(),
      attr_rec: self.attr_rec.clone(),
      program: self.program.clone(),
      after_rec: self.after_rec.clone(),
    }
  }

  /// The table the operations are over.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// The unique index the key goes through, for a unique shape.
  pub fn index(&self) -> Option<&Arc<IndexDef>> {
    self.index.as_ref()
  }

  /// The record the key row is laid out by.
  pub fn key_record(&self) -> &Record {
    &self.key_rec
  }

  /// The record the attribute row is laid out by.
  pub fn attr_record(&self) -> &Record {
    &self.attr_rec
  }
}

/// What a key operation does: a read with its lock, or a write. A
/// tagged union, as chapter 07 allows for a few documented types.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OpKind {
  /// A read, locking as the kind says.
  Read(ReadKind),
  /// A write of this kind.
  Write(WriteKind),
}

/// One key operation of a request. It owns its rows: the key row,
/// filled before the request is submitted, and the attribute row, which
/// holds a write's values or gets a read's row. After the request comes
/// back it says how the operation went.
pub struct KeyOp {
  shape: Arc<OpShape>,
  kind: OpKind,
  abort_option: AbortOption,
  key_row: Vec<u8>,
  attr_row: Vec<u8>,
  /// Its program's inputs, and what an update read after the program.
  inputs: [i64; IC_PROGRAM_INPUTS as usize],
  after_row: Vec<u8>,
  error: Option<IcError>,
  error_data: u32,
  result_len: u32,
  /// The query it runs as, while it is out.
  query: Option<QueryId>,
}

impl std::fmt::Debug for KeyOp {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self.error {
      Some(e) => write!(f, "KeyOp({:?}, error {})", self.kind, e.code),
      None => write!(f, "KeyOp({:?})", self.kind),
    }
  }
}

impl KeyOp {
  /// An operation of this shape and kind, with both rows zeroed at the
  /// sizes the shape's records give.
  pub fn new(shape: &Arc<OpShape>, kind: OpKind) -> KeyOp {
    KeyOp {
      shape: Arc::clone(shape),
      kind,
      abort_option: AbortOption::Default,
      key_row: vec![0; shape.key_rec.row_size() as usize],
      attr_row: vec![0; shape.attr_rec.row_size() as usize],
      inputs: [0; IC_PROGRAM_INPUTS as usize],
      after_row: match &shape.after_rec {
        Some(rec) => vec![0; rec.row_size() as usize],
        None => Vec::new(),
      },
      error: None,
      error_data: 0,
      result_len: 0,
      query: None,
    }
  }

  /// A read of this shape.
  pub fn read(shape: &Arc<OpShape>, kind: ReadKind) -> KeyOp {
    KeyOp::new(shape, OpKind::Read(kind))
  }

  /// A write of this shape.
  pub fn write(shape: &Arc<OpShape>, kind: WriteKind) -> KeyOp {
    KeyOp::new(shape, OpKind::Write(kind))
  }

  /// The shape it is of.
  pub fn shape(&self) -> &Arc<OpShape> {
    &self.shape
  }

  /// What it does.
  pub fn kind(&self) -> OpKind {
    self.kind
  }

  /// Make it do something else, on the same rows.
  pub fn set_kind(&mut self, kind: OpKind) {
    self.kind = kind;
  }

  /// What its failure does to the transaction; by default a write's
  /// rolls it back and a read's fails the read alone.
  pub fn set_abort_option(&mut self, abort_option: AbortOption) {
    self.abort_option = abort_option;
  }

  /// The key row, laid out by the shape's key record.
  pub fn key_row(&self) -> &[u8] {
    &self.key_row
  }

  /// The key row, to fill before the request is submitted.
  pub fn key_row_mut(&mut self) -> &mut [u8] {
    &mut self.key_row
  }

  /// The attribute row: a write's values, or the row a read brought.
  pub fn attr_row(&self) -> &[u8] {
    &self.attr_row
  }

  /// The attribute row, to fill with a write's values.
  pub fn attr_row_mut(&mut self) -> &mut [u8] {
    &mut self.attr_row
  }

  /// Set input `input`, 0 to 15, of the shape's program to `value`;
  /// each holds what it was last set to, 0 at first
  /// (`IC_ERROR_BAD_PROGRAM` beyond 15).
  pub fn set_input(&mut self, input: u32, value: i64) -> Result<(), IcError> {
    match self.inputs.get_mut(input as usize) {
      Some(slot) => {
        *slot = value;
        Ok(())
      }
      None => Err(IcError::new(err::IC_ERROR_BAD_PROGRAM)),
    }
  }

  /// The inputs its program is given.
  pub fn inputs(&self) -> &[i64; IC_PROGRAM_INPUTS as usize] {
    &self.inputs
  }

  /// What an update read after its program, laid out by the shape's
  /// [`after_record`](OpShape::after_record); empty without one.
  pub fn after_row(&self) -> &[u8] {
    &self.after_row
  }

  /// Why it failed, if it did: 626 for a row that is not there, 630 for
  /// one that is, 6000 for a row its program refused where that aborts,
  /// or the transaction's error when that failed it.
  pub fn error(&self) -> Option<IcError> {
    self.error
  }

  /// What the data node said about the failure beyond its code, or
  /// zero; see `ApidQuery::error_data`.
  pub fn error_data(&self) -> u32 {
    self.error_data
  }

  /// Bytes of row data a read brought; zero for a write, or a row that
  /// was not there.
  pub fn result_len(&self) -> u32 {
    self.result_len
  }

  fn is_read(&self) -> bool {
    matches!(self.kind, OpKind::Read(_))
  }

  fn reset_outcome(&mut self) {
    self.error = None;
    self.error_data = 0;
    self.result_len = 0;
    self.query = None;
  }
}

// ---- Requests and their completion ----

/// What is called with a finished request: on the executor's thread,
/// with the request and the `user_ref` it was submitted with. It must
/// not block, as nothing else of the executor's runs meanwhile, and
/// must not panic: a panic stops the executor. The plain function and
/// the `usize` are the form every callback of the library takes
/// (`QueryCallback`).
pub type RequestCallback = fn(Request, usize);

/// How a finished request goes back to its application thread. A
/// tagged union, as chapter 07 allows for a few documented types.
pub enum Done {
  /// Left in the slot, for the thread that waits on it.
  Slot(Arc<Slot>),
  /// Handed to the function, with the `usize`, on the executor's thread.
  Callback(RequestCallback, usize),
}

impl std::fmt::Debug for Done {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      Done::Slot(_) => write!(f, "Done::Slot"),
      Done::Callback(_, user_ref) => write!(f, "Done::Callback({})", user_ref),
    }
  }
}

/// A transaction of key operations, submitted to an executor and
/// handed back once it has ended (section 3's "run this transaction").
///
/// The operations are defined together, the last carrying the commit,
/// and sent together: the transaction commits, or rolls back as a
/// failed write or the coordinator decides. A request is used again
/// and again: submitting resets its outcome, and its rows keep their
/// allocations.
#[derive(Default)]
pub struct Request {
  ops: Vec<KeyOp>,
  hint: Option<TransactionHint>,
  deadline: u64,
  state: CommitState,
  error: Option<IcError>,
  error_data: u32,
  gci: u64,
  done: Option<Done>,
}

impl std::fmt::Debug for Request {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self.error {
      Some(e) => write!(
        f,
        "Request({} op(s), {:?}, error {})",
        self.ops.len(),
        self.state,
        e.code
      ),
      None => {
        write!(f, "Request({} op(s), {:?})", self.ops.len(), self.state)
      }
    }
  }
}

impl Request {
  /// A request with no operations yet.
  pub fn new() -> Request {
    Request::default()
  }

  /// A request of one operation.
  pub fn with_op(op: KeyOp) -> Request {
    let mut request = Request::default();
    request.ops.push(op);
    request
  }

  /// Add an operation, after those already there.
  pub fn push(&mut self, op: KeyOp) {
    self.ops.push(op);
  }

  /// Drop every operation.
  pub fn clear(&mut self) {
    self.ops.clear();
  }

  /// The operations, in order.
  pub fn ops(&self) -> &[KeyOp] {
    &self.ops
  }

  /// The operations, to fill their rows.
  pub fn ops_mut(&mut self) -> &mut [KeyOp] {
    &mut self.ops
  }

  /// Where the transaction is to start. `None`, the default, places it
  /// by the first operation's key: at a node holding the row, or the
  /// index row of a unique operation. A partition hint is of the first
  /// operation's table, or of its index's own table.
  pub fn set_hint(&mut self, hint: Option<TransactionHint>) {
    self.hint = hint;
  }

  /// Give up on the request if it has not been admitted within `ms`
  /// milliseconds from now: it then comes back with `IC_ERROR_TIMEOUT`,
  /// unsent. Zero takes the deadline away. Once admitted it runs to its
  /// end, whose wait the cluster's reply timeout bounds.
  pub fn set_timeout_ms(&mut self, ms: u32) {
    if ms == 0 {
      self.deadline = 0;
      return;
    }
    self.deadline = ic_port::time::gethrtime() + ms as u64 * 1_000_000;
  }

  /// Where the transaction ended: committed, or rolled back.
  pub fn commit_state(&self) -> CommitState {
    self.state
  }

  /// Why it did not commit, or why its reads were lost when a takeover
  /// committed it; see `Transaction::error`.
  pub fn error(&self) -> Option<IcError> {
    self.error
  }

  /// What the coordinator said about the error beyond its code, or zero.
  pub fn error_data(&self) -> u32 {
    self.error_data
  }

  /// The global checkpoint the commit went into.
  pub fn gci(&self) -> u64 {
    self.gci
  }

  /// `Ok` if the transaction committed with nothing lost; otherwise its
  /// error. An operation that failed alone, such as a read of a row
  /// that is not there, leaves the transaction committed and says so in
  /// its own error.
  pub fn outcome(&self) -> Result<(), IcError> {
    if let Some(e) = self.error {
      return Err(e);
    }
    if self.state == CommitState::Committed {
      return Ok(());
    }
    Err(IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK))
  }

  fn reset_outcome(&mut self) {
    self.state = CommitState::Started;
    self.error = None;
    self.error_data = 0;
    self.gci = 0;
    for op in &mut self.ops {
      op.reset_outcome();
    }
  }

  /// Not run, for this reason: rolled back, as far as its operations go.
  fn fail(&mut self, error: IcError) {
    self.state = CommitState::RolledBack;
    self.error = Some(error);
    for op in &mut self.ops {
      op.error = Some(error);
    }
  }
}

impl Drop for Request {
  fn drop(&mut self) {
    // A request let go of before it was handed back, which happens only
    // when the executor's thread has panicked: the loop hands back
    // everything it holds before it ends. The thread waiting on its slot
    // is told, rather than left waiting.
    let left = match self.done.take() {
      Some(left) => left,
      None => return,
    };
    if let Done::Slot(slot) = left {
      slot.put(Err(IcError::new(err::IC_ERROR_INTERNAL_PANIC)));
    }
  }
}

/// Where an executor leaves a finished request for the thread waiting
/// on it. A slot is used again and again; each [`Submitter`] has one for
/// [`execute`](Submitter::execute).
pub struct Slot {
  state: IcMutex<Option<Result<Request, IcError>>>,
  cond: IcCond,
}

impl std::fmt::Debug for Slot {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "Slot")
  }
}

impl Slot {
  /// An empty slot.
  pub fn new() -> Arc<Slot> {
    Arc::new(Slot {
      state: IcMutex::new(IC_MUTEX_LEVEL_COMPLETION, None),
      cond: IcCond::new(),
    })
  }

  /// Leave a result, and wake the thread waiting for it.
  fn put(&self, result: Result<Request, IcError>) {
    let mut state = self.state.lock();
    *state = Some(result);
    // Unlock, and only then wake, as for an inbox.
    drop(state);
    self.cond.signal();
  }

  /// Wait for the request to come back, and take it. An error,
  /// `IC_ERROR_INTERNAL_PANIC`, says it will not: the executor's thread
  /// failed, and the request is gone.
  pub fn wait(&self) -> Result<Request, IcError> {
    let mut state = self.state.lock();
    loop {
      if let Some(result) = state.take() {
        return result;
      }
      state = self.cond.wait(state);
    }
  }

  /// The request, if it has come back.
  pub fn try_take(&self) -> Option<Result<Request, IcError>> {
    self.state.lock().take()
  }
}

// ---- What goes through the inbox ----

/// What an application thread hands an executor: a message type, as
/// chapter 07 allows.
pub(crate) enum Work {
  /// A transaction of key operations.
  Transaction(Request),
  /// Bind a table or an index on the executor's connection, which an
  /// application thread holding only a submitter has no other way to.
  Bind(BindRequest),
  /// A scan, read through a reader.
  Scan(ScanWork),
}

impl std::fmt::Debug for Work {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      Work::Transaction(request) => {
        write!(f, "Work::Transaction({:?})", request)
      }
      Work::Bind(bind) => {
        write!(f, "Work::Bind({}.{})", bind.database, bind.table)
      }
      Work::Scan(scan) => write!(f, "Work::Scan({:?})", scan),
    }
  }
}

/// A table, or an index of it, to bind.
pub(crate) struct BindRequest {
  database: String,
  table: String,
  index: Option<String>,
  /// Where the answer goes; taken when it is given.
  reply: Option<Arc<BindReply>>,
}

impl BindRequest {
  /// Answer with an error, unbound.
  fn refuse(&mut self, error: IcError) {
    if let Some(reply) = self.reply.take() {
      reply.put(None, None, Some(error));
    }
  }
}

impl Drop for BindRequest {
  fn drop(&mut self) {
    // Let go of unanswered: the thread waiting is told.
    self.refuse(IcError::new(err::IC_ERROR_EXECUTOR_STOPPED));
  }
}

/// What a bind found, for the thread waiting on it.
#[derive(Default)]
struct BindAnswer {
  done: bool,
  table: Option<Arc<TableDef>>,
  index: Option<Arc<IndexDef>>,
  error: Option<IcError>,
}

/// Where a bind's answer is left.
pub(crate) struct BindReply {
  state: IcMutex<BindAnswer>,
  cond: IcCond,
}

impl BindReply {
  fn new() -> Arc<BindReply> {
    Arc::new(BindReply {
      state: IcMutex::new(IC_MUTEX_LEVEL_COMPLETION, BindAnswer::default()),
      cond: IcCond::new(),
    })
  }

  fn put(
    &self,
    table: Option<Arc<TableDef>>,
    index: Option<Arc<IndexDef>>,
    error: Option<IcError>,
  ) {
    let mut state = self.state.lock();
    state.done = true;
    state.table = table;
    state.index = index;
    state.error = error;
    drop(state);
    self.cond.signal();
  }

  /// Wait for the answer.
  fn wait(&self) -> BindAnswer {
    let mut state = self.state.lock();
    while !state.done {
      state = self.cond.wait(state);
    }
    std::mem::take(&mut *state)
  }
}

// ---- Executors ----

/// The choices an executor is started with.
#[derive(Clone, Copy, Debug)]
pub struct ExecutorOptions {
  /// Requests out at most, each a transaction holding a record at its
  /// coordinator. The data nodes allow only so many transactions at
  /// once, across every API node, as their configuration says.
  pub in_flight: u32,
  /// Requests the inbox holds at most before a submit refuses, or
  /// waits for room.
  pub queue: u32,
  /// Scans run at once at most; more wait their turn. A scan holds a
  /// record at its coordinator for as long as it runs, however slowly
  /// its reader reads.
  pub scans: u32,
}

impl Default for ExecutorOptions {
  fn default() -> ExecutorOptions {
    ExecutorOptions {
      in_flight: IC_EXECUTOR_IN_FLIGHT,
      queue: IC_EXECUTOR_QUEUE,
      scans: IC_EXECUTOR_SCANS,
    }
  }
}

/// What an executor has done, counted since it started.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ExecutorStats {
  /// Requests taken into the inbox.
  pub submitted: u64,
  /// Requests refused, for want of room or because it was stopping.
  pub refused: u64,
  /// Requests admitted: their transactions defined and sent.
  pub admitted: u64,
  /// Requests handed back, whatever their outcome.
  pub completed: u64,
  /// Requests failed unsent because their deadline had passed.
  pub expired: u64,
  /// Passes of the loop that sent what they admitted: the batches.
  /// Admitted over sends is how many requests a batch carried.
  pub sends: u64,
  /// Passes of the loop.
  pub passes: u64,
  /// Scans started, each for a reader.
  pub scans: u64,
}

/// What the executor's thread, its handle and its submitters share.
struct ExecutorShared {
  stopping: AtomicBool,
  /// The global's shared part, for the submitters' AUTO_INCREMENT
  /// calls, which take values beside the executor, not through it.
  apid: Arc<ApidShared>,
  /// The executor's thread, on which [`Submitter::execute`] would wait
  /// for itself.
  thread: OnceLock<ThreadId>,
  /// The user thread it is, among the connections.
  user_thread: u32,
  submitted: AtomicU64,
  refused: AtomicU64,
  admitted: AtomicU64,
  completed: AtomicU64,
  expired: AtomicU64,
  sends: AtomicU64,
  passes: AtomicU64,
  scans: AtomicU64,
}

impl ExecutorShared {
  fn on_executor_thread(&self) -> bool {
    match self.thread.get() {
      Some(id) => *id == std::thread::current().id(),
      None => false,
    }
  }

  fn stats(&self) -> ExecutorStats {
    ExecutorStats {
      submitted: self.submitted.load(Ordering::Relaxed),
      refused: self.refused.load(Ordering::Relaxed),
      admitted: self.admitted.load(Ordering::Relaxed),
      completed: self.completed.load(Ordering::Relaxed),
      expired: self.expired.load(Ordering::Relaxed),
      sends: self.sends.load(Ordering::Relaxed),
      passes: self.passes.load(Ordering::Relaxed),
      scans: self.scans.load(Ordering::Relaxed),
    }
  }
}

/// An executor: a user thread of its own that runs requests for any
/// number of application threads, which reach it through
/// [`Submitter`]s. Dropping it stops it.
pub struct Executor {
  shared: Arc<ExecutorShared>,
  inbox: Arc<ThreadConnection>,
  thread: Option<JoinHandle<()>>,
}

impl std::fmt::Debug for Executor {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "Executor(user thread {}{})",
      self.shared.user_thread,
      if self.thread.is_none() {
        ", stopped"
      } else {
        ""
      }
    )
  }
}

impl Executor {
  /// Start an executor on a connection of its own, which takes a user
  /// thread's block like any other.
  pub fn start(
    global: &ApidGlobal,
    options: &ExecutorOptions,
  ) -> Result<Executor, IcError> {
    let conn = global.create_connection()?;
    let inbox = Arc::clone(conn.inbox());
    inbox.open_requests(options.queue.max(1) as usize);
    let shared = Arc::new(ExecutorShared {
      stopping: AtomicBool::new(false),
      apid: Arc::clone(&conn.shared),
      thread: OnceLock::new(),
      user_thread: conn.thread_id(),
      submitted: AtomicU64::new(0),
      refused: AtomicU64::new(0),
      admitted: AtomicU64::new(0),
      completed: AtomicU64::new(0),
      expired: AtomicU64::new(0),
      sends: AtomicU64::new(0),
      passes: AtomicU64::new(0),
      scans: AtomicU64::new(0),
    });
    let mut runner = Runner::new(
      conn,
      Arc::clone(&shared),
      options.in_flight.max(1) as usize,
      options.scans.max(1) as usize,
    );
    let name = format!("ic_executor_{}", shared.user_thread);
    let spawned = std::thread::Builder::new()
      .name(name)
      .spawn(move || runner.run());
    let handle = match spawned {
      Ok(handle) => handle,
      Err(e) => return Err(IcError::from_io(&e)),
    };
    // Set before any submitter exists, so before any request runs.
    let _ = shared.thread.set(handle.thread().id());
    Ok(Executor {
      shared,
      inbox,
      thread: Some(handle),
    })
  }

  /// A handle for an application thread to submit requests with. Make
  /// one per thread: each has its own slot for
  /// [`execute`](Submitter::execute).
  pub fn submitter(&self) -> Submitter {
    Submitter {
      inbox: Arc::clone(&self.inbox),
      shared: Arc::clone(&self.shared),
      slot: Slot::new(),
    }
  }

  /// The user thread the executor is, among the connections.
  pub fn user_thread(&self) -> u32 {
    self.shared.user_thread
  }

  /// What it has done so far.
  pub fn stats(&self) -> ExecutorStats {
    self.shared.stats()
  }

  /// Stop it: take no more requests, fail those not yet admitted with
  /// `IC_ERROR_EXECUTOR_STOPPED`, finish those out, and wait for its
  /// thread. A submitter used after this is refused.
  pub fn stop(&mut self) {
    self.shared.stopping.store(true, Ordering::Release);
    self.inbox.close_requests();
    if let Some(handle) = self.thread.take() {
      let _ = handle.join();
    }
  }
}

impl Drop for Executor {
  fn drop(&mut self) {
    self.stop();
  }
}

/// A request turned away by [`Submitter::submit`], handed back with why.
#[derive(Debug)]
pub struct Refused {
  /// The request, as it was given.
  pub request: Request,
  /// `IC_ERROR_EXECUTOR_BUSY` for a full inbox, or
  /// `IC_ERROR_EXECUTOR_STOPPED`.
  pub error: IcError,
}

/// An application thread's handle on an executor. Cloning one makes
/// another, with a slot of its own; give each thread its own.
pub struct Submitter {
  inbox: Arc<ThreadConnection>,
  shared: Arc<ExecutorShared>,
  /// Where [`execute`](Self::execute) waits.
  slot: Arc<Slot>,
}

impl Clone for Submitter {
  fn clone(&self) -> Submitter {
    Submitter {
      inbox: Arc::clone(&self.inbox),
      shared: Arc::clone(&self.shared),
      slot: Slot::new(),
    }
  }
}

impl std::fmt::Debug for Submitter {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "Submitter(executor {})", self.shared.user_thread)
  }
}

impl Submitter {
  /// Submit a request, to be handed back as `done` says once it has
  /// ended. A full inbox refuses it with `IC_ERROR_EXECUTOR_BUSY`, and a
  /// stopping executor with `IC_ERROR_EXECUTOR_STOPPED`; a refused
  /// request is handed back, and `done` is never used.
  pub fn submit(&self, request: Request, done: Done) -> Result<(), Refused> {
    self.submit_with(request, done, false)
  }

  /// As [`submit`](Self::submit), waiting for room in a full inbox
  /// rather than refusing. Refused only by a stopping executor.
  pub fn submit_wait(
    &self,
    request: Request,
    done: Done,
  ) -> Result<(), Refused> {
    self.submit_with(request, done, true)
  }

  fn submit_with(
    &self,
    mut request: Request,
    done: Done,
    wait: bool,
  ) -> Result<(), Refused> {
    request.reset_outcome();
    request.done = Some(done);
    let refusal = match self.inbox.submit(Work::Transaction(request), wait) {
      Ok(()) => {
        self.shared.submitted.fetch_add(1, Ordering::Relaxed);
        return Ok(());
      }
      Err(refusal) => refusal,
    };
    self.shared.refused.fetch_add(1, Ordering::Relaxed);
    let (work, error) = refusal;
    let mut back = match work {
      Work::Transaction(back) => back,
      // Only a transaction was submitted.
      _ => Request::new(),
    };
    // Never used: not a slot to tell, nor a function to call.
    back.done = None;
    Err(Refused {
      request: back,
      error,
    })
  }

  /// Run a request and wait for it to end: the blocking form (section
  /// 4's form B). The request goes to the executor and comes back into
  /// `request`, with the rows read and each operation's outcome; `Ok`
  /// if the transaction committed, its error if not, as
  /// [`Request::outcome`] says. Waits for room in a full inbox. A
  /// stopped executor refuses with `IC_ERROR_EXECUTOR_STOPPED` and hands
  /// the request back as it was; `IC_ERROR_INTERNAL_PANIC` says the
  /// executor's thread failed, and the request is gone. Not to be called
  /// on the executor's own thread, from a callback, which would wait for
  /// itself: that is refused with `IC_ERROR_NOT_SUPPORTED`.
  pub fn execute(&mut self, request: &mut Request) -> Result<(), IcError> {
    if self.shared.on_executor_thread() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let taken = std::mem::take(request);
    let done = Done::Slot(Arc::clone(&self.slot));
    if let Err(refused) = self.submit_with(taken, done, true) {
      *request = refused.request;
      return Err(refused.error);
    }
    let back = self.slot.wait()?;
    *request = back;
    request.outcome()
  }

  /// Run a scan on the executor, read through `reader` (chapter 04,
  /// "Consuming scans"). The request goes to the executor, waiting for
  /// room in a full inbox, and the rows come with the reader's
  /// [`next_row`](ScanReader::next_row). `args` are as for a
  /// connection's scan; `trans` and `callback` are refused with
  /// `IC_ERROR_NOT_SUPPORTED`, as an executor's scan is in no
  /// transaction of the application's and its batches go to the
  /// reader, and so is `read_ahead` for a locking scan. `ordered` and
  /// `descending` are for a shape over an ordered index, as on a
  /// connection, the reader merging an ordered run. `timeout_ms`
  /// counts from now. A reader whose run is not over is refused with
  /// `IC_ERROR_SCAN_ACTIVE`, and a stopped executor with
  /// `IC_ERROR_EXECUTOR_STOPPED`. Not to be called on the executor's own
  /// thread, which would wait for itself (`IC_ERROR_NOT_SUPPORTED`).
  pub fn scan(
    &self,
    reader: &mut ScanReader,
    args: &ScanArgs,
  ) -> Result<(), IcError> {
    if self.shared.on_executor_thread() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let locking = args.lock != ScanLock::Committed;
    if args.trans.is_some()
      || args.callback.is_some()
      || (args.read_ahead && locking)
    {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let work = reader.begin_run(&self.inbox, args)?;
    match self.inbox.submit(Work::Scan(work), true) {
      Ok(()) => Ok(()),
      Err((work, error)) => {
        if let Work::Scan(work) = work {
          reader.abandon_run(work, error);
        }
        Err(error)
      }
    }
  }

  /// A table's description, bound on the executor's connection, for
  /// making records and shapes (`ApidConnection::table_bind`). The
  /// executor stops for the dictionary's answer, so bind once, not per
  /// request.
  pub fn table_bind(
    &self,
    database: &str,
    table: &str,
  ) -> Result<Arc<TableDef>, IcError> {
    let answer = self.bind(database, table, None)?;
    match answer.table {
      Some(def) => Ok(def),
      None => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    }
  }

  /// An index's description, bound on the executor's connection
  /// (`ApidConnection::index_bind`).
  pub fn index_bind(
    &self,
    database: &str,
    index: &str,
    table: &str,
  ) -> Result<Arc<IndexDef>, IcError> {
    let answer = self.bind(database, table, Some(index))?;
    match answer.index {
      Some(def) => Ok(def),
      None => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    }
  }

  /// The global's shared part, for the AUTO_INCREMENT calls; refused
  /// with `IC_ERROR_EXECUTOR_STOPPED` once the executor stops, as every
  /// call on a submitter is then, though these never reach it.
  pub(crate) fn apid(&self) -> Result<&Arc<ApidShared>, IcError> {
    if self.shared.stopping.load(Ordering::Acquire) {
      return Err(IcError::new(err::IC_ERROR_EXECUTOR_STOPPED));
    }
    Ok(&self.shared.apid)
  }

  fn bind(
    &self,
    database: &str,
    table: &str,
    index: Option<&str>,
  ) -> Result<BindAnswer, IcError> {
    if self.shared.on_executor_thread() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let reply = BindReply::new();
    let bind = BindRequest {
      database: database.to_string(),
      table: table.to_string(),
      index: index.map(str::to_string),
      reply: Some(Arc::clone(&reply)),
    };
    if let Err(refusal) = self.inbox.submit(Work::Bind(bind), true) {
      return Err(refusal.1);
    }
    let answer = reply.wait();
    if let Some(e) = answer.error {
      return Err(e);
    }
    Ok(answer)
  }
}

// ---- The executor's thread ----

/// A request out: its transaction, and how many of its operations have
/// still to complete.
struct Flight {
  request: Request,
  trans: TransId,
  pending: usize,
}

/// The idle queries kept for one shape, while anyone holds it.
struct Pool {
  id: u64,
  shape: Weak<OpShape>,
  idle: Vec<QueryId>,
}

/// A scan running for a reader: its run, and the connection's scan it
/// runs on, whose `user_ref` is the flight's number.
struct ScanFlight {
  work: ScanWork,
  scan: ScanId,
  /// The connection's scan has been told to close.
  closed: bool,
  /// What the reader is told in place of the scan's own outcome, as
  /// when the executor stops.
  error: Option<IcError>,
}

/// The idle scans kept for one scan shape, while anyone holds it.
struct ScanPool {
  id: u64,
  shape: Weak<ScanShape>,
  idle: Vec<ScanId>,
}

/// The executor's thread's own state, which nothing else touches.
struct Runner {
  conn: ApidConnection,
  shared: Arc<ExecutorShared>,
  in_flight_limit: usize,
  /// Work taken from the inbox and not yet admitted, oldest first.
  waiting: VecDeque<Work>,
  /// What the inbox hands over, kept for its allocation.
  taken: Vec<Work>,
  /// The requests out, by the number their queries carry in `user_ref`.
  flights: Vec<Option<Flight>>,
  free_flights: Vec<usize>,
  in_flight: usize,
  /// Flights whose operations are all complete, until their
  /// transactions end.
  ending: Vec<usize>,
  pools: Vec<Pool>,
  /// By shape id, the shape's place in `pools`.
  pool_of: HashMap<u64, usize>,
  next_sweep: u64,
  /// Scans waiting for room, oldest first.
  scan_queue: VecDeque<ScanWork>,
  scan_limit: usize,
  /// The scans running, by the number their nudges carry.
  scan_flights: Vec<Option<ScanFlight>>,
  free_scan_flights: Vec<usize>,
  /// The numbers of the scans running.
  running_scans: Vec<usize>,
  scan_pools: Vec<ScanPool>,
  /// By scan shape id, the shape's place in `scan_pools`.
  scan_pool_of: HashMap<u64, usize>,
  /// What the inbox hands over of the readers' nudges.
  nudges: Vec<u32>,
  /// Batches handed back, while they are dealt with.
  scan_back: Vec<FeedBatch>,
  /// A reader's ranges, while its run is started.
  scan_ranges: RangeSet,
}

impl Runner {
  fn new(
    conn: ApidConnection,
    shared: Arc<ExecutorShared>,
    in_flight_limit: usize,
    scan_limit: usize,
  ) -> Runner {
    Runner {
      conn,
      shared,
      in_flight_limit,
      waiting: VecDeque::new(),
      taken: Vec::new(),
      flights: Vec::new(),
      free_flights: Vec::new(),
      in_flight: 0,
      ending: Vec::new(),
      pools: Vec::new(),
      pool_of: HashMap::new(),
      next_sweep: 0,
      scan_queue: VecDeque::new(),
      scan_limit,
      scan_flights: Vec::new(),
      free_scan_flights: Vec::new(),
      running_scans: Vec::new(),
      scan_pools: Vec::new(),
      scan_pool_of: HashMap::new(),
      nudges: Vec::new(),
      scan_back: Vec::new(),
      scan_ranges: RangeSet::default(),
    }
  }

  /// The loop of the module note, until stopped with nothing left.
  fn run(&mut self) {
    loop {
      self.shared.passes.fetch_add(1, Ordering::Relaxed);
      let stopping = self.shared.stopping.load(Ordering::Acquire);
      let room = self.in_flight < self.in_flight_limit;
      let scan_room = self.running_scans.len() < self.scan_limit;
      // Sleep only with nothing to admit now.
      let mut wait_ms = IC_EXECUTOR_IDLE_MS;
      if !self.waiting.is_empty() && (room || stopping) {
        wait_ms = 0;
      }
      if !self.scan_queue.is_empty() && (scan_room || stopping) {
        wait_ms = 0;
      }
      // 1. Replies, the readers' nudges, and requests while none wait
      // here: a request left in the inbox keeps its place in the order,
      // and the inbox's bound keeps holding back its application
      // threads. Scans wait in a queue of their own.
      if self.waiting.is_empty() {
        self.conn.poll_with_requests(
          wait_ms,
          Some(&mut self.taken),
          Some(&mut self.nudges),
        );
        for work in self.taken.drain(..) {
          match work {
            Work::Scan(scan) => self.scan_queue.push_back(scan),
            other => self.waiting.push_back(other),
          }
        }
      } else {
        self
          .conn
          .poll_with_requests(wait_ms, None, Some(&mut self.nudges));
      }
      // 2 and 3. The operations the replies completed, and the requests
      // they finished, handed back; the batches completed, moved to
      // their readers, those handed back, and the scans that ended.
      self.take_completions();
      self.take_scan_batches();
      self.take_nudges();
      self.finish_scans();
      // 4. New work while there is room; none once stopping.
      let mut admitted: usize = 0;
      if stopping {
        self.refuse_waiting();
        self.stop_scans();
      } else {
        admitted = self.admit_waiting();
        self.admit_scans();
      }
      // 5. Everything admitted in this pass: one write per node.
      if admitted > 0 {
        // A failed write fails its transactions, whose requests come
        // back with the error.
        let _ = self.conn.send_queries(false);
        self.shared.sends.fetch_add(1, Ordering::Relaxed);
        // A request whose transaction ended at the send, rolled back
        // before it began, goes back now rather than after a sleep.
        self.finish_ended();
      }
      let now = ic_port::time::gethrtime();
      if now >= self.next_sweep {
        self.sweep();
        self.next_sweep = now + IC_EXECUTOR_SWEEP_NANOS;
      }
      let idle = self.waiting.is_empty()
        && self.in_flight == 0
        && self.scan_queue.is_empty()
        && self.running_scans.is_empty();
      if stopping && idle && self.conn.inbox().requests_drained() {
        return;
      }
    }
  }

  // ---- Admission ----

  /// Admit waiting work while there is room. Returns how many requests
  /// were defined, to be sent.
  fn admit_waiting(&mut self) -> usize {
    let mut admitted: usize = 0;
    while self.in_flight < self.in_flight_limit {
      let work = match self.waiting.pop_front() {
        Some(work) => work,
        None => break,
      };
      match work {
        Work::Transaction(request) => {
          if self.admit(request) {
            admitted += 1;
          }
        }
        Work::Bind(bind) => self.bind(bind),
        Work::Scan(scan) => self.scan_queue.push_back(scan),
      }
    }
    admitted
  }

  /// Fail every request waiting, unsent: the executor is stopping.
  fn refuse_waiting(&mut self) {
    let stopped = IcError::new(err::IC_ERROR_EXECUTOR_STOPPED);
    while let Some(work) = self.waiting.pop_front() {
      match work {
        Work::Transaction(mut request) => {
          request.fail(stopped);
          self.deliver(request);
        }
        Work::Bind(mut bind) => bind.refuse(stopped),
        Work::Scan(mut scan) => scan.finish(Some(stopped)),
      }
    }
  }

  /// Bind on this connection, for an application thread. The loop waits
  /// for the dictionary meanwhile, serving replies as it does.
  fn bind(&mut self, mut bind: BindRequest) {
    let reply = match bind.reply.take() {
      Some(reply) => reply,
      None => return,
    };
    match &bind.index {
      Some(index) => {
        match self.conn.index_bind(&bind.database, index, &bind.table) {
          Ok(def) => reply.put(None, Some(def), None),
          Err(e) => reply.put(None, None, Some(e)),
        }
      }
      None => match self.conn.table_bind(&bind.database, &bind.table) {
        Ok(def) => reply.put(Some(def), None, None),
        Err(e) => reply.put(None, None, Some(e)),
      },
    }
  }

  /// Start a request's transaction and define its operations on it,
  /// the commit on the last. True if it is now out, to be sent; false
  /// if it was handed back at once, unsent.
  fn admit(&mut self, mut request: Request) -> bool {
    if request.deadline != 0 {
      let now = ic_port::time::gethrtime();
      if now >= request.deadline {
        self.shared.expired.fetch_add(1, Ordering::Relaxed);
        request.fail(IcError::new(err::IC_ERROR_TIMEOUT));
        self.deliver(request);
        return false;
      }
    }
    if request.ops.is_empty() {
      // Nothing to do, which commits.
      request.state = CommitState::Committed;
      self.deliver(request);
      return false;
    }
    let trans = match self.start_for(&request) {
      Ok(trans) => trans,
      Err(e) => {
        request.fail(e);
        self.deliver(request);
        return false;
      }
    };
    self.shared.admitted.fetch_add(1, Ordering::Relaxed);
    let flight = self.take_flight();
    let count = request.ops.len();
    let mut failed: Option<IcError> = None;
    let mut position: usize = 0;
    while position < count {
      let defined =
        self.define_op(&mut request.ops[position], trans, flight, position);
      if let Err(e) = defined {
        failed = Some(e);
        break;
      }
      position += 1;
    }
    let mut pending = count;
    if failed.is_none() {
      if let Err(e) = self.conn.commit_transaction(trans) {
        failed = Some(e);
      }
    }
    if let Some(e) = failed {
      self.abandon_defined(&mut request, trans, position, e);
      pending = 0;
    }
    self.flights[flight] = Some(Flight {
      request,
      trans,
      pending,
    });
    if pending == 0 {
      self.ending.push(flight);
    }
    true
  }

  /// An operation could not be defined, the one at `failed_at` for
  /// `error`: roll the transaction back, which drops the operations
  /// defined so far unsent, and take their queries back. The
  /// transaction ends rolled back at the send, never having begun.
  fn abandon_defined(
    &mut self,
    request: &mut Request,
    trans: TransId,
    failed_at: usize,
    error: IcError,
  ) {
    let _ = self.conn.rollback_transaction(trans);
    let rolled_back = IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK);
    let mut position: usize = 0;
    while position < request.ops.len() {
      let op = &mut request.ops[position];
      op.error = Some(rolled_back);
      if position == failed_at {
        op.error = Some(error);
      }
      let shape_id = op.shape.id;
      if let Some(qid) = op.query.take() {
        self.give_back(shape_id, qid);
      }
      position += 1;
    }
    request.error = Some(error);
  }

  /// A transaction for a request, where its hint says, or by its first
  /// operation's key.
  fn start_for(&mut self, request: &Request) -> Result<TransId, IcError> {
    let first = &request.ops[0];
    let shape = &first.shape;
    // A unique operation's transaction is placed by its index's table.
    let table: &Arc<TableDef> = match &shape.index {
      Some(index) => index.as_table(),
      None => &shape.table,
    };
    let hint = match request.hint {
      Some(hint) => hint,
      None => match &shape.index {
        Some(index) => {
          self
            .conn
            .hint_for_unique_key(index, &shape.key_rec, &first.key_row)
        }
        None => {
          self
            .conn
            .hint_for_key(&shape.table, &shape.key_rec, &first.key_row)
        }
      },
    };
    self.conn.start_transaction(hint, Some(table.as_ref()))
  }

  /// Define one operation as a query of its shape, its key row, and a
  /// write's values, copied in. Its query carries the flight's number
  /// in `user_ref` and its place in the request in `binding`.
  fn define_op(
    &mut self,
    op: &mut KeyOp,
    trans: TransId,
    flight: usize,
    position: usize,
  ) -> Result<(), IcError> {
    let qid = self.query_for(&op.shape)?;
    let is_read = op.is_read();
    let fits = match self.conn.query_mut(qid) {
      Some(query) => {
        let same = query.key_row().len() == op.key_row.len()
          && query.attr_row().len() == op.attr_row.len();
        if same {
          query.key_row_mut().copy_from_slice(&op.key_row);
          if !is_read {
            query.attr_row_mut().copy_from_slice(&op.attr_row);
          }
          query.set_inputs(&op.inputs);
        }
        same
      }
      None => false,
    };
    let shape_id = op.shape.id;
    if !fits {
      self.give_back(shape_id, qid);
      return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
    }
    let defined = match op.kind {
      OpKind::Read(kind) => {
        let args = ReadKeyArgs {
          kind,
          abort_option: op.abort_option,
          user_ref: flight,
          callback: None,
          binding: position,
        };
        self.conn.read_key(qid, trans, &args)
      }
      OpKind::Write(kind) => {
        let args = WriteKeyArgs {
          kind,
          abort_option: op.abort_option,
          batch_hint: BatchHint::None,
          user_ref: flight,
          callback: None,
          binding: position,
        };
        self.conn.write_key(qid, trans, &args)
      }
    };
    if let Err(e) = defined {
      self.give_back(shape_id, qid);
      return Err(e);
    }
    op.query = Some(qid);
    Ok(())
  }

  /// An idle query of the shape: one kept, or a new one.
  fn query_for(&mut self, shape: &Arc<OpShape>) -> Result<QueryId, IcError> {
    let pool = self.pool_index(shape);
    if let Some(qid) = self.pools[pool].idle.pop() {
      return Ok(qid);
    }
    let qid = match &shape.index {
      Some(index) => {
        self
          .conn
          .create_unique_query(index, &shape.key_rec, &shape.attr_rec)?
      }
      None => {
        self
          .conn
          .create_query(&shape.table, &shape.key_rec, &shape.attr_rec)?
      }
    };
    // A query of the shape runs its program for as long as it is kept.
    let set = match self.conn.query_mut(qid) {
      Some(query) => query
        .set_program(shape.program.as_ref())
        .and_then(|()| query.set_reads_after(shape.after_rec.as_ref())),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if let Err(e) = set {
      let _ = self.conn.free_query(qid);
      return Err(e);
    }
    Ok(qid)
  }

  /// The shape's place in the pools, made if it has none.
  fn pool_index(&mut self, shape: &Arc<OpShape>) -> usize {
    if let Some(index) = self.pool_of.get(&shape.id) {
      return *index;
    }
    self.pools.push(Pool {
      id: shape.id,
      shape: Arc::downgrade(shape),
      idle: Vec::new(),
    });
    let index = self.pools.len() - 1;
    self.pool_of.insert(shape.id, index);
    index
  }

  /// An idle query back to its shape's pool, or let go of if the shape
  /// has none.
  fn give_back(&mut self, shape_id: u64, qid: QueryId) {
    match self.pool_of.get(&shape_id) {
      Some(index) => self.pools[*index].idle.push(qid),
      None => {
        let _ = self.conn.free_query(qid);
      }
    }
  }

  /// Let go of the queries of every shape nobody holds. A shape held by
  /// a request out is held, so its queries are all idle by then.
  fn sweep(&mut self) {
    let mut index: usize = 0;
    while index < self.pools.len() {
      if self.pools[index].shape.strong_count() > 0 {
        index += 1;
        continue;
      }
      let pool = self.pools.swap_remove(index);
      self.pool_of.remove(&pool.id);
      for qid in pool.idle {
        let _ = self.conn.free_query(qid);
      }
      if index < self.pools.len() {
        self.pool_of.insert(self.pools[index].id, index);
      }
    }
    let mut index: usize = 0;
    while index < self.scan_pools.len() {
      if self.scan_pools[index].shape.strong_count() > 0 {
        index += 1;
        continue;
      }
      let pool = self.scan_pools.swap_remove(index);
      self.scan_pool_of.remove(&pool.id);
      for scan in pool.idle {
        let _ = self.conn.free_scan(scan);
      }
      if index < self.scan_pools.len() {
        self.scan_pool_of.insert(self.scan_pools[index].id, index);
      }
    }
  }

  fn take_flight(&mut self) -> usize {
    self.in_flight += 1;
    match self.free_flights.pop() {
      Some(flight) => flight,
      None => {
        self.flights.push(None);
        self.flights.len() - 1
      }
    }
  }

  // ---- Scans ----

  /// Start waiting scans while fewer than the limit run.
  fn admit_scans(&mut self) {
    while self.running_scans.len() < self.scan_limit {
      match self.scan_queue.pop_front() {
        Some(work) => self.admit_scan(work),
        None => break,
      }
    }
  }

  /// Start a reader's run on a scan of its shape, its `user_ref` the
  /// flight's number. A run the reader has closed or left since is
  /// answered or dropped unstarted, and one past its deadline fails
  /// with `IC_ERROR_TIMEOUT`.
  fn admit_scan(&mut self, mut work: ScanWork) {
    // The run's ranges are taken under the lock that says it is still
    // the reader's, and an emptied set is left for the reader's next.
    let (gone, closing, inputs) = {
      let mut state = work.feed.state.lock();
      let gone = state.run != work.run || state.done;
      if !gone {
        self.scan_ranges.clear();
        std::mem::swap(&mut state.ranges, &mut self.scan_ranges);
      }
      (gone, state.closing, state.inputs)
    };
    if gone {
      work.forget();
      return;
    }
    if closing {
      work.finish(None);
      return;
    }
    let mut args = work.args;
    if work.deadline != 0 {
      let now = ic_port::time::gethrtime();
      if now >= work.deadline {
        self.shared.expired.fetch_add(1, Ordering::Relaxed);
        work.finish(Some(IcError::new(err::IC_ERROR_TIMEOUT)));
        return;
      }
      let left = (work.deadline - now) / 1_000_000;
      args.timeout_ms = left.clamp(1, u32::MAX as u64) as u32;
    }
    let scan = match self.scan_for(&work.shape) {
      Ok(scan) => scan,
      Err(e) => {
        work.finish(Some(e));
        return;
      }
    };
    let flight = self.take_scan_flight();
    // The executor hands batches to the reader itself.
    args.trans = None;
    args.callback = None;
    args.read_ahead = false;
    args.user_ref = flight;
    args.binding = 0;
    // An ordered run is merged by the reader: the scan here is unordered,
    // so that its batches go to the reader as they come, over every
    // fragment, a receiver each, as a merge needs.
    if args.ordered {
      args.ordered = false;
      args.parallelism = 0;
    }
    self.conn.set_scan_inputs(scan, &inputs);
    let mut started = Ok(());
    if work.shape.index_shape().is_some() {
      started = self.conn.set_scan_ranges(scan, &self.scan_ranges);
    }
    if started.is_ok() {
      started = self.conn.start_scan(scan, &args);
    }
    if let Err(e) = started {
      self.give_back_scan(work.shape.id(), scan);
      self.free_scan_flights.push(flight);
      work.finish(Some(e));
      return;
    }
    self.shared.scans.fetch_add(1, Ordering::Relaxed);
    let receivers = match self.conn.scan(scan) {
      Some(running) => running.parallelism(),
      None => 0,
    };
    // From here the reader may nudge. A close asked for meanwhile is
    // seen now, as it found no flight to nudge.
    let close_now = {
      let mut state = work.feed.state.lock();
      if state.run == work.run && !state.done {
        state.flight = Some(flight as u32);
        state.receivers = receivers;
        state.closing
      } else {
        true
      }
    };
    self.scan_flights[flight] = Some(ScanFlight {
      work,
      scan,
      closed: false,
      error: None,
    });
    self.running_scans.push(flight);
    if close_now {
      self.close_flight(flight, None);
    }
  }

  fn take_scan_flight(&mut self) -> usize {
    match self.free_scan_flights.pop() {
      Some(flight) => flight,
      None => {
        self.scan_flights.push(None);
        self.scan_flights.len() - 1
      }
    }
  }

  /// An idle scan of the shape: one kept, or a new one.
  fn scan_for(&mut self, shape: &Arc<ScanShape>) -> Result<ScanId, IcError> {
    let pool = match self.scan_pool_of.get(&shape.id()) {
      Some(index) => *index,
      None => {
        self.scan_pools.push(ScanPool {
          id: shape.id(),
          shape: Arc::downgrade(shape),
          idle: Vec::new(),
        });
        let index = self.scan_pools.len() - 1;
        self.scan_pool_of.insert(shape.id(), index);
        index
      }
    };
    if let Some(scan) = self.scan_pools[pool].idle.pop() {
      return Ok(scan);
    }
    let scan = match shape.index_shape() {
      Some(index) => self.conn.create_index_scan(
        index.index(),
        index.key_record(),
        shape.record(),
      )?,
      None => self.conn.create_scan(shape.table(), shape.record())?,
    };
    // A scan of the shape is filtered by its program while it is kept.
    if let Err(e) = self.conn.set_scan_program(scan, shape.program()) {
      let _ = self.conn.free_scan(scan);
      return Err(e);
    }
    Ok(scan)
  }

  /// An idle scan back to its shape's pool, or let go of if the shape
  /// has none.
  fn give_back_scan(&mut self, shape_id: u64, scan: ScanId) {
    match self.scan_pool_of.get(&shape_id) {
      Some(index) => self.scan_pools[*index].idle.push(scan),
      None => {
        let _ = self.conn.free_scan(scan);
      }
    }
  }

  /// Move every complete batch to its reader, and wake the reader if it
  /// sleeps. A batch of a run reading ahead is handed back at once; any
  /// other stays with the executor's connection until the reader hands
  /// it back. One whose reader is closing stays for the close to let
  /// go of.
  fn take_scan_batches(&mut self) {
    while let Some(batch) = self.conn.get_next_scan_batch() {
      let flight = match self.conn.scan(batch.scan()) {
        Some(scan) => scan.user_ref(),
        None => continue,
      };
      let (feed, run, read_ahead) =
        match self.scan_flights.get(flight).and_then(Option::as_ref) {
          Some(entry) if entry.scan == batch.scan() => (
            Arc::clone(&entry.work.feed),
            entry.work.run,
            entry.work.args.read_ahead,
          ),
          _ => continue,
        };
      let mut state = feed.state.lock();
      if state.run != run || state.done || state.closing {
        continue;
      }
      let mut moved = state.spare.pop().unwrap_or_default();
      let (place, last) = match self.conn.swap_scan_batch(
        batch,
        &mut moved.rows,
        &mut moved.row_ends,
      ) {
        Some(found) => found,
        None => {
          state.spare.push(moved);
          continue;
        }
      };
      moved.run = run;
      moved.batch = Some(batch);
      moved.place = place;
      moved.last = last;
      state.full.push_back(moved);
      let wake = state.reader_waiting;
      state.reader_waiting = false;
      drop(state);
      if wake {
        feed.cond.signal();
      }
      if read_ahead {
        let _ = self.conn.release_scan_batch(batch);
      }
    }
  }

  /// Serve the readers' nudges: hand back to their receivers the
  /// batches they have read, and close the scans they asked to close.
  fn take_nudges(&mut self) {
    let mut nudges = std::mem::take(&mut self.nudges);
    for word in &nudges {
      self.serve_nudge(*word as usize);
    }
    nudges.clear();
    self.nudges = nudges;
  }

  fn serve_nudge(&mut self, flight: usize) {
    let (feed, run, read_ahead) =
      match self.scan_flights.get(flight).and_then(Option::as_ref) {
        Some(entry) => (
          Arc::clone(&entry.work.feed),
          entry.work.run,
          entry.work.args.read_ahead,
        ),
        // A nudge for a scan that has ended since.
        None => return,
      };
    let mut back = std::mem::take(&mut self.scan_back);
    let closing = {
      let mut state = feed.state.lock();
      if state.run != run {
        drop(state);
        self.scan_back = back;
        return;
      }
      state.nudged = false;
      std::mem::swap(&mut state.back, &mut back);
      state.closing
    };
    for used in &mut back {
      if !read_ahead && used.run == run {
        if let Some(batch) = used.batch {
          // Its receiver goes on with the next send.
          let _ = self.conn.release_scan_batch(batch);
        }
      }
      used.clear();
    }
    if !back.is_empty() {
      feed.state.lock().spare.append(&mut back);
    }
    self.scan_back = back;
    if closing {
      self.close_flight(flight, None);
    }
  }

  /// Close a running scan, once, its reader to be told `error` if it
  /// is given, in place of the scan's own outcome.
  fn close_flight(&mut self, flight: usize, error: Option<IcError>) {
    let scan = match self.scan_flights.get_mut(flight).and_then(Option::as_mut)
    {
      Some(entry) => {
        if entry.error.is_none() {
          entry.error = error;
        }
        if entry.closed {
          return;
        }
        entry.closed = true;
        entry.scan
      }
      None => return,
    };
    let _ = self.conn.close_scan(scan);
  }

  /// Tell the reader of every scan whose run has ended how it ended,
  /// and keep the scan for the next run of its shape.
  fn finish_scans(&mut self) {
    let mut at: usize = 0;
    while at < self.running_scans.len() {
      let flight = self.running_scans[at];
      let scan = match self.scan_flights.get(flight).and_then(Option::as_ref) {
        Some(entry) => entry.scan,
        None => {
          self.running_scans.swap_remove(at);
          continue;
        }
      };
      if !self.conn.scan_done(scan) {
        at += 1;
        continue;
      }
      self.running_scans.swap_remove(at);
      let entry = self.scan_flights.get_mut(flight).and_then(Option::take);
      self.free_scan_flights.push(flight);
      if let Some(mut entry) = entry {
        let mut error = entry.error;
        if error.is_none() {
          error = self.conn.scan(scan).and_then(|done| done.error());
        }
        entry.work.finish(error);
        self.give_back_scan(entry.work.shape.id(), scan);
      }
    }
  }

  /// The executor stops: scans waiting fail with
  /// `IC_ERROR_EXECUTOR_STOPPED`, and those running are closed, their
  /// readers told the same.
  fn stop_scans(&mut self) {
    let stopped = IcError::new(err::IC_ERROR_EXECUTOR_STOPPED);
    while let Some(mut work) = self.scan_queue.pop_front() {
      work.finish(Some(stopped));
    }
    let mut at: usize = 0;
    while at < self.running_scans.len() {
      let flight = self.running_scans[at];
      self.close_flight(flight, Some(stopped));
      at += 1;
    }
  }

  // ---- Completion ----

  /// Every query the replies completed, into its request; then every
  /// request whose transaction has ended, handed back.
  fn take_completions(&mut self) {
    while let Some(qid) = self.conn.get_next_executed_query() {
      self.complete_op(qid);
    }
    self.finish_ended();
  }

  /// A completed query's outcome, and a read's row, into the operation
  /// it ran, and the query back to its pool.
  fn complete_op(&mut self, qid: QueryId) {
    let (flight, position, error, error_data, result_len) =
      match self.conn.query(qid) {
        Some(query) => (
          query.user_ref(),
          query.binding(),
          query.error(),
          query.error_data(),
          query.result_len(),
        ),
        None => return,
      };
    let mut shape_id: u64 = 0;
    let mut ended = false;
    let mut owned = false;
    let found = self.flights.get_mut(flight).and_then(Option::as_mut);
    if let Some(entry) = found {
      if let Some(op) = entry.request.ops.get_mut(position) {
        if op.query == Some(qid) {
          owned = true;
          op.query = None;
          op.error = error;
          op.error_data = error_data;
          op.result_len = result_len;
          if op.is_read() && error.is_none() {
            // The row is copied out, so that the query is free at once.
            if let Some(query) = self.conn.query(qid) {
              if query.attr_row().len() == op.attr_row.len() {
                op.attr_row.copy_from_slice(query.attr_row());
              }
            }
          }
          if !op.after_row.is_empty() && error.is_none() {
            if let Some(query) = self.conn.query(qid) {
              if query.after_row().len() == op.after_row.len() {
                op.after_row.copy_from_slice(query.after_row());
              }
            }
          }
          shape_id = op.shape.id;
        }
      }
      if owned && entry.pending > 0 {
        entry.pending -= 1;
        ended = entry.pending == 0;
      }
    }
    if !owned {
      // No operation runs it: nothing should complete so, and a query
      // nobody tracks is let go of rather than kept.
      let _ = self.conn.free_query(qid);
      return;
    }
    self.give_back(shape_id, qid);
    if ended {
      self.ending.push(flight);
    }
  }

  /// Hand back every request whose operations are all complete and
  /// whose transaction has ended.
  fn finish_ended(&mut self) {
    let mut index: usize = 0;
    while index < self.ending.len() {
      let flight = self.ending[index];
      let found = self.flights.get(flight).and_then(Option::as_ref);
      let trans = match found {
        Some(entry) => entry.trans,
        None => {
          self.ending.swap_remove(index);
          continue;
        }
      };
      let done = match self.conn.transaction(trans) {
        Some(transaction) => transaction.is_done(),
        None => true,
      };
      if !done {
        index += 1;
        continue;
      }
      self.ending.swap_remove(index);
      self.finish(flight);
    }
  }

  /// A request whose transaction has ended: its outcome from the
  /// transaction, the transaction closed, and the request handed back.
  fn finish(&mut self, flight: usize) {
    let entry = self.flights.get_mut(flight).and_then(Option::take);
    let mut finished = match entry {
      Some(finished) => finished,
      None => return,
    };
    self.free_flights.push(flight);
    self.in_flight -= 1;
    let request = &mut finished.request;
    match self.conn.transaction(finished.trans) {
      Some(trans) => {
        request.state = trans.commit_state();
        request.gci = trans.gci();
        // An error found before the send stands; see `abandon_defined`.
        if request.error.is_none() {
          request.error = trans.error();
          request.error_data = trans.error_data();
        }
      }
      None => {
        request.state = CommitState::RolledBack;
        if request.error.is_none() {
          request.error =
            Some(IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK));
        }
      }
    }
    let _ = self.conn.close_transaction(finished.trans);
    self.shared.completed.fetch_add(1, Ordering::Relaxed);
    self.deliver(finished.request);
  }

  /// Hand a request back as its submitter asked.
  fn deliver(&mut self, mut request: Request) {
    let done = match request.done.take() {
      Some(done) => done,
      None => return,
    };
    match done {
      Done::Slot(slot) => slot.put(Ok(request)),
      Done::Callback(callback, user_ref) => callback(request, user_ref),
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_slot_hands_over_what_is_put_and_is_used_again() {
    let slot = Slot::new();
    assert!(slot.try_take().is_none());
    let mut request = Request::new();
    request.state = CommitState::Committed;
    slot.put(Ok(request));
    let back = slot.wait().expect("the request");
    assert_eq!(back.commit_state(), CommitState::Committed);
    assert!(slot.try_take().is_none(), "taken once");
    slot.put(Err(IcError::new(err::IC_ERROR_TIMEOUT)));
    let e = slot.wait().expect_err("an error");
    assert_eq!(e.code, err::IC_ERROR_TIMEOUT);
  }

  #[test]
  fn a_waiter_is_woken_from_another_thread() {
    let slot = Slot::new();
    let putter = Arc::clone(&slot);
    let handle = std::thread::spawn(move || {
      ic_port::time::microsleep(50_000);
      putter.put(Ok(Request::new()));
    });
    let start = ic_port::time::gethrtime();
    assert!(slot.wait().is_ok());
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    handle.join().expect("joined");
    assert!(waited < 5_000, "woken after {} ms", waited);
  }

  #[test]
  fn a_request_dropped_unanswered_tells_its_slot() {
    // What a panicking executor leaves behind must not leave its
    // callers waiting for ever.
    let slot = Slot::new();
    let mut request = Request::new();
    request.done = Some(Done::Slot(Arc::clone(&slot)));
    drop(request);
    let e = slot.wait().expect_err("told");
    assert_eq!(e.code, err::IC_ERROR_INTERNAL_PANIC);
  }

  #[test]
  fn an_unanswered_bind_tells_its_waiter() {
    let reply = BindReply::new();
    let bind = BindRequest {
      database: "d".to_string(),
      table: "t".to_string(),
      index: None,
      reply: Some(Arc::clone(&reply)),
    };
    drop(bind);
    let answer = reply.wait();
    assert_eq!(
      answer.error.map(|e| e.code),
      Some(err::IC_ERROR_EXECUTOR_STOPPED)
    );
  }

  #[test]
  fn the_outcome_says_committed_or_why_not() {
    let mut request = Request::new();
    request.state = CommitState::Committed;
    assert!(request.outcome().is_ok());
    request.state = CommitState::RolledBack;
    let e = request.outcome().expect_err("rolled back");
    assert_eq!(e.code, err::IC_ERROR_TRANSACTION_ROLLED_BACK);
    request.error = Some(IcError::new(630));
    assert_eq!(request.outcome().expect_err("630").code, 630);
    // A takeover that committed and lost the reads: committed, and an
    // error all the same.
    request.state = CommitState::Committed;
    request.error = Some(IcError::new(4115));
    assert_eq!(request.outcome().expect_err("4115").code, 4115);
    request.reset_outcome();
    assert_eq!(request.commit_state(), CommitState::Started);
    assert!(request.error().is_none());
  }

  #[test]
  fn a_failed_request_fails_each_operation() {
    let mut request = Request::new();
    request.fail(IcError::new(err::IC_ERROR_TIMEOUT));
    assert_eq!(request.commit_state(), CommitState::RolledBack);
    assert_eq!(request.error().map(|e| e.code), Some(err::IC_ERROR_TIMEOUT));
  }

  #[test]
  fn a_deadline_is_set_from_now_and_taken_away_with_zero() {
    let mut request = Request::new();
    let before = ic_port::time::gethrtime();
    request.set_timeout_ms(1000);
    assert!(request.deadline >= before + 1_000_000_000);
    request.set_timeout_ms(0);
    assert_eq!(request.deadline, 0);
  }
}
