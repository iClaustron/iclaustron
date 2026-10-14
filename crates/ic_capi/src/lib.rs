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
//! - The query owns its rows; C gets pointers to them (Q10).

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
use ic_apid::key_op;
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

/// A query's id in its connection (`IC_QUERY_ID`).
pub type IcQueryId = u32;

/// A transaction's id in its connection (`IC_TRANS_ID`).
pub type IcTransId = u32;

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

/// Fill `options`, `size` bytes, with the defaults.
///
/// Leaves `options` as it is when `size` is smaller than this header's
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
  if options.is_null() || size < std::mem::size_of::<IcGlobalOptions>() {
    return;
  }
  let defaults = GlobalOptions::default();
  let value = IcGlobalOptions {
    size: size as u32,
    receive_threads: defaults.receive_threads,
    reply_wait_ms: defaults.reply_wait_ms,
    mgm_timeout_ms: defaults.mgm_timeout_ms,
  };
  // SAFETY: the caller's promise; the struct fits.
  unsafe { options.write(value) };
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
  // SAFETY: the caller's promise.
  if let Some(given) = unsafe { options.as_ref() } {
    if (given.size as usize) < std::mem::size_of::<IcGlobalOptions>() {
      return err::IC_ERROR_INCONSISTENT_DATA;
    }
    opts.receive_threads = given.receive_threads;
    opts.reply_wait_ms = given.reply_wait_ms;
    opts.mgm_timeout_ms = given.mgm_timeout_ms;
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
/// transaction left open does not commit.
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
/// `IC_ERROR_BAD_TEXT`. It waits for the data node when the description is not
/// cached yet. The handle is the caller's, freed with `ic_table_def_free`; it
/// may outlive the connection and be used by any thread.
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
/// long as the table handle.
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
/// number, a character or binary string as its own bytes, unquoted, or
/// `NULL`. The temporal types and decimals cannot be given as text; a
/// program puts their bytes into the row itself. For tools and
/// examples: an application works in the row's own bytes.
///
/// Errors: `IC_ERROR_BAD_TEXT` for text that is not UTF-8 or does not fit the
/// field; `IC_ERROR_NO_SUCH_FIELD`; `IC_ERROR_RECORD_LAYOUT` when `len` is less
/// than the record's row size.
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
/// quotes, binary as `0x` and hexadecimal digits, or `NULL`. The text's
/// whole length is returned, and a longer text is cut short.
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
/// Leaves `args` as it is when `size` is smaller than this header's
/// `IC_READ_KEY_ARGS`.
///
/// # Safety
///
/// A non-null `args` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_read_key_args_init(
  args: *mut IcReadKeyArgs,
  size: usize,
) {
  if args.is_null() || size < std::mem::size_of::<IcReadKeyArgs>() {
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
/// Leaves `args` as it is when `size` is smaller than this header's
/// `IC_WRITE_KEY_ARGS`.
///
/// # Safety
///
/// A non-null `args` points to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ic_write_key_args_init(
  args: *mut IcWriteKeyArgs,
  size: usize,
) {
  if args.is_null() || size < std::mem::size_of::<IcWriteKeyArgs>() {
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
  let given = match unsafe { args.as_ref() } {
    Some(given) => *given,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if (given.size as usize) < std::mem::size_of::<IcReadKeyArgs>() {
    return err::IC_ERROR_INCONSISTENT_DATA;
  }
  let kind = match given.kind {
    IC_READ_KIND_SIMPLE => ReadKind::Simple,
    IC_READ_KIND_LOCKED => ReadKind::Locked,
    IC_READ_KIND_EXCLUSIVE => ReadKind::Exclusive,
    IC_READ_KIND_COMMITTED => ReadKind::Committed,
    IC_READ_KIND_EXCLUSIVE_NO_WAIT => ReadKind::ExclusiveNoWait,
    _ => return err::IC_ERROR_NOT_SUPPORTED,
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
  let given = match unsafe { args.as_ref() } {
    Some(given) => *given,
    None => return err::IC_ERROR_NULL_POINTER,
  };
  if (given.size as usize) < std::mem::size_of::<IcWriteKeyArgs>() {
    return err::IC_ERROR_INCONSISTENT_DATA;
  }
  let kind = match given.kind {
    IC_WRITE_KIND_UPDATE => WriteKind::Update,
    IC_WRITE_KIND_WRITE => WriteKind::Write,
    IC_WRITE_KIND_INSERT => WriteKind::Insert,
    IC_WRITE_KIND_DELETE => WriteKind::Delete,
    _ => return err::IC_ERROR_NOT_SUPPORTED,
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

/// Send, then poll.
///
/// Errors: as `ic_apid_conn_send`.
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
/// `attr_len` writable ones; `found` to a writable `bool`.
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
  // SAFETY: the caller's promise.
  let (key_bytes, attr_bytes) =
    unsafe { (row_arg(key_row, key_len), row_arg_mut(attr_row, attr_len)) };
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
  // SAFETY: the caller's promise.
  let (key_bytes, attr_bytes) =
    unsafe { (row_arg(key_row, key_len), row_arg_mut(attr_row, attr_len)) };
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
    }
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
}
