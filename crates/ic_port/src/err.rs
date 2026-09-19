// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Error codes and the error value. Translates `legacy-c/include/ic_err.h`
//! and `legacy-c/util/ic_err.c`.
//!
//! Codes 7000..=[`IC_LAST_ERROR`] are iClaustron errors; smaller positive
//! codes are operating system `errno` values; codes outside both ranges
//! are NDB error codes reported by the data nodes. The numbers are part
//! of the interface and never change; new codes are appended.
//!
//! To add an error: bump [`IC_LAST_ERROR`], add the constant, add its
//! text in [`message`].

use std::fmt;

/// First iClaustron error code.
pub const IC_FIRST_ERROR: i32 = 7000;
/// Last iClaustron error code.
pub const IC_LAST_ERROR: i32 = 7138;

/// Line was too long.
pub const IC_ERROR_LINE_TOO_LONG: i32 = 7000;
/// Missing ] after initial [.
pub const IC_ERROR_CONFIG_BRACKET: i32 = 7001;
/// Found incorrect group id.
pub const IC_ERROR_CONFIG_INCORRECT_GROUP_ID: i32 = 7002;
/// Improper key-value pair.
pub const IC_ERROR_CONFIG_IMPROPER_KEY_VALUE: i32 = 7003;
/// Section name doesn't exist in this type of configuration file.
pub const IC_ERROR_CONFIG_NO_SUCH_SECTION: i32 = 7004;
/// Memory allocation failure.
pub const IC_ERROR_MEM_ALLOC: i32 = 7005;
/// Tried to define key value before first section defined.
pub const IC_ERROR_NO_SECTION_DEFINED_YET: i32 = 7006;
/// No such configuration key exists.
pub const IC_ERROR_NO_SUCH_CONFIG_KEY: i32 = 7007;
/// Trying to assign default value to a mandatory config entry.
pub const IC_ERROR_DEFAULT_VALUE_FOR_MANDATORY: i32 = 7008;
/// Assigning correct config entry in wrong section.
pub const IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION: i32 = 7009;
/// No nodes found in the configuration file.
pub const IC_ERROR_NO_NODES_FOUND: i32 = 7010;
/// Number expected in config file, true, false and endings with k, m, g
/// also allowed.
pub const IC_ERROR_WRONG_CONFIG_NUMBER: i32 = 7011;
/// Boolean value expected, got number larger than 1.
pub const IC_ERROR_NO_BOOLEAN_VALUE: i32 = 7012;
/// Configuration value is out of bounds, check data type and min, max values.
pub const IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS: i32 = 7013;
/// Server name must be provided in all connections.
pub const IC_ERROR_NO_SERVER_NAME: i32 = 7014;
/// Server port must be provided in all connections.
pub const IC_ERROR_NO_SERVER_PORT: i32 = 7015;
/// Provided client/server name/port not found by getaddrinfo.
pub const IC_ERROR_GETADDRINFO: i32 = 7016;
/// Provided server port isn't a legal port number.
pub const IC_ERROR_ILLEGAL_SERVER_PORT: i32 = 7017;
/// Trying to use IPv4 and IPv6 simultaneously on server/client part not
/// supported.
pub const IC_ERROR_DIFFERENT_IP_VERSIONS: i32 = 7018;
/// Provided client port isn't a legal port number.
pub const IC_ERROR_ILLEGAL_CLIENT_PORT: i32 = 7019;
/// Internal data structure error.
pub const IC_ERROR_INCONSISTENT_DATA: i32 = 7020;
/// Node failure occurred.
pub const IC_ERROR_NODE_DOWN: i32 = 7021;
/// No such cluster.
pub const IC_ERROR_NO_SUCH_CLUSTER: i32 = 7022;
/// No such node exists in this cluster.
pub const IC_ERROR_NO_SUCH_NODE: i32 = 7023;
/// Message received with wrong checksum.
pub const IC_ERROR_MESSAGE_CHECKSUM: i32 = 7024;
/// Timeout when waiting for connection to accept.
pub const IC_ERROR_ACCEPT_TIMEOUT: i32 = 7025;
/// Poll set is full, need to use another poll set.
pub const IC_ERROR_POLL_SET_FULL: i32 = 7026;
/// The file descriptor wasn't found in this poll set.
pub const IC_ERROR_NOT_FOUND_IN_POLL_SET: i32 = 7027;
/// Can't use the same node id twice in a cluster.
pub const IC_ERROR_NODE_ALREADY_DEFINED: i32 = 7028;
/// The process is not alive.
pub const IC_ERROR_PROCESS_NOT_ALIVE: i32 = 7029;
/// The Cluster Server failed to lock the configuration, other Cluster
/// Server already running.
pub const IC_ERROR_COULD_NOT_LOCK_CONFIGURATION: i32 = 7030;
/// The check process script failed.
pub const IC_ERROR_CHECK_PROCESS_SCRIPT: i32 = 7031;
/// Bootstrap on Cluster Server already performed.
pub const IC_ERROR_BOOTSTRAP_ALREADY_PERFORMED: i32 = 7032;
/// Cluster ids must be unique in configuration.
pub const IC_ERROR_CONFLICTING_CLUSTER_IDS: i32 = 7033;
/// Failed to open file.
pub const IC_ERROR_FAILED_TO_OPEN_FILE: i32 = 7034;
/// Starting Cluster Server for the first time without bootstrap flag set.
pub const IC_ERROR_BOOTSTRAP_NEEDED: i32 = 7035;
/// Connect thread stopped.
pub const IC_ERROR_CONNECT_THREAD_STOPPED: i32 = 7036;
/// Threadpool is full.
pub const IC_ERROR_THREADPOOL_FULL: i32 = 7037;
/// Start thread failed.
pub const IC_ERROR_START_THREAD_FAILED: i32 = 7038;
/// Timeout in receiving line in NDB Management Protocol.
pub const IC_ERROR_RECEIVE_TIMEOUT: i32 = 7039;
/// Timeout when waiting for connect on client side.
pub const IC_ERROR_CONNECT_TIMEOUT: i32 = 7040;
/// Stop ordered in send thread.
pub const IC_ERROR_STOP_ORDERED: i32 = 7041;
/// Accept error on socket.
pub const IC_ACCEPT_ERROR: i32 = 7042;
/// Unexpected end of file.
pub const IC_END_OF_FILE: i32 = 7043;
/// Protocol error in NDB MGM Protocol.
pub const IC_PROTOCOL_ERROR: i32 = 7044;
/// Authentication error at connection setup.
pub const IC_AUTHENTICATE_ERROR: i32 = 7045;
/// SSL error on socket.
pub const IC_SSL_ERROR: i32 = 7046;
/// An attempt to start this process is already ongoing.
pub const IC_ERROR_PC_START_ALREADY_ONGOING: i32 = 7047;
/// This process is already running.
pub const IC_ERROR_PC_PROCESS_ALREADY_RUNNING: i32 = 7048;
/// Failed to stop/kill process, process stuck in start phase.
pub const IC_ERROR_PROCESS_STUCK_IN_START_PHASE: i32 = 7049;
/// Failed to stop/kill process.
pub const IC_ERROR_FAILED_TO_STOP_PROCESS: i32 = 7050;
/// Set/Get connection parameter only supports set/get Server Port Parameter.
pub const IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_PARAM: i32 = 7051;
/// Set/Get connection parameter only supports client node as server side.
pub const IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_NODES: i32 = 7052;
/// Index 0 isn't allowed in dynamic translations.
pub const IC_ERROR_INDEX_ZERO_NOT_ALLOWED: i32 = 7053;
/// Index out of bound in dynamic pointer array.
pub const IC_ERROR_PTR_ARRAY_INDEX_OUT_OF_BOUND: i32 = 7054;
/// Trying to read non-existent entry in dynamic pointer array.
pub const IC_ERROR_PTR_ARRAY_INDEX_ERROR: i32 = 7055;
/// Error when parsing connect string.
pub const IC_ERROR_PARSE_CONNECTSTRING: i32 = 7056;
/// Too many hosts in connectstring.
pub const IC_ERROR_TOO_MANY_CS_HOSTS: i32 = 7057;
/// Failed to daemonize process.
pub const IC_ERROR_FAILED_TO_DAEMONIZE: i32 = 7058;
/// Application has been stopped.
pub const IC_ERROR_APPLICATION_STOPPED: i32 = 7059;
/// Trying to get configuration from network using cluster server.
pub const IC_ERROR_GET_CONFIG_BY_CLUSTER_SERVER: i32 = 7060;
/// Need to have valid buffers when creating APID operation object.
pub const IC_ERROR_BUFFER_MISSING_CREATE_APID_OP: i32 = 7061;
/// Defining more fields than table contains isn't valid.
pub const IC_ERROR_TOO_MANY_FIELDS: i32 = 7062;
/// Trying to define the same field twice.
pub const IC_ERROR_DUPLICATE_FIELD_IDS: i32 = 7063;
/// Trying to define the same field twice.
pub const IC_ERROR_FIELD_ALREADY_DEFINED: i32 = 7064;
/// Trying to define a characteristic on a field not defined.
pub const IC_ERROR_FIELD_NOT_DEFINED: i32 = 7065;
/// Trying to define character set on a field not using it.
pub const IC_ERROR_NOT_A_CHARSET_FIELD: i32 = 7066;
/// Trying to define decimal characteristics on a field not decimal.
pub const IC_ERROR_NOT_A_DECIMAL_FIELD: i32 = 7067;
/// Trying to define signed or not on a field not a number field.
pub const IC_ERROR_NOT_A_SIGNABLE_FIELD: i32 = 7068;
/// Trying to define the same index twice.
pub const IC_ERROR_INDEX_ALREADY_DEFINED: i32 = 7069;
/// Trying to operate on a non-existent index.
pub const IC_ERROR_INDEX_NOT_DEFINED: i32 = 7070;
/// Currently only supported with one metadata table/tablespace per transaction.
pub const IC_ERROR_MULTIPLE_METADATA_OPS: i32 = 7071;
/// Trying to create a table without fields.
pub const IC_ERROR_CREATE_TABLE_NO_FIELDS: i32 = 7072;
/// Trying to commit metadata transaction without any operations.
pub const IC_ERROR_MD_COMMIT_NO_OPERATION: i32 = 7073;
/// Trying to perform operation not allowed with drop/rename table.
pub const IC_ERROR_ILLEGAL_MD_OPERATION_DROP_RENAME: i32 = 7074;
/// Trying to perform operation not allowed with create table.
pub const IC_ERROR_ILLEGAL_MD_OPERATION_CREATE: i32 = 7075;
/// Tables defined in NDB must have a primary key.
pub const IC_ERROR_TABLE_MUST_HAVE_PRIMARY_KEY: i32 = 7076;
/// Record size exceeds the maximum.
pub const IC_ERROR_RECORD_SIZE_TOO_BIG: i32 = 7077;
/// Trying set dynamic port number when fixed port number is used.
pub const IC_ERROR_SET_CONNECTION_NO_DYNAMIC: i32 = 7078;
/// No such error.
pub const IC_ERROR_NO_SUCH_ERROR: i32 = 7079;
/// Cluster Servers have different views on master index.
pub const IC_ERROR_MASTER_INDEX_VIEW_DIFFERS: i32 = 7080;
/// Wrong node id.
pub const IC_ERROR_WRONG_NODE_ID: i32 = 7081;
/// Cluster Server changed view on master node order.
pub const IC_ERROR_CHANGE_VIEW: i32 = 7082;
/// Node id needs to be provided.
pub const IC_ERROR_NO_NODEID: i32 = 7083;
/// Node id provided didn't exist in config.
pub const IC_ERROR_NO_SUCH_CLUSTER_SERVER_NODEID: i32 = 7084;
/// Failed to open common grid configuration file grid_common.ini.
pub const IC_ERROR_FAILED_TO_OPEN_COMMON_GRID_CONFIG: i32 = 7085;
/// Failed to open configuration file of a cluster.
pub const IC_ERROR_FAILED_TO_OPEN_CLUSTER_CONFIG: i32 = 7086;
/// Failed to open configuration file of clusters in the grid, config.ini.
pub const IC_ERROR_FAILED_TO_OPEN_CLUSTER_LIST: i32 = 7087;
/// Failed to create config_version.ini.
pub const IC_ERROR_FAILED_TO_CREATE_CONFIG_VERSION: i32 = 7088;
/// Configuration has two nodes using the same host + port pair.
pub const IC_ERROR_TWO_NODES_USING_SAME_HOST_PORT_PAIR: i32 = 7089;
/// The Cluster Server was started with the wrong nodeid.
pub const IC_ERROR_CS_STARTED_WITH_WRONG_NODEID: i32 = 7090;
/// Command was too long.
pub const IC_ERROR_COMMAND_TOO_LONG: i32 = 7091;
/// Reserved, not used.
pub const IC_ERROR_NOT_USED_1: i32 = 7092;
/// Last command in command file must end with ;.
pub const IC_ERROR_NO_FINAL_COMMAND: i32 = 7093;
/// Inconsistent content in config_version.ini file.
pub const IC_ERROR_INCONSISTENT_CONTENT_IN_CONFIG_VERSION_FILE: i32 = 7094;
/// Syntax error in config_version.ini file.
pub const IC_ERROR_SYNTAX_ERROR_IN_CONFIG_VERSION_FILE: i32 = 7095;
/// The process is already in the process of being killed.
pub const IC_ERROR_PROCESS_ALREADY_BEING_KILLED: i32 = 7096;
/// Error message received is too large.
pub const IC_ERROR_TOO_LARGE_ERROR_MESSAGE: i32 = 7097;
/// Failed to start process.
pub const IC_ERROR_FAILED_TO_START_PROCESS: i32 = 7098;
/// Program not supported.
pub const IC_ERROR_PROGRAM_NOT_SUPPORTED: i32 = 7099;
/// File already exists.
pub const IC_ERROR_FILE_ALREADY_EXISTS: i32 = 7100;
/// Startup error.
pub const IC_ERROR_STARTUP: i32 = 7101;
/// Wrong IP family.
pub const IC_ERROR_WRONG_IP_FAMILY: i32 = 7102;
/// Missing schema name in metadata operation.
pub const IC_ERROR_MISSING_SCHEMA_NAME: i32 = 7103;
/// Missing database name in metadata operation.
pub const IC_ERROR_MISSING_DATABASE_NAME: i32 = 7104;
/// Missing table name in metadata operation.
pub const IC_ERROR_MISSING_TABLE_NAME: i32 = 7105;
/// Timeout waiting for API to succeed in setting up node connections to
/// cluster.
pub const IC_ERROR_TIMEOUT_WAITING_FOR_NODES: i32 = 7106;
/// API had no connected nodes for the cluster.
pub const IC_ERROR_FOUND_NO_CONNECTED_NODES: i32 = 7107;
/// No such data type exists.
pub const IC_ERROR_NO_SUCH_DATA_TYPE: i32 = 7108;
/// Missing column name in metadata operation.
pub const IC_ERROR_MISSING_COLUMN_NAME: i32 = 7109;
/// Table name too long.
pub const IC_ERROR_TOO_LONG_TABLE_NAME: i32 = 7110;
/// Metadata object already created on API Data Connection.
pub const IC_ERROR_ALREADY_CREATED_METADATA_OBJECT: i32 = 7111;
/// ic_daemonize failed to change directory.
pub const IC_ERROR_FAILED_TO_CHANGE_DIR: i32 = 7112;
/// Failed to open stdout file after daemonize.
pub const IC_ERROR_FAILED_OPEN_STDOUT: i32 = 7113;
/// Wrong content in pid file found when reading pid file.
pub const IC_ERROR_WRONG_PID_FILE_CONTENT: i32 = 7114;
/// Attempt to start uncertified program in process controller.
pub const IC_ERROR_PROGRAM_NOT_CERTIFIED_FOR_START: i32 = 7115;
/// Attempt to start program without proper node id set.
pub const IC_ERROR_NO_PROPER_NODE_ID_FOR_PROGRAM: i32 = 7116;
/// Failed to spawn program from process controller.
pub const IC_ERROR_FAILED_TO_SPAWN_PROGRAM: i32 = 7117;
/// Error found in configuration file(s).
pub const IC_ERROR_CONFIGURATION_ERROR: i32 = 7118;
/// Commands in client must end with ;, any other place for ; is an error.
pub const IC_ERROR_MALFORMED_CLIENT_STRING: i32 = 7119;
/// Too many cluster managers in configuration.
pub const IC_ERROR_TOO_MANY_CLUSTER_MANAGERS: i32 = 7120;
/// No configuration entry found in configuration.
pub const IC_ERROR_NO_CONF_ENTRY_FOUND: i32 = 7121;
/// No default node section found in configuration.
pub const IC_ERROR_NO_DEF_NODE_SECT_FOUND: i32 = 7122;
/// Incorrect node type given.
pub const IC_ERROR_NO_SUCH_NODE_TYPE: i32 = 7123;
/* New in the Rust port */
/// Unknown command line option.
pub const IC_ERROR_UNKNOWN_OPTION: i32 = 7124;
/// Bad value for command line option.
pub const IC_ERROR_OPTION_VALUE: i32 = 7125;
/// Help requested on the command line.
pub const IC_ERROR_HELP_REQUESTED: i32 = 7126;
/// Operation not supported in this release.
pub const IC_ERROR_NOT_SUPPORTED: i32 = 7127;
/// Timeout.
pub const IC_ERROR_TIMEOUT: i32 = 7128;
/// Internal error (panic) inside the library.
pub const IC_ERROR_INTERNAL_PANIC: i32 = 7129;
/// Condition needs more interpreter registers than available.
pub const IC_ERROR_CONDITION_TOO_COMPLEX: i32 = 7130;
/// No such field in table or record.
pub const IC_ERROR_NO_SUCH_FIELD: i32 = 7131;
/// The management server refused the request and said why.
pub const IC_ERROR_MGM_SERVER_REFUSED: i32 = 7132;
/// The management server is too old to serve this library.
pub const IC_ERROR_MGM_VERSION_TOO_OLD: i32 = 7133;
/// A data node kept its socket open but stopped answering heartbeats.
pub const IC_ERROR_HEARTBEAT_MISSED: i32 = 7134;
/// Our connection to a data node broke. This says nothing about the
/// node, which may well be up and serving everyone else, and still less
/// about the cluster. Only a failure report from another data node says
/// a node is down; that is [`IC_ERROR_NODE_DOWN`].
pub const IC_ERROR_LINK_LOST: i32 = 7135;
/// A data node answered our hello by asking us to go away, because it
/// is not expecting a connection from us yet. Ordinary while a node is
/// restarting; try again later.
pub const IC_ERROR_NODE_NOT_READY: i32 = 7136;
/// The management server says the node id we asked for is held by
/// another node. Unlike [`IC_ERROR_NO_NODEID`], waiting does not help
/// for as long as that node lives.
pub const IC_ERROR_NODEID_IN_USE: i32 = 7137;
/// The management server says the configuration does not allow the
/// node id we asked for, and that asking again will not change it.
pub const IC_ERROR_NODEID_NOT_ALLOWED: i32 = 7138;

