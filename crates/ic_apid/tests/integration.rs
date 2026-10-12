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
//! - `IC_TEST_NDB_MGM`: the command that runs the management client,
//!   with its connect string (`-c localhost:1186`), for the `failure`
//!   group, which stops a data node and starts it again. Set, it also
//!   lets the `timeout` group stop a data node's process for a few
//!   seconds with SIGSTOP, which needs `lsof` and the node's fixed port.
//! - `IC_TEST_RECEIVE_THREADS`: how many receive threads to start, one
//!   if not set, so that the suite can run with the nodes' links shared
//!   out over several.
//!
//! The groups are the phase-5 ones: `connect`, `dict`, `pk`, `uk`,
//! `types`, `failure`; `blob_guard`, which checks that rows of a table
//! with a blob column are not deleted and that writes leave the blobs
//! alone (chapter 06 §6.2); `timeout`, which checks that a transaction
//! whose node stops answering ends with 4012 (chapter 06 §6.3);
//! `charset`, which checks that keys with character columns are placed
//! where the data nodes place them, in every collation claimed
//! (chapter 06 §8.6); and `collation`, which asks the data nodes what a
//! collation is and for its weights (RonDB 26.04 main and later).
//! Each test is its own program against the cluster, as an application
//! would be: it takes an API node id, binds its tables, does its work
//! through the public API and lets go.

#![cfg(feature = "integration")]
#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::io::Write;
use std::process::Command;
use std::process::Stdio;
use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::apid_global::GlobalOptions;
use ic_apid::collation_client;
use ic_apid::collation_client::Collation;
use ic_apid::dict_cache::IndexDef;
use ic_apid::dict_cache::TableDef;
use ic_apid::key_op;
use ic_apid::query::AbortOption;
use ic_apid::query::QueryId;
use ic_apid::query::QueryState;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::TransId;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::record::Record;
use ic_apid::row_codec::IC_NDB_ERROR_NULL_KEY;
use ic_apid::text_row;
use ic_apid::transaction::CommitState;
use ic_apid::transaction::TransactionHint;
use ic_apid::transaction::IC_NDB_ERROR_NO_REPLY;
use ic_port::err;
use ic_port::time;
use ic_port::IcError;
use ic_signals::collation_desc;
use ic_signals::key_ops;

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
/// How long the `timeout` group lets a transaction wait for its
/// replies.
const IC_IT_REPLY_WAIT_MS: u32 = 1_000;

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
  cluster_with_reply_wait(0)
}

