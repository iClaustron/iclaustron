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
//! Every id is checked against RonDB 26.10, and chapter 05 §17.3 says
//! where. The ids are interface: they are what travels in the
//! configuration blob.

use crate::conf_blob::SectionType;

/* Common to every section. Verify: chapter 05 §17.3. */

/// The node's id in the cluster. Verify: chapter 05 §17.3.
pub const IC_PARAM_NODE_ID: u32 = 3;
/// The host the node runs on. Verify: chapter 05 §17.3.
pub const IC_PARAM_NODE_HOST: u32 = 5;
/// The node's data directory. Verify: chapter 05 §17.3.
pub const IC_PARAM_NODE_DATA_DIR: u32 = 7;
/// Whether the node id is reserved for one host. Verify: chapter 05 §17.3.
pub const IC_PARAM_NODE_DEDICATED: u32 = 11;
/// Bytes of send buffer shared across all links of the node.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_SEND_BUFFER_TOTAL: u32 = 9;
/// Which kind of section this is, written in every section.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_SECTION_KIND: u32 = 999;

// The system section, which describes the cluster itself. These ids
// overlap with the node ids above and mean something else: key 3 is the
// cluster's name here, not a node id. Verify: chapter 05 §17.3.

/// The management node that holds the master copy of the configuration.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_SYS_FIRST_MGM_NODE: u32 = 1;
/// Which generation of the configuration this is. Verify: chapter 05 §17.3.
pub const IC_PARAM_SYS_CONFIG_GENERATION: u32 = 2;
/// The cluster's name. Verify: chapter 05 §17.3.
pub const IC_PARAM_SYS_NAME: u32 = 3;
/// Whether API nodes may reach data nodes over RDMA, which RonDB adds.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_SYS_ALLOW_API_TO_DB_RDMA: u32 = 4;
/// The base port the cluster allocates from. Verify: chapter 05 §17.3.
pub const IC_PARAM_SYS_PORT_BASE: u32 = 8;

/* Data node settings an API node reads. */

/// Replicas per fragment, which says how many copies of a row exist.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_REPLICA_COUNT: u32 = 101;
/// How often a data node expects to hear from us, in milliseconds.
/// This sets our heartbeat period. Verify: chapter 05 §17.3.
pub const IC_PARAM_API_HEARTBEAT_MS: u32 = 119;
/// How often a data node checks its transactions for ones that have
/// waited too long, in milliseconds. Verify: chapter 05 §17.3.
pub const IC_PARAM_TIMEOUT_CHECK_PERIOD: u32 = 129;
/// How long a data node lets an operation wait for a lock before it
/// aborts the transaction, in milliseconds. Verify: chapter 05 §17.3.
pub const IC_PARAM_LOCK_WAIT_LIMIT: u32 = 131;
/// The node group a data node belongs to. Verify: chapter 05 §17.3.
pub const IC_PARAM_NODE_GROUP: u32 = 185;

/* API node settings, which are ours. Verify: chapter 05 §17.3. */

/// Largest total size of a scan batch in bytes. Verify: chapter 05 §17.3.
pub const IC_PARAM_SCAN_BATCH_BYTES_TOTAL: u32 = 800;
/// Bytes per batch in one scan. Verify: chapter 05 §17.3.
pub const IC_PARAM_SCAN_BATCH_BYTES: u32 = 801;
/// Rows per batch in one scan. Verify: chapter 05 §17.3.
pub const IC_PARAM_SCAN_BATCH_ROWS: u32 = 802;
/// Whether to reconnect automatically after losing a data node.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_AUTO_RECONNECT: u32 = 803;
/// What to do when the redo log is under pressure. Verify: chapter 05 §17.3.
pub const IC_PARAM_REDO_FULL_ACTION: u32 = 805;
/// Default number of hash map buckets for new tables.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_HASH_MAP_BUCKETS: u32 = 806;
/// Our rank as an arbitrator, 0 meaning we never arbitrate.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_ARBITER_RANK: u32 = 200;
/// Delay before we would act as arbitrator. Verify: chapter 05 §17.3.
pub const IC_PARAM_ARBITER_DELAY: u32 = 201;