/// An error: a code and, for operating system errors, nothing more.
///
/// This is the value carried in `Err(...)` by every fallible function in
/// the library. The C code returned the `int` directly; the C ABI turns
/// this struct back into that `int`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct IcError {
  /// Error code: 7000.. iClaustron, small positive `errno`, other
  /// values NDB error codes.
  pub code: i32,
}

impl IcError {
  /// An error with the given code.
  pub fn new(code: i32) -> IcError {
    IcError { code }
  }

  /// The error for the operating system's last reported error.
  pub fn last_os_error() -> IcError {
    IcError {
      code: crate::oserr::last_error(),
    }
  }

  /// The error for an `std::io::Error`, using its `errno` when it has
  /// one and `IC_ERROR_FAILED_TO_OPEN_FILE` otherwise.
  pub fn from_io(e: &std::io::Error) -> IcError {
    match e.raw_os_error() {
      Some(code) => IcError { code },
      None => IcError {
        code: IC_ERROR_FAILED_TO_OPEN_FILE,
      },
    }
  }

  /// True for an iClaustron error code.
  pub fn is_ic_error(&self) -> bool {
    self.code >= IC_FIRST_ERROR && self.code <= IC_LAST_ERROR
  }

  /// Text for the error: the iClaustron message for a code in our own
  /// range, otherwise whatever the C library calls it.
  ///
  /// Note that an `errno` value and an NDB error code can be the same
  /// number; nothing here can tell them apart. The Data API knows which
  /// domain a code came from and translates NDB codes with its own
  /// table before the application sees them.
  pub fn message(&self) -> String {
    if self.is_ic_error() {
      return message(self.code).to_string();
    }
    if self.code > 0 {
      return crate::oserr::strerror(self.code);
    }
    message(self.code).to_string()
  }
}

