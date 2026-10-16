// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The query object (`legacy-c/api/ic_apid_op.ic`, `IC_INT_APID_QUERY`;
//! `IC_APID_QUERY` in `ic_apid.h`): one key operation, defined once and
//! used many times.
//!
//! A query ties a table to a key record and an attribute record, and
//! **owns the rows they lay out.** The application fills the key row,
//! and the attribute row for a write, defines the query on a
//! transaction, and reads the attribute row when the query has
//! completed. Nothing in the library holds a pointer into the
//! application's memory, which is what lets the rows be read during a
//! send and written during a poll without any unsafe code; the C header
//! passes the rows with each call, and its binding will copy or map
//! them onto these. Decided here, 2026-09-22, and noted in chapter 04.
//!
//! A query is idle, then defined on a transaction, then sent, then
//! completed, and is idle again once the application has taken it from
//! the connection's list of executed queries. The redesign of the C's
//! field-by-field binding into records is chapter 04's.

use std::sync::Arc;

use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;
use ic_util::ptr_array::PtrId;

use crate::apid_conn::ApidConnection;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::record::Record;

/// A query's id in its connection, which is also what a data node names
/// it by in a reply.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct QueryId(pub(crate) PtrId);

impl QueryId {
  /// The word a request carries and a reply echoes.
  pub fn as_u32(&self) -> u32 {
    self.0.as_u32()
  }

  /// A query id from its word, as a language binding hands it back.
  /// Whether it still names a live query is for the connection to say.
  pub fn from_u32(word: u32) -> QueryId {
    QueryId(PtrId::from_u32(word))
  }
}

/// A transaction's id in its connection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransId(pub(crate) PtrId);

impl TransId {
  /// The id as a word, for a language binding.
  pub fn as_u32(&self) -> u32 {
    self.0.as_u32()
  }

  /// A transaction id from its word, as a language binding hands it
  /// back. Whether it still names a live transaction is for the
  /// connection to say.
  pub fn from_u32(word: u32) -> TransId {
    TransId(PtrId::from_u32(word))
  }
}

/// What is called when a query completes (`IC_APID_CALLBACK_FUNC`):
/// on the thread that polls, from inside the poll, with the connection,
/// the query's id, and the `user_ref` the query was defined with. The
/// query is by id since it lives in the connection, which the callback
/// has. Inside the callback the query is completed, with its error or
/// its row, and its transaction's state is settled as far as this
/// query decides it; the callback may define queries, start, commit
/// and send, but a poll inside it does nothing. When it returns the
/// query is idle, unless the callback defined it anew. A query with no
/// callback goes on the executed list instead.
pub type QueryCallback = fn(&mut ApidConnection, QueryId, usize);

/// How a read locks (`IC_READ_KEY_QUERY_TYPE`), in the header's order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ReadKind {
  /// A shared lock taken and let go of in the read itself. Through a
  /// unique index the lock is held until the transaction ends, as for
  /// `Locked`: the coordinator takes no simple read through an index.
  /// Verify: chapter 05 §6.8.
  #[default]
  Simple = 0,
  /// A shared lock held until the transaction ends.
  Locked = 1,
  /// An exclusive lock held until the transaction ends.
  Exclusive = 2,
  /// The last committed value, with no lock at all. Through a unique
  /// index a shared lock is taken and held until the transaction ends,
  /// and a backup replica may serve the read. Verify: chapter 05 §6.8.
  Committed = 3,
  /// As exclusive, but failing rather than waiting for another's lock.
  ExclusiveNoWait = 4,
}

/// What a write does (`IC_WRITE_KEY_QUERY_TYPE`), in the header's order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WriteKind {
  /// The row must be there.
  #[default]
  Update = 0,
  /// Insert, or overwrite what is there.
  Write = 1,
  /// The row must not be there.
  Insert = 2,
  /// Delete the row.
  Delete = 3,
}

