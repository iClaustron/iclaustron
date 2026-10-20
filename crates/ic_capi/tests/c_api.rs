// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Integration tests of the C ABI against a live RonDB cluster, run by
//! `cargo xtask test-integration` with the `ic_apid` ones. The exported
//! functions are called as a C program calls them: handles, ids, out
//! pointers, codes, and a callback through the trampoline. The cluster
//! and the MySQL client are named by the same variables as there
//! (`IC_TEST_CONNECTSTRING`, `IC_TEST_MYSQL`); a test that lacks one
//! says so and returns.

#![cfg(feature = "integration")]
#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::ffi::c_char;
use std::ffi::c_void;
use std::ffi::CStr;
use std::ffi::CString;
use std::io::Write;
use std::process::Command;
use std::process::Stdio;

use iclaustron::*;

const IC_IT_DB: &str = "ic_it";
/// How long to wait for the data nodes at start.
const IC_IT_START_WAIT_MS: u32 = 30_000;
/// How many flushes a transaction may take before the test gives up.
const IC_IT_FLUSHES: u32 = 100;

fn cs(text: &str) -> CString {
  CString::new(text).expect("no NUL in the text")
}

/// Run SQL through the MySQL client, or `None` without one.
fn mysql(sql: &str) -> Option<String> {
  let command = match std::env::var("IC_TEST_MYSQL") {
    Ok(command) => command,
    Err(_) => {
      eprintln!("skipped: IC_TEST_MYSQL is not set");
      return None;
    }
  };
  let mut child = Command::new("sh")
    .arg("-c")
    .arg(format!("{} --batch --skip-column-names --raw", command))
    .stdin(Stdio::piped())
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .expect("sh");
  child
    .stdin
    .take()
    .expect("stdin")
    .write_all(sql.as_bytes())
    .expect("write to the client");
  let out = child.wait_with_output().expect("wait for the client");
  assert!(
    out.status.success(),
    "mysql failed: {}",
    String::from_utf8_lossy(&out.stderr)
  );
  Some(String::from_utf8_lossy(&out.stdout).trim_end().to_string())
}

/// The table the group works on, made afresh.
fn setup_capi_table() -> Option<()> {
  mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.capi;
     CREATE TABLE {db}.capi (
       id INT NOT NULL PRIMARY KEY,
       val INT,
       name VARCHAR(32),
       code INT NOT NULL,
       UNIQUE KEY capi_code (code)
     ) ENGINE=NDB;",
    db = IC_IT_DB
  ))?;
  Some(())
}

/// The global, connected and with a data node started, or `None`
/// without a connectstring.
fn connect() -> Option<*mut IcApidGlobal> {
  let text = match std::env::var("IC_TEST_CONNECTSTRING") {
    Ok(text) => text,
    Err(_) => {
      eprintln!("skipped: IC_TEST_CONNECTSTRING is not set");
      return None;
    }
  };
  let connect_string = cs(&text);
  let name = cs("ic_it_capi");
  let mut options = std::mem::MaybeUninit::<IcGlobalOptions>::uninit();
  let mut global: *mut IcApidGlobal = std::ptr::null_mut();
  let mut started: u32 = 0;
  // SAFETY: the pointers are to locals, the strings NUL-terminated.
  unsafe {
    ic_global_options_init(
      options.as_mut_ptr(),
      std::mem::size_of::<IcGlobalOptions>(),
    );
    let options = options.assume_init();
    let mut tries: u32 = 0;
    loop {
      let ret = ic_apid_global_connect(
        connect_string.as_ptr(),
        name.as_ptr(),
        &options,
        &mut global,
      );
      if ret == 0 {
        break;
      }
      // The API node id the last test let go of may not be free yet.
      tries += 1;
      assert!(tries < 10, "could not connect: {}", message(ret));
      std::thread::sleep(std::time::Duration::from_secs(1));
    }
    let ret = ic_apid_global_wait_for_started(
      global,
      IC_IT_START_WAIT_MS,
      true,
      &mut started,
    );
    assert_eq!(ret, 0);
  }
  assert!(started > 0, "no data node is started");
  Some(global)
}

fn message(code: i32) -> String {
  let mut buf = [0u8; 200];
  // SAFETY: the buffer is a local of the length given.
  unsafe {
    ic_error_message(code, buf.as_mut_ptr() as *mut c_char, buf.len());
    CStr::from_ptr(buf.as_ptr() as *const c_char)
      .to_string_lossy()
      .to_string()
  }
}

/// A field's id, by name.
///
/// # Safety
///
/// `table` is a table handle not yet freed.
unsafe fn field(table: *const IcTableDef, name: &str) -> u32 {
  let name = cs(name);
  let mut id: u32 = 0;
  // SAFETY: the caller's promise; the rest are locals.
  let ret = unsafe { ic_table_def_field_id(table, name.as_ptr(), &mut id) };
  assert_eq!(ret, 0, "no field {:?}", name);
  id
}

/// Put values given as text into a row.
///
/// # Safety
///
/// `rec` is a record handle not yet freed, `row` points to `len`
/// writable bytes.
unsafe fn put(
  rec: *const IcRecord,
  row: *mut u8,
  len: usize,
  values: &[(u32, &str)],
) {
  for (id, text) in values {
    let text = cs(text);
    // SAFETY: the caller's promise.
    let ret = unsafe { ic_record_put_text(rec, *id, text.as_ptr(), row, len) };
    assert_eq!(ret, 0, "field {} = {:?}: {}", id, text, message(ret));
  }
}

/// A field of a row, as text.
///
/// # Safety
///
/// As [`put`], the row readable.
unsafe fn get(
  rec: *const IcRecord,
  row: *const u8,
  len: usize,
  id: u32,
) -> String {
  let mut buf = [0u8; 100];
  // SAFETY: the caller's promise; the buffer is a local.
  unsafe {
    ic_record_get_text(
      rec,
      id,
      row,
      len,
      buf.as_mut_ptr() as *mut c_char,
      buf.len(),
    );
    CStr::from_ptr(buf.as_ptr() as *const c_char)
      .to_string_lossy()
      .to_string()
  }
}

/// Flush until the transaction is done, and give how it ended.
///
/// # Safety
///
/// `conn` is a connection handle not yet freed.
unsafe fn finish(conn: *mut IcApidConnection, trans: IcTransId) -> u32 {
  let mut flushes: u32 = 0;
  // SAFETY: the caller's promise.
  unsafe {
    while !ic_apid_transaction_is_done(conn, trans) {
      flushes += 1;
      assert!(flushes < IC_IT_FLUSHES, "the transaction did not finish");
      let ret = ic_apid_conn_flush(conn, 100, true, std::ptr::null_mut());
      assert_eq!(ret, 0, "flush: {}", message(ret));
    }
    ic_apid_transaction_state(conn, trans)
  }
}

/// What the read callback saw.
struct Seen {
  calls: u32,
  query: IcQueryId,
  conn: *mut IcApidConnection,
  val: String,
  rec: *const IcRecord,
  val_id: u32,
}

/// The read's callback: notes what it was given, and reads the row
/// from inside, as a C callback may.
unsafe extern "C" fn on_read(
  conn: *mut IcApidConnection,
  query: IcQueryId,
  user_ref: *mut c_void,
) {
  // SAFETY: the test gave a pointer to its `Seen`, alive throughout.
  let seen = unsafe { &mut *(user_ref as *mut Seen) };
  seen.calls += 1;
  seen.query = query;
  seen.conn = conn;
  let mut len: usize = 0;
  // SAFETY: the connection the callback runs on, and its own query.
  unsafe {
    let row = ic_apid_query_attr_row(conn, query, &mut len);
    seen.val = get(seen.rec, row, len, seen.val_id);
  }
}

