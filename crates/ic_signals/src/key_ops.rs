// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Key operations through the transaction coordinator: the request,
//! its replies, and the acknowledgement of a commit. The C documents the
//! request's flags under `NDB_PRIM_KEYREQ` in
//! `ic_apid_handle_messages.ic`; the words here are RonDB 26.10's.
//!
//! **The request, `KEY_OP_REQ`,** is sent in the long form: eight words,
//! the key in section 0 and the column data in section 1. The
//! words after the eighth are there only if their flags say so, in this
//! order: the user id and its version for rate limits, the scan
//! information, the distribution key. The coordinator refuses a long
//! request that is not exactly as long as its flags make it, as a
//! malicious signal. None of the optional words is used here.
//!
//! **Every reply names its receiver in its first word.** `KEY_OP_CONF`
//! names the transaction, by the pointer given when its record was
//! allocated, or is IC_NULL_PTR, and the transaction is then found from the
//! operations it lists. `KEY_OP_REF` and `ROW_DATA_REP` name the operation,
//! by the pointer the request gave it.
//!
//! **`KEY_OP_CONF`** comes from the coordinator and lists, per operation,
//! how many words of row data it will have. When that count has its top
//! bit set, the operation was a dirty read, and the low bits name the
//! node that reads it; one `ROW_DATA_REP` then completes it, whatever its
//! length. The global checkpoint of a commit is split: its high word
//! comes second, its low word after the operations, when present.
//!
//! **`ROW_DATA_REP`** comes from the node that read the row, which need
//! not be the coordinator's. Three words, our operation's pointer and
//! the transaction id, then the row: inline in a short signal, in
//! section 0 in a long one. A long one sent in fragments has a fourth
//! word, the row's whole length.
//!
//! **A commit with a marker is acknowledged** with
//! `COMMIT_MARKER_FREE_ORD`, the transaction id, to the block that sent
//! the `KEY_OP_CONF`, even when the confirmation is otherwise thrown
//! away. The marker counts only when the commit bit is set too.
//!
//! Verify: chapter 05 §17.11.

use ic_port::err;
use ic_port::IcError;

/// Words in a `KEY_OP_REQ` without optional words.
pub const IC_KEY_OP_REQ_LEN: usize = 8;
/// Section of a `KEY_OP_REQ` holding the key.
pub const IC_KEY_OP_REQ_KEY_SECTION: usize = 0;
/// Section of a `KEY_OP_REQ` holding the column data.
pub const IC_KEY_OP_REQ_COLUMN_SECTION: usize = 1;

// Operation types.
/// Read.
pub const IC_OP_READ: u32 = 0;
/// Update an existing row.
pub const IC_OP_UPDATE: u32 = 1;
/// Insert a new row.
pub const IC_OP_INSERT: u32 = 2;
/// Delete a row.
pub const IC_OP_DELETE: u32 = 3;
/// Insert, or update if the row exists.
pub const IC_OP_WRITE: u32 = 4;
/// Read with an exclusive lock.
pub const IC_OP_READ_EXCLUSIVE: u32 = 5;

// Abort options.
/// A failed operation aborts the transaction.
pub const IC_ABORT_ON_ERROR: u32 = 0;
/// A failed operation is reported, and the transaction goes on.
pub const IC_IGNORE_ERROR: u32 = 2;

// Request flags, as bit positions in the request information word.
const IC_KEY_OP_DIRTY_SHIFT: u32 = 0;
const IC_KEY_OP_NO_DISK_SHIFT: u32 = 1;
const IC_KEY_OP_COMMIT_SHIFT: u32 = 4;
const IC_KEY_OP_OPERATION_SHIFT: u32 = 5;
const IC_KEY_OP_OPERATION_MASK: u32 = 7;
const IC_KEY_OP_SIMPLE_SHIFT: u32 = 8;
const IC_KEY_OP_EXECUTE_SHIFT: u32 = 10;
const IC_KEY_OP_START_SHIFT: u32 = 11;
const IC_KEY_OP_ABORT_SHIFT: u32 = 12;
const IC_KEY_OP_ABORT_MASK: u32 = 3;
const IC_KEY_OP_NO_WAIT_SHIFT: u32 = 21;
const IC_KEY_OP_BATCH_SAFE_SHIFT: u32 = 22;
const IC_KEY_OP_BATCH_UNSAFE_SHIFT: u32 = 23;