/// What a failure does to the transaction (`IC_ABORT_OPTION`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AbortOption {
  /// Abort on error for a write, ignore it for a read.
  #[default]
  Default = 0,
  /// The transaction is rolled back.
  AbortOnError = 1,
  /// The query fails alone, and the transaction goes on.
  IgnoreError = 2,
}

/// RonDB's batching hint on a write (`IC_BATCH_HINT`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum BatchHint {
  /// Leave it to the data node.
  #[default]
  None = 0,
  /// The write does not depend on earlier ones in its batch.
  Safe = 1,
  /// It does.
  Unsafe = 2,
}

/// What a read asks for (`IC_READ_KEY_ARGS`, less the rows, which the
/// query owns).
#[derive(Clone, Copy, Debug, Default)]
pub struct ReadKeyArgs {
  /// How to lock.
  pub kind: ReadKind,
  /// What a failure does to the transaction. A committed read can only
  /// ignore errors.
  pub abort_option: AbortOption,
  /// Anything, given back with the completed query.
  pub user_ref: usize,
  /// Called when the query completes, in place of the executed list.
  pub callback: Option<QueryCallback>,
  /// A word for a language binding's own use, read back with
  /// [`ApidQuery::binding`]: the C API keeps the C callback there for
  /// its trampoline. Rust callers leave it 0.
  pub binding: usize,
}

/// What a write asks for (`IC_WRITE_KEY_ARGS`, less the rows).
#[derive(Clone, Copy, Debug, Default)]
pub struct WriteKeyArgs {
  /// What to do.
  pub kind: WriteKind,
  /// What a failure does to the transaction.
  pub abort_option: AbortOption,
  /// RonDB's batching hint.
  pub batch_hint: BatchHint,
  /// Anything, given back with the completed query.
  pub user_ref: usize,
  /// Called when the query completes, in place of the executed list.
  pub callback: Option<QueryCallback>,
  /// A word for a language binding's own use; see
  /// [`ReadKeyArgs::binding`].
  pub binding: usize,
}

/// Where a query is in its life.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum QueryState {
  /// Ready to be defined.
  #[default]
  Idle,
  /// Defined on a transaction and not yet sent.
  Defined,
  /// Sent, its replies not all in.
  Sent,
  /// Done, and waiting to be taken from the executed list.
  Completed,
}

/// One execution's state, between define and completion.
#[derive(Clone, Debug, Default)]
pub(crate) struct Execution {
  /// The transaction it was defined on.
  pub trans: Option<TransId>,
  /// The transaction's id, which every reply must carry.
  pub trans_id: u64,
  /// The flags apart from start, execute and commit, which are set when
  /// it is sent.
  pub flags: key_ops::KeyOpFlags,
  /// Where its request lies, packed when it was defined, until sent.
  pub staged: crate::apid_conn::Staged,
  /// True for a read, whose row is to be put into the attribute row.
  pub is_read: bool,
  /// When a read was sent, for detecting a replaced reading-node link.
  pub sent_at: ic_port::time::IcTimer,
  /// What the coordinator said the operation would bring, once it has.
  pub confirmed: Option<key_ops::KeyOpResult>,
  /// The row, as it has come so far.
  pub row: Vec<u32>,
  /// True once any of the row has come.
  pub has_row: bool,
  /// Words of row that have come, whether kept in `row` or unpacked.
  pub row_words: u32,
  /// True once the row has been unpacked into the attribute row
  /// straight from the signal it came in.
  pub unpacked: bool,
  /// For a takeover of a scanned row, the receiver whose batch it is,
  /// which asks for more only once every takeover of the batch is done.
  pub takeover: Option<crate::scan::TakeoverOf>,
}

/// True if the record is over this version of the table.
fn over_table(rec: &Record, table: &TableDef) -> bool {
  rec.table().table_id() == table.table_id()
    && rec.table().table_version() == table.table_version()
}