/// As [`cluster`], with transactions that give up waiting for their
/// replies after `reply_wait_ms`, or when the configuration says if 0.
fn cluster_with_reply_wait(reply_wait_ms: u32) -> Option<Cluster> {
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
    let mut options = GlobalOptions {
      reply_wait_ms,
      ..GlobalOptions::default()
    };
    if let Ok(text) = std::env::var("IC_TEST_RECEIVE_THREADS") {
      options.receive_threads = text.parse().unwrap_or(1);
    }
    let started = ApidGlobal::start_with_options(
      config,
      mgm,
      connect_string,
      30_000,
      Some("ic_it"),
      &options,
    );
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
fn mgm_client(command: &str) -> Option<String> {
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
fn bind_uk_index(conn: &mut ApidConnection) -> Arc<IndexDef> {
  bind_unique_index(conn, "uk_code", "uk")
}

/// Bind the unique index a `UNIQUE KEY name` made on `table`. A MySQL
/// server names the unique hash index `name$unique`, next to an
/// ordered index `name`; the plain name is tried second, for a table
/// made another way.
fn bind_unique_index(
  conn: &mut ApidConnection,
  name: &str,
  table: &str,
) -> Arc<IndexDef> {
  let hash_name = format!("{}$unique", name);
  let first = match conn.index_bind(IC_IT_DB, &hash_name, table) {
    Ok(index) => return index,
    Err(e) => e,
  };
  match conn.index_bind(IC_IT_DB, name, table) {
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

  write_pk(&mut conn, &t, key_ops::IC_OP_INSERT, 1, "10", "one")
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

  write_pk(&mut conn, &t, key_ops::IC_OP_UPDATE, 1, "11", "one")
    .expect("update");
  let row = read_by_id(&mut conn, &t, 1).expect("still there");
  assert_eq!(t.get(&row, "val"), "11");
  let seen = mysql(&format!("SELECT val FROM {}.pk WHERE id = 1", IC_IT_DB));
  assert_eq!(seen, Some("11".to_string()));

  write_pk(&mut conn, &t, key_ops::IC_OP_WRITE, 1, "12", "uno")
    .expect("write over");
  write_pk(&mut conn, &t, key_ops::IC_OP_WRITE, 2, "20", "two")
    .expect("write new");
  let seen = mysql(&format!(
    "SELECT id, val, name FROM {}.pk ORDER BY id",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("1\t12\tuno\n2\t20\ttwo".to_string()));

  write_pk(&mut conn, &t, key_ops::IC_OP_DELETE, 1, "0", "").expect("delete");
  assert!(read_by_id(&mut conn, &t, 1).is_none(), "gone");
  let seen = mysql(&format!("SELECT COUNT(*) FROM {}.pk", IC_IT_DB));
  assert_eq!(seen, Some("1".to_string()));

  let mut row = t.row();
  t.put(&mut row, "id", "3");
  t.put(&mut row, "val", text_row::IC_NULL_TEXT);
  t.put(&mut row, "name", text_row::IC_NULL_TEXT);
  key_op::write_key(
    &mut conn,
    key_ops::IC_OP_INSERT,
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
  let e = write_pk(&mut conn, &t, key_ops::IC_OP_UPDATE, 999, "1", "x")
    .expect_err("an update of nothing fails");
  assert_eq!(e.code as u32, key_op::IC_NDB_ERROR_NO_SUCH_ROW);
  assert!(e.is_ndb_error());
  let e = write_pk(&mut conn, &t, key_ops::IC_OP_DELETE, 999, "1", "x")
    .expect_err("a delete of nothing fails");
  assert_eq!(e.code as u32, key_op::IC_NDB_ERROR_NO_SUCH_ROW);

  write_pk(&mut conn, &t, key_ops::IC_OP_INSERT, 5, "50", "five")
    .expect("first insert");
  let e = write_pk(&mut conn, &t, key_ops::IC_OP_INSERT, 5, "51", "five")
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

#[test]
fn pk_binary_keys_are_placed_where_the_data_nodes_put_them() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  // A binary column has no character set: a VARBINARY is hashed as its
  // length bytes and its bytes, a BINARY as its whole field padded with
  // zeros (the author, 2026-10-09; chapter 05 §6.3).
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.vb;
     CREATE TABLE {db}.vb (k VARBINARY(20) NOT NULL PRIMARY KEY, val INT)
       ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.lvb;
     CREATE TABLE {db}.lvb (k VARBINARY(300) NOT NULL PRIMARY KEY, val INT)
       ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.fb;
     CREATE TABLE {db}.fb (k BINARY(5) NOT NULL PRIMARY KEY, val INT)
       ENGINE=NDB;",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  // Every length up to the column's, trailing spaces kept as bytes, a
  // key long enough for two length bytes, and short fixed-size ones.
  let mut short: Vec<String> = Vec::new();
  for n in 0..=20 {
    short.push("z".repeat(n));
  }
  short.push("a ".to_string());
  short.push("a  ".to_string());
  let mut long: Vec<String> = Vec::new();
  for n in [250, 255, 256, 257, 299, 300] {
    long.push("y".repeat(n));
  }
  let fixed: Vec<String> = ["a", "ab", "abc", "abcd", "abcde", "a b"]
    .iter()
    .map(|k| k.to_string())
    .collect();

  let mut conn = cluster.global.create_connection().expect("a connection");
  for (table, keys) in [("vb", &short), ("lvb", &long), ("fb", &fixed)] {
    let mut values: Vec<String> = Vec::new();
    for (i, key) in keys.iter().enumerate() {
      values.push(format!("('{}', {})", key, i));
    }
    mysql(&format!(
      "INSERT INTO {}.{} VALUES {}",
      IC_IT_DB,
      table,
      values.join(", ")
    ));
    let t = bind(&mut conn, table);
    for key in keys.iter() {
      let mut row = t.row();
      t.put(&mut row, "k", key);
      let ours = ic_apid::hash::partition_of(&t.def, &t.rec, &row)
        .expect("a binary key is placed");
      let theirs = key_op::read_partition(&mut conn, &t.rec, &row)
        .expect("read the row's partition");
      assert_eq!(
        theirs,
        Some(ours),
        "{}: a key of {} bytes",
        table,
        key.len()
      );
    }
  }
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

/// The `uk2` table, made afresh, with three rows: its unique keys are
/// two columns in an order other than the table's, a nullable column, a
/// CHAR column and a VARCHAR column.
fn setup_uk2() -> Option<()> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.uk2;
     CREATE TABLE {db}.uk2 (
       id INT NOT NULL PRIMARY KEY,
       a INT NOT NULL,
       b INT NOT NULL,
       n INT,
       c CHAR(8) NOT NULL,
       v VARCHAR(20) NOT NULL,
       val INT,
       UNIQUE KEY uk2_ba (b, a),
       UNIQUE KEY uk2_n (n),
       UNIQUE KEY uk2_c (c),
       UNIQUE KEY uk2_v (v)
     ) ENGINE=NDB;
     INSERT INTO {db}.uk2 VALUES
       (1, 10, 100, NULL, 'one', 'uno', 1),
       (2, 20, 200, 0, 'two', 'dos', 2),
       (3, 30, 300, 3, 'three', 'tres', 3);",
    db = IC_IT_DB
  );
  mysql(&sql)?;
  Some(())
}

/// Read through a unique index, as a committed read, the row whose
/// indexed columns have the values given; `None` if there is none.
fn read_through(
  conn: &mut ApidConnection,
  t: &Table,
  index: &Arc<IndexDef>,
  values: &[(&str, &str)],
) -> Result<Option<Vec<u8>>, IcError> {
  let columns: Vec<&str> = values.iter().map(|(column, _)| *column).collect();
  let key_rec = record_over(t, &columns);
  let mut key = vec![0; key_rec.row_size() as usize];
  for (column, value) in values {
    put_into(t, &key_rec, &mut key, column, value);
  }
  let mut out = t.row();
  let found = key_op::read_unique_committed(
    conn, index, &key_rec, &key, &t.rec, &mut out,
  )?;
  if found {
    return Ok(Some(out));
  }
  Ok(None)
}

/// Update a row of the `uk` table by its primary key, sent and not
/// committed, so that its transaction holds the row's exclusive lock.
fn hold_row(
  conn: &mut ApidConnection,
  t: &Table,
  qid: QueryId,
  id: &str,
  code: &str,
  val: &str,
) -> TransId {
  let mut row = t.row();
  t.put(&mut row, "id", id);
  t.put(&mut row, "code", code);
  t.put(&mut row, "val", val);
  let tid = start_for(conn, t, &row);
  let query = conn.query_mut(qid).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn
    .write_key(qid, tid, &update)
    .expect("define the update");
  complete(conn, qid);
  tid
}

#[test]
fn uk_keys_of_several_columns_null_and_characters() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_uk2().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "uk2");

  // Two columns, b then a: the key is taken by name, in the index's
  // order, whatever the record's.
  let ba = bind_unique_index(&mut conn, "uk2_ba", "uk2");
  let row = read_through(&mut conn, &t, &ba, &[("a", "20"), ("b", "200")])
    .expect("read through uk2_ba")
    .expect("row 2");
  assert_eq!(t.get(&row, "id"), "2");
  let swapped = read_through(&mut conn, &t, &ba, &[("a", "200"), ("b", "20")])
    .expect("read through uk2_ba");
  assert!(swapped.is_none(), "swapped values name no row");

  // NULL is refused before anything is sent: a row with NULL there is
  // not in the index, and the bytes under the NULL could name row 2.
  let n = bind_unique_index(&mut conn, "uk2_n", "uk2");
  let key_rec = record_over(&t, &["n"]);
  let mut key = vec![0; key_rec.row_size() as usize];
  key_rec.set_null(&mut key, 0, true).expect("set NULL");
  let mut out = t.row();
  let e = key_op::read_unique_committed(
    &mut conn, &n, &key_rec, &key, &t.rec, &mut out,
  )
  .expect_err("a NULL key is refused");
  assert_eq!(e.code, IC_NDB_ERROR_NULL_KEY);
  let row = read_through(&mut conn, &t, &n, &[("n", "0")])
    .expect("read through uk2_n")
    .expect("row 2");
  assert_eq!(t.get(&row, "id"), "2");

  // CHAR and VARCHAR keys are found, and placed in turn until the
  // collations are known (chapter 06 §8.6).
  let c = bind_unique_index(&mut conn, "uk2_c", "uk2");
  let row = read_through(&mut conn, &t, &c, &[("c", "three")])
    .expect("read through uk2_c")
    .expect("row 3");
  assert_eq!(t.get(&row, "id"), "3");
  let v = bind_unique_index(&mut conn, "uk2_v", "uk2");
  let row = read_through(&mut conn, &t, &v, &[("v", "uno")])
    .expect("read through uk2_v")
    .expect("row 1");
  assert_eq!(t.get(&row, "id"), "1");
  let key_rec = record_over(&t, &["v"]);
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(&t, &key_rec, &mut key, "v", "uno");
  assert_eq!(
    conn.hint_for_unique_key(&v, &key_rec, &key),
    TransactionHint::Any
  );
}

#[test]
fn uk_duplicates_are_refused_with_893_naming_the_index() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() || setup_uk2().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");

  // By primary key: an insert whose code another row has.
  let t = bind(&mut conn, "uk");
  let index = bind_uk_index(&mut conn);
  mysql(&format!("INSERT INTO {}.uk VALUES (1, 100, 1)", IC_IT_DB));
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let mut row = t.row();
  t.put(&mut row, "id", "2");
  t.put(&mut row, "code", "100");
  t.put(&mut row, "val", "2");
  let tid = start_for(&mut conn, &t, &row);
  let query = conn.query_mut(qid).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  let insert = WriteKeyArgs {
    kind: WriteKind::Insert,
    ..WriteKeyArgs::default()
  };
  conn
    .write_key(qid, tid, &insert)
    .expect("define the insert");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  let query = conn.query(qid).expect("the query");
  assert_eq!(query.error().map(|e| e.code), Some(893));
  assert_eq!(query.error_data(), index.index_id(), "names uk_code");
  let trans = conn.transaction(tid).expect("the transaction");
  assert_eq!(trans.error_data(), index.index_id());
  conn.close_transaction(tid).expect("close");

  // Through one index, giving another index's column a taken value.
  let t2 = bind(&mut conn, "uk2");
  let ba = bind_unique_index(&mut conn, "uk2_ba", "uk2");
  let n = bind_unique_index(&mut conn, "uk2_n", "uk2");
  let key_rec = record_over(&t2, &["b", "a"]);
  let val_rec = record_over(&t2, &["n"]);
  let uq = conn
    .create_unique_query(&ba, &key_rec, &val_rec)
    .expect("a unique query");
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(&t2, &key_rec, &mut key, "b", "300");
  put_into(&t2, &key_rec, &mut key, "a", "30");
  let mut vals = vec![0; val_rec.row_size() as usize];
  put_into(&t2, &val_rec, &mut vals, "n", "0");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t2.def))
    .expect("a transaction");
  let query = conn.query_mut(uq).expect("the query");
  query.key_row_mut().copy_from_slice(&key);
  query.attr_row_mut().copy_from_slice(&vals);
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(uq, tid, &update).expect("define the update");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  let query = conn.query(uq).expect("the query");
  assert_eq!(query.error().map(|e| e.code), Some(893));
  assert_eq!(query.error_data(), n.index_id(), "names uk2_n");
  conn.close_transaction(tid).expect("close");
}