/// What a key request asks for, before it is made into the request
/// information word.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyOpFlags {
  /// One of the `IC_OP_*` values.
  pub operation: u32,
  /// The first operation of the transaction.
  pub start: bool,
  /// Commit after this operation.
  pub commit: bool,
  /// The last operation of what is sent now: execute it and those
  /// before it.
  pub execute: bool,
  /// A read that takes a lock only while it reads.
  pub simple: bool,
  /// A read of the last committed value, with no lock. Sent with
  /// `simple`, as the reference does.
  pub dirty: bool,
  /// The row is in memory only, so the disk need not be read.
  pub no_disk: bool,
  /// One of `IC_ABORT_ON_ERROR` or `IC_IGNORE_ERROR`.
  pub abort_option: u32,
  /// RonDB: fail rather than wait for a lock another transaction holds.
  pub no_wait: bool,
  /// RonDB: the operation does not depend on earlier ones in its batch.
  pub batch_safe: bool,
  /// RonDB: the operation depends on earlier ones in its batch.
  pub batch_unsafe: bool,
}

impl KeyOpFlags {
  /// The request information word.
  pub fn flags_word(&self) -> u32 {
    let mut info: u32 = 0;
    info |= (self.dirty as u32) << IC_KEY_OP_DIRTY_SHIFT;
    info |= (self.no_disk as u32) << IC_KEY_OP_NO_DISK_SHIFT;
    info |= (self.commit as u32) << IC_KEY_OP_COMMIT_SHIFT;
    info |=
      (self.operation & IC_KEY_OP_OPERATION_MASK) << IC_KEY_OP_OPERATION_SHIFT;
    info |= (self.simple as u32) << IC_KEY_OP_SIMPLE_SHIFT;
    info |= (self.execute as u32) << IC_KEY_OP_EXECUTE_SHIFT;
    info |= (self.start as u32) << IC_KEY_OP_START_SHIFT;
    info |= (self.abort_option & IC_KEY_OP_ABORT_MASK) << IC_KEY_OP_ABORT_SHIFT;
    info |= (self.no_wait as u32) << IC_KEY_OP_NO_WAIT_SHIFT;
    info |= (self.batch_safe as u32) << IC_KEY_OP_BATCH_SAFE_SHIFT;
    info |= (self.batch_unsafe as u32) << IC_KEY_OP_BATCH_UNSAFE_SHIFT;
    info
  }
}

/// A key operation, as sent to the coordinator.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyOpReq {
  /// The coordinator's pointer for the transaction record, from the
  /// allocate.
  pub coord_ptr: u32,
  /// Our pointer for the operation, which `KEY_OP_REF` and `ROW_DATA_REP`
  /// name it by.
  pub query_ref: u32,
  /// The table.
  pub table_id: u32,
  /// What to do; see [`KeyOpFlags`].
  pub flags_word: u32,
  /// The table version the operation was prepared for.
  pub table_version: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl KeyOpReq {
  /// The eight words of signal data. The column data length, third, is
  /// zero: in the long form it is the length of section 1.
  pub fn encode(&self) -> [u32; IC_KEY_OP_REQ_LEN] {
    [
      self.coord_ptr,
      self.query_ref,
      0,
      self.table_id,
      self.flags_word,
      self.table_version,
      self.trans_id_lo,
      self.trans_id_hi,
    ]
  }
}

