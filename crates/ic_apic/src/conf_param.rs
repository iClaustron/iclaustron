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

use crate::conf_blob::SectionType;

/* Common to every section. Verify: lines 36-41. */

/// The node's id in the cluster. Verify: line 36.
pub const IC_CFG_NODE_ID: u32 = 3;
/// The host the node runs on. Verify: line 38.
pub const IC_CFG_NODE_HOST: u32 = 5;
/// The node's data directory. Verify: line 40.
pub const IC_CFG_NODE_DATADIR: u32 = 7;
/// Whether the node id is reserved for one host. Verify: line 43.
pub const IC_CFG_NODE_DEDICATED: u32 = 11;
/// Bytes of send buffer shared across all links of the node.
/// Verify: line 41.
pub const IC_CFG_TOTAL_SEND_BUFFER_MEMORY: u32 = 9;
/// Which kind of section this is, written in every section.
/// Verify: line 450.
pub const IC_CFG_TYPE_OF_SECTION: u32 = 999;

// The system section, which describes the cluster itself. These ids
// overlap with the node ids above and mean something else: key 3 is the
// cluster's name here, not a node id. Verify: lines 30-34.

/// The management node that holds the master copy of the configuration.
/// Verify: line 31.
pub const IC_CFG_SYS_PRIMARY_MGM_NODE: u32 = 1;
/// Which generation of the configuration this is. Verify: line 32.
pub const IC_CFG_SYS_CONFIG_GENERATION: u32 = 2;
/// The cluster's name. Verify: line 30.
pub const IC_CFG_SYS_NAME: u32 = 3;
/// Whether API nodes may reach data nodes over RDMA, which RonDB adds.
/// Verify: line 33.
pub const IC_CFG_SYS_ALLOW_API_TO_DB_RDMA: u32 = 4;
/// The base port the cluster allocates from. Verify: line 34.
pub const IC_CFG_SYS_PORT_BASE: u32 = 8;

/* Data node settings an API node reads. */

/// Replicas per fragment, which says how many copies of a row exist.
/// Verify: line 53.
pub const IC_CFG_DB_NO_REPLICAS: u32 = 101;
/// How often a data node expects to hear from us, in milliseconds.
/// This sets our heartbeat period. Verify: line 74.
pub const IC_CFG_DB_API_HEARTBEAT_INTERVAL: u32 = 119;
/// The node group a data node belongs to. Verify: line 156.
pub const IC_CFG_DB_NODEGROUP: u32 = 185;

/* API node settings, which are ours. Verify: lines 433-439. */

/// Largest total size of a scan batch in bytes. Verify: line 433.
pub const IC_CFG_MAX_SCAN_BATCH_SIZE: u32 = 800;
/// Bytes per batch in one scan. Verify: line 434.
pub const IC_CFG_BATCH_BYTE_SIZE: u32 = 801;
/// Rows per batch in one scan. Verify: line 435.
pub const IC_CFG_BATCH_SIZE: u32 = 802;
/// Whether to reconnect automatically after losing a data node.
/// Verify: line 436.
pub const IC_CFG_AUTO_RECONNECT: u32 = 803;
/// What to do when the redo log is under pressure. Verify: line 438.
pub const IC_CFG_DEFAULT_OPERATION_REDO_PROBLEM_ACTION: u32 = 805;
/// Default number of hash map buckets for new tables. Verify: line 439.
pub const IC_CFG_DEFAULT_HASHMAP_SIZE: u32 = 806;
/// Our rank as an arbitrator, 0 meaning we never arbitrate.
/// Verify: line 328.
pub const IC_CFG_NODE_ARBIT_RANK: u32 = 200;
/// Delay before we would act as arbitrator. Verify: line 329.
pub const IC_CFG_NODE_ARBIT_DELAY: u32 = 201;

// Communication sections: one per pair of nodes that talk.
// Verify: lines 400-414.

