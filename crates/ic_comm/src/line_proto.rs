// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The line-oriented management protocol
//! (`legacy-c/comm/ic_protocol_support.c`).
//!
//! Every exchange with `ndb_mgmd` is lines ending in a newline, with an
//! empty line marking the end of a request or a reply and handing the
//! turn to the other side. A request looks like this, with the blank
//! line at the end:
//!
//! ```text
//!   get nodeid
//!   version: 1706240
//!   nodetype: 1
//!   nodeid: 0
//!
//! ```
//!
//! and the reply like this:
//!
//! ```text
//!   get nodeid reply
//!   nodeid: 68
//!   result: Ok
//!
//! ```
//!
//! Rust notes for C readers: the C kept a read buffer inside the
//! connection object so that `ic_rec_with_cr` could return one line at a
//! time. Here the buffer is a [`LineReader`], held by whichever thread
//! reads that connection. Keeping it separate is what lets one thread
//! read while another writes without either needing a lock.

use ic_port::consts::IC_CARRIAGE_RETURN;
use ic_port::debug::IC_CONFIG_PROTO_LEVEL;
use ic_port::err;
use ic_port::IcError;

use crate::connection::Connection;

/// Largest line the protocol may send, as the management server's own
/// parser allows.
pub const IC_MAX_LINE_LEN: usize = 512;

/// Size the read buffer grows to before a line is refused.
const IC_MAX_BUFFERED: usize = 64 * 1024;

/// Reads whole lines from a connection, keeping whatever arrived past
/// the end of the current line for the next call.
pub struct LineReader {
  buf: Vec<u8>,
  /// Where the unread part of `buf` starts.
  start: usize,
}

impl LineReader {
  /// A reader with an empty buffer.
  pub fn new() -> LineReader {
    LineReader {
      buf: Vec::with_capacity(IC_MAX_LINE_LEN),
      start: 0,
    }
  }

  /// Forget anything buffered, for a connection being reused.
  pub fn reset(&mut self) {
    self.buf.clear();
    self.start = 0;
  }

  /// True if a whole line is already buffered, so the next
  /// [`read_line`](Self::read_line) will not touch the socket.
  pub fn has_line(&self) -> bool {
    self.find_newline().is_some()
  }

  fn find_newline(&self) -> Option<usize> {
    let mut i = self.start;
    while i < self.buf.len() {
      if self.buf[i] == IC_CARRIAGE_RETURN {
        return Some(i);
      }
      i += 1;
    }
    None
  }

  fn take_line(&mut self, newline_at: usize) -> String {
    let line = String::from_utf8_lossy(&self.buf[self.start..newline_at])
      .trim_end_matches('\r')
      .to_string();
    self.start = newline_at + 1;
    if self.start >= self.buf.len() {
      self.buf.clear();
      self.start = 0;
    }
    line
  }

  /// Read one line, without its newline (`ic_rec_with_cr`).
  ///
  /// An empty line is returned as an empty string, which is how the
  /// protocol marks the end of a request or reply.
  pub fn read_line(&mut self, conn: &Connection) -> Result<String, IcError> {
    loop {
      if let Some(at) = self.find_newline() {
        let line = self.take_line(at);
        ic_port::debug_print!(IC_CONFIG_PROTO_LEVEL, "Received: {}", line);
        return Ok(line);
      }
      if self.buf.len() - self.start > IC_MAX_BUFFERED {
        return Err(IcError::new(err::IC_ERROR_LINE_TOO_LONG));
      }
      /* Drop the consumed front of the buffer before reading more. */
      if self.start > 0 {
        self.buf.drain(..self.start);
        self.start = 0;
      }
      let mut chunk = [0u8; 1024];
      let size = conn.read(&mut chunk)?;
      if size == 0 {
        return Err(IcError::new(err::IC_END_OF_FILE));
      }
      self.buf.extend_from_slice(&chunk[..size]);
    }
  }

  /// Read a line and require it to be `expected`
  /// (`ic_rec_simple_str`).
  pub fn expect_line(
    &mut self,
    conn: &Connection,
    expected: &str,
  ) -> Result<(), IcError> {
    let line = self.read_line(conn)?;
    if line != expected {
      ic_port::debug_print!(
        IC_CONFIG_PROTO_LEVEL,
        "Expected '{}' but got '{}'",
        expected,
        line
      );
      return Err(IcError::new(err::IC_PROTOCOL_ERROR));
    }
    Ok(())
  }