/// Words in a `KEY_OP_CONF` before the operations.
pub const IC_KEY_OP_CONF_LEN: usize = 5;
/// Operations a `KEY_OP_CONF` decodes without allocating. The coordinator
/// sends at most three; a confirmation with more than this still
/// decodes, into a vector.
pub const IC_KEY_OP_CONF_INLINE_OPS: usize = 16;
/// In an operation's row length: a dirty read, the low bits naming the
/// node that reads it.
pub const IC_KEY_OP_CONF_DIRTY_READ_BIT: u32 = 1 << 31;
/// Where `KEY_OP_CONF` names no transaction.
pub const IC_KEY_OP_CONF_NO_TRANSACTION: u32 = 0xFFFF_FF00;

/// One operation in a `KEY_OP_CONF`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyOpResult {
  /// Our pointer for the operation.
  pub query_ref: u32,
  /// Words of row data it will have, or for a dirty read the dirty read
  /// bit and the reading node.
  pub row_len: u32,
}

impl KeyOpResult {
  /// True for a dirty read: one `ROW_DATA_REP` completes it.
  pub fn is_dirty_read(&self) -> bool {
    self.row_len > IC_KEY_OP_CONF_DIRTY_READ_BIT
  }

  /// For a dirty read, the node that reads the row.
  pub fn reading_node(&self) -> u32 {
    self.row_len & !IC_KEY_OP_CONF_DIRTY_READ_BIT
  }
}

/// The coordinator confirms operations.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KeyOpConf {
  /// Our pointer for the transaction, or
  /// [`IC_KEY_OP_CONF_NO_TRANSACTION`].
  pub client_ptr: u32,
  /// Commit flag, marker flag and operation count.
  pub info_word: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The operations confirmed, the first `num_operations` of them; see
  /// [`operations`](Self::operations). Kept inline, so that decoding a
  /// confirmation allocates nothing: the coordinator puts at most three
  /// in one. Verify: chapter 05 §17.11.
  pub ops: [KeyOpResult; IC_KEY_OP_CONF_INLINE_OPS],
  /// How many operations the confirmation carries.
  pub num_operations: usize,
  /// All the operations instead, for a confirmation with more than fit
  /// inline; empty, and so no allocation, otherwise.
  pub more_ops: Vec<KeyOpResult>,
  /// The global checkpoint the commit went into; 0 when there was none,
  /// as for a transaction of dirty reads only.
  pub gci: u64,
}

impl KeyOpConf {
  /// Read a confirmation.
  pub fn decode(data: &[u32]) -> Result<KeyOpConf, IcError> {
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if data.len() < IC_KEY_OP_CONF_LEN {
      return Err(bad);
    }
    let info_word = data[2];
    let count = (info_word & 0xFFFF) as usize;
    let end = IC_KEY_OP_CONF_LEN + 2 * count;
    if data.len() < end {
      return Err(bad);
    }
    let mut ops = [KeyOpResult::default(); IC_KEY_OP_CONF_INLINE_OPS];
    let mut more_ops: Vec<KeyOpResult> = Vec::new();
    if count > IC_KEY_OP_CONF_INLINE_OPS {
      more_ops.reserve_exact(count);
    }
    let mut i: usize = 0;
    while i < count {
      let op = KeyOpResult {
        query_ref: data[IC_KEY_OP_CONF_LEN + 2 * i],
        row_len: data[IC_KEY_OP_CONF_LEN + 2 * i + 1],
      };
      if count > IC_KEY_OP_CONF_INLINE_OPS {
        more_ops.push(op);
      } else {
        ops[i] = op;
      }
      i += 1;
    }
    let mut gci_lo: u32 = 0;
    if data.len() > end {
      gci_lo = data[end];
    }
    Ok(KeyOpConf {
      client_ptr: data[0],
      info_word,
      trans_id_lo: data[3],
      trans_id_hi: data[4],
      ops,
      num_operations: count,
      more_ops,
      gci: ((data[1] as u64) << 32) | gci_lo as u64,
    })
  }