#[test]
fn uk_refusals_before_sending() {
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
  let key_rec = record_over(&t, &["code"]);

  // An update through the index cannot change the indexed column.
  let with_code = record_over(&t, &["code", "val"]);
  let uq = conn
    .create_unique_query(&index, &key_rec, &with_code)
    .expect("a unique query");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  for (kind, code) in [
    (WriteKind::Update, err::IC_ERROR_INDEXED_COLUMN_UPDATE),
    (WriteKind::Insert, err::IC_ERROR_NOT_SUPPORTED),
    (WriteKind::Write, err::IC_ERROR_NOT_SUPPORTED),
  ] {
    let args = WriteKeyArgs {
      kind,
      ..WriteKeyArgs::default()
    };
    let e = conn.write_key(uq, tid, &args).expect_err("refused");
    assert_eq!(e.code, code, "{:?}", kind);
  }
  conn.rollback_transaction(tid).expect("ask to roll back");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  conn.close_transaction(tid).expect("close");
}

#[test]
fn uk_index_reads_wait_for_a_conflicting_lock() {
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
  mysql(&format!("INSERT INTO {}.uk VALUES (1, 100, 1)", IC_IT_DB));
  let hq = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let key_rec = record_over(&t, &["code"]);
  let rq = conn
    .create_unique_query(&index, &key_rec, &t.rec)
    .expect("a unique query");
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(&t, &key_rec, &mut key, "code", "100");

  // A locked and an exclusive read wait for the update's lock, and see
  // what it committed.
  for (kind, val) in [(ReadKind::Locked, "2"), (ReadKind::Exclusive, "3")] {
    let holder = hold_row(&mut conn, &t, hq, "1", "100", val);
    let tid = conn
      .start_transaction(TransactionHint::Any, Some(&t.def))
      .expect("a transaction");
    conn
      .query_mut(rq)
      .expect("the query")
      .key_row_mut()
      .copy_from_slice(&key);
    let args = ReadKeyArgs {
      kind,
      ..ReadKeyArgs::default()
    };
    conn.read_key(rq, tid, &args).expect("define the read");
    let start = time::gethrtime();
    while time::millis_elapsed(start, time::gethrtime()) < 500 {
      let _ = conn.flush(50, true);
    }
    let state = conn.query(rq).expect("the query").state();
    assert_eq!(state, QueryState::Sent, "{:?} waits for the lock", kind);
    conn.commit_transaction(holder).expect("commit the holder");
    assert_eq!(finish(&mut conn, holder), CommitState::Committed);
    conn.close_transaction(holder).expect("close");
    conn.commit_transaction(tid).expect("commit the reader");
    assert_eq!(finish(&mut conn, tid), CommitState::Committed);
    let query = conn.query(rq).expect("the query");
    assert!(query.error().is_none(), "{:?}: {:?}", kind, query.error());
    assert_eq!(t.get(query.attr_row(), "val"), val, "{:?}", kind);
    conn.close_transaction(tid).expect("close");
  }

  // A no-wait read is refused at once rather than waiting.
  let holder = hold_row(&mut conn, &t, hq, "1", "100", "4");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  conn
    .query_mut(rq)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&key);
  let args = ReadKeyArgs {
    kind: ReadKind::ExclusiveNoWait,
    ..ReadKeyArgs::default()
  };
  conn.read_key(rq, tid, &args).expect("define the read");
  let start = time::gethrtime();
  complete(&mut conn, rq);
  let waited = time::millis_elapsed(start, time::gethrtime());
  let error = conn.query(rq).expect("the query").error();
  assert_eq!(error.map(|e| e.code), Some(635), "refused as locked");
  assert!(waited < 1_000, "refused after {} ms", waited);
  conn
    .rollback_transaction(tid)
    .expect("roll the reader back");
  let _ = finish(&mut conn, tid);
  conn.close_transaction(tid).expect("close");
  conn
    .rollback_transaction(holder)
    .expect("roll the holder back");
  assert_eq!(finish(&mut conn, holder), CommitState::RolledBack);
  conn.close_transaction(holder).expect("close");
}

#[test]
fn uk_with_pk_in_one_transaction_and_500_in_a_batch() {
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
  let mut values: Vec<String> = Vec::new();
  for i in 1..=500 {
    values.push(format!("({}, {}, {})", i, 1000 + i, i));
  }
  mysql(&format!(
    "INSERT INTO {}.uk VALUES {}",
    IC_IT_DB,
    values.join(", ")
  ));
  let key_rec = record_over(&t, &["code"]);

  // An update by primary key and a read through the index, together.
  let pq = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let mut row = t.row();
  t.put(&mut row, "id", "1");
  t.put(&mut row, "code", "1001");
  t.put(&mut row, "val", "7");
  let tid = start_for(&mut conn, &t, &row);
  let query = conn.query_mut(pq).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(pq, tid, &update).expect("define the update");
  let uq = conn
    .create_unique_query(&index, &key_rec, &t.rec)
    .expect("a unique query");
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(&t, &key_rec, &mut key, "code", "1002");
  conn
    .query_mut(uq)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&key);
  let locked = ReadKeyArgs {
    kind: ReadKind::Locked,
    ..ReadKeyArgs::default()
  };
  conn.read_key(uq, tid, &locked).expect("define the read");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  assert_eq!(t.get(conn.query(uq).expect("q").attr_row(), "id"), "2");
  conn.close_transaction(tid).expect("close");
  let seen = mysql(&format!("SELECT val FROM {}.uk WHERE id = 1", IC_IT_DB));
  assert_eq!(seen, Some("7".to_string()));

  // Five hundred committed reads through the index, sent together.
  let mut queries: Vec<QueryId> = Vec::new();
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let committed = ReadKeyArgs {
    kind: ReadKind::Committed,
    ..ReadKeyArgs::default()
  };
  for i in 1..=500 {
    let q = conn
      .create_unique_query(&index, &key_rec, &t.rec)
      .expect("a unique query");
    put_into(&t, &key_rec, &mut key, "code", &(1000 + i).to_string());
    conn
      .query_mut(q)
      .expect("the query")
      .key_row_mut()
      .copy_from_slice(&key);
    conn.read_key(q, tid, &committed).expect("define the read");
    queries.push(q);
  }
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  for (i, q) in queries.iter().enumerate() {
    let query = conn.query(*q).expect("the query");
    assert!(
      query.error().is_none(),
      "read {}: {:?}",
      i + 1,
      query.error()
    );
    assert_eq!(t.get(query.attr_row(), "id"), (i + 1).to_string());
  }
  conn.close_transaction(tid).expect("close");
  for q in queries {
    conn.free_query(q).expect("free");
  }
}

#[test]
fn uk_a_missing_key_with_abort_on_error_rolls_back_with_626() {
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
  mysql(&format!("INSERT INTO {}.uk VALUES (1, 100, 1)", IC_IT_DB));

  // An update of row 1, then a read through the index of a code no
  // row has, which aborts the whole transaction.
  let pq = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let mut row = t.row();
  t.put(&mut row, "id", "1");
  t.put(&mut row, "code", "100");
  t.put(&mut row, "val", "9");
  let tid = start_for(&mut conn, &t, &row);
  let query = conn.query_mut(pq).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(pq, tid, &update).expect("define the update");
  let key_rec = record_over(&t, &["code"]);
  let uq = conn
    .create_unique_query(&index, &key_rec, &t.rec)
    .expect("a unique query");
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(&t, &key_rec, &mut key, "code", "999");
  conn
    .query_mut(uq)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&key);
  let args = ReadKeyArgs {
    kind: ReadKind::Locked,
    abort_option: AbortOption::AbortOnError,
    ..ReadKeyArgs::default()
  };
  conn.read_key(uq, tid, &args).expect("define the read");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  let query = conn.query(uq).expect("the query");
  assert_eq!(query.error().map(|e| e.code), Some(626));
  conn.close_transaction(tid).expect("close");
  let seen = mysql(&format!("SELECT val FROM {}.uk WHERE id = 1", IC_IT_DB));
  assert_eq!(seen, Some("1".to_string()), "the update went with it");
}

