// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The cluster configuration as the rest of the library uses it
//! (`legacy-c/include/ic_apic_data.h`).
//!
//! [`ConfigBlob`] is what arrived; this is what it means. The C had one
//! struct per node type with every parameter as a field, around 180 of
//! them for a data node. An API node needs far fewer: its own settings,
//! where each data node is, and how each link to one is configured. The
//! blob is kept alongside, so a parameter with no field here can still
//! be read by id.

use ic_port::err;
use ic_port::IcError;

use crate::conf_blob::ConfigBlob;
use crate::conf_blob::Section;
use crate::conf_blob::SectionType;
use crate::conf_param::*;

// Values used when the configuration does not state a parameter. A
// management server fills in its own defaults before sending, so these
// are a backstop rather than the usual path.

/// Rows per scan batch when unstated.
pub const IC_DEFAULT_BATCH_SIZE: u32 = 256;
/// Bytes per scan batch when unstated.
pub const IC_DEFAULT_BATCH_BYTE_SIZE: u32 = 16384;
/// Largest scan batch in bytes when unstated.
pub const IC_DEFAULT_MAX_SCAN_BATCH_SIZE: u32 = 262144;
/// Milliseconds between heartbeats to a data node when unstated.
pub const IC_DEFAULT_API_HEARTBEAT_INTERVAL: u32 = 1500;
/// Hash map buckets for a new table when unstated.
pub const IC_DEFAULT_HASHMAP_SIZE: u32 = 240;

/// Our own settings as an API node.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ApiNodeConfig {
  /// Our node id in the cluster.
  pub node_id: u32,
  /// The host the configuration expects us on, if it names one.
  pub host: Option<String>,
  /// Rows per batch in a scan.
  pub scan_batch_rows: u32,
  /// Bytes per batch in a scan.
  pub scan_batch_bytes: u32,
  /// Largest total size of a scan batch, in bytes.
  pub scan_batch_bytes_total: u32,
  /// Bytes of send buffer shared across all our links.
  pub send_buffer_total: u64,
  /// Whether we reconnect by ourselves after losing a data node.
  pub auto_reconnect: bool,
  /// Our rank as an arbitrator; 0 means we never arbitrate.
  pub arbiter_rank: u32,
  /// Hash map buckets a new table gets by default.
  pub hash_map_buckets: u32,
}

/// A data node we may connect to.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DataNodeConfig {
  /// The data node's id.
  pub node_id: u32,
  /// The host it runs on.
  pub host: String,
  /// Its node group, which says which nodes hold the same fragments.
  pub node_group: u32,
  /// How often it expects to hear from us, in milliseconds. Falling
  /// silent for long enough makes it declare us dead.
  pub api_heartbeat_ms: u32,
  /// How often it checks its transactions for ones that have waited
  /// too long, in milliseconds; 0 if the configuration does not say.
  pub timeout_check_period_ms: u32,
  /// How long it lets an operation wait for a lock, in milliseconds; 0
  /// if the configuration does not say.
  pub lock_wait_limit_ms: u32,
}

/// A management server we may ask for the configuration.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MgmNodeConfig {
  /// The management server's node id.
  pub node_id: u32,
  /// The host it runs on.
  pub host: String,
}

/// How one link between us and another node is set up.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TcpLinkConfig {
  /// The lower numbered node of the pair.
  pub node_1: u32,
  /// The higher numbered node of the pair.
  pub node_2: u32,
  /// Which of the two listens; the other one dials.
  pub server_node_id: u32,
  /// The port the listening side uses. Zero means it is assigned when
  /// the node starts and has to be asked for.
  pub server_port: u16,
  /// The address to reach the listening side at.
  pub server_host: String,
  /// Bytes of send buffer for this link.
  pub send_buffer_size: u32,
  /// Bytes of receive buffer for this link.
  pub receive_buffer_size: u32,
  /// Whether every signal carries a checksum.
  pub checksum: bool,
  /// Whether every signal carries an id, for tracing.
  pub signal_ids: bool,
  /// `TCP_MAXSEG` for the socket, 0 to leave it alone.
  pub socket_max_segment: u32,
  /// `SO_RCVBUF` for the socket, 0 to leave it alone.
  pub socket_recv_buffer: u32,
  /// `SO_SNDBUF` for the socket, 0 to leave it alone.
  pub socket_send_buffer: u32,
  /// Bytes of queued send data before the link counts as overloaded.
  pub overload_bytes: u32,
  /// Whether to use IPv4 only.
  pub ipv4_only: bool,
  /// Whether the link must use TLS.
  pub require_tls: bool,
}

