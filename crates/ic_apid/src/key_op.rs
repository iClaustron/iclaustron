// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The first key operations: reading, inserting, updating and deleting
//! one row by its primary key, each as one transaction of its own,
//! waited for before returning.
//!
//! This is a stepping stone to the transactions of chapter 04, where a
//! thread defines many operations, sends them together and collects
//! their outcomes as they complete. It uses the same pieces: a
//! transaction record at the coordinator, a transaction id, the key and
//! the packed read of `row_codec`, and replies gathered by the
//! connection from whichever node sends them.
//!
//! Every operation is sent with the start, commit and execute flags: it
//! is the whole transaction. A committed read is besides simple and
//! dirty, and ignores errors, which is the only abort option the
//! reference allows it; a write aborts on error. What comes back:
//!
//! - `TCKEYCONF` from the coordinator, naming the transaction record,
//!   with the operation marked as a dirty read and the node that reads
//!   it. One `TRANSID_AI` then completes it.
//! - `TRANSID_AI` from the reading node, naming the operation, with the
//!   row packed.
//! - For a write, `TCKEYCONF` with no row to expect is the whole
//!   outcome.
//! - Or `TCKEYREF`, naming the operation: 626 says there is no such
//!   row, 630 that one is already there.
//! - Or `TCROLLBACKREP`, naming the transaction, when the coordinator
//!   rolls it back: some failures are reported only this way, so the
//!   transaction waits for it as well as for the confirmation.
//!
//! The two may come in either order, and either may come packed with
//! others; the receive thread has taken those apart.
//!
//! Verify: `NdbOperationDefine.cpp`, the lock mode switch that makes a
//! committed read simple and dirty; `NdbOperationExec.cpp`,
//! `prepareSendNdbRecord`, which makes a simple read ignore errors;
//! `NdbTransaction.cpp`, `receiveTCKEYCONF`; `NdbReceiver.cpp`,
//! `execTRANSID_AI`.

use ic_ndb_signals::attr_header;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::tc_key;
use ic_ndb_signals::tc_key::TcKeyConf;
use ic_ndb_signals::tc_key::TcKeyFlags;
use ic_ndb_signals::tc_key::TcKeyRef;
use ic_ndb_signals::tc_key::TcKeyReq;
use ic_ndb_signals::tc_key::TcRollbackRep;
use ic_ndb_signals::tc_key::TransIdAi;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;

use std::sync::Arc;

use crate::apid_conn::ApidConnection;
use crate::apid_conn::TcRecord;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::hash;
use crate::node_connect::ReceivedSignal;
use crate::query::QueryId;
use crate::query::ReadKeyArgs;
use crate::query::ReadKind;
use crate::query::TransId;
use crate::record::Record;
use crate::row_codec;
use crate::transaction::TransactionHint;

/// How long a key operation waits for its outcome.
pub const IC_KEY_OP_WAIT_MS: u32 = 10_000;
/// How long it sleeps on the inbox at a time.
const IC_KEY_OP_SLICE_MS: u32 = 100;
/// The NDB error for a key with no row.
pub const IC_NDB_ERROR_NO_SUCH_ROW: u32 = 626;
/// The NDB error for a key that already has a row.
pub const IC_NDB_ERROR_ROW_EXISTS: u32 = 630;

/// What has come back for the operation so far.
#[derive(Default)]
struct Outcome {
  /// The confirmation's word for the operation, once it has come.
  confirmed: Option<tc_key::OperationConf>,
  /// The row, as it came.
  row: Vec<u32>,
  /// True once a `TRANSID_AI` has come.
  has_row: bool,
  /// The refusal's error code, if refused.
  refused: Option<u32>,
}

impl Outcome {
  /// True when nothing more is to come.
  fn is_complete(&self) -> bool {
    if self.refused.is_some() {
      return true;
    }
    let conf = match self.confirmed {
      Some(conf) => conf,
      None => return false,
    };
    if conf.is_dirty_read() {
      return self.has_row;
    }
    self.row.len() as u32 >= conf.row_len
  }
}