// Communication sections: one per pair of nodes that talk.
// Verify: chapter 05 §17.3.

/// The lower numbered node of the pair. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_NODE_1: u32 = 400;
/// The higher numbered node of the pair. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_NODE_2: u32 = 401;
/// Whether every signal carries an id, for tracing. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_SIGNAL_IDS: u32 = 402;
/// Whether every signal carries a checksum. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_CHECKSUM: u32 = 403;
/// The port the server side of the link listens on; 0 means it is
/// assigned dynamically and must be asked for. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_PORT: u32 = 406;
/// Host name of node 1 of the pair. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_HOST_1: u32 = 407;
/// Host name of node 2 of the pair. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_HOST_2: u32 = 408;
/// Which of the two nodes listens; the other one dials.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_SERVER_NODE: u32 = 410;
/// Bytes of send buffer before the link counts as overloaded.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_OVERLOAD: u32 = 411;
/// Whether a checksum is computed before sending rather than after.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_CHECKSUM_BEFORE_SEND: u32 = 412;
/// Which IP version to prefer when a host resolves to both.
/// Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_IP_VERSION: u32 = 414;
/// Which group of links this one belongs to, used when a pair of nodes
/// has more than one. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_GROUP: u32 = 409;

/* TCP link settings. Verify: chapter 05 §17.3. */

/// Bytes of send buffer for this link. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_SEND_BUFFER: u32 = 454;
/// Bytes of receive buffer for this link. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_RECV_BUFFER: u32 = 455;
/// `SO_RCVBUF` for the socket. Verify: chapter 05 §17.3.
pub const IC_PARAM_SOCKET_RECV_BUFFER: u32 = 457;
/// `SO_SNDBUF` for the socket. Verify: chapter 05 §17.3.
pub const IC_PARAM_SOCKET_SEND_BUFFER: u32 = 458;
/// `TCP_MAXSEG` for the socket. Verify: chapter 05 §17.3.
pub const IC_PARAM_SOCKET_MAX_SEGMENT: u32 = 459;
/// Whether the listening side binds every address. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_BIND_ANY: u32 = 460;
/// Whether this link must use TLS. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_REQUIRE_TLS: u32 = 462;
/// Whether to use IPv4 only on this link. Verify: chapter 05 §17.3.
pub const IC_PARAM_LINK_IPV4_ONLY: u32 = 499;

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
  if id == IC_PARAM_SECTION_KIND {
    return Some("section_kind");
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
    IC_PARAM_SYS_FIRST_MGM_NODE => Some("first_mgm_node"),
    IC_PARAM_SYS_CONFIG_GENERATION => Some("config_generation"),
    IC_PARAM_SYS_NAME => Some("cluster_name"),
    IC_PARAM_SYS_ALLOW_API_TO_DB_RDMA => Some("api_rdma_allowed"),
    IC_PARAM_SYS_PORT_BASE => Some("port_base"),
    _ => None,
  }
}

fn node_parameter_name(id: u32) -> Option<&'static str> {
  match id {
    IC_PARAM_NODE_ID => Some("node_id"),
    IC_PARAM_NODE_HOST => Some("host"),
    IC_PARAM_NODE_DATA_DIR => Some("data_dir"),
    IC_PARAM_SEND_BUFFER_TOTAL => Some("send_buffer_total"),
    IC_PARAM_NODE_DEDICATED => Some("dedicated"),
    IC_PARAM_REPLICA_COUNT => Some("replica_count"),
    IC_PARAM_API_HEARTBEAT_MS => Some("api_heartbeat_ms"),
    IC_PARAM_TIMEOUT_CHECK_PERIOD => Some("timeout_check_period"),
    IC_PARAM_LOCK_WAIT_LIMIT => Some("lock_wait_limit"),
    IC_PARAM_NODE_GROUP => Some("node_group"),
    IC_PARAM_SCAN_BATCH_BYTES_TOTAL => Some("scan_batch_bytes_total"),
    IC_PARAM_SCAN_BATCH_BYTES => Some("scan_batch_bytes"),
    IC_PARAM_SCAN_BATCH_ROWS => Some("scan_batch_rows"),
    IC_PARAM_AUTO_RECONNECT => Some("auto_reconnect"),
    IC_PARAM_REDO_FULL_ACTION => Some("redo_full_action"),
    IC_PARAM_HASH_MAP_BUCKETS => Some("hash_map_buckets"),
    IC_PARAM_ARBITER_RANK => Some("arbiter_rank"),
    IC_PARAM_ARBITER_DELAY => Some("arbiter_delay"),
    IC_PARAM_LINK_IPV4_ONLY => Some("ipv4_only"),
    _ => None,
  }
}