#[test]
fn uk_an_index_made_again_while_bound_is_fetched_again() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "uk");
  let old = bind_uk_index(&mut conn);
  mysql(&format!("INSERT INTO {}.uk VALUES (1, 100, 1)", IC_IT_DB));
  let row = read_through(&mut conn, &t, &old, &[("code", "100")])
    .expect("read through the index")
    .expect("row 1");
  assert_eq!(t.get(&row, "id"), "1");

  mysql(&format!(
    "ALTER TABLE {db}.uk DROP INDEX uk_code;
     ALTER TABLE {db}.uk ADD UNIQUE KEY uk_code (code);",
    db = IC_IT_DB
  ));
  // The index bound before is gone: a read through it, with records
  // over the table it was bound for, is refused by the coordinator, and
  // it is let go of, by the dictionary's notice or by the refusal.
  let e = read_through(&mut conn, &t, &old, &[("code", "100")])
    .expect_err("the old index is refused");
  assert!(e.code == 241 || e.code == 284, "refused with {}", e.code);
  assert!(!old.is_valid(), "the old index is let go of");
  let t = bind(&mut conn, "uk");
  let new = bind_uk_index(&mut conn);
  assert_ne!(
    (new.index_id(), new.index_version()),
    (old.index_id(), old.index_version())
  );
  let row = read_through(&mut conn, &t, &new, &[("code", "100")])
    .expect("read through the new index")
    .expect("row 1");
  assert_eq!(t.get(&row, "id"), "1");
}

#[test]
fn uk_transactions_start_where_the_index_row_is() {
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
  let mut values: Vec<String> = Vec::new();
  for i in 1..=20 {
    values.push(format!("({}, {}, {})", i, 100 + i, i));
  }
  mysql(&format!(
    "INSERT INTO {}.uk VALUES {}",
    IC_IT_DB,
    values.join(", ")
  ));
  let key_rec = record_over(&t, &["code"]);
  // A record over the index's own table, whose key is the code, to
  // read back where the data nodes put each index row.
  let own = index.as_table();
  let code_in_own = own.field_id("code").expect("code in the index");
  let own_rec = Record::default_for(own, Some(&[code_in_own]))
    .expect("a record over the index's table");
  for i in 1..=20 {
    let code = (100 + i).to_string();
    let mut key = vec![0; key_rec.row_size() as usize];
    put_into(&t, &key_rec, &mut key, "code", &code);
    let partition = match conn.hint_for_unique_key(&index, &key_rec, &key) {
      TransactionHint::Partition(partition) => partition,
      other => panic!("code {} not placed: {:?}", code, other),
    };
    let mut own_key = vec![0; own_rec.row_size() as usize];
    text_row::put_value(&own_rec, 0, &code, &mut own_key)
      .expect("put the code");
    let found = key_op::read_partition(&mut conn, &own_rec, &own_key)
      .expect("read the index row's partition");
    assert_eq!(found, Some(partition), "code {}", code);
    let tid = conn
      .start_transaction(TransactionHint::Partition(partition), Some(own))
      .expect("a transaction");
    let node = conn.transaction(tid).expect("the transaction").node_id();
    assert!(
      own
        .info()
        .nodes_of_fragment(partition)
        .contains(&(node as u16)),
      "code {}: node {} does not hold partition {}",
      code,
      node,
      partition
    );
    conn.rollback_transaction(tid).expect("ask to roll back");
    assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
    conn.close_transaction(tid).expect("close");
  }
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
    key_ops::IC_OP_INSERT,
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
  write_pk(&mut conn, &t, key_ops::IC_OP_INSERT, 50, "1", "fifty")
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
  mgm_client(&format!("{} RESTART", victim));
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
      let show = mgm_client("show").unwrap_or_default();
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

// ---- blob_guard ----

/// Make the blob tables afresh, or `None` if there is no MySQL client:
/// `bt` with a TEXT column and a unique key, and one table for each of
/// the other kinds of column a MySQL server keeps as a blob.
fn setup_blob_tables() -> Option<()> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.bt;
     CREATE TABLE {db}.bt (
       id INT NOT NULL PRIMARY KEY,
       code INT NOT NULL,
       val INT,
       txt TEXT,
       UNIQUE KEY bt_code (code)
     ) ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.bb;
     CREATE TABLE {db}.bb (id INT NOT NULL PRIMARY KEY, val INT, b BLOB)
       ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.bj;
     CREATE TABLE {db}.bj (id INT NOT NULL PRIMARY KEY, val INT, j JSON)
       ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.bg;
     CREATE TABLE {db}.bg (id INT NOT NULL PRIMARY KEY, val INT, g GEOMETRY)
       ENGINE=NDB;",
    db = IC_IT_DB
  );
  mysql(&sql)?;
  Some(())
}

/// Write a row of the `bt` table, its TEXT left out, with one
/// operation that is its own transaction.
fn write_bt(
  conn: &mut ApidConnection,
  t: &Table,
  operation: u32,
  id: i64,
  code: i64,
  val: &str,
) -> Result<(), IcError> {
  let mut row = t.row();
  t.put(&mut row, "id", &id.to_string());
  t.put(&mut row, "code", &code.to_string());
  t.put(&mut row, "val", val);
  key_op::write_key(conn, operation, &t.rec, &row, &t.rec, &row)
}

/// The code something was refused with.
fn refused_with(result: Result<(), IcError>) -> i32 {
  match result {
    Ok(()) => panic!("it was not refused"),
    Err(e) => e.code,
  }
}

#[test]
fn blob_guard_knows_every_kind_of_blob_column() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_blob_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  for (table, column) in [("bt", "txt"), ("bb", "b"), ("bj", "j"), ("bg", "g")]
  {
    let t = bind(&mut conn, table);
    assert!(t.def.has_blob_columns(), "{} has a blob column", table);
    let blob = t
      .def
      .field(t.def.field_id(column).expect("the blob column"))
      .expect("the column");
    assert!(blob.is_blob(), "{}.{} is a blob", table, column);
    let val = t
      .def
      .field(t.def.field_id("val").expect("the val column"))
      .expect("the column");
    assert!(!val.is_blob(), "{}.val is not a blob", table);
    assert!(
      t.rec.position_of(blob.column_id).is_none(),
      "the default record of {} leaves the blob out",
      table
    );
  }
}

#[test]
fn blob_guard_refuses_deletes_by_primary_and_unique_key() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_blob_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "bt");
  mysql(&format!(
    "INSERT INTO {db}.bt VALUES (1, 100, 1, 'short'),
       (2, 200, 2, REPEAT('x', 5000))",
    db = IC_IT_DB
  ));

  // One operation, its own transaction.
  let mut key = t.row();
  t.put(&mut key, "id", "1");
  let done = key_op::write_key(
    &mut conn,
    key_ops::IC_OP_DELETE,
    &t.rec,
    &key,
    &t.rec,
    &key,
  );
  assert_eq!(refused_with(done), err::IC_ERROR_DELETE_WITH_BLOBS);

  // By primary key in a transaction: refused when it is defined, and
  // the transaction, with nothing sent, rolls back.
  let delete = WriteKeyArgs {
    kind: WriteKind::Delete,
    ..WriteKeyArgs::default()
  };
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let tid = start_for(&mut conn, &t, &key);
  conn
    .query_mut(qid)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&key);
  assert_eq!(
    refused_with(conn.write_key(qid, tid, &delete)),
    err::IC_ERROR_DELETE_WITH_BLOBS
  );
  conn.rollback_transaction(tid).expect("ask to roll back");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  conn.close_transaction(tid).expect("close");

  // Through the unique index alike.
  let index = bind_unique_index(&mut conn, "bt_code", "bt");
  let key_rec = record_over(&t, &["code"]);
  let val_rec = record_over(&t, &["id", "val"]);
  let uq = conn
    .create_unique_query(&index, &key_rec, &val_rec)
    .expect("a unique query");
  let mut code_key = vec![0; key_rec.row_size() as usize];
  put_into(&t, &key_rec, &mut code_key, "code", "200");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  conn
    .query_mut(uq)
    .expect("the query")
    .key_row_mut()
    .copy_from_slice(&code_key);
  assert_eq!(
    refused_with(conn.write_key(uq, tid, &delete)),
    err::IC_ERROR_DELETE_WITH_BLOBS
  );
  conn.rollback_transaction(tid).expect("ask to roll back");
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  conn.close_transaction(tid).expect("close");

  // Nothing went, and the long TEXT is whole.
  let seen = mysql(&format!(
    "SELECT id, LENGTH(txt) FROM {}.bt ORDER BY id",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("1\t5\n2\t5000".to_string()));
}

