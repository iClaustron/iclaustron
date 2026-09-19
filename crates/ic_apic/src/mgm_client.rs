// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The conversation with a management server
//! (`legacy-c/api/ic_apic_conf_read_proto.ic`).
//!
//! An API node starts knowing only a connectstring. It connects to a
//! management server, asks for a node id, asks for the configuration,
//! and from then on knows the cluster. The same connection can later be
//! turned into a transporter connection, which is how a node reaches a
//! data node it cannot dial directly.
//!
//! The exchange, each step a request ending in an empty line and a
//! reply ending in an empty line:
//!
//! ```text
//!   get version        ->  version, to check the server is new enough
//!   get nodeid         ->  our node id
//!   get config_v2      ->  the configuration, base64 after the header
//! ```
//!
//! Verified against RonDB 26.10 `src/mgmapi/mgmapi.cpp`: arguments are
//! written as `name: value` for numbers and `name:"value"` for text,
//! with no space before the quote, and a line may not exceed 512 bytes.

use std::collections::HashMap;

use ic_comm::connection::ConnectConfig;
use ic_comm::connection::Connection;
use ic_comm::line_proto;
use ic_comm::line_proto::LineReader;
use ic_port::consts::IC_NDB_VERSION;
use ic_port::debug::IC_COMM_LEVEL;
use ic_port::debug::IC_CONFIG_PROTO_LEVEL;
use ic_port::err;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_UNORDERED;
use ic_port::IcError;
use ic_protocol::base64;
use ic_protocol::proto_str::*;
use ic_util::connectstring::ConnectString;
use ic_util::connectstring::MgmServer;

use crate::conf_blob::ConfigBlob;
use crate::data::ClusterConfig;

/// How long to wait for a management server to answer, unless told
/// otherwise.
pub const IC_DEFAULT_MGM_TIMEOUT_MS: u32 = 30_000;

/// The oldest management server this library will talk to, as
/// `(major, minor)`. Older servers do not serve the version 2
/// configuration format.
pub const IC_MIN_MGM_VERSION: (u32, u32) = (24, 10);

/// What a management server reports about itself.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MgmVersion {
  /// Major version.
  pub major: u32,
  /// Minor version.
  pub minor: u32,
  /// Build number.
  pub build: u32,
  /// The version as the server spells it.
  pub text: String,
}

/// One argument of a request.
enum Arg<'a> {
  /// Written as `name: value`.
  Number(u64),
  /// Written as `name:"value"`.
  Text(&'a str),
}

/// The management server's error code for a node id request that was
/// refused for good: asking again will not change the answer. Every
/// other refusal may succeed if asked again.
/// Verify: `mgmapi_error.h`, the alloc node id failures.
pub const IC_MGM_ALLOCID_NOT_RETRIABLE: u32 = 1102;
/// The words with which the management server says an id is held by
/// another node. The error code does not tell this refusal from "the
/// cluster is not ready", both being "retry may succeed", so the text
/// is the only way to know. The server documents that the MySQL server
/// matches on these texts too, which makes them interface in practice.
/// Verify: `MgmtSrvr.cpp`, the end of `alloc_node_id_impl`.
pub const IC_MGM_TEXT_ID_HELD: &str = "already allocated by another node";

/// What the management server last said when it refused a request, in
/// its own words. Like `errno`, it is only meaningful straight after a
/// call failed with a refusal.
static LAST_REFUSAL: IcMutex<String> =
  IcMutex::new(IC_MUTEX_LEVEL_UNORDERED, String::new());

/// The management server's own words for its last refusal.
pub fn last_refusal() -> String {
  LAST_REFUSAL.lock().clone()
}

/// True when an error means a management server answered and said no,
/// which is when [`last_refusal`] has something to say. An unreachable
/// server is not a refusal.
pub fn is_refusal(code: i32) -> bool {
  code == err::IC_ERROR_MGM_SERVER_REFUSED
    || code == err::IC_ERROR_NO_NODEID
    || code == err::IC_ERROR_NODEID_IN_USE
    || code == err::IC_ERROR_NODEID_NOT_ALLOWED
}