#[test]
fn c_api_insert_read_update_delete_by_primary_and_unique_key() {
  let global = match connect() {
    Some(global) => global,
    None => return,
  };
  if setup_capi_table().is_none() {
    // SAFETY: the handle connect made.
    unsafe { ic_apid_global_free(global) };
    return;
  }
  let db = cs(IC_IT_DB);
  let table_name = cs("capi");
  // SAFETY: every handle below is used while alive and freed once;
  // every pointer is to a local or a row the library gave.
  unsafe {
    let mut conn: *mut IcApidConnection = std::ptr::null_mut();
    assert_eq!(ic_apid_global_create_connection(global, &mut conn), 0);
    let mut table: *mut IcTableDef = std::ptr::null_mut();
    let ret = ic_apid_conn_table_bind(
      conn,
      db.as_ptr(),
      table_name.as_ptr(),
      &mut table,
    );
    assert_eq!(ret, 0, "bind: {}", message(ret));
    assert_eq!(ic_table_def_num_fields(table), 4);
    let id = field(table, "id");
    let val = field(table, "val");
    let name = field(table, "name");
    let code = field(table, "code");
    let mut info = std::mem::MaybeUninit::<IcFieldInfo>::uninit();
    let ret = ic_table_def_field(
      table,
      id,
      info.as_mut_ptr(),
      std::mem::size_of::<IcFieldInfo>(),
    );
    assert_eq!(ret, 0);
    let info = info.assume_init();
    assert!(info.primary_key);
    assert!(!info.nullable);
    assert_eq!(CStr::from_ptr(info.name).to_str().expect("UTF-8"), "id");

    let mut rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(
      ic_record_create_default(table, std::ptr::null(), 0, &mut rec),
      0
    );

    // An insert, in a transaction placed by the key.
    let mut query: IcQueryId = 0;
    assert_eq!(ic_apid_query_create(conn, table, rec, rec, &mut query), 0);
    let mut key_len: usize = 0;
    let mut attr_len: usize = 0;
    let key = ic_apid_query_key_row(conn, query, &mut key_len);
    let attr = ic_apid_query_attr_row(conn, query, &mut attr_len);
    assert!(!key.is_null() && !attr.is_null());
    assert_eq!(key_len, ic_record_row_size(rec) as usize);
    let values = [(id, "1"), (val, "10"), (name, "one"), (code, "100")];
    put(rec, key, key_len, &values);
    put(rec, attr, attr_len, &values);
    let mut write = std::mem::MaybeUninit::<IcWriteKeyArgs>::uninit();
    ic_write_key_args_init(
      write.as_mut_ptr(),
      std::mem::size_of::<IcWriteKeyArgs>(),
    );
    let mut write = write.assume_init();
    write.kind = IC_WRITE_KIND_INSERT;
    let hint = ic_apid_conn_hint_for_key(conn, table, rec, key, key_len);
    assert_eq!(hint.kind, IC_HINT_PARTITION, "an INT key is placed");
    let mut trans: IcTransId = 0;
    assert_eq!(
      ic_apid_conn_start_transaction(conn, hint, table, &mut trans),
      0
    );
    let ret = ic_apid_conn_write_key(conn, query, trans, &write);
    assert_eq!(ret, 0, "insert: {}", message(ret));
    assert_eq!(ic_apid_query_state(conn, query), IC_QUERY_DEFINED);
    assert_eq!(ic_apid_conn_commit_transaction(conn, trans), 0);
    assert_eq!(finish(conn, trans), IC_COMMIT_STATE_COMMITTED);
    assert_eq!(ic_apid_transaction_error(conn, trans), 0);
    assert!(ic_apid_transaction_gci(conn, trans) > 0);
    let mut done: IcQueryId = 0;
    assert!(ic_apid_conn_get_next_executed_query(conn, &mut done));
    assert_eq!(done, query);
    assert_eq!(ic_apid_query_error(conn, query), 0);
    assert_eq!(ic_apid_conn_close_transaction(conn, trans), 0);
    let seen_sql = mysql(&format!(
      "SELECT val, name FROM {}.capi WHERE id = 1",
      IC_IT_DB
    ));
    assert_eq!(seen_sql, Some("10\tone".to_string()));

    // A committed read, its completion through a callback.
    let mut seen = Seen {
      calls: 0,
      query: 0,
      conn: std::ptr::null_mut(),
      val: String::new(),
      rec,
      val_id: val,
    };
    let mut read = std::mem::MaybeUninit::<IcReadKeyArgs>::uninit();
    ic_read_key_args_init(
      read.as_mut_ptr(),
      std::mem::size_of::<IcReadKeyArgs>(),
    );
    let mut read = read.assume_init();
    read.kind = IC_READ_KIND_COMMITTED;
    read.callback = Some(on_read);
    read.user_ref = &mut seen as *mut Seen as *mut c_void;
    assert_eq!(
      ic_apid_conn_start_transaction(conn, hint, table, &mut trans),
      0
    );
    let ret = ic_apid_conn_read_key(conn, query, trans, &read);
    assert_eq!(ret, 0, "read: {}", message(ret));
    assert_eq!(ic_apid_conn_commit_transaction(conn, trans), 0);
    assert_eq!(finish(conn, trans), IC_COMMIT_STATE_COMMITTED);
    assert_eq!(seen.calls, 1, "the callback ran once");
    assert_eq!(seen.query, query);
    assert_eq!(seen.conn, conn, "with the connection's own handle");
    assert_eq!(seen.val, "10");
    assert!(
      !ic_apid_conn_get_next_executed_query(conn, &mut done),
      "a query with a callback is not on the executed list"
    );
    assert_eq!(ic_apid_conn_close_transaction(conn, trans), 0);

    // Through the unique index, synchronously.
    let index_name = cs("capi_code$unique");
    let mut index: *mut IcIndexDef = std::ptr::null_mut();
    let ret = ic_apid_conn_index_bind(
      conn,
      db.as_ptr(),
      index_name.as_ptr(),
      table_name.as_ptr(),
      &mut index,
    );
    assert_eq!(ret, 0, "index bind: {}", message(ret));
    let mut key_rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(ic_record_create_default(table, &code, 1, &mut key_rec), 0);
    let mut index_key = vec![0u8; ic_record_row_size(key_rec) as usize];
    put(
      key_rec,
      index_key.as_mut_ptr(),
      index_key.len(),
      &[(code, "100")],
    );
    let mut out = vec![0u8; ic_record_row_size(rec) as usize];
    let mut found = false;
    let ret = ic_apid_conn_read_unique_committed(
      conn,
      index,
      key_rec,
      index_key.as_ptr(),
      index_key.len(),
      rec,
      out.as_mut_ptr(),
      out.len(),
      &mut found,
    );
    assert_eq!(ret, 0, "unique read: {}", message(ret));
    assert!(found);
    assert_eq!(get(rec, out.as_ptr(), out.len(), id), "1");
    assert_eq!(get(rec, out.as_ptr(), out.len(), name), "'one'");

    // An update and a delete, synchronously.
    let mut row = vec![0u8; ic_record_row_size(rec) as usize];
    put(
      rec,
      row.as_mut_ptr(),
      row.len(),
      &[(id, "1"), (val, "11"), (name, "NULL"), (code, "100")],
    );
    let ret = ic_apid_conn_write(
      conn,
      IC_WRITE_KIND_UPDATE,
      rec,
      row.as_ptr(),
      row.len(),
      rec,
      row.as_ptr(),
      row.len(),
    );
    assert_eq!(ret, 0, "update: {}", message(ret));
    let seen_sql = mysql(&format!(
      "SELECT val, name IS NULL FROM {}.capi WHERE id = 1",
      IC_IT_DB
    ));
    assert_eq!(seen_sql, Some("11\t1".to_string()));
    let ret = ic_apid_conn_write(
      conn,
      IC_WRITE_KIND_INSERT,
      rec,
      row.as_ptr(),
      row.len(),
      rec,
      row.as_ptr(),
      row.len(),
    );
    assert_eq!(ret, IC_NDB_ERROR_ROW_EXISTS, "a second insert is refused");
    assert!(!message(ret).is_empty());
    assert_eq!(ic_error_category(ret), IC_ERROR_CATEGORY_USER);
    let ret = ic_apid_conn_write(
      conn,
      IC_WRITE_KIND_DELETE,
      rec,
      row.as_ptr(),
      row.len(),
      rec,
      row.as_ptr(),
      row.len(),
    );
    assert_eq!(ret, 0, "delete: {}", message(ret));
    let ret = ic_apid_conn_read_committed(
      conn,
      rec,
      row.as_ptr(),
      row.len(),
      rec,
      out.as_mut_ptr(),
      out.len(),
      &mut found,
    );
    assert_eq!(ret, 0);
    assert!(!found, "gone");

    assert_eq!(ic_apid_query_free(conn, query), 0);
    ic_record_free(key_rec);
    ic_record_free(rec);
    ic_index_def_free(index);
    ic_table_def_free(table);
    ic_apid_conn_free(conn);
    ic_apid_global_free(global);
  }
}