impl TcpLinkConfig {
  /// The node at the other end from `own_node_id`.
  pub fn other_node(&self, own_node_id: u32) -> u32 {
    if self.node_1 == own_node_id {
      return self.node_2;
    }
    self.node_1
  }

  /// True when the far end listens and we dial, which is how an API
  /// node normally reaches a data node.
  pub fn we_dial(&self, own_node_id: u32) -> bool {
    self.server_node_id != own_node_id
  }

  /// True when the port must be asked for because it is assigned at
  /// startup rather than written in the configuration.
  pub fn port_is_dynamic(&self) -> bool {
    self.server_port == 0
  }
}

/// Everything an API node needs to know about its cluster.
#[derive(Clone, Debug, Default)]
pub struct ClusterConfig {
  /// Our own settings.
  pub api: ApiNodeConfig,
  /// Every data node in the cluster.
  pub data_nodes: Vec<DataNodeConfig>,
  /// Every management server.
  pub mgm_nodes: Vec<MgmNodeConfig>,
  /// The links between us and someone else, one per node we talk to.
  pub links: Vec<TcpLinkConfig>,
  /// How many copies of each row the cluster keeps.
  pub replica_count: u32,
  /// The configuration as it arrived, for parameters with no field
  /// here and for diagnostics.
  pub blob: ConfigBlob,
}

fn value_u32_or(
  blob: &ConfigBlob,
  section: &Section,
  key: u32,
  fallback: u32,
) -> u32 {
  match blob.value_u32(section, key) {
    Some(value) => value,
    None => fallback,
  }
}

fn value_bool(blob: &ConfigBlob, section: &Section, key: u32) -> bool {
  match blob.value_u32(section, key) {
    Some(value) => value != 0,
    None => false,
  }
}

fn value_string(
  blob: &ConfigBlob,
  section: &Section,
  key: u32,
) -> Option<String> {
  let text = blob.value_str(section, key)?;
  if text.is_empty() {
    return None;
  }
  Some(text.to_string())
}

impl ClusterConfig {
  /// Read a decoded configuration as the node `own_node_id`.
  ///
  /// Fails if that node is not an API node in this configuration, which
  /// is what happens when a node id is used for the wrong kind of node.
  pub fn from_blob(
    blob: ConfigBlob,
    own_node_id: u32,
  ) -> Result<ClusterConfig, IcError> {
    let own_section = match blob.node(own_node_id) {
      Some(section) => section.clone(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    if own_section.section_type != SectionType::ApiNode {
      return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE_TYPE));
    }
    let api = ApiNodeConfig {
      node_id: own_node_id,
      host: value_string(&blob, &own_section, IC_PARAM_NODE_HOST),
      scan_batch_rows: value_u32_or(
        &blob,
        &own_section,
        IC_PARAM_SCAN_BATCH_ROWS,
        IC_DEFAULT_BATCH_SIZE,
      ),
      scan_batch_bytes: value_u32_or(
        &blob,
        &own_section,
        IC_PARAM_SCAN_BATCH_BYTES,
        IC_DEFAULT_BATCH_BYTE_SIZE,
      ),
      scan_batch_bytes_total: value_u32_or(
        &blob,
        &own_section,
        IC_PARAM_SCAN_BATCH_BYTES_TOTAL,
        IC_DEFAULT_MAX_SCAN_BATCH_SIZE,
      ),
      send_buffer_total: blob
        .value_u64(&own_section, IC_PARAM_SEND_BUFFER_TOTAL)
        .unwrap_or(0),
      auto_reconnect: value_bool(&blob, &own_section, IC_PARAM_AUTO_RECONNECT),
      arbiter_rank: value_u32_or(&blob, &own_section, IC_PARAM_ARBITER_RANK, 0),
      hash_map_buckets: value_u32_or(
        &blob,
        &own_section,
        IC_PARAM_HASH_MAP_BUCKETS,
        IC_DEFAULT_HASHMAP_SIZE,
      ),
    };
    let mut config = ClusterConfig {
      api,
      replica_count: 0,
      ..ClusterConfig::default()
    };
    for section in blob.nodes_of_type(SectionType::DataNode) {
      let node_id = match section.node_id() {
        Some(id) => id,
        None => continue,
      };
      if config.replica_count == 0 {
        config.replica_count =
          value_u32_or(&blob, section, IC_PARAM_REPLICA_COUNT, 1);
      }
      config.data_nodes.push(DataNodeConfig {
        node_id,
        host: value_string(&blob, section, IC_PARAM_NODE_HOST)
          .unwrap_or_default(),
        node_group: value_u32_or(&blob, section, IC_PARAM_NODE_GROUP, 0),
        api_heartbeat_ms: value_u32_or(
          &blob,
          section,
          IC_PARAM_API_HEARTBEAT_MS,
          IC_DEFAULT_API_HEARTBEAT_INTERVAL,
        ),
        timeout_check_period_ms: value_u32_or(
          &blob,
          section,
          IC_PARAM_TIMEOUT_CHECK_PERIOD,
          0,
        ),
        lock_wait_limit_ms: value_u32_or(
          &blob,
          section,
          IC_PARAM_LOCK_WAIT_LIMIT,
          0,
        ),
      });
    }
    for section in blob.nodes_of_type(SectionType::MgmNode) {
      let node_id = match section.node_id() {
        Some(id) => id,
        None => continue,
      };
      config.mgm_nodes.push(MgmNodeConfig {
        node_id,
        host: value_string(&blob, section, IC_PARAM_NODE_HOST)
          .unwrap_or_default(),
      });
    }
    config.links = ClusterConfig::links_of(&blob, own_node_id);
    config.blob = blob;
    Ok(config)
  }

