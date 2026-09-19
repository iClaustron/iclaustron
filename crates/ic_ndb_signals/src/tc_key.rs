// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Key operations through the transaction coordinator: the request,
//! its replies, and the acknowledgement of a commit. The C documents the
//! request's flags under `NDB_PRIM_KEYREQ` in
//! `ic_apid_handle_messages.ic`; the words here are RonDB 26.10's.
//!
//! **The request, `TCKEYREQ`,** is sent in the long form: eight words,
//! the key in section 0 and the attribute information in section 1. The
//! words after the eighth are there only if their flags say so, in this
//! order: the user id and its version for rate limits, the scan
//! information, the distribution key. The coordinator refuses a long
//! request that is not exactly as long as its flags make it, as a
//! malicious signal. None of the optional words is used here.
//!
//! **Every reply names its receiver in its first word.** `TCKEYCONF`
//! names the transaction, by the pointer given when its record was
//! seized, or is RNIL, and the transaction is then found from the
//! operations it lists. `TCKEYREF` and `TRANSID_AI` name the operation,
//! by the pointer the request gave it.
//!
//! **`TCKEYCONF`** comes from the coordinator and lists, per operation,
//! how many words of row data it will have. When that count has its top
//! bit set, the operation was a dirty read, and the low bits name the
//! node that reads it; one `TRANSID_AI` then completes it, whatever its
//! length. The global checkpoint of a commit is split: its high word
//! comes second, its low word after the operations, when present.
//!
//! **`TRANSID_AI`** comes from the node that read the row, which need
//! not be the coordinator's. Three words, our operation's pointer and
//! the transaction id, then the row: inline in a short signal, in
//! section 0 in a long one. A long one sent in fragments has a fourth
//! word, the row's whole length.
//!
//! **A commit with a marker is acknowledged** with `TC_COMMIT_ACK`, the
//! transaction id, to the block that sent the `TCKEYCONF`, even when
//! the confirmation is otherwise thrown away. The marker counts only
//! when the commit bit is set too.
//!
//! Verify: `TcKeyReq.hpp`, the shifts at its end; `DbtcMain.cpp`,
//! `execTCKEYREQ`, where the optional words are found and the length
//! checked; `NdbOperationExec.cpp`, `fillTcKeyReqHdr`; `TcKeyConf.hpp`;
//! `TcKeyRef.hpp`; `TransIdAI.hpp`; `kernel_types.h`, the operation
//! types; `Ndbif.cpp`, where each reply is dispatched by its first word;
//! `NdbTransaction.cpp`, `receiveTCKEYCONF` and `sendTC_COMMIT_ACK`;
//! `NdbReceiver.hpp`, `execTCOPCONF`, and `NdbReceiver.cpp`,
//! `execTRANSID_AI`.

use ic_port::err;
use ic_port::IcError;

/// Words in a `TCKEYREQ` without optional words.
pub const IC_TCKEYREQ_LEN: usize = 8;
/// Section of a `TCKEYREQ` holding the key.
pub const IC_TCKEYREQ_KEY_SECTION: usize = 0;
/// Section of a `TCKEYREQ` holding the attribute information.
pub const IC_TCKEYREQ_ATTR_SECTION: usize = 1;

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
const IC_TCKEY_DIRTY_SHIFT: u32 = 0;
const IC_TCKEY_NO_DISK_SHIFT: u32 = 1;
const IC_TCKEY_COMMIT_SHIFT: u32 = 4;
const IC_TCKEY_OPERATION_SHIFT: u32 = 5;
const IC_TCKEY_OPERATION_MASK: u32 = 7;
const IC_TCKEY_SIMPLE_SHIFT: u32 = 8;
const IC_TCKEY_EXECUTE_SHIFT: u32 = 10;
const IC_TCKEY_START_SHIFT: u32 = 11;
const IC_TCKEY_ABORT_SHIFT: u32 = 12;
const IC_TCKEY_ABORT_MASK: u32 = 3;

/// What a key request asks for, before it is made into the request
/// information word.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcKeyFlags {
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
}