#[test]
fn blob_guard_leaves_the_text_alone_on_insert_update_and_write() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_blob_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "bt");

  // An insert leaves the TEXT NULL; an update and a read leave it be.
  write_bt(&mut conn, &t, key_ops::IC_OP_INSERT, 10, 1000, "10")
    .expect("insert");
  write_bt(&mut conn, &t, key_ops::IC_OP_UPDATE, 10, 1000, "11")
    .expect("update");
  let row = read_by_id(&mut conn, &t, 10).expect("the row");
  assert_eq!(t.get(&row, "code"), "1000");
  assert_eq!(t.get(&row, "val"), "11");
  let seen = mysql(&format!(
    "SELECT id, code, val, txt IS NULL FROM {}.bt WHERE id = 10",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("10\t1000\t11\t1".to_string()));

  // A row whose TEXT is long enough to be kept in parts, made by
  // MySQL, then written over and updated by us: the TEXT stays whole.
  mysql(&format!(
    "INSERT INTO {}.bt VALUES (20, 2000, 20, REPEAT('abc', 3000))",
    IC_IT_DB
  ));
  write_bt(&mut conn, &t, key_ops::IC_OP_WRITE, 20, 2000, "21")
    .expect("write over it");
  write_bt(&mut conn, &t, key_ops::IC_OP_UPDATE, 20, 2000, "22")
    .expect("update it");
  let seen = mysql(&format!(
    "SELECT val, LENGTH(txt), txt = REPEAT('abc', 3000) FROM {}.bt
       WHERE id = 20",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("22\t9000\t1".to_string()));

  // MySQL deletes the rows, parts and all.
  mysql(&format!("DELETE FROM {}.bt", IC_IT_DB));
  assert!(read_by_id(&mut conn, &t, 20).is_none());
}

// ---- timeout ----

/// A data node's process, stopped with SIGSTOP. Dropping this continues
/// it with SIGCONT, so that a failing test leaves the cluster running.
struct Stopped {
  pid: String,
}

impl Stopped {
  fn stop(pid: &str) -> Stopped {
    let status = Command::new("kill")
      .arg("-STOP")
      .arg(pid)
      .status()
      .expect("run kill");
    assert!(status.success(), "could not stop process {}", pid);
    Stopped {
      pid: pid.to_string(),
    }
  }
}

impl Drop for Stopped {
  fn drop(&mut self) {
    let _ = Command::new("kill").arg("-CONT").arg(&self.pid).status();
  }
}

/// The process that listens on a TCP port of this machine, by `lsof`,
/// or `None` if there is none, or no `lsof`.
fn listener_pid(port: u16) -> Option<String> {
  let out = Command::new("lsof")
    .arg("-t")
    .arg("-nP")
    .arg(format!("-iTCP:{}", port))
    .arg("-sTCP:LISTEN")
    .output()
    .ok()?;
  let text = String::from_utf8_lossy(&out.stdout);
  let pid = text.lines().next()?.trim().to_string();
  if pid.is_empty() {
    return None;
  }
  Some(pid)
}

/// Insert a row of the `pk` table in a transaction at `node_id`,
/// defined and asked to commit; the caller sends it.
fn insert_at(
  conn: &mut ApidConnection,
  t: &Table,
  qid: QueryId,
  node_id: u32,
  id: &str,
  val: &str,
) -> TransId {
  let mut row = t.row();
  t.put(&mut row, "id", id);
  t.put(&mut row, "val", val);
  t.put(&mut row, "name", "timeout");
  let tid = conn
    .start_transaction(TransactionHint::Node(node_id), Some(&t.def))
    .expect("a transaction");
  let query = conn.query_mut(qid).expect("the query");
  query.key_row_mut().copy_from_slice(&row);
  query.attr_row_mut().copy_from_slice(&row);
  let insert = WriteKeyArgs {
    kind: WriteKind::Insert,
    ..WriteKeyArgs::default()
  };
  conn
    .write_key(qid, tid, &insert)
    .expect("define the insert");
  conn.commit_transaction(tid).expect("ask to commit");
  tid
}

#[test]
fn timeout_a_transaction_whose_node_stops_answering_ends_with_4012() {
  if std::env::var("IC_TEST_NDB_MGM").is_err() {
    eprintln!("skipped: IC_TEST_NDB_MGM is not set, and this stops a node");
    return;
  }
  let cluster = match cluster_with_reply_wait(IC_IT_REPLY_WAIT_MS) {
    Some(cluster) => cluster,
    None => return,
  };
  assert_eq!(cluster.global.reply_wait_ms(), IC_IT_REPLY_WAIT_MS);
  if setup_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");
  let victim = *conn.started_nodes().last().expect("a started node");
  let port = match cluster.global.config().link_to(victim) {
    Some(link) => link.server_port,
    None => 0,
  };
  if port == 0 {
    eprintln!("skipped: data node {} has no fixed port", victim);
    return;
  }
  let pid = match listener_pid(port) {
    Some(pid) => pid,
    None => {
      eprintln!("skipped: no process found listening on port {}", port);
      return;
    }
  };
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");

  // The transaction's record is allocated while the node answers; its
  // insert and commit go once the node has stopped, its link still up.
  let tid = insert_at(&mut conn, &t, qid, victim, "21", "210");
  let stopped = Stopped::stop(&pid);
  let start = time::gethrtime();
  assert_eq!(finish(&mut conn, tid), CommitState::RolledBack);
  let waited = time::millis_elapsed(start, time::gethrtime());
  let trans = conn.transaction(tid).expect("the transaction");
  assert_eq!(trans.error().map(|e| e.code), Some(IC_NDB_ERROR_NO_REPLY));
  let query = conn.query(qid).expect("the query");
  assert_eq!(query.error().map(|e| e.code), Some(IC_NDB_ERROR_NO_REPLY));
  // After the wait, within the second between checks, and long before
  // the heartbeats would have failed the node.
  let limit = IC_IT_REPLY_WAIT_MS as u64;
  assert!(waited >= limit, "ended after {} ms", waited);
  assert!(waited < limit + 3_000, "ended after {} ms", waited);
  assert!(
    conn.started_nodes().contains(&victim),
    "node {} was taken for failed",
    victim
  );
  conn.close_transaction(tid).expect("close");
  drop(stopped);

  // The node goes on, its backlog with it; whether the first insert
  // committed is not known. The next transaction there works.
  let tid = insert_at(&mut conn, &t, qid, victim, "22", "220");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  let row = read_by_id(&mut conn, &t, 22).expect("row 22");
  assert_eq!(t.get(&row, "val"), "220");

  // Letting go of the connection gives back every record still held,
  // each once: the one given up above is freed already, and freeing it
  // again would stop a debug data node (chapter 05 §17.10).
  drop(conn);
  std::thread::sleep(std::time::Duration::from_secs(2));
  assert!(
    cluster.global.started_nodes().contains(&victim),
    "node {} stopped after the connection let go of its records",
    victim
  );
}

// ---- charset ----

/// A string in a character set's encoding, for a key row, or `None`
/// if the set cannot hold it.
fn encode(charset: &str, text: &str) -> Option<Vec<u8>> {
  match charset {
    "utf8mb4" => Some(text.as_bytes().to_vec()),
    "utf8mb3" | "ucs2" if text.chars().any(|c| c as u32 > 0xFFFF) => None,
    "utf8mb3" => Some(text.as_bytes().to_vec()),
    "utf16" | "ucs2" => Some(
      text
        .encode_utf16()
        .flat_map(|unit| unit.to_be_bytes())
        .collect(),
    ),
    "utf16le" => Some(
      text
        .encode_utf16()
        .flat_map(|unit| unit.to_le_bytes())
        .collect(),
    ),
    "utf32" => Some(
      text
        .chars()
        .flat_map(|c| (c as u32).to_be_bytes())
        .collect(),
    ),
    "latin1" => text
      .chars()
      .map(|c| {
        if (c as u32) < 0x100 {
          Some(c as u8)
        } else {
          None
        }
      })
      .collect(),
    "latin2" => text.chars().map(latin2_byte).collect(),
    "cp1251" => text.chars().map(cp1251_byte).collect(),
    _ => panic!("no encoder for {}", charset),
  }
}

/// The latin2 (ISO 8859-2) byte of the few letters the tests use.
fn latin2_byte(c: char) -> Option<u8> {
  let byte = match c {
    'Ł' => 0xA3,
    'ł' => 0xB3,
    'ó' => 0xF3,
    'Ó' => 0xD3,
    'ź' => 0xBC,
    'Ź' => 0xAC,
    'č' => 0xE8,
    'Č' => 0xC8,
    'š' => 0xB9,
    'Š' => 0xA9,
    'ž' => 0xBE,
    'Ž' => 0xAE,
    c if (c as u32) < 0x80 => c as u8,
    _ => return None,
  };
  Some(byte)
}

/// The cp1251 byte of the Cyrillic letters the tests use: А to я are
/// 0xC0 to 0xFF in order.
fn cp1251_byte(c: char) -> Option<u8> {
  match c as u32 {
    0x410..=0x44F => Some((c as u32 - 0x410 + 0xC0) as u8),
    n if n < 0x80 => Some(n as u8),
    _ => None,
  }
}

/// Put a string's bytes into a column of a key row: a CHAR padded with
/// the set's spaces, a VARCHAR behind its length.
fn put_raw(
  rec: &Record,
  column: &str,
  bytes: &[u8],
  space: &[u8],
  row: &mut [u8],
) {
  let id = rec.table().field_id(column).expect("the column");
  let position = rec.position_of(id).expect("in the record");
  let field = rec.field(position).expect("the field");
  let attr = rec.table().field(id).expect("the column");
  let at = field.offset() as usize;
  let size = field.size() as usize;
  match attr.array_kind {
    ic_signals::table_props::IC_ARRAY_KIND_SHORT_VARSIZE => {
      assert!(1 + bytes.len() <= size, "{} bytes do not fit", bytes.len());
      row[at] = bytes.len() as u8;
      row[at + 1..at + 1 + bytes.len()].copy_from_slice(bytes);
    }
    ic_signals::table_props::IC_ARRAY_KIND_LONG_VARSIZE => {
      assert!(2 + bytes.len() <= size, "{} bytes do not fit", bytes.len());
      row[at..at + 2].copy_from_slice(&(bytes.len() as u16).to_le_bytes());
      row[at + 2..at + 2 + bytes.len()].copy_from_slice(bytes);
    }
    _ => {
      assert!(bytes.len() <= size, "{} bytes do not fit", bytes.len());
      for (i, byte) in row[at..at + size].iter_mut().enumerate() {
        *byte = space[i % space.len()];
      }
      row[at..at + bytes.len()].copy_from_slice(bytes);
    }
  }
}

/// One collation to check, with the strings to store and look up.
struct CollationCase {
  charset: &'static str,
  collation: &'static str,
  strings: Vec<&'static str>,
}

/// Strings every set of the tests holds.
const ASCII_STRINGS: [&str; 8] = [
  "", "a", "abc", "Hello", "zebra", "ABC xyz", "x1y2z3", "llama",
];
/// Strings latin1 holds beyond those.
const LATIN1_STRINGS: [&str; 10] = [
  "café", "Ärger", "Aerger", "straße", "strasse", "ÅÄÖ", "mañana", "Øl", "ÿ",
  "Æble",
];
/// Strings Unicode holds beyond those.
const UNICODE_STRINGS: [&str; 12] = [
  "日本語",
  "한국어",
  "Ελληνικά",
  "Привет",
  "ﬁx",
  "e\u{301}t\u{e9}",
  "ǅemal",
  "カー",
  "かー",
  "ｶｰ",
  "中文",
  "chico",
];

/// For each collation, a VARCHAR(20) and a CHAR(12) keyed table made by
/// a MySQL server and filled with the strings; then each string and
/// its upper-case, lower-case and trailing-space variants are placed
/// both ways, ours and the data nodes', wherever the collation finds
/// a row for them. Every mismatch is gathered and reported at once.
fn check_collations(conn: &mut ApidConnection, cases: &[CollationCase]) {
  let mut failures: Vec<String> = Vec::new();
  for (n, case) in cases.iter().enumerate() {
    let mut sql = format!("CREATE DATABASE IF NOT EXISTS {};", IC_IT_DB);
    for (table, column) in [("v", "VARCHAR(20)"), ("f", "CHAR(12)")] {
      sql.push_str(&format!(
        "DROP TABLE IF EXISTS {db}.ck{t}{n};
         CREATE TABLE {db}.ck{t}{n} (
           k {column} CHARACTER SET {cs} COLLATE {coll} NOT NULL PRIMARY KEY,
           v INT) ENGINE=NDB;",
        db = IC_IT_DB,
        t = table,
        n = n,
        column = column,
        cs = case.charset,
        coll = case.collation
      ));
      let mut values: Vec<String> = Vec::new();
      for (i, text) in case.strings.iter().enumerate() {
        if table == "f" && text.chars().count() > 12 {
          continue;
        }
        values.push(format!("('{}', {})", text, i));
      }
      sql.push_str(&format!(
        "INSERT IGNORE INTO {}.ck{}{} VALUES {};",
        IC_IT_DB,
        table,
        n,
        values.join(", ")
      ));
    }
    if mysql(&sql).is_none() {
      return;
    }
    let space = encode(case.charset, " ").expect("a space");
    let mut found: usize = 0;
    for table in ["v", "f"] {
      let t = bind(conn, &format!("ck{}{}", table, n));
      for text in &case.strings {
        let mut variants: Vec<String> = vec![
          text.to_string(),
          text.to_uppercase(),
          text.to_lowercase(),
          format!("{} ", text),
          format!("{}  ", text),
        ];
        variants.dedup();
        for variant in variants {
          let limit = if table == "f" { 12 } else { 20 };
          if variant.chars().count() > limit {
            continue;
          }
          let bytes = match encode(case.charset, &variant) {
            Some(bytes) => bytes,
            // A variant the set cannot hold, such as Ÿ in latin1.
            None => continue,
          };
          let mut row = t.row();
          put_raw(&t.rec, "k", &bytes, &space, &mut row);
          let ours = match ic_apid::hash::partition_of(&t.def, &t.rec, &row) {
            Ok(partition) => partition,
            Err(e) => {
              failures.push(format!(
                "{} {}: {:?} not placed ({})",
                case.collation, table, variant, e.code
              ));
              continue;
            }
          };
          match key_op::read_partition(conn, &t.rec, &row) {
            Ok(Some(theirs)) => {
              found += 1;
              if theirs != ours {
                failures.push(format!(
                  "{} {}: {:?} is in partition {}, ours {}",
                  case.collation, table, variant, theirs, ours
                ));
              }
            }
            // No row that the collation calls equal: nothing to check.
            Ok(None) => {}
            Err(e) => failures.push(format!(
              "{} {}: {:?} could not be read ({})",
              case.collation, table, variant, e.code
            )),
          }
        }
      }
    }
    eprintln!("{}: {} keys checked", case.collation, found);
    if found < case.strings.len() {
      failures.push(format!(
        "{}: only {} keys found for {} strings",
        case.collation,
        found,
        case.strings.len()
      ));
    }
  }
  assert!(failures.is_empty(), "{}", failures.join("\n"));
}

/// The strings for a set: ASCII, then latin1's, then Unicode's, as far
/// as the set goes.
fn strings_for(latin1: bool, unicode: bool) -> Vec<&'static str> {
  let mut strings: Vec<&'static str> = ASCII_STRINGS.to_vec();
  if latin1 || unicode {
    strings.extend_from_slice(&LATIN1_STRINGS);
  }
  if unicode {
    strings.extend_from_slice(&UNICODE_STRINGS);
  }
  strings
}

#[test]
fn charset_latin1_keys_in_every_collation() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let mut cases: Vec<CollationCase> = Vec::new();
  for collation in [
    "latin1_swedish_ci",
    "latin1_german1_ci",
    "latin1_danish_ci",
    "latin1_german2_ci",
    "latin1_bin",
    "latin1_general_ci",
    "latin1_general_cs",
    "latin1_spanish_ci",
  ] {
    cases.push(CollationCase {
      charset: "latin1",
      collation,
      strings: strings_for(true, false),
    });
  }
  check_collations(&mut conn, &cases);
}