fn link_parameter_name(id: u32) -> Option<&'static str> {
  match id {
    IC_PARAM_LINK_NODE_1 => Some("node_1"),
    IC_PARAM_LINK_NODE_2 => Some("node_2"),
    IC_PARAM_LINK_SIGNAL_IDS => Some("signal_ids"),
    IC_PARAM_LINK_CHECKSUM => Some("checksum"),
    IC_PARAM_LINK_PORT => Some("port"),
    IC_PARAM_LINK_HOST_1 => Some("host_1"),
    IC_PARAM_LINK_HOST_2 => Some("host_2"),
    IC_PARAM_LINK_GROUP => Some("group"),
    IC_PARAM_LINK_SERVER_NODE => Some("server_node"),
    IC_PARAM_LINK_OVERLOAD => Some("overload_bytes"),
    IC_PARAM_LINK_CHECKSUM_BEFORE_SEND => Some("checksum_before_send"),
    IC_PARAM_LINK_IP_VERSION => Some("ip_version"),
    IC_PARAM_LINK_SEND_BUFFER => Some("send_buffer"),
    IC_PARAM_LINK_RECV_BUFFER => Some("recv_buffer"),
    IC_PARAM_SOCKET_RECV_BUFFER => Some("socket_recv_buffer"),
    IC_PARAM_SOCKET_SEND_BUFFER => Some("socket_send_buffer"),
    IC_PARAM_SOCKET_MAX_SEGMENT => Some("socket_max_segment"),
    IC_PARAM_LINK_BIND_ANY => Some("bind_any"),
    IC_PARAM_LINK_REQUIRE_TLS => Some("require_tls"),
    IC_PARAM_LINK_IPV4_ONLY => Some("ipv4_only"),
    _ => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn ids_are_distinct() {
    let ids = [
      IC_PARAM_NODE_ID,
      IC_PARAM_NODE_HOST,
      IC_PARAM_NODE_DATA_DIR,
      IC_PARAM_SEND_BUFFER_TOTAL,
      IC_PARAM_REPLICA_COUNT,
      IC_PARAM_API_HEARTBEAT_MS,
      IC_PARAM_TIMEOUT_CHECK_PERIOD,
      IC_PARAM_LOCK_WAIT_LIMIT,
      IC_PARAM_NODE_GROUP,
      IC_PARAM_SCAN_BATCH_BYTES_TOTAL,
      IC_PARAM_SCAN_BATCH_BYTES,
      IC_PARAM_SCAN_BATCH_ROWS,
      IC_PARAM_LINK_NODE_1,
      IC_PARAM_LINK_NODE_2,
      IC_PARAM_LINK_PORT,
      IC_PARAM_SOCKET_MAX_SEGMENT,
      IC_PARAM_SECTION_KIND,
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
      parameter_name(SectionType::DataNode, IC_PARAM_NODE_ID),
      Some("node_id")
    );
    /* The same id is the cluster's name in the system section. */
    assert_eq!(
      parameter_name(SectionType::System, IC_PARAM_SYS_NAME),
      Some("cluster_name")
    );
    assert_eq!(
      parameter_name(SectionType::Tcp, IC_PARAM_LINK_PORT),
      Some("port")
    );
    /* Every section names its own type. */
    assert_eq!(
      parameter_name(SectionType::Shm, IC_PARAM_SECTION_KIND),
      Some("section_kind")
    );
    assert_eq!(parameter_name(SectionType::DataNode, 999_999), None);
  }
}