  fn links_of(blob: &ConfigBlob, own_node_id: u32) -> Vec<TcpLinkConfig> {
    let mut links: Vec<TcpLinkConfig> = Vec::new();
    for section in &blob.links {
      if section.section_type != SectionType::Tcp {
        /* Shared memory and RDMA links are not ours to use. */
        continue;
      }
      let node_1 = match blob.value_u32(section, IC_PARAM_LINK_NODE_1) {
        Some(id) => id,
        None => continue,
      };
      let node_2 = match blob.value_u32(section, IC_PARAM_LINK_NODE_2) {
        Some(id) => id,
        None => continue,
      };
      if node_1 != own_node_id && node_2 != own_node_id {
        continue;
      }
      let server_node_id =
        value_u32_or(blob, section, IC_PARAM_LINK_SERVER_NODE, node_1);
      // The address to dial is the one of whichever end listens. Each
      // link may name the hosts itself; otherwise the node's own
      // section says where it runs.
      let host_key = if server_node_id == node_1 {
        IC_PARAM_LINK_HOST_1
      } else {
        IC_PARAM_LINK_HOST_2
      };
      let mut server_host = value_string(blob, section, host_key);
      if server_host.is_none() {
        if let Some(node_section) = blob.node(server_node_id) {
          server_host = value_string(blob, node_section, IC_PARAM_NODE_HOST);
        }
      }
      let port = value_u32_or(blob, section, IC_PARAM_LINK_PORT, 0);
      links.push(TcpLinkConfig {
        node_1,
        node_2,
        server_node_id,
        server_port: if port <= u16::MAX as u32 {
          port as u16
        } else {
          0
        },
        server_host: server_host.unwrap_or_default(),
        send_buffer_size: value_u32_or(
          blob,
          section,
          IC_PARAM_LINK_SEND_BUFFER,
          0,
        ),
        receive_buffer_size: value_u32_or(
          blob,
          section,
          IC_PARAM_LINK_RECV_BUFFER,
          0,
        ),
        checksum: value_bool(blob, section, IC_PARAM_LINK_CHECKSUM),
        signal_ids: value_bool(blob, section, IC_PARAM_LINK_SIGNAL_IDS),
        socket_max_segment: value_u32_or(
          blob,
          section,
          IC_PARAM_SOCKET_MAX_SEGMENT,
          0,
        ),
        socket_recv_buffer: value_u32_or(
          blob,
          section,
          IC_PARAM_SOCKET_RECV_BUFFER,
          0,
        ),
        socket_send_buffer: value_u32_or(
          blob,
          section,
          IC_PARAM_SOCKET_SEND_BUFFER,
          0,
        ),
        overload_bytes: value_u32_or(blob, section, IC_PARAM_LINK_OVERLOAD, 0),
        ipv4_only: value_bool(blob, section, IC_PARAM_LINK_IPV4_ONLY),
        require_tls: value_bool(blob, section, IC_PARAM_LINK_REQUIRE_TLS),
      });
    }
    links
  }

  /// The data node with this id.
  pub fn data_node(&self, node_id: u32) -> Option<&DataNodeConfig> {
    self.data_nodes.iter().find(|node| node.node_id == node_id)
  }