#[test]
fn charset_utf8_pad_space_keys() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let mut cases: Vec<CollationCase> = Vec::new();
  for collation in [
    "utf8mb4_general_ci",
    "utf8mb4_bin",
    "utf8mb4_unicode_ci",
    "utf8mb4_unicode_520_ci",
  ] {
    let mut strings = strings_for(true, true);
    strings.push("😀");
    cases.push(CollationCase {
      charset: "utf8mb4",
      collation,
      strings,
    });
  }
  for collation in ["utf8mb3_general_ci", "utf8mb3_unicode_ci", "utf8mb3_bin"] {
    cases.push(CollationCase {
      charset: "utf8mb3",
      collation,
      strings: strings_for(true, true),
    });
  }
  check_collations(&mut conn, &cases);
}

#[test]
fn charset_utf8mb4_0900_keys() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let mut cases: Vec<CollationCase> = Vec::new();
  for collation in [
    "utf8mb4_0900_ai_ci",
    "utf8mb4_0900_as_ci",
    "utf8mb4_0900_as_cs",
    "utf8mb4_0900_bin",
    "utf8mb4_es_trad_0900_ai_ci",
    "utf8mb4_de_pb_0900_ai_ci",
    "utf8mb4_sv_0900_ai_ci",
    "utf8mb4_ja_0900_as_cs",
    "utf8mb4_ja_0900_as_cs_ks",
    "utf8mb4_zh_0900_as_cs",
  ] {
    let mut strings = strings_for(true, true);
    strings.push("😀");
    cases.push(CollationCase {
      charset: "utf8mb4",
      collation,
      strings,
    });
  }
  check_collations(&mut conn, &cases);
}

