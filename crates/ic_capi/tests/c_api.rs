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