/// A connection to a management server, with the conversation on top.
pub struct MgmClient {
  conn: Connection,
  reader: LineReader,
  version: MgmVersion,
  host: String,
  port: u16,
}

impl MgmClient {
  /// Connect to one management server and ask its version.
  pub fn connect(
    host: &str,
    port: u16,
    timeout_ms: u32,
  ) -> Result<MgmClient, IcError> {
    let _dbg = ic_port::debug_entry!("MgmClient::connect");
    let config = ConnectConfig {
      server_name: host.to_string(),
      server_port: port,
      connect_timeout_ms: timeout_ms,
      ..ConnectConfig::default()
    };
    let conn = Connection::connect(&config)?;
    conn.set_read_timeout_ms(Some(timeout_ms))?;
    conn.set_write_timeout_ms(Some(timeout_ms))?;
    let mut client = MgmClient {
      conn,
      reader: LineReader::new(),
      version: MgmVersion::default(),
      host: host.to_string(),
      port,
    };
    client.version = client.read_version()?;
    if client.version.major < IC_MIN_MGM_VERSION.0 {
      return Err(IcError::new(err::IC_ERROR_MGM_VERSION_TOO_OLD));
    }
    Ok(client)
  }

  /// Try every management server in the connectstring in turn and
  /// return the first that answers.
  pub fn connect_any(
    connect_string: &ConnectString,
    timeout_ms: u32,
  ) -> Result<MgmClient, IcError> {
    let mut last = IcError::new(err::IC_ERROR_NO_SERVER_NAME);
    for server in &connect_string.servers {
      match MgmClient::connect(&server.host, server.port, timeout_ms) {
        Ok(client) => return Ok(client),
        Err(e) => {
          ic_port::debug_print!(
            IC_CONFIG_PROTO_LEVEL,
            "Management server {}:{} did not answer: {}",
            server.host,
            server.port,
            e.code
          );
          last = e;
        }
      }
    }
    Err(last)
  }

  /// The server we are talking to.
  pub fn server(&self) -> MgmServer {
    MgmServer {
      host: self.host.clone(),
      port: self.port,
    }
  }

  /// What the server reported about its version.
  pub fn version(&self) -> &MgmVersion {
    &self.version
  }

  /// The underlying connection, for adding to a poll set.
  pub fn connection(&self) -> &Connection {
    &self.conn
  }

