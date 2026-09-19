// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! One connection to one data node
//! (`legacy-c/api/ic_apid_send_thread.ic`).
//!
//! Getting from a configuration to a working connection takes four
//! steps, and the third is the one that catches people out:
//!
//! 1. find the link between us and the node in the configuration;
//! 2. if its port is zero, the node was given one when it started, so
//!    ask the management server what it is;
//! 3. connect and complete the transporter handshake;
//! 4. send a heartbeat and wait for the node to accept us.
//!
//! Until step 4 the node has not registered us, and signals sent before
//! it are not answered.
//!
//! This module is the single-threaded form: one thread owns the
//! connection and does its own sending and receiving. The send and
//! receive threads of the full design are built on top of it.

use ic_apic::conf_param::IC_CFG_CONNECTION_SERVER_PORT;
use ic_apic::data::ClusterConfig;
use ic_apic::data::TcpLinkConfig;
use ic_apic::mgm_client::MgmClient;
use ic_comm::connection::ConnectConfig;
use ic_comm::connection::Connection;
use ic_ndb_signals::blocks;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header;
use ic_ndb_signals::header::FragmentInfo;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::qmgr::ApiRegConf;
use ic_ndb_signals::qmgr::ApiRegRef;
use ic_ndb_signals::qmgr::ApiRegReq;
use ic_ndb_signals::qmgr::NodeState;
use ic_port::consts::IC_MYSQL_VERSION;
use ic_port::consts::IC_NDB_VERSION;
use ic_port::debug::IC_HEARTBEAT_LEVEL;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;

use crate::handshake;
use crate::signal_reader::SignalReader;

/// How long to wait for a data node to accept us before giving up.
pub const IC_REGISTRATION_TIMEOUT_MS: u32 = 10_000;

/// A connection to one data node, with the signals going over it.
pub struct NodeConnection {
  /// The data node at the far end.
  pub node_id: u32,
  /// Our own node id.
  pub own_node_id: u32,
  /// Our block reference for cluster membership traffic.
  pub block_ref: u32,
  /// Whether this link carries a checksum on every signal.
  pub use_checksum: bool,
  /// What the node last told us about itself, once it has answered.
  pub node_state: Option<NodeState>,
  /// How often the node expects to hear from us, in milliseconds.
  pub heartbeat_interval_ms: u32,
  conn: Connection,
  reader: SignalReader,
  send_buf: Vec<u32>,
}

impl std::fmt::Debug for NodeConnection {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    let level = match self.node_state {
      Some(state) => format!("{:?}", state.start_level),
      None => "not registered".to_string(),
    };
    write!(
      f,
      "NodeConnection(node {} from {}, {})",
      self.node_id, self.own_node_id, level
    )
  }
}

/// The port to dial for a link, asking the management server when the
/// configuration says the node picks its own.
///
/// The management server answers with a signed number whose sign says
/// where the port came from: a negative value is a port assigned when
/// the node started, and the port is its absolute value, so `-59733`
/// means port 59733. Zero means the node has not been given one yet,
/// which is what an unstarted node looks like.
pub fn resolve_port(
  link: &TcpLinkConfig,
  own_node_id: u32,
  mgm: &mut MgmClient,
) -> Result<u16, IcError> {
  if !link.port_is_dynamic() {
    return Ok(link.server_port);
  }
  let other = link.other_node(own_node_id);
  // The nodes are named in the order the configuration has them, which
  // is not necessarily ours first.
  let value = mgm.get_connection_parameter(
    link.node_1,
    link.node_2,
    IC_CFG_CONNECTION_SERVER_PORT,
  )?;
  let port = value.unsigned_abs();
  if port == 0 || port > u16::MAX as u32 {
    ic_port::ic_printf!(
      "Node {} has no port yet; it is probably still starting",
      other
    );
    return Err(IcError::new(err::IC_ERROR_NODE_DOWN));
  }
  ic_port::debug_print!(
    ic_port::debug::IC_COMM_LEVEL,
    "Node {} listens on port {}{}",
    other,
    port,
    if value < 0 {
      " (assigned at startup)"
    } else {
      ""
    }
  );
  Ok(port as u16)
}