  /// The link between us and this node.
  pub fn link_to(&self, node_id: u32) -> Option<&TcpLinkConfig> {
    let own = self.api.node_id;
    self
      .links
      .iter()
      .find(|link| link.other_node(own) == node_id)
  }

  /// The data nodes we have a link to, which is the set we will
  /// connect to at startup.
  pub fn connectable_data_nodes(&self) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::new();
    for node in &self.data_nodes {
      if self.link_to(node.node_id).is_some() {
        out.push(node.node_id);
      }
    }
    out
  }

  /// The shortest heartbeat interval any data node asks of us, which is
  /// the period we must keep to.
  pub fn heartbeat_interval_ms(&self) -> u32 {
    let mut shortest = IC_DEFAULT_API_HEARTBEAT_INTERVAL;
    let mut first = true;
    for node in &self.data_nodes {
      if first || node.api_heartbeat_ms < shortest {
        shortest = node.api_heartbeat_ms;
        first = false;
      }
    }
    shortest
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::conf_blob::BlobBuilder;
  use crate::conf_blob::ConfigValue;

  /// Two data nodes, one API node and one management node, with the
  /// links an API node would have.
  fn sample() -> ConfigBlob {
    let mut b = BlobBuilder::new(2, 1, 1, 3);
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_PARAM_REPLICA_COUNT, ConfigValue::Int(2)),
        (IC_PARAM_API_HEARTBEAT_MS, ConfigValue::Int(1500)),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[
        (IC_PARAM_SCAN_BATCH_ROWS, ConfigValue::Int(256)),
        (IC_PARAM_SCAN_BATCH_BYTES, ConfigValue::Int(16384)),
      ],
    );
    b.typed_section(SectionType::MgmNode, &[]);
    b.typed_section(
      SectionType::Tcp,
      &[(IC_PARAM_LINK_SEND_BUFFER, ConfigValue::Int(2097152))],
    );
    b.typed_section(SectionType::Shm, &[]);
    b.typed_section(SectionType::System, &[]);
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_PARAM_NODE_ID, ConfigValue::Int(1)),
        (
          IC_PARAM_NODE_HOST,
          ConfigValue::Str("node1.example".to_string()),
        ),
      ],
    );
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_PARAM_NODE_ID, ConfigValue::Int(2)),
        (
          IC_PARAM_NODE_HOST,
          ConfigValue::Str("node2.example".to_string()),
        ),
        (IC_PARAM_NODE_GROUP, ConfigValue::Int(1)),
        (IC_PARAM_API_HEARTBEAT_MS, ConfigValue::Int(800)),
        (IC_PARAM_TIMEOUT_CHECK_PERIOD, ConfigValue::Int(1000)),
        (IC_PARAM_LOCK_WAIT_LIMIT, ConfigValue::Int(300_000)),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[
        (IC_PARAM_NODE_ID, ConfigValue::Int(68)),
        (
          IC_PARAM_SEND_BUFFER_TOTAL,
          ConfigValue::Int64(8 * 1024 * 1024),
        ),
      ],
    );
    b.typed_section(
      SectionType::MgmNode,
      &[
        (IC_PARAM_NODE_ID, ConfigValue::Int(65)),
        (
          IC_PARAM_NODE_HOST,
          ConfigValue::Str("mgm.example".to_string()),
        ),
      ],
    );
    // Our link to data node 1: the data node listens, port is dynamic,
    // and the host comes from the node section.
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_PARAM_LINK_NODE_1, ConfigValue::Int(1)),
        (IC_PARAM_LINK_NODE_2, ConfigValue::Int(68)),
        (IC_PARAM_LINK_SERVER_NODE, ConfigValue::Int(1)),
        (IC_PARAM_LINK_PORT, ConfigValue::Int(0)),
      ],
    );
    // Our link to data node 2: fixed port, host named on the link,
    // checksum on.
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_PARAM_LINK_NODE_1, ConfigValue::Int(2)),
        (IC_PARAM_LINK_NODE_2, ConfigValue::Int(68)),
        (IC_PARAM_LINK_SERVER_NODE, ConfigValue::Int(2)),
        (IC_PARAM_LINK_PORT, ConfigValue::Int(45000)),
        (
          IC_PARAM_LINK_HOST_1,
          ConfigValue::Str("10.0.0.2".to_string()),
        ),
        (IC_PARAM_LINK_CHECKSUM, ConfigValue::Int(1)),
        (IC_PARAM_SOCKET_MAX_SEGMENT, ConfigValue::Int(61440)),
      ],
    );
    /* A link between the two data nodes, which is none of our business. */
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_PARAM_LINK_NODE_1, ConfigValue::Int(1)),
        (IC_PARAM_LINK_NODE_2, ConfigValue::Int(2)),
        (IC_PARAM_LINK_PORT, ConfigValue::Int(44000)),
      ],
    );
    ConfigBlob::decode(&b.finish()).expect("decode")
  }

  #[test]
  fn our_own_settings() {
    let config = ClusterConfig::from_blob(sample(), 68).expect("config");
    assert_eq!(config.api.node_id, 68);
    assert_eq!(config.api.scan_batch_rows, 256);
    assert_eq!(config.api.scan_batch_bytes, 16384);
    /* Not stated anywhere, so the fallback applies. */
    assert_eq!(
      config.api.scan_batch_bytes_total,
      IC_DEFAULT_MAX_SCAN_BATCH_SIZE
    );
    assert_eq!(config.api.send_buffer_total, 8 * 1024 * 1024);
    assert_eq!(config.api.host, None);
    assert_eq!(config.replica_count, 2);
  }

  #[test]
  fn the_data_nodes() {
    let config = ClusterConfig::from_blob(sample(), 68).expect("config");
    assert_eq!(config.data_nodes.len(), 2);
    let one = config.data_node(1).expect("node 1");
    assert_eq!(one.host, "node1.example");
    assert_eq!(one.node_group, 0);
    assert_eq!(one.api_heartbeat_ms, 1500);
    let two = config.data_node(2).expect("node 2");
    assert_eq!(two.node_group, 1);
    assert_eq!(two.api_heartbeat_ms, 800);
    /* The period we must keep is the shortest anyone asks for. */
    assert_eq!(config.heartbeat_interval_ms(), 800);
    /* Node 1 says nothing of its timeouts; node 2 says both. */
    assert_eq!(
      (one.timeout_check_period_ms, one.lock_wait_limit_ms),
      (0, 0)
    );
    assert_eq!(two.timeout_check_period_ms, 1000);
    assert_eq!(two.lock_wait_limit_ms, 300_000);
    assert!(config.data_node(99).is_none());
    assert_eq!(config.mgm_nodes.len(), 1);
    assert_eq!(config.mgm_nodes[0].host, "mgm.example");
  }

  #[test]
  fn our_links_only() {
    let config = ClusterConfig::from_blob(sample(), 68).expect("config");
    /* Three links exist but only two involve us. */
    assert_eq!(config.links.len(), 2);
    assert_eq!(config.connectable_data_nodes(), vec![1, 2]);
    let to_one = config.link_to(1).expect("link to 1");
    assert_eq!(to_one.other_node(68), 1);
    assert!(to_one.we_dial(68));
    assert!(to_one.port_is_dynamic());
    /* No host on the link, so the node section supplies it. */
    assert_eq!(to_one.server_host, "node1.example");
    /* The TCP default section gives the send buffer size. */
    assert_eq!(to_one.send_buffer_size, 2097152);
    assert!(!to_one.checksum);
    let to_two = config.link_to(2).expect("link to 2");
    assert_eq!(to_two.server_port, 45000);
    assert!(!to_two.port_is_dynamic());
    /* The link names the host itself, which wins. */
    assert_eq!(to_two.server_host, "10.0.0.2");
    assert!(to_two.checksum);
    assert_eq!(to_two.socket_max_segment, 61440);
    assert!(config.link_to(99).is_none());
  }

  #[test]
  fn a_wrong_node_id_is_refused() {
    /* Node 1 is a data node, not us. */
    let err = ClusterConfig::from_blob(sample(), 1).expect_err("data node");
    assert_eq!(err.code, err::IC_ERROR_NO_SUCH_NODE_TYPE);
    /* Node 99 is in no section at all. */
    let err = ClusterConfig::from_blob(sample(), 99).expect_err("absent");
    assert_eq!(err.code, err::IC_ERROR_NO_SUCH_NODE);
  }

  #[test]
  fn the_blob_is_still_reachable() {
    let config = ClusterConfig::from_blob(sample(), 68).expect("config");
    /* A parameter with no field here is still readable by id. */
    let node = config.blob.node(2).expect("node 2");
    assert_eq!(
      config.blob.value_str(node, IC_PARAM_NODE_HOST),
      Some("node2.example")
    );
  }
}