  /// The operations confirmed.
  pub fn operations(&self) -> &[KeyOpResult] {
    if !self.more_ops.is_empty() {
      return &self.more_ops;
    }
    &self.ops[..self.num_operations]
  }

  /// True if the transaction committed.
  pub fn is_committed(&self) -> bool {
    (self.info_word >> 16) & 1 != 0
  }

  /// True if the commit must be acknowledged with `COMMIT_MARKER_FREE_ORD`.
  pub fn needs_commit_ack(&self) -> bool {
    let bits: u32 = 3 << 16;
    self.info_word & bits == bits
  }
}

/// Words in a `KEY_OP_REF`.
pub const IC_KEY_OP_REF_LEN: usize = 5;

/// The coordinator refuses an operation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyOpRef {
  /// Our pointer for the operation.
  pub query_ref: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The NDB error code.
  pub error_code: u32,
  /// More about the error, for some codes.
  pub error_data: u32,
}

impl KeyOpRef {
  /// Read a refusal. The last word may be missing.
  pub fn decode(data: &[u32]) -> Result<KeyOpRef, IcError> {
    if data.len() < IC_KEY_OP_REF_LEN - 1 {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let mut error_data: u32 = 0;
    if data.len() >= IC_KEY_OP_REF_LEN {
      error_data = data[4];
    }
    Ok(KeyOpRef {
      query_ref: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
      error_code: data[3],
      error_data,
    })
  }
}

/// Words in a `COMMIT_REQ` and a `ROLLBACK_REQ`: the coordinator's
/// own pointer for the transaction record, as `COORD_ALLOC_CONF` gave it,
/// and the transaction id. The coordinator indexes its records by the
/// first word and checks the id against the record; a wrong pointer
/// gets no reply at all. The reference puts the coordinator's pointer
/// first too. Verify: chapter 05 §17.11.
pub const IC_TRANS_END_REQ_LEN: usize = 3;

/// Ask the coordinator to commit, or to roll back, a transaction with
/// nothing left to send: the request names the transaction by the
/// coordinator's pointer.
pub fn trans_end_req(
  coord_ptr: u32,
  trans_id_lo: u32,
  trans_id_hi: u32,
) -> [u32; IC_TRANS_END_REQ_LEN] {
  [coord_ptr, trans_id_lo, trans_id_hi]
}

/// Words in a `COMMIT_CONF`.
pub const IC_COMMIT_CONF_LEN: usize = 5;

/// The coordinator has committed a transaction asked to commit on its
/// own. The pointer's low bit is set when a commit marker was kept.
/// Verify: chapter 05 §17.11.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransCommitConf {
  /// Our pointer for the transaction, its low bit cleared.
  pub client_ptr: u32,
  /// True if the commit must be acknowledged with `COMMIT_MARKER_FREE_ORD`.
  pub needs_commit_ack: bool,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The global checkpoint the commit went into.
  pub gci: u64,
}

impl TransCommitConf {
  /// Read a commit confirmation.
  pub fn decode(data: &[u32]) -> Result<TransCommitConf, IcError> {
    if data.len() < IC_COMMIT_CONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TransCommitConf {
      client_ptr: data[0] & !1,
      needs_commit_ack: data[0] & 1 != 0,
      trans_id_lo: data[1],
      trans_id_hi: data[2],
      gci: ((data[3] as u64) << 32) | data[4] as u64,
    })
  }
}

/// Words in a `COMMIT_REF`, and the least in a `ROLLBACK_REF`, which
/// has a fifth: the coordinator's state.
pub const IC_TRANS_END_REF_LEN: usize = 4;

/// The coordinator refuses to commit, or to roll back.
/// Verify: chapter 05 §17.11.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransEndRef {
  /// Our pointer for the transaction.
  pub client_ptr: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The NDB error code.
  pub error_code: u32,
}

