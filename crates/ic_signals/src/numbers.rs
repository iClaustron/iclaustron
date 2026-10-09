// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Signal numbers: which signal a message is.
//!
//! Only the signals this library sends or receives are named. The C code
//! renamed these for a patched data node; we speak the numbers a stock
//! RonDB uses, so every one here is verified against RonDB 26.10, and
//! chapter 05 §17.5 gives the line for each.

// Cluster membership and heartbeats.

/// A data node's answer to our heartbeat, carrying its node state.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_HEARTBEAT_CONF: u16 = 1;
/// A refusal of our heartbeat, usually a version mismatch.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_HEARTBEAT_REF: u16 = 2;
/// Our heartbeat, which also tells a data node we are alive.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_HEARTBEAT_REQ: u16 = 3;
/// One or more nodes have failed. Verify: chapter 05 §17.5.
pub const IC_SIG_NODE_LOST_REP: u16 = 26;
/// A failed node's work has been taken over, so it may be reconnected.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_NODE_LOST_DONE_REP: u16 = 27;
/// A node has connected. Verify: chapter 05 §17.5.
pub const IC_SIG_NODE_JOINED_REP: u16 = 163;

// Transactions.

/// Take a transaction record in a data node's coordinator.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_COORD_ALLOC_REQ: u16 = 39;
/// The transaction record was taken. Verify: chapter 05 §17.5.
pub const IC_SIG_COORD_ALLOC_CONF: u16 = 37;
/// The transaction record could not be taken. Verify: chapter 05 §17.5.
pub const IC_SIG_COORD_ALLOC_REF: u16 = 38;
/// Give back a transaction record. Verify: chapter 05 §17.5.
pub const IC_SIG_COORD_FREE_REQ: u16 = 36;
/// The record was given back. Verify: chapter 05 §17.5.
pub const IC_SIG_COORD_FREE_CONF: u16 = 34;
/// The record could not be given back. Verify: chapter 05 §17.5.
pub const IC_SIG_COORD_FREE_REF: u16 = 35;
/// Commit a transaction that has no operation left to carry the flag.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_COMMIT_REQ: u16 = 19;
/// The transaction committed. Verify: chapter 05 §17.5.
pub const IC_SIG_COMMIT_CONF: u16 = 17;
/// The transaction did not commit. Verify: chapter 05 §17.5.
pub const IC_SIG_COMMIT_REF: u16 = 18;
/// Roll a transaction back. Verify: chapter 05 §17.5.
pub const IC_SIG_ROLLBACK_REQ: u16 = 15;
/// The transaction rolled back. Verify: chapter 05 §17.5.
pub const IC_SIG_ROLLBACK_CONF: u16 = 13;
/// The rollback failed. Verify: chapter 05 §17.5.
pub const IC_SIG_ROLLBACK_REF: u16 = 14;
/// The data node rolled the transaction back by itself, and says why.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_TRANS_ABORTED_REP: u16 = 16;
/// Tell the data node we have seen the commit, so it may forget it.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_COMMIT_MARKER_FREE_ORD: u16 = 469;
/// A transaction's outcome after the coordinator's node failed.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_TAKEOVER_CONF: u16 = 8;
/// As above, when the outcome was failure. Verify: chapter 05 §17.5.
pub const IC_SIG_TAKEOVER_REF: u16 = 9;
/// RonDB reports a deadlock before aborting. Verify: chapter 05 §17.5.
pub const IC_SIG_DEADLOCK_FOUND_REP: u16 = 980;

// Key operations.

/// A primary key operation: read, insert, update, delete or write.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_KEY_OP_REQ: u16 = 12;
/// One or more key operations succeeded. Verify: chapter 05 §17.5.
pub const IC_SIG_KEY_OP_CONF: u16 = 10;
/// A key operation failed. Verify: chapter 05 §17.5.
pub const IC_SIG_KEY_OP_REF: u16 = 11;
/// A unique index operation. Verify: chapter 05 §17.5.
pub const IC_SIG_UNIQUE_OP_REQ: u16 = 519;
/// A unique index operation succeeded. Verify: chapter 05 §17.5.
pub const IC_SIG_UNIQUE_OP_CONF: u16 = 520;
/// A unique index operation failed. Verify: chapter 05 §17.5.
pub const IC_SIG_UNIQUE_OP_REF: u16 = 521;
/// Row data for an operation we asked for. Verify: chapter 05 §17.5.
pub const IC_SIG_ROW_DATA_REP: u16 = 5;
/// Key data continued in its own signal, for the short form.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_KEY_DATA_ORD: u16 = 6;
/// Column data continued in its own signal. Verify: chapter 05 §17.5.
pub const IC_SIG_COLUMN_DATA_ORD: u16 = 4;
/// The key of a scanned row, so the row can be operated on.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_SCAN_ROW_KEY_REP: u16 = 33;

