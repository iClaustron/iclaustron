// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Transactions (`legacy-c/api/ic_apid_trans.ic`, which is stubs, and
//! `ic_apid_conn.ic`, `apid_conn_start_transaction` and its kin, also
//! stubs): defining key queries on a transaction, sending them, and
//! completing them as their replies come, with the accounting of
//! doc/rust/05-ndb-protocol.md, 6.6.
//!
//! **A transaction starts locally.** Starting one seizes a record at a
//! coordinator, chosen by the hint, and gives the transaction its id;
//! the data node knows nothing until the first operation is sent with
//! the start flag. Operations are defined one by one and sent together:
//! the last of each batch carries the execute flag, and the last of a
//! batch that commits carries the commit flag too. A commit or a
//! rollback with nothing left to send goes as a request of its own, and
//! one on a transaction that never began completes at once.
//!
//! **Replies find their objects by their first word.** The coordinator
//! answers about the transaction, naming the record it was seized on:
//! `TCKEYCONF` with what each operation will bring, `TC_COMMITCONF`,
//! `TCROLLBACKCONF`, their refusals, and `TCROLLBACKREP` when it has
//! rolled the transaction back on its own. The node that read a row
//! answers about the operation, naming the query: `TRANSID_AI` with the
//! row, and `TCKEYREF` when the operation alone failed. Every reply
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
//! What is not here yet: callbacks, which chapter 04 wants beside the
//! executed list; the takeover replies of a coordinator that failed,
//! `TCKEY_FAILCONF` and `TCKEY_FAILREF`, on which a lost link now
//! fails the transaction outright; and unique-key operations.

use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::tc_key;
use ic_ndb_signals::tc_key::TcCommitConf;
use ic_ndb_signals::tc_key::TcKeyConf;
use ic_ndb_signals::tc_key::TcKeyFlags;
use ic_ndb_signals::tc_key::TcKeyRef;
use ic_ndb_signals::tc_key::TcKeyReq;
use ic_ndb_signals::tc_key::TcRollbackConf;
use ic_ndb_signals::tc_key::TcRollbackRep;
use ic_ndb_signals::tc_key::TcTransRef;
use ic_ndb_signals::tc_key::TransIdAi;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_util::ptr_array::PtrId;

use crate::apid_conn::ApidConnection;
use crate::apid_conn::TcRecord;
use crate::dict_cache::TableDef;
use crate::hash;
use crate::node_connect::ReceivedSignal;
use crate::query::AbortOption;
use crate::query::ApidQuery;
use crate::query::BatchHint;
use crate::query::Execution;
use crate::query::QueryId;
use crate::query::QueryState;
use crate::query::ReadKeyArgs;
use crate::query::ReadKind;
use crate::query::TransId;
use crate::query::WriteKeyArgs;
use crate::query::WriteKind;
use crate::record::Record;
use crate::row_codec;

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
  /// The coordinator's node and the record seized there.
  pub(crate) tc: TcRecord,
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
  gci: u64,
}

impl std::fmt::Debug for Transaction {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "Transaction({:#x} at node {}, {:?}, {} sent, {} complete)",
      self.trans_id,
      self.tc.node_id,
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
    self.tc.node_id
  }

  /// Where the transaction is.
  pub fn commit_state(&self) -> CommitState {
    self.state
  }

  /// Why it was rolled back, if the coordinator or a lost link did it.
  pub fn error(&self) -> Option<IcError> {
    self.error
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

  fn ended(&self) -> bool {
    self.state == CommitState::Committed
      || self.state == CommitState::RolledBack
  }
}