/// The lower numbered node of the pair. Verify: line 400.
pub const IC_CFG_CONNECTION_NODE_1: u32 = 400;
/// The higher numbered node of the pair. Verify: line 401.
pub const IC_CFG_CONNECTION_NODE_2: u32 = 401;
/// Whether every signal carries an id, for tracing. Verify: line 402.
pub const IC_CFG_CONNECTION_SEND_SIGNAL_ID: u32 = 402;
/// Whether every signal carries a checksum. Verify: line 403.
pub const IC_CFG_CONNECTION_CHECKSUM: u32 = 403;
/// The port the server side of the link listens on; 0 means it is
/// assigned dynamically and must be asked for. Verify: line 406.
pub const IC_CFG_CONNECTION_SERVER_PORT: u32 = 406;
/// Host name of node 1 of the pair. Verify: line 407.
pub const IC_CFG_CONNECTION_HOSTNAME_1: u32 = 407;
/// Host name of node 2 of the pair. Verify: line 408.
pub const IC_CFG_CONNECTION_HOSTNAME_2: u32 = 408;
/// Which of the two nodes listens; the other one dials.
/// Verify: line 410.
pub const IC_CFG_CONNECTION_NODE_ID_SERVER: u32 = 410;
/// Bytes of send buffer before the link counts as overloaded.
/// Verify: line 411.
pub const IC_CFG_CONNECTION_OVERLOAD: u32 = 411;
/// Whether a checksum is computed before sending rather than after.
/// Verify: line 412.
pub const IC_CFG_CONNECTION_PRESEND_CHECKSUM: u32 = 412;
/// Which IP version to prefer when a host resolves to both.
/// Verify: line 414.
pub const IC_CFG_CONNECTION_PREFER_IP_VER: u32 = 414;
/// Which group of links this one belongs to, used when a pair of nodes
/// has more than one. Verify: line 409.
pub const IC_CFG_CONNECTION_GROUP: u32 = 409;

/* TCP link settings. Verify: lines 452-499. */

/// Bytes of send buffer for this link. Verify: line 454.
pub const IC_CFG_TCP_SEND_BUFFER_SIZE: u32 = 454;
/// Bytes of receive buffer for this link. Verify: line 455.
pub const IC_CFG_TCP_RECEIVE_BUFFER_SIZE: u32 = 455;
/// `SO_RCVBUF` for the socket. Verify: line 457.
pub const IC_CFG_TCP_RCV_BUF_SIZE: u32 = 457;
/// `SO_SNDBUF` for the socket. Verify: line 458.
pub const IC_CFG_TCP_SND_BUF_SIZE: u32 = 458;
/// `TCP_MAXSEG` for the socket. Verify: line 459.
pub const IC_CFG_TCP_MAXSEG_SIZE: u32 = 459;
/// Whether the listening side binds every address. Verify: line 460.
pub const IC_CFG_TCP_BIND_INADDR_ANY: u32 = 460;
/// Whether this link must use TLS. Verify: line 462.
pub const IC_CFG_TCP_REQUIRE_TLS: u32 = 462;
/// Whether to use IPv4 only on this link. Verify: line 499.
pub const IC_CFG_TCP_ONLY_IPV4: u32 = 499;

/// The name of a parameter id within a section of this type.
///
/// Ids are scoped to the kind of section they appear in: id 3 is the
/// node id in a node section and the cluster's name in the system
/// section. A flat table would mislabel one of them. Unknown ids have
/// no name and print as their number, which is what a diagnostic needs.
pub fn parameter_name(
  section_type: SectionType,
  id: u32,
) -> Option<&'static str> {
  if id == IC_CFG_TYPE_OF_SECTION {
    return Some("TypeOfSection");
  }
  match section_type {
    SectionType::System => system_parameter_name(id),
    SectionType::Tcp | SectionType::Shm | SectionType::Rdma => {
      link_parameter_name(id)
    }
    _ => node_parameter_name(id),
  }
}

fn system_parameter_name(id: u32) -> Option<&'static str> {
  match id {
    IC_CFG_SYS_PRIMARY_MGM_NODE => Some("PrimaryMGMNode"),
    IC_CFG_SYS_CONFIG_GENERATION => Some("ConfigGenerationNumber"),
    IC_CFG_SYS_NAME => Some("Name"),
    IC_CFG_SYS_ALLOW_API_TO_DB_RDMA => Some("AllowApiToDbRdma"),
    IC_CFG_SYS_PORT_BASE => Some("PortBase"),
    _ => None,
  }
}

fn node_parameter_name(id: u32) -> Option<&'static str> {
  match id {
    IC_CFG_NODE_ID => Some("NodeId"),
    IC_CFG_NODE_HOST => Some("HostName"),
    IC_CFG_NODE_DATADIR => Some("DataDir"),
    IC_CFG_TOTAL_SEND_BUFFER_MEMORY => Some("TotalSendBufferMemory"),
    IC_CFG_NODE_DEDICATED => Some("Dedicated"),
    IC_CFG_DB_NO_REPLICAS => Some("NoOfReplicas"),
    IC_CFG_DB_API_HEARTBEAT_INTERVAL => Some("HeartbeatIntervalDbApi"),
    IC_CFG_DB_NODEGROUP => Some("Nodegroup"),
    IC_CFG_MAX_SCAN_BATCH_SIZE => Some("MaxScanBatchSize"),
    IC_CFG_BATCH_BYTE_SIZE => Some("BatchByteSize"),
    IC_CFG_BATCH_SIZE => Some("BatchSize"),
    IC_CFG_AUTO_RECONNECT => Some("AutoReconnect"),
    IC_CFG_DEFAULT_OPERATION_REDO_PROBLEM_ACTION => {
      Some("DefaultOperationRedoProblemAction")
    }
    IC_CFG_DEFAULT_HASHMAP_SIZE => Some("DefaultHashMapSize"),
    IC_CFG_NODE_ARBIT_RANK => Some("ArbitrationRank"),
    IC_CFG_NODE_ARBIT_DELAY => Some("ArbitrationDelay"),
    IC_CFG_TCP_ONLY_IPV4 => Some("UseOnlyIPv4"),
    _ => None,
  }
}

