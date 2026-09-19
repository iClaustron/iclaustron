// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Turning a socket into a transporter connection to a data node
//! (`legacy-c/api/ic_apid_send_thread.ic`,
//! `authenticate_client_connection`).
//!
//! This is the one part of the protocol that has changed since the C was
//! written, so it is taken from RonDB 26.10 rather than from the C.
//!
//! Two exchanges of text lines, each ending in a newline, and then the
//! socket carries signals and nothing else:
//!
//! ```text
//!   ->  ndbd TLS disabled        what we can do about encryption
//!   ->                           an empty line
//!   <-  Cleartext ok             or "ok" from a server too old to know
//!
//!   ->  192 1 2 0                we are node 192, TCP, to node 2, link 0
//!   <-  2 1                      it is node 2, and agrees it is TCP
//! ```
//!
//! The first exchange used to be `ndbd` and `ndbd passwd` with `ok` in
//! reply, and the second used to send two numbers rather than four. The
//! form written here works with both: a server too old to understand
//! the TLS line reads it as a user name and a password and answers
//! `ok`, and a server too old for four numbers reads the first two and
//! ignores the rest.
//!
//! Verify: RonDB 26.10 `src/common/util/SocketAuthenticator.cpp:133`
//! and `src/common/transporter/Transporter.cpp:434`.

use ic_comm::connection::ConnectConfig;
use ic_comm::connection::Connection;
use ic_comm::line_proto;
use ic_comm::line_proto::LineReader;
use ic_port::debug::IC_COMM_LEVEL;
use ic_port::err;
use ic_port::IcError;

/// What kind of transporter this is. A data node link may also be
/// shared memory or RDMA between data nodes, but an API node uses TCP.
/// Verify: `TransporterDefinitions.hpp:49`.
pub const IC_TRANSPORTER_TYPE_TCP: u32 = 1;

/// The line we send to say what we can do about encryption. Cleartext
/// for now; TLS is a later release.
const IC_AUTH_LINE_CLEARTEXT: &str = "ndbd TLS disabled";
/// What a RonDB 26.10 server answers when it accepts cleartext.
const IC_AUTH_REPLY_CLEARTEXT: &str = "Cleartext ok";
/// What a server too old to know about TLS answers.
const IC_AUTH_REPLY_OK: &str = "ok";
/// The server insists on TLS, which this release cannot do.
const IC_AUTH_REPLY_TLS_REQUIRED: &str = "TLS required";

/// Longest hello line a data node will read. The limit is the one older
/// servers had, and RonDB keeps to it.
/// Verify: `Transporter.cpp:447`, `OldMaxHandshakeBytesLimit`.
pub const IC_MAX_HELLO_LEN: usize = 23;

/// A transporter connection, and anything that arrived on it before the
/// signals started.
pub struct TransporterConnection {
  /// The socket, now carrying signals.
  pub conn: Connection,
  /// The node at the far end.
  pub node_id: u32,
  /// Bytes read past the handshake, which are the first signal bytes
  /// and must be given to the receive path.
  pub leftover: Vec<u8>,
}

impl std::fmt::Debug for TransporterConnection {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "TransporterConnection(node {}, {}, {} bytes buffered)",
      self.node_id,
      self.conn.server_name(),
      self.leftover.len()
    )
  }
}