/// Refuse records that do not fit a key operation over `table`: both
/// must be over this version of it, and the key record must hold every
/// primary key column.
pub(crate) fn check_records(
  table: &TableDef,
  key_rec: &Record,
  attr_rec: &Record,
) -> Result<(), IcError> {
  let fits = over_table(key_rec, table)
    && over_table(attr_rec, table)
    && key_rec.covers_primary_key();
  if !fits {
    return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
  }
  Ok(())
}

/// As [`check_records`], for an operation through a unique index: the
/// index must be unique and the records over its table. That the key
/// record holds every indexed column is checked when the operation is
/// defined.
pub(crate) fn check_unique_records(
  index: &IndexDef,
  key_rec: &Record,
  attr_rec: &Record,
) -> Result<(), IcError> {
  if !index.is_unique() {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  let table = index.table();
  if !over_table(key_rec, table) || !over_table(attr_rec, table) {
    return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
  }
  Ok(())
}

/// One key operation, defined once and used many times
/// (`IC_APID_QUERY`).
pub struct ApidQuery {
  table: Arc<TableDef>,
  /// The unique index the key goes through, for a unique query.
  index: Option<Arc<IndexDef>>,
  key_rec: Record,
  attr_rec: Record,
  key_row: Vec<u8>,
  attr_row: Vec<u8>,
  state: QueryState,
  error: Option<IcError>,
  /// What the refusal said beyond its code, or zero.
  error_data: u32,
  user_ref: usize,
  callback: Option<QueryCallback>,
  /// The binding's word the query was defined with.
  binding: usize,
  /// Bytes of row data the last read brought.
  result_len: u32,
  pub(crate) execution: Execution,
}

impl std::fmt::Debug for ApidQuery {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ApidQuery({}, {:?}{})",
      self.table.name(),
      self.state,
      match self.error {
        Some(e) => format!(", error {}", e.code),
        None => String::new(),
      }
    )
  }
}

impl ApidQuery {
  /// A query over a table, with a record for its key and one for the
  /// attributes it reads or writes. Both must be over the same version
  /// of the table, and the key record must hold every primary key
  /// column.
  pub fn new(
    table: &Arc<TableDef>,
    key_rec: &Record,
    attr_rec: &Record,
  ) -> Result<ApidQuery, IcError> {
    check_records(table, key_rec, attr_rec)?;
    Ok(ApidQuery {
      table: Arc::clone(table),
      index: None,
      key_rec: key_rec.clone(),
      attr_rec: attr_rec.clone(),
      key_row: vec![0; key_rec.row_size() as usize],
      attr_row: vec![0; attr_rec.row_size() as usize],
      state: QueryState::Idle,
      error: None,
      error_data: 0,
      user_ref: 0,
      callback: None,
      binding: 0,
      result_len: 0,
      execution: Execution::default(),
    })
  }

  /// A query through a unique index (`ic_apid_query_create_unique`).
  /// The records are over the index's table; the key record must hold
  /// every column the index is over, which is checked when the query is
  /// defined. Only reads, updates and deletes go through an index.
  pub fn new_unique(
    index: &Arc<IndexDef>,
    key_rec: &Record,
    attr_rec: &Record,
  ) -> Result<ApidQuery, IcError> {
    check_unique_records(index, key_rec, attr_rec)?;
    let table = index.table();
    Ok(ApidQuery {
      table: Arc::clone(table),
      index: Some(Arc::clone(index)),
      key_rec: key_rec.clone(),
      attr_rec: attr_rec.clone(),
      key_row: vec![0; key_rec.row_size() as usize],
      attr_row: vec![0; attr_rec.row_size() as usize],
      state: QueryState::Idle,
      error: None,
      error_data: 0,
      user_ref: 0,
      callback: None,
      binding: 0,
      result_len: 0,
      execution: Execution::default(),
    })
  }

  /// The table the query is over.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// The unique index the key goes through, for a unique query.
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

  /// The key row, to fill before defining the query.
  pub fn key_row(&self) -> &[u8] {
    &self.key_row
  }

