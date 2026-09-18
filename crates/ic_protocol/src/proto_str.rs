// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The vocabulary of the NDB management protocol
//! (`legacy-c/protocol/ic_proto_str.c`), as RonDB 26.10 spells it.
//!
//! Every string here is checked against
//! `storage/ndb/src/mgmapi/mgmapi.cpp` in the RonDB 26.10 sources, which
//! is the other end of these conversations. The names of commands and
//! arguments are interface, not implementation: they have to match
//! exactly or the management server's parser rejects the request.
//!
//! A request is the command on one line, then `name: value` lines, then
//! an empty line. A reply is `<command> reply` on one line, then
//! `name: value` lines, then an empty line.

/* -------------------------------------------------------------- */
/* Commands                                                        */
/* -------------------------------------------------------------- */

/// Ask the management server for a node id.
/// Verify: `mgmapi.cpp:3301`.
pub const CMD_GET_NODEID: &str = "get nodeid";
/// Reply to [`CMD_GET_NODEID`]. Verify: `mgmapi.cpp:3295`.
pub const REPLY_GET_NODEID: &str = "get nodeid reply";

/// Ask for the cluster configuration in the version 2 format, which is
/// what every RonDB 26.10 management server serves.
/// Verify: `mgmapi.cpp:3136`.
pub const CMD_GET_CONFIG_V2: &str = "get config_v2";
/// The version 1 form, for a management server too old to speak v2.
/// Verify: `mgmapi.cpp:3137`.
pub const CMD_GET_CONFIG: &str = "get config";
/// Reply to either form; the name does not change with the version.
/// Verify: `mgmapi.cpp:3126`.
pub const REPLY_GET_CONFIG: &str = "get config reply";

/// Ask for the version of the management server, which decides whether
/// it can serve the v2 configuration. Verify: `mgmapi.cpp:3704`.
pub const CMD_GET_VERSION: &str = "get version";
/// Reply to [`CMD_GET_VERSION`]. Note that it is not the command plus
/// " reply" like the others. Verify: `mgmapi.cpp:3692`.
pub const REPLY_GET_VERSION: &str = "version";

/// Ask for the state of every node in the cluster.
/// Verify: `mgmapi.cpp:1212`.
pub const CMD_GET_STATUS: &str = "get status";
/// Reply to [`CMD_GET_STATUS`], again not the command plus " reply".
/// Verify: `mgmapi.cpp:1237`.
pub const REPLY_GET_STATUS: &str = "node status";

/// Ask for one parameter of a connection, which is how the dynamically
/// assigned port of a data node is found. Verify: `mgmapi.cpp:3565`.
pub const CMD_GET_CONNECTION_PARAMETER: &str = "get connection parameter";
/// Reply to [`CMD_GET_CONNECTION_PARAMETER`].
/// Verify: `mgmapi.cpp:3560`.
pub const REPLY_GET_CONNECTION_PARAMETER: &str =
  "get connection parameter reply";

/// Turn this management connection into a transporter connection, after
/// which the NDB signal protocol takes over the socket.
/// Verify: `mgmapi.cpp:3600`.
pub const CMD_TRANSPORTER_CONNECT: &str = "transporter connect";

/* -------------------------------------------------------------- */
/* Argument and reply field names                                  */
/* -------------------------------------------------------------- */

/// Our NDB version, as `(major << 16) | (minor << 8) | build`.
pub const ARG_VERSION: &str = "version";
/// What kind of node we are; see [`NODE_TYPE_API`].
pub const ARG_NODETYPE: &str = "nodetype";
/// The node id we want, or 0 to let the management server choose.
pub const ARG_NODEID: &str = "nodeid";
/// User name, which the protocol carries but does not check.
pub const ARG_USER: &str = "user";
/// Password, likewise unchecked.
pub const ARG_PASSWORD: &str = "password";
/// Public key, likewise unchecked.
pub const ARG_PUBLIC_KEY: &str = "public key";
/// Our byte order: [`ENDIAN_BIG`] or [`ENDIAN_LITTLE`].
pub const ARG_ENDIAN: &str = "endian";
/// An optional name for this connection, shown in the cluster log.
pub const ARG_NAME: &str = "name";
/// Whether the management server should log events about us.
pub const ARG_LOG_EVENT: &str = "log_event";
/// The node the configuration is wanted for, sent with `get config_v2`.
pub const ARG_NODE: &str = "node";
/// Ask for the configuration as another node sees it.
pub const ARG_FROM_NODE: &str = "from_node";
/// The two ends of a connection, for a connection parameter.
pub const ARG_NODE1: &str = "node1";
/// See [`ARG_NODE1`].
pub const ARG_NODE2: &str = "node2";
/// Which parameter of the connection is wanted.
pub const ARG_PARAM: &str = "param";
/// The value of the parameter.
pub const ARG_VALUE: &str = "value";