impl TransEndRef {
  /// Read a refusal.
  pub fn decode(data: &[u32]) -> Result<TransEndRef, IcError> {
    if data.len() < IC_TRANS_END_REF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TransEndRef {
      client_ptr: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
      error_code: data[3],
    })
  }
}

/// Words in a `ROLLBACK_CONF`: our pointer for the transaction and its
/// id. Verify: chapter 05 §17.11.
pub const IC_ROLLBACK_CONF_LEN: usize = 3;

/// The front of a `ROLLBACK_CONF`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RollbackConf {
  /// Our pointer for the transaction.
  pub client_ptr: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl RollbackConf {
  /// Read a rollback confirmation.
  pub fn decode(data: &[u32]) -> Result<RollbackConf, IcError> {
    if data.len() < IC_ROLLBACK_CONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(RollbackConf {
      client_ptr: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
    })
  }
}

/// Words in a `TAKEOVER_CONF` and a `TAKEOVER_REF`.
pub const IC_TAKEOVER_LEN: usize = 3;

/// The coordinator that took over from a failed one has committed the
/// transaction. Its low bit asks for `COMMIT_MARKER_FREE_ORD`, to the sender.
/// Verify: chapter 05 §17.11.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TakeoverConf {
  /// Our pointer for the transaction, its low bit cleared.
  pub client_ptr: u32,
  /// True if the commit must be acknowledged with `COMMIT_MARKER_FREE_ORD`.
  pub needs_commit_ack: bool,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl TakeoverConf {
  /// Read a takeover commit confirmation.
  pub fn decode(data: &[u32]) -> Result<TakeoverConf, IcError> {
    if data.len() < IC_TAKEOVER_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TakeoverConf {
      client_ptr: data[0] & !1,
      needs_commit_ack: data[0] & 1 != 0,
      trans_id_lo: data[1],
      trans_id_hi: data[2],
    })
  }
}

/// The coordinator that took over from a failed one has aborted the
/// transaction: our pointer for it and its id, three words.
/// Verify: chapter 05 §17.11.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TakeoverRef {
  /// Our pointer for the transaction.
  pub client_ptr: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl TakeoverRef {
  /// Read a takeover abort.
  pub fn decode(data: &[u32]) -> Result<TakeoverRef, IcError> {
    if data.len() < IC_TAKEOVER_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TakeoverRef {
      client_ptr: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
    })
  }
}

/// Words in a `TRANS_ABORTED_REP`.
pub const IC_TRANS_ABORTED_REP_LEN: usize = 5;

/// The coordinator has rolled the transaction back. It names the
/// transaction, not the operation, and carries the error that caused
/// it. Verify: chapter 05 §17.11.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransAbortedRep {
  /// Our pointer for the transaction.
  pub client_ptr: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The NDB error that rolled it back.
  pub error_code: u32,
  /// More about the error, for some codes.
  pub error_data: u32,
}

