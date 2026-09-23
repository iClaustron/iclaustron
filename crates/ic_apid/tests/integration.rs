// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Integration tests against a live RonDB 26.10 cluster (chapter 08,
//! "Integration tests"), run by `cargo xtask test-integration`.
//!
//! Three environment variables say what there is to test against, and
//! a test that lacks what it needs says so and returns, rather than
//! failing:
//!
//! - `IC_TEST_CONNECTSTRING`: the management server, `host:port`.
//!   Without it every test returns at once.
//! - `IC_TEST_MYSQL`: a `mysql` client command with its connection
//!   options, such as `mysql -uroot -S /tmp/rondb-dev.sock`, run
//!   through `sh -c`. The tests create their tables through it, in the
//!   database `ic_it`, so that what they read is what a MySQL server
//!   makes, and they check what they wrote through it too.
//! - `IC_TEST_NDB_MGM`: an `ndb_mgm` command with its connect string,
//!   such as `ndb_mgm -c localhost:1186`, for the `failure` group,
//!   which stops a data node and starts it again.
//!
//! The groups are the phase-5 ones: `connect`, `dict`, `pk`, `uk`,
//! `types`, `failure`. Each test is its own program against the
//! cluster, as an application would be: it takes an API node id, binds
//! its tables, does its work through the public API and lets go.

#![cfg(feature = "integration")]
#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::io::Write;
use std::process::Command;
use std::process::Stdio;
use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::IndexDef;
use ic_apid::dict_cache::TableDef;
use ic_apid::key_op;
use ic_apid::query::QueryId;
use ic_apid::query::QueryState;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::TransId;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::record::Record;
use ic_apid::text_row;
use ic_apid::transaction::CommitState;
use ic_apid::transaction::TransactionHint;
use ic_ndb_signals::tc_key;
use ic_port::time;
use ic_port::IcError;

/// The database the tests make their tables in.
const IC_IT_DB: &str = "ic_it";
/// How long to wait for the data nodes at start.
const IC_IT_START_WAIT_MS: u32 = 30_000;
/// How long a transaction may take before the test gives up on it.
const IC_IT_TRANS_WAIT_MS: u64 = 10_000;
/// How long the dictionary may take to notice a table changed.
const IC_IT_ALTER_WAIT_MS: u64 = 10_000;
/// How long a stopped data node may take to be started again.
const IC_IT_RESTART_WAIT_MS: u64 = 240_000;
/// How many times to try for an API node id, a second apart: the one
/// the last test let go of may not be free again at once.
const IC_IT_START_TRIES: u32 = 10;

// ---- The cluster, the MySQL server and the management client ----

/// The cluster under test: our global, stopped when the test is done.
struct Cluster {
  global: ApidGlobal,
}

impl Drop for Cluster {
  fn drop(&mut self) {
    self.global.stop();
  }
}

/// Connect to the cluster, or say why not and return `None`.
fn cluster() -> Option<Cluster> {
  let text = match std::env::var("IC_TEST_CONNECTSTRING") {
    Ok(text) => text,
    Err(_) => {
      eprintln!("skipped: IC_TEST_CONNECTSTRING is not set");
      return None;
    }
  };
  let mut last: Option<IcError> = None;
  let mut tries: u32 = 0;
  while tries < IC_IT_START_TRIES {
    tries += 1;
    let connect_string = ic_util::connectstring::parse(&text)
      .expect("IC_TEST_CONNECTSTRING is not a connectstring");
    let fetched =
      mgm_client::fetch_configuration(&connect_string, 30_000, Some("ic_it"));
    let (config, mgm) = match fetched {
      Ok(pair) => pair,
      Err(e) => {
        last = Some(e);
        std::thread::sleep(std::time::Duration::from_secs(1));
        continue;
      }
    };
    let started =
      ApidGlobal::start(config, mgm, connect_string, 30_000, Some("ic_it"));
    let mut global = match started {
      Ok(global) => global,
      Err(e) => {
        last = Some(e);
        std::thread::sleep(std::time::Duration::from_secs(1));
        continue;
      }
    };
    if global.wait_for_all_started(IC_IT_START_WAIT_MS) == 0 {
      global.stop();
      panic!("no data node is started");
    }
    return Some(Cluster { global });
  }
  match last {
    Some(e) => panic!("could not connect: {} ({})", e.message(), e.code),
    None => panic!("could not connect"),
  }
}

