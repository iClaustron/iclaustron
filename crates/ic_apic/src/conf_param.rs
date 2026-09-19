// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The configuration parameter ids an API node needs.
//!
//! The C carried a 3 056-line table describing every parameter of every
//! node type, because it also wrote configurations. An API node only
//! reads one, and only cares about its own settings, how to reach the
//! data nodes, and how the links to them are set up. The rest of the
//! configuration is kept as it arrives, so nothing is lost, but only
//! these have names here.
//!
//! Every id is verified against RonDB 26.10
//! `storage/ndb/include/mgmapi/mgmapi_config_parameters.h`, with the
//! line given. The ids are interface: they are what travels in the
//! configuration blob.

/* Common to every section. Verify: lines 36-41. */

/// The node's id in the cluster. Verify: line 36.
pub const CFG_NODE_ID: u32 = 3;
/// The host the node runs on. Verify: line 38.
pub const CFG_NODE_HOST: u32 = 5;
/// The node's data directory. Verify: line 40.
pub const CFG_NODE_DATADIR: u32 = 7;
/// Bytes of send buffer shared across all links of the node.
/// Verify: line 41.
pub const CFG_TOTAL_SEND_BUFFER_MEMORY: u32 = 9;
/// Which kind of section this is, written in every section.
/// Verify: line 450.
pub const CFG_TYPE_OF_SECTION: u32 = 999;

/* Data node settings an API node reads. */

/// Replicas per fragment, which says how many copies of a row exist.
/// Verify: line 53.
pub const CFG_DB_NO_REPLICAS: u32 = 101;
/// How often a data node expects to hear from us, in milliseconds.
/// This sets our heartbeat period. Verify: line 74.
pub const CFG_DB_API_HEARTBEAT_INTERVAL: u32 = 119;
/// The node group a data node belongs to. Verify: line 156.
pub const CFG_DB_NODEGROUP: u32 = 185;

/* API node settings, which are ours. Verify: lines 433-439. */

/// Largest total size of a scan batch in bytes. Verify: line 433.
pub const CFG_MAX_SCAN_BATCH_SIZE: u32 = 800;
/// Bytes per batch in one scan. Verify: line 434.
pub const CFG_BATCH_BYTE_SIZE: u32 = 801;
/// Rows per batch in one scan. Verify: line 435.
pub const CFG_BATCH_SIZE: u32 = 802;
/// Whether to reconnect automatically after losing a data node.
/// Verify: line 436.
pub const CFG_AUTO_RECONNECT: u32 = 803;
/// What to do when the redo log is under pressure. Verify: line 438.
pub const CFG_DEFAULT_OPERATION_REDO_PROBLEM_ACTION: u32 = 805;
/// Default number of hash map buckets for new tables. Verify: line 439.
pub const CFG_DEFAULT_HASHMAP_SIZE: u32 = 806;
/// Our rank as an arbitrator, 0 meaning we never arbitrate.
/// Verify: line 328.
pub const CFG_NODE_ARBIT_RANK: u32 = 200;
/// Delay before we would act as arbitrator. Verify: line 329.
pub const CFG_NODE_ARBIT_DELAY: u32 = 201;

/* Communication sections: one per pair of nodes that talk.
Verify: lines 400-414. */

/// The lower numbered node of the pair. Verify: line 400.
pub const CFG_CONNECTION_NODE_1: u32 = 400;
/// The higher numbered node of the pair. Verify: line 401.
pub const CFG_CONNECTION_NODE_2: u32 = 401;
/// Whether every signal carries an id, for tracing. Verify: line 402.
pub const CFG_CONNECTION_SEND_SIGNAL_ID: u32 = 402;
/// Whether every signal carries a checksum. Verify: line 403.
pub const CFG_CONNECTION_CHECKSUM: u32 = 403;
/// The port the server side of the link listens on; 0 means it is
/// assigned dynamically and must be asked for. Verify: line 406.
pub const CFG_CONNECTION_SERVER_PORT: u32 = 406;
/// Host name of node 1 of the pair. Verify: line 407.
pub const CFG_CONNECTION_HOSTNAME_1: u32 = 407;
/// Host name of node 2 of the pair. Verify: line 408.
pub const CFG_CONNECTION_HOSTNAME_2: u32 = 408;
/// Which of the two nodes listens; the other one dials.
/// Verify: line 410.
pub const CFG_CONNECTION_NODE_ID_SERVER: u32 = 410;
/// Bytes of send buffer before the link counts as overloaded.
/// Verify: line 411.
pub const CFG_CONNECTION_OVERLOAD: u32 = 411;
/// Whether a checksum is computed before sending rather than after.
/// Verify: line 412.
pub const CFG_CONNECTION_PRESEND_CHECKSUM: u32 = 412;
/// Which IP version to prefer when a host resolves to both.
/// Verify: line 414.
pub const CFG_CONNECTION_PREFER_IP_VER: u32 = 414;

/* TCP link settings. Verify: lines 452-499. */

