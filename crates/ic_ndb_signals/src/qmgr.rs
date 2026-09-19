// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Cluster membership: the heartbeat we send to each data node and what
//! it tells us in return.
//!
//! An API node sends `API_REGREQ` to the cluster manager block of every
//! data node it is connected to. The answer, `API_REGCONF`, is how we
//! learn whether that node is started, which node group it is in, and
//! which other nodes it can see. Missing several answers in a row is
//! how we learn a node has died, and a data node that stops hearing
//! from us declares us dead in the same way.
//!
//! Verify: RonDB 26.10
//! `include/kernel/signaldata/ApiRegSignalData.hpp:34` and
//! `include/kernel/NodeState.hpp:113`.

use ic_port::err;
use ic_port::IcError;

/// Words of signal data in an `API_REGREQ`.
pub const IC_API_REGREQ_LEN: usize = 3;
/// Words of signal data in an `API_REGREF`.
pub const IC_API_REGREF_LEN: usize = 4;
/// Words in the node state carried by an `API_REGCONF`.
/// Verify: `NodeState.hpp:113`, `DataLength`.
pub const IC_NODE_STATE_LEN: usize = 16;
/// Words of signal data in an `API_REGCONF`.
pub const IC_API_REGCONF_LEN: usize = 6 + IC_NODE_STATE_LEN;
/// Words of the connected-node bitmap inside the node state.
///
/// This is 256 bits, and stays 256 bits however far node ids rise: it
/// only has to cover data nodes, which keep the low numbers.
/// Verify: `NodeState.hpp`, `NodeBitmask255::Size`.
pub const IC_CONNECTED_NODES_WORDS: usize = 8;

/// How far through starting a node is.
/// Verify: `NodeState.hpp`, `StartLevel`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum StartLevel {
  /// Not started at all.
  #[default]
  Nothing = 0,
  /// The virtual machine is up but nothing else.
  Cmvmi = 1,
  /// Starting, and not yet able to serve us.
  Starting = 2,
  /// Started and serving.
  Started = 3,
  /// Started, but only one API node may use it.
  SingleUser = 4,
  /// Stopping, first phase.
  Stopping1 = 5,
  /// Stopping, second phase.
  Stopping2 = 6,
  /// Stopping, third phase.
  Stopping3 = 7,
  /// Stopping, last phase.
  Stopping4 = 8,
}

impl StartLevel {
  /// The level a word names.
  pub fn from_u32(value: u32) -> StartLevel {
    match value {
      1 => StartLevel::Cmvmi,
      2 => StartLevel::Starting,
      3 => StartLevel::Started,
      4 => StartLevel::SingleUser,
      5 => StartLevel::Stopping1,
      6 => StartLevel::Stopping2,
      7 => StartLevel::Stopping3,
      8 => StartLevel::Stopping4,
      _ => StartLevel::Nothing,
    }
  }

  /// True when the node can serve transactions.
  pub fn is_started(&self) -> bool {
    matches!(self, StartLevel::Started | StartLevel::SingleUser)
  }

  /// True when the node is on its way down.
  pub fn is_stopping(&self) -> bool {
    matches!(
      self,
      StartLevel::Stopping1
        | StartLevel::Stopping2
        | StartLevel::Stopping3
        | StartLevel::Stopping4
    )
  }
}

/// Why a data node refused our heartbeat.
/// Verify: `ApiRegSignalData.hpp`, `ApiRegRef::ErrorCode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ApiRegRefError {
  /// We claimed to be a kind of node we are not configured as.
  WrongType = 1,
  /// Our version and the node's cannot work together.
  UnsupportedVersion = 2,
  /// This node id is not in the cluster's configuration.
  UnconfiguredNode = 3,
  /// Something the node reported that we do not know about.
  Unknown = 0,
}

impl ApiRegRefError {
  /// The reason a word names.
  pub fn from_u32(value: u32) -> ApiRegRefError {
    match value {
      1 => ApiRegRefError::WrongType,
      2 => ApiRegRefError::UnsupportedVersion,
      3 => ApiRegRefError::UnconfiguredNode,
      _ => ApiRegRefError::Unknown,
    }
  }
}