#[test]
fn c_api_stale_ids_are_refused() {
  let global = match connect() {
    Some(global) => global,
    None => return,
  };
  if setup_capi_table().is_none() {
    // SAFETY: the handle connect made.
    unsafe { ic_apid_global_free(global) };
    return;
  }
  let db = cs(IC_IT_DB);
  let table_name = cs("capi");
  // SAFETY: as in the test above.
  unsafe {
    let mut conn: *mut IcApidConnection = std::ptr::null_mut();
    assert_eq!(ic_apid_global_create_connection(global, &mut conn), 0);
    let mut table: *mut IcTableDef = std::ptr::null_mut();
    assert_eq!(
      ic_apid_conn_table_bind(
        conn,
        db.as_ptr(),
        table_name.as_ptr(),
        &mut table
      ),
      0
    );
    let mut rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(
      ic_record_create_default(table, std::ptr::null(), 0, &mut rec),
      0
    );
    let mut query: IcQueryId = 0;
    assert_eq!(ic_apid_query_create(conn, table, rec, rec, &mut query), 0);
    assert_eq!(ic_apid_query_state(conn, query), IC_QUERY_IDLE);
    assert_eq!(ic_apid_query_free(conn, query), 0);
    assert_ne!(ic_apid_query_free(conn, query), 0, "freed already");
    assert_eq!(ic_apid_query_state(conn, query), IC_QUERY_UNKNOWN);
    let mut len: usize = 9;
    assert!(ic_apid_query_key_row(conn, query, &mut len).is_null());
    assert_eq!(len, 0);

    // A transaction with nothing in it ends at once when committed.
    let any = IcTransactionHint {
      kind: IC_HINT_ANY,
      value: 0,
    };
    let mut trans: IcTransId = 0;
    assert_eq!(
      ic_apid_conn_start_transaction(conn, any, std::ptr::null(), &mut trans),
      0
    );
    assert_eq!(ic_apid_conn_commit_transaction(conn, trans), 0);
    assert_eq!(finish(conn, trans), IC_COMMIT_STATE_COMMITTED);
    assert_eq!(ic_apid_conn_close_transaction(conn, trans), 0);
    assert_ne!(ic_apid_conn_close_transaction(conn, trans), 0);
    assert_eq!(
      ic_apid_transaction_state(conn, trans),
      IC_COMMIT_STATE_UNKNOWN
    );
    assert!(ic_apid_transaction_is_done(conn, trans));

    // Text that does not fit its field.
    let mut row = vec![0u8; ic_record_row_size(rec) as usize];
    let text = cs("not a number");
    let id = field(table, "id");
    assert_eq!(
      ic_record_put_text(rec, id, text.as_ptr(), row.as_mut_ptr(), row.len()),
      IC_ERROR_BAD_TEXT
    );

    ic_record_free(rec);
    ic_table_def_free(table);
    ic_apid_conn_free(conn);
    ic_apid_global_free(global);
  }
}

#[test]
fn c_api_an_executor_runs_requests_for_threads_without_connections() {
  let global = match connect() {
    Some(global) => global,
    None => return,
  };
  if setup_capi_table().is_none() {
    // SAFETY: the handle connect made.
    unsafe { ic_apid_global_free(global) };
    return;
  }
  let db = cs(IC_IT_DB);
  let table_name = cs("capi");
  let index_name = cs("capi_code$unique");
  // SAFETY: every handle below is used while alive and freed once;
  // every pointer is to a local or a row the library gave.
  unsafe {
    let mut options = std::mem::MaybeUninit::<IcExecutorOptions>::uninit();
    ic_executor_options_init(
      options.as_mut_ptr(),
      std::mem::size_of::<IcExecutorOptions>(),
    );
    let mut options = options.assume_init();
    options.in_flight = 32;
    let mut executor: *mut IcApidExecutor = std::ptr::null_mut();
    let ret = ic_apid_executor_start(global, &options, &mut executor);
    assert_eq!(ret, 0, "start: {}", message(ret));
    let mut submitter: *mut IcApidSubmitter = std::ptr::null_mut();
    assert_eq!(ic_apid_executor_submitter(executor, &mut submitter), 0);
    let mut table: *mut IcTableDef = std::ptr::null_mut();
    let ret = ic_apid_submitter_table_bind(
      submitter,
      db.as_ptr(),
      table_name.as_ptr(),
      &mut table,
    );
    assert_eq!(ret, 0, "bind: {}", message(ret));
    let id = field(table, "id");
    let val = field(table, "val");
    let name = field(table, "name");
    let code = field(table, "code");
    let mut rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(
      ic_record_create_default(table, std::ptr::null(), 0, &mut rec),
      0
    );
    let mut shape: *mut IcOpShape = std::ptr::null_mut();
    assert_eq!(ic_op_shape_create(table, rec, rec, &mut shape), 0);

    // Two inserts in one request: one transaction.
    let mut request: *mut IcRequest = std::ptr::null_mut();
    assert_eq!(ic_request_create(&mut request), 0);
    for (key, value, text, unique) in
      [("1", "10", "one", "100"), ("2", "20", "two", "200")]
    {
      let mut op: u32 = 99;
      assert_eq!(
        ic_request_add_write(request, shape, IC_WRITE_KIND_INSERT, &mut op),
        0
      );
      let mut key_len: usize = 0;
      let mut attr_len: usize = 0;
      let key_row = ic_request_key_row(request, op, &mut key_len);
      let attr_row = ic_request_attr_row(request, op, &mut attr_len);
      assert_eq!(key_len, ic_record_row_size(rec) as usize);
      let values = [(id, key), (val, value), (name, text), (code, unique)];
      put(rec, key_row, key_len, &values);
      put(rec, attr_row, attr_len, &values);
    }
    assert_eq!(ic_request_num_ops(request), 2);
    let ret = ic_apid_submitter_execute(submitter, request);
    assert_eq!(ret, 0, "insert: {}", message(ret));
    assert_eq!(ic_request_state(request), IC_COMMIT_STATE_COMMITTED);
    assert!(ic_request_gci(request) > 0);
    assert_eq!(
      mysql(&format!(
        "SELECT id, val FROM {}.capi ORDER BY id",
        IC_IT_DB
      )),
      Some("1\t10\n2\t20".to_string())
    );

    // Run again: the same inserts now find their rows there.
    let ret = ic_apid_submitter_execute(submitter, request);
    assert_eq!(ret, IC_NDB_ERROR_ROW_EXISTS);
    assert_eq!(ic_request_state(request), IC_COMMIT_STATE_ROLLED_BACK);

    // A read by primary key, and one of a row not there, which leaves
    // the transaction committed.
    assert_eq!(ic_request_clear(request), 0);
    let mut first: u32 = 0;
    let mut missing: u32 = 0;
    assert_eq!(
      ic_request_add_read(request, shape, IC_READ_KIND_COMMITTED, &mut first),
      0
    );
    assert_eq!(
      ic_request_add_read(request, shape, IC_READ_KIND_COMMITTED, &mut missing),
      0
    );
    let mut len: usize = 0;
    let row = ic_request_key_row(request, first, &mut len);
    put(rec, row, len, &[(id, "2")]);
    let row = ic_request_key_row(request, missing, &mut len);
    put(rec, row, len, &[(id, "3")]);
    let ret = ic_apid_submitter_execute(submitter, request);
    assert_eq!(ret, 0, "read: {}", message(ret));
    assert_eq!(ic_request_op_error(request, first), 0);
    assert!(ic_request_op_result_len(request, first) > 0);
    let row = ic_request_attr_row(request, first, &mut len);
    assert_eq!(get(rec, row, len, val), "20");
    assert_eq!(get(rec, row, len, name), "'two'");
    assert_eq!(
      ic_request_op_error(request, missing),
      IC_NDB_ERROR_NO_SUCH_ROW
    );

    // Through the unique index.
    let mut index: *mut IcIndexDef = std::ptr::null_mut();
    let ret = ic_apid_submitter_index_bind(
      submitter,
      db.as_ptr(),
      index_name.as_ptr(),
      table_name.as_ptr(),
      &mut index,
    );
    assert_eq!(ret, 0, "index bind: {}", message(ret));
    let mut key_rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(ic_record_create_default(table, &code, 1, &mut key_rec), 0);
    let mut unique: *mut IcOpShape = std::ptr::null_mut();
    assert_eq!(
      ic_op_shape_create_unique(index, key_rec, rec, &mut unique),
      0
    );
    assert_eq!(ic_request_clear(request), 0);
    let mut through: u32 = 0;
    assert_eq!(
      ic_request_add_read(
        request,
        unique,
        IC_READ_KIND_COMMITTED,
        &mut through
      ),
      0
    );
    let row = ic_request_key_row(request, through, &mut len);
    put(key_rec, row, len, &[(code, "100")]);
    let ret = ic_apid_submitter_execute(submitter, request);
    assert_eq!(ret, 0, "read through the index: {}", message(ret));
    let row = ic_request_attr_row(request, through, &mut len);
    assert_eq!(get(rec, row, len, id), "1");

    let mut stats = std::mem::MaybeUninit::<IcExecutorStats>::uninit();
    assert_eq!(
      ic_apid_executor_stats(
        executor,
        stats.as_mut_ptr(),
        std::mem::size_of::<IcExecutorStats>()
      ),
      0
    );
    let stats = stats.assume_init();
    assert_eq!(stats.completed, 4);
    assert_eq!(stats.refused, 0);

    // Once the executor is freed, its submitter refuses, and the request
    // is left as it was.
    ic_apid_executor_free(executor);
    let ret = ic_apid_submitter_execute(submitter, request);
    assert_eq!(ret, IC_ERROR_EXECUTOR_STOPPED);
    assert_eq!(ic_request_num_ops(request), 1);

    ic_request_free(request);
    ic_op_shape_free(unique);
    ic_op_shape_free(shape);
    ic_record_free(key_rec);
    ic_record_free(rec);
    ic_index_def_free(index);
    ic_table_def_free(table);
    ic_apid_submitter_free(submitter);
    ic_apid_global_free(global);
  }
}