/// Read the row whose key is in `key_row`, as `key_rec` lays it out,
/// into `attr_row`, as `attr_rec` lays it out. Every field of the
/// attribute record is read, and its null bit set or cleared. Returns
/// false if there is no such row.
pub fn read_committed(
  conn: &mut ApidConnection,
  key_rec: &Record,
  key_row: &[u8],
  attr_rec: &Record,
  attr_row: &mut [u8],
) -> Result<bool, IcError> {
  let table = key_rec.table();
  let same_table = table.table_id() == attr_rec.table().table_id()
    && table.table_version() == attr_rec.table().table_version();
  if !same_table || !key_rec.covers_primary_key() {
    return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
  }
  let key = row_codec::key_info(key_rec, key_row)?;
  let attr_info = row_codec::read_attr_info(attr_rec);
  let flags = TcKeyFlags {
    operation: tc_key::IC_OP_READ,
    start: true,
    commit: true,
    execute: true,
    simple: true,
    dirty: true,
    no_disk: false,
    abort_option: tc_key::IC_IGNORE_ERROR,
    ..TcKeyFlags::default()
  };
  let node_id = node_for_key(conn, table, key_rec, key_row)?;
  let outcome = run_op(conn, table, node_id, &key, &attr_info, &flags)?;
  if let Some(code) = outcome.refused {
    if code == IC_NDB_ERROR_NO_SUCH_ROW {
      return Ok(false);
    }
    return Err(IcError::new(code as i32));
  }
  row_codec::unpack_row(attr_rec, &outcome.row, attr_row)?;
  Ok(true)
}

