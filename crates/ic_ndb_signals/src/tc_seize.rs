// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Taking and giving back a transaction record in a data node.
//!
//! Before an API node can run a transaction through a data node, it
//! takes a record in that node's transaction coordinator. The record is
//! where the coordinator keeps the transaction's state, and the pointer
//! to it goes in every later signal of the transaction. Records are
//! taken once and kept, one transaction after another, and given back
//! at shutdown.
//!
//! These are the first signals a user thread sends under its own block
//! number, so the replies are also the first to be routed to a user
//! thread rather than executed where they arrive.
//!
//! All six are plain words with no sections. Verify the numbers in
//! `GlobalSignalNumbers.h`, what the API sends in `Ndb.cpp`,
//! `NDB_connect`, and `NdbApiSignal.cpp`, `setSignal`, and what the
//! coordinator reads and answers in `DbtcMain.cpp`, `execTCSEIZEREQ`
//! and `execTCRELEASEREQ`.

use ic_port::err;
use ic_port::IcError;

/// Words in a `TCSEIZEREQ`.
pub const IC_TCSEIZEREQ_LEN: usize = 3;
/// Words in a `TCSEIZECONF`.
pub const IC_TCSEIZECONF_LEN: usize = 3;
/// Words in a `TCSEIZEREF`.
pub const IC_TCSEIZEREF_LEN: usize = 2;
/// Words in a `TCRELEASEREQ`.
pub const IC_TCRELEASEREQ_LEN: usize = 3;
/// Words in a `TCRELEASECONF`.
pub const IC_TCRELEASECONF_LEN: usize = 1;
/// Words in a `TCRELEASEREF`. The third is where in the data node's
/// source the refusal was raised, which we keep for the trace only.
pub const IC_TCRELEASEREF_LEN: usize = 3;

/// Let the data node choose which of its coordinator instances serves
/// us.
pub const IC_ANY_TC_INSTANCE: u32 = 0;

/// Ask a data node's transaction coordinator for a record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcSeizeReq {
  /// Our own name for the record, which comes back in the answer so we
  /// can tell which request it answers.
  pub api_connect_ptr: u32,
  /// Our block reference, which is where the answer is sent. For a user
  /// thread this is its own block, and that is what routes the answer
  /// to it.
  pub api_block_ref: u32,
  /// Which coordinator instance we want, or [`IC_ANY_TC_INSTANCE`].
  pub instance: u32,
}

impl TcSeizeReq {
  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_TCSEIZEREQ_LEN] {
    [self.api_connect_ptr, self.api_block_ref, self.instance]
  }
}

/// The coordinator gave us a record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcSeizeConf {
  /// Our own name for the record, as we sent it.
  pub api_connect_ptr: u32,
  /// The coordinator's name for the record, to send in every signal of
  /// a transaction that uses it.
  pub tc_connect_ptr: u32,
  /// The coordinator's block reference, instance included, which is
  /// where those signals go.
  pub tc_block_ref: u32,
}

impl TcSeizeConf {
  /// Read the answer.
  pub fn decode(data: &[u32]) -> Result<TcSeizeConf, IcError> {
    if data.len() < IC_TCSEIZECONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TcSeizeConf {
      api_connect_ptr: data[0],
      tc_connect_ptr: data[1],
      tc_block_ref: data[2],
    })
  }
}

/// The coordinator would not give us a record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcSeizeRef {
  /// Our own name for the record, as we sent it.
  pub api_connect_ptr: u32,
  /// Why not, as an NDB error code: the node is not started, is
  /// shutting down, or has no record free.
  pub error_code: u32,
}

impl TcSeizeRef {
  /// Read the refusal.
  pub fn decode(data: &[u32]) -> Result<TcSeizeRef, IcError> {
    if data.len() < IC_TCSEIZEREF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TcSeizeRef {
      api_connect_ptr: data[0],
      error_code: data[1],
    })
  }
}