/// Rows in the `capi_scan` table.
const IC_IT_CAPI_SCAN_ROWS: u64 = 300;

/// The table the scan test reads: `IC_IT_CAPI_SCAN_ROWS` rows, id 1 up
/// and val ten times the id, so that every row's val sums to a known
/// total.
fn setup_capi_scan_table() -> Option<()> {
  mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.capi_scan;
     CREATE TABLE {db}.capi_scan (
       id INT NOT NULL PRIMARY KEY,
       val INT NOT NULL
     ) ENGINE=NDB;
     INSERT INTO {db}.capi_scan
       WITH RECURSIVE seq (n) AS
         (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < {rows})
       SELECT n, n * 10 FROM seq;",
    db = IC_IT_DB,
    rows = IC_IT_CAPI_SCAN_ROWS
  ))?;
  Some(())
}

/// The sum of every id, 1 up.
fn id_sum() -> u64 {
  IC_IT_CAPI_SCAN_ROWS * (IC_IT_CAPI_SCAN_ROWS + 1) / 2
}

/// What the scan callback saw.
struct BatchesSeen {
  batches: u32,
  rows: u64,
  ids: u64,
  scan: IcScanId,
  rec: *const IcRecord,
  id: u32,
}

/// The scan's callback: reads the batch's rows from inside, as a C
/// callback may, and hands it back.
unsafe extern "C" fn on_batch(
  conn: *mut IcApidConnection,
  batch: IcScanBatchId,
  user_ref: *mut c_void,
) {
  // SAFETY: the test gave a pointer to its `BatchesSeen`, alive
  // throughout.
  let seen = unsafe { &mut *(user_ref as *mut BatchesSeen) };
  seen.batches += 1;
  assert_eq!(ic_scan_batch_scan(batch), seen.scan);
  let mut more = false;
  let mut len: usize = 0;
  // SAFETY: the connection the callback runs on, and its own batch.
  unsafe {
    loop {
      let ret = ic_apid_conn_next_scan_row(conn, batch, &mut more);
      assert_eq!(ret, 0, "a row: {}", message(ret));
      if !more {
        break;
      }
      let row = ic_apid_scan_row(conn, seen.scan, &mut len);
      seen.rows += 1;
      seen.ids += get(seen.rec, row, len, seen.id)
        .parse::<u64>()
        .expect("an id");
    }
    assert_eq!(ic_apid_conn_release_scan_batch(conn, batch), 0);
  }
}