/// Run a shell command with `input` on its standard input and return
/// what it printed, failing the test if it failed.
fn run_shell(command: &str, input: &str) -> String {
  let mut child = Command::new("sh")
    .arg("-c")
    .arg(command)
    .stdin(Stdio::piped())
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .expect("sh");
  child
    .stdin
    .take()
    .expect("stdin")
    .write_all(input.as_bytes())
    .expect("write to the command");
  let out = child.wait_with_output().expect("wait for the command");
  // The management client says what went wrong on standard output.
  assert!(
    out.status.success(),
    "{} failed: {} {}",
    command,
    String::from_utf8_lossy(&out.stdout),
    String::from_utf8_lossy(&out.stderr)
  );
  String::from_utf8_lossy(&out.stdout).trim_end().to_string()
}

/// Run SQL through the MySQL client, returning its rows as text, tab
/// separated and one per line; or `None` if there is no client.
fn mysql(sql: &str) -> Option<String> {
  let command = match std::env::var("IC_TEST_MYSQL") {
    Ok(command) => command,
    Err(_) => {
      eprintln!("skipped: IC_TEST_MYSQL is not set");
      return None;
    }
  };
  let command = format!("{} --batch --skip-column-names --raw", command);
  Some(run_shell(&command, sql))
}

/// Run a management client command, or `None` if there is no client.
fn ndb_mgm(command: &str) -> Option<String> {
  let prefix = match std::env::var("IC_TEST_NDB_MGM") {
    Ok(prefix) => prefix,
    Err(_) => {
      eprintln!("skipped: IC_TEST_NDB_MGM is not set");
      return None;
    }
  };
  Some(run_shell(&format!("{} -e '{}'", prefix, command), ""))
}

/// Make the test tables afresh, or `None` if there is no MySQL client.
fn setup_tables() -> Option<()> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.pk;
     CREATE TABLE {db}.pk (
       id INT NOT NULL PRIMARY KEY,
       val INT,
       name VARCHAR(32)
     ) ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.uk;
     CREATE TABLE {db}.uk (
       id INT NOT NULL PRIMARY KEY,
       code INT NOT NULL,
       val INT,
       UNIQUE KEY uk_code (code)
     ) ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.types;
     CREATE TABLE {db}.types (
       id INT NOT NULL PRIMARY KEY,
       t TINYINT, tu TINYINT UNSIGNED,
       s SMALLINT, su SMALLINT UNSIGNED,
       m MEDIUMINT, mu MEDIUMINT UNSIGNED,
       i INT, iu INT UNSIGNED,
       b BIGINT, bu BIGINT UNSIGNED,
       c CHAR(8), vc VARCHAR(20),
       bi BINARY(4), vb VARBINARY(8),
       lvc VARCHAR(300), lvb VARBINARY(300),
       f FLOAT, d DOUBLE
     ) ENGINE=NDB;",
    db = IC_IT_DB
  );
  mysql(&sql)?;
  Some(())
}

// ---- Tables and rows ----

/// A bound table with its default record.
struct Table {
  def: Arc<TableDef>,
  rec: Record,
}

fn bind(conn: &mut ApidConnection, table: &str) -> Table {
  let def = conn.table_bind(IC_IT_DB, table).expect("bind the table");
  let rec = Record::default_for(&def, None).expect("make the record");
  Table { def, rec }
}

impl Table {
  fn row(&self) -> Vec<u8> {
    vec![0; self.rec.row_size() as usize]
  }

  fn position(&self, column: &str) -> u32 {
    let id = self.def.field_id(column).expect("no such column");
    self.rec.position_of(id).expect("column not in the record")
  }

  fn put(&self, row: &mut [u8], column: &str, text: &str) {
    text_row::put_value(&self.rec, self.position(column), text, row)
      .expect("put a value");
  }

  fn get(&self, row: &[u8], column: &str) -> String {
    text_row::value_text(&self.rec, self.position(column), row)
  }
}

