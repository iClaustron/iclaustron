// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Transactions (`legacy-c/api/ic_apid_trans.ic`, which is stubs, and
//! `ic_apid_conn.ic`, `apid_conn_start_transaction` and its kin, also
//! stubs): defining key queries on a transaction, sending them, and
//! completing them as their replies come, with the accounting of
//! doc/rust/05-ndb-protocol.md, 6.6.
//!
//! **A transaction starts locally.** Starting one allocates a record at a
//! coordinator, chosen by the hint, and gives the transaction its id;
//! the data node knows nothing until the first operation is sent with
//! the start flag. Operations are defined one by one and sent together:
//! the last of each batch carries the execute flag, and the last of a
//! batch that commits carries the commit flag too. A commit or a
//! rollback with nothing left to send goes as a request of its own, and
//! one on a transaction that never began completes at once.
//!
//! **Replies find their objects by their first word.** The coordinator
//! answers about the transaction, naming the record it was allocated on:
//! `KEY_OP_CONF` with what each operation will bring, `COMMIT_CONF`,
//! `ROLLBACK_CONF`, their refusals, and `TRANS_ABORTED_REP` when it has
//! rolled the transaction back on its own. The node that read a row
//! answers about the operation, naming the query: `ROW_DATA_REP` with the
//! row, and `KEY_OP_REF` when the operation alone failed. Every reply
//! carries the transaction id and is checked against it, so a reply to
//! an earlier use of the same query or record is passed over.
//!
//! **An operation is complete** when the coordinator has said how much
//! row data to expect and that much has come, or at the first row of a
//! dirty read, or at once for a write, or when it was refused. **A
//! transaction is done** when it has committed or rolled back and every
//! operation sent is complete; its record then goes back to the pool
//! and the transaction stays until the application closes it.
//!
//! **A unique query** goes through a unique hash index: the request is
//! `UNIQUE_OP_REQ`, naming the index's own table and carrying the index
//! columns as its key, and the coordinator finds the row's primary key
//! through the index and runs the operation on the table. Its replies
//! are `UNIQUE_OP_CONF` and `UNIQUE_OP_REF`, laid out as the key family's, and
//! the row comes as `ROW_DATA_REP` like any other. An insert cannot go
//! through an index, and a read through one always takes the shared
//! lock: the reference sends its committed and simple index reads that
//! way, and the coordinator asserts that an index request carries
//! neither the dirty nor the simple flag, which on a debug data node
//! is fatal. Verify: chapter 05 §17.14.
//!
//! **When the coordinator's node fails**, as the reference does it:
//! while a surviving data node has reported the failure and none has
//! yet reported it handled, the transaction waits, because the
//! coordinator that takes over may still answer for it: `TAKEOVER_CONF`
//! says it committed, `TAKEOVER_REF` that it was aborted, which is the
//! outcome wanted if a rollback had been asked for and an error
//! otherwise. Once the failure is reported handled, whatever is still
//! waiting was not found by the takeover and cannot have committed: it
//! is rolled back with the reference's error for that, or counted done
//! if a rollback was what was asked. A link that goes with no failure
//! reported fails the transaction at once, with its outcome unknown:
//! the coordinator aborts what an API node it lost had open.
//! Verify: chapter 05 §17.14.
//!
//! **A query that runs a program** goes interpreted, flag bit 15, its
//! column section in five parts (chapter 05 §9.5): on a read, an update
//! or a delete by primary key, a program that assigns only on an update,
//! and reads after the program only on an update. Its refusal says 6000
//! where the operation aborts on error and 626 where it ignores errors,
//! so that a missed condition aborts the transaction only where the
//! abort option says (§9.4).
//!
//! What is not here yet: placing a unique query by the index's own hash
//! map, so it goes to a node holding the index.

use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_signals::header::SignalHeader;
use ic_signals::interp;
use ic_signals::key_ops;
use ic_signals::key_ops::KeyOpConf;
use ic_signals::key_ops::KeyOpFlags;
use ic_signals::key_ops::KeyOpRef;
use ic_signals::key_ops::KeyOpReq;
use ic_signals::key_ops::RollbackConf;
use ic_signals::key_ops::RowDataRep;
use ic_signals::key_ops::TakeoverConf;
use ic_signals::key_ops::TakeoverRef;
use ic_signals::key_ops::TransAbortedRep;
use ic_signals::key_ops::TransCommitConf;
use ic_signals::key_ops::TransEndRef;
use ic_signals::numbers;
use ic_util::ptr_array::PtrId;

use crate::apid_conn::ApidConnection;
use crate::apid_conn::CoordRecord;
use crate::apid_conn::Staged;
use crate::apid_global::LinkStatus;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::hash;
use crate::node_connect::SignalView;
use crate::program;
use crate::query::AbortOption;
use crate::query::ApidQuery;
use crate::query::BatchHint;
use crate::query::Execution;
use crate::query::QueryCallback;
use crate::query::QueryId;
use crate::query::QueryState;
use crate::query::ReadKeyArgs;
use crate::query::ReadKind;
use crate::query::TransId;
use crate::query::WriteKeyArgs;
use crate::query::WriteKind;
use crate::record::Record;
use crate::row_codec;
use crate::scan::TakeoverKey;

/// NDB's error for a transaction its coordinator's failure aborted,
/// reported once the failure is handled.
pub const IC_NDB_ERROR_NODE_FAILURE_ABORT: i32 = 4010;
/// NDB's error for a transaction the takeover coordinator aborted.
pub const IC_NDB_ERROR_TAKEOVER_ABORT: i32 = 4031;
/// NDB's error for read results lost when a takeover committed the transaction.
pub const IC_NDB_ERROR_TAKEOVER_READ_LOST: i32 = 4115;
/// NDB's error for a simple or dirty read whose reading node failed.
pub const IC_NDB_ERROR_READ_NODE_FAILURE: i32 = 4119;
/// NDB's error for an operation on a table or index whose version has
/// changed since it was bound.
pub const IC_NDB_ERROR_SCHEMA_CHANGED: i32 = 241;
/// NDB's error for an operation on a table or index the coordinator
/// does not know, such as one dropped since it was bound.
pub const IC_NDB_ERROR_UNKNOWN_TABLE: i32 = 284;
/// NDB's error for a transaction whose replies never came while its
/// link stayed up: the data node did not answer in time, and the
/// outcome is unknown (chapter 06 §6.3).
pub const IC_NDB_ERROR_NO_REPLY: i32 = 4012;
/// Previous name for the lost-read error, retained for compatibility.
pub const IC_NDB_ERROR_TAKEOVER_COMMIT_UNASKED: i32 =
  IC_NDB_ERROR_TAKEOVER_READ_LOST;

/// Which node should coordinate a transaction (`IC_TRANSACTION_HINT`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TransactionHint {
  /// The next started node in turn.
  #[default]
  Any,
  /// This node.
  Node(u32),
  /// A node holding this partition of the table the transaction will
  /// mostly touch; see [`ApidConnection::hint_for_key`].
  Partition(u32),
}

/// Where a transaction is (`IC_COMMIT_STATE`), in the header's order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CommitState {
  /// Started, and not yet asked to end.
  #[default]
  Started = 0,
  /// Asked to commit; the coordinator has not answered.
  CommitRequested = 1,
  /// Asked to roll back; the coordinator has not answered.
  RollbackRequested = 2,
  /// Committed.
  Committed = 3,
  /// Rolled back, at the application's request or the coordinator's.
  RolledBack = 4,
}

/// A transaction (`IC_TRANSACTION`).
pub struct Transaction {
  trans_id: u64,
  /// The coordinator's node and the record allocated there.
  pub(crate) coord: CoordRecord,
  state: CommitState,
  /// True once the first operation has been sent: the coordinator has
  /// the transaction.
  pub(crate) begun: bool,
  /// Queries defined and not yet sent, in order.
  pub(crate) defined: Vec<QueryId>,
  /// Queries sent and not yet complete.
  pub(crate) sent: Vec<QueryId>,
  ops_sent: u32,
  ops_completed: u32,
  /// The application asked to commit, or to roll back, and the request
  /// has not gone yet.
  end_wanted: Option<CommitState>,
  error: Option<IcError>,
  /// What the coordinator's abort said beyond its error code, or zero.
  error_data: u32,
  gci: u64,
  /// True while it is on the connection's list for the next send.
  pub(crate) queued_for_send: bool,
  /// When it last sent, by the high-resolution clock, or zero if it
  /// never has: the wait for its replies runs from here.
  last_sent: u64,
}

impl std::fmt::Debug for Transaction {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "Transaction({:#x} at node {}, {:?}, {} sent, {} complete)",
      self.trans_id,
      self.coord.node_id,
      self.state,
      self.ops_sent,
      self.ops_completed
    )
  }
}

impl Transaction {
  /// The transaction id, as the data nodes know it.
  pub fn id(&self) -> u64 {
    self.trans_id
  }

  /// The node whose coordinator runs the transaction.
  pub fn node_id(&self) -> u32 {
    self.coord.node_id
  }