impl TcKeyFlags {
  /// The request information word.
  pub fn request_info(&self) -> u32 {
    let mut info: u32 = 0;
    info |= (self.dirty as u32) << IC_TCKEY_DIRTY_SHIFT;
    info |= (self.no_disk as u32) << IC_TCKEY_NO_DISK_SHIFT;
    info |= (self.commit as u32) << IC_TCKEY_COMMIT_SHIFT;
    info |=
      (self.operation & IC_TCKEY_OPERATION_MASK) << IC_TCKEY_OPERATION_SHIFT;
    info |= (self.simple as u32) << IC_TCKEY_SIMPLE_SHIFT;
    info |= (self.execute as u32) << IC_TCKEY_EXECUTE_SHIFT;
    info |= (self.start as u32) << IC_TCKEY_START_SHIFT;
    info |= (self.abort_option & IC_TCKEY_ABORT_MASK) << IC_TCKEY_ABORT_SHIFT;
    info
  }
}

/// A key operation, as sent to the coordinator.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcKeyReq {
  /// The coordinator's pointer for the transaction record, from the
  /// seize.
  pub tc_connect_ptr: u32,
  /// Our pointer for the operation, which `TCKEYREF` and `TRANSID_AI`
  /// name it by.
  pub api_operation_ptr: u32,
  /// The table.
  pub table_id: u32,
  /// What to do; see [`TcKeyFlags`].
  pub request_info: u32,
  /// The table version the operation was prepared for.
  pub table_version: u32,
  /// The transaction id, low word.
  pub trans_id1: u32,
  /// The transaction id, high word.
  pub trans_id2: u32,
}

impl TcKeyReq {
  /// The eight words of signal data. The attribute information length,
  /// third, is zero: in the long form it is the length of section 1.
  pub fn encode(&self) -> [u32; IC_TCKEYREQ_LEN] {
    [
      self.tc_connect_ptr,
      self.api_operation_ptr,
      0,
      self.table_id,
      self.request_info,
      self.table_version,
      self.trans_id1,
      self.trans_id2,
    ]
  }
}

/// Words in a `TCKEYCONF` before the operations.
pub const IC_TCKEYCONF_LEN: usize = 5;
/// In an operation's row length: a dirty read, the low bits naming the
/// node that reads it.
pub const IC_TCKEYCONF_DIRTY_READ_BIT: u32 = 1 << 31;
/// Where `TCKEYCONF` names no transaction.
pub const IC_TCKEYCONF_NO_TRANSACTION: u32 = 0xFFFF_FF00;

/// One operation in a `TCKEYCONF`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OperationConf {
  /// Our pointer for the operation.
  pub api_operation_ptr: u32,
  /// Words of row data it will have, or for a dirty read the dirty read
  /// bit and the reading node.
  pub row_len: u32,
}

impl OperationConf {
  /// True for a dirty read: one `TRANSID_AI` completes it.
  pub fn is_dirty_read(&self) -> bool {
    self.row_len > IC_TCKEYCONF_DIRTY_READ_BIT
  }

  /// For a dirty read, the node that reads the row.
  pub fn reading_node(&self) -> u32 {
    self.row_len & !IC_TCKEYCONF_DIRTY_READ_BIT
  }
}

/// The coordinator confirms operations.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TcKeyConf {
  /// Our pointer for the transaction, or
  /// [`IC_TCKEYCONF_NO_TRANSACTION`].
  pub api_connect_ptr: u32,
  /// Commit flag, marker flag and operation count.
  pub conf_info: u32,
  /// The transaction id, low word.
  pub trans_id1: u32,
  /// The transaction id, high word.
  pub trans_id2: u32,
  /// The operations confirmed.
  pub operations: Vec<OperationConf>,
  /// The global checkpoint the commit went into; 0 when there was none,
  /// as for a transaction of dirty reads only.
  pub gci: u64,
}

