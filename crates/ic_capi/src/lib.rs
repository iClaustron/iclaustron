// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_capi`: the C ABI of iClaustron, built as `libiclaustron`, with
//! its header `include/iclaustron/ic_apid.h` generated from this crate
//! by cbindgen (`cargo xtask header`).
//!
//! One exported function per method of the Data API (chapter 04, "The
//! C ABI"). The rules, in short:
//!
//! - Handles are opaque pointers to the Rust objects themselves: an
//!   `IC_APID_CONNECTION*` is the `ApidConnection`, so that a callback
//!   can be handed the connection it runs on. Queries and transactions
//!   are `uint32_t` ids within their connection; an id that names
//!   nothing is refused, never followed.
//! - A function that can fail returns `int`: 0, or the error code. What
//!   it makes it gives through an out pointer.
//! - Every entry catches a panic and returns `IC_ERROR_INTERNAL_PANIC`;
//!   a connection a panic went through is marked unusable and refuses
//!   every later call but its free.
//! - Option and argument structs begin with their own size, filled by
//!   their `..._init(&s, sizeof s)`, so that a later release can add
//!   fields at the end.
//! - The query owns its rows; C gets pointers to them (Q10). So does a
//!   scan, and a scan reader.

use std::ffi::c_char;
use std::ffi::c_void;
use std::ffi::CStr;
use std::ffi::CString;
use std::panic::catch_unwind;
use std::panic::AssertUnwindSafe;
use std::sync::Arc;
use std::sync::OnceLock;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::apid_global::GlobalOptions;
use ic_apid::dict_cache::IndexDef;
use ic_apid::dict_cache::TableDef;
use ic_apid::executor::Executor;
use ic_apid::executor::ExecutorOptions;
use ic_apid::executor::KeyOp;
use ic_apid::executor::OpShape;
use ic_apid::executor::Request;
use ic_apid::executor::Submitter;
use ic_apid::index_scan::ScanRange;
use ic_apid::key_op;
use ic_apid::program::Cmp;
use ic_apid::program::Cond;
use ic_apid::program::Program;
use ic_apid::program::ProgramBuilder;
use ic_apid::program::Value;
use ic_apid::query::AbortOption;
use ic_apid::query::BatchHint;
use ic_apid::query::QueryId;
use ic_apid::query::QueryState;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::TransId;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::record::FieldSpec;
use ic_apid::record::Record;
use ic_apid::scan::ScanArgs;
use ic_apid::scan::ScanBatchId;
use ic_apid::scan::ScanId;
use ic_apid::scan::ScanLock;
use ic_apid::scan::ScanState;
use ic_apid::scan::TakeoverArgs;
use ic_apid::scan_reader::ScanReader;
use ic_apid::scan_reader::ScanShape;
use ic_apid::text_row;
use ic_apid::transaction::CommitState;
use ic_apid::transaction::TransactionHint;
use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;
use ic_signals::table_props;

/// Major version of the C API.
pub const IC_API_VERSION_MAJOR: u32 = 0;
/// Minor version of the C API.
pub const IC_API_VERSION_MINOR: u32 = 1;
/// Patch version of the C API.
pub const IC_API_VERSION_PATCH: u32 = 0;

// ---- Values (the C header's #defines) ----

/// A shared lock taken and let go of in the read.
pub const IC_READ_KIND_SIMPLE: u32 = 0;
/// A shared lock held until the transaction ends.
pub const IC_READ_KIND_LOCKED: u32 = 1;
/// An exclusive lock held until the transaction ends.
pub const IC_READ_KIND_EXCLUSIVE: u32 = 2;
/// The last committed value, without a lock.
pub const IC_READ_KIND_COMMITTED: u32 = 3;
/// As exclusive, failing rather than waiting for another's lock.
pub const IC_READ_KIND_EXCLUSIVE_NO_WAIT: u32 = 4;

/// The row must be there.
pub const IC_WRITE_KIND_UPDATE: u32 = 0;
/// Insert, or overwrite what is there.
pub const IC_WRITE_KIND_WRITE: u32 = 1;
/// The row must not be there.
pub const IC_WRITE_KIND_INSERT: u32 = 2;
/// Delete the row.
pub const IC_WRITE_KIND_DELETE: u32 = 3;

/// Abort on error for a write, ignore it for a read.
pub const IC_ABORT_DEFAULT: u32 = 0;
/// A failure rolls the transaction back.
pub const IC_ABORT_ON_ERROR: u32 = 1;
/// A failure fails the query alone.
pub const IC_IGNORE_ERROR: u32 = 2;

/// No batching hint.
pub const IC_BATCH_HINT_NONE: u32 = 0;
/// The write does not depend on earlier ones in its batch.
pub const IC_BATCH_HINT_SAFE: u32 = 1;
/// It does.
pub const IC_BATCH_HINT_UNSAFE: u32 = 2;

/// Ready to be defined.
pub const IC_QUERY_IDLE: u32 = 0;
/// Defined on a transaction and not yet sent.
pub const IC_QUERY_DEFINED: u32 = 1;
/// Sent, its replies not all in.
pub const IC_QUERY_SENT: u32 = 2;
/// Done, and waiting to be taken from the executed list.
pub const IC_QUERY_COMPLETED: u32 = 3;
/// The id names no query.
pub const IC_QUERY_UNKNOWN: u32 = 0xFFFF_FFFF;

/// Started, and not yet asked to end.
pub const IC_COMMIT_STATE_STARTED: u32 = 0;
/// Asked to commit; the coordinator has not answered.
pub const IC_COMMIT_STATE_COMMIT_REQUESTED: u32 = 1;
/// Asked to roll back; the coordinator has not answered.
pub const IC_COMMIT_STATE_ROLLBACK_REQUESTED: u32 = 2;
/// Committed.
pub const IC_COMMIT_STATE_COMMITTED: u32 = 3;
/// Rolled back.
pub const IC_COMMIT_STATE_ROLLED_BACK: u32 = 4;
/// The id names no transaction.
pub const IC_COMMIT_STATE_UNKNOWN: u32 = 0xFFFF_FFFF;

/// The next started node in turn.
pub const IC_HINT_ANY: u32 = 0;
/// The node whose id the hint's value is.
pub const IC_HINT_NODE: u32 = 1;
/// A node holding the partition the hint's value names.
pub const IC_HINT_PARTITION: u32 = 2;

/// A scan reads the last committed value of each row, with no lock.
pub const IC_SCAN_LOCK_COMMITTED: u32 = 0;
/// A scan locks each row of a batch shared, until the batch is handed
/// back.
pub const IC_SCAN_LOCK_SHARED: u32 = 1;
/// A scan locks each row of a batch exclusively, until the batch is
/// handed back.
pub const IC_SCAN_LOCK_EXCLUSIVE: u32 = 2;

/// The scan is ready to be started.
pub const IC_SCAN_IDLE: u32 = 0;
/// Started, waiting for a record at its coordinator.
pub const IC_SCAN_STARTING: u32 = 1;
/// Its request has gone; batches come.
pub const IC_SCAN_RUNNING: u32 = 2;
/// Closing: waiting for what was asked for, then for the coordinator.
pub const IC_SCAN_CLOSING: u32 = 3;
/// The run is over: read to the end, closed, or failed.
pub const IC_SCAN_DONE: u32 = 4;
/// The id names no scan.
pub const IC_SCAN_UNKNOWN: u32 = 0xFFFF_FFFF;

/// No transaction, in `IC_SCAN_ARGS`.
pub const IC_NO_TRANS: u32 = 0xFFFF_FFFF;
/// No partition: every partition is scanned, in `IC_SCAN_ARGS`.
pub const IC_NO_PARTITION: u32 = 0xFFFF_FFFF;

/// Nothing went wrong.
pub const IC_ERROR_CATEGORY_NONE: u32 = 0;
/// The application asked for something wrong.
pub const IC_ERROR_CATEGORY_USER: u32 = 1;
/// A fault in the library or the cluster.
pub const IC_ERROR_CATEGORY_INTERNAL: u32 = 2;
/// The world outside: a node failure, an overload, a timeout.
pub const IC_ERROR_CATEGORY_EXTERNAL: u32 = 3;

/// Nothing went wrong.
pub const IC_ERROR_SEVERITY_NONE: u32 = 0;
/// Worth knowing, nothing failed.
pub const IC_ERROR_SEVERITY_WARNING: u32 = 1;
/// Failed now; the same request may well succeed if tried again.
pub const IC_ERROR_SEVERITY_TEMPORARY: u32 = 2;
/// Failed for good; the request has to change.
pub const IC_ERROR_SEVERITY_ERROR: u32 = 3;
/// The library can no longer be used.
pub const IC_ERROR_SEVERITY_STOP: u32 = 4;

/// A field without a null bit, in an `IC_FIELD_SPEC`.
pub const IC_NO_NULL_BIT: u32 = 0xFFFF_FFFF;

/// A panic inside the library; the connection it went through is
/// unusable.
pub const IC_ERROR_INTERNAL_PANIC: i32 = 7129;
/// A null handle or out pointer.
pub const IC_ERROR_NULL_POINTER: i32 = 7157;
/// Text that is not UTF-8, or a value given as text that does not fit.
pub const IC_ERROR_BAD_TEXT: i32 = 7158;
/// No row with that key (an NDB error).
pub const IC_NDB_ERROR_NO_SUCH_ROW: i32 = 626;
/// A row with that key is there already (an NDB error).
pub const IC_NDB_ERROR_ROW_EXISTS: i32 = 630;
/// The transaction's replies stopped coming; its outcome is unknown.
pub const IC_NDB_ERROR_NO_REPLY: i32 = 4012;
/// A NULL in a key (an NDB error).
pub const IC_NDB_ERROR_NULL_KEY: i32 = 4316;
/// The row would break a unique index, which the error data names (an
/// NDB error).
pub const IC_NDB_ERROR_UNIQUE_DUPLICATE: i32 = 893;
/// A lock wait timed out, probably in a deadlock; temporary (an NDB
/// error).
pub const IC_NDB_ERROR_LOCK_TIMEOUT: i32 = 266;
/// A node failure aborted the transaction; temporary (an NDB error).
pub const IC_NDB_ERROR_NODE_FAILURE_ABORT: i32 = 4010;
/// A scan's coordinator's node failed; temporary (an NDB error).
pub const IC_NDB_ERROR_SCAN_NODE_FAILURE: i32 = 4028;
/// Too many scans run at once at the coordinator; temporary (an NDB
/// error).
pub const IC_NDB_ERROR_TOO_MANY_SCANS: i32 = 245;
/// A scan waited too long for a lock, probably in a deadlock; temporary
/// (an NDB error).
pub const IC_NDB_ERROR_SCAN_LOCK_TIMEOUT: i32 = 274;
/// A scan was left too long without asking for more, as when a batch is
/// not handed back; temporary (an NDB error).
pub const IC_NDB_ERROR_SCAN_IDLE: i32 = 296;
/// The row did not meet the condition of an operation that aborts on
/// error, which rolls the transaction back (ours, in NDB's range for
/// programs).
pub const IC_NDB_ERROR_CONDITION: i32 = 6000;

/// A text that is not a connectstring.
pub const IC_ERROR_PARSE_CONNECTSTRING: i32 = 7056;
/// No management server answered in time.
pub const IC_ERROR_CONNECT_TIMEOUT: i32 = 7040;
/// The node id asked for is held by another node.
pub const IC_ERROR_NODEID_IN_USE: i32 = 7137;
/// The management server refused the request: no API slot free, or our
/// node id not allowed.
pub const IC_ERROR_MGM_SERVER_REFUSED: i32 = 7132;
/// The process has as many connections as it may, 4 711.
pub const IC_ERROR_TOO_MANY_USER_THREADS: i32 = 7139;
/// The link to a data node was lost; the node may be up. Temporary.
pub const IC_ERROR_LINK_LOST: i32 = 7135;
/// No data node is connected and started. Temporary.
pub const IC_ERROR_NO_STARTED_DATA_NODE: i32 = 7142;
/// A wait for the cluster ran out. Temporary.
pub const IC_ERROR_TIMEOUT: i32 = 7128;
/// No table by that name in that database.
pub const IC_ERROR_NO_SUCH_TABLE: i32 = 7140;
/// No unique index by that name on the table.
pub const IC_ERROR_NO_SUCH_INDEX: i32 = 7145;
/// The data node's dictionary refused the request.
pub const IC_ERROR_DICT_REFUSED: i32 = 7141;
/// The table changed every time its description was fetched.
pub const IC_ERROR_TABLE_KEEPS_CHANGING: i32 = 7144;
/// The index is still being built, being dropped, or broken.
pub const IC_ERROR_INDEX_NOT_ONLINE: i32 = 7154;
/// No field with that id or name, in the table or the record.
pub const IC_ERROR_NO_SUCH_FIELD: i32 = 7131;
/// More fields than the table has.
pub const IC_ERROR_TOO_MANY_FIELDS: i32 = 7062;
/// A field named twice.
pub const IC_ERROR_DUPLICATE_FIELD_IDS: i32 = 7063;
/// Fields overlap or lie outside the row, or a row is too short.
pub const IC_ERROR_RECORD_LAYOUT: i32 = 7146;
/// A nullable field was given no null bit, or NULL was set on a field
/// that cannot be NULL.
pub const IC_ERROR_NO_NULL_BIT: i32 = 7147;
/// The key record does not fit the query: a key field missing, or a
/// record over another table or another version of it.
pub const IC_ERROR_KEY_RECORD: i32 = 7148;
/// The query or the transaction is not in a state for that, or the id
/// names no transaction.
pub const IC_ERROR_TRANSACTION_ACTIVE: i32 = 7150;
/// Not done in this release, or a value no define in this header names.
pub const IC_ERROR_NOT_SUPPORTED: i32 = 7127;
/// Options or arguments not filled by their _init, or a struct smaller
/// than this header's.
pub const IC_ERROR_INCONSISTENT_DATA: i32 = 7020;
/// A delete on a table with a blob column, refused until blobs are read
/// and written.
pub const IC_ERROR_DELETE_WITH_BLOBS: i32 = 7153;
/// An update through a unique index that sets an indexed column.
pub const IC_ERROR_INDEXED_COLUMN_UPDATE: i32 = 7155;
/// A key longer than any table's key can be, 1 023 words.
pub const IC_ERROR_KEY_TOO_LONG: i32 = 7156;
/// More column data than a request may carry, 32 768 words.
pub const IC_ERROR_RECORD_SIZE_TOO_BIG: i32 = 7077;
/// An executor's list of requests is full. Temporary.
pub const IC_ERROR_EXECUTOR_BUSY: i32 = 7159;
/// The executor has stopped and takes no requests; the request was not
/// run.
pub const IC_ERROR_EXECUTOR_STOPPED: i32 = 7160;
/// The scan, or the scan reader, is running, and cannot be started or
/// freed until its run is over.
pub const IC_ERROR_SCAN_ACTIVE: i32 = 7161;
/// A takeover found no scanned row to act on: none read, or its batch
/// handed back.
pub const IC_ERROR_NO_SCAN_ROW: i32 = 7162;
/// A range of an index scan that cannot be sent: its number above 4095,
/// more columns than the index has, bounds too long, or in an ordered
/// run of several ranges a number not above the one before.
pub const IC_ERROR_BAD_RANGE: i32 = 7163;
/// A program needs more than eight registers at once, or is too long.
pub const IC_ERROR_CONDITION_TOO_COMPLEX: i32 = 7130;
/// A program compares or assigns values of different types.
pub const IC_ERROR_NOT_COMPARABLE: i32 = 7164;
/// A program names a value, condition or input it does not have, or is
/// put on an operation of another table.
pub const IC_ERROR_BAD_PROGRAM: i32 = 7165;
/// The table has no AUTO_INCREMENT column, nor a hidden key.
pub const IC_ERROR_NO_AUTO_INCREMENT: i32 = 7166;

/// Equal, in a program's comparison of its first value with its second.
pub const IC_CMP_EQ: u32 = 0;
/// Not equal.
pub const IC_CMP_NE: u32 = 1;
/// Less.
pub const IC_CMP_LT: u32 = 2;
/// Less or equal.
pub const IC_CMP_LE: u32 = 3;
/// Greater.
pub const IC_CMP_GT: u32 = 4;
/// Greater or equal.
pub const IC_CMP_GE: u32 = 5;
/// The inputs an operation gives its program, numbered from 0.
pub const IC_PROGRAM_INPUTS: u32 = 16;
/// A value or condition a builder could not make, from a null builder or
/// record or an unknown comparison; using it fails the finish with
/// `IC_ERROR_BAD_PROGRAM`.
pub const IC_PROGRAM_NONE: u32 = 0xFFFF_FFFF;

// ---- Types ----

/// The Data API, started (`IC_APID_GLOBAL`). Opaque.
pub struct IcApidGlobal {
  _private: [u8; 0],
}

/// One thread's connection to the cluster (`IC_APID_CONNECTION`).
/// Opaque.
pub struct IcApidConnection {
  _private: [u8; 0],
}

/// A table's description (`IC_TABLE_DEF`). Opaque.
pub struct IcTableDef {
  _private: [u8; 0],
}

/// A unique index's description (`IC_INDEX_DEF`). Opaque.
pub struct IcIndexDef {
  _private: [u8; 0],
}

/// How a row is laid out (`IC_RECORD`). Opaque.
pub struct IcRecord {
  _private: [u8; 0],
}

/// An executor: a thread of the library's that runs requests for any
/// number of the program's threads (`IC_APID_EXECUTOR`). Opaque.
pub struct IcApidExecutor {
  _private: [u8; 0],
}

/// A thread's handle on an executor (`IC_APID_SUBMITTER`). Opaque.
pub struct IcApidSubmitter {
  _private: [u8; 0],
}

/// The shape of a key operation: a table, or a unique index of it, and
/// the records of its rows (`IC_OP_SHAPE`). Opaque.
pub struct IcOpShape {
  _private: [u8; 0],
}

/// A transaction of key operations for an executor, with their rows
/// (`IC_REQUEST`). Opaque.
pub struct IcRequest {
  _private: [u8; 0],
}

/// The shape of a scan for executors: a table, and the record its rows
/// are read into (`IC_SCAN_SHAPE`). Opaque.
pub struct IcScanShape {
  _private: [u8; 0],
}

/// A thread's end of a scan an executor runs, with its row
/// (`IC_SCAN_READER`). Opaque.
pub struct IcScanReader {
  _private: [u8; 0],
}

/// A program being built over a table, its values and conditions
/// named by ids in it (`IC_PROGRAM_BUILDER`). Opaque.
pub struct IcProgramBuilder {
  _private: [u8; 0],
}

/// A compiled program, run by any number of operations, each with its
/// own inputs (`IC_PROGRAM`). Opaque.
pub struct IcProgram {
  _private: [u8; 0],
}

/// A value's id in its program builder (`IC_PROGRAM_VALUE`).
pub type IcProgramValue = u32;

/// A condition's id in its program builder (`IC_PROGRAM_COND`).
pub type IcProgramCond = u32;

/// A query's id in its connection (`IC_QUERY_ID`).
pub type IcQueryId = u32;

/// A transaction's id in its connection (`IC_TRANS_ID`).
pub type IcTransId = u32;

/// A scan's id in its connection (`IC_SCAN_ID`).
pub type IcScanId = u32;

/// A batch of a scan's rows (`IC_SCAN_BATCH_ID`): the scan's id in the
/// high half and its receiver's in the low.
pub type IcScanBatchId = u64;

/// What is called when a batch of a scan is complete
/// (`IC_SCAN_CALLBACK`): on the thread that polls, with the connection,
/// the batch and the `user_ref` the scan was started with.
pub type IcScanCallback = unsafe extern "C" fn(
  conn: *mut IcApidConnection,
  batch: IcScanBatchId,
  user_ref: *mut c_void,
);

/// What is called when a query completes (`IC_APID_CALLBACK`): on the
/// thread that polls, with the connection, the query and the
/// `user_ref` it was defined with.
pub type IcApidCallback = unsafe extern "C" fn(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  user_ref: *mut c_void,
);

/// Choices fixed for the life of a global (`IC_GLOBAL_OPTIONS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcGlobalOptions {
  /// The struct's size, as `ic_global_options_init` was given it.
  pub size: u32,
  /// Receive threads; 1 by default.
  pub receive_threads: u32,
  /// How long a transaction waits for its replies before it ends with
  /// 4012, in milliseconds; 0 takes it from the configuration.
  pub reply_wait_ms: u32,
  /// How long a management server has to answer, in milliseconds.
  pub mgm_timeout_ms: u32,
  /// How many AUTO_INCREMENT values a refill reserves for a table that
  /// does not say for itself; 4096 by default, zero counting as one.
  pub auto_increment_prefetch: u32,
  /// The step between AUTO_INCREMENT values, as MySQL's
  /// `auto_increment_increment`; 1 by default, zero counting as one.
  pub auto_increment_step: u32,
  /// Where AUTO_INCREMENT values start within the step, as MySQL's
  /// `auto_increment_offset`; 1 by default, zero or one above the step
  /// counting as one.
  pub auto_increment_offset: u32,
}

/// Where one field lies in a row (`IC_FIELD_SPEC`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcFieldSpec {
  /// The field, by the id the table gives it.
  pub field_id: u32,
  /// Byte offset of the value in the row.
  pub offset: u32,
  /// Byte offset of the null bit, or `IC_NO_NULL_BIT`.
  pub null_byte_offset: u32,
  /// The null bit within that byte, 0 to 7.
  pub null_bit: u32,
}

/// What a table says of one field (`IC_FIELD_INFO`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcFieldInfo {
  /// The struct's size, as the caller gave it.
  pub size: u32,
  /// The field's name, valid while the table handle lives.
  pub name: *const c_char,
  /// Its id.
  pub field_id: u32,
  /// Its type, an NDB type code.
  pub type_code: u32,
  /// Its precision, for types that have one.
  pub precision: u32,
  /// Its scale, for decimals.
  pub scale: u32,
  /// Its length, as declared.
  pub length: u32,
  /// Its character set number, 0 for types without one.
  pub charset: u32,
  /// How its length is stored: 0 fixed, 1 one length byte, 2 two.
  pub array_kind: u32,
  /// 0 in memory, 1 on disk, 2 the table's default.
  pub storage: u32,
  /// May be NULL.
  pub nullable: bool,
  /// Part of the primary key.
  pub primary_key: bool,
  /// Part of the distribution key.
  pub distribution_key: bool,
  /// A BLOB, TEXT or other blob column, which records leave out.
  pub blob: bool,
  /// The table's AUTO_INCREMENT column, a hidden key included.
  pub auto_increment: bool,
  /// The hidden key, `$PK`, of a table without a primary key: an
  /// ordinary column, which tools listing columns may leave out.
  pub hidden_key: bool,
}

/// What a read asks for (`IC_READ_KEY_ARGS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcReadKeyArgs {
  /// The struct's size, as `ic_read_key_args_init` was given it.
  pub size: u32,
  /// One of the `IC_READ_KIND_*` values.
  pub kind: u32,
  /// One of `IC_ABORT_DEFAULT`, `IC_ABORT_ON_ERROR`, `IC_IGNORE_ERROR`.
  pub abort_option: u32,
  /// Called when the query completes, or NULL for the executed list:
  /// an `IC_APID_CALLBACK`.
  pub callback: Option<
    unsafe extern "C" fn(
      conn: *mut IcApidConnection,
      query: IcQueryId,
      user_ref: *mut c_void,
    ),
  >,
  /// Given back with the completed query.
  pub user_ref: *mut c_void,
}

/// What a write asks for (`IC_WRITE_KEY_ARGS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcWriteKeyArgs {
  /// The struct's size, as `ic_write_key_args_init` was given it.
  pub size: u32,
  /// One of the `IC_WRITE_KIND_*` values.
  pub kind: u32,
  /// One of `IC_ABORT_DEFAULT`, `IC_ABORT_ON_ERROR`, `IC_IGNORE_ERROR`.
  pub abort_option: u32,
  /// One of the `IC_BATCH_HINT_*` values.
  pub batch_hint: u32,
  /// Called when the query completes, or NULL for the executed list:
  /// an `IC_APID_CALLBACK`.
  pub callback: Option<
    unsafe extern "C" fn(
      conn: *mut IcApidConnection,
      query: IcQueryId,
      user_ref: *mut c_void,
    ),
  >,
  /// Given back with the completed query.
  pub user_ref: *mut c_void,
}

/// Choices an executor is started with (`IC_EXECUTOR_OPTIONS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcExecutorOptions {
  /// The struct's size, as `ic_executor_options_init` was given it.
  pub size: u32,
  /// Requests out at most, each a transaction holding a record at its
  /// coordinator; 400 by default.
  pub in_flight: u32,
  /// Requests waiting at most before a submit is refused or waits;
  /// 4 000 by default.
  pub queue: u32,
  /// Scans run at once at most; 64 by default. Added after 0.1: a
  /// program built against 0.1 gets the default.
  pub scans: u32,
}

/// What an executor has done since it started (`IC_EXECUTOR_STATS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcExecutorStats {
  /// The struct's size, as the caller gave it.
  pub size: u32,
  /// Requests taken in.
  pub submitted: u64,
  /// Requests refused: no room, or the executor stopping.
  pub refused: u64,
  /// Requests admitted: their transactions defined and sent.
  pub admitted: u64,
  /// Requests handed back, whatever their outcome.
  pub completed: u64,
  /// Requests failed unsent because their deadline had passed.
  pub expired: u64,
  /// Batches sent; admitted over sends is requests per batch.
  pub sends: u64,
  /// Passes of the executor's loop.
  pub passes: u64,
  /// Scans started, each for a reader. Added after 0.1: left out for a
  /// program built against 0.1.
  pub scans: u64,
}

/// What a takeover of a scanned row asks for (`IC_TAKEOVER_ARGS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcTakeoverArgs {
  /// The struct's size, as `ic_takeover_args_init` was given it.
  pub size: u32,
  /// One of `IC_ABORT_DEFAULT`, `IC_ABORT_ON_ERROR`, `IC_IGNORE_ERROR`;
  /// the default rolls the transaction back if the takeover fails.
  pub abort_option: u32,
  /// Called when the takeover completes, or NULL for the executed
  /// list: an `IC_APID_CALLBACK`.
  pub callback: Option<
    unsafe extern "C" fn(
      conn: *mut IcApidConnection,
      query: IcQueryId,
      user_ref: *mut c_void,
    ),
  >,
  /// Given back with the completed query.
  pub user_ref: *mut c_void,
}