/// Bytes of send buffer for this link. Verify: line 454.
pub const CFG_TCP_SEND_BUFFER_SIZE: u32 = 454;
/// Bytes of receive buffer for this link. Verify: line 455.
pub const CFG_TCP_RECEIVE_BUFFER_SIZE: u32 = 455;
/// `SO_RCVBUF` for the socket. Verify: line 457.
pub const CFG_TCP_RCV_BUF_SIZE: u32 = 457;
/// `SO_SNDBUF` for the socket. Verify: line 458.
pub const CFG_TCP_SND_BUF_SIZE: u32 = 458;
/// `TCP_MAXSEG` for the socket. Verify: line 459.
pub const CFG_TCP_MAXSEG_SIZE: u32 = 459;
/// Whether the listening side binds every address. Verify: line 460.
pub const CFG_TCP_BIND_INADDR_ANY: u32 = 460;
/// Whether this link must use TLS. Verify: line 462.
pub const CFG_TCP_REQUIRE_TLS: u32 = 462;
/// Whether to use IPv4 only on this link. Verify: line 499.
pub const CFG_TCP_ONLY_IPV4: u32 = 499;

/// The name of a parameter id, for printing a configuration. Unknown
/// ids print as their number, which is what a diagnostic needs.
pub fn parameter_name(id: u32) -> Option<&'static str> {
  match id {
    CFG_NODE_ID => Some("NodeId"),
    CFG_NODE_HOST => Some("HostName"),
    CFG_NODE_DATADIR => Some("DataDir"),
    CFG_TOTAL_SEND_BUFFER_MEMORY => Some("TotalSendBufferMemory"),
    CFG_TYPE_OF_SECTION => Some("TypeOfSection"),
    CFG_DB_NO_REPLICAS => Some("NoOfReplicas"),
    CFG_DB_API_HEARTBEAT_INTERVAL => Some("HeartbeatIntervalDbApi"),
    CFG_DB_NODEGROUP => Some("Nodegroup"),
    CFG_MAX_SCAN_BATCH_SIZE => Some("MaxScanBatchSize"),
    CFG_BATCH_BYTE_SIZE => Some("BatchByteSize"),
    CFG_BATCH_SIZE => Some("BatchSize"),
    CFG_AUTO_RECONNECT => Some("AutoReconnect"),
    CFG_DEFAULT_OPERATION_REDO_PROBLEM_ACTION => {
      Some("DefaultOperationRedoProblemAction")
    }
    CFG_DEFAULT_HASHMAP_SIZE => Some("DefaultHashMapSize"),
    CFG_NODE_ARBIT_RANK => Some("ArbitrationRank"),
    CFG_NODE_ARBIT_DELAY => Some("ArbitrationDelay"),
    CFG_CONNECTION_NODE_1 => Some("NodeId1"),
    CFG_CONNECTION_NODE_2 => Some("NodeId2"),
    CFG_CONNECTION_SEND_SIGNAL_ID => Some("SendSignalId"),
    CFG_CONNECTION_CHECKSUM => Some("Checksum"),
    CFG_CONNECTION_SERVER_PORT => Some("PortNumber"),
    CFG_CONNECTION_HOSTNAME_1 => Some("HostName1"),
    CFG_CONNECTION_HOSTNAME_2 => Some("HostName2"),
    CFG_CONNECTION_NODE_ID_SERVER => Some("NodeIdServer"),
    CFG_CONNECTION_OVERLOAD => Some("OverloadLimit"),
    CFG_CONNECTION_PRESEND_CHECKSUM => Some("PreSendChecksum"),
    CFG_CONNECTION_PREFER_IP_VER => Some("PreferIPVersion"),
    CFG_TCP_SEND_BUFFER_SIZE => Some("SendBufferMemory"),
    CFG_TCP_RECEIVE_BUFFER_SIZE => Some("ReceiveBufferMemory"),
    CFG_TCP_RCV_BUF_SIZE => Some("TCP_RCV_BUF_SIZE"),
    CFG_TCP_SND_BUF_SIZE => Some("TCP_SND_BUF_SIZE"),
    CFG_TCP_MAXSEG_SIZE => Some("TCP_MAXSEG_SIZE"),
    CFG_TCP_BIND_INADDR_ANY => Some("TcpBind_INADDR_ANY"),
    CFG_TCP_REQUIRE_TLS => Some("RequireTls"),
    CFG_TCP_ONLY_IPV4 => Some("UseOnlyIPv4"),
    _ => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn ids_are_distinct() {
    let ids = [
      CFG_NODE_ID,
      CFG_NODE_HOST,
      CFG_NODE_DATADIR,
      CFG_TOTAL_SEND_BUFFER_MEMORY,
      CFG_DB_NO_REPLICAS,
      CFG_DB_API_HEARTBEAT_INTERVAL,
      CFG_DB_NODEGROUP,
      CFG_MAX_SCAN_BATCH_SIZE,
      CFG_BATCH_BYTE_SIZE,
      CFG_BATCH_SIZE,
      CFG_CONNECTION_NODE_1,
      CFG_CONNECTION_NODE_2,
      CFG_CONNECTION_SERVER_PORT,
      CFG_TCP_MAXSEG_SIZE,
      CFG_TYPE_OF_SECTION,
    ];
    let mut i: usize = 0;
    while i < ids.len() {
      let mut j = i + 1;
      while j < ids.len() {
        assert_ne!(ids[i], ids[j], "duplicate id {}", ids[i]);
        j += 1;
      }
      i += 1;
    }
  }

  #[test]
  fn every_named_id_has_a_name() {
    assert_eq!(parameter_name(CFG_NODE_ID), Some("NodeId"));
    assert_eq!(
      parameter_name(CFG_CONNECTION_SERVER_PORT),
      Some("PortNumber")
    );
    assert_eq!(parameter_name(999_999), None);
  }
}