fn link_parameter_name(id: u32) -> Option<&'static str> {
  match id {
    IC_CFG_CONNECTION_NODE_1 => Some("NodeId1"),
    IC_CFG_CONNECTION_NODE_2 => Some("NodeId2"),
    IC_CFG_CONNECTION_SEND_SIGNAL_ID => Some("SendSignalId"),
    IC_CFG_CONNECTION_CHECKSUM => Some("Checksum"),
    IC_CFG_CONNECTION_SERVER_PORT => Some("PortNumber"),
    IC_CFG_CONNECTION_HOSTNAME_1 => Some("HostName1"),
    IC_CFG_CONNECTION_HOSTNAME_2 => Some("HostName2"),
    IC_CFG_CONNECTION_GROUP => Some("Group"),
    IC_CFG_CONNECTION_NODE_ID_SERVER => Some("NodeIdServer"),
    IC_CFG_CONNECTION_OVERLOAD => Some("OverloadLimit"),
    IC_CFG_CONNECTION_PRESEND_CHECKSUM => Some("PreSendChecksum"),
    IC_CFG_CONNECTION_PREFER_IP_VER => Some("PreferIPVersion"),
    IC_CFG_TCP_SEND_BUFFER_SIZE => Some("SendBufferMemory"),
    IC_CFG_TCP_RECEIVE_BUFFER_SIZE => Some("ReceiveBufferMemory"),
    IC_CFG_TCP_RCV_BUF_SIZE => Some("TCP_RCV_BUF_SIZE"),
    IC_CFG_TCP_SND_BUF_SIZE => Some("TCP_SND_BUF_SIZE"),
    IC_CFG_TCP_MAXSEG_SIZE => Some("TCP_MAXSEG_SIZE"),
    IC_CFG_TCP_BIND_INADDR_ANY => Some("TcpBind_INADDR_ANY"),
    IC_CFG_TCP_REQUIRE_TLS => Some("RequireTls"),
    IC_CFG_TCP_ONLY_IPV4 => Some("UseOnlyIPv4"),
    _ => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn ids_are_distinct() {
    let ids = [
      IC_CFG_NODE_ID,
      IC_CFG_NODE_HOST,
      IC_CFG_NODE_DATADIR,
      IC_CFG_TOTAL_SEND_BUFFER_MEMORY,
      IC_CFG_DB_NO_REPLICAS,
      IC_CFG_DB_API_HEARTBEAT_INTERVAL,
      IC_CFG_DB_NODEGROUP,
      IC_CFG_MAX_SCAN_BATCH_SIZE,
      IC_CFG_BATCH_BYTE_SIZE,
      IC_CFG_BATCH_SIZE,
      IC_CFG_CONNECTION_NODE_1,
      IC_CFG_CONNECTION_NODE_2,
      IC_CFG_CONNECTION_SERVER_PORT,
      IC_CFG_TCP_MAXSEG_SIZE,
      IC_CFG_TYPE_OF_SECTION,
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
  fn names_are_scoped_to_the_section() {
    /* Id 3 is the node id in a node section. */
    assert_eq!(
      parameter_name(SectionType::DataNode, IC_CFG_NODE_ID),
      Some("NodeId")
    );
    /* The same id is the cluster's name in the system section. */
    assert_eq!(
      parameter_name(SectionType::System, IC_CFG_SYS_NAME),
      Some("Name")
    );
    assert_eq!(
      parameter_name(SectionType::Tcp, IC_CFG_CONNECTION_SERVER_PORT),
      Some("PortNumber")
    );
    /* Every section names its own type. */
    assert_eq!(
      parameter_name(SectionType::Shm, IC_CFG_TYPE_OF_SECTION),
      Some("TypeOfSection")
    );
    assert_eq!(parameter_name(SectionType::DataNode, 999_999), None);
  }
}