// Scans, for a later release.

/// Start a scan. Verify: chapter 05 §17.5.
pub const IC_SIG_SCAN_REQ: u16 = 32;
/// A batch of scanned rows is on its way. Verify: chapter 05 §17.5.
pub const IC_SIG_SCAN_CONF: u16 = 29;
/// The scan failed. Verify: chapter 05 §17.5.
pub const IC_SIG_SCAN_REF: u16 = 31;
/// Ask for the next batch, or stop the scan. Verify: chapter 05 §17.5.
pub const IC_SIG_SCAN_MORE_REQ: u16 = 28;

// Dictionary.

/// Ask for a table's definition, by id or by name.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_TABLE_DESC_REQ: u16 = 24;
/// The table definition. Verify: chapter 05 §17.5.
pub const IC_SIG_TABLE_DESC_CONF: u16 = 190;
/// There is no such table, or it cannot be read now.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_TABLE_DESC_REF: u16 = 23;
/// Ask for a list of tables. Verify: chapter 05 §17.5.
pub const IC_SIG_LIST_OBJECTS_REQ: u16 = 193;
/// The list of tables. Verify: chapter 05 §17.5.
pub const IC_SIG_LIST_OBJECTS_CONF: u16 = 194;
/// A table was altered or dropped, sent unasked to every API node.
/// Verify: chapter 05 §17.5.
pub const IC_SIG_TABLE_CHANGED_REP: u16 = 606;
/// Ask a data node's dictionary what a collation is; only to data nodes
/// of 26.10.0 or later. Verify: chapter 05 §8.2.
pub const IC_SIG_COLLATION_DESC_REQ: u16 = 984;
/// A collation's description. Verify: chapter 05 §8.2.
pub const IC_SIG_COLLATION_DESC_CONF: u16 = 985;
/// A collation description request refused. Verify: chapter 05 §8.2.
pub const IC_SIG_COLLATION_DESC_REF: u16 = 986;

// Every signal number has to fit the 16 bits the header gives it. The
// type already says so; this catches a number written as a wider
// constant by mistake.
const _: () = assert!(IC_SIG_COLLATION_DESC_REF as u32 <= 0xFFFF);
const _: () = assert!(IC_SIG_COMMIT_MARKER_FREE_ORD as u32 <= 0xFFFF);

