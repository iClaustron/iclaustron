// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The first key operation: a committed read of one row by its primary
//! key, as one transaction of its own, waited for before returning.
//!
//! This is a stepping stone to the transactions of chapter 04, where a
//! thread defines many operations, sends them together and collects
//! their outcomes as they complete. It uses the same pieces: a
//! transaction record at the coordinator, a transaction id, the key and
//! the packed read of `row_codec`, and replies gathered by the
//! connection from whichever node sends them.
//!
//! A committed read is sent with the start, commit and execute flags,
//! as a simple and dirty read that ignores errors, which is the only
//! abort option the reference allows it. What comes back:
//!
//! - `TCKEYCONF` from the coordinator, naming the transaction record,
//!   with the operation marked as a dirty read and the node that reads
//!   it. One `TRANSID_AI` then completes it.
//! - `TRANSID_AI` from the reading node, naming the operation, with the
//!   row packed.
//! - Or `TCKEYREF`, naming the operation: 626 says there is no such row.
//!
//! The two may come in either order, and either may come packed with
//! others; the receive thread has taken those apart.
//!
//! Verify: `NdbOperationDefine.cpp`, the lock mode switch that makes a
//! committed read simple and dirty; `NdbOperationExec.cpp`,
//! `prepareSendNdbRecord`, which makes a simple read ignore errors;
//! `NdbTransaction.cpp`, `receiveTCKEYCONF`; `NdbReceiver.cpp`,
//! `execTRANSID_AI`.

use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::tc_key;
use ic_ndb_signals::tc_key::TcKeyConf;
use ic_ndb_signals::tc_key::TcKeyFlags;
use ic_ndb_signals::tc_key::TcKeyRef;
use ic_ndb_signals::tc_key::TcKeyReq;
use ic_ndb_signals::tc_key::TransIdAi;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;

use crate::apid_conn::ApidConnection;
use crate::apid_conn::TcRecord;
use crate::node_connect::ReceivedSignal;
use crate::record::Record;
use crate::row_codec;

/// How long a key operation waits for its outcome.
pub const IC_KEY_OP_WAIT_MS: u32 = 10_000;
/// How long it sleeps on the inbox at a time.
const IC_KEY_OP_SLICE_MS: u32 = 100;
/// The NDB error for a key with no row.
pub const IC_NDB_ERROR_NO_SUCH_ROW: u32 = 626;

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
  let started = conn.started_nodes();
  if started.is_empty() {
    return Err(IcError::new(err::IC_ERROR_NO_STARTED_DATA_NODE));
  }
  // Any started node's coordinator will do until keys are hashed to
  // choose the node that holds the row; turn about meanwhile.
  let pick = conn.next_request_id() as usize % started.len();
  let rec = conn.tc_record(started[pick])?;
  let op_id = conn.next_request_id();
  let trans_id = conn.next_transaction_id();
  let flags = TcKeyFlags {
    operation: tc_key::IC_OP_READ,
    start: true,
    commit: true,
    execute: true,
    simple: true,
    dirty: true,
    no_disk: false,
    abort_option: tc_key::IC_IGNORE_ERROR,
  };
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
  conn.expect_several(
    rec.api_ptr,
    rec.node_id,
    &[gsn::IC_GSN_TCKEYCONF],
    false,
  )?;
  let header =
    SignalHeader::new(gsn::IC_GSN_TCKEYREQ, conn.block_number(), rec.tc_block);
  let sections: [&[u32]; 2] = [&key, &attr_info];
  let result = match conn.send(rec.node_id, &header, &req.encode(), &sections) {
    Ok(()) => wait_for_outcome(conn, &rec, op_id, &req),
    Err(e) => Err(e),
  };
  conn.forget(op_id);
  conn.forget(rec.api_ptr);
  let outcome = match result {
    Ok(outcome) => {
      conn.free_tc_record(&rec);
      outcome
    }
    Err(e) => {
      // The record's transaction may still be open at the coordinator.
      conn.lose_tc_record(&rec);
      return Err(e);
    }
  };
  if let Some(code) = outcome.refused {
    if code == IC_NDB_ERROR_NO_SUCH_ROW {
      return Ok(false);
    }
    return Err(IcError::new(code as i32));
  }
  row_codec::unpack_row(attr_rec, &outcome.row, attr_row)?;
  Ok(true)
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
      take_conf(conn, &mut outcome, &signal, op_id, req)?;
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
    if let Err(e) = conn.send(signal.sender_node_id, &header, &ack, &[]) {
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "TC_COMMIT_ACK to node {} not sent: {}",
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
