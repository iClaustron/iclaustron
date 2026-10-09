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
//! All six are plain words with no sections. Verify: chapter 05 §17.10.

use ic_port::err;
use ic_port::IcError;

/// Words in a `COORD_ALLOC_REQ`.
pub const IC_COORD_ALLOC_REQ_LEN: usize = 3;
/// Words in a `COORD_ALLOC_CONF`.
pub const IC_COORD_ALLOC_CONF_LEN: usize = 3;
/// Words in a `COORD_ALLOC_REF`.
pub const IC_COORD_ALLOC_REF_LEN: usize = 2;
/// Words in a `COORD_FREE_REQ`.
pub const IC_COORD_FREE_REQ_LEN: usize = 3;
/// Words in a `COORD_FREE_CONF`.
pub const IC_COORD_FREE_CONF_LEN: usize = 1;
/// Words in a `COORD_FREE_REF`. The third is where in the data node's
/// source the refusal was raised, which we keep for the trace only.
pub const IC_COORD_FREE_REF_LEN: usize = 3;

/// Let the data node choose which of its coordinator instances serves
/// us.
pub const IC_ANY_COORD_INSTANCE: u32 = 0;

/// Ask a data node's transaction coordinator for a record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoordAllocReq {
  /// Our own name for the record, which comes back in the answer so we
  /// can tell which request it answers.
  pub client_ptr: u32,
  /// Our block reference, which is where the answer is sent. For a user
  /// thread this is its own block, and that is what routes the answer
  /// to it.
  pub client_block_ref: u32,
  /// Which coordinator instance we want, or [`IC_ANY_COORD_INSTANCE`].
  pub instance: u32,
}

impl CoordAllocReq {
  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_COORD_ALLOC_REQ_LEN] {
    [self.client_ptr, self.client_block_ref, self.instance]
  }
}

/// The coordinator gave us a record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoordAllocConf {
  /// Our own name for the record, as we sent it.
  pub client_ptr: u32,
  /// The coordinator's name for the record, to send in every signal of
  /// a transaction that uses it.
  pub coord_ptr: u32,
  /// The coordinator's block reference, instance included, which is
  /// where those signals go.
  pub coord_block_ref: u32,
}

impl CoordAllocConf {
  /// Read the answer.
  pub fn decode(data: &[u32]) -> Result<CoordAllocConf, IcError> {
    if data.len() < IC_COORD_ALLOC_CONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CoordAllocConf {
      client_ptr: data[0],
      coord_ptr: data[1],
      coord_block_ref: data[2],
    })
  }
}

/// The coordinator would not give us a record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoordAllocRef {
  /// Our own name for the record, as we sent it.
  pub client_ptr: u32,
  /// Why not, as an NDB error code: the node is not started, is
  /// shutting down, or has no record free.
  pub error_code: u32,
}

impl CoordAllocRef {
  /// Read the refusal.
  pub fn decode(data: &[u32]) -> Result<CoordAllocRef, IcError> {
    if data.len() < IC_COORD_ALLOC_REF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CoordAllocRef {
      client_ptr: data[0],
      error_code: data[1],
    })
  }
}

/// Give a record back to the coordinator.
///
/// The word order differs from the allocate: the coordinator's pointer
/// comes first here, because it is the coordinator that has to find the
/// record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoordFreeReq {
  /// The coordinator's name for the record.
  pub coord_ptr: u32,
  /// Our block reference, which is where the answer is sent.
  pub client_block_ref: u32,
  /// Our own name for the record, which comes back in the answer.
  pub client_ptr: u32,
}

impl CoordFreeReq {
  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_COORD_FREE_REQ_LEN] {
    [self.coord_ptr, self.client_block_ref, self.client_ptr]
  }
}

/// The record was given back.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoordFreeConf {
  /// Our own name for the record.
  pub client_ptr: u32,
}

impl CoordFreeConf {
  /// Read the answer.
  pub fn decode(data: &[u32]) -> Result<CoordFreeConf, IcError> {
    if data.len() < IC_COORD_FREE_CONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CoordFreeConf {
      client_ptr: data[0],
    })
  }
}

/// The record could not be given back, because the coordinator does not
/// know it as ours.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoordFreeRef {
  /// Our own name for the record.
  pub client_ptr: u32,
  /// Why not, as an NDB error code.
  pub error_code: u32,
}

impl CoordFreeRef {
  /// Read the refusal. Older senders may leave the third word out.
  pub fn decode(data: &[u32]) -> Result<CoordFreeRef, IcError> {
    if data.len() < 2 {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CoordFreeRef {
      client_ptr: data[0],
      error_code: data[1],
    })
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::blocks::*;

  #[test]
  fn a_alloc_names_the_record_then_says_where_to_answer() {
    let block_ref = number_to_ref(api_block_of_thread(0), 192);
    let request = CoordAllocReq {
      client_ptr: 7,
      client_block_ref: block_ref,
      instance: IC_ANY_COORD_INSTANCE,
    };
    assert_eq!(request.encode(), [7, block_ref, 0]);
  }

  #[test]
  fn a_release_puts_the_coordinators_pointer_first() {
    // Easy to get wrong by symmetry with the allocate, and the data node
    // would then look up a record by our pointer and refuse.
    let request = CoordFreeReq {
      coord_ptr: 4711,
      client_block_ref: 0x8000_00C0,
      client_ptr: 7,
    };
    assert_eq!(request.encode(), [4711, 0x8000_00C0, 7]);
  }

  #[test]
  fn a_alloc_answer_gives_the_pointer_and_where_to_send() {
    let coord_ref = number_to_ref(IC_BLOCK_COORDINATOR, 2);
    let conf = CoordAllocConf::decode(&[7, 4711, coord_ref]).expect("conf");
    assert_eq!(conf.client_ptr, 7);
    assert_eq!(conf.coord_ptr, 4711);
    assert_eq!(ref_to_node(conf.coord_block_ref), 2);
    assert_eq!(ref_to_block(conf.coord_block_ref), IC_BLOCK_COORDINATOR);
    assert!(CoordAllocConf::decode(&[7, 4711]).is_err());
  }

  #[test]
  fn refusals_carry_our_pointer_and_a_reason() {
    let refusal = CoordAllocRef::decode(&[7, 203]).expect("ref");
    assert_eq!(refusal.client_ptr, 7);
    assert_eq!(refusal.error_code, 203);
    assert!(CoordAllocRef::decode(&[7]).is_err());
    // A release refusal has a third word, the source line, which an
    // older sender may leave out.
    let refusal = CoordFreeRef::decode(&[7, 229, 2481]).expect("ref");
    assert_eq!(refusal.error_code, 229);
    let refusal = CoordFreeRef::decode(&[7, 229]).expect("short ref");
    assert_eq!(refusal.client_ptr, 7);
  }

  #[test]
  fn a_release_answer_is_just_our_pointer() {
    let conf = CoordFreeConf::decode(&[7]).expect("conf");
    assert_eq!(conf.client_ptr, 7);
    assert!(CoordFreeConf::decode(&[]).is_err());
  }
}