/// What a data node says about itself.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NodeState {
  /// How far through starting the node is.
  pub start_level: StartLevel,
  /// Which node group it belongs to, while starting.
  pub node_group: u32,
  /// Its dynamic id, or the master's node id depending on the sender.
  pub dynamic_id: u32,
  /// Which start phase it has reached, while starting.
  pub start_phase: u32,
  /// What kind of restart it is performing.
  pub restart_type: u32,
  /// True when the whole cluster is shutting down.
  pub system_shutdown: bool,
  /// True when the cluster is in single user mode.
  pub single_user_mode: bool,
  /// The one API node allowed in single user mode.
  pub single_user_api: u32,
  /// Which nodes this node can see, as a bitmap over node ids.
  pub connected_nodes: [u32; IC_CONNECTED_NODES_WORDS],
}

impl NodeState {
  /// Read the node state out of the words that carry it.
  pub fn decode(words: &[u32]) -> Result<NodeState, IcError> {
    if words.len() < IC_NODE_STATE_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let mut connected = [0u32; IC_CONNECTED_NODES_WORDS];
    let mut i: usize = 0;
    while i < IC_CONNECTED_NODES_WORDS {
      connected[i] = words[8 + i];
      i += 1;
    }
    Ok(NodeState {
      start_level: StartLevel::from_u32(words[0]),
      node_group: words[1],
      dynamic_id: words[2],
      // Words 3 to 5 are one field read three ways, depending on what
      // the node is doing: starting, stopping, or neither.
      start_phase: words[3],
      restart_type: words[4],
      system_shutdown: words[3] != 0,
      single_user_mode: words[6] != 0,
      single_user_api: words[7],
      connected_nodes: connected,
    })
  }

  /// True if this node can see the node with that id.
  pub fn is_connected_to(&self, node_id: u32) -> bool {
    let word = (node_id / 32) as usize;
    if word >= IC_CONNECTED_NODES_WORDS {
      // Node ids above 255 are not in this bitmap, which only covers
      // the data nodes.
      return false;
    }
    (self.connected_nodes[word] & (1 << (node_id & 31))) != 0
  }

  /// The nodes this node can see.
  pub fn connected_node_ids(&self) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::new();
    let mut node_id: u32 = 0;
    while node_id < (IC_CONNECTED_NODES_WORDS as u32) * 32 {
      if self.is_connected_to(node_id) {
        out.push(node_id);
      }
      node_id += 1;
    }
    out
  }
}

/// Our heartbeat to a data node.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ApiRegReq {
  /// Our block reference, so the node knows where to answer.
  pub block_ref: u32,
  /// Our NDB version.
  pub version: u32,
  /// Our MySQL version.
  pub mysql_version: u32,
}

impl ApiRegReq {
  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_API_REGREQ_LEN] {
    [self.block_ref, self.version, self.mysql_version]
  }
}

/// A data node's answer to our heartbeat.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ApiRegConf {
  /// The cluster manager block that answered.
  pub qmgr_ref: u32,
  /// The node's NDB version.
  pub version: u32,
  /// How often the node expects to hear from us, in hundredths of a
  /// second. Use [`heartbeat_interval_ms`](ApiRegConf::heartbeat_interval_ms)
  /// rather than this field directly.
  ///
  /// The configuration states `HeartbeatIntervalDbApi` in
  /// milliseconds, and the signal carries a tenth of it: a cluster
  /// configured for 30000 ms answers 3000 here. Confirmed against a
  /// live RonDB 26.10 cluster, and matching the C++ cluster manager,
  /// which multiplies this field by ten.
  pub api_heartbeat_interval: u32,
  /// The node's MySQL version.
  pub mysql_version: u32,
  /// The oldest version among the data nodes, which limits what the
  /// cluster as a whole can do.
  pub min_db_version: u32,
  /// The oldest version among the API nodes.
  pub min_api_version: u32,
  /// What the node says about itself.
  pub node_state: NodeState,
}

impl ApiRegConf {
  /// How often the node expects to hear from us, in milliseconds.
  pub fn heartbeat_interval_ms(&self) -> u32 {
    self.api_heartbeat_interval * 10
  }

  /// Read the answer out of the words of a signal.
  pub fn decode(words: &[u32]) -> Result<ApiRegConf, IcError> {
    if words.len() < IC_API_REGCONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(ApiRegConf {
      qmgr_ref: words[0],
      version: words[1],
      api_heartbeat_interval: words[2],
      mysql_version: words[3],
      min_db_version: words[4],
      node_state: NodeState::decode(&words[5..5 + IC_NODE_STATE_LEN])?,
      min_api_version: words[5 + IC_NODE_STATE_LEN],
    })
  }