/// What a scan asks for when it is started (`IC_SCAN_ARGS`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcScanArgs {
  /// The struct's size, as `ic_scan_args_init` was given it.
  pub size: u32,
  /// One of the `IC_SCAN_LOCK_*` values.
  pub lock: u32,
  /// For a locking scan whose rows may be taken over, the transaction
  /// they are taken over into; `IC_NO_TRANS` for none.
  pub trans: IcTransId,
  /// Fragments scanned at once; 0 for every fragment.
  pub parallelism: u32,
  /// Rows a batch may hold; 0 for the configuration's.
  pub batch_rows: u32,
  /// Bytes a batch may hold; 0 for the configuration's.
  pub batch_bytes: u32,
  /// Scan this partition only; `IC_NO_PARTITION` for every partition.
  pub partition: u32,
  /// Close the scan with `IC_ERROR_TIMEOUT` if it is still running
  /// this long after it was started, in milliseconds; 0 for no limit.
  pub timeout_ms: u32,
  /// For a committed read through an executor: let a receiver go on as
  /// soon as its batch has moved to the reader.
  pub read_ahead: bool,
  /// Called with each batch as it completes, or NULL for the ready
  /// list: an `IC_SCAN_CALLBACK`.
  pub callback: Option<
    unsafe extern "C" fn(
      conn: *mut IcApidConnection,
      batch: IcScanBatchId,
      user_ref: *mut c_void,
    ),
  >,
  /// Given back to the callback.
  pub user_ref: *mut c_void,
  /// For a scan of an ordered index: the rows in the index's order,
  /// merged from every fragment, read row by row
  /// (`ic_apid_conn_next_row`, `ic_scan_reader_next_row`).
  pub ordered: bool,
  /// For a scan of an ordered index: each range from its high end.
  pub descending: bool,
  /// For a committed read: the continuous scan, four receivers for each
  /// fragment scanned at once, which goes on scanning while the program
  /// has its batches. Not with `read_ahead`.
  pub continuous: bool,
}

/// One range of a scan of an ordered index (`IC_SCAN_RANGE`), its bounds
/// taken from rows laid out by the scan's key record.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcScanRange {
  /// The struct's size, as `ic_scan_range_init` was given it.
  pub size: u32,
  /// How many of the index's leading columns the low bound takes from
  /// its row; 0 for no low bound.
  pub low_columns: u32,
  /// The low bound's own values are in the range.
  pub low_inclusive: bool,
  /// How many of the index's leading columns the high bound takes from
  /// its row; 0 for no high bound.
  pub high_columns: u32,
  /// The high bound's own values are in the range.
  pub high_inclusive: bool,
  /// The range's number, 0 to 4095, given back with each of its rows
  /// when a run has several ranges.
  pub range_no: u32,
}

/// Which node should coordinate a transaction
/// (`IC_TRANSACTION_HINT`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct IcTransactionHint {
  /// One of the `IC_HINT_*` values.
  pub kind: u32,
  /// The node id or the partition, as the kind says.
  pub value: u32,
}

/// What a table handle holds: the description, and its names as C
/// strings, made once at bind.
struct Table {
  def: Arc<TableDef>,
  name: CString,
  field_names: Vec<CString>,
}

/// What an index handle holds.
struct Index {
  def: Arc<IndexDef>,
  name: CString,
}

// ---- Helpers ----

fn c_string(text: &str) -> CString {
  // Names never hold a NUL; one that did would show as empty.
  CString::new(text).unwrap_or_default()
}

fn table_handle(def: Arc<TableDef>) -> *mut IcTableDef {
  let mut field_names: Vec<CString> = Vec::new();
  let mut id: u32 = 0;
  while id < def.num_fields() {
    match def.field(id) {
      Some(attr) => field_names.push(c_string(&attr.name)),
      None => field_names.push(CString::default()),
    }
    id += 1;
  }
  let table = Table {
    name: c_string(def.name()),
    def,
    field_names,
  };
  Box::into_raw(Box::new(table)) as *mut IcTableDef
}

/// The code a result gives C: 0 or the error's.
fn code_of(result: Result<(), IcError>) -> i32 {
  match result {
    Ok(()) => 0,
    Err(e) => e.code,
  }
}

/// Run `f`, turning a panic into `IC_ERROR_INTERNAL_PANIC`.
fn guarded<F: FnOnce() -> i32>(f: F) -> i32 {
  match catch_unwind(AssertUnwindSafe(f)) {
    Ok(code) => code,
    Err(_) => err::IC_ERROR_INTERNAL_PANIC,
  }
}

/// Run `f` on the connection a handle names, with the checks every
/// call on a connection makes: a null handle, a connection a panic went
/// through, and a panic in `f`, which leaves the connection unusable.
/// Gives `f`'s value, or the code of why `f` did not run or finish.
///
/// # Safety
///
/// A non-null `conn` is a connection handle not yet freed, used by one
/// thread at a time.
unsafe fn on_conn<T, F: FnOnce(&mut ApidConnection) -> T>(
  conn: *mut IcApidConnection,
  f: F,
) -> Result<T, i32> {
  let raw = conn as *mut ApidConnection;
  if raw.is_null() {
    return Err(err::IC_ERROR_NULL_POINTER);
  }
  // SAFETY: the caller's promise; the handle is the box
  // `ic_apid_global_create_connection` made.
  let c = unsafe { &mut *raw };
  if c.is_unusable() {
    return Err(err::IC_ERROR_INTERNAL_PANIC);
  }
  match catch_unwind(AssertUnwindSafe(|| f(c))) {
    Ok(value) => Ok(value),
    Err(_) => {
      // SAFETY: as above; the closure, and its borrow, are gone.
      unsafe { (*raw).set_unusable() };
      Err(err::IC_ERROR_INTERNAL_PANIC)
    }
  }
}

/// As [`on_conn`], for a call whose result is all C gets.
///
/// # Safety
///
/// As [`on_conn`].
unsafe fn conn_code<F: FnOnce(&mut ApidConnection) -> Result<(), IcError>>(
  conn: *mut IcApidConnection,
  f: F,
) -> i32 {
  // SAFETY: passed on from the caller.
  match unsafe { on_conn(conn, f) } {
    Ok(result) => code_of(result),
    Err(code) => code,
  }
}

/// A NUL-terminated UTF-8 string from C.
///
/// # Safety
///
/// A non-null `text` points to a NUL-terminated string that outlives
/// the returned reference.
unsafe fn text_arg<'a>(text: *const c_char) -> Result<&'a str, i32> {
  if text.is_null() {
    return Err(err::IC_ERROR_NULL_POINTER);
  }
  // SAFETY: the caller's promise.
  let c_text = unsafe { CStr::from_ptr(text) };
  match c_text.to_str() {
    Ok(text) => Ok(text),
    Err(_) => Err(err::IC_ERROR_BAD_TEXT),
  }
}

/// A row from C, as bytes.
///
/// # Safety
///
/// A non-null `row` points to `len` readable bytes that outlive the
/// returned slice.
unsafe fn row_arg<'a>(row: *const u8, len: usize) -> &'a [u8] {
  if row.is_null() || len == 0 {
    return &[];
  }
  // SAFETY: the caller's promise.
  unsafe { std::slice::from_raw_parts(row, len) }
}

/// A row from C, to be written.
///
/// # Safety
///
/// A non-null `row` points to `len` writable bytes that outlive the
/// returned slice, and nothing else reads or writes them meanwhile.
unsafe fn row_arg_mut<'a>(row: *mut u8, len: usize) -> &'a mut [u8] {
  if row.is_null() || len == 0 {
    return &mut [];
  }
  // SAFETY: the caller's promise.
  unsafe { std::slice::from_raw_parts_mut(row, len) }
}

/// A key row copied aside while the row it came from is written: on
/// the stack up to 256 bytes, else on the heap. The size is in the
/// type, not a constant, as cbindgen warns of a constant not `pub`.
struct KeyCopy {
  inline: [u8; 256],
  heap: Vec<u8>,
}

impl KeyCopy {
  fn new() -> KeyCopy {
    KeyCopy {
      inline: [0; 256],
      heap: Vec::new(),
    }
  }
}

/// True if the `a_len` bytes at `a` and the `b_len` bytes at `b` share
/// any byte.
fn overlaps(a: *const u8, a_len: usize, b: *const u8, b_len: usize) -> bool {
  if a.is_null() || b.is_null() || a_len == 0 || b_len == 0 {
    return false;
  }
  let a_start = a as usize;
  let b_start = b as usize;
  a_start < b_start.saturating_add(b_len)
    && b_start < a_start.saturating_add(a_len)
}

/// The key row of an operation that writes the `out_len` bytes at
/// `out`. C may name one buffer for both, as a read into the row that
/// holds its key does: the key is then copied now, before the row is
/// written, into `copy`, so that no reference to the key's bytes is
/// alive while they are written. The caller's own bytes when the two do
/// not overlap.
///
/// # Safety
///
/// As `row_arg`, for `key` and `key_len`; nothing writes the bytes at
/// `out` before this returns.
unsafe fn key_arg(
  key: *const u8,
  key_len: usize,
  out: *const u8,
  out_len: usize,
  copy: &mut KeyCopy,
) -> &[u8] {
  // SAFETY: the caller's promise; when the key overlaps the row to be
  // written, this reference is used for the copy alone, and ends here.
  let given = unsafe { row_arg(key, key_len) };
  if !overlaps(key, key_len, out, out_len) {
    return given;
  }
  if key_len <= copy.inline.len() {
    copy.inline[..key_len].copy_from_slice(given);
    return &copy.inline[..key_len];
  }
  copy.heap.clear();
  copy.heap.extend_from_slice(given);
  &copy.heap[..]
}

/// The table behind a handle.
///
/// # Safety
///
/// A non-null `table` is a table handle not yet freed.
unsafe fn table_of<'a>(table: *const IcTableDef) -> Option<&'a Table> {
  // SAFETY: the caller's promise.
  unsafe { (table as *const Table).as_ref() }
}

/// The index behind a handle.
///
/// # Safety
///
/// A non-null `index` is an index handle not yet freed.
unsafe fn index_of<'a>(index: *const IcIndexDef) -> Option<&'a Index> {
  // SAFETY: the caller's promise.
  unsafe { (index as *const Index).as_ref() }
}

/// The record behind a handle.
///
/// # Safety
///
/// A non-null `rec` is a record handle not yet freed.
unsafe fn record_of<'a>(rec: *const IcRecord) -> Option<&'a Record> {
  // SAFETY: the caller's promise.
  unsafe { (rec as *const Record).as_ref() }
}

/// The global behind a handle.
///
/// # Safety
///
/// A non-null `global` is a global handle not yet freed.
unsafe fn global_of<'a>(global: *const IcApidGlobal) -> Option<&'a ApidGlobal> {
  // SAFETY: the caller's promise.
  unsafe { (global as *const ApidGlobal).as_ref() }
}

/// The trampoline every C callback goes through: the query's callback
/// as Rust sees it, with the C function in the query's binding word.
fn trampoline(conn: &mut ApidConnection, query: QueryId, user_ref: usize) {
  let binding = match conn.query(query) {
    Some(q) => q.binding(),
    None => return,
  };
  if binding == 0 {
    return;
  }
  // SAFETY: the binding word was set by `ic_apid_conn_read_key` or
  // `ic_apid_conn_write_key` from a non-null `IcApidCallback`, and a
  // function pointer fits a usize and back.
  let callback =
    unsafe { std::mem::transmute::<usize, IcApidCallback>(binding) };
  let handle = conn as *mut ApidConnection as *mut IcApidConnection;
  // SAFETY: the handle is the connection the poll runs on; the callback
  // may call back into the library with it, which reborrows it from
  // this pointer while `conn` waits.
  unsafe { callback(handle, query.as_u32(), user_ref as *mut c_void) };
}

fn hint_to_c(hint: TransactionHint) -> IcTransactionHint {
  match hint {
    TransactionHint::Any => IcTransactionHint {
      kind: IC_HINT_ANY,
      value: 0,
    },
    TransactionHint::Node(node) => IcTransactionHint {
      kind: IC_HINT_NODE,
      value: node,
    },
    TransactionHint::Partition(partition) => IcTransactionHint {
      kind: IC_HINT_PARTITION,
      value: partition,
    },
  }
}

fn hint_from_c(hint: IcTransactionHint) -> TransactionHint {
  match hint.kind {
    IC_HINT_NODE => TransactionHint::Node(hint.value),
    IC_HINT_PARTITION => TransactionHint::Partition(hint.value),
    _ => TransactionHint::Any,
  }
}

fn read_kind_from_c(value: u32) -> Option<ReadKind> {
  match value {
    IC_READ_KIND_SIMPLE => Some(ReadKind::Simple),
    IC_READ_KIND_LOCKED => Some(ReadKind::Locked),
    IC_READ_KIND_EXCLUSIVE => Some(ReadKind::Exclusive),
    IC_READ_KIND_COMMITTED => Some(ReadKind::Committed),
    IC_READ_KIND_EXCLUSIVE_NO_WAIT => Some(ReadKind::ExclusiveNoWait),
    _ => None,
  }
}

fn write_kind_from_c(value: u32) -> Option<WriteKind> {
  match value {
    IC_WRITE_KIND_UPDATE => Some(WriteKind::Update),
    IC_WRITE_KIND_WRITE => Some(WriteKind::Write),
    IC_WRITE_KIND_INSERT => Some(WriteKind::Insert),
    IC_WRITE_KIND_DELETE => Some(WriteKind::Delete),
    _ => None,
  }
}

fn abort_from_c(value: u32) -> Option<AbortOption> {
  match value {
    IC_ABORT_DEFAULT => Some(AbortOption::Default),
    IC_ABORT_ON_ERROR => Some(AbortOption::AbortOnError),
    IC_IGNORE_ERROR => Some(AbortOption::IgnoreError),
    _ => None,
  }
}

fn query_state_to_c(state: QueryState) -> u32 {
  match state {
    QueryState::Idle => IC_QUERY_IDLE,
    QueryState::Defined => IC_QUERY_DEFINED,
    QueryState::Sent => IC_QUERY_SENT,
    QueryState::Completed => IC_QUERY_COMPLETED,
  }
}

fn commit_state_to_c(state: CommitState) -> u32 {
  match state {
    CommitState::Started => IC_COMMIT_STATE_STARTED,
    CommitState::CommitRequested => IC_COMMIT_STATE_COMMIT_REQUESTED,
    CommitState::RollbackRequested => IC_COMMIT_STATE_ROLLBACK_REQUESTED,
    CommitState::Committed => IC_COMMIT_STATE_COMMITTED,
    CommitState::RolledBack => IC_COMMIT_STATE_ROLLED_BACK,
  }
}

/// Copy `text` into a C buffer of `len` bytes, cut short if it must be
/// and always ended with a NUL when there is room for one. Gives the
/// text's whole length, as `snprintf` does.
///
/// # Safety
///
/// A non-null `buf` points to `len` writable bytes.
unsafe fn copy_out(text: &str, buf: *mut c_char, len: usize) -> usize {
  let bytes = text.as_bytes();
  if !buf.is_null() && len > 0 {
    let n = bytes.len().min(len - 1);
    // SAFETY: the caller's promise; n + 1 <= len.
    unsafe {
      std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
      *buf.add(n) = 0;
    }
  }
  bytes.len()
}

// ---- Version and errors ----

/// Report the version of the library.
///
/// Each pointer may be null, in which case that component is not written.
///
/// # Safety
///
/// Non-null pointers must point to writable `uint32_t` storage.
#[no_mangle]
pub unsafe extern "C" fn ic_api_version(
  major: *mut u32,
  minor: *mut u32,
  patch: *mut u32,
) {
  if !major.is_null() {
    // SAFETY: caller guarantees a valid, writable u32.
    unsafe { *major = IC_API_VERSION_MAJOR };
  }
  if !minor.is_null() {
    // SAFETY: as above.
    unsafe { *minor = IC_API_VERSION_MINOR };
  }
  if !patch.is_null() {
    // SAFETY: as above.
    unsafe { *patch = IC_API_VERSION_PATCH };
  }
}

/// What kind of thing an error code is: one of `IC_ERROR_CATEGORY_*`.
#[no_mangle]
pub extern "C" fn ic_error_category(code: i32) -> u32 {
  if code == 0 {
    return IC_ERROR_CATEGORY_NONE;
  }
  IcError::new(code).category() as u32
}

/// How bad an error is: one of `IC_ERROR_SEVERITY_*`.
#[no_mangle]
pub extern "C" fn ic_error_severity(code: i32) -> u32 {
  if code == 0 {
    return IC_ERROR_SEVERITY_NONE;
  }
  IcError::new(code).severity() as u32
}

/// True if the same request may well succeed if tried again, as after
/// a node failure or an overload. An unknown outcome, 4012 among them,
/// counts too, but the transaction may have committed: a write is tried
/// again only once it is known not to have, or when doing it twice does
/// no harm.
#[no_mangle]
pub extern "C" fn ic_error_is_temporary(code: i32) -> bool {
  code != 0 && IcError::new(code).is_temporary()
}

/// Copy the text of an error into `buf`, `len` bytes, ended with a NUL.
/// Gives the text's whole length; a longer text is cut short.
///
/// # Safety
///
/// A non-null `buf` points to `len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_error_message(
  code: i32,
  buf: *mut c_char,
  len: usize,
) -> usize {
  let text = catch_unwind(|| IcError::new(code).message()).unwrap_or_default();
  // SAFETY: passed on from the caller.
  unsafe { copy_out(&text, buf, len) }
}

/// What the data nodes call a type (`IC_FIELD_INFO.type_code`), for
/// printing: `"Unsigned"`, `"Varchar"` and so on. A static string, never
/// NULL.
#[no_mangle]
pub extern "C" fn ic_field_type_name(type_code: u32) -> *const c_char {
  // Kept in here, out of the header generator's sight.
  /// Type codes with a name, 0 up to this.
  const IC_TYPE_CODES: u32 = 64;
  /// The type names as C strings, made on first use.
  static TYPE_NAMES: OnceLock<Vec<CString>> = OnceLock::new();
  let names = TYPE_NAMES.get_or_init(|| {
    let mut names: Vec<CString> = Vec::with_capacity(IC_TYPE_CODES as usize);
    let mut code: u32 = 0;
    while code < IC_TYPE_CODES {
      names.push(c_string(table_props::type_name(code)));
      code += 1;
    }
    names
  });
  match names.get(type_code as usize) {
    Some(name) => name.as_ptr(),
    None => c"Undefined".as_ptr(),
  }
}

// ---- The global ----

/// Bytes of `IC_GLOBAL_OPTIONS` in release 0.1, before the
/// AUTO_INCREMENT fields.
fn global_options_0_1() -> usize {
  std::mem::offset_of!(IcGlobalOptions, auto_increment_prefetch)
}

/// Fill `options`, `size` bytes, with the defaults: the fields that fit,
/// so that a program built against an older header gets those it knows.
///
/// Leaves `options` as it is when `size` is smaller than release 0.1's
/// `IC_GLOBAL_OPTIONS`.
///
/// # Safety
///
/// A non-null `options` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_global_options_init(
  options: *mut IcGlobalOptions,
  size: usize,
) {
  if options.is_null() || size < global_options_0_1() {
    return;
  }
  let defaults = GlobalOptions::default();
  let fields: [(usize, u32); 7] = [
    (std::mem::offset_of!(IcGlobalOptions, size), size as u32),
    (
      std::mem::offset_of!(IcGlobalOptions, receive_threads),
      defaults.receive_threads,
    ),
    (
      std::mem::offset_of!(IcGlobalOptions, reply_wait_ms),
      defaults.reply_wait_ms,
    ),
    (
      std::mem::offset_of!(IcGlobalOptions, mgm_timeout_ms),
      defaults.mgm_timeout_ms,
    ),
    (
      std::mem::offset_of!(IcGlobalOptions, auto_increment_prefetch),
      defaults.auto_increment_prefetch,
    ),
    (
      std::mem::offset_of!(IcGlobalOptions, auto_increment_step),
      defaults.auto_increment_step,
    ),
    (
      std::mem::offset_of!(IcGlobalOptions, auto_increment_offset),
      defaults.auto_increment_offset,
    ),
  ];
  let base = options as *mut u8;
  for (offset, value) in fields {
    // SAFETY: the caller's promise; each field is written only if it
    // lies within `size` bytes.
    unsafe { put_sized(base, size, offset, value) };
  }
}

/// Start the Data API: ask a management server in `connect_string` for
/// a node id and the configuration, and start the threads that keep us
/// in the cluster. Returns at once; the data nodes are connected in the
/// background (see `ic_apid_global_wait_for_started`). `node_name` and
/// `options` may be NULL.
///
/// Errors: `IC_ERROR_PARSE_CONNECTSTRING` for text that is not a connectstring;
/// `IC_ERROR_CONNECT_TIMEOUT` or an operating system error when no management
/// server answers; `IC_ERROR_NODEID_IN_USE` when the node id asked for is held;
/// `IC_ERROR_MGM_SERVER_REFUSED` when no API slot is free or the node id is not
/// allowed; `IC_ERROR_INCONSISTENT_DATA` for `options` not filled by
/// `ic_global_options_init`; `IC_ERROR_BAD_TEXT` for text that is not UTF-8.
/// The global is the caller's, freed with `ic_apid_global_free`. It waits for
/// the management server, up to `mgm_timeout_ms`.
///
/// # Safety
///
/// `connect_string` and a non-null `node_name` are NUL-terminated
/// strings; a non-null `options` was filled by `ic_global_options_init`;
/// `global` points to writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_global_connect(
  connect_string: *const c_char,
  node_name: *const c_char,
  options: *const IcGlobalOptions,
  global: *mut *mut IcApidGlobal,
) -> i32 {
  if global.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let text = match unsafe { text_arg(connect_string) } {
    Ok(text) => text,
    Err(code) => return code,
  };
  let mut name: Option<&str> = None;
  if !node_name.is_null() {
    // SAFETY: the caller's promise.
    match unsafe { text_arg(node_name) } {
      Ok(given) => name = Some(given),
      Err(code) => return code,
    }
  }
  let mut opts = GlobalOptions::default();
  if !options.is_null() {
    let base = options as *const u8;
    // SAFETY: the caller's promise: the struct was filled by
    // `ic_global_options_init`, which wrote its size first.
    let size = unsafe { get_sized::<u32>(base, 4, 0) }.unwrap_or(0) as usize;
    if size < global_options_0_1() {
      return err::IC_ERROR_INCONSISTENT_DATA;
    }
    let fields: [(usize, &mut u32); 6] = [
      (
        std::mem::offset_of!(IcGlobalOptions, receive_threads),
        &mut opts.receive_threads,
      ),
      (
        std::mem::offset_of!(IcGlobalOptions, reply_wait_ms),
        &mut opts.reply_wait_ms,
      ),
      (
        std::mem::offset_of!(IcGlobalOptions, mgm_timeout_ms),
        &mut opts.mgm_timeout_ms,
      ),
      (
        std::mem::offset_of!(IcGlobalOptions, auto_increment_prefetch),
        &mut opts.auto_increment_prefetch,
      ),
      (
        std::mem::offset_of!(IcGlobalOptions, auto_increment_step),
        &mut opts.auto_increment_step,
      ),
      (
        std::mem::offset_of!(IcGlobalOptions, auto_increment_offset),
        &mut opts.auto_increment_offset,
      ),
    ];
    for (offset, field) in fields {
      // SAFETY: as above; each field is read only if that size holds it.
      if let Some(value) = unsafe { get_sized::<u32>(base, size, offset) } {
        *field = value;
      }
    }
  }
  guarded(|| match ApidGlobal::connect(text, name, &opts) {
    Ok(started) => {
      let handle = Box::into_raw(Box::new(started)) as *mut IcApidGlobal;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { global.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// Wait up to `wait_ms` for the first data node to be started, or with
/// `all` for every one, and give how many are started.
///
/// Returns 0 whether or not a node started; `started` says how many did. It
/// blocks the calling thread up to `wait_ms`.
///
/// # Safety
///
/// `global` is a global handle not yet freed; a non-null `started`
/// points to a writable `uint32_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_global_wait_for_started(
  global: *mut IcApidGlobal,
  wait_ms: u32,
  all: bool,
  started: *mut u32,
) -> i32 {
  // SAFETY: the caller's promise.
  let g = match unsafe { global_of(global) } {
    Some(g) => g,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    let count = if all {
      g.wait_for_all_started(wait_ms)
    } else {
      g.wait_for_first_started(wait_ms)
    };
    if !started.is_null() {
      // SAFETY: the caller's promise.
      unsafe { started.write(count) };
    }
    0
  })
}

/// Our node id in the cluster; 0 for a null handle.
///
/// # Safety
///
/// A non-null `global` is a global handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_global_node_id(
  global: *const IcApidGlobal,
) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { global_of(global) } {
    Some(g) => g.own_node_id(),
    None => 0,
  }
}

/// Stop every thread of the Data API, wait for each, and free the
/// global. Its connections must be freed first.
///
/// No other thread may use the global meanwhile, and every connection of it
/// must be freed already.
///
/// # Safety
///
/// A non-null `global` is a global handle not yet freed, freed here;
/// no other thread uses it meanwhile.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_global_free(global: *mut IcApidGlobal) {
  if global.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box connect made.
  let owned = unsafe { Box::from_raw(global as *mut ApidGlobal) };
  // Dropping the global stops its threads and waits for each.
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// A connection for the calling thread's use.
///
/// Errors: `IC_ERROR_TOO_MANY_USER_THREADS` when the process has 4 711
/// connections. Any thread may call it, at the same time as others. The
/// connection is the caller's, freed with `ic_apid_conn_free` before the
/// global; one thread uses it at a time.
///
/// # Safety
///
/// `global` is a global handle not yet freed; `conn` points to writable
/// storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_global_create_connection(
  global: *mut IcApidGlobal,
  conn: *mut *mut IcApidConnection,
) -> i32 {
  if conn.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let g = match unsafe { global_of(global) } {
    Some(g) => g,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| match g.create_connection() {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcApidConnection;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { conn.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// Free a connection, its queries and transactions with it.
///
/// Its queries and transactions go with it, and every row pointer they gave; a
/// transaction left open does not commit. It first waits for the answers to
/// the coordinator records it has asked for, so as to give them back: well
/// under a millisecond as a rule, 5 seconds at most, and no callback runs.
///
/// # Safety
///
/// A non-null `conn` is a connection handle not yet freed, freed here,
/// and not inside one of its own callbacks.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_free(conn: *mut IcApidConnection) {
  if conn.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(conn as *mut ApidConnection) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

// ---- Tables and indexes ----

/// Bind a table of database `db`, giving a handle to its description.
///
/// Errors: `IC_ERROR_NO_SUCH_TABLE` when the database has no such table;
/// `IC_ERROR_TABLE_KEEPS_CHANGING` when it changed every time it was fetched;
/// `IC_ERROR_DICT_REFUSED` when the data node's dictionary refused;
/// `IC_ERROR_BAD_TEXT`; `IC_ERROR_NOT_SUPPORTED` from a callback run inside
/// a bind of this thread, for a table whose description is being fetched,
/// which it would wait for for ever. It waits for the data node when the
/// description is not cached yet. The handle is the caller's, freed with
/// `ic_table_def_free`; it may outlive the connection and be used by any
/// thread.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `db` and `table` are
/// NUL-terminated strings; `def` points to writable storage for a
/// pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_table_bind(
  conn: *mut IcApidConnection,
  db: *const c_char,
  table: *const c_char,
  def: *mut *mut IcTableDef,
) -> i32 {
  if def.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let (db, table) = match unsafe { (text_arg(db), text_arg(table)) } {
    (Ok(db), Ok(table)) => (db, table),
    (Err(code), _) | (_, Err(code)) => return code,
  };
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.table_bind(db, table)) } {
    Ok(Ok(bound)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { def.write(table_handle(bound)) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Bind a unique index of a table, by the index's name (a MySQL server
/// names a unique index `name$unique`).
///
/// Errors: as `ic_apid_conn_table_bind`; `IC_ERROR_NO_SUCH_INDEX` when the
/// table has no such index; `IC_ERROR_INDEX_NOT_ONLINE` when it is being built,
/// being dropped, or broken. The handle is the caller's, freed with
/// `ic_index_def_free`; it may outlive the connection and be used by any
/// thread.
///
/// # Safety
///
/// As `ic_apid_conn_table_bind`, with `index` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_index_bind(
  conn: *mut IcApidConnection,
  db: *const c_char,
  index: *const c_char,
  table: *const c_char,
  def: *mut *mut IcIndexDef,
) -> i32 {
  if def.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let texts = unsafe { (text_arg(db), text_arg(index), text_arg(table)) };
  let (db, index, table) = match texts {
    (Ok(db), Ok(index), Ok(table)) => (db, index, table),
    (Err(code), _, _) | (_, Err(code), _) | (_, _, Err(code)) => return code,
  };
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.index_bind(db, index, table)) } {
    Ok(Ok(bound)) => {
      let handle = Index {
        name: c_string(bound.name()),
        def: bound,
      };
      let raw = Box::into_raw(Box::new(handle)) as *mut IcIndexDef;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { def.write(raw) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Let go of a table's description at once, so that the next bind
/// fetches it again. Handles to it stay valid.
///
/// # Safety
///
/// `conn` is a connection handle and `def` a table handle, neither yet
/// freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_table_unbind(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| {
      c.table_unbind(&table.def);
      Ok(())
    })
  }
}

/// Free a table handle.
///
/// The names it gave go with it.
///
/// # Safety
///
/// A non-null `def` is a table handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_table_def_free(def: *mut IcTableDef) {
  if def.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(def as *mut Table) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// The table's name, valid until the handle is freed; NULL for a null
/// handle.
///
/// # Safety
///
/// A non-null `def` is a table handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_table_def_name(
  def: *const IcTableDef,
) -> *const c_char {
  // SAFETY: the caller's promise.
  match unsafe { table_of(def) } {
    Some(table) => table.name.as_ptr(),
    None => std::ptr::null(),
  }
}

/// How many fields the table has; their ids are 0 up to that.
///
/// # Safety
///
/// A non-null `def` is a table handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_table_def_num_fields(
  def: *const IcTableDef,
) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { table_of(def) } {
    Some(table) => table.def.num_fields(),
    None => 0,
  }
}