  /// Where the transaction is.
  pub fn commit_state(&self) -> CommitState {
    self.state
  }

  /// Why it failed, including lost read results after a takeover commit.
  /// Check `commit_state()` separately: an error need not mean rollback.
  pub fn error(&self) -> Option<IcError> {
    self.error
  }

  /// What the coordinator said about the error that rolled it back,
  /// beyond its code, or zero: for 893, a duplicate key, the id of the
  /// unique index whose value was taken. Its queries that failed with
  /// the error carry it too.
  pub fn error_data(&self) -> u32 {
    self.error_data
  }

  /// The global checkpoint a commit went into.
  pub fn gci(&self) -> u64 {
    self.gci
  }

  /// True once committed or rolled back with every operation complete.
  pub fn is_done(&self) -> bool {
    let ended = self.state == CommitState::Committed
      || self.state == CommitState::RolledBack;
    ended && self.sent.is_empty() && self.defined.is_empty()
  }

  /// Accept new work only before commit or rollback has been requested.
  fn is_open(&self) -> bool {
    self.state == CommitState::Started && self.end_wanted.is_none()
  }

  fn ended(&self) -> bool {
    self.state == CommitState::Committed
      || self.state == CommitState::RolledBack
  }

  /// True if it waits for a reply, and has since it last sent at least
  /// `limit` nanoseconds before `now`: a query sent and not complete,
  /// or a commit or rollback asked of the coordinator and not answered.
  /// One that is open and waits for nothing, while the application
  /// prepares its next queries, is not waiting.
  fn waited_too_long(&self, now: u64, limit: u64) -> bool {
    let waiting = !self.sent.is_empty()
      || self.state == CommitState::CommitRequested
      || self.state == CommitState::RollbackRequested;
    waiting
      && self.last_sent != 0
      && now.saturating_sub(self.last_sent) >= limit
  }
}

/// True if `rec` holds a column the unique index is over: one of the
/// key columns of the index's own table, found in the record's table by
/// name.
fn holds_indexed_column(index: &IndexDef, rec: &Record) -> bool {
  for column in &index.info().attributes {
    if !column.primary_key {
      continue;
    }
    if let Ok(field_id) = rec.table().field_id(&column.name) {
      if rec.position_of(field_id).is_some() {
        return true;
      }
    }
  }
  false
}

/// The record of a query's transaction id and the transaction's, for
/// checking a reply.
fn same_trans(query: &ApidQuery, trans_id_lo: u32, trans_id_hi: u32) -> bool {
  let id = query.execution.trans_id;
  id as u32 == trans_id_lo && (id >> 32) as u32 == trans_id_hi
}

impl ApidConnection {
  // ---- Queries ----

  /// A query over a table (`ic_apid_query_create`). It lives in the
  /// connection until freed.
  pub fn create_query(
    &mut self,
    table: &std::sync::Arc<TableDef>,
    key_rec: &Record,
    attr_rec: &Record,
  ) -> Result<QueryId, IcError> {
    let query = ApidQuery::new(table, key_rec, attr_rec)?;
    self.insert_query(query)
  }

  /// A query into the object map, its id kept below the tag that marks
  /// a scan's receiver.
  fn insert_query(&mut self, query: ApidQuery) -> Result<QueryId, IcError> {
    let id = self.queries.insert(query)?;
    if !crate::scan::fits_below_tag(id) {
      self.queries.remove(id);
      return Err(IcError::new(err::IC_ERROR_PTR_ARRAY_INDEX_OUT_OF_BOUND));
    }
    Ok(QueryId(id))
  }

  /// A query through a unique index (`ic_apid_query_create_unique`),
  /// with records over the index's table.
  pub fn create_unique_query(
    &mut self,
    index: &std::sync::Arc<IndexDef>,
    key_rec: &Record,
    attr_rec: &Record,
  ) -> Result<QueryId, IcError> {
    let query = ApidQuery::new_unique(index, key_rec, attr_rec)?;
    self.insert_query(query)
  }

  /// A query, if the id names one.
  pub fn query(&self, id: QueryId) -> Option<&ApidQuery> {
    self.queries.get(id.0)
  }

  /// A query to fill or read, if the id names one.
  pub fn query_mut(&mut self, id: QueryId) -> Option<&mut ApidQuery> {
    self.queries.get_mut(id.0)
  }

  /// Free a query that is idle or completed (`ic_apid_query_free`).
  pub fn free_query(&mut self, id: QueryId) -> Result<(), IcError> {
    let query = match self.queries.get(id.0) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let state = query.state();
    if state == QueryState::Defined || state == QueryState::Sent {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
    }
    if state == QueryState::Completed && query.callback().is_none() {
      // Do not leave a completion naming a freed query.
      self.executed.retain(|done| *done != id);
    }
    self.queries.remove(id.0);
    Ok(())
  }

  /// The query completed longest ago and not yet taken, which is idle
  /// again once taken (`ic_apid_conn_get_next_executed_query`).
  pub fn get_next_executed_query(&mut self) -> Option<QueryId> {
    let id = self.executed.pop_front()?;
    if let Some(query) = self.queries.get_mut(id.0) {
      query.set_state(QueryState::Idle);
    }
    Some(id)
  }

  // ---- Transactions ----

  /// A hint for a transaction that will mostly touch the row this key
  /// names (`ic_transaction_hint_from_key`): the row's partition, or
  /// any node when it cannot be worked out.
  pub fn hint_for_key(
    &self,
    table: &TableDef,
    key_rec: &Record,
    key_row: &[u8],
  ) -> TransactionHint {
    match hash::partition_of(table, key_rec, key_row) {
      Ok(partition) => TransactionHint::Partition(partition),
      Err(_) => TransactionHint::Any,
    }
  }

  /// A hint for a transaction whose first operation goes through a
  /// unique index: the partition of the index's own table that the key
  /// is in, to be given to [`start_transaction`](Self::start_transaction)
  /// with `index.as_table()`, as the coordinator places the index read
  /// there. Any node when it cannot be worked out, such as for an index
  /// over a character column whose collation is not known or cannot be
  /// placed. A key of a few short columns is built on the stack.
  pub fn hint_for_unique_key(
    &self,
    index: &IndexDef,
    key_rec: &Record,
    key_row: &[u8],
  ) -> TransactionHint {
    match hash::index_partition_of(index, key_rec, key_row) {
      Ok(partition) => TransactionHint::Partition(partition),
      Err(_) => TransactionHint::Any,
    }
  }

  /// Start a transaction at the node the hint chooses
  /// (`ic_apid_conn_start_transaction`). A partition hint needs the
  /// table it is over: for a unique key's hint, the index's own table.
  pub fn start_transaction(
    &mut self,
    hint: TransactionHint,
    table: Option<&TableDef>,
  ) -> Result<TransId, IcError> {
    // The buffer is the connection's, so that a thread starting
    // thousands of transactions a second allocates nothing here.
    let mut started = std::mem::take(&mut self.started_scratch);
    self.shared.started_nodes_into(&mut started);
    let chosen = self.choose_start_node(hint, table, &started);
    self.started_scratch = started;
    let node_id = chosen?;
    let coord = self.coord_record(node_id)?;
    let trans = Transaction {
      trans_id: self.next_transaction_id(),
      coord,
      state: CommitState::Started,
      begun: false,
      defined: self.spare_lists.pop().unwrap_or_default(),
      sent: self.spare_lists.pop().unwrap_or_default(),
      ops_sent: 0,
      ops_completed: 0,
      end_wanted: None,
      error: None,
      error_data: 0,
      gci: 0,
      queued_for_send: false,
      last_sent: 0,
    };
    let id = TransId(self.transactions.insert(trans)?);
    self.active_insert(coord.index, id);
    Ok(id)
  }

  /// The node a transaction goes to: the hint's, or the started nodes
  /// in turn. A scan with no transaction is placed the same way.
  pub(crate) fn choose_start_node(
    &mut self,
    hint: TransactionHint,
    table: Option<&TableDef>,
    started: &[u32],
  ) -> Result<u32, IcError> {
    if started.is_empty() {
      return Err(IcError::new(err::IC_ERROR_NO_STARTED_DATA_NODE));
    }
    let turn = self.next_request_id();
    let mut node_id = started[turn as usize % started.len()];
    match hint {
      TransactionHint::Any => {}
      TransactionHint::Node(node) => {
        if !started.contains(&node) {
          return Err(IcError::new(err::IC_ERROR_NODE_NOT_READY));
        }
        node_id = node;
      }
      TransactionHint::Partition(partition) => {
        if let Some(table) = table {
          if let Some(node) = hash::choose_node(table, partition, started, turn)
          {
            node_id = node;
          }
        }
      }
    }
    Ok(node_id)
  }

  /// A transaction, if the id names one.
  pub fn transaction(&self, id: TransId) -> Option<&Transaction> {
    self.transactions.get(id.0)
  }