  /// Write the answer as a data node would, for tests.
  pub fn encode(&self) -> [u32; IC_API_REGCONF_LEN] {
    let mut words = [0u32; IC_API_REGCONF_LEN];
    words[0] = self.qmgr_ref;
    words[1] = self.version;
    words[2] = self.api_heartbeat_interval;
    words[3] = self.mysql_version;
    words[4] = self.min_db_version;
    let state = &self.node_state;
    words[5] = state.start_level as u32;
    words[6] = state.node_group;
    words[7] = state.dynamic_id;
    words[8] = state.start_phase;
    words[9] = state.restart_type;
    words[11] = u32::from(state.single_user_mode);
    words[12] = state.single_user_api;
    let mut i: usize = 0;
    while i < IC_CONNECTED_NODES_WORDS {
      words[13 + i] = state.connected_nodes[i];
      i += 1;
    }
    words[5 + IC_NODE_STATE_LEN] = self.min_api_version;
    words
  }
}

/// A data node's refusal of our heartbeat.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ApiRegRef {
  /// The cluster manager block that refused.
  pub qmgr_ref: u32,
  /// The node's NDB version.
  pub version: u32,
  /// Why it refused.
  pub error_code: u32,
  /// The node's MySQL version.
  pub mysql_version: u32,
}

impl ApiRegRef {
  /// Read the refusal out of the words of a signal.
  pub fn decode(words: &[u32]) -> Result<ApiRegRef, IcError> {
    if words.len() < IC_API_REGREF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(ApiRegRef {
      qmgr_ref: words[0],
      version: words[1],
      error_code: words[2],
      mysql_version: words[3],
    })
  }

  /// Why the node refused.
  pub fn reason(&self) -> ApiRegRefError {
    ApiRegRefError::from_u32(self.error_code)
  }
}

/// Words of signal data in an `NF_COMPLETEREP`.
/// Verify: `NFCompleteRep.hpp`, `SignalLength`.
pub const IC_NF_COMPLETEREP_LEN: usize = 5;

/// Words of signal data in the modern `NODE_FAILREP`, whose bitmap of
/// failed nodes travels in a section rather than in the signal.
/// Verify: `NodeFailRep.hpp`, `SignalLength`.
pub const IC_NODE_FAILREP_LEN: usize = 3;

/// One or more nodes have failed.
///
/// The bitmap of failed nodes is carried in one of three ways, which is
/// why its length is never assumed: in the modern form the signal data
/// is three words and the bitmap is in section 0; in the two older
/// forms it follows the three words inside the signal, sized either for
/// data nodes alone or for every node. The length is always the signal
/// data length less three.
/// Verify: `NodeFailRep.hpp`, `getNodeMaskLength`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NodeFailRep {
  /// Which failure this is, counting up as nodes fail.
  pub fail_number: u32,
  /// The master data node, set only when the report comes from the
  /// start and stop block rather than the cluster manager.
  pub master_node_id: u32,
  /// How many nodes the bitmap names.
  pub num_nodes: u32,
  /// The nodes that failed.
  pub failed_nodes: Vec<u32>,
}

impl NodeFailRep {
  /// Read the report. `section` is section 0 of the signal, which is
  /// where the bitmap lives unless the signal carries it inline.
  pub fn decode(data: &[u32], section: &[u32]) -> Result<NodeFailRep, IcError> {
    if data.len() < IC_NODE_FAILREP_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    // Anything past the three fixed words is the bitmap; if there is
    // nothing past them, it came in the section.
    let bitmap: &[u32] = if data.len() > IC_NODE_FAILREP_LEN {
      &data[IC_NODE_FAILREP_LEN..]
    } else {
      section
    };
    Ok(NodeFailRep {
      fail_number: data[0],
      master_node_id: data[1],
      num_nodes: data[2],
      failed_nodes: nodes_in_bitmap(bitmap),
    })
  }
}

/// The node ids set in a bitmap of any length.
pub fn nodes_in_bitmap(bitmap: &[u32]) -> Vec<u32> {
  let mut out: Vec<u32> = Vec::new();
  let mut word: usize = 0;
  while word < bitmap.len() {
    let mut bit: u32 = 0;
    while bit < 32 {
      if (bitmap[word] & (1 << bit)) != 0 {
        out.push((word as u32) * 32 + bit);
      }
      bit += 1;
    }
    word += 1;
  }
  out
}