/// Whether the command succeeded; [`RESULT_OK`] or a message.
pub const ARG_RESULT: &str = "result";
/// A numeric error code in a reply that failed.
pub const ARG_ERROR_CODE: &str = "error_code";
/// Length in bytes of the blob that follows a reply.
pub const ARG_CONTENT_LENGTH: &str = "Content-Length";
/// What the blob is; always [`CONTENT_TYPE_CONFIG`].
pub const ARG_CONTENT_TYPE: &str = "Content-Type";
/// How the blob is encoded; always [`CONTENT_ENCODING_BASE64`].
pub const ARG_CONTENT_ENCODING: &str = "Content-Transfer-Encoding";

/* -------------------------------------------------------------- */
/* Values                                                          */
/* -------------------------------------------------------------- */

/// The value of `result` when a command succeeded.
pub const RESULT_OK: &str = "Ok";
/// The `Content-Type` of a configuration blob.
/// Verify: `mgmapi.cpp:3148`.
pub const CONTENT_TYPE_CONFIG: &str = "ndbconfig/octet-stream";
/// The `Content-Transfer-Encoding` of a configuration blob.
/// Verify: `mgmapi.cpp:3156`.
pub const CONTENT_ENCODING_BASE64: &str = "base64";
/// Byte order value for a big endian machine.
pub const ENDIAN_BIG: &str = "big";
/// Byte order value for a little endian machine.
pub const ENDIAN_LITTLE: &str = "little";
/// The user name the C++ API sends, and so do we, because the
/// management server logs it. Verify: `mgmapi.cpp:3286`.
pub const USER_MYSQLD: &str = "mysqld";
/// The public key the C++ API sends. Verify: `mgmapi.cpp:3288`.
pub const PUBLIC_KEY_TEXT: &str = "a public key";

/* -------------------------------------------------------------- */
/* Node types, as the protocol numbers them                        */
/* -------------------------------------------------------------- */
/* Verify: RonDB include/mgmapi/mgmapi_config_parameters.h:455-457, and
the NodeType enum in include/kernel/NodeInfo.hpp:53 which takes the
same values. */

/// A data node.
pub const NODE_TYPE_DB: u32 = 0;
/// An API node, which is what this library is.
pub const NODE_TYPE_API: u32 = 1;
/// A management server.
pub const NODE_TYPE_MGM: u32 = 2;

/// The name of the connection parameter holding a node's port, asked
/// for when the configuration gives port 0 and the real port is
/// assigned dynamically.
/// Verify: RonDB `include/mgmapi/mgmapi_config_parameters.h:365`.
pub const CFG_CONNECTION_SERVER_PORT: u32 = 406;

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn commands_and_replies_pair_up() {
    /* A reply line is the command followed by " reply", except for
    get config_v2, whose reply keeps the v1 name, and the three that
    answer with a different word entirely. */
    assert_eq!(REPLY_GET_NODEID, format!("{} reply", CMD_GET_NODEID));
    assert_eq!(REPLY_GET_CONFIG, format!("{} reply", CMD_GET_CONFIG));
    assert_eq!(
      REPLY_GET_CONNECTION_PARAMETER,
      format!("{} reply", CMD_GET_CONNECTION_PARAMETER)
    );
    assert_ne!(REPLY_GET_CONFIG, format!("{} reply", CMD_GET_CONFIG_V2));
  }

  #[test]
  fn nothing_carries_stray_spaces() {
    let all = [
      CMD_GET_NODEID,
      CMD_GET_CONFIG,
      CMD_GET_CONFIG_V2,
      CMD_GET_VERSION,
      CMD_GET_STATUS,
      CMD_GET_CONNECTION_PARAMETER,
      CMD_TRANSPORTER_CONNECT,
      ARG_VERSION,
      ARG_NODETYPE,
      ARG_NODEID,
      ARG_CONTENT_LENGTH,
      ARG_CONTENT_TYPE,
      ARG_CONTENT_ENCODING,
      RESULT_OK,
      CONTENT_TYPE_CONFIG,
      CONTENT_ENCODING_BASE64,
    ];
    for text in &all {
      assert_eq!(*text, text.trim(), "'{}' has stray spaces", text);
      assert!(!text.is_empty());
      assert!(!text.contains('\n'));
    }
  }

  #[test]
  fn node_types_are_distinct() {
    assert_ne!(NODE_TYPE_DB, NODE_TYPE_API);
    assert_ne!(NODE_TYPE_API, NODE_TYPE_MGM);
    assert_eq!(NODE_TYPE_API, 1);
  }
}