impl fmt::Display for IcError {
  fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    write!(f, "error {}: {}", self.code, self.message())
  }
}

impl std::error::Error for IcError {}

/// Text of an iClaustron error code; "No such error" for anything else.
pub fn message(code: i32) -> &'static str {
  match code {
    IC_ERROR_LINE_TOO_LONG => "Line was too long",
    IC_ERROR_CONFIG_BRACKET => "Missing ] after initial [",
    IC_ERROR_CONFIG_INCORRECT_GROUP_ID => "Found incorrect group id",
    IC_ERROR_CONFIG_IMPROPER_KEY_VALUE => "Improper key-value pair",
    IC_ERROR_CONFIG_NO_SUCH_SECTION => {
      "Section name doesn't exist in this type of configuration file"
    }
    IC_ERROR_MEM_ALLOC => "Memory allocation failure",
    IC_ERROR_NO_SECTION_DEFINED_YET => {
      "Tried to define key value before first section defined"
    }
    IC_ERROR_NO_SUCH_CONFIG_KEY => "No such configuration key exists",
    IC_ERROR_DEFAULT_VALUE_FOR_MANDATORY => {
      "Trying to assign default value to a mandatory config entry"
    }
    IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION => {
      "Assigning correct config entry in wrong section"
    }
    IC_ERROR_NO_NODES_FOUND => "No nodes found in the configuration file",
    IC_ERROR_WRONG_CONFIG_NUMBER => {
      "Number expected in config file, true, false and endings \
             with k, m, g also allowed"
    }
    IC_ERROR_NO_BOOLEAN_VALUE => {
      "Boolean value expected, got number larger than 1"
    }
    IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS => {
      "Configuration value is out of bounds, check data type and \
             min, max values"
    }
    IC_ERROR_NO_SERVER_NAME => {
      "Server name must be provided in all connections"
    }
    IC_ERROR_NO_SERVER_PORT => {
      "Server port must be provided in all connections"
    }
    IC_ERROR_GETADDRINFO => {
      "Provided client/server name/port not found by getaddrinfo"
    }
    IC_ERROR_ILLEGAL_SERVER_PORT => {
      "Provided server port isn't a legal port number"
    }
    IC_ERROR_DIFFERENT_IP_VERSIONS => {
      "Trying to use IPv4 and IPv6 simultaneously on server/client \
             part not supported"
    }
    IC_ERROR_ILLEGAL_CLIENT_PORT => {
      "Provided client port isn't a legal port number"
    }
    IC_ERROR_INCONSISTENT_DATA => "Internal data structure error",
    IC_ERROR_NODE_DOWN => "Node failure occurred",
    IC_ERROR_NO_SUCH_CLUSTER => "No such cluster",
    IC_ERROR_NO_SUCH_NODE => "No such node exists in this cluster",
    IC_ERROR_MESSAGE_CHECKSUM => "Message received with wrong checksum",
    IC_ERROR_ACCEPT_TIMEOUT => "Timeout when waiting for connection to accept",
    IC_ERROR_POLL_SET_FULL => "Poll set is full, need to use another poll set",
    IC_ERROR_NOT_FOUND_IN_POLL_SET => {
      "The file descriptor wasn't found in this poll set"
    }
    IC_ERROR_NODE_ALREADY_DEFINED => {
      "Can't use the same node id twice in a cluster"
    }
    IC_ERROR_PROCESS_NOT_ALIVE => "The process is not alive",
    IC_ERROR_COULD_NOT_LOCK_CONFIGURATION => {
      "The Cluster Server failed to lock the configuration, other \
             Cluster Server already running"
    }
    IC_ERROR_CHECK_PROCESS_SCRIPT => "The check process script failed",
    IC_ERROR_BOOTSTRAP_ALREADY_PERFORMED => {
      "Bootstrap on Cluster Server already performed"
    }
    IC_ERROR_CONFLICTING_CLUSTER_IDS => {
      "Cluster ids must be unique in configuration"
    }
    IC_ERROR_FAILED_TO_OPEN_FILE => "Failed to open file",
    IC_ERROR_BOOTSTRAP_NEEDED => {
      "Starting Cluster Server for the first time without bootstrap \
             flag set"
    }
    IC_ERROR_CONNECT_THREAD_STOPPED => "Connect thread stopped",
    IC_ERROR_THREADPOOL_FULL => "Threadpool is full",
    IC_ERROR_START_THREAD_FAILED => "Start thread failed",
    IC_ERROR_RECEIVE_TIMEOUT => {
      "Timeout in receiving line in NDB Management Protocol"
    }
    IC_ERROR_CONNECT_TIMEOUT => {
      "Timeout when waiting for connect on client side"
    }
    IC_ERROR_STOP_ORDERED => "Stop ordered in send thread",
    IC_ACCEPT_ERROR => "Accept error on socket",
    IC_END_OF_FILE => "Unexpected end of file",
    IC_PROTOCOL_ERROR => "Protocol error in NDB MGM Protocol",
    IC_AUTHENTICATE_ERROR => "Authentication error at connection setup",
    IC_SSL_ERROR => "SSL error on socket",
    IC_ERROR_PC_START_ALREADY_ONGOING => {
      "An attempt to start this process is already ongoing"
    }
    IC_ERROR_PC_PROCESS_ALREADY_RUNNING => "This process is already running",
    IC_ERROR_PROCESS_STUCK_IN_START_PHASE => {
      "Failed to stop/kill process, process stuck in start phase"
    }
    IC_ERROR_FAILED_TO_STOP_PROCESS => "Failed to stop/kill process",
    IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_PARAM => {
      "Set/Get connection parameter only supports set/get Server \
             Port Parameter"
    }
    IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_NODES => {
      "Set/Get connection parameter only supports client node as \
             server side"
    }
    IC_ERROR_INDEX_ZERO_NOT_ALLOWED => {
      "Index 0 isn't allowed in dynamic translations"
    }
    IC_ERROR_PTR_ARRAY_INDEX_OUT_OF_BOUND => {
      "Index out of bound in dynamic pointer array"
    }
    IC_ERROR_PTR_ARRAY_INDEX_ERROR => {
      "Trying to read non-existent entry in dynamic pointer array"
    }
    IC_ERROR_PARSE_CONNECTSTRING => "Error when parsing connect string",
    IC_ERROR_TOO_MANY_CS_HOSTS => "Too many hosts in connectstring",
    IC_ERROR_FAILED_TO_DAEMONIZE => "Failed to daemonize process",
    IC_ERROR_APPLICATION_STOPPED => "Application has been stopped",
    IC_ERROR_GET_CONFIG_BY_CLUSTER_SERVER => {
      "Trying to get configuration from network using cluster server"
    }
    IC_ERROR_BUFFER_MISSING_CREATE_APID_OP => {
      "Need to have valid buffers when creating APID operation object"
    }
    IC_ERROR_TOO_MANY_FIELDS => {
      "Defining more fields than table contains isn't valid"
    }
    IC_ERROR_DUPLICATE_FIELD_IDS => "Trying to define the same field twice",
    IC_ERROR_FIELD_ALREADY_DEFINED => "Trying to define the same field twice",
    IC_ERROR_FIELD_NOT_DEFINED => {
      "Trying to define a characteristic on a field not defined"
    }
    IC_ERROR_NOT_A_CHARSET_FIELD => {
      "Trying to define character set on a field not using it"
    }
    IC_ERROR_NOT_A_DECIMAL_FIELD => {
      "Trying to define decimal characteristics on a field not decimal"
    }
    IC_ERROR_NOT_A_SIGNABLE_FIELD => {
      "Trying to define signed or not on a field not a number field"
    }
    IC_ERROR_INDEX_ALREADY_DEFINED => "Trying to define the same index twice",
    IC_ERROR_INDEX_NOT_DEFINED => "Trying to operate on a non-existent index",
    IC_ERROR_MULTIPLE_METADATA_OPS => {
      "Currently only supported with one metadata table/tablespace \
             per transaction"
    }
    IC_ERROR_CREATE_TABLE_NO_FIELDS => {
      "Trying to create a table without fields"
    }
    IC_ERROR_MD_COMMIT_NO_OPERATION => {
      "Trying to commit metadata transaction without any operations"
    }
    IC_ERROR_ILLEGAL_MD_OPERATION_DROP_RENAME => {
      "Trying to perform operation not allowed with drop/rename table"
    }
    IC_ERROR_ILLEGAL_MD_OPERATION_CREATE => {
      "Trying to perform operation not allowed with create table"
    }
    IC_ERROR_TABLE_MUST_HAVE_PRIMARY_KEY => {
      "Tables defined in NDB must have a primary key"
    }
    IC_ERROR_RECORD_SIZE_TOO_BIG => "Record size exceeds the maximum",
    IC_ERROR_SET_CONNECTION_NO_DYNAMIC => {
      "Trying set dynamic port number when fixed port number is used"
    }
    IC_ERROR_NO_SUCH_ERROR => "No such error",
    IC_ERROR_MASTER_INDEX_VIEW_DIFFERS => {
      "Cluster Servers have different views on master index"
    }
    IC_ERROR_WRONG_NODE_ID => "Wrong node id",
    IC_ERROR_CHANGE_VIEW => "Cluster Server changed view on master node order",
    IC_ERROR_NO_NODEID => "No node id granted for now; asking again may help",
    IC_ERROR_NO_SUCH_CLUSTER_SERVER_NODEID => {
      "Node id provided didn't exist in config"
    }
    IC_ERROR_FAILED_TO_OPEN_COMMON_GRID_CONFIG => {
      "Failed to open common grid configuration file grid_common.ini"
    }
    IC_ERROR_FAILED_TO_OPEN_CLUSTER_CONFIG => {
      "Failed to open configuration file of a cluster"
    }
    IC_ERROR_FAILED_TO_OPEN_CLUSTER_LIST => {
      "Failed to open configuration file of clusters in the grid, \
             config.ini"
    }
    IC_ERROR_FAILED_TO_CREATE_CONFIG_VERSION => {
      "Failed to create config_version.ini"
    }
    IC_ERROR_TWO_NODES_USING_SAME_HOST_PORT_PAIR => {
      "Configuration has two nodes using the same host + port pair"
    }
    IC_ERROR_CS_STARTED_WITH_WRONG_NODEID => {
      "The Cluster Server was started with the wrong nodeid"
    }
    IC_ERROR_COMMAND_TOO_LONG => "Command was too long",
    IC_ERROR_NOT_USED_1 => "",
    IC_ERROR_NO_FINAL_COMMAND => "Last command in command file must end with ;",
    IC_ERROR_INCONSISTENT_CONTENT_IN_CONFIG_VERSION_FILE => {
      "Inconsistent content in config_version.ini file"
    }
    IC_ERROR_SYNTAX_ERROR_IN_CONFIG_VERSION_FILE => {
      "Syntax error in config_version.ini file"
    }
    IC_ERROR_PROCESS_ALREADY_BEING_KILLED => {
      "The process is already in the process of being killed"
    }
    IC_ERROR_TOO_LARGE_ERROR_MESSAGE => "Error message received is too large",
    IC_ERROR_FAILED_TO_START_PROCESS => "Failed to start process",
    IC_ERROR_PROGRAM_NOT_SUPPORTED => "Program not supported",
    IC_ERROR_FILE_ALREADY_EXISTS => "File already exists",
    IC_ERROR_STARTUP => "Startup error",
    IC_ERROR_WRONG_IP_FAMILY => "Wrong IP family",
    IC_ERROR_MISSING_SCHEMA_NAME => "Missing schema name in metadata operation",
    IC_ERROR_MISSING_DATABASE_NAME => {
      "Missing database name in metadata operation"
    }
    IC_ERROR_MISSING_TABLE_NAME => "Missing table name in metadata operation",
    IC_ERROR_TIMEOUT_WAITING_FOR_NODES => {
      "Timeout waiting for API to succeed in setting up node \
             connections to cluster"
    }
    IC_ERROR_FOUND_NO_CONNECTED_NODES => {
      "API had no connected nodes for the cluster"
    }
    IC_ERROR_NO_SUCH_DATA_TYPE => "No such data type exists",
    IC_ERROR_MISSING_COLUMN_NAME => "Missing column name in metadata operation",
    IC_ERROR_TOO_LONG_TABLE_NAME => "Table name too long",
    IC_ERROR_ALREADY_CREATED_METADATA_OBJECT => {
      "Metadata object already created on API Data Connection"
    }
    IC_ERROR_FAILED_TO_CHANGE_DIR => "ic_daemonize failed to change directory",
    IC_ERROR_FAILED_OPEN_STDOUT => "Failed to open stdout file after daemonize",
    IC_ERROR_WRONG_PID_FILE_CONTENT => {
      "Wrong content in pid file found when reading pid file"
    }
    IC_ERROR_PROGRAM_NOT_CERTIFIED_FOR_START => {
      "Attempt to start uncertified program in process controller"
    }
    IC_ERROR_NO_PROPER_NODE_ID_FOR_PROGRAM => {
      "Attempt to start program without proper node id set"
    }
    IC_ERROR_FAILED_TO_SPAWN_PROGRAM => {
      "Failed to spawn program from process controller"
    }
    IC_ERROR_CONFIGURATION_ERROR => "Error found in configuration file(s)",
    IC_ERROR_MALFORMED_CLIENT_STRING => {
      "Commands in client must end with ;, any other place for ; is \
             an error"
    }
    IC_ERROR_TOO_MANY_CLUSTER_MANAGERS => {
      "Too many cluster managers in configuration"
    }
    IC_ERROR_NO_CONF_ENTRY_FOUND => {
      "No configuration entry found in configuration"
    }
    IC_ERROR_NO_DEF_NODE_SECT_FOUND => {
      "No default node section found in configuration"
    }
    IC_ERROR_NO_SUCH_NODE_TYPE => "Incorrect node type given",
    IC_ERROR_UNKNOWN_OPTION => "Unknown command line option",
    IC_ERROR_OPTION_VALUE => "Bad value for command line option",
    IC_ERROR_HELP_REQUESTED => "Help requested on the command line",
    IC_ERROR_NOT_SUPPORTED => "Operation not supported in this release",
    IC_ERROR_TIMEOUT => "Timeout",
    IC_ERROR_INTERNAL_PANIC => "Internal error (panic) inside the library",
    IC_ERROR_CONDITION_TOO_COMPLEX => {
      "Condition needs more interpreter registers than available"
    }
    IC_ERROR_NO_SUCH_FIELD => "No such field in table or record",
    IC_ERROR_MGM_SERVER_REFUSED => "The management server refused the request",
    IC_ERROR_MGM_VERSION_TOO_OLD => {
      "The management server is too old for this library"
    }
    IC_ERROR_HEARTBEAT_MISSED => "Data node stopped answering heartbeats",
    IC_ERROR_LINK_LOST => "Connection to data node lost; the node may be up",
    IC_ERROR_NODE_NOT_READY => "Data node is not accepting our connection yet",
    IC_ERROR_NODEID_IN_USE => "Node id is held by another node",
    IC_ERROR_NODEID_NOT_ALLOWED => {
      "The configuration does not allow this node id here"
    }
    _ => "Unknown error code",
  }
}

