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

/* Values used when the configuration does not state a parameter. A
management server fills in its own defaults before sending, so these
are a backstop rather than the usual path. */

/// Rows per scan batch when unstated.
pub const DEFAULT_BATCH_SIZE: u32 = 256;
/// Bytes per scan batch when unstated.
pub const DEFAULT_BATCH_BYTE_SIZE: u32 = 16384;
/// Largest scan batch in bytes when unstated.
pub const DEFAULT_MAX_SCAN_BATCH_SIZE: u32 = 262144;
/// Milliseconds between heartbeats to a data node when unstated.
pub const DEFAULT_API_HEARTBEAT_INTERVAL: u32 = 1500;
/// Hash map buckets for a new table when unstated.
pub const DEFAULT_HASHMAP_SIZE: u32 = 240;

/// Our own settings as an API node.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ApiNodeConfig {
  /// Our node id in the cluster.
  pub node_id: u32,
  /// The host the configuration expects us on, if it names one.
  pub host: Option<String>,
  /// Rows per batch in a scan.
  pub batch_size: u32,
  /// Bytes per batch in a scan.
  pub batch_byte_size: u32,
  /// Largest total size of a scan batch, in bytes.
  pub max_scan_batch_size: u32,
  /// Bytes of send buffer shared across all our links.
  pub total_send_buffer_memory: u64,
  /// Whether we reconnect by ourselves after losing a data node.
  pub auto_reconnect: bool,
  /// Our rank as an arbitrator; 0 means we never arbitrate.
  pub arbitration_rank: u32,
  /// Hash map buckets a new table gets by default.
  pub default_hashmap_size: u32,
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
  pub api_heartbeat_interval_ms: u32,
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
  pub send_signal_id: bool,
  /// `TCP_MAXSEG` for the socket, 0 to leave it alone.
  pub tcp_maxseg_size: u32,
  /// `SO_RCVBUF` for the socket, 0 to leave it alone.
  pub tcp_rcv_buf_size: u32,
  /// `SO_SNDBUF` for the socket, 0 to leave it alone.
  pub tcp_snd_buf_size: u32,
  /// Bytes of queued send data before the link counts as overloaded.
  pub overload_limit: u32,
  /// Whether to use IPv4 only.
  pub only_ipv4: bool,
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
  pub no_of_replicas: u32,
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
      host: value_string(&blob, &own_section, CFG_NODE_HOST),
      batch_size: value_u32_or(
        &blob,
        &own_section,
        CFG_BATCH_SIZE,
        DEFAULT_BATCH_SIZE,
      ),
      batch_byte_size: value_u32_or(
        &blob,
        &own_section,
        CFG_BATCH_BYTE_SIZE,
        DEFAULT_BATCH_BYTE_SIZE,
      ),
      max_scan_batch_size: value_u32_or(
        &blob,
        &own_section,
        CFG_MAX_SCAN_BATCH_SIZE,
        DEFAULT_MAX_SCAN_BATCH_SIZE,
      ),
      total_send_buffer_memory: blob
        .value_u64(&own_section, CFG_TOTAL_SEND_BUFFER_MEMORY)
        .unwrap_or(0),
      auto_reconnect: value_bool(&blob, &own_section, CFG_AUTO_RECONNECT),
      arbitration_rank: value_u32_or(
        &blob,
        &own_section,
        CFG_NODE_ARBIT_RANK,
        0,
      ),
      default_hashmap_size: value_u32_or(
        &blob,
        &own_section,
        CFG_DEFAULT_HASHMAP_SIZE,
        DEFAULT_HASHMAP_SIZE,
      ),
    };
    let mut config = ClusterConfig {
      api,
      no_of_replicas: 0,
      ..ClusterConfig::default()
    };
    for section in blob.nodes_of_type(SectionType::DataNode) {
      let node_id = match section.node_id() {
        Some(id) => id,
        None => continue,
      };
      if config.no_of_replicas == 0 {
        config.no_of_replicas =
          value_u32_or(&blob, section, CFG_DB_NO_REPLICAS, 1);
      }
      config.data_nodes.push(DataNodeConfig {
        node_id,
        host: value_string(&blob, section, CFG_NODE_HOST).unwrap_or_default(),
        node_group: value_u32_or(&blob, section, CFG_DB_NODEGROUP, 0),
        api_heartbeat_interval_ms: value_u32_or(
          &blob,
          section,
          CFG_DB_API_HEARTBEAT_INTERVAL,
          DEFAULT_API_HEARTBEAT_INTERVAL,
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
        host: value_string(&blob, section, CFG_NODE_HOST).unwrap_or_default(),
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
      let node_1 = match blob.value_u32(section, CFG_CONNECTION_NODE_1) {
        Some(id) => id,
        None => continue,
      };
      let node_2 = match blob.value_u32(section, CFG_CONNECTION_NODE_2) {
        Some(id) => id,
        None => continue,
      };
      if node_1 != own_node_id && node_2 != own_node_id {
        continue;
      }
      let server_node_id =
        value_u32_or(blob, section, CFG_CONNECTION_NODE_ID_SERVER, node_1);
      /* The address to dial is the one of whichever end listens. Each
      link may name the hosts itself; otherwise the node's own
      section says where it runs. */
      let host_key = if server_node_id == node_1 {
        CFG_CONNECTION_HOSTNAME_1
      } else {
        CFG_CONNECTION_HOSTNAME_2
      };
      let mut server_host = value_string(blob, section, host_key);
      if server_host.is_none() {
        if let Some(node_section) = blob.node(server_node_id) {
          server_host = value_string(blob, node_section, CFG_NODE_HOST);
        }
      }
      let port = value_u32_or(blob, section, CFG_CONNECTION_SERVER_PORT, 0);
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
          CFG_TCP_SEND_BUFFER_SIZE,
          0,
        ),
        receive_buffer_size: value_u32_or(
          blob,
          section,
          CFG_TCP_RECEIVE_BUFFER_SIZE,
          0,
        ),
        checksum: value_bool(blob, section, CFG_CONNECTION_CHECKSUM),
        send_signal_id: value_bool(
          blob,
          section,
          CFG_CONNECTION_SEND_SIGNAL_ID,
        ),
        tcp_maxseg_size: value_u32_or(blob, section, CFG_TCP_MAXSEG_SIZE, 0),
        tcp_rcv_buf_size: value_u32_or(blob, section, CFG_TCP_RCV_BUF_SIZE, 0),
        tcp_snd_buf_size: value_u32_or(blob, section, CFG_TCP_SND_BUF_SIZE, 0),
        overload_limit: value_u32_or(blob, section, CFG_CONNECTION_OVERLOAD, 0),
        only_ipv4: value_bool(blob, section, CFG_TCP_ONLY_IPV4),
        require_tls: value_bool(blob, section, CFG_TCP_REQUIRE_TLS),
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
    let mut shortest = DEFAULT_API_HEARTBEAT_INTERVAL;
    let mut first = true;
    for node in &self.data_nodes {
      if first || node.api_heartbeat_interval_ms < shortest {
        shortest = node.api_heartbeat_interval_ms;
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
        (CFG_DB_NO_REPLICAS, ConfigValue::Int(2)),
        (CFG_DB_API_HEARTBEAT_INTERVAL, ConfigValue::Int(1500)),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[
        (CFG_BATCH_SIZE, ConfigValue::Int(256)),
        (CFG_BATCH_BYTE_SIZE, ConfigValue::Int(16384)),
      ],
    );
    b.typed_section(SectionType::MgmNode, &[]);
    b.typed_section(
      SectionType::Tcp,
      &[(CFG_TCP_SEND_BUFFER_SIZE, ConfigValue::Int(2097152))],
    );
    b.typed_section(SectionType::Shm, &[]);
    b.typed_section(SectionType::System, &[]);
    b.typed_section(
      SectionType::DataNode,
      &[
        (CFG_NODE_ID, ConfigValue::Int(1)),
        (CFG_NODE_HOST, ConfigValue::Str("node1.example".to_string())),
      ],
    );
    b.typed_section(
      SectionType::DataNode,
      &[
        (CFG_NODE_ID, ConfigValue::Int(2)),
        (CFG_NODE_HOST, ConfigValue::Str("node2.example".to_string())),
        (CFG_DB_NODEGROUP, ConfigValue::Int(1)),
        (CFG_DB_API_HEARTBEAT_INTERVAL, ConfigValue::Int(800)),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[
        (CFG_NODE_ID, ConfigValue::Int(68)),
        (
          CFG_TOTAL_SEND_BUFFER_MEMORY,
          ConfigValue::Int64(8 * 1024 * 1024),
        ),
      ],
    );
    b.typed_section(
      SectionType::MgmNode,
      &[
        (CFG_NODE_ID, ConfigValue::Int(65)),
        (CFG_NODE_HOST, ConfigValue::Str("mgm.example".to_string())),
      ],
    );
    /* Our link to data node 1: the data node listens, port is dynamic,
    and the host comes from the node section. */
    b.typed_section(
      SectionType::Tcp,
      &[
        (CFG_CONNECTION_NODE_1, ConfigValue::Int(1)),
        (CFG_CONNECTION_NODE_2, ConfigValue::Int(68)),
        (CFG_CONNECTION_NODE_ID_SERVER, ConfigValue::Int(1)),
        (CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(0)),
      ],
    );
    /* Our link to data node 2: fixed port, host named on the link,
    checksum on. */
    b.typed_section(
      SectionType::Tcp,
      &[
        (CFG_CONNECTION_NODE_1, ConfigValue::Int(2)),
        (CFG_CONNECTION_NODE_2, ConfigValue::Int(68)),
        (CFG_CONNECTION_NODE_ID_SERVER, ConfigValue::Int(2)),
        (CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(45000)),
        (
          CFG_CONNECTION_HOSTNAME_1,
          ConfigValue::Str("10.0.0.2".to_string()),
        ),
        (CFG_CONNECTION_CHECKSUM, ConfigValue::Int(1)),
        (CFG_TCP_MAXSEG_SIZE, ConfigValue::Int(61440)),
      ],
    );
    /* A link between the two data nodes, which is none of our business. */
    b.typed_section(
      SectionType::Tcp,
      &[
        (CFG_CONNECTION_NODE_1, ConfigValue::Int(1)),
        (CFG_CONNECTION_NODE_2, ConfigValue::Int(2)),
        (CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(44000)),
      ],
    );
    ConfigBlob::decode(&b.finish()).expect("decode")
  }

  #[test]
  fn our_own_settings() {
    let config = ClusterConfig::from_blob(sample(), 68).expect("config");
    assert_eq!(config.api.node_id, 68);
    assert_eq!(config.api.batch_size, 256);
    assert_eq!(config.api.batch_byte_size, 16384);
    /* Not stated anywhere, so the fallback applies. */
    assert_eq!(config.api.max_scan_batch_size, DEFAULT_MAX_SCAN_BATCH_SIZE);
    assert_eq!(config.api.total_send_buffer_memory, 8 * 1024 * 1024);
    assert_eq!(config.api.host, None);
    assert_eq!(config.no_of_replicas, 2);
  }

  #[test]
  fn the_data_nodes() {
    let config = ClusterConfig::from_blob(sample(), 68).expect("config");
    assert_eq!(config.data_nodes.len(), 2);
    let one = config.data_node(1).expect("node 1");
    assert_eq!(one.host, "node1.example");
    assert_eq!(one.node_group, 0);
    assert_eq!(one.api_heartbeat_interval_ms, 1500);
    let two = config.data_node(2).expect("node 2");
    assert_eq!(two.node_group, 1);
    assert_eq!(two.api_heartbeat_interval_ms, 800);
    /* The period we must keep is the shortest anyone asks for. */
    assert_eq!(config.heartbeat_interval_ms(), 800);
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
    assert_eq!(to_two.tcp_maxseg_size, 61440);
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
      config.blob.value_str(node, CFG_NODE_HOST),
      Some("node2.example")
    );
  }
}