/// The id of the field named `name`.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD`; `IC_ERROR_BAD_TEXT`.
///
/// # Safety
///
/// `def` is a table handle not yet freed, `name` a NUL-terminated
/// string, `field_id` points to a writable `uint32_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_table_def_field_id(
  def: *const IcTableDef,
  name: *const c_char,
  field_id: *mut u32,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if field_id.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let name = match unsafe { text_arg(name) } {
    Ok(name) => name,
    Err(code) => return code,
  };
  guarded(|| match table.def.field_id(name) {
    Ok(id) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { field_id.write(id) };
      0
    }
    Err(e) => e.code,
  })
}

/// What the table says of field `field_id`, into `info`, `size` bytes.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD`; `IC_ERROR_INCONSISTENT_DATA` when `size`
/// is smaller than this header's `IC_FIELD_INFO`. The name in `info` lives as
/// long as the table handle. `auto_increment` and `hidden_key` came after
/// 0.1, in what was the struct's padding, so its size is 0.1's.
///
/// # Safety
///
/// `def` is a table handle not yet freed; `info` points to `size`
/// writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_table_def_field(
  def: *const IcTableDef,
  field_id: u32,
  info: *mut IcFieldInfo,
  size: usize,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if info.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  if size < std::mem::size_of::<IcFieldInfo>() {
    return err::IC_ERROR_INCONSISTENT_DATA;
  }
  let attr = match table.def.field(field_id) {
    Some(attr) => attr,
    None => return err::IC_ERROR_NO_SUCH_FIELD,
  };
  let name = match table.field_names.get(field_id as usize) {
    Some(name) => name.as_ptr(),
    None => std::ptr::null(),
  };
  let value = IcFieldInfo {
    size: size as u32,
    name,
    field_id,
    type_code: attr.type_code,
    precision: attr.precision,
    scale: attr.scale,
    length: attr.length,
    charset: attr.charset,
    array_kind: attr.array_kind,
    storage: attr.storage_kind,
    nullable: attr.nullable,
    primary_key: attr.primary_key,
    distribution_key: attr.distribution_key,
    blob: attr.is_blob(),
    auto_increment: table.def.auto_increment_column() == Some(field_id),
    hidden_key: table.def.hidden_key() == Some(field_id),
  };
  // SAFETY: checked non-null and large enough above.
  unsafe { info.write(value) };
  0
}

/// Free an index handle.
///
/// The name it gave goes with it.
///
/// # Safety
///
/// A non-null `def` is an index handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_index_def_free(def: *mut IcIndexDef) {
  if def.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(def as *mut Index) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// The index's name, valid until the handle is freed.
///
/// # Safety
///
/// A non-null `def` is an index handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_index_def_name(
  def: *const IcIndexDef,
) -> *const c_char {
  // SAFETY: the caller's promise.
  match unsafe { index_of(def) } {
    Some(index) => index.name.as_ptr(),
    None => std::ptr::null(),
  }
}

/// A handle to the index's own table, whose fields are the indexed
/// columns, to be freed with `ic_table_def_free`.
///
/// # Safety
///
/// `def` is an index handle not yet freed; `table` points to writable
/// storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_index_def_table(
  def: *const IcIndexDef,
  table: *mut *mut IcTableDef,
) -> i32 {
  // SAFETY: the caller's promise.
  let index = match unsafe { index_of(def) } {
    Some(index) => index,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if table.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let handle = table_handle(Arc::clone(index.def.as_table()));
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { table.write(handle) };
    0
  })
}

// ---- Records ----

/// A record as the application lays out its row: one `IC_FIELD_SPEC`
/// per field, and the row's size.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD` for a field the table does not have;
/// `IC_ERROR_DUPLICATE_FIELD_IDS` for a field named twice;
/// `IC_ERROR_TOO_MANY_FIELDS`; `IC_ERROR_RECORD_LAYOUT` when values or null
/// bits overlap or lie outside the row; `IC_ERROR_NO_NULL_BIT` for a nullable
/// field given no null bit; `IC_ERROR_NOT_SUPPORTED` for a blob column. The
/// record is the caller's, freed with `ic_record_free`; any thread may use it.
///
/// # Safety
///
/// `def` is a table handle not yet freed; `specs` points to `count`
/// readable specs; `rec` points to writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_record_create(
  def: *const IcTableDef,
  specs: *const IcFieldSpec,
  count: u32,
  row_size: u32,
  rec: *mut *mut IcRecord,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if rec.is_null() || (specs.is_null() && count > 0) {
    return err::IC_ERROR_NULL_POINTER;
  }
  let mut given: &[IcFieldSpec] = &[];
  if count > 0 {
    // SAFETY: the caller's promise.
    given = unsafe { std::slice::from_raw_parts(specs, count as usize) };
  }
  guarded(|| {
    let mut spec: Vec<FieldSpec> = Vec::with_capacity(given.len());
    for one in given {
      spec.push(FieldSpec {
        field_id: one.field_id,
        offset: one.offset,
        null_byte_offset: one.null_byte_offset,
        null_bit: one.null_bit,
      });
    }
    match Record::new(&table.def, &spec, row_size) {
      Ok(made) => {
        let handle = Box::into_raw(Box::new(made)) as *mut IcRecord;
        // SAFETY: checked non-null above; the caller's promise.
        unsafe { rec.write(handle) };
        0
      }
      Err(e) => e.code,
    }
  })
}

/// A record laid out by the library over the fields named, or over
/// every field but the blob columns when `field_ids` is NULL or `count`
/// is 0; `ic_record_field` tells where each field went.
///
/// Errors: as `ic_record_create`. The record is the caller's, freed with
/// `ic_record_free`; any thread may use it.
///
/// # Safety
///
/// `def` is a table handle not yet freed; a non-null `field_ids` points
/// to `count` readable ids; `rec` points to writable storage for a
/// pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_record_create_default(
  def: *const IcTableDef,
  field_ids: *const u32,
  count: u32,
  rec: *mut *mut IcRecord,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if rec.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  let mut ids: Option<&[u32]> = None;
  if !field_ids.is_null() && count > 0 {
    // SAFETY: the caller's promise.
    ids =
      Some(unsafe { std::slice::from_raw_parts(field_ids, count as usize) });
  }
  guarded(|| match Record::default_for(&table.def, ids) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcRecord;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { rec.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// The size of a row the record lays out; 0 for a null handle.
///
/// # Safety
///
/// A non-null `rec` is a record handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_record_row_size(rec: *const IcRecord) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { record_of(rec) } {
    Some(record) => record.row_size(),
    None => 0,
  }
}

/// Where field `field_id` lies in the record's row: its offset and its
/// size in bytes, length bytes included. Either pointer may be NULL.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD` when the record does not hold the field.
///
/// # Safety
///
/// `rec` is a record handle not yet freed; non-null `offset` and `size`
/// point to writable `uint32_t`s.
#[no_mangle]
pub unsafe extern "C" fn ic_record_field(
  rec: *const IcRecord,
  field_id: u32,
  offset: *mut u32,
  size: *mut u32,
) -> i32 {
  // SAFETY: the caller's promise.
  let record = match unsafe { record_of(rec) } {
    Some(record) => record,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  let field = match record.position_of(field_id) {
    Some(position) => record.field(position),
    None => None,
  };
  let field = match field {
    Some(field) => field,
    None => return err::IC_ERROR_NO_SUCH_FIELD,
  };
  if !offset.is_null() {
    // SAFETY: the caller's promise.
    unsafe { offset.write(field.offset()) };
  }
  if !size.is_null() {
    // SAFETY: the caller's promise.
    unsafe { size.write(field.size()) };
  }
  0
}

/// True if field `field_id` is NULL in the row, `len` bytes.
///
/// False too for a field the record does not hold, or a row too short to hold
/// its null bit.
///
/// # Safety
///
/// `rec` is a record handle not yet freed; a non-null `row` points to
/// `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_record_is_null(
  rec: *const IcRecord,
  row: *const u8,
  len: usize,
  field_id: u32,
) -> bool {
  // SAFETY: the caller's promise.
  let record = match unsafe { record_of(rec) } {
    Some(record) => record,
    None => return false,
  };
  let position = match record.position_of(field_id) {
    Some(position) => position,
    None => return false,
  };
  // SAFETY: the caller's promise.
  let bytes = unsafe { row_arg(row, len) };
  record.is_null(bytes, position)
}

/// Set or clear the null bit of field `field_id` in the row.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD`; `IC_ERROR_NO_NULL_BIT` for a field that
/// cannot be NULL; `IC_ERROR_RECORD_LAYOUT` for a row too short.
///
/// # Safety
///
/// `rec` is a record handle not yet freed; `row` points to `len`
/// writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_record_set_null(
  rec: *const IcRecord,
  row: *mut u8,
  len: usize,
  field_id: u32,
  is_null: bool,
) -> i32 {
  // SAFETY: the caller's promise.
  let record = match unsafe { record_of(rec) } {
    Some(record) => record,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  let position = match record.position_of(field_id) {
    Some(position) => position,
    None => return err::IC_ERROR_NO_SUCH_FIELD,
  };
  // SAFETY: the caller's promise.
  let bytes = unsafe { row_arg_mut(row, len) };
  guarded(|| code_of(record.set_null(bytes, position, is_null)))
}

/// Put a value given as text into field `field_id` of the row: a
/// number; a character string, unquoted, converted to its column's
/// character set (utf8mb4, utf8mb3, utf16, utf16le, ucs2, utf32 and
/// latin1), or for another set its bytes as `0x` and hexadecimal
/// digits; a binary string as the text's own bytes; or `NULL`. The
/// temporal types and decimals cannot be given as text; a program puts
/// their bytes into the row itself. For tools and examples: an
/// application works in the row's own bytes.
///
/// Errors: `IC_ERROR_BAD_TEXT` for text that is not UTF-8, does not fit the
/// field, or has a character the column's set cannot hold;
/// `IC_ERROR_NO_SUCH_FIELD`; `IC_ERROR_RECORD_LAYOUT` when `len` is less than
/// the record's row size.
///
/// # Safety
///
/// `rec` is a record handle not yet freed; `text` a NUL-terminated
/// string; `row` points to `len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_record_put_text(
  rec: *const IcRecord,
  field_id: u32,
  text: *const c_char,
  row: *mut u8,
  len: usize,
) -> i32 {
  // SAFETY: the caller's promise.
  let record = match unsafe { record_of(rec) } {
    Some(record) => record,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  if len < record.row_size() as usize {
    return err::IC_ERROR_RECORD_LAYOUT;
  }
  // SAFETY: the caller's promise.
  let text = match unsafe { text_arg(text) } {
    Ok(text) => text,
    Err(code) => return code,
  };
  let position = match record.position_of(field_id) {
    Some(position) => position,
    None => return err::IC_ERROR_NO_SUCH_FIELD,
  };
  // SAFETY: the caller's promise.
  let bytes = unsafe { row_arg_mut(row, len) };
  guarded(
    || match text_row::put_value(record, position, text, bytes) {
      Ok(()) => 0,
      Err(_) => err::IC_ERROR_BAD_TEXT,
    },
  )
}

/// Give field `field_id` of the row as text, into `buf`, `buf_len`
/// bytes, ended with a NUL: a number, a character string in single
/// quotes and UTF-8, a CHAR without its padding, or `NULL`. Binary
/// strings, and character strings of a set not converted here, are
/// given as `0x` and hexadecimal digits. The text's whole length is
/// returned, and a longer text is cut short.
///
/// 0, with nothing written, for a field the record does not hold or a row
/// shorter than the record's.
///
/// # Safety
///
/// `rec` is a record handle not yet freed; `row` points to `len`
/// readable bytes; a non-null `buf` to `buf_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_record_get_text(
  rec: *const IcRecord,
  field_id: u32,
  row: *const u8,
  len: usize,
  buf: *mut c_char,
  buf_len: usize,
) -> usize {
  // SAFETY: the caller's promise.
  let record = match unsafe { record_of(rec) } {
    Some(record) => record,
    None => return 0,
  };
  if len < record.row_size() as usize {
    return 0;
  }
  let position = match record.position_of(field_id) {
    Some(position) => position,
    None => return 0,
  };
  // SAFETY: the caller's promise.
  let bytes = unsafe { row_arg(row, len) };
  let text = catch_unwind(AssertUnwindSafe(|| {
    text_row::value_text(record, position, bytes)
  }))
  .unwrap_or_default();
  // SAFETY: passed on from the caller.
  unsafe { copy_out(&text, buf, buf_len) }
}

/// Free a record handle. Queries made with it keep their own copy.
///
/// # Safety
///
/// A non-null `rec` is a record handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_record_free(rec: *mut IcRecord) {
  if rec.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(rec as *mut Record) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

// ---- Queries ----

/// A query over a table, with a record for its key and one for the
/// values it reads or writes. It owns its rows, sized by the records.
///
/// Errors: `IC_ERROR_KEY_RECORD` when the key record does not hold every
/// primary key field, or a record is over another table or another version of
/// it. The query lives in the connection until `ic_apid_query_free` or the
/// connection's free.
///
/// # Safety
///
/// `conn`, `def`, `key_rec` and `attr_rec` are handles not yet freed;
/// `query` points to a writable `IC_QUERY_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_create(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  key_rec: *const IcRecord,
  attr_rec: *const IcRecord,
  query: *mut IcQueryId,
) -> i32 {
  // SAFETY: the caller's promise.
  let (table, key, attr) =
    match unsafe { (table_of(def), record_of(key_rec), record_of(attr_rec)) } {
      (Some(table), Some(key), Some(attr)) => (table, key, attr),
      _ => return err::IC_ERROR_NULL_POINTER,
    };
  if query.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.create_query(&table.def, key, attr)) } {
    Ok(Ok(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { query.write(id.as_u32()) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// A query through a unique index: the key record holds the indexed
/// columns.
///
/// Errors: `IC_ERROR_KEY_RECORD` when a record is over another table than the
/// index's, or another version of it; `IC_ERROR_NOT_SUPPORTED` for an index
/// that is not unique. The query lives in the connection until
/// `ic_apid_query_free` or the connection's free.
///
/// # Safety
///
/// As `ic_apid_query_create`, with `index` an index handle.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_create_unique(
  conn: *mut IcApidConnection,
  index: *const IcIndexDef,
  key_rec: *const IcRecord,
  attr_rec: *const IcRecord,
  query: *mut IcQueryId,
) -> i32 {
  // SAFETY: the caller's promise.
  let (index, key, attr) =
    match unsafe { (index_of(index), record_of(key_rec), record_of(attr_rec)) }
    {
      (Some(index), Some(key), Some(attr)) => (index, key, attr),
      _ => return err::IC_ERROR_NULL_POINTER,
    };
  if query.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  match unsafe {
    on_conn(conn, |c| c.create_unique_query(&index.def, key, attr))
  } {
    Ok(Ok(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { query.write(id.as_u32()) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Free a query that is idle or completed.
///
/// Errors: `IC_ERROR_TRANSACTION_ACTIVE` while the query is defined or sent;
/// `IC_ERROR_NO_SUCH_FIELD` for an id that names no query. Its row pointers are
/// invalid from here.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_free(
  conn: *mut IcApidConnection,
  query: IcQueryId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.free_query(QueryId::from_u32(query))) }
}

/// The query's key row and its length: filled by the application before
/// the query is defined. Valid until the query is freed; NULL for an id
/// that names no query.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `len` points
/// to a writable `size_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_key_row(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  len: *mut usize,
) -> *mut u8 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query_mut(QueryId::from_u32(query)) {
      Some(q) => {
        let row = q.key_row_mut();
        (row.as_mut_ptr(), row.len())
      }
      None => (std::ptr::null_mut(), 0),
    })
  };
  let (row, row_len) = found.unwrap_or((std::ptr::null_mut(), 0));
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// The query's attribute row and its length: the values of a write,
/// filled before the query is defined, or the row a read brought, once
/// the query has completed. Valid until the query is freed; NULL for an
/// id that names no query.
///
/// # Safety
///
/// As `ic_apid_query_key_row`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_attr_row(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  len: *mut usize,
) -> *mut u8 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query_mut(QueryId::from_u32(query)) {
      Some(q) => {
        let row = q.attr_row_mut();
        (row.as_mut_ptr(), row.len())
      }
      None => (std::ptr::null_mut(), 0),
    })
  };
  let (row, row_len) = found.unwrap_or((std::ptr::null_mut(), 0));
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// Where the query is: one of the `IC_QUERY_*` values.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_state(
  conn: *mut IcApidConnection,
  query: IcQueryId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query(QueryId::from_u32(query)) {
      Some(q) => query_state_to_c(q.state()),
      None => IC_QUERY_UNKNOWN,
    })
  };
  found.unwrap_or(IC_QUERY_UNKNOWN)
}

/// The completed query's error code, or 0 if it succeeded.
///
/// `IC_ERROR_NO_SUCH_FIELD` for an id that names no query.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_error(
  conn: *mut IcApidConnection,
  query: IcQueryId,
) -> i32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query(QueryId::from_u32(query)) {
      Some(q) => match q.error() {
        Some(e) => e.code,
        None => 0,
      },
      None => err::IC_ERROR_NO_SUCH_FIELD,
    })
  };
  match found {
    Ok(code) => code,
    Err(code) => code,
  }
}

/// What the query's refusal said beyond its code, such as the index a
/// duplicate key names (893); 0 if nothing.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_error_data(
  conn: *mut IcApidConnection,
  query: IcQueryId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query(QueryId::from_u32(query)) {
      Some(q) => q.error_data(),
      None => 0,
    })
  };
  found.unwrap_or(0)
}

/// The `user_ref` the query was defined with.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_user_ref(
  conn: *mut IcApidConnection,
  query: IcQueryId,
) -> *mut c_void {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query(QueryId::from_u32(query)) {
      Some(q) => q.user_ref(),
      None => 0,
    })
  };
  found.unwrap_or(0) as *mut c_void
}

/// Fill `args`, `size` bytes, with the defaults: a simple read, the
/// default abort option, no callback.
///
/// When `size` is smaller than this header's `IC_READ_KEY_ARGS`, as for a
/// program built against an older header, only the size is written
/// into `args`, so that a call given it sees how short it is and
/// refuses it, without reading past it.
///
/// # Safety
///
/// A non-null `args` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_read_key_args_init(
  args: *mut IcReadKeyArgs,
  size: usize,
) {
  if args.is_null() {
    return;
  }
  if size < std::mem::size_of::<IcReadKeyArgs>() {
    // SAFETY: the caller's promise.
    unsafe { mark_short(args.cast::<u8>(), size) };
    return;
  }
  let value = IcReadKeyArgs {
    size: size as u32,
    kind: IC_READ_KIND_SIMPLE,
    abort_option: IC_ABORT_DEFAULT,
    callback: None,
    user_ref: std::ptr::null_mut(),
  };
  // SAFETY: the caller's promise; the struct fits.
  unsafe { args.write(value) };
}

/// Fill `args`, `size` bytes, with the defaults: an update, the default
/// abort option, no batching hint, no callback.
///
/// When `size` is smaller than this header's `IC_WRITE_KEY_ARGS`, as for a
/// program built against an older header, only the size is written
/// into `args`, so that a call given it sees how short it is and
/// refuses it, without reading past it.
///
/// # Safety
///
/// A non-null `args` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_write_key_args_init(
  args: *mut IcWriteKeyArgs,
  size: usize,
) {
  if args.is_null() {
    return;
  }
  if size < std::mem::size_of::<IcWriteKeyArgs>() {
    // SAFETY: the caller's promise.
    unsafe { mark_short(args.cast::<u8>(), size) };
    return;
  }
  let value = IcWriteKeyArgs {
    size: size as u32,
    kind: IC_WRITE_KIND_UPDATE,
    abort_option: IC_ABORT_DEFAULT,
    batch_hint: IC_BATCH_HINT_NONE,
    callback: None,
    user_ref: std::ptr::null_mut(),
  };
  // SAFETY: the caller's promise; the struct fits.
  unsafe { args.write(value) };
}

/// Define a read by key on a transaction; the query's key row holds the
/// key. It goes with the next send.
///
/// Errors: `IC_ERROR_TRANSACTION_ACTIVE` when the query is already defined or
/// sent, or the transaction is ending, ended, or named by no id;
/// `IC_ERROR_NO_SUCH_FIELD` for an id that names no query;
/// `IC_ERROR_NOT_SUPPORTED` for a kind or abort option no define names;
/// `IC_ERROR_INCONSISTENT_DATA` for arguments not filled by
/// `ic_read_key_args_init`; `IC_NDB_ERROR_NULL_KEY` for a NULL in the key;
/// `IC_ERROR_KEY_TOO_LONG`. From here until the query completes its rows are
/// the library's.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `args` was filled by
/// `ic_read_key_args_init`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_read_key(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  trans: IcTransId,
  args: *const IcReadKeyArgs,
) -> i32 {
  // SAFETY: the caller's promise.
  let given = match unsafe { sized_from_c(args) } {
    Ok(given) => given,
    Err(code) => return code,
  };
  let kind = match read_kind_from_c(given.kind) {
    Some(kind) => kind,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  let abort_option = match abort_from_c(given.abort_option) {
    Some(option) => option,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  let mut rust_args = ReadKeyArgs {
    kind,
    abort_option,
    user_ref: given.user_ref as usize,
    ..ReadKeyArgs::default()
  };
  if let Some(callback) = given.callback {
    rust_args.callback = Some(trampoline);
    rust_args.binding = callback as usize;
  }
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| {
      c.read_key(
        QueryId::from_u32(query),
        TransId::from_u32(trans),
        &rust_args,
      )
    })
  }
}

/// Define a write by key on a transaction; the query's key row holds the
/// key and its attribute row the values. It goes with the next send.
///
/// Errors: as `ic_apid_conn_read_key`; `IC_ERROR_NOT_SUPPORTED` too for an
/// insert or a write through a unique index; `IC_ERROR_INDEXED_COLUMN_UPDATE`
/// for an update through a unique index whose values hold an indexed column;
/// `IC_ERROR_DELETE_WITH_BLOBS` for a delete on a table with a blob column;
/// `IC_ERROR_RECORD_SIZE_TOO_BIG` for more column data than a request may
/// carry. From here until the query completes its rows are the library's.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `args` was filled by
/// `ic_write_key_args_init`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_write_key(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  trans: IcTransId,
  args: *const IcWriteKeyArgs,
) -> i32 {
  // SAFETY: the caller's promise.
  let given = match unsafe { sized_from_c(args) } {
    Ok(given) => given,
    Err(code) => return code,
  };
  let kind = match write_kind_from_c(given.kind) {
    Some(kind) => kind,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  let abort_option = match abort_from_c(given.abort_option) {
    Some(option) => option,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  let batch_hint = match given.batch_hint {
    IC_BATCH_HINT_NONE => BatchHint::None,
    IC_BATCH_HINT_SAFE => BatchHint::Safe,
    IC_BATCH_HINT_UNSAFE => BatchHint::Unsafe,
    _ => return err::IC_ERROR_NOT_SUPPORTED,
  };
  let mut rust_args = WriteKeyArgs {
    kind,
    abort_option,
    batch_hint,
    user_ref: given.user_ref as usize,
    ..WriteKeyArgs::default()
  };
  if let Some(callback) = given.callback {
    rust_args.callback = Some(trampoline);
    rust_args.binding = callback as usize;
  }
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| {
      c.write_key(
        QueryId::from_u32(query),
        TransId::from_u32(trans),
        &rust_args,
      )
    })
  }
}

/// Take the query completed longest ago without a callback, which is
/// idle again once taken. False when there is none.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `query` points to a
/// writable `IC_QUERY_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_get_next_executed_query(
  conn: *mut IcApidConnection,
  query: *mut IcQueryId,
) -> bool {
  if query.is_null() {
    return false;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.get_next_executed_query()) } {
    Ok(Some(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { query.write(id.as_u32()) };
      true
    }
    _ => false,
  }
}

// ---- Transactions ----

/// A hint for a transaction that will mostly touch the row a key names:
/// its partition, or any node when that cannot be worked out.
///
/// Never fails: a hint it cannot work out is `IC_HINT_ANY`, as for a key in a
/// collation whose keys cannot be placed.
///
/// # Safety
///
/// `conn`, `def` and `key_rec` are handles not yet freed; `key_row`
/// points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_hint_for_key(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  key_rec: *const IcRecord,
  key_row: *const u8,
  len: usize,
) -> IcTransactionHint {
  let any = hint_to_c(TransactionHint::Any);
  // SAFETY: the caller's promise.
  let (table, key) = match unsafe { (table_of(def), record_of(key_rec)) } {
    (Some(table), Some(key)) => (table, key),
    _ => return any,
  };
  if len < key.row_size() as usize {
    return any;
  }
  // SAFETY: the caller's promise.
  let row = unsafe { row_arg(key_row, len) };
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.hint_for_key(&table.def, key, row)) } {
    Ok(hint) => hint_to_c(hint),
    Err(_) => any,
  }
}