/// Find the iClaustron error code with exactly this message text;
/// `IC_ERROR_NO_SUCH_ERROR` when none matches.
pub fn translate_error_string(text: &str) -> i32 {
  let mut code: i32 = IC_FIRST_ERROR;
  while code <= IC_LAST_ERROR {
    if message(code) == text {
      return code;
    }
    code += 1;
  }
  IC_ERROR_NO_SUCH_ERROR
}

/// Print an error code with its text through `ic_printf!`, and the last
/// OS error if there is one (`ic_print_error` in the C code).
pub fn print_error(code: i32) {
  let err = IcError::new(code);
  if err.is_ic_error() {
    crate::ic_printf!("{}", message(code));
  } else {
    crate::ic_printf!("OS Error number = {}", code);
    crate::ic_printf!("OS Error: {}", crate::oserr::strerror(code));
  }
  let last = crate::oserr::last_error();
  if last != 0 {
    crate::ic_printf!(
      "Last reported OS Error: {}",
      crate::oserr::strerror(last)
    );
  }
}

/// `ic_assert!(cond)`: in debug builds print the failing location and
/// abort; compiled to nothing in release builds, like the C `ic_assert`.
#[macro_export]
macro_rules! ic_assert {
  ($cond:expr) => {
    if cfg!(debug_assertions) && !($cond) {
      $crate::ic_printf!(
        "Failed assert on line {} in file: {}",
        line!(),
        file!()
      );
      ::std::process::abort();
    }
  };
}