/// Write a row of the `pk` table with one operation, which is its own
/// transaction.
fn write_pk(
  conn: &mut ApidConnection,
  t: &Table,
  operation: u32,
  id: i64,
  val: &str,
  name: &str,
) -> Result<(), IcError> {
  let mut row = t.row();
  t.put(&mut row, "id", &id.to_string());
  t.put(&mut row, "val", val);
  t.put(&mut row, "name", name);
  key_op::write_key(conn, operation, &t.rec, &row, &t.rec, &row)
}

/// Read a row of a table by its integer key column `id`, if it is
/// there.
fn read_by_id(
  conn: &mut ApidConnection,
  t: &Table,
  id: i64,
) -> Option<Vec<u8>> {
  let mut key = t.row();
  t.put(&mut key, "id", &id.to_string());
  let mut out = t.row();
  match key_op::read_committed(conn, &t.rec, &key, &t.rec, &mut out) {
    Ok(true) => Some(out),
    Ok(false) => None,
    Err(e) => panic!("read failed: {} ({})", e.message(), e.code),
  }
}

/// Send and poll until the transaction is done, and give back how it
/// ended. A send that fails because the link went is not the end of
/// the transaction: the poll reports that.
fn finish(conn: &mut ApidConnection, tid: TransId) -> CommitState {
  let start = time::gethrtime();
  loop {
    let _ = conn.flush(100, true);
    let done = match conn.transaction(tid) {
      Some(trans) => {
        if trans.is_done() {
          Some(trans.commit_state())
        } else {
          None
        }
      }
      None => panic!("no such transaction"),
    };
    if let Some(state) = done {
      while conn.get_next_executed_query().is_some() {}
      return state;
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    if waited >= IC_IT_TRANS_WAIT_MS {
      panic!(
        "the transaction did not finish: {:?}, started nodes {:?}",
        conn.transaction(tid),
        conn.started_nodes()
      );
    }
  }
}

/// Send and poll until the query has completed, leaving its transaction
/// open.
fn complete(conn: &mut ApidConnection, qid: QueryId) {
  let start = time::gethrtime();
  loop {
    let _ = conn.flush(100, true);
    let mut seen = false;
    while let Some(id) = conn.get_next_executed_query() {
      if id == qid {
        seen = true;
      }
    }
    if seen {
      return;
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_TRANS_WAIT_MS, "the query did not complete");
  }
}

/// A transaction hinted to the row the key row names.
fn start_for(conn: &mut ApidConnection, t: &Table, key_row: &[u8]) -> TransId {
  let hint = conn.hint_for_key(&t.def, &t.rec, key_row);
  conn
    .start_transaction(hint, Some(&t.def))
    .expect("start a transaction")
}

/// Bind the unique index the `uk` table's `UNIQUE KEY uk_code` made.
/// A MySQL server names the unique hash index `uk_code$unique`, next
/// to an ordered index `uk_code`; the plain name is tried second, for
/// a table made another way.
fn bind_uk_index(conn: &mut ApidConnection) -> Arc<IndexDef> {
  let first = match conn.index_bind(IC_IT_DB, "uk_code$unique", "uk") {
    Ok(index) => return index,
    Err(e) => e,
  };
  match conn.index_bind(IC_IT_DB, "uk_code", "uk") {
    Ok(index) => index,
    Err(e) => panic!(
      "no unique index: {} ({}), then {} ({})",
      first.message(),
      first.code,
      e.message(),
      e.code
    ),
  }
}

/// A record over the columns named, for a key or an attribute row
/// narrower than the table.
fn record_over(t: &Table, columns: &[&str]) -> Record {
  let mut ids: Vec<u32> = Vec::new();
  for column in columns {
    ids.push(t.def.field_id(column).expect("no such column"));
  }
  Record::default_for(&t.def, Some(ids.as_slice())).expect("make the record")
}

/// Put a value into a row laid out by a record other than the table's
/// default one.
fn put_into(t: &Table, rec: &Record, row: &mut [u8], column: &str, text: &str) {
  let id = t.def.field_id(column).expect("no such column");
  let position = rec.position_of(id).expect("column not in the record");
  text_row::put_value(rec, position, text, row).expect("put a value");
}

// ---- connect ----

#[test]
fn connect_registers_with_every_started_node() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  assert_ne!(cluster.global.own_node_id(), 0, "we got a node id");
  let started = cluster.global.started_nodes();
  assert!(!started.is_empty(), "a data node is started");
  assert!(
    cluster.global.num_connected() as usize >= started.len(),
    "every started node has a link"
  );
  let conn = cluster.global.create_connection().expect("a connection");
  assert!(
    conn.block_number() >= 0x8000,
    "a user thread's block number"
  );
  assert_eq!(conn.started_nodes(), started);
}

