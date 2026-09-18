// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Constants from `legacy-c/include/ic_base_header.h`, updated for
//! RonDB 26.10 where the C values described NDB 7.2.9. Constants that
//! only served the cluster server and the other management programs are
//! not carried over.

/// NDB "null" reference value (RNIL).
pub const IC_RNIL: u32 = 0xFFFF_FF00;
/// NDB 16-bit "null" value (ZNIL).
pub const IC_ZNIL: u32 = 0xFFFF;
/// All bits set, used as an undefined 32-bit value.
pub const IC_MINUS_ONE: u32 = 0xFFFF_FFFF;
/// Largest 32-bit value.
pub const IC_MAX_UINT32: u32 = 0xFFFF_FFFF;
/// Size in bytes of a 32-bit word.
pub const IC_SIZE_UINT32: usize = 4;

/// Number of send timestamps kept per node by the adaptive send algorithm.
pub const IC_MAX_SEND_TIMERS: usize = 16;
/// Upper bound on sends tracked before a decision by adaptive send.
pub const IC_MAX_SENDS_TRACKED: u32 = 8;
/// Largest single socket send in bytes.
pub const IC_MAX_SEND_SIZE: usize = 65535;
/// Number of send buffers per node connection.
pub const IC_MAX_SEND_BUFFERS: usize = 16;
/// Default memory buffer page size in bytes.
pub const IC_MEMBUF_SIZE: usize = 32768;

/// Buffer size used when reading a configuration line.
pub const CONFIG_READ_BUF_SIZE: usize = 256;
/// Largest error string returned to the application.
pub const IC_MAX_ERROR_STRING_SIZE: usize = 256;
/// Buffer size used when reading a protocol command line.
pub const COMMAND_READ_BUF_SIZE: usize = 2048;
/// Buffer size large enough for any printed 64-bit number.
pub const IC_NUMBER_SIZE: usize = 32;
/// Buffer size large enough for any printed IP address.
pub const IC_IP_ADDRESS_SIZE: usize = 128;
/// Longest accepted configuration line.
pub const IC_MAX_CONFIG_LINE_LEN: usize = 120;

/// Default port of `ndb_mgmd` (the NDB management server).
pub const IC_DEF_CLUSTER_SERVER_PORT: u16 = 1186;
/// Default port of `ndb_mgmd` as text.
pub const IC_DEF_CLUSTER_SERVER_PORT_STR: &str = "1186";
/// Default port for iClaustron servers.
pub const IC_DEF_PORT: u16 = 1187;

/// Stack size for lightweight library threads (bytes).
pub const IC_SMALL_STACK_SIZE: usize = 64 * 1024 + PTHREAD_STACK_MIN;
/// Stack size for medium library threads (bytes).
pub const IC_MEDIUM_STACK_SIZE: usize = 256 * 1024 + PTHREAD_STACK_MIN;
/// Stack size 0 means the platform default (usually around 1 MByte).
pub const IC_NORMAL_STACK_SIZE: usize = 0;
/// Minimum stack the threading library needs for itself.
pub const PTHREAD_STACK_MIN: usize = 64 * 1024;

/// Assumed cache line size for padding hot structures.
pub const IC_STD_CACHE_LINE_SIZE: usize = 128;

/// NDB signal header size in 32-bit words.
pub const IC_NDB_MESSAGE_HEADER_SIZE: u32 = 3;
/// Highest NDB signal priority level.
pub const IC_NDB_MAX_PRIO_LEVEL: u32 = 1;
/// Largest signal data part in 32-bit words.
pub const IC_NDB_MAX_MAIN_MESSAGE_SIZE: u32 = 25;
/// Kernel block numbers are below this value.
pub const IC_NDB_MAX_MODULE_ID: u32 = 4096;
/// First block number for API client threads (`MIN_API_BLOCK_NO`).
/// Verify: RonDB `include/kernel/BlockNumbers.h:34`.
pub const IC_NDB_MIN_MODULE_ID_FOR_THREADS: u32 = 32768;
/// Block number that packed signals are addressed to (`API_PACKED`).
/// Verify: RonDB `include/kernel/BlockNumbers.h:37`.
pub const IC_NDB_PACKED_MODULE_ID: u32 = 2047;
/// Fixed block number of the API cluster manager (`API_CLUSTERMGR`).
/// Verify: RonDB `include/kernel/BlockNumbers.h:40`.
pub const IC_NDB_API_CLUSTERMGR_MODULE_ID: u32 = 0x0FA2;
/// Highest block number this library will address.
pub const IC_MAX_MODULE_ID: u32 = 8192;
/// Signal buffers preallocated per thread.
pub const IC_PREALLOC_NUM_MESSAGES: u32 = 8;
/// Milliseconds to wait for a free send buffer page.
pub const IC_WAIT_SEND_BUF_POOL: u32 = 3000;

/// Normal NDB signal priority.
pub const IC_NDB_NORMAL_PRIO: u32 = 0;
/// High NDB signal priority.
pub const IC_NDB_HIGH_PRIO: u32 = 1;