/// The name of a signal, for tracing. Unknown numbers have none, and
/// print as their number.
pub fn sig_name(sig_no: u16) -> Option<&'static str> {
  match sig_no {
    IC_SIG_HEARTBEAT_CONF => Some("HEARTBEAT_CONF"),
    IC_SIG_HEARTBEAT_REF => Some("HEARTBEAT_REF"),
    IC_SIG_HEARTBEAT_REQ => Some("HEARTBEAT_REQ"),
    IC_SIG_NODE_LOST_REP => Some("NODE_LOST_REP"),
    IC_SIG_NODE_LOST_DONE_REP => Some("NODE_LOST_DONE_REP"),
    IC_SIG_NODE_JOINED_REP => Some("NODE_JOINED_REP"),
    IC_SIG_COORD_ALLOC_REQ => Some("COORD_ALLOC_REQ"),
    IC_SIG_COORD_ALLOC_CONF => Some("COORD_ALLOC_CONF"),
    IC_SIG_COORD_ALLOC_REF => Some("COORD_ALLOC_REF"),
    IC_SIG_COORD_FREE_REQ => Some("COORD_FREE_REQ"),
    IC_SIG_COORD_FREE_CONF => Some("COORD_FREE_CONF"),
    IC_SIG_COORD_FREE_REF => Some("COORD_FREE_REF"),
    IC_SIG_COMMIT_REQ => Some("COMMIT_REQ"),
    IC_SIG_COMMIT_CONF => Some("COMMIT_CONF"),
    IC_SIG_COMMIT_REF => Some("COMMIT_REF"),
    IC_SIG_ROLLBACK_REQ => Some("ROLLBACK_REQ"),
    IC_SIG_ROLLBACK_CONF => Some("ROLLBACK_CONF"),
    IC_SIG_ROLLBACK_REF => Some("ROLLBACK_REF"),
    IC_SIG_TRANS_ABORTED_REP => Some("TRANS_ABORTED_REP"),
    IC_SIG_COMMIT_MARKER_FREE_ORD => Some("COMMIT_MARKER_FREE_ORD"),
    IC_SIG_TAKEOVER_CONF => Some("TAKEOVER_CONF"),
    IC_SIG_TAKEOVER_REF => Some("TAKEOVER_REF"),
    IC_SIG_DEADLOCK_FOUND_REP => Some("DEADLOCK_FOUND_REP"),
    IC_SIG_KEY_OP_REQ => Some("KEY_OP_REQ"),
    IC_SIG_KEY_OP_CONF => Some("KEY_OP_CONF"),
    IC_SIG_KEY_OP_REF => Some("KEY_OP_REF"),
    IC_SIG_UNIQUE_OP_REQ => Some("UNIQUE_OP_REQ"),
    IC_SIG_UNIQUE_OP_CONF => Some("UNIQUE_OP_CONF"),
    IC_SIG_UNIQUE_OP_REF => Some("UNIQUE_OP_REF"),
    IC_SIG_ROW_DATA_REP => Some("ROW_DATA_REP"),
    IC_SIG_KEY_DATA_ORD => Some("KEY_DATA_ORD"),
    IC_SIG_COLUMN_DATA_ORD => Some("COLUMN_DATA_ORD"),
    IC_SIG_SCAN_ROW_KEY_REP => Some("SCAN_ROW_KEY_REP"),
    IC_SIG_SCAN_REQ => Some("SCAN_REQ"),
    IC_SIG_SCAN_CONF => Some("SCAN_CONF"),
    IC_SIG_SCAN_REF => Some("SCAN_REF"),
    IC_SIG_SCAN_MORE_REQ => Some("SCAN_MORE_REQ"),
    IC_SIG_TABLE_DESC_REQ => Some("TABLE_DESC_REQ"),
    IC_SIG_TABLE_DESC_CONF => Some("TABLE_DESC_CONF"),
    IC_SIG_TABLE_DESC_REF => Some("TABLE_DESC_REF"),
    IC_SIG_LIST_OBJECTS_REQ => Some("LIST_OBJECTS_REQ"),
    IC_SIG_LIST_OBJECTS_CONF => Some("LIST_OBJECTS_CONF"),
    IC_SIG_TABLE_CHANGED_REP => Some("TABLE_CHANGED_REP"),
    IC_SIG_COLLATION_DESC_REQ => Some("COLLATION_DESC_REQ"),
    IC_SIG_COLLATION_DESC_CONF => Some("COLLATION_DESC_CONF"),
    IC_SIG_COLLATION_DESC_REF => Some("COLLATION_DESC_REF"),
    _ => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn numbers_are_distinct() {
    let all = [
      IC_SIG_HEARTBEAT_CONF,
      IC_SIG_HEARTBEAT_REF,
      IC_SIG_HEARTBEAT_REQ,
      IC_SIG_NODE_LOST_REP,
      IC_SIG_NODE_LOST_DONE_REP,
      IC_SIG_NODE_JOINED_REP,
      IC_SIG_COORD_ALLOC_REQ,
      IC_SIG_COORD_ALLOC_CONF,
      IC_SIG_COORD_ALLOC_REF,
      IC_SIG_COORD_FREE_REQ,
      IC_SIG_COMMIT_REQ,
      IC_SIG_ROLLBACK_REQ,
      IC_SIG_TRANS_ABORTED_REP,
      IC_SIG_COMMIT_MARKER_FREE_ORD,
      IC_SIG_KEY_OP_REQ,
      IC_SIG_KEY_OP_CONF,
      IC_SIG_KEY_OP_REF,
      IC_SIG_UNIQUE_OP_REQ,
      IC_SIG_ROW_DATA_REP,
      IC_SIG_SCAN_ROW_KEY_REP,
      IC_SIG_SCAN_REQ,
      IC_SIG_TABLE_DESC_REQ,
      IC_SIG_TABLE_DESC_CONF,
      IC_SIG_TABLE_CHANGED_REP,
      IC_SIG_COLLATION_DESC_REQ,
      IC_SIG_COLLATION_DESC_CONF,
      IC_SIG_COLLATION_DESC_REF,
    ];
    let mut i: usize = 0;
    while i < all.len() {
      let mut j = i + 1;
      while j < all.len() {
        assert_ne!(all[i], all[j], "duplicate signal number {}", all[i]);
        j += 1;
      }
      i += 1;
    }
  }

  #[test]
  fn names_are_there_for_tracing() {
    assert_eq!(sig_name(IC_SIG_KEY_OP_REQ), Some("KEY_OP_REQ"));
    assert_eq!(sig_name(IC_SIG_HEARTBEAT_REQ), Some("HEARTBEAT_REQ"));
    assert_eq!(sig_name(9999), None);
  }
}