// ---- dict ----

#[test]
fn dict_describes_columns_and_the_key() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");
  let id_field = t
    .def
    .field(t.def.field_id("id").expect("the id column"))
    .expect("the column");
  assert!(id_field.primary_key);
  assert!(!id_field.nullable);
  let val = t
    .def
    .field(t.def.field_id("val").expect("the val column"))
    .expect("the column");
  assert!(!val.primary_key);
  assert!(val.nullable);
  let name = t
    .def
    .field(t.def.field_id("name").expect("the name column"))
    .expect("the column");
  assert!(name.nullable);
  assert!(t.def.field_id("nowhere").is_err());
  assert!(t.rec.covers_primary_key());
  assert!(t.def.is_valid());
}

#[test]
fn dict_index_lists_its_columns_in_key_order() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let index = bind_uk_index(&mut conn);
  assert!(index.is_unique());
  let columns = &index.info().attributes;
  assert!(!columns.is_empty());
  assert_eq!(columns[0].name, "code", "the indexed column comes first");
  let t = bind(&mut conn, "uk");
  assert_eq!(index.table_id(), t.def.table_id());
  assert_eq!(index.table_version(), t.def.table_version());
}

#[test]
fn dict_lets_go_of_a_table_that_changed() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let before = bind(&mut conn, "pk");
  let version = before.def.table_version();
  mysql(&format!("ALTER TABLE {}.pk ADD COLUMN extra INT", IC_IT_DB));
  let start = time::gethrtime();
  while before.def.is_valid() {
    conn.poll(100);
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_ALTER_WAIT_MS, "the change was not noticed");
  }
  conn.table_unbind(&before.def);
  let after = bind(&mut conn, "pk");
  assert_ne!(after.def.table_version(), version, "a new version");
  assert!(after.def.field_id("extra").is_ok(), "with the new column");
  assert!(after.def.is_valid());
}

// ---- pk ----

