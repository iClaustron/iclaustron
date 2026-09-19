// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The notice a data node sends every API node when a table has been
//! altered or dropped, so that a cached description can be let go at
//! once instead of when an operation on it fails.
//!
//! It is addressed to the cluster manager block, not to any user
//! thread, and nobody asked for it.
//!
//! **Altered: once from every data node.** The dictionary of each data
//! node sends it, through that node's cluster manager, to every API node
//! registered there, so the same notice arrives once per data node. The
//! version it names is the one being replaced, and the name is the name
//! before the change, which differs from the new one after a rename.
//!
//! **Dropped: once, from the master.** Only to API nodes that report a
//! version new enough: 26.05 and later, or 26.02.5 and 25.10.14 in
//! their series. This library reports 26.10.
//!
//! Three words: table id, table version, kind of change. Section 0 is
//! the table's internal name, NUL-padded to 128 bytes.
//!
//! Verify: `AlterTable.hpp`, `AlterTableRep`; `Dbdict.cpp`,
//! `alterTable_fromCommitComplete` and `dropTable_commit`;
//! `QmgrMain.cpp`, `execAPI_BROADCAST_REP`; `ndb_version.h.in`,
//! `ndbd_support_drop_table_notification`; `ClusterMgr.cpp`, where the
//! reference hands it to its dictionary cache.

use ic_port::err;
use ic_port::IcError;

/// Words in an `ALTER_TABLE_REP`.
pub const IC_ALTER_TABLE_REP_LEN: usize = 3;
/// Kind of change: the table was altered.
pub const IC_ALTER_TABLE_CHANGE_ALTERED: u32 = 1;
/// Kind of change: the table was dropped.
pub const IC_ALTER_TABLE_CHANGE_DROPPED: u32 = 2;

/// A table was altered or dropped.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AlterTableRep {
  /// Its id.
  pub table_id: u32,
  /// The version that no longer holds: the one before an alteration,
  /// or the one dropped.
  pub table_version: u32,
  /// One of the `IC_ALTER_TABLE_CHANGE_*` values.
  pub change_type: u32,
  /// Its internal name before the change, `database/schema/table`, or
  /// `sys/def/<table id>/<index>` for an index.
  pub name: String,
}

impl AlterTableRep {
  /// Read a notice from its data and its section 0.
  pub fn decode(data: &[u32], name: &[u32]) -> Result<AlterTableRep, IcError> {
    if data.len() < IC_ALTER_TABLE_REP_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(AlterTableRep {
      table_id: data[0],
      table_version: data[1],
      change_type: data[2],
      name: text_of_words(name),
    })
  }

  /// True if the table was dropped rather than altered.
  pub fn is_drop(&self) -> bool {
    self.change_type == IC_ALTER_TABLE_CHANGE_DROPPED
  }
}

/// A name from words holding its bytes as they are, up to its NUL.
fn text_of_words(words: &[u32]) -> String {
  let mut bytes: Vec<u8> = Vec::with_capacity(words.len() * 4);
  for word in words {
    bytes.extend_from_slice(&word.to_ne_bytes());
  }
  let mut end: usize = 0;
  while end < bytes.len() && bytes[end] != 0 {
    end += 1;
  }
  bytes.truncate(end);
  String::from_utf8_lossy(&bytes).into_owned()
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::get_tab_info::name_section;

  /// The name as the dictionary sends it: padded with NULs to 128 bytes.
  fn padded(name: &str) -> Vec<u32> {
    let mut words = name_section(name);
    while words.len() < 32 {
      words.push(0);
    }
    words
  }

  #[test]
  fn an_alteration_is_read() {
    let rep = AlterTableRep::decode(&[13, 1, 1], &padded("ictest/def/t1"))
      .expect("read");
    assert_eq!(rep.table_id, 13);
    assert_eq!(rep.table_version, 1);
    assert!(!rep.is_drop());
    assert_eq!(rep.name, "ictest/def/t1");
  }

  #[test]
  fn a_drop_is_told_apart() {
    let rep = AlterTableRep::decode(&[13, 1, 2], &padded("ictest/def/t1"))
      .expect("read");
    assert!(rep.is_drop());
  }

  #[test]
  fn a_short_notice_is_an_error() {
    assert!(AlterTableRep::decode(&[13, 1], &[]).is_err());
  }
}