/// A hint for a transaction whose first query goes through a unique
/// index, for `ic_apid_conn_start_transaction_on_index`.
///
/// Never fails: a hint it cannot work out is `IC_HINT_ANY`.
///
/// # Safety
///
/// As `ic_apid_conn_hint_for_key`, with `index` an index handle.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_hint_for_unique_key(
  conn: *mut IcApidConnection,
  index: *const IcIndexDef,
  key_rec: *const IcRecord,
  key_row: *const u8,
  len: usize,
) -> IcTransactionHint {
  let any = hint_to_c(TransactionHint::Any);
  // SAFETY: the caller's promise.
  let (index, key) = match unsafe { (index_of(index), record_of(key_rec)) } {
    (Some(index), Some(key)) => (index, key),
    _ => return any,
  };
  if len < key.row_size() as usize {
    return any;
  }
  // SAFETY: the caller's promise.
  let row = unsafe { row_arg(key_row, len) };
  // SAFETY: the caller's promise.
  match unsafe {
    on_conn(conn, |c| c.hint_for_unique_key(&index.def, key, row))
  } {
    Ok(hint) => hint_to_c(hint),
    Err(_) => any,
  }
}

/// Start a transaction at the node the hint chooses. A partition hint
/// needs the table it is over; `def` may be NULL otherwise.
///
/// Errors: `IC_ERROR_NO_STARTED_DATA_NODE` when no data node is started;
/// `IC_ERROR_LINK_LOST`, `IC_ERROR_TIMEOUT` or the coordinator's refusal, an
/// NDB code such as 4006, when no coordinator record could be had. It waits for
/// a round trip to the data node when the connection has no coordinator record
/// free there, up to five seconds.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `def` a table
/// handle; `trans` points to a writable `IC_TRANS_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_start_transaction(
  conn: *mut IcApidConnection,
  hint: IcTransactionHint,
  def: *const IcTableDef,
  trans: *mut IcTransId,
) -> i32 {
  if trans.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let table = unsafe { table_of(def) };
  let hint = hint_from_c(hint);
  // SAFETY: the caller's promise.
  match unsafe {
    on_conn(conn, |c| match table {
      Some(table) => c.start_transaction(hint, Some(table.def.as_ref())),
      None => c.start_transaction(hint, None),
    })
  } {
    Ok(Ok(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { trans.write(id.as_u32()) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Start a transaction whose first query goes through a unique index, at
/// the node `ic_apid_conn_hint_for_unique_key` chose.
///
/// Errors: as `ic_apid_conn_start_transaction`.
///
/// # Safety
///
/// `conn` and `index` are handles not yet freed; `trans` points to a
/// writable `IC_TRANS_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_start_transaction_on_index(
  conn: *mut IcApidConnection,
  hint: IcTransactionHint,
  index: *const IcIndexDef,
  trans: *mut IcTransId,
) -> i32 {
  if trans.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let index = match unsafe { index_of(index) } {
    Some(index) => index,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  let hint = hint_from_c(hint);
  // SAFETY: the caller's promise.
  match unsafe {
    on_conn(conn, |c| {
      c.start_transaction(hint, Some(index.def.as_table().as_ref()))
    })
  } {
    Ok(Ok(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { trans.write(id.as_u32()) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Ask for the transaction to commit, with the next send.
///
/// Errors: `IC_ERROR_TRANSACTION_ACTIVE` when the transaction is ending or
/// ended already, or the id names none. The commit goes with the next send; the
/// transaction says how it ended once it is done.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_commit_transaction(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.commit_transaction(TransId::from_u32(trans))) }
}

/// Ask for the transaction to roll back; queries not yet sent are
/// dropped.
///
/// Errors: `IC_ERROR_TRANSACTION_ACTIVE` when the transaction is ending or
/// ended already, or the id names none.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_rollback_transaction(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| c.rollback_transaction(TransId::from_u32(trans)))
  }
}

/// Let go of a transaction that is done.
///
/// Errors: `IC_ERROR_TRANSACTION_ACTIVE` while it is not done, or for an id
/// that names none.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_close_transaction(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.close_transaction(TransId::from_u32(trans))) }
}

/// Where the transaction is: one of the `IC_COMMIT_STATE_*` values.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_transaction_state(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.transaction(TransId::from_u32(trans)) {
      Some(t) => commit_state_to_c(t.commit_state()),
      None => IC_COMMIT_STATE_UNKNOWN,
    })
  };
  found.unwrap_or(IC_COMMIT_STATE_UNKNOWN)
}

/// True when the transaction has ended and every query in it has
/// completed; true too for an id that names no transaction, so that a
/// loop waiting for it ends.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_transaction_is_done(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> bool {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.transaction(TransId::from_u32(trans)) {
      Some(t) => t.is_done(),
      None => true,
    })
  };
  found.unwrap_or(true)
}

/// The error the transaction ended with, or 0.
///
/// `IC_ERROR_TRANSACTION_ACTIVE` for an id that names no transaction.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_transaction_error(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> i32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.transaction(TransId::from_u32(trans)) {
      Some(t) => match t.error() {
        Some(e) => e.code,
        None => 0,
      },
      None => err::IC_ERROR_TRANSACTION_ACTIVE,
    })
  };
  match found {
    Ok(code) => code,
    Err(code) => code,
  }
}

/// What the transaction's abort said beyond its code, or 0.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_transaction_error_data(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.transaction(TransId::from_u32(trans)) {
      Some(t) => t.error_data(),
      None => 0,
    })
  };
  found.unwrap_or(0)
}

/// The global checkpoint a committed transaction belongs to, or 0.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_transaction_gci(
  conn: *mut IcApidConnection,
  trans: IcTransId,
) -> u64 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.transaction(TransId::from_u32(trans)) {
      Some(t) => t.gci(),
      None => 0,
    })
  };
  found.unwrap_or(0)
}

// ---- Sending and polling ----

/// Send everything defined, and every end asked for, one write per
/// node. With `force` the writes happen now.
///
/// Errors: the first write that failed, such as `IC_ERROR_LINK_LOST`; what
/// could go to the other nodes went. A transaction whose send failed learns of
/// it on its queries.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_send(
  conn: *mut IcApidConnection,
  force: bool,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.send_queries(force)) }
}

/// Wait up to `wait_ms` for replies and complete the queries they
/// finish, calling their callbacks; give how many signals were taken.
///
/// Fails only for a connection that is null or unusable: errors come back on
/// the queries and transactions they belong to. It blocks the calling thread up
/// to `wait_ms` when nothing has arrived.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `signals`
/// points to a writable `uint32_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_poll(
  conn: *mut IcApidConnection,
  wait_ms: u32,
  signals: *mut u32,
) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.poll(wait_ms)) } {
    Ok(taken) => {
      if !signals.is_null() {
        // SAFETY: the caller's promise.
        unsafe { signals.write(taken as u32) };
      }
      0
    }
    Err(code) => code,
  }
}

/// Send, then poll. The poll always runs, and a send that failed
/// because a link has gone is no error here: the poll fails the
/// transactions sent over that link with its error.
///
/// Errors: as `ic_apid_conn_send`, but for a lost link; returned after
/// the poll.
///
/// # Safety
///
/// As `ic_apid_conn_poll`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_flush(
  conn: *mut IcApidConnection,
  wait_ms: u32,
  force: bool,
  signals: *mut u32,
) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.flush(wait_ms, force)) } {
    Ok(Ok(taken)) => {
      if !signals.is_null() {
        // SAFETY: the caller's promise.
        unsafe { signals.write(taken as u32) };
      }
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

// ---- Synchronous ----

/// Read the committed row a key names, as its own transaction, and wait
/// for it: the row goes into `attr_row`, and `found` says whether there
/// was one.
///
/// Errors: as `ic_apid_conn_start_transaction` and `ic_apid_conn_read_key`;
/// `IC_ERROR_RECORD_LAYOUT` when a row is shorter than its record's;
/// `IC_ERROR_KEY_RECORD` for records over different tables; `IC_ERROR_TIMEOUT`
/// when no answer came in 10 seconds. A row not there is no error: `found` is
/// false.
///
/// # Safety
///
/// `conn`, `key_rec` and `attr_rec` are handles not yet freed;
/// `key_row` points to `key_len` readable bytes, `attr_row` to
/// `attr_len` writable ones, which may be the same buffer, as the key
/// is read before the row is written; `found` to a writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_read_committed(
  conn: *mut IcApidConnection,
  key_rec: *const IcRecord,
  key_row: *const u8,
  key_len: usize,
  attr_rec: *const IcRecord,
  attr_row: *mut u8,
  attr_len: usize,
  found: *mut bool,
) -> i32 {
  // SAFETY: the caller's promise.
  let (key, attr) = match unsafe { (record_of(key_rec), record_of(attr_rec)) } {
    (Some(key), Some(attr)) => (key, attr),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if found.is_null() || attr_row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  if key_len < key.row_size() as usize || attr_len < attr.row_size() as usize {
    return err::IC_ERROR_RECORD_LAYOUT;
  }
  // The key first, copied aside if it is in the row to be written; only
  // then the row, so that the two are never borrowed over the same bytes.
  let mut copy = KeyCopy::new();
  // SAFETY: the caller's promise; nothing is written yet.
  let key_bytes =
    unsafe { key_arg(key_row, key_len, attr_row, attr_len, &mut copy) };
  // SAFETY: the caller's promise; the key is no longer read from here.
  let attr_bytes = unsafe { row_arg_mut(attr_row, attr_len) };
  // SAFETY: the caller's promise.
  match unsafe {
    on_conn(conn, |c| {
      key_op::read_committed(c, key, key_bytes, attr, attr_bytes)
    })
  } {
    Ok(Ok(there)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { found.write(there) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// As `ic_apid_conn_read_committed`, through a unique index.
///
/// Errors: as `ic_apid_conn_read_committed`.
///
/// # Safety
///
/// As `ic_apid_conn_read_committed`, with `index` an index handle.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_read_unique_committed(
  conn: *mut IcApidConnection,
  index: *const IcIndexDef,
  key_rec: *const IcRecord,
  key_row: *const u8,
  key_len: usize,
  attr_rec: *const IcRecord,
  attr_row: *mut u8,
  attr_len: usize,
  found: *mut bool,
) -> i32 {
  // SAFETY: the caller's promise.
  let handles =
    unsafe { (index_of(index), record_of(key_rec), record_of(attr_rec)) };
  let (index, key, attr) = match handles {
    (Some(index), Some(key), Some(attr)) => (index, key, attr),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if found.is_null() || attr_row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  if key_len < key.row_size() as usize || attr_len < attr.row_size() as usize {
    return err::IC_ERROR_RECORD_LAYOUT;
  }
  // The key first, copied aside if it is in the row to be written; only
  // then the row, so that the two are never borrowed over the same bytes.
  let mut copy = KeyCopy::new();
  // SAFETY: the caller's promise; nothing is written yet.
  let key_bytes =
    unsafe { key_arg(key_row, key_len, attr_row, attr_len, &mut copy) };
  // SAFETY: the caller's promise; the key is no longer read from here.
  let attr_bytes = unsafe { row_arg_mut(attr_row, attr_len) };
  // SAFETY: the caller's promise.
  match unsafe {
    on_conn(conn, |c| {
      key_op::read_unique_committed(
        c, &index.def, key, key_bytes, attr, attr_bytes,
      )
    })
  } {
    Ok(Ok(there)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { found.write(there) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Write by key, as its own transaction, and wait for it: `kind` one of
/// the `IC_WRITE_KIND_*` values.
///
/// Errors: the operation's own, as the data node gives it:
/// `IC_NDB_ERROR_NO_SUCH_ROW` for an update or delete of no row,
/// `IC_NDB_ERROR_ROW_EXISTS` for an insert of one there,
/// `IC_NDB_ERROR_UNIQUE_DUPLICATE` when a unique index already holds the value;
/// and as `ic_apid_conn_write_key`, `ic_apid_conn_start_transaction` and
/// `ic_apid_conn_read_committed`. `IC_ERROR_NOT_SUPPORTED` for a kind no define
/// names.
///
/// # Safety
///
/// `conn`, `key_rec` and `attr_rec` are handles not yet freed;
/// `key_row` points to `key_len` readable bytes, `attr_row` to
/// `attr_len` readable ones.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_write(
  conn: *mut IcApidConnection,
  kind: u32,
  key_rec: *const IcRecord,
  key_row: *const u8,
  key_len: usize,
  attr_rec: *const IcRecord,
  attr_row: *const u8,
  attr_len: usize,
) -> i32 {
  let operation = match kind {
    IC_WRITE_KIND_UPDATE => key_ops::IC_OP_UPDATE,
    IC_WRITE_KIND_WRITE => key_ops::IC_OP_WRITE,
    IC_WRITE_KIND_INSERT => key_ops::IC_OP_INSERT,
    IC_WRITE_KIND_DELETE => key_ops::IC_OP_DELETE,
    _ => return err::IC_ERROR_NOT_SUPPORTED,
  };
  // SAFETY: the caller's promise.
  let (key, attr) = match unsafe { (record_of(key_rec), record_of(attr_rec)) } {
    (Some(key), Some(attr)) => (key, attr),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if key_len < key.row_size() as usize || attr_len < attr.row_size() as usize {
    return err::IC_ERROR_RECORD_LAYOUT;
  }
  // SAFETY: the caller's promise.
  let (key_bytes, attr_bytes) =
    unsafe { (row_arg(key_row, key_len), row_arg(attr_row, attr_len)) };
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| {
      key_op::write_key(c, operation, key, key_bytes, attr, attr_bytes)
    })
  }
}

// ---- Scans ----

fn scan_lock_from_c(value: u32) -> Option<ScanLock> {
  match value {
    IC_SCAN_LOCK_COMMITTED => Some(ScanLock::Committed),
    IC_SCAN_LOCK_SHARED => Some(ScanLock::Shared),
    IC_SCAN_LOCK_EXCLUSIVE => Some(ScanLock::Exclusive),
    _ => None,
  }
}

fn scan_state_to_c(state: ScanState) -> u32 {
  match state {
    ScanState::Idle => IC_SCAN_IDLE,
    ScanState::Starting => IC_SCAN_STARTING,
    ScanState::Running => IC_SCAN_RUNNING,
    ScanState::Closing => IC_SCAN_CLOSING,
    ScanState::Done => IC_SCAN_DONE,
  }
}

/// A scan's C callback as Rust sees it: the C function is in the scan's
/// binding word.
fn scan_trampoline(
  conn: &mut ApidConnection,
  batch: ScanBatchId,
  user_ref: usize,
) {
  let binding = match conn.scan(batch.scan()) {
    Some(scan) => scan.binding(),
    None => return,
  };
  if binding == 0 {
    return;
  }
  // SAFETY: the binding word was set by `ic_apid_conn_start_scan` from a
  // non-null `IcScanCallback`, and a function pointer fits a usize and
  // back.
  let callback =
    unsafe { std::mem::transmute::<usize, IcScanCallback>(binding) };
  let handle = conn as *mut ApidConnection as *mut IcApidConnection;
  // SAFETY: the handle is the connection the poll runs on; the callback
  // may call back into the library with it, which reborrows it from
  // this pointer while `conn` waits.
  unsafe { callback(handle, batch.as_u64(), user_ref as *mut c_void) };
}

/// A scan's arguments from C.
///
/// # Safety
///
/// A non-null `args` was filled by `ic_scan_args_init`.
unsafe fn scan_args_from_c(args: *const IcScanArgs) -> Result<ScanArgs, i32> {
  // SAFETY: the caller's promise.
  let given = unsafe { sized_from_c(args) }?;
  let lock = match scan_lock_from_c(given.lock) {
    Some(lock) => lock,
    None => return Err(err::IC_ERROR_NOT_SUPPORTED),
  };
  let mut rust_args = ScanArgs {
    lock,
    parallelism: given.parallelism,
    batch_rows: given.batch_rows,
    batch_bytes: given.batch_bytes,
    timeout_ms: given.timeout_ms,
    read_ahead: given.read_ahead,
    ordered: given.ordered,
    descending: given.descending,
    continuous: given.continuous,
    user_ref: given.user_ref as usize,
    ..ScanArgs::default()
  };
  if given.trans != IC_NO_TRANS {
    rust_args.trans = Some(TransId::from_u32(given.trans));
  }
  if given.partition != IC_NO_PARTITION {
    rust_args.partition = Some(given.partition);
  }
  if let Some(callback) = given.callback {
    rust_args.callback = Some(scan_trampoline);
    rust_args.binding = callback as usize;
  }
  Ok(rust_args)
}

/// Fill `args`, `size` bytes, with the defaults: a committed read of
/// every fragment, batches as the configuration says, no transaction,
/// no partition, no limit, no callback, in no order, not continuous.
///
/// When `size` is smaller than this header's `IC_SCAN_ARGS`, as for a
/// program built against an older header, only the size is written
/// into `args`, so that a call given it sees how short it is and
/// refuses it, without reading past it.
///
/// # Safety
///
/// A non-null `args` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_args_init(args: *mut IcScanArgs, size: usize) {
  if args.is_null() {
    return;
  }
  if size < std::mem::size_of::<IcScanArgs>() {
    // SAFETY: the caller's promise.
    unsafe { mark_short(args.cast::<u8>(), size) };
    return;
  }
  let value = IcScanArgs {
    size: size as u32,
    lock: IC_SCAN_LOCK_COMMITTED,
    trans: IC_NO_TRANS,
    parallelism: 0,
    batch_rows: 0,
    batch_bytes: 0,
    partition: IC_NO_PARTITION,
    timeout_ms: 0,
    read_ahead: false,
    callback: None,
    user_ref: std::ptr::null_mut(),
    ordered: false,
    descending: false,
    continuous: false,
  };
  // SAFETY: the caller's promise; the struct fits.
  unsafe { args.write(value) };
}

/// A scan of a table, reading the fields of `rec` into its row. Made
/// once and run again and again (`ic_apid_conn_start_scan`).
///
/// Errors: `IC_ERROR_KEY_RECORD` when the record is over another table,
/// or another version of it. The scan lives in the connection until
/// `ic_apid_scan_free` or the connection's free.
///
/// # Safety
///
/// `conn`, `def` and `rec` are handles not yet freed; `scan` points to a
/// writable `IC_SCAN_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_create(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  rec: *const IcRecord,
  scan: *mut IcScanId,
) -> i32 {
  // SAFETY: the caller's promise.
  let (table, rec) = match unsafe { (table_of(def), record_of(rec)) } {
    (Some(table), Some(rec)) => (table, rec),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if scan.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.create_scan(&table.def, rec)) } {
    Ok(Ok(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { scan.write(id.as_u32()) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// A scan of an ordered index (chapter 04, "Index scans"): its ranges
/// laid out by `key_rec`, which must hold every column of the index,
/// and the fields of `rec` read into its row, both records over the
/// index's table. Made once and run again and again, each run reading
/// the ranges added before it (`ic_apid_conn_add_scan_range`); a run
/// given none reads the whole index.
///
/// Errors: `IC_ERROR_NOT_SUPPORTED` for an index that is not ordered;
/// `IC_ERROR_KEY_RECORD` when a record is over another table, or the key
/// record lacks a column of the index. The scan lives in the connection
/// until `ic_apid_scan_free` or the connection's free.
///
/// # Safety
///
/// `conn`, `index`, `key_rec` and `rec` are handles not yet freed;
/// `scan` points to a writable `IC_SCAN_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_index_scan_create(
  conn: *mut IcApidConnection,
  index: *const IcIndexDef,
  key_rec: *const IcRecord,
  rec: *const IcRecord,
  scan: *mut IcScanId,
) -> i32 {
  // SAFETY: the caller's promise.
  let (index, key_rec, rec) =
    match unsafe { (index_of(index), record_of(key_rec), record_of(rec)) } {
      (Some(index), Some(key_rec), Some(rec)) => (index, key_rec, rec),
      _ => return err::IC_ERROR_NULL_POINTER,
    };
  if scan.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let made =
    unsafe { on_conn(conn, |c| c.create_index_scan(&index.def, key_rec, rec)) };
  match made {
    Ok(Ok(id)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { scan.write(id.as_u32()) };
      0
    }
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// Fill `range`, `size` bytes, with the defaults: no bound on either
/// side, which takes in every row, as range 0.
///
/// When `size` is smaller than this header's `IC_SCAN_RANGE`, as for a
/// program built against an older header, only the size is written
/// into `range`, so that a call given it sees how short it is and
/// refuses it, without reading past it.
///
/// # Safety
///
/// A non-null `range` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_range_init(
  range: *mut IcScanRange,
  size: usize,
) {
  if range.is_null() {
    return;
  }
  if size < std::mem::size_of::<IcScanRange>() {
    // SAFETY: the caller's promise.
    unsafe { mark_short(range.cast::<u8>(), size) };
    return;
  }
  let value = IcScanRange {
    size: size as u32,
    low_columns: 0,
    low_inclusive: false,
    high_columns: 0,
    high_inclusive: false,
    range_no: 0,
  };
  // SAFETY: the caller's promise; the struct fits.
  unsafe { range.write(value) };
}

/// A range from C.
///
/// # Safety
///
/// A non-null `range` was filled by `ic_scan_range_init`.
unsafe fn scan_range_from_c(
  range: *const IcScanRange,
) -> Result<ScanRange, i32> {
  // SAFETY: the caller's promise.
  let given = unsafe { sized_from_c(range) }?;
  Ok(ScanRange {
    low_columns: given.low_columns,
    low_inclusive: given.low_inclusive,
    high_columns: given.high_columns,
    high_inclusive: given.high_inclusive,
    range_no: given.range_no,
  })
}

/// Add a range to the next run of a scan of an ordered index, its bounds
/// taken from `low` and `high`, `low_len` and `high_len` bytes, rows laid
/// out by the scan's key record. A bound the range does not use may be
/// NULL with length 0. The range is encoded at once; the rows are not
/// kept.
///
/// Errors: `IC_ERROR_BAD_RANGE` for a number above 4095, more columns
/// than the index has, or bounds too long to send;
/// `IC_ERROR_RECORD_LAYOUT` for a row shorter than the key record's;
/// `IC_ERROR_NOT_SUPPORTED` for a scan of a table;
/// `IC_ERROR_INCONSISTENT_DATA` for `range` not filled by
/// `ic_scan_range_init`. Nothing is added then.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `range` was filled by
/// `ic_scan_range_init`; a non-null `low` or `high` points to as many
/// readable bytes as its length says.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_add_scan_range(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  range: *const IcScanRange,
  low: *const u8,
  low_len: usize,
  high: *const u8,
  high_len: usize,
) -> i32 {
  // SAFETY: the caller's promise.
  let given = match unsafe { scan_range_from_c(range) } {
    Ok(given) => given,
    Err(code) => return code,
  };
  // SAFETY: the caller's promise.
  let (low_row, high_row) =
    unsafe { (row_arg(low, low_len), row_arg(high, high_len)) };
  // SAFETY: the caller's promise.
  let added = unsafe {
    on_conn(conn, |c| {
      c.add_scan_range(ScanId::from_u32(scan), &given, low_row, high_row)
    })
  };
  match added {
    Ok(Ok(())) => 0,
    Ok(Err(e)) => e.code,
    Err(code) => code,
  }
}

/// The number of the range the scan's row last read is in; 0 for an id
/// that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_range_no(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found =
    unsafe { on_conn(conn, |c| c.scan_range_no(ScanId::from_u32(scan))) };
  found.unwrap_or(0)
}

/// Free a scan that is not running.
///
/// Errors: `IC_ERROR_SCAN_ACTIVE` while it runs; `IC_ERROR_NO_SUCH_FIELD`
/// for an id that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_free(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.free_scan(ScanId::from_u32(scan))) }
}

/// Start a run of the scan, as `args` says (`ic_scan_args_init`). The
/// request goes with the next send, or with the poll that brings its
/// record at the coordinator; batches come in later polls, to the
/// callback or onto the ready list (`ic_apid_conn_get_next_scan_batch`),
/// or are read row by row with `ic_apid_conn_next_row`.
///
/// Errors: `IC_ERROR_SCAN_ACTIVE` while a run is not over;
/// `IC_ERROR_NOT_SUPPORTED` for an unknown lock, for `read_ahead`, which
/// is for executors, for a committed read given a transaction, and for
/// `continuous` with a lock;
/// `IC_ERROR_TRANSACTION_ACTIVE` for a transaction not started or not
/// open; `IC_ERROR_INCONSISTENT_DATA` for `args` not filled by
/// `ic_scan_args_init`.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `args` was filled by
/// `ic_scan_args_init`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_start_scan(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  args: *const IcScanArgs,
) -> i32 {
  // SAFETY: the caller's promise.
  let rust_args = match unsafe { scan_args_from_c(args) } {
    Ok(rust_args) => rust_args,
    Err(code) => return code,
  };
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| c.start_scan(ScanId::from_u32(scan), &rust_args))
  }
}

/// End a run before it reads to the end: the coordinator is told to
/// stop at once, and the run is done when it confirms, in later polls.
/// The batches asked for are not waited for, and their rows are dropped
/// as they come; only a locking scan whose transaction goes on first
/// waits for the takeovers of its rows still to be received. Batches
/// with the program are let go of.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD` for an id that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_close_scan(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.close_scan(ScanId::from_u32(scan))) }
}

/// Where the scan is: one of the `IC_SCAN_*` states, `IC_SCAN_UNKNOWN`
/// for an id that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_state(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.scan(ScanId::from_u32(scan)) {
      Some(s) => scan_state_to_c(s.state()),
      None => IC_SCAN_UNKNOWN,
    })
  };
  found.unwrap_or(IC_SCAN_UNKNOWN)
}

/// True once the scan's run is over, read to the end, closed or failed;
/// true too for an id that names no scan, so that a loop waiting for it
/// ends.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_is_done(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> bool {
  // SAFETY: the caller's promise.
  let found = unsafe { on_conn(conn, |c| c.scan_done(ScanId::from_u32(scan))) };
  found.unwrap_or(true)
}

/// Why the scan's run failed, or 0 if it read to the end, was closed,
/// or is still going.
///
/// `IC_ERROR_NO_SUCH_FIELD` for an id that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_error(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> i32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.scan(ScanId::from_u32(scan)) {
      Some(s) => match s.error() {
        Some(e) => e.code,
        None => 0,
      },
      None => err::IC_ERROR_NO_SUCH_FIELD,
    })
  };
  match found {
    Ok(code) => code,
    Err(code) => code,
  }
}

/// Rows read from the run's batches so far; 0 for an id that names no
/// scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_rows_read(
  conn: *mut IcApidConnection,
  scan: IcScanId,
) -> u64 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.scan(ScanId::from_u32(scan)) {
      Some(s) => s.rows_read(),
      None => 0,
    })
  };
  found.unwrap_or(0)
}

/// The scan's row and its length: the row last read from one of its
/// batches, laid out by its record. Valid until the scan is freed; NULL
/// for an id that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `len` points
/// to a writable `size_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_row(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  len: *mut usize,
) -> *const u8 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| {
      let row = c.scan_row(ScanId::from_u32(scan));
      if row.is_empty() {
        return (std::ptr::null(), 0);
      }
      (row.as_ptr(), row.len())
    })
  };
  let (row, row_len) = found.unwrap_or((std::ptr::null(), 0));
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// The scan a batch is of.
#[no_mangle]
pub extern "C" fn ic_scan_batch_scan(batch: IcScanBatchId) -> IcScanId {
  ScanBatchId::from_u64(batch).scan().as_u32()
}