/// A signal taken off the wire, owning its words.
///
/// The words are copied out of the receive buffer so that the buffer
/// can be reused while the signal is handled. A signal handed to
/// another thread has to own its words in any case.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReceivedSignal {
  /// Which signal it is.
  pub gsn: u16,
  /// The block it is addressed to, which is what routing goes by: a
  /// user thread's block, or one of our fixed blocks.
  pub receiver_block: u16,
  /// The block that sent it, for replying.
  pub sender_block: u16,
  /// The node that sent it.
  pub sender_node_id: u32,
  /// Where it stands in a train of fragments, when the sender had to
  /// split a large signal. A fragment's data ends with the numbers of
  /// the sections it carries and a fragment id; see
  /// [`fragments`](crate::fragments).
  pub fragment_info: FragmentInfo,
  /// The signal data words.
  pub data: Vec<u32>,
  /// The sections that were present, in order.
  pub sections: Vec<Vec<u32>>,
}

impl ReceivedSignal {
  /// One section, or an empty slice when the signal did not carry it.
  pub fn section(&self, index: usize) -> &[u32] {
    match self.sections.get(index) {
      Some(section) => section,
      None => &[],
    }
  }
}

impl NodeConnection {
  /// Connect to a data node and register with it, so that it will
  /// answer our signals.
  ///
  /// The port is asked of the management server, because a data node
  /// that has restarted comes back on a different one. A caller that
  /// manages the management connection itself should ask with
  /// [`resolve_port`] and call [`connect_to_port`] instead, so that a
  /// management server that has gone away can be told apart from a data
  /// node that has.
  ///
  /// [`connect_to_port`]: NodeConnection::connect_to_port
  pub fn connect(
    config: &ClusterConfig,
    mgm: &mut MgmClient,
    node_id: u32,
  ) -> Result<NodeConnection, IcError> {
    let _dbg = ic_port::debug_entry!("NodeConnection::connect");
    let link = match config.link_to(node_id) {
      Some(link) => link.clone(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    let port = resolve_port(&link, config.api.node_id, mgm)?;
    NodeConnection::connect_to_port(config, node_id, port)
  }

  /// Connect to a data node on a port already known and register with
  /// it.
  pub fn connect_to_port(
    config: &ClusterConfig,
    node_id: u32,
    port: u16,
  ) -> Result<NodeConnection, IcError> {
    let _dbg = ic_port::debug_entry!("NodeConnection::connect_to_port");
    let link = match config.link_to(node_id) {
      Some(link) => link.clone(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    let own_node_id = config.api.node_id;
    if !link.we_dial(own_node_id) {
      // The configuration says this node dials us, which needs a
      // listening socket this release does not have.
      ic_port::ic_printf!(
        "The configuration expects node {} to connect to us, which is \
         not supported",
        node_id
      );
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let connect_config = ConnectConfig {
      server_name: link.server_host.clone(),
      server_port: port,
      tcp_maxseg: link.tcp_maxseg_size,
      tcp_receive_buffer_size: link.tcp_rcv_buf_size,
      tcp_send_buffer_size: link.tcp_snd_buf_size,
      connect_timeout_ms: 10_000,
      ..ConnectConfig::default()
    };
    let transporter =
      handshake::connect_to_data_node(&connect_config, own_node_id, node_id)?;
    let mut reader = SignalReader::new();
    reader.push_bytes(&transporter.leftover);
    let heartbeat = match config.data_node(node_id) {
      Some(node) => node.api_heartbeat_interval_ms,
      None => 0,
    };
    let mut node = NodeConnection {
      node_id,
      own_node_id,
      block_ref: blocks::number_to_ref(
        blocks::IC_BLOCK_API_CLUSTERMGR,
        own_node_id,
      ),
      use_checksum: link.checksum,
      node_state: None,
      heartbeat_interval_ms: heartbeat,
      conn: transporter.conn,
      reader,
      send_buf: Vec::with_capacity(header::IC_MAX_MESSAGE_WORDS),
    };
    node.register(IC_REGISTRATION_TIMEOUT_MS)?;
    Ok(node)
  }

  /// The socket, for adding to a poll set.
  pub fn fd(&self) -> i32 {
    self.conn.fd()
  }

  /// False once the connection has failed or been closed.
  pub fn is_connected(&self) -> bool {
    self.conn.is_connected()
  }

  /// Close the connection.
  pub fn close(&mut self) {
    self.conn.close();
    self.reader.reset();
  }

  /// Send one signal.
  pub fn send_signal(
    &mut self,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
  ) -> Result<(), IcError> {
    self.send_buf.clear();
    header::encode(
      header,
      data,
      sections,
      self.use_checksum,
      &mut self.send_buf,
    )?;
    ic_port::debug_print!(
      IC_NDB_MESSAGE_LEVEL,
      "-> node {} {} ({} words, {} sections)",
      self.node_id,
      gsn::gsn_name(header.gsn()).unwrap_or("unknown"),
      data.len(),
      sections.len()
    );
    let bytes = words_as_bytes(&self.send_buf);
    self.conn.write(bytes)
  }

  /// Send a heartbeat, which is also what tells a data node we are
  /// still here.
  pub fn send_heartbeat(&mut self) -> Result<(), IcError> {
    let request = ApiRegReq {
      block_ref: self.block_ref,
      version: IC_NDB_VERSION,
      mysql_version: IC_MYSQL_VERSION,
    };
    let header = SignalHeader::new(
      gsn::IC_GSN_API_REGREQ,
      blocks::IC_BLOCK_API_CLUSTERMGR,
      blocks::IC_BLOCK_QMGR,
    );
    self.send_signal(&header, &request.encode(), &[])
  }

  /// Wait for one signal and return its number and data.
  ///
  /// The data is copied out, because the next read moves what is left
  /// in the buffer.
  pub fn receive_signal(
    &mut self,
    wait_ms: u32,
  ) -> Result<Option<(u16, Vec<u32>)>, IcError> {
    let deadline = ic_port::time::gethrtime();
    loop {
      if self.reader.has_message() {
        let words = self.reader.complete_words();
        let message = header::decode(words)?;
        let gsn_value = message.header.gsn();
        let data = message.data.to_vec();
        let len = message.total_words;
        ic_port::debug_print!(
          IC_NDB_MESSAGE_LEVEL,
          "<- node {} {} ({} words)",
          self.node_id,
          gsn::gsn_name(gsn_value).unwrap_or("unknown"),
          data.len()
        );
        self.reader.consume(len);
        return Ok(Some((gsn_value, data)));
      }
      let waited =
        ic_port::time::millis_elapsed(deadline, ic_port::time::gethrtime());
      if waited >= wait_ms as u64 {
        return Ok(None);
      }
      let left = (wait_ms as u64 - waited) as i32;
      if !self.conn.check_for_data(left) {
        continue;
      }
      let size = self.reader.read_from(&self.conn)?;
      if size == 0 {
        return Err(IcError::new(err::IC_ERROR_LINK_LOST));
      }
    }
  }

  /// Send a heartbeat and wait for the node to accept us.
  ///
  /// Until this succeeds the node has not registered us and will not
  /// answer anything else.
  pub fn register(&mut self, wait_ms: u32) -> Result<(), IcError> {
    let _dbg = ic_port::debug_entry!("NodeConnection::register");
    self.send_heartbeat()?;
    let start = ic_port::time::gethrtime();
    loop {
      let waited =
        ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
      if waited >= wait_ms as u64 {
        return Err(IcError::new(err::IC_ERROR_TIMEOUT_WAITING_FOR_NODES));
      }
      let left = (wait_ms as u64 - waited) as u32;
      let received = self.receive_signal(left)?;
      let (gsn_value, data) = match received {
        Some(pair) => pair,
        None => continue,
      };
      if gsn_value == gsn::IC_GSN_API_REGCONF {
        let conf = ApiRegConf::decode(&data)?;
        ic_port::debug_print!(
          IC_HEARTBEAT_LEVEL,
          "Node {} registered us: start level {:?}, node group {}",
          self.node_id,
          conf.node_state.start_level,
          conf.node_state.node_group
        );
        if conf.api_heartbeat_interval != 0 {
          // The signal carries hundredths of a second; this field and
          // the configuration are both in milliseconds.
          self.heartbeat_interval_ms = conf.heartbeat_interval_ms();
        }
        self.node_state = Some(conf.node_state);
        return Ok(());
      }
      if gsn_value == gsn::IC_GSN_API_REGREF {
        let refusal = ApiRegRef::decode(&data)?;
        ic_port::ic_printf!(
          "Node {} refused us: {:?}",
          self.node_id,
          refusal.reason()
        );
        return Err(IcError::new(err::IC_AUTHENTICATE_ERROR));
      }
      // Anything else this early is not for us; a data node sends a
      // few reports as soon as a connection opens.
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "Ignoring signal {} while registering with node {}",
        gsn::gsn_name(gsn_value).unwrap_or("unknown"),
        self.node_id
      );
    }
  }

  /// Read whatever has arrived and return every whole signal in it,
  /// without waiting.
  ///
  /// This is what a receive thread calls once a poll set says the
  /// socket is ready. An empty result means the bytes that arrived did
  /// not complete a signal, which is ordinary.
  pub fn read_available(&mut self) -> Result<Vec<ReceivedSignal>, IcError> {
    let size = self.reader.read_from(&self.conn)?;
    if size == 0 {
      // The peer closed, which we notice now rather than when the next
      // send fails. It is the link that is known to be gone, not the
      // node: only another data node's failure report says that.
      return Err(IcError::new(err::IC_ERROR_LINK_LOST));
    }
    let mut signals: Vec<ReceivedSignal> = Vec::new();
    take_signals(&mut self.reader, self.node_id, &mut signals)?;
    Ok(signals)
  }

  /// Give up the connection as its two halves: the socket, and the
  /// reader holding any bytes that arrived past the registration. A
  /// connect thread does this to hand a registered link to the receive
  /// thread that will own it from then on.
  pub fn into_parts(self) -> (Connection, SignalReader) {
    (self.conn, self.reader)
  }

  /// Take what a signal says about the node, if it is one that speaks
  /// about node state.
  pub fn apply_signal(&mut self, gsn_value: u16, data: &[u32]) -> bool {
    if gsn_value != gsn::IC_GSN_API_REGCONF {
      return false;
    }
    let conf = match ApiRegConf::decode(data) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    if conf.api_heartbeat_interval != 0 {
      self.heartbeat_interval_ms = conf.heartbeat_interval_ms();
    }
    self.node_state = Some(conf.node_state);
    true
  }

  /// Send a heartbeat and read the answer, which is what keeps a
  /// connection alive.
  pub fn heartbeat_round(&mut self, wait_ms: u32) -> Result<(), IcError> {
    self.send_heartbeat()?;
    self.register(wait_ms)
  }

  /// True when the node says it is started and can serve us.
  pub fn is_started(&self) -> bool {
    match self.node_state {
      Some(state) => state.start_level.is_started(),
      None => false,
    }
  }
}

/// Take every complete signal the reader holds, appending them to
/// `out`, without reading the socket. Bytes of a signal not yet complete
/// stay in the reader for the next read.
///
/// Shared by a connection that reads for itself and by a receive thread
/// that owns the reader of a link handed to it.
pub fn take_signals(
  reader: &mut SignalReader,
  node_id: u32,
  out: &mut Vec<ReceivedSignal>,
) -> Result<(), IcError> {
  while reader.has_message() {
    let len = {
      let words = reader.complete_words();
      let message = header::decode(words)?;
      let gsn_value = message.header.gsn();
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "<- node {} {} ({} words, {} section(s))",
        node_id,
        gsn::gsn_name(gsn_value).unwrap_or("unknown"),
        message.data.len(),
        message.header.num_sections
      );
      let mut sections: Vec<Vec<u32>> = Vec::new();
      let mut index: usize = 0;
      while index < message.header.num_sections as usize {
        sections.push(message.sections[index].to_vec());
        index += 1;
      }
      out.push(ReceivedSignal {
        gsn: gsn_value,
        receiver_block: message.header.receiver_block,
        sender_block: message.header.sender_block,
        sender_node_id: node_id,
        fragment_info: message.header.fragment_info,
        data: message.data.to_vec(),
        sections,
      });
      message.total_words
    };
    reader.consume(len);
  }
  Ok(())
}

/// The words of a message seen as the bytes to put on the wire. No
/// conversion happens: the protocol carries the sender's own order.
pub(crate) fn words_as_bytes(words: &[u32]) -> &[u8] {
  let ptr = words.as_ptr() as *const u8;
  // SAFETY: the pointer comes from a live slice of that many words, so
  // the range covers exactly its storage, and every byte pattern is a
  // valid u8.
  unsafe { std::slice::from_raw_parts(ptr, words.len() * 4) }
}

#[cfg(test)]
mod tests {
  use super::ReceivedSignal;

  #[test]
  fn a_signal_without_sections_reads_them_as_empty() {
    // A handler asks for the section it expects without first asking
    // whether it is there, so an absent one has to read as empty
    // rather than panic.
    let signal = ReceivedSignal {
      gsn: 26,
      data: vec![1, 2, 3],
      ..ReceivedSignal::default()
    };
    assert!(signal.section(0).is_empty());
    assert!(signal.section(2).is_empty());
  }

  #[test]
  fn a_signal_hands_back_the_sections_it_carries() {
    let signal = ReceivedSignal {
      gsn: 26,
      data: vec![1, 2, 3],
      sections: vec![vec![0x10], vec![0x20, 0x21]],
      ..ReceivedSignal::default()
    };
    assert_eq!(signal.section(0), &[0x10]);
    assert_eq!(signal.section(1), &[0x20, 0x21]);
    assert!(signal.section(2).is_empty());
  }

  use super::*;
  use ic_apic::data::ApiNodeConfig;

  fn link(port: u16, server: u32) -> TcpLinkConfig {
    TcpLinkConfig {
      node_1: 2,
      node_2: 192,
      server_node_id: server,
      server_port: port,
      server_host: "127.0.0.1".to_string(),
      ..TcpLinkConfig::default()
    }
  }

  #[test]
  fn a_fixed_port_needs_no_lookup() {
    // resolve_port only reaches for the management server when the
    // configuration gives no port, so a fixed one returns as it is.
    let fixed = link(45000, 2);
    assert!(!fixed.port_is_dynamic());
    assert_eq!(fixed.server_port, 45000);
    let dynamic = link(0, 2);
    assert!(dynamic.port_is_dynamic());
  }

  #[test]
  fn the_far_end_is_identified() {
    let l = link(0, 2);
    assert_eq!(l.other_node(192), 2);
    assert!(l.we_dial(192));
    // If the configuration made us the server we would have to listen.
    let inverted = link(0, 192);
    assert!(!inverted.we_dial(192));
  }

  #[test]
  fn words_become_bytes_without_conversion() {
    let words = [0x0102_0304u32, 0x0506_0708];
    let bytes = words_as_bytes(&words);
    assert_eq!(bytes.len(), 8);
    // Whatever this machine's order is, the bytes are the words as it
    // stores them, which is what the protocol asks for.
    assert_eq!(&bytes[..4], &words[0].to_ne_bytes());
    assert_eq!(&bytes[4..], &words[1].to_ne_bytes());
  }

  #[test]
  fn a_connection_without_a_link_is_refused() {
    let config = ClusterConfig {
      api: ApiNodeConfig {
        node_id: 192,
        ..ApiNodeConfig::default()
      },
      ..ClusterConfig::default()
    };
    assert!(config.link_to(2).is_none());
  }
}