#[test]
fn c_api_scan_on_a_connection_and_through_an_executor() {
  let global = match connect() {
    Some(global) => global,
    None => return,
  };
  if setup_capi_scan_table().is_none() {
    // SAFETY: the handle connect made.
    unsafe { ic_apid_global_free(global) };
    return;
  }
  let db = cs(IC_IT_DB);
  let table_name = cs("capi_scan");
  // SAFETY: every handle below is used while alive and freed once;
  // every pointer is to a local or a row the library gave.
  unsafe {
    let mut conn: *mut IcApidConnection = std::ptr::null_mut();
    assert_eq!(ic_apid_global_create_connection(global, &mut conn), 0);
    let mut table: *mut IcTableDef = std::ptr::null_mut();
    let ret = ic_apid_conn_table_bind(
      conn,
      db.as_ptr(),
      table_name.as_ptr(),
      &mut table,
    );
    assert_eq!(ret, 0, "bind: {}", message(ret));
    let id = field(table, "id");
    let mut rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(
      ic_record_create_default(table, std::ptr::null(), 0, &mut rec),
      0
    );
    let row_size = ic_record_row_size(rec) as usize;
    let mut scan: IcScanId = 0;
    let ret = ic_apid_scan_create(conn, table, rec, &mut scan);
    assert_eq!(ret, 0, "create: {}", message(ret));
    assert_eq!(ic_apid_scan_state(conn, scan), IC_SCAN_IDLE);
    let mut args = std::mem::MaybeUninit::<IcScanArgs>::uninit();
    ic_scan_args_init(args.as_mut_ptr(), std::mem::size_of::<IcScanArgs>());
    let mut args = args.assume_init();
    args.batch_rows = 50;
    args.timeout_ms = 30_000;

    // Row by row, in each lock mode.
    for lock in [
      IC_SCAN_LOCK_COMMITTED,
      IC_SCAN_LOCK_SHARED,
      IC_SCAN_LOCK_EXCLUSIVE,
    ] {
      args.lock = lock;
      let ret = ic_apid_conn_start_scan(conn, scan, &args);
      assert_eq!(ret, 0, "start: {}", message(ret));
      // Running: not to be freed.
      assert_eq!(ic_apid_scan_free(conn, scan), IC_ERROR_SCAN_ACTIVE);
      let mut more = true;
      let mut ids: u64 = 0;
      let mut rows: u64 = 0;
      let mut len: usize = 0;
      loop {
        let ret = ic_apid_conn_next_row(conn, scan, &mut more);
        assert_eq!(ret, 0, "next row: {}", message(ret));
        if !more {
          break;
        }
        let row = ic_apid_scan_row(conn, scan, &mut len);
        assert_eq!(len, row_size);
        ids += get(rec, row, len, id).parse::<u64>().expect("an id");
        rows += 1;
      }
      assert_eq!(rows, IC_IT_CAPI_SCAN_ROWS, "lock {}", lock);
      assert_eq!(ids, id_sum());
      assert!(ic_apid_scan_is_done(conn, scan));
      assert_eq!(ic_apid_scan_state(conn, scan), IC_SCAN_DONE);
      assert_eq!(ic_apid_scan_error(conn, scan), 0);
      assert_eq!(ic_apid_scan_rows_read(conn, scan), IC_IT_CAPI_SCAN_ROWS);
    }

    // A batch at a time from the ready list, into the program's row.
    args.lock = IC_SCAN_LOCK_COMMITTED;
    assert_eq!(ic_apid_conn_start_scan(conn, scan, &args), 0);
    let mut row = vec![0u8; row_size];
    let mut rows: u64 = 0;
    let mut ids: u64 = 0;
    let mut polls: u32 = 0;
    while !ic_apid_scan_is_done(conn, scan) {
      polls += 1;
      assert!(polls < 300, "the scan did not end");
      ic_apid_conn_flush(conn, 100, true, std::ptr::null_mut());
      let mut batch: IcScanBatchId = 0;
      while ic_apid_conn_get_next_scan_batch(conn, &mut batch) {
        assert!(ic_apid_scan_batch_rows(conn, batch) <= 50);
        let mut more = true;
        loop {
          let ret = ic_apid_conn_next_scan_row_into(
            conn,
            batch,
            row.as_mut_ptr(),
            row.len(),
            &mut more,
          );
          assert_eq!(ret, 0, "a row: {}", message(ret));
          if !more {
            break;
          }
          ids += get(rec, row.as_ptr(), row.len(), id)
            .parse::<u64>()
            .expect("an id");
          rows += 1;
        }
        assert_eq!(ic_apid_conn_release_scan_batch(conn, batch), 0);
      }
    }
    assert_eq!(rows, IC_IT_CAPI_SCAN_ROWS);
    assert_eq!(ids, id_sum());

    // To a callback, which reads and hands back each batch.
    let mut seen = BatchesSeen {
      batches: 0,
      rows: 0,
      ids: 0,
      scan,
      rec,
      id,
    };
    args.callback = Some(on_batch);
    args.user_ref = &mut seen as *mut BatchesSeen as *mut c_void;
    assert_eq!(ic_apid_conn_start_scan(conn, scan, &args), 0);
    let mut polls: u32 = 0;
    while !ic_apid_scan_is_done(conn, scan) {
      polls += 1;
      assert!(polls < 300, "the callback scan did not end");
      ic_apid_conn_flush(conn, 100, true, std::ptr::null_mut());
    }
    assert_eq!(seen.rows, IC_IT_CAPI_SCAN_ROWS);
    assert_eq!(seen.ids, id_sum());
    assert!(seen.batches >= 6, "{} batches", seen.batches);
    args.callback = None;
    args.user_ref = std::ptr::null_mut();

    // Closed early: done with no error.
    assert_eq!(ic_apid_conn_start_scan(conn, scan, &args), 0);
    let mut more = false;
    assert_eq!(ic_apid_conn_next_row(conn, scan, &mut more), 0);
    assert!(more);
    assert_eq!(ic_apid_conn_close_scan(conn, scan), 0);
    let mut polls: u32 = 0;
    while !ic_apid_scan_is_done(conn, scan) {
      polls += 1;
      assert!(polls < 300, "the close did not end");
      ic_apid_conn_flush(conn, 100, true, std::ptr::null_mut());
    }
    assert_eq!(ic_apid_scan_error(conn, scan), 0);
    assert_eq!(ic_apid_scan_free(conn, scan), 0);
    assert_eq!(ic_apid_scan_state(conn, scan), IC_SCAN_UNKNOWN);

    // Rows taken over from an exclusive scan on a transaction: each row's
    // val set to its id, then committed.
    let mut taken: IcScanId = 0;
    assert_eq!(ic_apid_scan_create(conn, table, rec, &mut taken), 0);
    let any = IcTransactionHint {
      kind: IC_HINT_ANY,
      value: 0,
    };
    let mut trans: IcTransId = 0;
    let ret = ic_apid_conn_start_transaction(conn, any, table, &mut trans);
    assert_eq!(ret, 0, "start a transaction: {}", message(ret));
    let mut queries: Vec<IcQueryId> = Vec::new();
    while (queries.len() as u64) < IC_IT_CAPI_SCAN_ROWS {
      let mut q: IcQueryId = 0;
      assert_eq!(ic_apid_query_create(conn, table, rec, rec, &mut q), 0);
      queries.push(q);
    }
    let mut takeover = std::mem::MaybeUninit::<IcTakeoverArgs>::uninit();
    ic_takeover_args_init(
      takeover.as_mut_ptr(),
      std::mem::size_of::<IcTakeoverArgs>(),
    );
    let takeover = takeover.assume_init();
    let val = field(table, "val");
    args.lock = IC_SCAN_LOCK_EXCLUSIVE;
    args.trans = trans;
    assert_eq!(ic_apid_conn_start_scan(conn, taken, &args), 0);
    // None read yet: nothing to take over.
    assert_eq!(
      ic_apid_conn_update_scan_row(conn, queries[0], taken, &takeover),
      IC_ERROR_NO_SCAN_ROW
    );
    let mut used: usize = 0;
    loop {
      let mut more = false;
      let ret = ic_apid_conn_next_row(conn, taken, &mut more);
      assert_eq!(ret, 0, "next row: {}", message(ret));
      if !more {
        break;
      }
      let mut len: usize = 0;
      let row = ic_apid_scan_row(conn, taken, &mut len);
      let id_text = get(rec, row, len, id);
      let q = queries[used];
      used += 1;
      let mut attr_len: usize = 0;
      let attr = ic_apid_query_attr_row(conn, q, &mut attr_len);
      put(rec, attr, attr_len, &[(id, &id_text), (val, &id_text)]);
      let ret = ic_apid_conn_update_scan_row(conn, q, taken, &takeover);
      assert_eq!(ret, 0, "update the row: {}", message(ret));
    }
    assert_eq!(used as u64, IC_IT_CAPI_SCAN_ROWS);
    // The scan ended only once every batch's takeovers were confirmed.
    for q in &queries {
      assert_eq!(ic_apid_query_state(conn, *q), IC_QUERY_COMPLETED);
      assert_eq!(ic_apid_query_error(conn, *q), 0);
    }
    assert_eq!(ic_apid_conn_commit_transaction(conn, trans), 0);
    assert_eq!(finish(conn, trans), IC_COMMIT_STATE_COMMITTED);
    assert_eq!(ic_apid_conn_close_transaction(conn, trans), 0);
    assert_eq!(
      mysql(&format!("SELECT SUM(val) FROM {}.capi_scan", IC_IT_DB)),
      Some(id_sum().to_string())
    );
    let mut q: IcQueryId = 0;
    while ic_apid_conn_get_next_executed_query(conn, &mut q) {}
    for q in queries {
      assert_eq!(ic_apid_query_free(conn, q), 0);
    }
    assert_eq!(ic_apid_scan_free(conn, taken), 0);
    args.lock = IC_SCAN_LOCK_COMMITTED;
    args.trans = IC_NO_TRANS;

    // Through an executor, with a reader.
    let mut options = std::mem::MaybeUninit::<IcExecutorOptions>::uninit();
    ic_executor_options_init(
      options.as_mut_ptr(),
      std::mem::size_of::<IcExecutorOptions>(),
    );
    let options = options.assume_init();
    assert_eq!(options.scans, 64);
    let mut executor: *mut IcApidExecutor = std::ptr::null_mut();
    let ret = ic_apid_executor_start(global, &options, &mut executor);
    assert_eq!(ret, 0, "start: {}", message(ret));
    let mut submitter: *mut IcApidSubmitter = std::ptr::null_mut();
    assert_eq!(ic_apid_executor_submitter(executor, &mut submitter), 0);
    let mut shape: *mut IcScanShape = std::ptr::null_mut();
    assert_eq!(ic_scan_shape_create(table, rec, &mut shape), 0);
    let mut reader: *mut IcScanReader = std::ptr::null_mut();
    assert_eq!(ic_scan_reader_create(shape, &mut reader), 0);
    assert!(ic_scan_reader_is_done(reader));
    for read_ahead in [false, true] {
      args.read_ahead = read_ahead;
      let ret = ic_apid_submitter_scan(submitter, reader, &args);
      assert_eq!(ret, 0, "scan: {}", message(ret));
      let mut more = true;
      let mut rows: u64 = 0;
      let mut ids: u64 = 0;
      let mut len: usize = 0;
      loop {
        let ret = ic_scan_reader_next_row(reader, &mut more);
        assert_eq!(ret, 0, "next row: {}", message(ret));
        if !more {
          break;
        }
        let row = ic_scan_reader_row(reader, &mut len);
        ids += get(rec, row, len, id).parse::<u64>().expect("an id");
        rows += 1;
      }
      assert_eq!(rows, IC_IT_CAPI_SCAN_ROWS);
      assert_eq!(ids, id_sum());
      assert_eq!(ic_scan_reader_rows_read(reader), IC_IT_CAPI_SCAN_ROWS);
      assert_eq!(ic_scan_reader_error(reader), 0);
      assert!(ic_scan_reader_is_done(reader));
    }
    // Read ahead with a lock is refused.
    args.lock = IC_SCAN_LOCK_SHARED;
    assert_eq!(
      ic_apid_submitter_scan(submitter, reader, &args),
      IC_ERROR_NOT_SUPPORTED
    );
    args.read_ahead = false;
    // A close part way, then the reader runs again into a row of the
    // program's.
    assert_eq!(ic_apid_submitter_scan(submitter, reader, &args), 0);
    let mut more = false;
    assert_eq!(ic_scan_reader_next_row(reader, &mut more), 0);
    assert!(more);
    assert_eq!(
      ic_apid_submitter_scan(submitter, reader, &args),
      IC_ERROR_SCAN_ACTIVE
    );
    assert_eq!(ic_scan_reader_close(reader), 0);
    assert!(ic_scan_reader_is_done(reader));
    assert_eq!(ic_apid_submitter_scan(submitter, reader, &args), 0);
    let mut rows: u64 = 0;
    loop {
      let ret = ic_scan_reader_next_row_into(
        reader,
        row.as_mut_ptr(),
        row.len(),
        &mut more,
      );
      assert_eq!(ret, 0, "next row: {}", message(ret));
      if !more {
        break;
      }
      rows += 1;
    }
    assert_eq!(rows, IC_IT_CAPI_SCAN_ROWS);
    let mut stats = std::mem::MaybeUninit::<IcExecutorStats>::uninit();
    assert_eq!(
      ic_apid_executor_stats(
        executor,
        stats.as_mut_ptr(),
        std::mem::size_of::<IcExecutorStats>()
      ),
      0
    );
    let stats = stats.assume_init();
    assert_eq!(stats.scans, 4);

    ic_scan_reader_free(reader);
    ic_scan_shape_free(shape);
    ic_apid_submitter_free(submitter);
    ic_apid_executor_free(executor);
    ic_record_free(rec);
    ic_table_def_free(table);
    ic_apid_conn_free(conn);
    ic_apid_global_free(global);
  }
}