/// Take the batch completed longest ago, of a scan with no callback,
/// into `batch`. False when there is none.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `batch` points to a
/// writable `IC_SCAN_BATCH_ID`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_get_next_scan_batch(
  conn: *mut IcApidConnection,
  batch: *mut IcScanBatchId,
) -> bool {
  if batch.is_null() {
    return false;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.get_next_scan_batch()) } {
    Ok(Some(next)) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { batch.write(next.as_u64()) };
      true
    }
    _ => false,
  }
}

/// Rows in a batch; 0 for one no longer with the program.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_scan_batch_rows(
  conn: *mut IcApidConnection,
  batch: IcScanBatchId,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| c.scan_batch_rows(ScanBatchId::from_u64(batch)))
  };
  found.unwrap_or(0)
}

/// What a call that reads a row gives C: the code, and whether a row
/// was read into `more`.
///
/// # Safety
///
/// `more` points to a writable `bool`.
unsafe fn row_read(
  found: Result<Result<bool, IcError>, i32>,
  more: *mut bool,
) -> i32 {
  let (code, read) = match found {
    Ok(Ok(read)) => (0, read),
    Ok(Err(e)) => (e.code, false),
    Err(code) => (code, false),
  };
  // SAFETY: the caller's promise.
  unsafe { more.write(read) };
  code
}

/// Unpack the batch's next row into the scan's row
/// (`ic_apid_scan_row`); `more` says whether there was one.
///
/// Errors: `IC_ERROR_NO_SUCH_FIELD` for a batch no longer with the
/// program.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `more` points to a
/// writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_next_scan_row(
  conn: *mut IcApidConnection,
  batch: IcScanBatchId,
  more: *mut bool,
) -> i32 {
  if more.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let found =
    unsafe { on_conn(conn, |c| c.next_scan_row(ScanBatchId::from_u64(batch))) };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe { row_read(found, more) }
}

/// As `ic_apid_conn_next_scan_row`, into `row`, `len` bytes of the
/// program's laid out by the scan's record, a copy less.
///
/// Errors: as `ic_apid_conn_next_scan_row`; `IC_ERROR_RECORD_SIZE_TOO_BIG`
/// for a row shorter than the record's.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `row` points to `len`
/// writable bytes; `more` points to a writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_next_scan_row_into(
  conn: *mut IcApidConnection,
  batch: IcScanBatchId,
  row: *mut u8,
  len: usize,
  more: *mut bool,
) -> i32 {
  if more.is_null() || row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let into = unsafe { row_arg_mut(row, len) };
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| {
      c.next_scan_row_into(ScanBatchId::from_u64(batch), into)
    })
  };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe { row_read(found, more) }
}

/// Hand a batch back: its receiver is asked for more with the next send
/// or poll. A batch of a run that has ended is passed over.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_release_scan_batch(
  conn: *mut IcApidConnection,
  batch: IcScanBatchId,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| c.release_scan_batch(ScanBatchId::from_u64(batch)))
  }
}

/// Read the scan's next row into its row (`ic_apid_scan_row`), as a
/// stream of rows: a batch is handed back once its rows are read, and
/// the next waited for in polls, which serve every other scan and query
/// of the connection meanwhile. `more` is false once the run has read
/// to the end or was closed, or for a scan not started.
///
/// Errors: the run's error once it has failed; `IC_ERROR_NOT_SUPPORTED`
/// for a scan with a callback, and inside a callback once it would have
/// to wait; `IC_ERROR_NO_SUCH_FIELD` for an id that names no scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `more` points to a
/// writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_next_row(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  more: *mut bool,
) -> i32 {
  if more.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let found = unsafe { on_conn(conn, |c| c.next_row(ScanId::from_u32(scan))) };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe { row_read(found, more) }
}

/// As `ic_apid_conn_next_row`, into `row`, `len` bytes of the program's
/// laid out by the scan's record, a copy less.
///
/// Errors: as `ic_apid_conn_next_row`; `IC_ERROR_RECORD_SIZE_TOO_BIG` for
/// a row shorter than the record's.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `row` points to `len`
/// writable bytes; `more` points to a writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_next_row_into(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  row: *mut u8,
  len: usize,
  more: *mut bool,
) -> i32 {
  if more.is_null() || row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let into = unsafe { row_arg_mut(row, len) };
  // SAFETY: the caller's promise.
  let found =
    unsafe { on_conn(conn, |c| c.next_row_into(ScanId::from_u32(scan), into)) };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe { row_read(found, more) }
}

/// The scan's next complete batch into `batch`, waiting as
/// `ic_apid_conn_next_row` does; `more` is false, and `batch` left as
/// it is, once the run is over with no error. The program reads the
/// batch with `ic_apid_conn_next_scan_row` and hands it back itself.
///
/// Errors: as `ic_apid_conn_next_row`.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `batch` points to a
/// writable `IC_SCAN_BATCH_ID` and `more` to a writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_next_batch(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  batch: *mut IcScanBatchId,
  more: *mut bool,
) -> i32 {
  if more.is_null() || batch.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let found =
    unsafe { on_conn(conn, |c| c.next_batch(ScanId::from_u32(scan))) };
  let (code, next) = match found {
    Ok(Ok(next)) => (0, next),
    Ok(Err(e)) => (e.code, None),
    Err(code) => (code, None),
  };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe {
    more.write(next.is_some());
    if let Some(next) = next {
      batch.write(next.as_u64());
    }
  }
  code
}

/// Fill `args`, `size` bytes, with the defaults: the default abort
/// option, no callback.
///
/// When `size` is smaller than this header's `IC_TAKEOVER_ARGS`, as for a
/// program built against an older header, only the size is written
/// into `args`, so that a call given it sees how short it is and
/// refuses it, without reading past it.
///
/// # Safety
///
/// A non-null `args` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_takeover_args_init(
  args: *mut IcTakeoverArgs,
  size: usize,
) {
  if args.is_null() {
    return;
  }
  if size < std::mem::size_of::<IcTakeoverArgs>() {
    // SAFETY: the caller's promise.
    unsafe { mark_short(args.cast::<u8>(), size) };
    return;
  }
  let value = IcTakeoverArgs {
    size: size as u32,
    abort_option: IC_ABORT_DEFAULT,
    callback: None,
    user_ref: std::ptr::null_mut(),
  };
  // SAFETY: the caller's promise; the struct fits.
  unsafe { args.write(value) };
}

/// A takeover's arguments from C.
///
/// # Safety
///
/// A non-null `args` was filled by `ic_takeover_args_init`.
unsafe fn takeover_args_from_c(
  args: *const IcTakeoverArgs,
) -> Result<TakeoverArgs, i32> {
  // SAFETY: the caller's promise.
  let given = unsafe { sized_from_c(args) }?;
  let abort_option = match abort_from_c(given.abort_option) {
    Some(option) => option,
    None => return Err(err::IC_ERROR_NOT_SUPPORTED),
  };
  let mut rust_args = TakeoverArgs {
    abort_option,
    user_ref: given.user_ref as usize,
    ..TakeoverArgs::default()
  };
  if let Some(callback) = given.callback {
    rust_args.callback = Some(trampoline);
    rust_args.binding = callback as usize;
  }
  Ok(rust_args)
}

/// What the three takeovers do.
#[derive(Clone, Copy)]
enum Takeover {
  Lock,
  Update,
  Delete,
}

/// A takeover from C.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `args` was filled by
/// `ic_takeover_args_init`.
unsafe fn take_over(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  scan: IcScanId,
  args: *const IcTakeoverArgs,
  what: Takeover,
) -> i32 {
  // SAFETY: the caller's promise.
  let rust_args = match unsafe { takeover_args_from_c(args) } {
    Ok(rust_args) => rust_args,
    Err(code) => return code,
  };
  let query = QueryId::from_u32(query);
  let scan = ScanId::from_u32(scan);
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| match what {
      Takeover::Lock => c.lock_scan_row(query, scan, &rust_args),
      Takeover::Update => c.update_scan_row(query, scan, &rust_args),
      Takeover::Delete => c.delete_scan_row(query, scan, &rust_args),
    })
  }
}

/// Take the lock on the row last read from the scan over into the scan's
/// transaction (`IC_SCAN_ARGS`' `trans`), reading the fields of the
/// query's attribute record into its attribute row: shared from a
/// shared-lock scan, exclusive from an exclusive one. The query, over
/// the scanned table, completes as any query does and goes with the next
/// send; the batch the row came from asks for more only once every
/// takeover of it is done.
///
/// Errors: `IC_ERROR_NOT_SUPPORTED` for a scan with no transaction or a
/// query through an index; `IC_ERROR_NO_SCAN_ROW` when no row has been
/// read from a batch still with the program; `IC_ERROR_KEY_RECORD` for a
/// query over another table; `IC_ERROR_TRANSACTION_ACTIVE` for a query
/// already defined or a transaction no longer open.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; `args` was filled by
/// `ic_takeover_args_init`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_lock_scan_row(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  scan: IcScanId,
  args: *const IcTakeoverArgs,
) -> i32 {
  // SAFETY: passed on from the caller.
  unsafe { take_over(conn, query, scan, args, Takeover::Lock) }
}

/// Update the row last read from the scan, in the scan's transaction,
/// with the values of the query's attribute row, its key fields left
/// out.
///
/// Errors: as `ic_apid_conn_lock_scan_row`.
///
/// # Safety
///
/// As `ic_apid_conn_lock_scan_row`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_update_scan_row(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  scan: IcScanId,
  args: *const IcTakeoverArgs,
) -> i32 {
  // SAFETY: passed on from the caller.
  unsafe { take_over(conn, query, scan, args, Takeover::Update) }
}

/// Delete the row last read from the scan, in the scan's transaction.
///
/// Errors: as `ic_apid_conn_lock_scan_row`; `IC_ERROR_DELETE_WITH_BLOBS`
/// on a table with a blob column.
///
/// # Safety
///
/// As `ic_apid_conn_lock_scan_row`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_delete_scan_row(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  scan: IcScanId,
  args: *const IcTakeoverArgs,
) -> i32 {
  // SAFETY: passed on from the caller.
  unsafe { take_over(conn, query, scan, args, Takeover::Delete) }
}

// ---- Executors ----

/// The executor behind a handle.
///
/// # Safety
///
/// A non-null `executor` is an executor handle not yet freed.
unsafe fn executor_of<'a>(
  executor: *const IcApidExecutor,
) -> Option<&'a Executor> {
  // SAFETY: the caller's promise.
  unsafe { (executor as *const Executor).as_ref() }
}

/// The submitter behind a handle.
///
/// # Safety
///
/// A non-null `submitter` is a submitter handle not yet freed, used by
/// one thread at a time.
unsafe fn submitter_of<'a>(
  submitter: *mut IcApidSubmitter,
) -> Option<&'a mut Submitter> {
  // SAFETY: the caller's promise.
  unsafe { (submitter as *mut Submitter).as_mut() }
}

/// The shape behind a handle.
///
/// # Safety
///
/// A non-null `shape` is a shape handle not yet freed.
unsafe fn shape_of<'a>(shape: *const IcOpShape) -> Option<&'a Arc<OpShape>> {
  // SAFETY: the caller's promise.
  unsafe { (shape as *const Arc<OpShape>).as_ref() }
}

/// The request behind a handle.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed, used by one
/// thread at a time.
unsafe fn request_of<'a>(request: *mut IcRequest) -> Option<&'a mut Request> {
  // SAFETY: the caller's promise.
  unsafe { (request as *mut Request).as_mut() }
}

/// As [`request_of`], to read.
///
/// # Safety
///
/// As [`request_of`].
unsafe fn request_ref<'a>(request: *const IcRequest) -> Option<&'a Request> {
  // SAFETY: the caller's promise.
  unsafe { (request as *const Request).as_ref() }
}

/// Operation `op` of a request, to read.
///
/// # Safety
///
/// As [`request_of`].
unsafe fn op_ref<'a>(request: *const IcRequest, op: u32) -> Option<&'a KeyOp> {
  // SAFETY: the caller's promise.
  let found = unsafe { request_ref(request) }?;
  found.ops().get(op as usize)
}

/// Bytes of `IC_EXECUTOR_OPTIONS` in release 0.1, before `scans`.
fn executor_options_0_1() -> usize {
  std::mem::offset_of!(IcExecutorOptions, scans)
}

/// Bytes of `IC_EXECUTOR_STATS` in release 0.1, before `scans`.
fn executor_stats_0_1() -> usize {
  std::mem::offset_of!(IcExecutorStats, scans)
}

/// Write a field of a struct with a size at `offset` in it, if the
/// caller's struct, `size` bytes, holds it: one built against an older
/// header is shorter, and gets only the fields it knows.
///
/// # Safety
///
/// `base` points to `size` writable bytes.
unsafe fn put_sized<T: Copy>(base: *mut u8, size: usize, offset: usize, v: T) {
  if offset + std::mem::size_of::<T>() > size {
    return;
  }
  // SAFETY: the caller's promise; the field lies within `size` bytes.
  unsafe { base.add(offset).cast::<T>().write_unaligned(v) };
}

/// Read a field of a struct with a size, as [`put_sized`] writes one:
/// `None` if the caller's struct is too short to hold it.
///
/// # Safety
///
/// `base` points to `size` readable bytes.
unsafe fn get_sized<T: Copy>(
  base: *const u8,
  size: usize,
  offset: usize,
) -> Option<T> {
  if offset + std::mem::size_of::<T>() > size {
    return None;
  }
  // SAFETY: the caller's promise; the field lies within `size` bytes.
  Some(unsafe { base.add(offset).cast::<T>().read_unaligned() })
}

/// A struct with a size from C, whose first field is its size: copied
/// whole only once that field says it is as long as this header's. One
/// built against an older header is shorter, and is refused with
/// `IC_ERROR_INCONSISTENT_DATA` without a byte past its size field
/// read; a null pointer with `IC_ERROR_NULL_POINTER`.
///
/// # Safety
///
/// A non-null `given` points to a struct its `_init` was given, at
/// least as many bytes as its size field says.
unsafe fn sized_from_c<T: Copy>(given: *const T) -> Result<T, i32> {
  if given.is_null() {
    return Err(err::IC_ERROR_NULL_POINTER);
  }
  // SAFETY: the caller's promise; the size field is the first 4 bytes.
  let size = unsafe { get_sized::<u32>(given.cast::<u8>(), 4, 0) };
  if (size.unwrap_or(0) as usize) < std::mem::size_of::<T>() {
    return Err(err::IC_ERROR_INCONSISTENT_DATA);
  }
  // SAFETY: the caller's promise, and its size says it holds a whole T.
  Ok(unsafe { given.read_unaligned() })
}

/// For an `_init` given a struct shorter than this header's: its size
/// alone written into its first field, if it holds that much, so that a
/// call given it sees how short it is (`sized_from_c`).
///
/// # Safety
///
/// `base` points to `size` writable bytes.
unsafe fn mark_short(base: *mut u8, size: usize) {
  // SAFETY: the caller's promise; put_sized writes within `size` bytes.
  unsafe { put_sized(base, size, 0, size as u32) };
}

/// Fill `options`, `size` bytes, with the defaults: the fields that fit,
/// so that a program built against an older header gets those it knows.
///
/// Leaves `options` as it is when `size` is smaller than release 0.1's
/// `IC_EXECUTOR_OPTIONS`.
///
/// # Safety
///
/// A non-null `options` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_executor_options_init(
  options: *mut IcExecutorOptions,
  size: usize,
) {
  if options.is_null() || size < executor_options_0_1() {
    return;
  }
  let defaults = ExecutorOptions::default();
  let base = options as *mut u8;
  // SAFETY: the caller's promise; each field is written only if it
  // lies within `size` bytes.
  unsafe {
    put_sized(
      base,
      size,
      std::mem::offset_of!(IcExecutorOptions, size),
      size as u32,
    );
    put_sized(
      base,
      size,
      std::mem::offset_of!(IcExecutorOptions, in_flight),
      defaults.in_flight,
    );
    put_sized(
      base,
      size,
      std::mem::offset_of!(IcExecutorOptions, queue),
      defaults.queue,
    );
    put_sized(
      base,
      size,
      std::mem::offset_of!(IcExecutorOptions, scans),
      defaults.scans,
    );
  }
}

/// Start an executor: a thread of the library's, with a connection of
/// its own, that runs requests for any number of the program's threads,
/// each through a submitter of its own (`ic_apid_executor_submitter`).
/// Everything it admits in one pass goes in one write per data node, so
/// under load the requests of many threads travel together. `options`
/// may be NULL for the defaults.
///
/// Errors: `IC_ERROR_TOO_MANY_USER_THREADS` when the process has 4 711
/// connections, the executor's own being one; `IC_ERROR_INCONSISTENT_DATA` for
/// `options` not filled by `ic_executor_options_init`; an operating system
/// error when its thread cannot be started. The executor is the caller's,
/// freed with `ic_apid_executor_free` before the global.
///
/// # Safety
///
/// `global` is a global handle not yet freed; a non-null `options` was
/// filled by `ic_executor_options_init`; `executor` points to writable
/// storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_executor_start(
  global: *mut IcApidGlobal,
  options: *const IcExecutorOptions,
  executor: *mut *mut IcApidExecutor,
) -> i32 {
  if executor.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let g = match unsafe { global_of(global) } {
    Some(g) => g,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  let mut opts = ExecutorOptions::default();
  if !options.is_null() {
    let base = options as *const u8;
    // SAFETY: the caller's promise: the struct was filled by
    // `ic_executor_options_init`, which wrote its size first. Each later
    // field is read only if that size holds it.
    unsafe {
      let size = get_sized::<u32>(base, 4, 0).unwrap_or(0) as usize;
      if size < executor_options_0_1() {
        return err::IC_ERROR_INCONSISTENT_DATA;
      }
      let in_flight = std::mem::offset_of!(IcExecutorOptions, in_flight);
      if let Some(value) = get_sized::<u32>(base, size, in_flight) {
        opts.in_flight = value;
      }
      let queue = std::mem::offset_of!(IcExecutorOptions, queue);
      if let Some(value) = get_sized::<u32>(base, size, queue) {
        opts.queue = value;
      }
      let scans = std::mem::offset_of!(IcExecutorOptions, scans);
      if let Some(value) = get_sized::<u32>(base, size, scans) {
        opts.scans = value;
      }
    }
  }
  guarded(|| match Executor::start(g, &opts) {
    Ok(started) => {
      let handle = Box::into_raw(Box::new(started)) as *mut IcApidExecutor;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { executor.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// Stop an executor and free it. Requests not yet admitted come back
/// with `IC_ERROR_EXECUTOR_STOPPED`; those out are finished first, so
/// this waits for their replies.
///
/// Its submitters stay valid and refuse every request from here on;
/// free them before or after. No thread may be inside a call on the
/// executor itself meanwhile.
///
/// # Safety
///
/// A non-null `executor` is an executor handle not yet freed, freed
/// here, and this is not called from inside an executor's own thread.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_executor_free(executor: *mut IcApidExecutor) {
  if executor.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box start made.
  let owned = unsafe { Box::from_raw(executor as *mut Executor) };
  // Dropping it stops it and waits for its thread.
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// What the executor has done since it started, into `stats`, `size`
/// bytes: the fields that fit, so that a program built against an older
/// header gets those it knows.
///
/// Errors: `IC_ERROR_INCONSISTENT_DATA` when `size` is smaller than
/// release 0.1's `IC_EXECUTOR_STATS`. Any thread may call it.
///
/// # Safety
///
/// `executor` is an executor handle not yet freed; `stats` points to
/// `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_executor_stats(
  executor: *const IcApidExecutor,
  stats: *mut IcExecutorStats,
  size: usize,
) -> i32 {
  // SAFETY: the caller's promise.
  let e = match unsafe { executor_of(executor) } {
    Some(e) => e,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if stats.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  if size < executor_stats_0_1() {
    return err::IC_ERROR_INCONSISTENT_DATA;
  }
  let now = e.stats();
  let base = stats as *mut u8;
  let counts: [(usize, u64); 8] = [
    (
      std::mem::offset_of!(IcExecutorStats, submitted),
      now.submitted,
    ),
    (std::mem::offset_of!(IcExecutorStats, refused), now.refused),
    (
      std::mem::offset_of!(IcExecutorStats, admitted),
      now.admitted,
    ),
    (
      std::mem::offset_of!(IcExecutorStats, completed),
      now.completed,
    ),
    (std::mem::offset_of!(IcExecutorStats, expired), now.expired),
    (std::mem::offset_of!(IcExecutorStats, sends), now.sends),
    (std::mem::offset_of!(IcExecutorStats, passes), now.passes),
    (std::mem::offset_of!(IcExecutorStats, scans), now.scans),
  ];
  // SAFETY: the caller's promise; each field is written only if it lies
  // within `size` bytes.
  unsafe {
    put_sized(
      base,
      size,
      std::mem::offset_of!(IcExecutorStats, size),
      size as u32,
    );
    for (offset, count) in counts {
      put_sized(base, size, offset, count);
    }
  }
  0
}

/// A submitter: one thread's handle on an executor, through which it
/// binds tables and runs requests. Make one per thread.
///
/// Any thread may call it. The submitter is the caller's, freed with
/// `ic_apid_submitter_free`; one thread uses it at a time.
///
/// # Safety
///
/// `executor` is an executor handle not yet freed; `submitter` points to
/// writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_executor_submitter(
  executor: *const IcApidExecutor,
  submitter: *mut *mut IcApidSubmitter,
) -> i32 {
  if submitter.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let e = match unsafe { executor_of(executor) } {
    Some(e) => e,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    let made = e.submitter();
    let handle = Box::into_raw(Box::new(made)) as *mut IcApidSubmitter;
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { submitter.write(handle) };
    0
  })
}

/// Free a submitter.
///
/// # Safety
///
/// A non-null `submitter` is a submitter handle not yet freed, freed
/// here, with no call on it under way.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_free(
  submitter: *mut IcApidSubmitter,
) {
  if submitter.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(submitter as *mut Submitter) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// Bind a table through the executor, for a thread that has no
/// connection of its own: as `ic_apid_conn_table_bind`.
///
/// Errors: as `ic_apid_conn_table_bind`; `IC_ERROR_EXECUTOR_STOPPED`. The
/// executor waits for the dictionary meanwhile, so bind once, not per
/// request. The handle is the caller's, freed with `ic_table_def_free`.
///
/// # Safety
///
/// `submitter` is a submitter handle not yet freed; `db` and `table` are
/// NUL-terminated strings; `def` points to writable storage for a
/// pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_table_bind(
  submitter: *mut IcApidSubmitter,
  db: *const c_char,
  table: *const c_char,
  def: *mut *mut IcTableDef,
) -> i32 {
  if def.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let s = match unsafe { submitter_of(submitter) } {
    Some(s) => s,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  let (db, table) = match unsafe { (text_arg(db), text_arg(table)) } {
    (Ok(db), Ok(table)) => (db, table),
    (Err(code), _) | (_, Err(code)) => return code,
  };
  guarded(|| match s.table_bind(db, table) {
    Ok(bound) => {
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { def.write(table_handle(bound)) };
      0
    }
    Err(e) => e.code,
  })
}

/// Bind a unique index through the executor: as
/// `ic_apid_conn_index_bind`.
///
/// Errors: as `ic_apid_conn_index_bind`; `IC_ERROR_EXECUTOR_STOPPED`. The
/// handle is the caller's, freed with `ic_index_def_free`.
///
/// # Safety
///
/// As `ic_apid_submitter_table_bind`, with `index` a NUL-terminated
/// string.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_index_bind(
  submitter: *mut IcApidSubmitter,
  db: *const c_char,
  index: *const c_char,
  table: *const c_char,
  def: *mut *mut IcIndexDef,
) -> i32 {
  if def.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let s = match unsafe { submitter_of(submitter) } {
    Some(s) => s,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  let texts = unsafe { (text_arg(db), text_arg(index), text_arg(table)) };
  let (db, index, table) = match texts {
    (Ok(db), Ok(index), Ok(table)) => (db, index, table),
    (Err(code), _, _) | (_, Err(code), _) | (_, _, Err(code)) => return code,
  };
  guarded(|| match s.index_bind(db, index, table) {
    Ok(bound) => {
      let handle = Index {
        name: c_string(bound.name()),
        def: bound,
      };
      let raw = Box::into_raw(Box::new(handle)) as *mut IcIndexDef;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { def.write(raw) };
      0
    }
    Err(e) => e.code,
  })
}

/// The shape of key operations on a table by primary key: the records
/// their key rows and attribute rows are laid out by. Made once, and
/// shared by every request that uses it.
///
/// Errors: `IC_ERROR_KEY_RECORD` when the key record does not hold every
/// primary key field, or a record is over another table or another version of
/// it. The shape is the caller's, freed with `ic_op_shape_free`; any thread
/// may use it, and a request keeps what it needs of it.
///
/// # Safety
///
/// `def`, `key_rec` and `attr_rec` are handles not yet freed; `shape`
/// points to writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_op_shape_create(
  def: *const IcTableDef,
  key_rec: *const IcRecord,
  attr_rec: *const IcRecord,
  shape: *mut *mut IcOpShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let (table, key, attr) =
    match unsafe { (table_of(def), record_of(key_rec), record_of(attr_rec)) } {
      (Some(table), Some(key), Some(attr)) => (table, key, attr),
      _ => return err::IC_ERROR_NULL_POINTER,
    };
  if shape.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match OpShape::new(&table.def, key, attr) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcOpShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { shape.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// The shape of key operations through a unique index: the key record
/// holds the indexed columns, and both records are over the index's
/// table.
///
/// Errors: as `ic_op_shape_create`; `IC_ERROR_NOT_SUPPORTED` for an index
/// that is not unique.
///
/// # Safety
///
/// As `ic_op_shape_create`, with `index` an index handle.
#[no_mangle]
pub unsafe extern "C" fn ic_op_shape_create_unique(
  index: *const IcIndexDef,
  key_rec: *const IcRecord,
  attr_rec: *const IcRecord,
  shape: *mut *mut IcOpShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let handles =
    unsafe { (index_of(index), record_of(key_rec), record_of(attr_rec)) };
  let (index, key, attr) = match handles {
    (Some(index), Some(key), Some(attr)) => (index, key, attr),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if shape.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match OpShape::new_unique(&index.def, key, attr) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcOpShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { shape.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// Free a shape handle. Requests that hold operations of it keep what
/// they need.
///
/// # Safety
///
/// A non-null `shape` is a shape handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_op_shape_free(shape: *mut IcOpShape) {
  if shape.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(shape as *mut Arc<OpShape>) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// A request with no operations: a transaction for an executor to run.
/// A request is filled once and run again and again, its rows changed
/// between runs.
///
/// The request is the caller's, freed with `ic_request_free`; one thread
/// uses it at a time.
///
/// # Safety
///
/// `request` points to writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_request_create(
  request: *mut *mut IcRequest,
) -> i32 {
  if request.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let handle = Box::into_raw(Box::new(Request::new())) as *mut IcRequest;
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { request.write(handle) };
    0
  })
}

/// Free a request, its rows with it.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed, freed here,
/// and not being run.
#[no_mangle]
pub unsafe extern "C" fn ic_request_free(request: *mut IcRequest) {
  if request.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(request as *mut Request) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// Drop every operation of a request, and the row pointers they gave.
///
/// # Safety
///
/// `request` is a request handle not yet freed, not being run.
#[no_mangle]
pub unsafe extern "C" fn ic_request_clear(request: *mut IcRequest) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { request_of(request) } {
    Some(r) => guarded(|| {
      r.clear();
      0
    }),
    None => err::IC_ERROR_NULL_POINTER,
  }
}

/// Add a read of `shape` to a request, `kind` one of the
/// `IC_READ_KIND_*` values, after the operations already there, and give
/// its number in `op` (which may be NULL). Its key row and attribute
/// row are zeroed, at the sizes the shape's records give.
///
/// Errors: `IC_ERROR_NOT_SUPPORTED` for a kind no define names.
///
/// # Safety
///
/// `request` is a request handle and `shape` a shape handle, neither yet
/// freed nor the request being run; a non-null `op` points to a
/// writable `uint32_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_request_add_read(
  request: *mut IcRequest,
  shape: *const IcOpShape,
  kind: u32,
  op: *mut u32,
) -> i32 {
  let read = match read_kind_from_c(kind) {
    Some(read) => read,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  // SAFETY: the caller's promise.
  let (r, sh) = match unsafe { (request_of(request), shape_of(shape)) } {
    (Some(r), Some(sh)) => (r, sh),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    r.push(KeyOp::read(sh, read));
    if !op.is_null() {
      // SAFETY: the caller's promise.
      unsafe { op.write(r.ops().len() as u32 - 1) };
    }
    0
  })
}

/// Add a write of `shape` to a request, `kind` one of the
/// `IC_WRITE_KIND_*` values: as `ic_request_add_read`.
///
/// Errors: `IC_ERROR_NOT_SUPPORTED` for a kind no define names.
///
/// # Safety
///
/// As `ic_request_add_read`.
#[no_mangle]
pub unsafe extern "C" fn ic_request_add_write(
  request: *mut IcRequest,
  shape: *const IcOpShape,
  kind: u32,
  op: *mut u32,
) -> i32 {
  let write = match write_kind_from_c(kind) {
    Some(write) => write,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  // SAFETY: the caller's promise.
  let (r, sh) = match unsafe { (request_of(request), shape_of(shape)) } {
    (Some(r), Some(sh)) => (r, sh),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    r.push(KeyOp::write(sh, write));
    if !op.is_null() {
      // SAFETY: the caller's promise.
      unsafe { op.write(r.ops().len() as u32 - 1) };
    }
    0
  })
}

/// What a failure of operation `op` does to the transaction: one of
/// `IC_ABORT_DEFAULT`, `IC_ABORT_ON_ERROR`, `IC_IGNORE_ERROR`.
///
/// Errors: `IC_ERROR_NOT_SUPPORTED` for a value no define names;
/// `IC_ERROR_NO_SUCH_FIELD` for an `op` the request does not have.
///
/// # Safety
///
/// `request` is a request handle not yet freed, not being run.
#[no_mangle]
pub unsafe extern "C" fn ic_request_set_abort_option(
  request: *mut IcRequest,
  op: u32,
  abort_option: u32,
) -> i32 {
  let option = match abort_from_c(abort_option) {
    Some(option) => option,
    None => return err::IC_ERROR_NOT_SUPPORTED,
  };
  // SAFETY: the caller's promise.
  let r = match unsafe { request_of(request) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  match r.ops_mut().get_mut(op as usize) {
    Some(found) => {
      found.set_abort_option(option);
      0
    }
    None => err::IC_ERROR_NO_SUCH_FIELD,
  }
}

/// Where the request's transaction starts: the hint's node or
/// partition, or with NULL, the default, by the first operation's key.
/// A partition is of the first operation's table, or of its index's own
/// table.
///
/// # Safety
///
/// `request` is a request handle not yet freed, not being run; a
/// non-null `hint` points to a readable `IC_TRANSACTION_HINT`.
#[no_mangle]
pub unsafe extern "C" fn ic_request_set_hint(
  request: *mut IcRequest,
  hint: *const IcTransactionHint,
) -> i32 {
  // SAFETY: the caller's promise.
  let r = match unsafe { request_of(request) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  let given = unsafe { hint.as_ref() };
  r.set_hint(given.map(|h| hint_from_c(*h)));
  0
}

/// Give up on the request if the executor has not admitted it within
/// `ms` milliseconds from now: it then fails with `IC_ERROR_TIMEOUT`,
/// unsent. 0 takes the deadline away. Set it before each run.
///
/// # Safety
///
/// `request` is a request handle not yet freed, not being run.
#[no_mangle]
pub unsafe extern "C" fn ic_request_set_timeout_ms(
  request: *mut IcRequest,
  ms: u32,
) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { request_of(request) } {
    Some(r) => {
      r.set_timeout_ms(ms);
      0
    }
    None => err::IC_ERROR_NULL_POINTER,
  }
}

/// How many operations the request has; 0 for a null handle.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_num_ops(request: *const IcRequest) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { request_ref(request) } {
    Some(r) => r.ops().len() as u32,
    None => 0,
  }
}

/// Operation `op`'s key row and its length, to fill before the request
/// is run. Valid until the request is freed or cleared, through every
/// run; NULL for an `op` the request does not have.
///
/// # Safety
///
/// `request` is a request handle not yet freed; a non-null `len` points
/// to a writable `size_t`. The row is not touched while the request runs.
#[no_mangle]
pub unsafe extern "C" fn ic_request_key_row(
  request: *mut IcRequest,
  op: u32,
  len: *mut usize,
) -> *mut u8 {
  let mut row: *mut u8 = std::ptr::null_mut();
  let mut row_len: usize = 0;
  // SAFETY: the caller's promise.
  if let Some(r) = unsafe { request_of(request) } {
    if let Some(found) = r.ops_mut().get_mut(op as usize) {
      let bytes = found.key_row_mut();
      row = bytes.as_mut_ptr();
      row_len = bytes.len();
    }
  }
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// Operation `op`'s attribute row and its length: a write's values, to
/// fill before the request is run, or the row a read brought, once it
/// has. Valid as `ic_request_key_row`'s.
///
/// # Safety
///
/// As `ic_request_key_row`.
#[no_mangle]
pub unsafe extern "C" fn ic_request_attr_row(
  request: *mut IcRequest,
  op: u32,
  len: *mut usize,
) -> *mut u8 {
  let mut row: *mut u8 = std::ptr::null_mut();
  let mut row_len: usize = 0;
  // SAFETY: the caller's promise.
  if let Some(r) = unsafe { request_of(request) } {
    if let Some(found) = r.ops_mut().get_mut(op as usize) {
      let bytes = found.attr_row_mut();
      row = bytes.as_mut_ptr();
      row_len = bytes.len();
    }
  }
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// Why operation `op` failed in the last run, or 0: for a read,
/// `IC_NDB_ERROR_NO_SUCH_ROW` when the row is not there, which leaves
/// the transaction committed. `IC_ERROR_NO_SUCH_FIELD` for an `op` the
/// request does not have.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_op_error(
  request: *const IcRequest,
  op: u32,
) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { op_ref(request, op) } {
    Some(found) => match found.error() {
      Some(e) => e.code,
      None => 0,
    },
    None => err::IC_ERROR_NO_SUCH_FIELD,
  }
}

/// What the data node said of operation `op`'s failure beyond its code,
/// or 0: for `IC_NDB_ERROR_UNIQUE_DUPLICATE`, the index's id.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_op_error_data(
  request: *const IcRequest,
  op: u32,
) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { op_ref(request, op) } {
    Some(found) => found.error_data(),
    None => 0,
  }
}

/// Bytes of row data operation `op`, a read, brought in the last run;
/// 0 for a write, or a row that was not there.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_op_result_len(
  request: *const IcRequest,
  op: u32,
) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { op_ref(request, op) } {
    Some(found) => found.result_len(),
    None => 0,
  }
}

