// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Parsing an NDB connectstring
//! (`legacy-c/util/ic_parse_connectstring.c`).
//!
//! A connectstring names the management servers to ask for the cluster
//! configuration. The forms accepted are those a RonDB operator already
//! uses with `ndb_mgm` and `mysqld`:
//!
//! ```text
//! localhost
//! localhost:1186
//! host1:1186,host2:1186
//! nodeid=68,host1:1186,host2:1186
//! bind-address=10.0.0.5,host1:1186
//! ```
//!
//! A host with no port gets the default 1186. Hosts may be names, IPv4
//! addresses, or IPv6 addresses in brackets (`[::1]:1186`).

use ic_port::consts::IC_DEF_CLUSTER_SERVER_PORT;
use ic_port::err;
use ic_port::IcError;

/// Largest number of management servers accepted in one connectstring.
pub const IC_MAX_CLUSTER_SERVERS: usize = 4;

/// One management server to try.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MgmServer {
  /// Host name or address as written, without brackets.
  pub host: String,
  /// Port, 1186 when the connectstring did not say.
  pub port: u16,
}

/// A parsed connectstring.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConnectString {
  /// The management servers, in the order given.
  pub servers: Vec<MgmServer>,
  /// The node id requested with `nodeid=`, if any. Without it the
  /// management server allocates one.
  pub node_id: Option<u32>,
  /// The local address to bind sockets to, from `bind-address=`.
  pub bind_address: Option<String>,
}

fn is_host_char(c: u8) -> bool {
  c.is_ascii_alphanumeric() || c == b'_' || c == b'-' || c == b'.'
}

fn parse_host_and_port(text: &str) -> Result<MgmServer, IcError> {
  let bad = IcError::new(err::IC_ERROR_PARSE_CONNECTSTRING);
  if text.is_empty() {
    return Err(bad);
  }
  // An IPv6 address is written in brackets so its colons do not read as
  // the port separator.
  if let Some(rest) = text.strip_prefix('[') {
    let end = match rest.find(']') {
      Some(pos) => pos,
      None => return Err(bad),
    };
    let host = &rest[..end];
    if host.is_empty() {
      return Err(bad);
    }
    let after = &rest[end + 1..];
    if after.is_empty() {
      return Ok(MgmServer {
        host: host.to_string(),
        port: IC_DEF_CLUSTER_SERVER_PORT,
      });
    }
    let port_text = match after.strip_prefix(':') {
      Some(p) => p,
      None => return Err(bad),
    };
    return Ok(MgmServer {
      host: host.to_string(),
      port: parse_port(port_text)?,
    });
  }
  let (host, port) = match text.find(':') {
    Some(pos) => (&text[..pos], parse_port(&text[pos + 1..])?),
    None => (text, IC_DEF_CLUSTER_SERVER_PORT),
  };
  if host.is_empty() {
    return Err(bad);
  }
  for c in host.as_bytes() {
    if !is_host_char(*c) {
      return Err(bad);
    }
  }
  Ok(MgmServer {
    host: host.to_string(),
    port,
  })
}

fn parse_port(text: &str) -> Result<u16, IcError> {
  let bad = IcError::new(err::IC_ERROR_PARSE_CONNECTSTRING);
  if text.is_empty() {
    return Err(bad);
  }
  for c in text.as_bytes() {
    if !c.is_ascii_digit() {
      return Err(bad);
    }
  }
  match text.parse::<u16>() {
    Ok(port) => {
      if port == 0 {
        return Err(IcError::new(err::IC_ERROR_ILLEGAL_SERVER_PORT));
      }
      Ok(port)
    }
    Err(_) => Err(IcError::new(err::IC_ERROR_ILLEGAL_SERVER_PORT)),
  }
}

/// Parse a connectstring. An empty string gives one server, `localhost`
/// on the default port, which is what the C did when no connectstring
/// was supplied.
pub fn parse(connect_string: &str) -> Result<ConnectString, IcError> {
  let mut result = ConnectString::default();
  let trimmed = connect_string.trim();
  if trimmed.is_empty() {
    result.servers.push(MgmServer {
      host: "localhost".to_string(),
      port: IC_DEF_CLUSTER_SERVER_PORT,
    });
    return Ok(result);
  }
  for part in trimmed.split(',') {
    let item = part.trim();
    if item.is_empty() {
      return Err(IcError::new(err::IC_ERROR_PARSE_CONNECTSTRING));
    }
    if let Some(value) = strip_key(item, "nodeid=") {
      let node_id = match value.parse::<u32>() {
        Ok(v) => v,
        Err(_) => return Err(IcError::new(err::IC_ERROR_PARSE_CONNECTSTRING)),
      };
      // Checked against what the protocol can carry, not against a
      // cluster's current limit: that limit rises between releases and
      // the configuration, which we do not have yet, is what really
      // decides.
      if node_id == 0 || node_id > ic_port::consts::IC_MAX_NODE_ID_WIRE {
        return Err(IcError::new(err::IC_ERROR_WRONG_NODE_ID));
      }
      result.node_id = Some(node_id);
      continue;
    }
    if let Some(value) = strip_key(item, "bind-address=") {
      if value.is_empty() {
        return Err(IcError::new(err::IC_ERROR_PARSE_CONNECTSTRING));
      }
      result.bind_address = Some(value.to_string());
      continue;
    }
    if result.servers.len() == IC_MAX_CLUSTER_SERVERS {
      return Err(IcError::new(err::IC_ERROR_TOO_MANY_CS_HOSTS));
    }
    result.servers.push(parse_host_and_port(item)?);
  }
  if result.servers.is_empty() {
    return Err(IcError::new(err::IC_ERROR_NO_SERVER_NAME));
  }
  Ok(result)
}