  /// Read a line and require it to be empty, which hands the turn back
  /// (`ic_rec_empty_line`).
  pub fn expect_empty_line(
    &mut self,
    conn: &Connection,
  ) -> Result<(), IcError> {
    self.expect_line(conn, "")
  }

  /// Read a line of the form `name: value` and return the value; fails
  /// if the name is not the expected one (`ic_rec_string`).
  pub fn read_value(
    &mut self,
    conn: &Connection,
    name: &str,
  ) -> Result<String, IcError> {
    let line = self.read_line(conn)?;
    match value_of(&line, name) {
      Some(value) => Ok(value),
      None => {
        ic_port::debug_print!(
          IC_CONFIG_PROTO_LEVEL,
          "Expected '{}: ...' but got '{}'",
          name,
          line
        );
        Err(IcError::new(err::IC_PROTOCOL_ERROR))
      }
    }
  }

  /// As [`read_value`](Self::read_value), but parse the value as a
  /// number (`ic_rec_number`, `ic_rec_long_number`).
  pub fn read_number(
    &mut self,
    conn: &Connection,
    name: &str,
  ) -> Result<u64, IcError> {
    let text = self.read_value(conn, name)?;
    match text.trim().parse::<u64>() {
      Ok(value) => Ok(value),
      Err(_) => Err(IcError::new(err::IC_PROTOCOL_ERROR)),
    }
  }

  /// Read every line up to the empty line that ends the reply, and
  /// return them.
  pub fn read_until_empty_line(
    &mut self,
    conn: &Connection,
  ) -> Result<Vec<String>, IcError> {
    let mut lines: Vec<String> = Vec::new();
    loop {
      let line = self.read_line(conn)?;
      if line.is_empty() {
        return Ok(lines);
      }
      lines.push(line);
      if lines.len() > 4096 {
        return Err(IcError::new(err::IC_PROTOCOL_ERROR));
      }
    }
  }

  /// Take whatever has been read but not yet returned as a line.
  ///
  /// After a handshake the same socket carries something else, and a
  /// read may well have pulled in the first bytes of it along with the
  /// last line. Those bytes belong to whoever takes over the socket;
  /// dropping them loses the start of the first signal.
  pub fn take_buffered(&mut self) -> Vec<u8> {
    let rest = self.buf[self.start..].to_vec();
    self.buf.clear();
    self.start = 0;
    rest
  }

  /// Read the exact number of bytes that follow a reply, used for the
  /// base64 configuration blob that comes after its header lines.
  pub fn read_exact(
    &mut self,
    conn: &Connection,
    len: usize,
  ) -> Result<Vec<u8>, IcError> {
    let mut out: Vec<u8> = Vec::with_capacity(len);
    /* Whatever is already buffered comes first. */
    let buffered = self.buf.len() - self.start;
    let take = std::cmp::min(buffered, len);
    out.extend_from_slice(&self.buf[self.start..self.start + take]);
    self.start += take;
    if self.start >= self.buf.len() {
      self.buf.clear();
      self.start = 0;
    }
    while out.len() < len {
      let mut chunk = vec![0u8; std::cmp::min(len - out.len(), 16 * 1024)];
      let size = conn.read(&mut chunk)?;
      if size == 0 {
        return Err(IcError::new(err::IC_END_OF_FILE));
      }
      out.extend_from_slice(&chunk[..size]);
    }
    Ok(out)
  }
}

impl Default for LineReader {
  fn default() -> LineReader {
    LineReader::new()
  }
}

/// The value in a `name: value` line, if the name matches, ignoring
/// case and surrounding spaces.
pub fn value_of(line: &str, name: &str) -> Option<String> {
  let colon = line.find(':')?;
  if !line[..colon].trim().eq_ignore_ascii_case(name) {
    return None;
  }
  Some(line[colon + 1..].trim().to_string())
}