#[test]
fn pk_insert_read_update_delete() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");

  write_pk(&mut conn, &t, tc_key::IC_OP_INSERT, 1, "10", "one")
    .expect("insert");
  let row = read_by_id(&mut conn, &t, 1).expect("the row is there");
  assert_eq!(t.get(&row, "id"), "1");
  assert_eq!(t.get(&row, "val"), "10");
  assert_eq!(t.get(&row, "name"), "'one'");
  let seen = mysql(&format!(
    "SELECT val, name FROM {}.pk WHERE id = 1",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("10\tone".to_string()));

  write_pk(&mut conn, &t, tc_key::IC_OP_UPDATE, 1, "11", "one")
    .expect("update");
  let row = read_by_id(&mut conn, &t, 1).expect("still there");
  assert_eq!(t.get(&row, "val"), "11");
  let seen = mysql(&format!("SELECT val FROM {}.pk WHERE id = 1", IC_IT_DB));
  assert_eq!(seen, Some("11".to_string()));

  write_pk(&mut conn, &t, tc_key::IC_OP_WRITE, 1, "12", "uno")
    .expect("write over");
  write_pk(&mut conn, &t, tc_key::IC_OP_WRITE, 2, "20", "two")
    .expect("write new");
  let seen = mysql(&format!(
    "SELECT id, val, name FROM {}.pk ORDER BY id",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("1\t12\tuno\n2\t20\ttwo".to_string()));

  write_pk(&mut conn, &t, tc_key::IC_OP_DELETE, 1, "0", "").expect("delete");
  assert!(read_by_id(&mut conn, &t, 1).is_none(), "gone");
  let seen = mysql(&format!("SELECT COUNT(*) FROM {}.pk", IC_IT_DB));
  assert_eq!(seen, Some("1".to_string()));

  let mut row = t.row();
  t.put(&mut row, "id", "3");
  t.put(&mut row, "val", text_row::IC_NULL_TEXT);
  t.put(&mut row, "name", text_row::IC_NULL_TEXT);
  key_op::write_key(
    &mut conn,
    tc_key::IC_OP_INSERT,
    &t.rec,
    &row,
    &t.rec,
    &row,
  )
  .expect("insert nulls");
  let row = read_by_id(&mut conn, &t, 3).expect("the row is there");
  assert_eq!(t.get(&row, "val"), text_row::IC_NULL_TEXT);
  assert_eq!(t.get(&row, "name"), text_row::IC_NULL_TEXT);
  let seen = mysql(&format!(
    "SELECT val IS NULL, name IS NULL FROM {}.pk WHERE id = 3",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("1\t1".to_string()));
}

#[test]
fn pk_errors_for_a_missing_and_a_present_row() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");

  assert!(read_by_id(&mut conn, &t, 999).is_none(), "nothing there");
  let e = write_pk(&mut conn, &t, tc_key::IC_OP_UPDATE, 999, "1", "x")
    .expect_err("an update of nothing fails");
  assert_eq!(e.code as u32, key_op::IC_NDB_ERROR_NO_SUCH_ROW);
  assert!(e.is_ndb_error());
  let e = write_pk(&mut conn, &t, tc_key::IC_OP_DELETE, 999, "1", "x")
    .expect_err("a delete of nothing fails");
  assert_eq!(e.code as u32, key_op::IC_NDB_ERROR_NO_SUCH_ROW);

  write_pk(&mut conn, &t, tc_key::IC_OP_INSERT, 5, "50", "five")
    .expect("first insert");
  let e = write_pk(&mut conn, &t, tc_key::IC_OP_INSERT, 5, "51", "five")
    .expect_err("a second insert fails");
  assert_eq!(e.code as u32, key_op::IC_NDB_ERROR_ROW_EXISTS);
  assert!(e.is_ndb_error());
  let row = read_by_id(&mut conn, &t, 5).expect("the first one stands");
  assert_eq!(t.get(&row, "val"), "50");
}

#[test]
fn pk_commit_and_rollback_are_what_mysql_sees() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");

  // An insert, committed.
  let mut row = t.row();
  t.put(&mut row, "id", "7");
  t.put(&mut row, "val", "70");
  t.put(&mut row, "name", "seven");
  let tid = start_for(&mut conn, &t, &row);
  conn
    .query_mut(qid)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&row);
  conn
    .query_mut(qid)
    .expect("the query")
    .attr_row_mut()
    .copy_from_slice(&row);
  let args = WriteKeyArgs {
    kind: WriteKind::Insert,
    ..WriteKeyArgs::default()
  };
  conn.write_key(qid, tid, &args).expect("define the insert");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  assert!(conn
    .transaction(tid)
    .expect("the transaction")
    .error()
    .is_none());
  conn.close_transaction(tid).expect("close");
  let seen = mysql(&format!("SELECT val FROM {}.pk WHERE id = 7", IC_IT_DB));
  assert_eq!(seen, Some("70".to_string()));

  // An update, sent and then rolled back: MySQL never sees it.
  t.put(&mut row, "val", "71");
  let tid = start_for(&mut conn, &t, &row);
  conn
    .query_mut(qid)
    .expect("the query")
    .attr_row_mut()
    .copy_from_slice(&row);
  let args = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(qid, tid, &args).expect("define the update");
  complete(&mut conn, qid);
  assert!(
    conn.query(qid).expect("the query").error().is_none(),
    "the update went"
  );
  assert!(
    !conn.transaction(tid).expect("the transaction").is_done(),
    "and is still open"
  );
  conn.rollback_transaction(tid).expect("ask to roll back");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  conn.close_transaction(tid).expect("close");
  let seen = mysql(&format!("SELECT val FROM {}.pk WHERE id = 7", IC_IT_DB));
  assert_eq!(seen, Some("70".to_string()), "the rollback held");

  // A read in a transaction sees the committed value.
  let tid = start_for(&mut conn, &t, &row);
  let args = ReadKeyArgs {
    kind: ReadKind::Locked,
    ..ReadKeyArgs::default()
  };
  conn.read_key(qid, tid, &args).expect("define the read");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  let query = conn.query(qid).expect("the query");
  assert!(query.error().is_none());
  assert_eq!(query.state(), QueryState::Idle);
  assert_eq!(t.get(query.attr_row(), "val"), "70");
  conn.close_transaction(tid).expect("close");
}