  fn send_request(
    &self,
    command: &str,
    args: &[(&str, Arg<'_>)],
  ) -> Result<(), IcError> {
    line_proto::send_line(&self.conn, command)?;
    for (name, value) in args {
      match value {
        Arg::Number(number) => {
          line_proto::send_number(&self.conn, name, *number)?;
        }
        Arg::Text(text) => {
          line_proto::send_quoted_value(&self.conn, name, text)?;
        }
      }
    }
    line_proto::send_empty_line(&self.conn)
  }

  /// Read a reply: its first line must be `expected`, then the
  /// `name: value` lines up to the empty line, returned by name.
  fn read_reply(
    &mut self,
    expected: &str,
  ) -> Result<HashMap<String, String>, IcError> {
    self.reader.expect_line(&self.conn, expected)?;
    let lines = self.reader.read_until_empty_line(&self.conn)?;
    let mut fields: HashMap<String, String> = HashMap::new();
    for line in &lines {
      match line.find(':') {
        Some(colon) => {
          let name = line[..colon].trim().to_ascii_lowercase();
          let value = line[colon + 1..].trim().to_string();
          fields.insert(name, value);
        }
        None => return Err(IcError::new(err::IC_PROTOCOL_ERROR)),
      }
    }
    Ok(fields)
  }

  /// Check that a reply says `result: Ok`, reporting what it said if
  /// not.
  fn check_result(
    &self,
    fields: &HashMap<String, String>,
  ) -> Result<(), IcError> {
    let result = match fields.get(IC_ARG_RESULT) {
      Some(value) => value.as_str(),
      None => return Err(IcError::new(err::IC_PROTOCOL_ERROR)),
    };
    if result == IC_RESULT_OK {
      return Ok(());
    }
    // Traced, not printed: a caller that retries for the length of an
    // outage would otherwise fill the application's output. Whoever
    // wants the words asks for them with `last_refusal`.
    ic_port::debug_print!(
      IC_CONFIG_PROTO_LEVEL | IC_COMM_LEVEL,
      "Management server {}:{} refused: {}",
      self.host,
      self.port,
      result
    );
    let mut last = LAST_REFUSAL.lock();
    last.clear();
    last.push_str(result);
    Err(IcError::new(err::IC_ERROR_MGM_SERVER_REFUSED))
  }

  fn field_u32(
    fields: &HashMap<String, String>,
    name: &str,
  ) -> Result<u32, IcError> {
    let text = match fields.get(name) {
      Some(value) => value,
      None => return Err(IcError::new(err::IC_PROTOCOL_ERROR)),
    };
    match text.parse::<u32>() {
      Ok(value) => Ok(value),
      Err(_) => Err(IcError::new(err::IC_PROTOCOL_ERROR)),
    }
  }

  fn field_i32(
    fields: &HashMap<String, String>,
    name: &str,
  ) -> Result<i32, IcError> {
    let text = match fields.get(name) {
      Some(value) => value,
      None => return Err(IcError::new(err::IC_PROTOCOL_ERROR)),
    };
    match text.parse::<i32>() {
      Ok(value) => Ok(value),
      Err(_) => Err(IcError::new(err::IC_PROTOCOL_ERROR)),
    }
  }

  fn read_version(&mut self) -> Result<MgmVersion, IcError> {
    let _dbg = ic_port::debug_entry!("MgmClient::read_version");
    self.send_request(IC_CMD_GET_VERSION, &[])?;
    let fields = self.read_reply(IC_REPLY_GET_VERSION)?;
    let major = MgmClient::field_u32(&fields, "major")?;
    let minor = MgmClient::field_u32(&fields, "minor")?;
    /* A management server may leave the build number out. */
    let build = MgmClient::field_u32(&fields, "build").unwrap_or(0);
    let text = fields.get("string").cloned().unwrap_or_default();
    ic_port::debug_print!(
      IC_CONFIG_PROTO_LEVEL,
      "Management server {}:{} is version {}.{}.{} ({})",
      self.host,
      self.port,
      major,
      minor,
      build,
      text
    );
    Ok(MgmVersion {
      major,
      minor,
      build,
      text,
    })
  }

  /// Ask for a node id. `wanted_node_id` of 0 lets the server choose;
  /// any other value asks for that one and fails if it is taken or is
  /// not an API node (`ndb_mgm_alloc_nodeid`).
  pub fn alloc_node_id(
    &mut self,
    wanted_node_id: u32,
    name: Option<&str>,
  ) -> Result<u32, IcError> {
    let _dbg = ic_port::debug_entry!("MgmClient::alloc_node_id");
    let endian = if ic_port::endian::byte_order() == 0 {
      IC_ENDIAN_LITTLE
    } else {
      IC_ENDIAN_BIG
    };
    let mut args: Vec<(&str, Arg<'_>)> = vec![
      (IC_ARG_VERSION, Arg::Number(IC_NDB_VERSION as u64)),
      (IC_ARG_NODETYPE, Arg::Number(IC_NODE_TYPE_API as u64)),
      (IC_ARG_NODEID, Arg::Number(wanted_node_id as u64)),
      (IC_ARG_USER, Arg::Text(IC_USER_MYSQLD)),
      (IC_ARG_PASSWORD, Arg::Text(IC_USER_MYSQLD)),
      (IC_ARG_PUBLIC_KEY, Arg::Text(IC_PUBLIC_KEY_TEXT)),
      (IC_ARG_ENDIAN, Arg::Text(endian)),
    ];
    if let Some(text) = name {
      args.push((IC_ARG_NAME, Arg::Text(text)));
    }
    args.push((IC_ARG_LOG_EVENT, Arg::Number(1)));
    self.send_request(IC_CMD_GET_NODEID, &args)?;
    let fields = self.read_reply(IC_REPLY_GET_NODEID)?;
    if self.check_result(&fields).is_err() {
      return Err(IcError::new(node_id_refusal(&fields)));
    }
    let node_id = MgmClient::field_u32(&fields, IC_ARG_NODEID)?;
    ic_port::debug_print!(
      IC_CONFIG_PROTO_LEVEL,
      "Management server gave us node id {}",
      node_id
    );
    Ok(node_id)
  }

  /// Ask for the cluster configuration as `node_id` sees it, and
  /// return the decoded blob bytes
  /// (`ndb_mgm_get_configuration2`).
  pub fn get_config(&mut self, node_id: u32) -> Result<Vec<u8>, IcError> {
    let _dbg = ic_port::debug_entry!("MgmClient::get_config");
    self.send_request(
      IC_CMD_GET_CONFIG_V2,
      &[
        (IC_ARG_VERSION, Arg::Number(IC_NDB_VERSION as u64)),
        (IC_ARG_NODETYPE, Arg::Number(IC_NODE_TYPE_API as u64)),
        (IC_ARG_NODE, Arg::Number(node_id as u64)),
      ],
    )?;
    let fields = self.read_reply(IC_REPLY_GET_CONFIG)?;
    self.check_result(&fields)?;
    let content_type = fields.get("content-type");
    if content_type.map(|s| s.as_str()) != Some(IC_CONTENT_TYPE_CONFIG) {
      return Err(IcError::new(err::IC_PROTOCOL_ERROR));
    }
    let encoding = fields.get("content-transfer-encoding");
    if encoding.map(|s| s.as_str()) != Some(IC_CONTENT_ENCODING_BASE64) {
      return Err(IcError::new(err::IC_PROTOCOL_ERROR));
    }
    let length = MgmClient::field_u32(&fields, "content-length")? as usize;
    // The server writes one more byte than it counted: the newline
    // that ends the last line of base64.
    let encoded = self.reader.read_exact(&self.conn, length + 1)?;
    let blob = base64::decode(&encoded)?;
    ic_port::debug_print!(
      IC_CONFIG_PROTO_LEVEL,
      "Configuration is {} base64 characters, {} bytes decoded",
      length,
      blob.len()
    );
    Ok(blob)
  }

  /// Ask for one parameter of the link between two nodes, which is how
  /// a data node's port is found when the configuration says zero
  /// (`ndb_mgm_get_connection_int_parameter`).
  ///
  /// The answer is signed, and for a port the sign carries meaning: a
  /// negative value is a port the node was given when it started
  /// rather than one written in the configuration, and the port itself
  /// is the absolute value. A value of `-59733` means port 59733,
  /// assigned dynamically.
  pub fn get_connection_parameter(
    &mut self,
    node_1: u32,
    node_2: u32,
    parameter: u32,
  ) -> Result<i32, IcError> {
    let _dbg = ic_port::debug_entry!("MgmClient::get_connection_parameter");
    self.send_request(
      IC_CMD_GET_CONNECTION_PARAMETER,
      &[
        (IC_ARG_NODE1, Arg::Number(node_1 as u64)),
        (IC_ARG_NODE2, Arg::Number(node_2 as u64)),
        (IC_ARG_PARAM, Arg::Number(parameter as u64)),
      ],
    )?;
    let fields = self.read_reply(IC_REPLY_GET_CONNECTION_PARAMETER)?;
    self.check_result(&fields)?;
    MgmClient::field_i32(&fields, IC_ARG_VALUE)
  }

  /// Hand this connection over to the signal protocol: after this the
  /// management protocol is finished with it and the transporter
  /// handshake follows (`ndb_mgm_convert_to_transporter`).
  pub fn into_transporter(self) -> Result<Connection, IcError> {
    line_proto::send_line(&self.conn, IC_CMD_TRANSPORTER_CONNECT)?;
    line_proto::send_empty_line(&self.conn)?;
    Ok(self.conn)
  }
}

impl std::fmt::Debug for MgmClient {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "MgmClient({}:{}, server version {}.{}.{})",
      self.host,
      self.port,
      self.version.major,
      self.version.minor,
      self.version.build
    )
  }
}