#[test]
fn charset_other_sets() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let mut cases: Vec<CollationCase> = Vec::new();
  for (charset, collation) in [
    ("ucs2", "ucs2_general_ci"),
    ("utf16", "utf16_general_ci"),
    ("utf16le", "utf16le_general_ci"),
    ("utf32", "utf32_general_ci"),
  ] {
    cases.push(CollationCase {
      charset,
      collation,
      strings: strings_for(true, true),
    });
  }
  let mut latin2 = ASCII_STRINGS.to_vec();
  latin2.extend_from_slice(&["Łódź", "čšž", "ŽŠČ"]);
  cases.push(CollationCase {
    charset: "latin2",
    collation: "latin2_general_ci",
    strings: latin2,
  });
  let mut cyrillic = ASCII_STRINGS.to_vec();
  cyrillic.extend_from_slice(&["Привет", "мир"]);
  cases.push(CollationCase {
    charset: "cp1251",
    collation: "cp1251_general_ci",
    strings: cyrillic,
  });
  check_collations(&mut conn, &cases);
}

#[test]
fn charset_neighbour_collations_are_placed_in_turn() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.cz;
     CREATE TABLE {db}.cz (
       k VARCHAR(20) CHARACTER SET latin2 COLLATE latin2_czech_cs NOT NULL
         PRIMARY KEY, v INT) ENGINE=NDB;
     INSERT INTO {db}.cz VALUES ('chata', 1);",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "cz");
  let mut row = t.row();
  put_raw(&t.rec, "k", b"chata", b" ", &mut row);
  // Czech weighs a character by its neighbours: not placed, and the
  // operation still finds its row wherever it goes.
  assert!(ic_apid::hash::partition_of(&t.def, &t.rec, &row).is_err());
  assert_eq!(
    conn.hint_for_key(&t.def, &t.rec, &row),
    TransactionHint::Any
  );
  assert!(key_op::read_partition(&mut conn, &t.rec, &row)
    .expect("read")
    .is_some());
}