/// Rows in the `capi_range` table, and how many share each value of a.
const IC_IT_CAPI_RANGE_ROWS: u64 = 300;
const IC_IT_CAPI_RANGE_EACH: u64 = 10;

/// The table the index scan test reads: id 1 up, a the id modulo 30,
/// with an ordered index over a.
fn setup_capi_range_table() -> Option<()> {
  mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.capi_range;
     CREATE TABLE {db}.capi_range (
       id INT NOT NULL PRIMARY KEY,
       a INT NOT NULL,
       KEY ix_a (a)
     ) ENGINE=NDB;
     INSERT INTO {db}.capi_range
       WITH RECURSIVE seq (n) AS
         (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < {rows})
       SELECT n, n % 30 FROM seq;",
    db = IC_IT_DB,
    rows = IC_IT_CAPI_RANGE_ROWS
  ))?;
  Some(())
}

/// A range of one column, both bounds inclusive, as `range_no`.
fn one_column_range(range_no: u32) -> IcScanRange {
  let mut range = std::mem::MaybeUninit::<IcScanRange>::uninit();
  // SAFETY: a local, its size given.
  unsafe {
    ic_scan_range_init(range.as_mut_ptr(), std::mem::size_of::<IcScanRange>());
  }
  // SAFETY: filled by ic_scan_range_init above.
  let mut range = unsafe { range.assume_init() };
  range.low_columns = 1;
  range.low_inclusive = true;
  range.high_columns = 1;
  range.high_inclusive = true;
  range.range_no = range_no;
  range
}

/// True if `values` never fall, or with `descending` never rise.
fn in_order(values: &[u64], descending: bool) -> bool {
  let mut at: usize = 1;
  while at < values.len() {
    let rises = values[at] >= values[at - 1];
    let falls = values[at] <= values[at - 1];
    if (descending && !falls) || (!descending && !rises) {
      return false;
    }
    at += 1;
  }
  true
}

#[test]
fn c_api_index_scans_on_a_connection_and_through_an_executor() {
  let global = match connect() {
    Some(global) => global,
    None => return,
  };
  if setup_capi_range_table().is_none() {
    // SAFETY: the handle connect made.
    unsafe { ic_apid_global_free(global) };
    return;
  }
  let db = cs(IC_IT_DB);
  let table_name = cs("capi_range");
  let index_name = cs("ix_a");
  // SAFETY: every handle below is used while alive and freed once;
  // every pointer is to a local or a row the library gave.
  unsafe {
    let mut conn: *mut IcApidConnection = std::ptr::null_mut();
    assert_eq!(ic_apid_global_create_connection(global, &mut conn), 0);
    let mut table: *mut IcTableDef = std::ptr::null_mut();
    let ret = ic_apid_conn_table_bind(
      conn,
      db.as_ptr(),
      table_name.as_ptr(),
      &mut table,
    );
    assert_eq!(ret, 0, "bind: {}", message(ret));
    let mut index: *mut IcIndexDef = std::ptr::null_mut();
    let ret = ic_apid_conn_index_bind(
      conn,
      db.as_ptr(),
      index_name.as_ptr(),
      table_name.as_ptr(),
      &mut index,
    );
    assert_eq!(ret, 0, "index bind: {}", message(ret));
    let a = field(table, "a");
    let mut rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(
      ic_record_create_default(table, std::ptr::null(), 0, &mut rec),
      0
    );
    let mut key_rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(ic_record_create_default(table, &a, 1, &mut key_rec), 0);
    let key_size = ic_record_row_size(key_rec) as usize;
    let mut low = vec![0u8; key_size];
    let mut high = vec![0u8; key_size];
    put(key_rec, low.as_mut_ptr(), key_size, &[(a, "5")]);
    put(key_rec, high.as_mut_ptr(), key_size, &[(a, "9")]);
    let mut scan: IcScanId = 0;
    let ret = ic_apid_index_scan_create(conn, index, key_rec, rec, &mut scan);
    assert_eq!(ret, 0, "create: {}", message(ret));
    let mut args = std::mem::MaybeUninit::<IcScanArgs>::uninit();
    ic_scan_args_init(args.as_mut_ptr(), std::mem::size_of::<IcScanArgs>());
    let mut args = args.assume_init();
    assert!(!args.ordered && !args.descending);
    args.batch_rows = 4;
    args.timeout_ms = 30_000;
    let range = one_column_range(0);

    // a from 5 to 9, in the index's order both ways.
    for descending in [false, true] {
      let ret = ic_apid_conn_add_scan_range(
        conn,
        scan,
        &range,
        low.as_ptr(),
        low.len(),
        high.as_ptr(),
        high.len(),
      );
      assert_eq!(ret, 0, "add a range: {}", message(ret));
      args.ordered = true;
      args.descending = descending;
      assert_eq!(ic_apid_conn_start_scan(conn, scan, &args), 0);
      let mut values: Vec<u64> = Vec::new();
      loop {
        let mut more = false;
        let ret = ic_apid_conn_next_row(conn, scan, &mut more);
        assert_eq!(ret, 0, "next row: {}", message(ret));
        if !more {
          break;
        }
        let mut len: usize = 0;
        let row = ic_apid_scan_row(conn, scan, &mut len);
        values.push(get(rec, row, len, a).parse::<u64>().expect("a value"));
      }
      assert_eq!(values.len() as u64, 5 * IC_IT_CAPI_RANGE_EACH);
      assert!(in_order(&values, descending), "{:?}", values);
      assert_eq!(ic_apid_scan_error(conn, scan), 0);
    }

    // Two numbered ranges, each row's number given back: 3 for a = 20,
    // 1 for a = 2, the ranges added in the order of their numbers.
    let mut two = vec![0u8; key_size];
    let mut twenty = vec![0u8; key_size];
    put(key_rec, two.as_mut_ptr(), key_size, &[(a, "2")]);
    put(key_rec, twenty.as_mut_ptr(), key_size, &[(a, "20")]);
    for (range_no, row) in [(1u32, &two), (3u32, &twenty)] {
      let ret = ic_apid_conn_add_scan_range(
        conn,
        scan,
        &one_column_range(range_no),
        row.as_ptr(),
        row.len(),
        row.as_ptr(),
        row.len(),
      );
      assert_eq!(ret, 0, "add a range: {}", message(ret));
    }
    args.descending = false;
    assert_eq!(ic_apid_conn_start_scan(conn, scan, &args), 0);
    let mut seen: Vec<(u64, u32)> = Vec::new();
    loop {
      let mut more = false;
      assert_eq!(ic_apid_conn_next_row(conn, scan, &mut more), 0);
      if !more {
        break;
      }
      let mut len: usize = 0;
      let row = ic_apid_scan_row(conn, scan, &mut len);
      let value = get(rec, row, len, a).parse::<u64>().expect("a value");
      seen.push((value, ic_apid_scan_range_no(conn, scan)));
    }
    assert_eq!(seen.len() as u64, 2 * IC_IT_CAPI_RANGE_EACH);
    let mut at: usize = 0;
    while at < seen.len() {
      let expected = if (at as u64) < IC_IT_CAPI_RANGE_EACH {
        (2, 1)
      } else {
        (20, 3)
      };
      assert_eq!(seen[at], expected, "row {}", at);
      at += 1;
    }

    // Refusals: a number above 4095, and an order on a table scan.
    let too_big = one_column_range(4096);
    assert_eq!(
      ic_apid_conn_add_scan_range(
        conn,
        scan,
        &too_big,
        low.as_ptr(),
        low.len(),
        high.as_ptr(),
        high.len(),
      ),
      IC_ERROR_BAD_RANGE
    );
    let mut table_scan: IcScanId = 0;
    assert_eq!(ic_apid_scan_create(conn, table, rec, &mut table_scan), 0);
    assert_eq!(
      ic_apid_conn_start_scan(conn, table_scan, &args),
      IC_ERROR_NOT_SUPPORTED
    );
    assert_eq!(
      ic_apid_conn_add_scan_range(
        conn,
        table_scan,
        &range,
        low.as_ptr(),
        low.len(),
        high.as_ptr(),
        high.len(),
      ),
      IC_ERROR_NOT_SUPPORTED
    );
    assert_eq!(ic_apid_scan_free(conn, table_scan), 0);
    assert_eq!(ic_apid_scan_free(conn, scan), 0);

    // Through an executor: a shape over the index, the range added to
    // the reader, the run merged by the reader.
    let mut options = std::mem::MaybeUninit::<IcExecutorOptions>::uninit();
    ic_executor_options_init(
      options.as_mut_ptr(),
      std::mem::size_of::<IcExecutorOptions>(),
    );
    let options = options.assume_init();
    let mut executor: *mut IcApidExecutor = std::ptr::null_mut();
    let ret = ic_apid_executor_start(global, &options, &mut executor);
    assert_eq!(ret, 0, "start: {}", message(ret));
    let mut submitter: *mut IcApidSubmitter = std::ptr::null_mut();
    assert_eq!(ic_apid_executor_submitter(executor, &mut submitter), 0);
    let mut shape: *mut IcScanShape = std::ptr::null_mut();
    let ret = ic_scan_shape_create_index(index, key_rec, rec, &mut shape);
    assert_eq!(ret, 0, "shape: {}", message(ret));
    let mut reader: *mut IcScanReader = std::ptr::null_mut();
    assert_eq!(ic_scan_reader_create(shape, &mut reader), 0);
    for descending in [false, true] {
      let ret = ic_scan_reader_add_range(
        reader,
        &range,
        low.as_ptr(),
        low.len(),
        high.as_ptr(),
        high.len(),
      );
      assert_eq!(ret, 0, "add a range: {}", message(ret));
      args.descending = descending;
      let ret = ic_apid_submitter_scan(submitter, reader, &args);
      assert_eq!(ret, 0, "scan: {}", message(ret));
      let mut values: Vec<u64> = Vec::new();
      loop {
        let mut more = false;
        let ret = ic_scan_reader_next_row(reader, &mut more);
        assert_eq!(ret, 0, "next row: {}", message(ret));
        if !more {
          break;
        }
        let mut len: usize = 0;
        let row = ic_scan_reader_row(reader, &mut len);
        values.push(get(rec, row, len, a).parse::<u64>().expect("a value"));
        assert_eq!(ic_scan_reader_range_no(reader), 0);
      }
      assert_eq!(values.len() as u64, 5 * IC_IT_CAPI_RANGE_EACH);
      assert!(in_order(&values, descending), "{:?}", values);
      assert_eq!(ic_scan_reader_error(reader), 0);
    }
    // A shape over the table takes no range.
    let mut table_shape: *mut IcScanShape = std::ptr::null_mut();
    assert_eq!(ic_scan_shape_create(table, rec, &mut table_shape), 0);
    let mut table_reader: *mut IcScanReader = std::ptr::null_mut();
    assert_eq!(ic_scan_reader_create(table_shape, &mut table_reader), 0);
    assert_eq!(
      ic_scan_reader_add_range(
        table_reader,
        &range,
        low.as_ptr(),
        low.len(),
        high.as_ptr(),
        high.len(),
      ),
      IC_ERROR_NOT_SUPPORTED
    );

    ic_scan_reader_free(table_reader);
    ic_scan_shape_free(table_shape);
    ic_scan_reader_free(reader);
    ic_scan_shape_free(shape);
    ic_apid_submitter_free(submitter);
    ic_apid_executor_free(executor);
    ic_record_free(key_rec);
    ic_record_free(rec);
    ic_index_def_free(index);
    ic_table_def_free(table);
    ic_apid_conn_free(conn);
    ic_apid_global_free(global);
  }
}