/// Where the last run's transaction ended: `IC_COMMIT_STATE_COMMITTED`
/// or `IC_COMMIT_STATE_ROLLED_BACK`; `IC_COMMIT_STATE_UNKNOWN` for a
/// null handle.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_state(request: *const IcRequest) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { request_ref(request) } {
    Some(r) => commit_state_to_c(r.commit_state()),
    None => IC_COMMIT_STATE_UNKNOWN,
  }
}

/// Why the last run's transaction did not commit, or why its reads were
/// lost when a takeover committed it; 0 if neither.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_error(request: *const IcRequest) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { request_ref(request) } {
    Some(r) => match r.error() {
      Some(e) => e.code,
      None => 0,
    },
    None => err::IC_ERROR_NULL_POINTER,
  }
}

/// What the coordinator said of the transaction's error beyond its
/// code, or 0.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_error_data(
  request: *const IcRequest,
) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { request_ref(request) } {
    Some(r) => r.error_data(),
    None => 0,
  }
}

/// The global checkpoint the last run's commit went into, or 0.
///
/// # Safety
///
/// A non-null `request` is a request handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_request_gci(request: *const IcRequest) -> u64 {
  // SAFETY: the caller's promise.
  match unsafe { request_ref(request) } {
    Some(r) => r.gci(),
    None => 0,
  }
}

/// Run a request through the executor and wait for its transaction to
/// end: every operation defined together, the commit on the last, and
/// sent with whatever else the executor admits meanwhile. The request
/// comes back with the rows read and each operation's outcome
/// (`ic_request_op_error`).
///
/// Errors: the transaction's own, as the data node gives it, when it
/// did not commit: for a write, `IC_NDB_ERROR_NO_SUCH_ROW`,
/// `IC_NDB_ERROR_ROW_EXISTS`, `IC_NDB_ERROR_UNIQUE_DUPLICATE`; a failed
/// read alone leaves it committed and says so in its operation. Before
/// anything is sent: an operation's own refusal, as `ic_apid_conn_read_key`
/// and `ic_apid_conn_write_key` give them; `IC_ERROR_TIMEOUT` when the
/// request's deadline passed before the executor admitted it;
/// `IC_ERROR_EXECUTOR_STOPPED` when the executor is stopped, the request
/// left as it was; `IC_ERROR_NOT_SUPPORTED` when called on the executor's
/// own thread. `IC_ERROR_INTERNAL_PANIC` when the executor's thread failed:
/// the request is then empty, and its row pointers invalid. It blocks the
/// calling thread until the transaction ends, a wait the cluster's reply
/// timeout bounds (`IC_NDB_ERROR_NO_REPLY`), and waits for room when the
/// executor's list is full.
///
/// # Safety
///
/// `submitter` and `request` are handles not yet freed, each used by
/// this thread alone meanwhile.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_execute(
  submitter: *mut IcApidSubmitter,
  request: *mut IcRequest,
) -> i32 {
  // SAFETY: the caller's promise.
  let (s, r) = match unsafe { (submitter_of(submitter), request_of(request)) } {
    (Some(s), Some(r)) => (s, r),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| code_of(s.execute(r)))
}

// ---- Scans through an executor ----

/// The scan shape behind a handle.
///
/// # Safety
///
/// A non-null `shape` is a scan shape handle not yet freed.
unsafe fn scan_shape_of<'a>(
  shape: *const IcScanShape,
) -> Option<&'a Arc<ScanShape>> {
  // SAFETY: the caller's promise.
  unsafe { (shape as *const Arc<ScanShape>).as_ref() }
}

/// The reader behind a handle.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed, used by one
/// thread at a time.
unsafe fn reader_of<'a>(
  reader: *mut IcScanReader,
) -> Option<&'a mut ScanReader> {
  // SAFETY: the caller's promise.
  unsafe { (reader as *mut ScanReader).as_mut() }
}

/// The shape of scans for executors: a table, and the record its rows
/// are read into. Made once, and shared by every reader that uses it.
///
/// Errors: `IC_ERROR_KEY_RECORD` when the record is over another table,
/// or another version of it. The shape is the caller's, freed with
/// `ic_scan_shape_free`; any thread may use it, and a reader keeps what
/// it needs of it.
///
/// # Safety
///
/// `def` and `rec` are handles not yet freed; `shape` points to
/// writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_shape_create(
  def: *const IcTableDef,
  rec: *const IcRecord,
  shape: *mut *mut IcScanShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let (table, rec) = match unsafe { (table_of(def), record_of(rec)) } {
    (Some(table), Some(rec)) => (table, rec),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if shape.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match ScanShape::new(&table.def, rec) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcScanShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { shape.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// The shape of scans of an ordered index for executors: their ranges
/// laid out by `key_rec`, which must hold every column of the index,
/// and their rows read into `rec`, both over the index's table. Ranges
/// are added to each reader (`ic_scan_reader_add_range`), and the
/// reader merges an ordered run.
///
/// Errors: `IC_ERROR_NOT_SUPPORTED` for an index that is not ordered;
/// `IC_ERROR_KEY_RECORD` when a record is over another table, or the key
/// record lacks a column of the index. The shape is the caller's, freed
/// with `ic_scan_shape_free`.
///
/// # Safety
///
/// `index`, `key_rec` and `rec` are handles not yet freed; `shape`
/// points to writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_shape_create_index(
  index: *const IcIndexDef,
  key_rec: *const IcRecord,
  rec: *const IcRecord,
  shape: *mut *mut IcScanShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let (index, key_rec, rec) =
    match unsafe { (index_of(index), record_of(key_rec), record_of(rec)) } {
      (Some(index), Some(key_rec), Some(rec)) => (index, key_rec, rec),
      _ => return err::IC_ERROR_NULL_POINTER,
    };
  if shape.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match ScanShape::new_index(&index.def, key_rec, rec) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcScanShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { shape.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// Free a scan shape handle. Readers of it keep what they need.
///
/// # Safety
///
/// A non-null `shape` is a scan shape handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_shape_free(shape: *mut IcScanShape) {
  if shape.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(shape as *mut Arc<ScanShape>) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// A reader of scans of `shape`: a thread's end of a scan an executor
/// runs, with its row. Made once and run again and again
/// (`ic_apid_submitter_scan`).
///
/// The reader is the caller's, freed with `ic_scan_reader_free`; one
/// thread uses it at a time.
///
/// # Safety
///
/// `shape` is a scan shape handle not yet freed; `reader` points to
/// writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_create(
  shape: *const IcScanShape,
  reader: *mut *mut IcScanReader,
) -> i32 {
  // SAFETY: the caller's promise.
  let shape = match unsafe { scan_shape_of(shape) } {
    Some(shape) => shape,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if reader.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let made = ScanReader::new(shape);
    let handle = Box::into_raw(Box::new(made)) as *mut IcScanReader;
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { reader.write(handle) };
    0
  })
}

/// Free a reader. A run not over is ended without waiting: the executor
/// closes its scan.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed, freed here,
/// with no call on it under way.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_free(reader: *mut IcScanReader) {
  if reader.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(reader as *mut ScanReader) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// Run a scan on the executor, read through `reader`: the request goes
/// to the executor, waiting for room in a full list, and the rows come
/// with `ic_scan_reader_next_row`. Each batch is handed back to its
/// receiver once the reader has read it, whatever the lock, unless a
/// committed read asks for `read_ahead`.
///
/// Errors: `IC_ERROR_SCAN_ACTIVE` while the reader's last run is not
/// over; `IC_ERROR_NOT_SUPPORTED` for a transaction or a callback in
/// `args`, for `read_ahead` or `continuous` with a lock, for the two
/// together, and on the executor's own thread;
/// `IC_ERROR_EXECUTOR_STOPPED`; `IC_ERROR_INCONSISTENT_DATA` for `args`
/// not filled by `ic_scan_args_init`.
///
/// # Safety
///
/// `submitter` and `reader` are handles not yet freed, each used by this
/// thread alone meanwhile; `args` was filled by `ic_scan_args_init`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_scan(
  submitter: *mut IcApidSubmitter,
  reader: *mut IcScanReader,
  args: *const IcScanArgs,
) -> i32 {
  // SAFETY: the caller's promise.
  let (s, r) = match unsafe { (submitter_of(submitter), reader_of(reader)) } {
    (Some(s), Some(r)) => (s, r),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  let rust_args = match unsafe { scan_args_from_c(args) } {
    Ok(rust_args) => rust_args,
    Err(code) => return code,
  };
  guarded(|| code_of(s.scan(r, &rust_args)))
}

/// Read the next row into the reader's row (`ic_scan_reader_row`),
/// waiting for the executor while no batch is there; `more` is false
/// once the run has read to the end or was closed, or before any run.
///
/// Errors: the run's error once it has failed, its unread rows dropped.
///
/// # Safety
///
/// `reader` is a reader handle not yet freed, used by this thread alone;
/// `more` points to a writable `bool`.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_next_row(
  reader: *mut IcScanReader,
  more: *mut bool,
) -> i32 {
  if more.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let r = match unsafe { reader_of(reader) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  let found = match catch_unwind(AssertUnwindSafe(|| r.next_row())) {
    Ok(result) => Ok(result),
    Err(_) => Err(err::IC_ERROR_INTERNAL_PANIC),
  };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe { row_read(found, more) }
}

/// As `ic_scan_reader_next_row`, into `row`, `len` bytes of the
/// program's laid out by the shape's record, a copy less.
///
/// Errors: as `ic_scan_reader_next_row`; `IC_ERROR_RECORD_SIZE_TOO_BIG`
/// for a row shorter than the record's.
///
/// # Safety
///
/// As `ic_scan_reader_next_row`; `row` points to `len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_next_row_into(
  reader: *mut IcScanReader,
  row: *mut u8,
  len: usize,
  more: *mut bool,
) -> i32 {
  if more.is_null() || row.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  let r = match unsafe { reader_of(reader) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  let into = unsafe { row_arg_mut(row, len) };
  let found = match catch_unwind(AssertUnwindSafe(|| r.next_row_into(into))) {
    Ok(result) => Ok(result),
    Err(_) => Err(err::IC_ERROR_INTERNAL_PANIC),
  };
  // SAFETY: checked non-null above; the caller's promise.
  unsafe { row_read(found, more) }
}

/// The reader's row and its length: the row last read, laid out by the
/// shape's record. Valid until the reader is freed; NULL for a null
/// reader.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed; a non-null
/// `len` points to a writable `size_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_row(
  reader: *const IcScanReader,
  len: *mut usize,
) -> *const u8 {
  // SAFETY: the caller's promise.
  let found = unsafe { (reader as *const ScanReader).as_ref() };
  let (row, row_len) = match found {
    Some(r) => (r.row().as_ptr(), r.row().len()),
    None => (std::ptr::null(), 0),
  };
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// Add a range to the reader's next run, for a shape over an ordered
/// index, as `ic_apid_conn_add_scan_range` adds one on a connection.
///
/// Errors: as `ic_apid_conn_add_scan_range`; `IC_ERROR_NOT_SUPPORTED`
/// for a shape over a table.
///
/// # Safety
///
/// `reader` is a reader handle not yet freed, used by this thread alone;
/// `range` was filled by `ic_scan_range_init`; a non-null `low` or
/// `high` points to as many readable bytes as its length says.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_add_range(
  reader: *mut IcScanReader,
  range: *const IcScanRange,
  low: *const u8,
  low_len: usize,
  high: *const u8,
  high_len: usize,
) -> i32 {
  // SAFETY: the caller's promise.
  let r = match unsafe { reader_of(reader) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  let given = match unsafe { scan_range_from_c(range) } {
    Ok(given) => given,
    Err(code) => return code,
  };
  // SAFETY: the caller's promise.
  let (low_row, high_row) =
    unsafe { (row_arg(low, low_len), row_arg(high, high_len)) };
  guarded(|| code_of(r.add_range(&given, low_row, high_row)))
}

/// The number of the range the reader's row last read is in; 0 for a
/// null reader.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_range_no(
  reader: *const IcScanReader,
) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { (reader as *const ScanReader).as_ref() } {
    Some(r) => r.range_no(),
    None => 0,
  }
}

/// Rows read in the reader's run so far; 0 for a null reader.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_rows_read(
  reader: *const IcScanReader,
) -> u64 {
  // SAFETY: the caller's promise.
  match unsafe { (reader as *const ScanReader).as_ref() } {
    Some(r) => r.rows_read(),
    None => 0,
  }
}

/// Why the reader's run failed, or 0 if it read to the end, was closed,
/// or is still going.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_error(
  reader: *const IcScanReader,
) -> i32 {
  // SAFETY: the caller's promise.
  match unsafe { (reader as *const ScanReader).as_ref() } {
    Some(r) => match r.error() {
      Some(e) => e.code,
      None => 0,
    },
    None => err::IC_ERROR_NULL_POINTER,
  }
}

/// True once the reader's run is over, and before any run; true too for
/// a null reader.
///
/// # Safety
///
/// A non-null `reader` is a reader handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_is_done(
  reader: *const IcScanReader,
) -> bool {
  // SAFETY: the caller's promise.
  match unsafe { (reader as *const ScanReader).as_ref() } {
    Some(r) => r.is_done(),
    None => true,
  }
}

/// End the reader's run before it reads to the end, and wait for the
/// executor to finish it, so that the reader can run again. The rows
/// not read are dropped.
///
/// # Safety
///
/// `reader` is a reader handle not yet freed, used by this thread alone.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_close(
  reader: *mut IcScanReader,
) -> i32 {
  // SAFETY: the caller's promise.
  let r = match unsafe { reader_of(reader) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    r.close();
    0
  })
}

// ---- Programs ----

/// The builder behind a handle.
///
/// # Safety
///
/// A non-null `builder` is a builder handle not yet freed, used by one
/// thread at a time.
unsafe fn builder_of<'a>(
  builder: *mut IcProgramBuilder,
) -> Option<&'a mut ProgramBuilder> {
  // SAFETY: the caller's promise.
  unsafe { (builder as *mut ProgramBuilder).as_mut() }
}

/// The program behind a handle.
///
/// # Safety
///
/// A non-null `program` is a program handle not yet freed.
unsafe fn program_of<'a>(
  program: *const IcProgram,
) -> Option<&'a Arc<Program>> {
  // SAFETY: the caller's promise.
  unsafe { (program as *const Arc<Program>).as_ref() }
}

fn cmp_from_c(value: u32) -> Option<Cmp> {
  match value {
    IC_CMP_EQ => Some(Cmp::Eq),
    IC_CMP_NE => Some(Cmp::Ne),
    IC_CMP_LT => Some(Cmp::Lt),
    IC_CMP_LE => Some(Cmp::Le),
    IC_CMP_GT => Some(Cmp::Gt),
    IC_CMP_GE => Some(Cmp::Ge),
    _ => None,
  }
}

/// Run `f` on the builder a handle names, giving the id it makes, or
/// `IC_PROGRAM_NONE` for a null builder or a panic.
///
/// # Safety
///
/// As [`builder_of`].
unsafe fn on_builder<F: FnOnce(&mut ProgramBuilder) -> u32>(
  builder: *mut IcProgramBuilder,
  f: F,
) -> u32 {
  // SAFETY: the caller's promise.
  let found = match unsafe { builder_of(builder) } {
    Some(found) => found,
    None => return IC_PROGRAM_NONE,
  };
  catch_unwind(AssertUnwindSafe(|| f(found))).unwrap_or(IC_PROGRAM_NONE)
}

/// A builder of a program over a table (chapter 04, "Conditions and
/// assignments"): its values and conditions made bottom-up, each by a
/// function that gives its id in the builder, then compiled whole by
/// `ic_program_builder_finish`. What cannot be made, a column the table
/// lacks or an input beyond 15, is kept as the builder's error, which
/// the finish gives; a value or condition from a null builder or record,
/// or with an unknown comparison, is `IC_PROGRAM_NONE`, and using one
/// fails the finish with `IC_ERROR_BAD_PROGRAM`.
///
/// The builder is the caller's, freed with `ic_program_builder_free`;
/// one thread uses it at a time.
///
/// # Safety
///
/// `def` is a table handle not yet freed; `builder` points to writable
/// storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_program_builder_create(
  def: *const IcTableDef,
  builder: *mut *mut IcProgramBuilder,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if builder.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let made = ProgramBuilder::new(&table.def);
    let handle = Box::into_raw(Box::new(made)) as *mut IcProgramBuilder;
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { builder.write(handle) };
    0
  })
}

/// Free a builder handle. The programs it finished stay.
///
/// # Safety
///
/// A non-null `builder` is a builder handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_program_builder_free(
  builder: *mut IcProgramBuilder,
) {
  if builder.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(builder as *mut ProgramBuilder) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// The row's value of column `column`.
///
/// # Safety
///
/// A non-null `builder` is a builder handle not yet freed, used by this
/// thread alone.
#[no_mangle]
pub unsafe extern "C" fn ic_program_column(
  builder: *mut IcProgramBuilder,
  column: u32,
) -> IcProgramValue {
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |b| b.column(column).as_u32()) }
}

/// The value of column `column` in `row`, a row of `len` bytes laid out
/// by `rec`, a record over the builder's table: copied now, of that
/// column's type, NULL if the row has NULL there. A column compared with
/// it must be of one type, length, precision, scale and collation with
/// it.
///
/// # Safety
///
/// As `ic_program_column`; a non-null `rec` is a record handle not yet
/// freed; a non-null `row` points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_program_constant(
  builder: *mut IcProgramBuilder,
  rec: *const IcRecord,
  row: *const u8,
  len: usize,
  column: u32,
) -> IcProgramValue {
  // SAFETY: the caller's promise.
  let rec = match unsafe { record_of(rec) } {
    Some(rec) => rec,
    None => return IC_PROGRAM_NONE,
  };
  // SAFETY: the caller's promise.
  let bytes = unsafe { row_arg(row, len) };
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |b| b.constant(rec, bytes, column).as_u32()) }
}

/// An integer, as a sum adds it or a register holds it.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_integer(
  builder: *mut IcProgramBuilder,
  value: i64,
) -> IcProgramValue {
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |b| b.integer(value).as_u32()) }
}

/// Input `input`, 0 to 15, a signed integer each operation sets.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_input(
  builder: *mut IcProgramBuilder,
  input: u32,
) -> IcProgramValue {
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |b| b.input(input).as_u32()) }
}

/// `a + b`, NULL if either is.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_add(
  builder: *mut IcProgramBuilder,
  a: IcProgramValue,
  b: IcProgramValue,
) -> IcProgramValue {
  let (a, b) = (Value::from_u32(a), Value::from_u32(b));
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.add(a, b).as_u32()) }
}

/// `a − b`, NULL if either is.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_subtract(
  builder: *mut IcProgramBuilder,
  a: IcProgramValue,
  b: IcProgramValue,
) -> IcProgramValue {
  let (a, b) = (Value::from_u32(a), Value::from_u32(b));
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.subtract(a, b).as_u32()) }
}

/// `a` compared with `b` as `cmp`, one of the `IC_CMP_*` values, says;
/// unknown if either is NULL. `IC_PROGRAM_NONE` for another `cmp`.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_compare(
  builder: *mut IcProgramBuilder,
  cmp: u32,
  a: IcProgramValue,
  b: IcProgramValue,
) -> IcProgramCond {
  let cmp = match cmp_from_c(cmp) {
    Some(cmp) => cmp,
    None => return IC_PROGRAM_NONE,
  };
  let (a, b) = (Value::from_u32(a), Value::from_u32(b));
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.compare(cmp, a, b).as_u32()) }
}