/// The record of a query's transaction id and the transaction's, for
/// checking a reply.
fn same_trans(query: &ApidQuery, trans_id1: u32, trans_id2: u32) -> bool {
  let id = query.execution.trans_id;
  id as u32 == trans_id1 && (id >> 32) as u32 == trans_id2
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
    Ok(QueryId(self.queries.insert(query)?))
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
    let state = match self.queries.get(id.0) {
      Some(query) => query.state(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if state == QueryState::Defined || state == QueryState::Sent {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
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

  /// Start a transaction at the node the hint chooses
  /// (`ic_apid_conn_start_transaction`). A partition hint needs the
  /// table it is over.
  pub fn start_transaction(
    &mut self,
    hint: TransactionHint,
    table: Option<&TableDef>,
  ) -> Result<TransId, IcError> {
    let started = self.started_nodes();
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
          if let Some(node) =
            hash::choose_node(table, partition, &started, turn)
          {
            node_id = node;
          }
        }
      }
    }
    let tc = self.tc_record(node_id)?;
    let trans = Transaction {
      trans_id: self.next_transaction_id(),
      tc,
      state: CommitState::Started,
      begun: false,
      defined: Vec::new(),
      sent: Vec::new(),
      ops_sent: 0,
      ops_completed: 0,
      end_wanted: None,
      error: None,
      gci: 0,
    };
    let id = TransId(self.transactions.insert(trans)?);
    self.active.push((tc.api_ptr, id));
    Ok(id)
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
    self.transactions.remove(id.0);
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
        tc_key::IC_ABORT_ON_ERROR
      }
      _ => tc_key::IC_IGNORE_ERROR,
    };
    let exclusive = args.kind == ReadKind::Exclusive
      || args.kind == ReadKind::ExclusiveNoWait;
    let flags = TcKeyFlags {
      operation: if exclusive {
        tc_key::IC_OP_READ_EXCLUSIVE
      } else {
        tc_key::IC_OP_READ
      },
      simple: args.kind == ReadKind::Simple || committed,
      dirty: committed,
      abort_option,
      no_wait: args.kind == ReadKind::ExclusiveNoWait,
      ..TcKeyFlags::default()
    };
    self.define(query_id, trans_id, flags, true, args.user_ref)
  }

  /// Define a write by primary key on a transaction
  /// (`ic_apid_conn_write_key`). The query's key row holds the key and
  /// its attribute row the values; an update leaves the key columns
  /// out, and a delete sends no values.
  pub fn write_key(
    &mut self,
    query_id: QueryId,
    trans_id: TransId,
    args: &WriteKeyArgs,
  ) -> Result<(), IcError> {
    let operation = match args.kind {
      WriteKind::Update => tc_key::IC_OP_UPDATE,
      WriteKind::Write => tc_key::IC_OP_WRITE,
      WriteKind::Insert => tc_key::IC_OP_INSERT,
      WriteKind::Delete => tc_key::IC_OP_DELETE,
    };
    let abort_option = match args.abort_option {
      AbortOption::IgnoreError => tc_key::IC_IGNORE_ERROR,
      _ => tc_key::IC_ABORT_ON_ERROR,
    };
    let flags = TcKeyFlags {
      operation,
      abort_option,
      batch_safe: args.batch_hint == BatchHint::Safe,
      batch_unsafe: args.batch_hint == BatchHint::Unsafe,
      ..TcKeyFlags::default()
    };
    self.define(query_id, trans_id, flags, false, args.user_ref)
  }

  /// What read and write have in common: the sections from the rows,
  /// and the query onto the transaction's list.
  fn define(
    &mut self,
    query_id: QueryId,
    trans_id: TransId,
    flags: TcKeyFlags,
    is_read: bool,
    user_ref: usize,
  ) -> Result<(), IcError> {
    let (id, ended) = match self.transactions.get(trans_id.0) {
      Some(trans) => (trans.trans_id, trans.ended()),
      None => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
    };
    if ended {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
    }
    let query = match self.queries.get_mut(query_id.0) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let state = query.state();
    if state == QueryState::Defined || state == QueryState::Sent {
      return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
    }
    let key = row_codec::key_info(query.key_record(), query.key_row())?;
    let mut attr_info: Vec<u32> = Vec::new();
    if is_read {
      attr_info = row_codec::read_attr_info(query.attr_record());
    } else if flags.operation != tc_key::IC_OP_DELETE {
      let skip_keys = flags.operation == tc_key::IC_OP_UPDATE;
      attr_info = row_codec::write_attr_info(
        query.attr_record(),
        query.attr_row(),
        skip_keys,
      )?;
    }
    query.begin(
      Execution {
        trans: Some(trans_id),
        trans_id: id,
        flags,
        key,
        attr_info,
        is_read,
        confirmed: None,
        row: Vec::new(),
        has_row: false,
      },
      user_ref,
    );
    if let Some(trans) = self.transactions.get_mut(trans_id.0) {
      trans.defined.push(query_id);
    }
    Ok(())
  }

  /// Ask for the transaction to commit (`ic_apid_conn_commit_transaction`).
  /// The commit goes with the next send: on the last query defined and
  /// not yet sent, or as a request of its own.
  pub fn commit_transaction(&mut self, id: TransId) -> Result<(), IcError> {
    self.ask_to_end(id, CommitState::CommitRequested)
  }

  /// Ask for the transaction to roll back
  /// (`ic_apid_conn_rollback_transaction`). Queries defined and not yet
  /// sent are dropped.
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
        if trans.ended() || trans.end_wanted.is_some() {
          return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
        }
        trans.end_wanted = Some(how);
        if how == CommitState::RollbackRequested {
          // Nothing defined and unsent is worth sending now.
          dropped = std::mem::take(&mut trans.defined);
        }
      }
      None => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
    }
    for qid in dropped {
      if let Some(query) = self.queries.get_mut(qid.0) {
        query.set_state(QueryState::Idle);
      }
    }
    Ok(())
  }

  /// Send every query defined on this connection, and every commit or
  /// rollback asked for (`ic_apid_conn_send`).
  pub fn send_queries(&mut self) -> Result<(), IcError> {
    let ids: Vec<TransId> = self.active_transactions();
    let mut first_error: Option<IcError> = None;
    for id in ids {
      if let Err(e) = self.send_transaction(id) {
        if first_error.is_none() {
          first_error = Some(e);
        }
      }
    }
    match first_error {
      Some(e) => Err(e),
      None => Ok(()),
    }
  }

  /// Send, then poll (`ic_apid_conn_flush`). Returns how many signals
  /// the poll took.
  pub fn flush(&mut self, wait_ms: u32) -> Result<usize, IcError> {
    self.send_queries()?;
    Ok(self.poll(wait_ms))
  }

  fn active_transactions(&self) -> Vec<TransId> {
    let mut ids: Vec<TransId> = Vec::new();
    for (_, id) in &self.active {
      ids.push(*id);
    }
    ids
  }

  /// The queries of one transaction, with the flags of their place in
  /// the batch, then its commit or rollback if one is due.
  fn send_transaction(&mut self, id: TransId) -> Result<(), IcError> {
    let (defined, end_wanted, begun, tc, trans_id) =
      match self.transactions.get_mut(id.0) {
        Some(trans) => (
          std::mem::take(&mut trans.defined),
          trans.end_wanted,
          trans.begun,
          trans.tc,
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
      let sent =
        self.send_query(qid, &tc, trans_id, !begun, last, last && commit_now);
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
          self.fail_query(qid, e);
          let mut j = i;
          while j < defined.len() {
            self.fail_query(defined[j], e);
            j += 1;
          }
          return Err(e);
        }
      }
    }
    let end = match self.transactions.get(id.0) {
      Some(trans) => trans.end_wanted,
      None => None,
    };
    if let Some(how) = end {
      self.send_end(id, how, &tc, trans_id, begun)?;
    }
    Ok(())
  }

  /// One query's request.
  fn send_query(
    &mut self,
    qid: QueryId,
    tc: &TcRecord,
    trans_id: u64,
    start: bool,
    execute: bool,
    commit: bool,
  ) -> Result<(), IcError> {
    let query = match self.queries.get_mut(qid.0) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let table_id = query.table().table_id();
    let table_version = query.table().table_version();
    let mut flags = query.execution.flags;
    flags.start = start;
    flags.execute = execute;
    flags.commit = commit;
    let req = flags.request_info();
    let key = std::mem::take(&mut query.execution.key);
    let attr_info = std::mem::take(&mut query.execution.attr_info);
    let request = TcKeyReq {
      tc_connect_ptr: tc.tc_ptr,
      api_operation_ptr: qid.as_u32(),
      table_id,
      request_info: req,
      table_version,
      trans_id1: trans_id as u32,
      trans_id2: (trans_id >> 32) as u32,
    };
    let header =
      SignalHeader::new(gsn::IC_GSN_TCKEYREQ, self.block_number(), tc.tc_block);
    let both: [&[u32]; 2] = [&key, &attr_info];
    let mut sections: &[&[u32]] = &both;
    if attr_info.is_empty() {
      sections = &both[..1];
    }
    self.send(tc.node_id, &header, &request.encode(), sections)?;
    if let Some(query) = self.queries.get_mut(qid.0) {
      query.set_state(QueryState::Sent);
    }
    Ok(())
  }

  /// A commit or rollback that could ride on no query.
  fn send_end(
    &mut self,
    id: TransId,
    how: CommitState,
    tc: &TcRecord,
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
      CommitState::CommitRequested => gsn::IC_GSN_TC_COMMITREQ,
      _ => gsn::IC_GSN_TCROLLBACKREQ,
    };
    let data = tc_key::tc_trans_req(
      tc.api_ptr,
      trans_id as u32,
      (trans_id >> 32) as u32,
    );
    let header = SignalHeader::new(signal, self.block_number(), tc.tc_block);
    self.send(tc.node_id, &header, &data, &[])?;
    if let Some(trans) = self.transactions.get_mut(id.0) {
      trans.state = how;
      trans.end_wanted = None;
    }
    Ok(())
  }

  // ---- Replies ----

  /// A whole signal no request was waiting for: a reply to a
  /// transaction or one of its queries, if its first word names one.
  /// Hands the signal back otherwise.
  pub(crate) fn route_reply(
    &mut self,
    signal: ReceivedSignal,
  ) -> Option<ReceivedSignal> {
    let taken = match signal.gsn {
      gsn::IC_GSN_TCKEYCONF => self.take_key_conf(&signal),
      gsn::IC_GSN_TRANSID_AI => self.take_row(&signal),
      gsn::IC_GSN_TCKEYREF => self.take_key_ref(&signal),
      gsn::IC_GSN_TCROLLBACKREP => self.take_rollback_rep(&signal),
      gsn::IC_GSN_TC_COMMITCONF => self.take_commit_conf(&signal),
      gsn::IC_GSN_TC_COMMITREF => self.take_trans_ref(&signal),
      gsn::IC_GSN_TCROLLBACKCONF => self.take_rollback_conf(&signal),
      gsn::IC_GSN_TCROLLBACKREF => self.take_trans_ref(&signal),
      _ => false,
    };
    if taken {
      return None;
    }
    Some(signal)
  }

  /// The transaction a coordinator's reply names, if the id is right.
  fn trans_named(
    &self,
    api_ptr: u32,
    trans_id1: u32,
    trans_id2: u32,
  ) -> Option<TransId> {
    for (ptr, id) in &self.active {
      if *ptr != api_ptr {
        continue;
      }
      if let Some(trans) = self.transactions.get(id.0) {
        let want = trans.trans_id;
        if want as u32 == trans_id1 && (want >> 32) as u32 == trans_id2 {
          return Some(*id);
        }
      }
    }
    None
  }

  fn take_key_conf(&mut self, signal: &ReceivedSignal) -> bool {
    let conf = match TcKeyConf::decode(&signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    // A confirmation with no transaction named is found from its first
    // operation, as the reference does.
    let mut api_ptr = conf.api_connect_ptr;
    if api_ptr == tc_key::IC_TCKEYCONF_NO_TRANSACTION {
      let first = match conf.operations.first() {
        Some(op) => QueryId(PtrId::from_u32(op.api_operation_ptr)),
        None => return false,
      };
      api_ptr = match self.queries.get(first.0) {
        Some(query) => match query.execution.trans {
          Some(tid) => match self.transactions.get(tid.0) {
            Some(trans) => trans.tc.api_ptr,
            None => return false,
          },
          None => return false,
        },
        None => return false,
      };
    }
    let tid = match self.trans_named(api_ptr, conf.trans_id1, conf.trans_id2) {
      Some(tid) => tid,
      None => return false,
    };
    if conf.needs_commit_ack() {
      self.send_commit_ack(signal, conf.trans_id1, conf.trans_id2);
    }
    for op in &conf.operations {
      let qid = QueryId(PtrId::from_u32(op.api_operation_ptr));
      if let Some(query) = self.queries.get_mut(qid.0) {
        if same_trans(query, conf.trans_id1, conf.trans_id2) {
          query.execution.confirmed = Some(*op);
        }
      }
      self.complete_if_done(qid);
    }
    if conf.is_committed() {
      self.end_transaction(tid, CommitState::Committed, None, conf.gci);
    }
    true
  }

  fn take_row(&mut self, signal: &ReceivedSignal) -> bool {
    let front = match TransIdAi::decode(&signal.data) {
      Ok(front) => front,
      Err(_) => return false,
    };
    let qid = QueryId(PtrId::from_u32(front.api_operation_ptr));
    let query = match self.queries.get_mut(qid.0) {
      Some(query) => query,
      None => return false,
    };
    if query.state() != QueryState::Sent
      || !same_trans(query, front.trans_id1, front.trans_id2)
    {
      return false;
    }
    let mut section: Option<&[u32]> = None;
    if !signal.sections.is_empty() {
      section = Some(signal.section(0));
    }
    let row = TransIdAi::row(&signal.data, section);
    query.execution.row.extend_from_slice(row);
    query.execution.has_row = true;
    self.complete_if_done(qid);
    true
  }

  fn take_key_ref(&mut self, signal: &ReceivedSignal) -> bool {
    let refusal = match TcKeyRef::decode(&signal.data) {
      Ok(refusal) => refusal,
      Err(_) => return false,
    };
    let qid = QueryId(PtrId::from_u32(refusal.api_operation_ptr));
    let ok = match self.queries.get(qid.0) {
      Some(query) => {
        query.state() == QueryState::Sent
          && same_trans(query, refusal.trans_id1, refusal.trans_id2)
      }
      None => false,
    };
    if !ok {
      return false;
    }
    self.fail_query(qid, IcError::new(refusal.error_code as i32));
    true
  }

  fn take_rollback_rep(&mut self, signal: &ReceivedSignal) -> bool {
    let rep = match TcRollbackRep::decode(&signal.data) {
      Ok(rep) => rep,
      Err(_) => return false,
    };
    let tid =
      match self.trans_named(rep.api_connect_ptr, rep.trans_id1, rep.trans_id2)
      {
        Some(tid) => tid,
        None => return false,
      };
    let error = IcError::new(rep.error_code as i32);
    self.end_transaction(tid, CommitState::RolledBack, Some(error), 0);
    true
  }

  fn take_commit_conf(&mut self, signal: &ReceivedSignal) -> bool {
    let conf = match TcCommitConf::decode(&signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      conf.api_connect_ptr,
      conf.trans_id1,
      conf.trans_id2,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    if conf.needs_commit_ack {
      self.send_commit_ack(signal, conf.trans_id1, conf.trans_id2);
    }
    self.end_transaction(tid, CommitState::Committed, None, conf.gci);
    true
  }

  fn take_rollback_conf(&mut self, signal: &ReceivedSignal) -> bool {
    let conf = match TcRollbackConf::decode(&signal.data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      conf.api_connect_ptr,
      conf.trans_id1,
      conf.trans_id2,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    self.end_transaction(tid, CommitState::RolledBack, None, 0);
    true
  }

  /// A refused commit, or a refused rollback: either way the
  /// transaction is over at the coordinator, and the error says why.
  fn take_trans_ref(&mut self, signal: &ReceivedSignal) -> bool {
    let refusal = match TcTransRef::decode(&signal.data) {
      Ok(refusal) => refusal,
      Err(_) => return false,
    };
    let tid = match self.trans_named(
      refusal.api_connect_ptr,
      refusal.trans_id1,
      refusal.trans_id2,
    ) {
      Some(tid) => tid,
      None => return false,
    };
    let error = IcError::new(refusal.error_code as i32);
    self.end_transaction(tid, CommitState::RolledBack, Some(error), 0);
    true
  }

  fn send_commit_ack(&mut self, signal: &ReceivedSignal, id1: u32, id2: u32) {
    let ack = tc_key::tc_commit_ack(id1, id2);
    let header = SignalHeader::new(
      gsn::IC_GSN_TC_COMMIT_ACK,
      self.block_number(),
      signal.sender_block,
    );
    if let Err(e) = self.send(signal.sender_node_id, &header, &ack, &[]) {
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "TC_COMMIT_ACK to node {} not sent: {}",
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
              exec.row.len() as u32 >= conf.row_len
            }
          }
          None => false,
        };
        (done, exec.is_read)
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
    if let Some(query) = self.queries.get_mut(qid.0) {
      if query.state() == QueryState::Completed
        || query.state() == QueryState::Idle
      {
        return;
      }
      query.fail(error);
    }
    self.finish_query(qid);
  }

  /// A completed query off its transaction's list and onto the executed
  /// list, and the transaction closed out if that was its last.
  fn finish_query(&mut self, qid: QueryId) {
    let tid = match self.queries.get(qid.0) {
      Some(query) => query.execution.trans,
      None => None,
    };
    self.executed.push_back(qid);
    let tid = match tid {
      Some(tid) => tid,
      None => return,
    };
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

  /// The coordinator's last word on a transaction. A rollback fails
  /// every query still out, with the error that caused it.
  fn end_transaction(
    &mut self,
    tid: TransId,
    ended: CommitState,
    error: Option<IcError>,
    gci: u64,
  ) {
    let sent: Vec<QueryId> = match self.transactions.get_mut(tid.0) {
      Some(trans) => {
        if trans.ended() {
          return;
        }
        trans.state = ended;
        trans.end_wanted = None;
        trans.error = error;
        trans.gci = gci;
        if ended == CommitState::RolledBack {
          trans.sent.clone()
        } else {
          Vec::new()
        }
      }
      None => return,
    };
    let why = match error {
      Some(e) => e,
      None => IcError::new(err::IC_ERROR_TRANSACTION_ROLLED_BACK),
    };
    for qid in sent {
      self.fail_query(qid, why);
    }
    self.release_if_done(tid);
  }

  /// Give the coordinator record back once the transaction is done.
  fn release_if_done(&mut self, tid: TransId) {
    let tc = match self.transactions.get(tid.0) {
      Some(trans) => {
        if !trans.is_done() {
          return;
        }
        trans.tc
      }
      None => return,
    };
    let mut i: usize = 0;
    while i < self.active.len() {
      if self.active[i].1 == tid {
        self.active.remove(i);
      } else {
        i += 1;
      }
    }
    self.free_tc_record(&tc);
  }

  /// Fail the transactions whose coordinator's link has gone, or been
  /// replaced, since they began: their outcome cannot be known here.
  pub(crate) fn fail_lost_transactions(&mut self) {
    let ids = self.active_transactions();
    for tid in ids {
      let (node_id, generation) = match self.transactions.get(tid.0) {
        Some(trans) => (trans.tc.node_id, trans.tc.generation),
        None => continue,
      };
      if self.link_is(node_id, generation) {
        continue;
      }
      let error = match self.shared.node(node_id) {
        Some(node) => node.not_connected_error(),
        None => IcError::new(err::IC_ERROR_LINK_LOST),
      };
      // The record went with the link.
      let tc = match self.transactions.get(tid.0) {
        Some(trans) => trans.tc,
        None => continue,
      };
      self.lose_tc_record(&tc);
      if let Some(trans) = self.transactions.get_mut(tid.0) {
        let defined = std::mem::take(&mut trans.defined);
        for qid in defined {
          if let Some(query) = self.queries.get_mut(qid.0) {
            query.fail(error);
          }
          self.executed.push_back(qid);
        }
      }
      self.end_transaction(tid, CommitState::RolledBack, Some(error), 0);
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_transaction_is_done_only_when_ended_and_empty() {
    let mut trans = Transaction {
      trans_id: 5,
      tc: TcRecord::for_test(1),
      state: CommitState::Started,
      begun: true,
      defined: Vec::new(),
      sent: vec![QueryId(PtrId::from_u32(1))],
      ops_sent: 1,
      ops_completed: 0,
      end_wanted: None,
      error: None,
      gci: 0,
    };
    assert!(!trans.is_done());
    trans.state = CommitState::Committed;
    assert!(!trans.is_done());
    trans.sent.clear();
    assert!(trans.is_done());
  }
}
