// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Block numbers and block references.
//!
//! Every signal travels from one block to another. A block is a part of
//! a node: in a data node the transaction coordinator, the dictionary,
//! the membership block and so on; in an API node one block per thread
//! that talks to the cluster, plus a fixed one for the heartbeat.
//!
//! A block reference packs a block number and a node id into one word,
//! the node id in the low 16 bits. Only the block number travels in a
//! signal header; the receiver rebuilds the full reference by adding the
//! node it came from, which it knows from the socket the signal arrived
//! on.
//!
//! Verify: chapter 05 §17.4.

/// Backups.
pub const IC_BLOCK_ARCHIVE: u16 = 0xF4;
/// Transaction coordinator: where a transaction is driven from, and the
/// block an API node sends its key and scan operations to.
pub const IC_BLOCK_COORDINATOR: u16 = 0xF5;
/// Distribution: which nodes hold the replicas of each fragment.
pub const IC_BLOCK_DISTRIBUTION: u16 = 0xF6;
/// The block that runs operations on a fragment and owns its rows.
pub const IC_BLOCK_LOCAL_OPS: u16 = 0xF7;
/// The hash index over each fragment's primary key.
pub const IC_BLOCK_HASH_INDEX: u16 = 0xF8;
/// Row store: the block that stores the rows themselves.
pub const IC_BLOCK_ROW_STORE: u16 = 0xF9;
/// Dictionary: table and index metadata.
pub const IC_BLOCK_DICTIONARY: u16 = 0xFA;
/// Start and stop of a node.
pub const IC_BLOCK_START_CONTROL: u16 = 0xFB;
/// Membership: which nodes are in the cluster, and heartbeats, which is
/// where an API node sends its `HEARTBEAT_REQ`.
pub const IC_BLOCK_MEMBERSHIP: u16 = 0xFC;
/// File system.
pub const IC_BLOCK_FILE_IO: u16 = 0xFD;
/// Configuration and connection management.
pub const IC_BLOCK_NODE_ADMIN: u16 = 0xFE;
/// Index builds and other work driven by triggers.
pub const IC_BLOCK_INDEX_BUILD: u16 = 0xFF;
/// Utility block.
pub const IC_BLOCK_UTILITY: u16 = 0x100;
/// Change feed: events and replication.
pub const IC_BLOCK_CHANGE_FEED: u16 = 0x101;
/// Ordered indexes.
pub const IC_BLOCK_ORDERED_INDEX: u16 = 0x102;
/// Joins pushed down to the data nodes. It was 0x109 until 2026-10-08,
/// which is a thread administration block; nothing used it yet.
pub const IC_BLOCK_JOIN: u16 = 0x108;
/// The links to other nodes.
pub const IC_BLOCK_LINK_ADMIN: u16 = 0x10A;
/// First of the read worker blocks, which can send row data to us.
pub const IC_BLOCK_READ_WORKER_FIRST: u16 = 0x10B;
/// Last of the read worker blocks.
pub const IC_BLOCK_READ_WORKER_LAST: u16 = 0x111;

/// First block number an API node's threads may use. Each thread that
/// talks to the cluster takes one, and its number is how data nodes
/// address replies to that thread.
/// Verify: chapter 05 §17.4.
pub const IC_FIRST_USER_THREAD_BLOCK: u16 = 0x8000;
/// Largest number of API threads one node may have.
/// Verify: chapter 05 §17.4.
pub const IC_MAX_API_THREADS: u32 = 4711;
/// The pseudo-block several small replies are bundled to, so that one
/// signal can carry answers for several operations.
/// Verify: chapter 05 §17.4.
pub const IC_BLOCK_BUNDLE: u16 = 0x07FF;
/// The fixed block an API node uses for heartbeats and node state,
/// separate from the numbered per-thread blocks.
/// Verify: chapter 05 §17.4.
pub const IC_BLOCK_NODE_WATCH: u16 = 0x0FA2;
/// The fixed block the management server's configuration manager uses.
/// Verify: chapter 05 §17.4.
pub const IC_BLOCK_CONFIG_SERVICE: u16 = 0x0FA3;

/// Build a block reference from a block number and a node id.
/// Verify: chapter 05 §17.4.
pub fn number_to_ref(block: u16, node_id: u32) -> u32 {
  (node_id & 0xFFFF) | ((block as u32) << 16)
}

/// The node a block reference names.
/// Verify: chapter 05 §17.4.
pub fn ref_to_node(block_ref: u32) -> u32 {
  block_ref & 0xFFFF
}

/// The block a block reference names.
/// Verify: chapter 05 §17.4.
pub fn ref_to_block(block_ref: u32) -> u16 {
  (block_ref >> 16) as u16
}

/// The block number an API thread with this id uses. The id is the
/// thread's own, counted from zero.
pub fn api_block_of_thread(thread_id: u32) -> u16 {
  IC_FIRST_USER_THREAD_BLOCK + (thread_id as u16)
}