/// `ic_require!(cond)`: abort the process when the condition is false,
/// in every build (the C `ic_require`).
#[macro_export]
macro_rules! ic_require {
  ($cond:expr) => {
    if !($cond) {
      $crate::ic_printf!(
        "Failed require on line {} in file: {}",
        line!(),
        file!()
      );
      ::std::process::abort();
    }
  };
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn every_code_has_a_message() {
    let mut code: i32 = IC_FIRST_ERROR;
    while code <= IC_LAST_ERROR {
      if code != IC_ERROR_NOT_USED_1 {
        let text = message(code);
        assert_ne!(text, "Unknown error code", "code {}", code);
        assert!(!text.is_empty(), "code {}", code);
      }
      code += 1;
    }
    assert_eq!(message(IC_LAST_ERROR + 1), "Unknown error code");
    assert_eq!(message(IC_FIRST_ERROR - 1), "Unknown error code");
  }

  #[test]
  fn translate_round_trip() {
    let text = message(IC_ERROR_MEM_ALLOC);
    assert_eq!(translate_error_string(text), IC_ERROR_MEM_ALLOC);
    assert_eq!(translate_error_string("nonsense"), IC_ERROR_NO_SUCH_ERROR);
  }

  #[test]
  fn error_value_classifies() {
    let ic = IcError::new(IC_ERROR_TIMEOUT);
    assert!(ic.is_ic_error());
    assert_eq!(ic.message(), "Timeout");
    /* An errno gets the text the C library gives it. */
    let os = IcError::new(libc::ENOENT);
    assert!(!os.is_ic_error());
    assert!(!os.message().is_empty());
    /* 7079 means "no such error"; it is not the unknown-code text. */
    let no_such = IcError::new(IC_ERROR_NO_SUCH_ERROR);
    assert_eq!(no_such.message(), "No such error");
    assert_ne!(message(99999), "No such error");
  }

  #[test]
  fn require_passes_on_true() {
    ic_require!(1 + 1 == 2);
    ic_assert!(true);
  }
}