/// Give a record back to the coordinator.
///
/// The word order differs from the seize: the coordinator's pointer
/// comes first here, because it is the coordinator that has to find the
/// record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcReleaseReq {
  /// The coordinator's name for the record.
  pub tc_connect_ptr: u32,
  /// Our block reference, which is where the answer is sent.
  pub api_block_ref: u32,
  /// Our own name for the record, which comes back in the answer.
  pub api_connect_ptr: u32,
}

impl TcReleaseReq {
  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_TCRELEASEREQ_LEN] {
    [
      self.tc_connect_ptr,
      self.api_block_ref,
      self.api_connect_ptr,
    ]
  }
}

/// The record was given back.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcReleaseConf {
  /// Our own name for the record.
  pub api_connect_ptr: u32,
}

impl TcReleaseConf {
  /// Read the answer.
  pub fn decode(data: &[u32]) -> Result<TcReleaseConf, IcError> {
    if data.len() < IC_TCRELEASECONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TcReleaseConf {
      api_connect_ptr: data[0],
    })
  }
}

/// The record could not be given back, because the coordinator does not
/// know it as ours.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TcReleaseRef {
  /// Our own name for the record.
  pub api_connect_ptr: u32,
  /// Why not, as an NDB error code.
  pub error_code: u32,
}

impl TcReleaseRef {
  /// Read the refusal. Older senders may leave the third word out.
  pub fn decode(data: &[u32]) -> Result<TcReleaseRef, IcError> {
    if data.len() < 2 {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(TcReleaseRef {
      api_connect_ptr: data[0],
      error_code: data[1],
    })
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::blocks::*;

  #[test]
  fn a_seize_names_the_record_then_says_where_to_answer() {
    let block_ref = number_to_ref(api_block_of_thread(0), 192);
    let request = TcSeizeReq {
      api_connect_ptr: 7,
      api_block_ref: block_ref,
      instance: IC_ANY_TC_INSTANCE,
    };
    assert_eq!(request.encode(), [7, block_ref, 0]);
  }

  #[test]
  fn a_release_puts_the_coordinators_pointer_first() {
    // Easy to get wrong by symmetry with the seize, and the data node
    // would then look up a record by our pointer and refuse.
    let request = TcReleaseReq {
      tc_connect_ptr: 4711,
      api_block_ref: 0x8000_00C0,
      api_connect_ptr: 7,
    };
    assert_eq!(request.encode(), [4711, 0x8000_00C0, 7]);
  }

  #[test]
  fn a_seize_answer_gives_the_pointer_and_where_to_send() {
    let tc_ref = number_to_ref(IC_BLOCK_DBTC, 2);
    let conf = TcSeizeConf::decode(&[7, 4711, tc_ref]).expect("conf");
    assert_eq!(conf.api_connect_ptr, 7);
    assert_eq!(conf.tc_connect_ptr, 4711);
    assert_eq!(ref_to_node(conf.tc_block_ref), 2);
    assert_eq!(ref_to_block(conf.tc_block_ref), IC_BLOCK_DBTC);
    assert!(TcSeizeConf::decode(&[7, 4711]).is_err());
  }

  #[test]
  fn refusals_carry_our_pointer_and_a_reason() {
    let refusal = TcSeizeRef::decode(&[7, 203]).expect("ref");
    assert_eq!(refusal.api_connect_ptr, 7);
    assert_eq!(refusal.error_code, 203);
    assert!(TcSeizeRef::decode(&[7]).is_err());
    // A release refusal has a third word, the source line, which an
    // older sender may leave out.
    let refusal = TcReleaseRef::decode(&[7, 229, 2481]).expect("ref");
    assert_eq!(refusal.error_code, 229);
    let refusal = TcReleaseRef::decode(&[7, 229]).expect("short ref");
    assert_eq!(refusal.api_connect_ptr, 7);
  }

  #[test]
  fn a_release_answer_is_just_our_pointer() {
    let conf = TcReleaseConf::decode(&[7]).expect("conf");
    assert_eq!(conf.api_connect_ptr, 7);
    assert!(TcReleaseConf::decode(&[]).is_err());
  }
}