#[test]
fn pk_a_rollback_before_the_send_sends_nothing() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");
  let first = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let second = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let insert = WriteKeyArgs {
    kind: WriteKind::Insert,
    ..WriteKeyArgs::default()
  };

  // An insert defined, its request packed, and the transaction rolled
  // back before anything is sent.
  let mut row = t.row();
  t.put(&mut row, "id", "11");
  t.put(&mut row, "val", "110");
  t.put(&mut row, "name", "eleven");
  let dropped = start_for(&mut conn, &t, &row);
  let query = conn.query_mut(first).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  conn.write_key(first, dropped, &insert).expect("define");
  conn
    .rollback_transaction(dropped)
    .expect("ask to roll back");

  // Another insert, defined after it on the same connection, packed
  // behind it and committed in the same send.
  t.put(&mut row, "id", "12");
  t.put(&mut row, "val", "120");
  t.put(&mut row, "name", "twelve");
  let kept = start_for(&mut conn, &t, &row);
  let query = conn.query_mut(second).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  conn.write_key(second, kept, &insert).expect("define");
  conn.commit_transaction(kept).expect("ask to commit");

  assert_eq!(finish(&mut conn, dropped), CommitState::RolledBack);
  assert_eq!(finish(&mut conn, kept), CommitState::Committed);
  conn.close_transaction(dropped).expect("close");
  conn.close_transaction(kept).expect("close");
  assert!(read_by_id(&mut conn, &t, 11).is_none(), "never sent");
  let row = read_by_id(&mut conn, &t, 12).expect("sent whole");
  assert_eq!(t.get(&row, "val"), "120");
  let seen = mysql(&format!("SELECT id, val FROM {}.pk ORDER BY id", IC_IT_DB));
  assert_eq!(seen, Some("12\t120".to_string()));
}

// ---- uk ----

#[test]
fn uk_read_update_delete_through_the_index() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "uk");
  let index = bind_uk_index(&mut conn);
  mysql(&format!(
    "INSERT INTO {db}.uk VALUES (1, 100, 1), (2, 200, 2)",
    db = IC_IT_DB
  ));

  // The key of a query through the index is the indexed column; the
  // values are the rest, so that an update leaves the index alone.
  let key_rec = record_over(&t, &["code"]);
  let val_rec = record_over(&t, &["id", "val"]);

  // A read through the index finds the row by its code.
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(&t, &key_rec, &mut key, "code", "200");
  let mut out = t.row();
  let found = key_op::read_unique_committed(
    &mut conn, &index, &key_rec, &key, &t.rec, &mut out,
  )
  .expect("read through the index");
  assert!(found);
  assert_eq!(t.get(&out, "id"), "2");
  assert_eq!(t.get(&out, "val"), "2");
  put_into(&t, &key_rec, &mut key, "code", "300");
  let found = key_op::read_unique_committed(
    &mut conn, &index, &key_rec, &key, &t.rec, &mut out,
  )
  .expect("read of nothing through the index");
  assert!(!found);

  // An update through the index, in a transaction.
  let qid = conn
    .create_unique_query(&index, &key_rec, &val_rec)
    .expect("a unique query");
  let mut vals = vec![0; val_rec.row_size() as usize];
  put_into(&t, &val_rec, &mut vals, "id", "1");
  put_into(&t, &val_rec, &mut vals, "val", "5");
  put_into(&t, &key_rec, &mut key, "code", "100");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  conn
    .query_mut(qid)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&key);
  conn
    .query_mut(qid)
    .expect("the query")
    .attr_row_mut()
    .copy_from_slice(&vals);
  let args = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(qid, tid, &args).expect("define the update");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  let seen = mysql(&format!("SELECT val FROM {}.uk WHERE id = 1", IC_IT_DB));
  assert_eq!(seen, Some("5".to_string()));

  // A delete through the index.
  put_into(&t, &key_rec, &mut key, "code", "200");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  conn
    .query_mut(qid)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&key);
  let args = WriteKeyArgs {
    kind: WriteKind::Delete,
    ..WriteKeyArgs::default()
  };
  conn.write_key(qid, tid, &args).expect("define the delete");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  assert!(read_by_id(&mut conn, &t, 2).is_none(), "row 2 is gone");
  let seen = mysql(&format!(
    "SELECT id, code, val FROM {}.uk ORDER BY id",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("1\t100\t5".to_string()));
}

