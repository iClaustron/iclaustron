// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Global signal numbers: which signal a message is.
//!
//! Only the signals this library sends or receives are named. The C code
//! renamed these for a patched data node; we speak the numbers a stock
//! RonDB uses, so every one here is verified against RonDB 26.10
//! `include/kernel/GlobalSignalNumbers.h` at the line given.

// Cluster membership and heartbeats.

/// A data node's answer to our heartbeat, carrying its node state.
/// Verify: line 53.
pub const IC_GSN_API_REGCONF: u16 = 1;
/// A refusal of our heartbeat, usually a version mismatch. Verify: 54.
pub const IC_GSN_API_REGREF: u16 = 2;
/// Our heartbeat, which also tells a data node we are alive.
/// Verify: line 55.
pub const IC_GSN_API_REGREQ: u16 = 3;
/// One or more nodes have failed. Verify: line 91.
pub const IC_GSN_NODE_FAILREP: u16 = 26;
/// A failed node's work has been taken over, so it may be reconnected.
/// Verify: line 92.
pub const IC_GSN_NF_COMPLETEREP: u16 = 27;
/// A node has connected. Verify: line 249.
pub const IC_GSN_CONNECT_REP: u16 = 163;

// Transactions.

/// Take a transaction record in a data node's coordinator.
/// Verify: line 107.
pub const IC_GSN_TCSEIZEREQ: u16 = 39;
/// The transaction record was taken. Verify: line 105.
pub const IC_GSN_TCSEIZECONF: u16 = 37;
/// The transaction record could not be taken. Verify: line 106.
pub const IC_GSN_TCSEIZEREF: u16 = 38;
/// Give back a transaction record. Verify: line 103.
pub const IC_GSN_TCRELEASEREQ: u16 = 36;
/// The record was given back. Verify: line 101.
pub const IC_GSN_TCRELEASECONF: u16 = 34;
/// The record could not be given back. Verify: line 102.
pub const IC_GSN_TCRELEASEREF: u16 = 35;
/// Commit a transaction that has no operation left to carry the flag.
/// Verify: line 75.
pub const IC_GSN_TC_COMMITREQ: u16 = 19;
/// The transaction committed. Verify: line 73.
pub const IC_GSN_TC_COMMITCONF: u16 = 17;
/// The transaction did not commit. Verify: line 74.
pub const IC_GSN_TC_COMMITREF: u16 = 18;
/// Roll a transaction back. Verify: line 70.
pub const IC_GSN_TCROLLBACKREQ: u16 = 15;
/// The transaction rolled back. Verify: line 68.
pub const IC_GSN_TCROLLBACKCONF: u16 = 13;
/// The rollback failed. Verify: line 69.
pub const IC_GSN_TCROLLBACKREF: u16 = 14;
/// The data node rolled the transaction back by itself, and says why.
/// Verify: line 71.
pub const IC_GSN_TCROLLBACKREP: u16 = 16;
/// Tell the data node we have seen the commit, so it may forget it.
/// Verify: line 617.
pub const IC_GSN_TC_COMMIT_ACK: u16 = 469;
/// A transaction's outcome after the coordinator's node failed.
/// Verify: line 62.
pub const IC_GSN_TCKEY_FAILCONF: u16 = 8;
/// As above, when the outcome was failure. Verify: line 63.
pub const IC_GSN_TCKEY_FAILREF: u16 = 9;
/// RonDB reports a deadlock before aborting. Verify: line 1330.
pub const IC_GSN_TC_DEADLOCK_REP: u16 = 980;

// Key operations.