/// Kernel block QMGR (0xFC). Verify: `BlockNumbers.h`.
pub const IC_NDB_QMGR_MODULE: u32 = 252;
/// Kernel block DBTC (0xF5).
pub const IC_NDB_TC_MODULE: u32 = 245;
/// Kernel block DBDICT (0xFA).
pub const IC_NDB_DICT_MODULE: u32 = 250;
/// Kernel block CMVMI (0xFE).
pub const IC_NDB_CMVMI_MODULE: u32 = 254;
/// Kernel block SUMA (0x101).
pub const IC_NDB_SUMA_MODULE: u32 = 257;
/// Kernel block DBINFO (0x107).
pub const IC_NDB_INFO_MODULE: u32 = 263;
/// Kernel block TRPMAN (0x10A).
pub const IC_NDB_TRPMAN_MODULE: u32 = 266;

/// Seconds a thread waits before rechecking the stop flag.
pub const IC_MAX_THREAD_WAIT_TIME: u32 = 60;
/// Microseconds per second.
pub const IC_MICROSEC_PER_SECOND: u64 = 1_000_000;
/// Microseconds per millisecond.
pub const IC_MICROSEC_PER_MILLI: u64 = 1000;
/// Seconds between stop flag checks in long waits.
pub const IC_STOP_CHECK_TIMER: u32 = 3;
/// Size of error message buffers.
pub const ERROR_MESSAGE_SIZE: usize = 512;

/// iClaustron version number.
pub const IC_VERSION: u32 = 0x00_0100;
/// iClaustron version string.
pub const IC_VERSION_STR: &str = "iclaustron-0.1.0";
/// NDB version announced to the cluster: RonDB 26.10.0 encoded as
/// `(major << 16) | (minor << 8) | build`. Verify:
/// RonDB `include/ndb_version.h.in:48-61`.
pub const NDB_VERSION: u32 = 0x1A_0A00;
/// MySQL version announced to the cluster; RonDB uses the same number.
pub const MYSQL_VERSION: u32 = 0x1A_0A00;
/// Version string announced to the cluster.
pub const MYSQL_VERSION_STRING: &str = "rondb-26.10.0";
/// Bit position of the version field in the NDB protocol.
pub const IC_VERSION_BIT_START: u32 = 24;
/// Bit position of the protocol flag in the NDB protocol.
pub const IC_PROTOCOL_BIT: u32 = 20;

/// Highest node id in a cluster.
pub const IC_MAX_NODE_ID: u32 = 255;
/// Highest number of data nodes in a RonDB cluster (`MAX_NDB_NODES - 1`).
/// Verify: RonDB `include/kernel/ndb_limits.h`.
pub const IC_MAX_NDB_DATA_NODES: u32 = 144;
/// Highest number of user threads (connection objects) per process.
pub const IC_MAX_THREAD_CONNECTIONS: u32 = 256;
/// Same limit under its Data API name.
pub const IC_MAX_APID_NUM_THREADS: u32 = 256;
/// Longest file name handled.
pub const IC_MAX_FILE_NAME_SIZE: usize = 255;
/// Longest printed integer.
pub const IC_MAX_INT_STRING: usize = 32;
/// Largest row in bytes (`NDB_MAX_TUPLE_SIZE_IN_WORDS * 4`).
/// Verify: RonDB `include/ndbapi/ndbapi_limits.h`.
pub const IC_MAX_RECORD_SIZE: usize = 72000;
/// Longest table name.
pub const IC_MAX_TABLE_NAME_SIZE: usize = 512;

/// The space character.
pub const SPACE_CHAR: u8 = 32;
/// Line terminator used by the NDB management protocol (`\n`).
pub const CARRIAGE_RETURN: u8 = 10;
/// The `\r` character.
pub const LINE_FEED: u8 = 13;
/// The NUL byte.
pub const NULL_BYTE: u8 = 0;
/// Command separator in the client protocols.
pub const CMD_SEPARATOR: u8 = b';';

/// Round `a` up to a multiple of `b` (`ic_align` in the C code).
pub fn ic_align(a: usize, b: usize) -> usize {
  /* The C macro is ((a + (b - 1)) / b) * b; div_ceil is that division. */
  let blocks = a.div_ceil(b);
  blocks * b
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn align_rounds_up() {
    assert_eq!(ic_align(0, 4), 0);
    assert_eq!(ic_align(1, 4), 4);
    assert_eq!(ic_align(4, 4), 4);
    assert_eq!(ic_align(5, 4), 8);
    assert_eq!(ic_align(10, 4), 12);
  }

  #[test]
  fn ndb_version_is_26_10_0() {
    assert_eq!(NDB_VERSION >> 16, 26);
    assert_eq!((NDB_VERSION >> 8) & 0xFF, 10);
    assert_eq!(NDB_VERSION & 0xFF, 0);
  }
}