/// Send one line, with the newline the protocol expects
/// (`ic_send_with_cr`).
pub fn send_line(conn: &Connection, line: &str) -> Result<(), IcError> {
  if line.len() > IC_MAX_LINE_LEN {
    return Err(IcError::new(err::IC_ERROR_LINE_TOO_LONG));
  }
  ic_port::debug_print!(IC_CONFIG_PROTO_LEVEL, "Sending: {}", line);
  let mut out: Vec<u8> = Vec::with_capacity(line.len() + 1);
  out.extend_from_slice(line.as_bytes());
  out.push(IC_CARRIAGE_RETURN);
  conn.write(&out)
}

/// Send the empty line that ends a request (`ic_send_empty_line`).
pub fn send_empty_line(conn: &Connection) -> Result<(), IcError> {
  send_line(conn, "")
}

/// Send a `name: value` line with a number
/// (`ic_send_with_cr_with_number`).
pub fn send_number(
  conn: &Connection,
  name: &str,
  value: u64,
) -> Result<(), IcError> {
  send_line(conn, &format!("{}: {}", name, value))
}

/// Send a `name: value` line with text.
pub fn send_value(
  conn: &Connection,
  name: &str,
  value: &str,
) -> Result<(), IcError> {
  send_line(conn, &format!("{}: {}", name, value))
}