#[test]
fn charset_keys_of_several_columns() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  // Columns of three collations and an integer in one key; the same
  // with the distribution key a subset in another order; and a key
  // whose columns come in another order than the table's.
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.mk;
     CREATE TABLE {db}.mk (
       a VARCHAR(10) CHARACTER SET latin1 COLLATE latin1_swedish_ci NOT NULL,
       b INT NOT NULL,
       c VARCHAR(10) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
       d CHAR(4) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL,
       v INT,
       PRIMARY KEY (a, b, c, d)) ENGINE=NDB;
     DROP TABLE IF EXISTS {db}.mkp;
     CREATE TABLE {db}.mkp (
       a VARCHAR(10) CHARACTER SET latin1 COLLATE latin1_swedish_ci NOT NULL,
       b INT NOT NULL,
       c VARCHAR(10) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
       d CHAR(4) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL,
       v INT,
       PRIMARY KEY (a, b, c, d)) ENGINE=NDB PARTITION BY KEY (c, b);
     DROP TABLE IF EXISTS {db}.mko;
     CREATE TABLE {db}.mko (
       c VARCHAR(10) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs NOT NULL,
       v INT,
       a CHAR(5) CHARACTER SET latin1 COLLATE latin1_bin NOT NULL,
       PRIMARY KEY (a, c)) ENGINE=NDB;",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  let rows = [
    ("abc", 1, "Hello", "x"),
    ("café", 2, "Ärger", "yz"),
    ("ÅÄÖ", 3, "日本語", "äb"),
    ("", 4, "", ""),
    ("zz", -5, "😀", "w"),
  ];
  let mut sql = String::new();
  for table in ["mk", "mkp"] {
    let mut values: Vec<String> = Vec::new();
    for (a, b, c, d) in rows {
      values.push(format!("('{}', {}, '{}', '{}', 0)", a, b, c, d));
    }
    sql.push_str(&format!(
      "INSERT INTO {}.{} VALUES {};",
      IC_IT_DB,
      table,
      values.join(", ")
    ));
  }
  let mut values: Vec<String> = Vec::new();
  for (a, _, c, _) in rows {
    values.push(format!("('{}', 0, '{}')", c, a));
  }
  sql.push_str(&format!(
    "INSERT INTO {}.mko VALUES {};",
    IC_IT_DB,
    values.join(", ")
  ));
  mysql(&sql);

  let mut conn = cluster.global.create_connection().expect("a connection");
  let mut failures: Vec<String> = Vec::new();
  for table in ["mk", "mkp", "mko"] {
    let t = bind(&mut conn, table);
    for (a, b, c, d) in rows {
      // As stored, and with the case of the case-insensitive columns
      // changed, which names the same row.
      let mut variants = vec![(a.to_string(), c.to_string(), d.to_string())];
      if table != "mko" {
        variants.push((a.to_uppercase(), c.to_uppercase(), d.to_uppercase()));
      }
      for (a, c, d) in variants {
        let mut row = t.row();
        let latin1 = encode("latin1", &a).expect("latin1");
        put_raw(&t.rec, "a", &latin1, b" ", &mut row);
        put_raw(&t.rec, "c", c.as_bytes(), b" ", &mut row);
        if table != "mko" {
          t.put(&mut row, "b", &b.to_string());
          put_raw(&t.rec, "d", d.as_bytes(), b" ", &mut row);
        }
        let ours = match ic_apid::hash::partition_of(&t.def, &t.rec, &row) {
          Ok(partition) => partition,
          Err(e) => {
            failures.push(format!(
              "{} {:?}: not placed ({})",
              table,
              (&a, b, &c, &d),
              e.code
            ));
            continue;
          }
        };
        match key_op::read_partition(&mut conn, &t.rec, &row) {
          Ok(Some(theirs)) if theirs == ours => {}
          other => failures.push(format!(
            "{} {:?}: ours {}, the data nodes' {:?}",
            table,
            (&a, b, &c, &d),
            ours,
            other.map_err(|e| e.code)
          )),
        }
      }
    }
  }
  assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn charset_unique_keys_over_character_columns() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.uc;
     CREATE TABLE {db}.uc (
       id INT NOT NULL PRIMARY KEY,
       u VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
       w VARCHAR(20) CHARACTER SET latin1 COLLATE latin1_swedish_ci NOT NULL,
       v INT,
       UNIQUE KEY uc_u (u),
       UNIQUE KEY uc_wu (w, u)) ENGINE=NDB;
     INSERT INTO {db}.uc VALUES
       (1, 'Hello', 'one', 1), (2, 'Ärger', 'café', 2),
       (3, '日本語', 'ÅÄÖ', 3), (4, '', '', 4), (5, '😀', 'zz', 5);",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "uc");
  let keys = [
    ("Hello", "one"),
    ("HELLO", "ONE"),
    ("Ärger", "café"),
    ("ärger", "CAFÉ"),
    ("日本語", "ÅÄÖ"),
    ("", ""),
    ("😀", "zz"),
  ];
  let mut failures: Vec<String> = Vec::new();
  for (name, columns) in [("uc_u", vec!["u"]), ("uc_wu", vec!["w", "u"])] {
    let index = bind_unique_index(&mut conn, name, "uc");
    let key_rec = record_over(&t, &columns);
    let own = index.as_table();
    let mut own_ids: Vec<u32> = Vec::new();
    for column in &columns {
      own_ids.push(own.field_id(column).expect("in the index"));
    }
    let own_rec = Record::default_for(own, Some(own_ids.as_slice()))
      .expect("a record over the index's table");
    for (u, w) in keys {
      let mut key = vec![0; key_rec.row_size() as usize];
      let mut own_key = vec![0; own_rec.row_size() as usize];
      put_raw(&key_rec, "u", u.as_bytes(), b" ", &mut key);
      put_raw(&own_rec, "u", u.as_bytes(), b" ", &mut own_key);
      if columns.len() == 2 {
        let latin1 = encode("latin1", w).expect("latin1");
        put_raw(&key_rec, "w", &latin1, b" ", &mut key);
        put_raw(&own_rec, "w", &latin1, b" ", &mut own_key);
      }
      let ours = match conn.hint_for_unique_key(&index, &key_rec, &key) {
        TransactionHint::Partition(partition) => partition,
        other => {
          failures.push(format!(
            "{} {:?}: not placed: {:?}",
            name,
            (u, w),
            other
          ));
          continue;
        }
      };
      match key_op::read_partition(&mut conn, &own_rec, &own_key) {
        Ok(Some(theirs)) if theirs == ours => {}
        other => failures.push(format!(
          "{} {:?}: ours {}, the data nodes' {:?}",
          name,
          (u, w),
          ours,
          other.map_err(|e| e.code)
        )),
      }
    }
  }
  assert!(failures.is_empty(), "{}", failures.join("\n"));
}

// ---- collation ----

/// The weight string of one code point, empty if the set has none.
fn weight(found: &Collation, code_point: u32) -> Vec<u8> {
  let mut out: Vec<u8> = Vec::new();
  for run in &found.runs {
    if code_point >= run.first && code_point - run.first < run.count {
      run.weight_of(code_point - run.first, &mut out);
      return out;
    }
  }
  out
}

#[test]
fn collation_is_described_by_name_number_and_character_set() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let by_name = collation_desc::IC_COLLATION_BY_NAME;
  let by_set = collation_desc::IC_COLLATION_BY_CHARSET;
  let named = collation_client::describe_by_name(
    &mut conn,
    by_name,
    "latin1_swedish_ci",
    false,
  )
  .expect("by name");
  assert_eq!(named.desc.number, 8);
  assert_eq!(named.desc.charset_name, "latin1");
  assert_eq!(named.desc.pad_byte, 0x20);
  assert!(!named.desc.is_no_pad());
  assert_eq!(named.round_trips, 1);
  let numbered =
    collation_client::describe_by_number(&mut conn, 8, false).expect("number");
  assert_eq!(numbered.desc.collation_name, "latin1_swedish_ci");
  // Names are matched without regard to case.
  let set =
    collation_client::describe_by_name(&mut conn, by_set, "LATIN1", false)
      .expect("by character set");
  assert_eq!(set.desc.number, set.desc.default_number);
  assert_eq!(set.desc.number, 8);
  let missing = collation_client::describe_by_name(
    &mut conn,
    by_name,
    "no_such_collation",
    false,
  );
  match missing {
    Ok(_) => panic!("a collation that does not exist was described"),
    Err(e) => assert_eq!(e.code, err::IC_ERROR_NO_SUCH_COLLATION),
  }
}

#[test]
fn collation_weights_cover_the_set_and_fold_case() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let by_name = collation_desc::IC_COLLATION_BY_NAME;
  let swedish = collation_client::describe_by_name(
    &mut conn,
    by_name,
    "latin1_swedish_ci",
    true,
  )
  .expect("latin1_swedish_ci with weights");
  let mut covered: u64 = 0;
  for run in &swedish.runs {
    covered += run.count as u64;
  }
  // Every byte of latin1 is a character of its own.
  assert_eq!(covered, 256);
  // Case folds in a case-insensitive collation, and letters differ.
  assert!(!weight(&swedish, 0x41).is_empty());
  assert_eq!(weight(&swedish, 0x41), weight(&swedish, 0x61));
  assert_ne!(weight(&swedish, 0x61), weight(&swedish, 0x62));
  let bin =
    collation_client::describe_by_name(&mut conn, by_name, "latin1_bin", true)
      .expect("latin1_bin with weights");
  assert_ne!(weight(&bin, 0x41), weight(&bin, 0x61));
  // A large collation: as many round trips as it takes, its runs in
  // rising code point order and not overlapping.
  let unicode = collation_client::describe_by_name(
    &mut conn,
    by_name,
    "utf8mb4_0900_ai_ci",
    true,
  )
  .expect("utf8mb4_0900_ai_ci with weights");
  assert!(unicode.desc.is_no_pad());
  assert!(!unicode.runs.is_empty());
  let mut next: u32 = 0;
  for run in &unicode.runs {
    assert!(run.first >= next, "runs overlap at U+{:04X}", run.first);
    next = run.first + run.count;
  }
  println!(
    "utf8mb4_0900_ai_ci: {} runs, {} contractions, {} round trips",
    unicode.runs.len(),
    unicode.contractions.len(),
    unicode.round_trips
  );
}