/// Which error a refused node id request amounts to. The three kinds
/// call for different things: wait and ask again, give the id up, or
/// stop asking for it altogether.
fn node_id_refusal(fields: &HashMap<String, String>) -> i32 {
  let mut server_code: u32 = 0;
  if let Some(text) = fields.get(IC_ARG_ERROR_CODE) {
    if let Ok(value) = text.parse::<u32>() {
      server_code = value;
    }
  }
  if server_code == IC_MGM_ALLOCID_NOT_RETRIABLE {
    return err::IC_ERROR_NODEID_NOT_ALLOWED;
  }
  if let Some(result) = fields.get(IC_ARG_RESULT) {
    if result.contains(IC_MGM_TEXT_ID_HELD) {
      return err::IC_ERROR_NODEID_IN_USE;
    }
  }
  // The cluster is not ready, the id is still being cleared up after
  // its last holder, or anything else that time may cure.
  err::IC_ERROR_NO_NODEID
}

/// Connect to a management server, get a node id and the configuration,
/// and read it as that node (`ic_get_configuration`).
///
/// `wanted_node_id` of 0 lets the server choose. The client is returned
/// alongside the configuration because the same connection answers
/// later questions, such as the dynamic port of a data node.
pub fn fetch_configuration(
  connect_string: &ConnectString,
  timeout_ms: u32,
  name: Option<&str>,
) -> Result<(ClusterConfig, MgmClient), IcError> {
  let _dbg = ic_port::debug_entry!("fetch_configuration");
  let mut client = MgmClient::connect_any(connect_string, timeout_ms)?;
  let wanted = connect_string.node_id.unwrap_or(0);
  let node_id = client.alloc_node_id(wanted, name)?;
  let bytes = client.get_config(node_id)?;
  let blob = ConfigBlob::decode(&bytes)?;
  let config = ClusterConfig::from_blob(blob, node_id)?;
  Ok((config, client))
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::conf_blob::BlobBuilder;
  use crate::conf_blob::ConfigValue;
  use crate::conf_blob::SectionType;
  use crate::conf_param::*;
  use std::io::Read;
  use std::io::Write;
  use std::net::TcpListener;

  /// A configuration a pretend management server can serve.
  fn sample_blob() -> Vec<u8> {
    let mut b = BlobBuilder::new(1, 1, 1, 1);
    b.typed_section(
      SectionType::DataNode,
      &[(IC_CFG_DB_NO_REPLICAS, ConfigValue::Int(1))],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[(IC_CFG_BATCH_SIZE, ConfigValue::Int(64))],
    );
    b.typed_section(SectionType::MgmNode, &[]);
    b.typed_section(SectionType::Tcp, &[]);
    b.typed_section(SectionType::Shm, &[]);
    b.typed_section(SectionType::System, &[]);
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_CFG_NODE_ID, ConfigValue::Int(1)),
        (IC_CFG_NODE_HOST, ConfigValue::Str("127.0.0.1".to_string())),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[(IC_CFG_NODE_ID, ConfigValue::Int(68))],
    );
    b.typed_section(
      SectionType::MgmNode,
      &[(IC_CFG_NODE_ID, ConfigValue::Int(65))],
    );
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_CFG_CONNECTION_NODE_1, ConfigValue::Int(1)),
        (IC_CFG_CONNECTION_NODE_2, ConfigValue::Int(68)),
        (IC_CFG_CONNECTION_NODE_ID_SERVER, ConfigValue::Int(1)),
        (IC_CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(0)),
      ],
    );
    b.finish()
  }

  /// A management server that answers the three requests of a startup,
  /// and records what it was asked.
  fn fake_mgmd(
    version_reply: &'static str,
    node_id_reply: &'static str,
  ) -> (u16, std::thread::JoinHandle<Vec<String>>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("addr").port();
    let handle = std::thread::spawn(move || {
      let (mut peer, _) = listener.accept().expect("accept");
      peer
        .set_read_timeout(Some(std::time::Duration::from_secs(10)))
        .expect("read timeout");
      let mut seen: Vec<String> = Vec::new();
      let mut pending = Vec::new();
      let mut buf = [0u8; 1024];
      let mut request_no = 0;
      loop {
        let size = match peer.read(&mut buf) {
          Ok(0) => break,
          Ok(n) => n,
          Err(_) => break,
        };
        pending.extend_from_slice(&buf[..size]);
        /* A request is finished when an empty line arrives. */
        while let Some(at) = find_blank_line(&pending) {
          // `at` is the index of the second newline, so the request
          // runs up to and including it.
          let text = String::from_utf8_lossy(&pending[..at]).to_string();
          pending.drain(..at + 1);
          for line in text.split('\n') {
            seen.push(line.to_string());
          }
          let reply = match request_no {
            0 => version_reply.to_string(),
            1 => node_id_reply.to_string(),
            _ => config_reply(),
          };
          request_no += 1;
          peer.write_all(reply.as_bytes()).expect("write");
          peer.flush().expect("flush");
        }
      }
      seen
    });
    (port, handle)
  }

  fn find_blank_line(buf: &[u8]) -> Option<usize> {
    let mut i: usize = 0;
    while i + 1 < buf.len() {
      if buf[i] == b'\n' && buf[i + 1] == b'\n' {
        return Some(i + 1);
      }
      i += 1;
    }
    None
  }

  fn config_reply() -> String {
    let encoded = base64::encode(&sample_blob());
    format!(
      "get config reply\nresult: Ok\nContent-Type: {}\n\
       Content-Transfer-Encoding: {}\nContent-Length: {}\n\n{}",
      IC_CONTENT_TYPE_CONFIG,
      IC_CONTENT_ENCODING_BASE64,
      encoded.len() - 1,
      encoded
    )
  }

  const VERSION_OK: &str =
    "version\nid: 1706240\nmajor: 26\nminor: 10\nbuild: 0\n\
     string: RonDB-26.10.0\n\n";
  const NODEID_OK: &str = "get nodeid reply\nnodeid: 68\nresult: Ok\n\n";

  fn connect_string(port: u16) -> ConnectString {
    ic_util::connectstring::parse(&format!("127.0.0.1:{}", port))
      .expect("parse")
  }

  #[test]
  fn a_whole_startup() {
    let (port, handle) = fake_mgmd(VERSION_OK, NODEID_OK);
    let cs = connect_string(port);
    let (config, client) =
      fetch_configuration(&cs, 5000, Some("test")).expect("fetch");
    assert_eq!(config.api.node_id, 68);
    assert_eq!(config.api.batch_size, 64);
    assert_eq!(config.data_nodes.len(), 1);
    assert_eq!(config.data_nodes[0].node_id, 1);
    assert_eq!(config.data_nodes[0].host, "127.0.0.1");
    assert_eq!(config.links.len(), 1);
    assert!(config.link_to(1).expect("link").port_is_dynamic());
    assert_eq!(client.version().major, 26);
    assert_eq!(client.version().minor, 10);
    assert_eq!(client.version().text, "RonDB-26.10.0");
    drop(client);
    let asked = handle.join().expect("join");
    // The three requests, in order, with arguments in the shape the
    // management server's parser expects.
    assert_eq!(asked[0], "get version");
    assert!(asked.contains(&"get nodeid".to_string()));
    assert!(asked.contains(&"nodetype: 1".to_string()));
    assert!(asked.contains(&"user:\"mysqld\"".to_string()));
    assert!(asked.contains(&"public key:\"a public key\"".to_string()));
    assert!(asked.contains(&"name:\"test\"".to_string()));
    assert!(asked.contains(&"get config_v2".to_string()));
    assert!(asked.contains(&"node: 68".to_string()));
  }

  #[test]
  fn a_refused_node_id_is_reported() {
    let refused = "get nodeid reply\nresult: Id 68 already allocated\n\n";
    let (port, handle) = fake_mgmd(VERSION_OK, refused);
    let cs = connect_string(port);
    let err = fetch_configuration(&cs, 5000, None).expect_err("refused");
    assert_eq!(err.code, err::IC_ERROR_NO_NODEID);
    let _ = handle.join();
  }

  fn refusal(result: &str, error_code: Option<&str>) -> i32 {
    let mut fields: HashMap<String, String> = HashMap::new();
    fields.insert(IC_ARG_RESULT.to_string(), result.to_string());
    if let Some(code) = error_code {
      fields.insert(IC_ARG_ERROR_CODE.to_string(), code.to_string());
    }
    node_id_refusal(&fields)
  }

  #[test]
  fn a_cluster_that_is_not_ready_is_worth_asking_again() {
    // Seen live during a cluster restart. It must not count as our id
    // having gone to somebody else, or a node would give up its
    // identity because the cluster was slow to come back.
    let code =
      refusal("Cluster not ready for nodeid allocation.", Some("1101"));
    assert_eq!(code, err::IC_ERROR_NO_NODEID);
  }

  #[test]
  fn an_id_held_by_another_node_is_told_apart_by_its_words() {
    // The server's code is the same 1101 as above, so only the text
    // says that waiting will not help.
    let code =
      refusal("Id 192 already allocated by another node.", Some("1101"));
    assert_eq!(code, err::IC_ERROR_NODEID_IN_USE);
    // A server too old to send the code is read the same way.
    let code = refusal("Id 192 already allocated by another node.", None);
    assert_eq!(code, err::IC_ERROR_NODEID_IN_USE);
  }

  #[test]
  fn an_id_reserved_on_this_management_server_is_not_held_for_good() {
    // This is a reservation that times out, quite possibly our own
    // from a moment ago, so it is worth asking again.
    let code =
      refusal("Id 192 is already allocated by this ndb_mgmd", Some("1101"));
    assert_eq!(code, err::IC_ERROR_NO_NODEID);
  }

  #[test]
  fn a_refusal_for_good_says_so_whatever_the_words() {
    let code =
      refusal("No node defined with id=192 in config file.", Some("1102"));
    assert_eq!(code, err::IC_ERROR_NODEID_NOT_ALLOWED);
  }

  #[test]
  fn the_servers_own_words_can_be_asked_for() {
    let refused =
      "get nodeid reply\nresult: Cluster not ready for nodeid allocation.\n\n";
    let (port, handle) = fake_mgmd(VERSION_OK, refused);
    let cs = connect_string(port);
    let _ = fetch_configuration(&cs, 5000, None).expect_err("refused");
    // Other tests refuse things too and the words are process wide, so
    // only check that something was kept.
    assert!(!last_refusal().is_empty());
    let _ = handle.join();
  }

  #[test]
  fn an_old_management_server_is_refused() {
    let old = "version\nid: 460032\nmajor: 7\nminor: 6\nbuild: 0\n\
               string: mysql-5.7\n\n";
    let (port, handle) = fake_mgmd(old, NODEID_OK);
    let cs = connect_string(port);
    let err = fetch_configuration(&cs, 5000, None).expect_err("too old");
    assert_eq!(err.code, err::IC_ERROR_MGM_VERSION_TOO_OLD);
    let _ = handle.join();
  }

  #[test]
  fn a_server_that_does_not_answer_is_skipped() {
    /* Two servers in the connectstring, the first one dead. */
    let (port, handle) = fake_mgmd(VERSION_OK, NODEID_OK);
    let cs =
      ic_util::connectstring::parse(&format!("127.0.0.1:1,127.0.0.1:{}", port))
        .expect("parse");
    let (config, client) = fetch_configuration(&cs, 2000, None).expect("fetch");
    assert_eq!(config.api.node_id, 68);
    /* Closing the connection is what tells the server to finish. */
    drop(client);
    let _ = handle.join();
  }

  #[test]
  fn nothing_answers_at_all() {
    let cs = ic_util::connectstring::parse("127.0.0.1:1").expect("parse");
    assert!(fetch_configuration(&cs, 500, None).is_err());
  }
}