/// The node to send an operation on this key to: one holding the row
/// when the key's partition can be worked out and a node holding it is
/// started, otherwise the next started node in turn.
fn node_for_key(
  conn: &mut ApidConnection,
  table: &TableDef,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<u32, IcError> {
  let started = conn.started_nodes();
  if started.is_empty() {
    return Err(IcError::new(err::IC_ERROR_NO_STARTED_DATA_NODE));
  }
  let turn = conn.next_request_id();
  if let Ok(partition) = hash::partition_of(table, key_rec, key_row) {
    if let Some(node) = hash::choose_node(table, partition, &started, turn) {
      return Ok(node);
    }
  }
  Ok(started[turn as usize % started.len()])
}

/// Read the row a unique index key names, as a committed read through
/// the index, into `attr_row`; `key_row` is laid out by `key_rec` over
/// the index's table and holds the indexed columns. Returns false if
/// there is no such row. This one is built on the transaction API,
/// which the reads and writes above will move onto too.
pub fn read_unique_committed(
  conn: &mut ApidConnection,
  index: &Arc<IndexDef>,
  key_rec: &Record,
  key_row: &[u8],
  attr_rec: &Record,
  attr_row: &mut [u8],
) -> Result<bool, IcError> {
  let key_len = key_rec.row_size() as usize;
  let attr_len = attr_rec.row_size() as usize;
  if key_row.len() < key_len || attr_row.len() < attr_len {
    return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
  }
  let qid = conn.create_unique_query(index, key_rec, attr_rec)?;
  let result = unique_read(conn, qid, &key_row[..key_len], attr_row);
  let _ = conn.free_query(qid);
  result
}

/// The transaction of one committed read, waited for.
fn unique_read(
  conn: &mut ApidConnection,
  qid: QueryId,
  key_row: &[u8],
  attr_row: &mut [u8],
) -> Result<bool, IcError> {
  let tid = conn.start_transaction(TransactionHint::Any, None)?;
  if let Some(query) = conn.query_mut(qid) {
    query.key_row_mut().copy_from_slice(key_row);
  }
  let args = ReadKeyArgs {
    kind: ReadKind::Committed,
    ..ReadKeyArgs::default()
  };
  let result = match conn.read_key(qid, tid, &args) {
    Ok(()) => match conn.commit_transaction(tid) {
      Ok(()) => wait_for_transaction(conn, tid),
      Err(e) => Err(e),
    },
    Err(e) => Err(e),
  };
  let _ = conn.close_transaction(tid);
  result?;
  // Ours is the only query on this connection's executed list here.
  let mut found: Option<bool> = None;
  while let Some(done) = conn.get_next_executed_query() {
    if done != qid {
      continue;
    }
    let query = match conn.query(qid) {
      Some(query) => query,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    match query.error() {
      Some(e) if e.code == IC_NDB_ERROR_NO_SUCH_ROW as i32 => {
        found = Some(false);
      }
      Some(e) => return Err(e),
      None => {
        let len = query.attr_row().len();
        attr_row[..len].copy_from_slice(query.attr_row());
        found = Some(true);
      }
    }
  }
  match found {
    Some(found) => Ok(found),
    None => Err(IcError::new(err::IC_ERROR_TIMEOUT)),
  }
}

/// Send, then poll until the transaction is done or the wait runs out.
fn wait_for_transaction(
  conn: &mut ApidConnection,
  tid: TransId,
) -> Result<(), IcError> {
  let start = ic_port::time::gethrtime();
  loop {
    conn.flush(IC_KEY_OP_SLICE_MS, true)?;
    if let Some(trans) = conn.transaction(tid) {
      if trans.is_done() {
        return Ok(());
      }
    }
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if waited >= IC_KEY_OP_WAIT_MS as u64 {
      return Err(IcError::new(err::IC_ERROR_TIMEOUT));
    }
  }
}

/// The partition the row with this key is in, read from the data node
/// through the `FRAGMENT` pseudo column, or `None` if there is no such
/// row. This is what `hash::partition_of` is checked against.
pub fn read_partition(
  conn: &mut ApidConnection,
  key_rec: &Record,
  key_row: &[u8],
) -> Result<Option<u32>, IcError> {
  if !key_rec.covers_primary_key() {
    return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
  }
  let key = row_codec::key_info(key_rec, key_row)?;
  let attr_info = [attr_header::attr_header(row_codec::IC_ATTR_FRAGMENT, 0)];
  let flags = TcKeyFlags {
    operation: tc_key::IC_OP_READ,
    start: true,
    commit: true,
    execute: true,
    simple: true,
    dirty: true,
    no_disk: false,
    abort_option: tc_key::IC_IGNORE_ERROR,
    ..TcKeyFlags::default()
  };
  let table = key_rec.table();
  let node_id = node_for_key(conn, table, key_rec, key_row)?;
  let outcome = run_op(conn, table, node_id, &key, &attr_info, &flags)?;
  if let Some(code) = outcome.refused {
    if code == IC_NDB_ERROR_NO_SUCH_ROW {
      return Ok(None);
    }
    return Err(IcError::new(code as i32));
  }
  // A header naming the pseudo column, then the partition.
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  if outcome.row.len() < 2 {
    return Err(bad);
  }
  let header = outcome.row[0];
  if attr_header::attribute_id(header) != row_codec::IC_ATTR_FRAGMENT
    || attr_header::byte_size(header) != 4
  {
    return Err(bad);
  }
  Ok(Some(outcome.row[1]))
}

/// Insert, update, delete or write the row whose key is in `key_row`.
/// `operation` is one of the `IC_OP_*` values of `tc_key`. Every field
/// of the attribute record is written, except the key columns of an
/// update, which may not change; a delete sends no values at all, and
/// then `attr_row` is not read.
///
/// The NDB error comes back as it is: 626 for a row that is not there,
/// 630 for one that already is.
pub fn write_key(
  conn: &mut ApidConnection,
  operation: u32,
  key_rec: &Record,
  key_row: &[u8],
  attr_rec: &Record,
  attr_row: &[u8],
) -> Result<(), IcError> {
  let table = key_rec.table();
  let same_table = table.table_id() == attr_rec.table().table_id()
    && table.table_version() == attr_rec.table().table_version();
  if !same_table || !key_rec.covers_primary_key() {
    return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
  }
  let key = row_codec::key_info(key_rec, key_row)?;
  let mut attr_info: Vec<u32> = Vec::new();
  if operation != tc_key::IC_OP_DELETE {
    let skip_keys = operation == tc_key::IC_OP_UPDATE;
    attr_info = row_codec::write_attr_info(attr_rec, attr_row, skip_keys)?;
  }
  let flags = TcKeyFlags {
    operation,
    start: true,
    commit: true,
    execute: true,
    simple: false,
    dirty: false,
    no_disk: false,
    abort_option: tc_key::IC_ABORT_ON_ERROR,
    ..TcKeyFlags::default()
  };
  let node_id = node_for_key(conn, table, key_rec, key_row)?;
  let outcome = run_op(conn, table, node_id, &key, &attr_info, &flags)?;
  match outcome.refused {
    Some(code) => Err(IcError::new(code as i32)),
    None => Ok(()),
  }
}

/// Send one operation as a whole transaction to `node_id`'s
/// coordinator and wait for its outcome. A request with no values to
/// send carries the key section alone, as a delete does.
fn run_op(
  conn: &mut ApidConnection,
  table: &TableDef,
  node_id: u32,
  key: &[u32],
  attr_info: &[u32],
  flags: &TcKeyFlags,
) -> Result<Outcome, IcError> {
  let rec = conn.tc_record(node_id)?;
  let op_id = conn.next_request_id();
  let trans_id = conn.next_transaction_id();
  let req = TcKeyReq {
    tc_connect_ptr: rec.tc_ptr,
    api_operation_ptr: op_id,
    table_id: table.table_id(),
    request_info: flags.request_info(),
    table_version: table.table_version(),
    trans_id1: trans_id as u32,
    trans_id2: (trans_id >> 32) as u32,
  };
  let op_replies = [gsn::IC_GSN_TCKEYREF, gsn::IC_GSN_TRANSID_AI];
  conn.expect_several(op_id, rec.node_id, &op_replies, true)?;
  let trans_replies = [gsn::IC_GSN_TCKEYCONF, gsn::IC_GSN_TCROLLBACKREP];
  conn.expect_several(rec.api_ptr, rec.node_id, &trans_replies, false)?;
  let header =
    SignalHeader::new(gsn::IC_GSN_TCKEYREQ, conn.block_number(), rec.tc_block);
  let both: [&[u32]; 2] = [key, attr_info];
  let mut sections: &[&[u32]] = &both;
  if attr_info.is_empty() {
    sections = &both[..1];
  }
  let result = match conn.send(rec.node_id, &header, &req.encode(), sections) {
    Ok(()) => wait_for_outcome(conn, &rec, op_id, &req),
    Err(e) => Err(e),
  };
  conn.forget(op_id);
  conn.forget(rec.api_ptr);
  match result {
    Ok(outcome) => {
      conn.free_tc_record(&rec);
      Ok(outcome)
    }
    Err(e) => {
      // The record's transaction may still be open at the coordinator.
      conn.lose_tc_record(&rec);
      Err(e)
    }
  }
}

/// Poll until the operation is complete, or the link goes, or the wait
/// runs out.
fn wait_for_outcome(
  conn: &mut ApidConnection,
  rec: &TcRecord,
  op_id: u32,
  req: &TcKeyReq,
) -> Result<Outcome, IcError> {
  let mut outcome = Outcome::default();
  let start = ic_port::time::gethrtime();
  loop {
    for signal in conn.take_replies(op_id)? {
      take_op_reply(&mut outcome, &signal, req)?;
    }
    for signal in conn.take_replies(rec.api_ptr)? {
      take_trans_reply(conn, &mut outcome, &signal, op_id, req)?;
    }
    if outcome.is_complete() {
      return Ok(outcome);
    }
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if waited >= IC_KEY_OP_WAIT_MS as u64 {
      return Err(IcError::new(err::IC_ERROR_TIMEOUT));
    }
    let mut slice = IC_KEY_OP_WAIT_MS - waited as u32;
    if slice > IC_KEY_OP_SLICE_MS {
      slice = IC_KEY_OP_SLICE_MS;
    }
    conn.poll(slice);
  }
}

/// A row or a refusal for the operation.
fn take_op_reply(
  outcome: &mut Outcome,
  signal: &ReceivedSignal,
  req: &TcKeyReq,
) -> Result<(), IcError> {
  if signal.gsn == gsn::IC_GSN_TCKEYREF {
    let refusal = TcKeyRef::decode(&signal.data)?;
    if same_transaction(req, refusal.trans_id1, refusal.trans_id2) {
      outcome.refused = Some(refusal.error_code);
    }
    return Ok(());
  }
  let front = TransIdAi::decode(&signal.data)?;
  if !same_transaction(req, front.trans_id1, front.trans_id2) {
    return Ok(());
  }
  let mut section: Option<&[u32]> = None;
  if !signal.sections.is_empty() {
    section = Some(signal.section(0));
  }
  outcome
    .row
    .extend_from_slice(TransIdAi::row(&signal.data, section));
  outcome.has_row = true;
  Ok(())
}

/// What the coordinator says of the transaction: its confirmation, or
/// its rollback.
fn take_trans_reply(
  conn: &mut ApidConnection,
  outcome: &mut Outcome,
  signal: &ReceivedSignal,
  op_id: u32,
  req: &TcKeyReq,
) -> Result<(), IcError> {
  if signal.gsn == gsn::IC_GSN_TCROLLBACKREP {
    let rollback = TcRollbackRep::decode(&signal.data)?;
    if same_transaction(req, rollback.trans_id1, rollback.trans_id2) {
      outcome.refused = Some(rollback.error_code);
    }
    return Ok(());
  }
  take_conf(conn, outcome, signal, op_id, req)
}

/// The coordinator's confirmation: note what it says of the operation,
/// and acknowledge a commit that asks for it.
fn take_conf(
  conn: &mut ApidConnection,
  outcome: &mut Outcome,
  signal: &ReceivedSignal,
  op_id: u32,
  req: &TcKeyReq,
) -> Result<(), IcError> {
  let conf = TcKeyConf::decode(&signal.data)?;
  if !same_transaction(req, conf.trans_id1, conf.trans_id2) {
    return Ok(());
  }
  if conf.needs_commit_ack() {
    let ack = tc_key::tc_commit_ack(conf.trans_id1, conf.trans_id2);
    let header = SignalHeader::new(
      gsn::IC_GSN_TC_COMMIT_ACK,
      conn.block_number(),
      signal.sender_block,
    );
    let queued = conn.queue_signal(signal.sender_node_id, &header, &ack, &[]);
    if let Err(e) = queued {
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "TC_COMMIT_ACK to node {} not queued: {}",
        signal.sender_node_id,
        e.message()
      );
    }
  }
  for op in &conf.operations {
    if op.api_operation_ptr == op_id {
      outcome.confirmed = Some(*op);
    }
  }
  Ok(())
}

fn same_transaction(req: &TcKeyReq, trans_id1: u32, trans_id2: u32) -> bool {
  req.trans_id1 == trans_id1 && req.trans_id2 == trans_id2
}

#[cfg(test)]
mod tests {
  use super::*;

  fn conf(row_len: u32) -> Option<tc_key::OperationConf> {
    Some(tc_key::OperationConf {
      api_operation_ptr: 22,
      row_len,
    })
  }

  #[test]
  fn a_dirty_read_is_complete_with_its_row_and_confirmation() {
    let mut outcome = Outcome::default();
    assert!(!outcome.is_complete());
    outcome.has_row = true;
    outcome.row = vec![1, 2, 3];
    // The row alone is not enough: the confirmation may still come.
    assert!(!outcome.is_complete());
    outcome.confirmed = conf(tc_key::IC_TCKEYCONF_DIRTY_READ_BIT | 2);
    assert!(outcome.is_complete());
  }

  #[test]
  fn a_plain_read_is_complete_when_its_words_have_come() {
    let mut outcome = Outcome {
      confirmed: conf(4),
      ..Outcome::default()
    };
    outcome.row = vec![1, 2, 3];
    assert!(!outcome.is_complete());
    outcome.row.push(4);
    assert!(outcome.is_complete());
  }

  #[test]
  fn a_refusal_completes_it() {
    let outcome = Outcome {
      refused: Some(IC_NDB_ERROR_NO_SUCH_ROW),
      ..Outcome::default()
    };
    assert!(outcome.is_complete());
  }
}