// ---- types ----

#[test]
fn types_round_trip_through_the_api_and_mysql() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "types");
  let long_text = "L".repeat(250);
  let long_bytes = "B".repeat(250);
  let mut long_hex = String::from("0x");
  for _ in 0..250 {
    long_hex.push_str("42");
  }

  // The integers at their edges, and the strings, written through the
  // API and read back both ways.
  let values: [(&str, &str); 16] = [
    ("t", "-128"),
    ("tu", "255"),
    ("s", "-32768"),
    ("su", "65535"),
    ("m", "-8388608"),
    ("mu", "16777215"),
    ("i", "-2147483648"),
    ("iu", "4294967295"),
    ("b", "-9223372036854775808"),
    ("bu", "18446744073709551615"),
    ("c", "abc"),
    ("vc", "hello world"),
    ("bi", "abcd"),
    ("vb", "xyz"),
    ("lvc", long_text.as_str()),
    ("lvb", long_bytes.as_str()),
  ];
  let mut row = t.row();
  t.put(&mut row, "id", "1");
  for (column, text) in &values {
    t.put(&mut row, column, text);
  }
  t.put(&mut row, "f", text_row::IC_NULL_TEXT);
  t.put(&mut row, "d", text_row::IC_NULL_TEXT);
  key_op::write_key(
    &mut conn,
    tc_key::IC_OP_INSERT,
    &t.rec,
    &row,
    &t.rec,
    &row,
  )
  .expect("insert");
  let back = read_by_id(&mut conn, &t, 1).expect("the row is there");
  let mut i = 0;
  while i < 10 {
    assert_eq!(t.get(&back, values[i].0), values[i].1, "{}", values[i].0);
    i += 1;
  }
  assert_eq!(t.get(&back, "c"), "'abc'");
  assert_eq!(t.get(&back, "vc"), "'hello world'");
  assert_eq!(t.get(&back, "bi"), "0x61626364");
  assert_eq!(t.get(&back, "vb"), "0x78797a");
  assert_eq!(t.get(&back, "lvc"), format!("'{}'", long_text));
  assert_eq!(t.get(&back, "lvb"), long_hex);
  assert_eq!(t.get(&back, "f"), text_row::IC_NULL_TEXT);
  let seen = mysql(&format!(
    "SELECT t, tu, s, su, m, mu, i, iu, b, bu, c, vc, bi, vb, \
     LENGTH(lvc), LENGTH(lvb) FROM {}.types WHERE id = 1",
    IC_IT_DB
  ));
  let mut expected = String::new();
  i = 0;
  while i < 14 {
    if i > 0 {
      expected.push('\t');
    }
    expected.push_str(values[i].1);
    i += 1;
  }
  expected.push_str("\t250\t250");
  assert_eq!(seen, Some(expected));

  // What MySQL writes, the API reads: the floating point columns, and
  // a row of NULLs.
  mysql(&format!(
    "INSERT INTO {db}.types (id, f, d) VALUES (2, 1.5, -2.25);
     INSERT INTO {db}.types (id) VALUES (3)",
    db = IC_IT_DB
  ));
  let back = read_by_id(&mut conn, &t, 2).expect("row 2 is there");
  let f: f64 = t.get(&back, "f").parse().expect("a float");
  assert!((f - 1.5).abs() < 1e-6);
  let d: f64 = t.get(&back, "d").parse().expect("a double");
  assert!((d + 2.25).abs() < 1e-12);
  assert_eq!(t.get(&back, "t"), text_row::IC_NULL_TEXT);
  let back = read_by_id(&mut conn, &t, 3).expect("row 3 is there");
  for (column, _) in &values {
    assert_eq!(t.get(&back, column), text_row::IC_NULL_TEXT, "{}", column);
  }
  assert_eq!(t.get(&back, "d"), text_row::IC_NULL_TEXT);
}