/// A failed node's work has been taken over, so it may be reconnected.
///
/// Every surviving data node sends one to every API node it has
/// registered, once all of its blocks have finished handling the
/// failure. It is what lets an API node abort the transactions that
/// were waiting on the failed node, and until one arrives, reconnecting
/// to that node is premature.
///
/// **The block field does not say "whole node" by being zero.** Inside
/// a data node these reports travel block to block and zero has that
/// meaning there, but the one that reaches an API node is sent by the
/// cluster manager block with its own reference in the field. An API
/// node acts on the failed node id alone and ignores the block. An
/// earlier version of this decoder tested for zero and would have
/// ignored every real report.
/// Verify: `NFCompleteRep.hpp`; `QmgrMain.cpp`, `execNDB_FAILCONF`
/// (the send to API nodes); `ClusterMgr.cpp`, `execNF_COMPLETEREP`
/// (the receiver, which reads only the failed node id).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NfCompleteRep {
  /// The block that finished. Informational: see the note above.
  pub block_no: u32,
  /// The node reporting.
  pub node_id: u32,
  /// The node that failed.
  pub failed_node_id: u32,
  /// Where the report came from.
  pub from: u32,
}

impl NfCompleteRep {
  /// Read the report.
  pub fn decode(data: &[u32]) -> Result<NfCompleteRep, IcError> {
    if data.len() < IC_NF_COMPLETEREP_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(NfCompleteRep {
      block_no: data[0],
      node_id: data[1],
      failed_node_id: data[2],
      from: data[4],
    })
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::blocks::*;

  #[test]
  fn a_heartbeat_carries_three_words() {
    let request = ApiRegReq {
      block_ref: number_to_ref(IC_BLOCK_API_CLUSTERMGR, 192),
      version: 0x1A_0A00,
      mysql_version: 0x1A_0A00,
    };
    let words = request.encode();
    assert_eq!(words.len(), IC_API_REGREQ_LEN);
    assert_eq!(ref_to_node(words[0]), 192);
    assert_eq!(ref_to_block(words[0]), IC_BLOCK_API_CLUSTERMGR);
  }

  fn started_node(node_group: u32, connected: &[u32]) -> ApiRegConf {
    let mut state = NodeState {
      start_level: StartLevel::Started,
      node_group,
      dynamic_id: 7,
      single_user_api: 0,
      ..NodeState::default()
    };
    for node_id in connected {
      let word = (*node_id / 32) as usize;
      state.connected_nodes[word] |= 1 << (*node_id & 31);
    }
    ApiRegConf {
      qmgr_ref: number_to_ref(IC_BLOCK_QMGR, 2),
      version: 0x1A_0A00,
      api_heartbeat_interval: 3000,
      mysql_version: 0x1A_0A00,
      min_db_version: 0x1A_0A00,
      min_api_version: 0x1A_0A00,
      node_state: state,
    }
  }

  #[test]
  fn an_answer_round_trips() {
    let original = started_node(0, &[1, 2, 192]);
    let words = original.encode();
    assert_eq!(words.len(), IC_API_REGCONF_LEN);
    assert_eq!(words.len(), 22);
    let decoded = ApiRegConf::decode(&words).expect("decode");
    assert_eq!(decoded.qmgr_ref, original.qmgr_ref);
    assert_eq!(ref_to_node(decoded.qmgr_ref), 2);
    assert_eq!(decoded.api_heartbeat_interval, 3000);
    // The configuration would call this 30000 milliseconds.
    assert_eq!(decoded.heartbeat_interval_ms(), 30000);
    assert_eq!(decoded.min_db_version, 0x1A_0A00);
    assert_eq!(decoded.min_api_version, 0x1A_0A00);
    assert_eq!(decoded.node_state.start_level, StartLevel::Started);
    assert!(decoded.node_state.start_level.is_started());
    assert_eq!(decoded.node_state.node_group, 0);
    assert_eq!(decoded.node_state.dynamic_id, 7);
  }

  #[test]
  fn the_connected_bitmap_is_read() {
    let conf = started_node(0, &[1, 2, 192]);
    let words = conf.encode();
    let decoded = ApiRegConf::decode(&words).expect("decode");
    let state = decoded.node_state;
    assert!(state.is_connected_to(1));
    assert!(state.is_connected_to(2));
    assert!(state.is_connected_to(192));
    assert!(!state.is_connected_to(3));
    assert_eq!(state.connected_node_ids(), vec![1, 2, 192]);
    // The bitmap covers 256 node ids, which is all the data nodes
    // there can be; larger API node ids simply are not in it.
    assert!(!state.is_connected_to(1600));
    assert!(!state.is_connected_to(255));
  }

  #[test]
  fn start_levels_are_classified() {
    assert!(StartLevel::Started.is_started());
    assert!(StartLevel::SingleUser.is_started());
    assert!(!StartLevel::Starting.is_started());
    assert!(!StartLevel::Nothing.is_started());
    assert!(StartLevel::Stopping1.is_stopping());
    assert!(StartLevel::Stopping4.is_stopping());
    assert!(!StartLevel::Started.is_stopping());
    assert_eq!(StartLevel::from_u32(3), StartLevel::Started);
    assert_eq!(StartLevel::from_u32(99), StartLevel::Nothing);
  }

  #[test]
  fn a_refusal_says_why() {
    let words = [number_to_ref(IC_BLOCK_QMGR, 2), 0x1A_0A00, 2, 0x1A_0A00];
    let refusal = ApiRegRef::decode(&words).expect("decode");
    assert_eq!(refusal.reason(), ApiRegRefError::UnsupportedVersion);
    let words = [0, 0, 3, 0];
    let refusal = ApiRegRef::decode(&words).expect("decode");
    assert_eq!(refusal.reason(), ApiRegRefError::UnconfiguredNode);
    let words = [0, 0, 77, 0];
    let refusal = ApiRegRef::decode(&words).expect("decode");
    assert_eq!(refusal.reason(), ApiRegRefError::Unknown);
  }

  fn bitmap_of(nodes: &[u32], words: usize) -> Vec<u32> {
    let mut bitmap = vec![0u32; words];
    for node in nodes {
      bitmap[(*node / 32) as usize] |= 1 << (*node & 31);
    }
    bitmap
  }

  #[test]
  fn a_failure_report_in_any_of_its_three_shapes() {
    // Modern: three words and the bitmap in a section.
    let section = bitmap_of(&[2], 64);
    let report = NodeFailRep::decode(&[7, 1, 1], &section).expect("modern");
    assert_eq!(report.fail_number, 7);
    assert_eq!(report.master_node_id, 1);
    assert_eq!(report.num_nodes, 1);
    assert_eq!(report.failed_nodes, vec![2]);

    // Older, data nodes only: the bitmap follows the three words.
    let mut data = vec![7u32, 1, 2];
    data.extend_from_slice(&bitmap_of(&[2, 3], 2));
    let report = NodeFailRep::decode(&data, &[]).expect("inline short");
    assert_eq!(report.failed_nodes, vec![2, 3]);

    // Older, every node: a longer bitmap, same place.
    let mut data = vec![7u32, 1, 2];
    data.extend_from_slice(&bitmap_of(&[2, 192], 64));
    let report = NodeFailRep::decode(&data, &[]).expect("inline long");
    assert_eq!(report.failed_nodes, vec![2, 192]);

    // An inline bitmap wins over a section, since a signal carrying
    // both would be malformed and the inline one is what it claims.
    let report = NodeFailRep::decode(&data, &section).expect("both");
    assert_eq!(report.failed_nodes, vec![2, 192]);
    assert!(NodeFailRep::decode(&[7, 1], &section).is_err());
  }

  #[test]
  fn a_takeover_report_names_the_node_that_is_done() {
    // As a data node sends it to us: its cluster manager block in the
    // first word, not zero. The failed node id is what matters.
    let qmgr_ref = number_to_ref(IC_BLOCK_QMGR, 1);
    let report =
      NfCompleteRep::decode(&[qmgr_ref, 1, 2, 0, 4870]).expect("dec");
    assert_eq!(report.block_no, qmgr_ref);
    assert_eq!(report.node_id, 1);
    assert_eq!(report.failed_node_id, 2);
    assert_eq!(report.from, 4870);
    // One word short is not a report.
    assert!(NfCompleteRep::decode(&[qmgr_ref, 1, 2, 0]).is_err());
  }

  #[test]
  fn bitmaps_of_any_length_are_read() {
    assert_eq!(nodes_in_bitmap(&[]), Vec::<u32>::new());
    assert_eq!(nodes_in_bitmap(&[0b1010]), vec![1, 3]);
    assert_eq!(nodes_in_bitmap(&[0, 1]), vec![32]);
    // A bitmap sized for every node reaches far past 255.
    let wide = bitmap_of(&[1600], 64);
    assert_eq!(nodes_in_bitmap(&wide), vec![1600]);
  }

  #[test]
  fn short_signals_are_refused() {
    assert!(ApiRegConf::decode(&[0u32; 21]).is_err());
    assert!(ApiRegRef::decode(&[0u32; 3]).is_err());
    assert!(NodeState::decode(&[0u32; 15]).is_err());
    assert!(ApiRegConf::decode(&[0u32; 22]).is_ok());
  }
}