  /// Let go of a transaction that is done.
  pub fn close_transaction(&mut self, id: TransId) -> Result<(), IcError> {
    let done = match self.transactions.get(id.0) {
      Some(trans) => trans.is_done(),
      None => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
    };
    if !done {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
    }
    // Its two lists are kept, emptied, for the next transaction, rather
    // than freed and allocated again.
    if let Some(mut trans) = self.transactions.remove(id.0) {
      if self.spare_lists.len() < IC_SPARE_QUERY_LISTS {
        trans.defined.clear();
        trans.sent.clear();
        self.spare_lists.push(std::mem::take(&mut trans.defined));
        self.spare_lists.push(std::mem::take(&mut trans.sent));
      }
    }
    Ok(())
  }

  /// Define a read by primary key on a transaction
  /// (`ic_apid_conn_read_key`). The query's key row holds the key; the
  /// attribute row gets the columns of the attribute record when the
  /// query completes.
  pub fn read_key(
    &mut self,
    query_id: QueryId,
    trans_id: TransId,
    args: &ReadKeyArgs,
  ) -> Result<(), IcError> {
    let committed = args.kind == ReadKind::Committed;
    let abort_option = match args.abort_option {
      AbortOption::AbortOnError => {
        if committed {
          // A committed read has no transaction to abort.
          return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
        }
        key_ops::IC_ABORT_ON_ERROR
      }
      _ => key_ops::IC_IGNORE_ERROR,
    };
    let exclusive = args.kind == ReadKind::Exclusive
      || args.kind == ReadKind::ExclusiveNoWait;
    // A read through a unique index always takes the shared lock: the
    // reference sends its committed and simple reads that way, and the
    // coordinator asserts that an index request carries neither flag.
    let through_index = match self.queries.get(query_id.0) {
      Some(query) => query.index().is_some(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let flags = KeyOpFlags {
      operation: if exclusive {
        key_ops::IC_OP_READ_EXCLUSIVE
      } else {
        key_ops::IC_OP_READ
      },
      simple: (args.kind == ReadKind::Simple || committed) && !through_index,
      dirty: committed && !through_index,
      // Through an index, a committed read asks for the committed row
      // this way instead, which a backup replica may serve.
      committed_base: committed && through_index,
      abort_option,
      no_wait: args.kind == ReadKind::ExclusiveNoWait,
      ..KeyOpFlags::default()
    };
    self.define(
      query_id,
      trans_id,
      flags,
      true,
      args.user_ref,
      args.callback,
      args.binding,
      None,
    )
  }

  /// Define a write by primary key on a transaction
  /// (`ic_apid_conn_write_key`). The query's key row holds the key and
  /// its attribute row the values; an update leaves the key columns
  /// out, and a delete sends no values. A delete on a table with a blob
  /// column is refused, by primary or unique key alike, with
  /// `IC_ERROR_DELETE_WITH_BLOBS` (chapter 06 §6.2).
  pub fn write_key(
    &mut self,
    query_id: QueryId,
    trans_id: TransId,
    args: &WriteKeyArgs,
  ) -> Result<(), IcError> {
    let operation = match args.kind {
      WriteKind::Update => key_ops::IC_OP_UPDATE,
      WriteKind::Write => key_ops::IC_OP_WRITE,
      WriteKind::Insert => key_ops::IC_OP_INSERT,
      WriteKind::Delete => key_ops::IC_OP_DELETE,
    };
    let abort_option = match args.abort_option {
      AbortOption::IgnoreError => key_ops::IC_IGNORE_ERROR,
      _ => key_ops::IC_ABORT_ON_ERROR,
    };
    let flags = KeyOpFlags {
      operation,
      abort_option,
      batch_safe: args.batch_hint == BatchHint::Safe,
      batch_unsafe: args.batch_hint == BatchHint::Unsafe,
      ..KeyOpFlags::default()
    };
    self.define(
      query_id,
      trans_id,
      flags,
      false,
      args.user_ref,
      args.callback,
      args.binding,
      None,
    )
  }

  /// What read and write have in common: the sections from the rows,
  /// and the query onto the transaction's list. A takeover of a scanned
  /// row takes its key from the scan instead, and its request carries
  /// the scan information and the row's fragment (chapter 05 §7.2).
  #[allow(clippy::too_many_arguments)]
  pub(crate) fn define(
    &mut self,
    query_id: QueryId,
    trans_id: TransId,
    flags: KeyOpFlags,
    is_read: bool,
    user_ref: usize,
    callback: Option<QueryCallback>,
    binding: usize,
    takeover: Option<&TakeoverKey>,
  ) -> Result<(), IcError> {
    let mut flags = flags;
    let (id, coord) = match self.transactions.get(trans_id.0) {
      Some(trans) if trans.is_open() => (trans.trans_id, trans.coord),
      _ => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
    };
    let query = match self.queries.get(query_id.0) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let state = query.state();
    if state == QueryState::Defined || state == QueryState::Sent {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
    }
    // The blobs' parts are rows of another table that the client keeps
    // in step; a delete of the row alone would leave them behind. A
    // unique query's table is the base table, so this covers both.
    if flags.operation == key_ops::IC_OP_DELETE
      && query.table().has_blob_columns()
    {
      return Err(IcError::new(err::IC_ERROR_DELETE_WITH_BLOBS));
    }
    // A takeover names the scanned row of the base table.
    if takeover.is_some() && query.index().is_some() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    // A program, or reads after one, makes the operation interpreted:
    // a read, an update or a delete by primary key, assigning and
    // reading after only on an update.
    let reads_after = query.after_record().is_some();
    if query.program().is_some() || reads_after {
      let operation = flags.operation;
      let update = operation == key_ops::IC_OP_UPDATE;
      let runs = operation == key_ops::IC_OP_READ
        || operation == key_ops::IC_OP_READ_EXCLUSIVE
        || operation == key_ops::IC_OP_DELETE
        || update;
      let writes = matches!(query.program(), Some(p) if p.writes());
      if !runs
        || query.index().is_some()
        || takeover.is_some()
        || ((writes || reads_after) && !update)
      {
        return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
      }
      flags.interpreted = true;
    }
    // A unique query names the index's own table and goes as a
    // UNIQUE_OP_REQ; the coordinator looks the row up through the index.
    let (signal, table_id, table_version) = match query.index() {
      Some(index) => {
        // The reference refuses an insert through an index too.
        let inserts = flags.operation == key_ops::IC_OP_INSERT
          || flags.operation == key_ops::IC_OP_WRITE;
        if inserts {
          return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
        }
        // The row is found by the indexed columns, so an update through
        // the index cannot change them; the reference takes them from
        // the key row whatever the values say (chapter 06 §6.5, Q12).
        if flags.operation == key_ops::IC_OP_UPDATE
          && holds_indexed_column(index, query.attr_record())
        {
          return Err(IcError::new(err::IC_ERROR_INDEXED_COLUMN_UPDATE));
        }
        (
          numbers::IC_SIG_UNIQUE_OP_REQ,
          index.index_id(),
          index.index_version(),
        )
      }
      None => (
        numbers::IC_SIG_KEY_OP_REQ,
        query.table().table_id(),
        query.table().table_version(),
      ),
    };
    // The request is packed now, into the node's staged buffer, with
    // its sections built from the rows into the connection's scratch
    // buffers; the send sets its place in the batch.
    let request = KeyOpReq {
      coord_ptr: coord.coord_ptr,
      query_ref: query_id.as_u32(),
      table_id,
      flags_word: flags.flags_word(),
      table_version,
      trans_id_lo: id as u32,
      trans_id_hi: (id >> 32) as u32,
    };
    let mut key = std::mem::take(&mut self.key_scratch);
    let mut attr = std::mem::take(&mut self.attr_scratch);
    let built = match takeover {
      Some(over) => {
        key.clear();
        key.extend_from_slice(&over.key);
        build_attr_section(query, &flags, is_read, &mut attr)
          .and_then(|()| sections_fit(&key, &attr))
      }
      None => build_sections(query, &flags, is_read, &mut key, &mut attr),
    };
    let header =
      SignalHeader::new(signal, self.block_number(), coord.coord_block);
    let eight = request.encode();
    let ten;
    let mut words: &[u32] = &eight;
    if let Some(over) = takeover {
      ten = request.encode_takeover(over.info, over.fragment);
      words = &ten;
    }
    let staged = match built {
      Ok(()) => {
        let both: [&[u32]; 2] = [&key, &attr];
        let mut sections: &[&[u32]] = &both;
        if attr.is_empty() {
          sections = &both[..1];
        }
        self.stage_signal(
          coord.node_id,
          coord.generation,
          &header,
          words,
          sections,
        )
      }
      Err(e) => Err(e),
    };
    self.key_scratch = key;
    self.attr_scratch = attr;
    let staged = staged?;
    if let Some(query) = self.queries.get_mut(query_id.0) {
      if query.state() == QueryState::Completed && query.callback().is_none() {
        // Successful reuse consumes the previous completion. Keep it if
        // defining the new operation failed, and never scan for callbacks.
        self.executed.retain(|done| *done != query_id);
      }
      query.begin(
        Execution {
          trans: Some(trans_id),
          trans_id: id,
          flags,
          staged,
          is_read,
          reads_after,
          sent_at: 0,
          confirmed: None,
          row: Vec::new(),
          has_row: false,
          row_words: 0,
          unpacked: false,
          takeover: takeover.map(|over| over.of),
        },
        user_ref,
        callback,
        binding,
      );
    }
    if let Some(trans) = self.transactions.get_mut(trans_id.0) {
      trans.defined.push(query_id);
    }
    self.queue_for_send(trans_id);
    Ok(())
  }

  /// Put a transaction on the list for the next send, once.
  fn queue_for_send(&mut self, id: TransId) {
    if let Some(trans) = self.transactions.get_mut(id.0) {
      if !trans.queued_for_send {
        trans.queued_for_send = true;
        self.to_send.push(id);
      }
    }
  }

  /// Ask for the transaction to commit (`ic_apid_conn_commit_transaction`).
  /// The commit goes with the next send: on the last query defined and
  /// not yet sent, or as a request of its own. No further queries may
  /// be defined once commit is requested.
  pub fn commit_transaction(&mut self, id: TransId) -> Result<(), IcError> {
    self.ask_to_end(id, CommitState::CommitRequested)
  }

  /// Ask for the transaction to roll back
  /// (`ic_apid_conn_rollback_transaction`). Queries defined and not yet
  /// sent are dropped. No further queries may be defined once rollback
  /// is requested.
  pub fn rollback_transaction(&mut self, id: TransId) -> Result<(), IcError> {
    self.ask_to_end(id, CommitState::RollbackRequested)
  }

  fn ask_to_end(
    &mut self,
    id: TransId,
    how: CommitState,
  ) -> Result<(), IcError> {
    let mut dropped: Vec<QueryId> = Vec::new();
    match self.transactions.get_mut(id.0) {
      Some(trans) => {
        if !trans.is_open() {
          return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
        }
        trans.end_wanted = Some(how);
        if !trans.queued_for_send {
          trans.queued_for_send = true;
          self.to_send.push(id);
        }
        if how == CommitState::RollbackRequested {
          // Nothing defined and unsent is worth sending now.
          dropped = std::mem::take(&mut trans.defined);
        }
      }
      None => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
    }
    for qid in dropped {
      self.unstage_query(qid);
      let mut takeover = None;
      if let Some(query) = self.queries.get_mut(qid.0) {
        query.set_state(QueryState::Idle);
        takeover = query.execution.takeover.take();
      }
      // A takeover that never went holds its receiver back no longer.
      if let Some(of) = takeover {
        self.takeover_settled(of);
      }
    }
    Ok(())
  }

  /// Send every query defined on this connection, and every commit or
  /// rollback asked for (`ic_apid_conn_send`, `ic_send`): the signals
  /// are packed per node and each node gets one write. With `force`
  /// the writes happen now; without it the adaptive send algorithm may
  /// hold a node's write back for a moment to go with other threads'
  /// signals, never for longer than its bound. A thread that goes on
  /// to poll for the replies loses nothing by leaving `force` off: a
  /// send held back goes when its time is up, and the algorithm learns
  /// from a thread that sends alone not to hold its sends at all.
  pub fn send_queries(&mut self, force: bool) -> Result<(), IcError> {
    // Only the transactions with something to send, not every one in
    // flight; the list is kept for its allocation.
    let mut ids = std::mem::take(&mut self.to_send);
    let mut first_error: Option<IcError> = None;
    // The wait for the replies to what goes now starts now; the clock is
    // read once for every transaction in the send.
    let mut now: u64 = 0;
    if !ids.is_empty() {
      now = ic_port::time::gethrtime();
    }
    for id in &ids {
      if let Some(trans) = self.transactions.get_mut(id.0) {
        trans.queued_for_send = false;
        trans.last_sent = now;
      }
      if let Err(e) = self.send_transaction(*id) {
        if first_error.is_none() {
          first_error = Some(e);
        }
      }
    }
    ids.clear();
    if self.to_send.is_empty() {
      self.to_send = ids;
    }
    // What was staged goes with this send, its flags now set. A node
    // whose link is down fails its write here; the transactions sent to
    // it are failed by the next poll, as their link is gone. The scans'
    // requests for more go too.
    self.send_staged();
    self.queue_scan_more();
    self.scan_send_due = false;
    if let Err(e) = self.send_queued(force) {
      if first_error.is_none() {
        first_error = Some(e);
      }
    }
    match first_error {
      Some(e) => Err(e),
      None => Ok(()),
    }
  }

  /// Send, then poll (`ic_apid_conn_flush`, `ic_flush`). Returns how
  /// many signals the poll took.
  pub fn flush(&mut self, wait_ms: u32, force: bool) -> Result<usize, IcError> {
    self.send_queries(force)?;
    Ok(self.poll(wait_ms))
  }

  fn active_transactions(&self) -> Vec<TransId> {
    self.active_ids().collect()
  }

  /// The queries of one transaction, with the flags of their place in
  /// the batch, then its commit or rollback if one is due.
  fn send_transaction(&mut self, id: TransId) -> Result<(), IcError> {
    let (defined, end_wanted, begun, coord, trans_id) =
      match self.transactions.get_mut(id.0) {
        Some(trans) if trans.ended() => return Ok(()),
        Some(trans) => (
          std::mem::take(&mut trans.defined),
          trans.end_wanted,
          trans.begun,
          trans.coord,
          trans.trans_id,
        ),
        None => return Ok(()),
      };
    let commit_now = end_wanted == Some(CommitState::CommitRequested);
    let mut begun = begun;
    let mut i: usize = 0;
    while i < defined.len() {
      let qid = defined[i];
      let last = i + 1 == defined.len();
      let sent = self.send_query(qid, !begun, last, last && commit_now);
      i += 1;
      match sent {
        Ok(()) => {
          begun = true;
          if let Some(trans) = self.transactions.get_mut(id.0) {
            trans.begun = true;
            trans.sent.push(qid);
            trans.ops_sent += 1;
            if last && commit_now {
              trans.state = CommitState::CommitRequested;
              trans.end_wanted = None;
            }
          }
        }
        Err(e) => {
          // The rest of the batch is not sent; the queries fail here.
          self.unstage_query(qid);
          self.fail_query(qid, e);
          let mut j = i;
          while j < defined.len() {
            self.unstage_query(defined[j]);
            self.fail_query(defined[j], e);
            j += 1;
          }
          return Err(e);
        }
      }
    }
    // The list goes back, emptied, for the next queries defined, rather
    // than being dropped and allocated again.
    let mut defined = defined;
    defined.clear();
    let end = match self.transactions.get_mut(id.0) {
      Some(trans) => {
        if trans.defined.is_empty() && trans.defined.capacity() == 0 {
          trans.defined = defined;
        }
        trans.end_wanted
      }
      None => None,
    };
    if let Some(how) = end {
      self.send_end(id, how, &coord, trans_id, begun)?;
    }
    Ok(())
  }

  /// A query's staged request is not to go.
  fn unstage_query(&mut self, qid: QueryId) {
    let staged = match self.queries.get_mut(qid.0) {
      Some(query) => std::mem::take(&mut query.execution.staged),
      None => return,
    };
    self.unstage(&staged);
  }

  /// One query's request.
  fn send_query(
    &mut self,
    qid: QueryId,
    start: bool,
    execute: bool,
    commit: bool,
  ) -> Result<(), IcError> {
    let (mut flags, staged) = match self.queries.get(qid.0) {
      Some(query) => (query.execution.flags, query.execution.staged),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    // The request was packed when the query was defined; only its
    // place in the batch is set now.
    flags.start = start;
    flags.execute = execute;
    flags.commit = commit;
    self.patch_staged(&staged, IC_KEY_OP_REQ_REQUEST_INFO, flags.flags_word());
    if let Some(query) = self.queries.get_mut(qid.0) {
      query.execution.staged = Staged::default();
      if query.execution.is_read {
        query.execution.sent_at = ic_port::time::gethrtime();
      }
      query.set_state(QueryState::Sent);
    }
    Ok(())
  }

  /// A commit or rollback that could ride on no query.
  fn send_end(
    &mut self,
    id: TransId,
    how: CommitState,
    coord: &CoordRecord,
    trans_id: u64,
    begun: bool,
  ) -> Result<(), IcError> {
    if !begun {
      // The coordinator never heard of it: nothing to end there.
      let ended = match how {
        CommitState::CommitRequested => CommitState::Committed,
        _ => CommitState::RolledBack,
      };
      self.end_transaction(id, ended, None, 0);
      return Ok(());
    }
    let signal = match how {
      CommitState::CommitRequested => numbers::IC_SIG_COMMIT_REQ,
      _ => numbers::IC_SIG_ROLLBACK_REQ,
    };
    // The coordinator finds the transaction by its own pointer for it;
    // the reply carries ours.
    let data = key_ops::trans_end_req(
      coord.coord_ptr,
      trans_id as u32,
      (trans_id >> 32) as u32,
    );
    let header =
      SignalHeader::new(signal, self.block_number(), coord.coord_block);
    self.queue_signal_for_generation(
      coord.node_id,
      coord.generation,
      &header,
      &data,
      &[],
    )?;
    if let Some(trans) = self.transactions.get_mut(id.0) {
      trans.state = how;
      trans.end_wanted = None;
    }
    Ok(())
  }

  // ---- Replies ----

  /// A whole signal no request was waiting for: a reply to a
  /// transaction or one of its queries, if its first word names one.
  /// False if it is neither.
  pub(crate) fn route_reply(&mut self, signal: &SignalView<'_>) -> bool {
    // The unique index family answers in the key family's layouts.
    match signal.sig_no {
      numbers::IC_SIG_KEY_OP_CONF | numbers::IC_SIG_UNIQUE_OP_CONF => {
        self.take_key_conf(signal)
      }
      // A scan's row names its receiver, whose id has the tag a query's
      // never has (chapter 02, "Data flow for a scan").
      numbers::IC_SIG_ROW_DATA_REP => {
        let first = signal.data.first().copied().unwrap_or(0);
        if first & crate::scan::IC_RECEIVER_TAG != 0 {
          self.take_scan_row(signal)
        } else {
          self.take_row(signal)
        }
      }
      numbers::IC_SIG_SCAN_ROW_KEY_REP => self.take_scan_key(signal),
      numbers::IC_SIG_SCAN_CONF => self.take_scan_conf(signal),
      numbers::IC_SIG_SCAN_REF => self.take_scan_ref(signal),
      numbers::IC_SIG_KEY_OP_REF | numbers::IC_SIG_UNIQUE_OP_REF => {
        self.take_key_ref(signal)
      }
      numbers::IC_SIG_TRANS_ABORTED_REP => self.take_rollback_rep(signal),
      numbers::IC_SIG_COMMIT_CONF => self.take_commit_conf(signal),
      numbers::IC_SIG_COMMIT_REF => self.take_trans_ref(signal),
      numbers::IC_SIG_ROLLBACK_CONF => self.take_rollback_conf(signal),
      numbers::IC_SIG_ROLLBACK_REF => self.take_trans_ref(signal),
      numbers::IC_SIG_TAKEOVER_CONF => self.take_fail_conf(signal),
      numbers::IC_SIG_TAKEOVER_REF => self.take_fail_ref(signal),
      numbers::IC_SIG_COORD_FREE_CONF | numbers::IC_SIG_COORD_FREE_REF => {
        self.take_free_reply(signal)
      }
      _ => false,
    }
  }

  /// The coordinator's answer to the COORD_FREE_REQ that gave up a
  /// timed-out transaction's record: nothing more to do. Both answers
  /// lead with our pointer for the record.
  fn take_free_reply(&mut self, signal: &SignalView<'_>) -> bool {
    match signal.data.first() {
      Some(&client_ptr) => self.names_lost_record(client_ptr),
      None => false,
    }
  }

  /// The transaction a coordinator's reply names, if the id is right.
  fn trans_named(
    &self,
    api_ptr: u32,
    trans_id_lo: u32,
    trans_id_hi: u32,
  ) -> Option<TransId> {
    let index = crate::apid_conn::coord_index_of(api_ptr)?;
    let id = self.active_get(index as u32)?;
    if let Some(trans) = self.transactions.get(id.0) {
      let want = trans.trans_id;
      if want as u32 == trans_id_lo && (want >> 32) as u32 == trans_id_hi {
        return Some(id);
      }
    }
    None
  }

  fn take_key_conf(&mut self, signal: &SignalView<'_>) -> bool {
    let conf = match KeyOpConf::decode(signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    // A confirmation with no transaction named is found from its first
    // operation, as the reference does.
    let mut api_ptr = conf.client_ptr;
    if api_ptr == key_ops::IC_KEY_OP_CONF_NO_TRANSACTION {
      let first = match conf.operations().first() {
        Some(op) => QueryId(PtrId::from_u32(op.query_ref)),
        None => return false,
      };
      api_ptr = match self.queries.get(first.0) {
        Some(query) => match query.execution.trans {
          Some(tid) => match self.transactions.get(tid.0) {
            Some(trans) => trans.coord.api_ptr,
            None => return false,
          },
          None => return false,
        },
        None => return false,
      };
    }
    let tid =
      match self.trans_named(api_ptr, conf.trans_id_lo, conf.trans_id_hi) {
        Some(tid) => tid,
        None => return false,
      };
    if conf.needs_commit_ack() {
      self.send_commit_ack(signal, conf.trans_id_lo, conf.trans_id_hi);
    }
    // Record the commit and GCI before completing queries: the last
    // query's callback may close the transaction and start another.
    if conf.is_committed() {
      self.end_transaction(tid, CommitState::Committed, None, conf.gci);
    }
    for op in conf.operations() {
      let qid = QueryId(PtrId::from_u32(op.query_ref));
      if let Some(query) = self.queries.get_mut(qid.0) {
        if same_trans(query, conf.trans_id_lo, conf.trans_id_hi) {
          query.execution.confirmed = Some(*op);
        }
      }
      self.complete_if_done(qid);
    }
    true
  }

  fn take_row(&mut self, signal: &SignalView<'_>) -> bool {
    let front = match RowDataRep::decode(signal.data) {
      Ok(front) => front,
      Err(_) => return false,
    };
    let qid = QueryId(PtrId::from_u32(front.query_ref));
    let query = match self.queries.get_mut(qid.0) {
      Some(query) => query,
      None => return false,
    };
    if query.state() != QueryState::Sent
      || !same_trans(query, front.trans_id_lo, front.trans_id_hi)
    {
      return false;
    }
    let mut section: Option<&[u32]> = None;
    if signal.num_sections > 0 {
      section = Some(signal.section(0));
    }
    let row = RowDataRep::row(signal.data, section);
    // A row that comes whole is unpacked from the signal straight into
    // the attribute row, where it lies in the page the receive thread
    // handed over: a committed read's row always comes in one signal,
    // and another's does when the confirmation has already said this
    // is all of it. Anything else is kept until the rest has come.
    let exec = &query.execution;
    let whole = exec.row_words == 0
      && (exec.flags.dirty
        || matches!(exec.confirmed,
          Some(conf) if !conf.is_dirty_read()
            && conf.row_len as usize == row.len()));
    query.execution.row_words += row.len() as u32;
    query.execution.has_row = true;
    if !(whole && query.unpack_direct(row)) {
      query.execution.row.extend_from_slice(row);
    }
    self.complete_if_done(qid);
    true
  }

  fn take_key_ref(&mut self, signal: &SignalView<'_>) -> bool {
    let refusal = match KeyOpRef::decode(signal.data) {
      Ok(refusal) => refusal,
      Err(_) => return false,
    };
    let qid = QueryId(PtrId::from_u32(refusal.query_ref));
    let ok = match self.queries.get(qid.0) {
      Some(query) => {
        query.state() == QueryState::Sent
          && same_trans(query, refusal.trans_id_lo, refusal.trans_id_hi)
      }
      None => false,
    };
    if !ok {
      return false;
    }
    // A table or index found out of date is let go of, so that the
    // next bind fetches it afresh; the query's index if it has one,
    // since a unique request names the index's own version.
    let code = refusal.error_code as i32;
    if code == IC_NDB_ERROR_SCHEMA_CHANGED || code == IC_NDB_ERROR_UNKNOWN_TABLE
    {
      let bound = self.queries.get(qid.0).map(|query| {
        (std::sync::Arc::clone(query.table()), query.index().cloned())
      });
      match bound {
        Some((_, Some(index))) => self.index_changed(&index),
        Some((table, None)) => self.table_changed(&table),
        None => {}
      }
    }
    // What the refusal says beyond its code, kept with it; failing the
    // query then completes it as any other failure does.
    if let Some(query) = self.queries.get_mut(qid.0) {
      query.refuse(IcError::new(refusal.error_code as i32), refusal.error_data);
    }
    self.finish_query(qid);
    true
  }

  fn take_rollback_rep(&mut self, signal: &SignalView<'_>) -> bool {
    let rep = match TransAbortedRep::decode(signal.data) {
      Ok(rep) => rep,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      rep.client_ptr,
      rep.trans_id_lo,
      rep.trans_id_hi,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    // An operation sent to abort on error is refused this way, so the
    // error data comes here, not with a refusal.
    if let Some(trans) = self.transactions.get_mut(tid.0) {
      trans.error_data = rep.error_data;
    }
    let error = IcError::new(rep.error_code as i32);
    self.end_transaction(tid, CommitState::RolledBack, Some(error), 0);
    true
  }

  fn take_commit_conf(&mut self, signal: &SignalView<'_>) -> bool {
    let conf = match TransCommitConf::decode(signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      conf.client_ptr,
      conf.trans_id_lo,
      conf.trans_id_hi,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    if conf.needs_commit_ack {
      self.send_commit_ack(signal, conf.trans_id_lo, conf.trans_id_hi);
    }
    self.end_transaction(tid, CommitState::Committed, None, conf.gci);
    true
  }

  fn take_rollback_conf(&mut self, signal: &SignalView<'_>) -> bool {
    let conf = match RollbackConf::decode(signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      conf.client_ptr,
      conf.trans_id_lo,
      conf.trans_id_hi,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    self.end_transaction(tid, CommitState::RolledBack, None, 0);
    true
  }

  /// A refused commit, or a refused rollback: either way the
  /// transaction is over at the coordinator, and the error says why.
  fn take_trans_ref(&mut self, signal: &SignalView<'_>) -> bool {
    let refusal = match TransEndRef::decode(signal.data) {
      Ok(refusal) => refusal,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      refusal.client_ptr,
      refusal.trans_id_lo,
      refusal.trans_id_hi,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    let error = IcError::new(refusal.error_code as i32);
    self.end_transaction(tid, CommitState::RolledBack, Some(error), 0);
    true
  }

  /// The takeover committed the transaction. Ordinary replies may have
  /// been lost: complete outstanding writes and fail outstanding reads,
  /// while keeping the transaction's outcome committed.
  fn take_fail_conf(&mut self, signal: &SignalView<'_>) -> bool {
    let conf = match TakeoverConf::decode(signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    let tid =
      self.trans_named(conf.client_ptr, conf.trans_id_lo, conf.trans_id_hi);
    if conf.needs_commit_ack {
      // Acknowledged whatever became of the transaction here.
      self.send_commit_ack(signal, conf.trans_id_lo, conf.trans_id_hi);
    }
    let tid = match tid {
      Some(tid) => tid,
      None => return false,
    };
    let (coord, sent) = match self.transactions.get(tid.0) {
      Some(trans) => (trans.coord, trans.sent.clone()),
      None => return false,
    };
    let lost_read = IcError::new(IC_NDB_ERROR_TAKEOVER_READ_LOST);
    let mut error = None;
    for qid in &sent {
      if let Some(query) = self.queries.get(qid.0) {
        if query.execution.is_read || query.execution.reads_after {
          error = Some(lost_read);
        }
      }
    }
    // Set the outcome before any callback runs, including when an earlier
    // confirmation already marked it committed but rows were still pending.
    if let Some(trans) = self.transactions.get_mut(tid.0) {
      trans.state = CommitState::Committed;
      trans.end_wanted = None;
      trans.error = error;
    }
    // The failed coordinator's record must never go back into the pool.
    self.lose_coord_record(&coord);
    for qid in sent {
      let query = match self.queries.get_mut(qid.0) {
        Some(query) => query,
        None => continue,
      };
      query.set_result_len(0);
      if query.execution.is_read || query.execution.reads_after {
        query.fail(lost_read);
      } else {
        query.set_state(QueryState::Completed);
      }
      self.finish_query(qid);
    }
    self.release_if_done(tid);
    true
  }

  /// The coordinator that took over aborted the transaction: what was
  /// wanted, if a rollback had been asked for.
  fn take_fail_ref(&mut self, signal: &SignalView<'_>) -> bool {
    let refusal = match TakeoverRef::decode(signal.data) {
      Ok(refusal) => refusal,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      refusal.client_ptr,
      refusal.trans_id_lo,
      refusal.trans_id_hi,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    let wanted = match self.transactions.get(tid.0) {
      Some(trans) => trans.state == CommitState::RollbackRequested,
      None => return false,
    };
    let mut error: Option<IcError> = None;
    if !wanted {
      error = Some(IcError::new(IC_NDB_ERROR_TAKEOVER_ABORT));
    }
    self.end_transaction(tid, CommitState::RolledBack, error, 0);
    true
  }

  fn send_commit_ack(&mut self, signal: &SignalView<'_>, id1: u32, id2: u32) {
    let ack = key_ops::commit_marker_free_ord(id1, id2);
    let header = SignalHeader::new(
      numbers::IC_SIG_COMMIT_MARKER_FREE_ORD,
      self.block_number(),
      signal.sender_block,
    );
    let queued = self.queue_signal(signal.sender_node_id, &header, &ack, &[]);
    if let Err(e) = queued {
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "COMMIT_MARKER_FREE_ORD to node {} not queued: {}",
        signal.sender_node_id,
        e.message()
      );
    }
  }

  // ---- Completion ----

  /// Complete the query if its replies are all in: a read's row into
  /// its attribute row, and the query onto the executed list.
  fn complete_if_done(&mut self, qid: QueryId) {
    let (done, is_read) = match self.queries.get(qid.0) {
      Some(query) => {
        if query.state() != QueryState::Sent {
          return;
        }
        let exec = &query.execution;
        let done = match exec.confirmed {
          Some(conf) => {
            if conf.is_dirty_read() {
              exec.has_row
            } else {
              exec.row_words >= conf.row_len
            }
          }
          None => false,
        };
        (done, exec.is_read || exec.reads_after)
      }
      None => return,
    };
    if !done {
      return;
    }
    if let Some(query) = self.queries.get_mut(qid.0) {
      if is_read {
        if let Err(e) = query.take_row() {
          query.fail(e);
        }
      } else {
        query.set_result_len(0);
      }
      query.set_state(QueryState::Completed);
    }
    self.finish_query(qid);
  }

  /// Fail a query, whether sent or only defined.
  fn fail_query(&mut self, qid: QueryId, error: IcError) {
    self.fail_query_with(qid, error, 0);
  }

  /// As `fail_query`, with what the data node said beyond the code.
  fn fail_query_with(&mut self, qid: QueryId, error: IcError, data: u32) {
    if let Some(query) = self.queries.get_mut(qid.0) {
      if query.state() == QueryState::Completed
        || query.state() == QueryState::Idle
      {
        return;
      }
      query.refuse(error, data);
    }
    self.finish_query(qid);
  }

  /// A completed query off its transaction's list, the transaction
  /// closed out if that was its last, and then the query's callback
  /// called, or the query put on the executed list if it has none.
  /// The callback sees the transaction settled as far as this query
  /// decides it, and runs with the poll marked, so that a poll from
  /// inside it does nothing.
  fn finish_query(&mut self, qid: QueryId) {
    let (tid, callback, user_ref, takeover) = match self.queries.get_mut(qid.0)
    {
      Some(query) => (
        query.execution.trans,
        query.callback(),
        query.user_ref(),
        query.execution.takeover.take(),
      ),
      None => (None, None, 0, None),
    };
    if callback.is_none() {
      self.executed.push_back(qid);
    }
    if let Some(tid) = tid {
      self.leave_transaction(qid, tid);
    }
    // A takeover is done, confirmed or failed: its receiver may go on
    // once the batch's others are (chapter 05 §7.2).
    if let Some(of) = takeover {
      self.takeover_settled(of);
    }
    if let Some(callback) = callback {
      self.in_callback += 1;
      callback(self, qid, user_ref);
      self.in_callback -= 1;
      // Idle again, unless the callback defined it anew.
      if let Some(query) = self.queries.get_mut(qid.0) {
        if query.state() == QueryState::Completed {
          query.set_state(QueryState::Idle);
        }
      }
    }
  }

  /// The query off its transaction's lists, and the transaction closed
  /// out if that was its last.
  fn leave_transaction(&mut self, qid: QueryId, tid: TransId) {
    if let Some(trans) = self.transactions.get_mut(tid.0) {
      let mut i: usize = 0;
      while i < trans.sent.len() {
        if trans.sent[i] == qid {
          trans.sent.remove(i);
          trans.ops_completed += 1;
        } else {
          i += 1;
        }
      }
      i = 0;
      while i < trans.defined.len() {
        if trans.defined[i] == qid {
          trans.defined.remove(i);
        } else {
          i += 1;
        }
      }
    }
    self.release_if_done(tid);
  }

  /// The coordinator's last word on a transaction. A rollback cancels
  /// staged requests and fails every sent or defined query with its error.
  fn end_transaction(
    &mut self,
    tid: TransId,
    ended: CommitState,
    error: Option<IcError>,
    gci: u64,
  ) {
    let mut defined: Vec<QueryId> = Vec::new();
    let (sent, error_data) = match self.transactions.get_mut(tid.0) {
      Some(trans) => {
        if trans.ended() {
          return;
        }
        trans.state = ended;
        trans.end_wanted = None;
        trans.error = error;
        trans.gci = gci;
        let mut sent: Vec<QueryId> = Vec::new();
        if ended == CommitState::RolledBack {
          defined = trans.defined.clone();
          sent = trans.sent.clone();
        }
        (sent, trans.error_data)
      }
      None => return,
    };
    let why = match error {
      Some(e) => e,
      None => IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK),
    };
    // Cancel every staged request before invoking any callback: a
    // callback may send work. Keep the queries on the transaction's
    // lists until each completion removes it, so is_done stays accurate.
    for qid in &defined {
      self.unstage_query(*qid);
    }
    for qid in sent {
      self.fail_query_with(qid, why, error_data);
    }
    for qid in defined {
      self.fail_query_with(qid, why, error_data);
    }
    self.release_if_done(tid);
  }

  /// Give the coordinator record back once the transaction is done.
  fn release_if_done(&mut self, tid: TransId) {
    let coord = match self.transactions.get(tid.0) {
      Some(trans) => {
        if !trans.is_done() {
          return;
        }
        trans.coord
      }
      None => return,
    };
    // Completing the last query may already have released this record,
    // and its callback may have started another transaction using it.
    // Release only while the active mapping still belongs to us.
    if self.active_get(coord.index) != Some(tid) {
      return;
    }
    self.active_remove(coord.index);
    self.free_coord_record(&coord);
  }

  /// Fail confirmed dirty reads whose row cannot arrive from the named
  /// reading node. Run after draining the inbox so an arrived row wins.
  pub(crate) fn fail_lost_readers(&mut self) {
    let mut lost = Vec::new();
    for tid in self.active_ids() {
      let trans = match self.transactions.get(tid.0) {
        Some(trans) => trans,
        None => continue,
      };
      for qid in &trans.sent {
        let query = match self.queries.get(qid.0) {
          Some(query) => query,
          None => continue,
        };
        let exec = &query.execution;
        if query.state() != QueryState::Sent
          || exec.has_row
          || exec.sent_at == 0
        {
          continue;
        }
        let conf = match exec.confirmed {
          Some(conf) if conf.is_dirty_read() => conf,
          _ => continue,
        };
        let gone = match self.shared.node(conf.reading_node()) {
          Some(node) => {
            let since = node.published.connected_since();
            node.failure_reported() || since == 0 || since >= exec.sent_at
          }
          None => true,
        };
        if gone {
          lost.push(*qid);
        }
      }
    }
    // Dispatch after walking the lists: callbacks may change them.
    let error = IcError::new(IC_NDB_ERROR_READ_NODE_FAILURE);
    for qid in lost {
      self.fail_query(qid, error);
    }
  }

  /// Snapshot failures before taking the inbox. Each entry holds the
  /// transaction id, whether the node failed, and the link error. While
  /// takeover is pending, leave the transaction waiting for its reply.
  pub(crate) fn lost_transactions(&self) -> Vec<(TransId, bool, IcError)> {
    let mut lost = Vec::new();
    let ids = self.active_transactions();
    for tid in ids {
      let (node_id, generation) = match self.transactions.get(tid.0) {
        Some(trans) => (trans.coord.node_id, trans.coord.generation),
        None => continue,
      };
      if self.link_is(node_id, generation) {
        continue;
      }
      let (awaiting_takeover, failed, link_error) =
        match self.shared.node(node_id) {
          Some(node) => (
            node.membership() == LinkStatus::AwaitingTakeover,
            node.failure_reported(),
            node.not_connected_error(),
          ),
          None => (false, false, IcError::new(err::IC_ERROR_LINK_LOST)),
        };
      if awaiting_takeover {
        // The takeover may still answer for it.
        continue;
      }
      lost.push((tid, failed, link_error));
    }
    lost
  }

  /// Apply the snapshot after receiving replies, which take precedence.
  pub(crate) fn fail_lost_transactions(
    &mut self,
    lost: Vec<(TransId, bool, IcError)>,
  ) {
    for (tid, failed, link_error) in lost {
      let (coord, state) = match self.transactions.get(tid.0) {
        Some(trans) if !trans.ended() => (trans.coord, trans.state),
        // A reply ended it, or a callback has already closed it.
        _ => continue,
      };
      // The record went with the link, whatever became of the node.
      self.lose_coord_record(&coord);
      let rollback_wanted = state == CommitState::RollbackRequested;
      let mut error: Option<IcError> = Some(link_error);
      if failed {
        // Not found by the takeover, so not committed: aborted by the
        // failure, which is what was asked if a rollback was.
        error = None;
        if !rollback_wanted {
          error = Some(IcError::new(IC_NDB_ERROR_NODE_FAILURE_ABORT));
        }
      }
      // Use the common rollback path for both sent and defined queries,
      // including cancellation of staged requests and callback delivery.
      self.end_transaction(tid, CommitState::RolledBack, error, 0);
    }
  }

  /// End every transaction that has waited too long for its replies
  /// while its link stayed up, as of `now` (chapter 06 §6.3). Run by a
  /// poll at most once a second, after it has taken the replies that
  /// came.
  ///
  /// Such a transaction ends rolled back with 4012, whose class says
  /// the outcome is unknown: the coordinator may yet commit it. Each of
  /// its queries not complete fails with 4012. Its record is never used
  /// again, and is freed with COORD_FREE_REQ, which also makes the
  /// coordinator clean up whatever it still holds of the transaction; a
  /// reply that comes later finds no transaction of that id and is
  /// dropped. A transaction that has ended, and waits only for rows of
  /// its reads, fails those queries alone.
  pub(crate) fn expire_transactions(&mut self, now: u64) {
    let limit = self.reply_wait_nanos;
    let mut expired: Vec<TransId> = Vec::new();
    for tid in self.active_ids() {
      if let Some(trans) = self.transactions.get(tid.0) {
        if trans.waited_too_long(now, limit) {
          expired.push(tid);
        }
      }
    }
    // Dispatch after walking the list: callbacks may change it.
    let error = IcError::new(IC_NDB_ERROR_NO_REPLY);
    for tid in expired {
      let (coord, ended, sent) = match self.transactions.get(tid.0) {
        Some(trans) => (trans.coord, trans.ended(), trans.sent.clone()),
        // A callback has already closed it.
        None => continue,
      };
      if ended {
        for qid in sent {
          self.fail_query(qid, error);
        }
        continue;
      }
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "Node {} did not answer transaction {:?} in time",
        coord.node_id,
        tid
      );
      self.lose_coord_record(&coord);
      self.send_coord_free(&coord);
      self.end_transaction(tid, CommitState::RolledBack, Some(error), 0);
    }
  }
}

/// Emptied query lists a connection keeps for its next transactions;
/// enough for a few thousand in flight, beyond which they are freed.
const IC_SPARE_QUERY_LISTS: usize = 8192;

/// The request word of a `KEY_OP_REQ` or `UNIQUE_OP_REQ` that holds the
/// flags, among its data words (`KeyOpReq::encode`).
const IC_KEY_OP_REQ_REQUEST_INFO: usize = 4;

/// A query's key and attribute sections from its rows, into buffers
/// the caller keeps, cleared first: the key, then for a read what to
/// read, for a write the values, and for a delete nothing.
fn build_sections(
  query: &ApidQuery,
  flags: &KeyOpFlags,
  is_read: bool,
  key: &mut Vec<u32>,
  attr_info: &mut Vec<u32>,
) -> Result<(), IcError> {
  match query.index() {
    Some(index) => row_codec::index_key_info_into(
      index,
      query.key_record(),
      query.key_row(),
      key,
    )?,
    None => row_codec::key_info_into(query.key_record(), query.key_row(), key)?,
  }
  build_attr_section(query, flags, is_read, attr_info)?;
  sections_fit(key, attr_info)
}

/// A query's attribute section from its rows, into a buffer the caller
/// keeps, cleared first: for a read what to read, for a write the
/// values, and for a delete nothing.
fn build_attr_section(
  query: &ApidQuery,
  flags: &KeyOpFlags,
  is_read: bool,
  attr_info: &mut Vec<u32>,
) -> Result<(), IcError> {
  if flags.interpreted {
    return build_interpreted_section(query, flags, is_read, attr_info);
  }
  attr_info.clear();
  if is_read {
    row_codec::read_attr_info_into(query.attr_record(), attr_info);
  } else if flags.operation != key_ops::IC_OP_DELETE {
    let skip_keys = flags.operation == key_ops::IC_OP_UPDATE;
    row_codec::write_attr_info_into(
      query.attr_record(),
      query.attr_row(),
      skip_keys,
      attr_info,
    )?;
  }
  Ok(())
}

/// An interpreted operation's column section (chapter 05 §9.5): the
/// inputs its program reads and, for a read, what to read, before the
/// program; the program, refusing with 6000 where the operation aborts
/// on error and with 626 where it ignores errors (§9.4); for an update
/// the program's constants and then the attribute row's values, written
/// after it; and for an update with reads after, those.
fn build_interpreted_section(
  query: &ApidQuery,
  flags: &KeyOpFlags,
  is_read: bool,
  attr_info: &mut Vec<u32>,
) -> Result<(), IcError> {
  let mut refusal = interp::IC_REFUSE_NO_ROW;
  if flags.abort_option == key_ops::IC_ABORT_ON_ERROR {
    refusal = interp::IC_REFUSE_CONDITION;
  }
  let program = query.program();
  let mut section = program::Section::new(attr_info);
  if let Some(program) = program {
    program.inputs_into(query.inputs(), section.out());
  }
  if is_read {
    row_codec::read_attr_info_append(query.attr_record(), section.out());
  }
  section.end_part();
  match program {
    Some(program) => program.code_into(refusal, section.out()),
    // Reads after an update with no program: one that accepts.
    None => section.out().push(interp::IC_INSN_ACCEPT),
  }
  section.end_part();
  if flags.operation == key_ops::IC_OP_UPDATE {
    if let Some(program) = program {
      section.out().extend_from_slice(program.updates());
    }
    row_codec::write_attr_info_append(
      query.attr_record(),
      query.attr_row(),
      true,
      section.out(),
    )?;
  }
  section.end_part();
  if let Some(after) = query.after_record() {
    row_codec::read_attr_info_append(after, section.out());
  }
  section.end_part();
  section.end_part();
  Ok(())
}

/// Refuse what a data node would take for a malformed request rather
/// than a large one: it is never sent.
fn sections_fit(key: &[u32], attr_info: &[u32]) -> Result<(), IcError> {
  if key.len() > key_ops::IC_MAX_KEY_WORDS {
    return Err(IcError::new(err::IC_ERROR_KEY_TOO_LONG));
  }
  if attr_info.len() >= key_ops::IC_MAX_COLUMN_DATA_WORDS {
    return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn sections_too_long_are_refused() {
    let key = vec![1u32; key_ops::IC_MAX_KEY_WORDS];
    let attr = vec![2u32; key_ops::IC_MAX_COLUMN_DATA_WORDS - 1];
    assert!(sections_fit(&key, &attr).is_ok());
    let long_key = vec![1u32; key_ops::IC_MAX_KEY_WORDS + 1];
    let e = sections_fit(&long_key, &[]).expect_err("key");
    assert_eq!(e.code, err::IC_ERROR_KEY_TOO_LONG);
    let long_attr = vec![2u32; key_ops::IC_MAX_COLUMN_DATA_WORDS];
    let e = sections_fit(&key, &long_attr).expect_err("column data");
    assert_eq!(e.code, err::IC_ERROR_RECORD_SIZE_TOO_BIG);
  }

  #[test]
  fn an_interpreted_update_sends_its_five_parts() {
    use crate::program::Cmp;
    use crate::program::ProgramBuilder;
    // interp_t: 0 id, 1 val INT NULL, 2 name VARCHAR(32), 4 big BIGINT
    // UNSIGNED. WHERE val > input 1, big = big + input 0; the row sets
    // name, and big is read back.
    let t = crate::program::test_table();
    let key_rec = Record::default_for(&t, Some(&[0])).expect("key");
    let attr_rec = Record::default_for(&t, Some(&[2])).expect("attr");
    let after_rec = Record::default_for(&t, Some(&[4])).expect("after");
    let mut b = ProgramBuilder::new(&t);
    let val = b.column(1);
    let low = b.input(1);
    let top = b.compare(Cmp::Gt, val, low);
    let big = b.column(4);
    let n = b.input(0);
    let sum = b.add(big, n);
    b.assign(4, sum);
    let p = b.finish(Some(top)).expect("finish");
    let mut query = ApidQuery::new(&t, &key_rec, &attr_rec).expect("query");
    query.set_program(Some(&p)).expect("program");
    query.set_input(0, 512).expect("input");
    query.set_input(1, -3).expect("input");
    query.set_reads_after(Some(&after_rec)).expect("after");
    let at = attr_rec.field(0).expect("name").offset() as usize;
    query.attr_row_mut()[at..at + 3].copy_from_slice(b"\x02r3");
    let flags = KeyOpFlags {
      operation: key_ops::IC_OP_UPDATE,
      abort_option: key_ops::IC_ABORT_ON_ERROR,
      interpreted: true,
      ..KeyOpFlags::default()
    };
    let mut out = vec![99];
    build_attr_section(&query, &flags, false, &mut out).expect("section");
    let mut code: Vec<u32> = Vec::new();
    p.code_into(interp::IC_REFUSE_CONDITION, &mut code);
    let mut want = vec![7, code.len() as u32, 2, 2, 0];
    // Inputs 0 and 1 before the reads, of which an update has none.
    want.extend_from_slice(&[0xffff_0007, 0xffa0_0008, 512, 0]);
    want.extend_from_slice(&[0xffa1_0008, 0xffff_fffd, 0xffff_ffff]);
    want.extend_from_slice(&code);
    // name, written after the program; big, read after that.
    want.extend_from_slice(&[0x0002_0003, 0x0033_7202]);
    want.extend_from_slice(&[0xfff3_0004, 1 << 4]);
    assert_eq!(out, want);
    // A query takes any program of its table, one that assigns too;
    // whether its operation may run it is for the define to say.
    let mut other = ApidQuery::new(&t, &key_rec, &attr_rec).expect("query");
    let mut b = ProgramBuilder::new(&t);
    let k = b.integer(1);
    b.assign(1, k);
    let writes = b.finish(None).expect("finish");
    assert!(other.set_program(Some(&writes)).is_ok());
    assert!(other.set_program(None).is_ok());
    let e = other.set_input(16, 1).expect_err("input");
    assert_eq!(e.code, err::IC_ERROR_BAD_PROGRAM);
  }

  #[test]
  fn a_filtered_read_reads_after_its_program_passes() {
    use crate::program::ProgramBuilder;
    let t = crate::program::test_table();
    let key_rec = Record::default_for(&t, Some(&[0])).expect("key");
    let attr_rec = Record::default_for(&t, None).expect("attr");
    let mut b = ProgramBuilder::new(&t);
    let val = b.column(1);
    let top = b.is_not_null(val);
    let p = b.finish(Some(top)).expect("finish");
    let mut query = ApidQuery::new(&t, &key_rec, &attr_rec).expect("query");
    query.set_program(Some(&p)).expect("program");
    let flags = KeyOpFlags {
      operation: key_ops::IC_OP_READ,
      abort_option: key_ops::IC_IGNORE_ERROR,
      interpreted: true,
      ..KeyOpFlags::default()
    };
    let mut out: Vec<u32> = Vec::new();
    build_attr_section(&query, &flags, true, &mut out).expect("section");
    // Every column read, the program refusing with 626, nothing after.
    let mut want = vec![1, p.words().len() as u32, 0, 0, 0, 0xfff0_0007];
    want.extend_from_slice(p.words());
    assert_eq!(out, want);
    assert_eq!(p.words(), [0x0003_0018, 0x0001_0000, 0x12, 0x0272_0013]);
  }

  #[test]
  fn a_transaction_is_done_only_when_ended_and_empty() {
    let mut trans = Transaction {
      trans_id: 5,
      coord: CoordRecord::for_test(1),
      state: CommitState::Started,
      begun: true,
      defined: Vec::new(),
      sent: vec![QueryId(PtrId::from_u32(1))],
      ops_sent: 1,
      ops_completed: 0,
      end_wanted: None,
      error: None,
      error_data: 0,
      gci: 0,
      queued_for_send: false,
      last_sent: 0,
    };
    assert!(!trans.is_done());
    trans.state = CommitState::Committed;
    assert!(!trans.is_done());
    trans.sent.clear();
    assert!(trans.is_done());
  }

  /// A transaction begun at the coordinator, open, that last sent at
  /// `last_sent` and waits for nothing.
  fn sent_at(last_sent: u64) -> Transaction {
    Transaction {
      trans_id: 5,
      coord: CoordRecord::for_test(1),
      state: CommitState::Started,
      begun: true,
      defined: Vec::new(),
      sent: Vec::new(),
      ops_sent: 1,
      ops_completed: 1,
      end_wanted: None,
      error: None,
      error_data: 0,
      gci: 0,
      queued_for_send: false,
      last_sent,
    }
  }

  #[test]
  fn a_transaction_waits_too_long_only_for_a_reply() {
    const LIMIT: u64 = 1_000;
    const AT: u64 = 50_000;
    let mut trans = sent_at(AT);
    // Open with every reply in: the application's turn, however long.
    assert!(!trans.waited_too_long(AT + 10 * LIMIT, LIMIT));
    // A query sent and not complete.
    trans.sent.push(QueryId(PtrId::from_u32(1)));
    assert!(!trans.waited_too_long(AT + LIMIT - 1, LIMIT));
    assert!(trans.waited_too_long(AT + LIMIT, LIMIT));
    // A clock that reads earlier than the send is not a wait.
    assert!(!trans.waited_too_long(AT - 1, LIMIT));
    // A commit asked of the coordinator, every query complete.
    trans.sent.clear();
    trans.state = CommitState::CommitRequested;
    assert!(!trans.waited_too_long(AT + LIMIT - 1, LIMIT));
    assert!(trans.waited_too_long(AT + LIMIT, LIMIT));
    trans.state = CommitState::RollbackRequested;
    assert!(trans.waited_too_long(AT + LIMIT, LIMIT));
    // Ended with every reply in.
    trans.state = CommitState::Committed;
    assert!(!trans.waited_too_long(AT + 10 * LIMIT, LIMIT));
    // Ended, with a read's row still to come.
    trans.sent.push(QueryId(PtrId::from_u32(2)));
    assert!(trans.waited_too_long(AT + LIMIT, LIMIT));
    // Never sent: nothing to wait for.
    let mut idle = sent_at(0);
    idle.state = CommitState::CommitRequested;
    assert!(!idle.waited_too_long(AT + 10 * LIMIT, LIMIT));
  }
}