// ---- failure ----

#[test]
fn failure_a_stopped_coordinator_fails_the_transaction_and_a_retry_commits() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  if std::env::var("IC_TEST_NDB_MGM").is_err() {
    eprintln!("skipped: IC_TEST_NDB_MGM is not set");
    return;
  }
  let started = cluster.global.started_nodes();
  if started.len() < 2 {
    eprintln!("skipped: the failure group needs two started data nodes");
    return;
  }
  let victim = started[1];
  let survivor = started[0];
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");
  write_pk(&mut conn, &t, tc_key::IC_OP_INSERT, 50, "1", "fifty")
    .expect("the row to update");

  // An update on a transaction coordinated by the victim, sent and
  // confirmed but not committed, when the victim stops.
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let mut row = t.row();
  t.put(&mut row, "id", "50");
  t.put(&mut row, "val", "2");
  t.put(&mut row, "name", "fifty");
  let tid = conn
    .start_transaction(TransactionHint::Node(victim), Some(&t.def))
    .expect("a transaction at the victim");
  conn
    .query_mut(qid)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&row);
  conn
    .query_mut(qid)
    .expect("the query")
    .attr_row_mut()
    .copy_from_slice(&row);
  let args = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(qid, tid, &args).expect("define the update");
  complete(&mut conn, qid);
  assert!(conn.query(qid).expect("the query").error().is_none());

  // A restart takes the node down and brings it back by itself; the
  // management client returns once the node has shut down. A stop
  // would take the whole process down, with nothing left to start.
  eprintln!("restarting data node {}", victim);
  ndb_mgm(&format!("{} RESTART", victim));
  conn.commit_transaction(tid).expect("ask to commit");
  let start = time::gethrtime();
  let state = loop {
    let _ = conn.flush(100, true);
    let done = match conn.transaction(tid) {
      Some(trans) => {
        if trans.is_done() {
          Some(trans.commit_state())
        } else {
          None
        }
      }
      None => panic!("no such transaction"),
    };
    if let Some(state) = done {
      break state;
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < 60_000, "the transaction never settled");
  };
  let error = conn.transaction(tid).expect("the transaction").error();
  eprintln!("the transaction ended {:?} with {:?}", state, error);
  assert_ne!(state, CommitState::Committed, "it could not commit");
  let e = error.expect("with an error");
  assert!(
    e.is_temporary(),
    "a temporary one: {} ({})",
    e.message(),
    e.code
  );
  while conn.get_next_executed_query().is_some() {}
  conn.close_transaction(tid).expect("close");
  let row = read_by_id(&mut conn, &t, 50).expect("the row is there");
  assert_eq!(t.get(&row, "val"), "1", "and unchanged");

  // The retry, at the survivor.
  let tid = conn
    .start_transaction(TransactionHint::Node(survivor), Some(&t.def))
    .expect("a transaction at the survivor");
  conn
    .query_mut(qid)
    .expect("the query")
    .attr_row_mut()
    .copy_from_slice(&row_with_val(&t, &row, "2"));
  conn
    .write_key(qid, tid, &args)
    .expect("define the update again");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  let row = read_by_id(&mut conn, &t, 50).expect("the row is there");
  assert_eq!(t.get(&row, "val"), "2", "the retry took");

  // Start the victim again and wait for it, so the cluster is left as
  // it was found.
  // Wait for the victim to be started again, so that the cluster is
  // left as it was found and the next test finds it whole.
  eprintln!("waiting for data node {} to start again", victim);
  let start = time::gethrtime();
  while cluster.global.started_nodes().len() < started.len() {
    std::thread::sleep(std::time::Duration::from_millis(500));
    let waited = time::millis_elapsed(start, time::gethrtime());
    if waited >= IC_IT_RESTART_WAIT_MS {
      let show = ndb_mgm("show").unwrap_or_default();
      panic!("data node {} did not come back:\n{}", victim, show);
    }
  }
  eprintln!("data node {} is started again", victim);
}

/// A copy of `row` with `val` changed.
fn row_with_val(t: &Table, row: &[u8], val: &str) -> Vec<u8> {
  let mut copy = row.to_vec();
  t.put(&mut copy, "val", val);
  copy
}