/// True if `a` is NULL.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_is_null(
  builder: *mut IcProgramBuilder,
  a: IcProgramValue,
) -> IcProgramCond {
  let a = Value::from_u32(a);
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.is_null(a).as_u32()) }
}

/// True if `a` is not NULL.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_is_not_null(
  builder: *mut IcProgramBuilder,
  a: IcProgramValue,
) -> IcProgramCond {
  let a = Value::from_u32(a);
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.is_not_null(a).as_u32()) }
}

/// `x AND y`.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_and(
  builder: *mut IcProgramBuilder,
  x: IcProgramCond,
  y: IcProgramCond,
) -> IcProgramCond {
  let (x, y) = (Cond::from_u32(x), Cond::from_u32(y));
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.and(x, y).as_u32()) }
}

/// `x OR y`.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_or(
  builder: *mut IcProgramBuilder,
  x: IcProgramCond,
  y: IcProgramCond,
) -> IcProgramCond {
  let (x, y) = (Cond::from_u32(x), Cond::from_u32(y));
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.or(x, y).as_u32()) }
}

/// `NOT x`.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_not(
  builder: *mut IcProgramBuilder,
  x: IcProgramCond,
) -> IcProgramCond {
  let x = Cond::from_u32(x);
  // SAFETY: the caller's promise.
  unsafe { on_builder(builder, |pb| pb.not(x).as_u32()) }
}

/// Column `column` = `value`, once the program's condition has passed:
/// a constant of any type, or for an integer column of 32 or 64 bits a
/// value worked out in registers. A column is assigned once; a program
/// that assigns runs only on an update. What is wrong comes back from
/// the finish.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_assign(
  builder: *mut IcProgramBuilder,
  column: u32,
  value: IcProgramValue,
) -> i32 {
  // SAFETY: the caller's promise.
  let b = match unsafe { builder_of(builder) } {
    Some(b) => b,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    b.assign(column, Value::from_u32(value));
    0
  })
}

/// As `ic_program_assign`, only when `cond` is true.
///
/// # Safety
///
/// As `ic_program_column`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_assign_if(
  builder: *mut IcProgramBuilder,
  column: u32,
  value: IcProgramValue,
  cond: IcProgramCond,
) -> i32 {
  // SAFETY: the caller's promise.
  let b = match unsafe { builder_of(builder) } {
    Some(b) => b,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    b.assign_if(column, Value::from_u32(value), Cond::from_u32(cond));
    0
  })
}

/// The program's handle, made from a compiled program.
fn program_out(made: Arc<Program>, program: *mut *mut IcProgram) -> i32 {
  let handle = Box::into_raw(Box::new(made)) as *mut IcProgram;
  // SAFETY: checked non-null by the caller; the C caller's promise.
  unsafe { program.write(handle) };
  0
}

/// Compile the condition `top` with the builder's assignments into a
/// program, which any number of operations may run, each with its own
/// inputs. The builder stays, and may finish another.
///
/// Errors: the first error met in building; `IC_ERROR_NOT_COMPARABLE`
/// for a comparison or assignment of values of different types;
/// `IC_ERROR_BAD_PROGRAM` for a value or condition the builder did not
/// make, `IC_PROGRAM_NONE` among them; `IC_ERROR_NOT_SUPPORTED` for a
/// string or other non-integer worked out in registers;
/// `IC_ERROR_CONDITION_TOO_COMPLEX` for more than eight registers at
/// once, or a program too long; `IC_ERROR_FIELD_ALREADY_DEFINED` for a
/// column assigned twice. The program is the caller's, freed with
/// `ic_program_free`; any thread may use it.
///
/// # Safety
///
/// A non-null `builder` is a builder handle not yet freed, used by this
/// thread alone; `program` points to writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_program_builder_finish(
  builder: *mut IcProgramBuilder,
  top: IcProgramCond,
  program: *mut *mut IcProgram,
) -> i32 {
  // SAFETY: the caller's promise.
  let b = match unsafe { builder_of(builder) } {
    Some(b) => b,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if program.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match b.finish(Some(Cond::from_u32(top))) {
    Ok(made) => program_out(made, program),
    Err(e) => e.code,
  })
}

/// As `ic_program_builder_finish`, for a program with no condition,
/// which only assigns.
///
/// # Safety
///
/// As `ic_program_builder_finish`.
#[no_mangle]
pub unsafe extern "C" fn ic_program_builder_finish_no_condition(
  builder: *mut IcProgramBuilder,
  program: *mut *mut IcProgram,
) -> i32 {
  // SAFETY: the caller's promise.
  let b = match unsafe { builder_of(builder) } {
    Some(b) => b,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if program.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match b.finish(None) {
    Ok(made) => program_out(made, program),
    Err(e) => e.code,
  })
}

/// Free a program handle. The queries, scans and shapes that run it
/// keep it.
///
/// # Safety
///
/// A non-null `program` is a program handle not yet freed, freed here.
#[no_mangle]
pub unsafe extern "C" fn ic_program_free(program: *mut IcProgram) {
  if program.is_null() {
    return;
  }
  // SAFETY: the caller's promise; the handle is the box made for it.
  let owned = unsafe { Box::from_raw(program as *mut Arc<Program>) };
  let _ = guarded(move || {
    drop(owned);
    0
  });
}

/// True if the program assigns columns, which only an update may run;
/// false for a null program.
///
/// # Safety
///
/// A non-null `program` is a program handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_program_writes(program: *const IcProgram) -> bool {
  // SAFETY: the caller's promise.
  match unsafe { program_of(program) } {
    Some(p) => p.writes(),
    None => false,
  }
}

/// The inputs the program reads, a bit each, input 0 the lowest; 0 for a
/// null program.
///
/// # Safety
///
/// A non-null `program` is a program handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_program_inputs(program: *const IcProgram) -> u32 {
  // SAFETY: the caller's promise.
  match unsafe { program_of(program) } {
    Some(p) => p.inputs(),
    None => 0,
  }
}

/// Run `program` with every execution of the query defined from now on,
/// or none for a NULL `program`: on a read, an update or a delete by
/// primary key, and a program that assigns only on an update, which is
/// checked as each is defined (`IC_ERROR_NOT_SUPPORTED`). A row the
/// program refuses fails the query with `IC_NDB_ERROR_CONDITION` where it
/// aborts on error, rolling the transaction back, and with
/// `IC_NDB_ERROR_NO_SUCH_ROW` where it ignores errors.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` for a program of another table;
/// `IC_ERROR_TRANSACTION_ACTIVE` while the query is defined or sent;
/// `IC_ERROR_NO_SUCH_FIELD` for an id that names no query.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `program` is
/// a program handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_set_program(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  program: *const IcProgram,
) -> i32 {
  // SAFETY: the caller's promise.
  let program = unsafe { program_of(program) };
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| match c.query_mut(QueryId::from_u32(query)) {
      Some(q) => q.set_program(program),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    })
  }
}

/// Set input `input`, 0 to 15, of the query's program to `value`, for
/// the executions defined from now on; each holds what it was last set
/// to, 0 at first.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` beyond 15; `IC_ERROR_NO_SUCH_FIELD`
/// for an id that names no query.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_set_input(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  input: u32,
  value: i64,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| match c.query_mut(QueryId::from_u32(query)) {
      Some(q) => q.set_input(input, value),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    })
  }
}

/// For an update, read the columns of `rec` after its program, into the
/// row `ic_apid_query_after_row` gives, or none for a NULL `rec`. Only
/// an update takes it, which is checked as it is defined.
///
/// Errors: `IC_ERROR_KEY_RECORD` for a record over another table;
/// `IC_ERROR_TRANSACTION_ACTIVE` while the query is defined or sent;
/// `IC_ERROR_NO_SUCH_FIELD` for an id that names no query.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `rec` is a
/// record handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_set_reads_after(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  rec: *const IcRecord,
) -> i32 {
  // SAFETY: the caller's promise.
  let rec = unsafe { record_of(rec) };
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| match c.query_mut(QueryId::from_u32(query)) {
      Some(q) => q.set_reads_after(rec),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    })
  }
}

/// What the query's update read after its program, and its length, once
/// the query has completed: laid out by the record given to
/// `ic_apid_query_set_reads_after`. Valid until the query is freed or
/// given another such record; NULL without one, or for an id that names
/// no query.
///
/// # Safety
///
/// As `ic_apid_query_key_row`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_query_after_row(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  len: *mut usize,
) -> *const u8 {
  // SAFETY: the caller's promise.
  let found = unsafe {
    on_conn(conn, |c| match c.query(QueryId::from_u32(query)) {
      Some(q) if !q.after_row().is_empty() => {
        (q.after_row().as_ptr(), q.after_row().len())
      }
      _ => (std::ptr::null(), 0),
    })
  };
  let (row, row_len) = found.unwrap_or((std::ptr::null(), 0));
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// Filter the scan's runs from the next on by `program`, or by none for
/// a NULL `program`: a row it refuses is passed over at the data node.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` for a program of another table;
/// `IC_ERROR_NOT_SUPPORTED` for one that assigns; `IC_ERROR_SCAN_ACTIVE`
/// while a run is on; `IC_ERROR_NO_SUCH_FIELD` for an id that names no
/// scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed; a non-null `program` is
/// a program handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_set_scan_program(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  program: *const IcProgram,
) -> i32 {
  // SAFETY: the caller's promise.
  let program = unsafe { program_of(program) };
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| {
      c.set_scan_program(ScanId::from_u32(scan), program)
    })
  }
}

/// Set input `input`, 0 to 15, of the scan's program to `value`, for its
/// runs from the next on; each holds what it was last set to, 0 at first.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` beyond 15; `IC_ERROR_SCAN_ACTIVE`
/// while a run is on; `IC_ERROR_NO_SUCH_FIELD` for an id that names no
/// scan.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_set_scan_input(
  conn: *mut IcApidConnection,
  scan: IcScanId,
  input: u32,
  value: i64,
) -> i32 {
  // SAFETY: the caller's promise.
  unsafe {
    conn_code(conn, |c| {
      c.set_scan_input(ScanId::from_u32(scan), input, value)
    })
  }
}

/// A shape of the same records as `shape` whose operations run
/// `program`, as `ic_apid_query_set_program` has a query run it. A plain
/// operation and one with a program are of two shapes.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` for a program of another table;
/// `IC_ERROR_NOT_SUPPORTED` for a shape through a unique index. The new
/// shape is the caller's, freed with `ic_op_shape_free`, as is `shape`.
///
/// # Safety
///
/// `shape` and `program` are handles not yet freed; `out` points to
/// writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_op_shape_with_program(
  shape: *const IcOpShape,
  program: *const IcProgram,
  out: *mut *mut IcOpShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let handles = unsafe { (shape_of(shape), program_of(program)) };
  let (shape, program) = match handles {
    (Some(shape), Some(program)) => (shape, program),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if out.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match shape.with_program(program) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcOpShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { out.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// A shape of the same records as `shape` whose updates read the
/// columns of `rec` after their program, into each operation's row
/// `ic_request_after_row` gives.
///
/// Errors: `IC_ERROR_KEY_RECORD` for a record over another table;
/// `IC_ERROR_NOT_SUPPORTED` for a shape through a unique index. The new
/// shape is the caller's, freed with `ic_op_shape_free`.
///
/// # Safety
///
/// `shape` and `rec` are handles not yet freed; `out` points to writable
/// storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_op_shape_reading_after(
  shape: *const IcOpShape,
  rec: *const IcRecord,
  out: *mut *mut IcOpShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let (shape, rec) = match unsafe { (shape_of(shape), record_of(rec)) } {
    (Some(shape), Some(rec)) => (shape, rec),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if out.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match shape.reading_after(rec) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcOpShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { out.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// A shape of the same table, index and record as `shape` whose scans
/// are filtered by `program`, as `ic_apid_conn_set_scan_program` filters
/// a scan.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` for a program of another table;
/// `IC_ERROR_NOT_SUPPORTED` for one that assigns. The new shape is the
/// caller's, freed with `ic_scan_shape_free`.
///
/// # Safety
///
/// `shape` and `program` are handles not yet freed; `out` points to
/// writable storage for a pointer.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_shape_with_program(
  shape: *const IcScanShape,
  program: *const IcProgram,
  out: *mut *mut IcScanShape,
) -> i32 {
  // SAFETY: the caller's promise.
  let handles = unsafe { (scan_shape_of(shape), program_of(program)) };
  let (shape, program) = match handles {
    (Some(shape), Some(program)) => (shape, program),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if out.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| match shape.with_program(program) {
    Ok(made) => {
      let handle = Box::into_raw(Box::new(made)) as *mut IcScanShape;
      // SAFETY: checked non-null above; the caller's promise.
      unsafe { out.write(handle) };
      0
    }
    Err(e) => e.code,
  })
}

/// Set input `input`, 0 to 15, of operation `op`'s program to `value`;
/// each holds what it was last set to, 0 at first.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` beyond 15; `IC_ERROR_NO_SUCH_FIELD`
/// for an `op` the request does not have.
///
/// # Safety
///
/// `request` is a request handle not yet freed, used by this thread
/// alone, and not running.
#[no_mangle]
pub unsafe extern "C" fn ic_request_set_input(
  request: *mut IcRequest,
  op: u32,
  input: u32,
  value: i64,
) -> i32 {
  // SAFETY: the caller's promise.
  let r = match unsafe { request_of(request) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  match r.ops_mut().get_mut(op as usize) {
    Some(found) => code_of(found.set_input(input, value)),
    None => err::IC_ERROR_NO_SUCH_FIELD,
  }
}

/// What operation `op`'s update read after its program in the last run,
/// and its length, laid out by its shape's record for that
/// (`ic_op_shape_reading_after`). Valid as `ic_request_key_row`'s; NULL
/// without such a record, or for an `op` the request does not have.
///
/// # Safety
///
/// As `ic_request_key_row`.
#[no_mangle]
pub unsafe extern "C" fn ic_request_after_row(
  request: *const IcRequest,
  op: u32,
  len: *mut usize,
) -> *const u8 {
  let mut row: *const u8 = std::ptr::null();
  let mut row_len: usize = 0;
  // SAFETY: the caller's promise.
  if let Some(found) = unsafe { op_ref(request, op) } {
    if !found.after_row().is_empty() {
      row = found.after_row().as_ptr();
      row_len = found.after_row().len();
    }
  }
  if !len.is_null() {
    // SAFETY: the caller's promise.
    unsafe { len.write(row_len) };
  }
  row
}

/// Set input `input`, 0 to 15, of the reader's shape's program to
/// `value`, for its runs from the next on; each holds what it was last
/// set to, 0 at first.
///
/// Errors: `IC_ERROR_BAD_PROGRAM` beyond 15.
///
/// # Safety
///
/// `reader` is a reader handle not yet freed, used by this thread alone.
#[no_mangle]
pub unsafe extern "C" fn ic_scan_reader_set_input(
  reader: *mut IcScanReader,
  input: u32,
  value: i64,
) -> i32 {
  // SAFETY: the caller's promise.
  let r = match unsafe { reader_of(reader) } {
    Some(r) => r,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  code_of(r.set_input(input, value))
}

// ---- AUTO_INCREMENT ----

/// What a call that gives a value gives C: 0 with the value in `out`,
/// or the error's code.
///
/// # Safety
///
/// `out` points to a writable `uint64_t`.
unsafe fn give_value(result: Result<u64, IcError>, out: *mut u64) -> i32 {
  match result {
    Ok(value) => {
      // SAFETY: the caller's promise.
      unsafe { out.write(value) };
      0
    }
    Err(e) => e.code,
  }
}

/// The next value of the table's AUTO_INCREMENT column, into `value`,
/// for the program to put in its row (chapter 04, "AUTO_INCREMENT"):
/// from the range the process holds for the table, shared by its
/// connections and executors, or, when that is used up, from a refill,
/// a round trip on a connection the library keeps for the counters.
/// Unique across every client of the cluster, MySQL servers included,
/// not in order across them; values not used are gaps. Not checked
/// against the column's type.
///
/// Errors: `IC_ERROR_NO_AUTO_INCREMENT` for a table without such a
/// column; a temporary error still there after 3.6 s of trying again.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed, used by this thread
/// alone; `def` is a table handle not yet freed; `value` points to a
/// writable `uint64_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_auto_increment(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  value: *mut u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if value.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.auto_increment(&table.def)) } {
    // SAFETY: checked non-null above; the caller's promise.
    Ok(result) => unsafe { give_value(result, value) },
    Err(code) => code,
  }
}

/// The first of `n` values a step apart, reserved together, into
/// `first`, for an insert of many rows: from the range if it holds
/// them all, otherwise from a refill of at least `n`, what the range
/// held then left as a gap. Zero counts as one.
///
/// Errors: as `ic_apid_conn_auto_increment`.
///
/// # Safety
///
/// As `ic_apid_conn_auto_increment`, with `first` for `value`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_auto_increment_block(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  n: u32,
  first: *mut u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if first.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.auto_increment_block(&table.def, n)) } {
    // SAFETY: checked non-null above; the caller's promise.
    Ok(result) => unsafe { give_value(result, first) },
    Err(code) => code,
  }
}

/// After an insert with a value of the program's own in the
/// AUTO_INCREMENT column: the counter is moved past `value`, only ever
/// up, so that no client hands it out. A value within the range the
/// process holds moves only the range; one below what it handed out or
/// saw moves nothing.
///
/// Errors: as `ic_apid_conn_auto_increment`; `IC_ERROR_NOT_SUPPORTED`
/// for a value of 2^63 - 1 or more that would move the counter.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed, used by this thread
/// alone; `def` is a table handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_auto_increment_seen(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  value: u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  // SAFETY: the caller's promise.
  unsafe { conn_code(conn, |c| c.auto_increment_seen(&table.def, value)) }
}

/// The counter as it stands, into `next`: the next value no client has
/// reserved, by one read of the counter, nothing reserved and the
/// range not touched. Only advisory: not the next value this process
/// hands out from its range, and out of date once any client refills.
///
/// Errors: as `ic_apid_conn_auto_increment`.
///
/// # Safety
///
/// As `ic_apid_conn_auto_increment`, with `next` for `value`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_conn_auto_increment_peek(
  conn: *mut IcApidConnection,
  def: *const IcTableDef,
  next: *mut u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if next.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  // SAFETY: the caller's promise.
  match unsafe { on_conn(conn, |c| c.auto_increment_peek(&table.def)) } {
    // SAFETY: checked non-null above; the caller's promise.
    Ok(result) => unsafe { give_value(result, next) },
    Err(code) => code,
  }
}

/// As `ic_apid_conn_auto_increment`, for a thread with no connection of
/// its own. The values come from the same range, the refill on the
/// same connection for the counters, never through the executor; so
/// the call may be made from a callback on the executor's own thread,
/// which a refill holds for its round trip.
///
/// Errors: as `ic_apid_conn_auto_increment`; `IC_ERROR_EXECUTOR_STOPPED`
/// once the executor has stopped.
///
/// # Safety
///
/// `submitter` is a submitter handle not yet freed, used by this thread
/// alone; `def` is a table handle not yet freed; `value` points to a
/// writable `uint64_t`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_auto_increment(
  submitter: *mut IcApidSubmitter,
  def: *const IcTableDef,
  value: *mut u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let (s, table) = match unsafe { (submitter_of(submitter), table_of(def)) } {
    (Some(s), Some(table)) => (s, table),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if value.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let result = s.auto_increment(&table.def);
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { give_value(result, value) }
  })
}

/// As `ic_apid_conn_auto_increment_block`, through a submitter.
///
/// Errors: as `ic_apid_submitter_auto_increment`.
///
/// # Safety
///
/// As `ic_apid_submitter_auto_increment`, with `first` for `value`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_auto_increment_block(
  submitter: *mut IcApidSubmitter,
  def: *const IcTableDef,
  n: u32,
  first: *mut u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let (s, table) = match unsafe { (submitter_of(submitter), table_of(def)) } {
    (Some(s), Some(table)) => (s, table),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if first.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let result = s.auto_increment_block(&table.def, n);
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { give_value(result, first) }
  })
}

/// As `ic_apid_conn_auto_increment_seen`, through a submitter.
///
/// Errors: as `ic_apid_conn_auto_increment_seen`;
/// `IC_ERROR_EXECUTOR_STOPPED` once the executor has stopped.
///
/// # Safety
///
/// `submitter` is a submitter handle not yet freed, used by this thread
/// alone; `def` is a table handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_auto_increment_seen(
  submitter: *mut IcApidSubmitter,
  def: *const IcTableDef,
  value: u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let (s, table) = match unsafe { (submitter_of(submitter), table_of(def)) } {
    (Some(s), Some(table)) => (s, table),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| code_of(s.auto_increment_seen(&table.def, value)))
}

/// As `ic_apid_conn_auto_increment_peek`, through a submitter.
///
/// Errors: as `ic_apid_submitter_auto_increment`.
///
/// # Safety
///
/// As `ic_apid_submitter_auto_increment`, with `next` for `value`.
#[no_mangle]
pub unsafe extern "C" fn ic_apid_submitter_auto_increment_peek(
  submitter: *mut IcApidSubmitter,
  def: *const IcTableDef,
  next: *mut u64,
) -> i32 {
  // SAFETY: the caller's promise.
  let (s, table) = match unsafe { (submitter_of(submitter), table_of(def)) } {
    (Some(s), Some(table)) => (s, table),
    _ => return err::IC_ERROR_NULL_POINTER,
  };
  if next.is_null() {
    return err::IC_ERROR_NULL_POINTER;
  }
  guarded(|| {
    let result = s.auto_increment_peek(&table.def);
    // SAFETY: checked non-null above; the caller's promise.
    unsafe { give_value(result, next) }
  })
}

