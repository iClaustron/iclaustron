// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Block numbers and block references.
//!
//! Every signal travels from one block to another. A block is a part of
//! a node: in a data node the transaction coordinator, the dictionary,
//! the cluster manager and so on; in an API node one block per thread
//! that talks to the cluster, plus a fixed one for the heartbeat.
//!
//! A block reference packs a block number and a node id into one word,
//! the node id in the low 16 bits. Only the block number travels in a
//! signal header; the receiver rebuilds the full reference by adding the
//! node it came from, which it knows from the socket the signal arrived
//! on.
//!
//! Verify: RonDB 26.10 `include/kernel/BlockNumbers.h` and
//! `include/kernel/RefConvert.hpp`.

/// Backup block.
pub const IC_BLOCK_BACKUP: u16 = 0xF4;
/// Transaction coordinator: where a transaction is driven from, and the
/// block an API node sends its key and scan operations to.
pub const IC_BLOCK_DBTC: u16 = 0xF5;
/// Distribution handler.
pub const IC_BLOCK_DBDIH: u16 = 0xF6;
/// Local query handler: the block that owns a fragment's rows.
pub const IC_BLOCK_DBLQH: u16 = 0xF7;
/// Hash index handler.
pub const IC_BLOCK_DBACC: u16 = 0xF8;
/// Tuple manager: the block that stores the rows themselves.
pub const IC_BLOCK_DBTUP: u16 = 0xF9;
/// Dictionary: table and index metadata.
pub const IC_BLOCK_DBDICT: u16 = 0xFA;
/// Start and stop of a node.
pub const IC_BLOCK_NDBCNTR: u16 = 0xFB;
/// Cluster manager: node membership and heartbeats, which is where an
/// API node sends its `API_REGREQ`.
pub const IC_BLOCK_QMGR: u16 = 0xFC;
/// File system.
pub const IC_BLOCK_NDBFS: u16 = 0xFD;
/// Configuration and connection management.
pub const IC_BLOCK_CMVMI: u16 = 0xFE;
/// Trigger and index handler.
pub const IC_BLOCK_TRIX: u16 = 0xFF;
/// Utility block.
pub const IC_BLOCK_DBUTIL: u16 = 0x100;
/// Subscription manager: events and replication.
pub const IC_BLOCK_SUMA: u16 = 0x101;
/// Ordered index handler.
pub const IC_BLOCK_DBTUX: u16 = 0x102;
/// Query handler for pushed-down joins.
pub const IC_BLOCK_DBSPJ: u16 = 0x109;
/// Transporter manager.
pub const IC_BLOCK_TRPMAN: u16 = 0x10A;
/// First of the RonDB query blocks, which can send row data to us.
pub const IC_BLOCK_FIRST_QUERY: u16 = 0x10B;
/// Last of the RonDB query blocks.
pub const IC_BLOCK_LAST_QUERY: u16 = 0x111;

/// First block number an API node's threads may use. Each thread that
/// talks to the cluster takes one, and its number is how data nodes
/// address replies to that thread.
/// Verify: `BlockNumbers.h:34`, `MIN_API_BLOCK_NO`.
pub const IC_MIN_API_BLOCK_NO: u16 = 0x8000;
/// Largest number of API threads one node may have.
/// Verify: `TransporterFacade.hpp:69`, `MAX_NO_THREADS`.
pub const IC_MAX_API_THREADS: u32 = 4711;
/// The pseudo-block several small replies are bundled to, so that one
/// signal can carry answers for several operations.
/// Verify: `BlockNumbers.h:37`, `API_PACKED`.
pub const IC_BLOCK_API_PACKED: u16 = 0x07FF;
/// The fixed block an API node uses for heartbeats and node state,
/// separate from the numbered per-thread blocks.
/// Verify: `BlockNumbers.h:40`, `API_CLUSTERMGR`.
pub const IC_BLOCK_API_CLUSTERMGR: u16 = 0x0FA2;
/// The fixed block the management server's configuration manager uses.
/// Verify: `BlockNumbers.h:41`, `MGM_CONFIG_MAN`.
pub const IC_BLOCK_MGM_CONFIG_MAN: u16 = 0x0FA3;

/// Build a block reference from a block number and a node id.
/// Verify: `RefConvert.hpp:184`, `numberToRef`.
pub fn number_to_ref(block: u16, node_id: u32) -> u32 {
  (node_id & 0xFFFF) | ((block as u32) << 16)
}

/// The node a block reference names.
/// Verify: `RefConvert.hpp:161`, `refToNode`.
pub fn ref_to_node(block_ref: u32) -> u32 {
  block_ref & 0xFFFF
}

/// The block a block reference names.
/// Verify: `RefConvert.hpp:166`, `refToBlock`.
pub fn ref_to_block(block_ref: u32) -> u16 {
  (block_ref >> 16) as u16
}

/// The block number an API thread with this id uses. The id is the
/// thread's own, counted from zero.
pub fn api_block_of_thread(thread_id: u32) -> u16 {
  IC_MIN_API_BLOCK_NO + (thread_id as u16)
}

/// The thread an API block number belongs to, or `None` if the number
/// is not one of ours.
pub fn thread_of_api_block(block: u16) -> Option<u32> {
  if block < IC_MIN_API_BLOCK_NO {
    return None;
  }
  Some((block - IC_MIN_API_BLOCK_NO) as u32)
}

/// True for a block that belongs to a data node rather than to us.
pub fn is_kernel_block(block: u16) -> bool {
  (IC_BLOCK_BACKUP..=IC_BLOCK_LAST_QUERY).contains(&block)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn references_pack_and_unpack() {
    // A large API node id has to survive the round trip: node ids go
    // well past 255 and the reference gives them 16 bits.
    let reference = number_to_ref(IC_BLOCK_DBTC, 1600);
    assert_eq!(ref_to_node(reference), 1600);
    assert_eq!(ref_to_block(reference), IC_BLOCK_DBTC);
    let ours = number_to_ref(api_block_of_thread(3), 65535);
    assert_eq!(ref_to_node(ours), 65535);
    assert_eq!(ref_to_block(ours), 0x8003);
  }

  #[test]
  fn api_blocks_map_to_threads() {
    assert_eq!(api_block_of_thread(0), IC_MIN_API_BLOCK_NO);
    assert_eq!(api_block_of_thread(7), 0x8007);
    assert_eq!(thread_of_api_block(0x8007), Some(7));
    assert_eq!(thread_of_api_block(IC_MIN_API_BLOCK_NO), Some(0));
    // The packed and cluster manager blocks are not thread blocks.
    assert_eq!(thread_of_api_block(IC_BLOCK_API_PACKED), None);
    assert_eq!(thread_of_api_block(IC_BLOCK_API_CLUSTERMGR), None);
    assert_eq!(thread_of_api_block(IC_BLOCK_DBTC), None);
  }

  #[test]
  fn kernel_blocks_are_recognised() {
    assert!(is_kernel_block(IC_BLOCK_DBTC));
    assert!(is_kernel_block(IC_BLOCK_QMGR));
    assert!(is_kernel_block(IC_BLOCK_SUMA));
    // RonDB's query blocks send row data too, so they count.
    assert!(is_kernel_block(IC_BLOCK_FIRST_QUERY));
    assert!(is_kernel_block(IC_BLOCK_LAST_QUERY));
    assert!(!is_kernel_block(IC_BLOCK_API_PACKED));
    assert!(!is_kernel_block(IC_MIN_API_BLOCK_NO));
  }
}