impl TcKeyConf {
  /// Read a confirmation.
  pub fn decode(data: &[u32]) -> Result<TcKeyConf, IcError> {
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if data.len() < IC_TCKEYCONF_LEN {
      return Err(bad);
    }
    let conf_info = data[2];
    let count = (conf_info & 0xFFFF) as usize;
    let end = IC_TCKEYCONF_LEN + 2 * count;
    if data.len() < end {
      return Err(bad);
    }
    let mut operations: Vec<OperationConf> = Vec::with_capacity(count);
    let mut i: usize = 0;
    while i < count {
      operations.push(OperationConf {
        api_operation_ptr: data[IC_TCKEYCONF_LEN + 2 * i],
        row_len: data[IC_TCKEYCONF_LEN + 2 * i + 1],
      });
      i += 1;
    }
    let mut gci_lo: u32 = 0;
    if data.len() > end {
      gci_lo = data[end];
    }
    Ok(TcKeyConf {
      api_connect_ptr: data[0],
      conf_info,
      trans_id1: data[3],
      trans_id2: data[4],
      operations,
      gci: ((data[1] as u64) << 32) | gci_lo as u64,
    })
  }

  /// True if the transaction committed.
  pub fn is_committed(&self) -> bool {
    (self.conf_info >> 16) & 1 != 0
  }

  /// True if the commit must be acknowledged with `TC_COMMIT_ACK`.
  pub fn needs_commit_ack(&self) -> bool {
    let bits: u32 = 3 << 16;
    self.conf_info & bits == bits
  }
}

/// Words in a `TCKEYREF`.
pub const IC_TCKEYREF_LEN: usize = 5;

/// The coordinator refuses an operation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcKeyRef {
  /// Our pointer for the operation.
  pub api_operation_ptr: u32,
  /// The transaction id, low word.
  pub trans_id1: u32,
  /// The transaction id, high word.
  pub trans_id2: u32,
  /// The NDB error code.
  pub error_code: u32,
  /// More about the error, for some codes.
  pub error_data: u32,
}

impl TcKeyRef {
  /// Read a refusal. The last word may be missing.
  pub fn decode(data: &[u32]) -> Result<TcKeyRef, IcError> {
    if data.len() < IC_TCKEYREF_LEN - 1 {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let mut error_data: u32 = 0;
    if data.len() >= IC_TCKEYREF_LEN {
      error_data = data[4];
    }
    Ok(TcKeyRef {
      api_operation_ptr: data[0],
      trans_id1: data[1],
      trans_id2: data[2],
      error_code: data[3],
      error_data,
    })
  }
}

/// Words in front of the row in a `TRANSID_AI`.
pub const IC_TRANSID_AI_HEADER_LEN: usize = 3;

/// The front of a `TRANSID_AI`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransIdAi {
  /// Our pointer for the operation.
  pub api_operation_ptr: u32,
  /// The transaction id, low word.
  pub trans_id1: u32,
  /// The transaction id, high word.
  pub trans_id2: u32,
}

impl TransIdAi {
  /// Read the front of a `TRANSID_AI`.
  pub fn decode(data: &[u32]) -> Result<TransIdAi, IcError> {
    if data.len() < IC_TRANSID_AI_HEADER_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TransIdAi {
      api_operation_ptr: data[0],
      trans_id1: data[1],
      trans_id2: data[2],
    })
  }

  /// The row: section 0 of a long signal, or what follows the header in
  /// a short one.
  pub fn row<'a>(data: &'a [u32], section0: Option<&'a [u32]>) -> &'a [u32] {
    if let Some(section) = section0 {
      return section;
    }
    if data.len() <= IC_TRANSID_AI_HEADER_LEN {
      return &[];
    }
    &data[IC_TRANSID_AI_HEADER_LEN..]
  }
}