/// How many AUTO_INCREMENT values a refill reserves for this table from
/// now on, in every thread of the process, whichever handle it was
/// bound through; 0 goes back to the global's `auto_increment_prefetch`.
/// A table without an AUTO_INCREMENT column has nothing to set.
///
/// # Safety
///
/// `def` is a table handle not yet freed.
#[no_mangle]
pub unsafe extern "C" fn ic_table_def_set_auto_increment_prefetch(
  def: *const IcTableDef,
  prefetch: u32,
) -> i32 {
  // SAFETY: the caller's promise.
  let table = match unsafe { table_of(def) } {
    Some(table) => table,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  guarded(|| {
    table.def.set_auto_increment_prefetch(prefetch);
    0
  })
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn the_c_values_are_the_rust_ones() {
    assert_eq!(IC_ERROR_INTERNAL_PANIC, err::IC_ERROR_INTERNAL_PANIC);
    assert_eq!(IC_ERROR_NULL_POINTER, err::IC_ERROR_NULL_POINTER);
    assert_eq!(IC_ERROR_BAD_TEXT, err::IC_ERROR_BAD_TEXT);
    let pairs: [(i32, i32); 25] = [
      (
        IC_ERROR_PARSE_CONNECTSTRING,
        err::IC_ERROR_PARSE_CONNECTSTRING,
      ),
      (IC_ERROR_CONNECT_TIMEOUT, err::IC_ERROR_CONNECT_TIMEOUT),
      (IC_ERROR_NODEID_IN_USE, err::IC_ERROR_NODEID_IN_USE),
      (
        IC_ERROR_MGM_SERVER_REFUSED,
        err::IC_ERROR_MGM_SERVER_REFUSED,
      ),
      (
        IC_ERROR_TOO_MANY_USER_THREADS,
        err::IC_ERROR_TOO_MANY_USER_THREADS,
      ),
      (IC_ERROR_LINK_LOST, err::IC_ERROR_LINK_LOST),
      (
        IC_ERROR_NO_STARTED_DATA_NODE,
        err::IC_ERROR_NO_STARTED_DATA_NODE,
      ),
      (IC_ERROR_TIMEOUT, err::IC_ERROR_TIMEOUT),
      (IC_ERROR_NO_SUCH_TABLE, err::IC_ERROR_NO_SUCH_TABLE),
      (IC_ERROR_NO_SUCH_INDEX, err::IC_ERROR_NO_SUCH_INDEX),
      (IC_ERROR_DICT_REFUSED, err::IC_ERROR_DICT_REFUSED),
      (
        IC_ERROR_TABLE_KEEPS_CHANGING,
        err::IC_ERROR_TABLE_KEEPS_CHANGING,
      ),
      (IC_ERROR_INDEX_NOT_ONLINE, err::IC_ERROR_INDEX_NOT_ONLINE),
      (IC_ERROR_NO_SUCH_FIELD, err::IC_ERROR_NO_SUCH_FIELD),
      (IC_ERROR_TOO_MANY_FIELDS, err::IC_ERROR_TOO_MANY_FIELDS),
      (
        IC_ERROR_DUPLICATE_FIELD_IDS,
        err::IC_ERROR_DUPLICATE_FIELD_IDS,
      ),
      (IC_ERROR_RECORD_LAYOUT, err::IC_ERROR_RECORD_LAYOUT),
      (IC_ERROR_NO_NULL_BIT, err::IC_ERROR_NO_NULL_BIT),
      (IC_ERROR_KEY_RECORD, err::IC_ERROR_KEY_RECORD),
      (
        IC_ERROR_TRANSACTION_ACTIVE,
        err::IC_ERROR_TRANSACTION_ACTIVE,
      ),
      (IC_ERROR_NOT_SUPPORTED, err::IC_ERROR_NOT_SUPPORTED),
      (IC_ERROR_INCONSISTENT_DATA, err::IC_ERROR_INCONSISTENT_DATA),
      (IC_ERROR_DELETE_WITH_BLOBS, err::IC_ERROR_DELETE_WITH_BLOBS),
      (
        IC_ERROR_INDEXED_COLUMN_UPDATE,
        err::IC_ERROR_INDEXED_COLUMN_UPDATE,
      ),
      (IC_ERROR_KEY_TOO_LONG, err::IC_ERROR_KEY_TOO_LONG),
    ];
    for (ours, theirs) in pairs {
      assert_eq!(ours, theirs);
    }
    assert_eq!(
      IC_ERROR_RECORD_SIZE_TOO_BIG,
      err::IC_ERROR_RECORD_SIZE_TOO_BIG
    );
    assert_eq!(IC_ERROR_EXECUTOR_BUSY, err::IC_ERROR_EXECUTOR_BUSY);
    assert_eq!(IC_ERROR_EXECUTOR_STOPPED, err::IC_ERROR_EXECUTOR_STOPPED);
    assert_eq!(IC_ERROR_SCAN_ACTIVE, err::IC_ERROR_SCAN_ACTIVE);
    assert_eq!(IC_ERROR_NO_SCAN_ROW, err::IC_ERROR_NO_SCAN_ROW);
    assert_eq!(IC_ERROR_BAD_RANGE, err::IC_ERROR_BAD_RANGE);
    assert_eq!(
      IC_ERROR_CONDITION_TOO_COMPLEX,
      err::IC_ERROR_CONDITION_TOO_COMPLEX
    );
    assert_eq!(IC_ERROR_NOT_COMPARABLE, err::IC_ERROR_NOT_COMPARABLE);
    assert_eq!(IC_ERROR_BAD_PROGRAM, err::IC_ERROR_BAD_PROGRAM);
    assert_eq!(IC_ERROR_NO_AUTO_INCREMENT, err::IC_ERROR_NO_AUTO_INCREMENT);
    assert_eq!(
      IC_NDB_ERROR_CONDITION as u16,
      ic_signals::interp::IC_REFUSE_CONDITION
    );
    assert!(IcError::new(IC_NDB_ERROR_CONDITION).is_ndb_error());
    assert!(!IcError::new(IC_NDB_ERROR_CONDITION).is_temporary());
    assert_eq!(IC_PROGRAM_INPUTS, ic_apid::program::IC_PROGRAM_INPUTS);
    assert_eq!(cmp_from_c(IC_CMP_LE), Some(Cmp::Le));
    assert_eq!(cmp_from_c(6), None);
    let scan_errors: [(i32, i32); 4] = [
      (
        IC_NDB_ERROR_SCAN_NODE_FAILURE,
        ic_apid::scan::IC_NDB_ERROR_SCAN_NODE_FAILURE,
      ),
      (
        IC_NDB_ERROR_TOO_MANY_SCANS,
        ic_apid::scan::IC_NDB_ERROR_TOO_MANY_SCANS,
      ),
      (
        IC_NDB_ERROR_SCAN_LOCK_TIMEOUT,
        ic_apid::scan::IC_NDB_ERROR_SCAN_LOCK_TIMEOUT,
      ),
      (
        IC_NDB_ERROR_SCAN_IDLE,
        ic_apid::scan::IC_NDB_ERROR_SCAN_IDLE,
      ),
    ];
    for (ours, theirs) in scan_errors {
      assert_eq!(ours, theirs);
      assert!(IcError::new(ours).is_ndb_error(), "{} is NDB's", ours);
      assert!(IcError::new(ours).is_temporary(), "{} is temporary", ours);
    }
    assert_eq!(
      scan_lock_from_c(IC_SCAN_LOCK_SHARED),
      Some(ScanLock::Shared)
    );
    assert_eq!(scan_lock_from_c(3), None);
    assert_eq!(scan_state_to_c(ScanState::Done), IC_SCAN_DONE);
    assert!(IcError::new(IC_ERROR_EXECUTOR_BUSY).is_temporary());
    assert_eq!(
      IC_NDB_ERROR_NULL_KEY,
      ic_apid::row_codec::IC_NDB_ERROR_NULL_KEY
    );
    assert_eq!(
      IC_NDB_ERROR_NODE_FAILURE_ABORT,
      ic_apid::transaction::IC_NDB_ERROR_NODE_FAILURE_ABORT
    );
    for code in [IC_NDB_ERROR_UNIQUE_DUPLICATE, IC_NDB_ERROR_LOCK_TIMEOUT] {
      assert!(IcError::new(code).is_ndb_error(), "{} is NDB's", code);
    }
    assert!(IcError::new(IC_NDB_ERROR_LOCK_TIMEOUT).is_temporary());
    assert_eq!(IC_NO_NULL_BIT, ic_apid::record::IC_NO_NULL_BIT);
    assert_eq!(
      IC_NDB_ERROR_NO_SUCH_ROW as u32,
      key_op::IC_NDB_ERROR_NO_SUCH_ROW
    );
    assert_eq!(
      IC_NDB_ERROR_ROW_EXISTS as u32,
      key_op::IC_NDB_ERROR_ROW_EXISTS
    );
    assert_eq!(
      IC_NDB_ERROR_NO_REPLY,
      ic_apid::transaction::IC_NDB_ERROR_NO_REPLY
    );
    assert_eq!(IC_READ_KIND_COMMITTED, ReadKind::Committed as u32);
    assert_eq!(IC_WRITE_KIND_DELETE, WriteKind::Delete as u32);
    assert_eq!(IC_COMMIT_STATE_ROLLED_BACK, CommitState::RolledBack as u32);
    assert_eq!(
      IC_ERROR_SEVERITY_TEMPORARY,
      ic_port::err::ErrorSeverity::Temporary as u32
    );
    assert_eq!(
      IC_ERROR_CATEGORY_EXTERNAL,
      ic_port::err::ErrorCategory::External as u32
    );
  }

  #[test]
  fn null_handles_are_refused_not_followed() {
    let mut q: IcQueryId = 0;
    let mut len: usize = 7;
    // SAFETY: every pointer given is null or to a local.
    unsafe {
      assert_eq!(
        ic_apid_query_free(std::ptr::null_mut(), 1),
        IC_ERROR_NULL_POINTER
      );
      assert!(
        ic_apid_query_key_row(std::ptr::null_mut(), 1, &mut len).is_null()
      );
      assert_eq!(len, 0);
      assert!(!ic_apid_conn_get_next_executed_query(
        std::ptr::null_mut(),
        &mut q
      ));
      assert_eq!(ic_table_def_num_fields(std::ptr::null()), 0);
      assert!(ic_table_def_name(std::ptr::null()).is_null());
      assert_eq!(
        ic_record_create_default(
          std::ptr::null(),
          std::ptr::null(),
          0,
          std::ptr::null_mut()
        ),
        IC_ERROR_NULL_POINTER
      );
      assert!(ic_apid_transaction_is_done(std::ptr::null_mut(), 1));
      ic_apid_conn_free(std::ptr::null_mut());
      ic_apid_global_free(std::ptr::null_mut());
      assert_eq!(
        ic_apid_submitter_execute(std::ptr::null_mut(), std::ptr::null_mut()),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(ic_request_num_ops(std::ptr::null()), 0);
      assert_eq!(ic_request_state(std::ptr::null()), IC_COMMIT_STATE_UNKNOWN);
      ic_apid_executor_free(std::ptr::null_mut());
      ic_apid_submitter_free(std::ptr::null_mut());
      ic_op_shape_free(std::ptr::null_mut());
      ic_request_free(std::ptr::null_mut());
      let mut more = true;
      let mut batch: IcScanBatchId = 0;
      assert_eq!(
        ic_apid_scan_free(std::ptr::null_mut(), 1),
        IC_ERROR_NULL_POINTER
      );
      assert!(ic_apid_scan_is_done(std::ptr::null_mut(), 1));
      assert_eq!(ic_apid_scan_state(std::ptr::null_mut(), 1), IC_SCAN_UNKNOWN);
      assert!(ic_apid_scan_row(std::ptr::null_mut(), 1, &mut len).is_null());
      assert_eq!(len, 0);
      assert!(!ic_apid_conn_get_next_scan_batch(
        std::ptr::null_mut(),
        &mut batch
      ));
      assert_eq!(
        ic_apid_conn_next_row(std::ptr::null_mut(), 1, &mut more),
        IC_ERROR_NULL_POINTER
      );
      assert!(!more);
      assert_eq!(
        ic_apid_conn_next_row(std::ptr::null_mut(), 1, std::ptr::null_mut()),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_apid_conn_start_scan(std::ptr::null_mut(), 1, std::ptr::null()),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_scan_reader_create(std::ptr::null(), std::ptr::null_mut()),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_scan_reader_next_row(std::ptr::null_mut(), &mut more),
        IC_ERROR_NULL_POINTER
      );
      assert!(ic_scan_reader_row(std::ptr::null(), &mut len).is_null());
      assert!(ic_scan_reader_is_done(std::ptr::null()));
      assert_eq!(
        ic_scan_reader_close(std::ptr::null_mut()),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_apid_submitter_scan(
          std::ptr::null_mut(),
          std::ptr::null_mut(),
          std::ptr::null()
        ),
        IC_ERROR_NULL_POINTER
      );
      ic_scan_reader_free(std::ptr::null_mut());
      ic_scan_shape_free(std::ptr::null_mut());
      let mut value: u64 = 7;
      assert_eq!(
        ic_apid_conn_auto_increment(
          std::ptr::null_mut(),
          std::ptr::null(),
          &mut value
        ),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_apid_conn_auto_increment_seen(
          std::ptr::null_mut(),
          std::ptr::null(),
          5
        ),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_apid_submitter_auto_increment_block(
          std::ptr::null_mut(),
          std::ptr::null(),
          3,
          &mut value
        ),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(
        ic_apid_submitter_auto_increment_peek(
          std::ptr::null_mut(),
          std::ptr::null(),
          &mut value
        ),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(value, 7, "nothing written");
      assert_eq!(
        ic_table_def_set_auto_increment_prefetch(std::ptr::null(), 10),
        IC_ERROR_NULL_POINTER
      );
    }
  }

  #[test]
  fn scan_args_start_from_the_defaults_and_cross_to_rust() {
    let mut args = std::mem::MaybeUninit::<IcScanArgs>::uninit();
    // SAFETY: the init writes the whole struct, of the size given.
    let mut args = unsafe {
      ic_scan_args_init(args.as_mut_ptr(), std::mem::size_of::<IcScanArgs>());
      args.assume_init()
    };
    assert_eq!(args.lock, IC_SCAN_LOCK_COMMITTED);
    assert_eq!(args.trans, IC_NO_TRANS);
    assert_eq!(args.partition, IC_NO_PARTITION);
    assert!(!args.read_ahead);
    assert!(!args.continuous);
    assert!(args.callback.is_none());
    // SAFETY: filled by the init above.
    let crossed = unsafe { scan_args_from_c(&args) };
    assert!(crossed.is_ok(), "refused: {:?}", crossed.err());
    let rust = crossed.unwrap_or_default();
    assert_eq!(rust.lock, ScanLock::Committed);
    assert!(rust.trans.is_none() && rust.partition.is_none());
    assert!(rust.callback.is_none());
    args.continuous = true;
    // SAFETY: as above.
    let crossed = unsafe { scan_args_from_c(&args) };
    assert!(crossed.unwrap_or_default().continuous);
    args.continuous = false;
    args.lock = IC_SCAN_LOCK_EXCLUSIVE;
    args.partition = 3;
    args.trans = 7;
    // SAFETY: as above.
    let crossed = unsafe { scan_args_from_c(&args) };
    assert!(crossed.is_ok(), "refused: {:?}", crossed.err());
    let rust = crossed.unwrap_or_default();
    assert_eq!(rust.lock, ScanLock::Exclusive);
    assert_eq!(rust.partition, Some(3));
    assert_eq!(rust.trans, Some(TransId::from_u32(7)));
    args.lock = 9;
    // SAFETY: as above.
    let refused = unsafe { scan_args_from_c(&args) };
    assert_eq!(refused.err(), Some(IC_ERROR_NOT_SUPPORTED));
    args.size = 4;
    // SAFETY: as above; the struct is whole, its size says otherwise.
    let short = unsafe { scan_args_from_c(&args) };
    assert_eq!(short.err(), Some(IC_ERROR_INCONSISTENT_DATA));
  }

  #[test]
  fn takeover_args_start_from_the_defaults() {
    let mut args = std::mem::MaybeUninit::<IcTakeoverArgs>::uninit();
    // SAFETY: the init writes the whole struct, of the size given.
    let mut args = unsafe {
      ic_takeover_args_init(
        args.as_mut_ptr(),
        std::mem::size_of::<IcTakeoverArgs>(),
      );
      args.assume_init()
    };
    assert_eq!(args.abort_option, IC_ABORT_DEFAULT);
    assert!(args.callback.is_none());
    // SAFETY: filled by the init above.
    let crossed = unsafe { takeover_args_from_c(&args) };
    assert!(crossed.is_ok(), "refused: {:?}", crossed.err());
    args.abort_option = 9;
    // SAFETY: as above.
    let refused = unsafe { takeover_args_from_c(&args) };
    assert_eq!(refused.err(), Some(IC_ERROR_NOT_SUPPORTED));
    // SAFETY: a null connection is refused before anything is followed.
    let code = unsafe {
      ic_apid_conn_update_scan_row(std::ptr::null_mut(), 1, 1, &args)
    };
    assert_eq!(code, IC_ERROR_NOT_SUPPORTED, "the arguments come first");
  }

  /// True if an init wrote the size alone into `bytes`, its first four
  /// bytes, and left the rest as they were, 0xA5.
  fn holds_its_size_alone(bytes: &[u8]) -> bool {
    let mut size = [0u8; 4];
    size.copy_from_slice(&bytes[..4]);
    if u32::from_ne_bytes(size) as usize != bytes.len() {
      return false;
    }
    let mut at: usize = 4;
    while at < bytes.len() {
      if bytes[at] != 0xA5 {
        return false;
      }
      at += 1;
    }
    true
  }

  #[test]
  fn a_struct_shorter_than_this_headers_is_refused_unread() {
    // Each struct with a size as an older header might have made it, a
    // field short, in a buffer no longer: its init writes its size
    // alone, and the call given it refuses it, reading nothing past
    // that field.
    let read = std::mem::size_of::<IcReadKeyArgs>() - 8;
    let mut bytes = vec![0xA5u8; read];
    // SAFETY: the init writes within the bytes given; the arguments are
    // read before the connection, which is never followed.
    let code = unsafe {
      ic_read_key_args_init(bytes.as_mut_ptr().cast(), read);
      ic_apid_conn_read_key(std::ptr::null_mut(), 1, 1, bytes.as_ptr().cast())
    };
    assert!(holds_its_size_alone(&bytes), "read key args");
    assert_eq!(code, IC_ERROR_INCONSISTENT_DATA);
    let write = std::mem::size_of::<IcWriteKeyArgs>() - 8;
    let mut bytes = vec![0xA5u8; write];
    // SAFETY: as above.
    let code = unsafe {
      ic_write_key_args_init(bytes.as_mut_ptr().cast(), write);
      ic_apid_conn_write_key(std::ptr::null_mut(), 1, 1, bytes.as_ptr().cast())
    };
    assert!(holds_its_size_alone(&bytes), "write key args");
    assert_eq!(code, IC_ERROR_INCONSISTENT_DATA);
    let scan = std::mem::size_of::<IcScanArgs>() - 8;
    let mut bytes = vec![0xA5u8; scan];
    // SAFETY: as above; the reader reads within the bytes given.
    let refused = unsafe {
      ic_scan_args_init(bytes.as_mut_ptr().cast(), scan);
      scan_args_from_c(bytes.as_ptr().cast())
    };
    assert!(holds_its_size_alone(&bytes), "scan args");
    assert_eq!(refused.err(), Some(IC_ERROR_INCONSISTENT_DATA));
    let range = std::mem::size_of::<IcScanRange>() - 4;
    let mut bytes = vec![0xA5u8; range];
    // SAFETY: as above.
    let refused = unsafe {
      ic_scan_range_init(bytes.as_mut_ptr().cast(), range);
      scan_range_from_c(bytes.as_ptr().cast())
    };
    assert!(holds_its_size_alone(&bytes), "scan range");
    assert_eq!(refused.err(), Some(IC_ERROR_INCONSISTENT_DATA));
    let takeover = std::mem::size_of::<IcTakeoverArgs>() - 8;
    let mut bytes = vec![0xA5u8; takeover];
    // SAFETY: as above.
    let refused = unsafe {
      ic_takeover_args_init(bytes.as_mut_ptr().cast(), takeover);
      takeover_args_from_c(bytes.as_ptr().cast())
    };
    assert!(holds_its_size_alone(&bytes), "takeover args");
    assert_eq!(refused.err(), Some(IC_ERROR_INCONSISTENT_DATA));
    // Too short for even its size: nothing is written.
    let mut tiny = vec![0xA5u8; 2];
    // SAFETY: the init writes within the two bytes given, or not at all.
    unsafe { ic_scan_args_init(tiny.as_mut_ptr().cast(), 2) };
    assert_eq!(tiny, vec![0xA5u8, 0xA5]);
    // A null pointer is refused as one.
    // SAFETY: nothing is followed.
    let refused = unsafe { scan_args_from_c(std::ptr::null()) };
    assert_eq!(refused.err(), Some(IC_ERROR_NULL_POINTER));
  }

  #[test]
  fn a_batch_names_its_scan() {
    let batch = ((42u64) << 32) | 0x0080_0001;
    assert_eq!(ic_scan_batch_scan(batch), 42);
  }

  #[test]
  fn a_request_is_filled_and_read_through_its_handle() {
    let mut request: *mut IcRequest = std::ptr::null_mut();
    let mut len: usize = 9;
    // SAFETY: every pointer is to a local, or the request made here.
    unsafe {
      assert_eq!(ic_request_create(&mut request), 0);
      assert_eq!(ic_request_num_ops(request), 0);
      assert!(ic_request_key_row(request, 0, &mut len).is_null());
      assert_eq!(len, 0);
      assert_eq!(ic_request_op_error(request, 0), IC_ERROR_NO_SUCH_FIELD);
      assert_eq!(ic_request_state(request), IC_COMMIT_STATE_STARTED);
      assert_eq!(ic_request_error(request), 0);
      assert_eq!(ic_request_set_timeout_ms(request, 5), 0);
      assert_eq!(ic_request_set_hint(request, std::ptr::null()), 0);
      assert_eq!(
        ic_request_set_abort_option(request, 0, IC_ABORT_ON_ERROR),
        IC_ERROR_NO_SUCH_FIELD
      );
      assert_eq!(
        ic_request_add_read(
          request,
          std::ptr::null(),
          99,
          std::ptr::null_mut()
        ),
        IC_ERROR_NOT_SUPPORTED
      );
      assert_eq!(
        ic_request_add_read(
          request,
          std::ptr::null(),
          IC_READ_KIND_COMMITTED,
          std::ptr::null_mut()
        ),
        IC_ERROR_NULL_POINTER
      );
      assert_eq!(ic_request_clear(request), 0);
      ic_request_free(request);
    }
  }

  #[test]
  fn executor_options_start_from_the_defaults() {
    let mut opts = std::mem::MaybeUninit::<IcExecutorOptions>::uninit();
    // SAFETY: the init writes the whole struct, of the size given.
    let opts = unsafe {
      ic_executor_options_init(
        opts.as_mut_ptr(),
        std::mem::size_of::<IcExecutorOptions>(),
      );
      opts.assume_init()
    };
    let defaults = ExecutorOptions::default();
    assert_eq!(opts.in_flight, defaults.in_flight);
    assert_eq!(opts.queue, defaults.queue);
    assert_eq!(opts.scans, defaults.scans);
  }

  #[test]
  fn a_struct_from_an_older_header_gets_the_fields_it_knows() {
    // Release 0.1's IC_EXECUTOR_OPTIONS ends before `scans`: the init
    // writes no further, and a later field read from it is the default.
    let mut bytes = [0xAAu8; 32];
    // SAFETY: the buffer is a local, longer than the size given.
    unsafe {
      ic_executor_options_init(
        bytes.as_mut_ptr() as *mut IcExecutorOptions,
        executor_options_0_1(),
      );
    }
    assert!(bytes[executor_options_0_1()..].iter().all(|b| *b == 0xAA));
    // SAFETY: as above.
    let (size, queue, scans) = unsafe {
      let base = bytes.as_ptr();
      let size = get_sized::<u32>(base, 4, 0).unwrap_or(0) as usize;
      (
        size,
        get_sized::<u32>(
          base,
          size,
          std::mem::offset_of!(IcExecutorOptions, queue),
        ),
        get_sized::<u32>(
          base,
          size,
          std::mem::offset_of!(IcExecutorOptions, scans),
        ),
      )
    };
    assert_eq!(size, executor_options_0_1());
    assert_eq!(queue, Some(ExecutorOptions::default().queue));
    assert_eq!(scans, None, "not in the older struct");
    // The stats likewise end where 0.1's did.
    assert_eq!(executor_stats_0_1(), 64);
    assert_eq!(executor_options_0_1(), 12);
  }

  #[test]
  fn global_options_from_an_older_header_get_the_fields_they_know() {
    // Release 0.1's IC_GLOBAL_OPTIONS ends before the AUTO_INCREMENT
    // fields: the init writes no further.
    assert_eq!(global_options_0_1(), 16);
    let mut bytes = [0xAAu8; 32];
    // SAFETY: the buffer is a local, longer than the size given.
    unsafe {
      ic_global_options_init(
        bytes.as_mut_ptr() as *mut IcGlobalOptions,
        global_options_0_1(),
      );
    }
    assert!(bytes[global_options_0_1()..].iter().all(|b| *b == 0xAA));
    // SAFETY: as above.
    let (size, mgm, prefetch) = unsafe {
      let base = bytes.as_ptr();
      let size = get_sized::<u32>(base, 4, 0).unwrap_or(0) as usize;
      (
        size,
        get_sized::<u32>(
          base,
          size,
          std::mem::offset_of!(IcGlobalOptions, mgm_timeout_ms),
        ),
        get_sized::<u32>(
          base,
          size,
          std::mem::offset_of!(IcGlobalOptions, auto_increment_prefetch),
        ),
      )
    };
    assert_eq!(size, 16);
    assert_eq!(mgm, Some(GlobalOptions::default().mgm_timeout_ms));
    assert_eq!(prefetch, None, "not in the older struct");
    // A smaller struct still is left as it was.
    let mut short = [0x55u8; 12];
    // SAFETY: the buffer is a local of the length given.
    unsafe {
      ic_global_options_init(short.as_mut_ptr() as *mut IcGlobalOptions, 12);
    }
    assert!(short.iter().all(|b| *b == 0x55));
  }

  #[test]
  fn field_info_kept_its_size_as_it_grew() {
    // The two flags after 0.1 took what was padding: a program built
    // against 0.1's header gives the size the library asks for.
    assert_eq!(std::mem::offset_of!(IcFieldInfo, auto_increment), 52);
    assert_eq!(std::mem::offset_of!(IcFieldInfo, hidden_key), 53);
    assert_eq!(std::mem::size_of::<IcFieldInfo>(), 56);
  }

  #[test]
  fn an_error_text_is_cut_to_the_buffer() {
    let mut buf = [0x55u8; 8];
    // SAFETY: the buffer is a local of the length given.
    let whole = unsafe {
      ic_error_message(
        IC_ERROR_NULL_POINTER,
        buf.as_mut_ptr() as *mut c_char,
        buf.len(),
      )
    };
    let text = IcError::new(IC_ERROR_NULL_POINTER).message();
    assert_eq!(whole, text.len());
    assert_eq!(&buf[..7], &text.as_bytes()[..7]);
    assert_eq!(buf[7], 0);
    assert_eq!(ic_error_category(0), IC_ERROR_CATEGORY_NONE);
    assert!(!ic_error_is_temporary(IC_ERROR_NULL_POINTER));
  }

  #[test]
  fn options_and_arguments_start_from_the_defaults() {
    let mut opts = std::mem::MaybeUninit::<IcGlobalOptions>::uninit();
    let mut read = std::mem::MaybeUninit::<IcReadKeyArgs>::uninit();
    let mut write = std::mem::MaybeUninit::<IcWriteKeyArgs>::uninit();
    // SAFETY: each init writes the whole struct, of the size given.
    let (opts, read, write) = unsafe {
      ic_global_options_init(
        opts.as_mut_ptr(),
        std::mem::size_of::<IcGlobalOptions>(),
      );
      ic_read_key_args_init(
        read.as_mut_ptr(),
        std::mem::size_of::<IcReadKeyArgs>(),
      );
      ic_write_key_args_init(
        write.as_mut_ptr(),
        std::mem::size_of::<IcWriteKeyArgs>(),
      );
      (opts.assume_init(), read.assume_init(), write.assume_init())
    };
    assert_eq!(opts.receive_threads, 1);
    assert_eq!(opts.mgm_timeout_ms, 30_000);
    assert_eq!(opts.size as usize, std::mem::size_of::<IcGlobalOptions>());
    assert_eq!(opts.auto_increment_prefetch, 4096);
    assert_eq!(opts.auto_increment_step, 1);
    assert_eq!(opts.auto_increment_offset, 1);
    assert_eq!(read.kind, IC_READ_KIND_SIMPLE);
    assert!(read.callback.is_none());
    assert_eq!(write.kind, IC_WRITE_KIND_UPDATE);
    assert_eq!(write.batch_hint, IC_BATCH_HINT_NONE);
  }

  #[test]
  fn type_names_are_c_strings() {
    let name = ic_field_type_name(table_props::IC_TYPE_VARCHAR);
    // SAFETY: a static C string.
    let text = unsafe { CStr::from_ptr(name) };
    assert_eq!(
      text.to_str().unwrap_or_default(),
      table_props::type_name(table_props::IC_TYPE_VARCHAR)
    );
    // SAFETY: as above.
    let unknown = unsafe { CStr::from_ptr(ic_field_type_name(1000)) };
    assert_eq!(unknown.to_str().unwrap_or_default(), "Undefined");
  }

  #[test]
  fn hints_cross_both_ways() {
    let hints = [
      TransactionHint::Any,
      TransactionHint::Node(2),
      TransactionHint::Partition(7),
    ];
    for hint in hints {
      assert_eq!(hint_from_c(hint_to_c(hint)), hint);
    }
  }

  #[test]
  fn buffers_overlap_only_where_they_share_a_byte() {
    let buf = [0u8; 16];
    let p = buf.as_ptr();
    // SAFETY: within the 16 bytes of `buf`.
    let (four, eight) = unsafe { (p.add(4), p.add(8)) };
    assert!(overlaps(p, 8, p, 8));
    assert!(overlaps(p, 8, four, 8));
    assert!(overlaps(four, 8, p, 8));
    // Side by side, or empty, or none at all: apart.
    assert!(!overlaps(p, 8, eight, 8));
    assert!(!overlaps(eight, 8, p, 8));
    assert!(!overlaps(p, 0, p, 8));
    assert!(!overlaps(std::ptr::null(), 8, p, 8));
  }

  #[test]
  fn a_key_in_the_row_to_be_written_is_copied_aside_first() {
    // One buffer for the key and the row read into it: the key comes
    // back as a copy, which writing the row leaves as it was.
    let mut row = vec![1u8, 2, 3, 4, 5, 6, 7, 8];
    let p = row.as_mut_ptr();
    let mut copy = KeyCopy::new();
    // SAFETY: the row's 8 bytes, written only once the key is taken.
    let key = unsafe { key_arg(p, 8, p, 8, &mut copy) };
    assert_ne!(key.as_ptr(), p.cast_const());
    // SAFETY: as above.
    unsafe { p.write_bytes(0, 8) };
    assert_eq!(key, [1, 2, 3, 4, 5, 6, 7, 8]);
    // A key too long for the stack, overlapping part of the row: on the
    // heap.
    let mut copy = KeyCopy::new();
    let len = copy.inline.len() + 10;
    let mut long = vec![9u8; len];
    let q = long.as_mut_ptr();
    // SAFETY: 20 bytes inside the `len` of `long`.
    let part = unsafe { q.add(5) };
    // SAFETY: as above.
    let key = unsafe { key_arg(q, len, part, 20, &mut copy) };
    // SAFETY: as above.
    unsafe { part.write_bytes(0, 20) };
    assert_eq!(key.len(), len);
    assert_ne!(key.as_ptr(), q.cast_const());
    let mut at: usize = 0;
    while at < len {
      assert_eq!(key[at], 9, "byte {}", at);
      at += 1;
    }
    // Apart, the key is the caller's own bytes, not copied.
    let apart = [7u8; 8];
    let mut out = [0u8; 8];
    let mut copy = KeyCopy::new();
    // SAFETY: two buffers of 8 bytes each.
    let key =
      unsafe { key_arg(apart.as_ptr(), 8, out.as_mut_ptr(), 8, &mut copy) };
    assert_eq!(key.as_ptr(), apart.as_ptr());
    assert!(copy.heap.is_empty(), "nothing copied");
  }
}