impl TransAbortedRep {
  /// Read a rollback report. The last word may be missing.
  pub fn decode(data: &[u32]) -> Result<TransAbortedRep, IcError> {
    if data.len() < IC_TRANS_ABORTED_REP_LEN - 1 {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let mut error_data: u32 = 0;
    if data.len() >= IC_TRANS_ABORTED_REP_LEN {
      error_data = data[4];
    }
    Ok(TransAbortedRep {
      client_ptr: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
      error_code: data[3],
      error_data,
    })
  }
}

/// Words in front of the row in a `ROW_DATA_REP`.
pub const IC_ROW_DATA_REP_HEADER_LEN: usize = 3;

/// The front of a `ROW_DATA_REP`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RowDataRep {
  /// Our pointer for the operation.
  pub query_ref: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl RowDataRep {
  /// Read the front of a `ROW_DATA_REP`.
  pub fn decode(data: &[u32]) -> Result<RowDataRep, IcError> {
    if data.len() < IC_ROW_DATA_REP_HEADER_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(RowDataRep {
      query_ref: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
    })
  }

  /// The row: section 0 of a long signal, or what follows the header in
  /// a short one.
  pub fn row<'a>(data: &'a [u32], section0: Option<&'a [u32]>) -> &'a [u32] {
    if let Some(section) = section0 {
      return section;
    }
    if data.len() <= IC_ROW_DATA_REP_HEADER_LEN {
      return &[];
    }
    &data[IC_ROW_DATA_REP_HEADER_LEN..]
  }
}

/// The acknowledgement of a commit with a marker: the transaction id.
pub fn commit_marker_free_ord(trans_id_lo: u32, trans_id_hi: u32) -> [u32; 2] {
  [trans_id_lo, trans_id_hi]
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_committed_read_sets_its_bits() {
    let flags = KeyOpFlags {
      operation: IC_OP_READ,
      start: true,
      commit: true,
      execute: true,
      simple: true,
      dirty: true,
      no_disk: false,
      abort_option: IC_IGNORE_ERROR,
      ..KeyOpFlags::default()
    };
    // Dirty 0, commit 4, simple 8, execute 10, start 11, abort 12-13.
    let expect = 1 | (1 << 4) | (1 << 8) | (1 << 10) | (1 << 11) | (2 << 12);
    assert_eq!(flags.flags_word(), expect);
  }

  #[test]
  fn the_operation_type_goes_in_bits_five_to_seven() {
    let flags = KeyOpFlags {
      operation: IC_OP_DELETE,
      ..KeyOpFlags::default()
    };
    assert_eq!(flags.flags_word(), 3 << 5);
  }

  #[test]
  fn a_request_is_eight_words_with_no_attribute_length() {
    let req = KeyOpReq {
      coord_ptr: 11,
      query_ref: 22,
      table_id: 15,
      flags_word: 0x0D11,
      table_version: 3,
      trans_id_lo: 0x1000,
      trans_id_hi: 0x2000,
    };
    assert_eq!(req.encode(), [11, 22, 0, 15, 0x0D11, 3, 0x1000, 0x2000]);
  }

  #[test]
  fn a_confirmation_lists_its_operations_and_gci() {
    // Committed with marker, two operations, then gci_lo.
    let info_word = (3 << 16) | 2;
    let data = [7, 0x10, info_word, 0x1000, 0x2000, 22, 4, 23, 0, 0x20];
    let conf = KeyOpConf::decode(&data).expect("read");
    assert_eq!(conf.client_ptr, 7);
    assert!(conf.is_committed());
    assert!(conf.needs_commit_ack());
    assert_eq!(conf.operations().len(), 2);
    assert_eq!(conf.operations()[0].query_ref, 22);
    assert_eq!(conf.operations()[0].row_len, 4);
    assert_eq!(conf.gci, (0x10 << 32) | 0x20);
  }

  #[test]
  fn a_confirmation_with_many_operations_still_decodes() {
    let count: u32 = IC_KEY_OP_CONF_INLINE_OPS as u32 + 3;
    let mut data: Vec<u32> = vec![7, 0, count, 0x10, 0x20];
    let mut i: u32 = 0;
    while i < count {
      data.push(100 + i);
      data.push(i);
      i += 1;
    }
    let conf = KeyOpConf::decode(&data).expect("decoded");
    assert_eq!(conf.operations().len(), count as usize);
    assert_eq!(conf.operations()[0].query_ref, 100);
    let last = conf.operations()[count as usize - 1];
    assert_eq!(last.query_ref, 100 + count - 1);
    assert_eq!(last.row_len, count - 1);
  }

  #[test]
  fn a_marker_without_commit_needs_no_ack() {
    let data = [7, 0, 2 << 16, 0x1000, 0x2000];
    let conf = KeyOpConf::decode(&data).expect("read");
    assert!(!conf.is_committed());
    assert!(!conf.needs_commit_ack());
    assert_eq!(conf.gci, 0);
  }

  #[test]
  fn a_dirty_read_names_its_reading_node() {
    let op = KeyOpResult {
      query_ref: 22,
      row_len: IC_KEY_OP_CONF_DIRTY_READ_BIT | 2,
    };
    assert!(op.is_dirty_read());
    assert_eq!(op.reading_node(), 2);
    let plain = KeyOpResult {
      query_ref: 22,
      row_len: 4,
    };
    assert!(!plain.is_dirty_read());
  }

  #[test]
  fn a_short_confirmation_is_an_error() {
    // Says two operations, carries one.
    let data = [7, 0, 2, 0x1000, 0x2000, 22, 4];
    assert!(KeyOpConf::decode(&data).is_err());
  }

  #[test]
  fn a_refusal_is_read_with_or_without_its_last_word() {
    let full = KeyOpRef::decode(&[22, 0x1000, 0x2000, 626, 9]).expect("full");
    assert_eq!(full.query_ref, 22);
    assert_eq!(full.error_code, 626);
    assert_eq!(full.error_data, 9);
    let short = KeyOpRef::decode(&[22, 0x1000, 0x2000, 626]).expect("short");
    assert_eq!(short.error_data, 0);
  }

  #[test]
  fn the_rondb_flags_take_their_bits() {
    let flags = KeyOpFlags {
      no_wait: true,
      batch_safe: true,
      ..KeyOpFlags::default()
    };
    assert_eq!(flags.flags_word(), (1 << 21) | (1 << 22));
  }

  #[test]
  fn a_commit_confirmation_says_whether_to_acknowledge() {
    let conf = TransCommitConf::decode(&[8 | 1, 0x1000, 0x2000, 3, 4]);
    let conf = conf.expect("read");
    assert_eq!(conf.client_ptr, 8);
    assert!(conf.needs_commit_ack);
    assert_eq!(conf.gci, (3 << 32) | 4);
    let plain = TransCommitConf::decode(&[8, 0x1000, 0x2000, 0, 0]);
    assert!(!plain.expect("read").needs_commit_ack);
    assert_eq!(trans_end_req(8, 0x1000, 0x2000), [8, 0x1000, 0x2000]);
    let refused = TransEndRef::decode(&[8, 0x1000, 0x2000, 4350, 9]);
    assert_eq!(refused.expect("read").error_code, 4350);
    let rolled = RollbackConf::decode(&[8, 0x1000, 0x2000]);
    assert_eq!(rolled.expect("read").client_ptr, 8);
  }

  #[test]
  fn the_takeover_replies_name_the_transaction() {
    let conf = TakeoverConf::decode(&[8 | 1, 0x1000, 0x2000]).expect("read");
    assert_eq!(conf.client_ptr, 8);
    assert!(conf.needs_commit_ack);
    let refused = TakeoverRef::decode(&[8, 0x1000, 0x2000]).expect("read");
    assert_eq!(refused.trans_id_hi, 0x2000);
    assert!(TakeoverRef::decode(&[8, 0x1000]).is_err());
  }

  #[test]
  fn a_rollback_report_carries_the_error() {
    let rep = TransAbortedRep::decode(&[7, 0x1000, 0x2000, 899, 0]);
    let rep = rep.expect("read");
    assert_eq!(rep.client_ptr, 7);
    assert_eq!(rep.error_code, 899);
    let short = TransAbortedRep::decode(&[7, 0x1000, 0x2000, 899]);
    assert_eq!(short.expect("read").error_data, 0);
  }

  #[test]
  fn a_row_comes_inline_or_in_its_section() {
    let short = [22, 0x1000, 0x2000, (1 << 16) | 4, 42];
    let front = RowDataRep::decode(&short).expect("front");
    assert_eq!(front.query_ref, 22);
    assert_eq!(RowDataRep::row(&short, None), &[(1 << 16) | 4, 42]);
    let long = [22, 0x1000, 0x2000];
    let section = [(1 << 16) | 4, 42];
    assert_eq!(RowDataRep::row(&long, Some(&section[..])), &section);
  }
}