/// A primary key operation: read, insert, update, delete or write.
/// Verify: line 66.
pub const IC_GSN_TCKEYREQ: u16 = 12;
/// One or more key operations succeeded. Verify: line 64.
pub const IC_GSN_TCKEYCONF: u16 = 10;
/// A key operation failed. Verify: line 65.
pub const IC_GSN_TCKEYREF: u16 = 11;
/// A unique index operation. Verify: line 685.
pub const IC_GSN_TCINDXREQ: u16 = 519;
/// A unique index operation succeeded. Verify: line 686.
pub const IC_GSN_TCINDXCONF: u16 = 520;
/// A unique index operation failed. Verify: line 687.
pub const IC_GSN_TCINDXREF: u16 = 521;
/// Row data for an operation we asked for. Verify: line 58.
pub const IC_GSN_TRANSID_AI: u16 = 5;
/// Key data continued in its own signal, for the short form.
/// Verify: line 59.
pub const IC_GSN_KEYINFO: u16 = 6;
/// Attribute data continued in its own signal. Verify: line 57.
pub const IC_GSN_ATTRINFO: u16 = 4;
/// The key of a scanned row, so the row can be operated on.
/// Verify: line 99.
pub const IC_GSN_KEYINFO20: u16 = 33;

// Scans, for a later release.

/// Start a scan. Verify: line 98.
pub const IC_GSN_SCAN_TABREQ: u16 = 32;
/// A batch of scanned rows is on its way. Verify: line 95.
pub const IC_GSN_SCAN_TABCONF: u16 = 29;
/// The scan failed. Verify: line 97.
pub const IC_GSN_SCAN_TABREF: u16 = 31;
/// Ask for the next batch, or stop the scan. Verify: line 94.
pub const IC_GSN_SCAN_NEXTREQ: u16 = 28;

// Dictionary.

/// Ask for a table's definition, by id or by name. Verify: line 82.
pub const IC_GSN_GET_TABINFOREQ: u16 = 24;
/// The table definition. Verify: line 83.
pub const IC_GSN_GET_TABINFO_CONF: u16 = 190;
/// There is no such table, or it cannot be read now. Verify: line 81.
pub const IC_GSN_GET_TABINFOREF: u16 = 23;
/// Ask for a list of tables. Verify: line 281.
pub const IC_GSN_LIST_TABLES_REQ: u16 = 193;
/// The list of tables. Verify: line 282.
pub const IC_GSN_LIST_TABLES_CONF: u16 = 194;
/// A table was altered or dropped, sent unasked to every API node.
/// Verify: line 836.
pub const IC_GSN_ALTER_TABLE_REP: u16 = 606;

// Every signal number has to fit the 16 bits the header gives it. The
// type already says so; this catches a number written as a wider
// constant by mistake.
const _: () = assert!(IC_GSN_TC_DEADLOCK_REP as u32 <= 0xFFFF);
const _: () = assert!(IC_GSN_TC_COMMIT_ACK as u32 <= 0xFFFF);