fn strip_key<'a>(item: &'a str, key: &str) -> Option<&'a str> {
  if item.len() < key.len() {
    return None;
  }
  if !item[..key.len()].eq_ignore_ascii_case(key) {
    return None;
  }
  Some(item[key.len()..].trim())
}

#[cfg(test)]
mod tests {
  use super::*;

  fn server(host: &str, port: u16) -> MgmServer {
    MgmServer {
      host: host.to_string(),
      port,
    }
  }

  #[test]
  fn plain_forms() {
    let c = parse("localhost").expect("parse");
    assert_eq!(c.servers, vec![server("localhost", 1186)]);
    assert_eq!(c.node_id, None);
    let c = parse("localhost:1186").expect("parse");
    assert_eq!(c.servers, vec![server("localhost", 1186)]);
    let c = parse("10.0.0.1:2200").expect("parse");
    assert_eq!(c.servers, vec![server("10.0.0.1", 2200)]);
    let c = parse("").expect("parse");
    assert_eq!(c.servers, vec![server("localhost", 1186)]);
  }

  #[test]
  fn several_servers() {
    let c = parse("host1:1186,host2,host3:2000").expect("parse");
    assert_eq!(
      c.servers,
      vec![
        server("host1", 1186),
        server("host2", 1186),
        server("host3", 2000)
      ]
    );
  }

  #[test]
  fn node_id_and_bind_address() {
    let c = parse("nodeid=68,host1:1186").expect("parse");
    assert_eq!(c.node_id, Some(68));
    assert_eq!(c.servers, vec![server("host1", 1186)]);
    let c = parse("bind-address=10.0.0.5,host1").expect("parse");
    assert_eq!(c.bind_address, Some("10.0.0.5".to_string()));
    let c = parse("NodeId=7,host1").expect("parse");
    assert_eq!(c.node_id, Some(7));
    /* RonDB API node ids go well past 255. */
    let c = parse("nodeid=1600,host1").expect("parse");
    assert_eq!(c.node_id, Some(1600));
  }

  #[test]
  fn ipv6_in_brackets() {
    let c = parse("[::1]:1186").expect("parse");
    assert_eq!(c.servers, vec![server("::1", 1186)]);
    let c = parse("[fe80::1]").expect("parse");
    assert_eq!(c.servers, vec![server("fe80::1", 1186)]);
  }

  #[test]
  fn errors() {
    assert!(parse("host1,,host2").is_err());
    assert!(parse(":1186").is_err());
    assert!(parse("host:").is_err());
    assert!(parse("host:abc").is_err());
    assert!(parse("host:0").is_err());
    assert!(parse("host:99999").is_err());
    assert!(parse("ho st").is_err());
    assert!(parse("nodeid=0,host").is_err());
    assert!(parse("nodeid=99999,host").is_err());
    // The limit rises between releases, so a plausible large id is
    // accepted here and settled against the configuration later.
    assert!(parse("nodeid=8191,host").is_ok());
    assert!(parse("nodeid=x,host").is_err());
    assert!(parse("[::1").is_err());
    assert!(parse("h1,h2,h3,h4,h5").is_err());
  }

  /* The C unit test, test type 7. */
  #[test]
  fn the_c_test_cases() {
    let c = parse("192.168.1.1:1186,192.168.1.2:1187").expect("parse");
    assert_eq!(c.servers.len(), 2);
    assert_eq!(c.servers[0].host, "192.168.1.1");
    assert_eq!(c.servers[0].port, 1186);
    assert_eq!(c.servers[1].host, "192.168.1.2");
    assert_eq!(c.servers[1].port, 1187);
    let c = parse("myhost").expect("parse");
    assert_eq!(c.servers[0].host, "myhost");
    assert_eq!(c.servers[0].port, 1186);
  }
}