  /// The key row, to fill before defining the query. Not to be changed
  /// while the query is defined or sent.
  pub fn key_row_mut(&mut self) -> &mut [u8] {
    &mut self.key_row
  }

  /// The attribute row: what a read brought, or what a write is to send.
  pub fn attr_row(&self) -> &[u8] {
    &self.attr_row
  }

  /// The attribute row, to fill before defining a write.
  pub fn attr_row_mut(&mut self) -> &mut [u8] {
    &mut self.attr_row
  }

  /// Where the query is in its life.
  pub fn state(&self) -> QueryState {
    self.state
  }

  /// True if the last execution failed (`ic_apid_query_is_failed`).
  pub fn is_failed(&self) -> bool {
    self.error.is_some()
  }

  /// Why the last execution failed. An NDB error comes as its code: 626
  /// for a row that is not there, 630 for one that already is.
  pub fn error(&self) -> Option<IcError> {
    self.error
  }

  /// What the data node said about the last execution's refusal beyond
  /// its code, or zero: for 893, a duplicate key, the id of the unique
  /// index whose value was taken. Verify: chapter 05 §6.8.
  pub fn error_data(&self) -> u32 {
    self.error_data
  }

  /// What was given with the arguments of the last execution.
  pub fn user_ref(&self) -> usize {
    self.user_ref
  }

  /// The callback of the last execution, if it had one.
  pub fn callback(&self) -> Option<QueryCallback> {
    self.callback
  }

  /// The binding's word the query was defined with
  /// ([`ReadKeyArgs::binding`]).
  pub fn binding(&self) -> usize {
    self.binding
  }

  /// Bytes of row data the last read brought; zero for a row that was
  /// not there.
  pub fn result_len(&self) -> u32 {
    self.result_len
  }

  /// The transaction the query was last defined on.
  pub fn transaction(&self) -> Option<TransId> {
    self.execution.trans
  }

  pub(crate) fn set_state(&mut self, state: QueryState) {
    self.state = state;
  }

  pub(crate) fn begin(
    &mut self,
    mut execution: Execution,
    user_ref: usize,
    callback: Option<QueryCallback>,
    binding: usize,
  ) {
    // The last execution's row buffer is kept for its allocation.
    let mut row = std::mem::take(&mut self.execution.row);
    row.clear();
    execution.row = row;
    self.execution = execution;
    self.user_ref = user_ref;
    self.callback = callback;
    self.binding = binding;
    self.error = None;
    self.error_data = 0;
    self.result_len = 0;
    self.state = QueryState::Defined;
  }

  pub(crate) fn fail(&mut self, error: IcError) {
    self.error = Some(error);
    self.state = QueryState::Completed;
  }

  /// As [`fail`](Self::fail), for a refusal that says more than its code.
  pub(crate) fn refuse(&mut self, error: IcError, error_data: u32) {
    self.fail(error);
    self.error_data = error_data;
  }

  pub(crate) fn set_result_len(&mut self, len: u32) {
    self.result_len = len;
  }

  /// Put a read's packed row into the attribute row.
  /// Unpack a whole row straight from the signal it came in into the
  /// attribute row. False if it would not unpack, for the caller to
  /// keep it and let the completion report why.
  pub(crate) fn unpack_direct(&mut self, words: &[u32]) -> bool {
    let unpacked =
      crate::row_codec::unpack_row(&self.attr_rec, words, &mut self.attr_row);
    if unpacked.is_err() {
      return false;
    }
    self.result_len = 4 * words.len() as u32;
    self.execution.unpacked = true;
    true
  }

  pub(crate) fn take_row(&mut self) -> Result<(), IcError> {
    if self.execution.unpacked {
      return Ok(());
    }
    let words = &self.execution.row;
    crate::row_codec::unpack_row(&self.attr_rec, words, &mut self.attr_row)?;
    self.result_len = 4 * words.len() as u32;
    self.execution.row.clear();
    Ok(())
  }
}