/// Rows in the `capi_prog` table: id 1 up, val ten times the id, cnt 0.
const IC_IT_CAPI_PROG_ROWS: i64 = 10;

fn setup_capi_prog_table() -> Option<()> {
  let mut values: Vec<String> = Vec::new();
  let mut id: i64 = 1;
  while id <= IC_IT_CAPI_PROG_ROWS {
    values.push(format!("({}, {}, 0)", id, id * 10));
    id += 1;
  }
  mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.capi_prog;
     CREATE TABLE {db}.capi_prog (
       id INT NOT NULL PRIMARY KEY,
       val INT,
       cnt BIGINT UNSIGNED NOT NULL
     ) ENGINE=NDB;
     INSERT INTO {db}.capi_prog VALUES {values};",
    db = IC_IT_DB,
    values = values.join(",")
  ))?;
  Some(())
}

#[test]
fn c_api_programs_filter_scans_guard_updates_and_count() {
  let global = match connect() {
    Some(global) => global,
    None => return,
  };
  if setup_capi_prog_table().is_none() {
    // SAFETY: the handle connect made.
    unsafe { ic_apid_global_free(global) };
    return;
  }
  let db = cs(IC_IT_DB);
  let table_name = cs("capi_prog");
  // SAFETY: every handle below is used while alive and freed once;
  // every pointer is to a local or a row the library gave.
  unsafe {
    let mut conn: *mut IcApidConnection = std::ptr::null_mut();
    assert_eq!(ic_apid_global_create_connection(global, &mut conn), 0);
    let mut table: *mut IcTableDef = std::ptr::null_mut();
    let ret = ic_apid_conn_table_bind(
      conn,
      db.as_ptr(),
      table_name.as_ptr(),
      &mut table,
    );
    assert_eq!(ret, 0, "bind: {}", message(ret));
    let id = field(table, "id");
    let val = field(table, "val");
    let cnt = field(table, "cnt");
    let mut rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(
      ic_record_create_default(table, std::ptr::null(), 0, &mut rec),
      0
    );
    let mut key_rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(ic_record_create_default(table, &id, 1, &mut key_rec), 0);

    // WHERE val > input 0, on a scan.
    let mut builder: *mut IcProgramBuilder = std::ptr::null_mut();
    assert_eq!(ic_program_builder_create(table, &mut builder), 0);
    let column = ic_program_column(builder, val);
    let low = ic_program_input(builder, 0);
    let above = ic_program_compare(builder, IC_CMP_GT, column, low);
    assert_ne!(above, IC_PROGRAM_NONE);
    let mut filter: *mut IcProgram = std::ptr::null_mut();
    let ret = ic_program_builder_finish(builder, above, &mut filter);
    assert_eq!(ret, 0, "finish: {}", message(ret));
    assert!(!ic_program_writes(filter));
    assert_eq!(ic_program_inputs(filter), 1);
    // An unknown comparison makes nothing the finish takes.
    let none = ic_program_compare(builder, 99, column, low);
    assert_eq!(none, IC_PROGRAM_NONE);
    let mut refused: *mut IcProgram = std::ptr::null_mut();
    let ret = ic_program_builder_finish(builder, none, &mut refused);
    assert_eq!(ret, IC_ERROR_BAD_PROGRAM);
    ic_program_builder_free(builder);

    let mut scan: IcScanId = 0;
    assert_eq!(ic_apid_scan_create(conn, table, rec, &mut scan), 0);
    assert_eq!(ic_apid_conn_set_scan_program(conn, scan, filter), 0);
    assert_eq!(ic_apid_conn_set_scan_input(conn, scan, 0, 45), 0);
    assert_eq!(
      ic_apid_conn_set_scan_input(conn, scan, 16, 1),
      IC_ERROR_BAD_PROGRAM
    );
    let mut args = std::mem::MaybeUninit::<IcScanArgs>::uninit();
    ic_scan_args_init(args.as_mut_ptr(), std::mem::size_of::<IcScanArgs>());
    let args = args.assume_init();
    assert_eq!(ic_apid_conn_start_scan(conn, scan, &args), 0);
    let mut ids: Vec<i64> = Vec::new();
    let mut more = true;
    let mut len: usize = 0;
    loop {
      let ret = ic_apid_conn_next_row(conn, scan, &mut more);
      assert_eq!(ret, 0, "next row: {}", message(ret));
      if !more {
        break;
      }
      let row = ic_apid_scan_row(conn, scan, &mut len);
      ids.push(get(rec, row, len, id).parse().expect("an id"));
    }
    ids.sort();
    assert_eq!(ids, [5, 6, 7, 8, 9, 10], "val above 45");

    // WHERE val < input 0, val = val + 1, on an update of row 3.
    assert_eq!(ic_program_builder_create(table, &mut builder), 0);
    let column = ic_program_column(builder, val);
    let high = ic_program_input(builder, 0);
    let below = ic_program_compare(builder, IC_CMP_LT, column, high);
    let one = ic_program_integer(builder, 1);
    let sum = ic_program_add(builder, column, one);
    assert_eq!(ic_program_assign(builder, val, sum), 0);
    let mut guard: *mut IcProgram = std::ptr::null_mut();
    assert_eq!(ic_program_builder_finish(builder, below, &mut guard), 0);
    assert!(ic_program_writes(guard));
    ic_program_builder_free(builder);
    let mut query: IcQueryId = 0;
    assert_eq!(
      ic_apid_query_create(conn, table, key_rec, key_rec, &mut query),
      0
    );
    assert_eq!(ic_apid_query_set_program(conn, query, guard), 0);
    let key = ic_apid_query_key_row(conn, query, &mut len);
    put(key_rec, key, len, &[(id, "3")]);
    let mut write = std::mem::MaybeUninit::<IcWriteKeyArgs>::uninit();
    ic_write_key_args_init(
      write.as_mut_ptr(),
      std::mem::size_of::<IcWriteKeyArgs>(),
    );
    let mut write = write.assume_init();
    write.kind = IC_WRITE_KIND_UPDATE;
    // Row 3's val is 30: below 20 it is not, below 40 it is.
    for (limit, error, state, after) in [
      (
        20,
        IC_NDB_ERROR_CONDITION,
        IC_COMMIT_STATE_ROLLED_BACK,
        "30",
      ),
      (40, 0, IC_COMMIT_STATE_COMMITTED, "31"),
    ] {
      assert_eq!(ic_apid_query_set_input(conn, query, 0, limit), 0);
      let hint = ic_apid_conn_hint_for_key(conn, table, key_rec, key, len);
      let mut trans: IcTransId = 0;
      assert_eq!(
        ic_apid_conn_start_transaction(conn, hint, table, &mut trans),
        0
      );
      let ret = ic_apid_conn_write_key(conn, query, trans, &write);
      assert_eq!(ret, 0, "define: {}", message(ret));
      assert_eq!(ic_apid_conn_commit_transaction(conn, trans), 0);
      assert_eq!(finish(conn, trans), state, "below {}", limit);
      let seen = [
        ic_apid_query_error(conn, query),
        ic_apid_transaction_error(conn, trans),
      ];
      assert!(seen.contains(&error), "below {}: {:?}", limit, seen);
      let mut done: IcQueryId = 0;
      while ic_apid_conn_get_next_executed_query(conn, &mut done) {}
      assert_eq!(ic_apid_conn_close_transaction(conn, trans), 0);
      let seen_sql = mysql(&format!(
        "SELECT val FROM {}.capi_prog WHERE id = 3",
        IC_IT_DB
      ));
      assert_eq!(seen_sql, Some(after.to_string()), "below {}", limit);
    }

    // cnt = cnt + input 0 through an executor, cnt read back after.
    assert_eq!(ic_program_builder_create(table, &mut builder), 0);
    let column = ic_program_column(builder, cnt);
    let n = ic_program_input(builder, 0);
    let sum = ic_program_add(builder, column, n);
    assert_eq!(ic_program_assign(builder, cnt, sum), 0);
    let mut counter: *mut IcProgram = std::ptr::null_mut();
    let ret = ic_program_builder_finish_no_condition(builder, &mut counter);
    assert_eq!(ret, 0, "finish: {}", message(ret));
    ic_program_builder_free(builder);
    let mut options = std::mem::MaybeUninit::<IcExecutorOptions>::uninit();
    ic_executor_options_init(
      options.as_mut_ptr(),
      std::mem::size_of::<IcExecutorOptions>(),
    );
    let options = options.assume_init();
    let mut executor: *mut IcApidExecutor = std::ptr::null_mut();
    assert_eq!(ic_apid_executor_start(global, &options, &mut executor), 0);
    let mut submitter: *mut IcApidSubmitter = std::ptr::null_mut();
    assert_eq!(ic_apid_executor_submitter(executor, &mut submitter), 0);
    // The executor's own binding of the table, and records over it; the
    // program, made over the connection's, is for the same table.
    let mut etable: *mut IcTableDef = std::ptr::null_mut();
    let ret = ic_apid_submitter_table_bind(
      submitter,
      db.as_ptr(),
      table_name.as_ptr(),
      &mut etable,
    );
    assert_eq!(ret, 0, "bind: {}", message(ret));
    let mut ekey_rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(ic_record_create_default(etable, &id, 1, &mut ekey_rec), 0);
    let mut ecnt_rec: *mut IcRecord = std::ptr::null_mut();
    assert_eq!(ic_record_create_default(etable, &cnt, 1, &mut ecnt_rec), 0);
    let mut plain: *mut IcOpShape = std::ptr::null_mut();
    assert_eq!(
      ic_op_shape_create(etable, ekey_rec, ekey_rec, &mut plain),
      0
    );
    let mut counting: *mut IcOpShape = std::ptr::null_mut();
    assert_eq!(ic_op_shape_with_program(plain, counter, &mut counting), 0);
    let mut reading: *mut IcOpShape = std::ptr::null_mut();
    assert_eq!(
      ic_op_shape_reading_after(counting, ecnt_rec, &mut reading),
      0
    );
    let mut request: *mut IcRequest = std::ptr::null_mut();
    assert_eq!(ic_request_create(&mut request), 0);
    let mut op: u32 = 0;
    assert_eq!(
      ic_request_add_write(request, reading, IC_WRITE_KIND_UPDATE, &mut op),
      0
    );
    let row = ic_request_key_row(request, op, &mut len);
    put(ekey_rec, row, len, &[(id, "7")]);
    let mut total: i64 = 0;
    for step in [5i64, 6, 7] {
      assert_eq!(ic_request_set_input(request, op, 0, step), 0);
      let ret = ic_apid_submitter_execute(submitter, request);
      assert_eq!(ret, 0, "count: {}", message(ret));
      assert_eq!(ic_request_op_error(request, op), 0);
      total += step;
      let after = ic_request_after_row(request, op, &mut len);
      assert!(!after.is_null());
      assert_eq!(get(ecnt_rec, after, len, cnt), total.to_string());
    }
    let seen_sql = mysql(&format!(
      "SELECT cnt FROM {}.capi_prog WHERE id = 7",
      IC_IT_DB
    ));
    assert_eq!(seen_sql, Some(total.to_string()));

    ic_request_free(request);
    ic_op_shape_free(reading);
    ic_op_shape_free(counting);
    ic_op_shape_free(plain);
    ic_record_free(ecnt_rec);
    ic_record_free(ekey_rec);
    ic_table_def_free(etable);
    ic_apid_submitter_free(submitter);
    ic_apid_executor_free(executor);
    ic_program_free(counter);
    ic_program_free(guard);
    ic_program_free(filter);
    assert_eq!(ic_apid_query_free(conn, query), 0);
    assert_eq!(ic_apid_scan_free(conn, scan), 0);
    ic_record_free(key_rec);
    ic_record_free(rec);
    ic_table_def_free(table);
    ic_apid_conn_free(conn);
    ic_apid_global_free(global);
  }
}