/// The name of a signal, for tracing. Unknown numbers have none, and
/// print as their number.
pub fn gsn_name(gsn: u16) -> Option<&'static str> {
  match gsn {
    IC_GSN_API_REGCONF => Some("API_REGCONF"),
    IC_GSN_API_REGREF => Some("API_REGREF"),
    IC_GSN_API_REGREQ => Some("API_REGREQ"),
    IC_GSN_NODE_FAILREP => Some("NODE_FAILREP"),
    IC_GSN_NF_COMPLETEREP => Some("NF_COMPLETEREP"),
    IC_GSN_CONNECT_REP => Some("CONNECT_REP"),
    IC_GSN_TCSEIZEREQ => Some("TCSEIZEREQ"),
    IC_GSN_TCSEIZECONF => Some("TCSEIZECONF"),
    IC_GSN_TCSEIZEREF => Some("TCSEIZEREF"),
    IC_GSN_TCRELEASEREQ => Some("TCRELEASEREQ"),
    IC_GSN_TCRELEASECONF => Some("TCRELEASECONF"),
    IC_GSN_TCRELEASEREF => Some("TCRELEASEREF"),
    IC_GSN_TC_COMMITREQ => Some("TC_COMMITREQ"),
    IC_GSN_TC_COMMITCONF => Some("TC_COMMITCONF"),
    IC_GSN_TC_COMMITREF => Some("TC_COMMITREF"),
    IC_GSN_TCROLLBACKREQ => Some("TCROLLBACKREQ"),
    IC_GSN_TCROLLBACKCONF => Some("TCROLLBACKCONF"),
    IC_GSN_TCROLLBACKREF => Some("TCROLLBACKREF"),
    IC_GSN_TCROLLBACKREP => Some("TCROLLBACKREP"),
    IC_GSN_TC_COMMIT_ACK => Some("TC_COMMIT_ACK"),
    IC_GSN_TCKEY_FAILCONF => Some("TCKEY_FAILCONF"),
    IC_GSN_TCKEY_FAILREF => Some("TCKEY_FAILREF"),
    IC_GSN_TC_DEADLOCK_REP => Some("TC_DEADLOCK_REP"),
    IC_GSN_TCKEYREQ => Some("TCKEYREQ"),
    IC_GSN_TCKEYCONF => Some("TCKEYCONF"),
    IC_GSN_TCKEYREF => Some("TCKEYREF"),
    IC_GSN_TCINDXREQ => Some("TCINDXREQ"),
    IC_GSN_TCINDXCONF => Some("TCINDXCONF"),
    IC_GSN_TCINDXREF => Some("TCINDXREF"),
    IC_GSN_TRANSID_AI => Some("TRANSID_AI"),
    IC_GSN_KEYINFO => Some("KEYINFO"),
    IC_GSN_ATTRINFO => Some("ATTRINFO"),
    IC_GSN_KEYINFO20 => Some("KEYINFO20"),
    IC_GSN_SCAN_TABREQ => Some("SCAN_TABREQ"),
    IC_GSN_SCAN_TABCONF => Some("SCAN_TABCONF"),
    IC_GSN_SCAN_TABREF => Some("SCAN_TABREF"),
    IC_GSN_SCAN_NEXTREQ => Some("SCAN_NEXTREQ"),
    IC_GSN_GET_TABINFOREQ => Some("GET_TABINFOREQ"),
    IC_GSN_GET_TABINFO_CONF => Some("GET_TABINFO_CONF"),
    IC_GSN_GET_TABINFOREF => Some("GET_TABINFOREF"),
    IC_GSN_LIST_TABLES_REQ => Some("LIST_TABLES_REQ"),
    IC_GSN_LIST_TABLES_CONF => Some("LIST_TABLES_CONF"),
    IC_GSN_ALTER_TABLE_REP => Some("ALTER_TABLE_REP"),
    _ => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn numbers_are_distinct() {
    let all = [
      IC_GSN_API_REGCONF,
      IC_GSN_API_REGREF,
      IC_GSN_API_REGREQ,
      IC_GSN_NODE_FAILREP,
      IC_GSN_NF_COMPLETEREP,
      IC_GSN_CONNECT_REP,
      IC_GSN_TCSEIZEREQ,
      IC_GSN_TCSEIZECONF,
      IC_GSN_TCSEIZEREF,
      IC_GSN_TCRELEASEREQ,
      IC_GSN_TC_COMMITREQ,
      IC_GSN_TCROLLBACKREQ,
      IC_GSN_TCROLLBACKREP,
      IC_GSN_TC_COMMIT_ACK,
      IC_GSN_TCKEYREQ,
      IC_GSN_TCKEYCONF,
      IC_GSN_TCKEYREF,
      IC_GSN_TCINDXREQ,
      IC_GSN_TRANSID_AI,
      IC_GSN_KEYINFO20,
      IC_GSN_SCAN_TABREQ,
      IC_GSN_GET_TABINFOREQ,
      IC_GSN_GET_TABINFO_CONF,
      IC_GSN_ALTER_TABLE_REP,
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
    assert_eq!(gsn_name(IC_GSN_TCKEYREQ), Some("TCKEYREQ"));
    assert_eq!(gsn_name(IC_GSN_API_REGREQ), Some("API_REGREQ"));
    assert_eq!(gsn_name(9999), None);
  }
}