/// The acknowledgement of a commit with a marker: the transaction id.
pub fn tc_commit_ack(trans_id1: u32, trans_id2: u32) -> [u32; 2] {
  [trans_id1, trans_id2]
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_committed_read_sets_its_bits() {
    let flags = TcKeyFlags {
      operation: IC_OP_READ,
      start: true,
      commit: true,
      execute: true,
      simple: true,
      dirty: true,
      no_disk: false,
      abort_option: IC_IGNORE_ERROR,
    };
    // Dirty 0, commit 4, simple 8, execute 10, start 11, abort 12-13.
    let expect = 1 | (1 << 4) | (1 << 8) | (1 << 10) | (1 << 11) | (2 << 12);
    assert_eq!(flags.request_info(), expect);
  }

  #[test]
  fn the_operation_type_goes_in_bits_five_to_seven() {
    let flags = TcKeyFlags {
      operation: IC_OP_DELETE,
      ..TcKeyFlags::default()
    };
    assert_eq!(flags.request_info(), 3 << 5);
  }

  #[test]
  fn a_request_is_eight_words_with_no_attribute_length() {
    let req = TcKeyReq {
      tc_connect_ptr: 11,
      api_operation_ptr: 22,
      table_id: 15,
      request_info: 0x0D11,
      table_version: 3,
      trans_id1: 0x1000,
      trans_id2: 0x2000,
    };
    assert_eq!(req.encode(), [11, 22, 0, 15, 0x0D11, 3, 0x1000, 0x2000]);
  }

  #[test]
  fn a_confirmation_lists_its_operations_and_gci() {
    // Committed with marker, two operations, then gci_lo.
    let conf_info = (3 << 16) | 2;
    let data = [7, 0x10, conf_info, 0x1000, 0x2000, 22, 4, 23, 0, 0x20];
    let conf = TcKeyConf::decode(&data).expect("read");
    assert_eq!(conf.api_connect_ptr, 7);
    assert!(conf.is_committed());
    assert!(conf.needs_commit_ack());
    assert_eq!(conf.operations.len(), 2);
    assert_eq!(conf.operations[0].api_operation_ptr, 22);
    assert_eq!(conf.operations[0].row_len, 4);
    assert_eq!(conf.gci, (0x10 << 32) | 0x20);
  }

  #[test]
  fn a_marker_without_commit_needs_no_ack() {
    let data = [7, 0, 2 << 16, 0x1000, 0x2000];
    let conf = TcKeyConf::decode(&data).expect("read");
    assert!(!conf.is_committed());
    assert!(!conf.needs_commit_ack());
    assert_eq!(conf.gci, 0);
  }

  #[test]
  fn a_dirty_read_names_its_reading_node() {
    let op = OperationConf {
      api_operation_ptr: 22,
      row_len: IC_TCKEYCONF_DIRTY_READ_BIT | 2,
    };
    assert!(op.is_dirty_read());
    assert_eq!(op.reading_node(), 2);
    let plain = OperationConf {
      api_operation_ptr: 22,
      row_len: 4,
    };
    assert!(!plain.is_dirty_read());
  }

  #[test]
  fn a_short_confirmation_is_an_error() {
    // Says two operations, carries one.
    let data = [7, 0, 2, 0x1000, 0x2000, 22, 4];
    assert!(TcKeyConf::decode(&data).is_err());
  }

  #[test]
  fn a_refusal_is_read_with_or_without_its_last_word() {
    let full = TcKeyRef::decode(&[22, 0x1000, 0x2000, 626, 9]).expect("full");
    assert_eq!(full.api_operation_ptr, 22);
    assert_eq!(full.error_code, 626);
    assert_eq!(full.error_data, 9);
    let short = TcKeyRef::decode(&[22, 0x1000, 0x2000, 626]).expect("short");
    assert_eq!(short.error_data, 0);
  }

  #[test]
  fn a_row_comes_inline_or_in_its_section() {
    let short = [22, 0x1000, 0x2000, (1 << 16) | 4, 42];
    let front = TransIdAi::decode(&short).expect("front");
    assert_eq!(front.api_operation_ptr, 22);
    assert_eq!(TransIdAi::row(&short, None), &[(1 << 16) | 4, 42]);
    let long = [22, 0x1000, 0x2000];
    let section = [(1 << 16) | 4, 42];
    assert_eq!(TransIdAi::row(&long, Some(&section[..])), &section);
  }
}