/// Say what we can do about encryption and read the answer.
///
/// Accepts cleartext in either of the two spellings a server may use,
/// and fails if the server insists on TLS.
pub fn authenticate(
  conn: &Connection,
  reader: &mut LineReader,
) -> Result<(), IcError> {
  let _dbg = ic_port::debug_entry!("handshake::authenticate");
  line_proto::send_line(conn, IC_AUTH_LINE_CLEARTEXT)?;
  line_proto::send_empty_line(conn)?;
  let reply = reader.read_line(conn)?;
  ic_port::debug_print!(IC_COMM_LEVEL, "Authentication reply: {}", reply);
  if reply == IC_AUTH_REPLY_CLEARTEXT || reply.starts_with(IC_AUTH_REPLY_OK) {
    return Ok(());
  }
  if reply == IC_AUTH_REPLY_TLS_REQUIRED {
    ic_port::ic_printf!(
      "The data node requires TLS, which this release does not support"
    );
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  ic_port::ic_printf!("Unexpected authentication reply: {}", reply);
  Err(IcError::new(err::IC_AUTHENTICATE_ERROR))
}

/// Say who we are and who we think we are talking to, and check that
/// the far end agrees.
pub fn say_hello(
  conn: &Connection,
  reader: &mut LineReader,
  own_node_id: u32,
  remote_node_id: u32,
) -> Result<(), IcError> {
  let _dbg = ic_port::debug_entry!("handshake::say_hello");
  // Our node id, the kind of transporter, the node we want, and which
  // link of a multi-link pair. An API node always uses link 0.
  let hello = format!(
    "{} {} {} 0",
    own_node_id, IC_TRANSPORTER_TYPE_TCP, remote_node_id
  );
  if hello.len() > IC_MAX_HELLO_LEN {
    // Only reachable with node ids far larger than any release allows.
    return Err(IcError::new(err::IC_ERROR_WRONG_NODE_ID));
  }
  line_proto::send_line(conn, &hello)?;
  let reply = reader.read_line(conn)?;
  ic_port::debug_print!(IC_COMM_LEVEL, "Hello reply: {}", reply);
  let (their_node_id, their_type) = parse_hello_reply(&reply)?;
  if their_node_id != remote_node_id {
    ic_port::ic_printf!(
      "Connected to node {} but expected node {}",
      their_node_id,
      remote_node_id
    );
    return Err(IcError::new(err::IC_ERROR_WRONG_NODE_ID));
  }
  if their_type != IC_TRANSPORTER_TYPE_TCP {
    ic_port::ic_printf!(
      "Node {} answered with transporter type {}, expected TCP",
      their_node_id,
      their_type
    );
    return Err(IcError::new(err::IC_AUTHENTICATE_ERROR));
  }
  Ok(())
}

/// Read the two numbers a hello reply carries: the node answering and
/// the kind of transporter it agrees to.
fn parse_hello_reply(reply: &str) -> Result<(u32, u32), IcError> {
  let bad = IcError::new(err::IC_AUTHENTICATE_ERROR);
  let mut numbers: Vec<u32> = Vec::new();
  for field in reply.split_whitespace() {
    match field.parse::<u32>() {
      Ok(value) => numbers.push(value),
      Err(_) => return Err(bad),
    }
  }
  if numbers.len() < 2 {
    // A server that refuses the connection answers "BYE" instead, and
    // a server in the wrong state may answer nothing sensible at all.
    return Err(bad);
  }
  Ok((numbers[0], numbers[1]))
}

/// Connect to a data node and complete both exchanges, leaving a socket
/// that carries signals.
pub fn connect_to_data_node(
  config: &ConnectConfig,
  own_node_id: u32,
  remote_node_id: u32,
) -> Result<TransporterConnection, IcError> {
  let _dbg = ic_port::debug_entry!("handshake::connect_to_data_node");
  let conn = Connection::connect(config)?;
  // The handshake is short; a node that does not answer promptly is a
  // node to give up on and retry later.
  conn.set_read_timeout_ms(Some(10_000))?;
  conn.set_write_timeout_ms(Some(10_000))?;
  let mut reader = LineReader::new();
  authenticate(&conn, &mut reader)?;
  say_hello(&conn, &mut reader, own_node_id, remote_node_id)?;
  // From here the socket carries signals. Whatever the reader pulled in
  // past the last line is the start of them.
  let leftover = reader.take_buffered();
  ic_port::debug_print!(
    IC_COMM_LEVEL,
    "Transporter connection to node {} established, {} bytes already read",
    remote_node_id,
    leftover.len()
  );
  // Signals are answered as they arrive; a blocking read with no
  // deadline is the receive thread's business, not ours.
  conn.set_read_timeout_ms(None)?;
  conn.set_write_timeout_ms(None)?;
  Ok(TransporterConnection {
    conn,
    node_id: remote_node_id,
    leftover,
  })
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::io::Read;
  use std::io::Write;
  use std::net::TcpListener;

  type ServerHandle = std::thread::JoinHandle<Vec<String>>;

  /// A pretend data node: reads the three lines a client sends and
  /// answers with the given script, then records what it saw.
  fn fake_data_node(
    auth_reply: &'static str,
    hello_reply: &'static str,
    trailing: &'static [u8],
  ) -> (u16, ServerHandle) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("addr").port();
    let handle = std::thread::spawn(move || {
      let (mut peer, _) = listener.accept().expect("accept");
      peer
        .set_read_timeout(Some(std::time::Duration::from_secs(10)))
        .expect("timeout");
      let mut seen: Vec<String> = Vec::new();
      let mut pending: Vec<u8> = Vec::new();
      let mut buf = [0u8; 256];
      let mut replied_auth = false;
      loop {
        let size = match peer.read(&mut buf) {
          Ok(0) => break,
          Ok(n) => n,
          Err(_) => break,
        };
        pending.extend_from_slice(&buf[..size]);
        while let Some(at) = pending.iter().position(|b| *b == b'\n') {
          let line = String::from_utf8_lossy(&pending[..at]).to_string();
          pending.drain(..at + 1);
          seen.push(line);
          // The client sends two lines, then waits; then one more.
          if seen.len() == 2 && !replied_auth {
            peer.write_all(auth_reply.as_bytes()).expect("write");
            peer.flush().expect("flush");
            replied_auth = true;
          } else if seen.len() == 3 {
            // One write, so the reply and the bytes that follow it
            // reach the client together. Two writes could go in two
            // segments, and then the reader would rightly return after
            // the first and buffer nothing.
            let mut reply: Vec<u8> = Vec::new();
            reply.extend_from_slice(hello_reply.as_bytes());
            reply.extend_from_slice(trailing);
            peer.write_all(&reply).expect("write");
            peer.flush().expect("flush");
          }
        }
        if seen.len() >= 3 {
          // Give the client time to read before the socket closes.
          ic_port::time::microsleep(50_000);
          break;
        }
      }
      seen
    });
    (port, handle)
  }

  fn config_for(port: u16) -> ConnectConfig {
    ConnectConfig {
      server_name: "127.0.0.1".to_string(),
      server_port: port,
      connect_timeout_ms: 2000,
      ..ConnectConfig::default()
    }
  }

  #[test]
  fn a_whole_handshake() {
    let (port, handle) = fake_data_node("Cleartext ok\n", "2 1\n", b"");
    let transporter =
      connect_to_data_node(&config_for(port), 192, 2).expect("handshake");
    assert_eq!(transporter.node_id, 2);
    assert!(transporter.leftover.is_empty());
    drop(transporter);
    let seen = handle.join().expect("join");
    assert_eq!(seen[0], "ndbd TLS disabled");
    assert_eq!(seen[1], "");
    assert_eq!(seen[2], "192 1 2 0");
  }

  #[test]
  fn an_older_data_node_answers_ok() {
    // A server that predates the TLS line reads our two lines as a user
    // name and a password and answers "ok".
    let (port, handle) = fake_data_node("ok\n", "2 1\n", b"");
    let transporter =
      connect_to_data_node(&config_for(port), 192, 2).expect("handshake");
    assert_eq!(transporter.node_id, 2);
    let _ = handle.join();
  }

  #[test]
  fn signal_bytes_arriving_early_are_kept() {
    // The hello reply and the first signal bytes come in one packet.
    let (port, handle) =
      fake_data_node("Cleartext ok\n", "2 1\n", &[0xAA, 0xBB, 0xCC, 0xDD]);
    let transporter =
      connect_to_data_node(&config_for(port), 192, 2).expect("handshake");
    assert_eq!(transporter.leftover, vec![0xAA, 0xBB, 0xCC, 0xDD]);
    let _ = handle.join();
  }

  #[test]
  fn the_wrong_node_is_refused() {
    // We asked for node 2 and node 3 answered.
    let (port, handle) = fake_data_node("Cleartext ok\n", "3 1\n", b"");
    let err =
      connect_to_data_node(&config_for(port), 192, 2).expect_err("wrong node");
    assert_eq!(err.code, err::IC_ERROR_WRONG_NODE_ID);
    let _ = handle.join();
  }

  #[test]
  fn a_node_demanding_tls_is_refused() {
    let (port, handle) = fake_data_node("TLS required\n", "2 1\n", b"");
    let err = connect_to_data_node(&config_for(port), 192, 2)
      .expect_err("tls required");
    assert_eq!(err.code, err::IC_ERROR_NOT_SUPPORTED);
    let _ = handle.join();
  }

  #[test]
  fn a_refusal_is_reported() {
    // A data node that will not accept us answers "BYE".
    let (port, handle) = fake_data_node("Cleartext ok\n", "BYE\n", b"");
    let err =
      connect_to_data_node(&config_for(port), 192, 2).expect_err("refused");
    assert_eq!(err.code, err::IC_AUTHENTICATE_ERROR);
    let _ = handle.join();
  }

  #[test]
  fn replies_are_parsed() {
    assert_eq!(parse_hello_reply("2 1").expect("ok"), (2, 1));
    assert_eq!(parse_hello_reply("144 1").expect("ok"), (144, 1));
    assert!(parse_hello_reply("2").is_err());
    assert!(parse_hello_reply("BYE").is_err());
    assert!(parse_hello_reply("").is_err());
  }

  #[test]
  fn the_hello_line_stays_short() {
    // The limit is 23 characters, which a large node id still fits.
    let hello = format!("{} {} {} 0", 8191, IC_TRANSPORTER_TYPE_TCP, 144);
    assert!(hello.len() <= IC_MAX_HELLO_LEN, "{}", hello);
  }
}