/// The thread an API block number belongs to, or `None` if the number
/// is not one of ours.
pub fn thread_of_api_block(block: u16) -> Option<u32> {
  if block < IC_FIRST_USER_THREAD_BLOCK {
    return None;
  }
  Some((block - IC_FIRST_USER_THREAD_BLOCK) as u32)
}

/// The first data node block number, from which the main block is
/// counted in a block number's low part.
const IC_FIRST_KERNEL_BLOCK: u32 = 244;

/// Which instance of its block a data node's block number names, such
/// as the coordinator instance a record was allocated on, for asking
/// another record of the same one. The high 7 bits of the number are
/// the instance's low 7 bits; the low 9 bits hold the main block,
/// counted from 244 in steps of one, and the instance's higher bits in
/// steps of 64, the sum wrapping at 512 and skipping 0. Verify: chapter
/// 05 §17.4.
pub fn block_instance(block: u16) -> u32 {
  let low = (block as u32) >> 9;
  let mut part = (block as u32) & 0x1FF;
  if part == 0 {
    return low;
  }
  if part < IC_FIRST_KERNEL_BLOCK {
    // Wrapped past 512: one more was added to skip 0.
    part -= 1;
  }
  let counted = (part + 512 - IC_FIRST_KERNEL_BLOCK) & 0x1FF;
  low + ((counted >> 6) << 7)
}

/// True for a block that belongs to a data node rather than to us.
pub fn is_kernel_block(block: u16) -> bool {
  (IC_BLOCK_ARCHIVE..=IC_BLOCK_READ_WORKER_LAST).contains(&block)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn references_pack_and_unpack() {
    // A large API node id has to survive the round trip: node ids go
    // well past 255 and the reference gives them 16 bits.
    let reference = number_to_ref(IC_BLOCK_COORDINATOR, 1600);
    assert_eq!(ref_to_node(reference), 1600);
    assert_eq!(ref_to_block(reference), IC_BLOCK_COORDINATOR);
    let ours = number_to_ref(api_block_of_thread(3), 65535);
    assert_eq!(ref_to_node(ours), 65535);
    assert_eq!(ref_to_block(ours), 0x8003);
  }

  #[test]
  fn api_blocks_map_to_threads() {
    assert_eq!(api_block_of_thread(0), IC_FIRST_USER_THREAD_BLOCK);
    assert_eq!(api_block_of_thread(7), 0x8007);
    assert_eq!(thread_of_api_block(0x8007), Some(7));
    assert_eq!(thread_of_api_block(IC_FIRST_USER_THREAD_BLOCK), Some(0));
    // The bundle and node watch blocks are not thread blocks.
    assert_eq!(thread_of_api_block(IC_BLOCK_BUNDLE), None);
    assert_eq!(thread_of_api_block(IC_BLOCK_NODE_WATCH), None);
    assert_eq!(thread_of_api_block(IC_BLOCK_COORDINATOR), None);
  }

  /// A block number made as the data nodes make one, for the tests.
  fn instance_block(main: u32, instance: u32) -> u16 {
    let high = instance >> 7;
    let mut part =
      ((main - IC_FIRST_KERNEL_BLOCK) + (high << 6) + IC_FIRST_KERNEL_BLOCK)
        & 0x1FF;
    if part < IC_FIRST_KERNEL_BLOCK {
      part += 1;
    }
    (part | ((instance & 0x7F) << 9)) as u16
  }

  #[test]
  fn a_block_number_names_its_instance() {
    // Seen on the wire: the coordinator, instance 2.
    assert_eq!(block_instance(0x04F5), 2);
    assert_eq!(block_instance(IC_BLOCK_COORDINATOR), 0);
    for instance in [0u32, 1, 2, 127, 128, 200, 359, 511, 512, 700, 1023] {
      let block = instance_block(IC_BLOCK_COORDINATOR as u32, instance);
      assert_eq!(block_instance(block), instance, "{}", instance);
    }
    // Up to 127 the low 9 bits are the block itself.
    assert_eq!(instance_block(IC_BLOCK_COORDINATOR as u32, 5) & 0x1FF, 0xF5);
  }

  #[test]
  fn kernel_blocks_are_recognised() {
    assert!(is_kernel_block(IC_BLOCK_COORDINATOR));
    assert!(is_kernel_block(IC_BLOCK_MEMBERSHIP));
    assert!(is_kernel_block(IC_BLOCK_CHANGE_FEED));
    // The read worker blocks send row data too, so they count.
    assert!(is_kernel_block(IC_BLOCK_READ_WORKER_FIRST));
    assert!(is_kernel_block(IC_BLOCK_READ_WORKER_LAST));
    assert!(!is_kernel_block(IC_BLOCK_BUNDLE));
    assert!(!is_kernel_block(IC_FIRST_USER_THREAD_BLOCK));
  }
}