/// Send a `name: "value"` line, the quoted form the management protocol
/// uses for strings that may contain spaces.
pub fn send_quoted_value(
  conn: &Connection,
  name: &str,
  value: &str,
) -> Result<(), IcError> {
  send_line(conn, &format!("{}:\"{}\"", name, value))
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::connection::ConnectConfig;
  use std::io::Read;
  use std::io::Write;
  use std::net::TcpListener;

  /// A server that replies with a fixed script once it has read a
  /// request ending in an empty line.
  type ServerHandle = std::thread::JoinHandle<Vec<String>>;

  fn scripted_server(reply: &'static str) -> (u16, ServerHandle) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("addr").port();
    let handle = std::thread::spawn(move || {
      let (mut peer, _) = listener.accept().expect("accept");
      let mut request = Vec::new();
      let mut buf = [0u8; 256];
      loop {
        let size = peer.read(&mut buf).expect("read");
        if size == 0 {
          break;
        }
        request.extend_from_slice(&buf[..size]);
        /* The request ends with an empty line, so two newlines. */
        if request.len() >= 2 && request.ends_with(b"\n\n") {
          break;
        }
      }
      peer.write_all(reply.as_bytes()).expect("write");
      peer.flush().expect("flush");
      ic_port::time::microsleep(50_000);
      let text = String::from_utf8_lossy(&request).to_string();
      let mut lines: Vec<String> = Vec::new();
      for line in text.split('\n') {
        lines.push(line.to_string());
      }
      lines
    });
    (port, handle)
  }

  fn connect(port: u16) -> Connection {
    let config = ConnectConfig {
      server_name: "127.0.0.1".to_string(),
      server_port: port,
      connect_timeout_ms: 2000,
      ..ConnectConfig::default()
    };
    Connection::connect(&config).expect("connect")
  }

  #[test]
  fn a_request_and_its_reply() {
    let reply = "get nodeid reply\nnodeid: 68\nresult: Ok\n\n";
    let (port, handle) = scripted_server(reply);
    let conn = connect(port);
    send_line(&conn, "get nodeid").expect("send");
    send_number(&conn, "version", 1706240).expect("send");
    send_value(&conn, "nodetype", "1").expect("send");
    send_quoted_value(&conn, "user", "mysqld").expect("send");
    send_empty_line(&conn).expect("send");
    let mut reader = LineReader::new();
    reader
      .expect_line(&conn, "get nodeid reply")
      .expect("reply");
    let nodeid = reader.read_number(&conn, "nodeid").expect("nodeid");
    assert_eq!(nodeid, 68);
    let result = reader.read_value(&conn, "result").expect("result");
    assert_eq!(result, "Ok");
    reader.expect_empty_line(&conn).expect("empty");
    let sent = handle.join().expect("join");
    assert_eq!(sent[0], "get nodeid");
    assert_eq!(sent[1], "version: 1706240");
    assert_eq!(sent[2], "nodetype: 1");
    assert_eq!(sent[3], "user:\"mysqld\"");
    assert_eq!(sent[4], "");
  }

  #[test]
  fn lines_split_across_reads() {
    /* The reply arrives in pieces; the reader must join them. */
    let reply = "one\ntwo\nthree\n\n";
    let (port, handle) = scripted_server(reply);
    let conn = connect(port);
    send_empty_line(&conn).expect("send");
    send_empty_line(&conn).expect("send");
    let mut reader = LineReader::new();
    let lines = reader.read_until_empty_line(&conn).expect("lines");
    assert_eq!(lines, vec!["one", "two", "three"]);
    handle.join().expect("join");
  }

  #[test]
  fn a_wrong_line_is_a_protocol_error() {
    let (port, handle) = scripted_server("unexpected\n\n");
    let conn = connect(port);
    send_empty_line(&conn).expect("send");
    send_empty_line(&conn).expect("send");
    let mut reader = LineReader::new();
    let result = reader.expect_line(&conn, "expected");
    assert_eq!(result, Err(IcError::new(err::IC_PROTOCOL_ERROR)));
    handle.join().expect("join");
  }

  #[test]
  fn bytes_after_the_header_are_read_exactly() {
    // This is the shape of a get config reply: header lines, an empty
    // line, then Content-Length bytes of base64.
    let reply = "get config reply\nresult: Ok\n\
                 Content-Length: 10\n\nABCDEFGHIJrest";
    let (port, handle) = scripted_server(reply);
    let conn = connect(port);
    send_empty_line(&conn).expect("send");
    send_empty_line(&conn).expect("send");
    let mut reader = LineReader::new();
    reader.expect_line(&conn, "get config reply").expect("line");
    assert_eq!(reader.read_value(&conn, "result").expect("v"), "Ok");
    let len = reader.read_number(&conn, "content-length").expect("len");
    assert_eq!(len, 10);
    reader.expect_empty_line(&conn).expect("empty");
    let blob = reader.read_exact(&conn, len as usize).expect("blob");
    assert_eq!(blob, b"ABCDEFGHIJ");
    handle.join().expect("join");
  }

  #[test]
  fn end_of_file_is_reported() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("addr").port();
    let handle = std::thread::spawn(move || {
      let (peer, _) = listener.accept().expect("accept");
      drop(peer);
    });
    let conn = connect(port);
    let mut reader = LineReader::new();
    let result = reader.read_line(&conn);
    assert_eq!(result, Err(IcError::new(err::IC_END_OF_FILE)));
    handle.join().expect("join");
  }

  #[test]
  fn bytes_past_the_last_line_are_handed_on() {
    // A handshake reply and the first bytes of what follows arrive in
    // one read; the reader must not swallow the tail.
    let (port, handle) = scripted_server("ok\n\x01\x02\x03");
    let conn = connect(port);
    send_empty_line(&conn).expect("send");
    send_empty_line(&conn).expect("send");
    let mut reader = LineReader::new();
    reader.expect_line(&conn, "ok").expect("line");
    let rest = reader.take_buffered();
    assert_eq!(rest, vec![1u8, 2, 3]);
    assert!(reader.take_buffered().is_empty());
    handle.join().expect("join");
  }

  #[test]
  fn value_parsing() {
    assert_eq!(value_of("nodeid: 68", "nodeid"), Some("68".to_string()));
    assert_eq!(value_of("NodeId:68", "nodeid"), Some("68".to_string()));
    assert_eq!(
      value_of("Content-Length: 4096", "content-length"),
      Some("4096".to_string())
    );
    assert_eq!(value_of("result: Ok", "nodeid"), None);
    assert_eq!(value_of("no colon here", "nodeid"), None);
    let quoted = value_of("user:\"mysqld\"", "user");
    assert_eq!(quoted, Some("\"mysqld\"".to_string()));
  }

  #[test]
  fn a_line_that_is_too_long_is_refused() {
    let (port, handle) = scripted_server("ok\n\n");
    let conn = connect(port);
    let long = "x".repeat(IC_MAX_LINE_LEN + 1);
    let result = send_line(&conn, &long);
    assert_eq!(result, Err(IcError::new(err::IC_ERROR_LINE_TOO_LONG)));
    send_empty_line(&conn).expect("send");
    send_empty_line(&conn).expect("send");
    handle.join().expect("join");
  }
}
