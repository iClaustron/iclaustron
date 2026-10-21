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
//!   group, which restarts a data node under a transaction, and at once
//!   during a scan, and has the management server gone for a while.
//!   Set, it also lets the `timeout` group stop a data node's process
//!   for a few seconds with SIGSTOP, which needs `lsof` and the node's
//!   fixed port.
//! - `IC_TEST_MGM_RESTART`: a command that restarts the management
//!   server, for the `failure` group to use rather than pausing it with
//!   SIGSTOP, which needs `lsof` and the server's port.
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
//! (chapter 06 §8.6); `collation`, which asks the data nodes what a
//! collation is and for its weights (RonDB 26.04 main and later); and
//! `big_rows`, which writes and reads rows too large for one message,
//! in memory and on disk (chapter 06 §6.6); `executor`, which runs key
//! operations for application threads through an executor (chapter 06
//! §8.1); `scan`, which reads tables by scans in every lock mode, closes
//! them early and prunes them (chapter 06 §8.3), and reads ordered
//! indexes by ranges, merged in their order (chapter 06 §8.7); `exit`,
//! the runs phase 8 is accepted by: a scan of a million rows, which
//! takes about 60 MB a data node and is dropped after, and one executor
//! serving 1 000 threads (chapter 06 §8.8); `where`, which filters
//! scans and key reads by drawn conditions and compares the rows with
//! what MySQL selects, and `assign`, which runs drawn conditional
//! updates and deletes on one of twin tables while MySQL runs them on
//! the other, and compares the twins (chapter 06 §9.1). The `big_rows`
//! group makes a log file group and a tablespace if the cluster has
//! none, and keeps them.
//! Each test is its own program against the cluster, as an application
//! would be: it takes an API node id, binds its tables, does its work
//! through the public API and lets go.

#![cfg(feature = "integration")]
#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::collections::HashMap;
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
use ic_apid::executor::Done;
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
use ic_apid::query::QueryId;
use ic_apid::query::QueryState;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::TransId;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::record::Record;
use ic_apid::row_codec::IC_NDB_ERROR_NULL_KEY;
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
  cluster_with(reply_wait_ms, 30_000)
}

/// As [`cluster_with_reply_wait`], with a management server given
/// `mgm_timeout_ms` to answer once the global runs.
fn cluster_with(reply_wait_ms: u32, mgm_timeout_ms: u32) -> Option<Cluster> {
  let options = GlobalOptions {
    reply_wait_ms,
    ..GlobalOptions::default()
  };
  cluster_with_options(&options, mgm_timeout_ms)
}

/// As [`cluster_with`], the global started with `given`.
fn cluster_with_options(
  given: &GlobalOptions,
  mgm_timeout_ms: u32,
) -> Option<Cluster> {
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
    let mut options = *given;
    if let Ok(text) = std::env::var("IC_TEST_RECEIVE_THREADS") {
      options.receive_threads = text.parse().unwrap_or(1);
    }
    let started = ApidGlobal::start_with_options(
      config,
      mgm,
      connect_string,
      mgm_timeout_ms,
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
fn pk_every_user_thread_and_the_last_ones_work() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  // As many connections as an API node may have, and not one more.
  let limit = ic_port::consts::IC_MAX_THREAD_CONNECTIONS as usize;
  let mut conns: Vec<ApidConnection> = Vec::with_capacity(limit);
  while conns.len() < limit {
    conns.push(cluster.global.create_connection().expect("a connection"));
  }
  let e = cluster
    .global
    .create_connection()
    .expect_err("every user thread is taken");
  assert_eq!(e.code, err::IC_ERROR_TOO_MANY_USER_THREADS);

  // Two threads whose block indexes differ by 4 096: their transaction
  // ids differ in bit 38 alone, and each write's replies must come back
  // to the thread that sent it.
  let low: usize = 5;
  let high: usize = low + 4096;
  assert_eq!(conns[high].thread_id(), high as u32);
  {
    let conn = &mut conns[low];
    let t = bind(conn, "pk");
    write_pk(conn, &t, key_ops::IC_OP_WRITE, 41, "1", "low")
      .expect("a write from the low thread");
  }
  {
    let conn = &mut conns[high];
    let t = bind(conn, "pk");
    write_pk(conn, &t, key_ops::IC_OP_WRITE, 42, "2", "high")
      .expect("a write from the high thread");
    let row = read_by_id(conn, &t, 41).expect("the low thread's row");
    assert_eq!(t.get(&row, "name"), "'low'");
  }
  let last = limit - 1;
  {
    let conn = &mut conns[last];
    let t = bind(conn, "pk");
    let row = read_by_id(conn, &t, 42).expect("the high thread's row");
    assert_eq!(t.get(&row, "name"), "'high'");
  }
  let seen = mysql(&format!(
    "SELECT id, name FROM {}.pk WHERE id IN (41, 42) ORDER BY id",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("41\tlow\n42\thigh".to_string()));
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
  let mut columns: Vec<&str> = Vec::new();
  for (column, _) in values {
    columns.push(*column);
  }
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

  // CHAR and VARCHAR keys are found, and placed where the data nodes
  // put the index row (chapter 05 §6.3).
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
  for (index, column, value) in [(&c, "c", "three"), (&v, "v", "uno")] {
    let key_rec = record_over(&t, &[column]);
    let mut key = vec![0; key_rec.row_size() as usize];
    put_into(&t, &key_rec, &mut key, column, value);
    let own = index.as_table();
    let own_id = own.field_id(column).expect("in the index");
    let own_rec =
      Record::default_for(own, Some(&[own_id][..])).expect("a record");
    let mut own_key = vec![0; own_rec.row_size() as usize];
    let position = own_rec.position_of(own_id).expect("in the record");
    text_row::put_value(&own_rec, position, value, &mut own_key)
      .expect("put a value");
    let theirs = key_op::read_partition(&mut conn, &own_rec, &own_key)
      .expect("ask where the index row is")
      .expect("the index row is there");
    assert_eq!(
      conn.hint_for_unique_key(index, &key_rec, &key),
      TransactionHint::Partition(theirs),
      "{} = {:?}",
      column,
      value
    );
  }
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

// ---- executor ----

/// An executor on the cluster, with these limits.
fn executor_with(cluster: &Cluster, in_flight: u32, queue: u32) -> Executor {
  let options = ExecutorOptions {
    in_flight,
    queue,
    ..ExecutorOptions::default()
  };
  Executor::start(&cluster.global, &options).expect("an executor")
}

/// The `pk` table bound through a submitter, with its default record,
/// and the shape of operations on it by primary key.
fn pk_through(submitter: &Submitter) -> (Table, Arc<OpShape>) {
  let def = submitter.table_bind(IC_IT_DB, "pk").expect("bind pk");
  let rec = Record::default_for(&def, None).expect("a record");
  let shape = OpShape::new(&def, &rec, &rec).expect("a shape");
  (Table { def, rec }, shape)
}

/// A write of a `pk` row, its values filled in.
fn pk_write_op(
  t: &Table,
  shape: &Arc<OpShape>,
  kind: WriteKind,
  id: i64,
  val: &str,
  name: &str,
) -> KeyOp {
  let mut op = KeyOp::write(shape, kind);
  t.put(op.key_row_mut(), "id", &id.to_string());
  t.put(op.attr_row_mut(), "id", &id.to_string());
  t.put(op.attr_row_mut(), "val", val);
  t.put(op.attr_row_mut(), "name", name);
  op
}

/// A committed read of a `pk` row.
fn pk_read_op(t: &Table, shape: &Arc<OpShape>, id: i64) -> KeyOp {
  let mut op = KeyOp::read(shape, ReadKind::Committed);
  t.put(op.key_row_mut(), "id", &id.to_string());
  op
}

/// Read a `pk` row through the executor: the row, or `None` if there is
/// none, which leaves the transaction committed.
fn exec_read(
  submitter: &mut Submitter,
  t: &Table,
  shape: &Arc<OpShape>,
  id: i64,
) -> Option<Vec<u8>> {
  let mut request = Request::with_op(pk_read_op(t, shape, id));
  submitter.execute(&mut request).expect("a read commits");
  let read = &request.ops()[0];
  match read.error() {
    None => Some(read.attr_row().to_vec()),
    Some(e) if e.code == 626 => None,
    Some(e) => panic!("the read failed: {} ({})", e.message(), e.code),
  }
}

#[test]
fn executor_reads_and_writes_by_key_in_the_blocking_form() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let (t, shape) = pk_through(&submitter);

  let mut insert = Request::with_op(pk_write_op(
    &t,
    &shape,
    WriteKind::Insert,
    1,
    "10",
    "one",
  ));
  submitter.execute(&mut insert).expect("insert");
  assert_eq!(insert.commit_state(), CommitState::Committed);
  assert!(insert.gci() > 0, "a commit has a global checkpoint");
  let row = exec_read(&mut submitter, &t, &shape, 1).expect("the row");
  assert_eq!(t.get(&row, "val"), "10");
  assert_eq!(t.get(&row, "name"), "'one'");
  let seen = mysql(&format!(
    "SELECT val, name FROM {}.pk WHERE id = 1",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("10\tone".to_string()));

  // The request is used again: its rows, then its outcome, are reset.
  insert.ops_mut()[0]
    .set_kind(ic_apid::executor::OpKind::Write(WriteKind::Update));
  t.put(insert.ops_mut()[0].attr_row_mut(), "val", "11");
  submitter.execute(&mut insert).expect("update");
  let row = exec_read(&mut submitter, &t, &shape, 1).expect("still there");
  assert_eq!(t.get(&row, "val"), "11");

  // An insert of a row that is there rolls the transaction back.
  let mut again = Request::with_op(pk_write_op(
    &t,
    &shape,
    WriteKind::Insert,
    1,
    "12",
    "uno",
  ));
  let e = submitter.execute(&mut again).expect_err("there already");
  assert_eq!(e.code, 630);
  assert_eq!(again.commit_state(), CommitState::RolledBack);
  assert_eq!(again.ops()[0].error().map(|e| e.code), Some(630));

  let mut delete =
    Request::with_op(pk_write_op(&t, &shape, WriteKind::Delete, 1, "0", ""));
  submitter.execute(&mut delete).expect("delete");
  assert!(exec_read(&mut submitter, &t, &shape, 1).is_none(), "gone");
  let seen = mysql(&format!("SELECT COUNT(*) FROM {}.pk", IC_IT_DB));
  assert_eq!(seen, Some("0".to_string()));
  let stats = executor.stats();
  assert_eq!(stats.completed, stats.submitted);
  assert_eq!(stats.refused, 0);
}

#[test]
fn executor_a_request_of_several_operations_is_one_transaction() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let (t, shape) = pk_through(&submitter);

  let mut three = Request::new();
  for id in 1..=3 {
    three.push(pk_write_op(&t, &shape, WriteKind::Insert, id, "1", "x"));
  }
  submitter.execute(&mut three).expect("three inserts");
  let seen = mysql(&format!("SELECT COUNT(*) FROM {}.pk", IC_IT_DB));
  assert_eq!(seen, Some("3".to_string()));

  // One write fails, so neither goes in.
  let mut both = Request::new();
  both.push(pk_write_op(&t, &shape, WriteKind::Insert, 4, "4", "four"));
  both.push(pk_write_op(&t, &shape, WriteKind::Insert, 2, "2", "two"));
  let e = submitter.execute(&mut both).expect_err("2 is there");
  assert_eq!(e.code, 630);
  assert_eq!(both.ops()[1].error().map(|e| e.code), Some(630));
  let seen = mysql(&format!("SELECT COUNT(*) FROM {}.pk", IC_IT_DB));
  assert_eq!(seen, Some("3".to_string()), "4 was not inserted");

  // An operation that cannot be defined fails the request before
  // anything is sent: a committed read cannot abort on error.
  let mut bad = Request::new();
  bad.push(pk_write_op(&t, &shape, WriteKind::Insert, 5, "5", "five"));
  let mut read = pk_read_op(&t, &shape, 1);
  read.set_abort_option(AbortOption::AbortOnError);
  bad.push(read);
  let e = submitter.execute(&mut bad).expect_err("not definable");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  assert_eq!(bad.commit_state(), CommitState::RolledBack);
  assert_eq!(
    bad.ops()[0].error().map(|e| e.code),
    Some(err::IC_ERROR_TRANSACTION_ROLLED_BACK)
  );
  let seen = mysql(&format!(
    "SELECT COUNT(*) FROM {}.pk WHERE id = 5",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("0".to_string()), "5 was not inserted");

  // A locked read and an update of the same row, together.
  let mut locked = Request::new();
  let mut read_locked = pk_read_op(&t, &shape, 3);
  read_locked.set_kind(ic_apid::executor::OpKind::Read(ReadKind::Exclusive));
  locked.push(read_locked);
  locked.push(pk_write_op(&t, &shape, WriteKind::Update, 3, "33", "y"));
  submitter.execute(&mut locked).expect("read and update");
  assert_eq!(t.get(locked.ops()[0].attr_row(), "val"), "1");
  let seen = mysql(&format!("SELECT val FROM {}.pk WHERE id = 3", IC_IT_DB));
  assert_eq!(seen, Some("33".to_string()));
}

/// How many requests the callback test's function has been handed, and
/// how many of them were handed on the executor's own thread.
static EXEC_CALLBACKS: std::sync::atomic::AtomicUsize =
  std::sync::atomic::AtomicUsize::new(0);
static EXEC_CALLBACKS_ON_EXECUTOR: std::sync::atomic::AtomicUsize =
  std::sync::atomic::AtomicUsize::new(0);
static EXEC_CALLBACK_REFS: std::sync::atomic::AtomicUsize =
  std::sync::atomic::AtomicUsize::new(0);

fn note_request(request: Request, user_ref: usize) {
  use std::sync::atomic::Ordering;
  assert!(request.outcome().is_ok(), "{:?}", request);
  let name = std::thread::current().name().unwrap_or("").to_string();
  if name.starts_with("ic_executor_") {
    EXEC_CALLBACKS_ON_EXECUTOR.fetch_add(1, Ordering::Relaxed);
  }
  EXEC_CALLBACK_REFS.fetch_add(user_ref, Ordering::Relaxed);
  EXEC_CALLBACKS.fetch_add(1, Ordering::Release);
}

#[test]
fn executor_hands_requests_to_a_callback_on_its_thread() {
  use std::sync::atomic::Ordering;
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let (t, shape) = pk_through(&submitter);
  let mut row = Request::with_op(pk_write_op(
    &t,
    &shape,
    WriteKind::Insert,
    7,
    "70",
    "seven",
  ));
  submitter.execute(&mut row).expect("insert");
  let count: usize = 100;
  let mut i: usize = 1;
  while i <= count {
    let request = Request::with_op(pk_read_op(&t, &shape, 7));
    submitter
      .submit_wait(request, Done::Callback(note_request, i))
      .expect("submitted");
    i += 1;
  }
  let start = time::gethrtime();
  while EXEC_CALLBACKS.load(Ordering::Acquire) < count {
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(
      waited < IC_IT_TRANS_WAIT_MS,
      "the callbacks did not all come"
    );
    time::microsleep(1_000);
  }
  assert_eq!(EXEC_CALLBACKS.load(Ordering::Acquire), count);
  assert_eq!(EXEC_CALLBACKS_ON_EXECUTOR.load(Ordering::Relaxed), count);
  assert_eq!(
    EXEC_CALLBACK_REFS.load(Ordering::Relaxed),
    count * (count + 1) / 2,
    "each user_ref handed back once"
  );
}

#[test]
fn executor_serves_many_application_threads_together() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let mut submitter = executor.submitter();
  let (t, shape) = pk_through(&submitter);
  let rows: i64 = 200;
  let mut load = Request::new();
  for id in 1..=rows {
    let val = (id * 10).to_string();
    load.push(pk_write_op(&t, &shape, WriteKind::Insert, id, &val, "r"));
  }
  submitter
    .execute(&mut load)
    .expect("200 inserts in one transaction");
  let threads: usize = 32;
  let per_thread: i64 = 200;
  std::thread::scope(|scope| {
    let mut handles = Vec::new();
    for thread in 0..threads {
      let mut mine = submitter.clone();
      let t = &t;
      let shape = &shape;
      handles.push(scope.spawn(move || {
        let mut request = Request::with_op(pk_read_op(t, shape, 1));
        for n in 0..per_thread {
          let id = 1 + (thread as i64 * 7 + n) % rows;
          t.put(request.ops_mut()[0].key_row_mut(), "id", &id.to_string());
          mine.execute(&mut request).expect("a read");
          let read = &request.ops()[0];
          assert!(read.error().is_none(), "row {} is there", id);
          assert_eq!(t.get(read.attr_row(), "val"), (id * 10).to_string());
        }
      }));
    }
    for handle in handles {
      handle.join().expect("an application thread");
    }
  });
  let stats = executor.stats();
  let reads = threads as u64 * per_thread as u64;
  assert!(stats.completed > reads, "{:?}", stats);
  // Thirty-two threads waiting at once: passes of the loop admit
  // several requests and send them together.
  assert!(stats.sends < stats.admitted, "no batching: {:?}", stats);
}

#[test]
fn executor_runs_operations_through_a_unique_index() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  mysql(&format!(
    "INSERT INTO {}.uk VALUES (1, 100, 1), (2, 200, 2)",
    IC_IT_DB
  ));
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let def = submitter.table_bind(IC_IT_DB, "uk").expect("bind uk");
  let rec = Record::default_for(&def, None).expect("a record");
  let t = Table { def, rec };
  let index = submitter
    .index_bind(IC_IT_DB, "uk_code$unique", "uk")
    .expect("bind the index");
  let key_rec = record_over(&t, &["code"]);
  let shape = OpShape::new_unique(&index, &key_rec, &t.rec).expect("shape");
  let mut read = KeyOp::read(&shape, ReadKind::Committed);
  put_into(&t, &key_rec, read.key_row_mut(), "code", "200");
  let mut request = Request::with_op(read);
  submitter
    .execute(&mut request)
    .expect("read through the index");
  assert!(request.ops()[0].error().is_none());
  assert_eq!(t.get(request.ops()[0].attr_row(), "id"), "2");

  // An update through the index, of a column it is not over.
  let update_rec = record_over(&t, &["val"]);
  let update_shape =
    OpShape::new_unique(&index, &key_rec, &update_rec).expect("shape");
  let mut update = KeyOp::write(&update_shape, WriteKind::Update);
  put_into(&t, &key_rec, update.key_row_mut(), "code", "100");
  put_into(&t, &update_rec, update.attr_row_mut(), "val", "17");
  let mut request = Request::with_op(update);
  submitter
    .execute(&mut request)
    .expect("update through the index");
  let seen = mysql(&format!("SELECT val FROM {}.uk WHERE id = 1", IC_IT_DB));
  assert_eq!(seen, Some("17".to_string()));
}

#[test]
fn executor_deadlines_and_stopping_fail_requests_unsent() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_tables().is_none() {
    return;
  }
  let mut executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let (t, shape) = pk_through(&submitter);

  // Past its deadline when its turn comes: back unsent.
  let mut late = Request::with_op(pk_write_op(
    &t,
    &shape,
    WriteKind::Insert,
    9,
    "9",
    "late",
  ));
  late.set_timeout_ms(1);
  time::microsleep(20_000);
  let e = submitter.execute(&mut late).expect_err("too late");
  assert_eq!(e.code, err::IC_ERROR_TIMEOUT);
  assert_eq!(
    late.ops()[0].error().map(|e| e.code),
    Some(err::IC_ERROR_TIMEOUT)
  );
  assert_eq!(executor.stats().expired, 1);
  let seen = mysql(&format!("SELECT COUNT(*) FROM {}.pk", IC_IT_DB));
  assert_eq!(seen, Some("0".to_string()), "nothing was sent");

  // Without a deadline it goes.
  late.set_timeout_ms(0);
  submitter.execute(&mut late).expect("in time");

  // A stopped executor takes nothing, and hands the request back.
  executor.stop();
  let mut after = Request::with_op(pk_read_op(&t, &shape, 9));
  let e = submitter.execute(&mut after).expect_err("stopped");
  assert_eq!(e.code, err::IC_ERROR_EXECUTOR_STOPPED);
  assert_eq!(after.ops().len(), 1, "handed back whole");
  let refused = submitter
    .submit(after, Done::Slot(ic_apid::executor::Slot::new()))
    .expect_err("stopped");
  assert_eq!(refused.error.code, err::IC_ERROR_EXECUTOR_STOPPED);
  assert_eq!(refused.request.ops().len(), 1);
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
/// The port of the management server the tests connect to.
fn mgm_port() -> u16 {
  let text = std::env::var("IC_TEST_CONNECTSTRING").unwrap_or_default();
  let first = text.split(',').next().unwrap_or_default();
  match first.rsplit_once(':') {
    Some((_, port)) => port.parse().unwrap_or(1186),
    None => 1186,
  }
}

#[test]
fn failure_a_management_server_gone_for_a_while_is_found_again() {
  // A management server that answers late is given up on in 2 s rather
  // than 30, so that the test does not wait out the default.
  let cluster = match cluster_with(0, 2_000) {
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
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "pk");
  write_pk(&mut conn, &t, key_ops::IC_OP_WRITE, 60, "1", "sixty")
    .expect("a write before");

  // A data node restarts. To link to it again our node has to ask the
  // management server for its port, and the management server is gone
  // for a while just then: restarted by the operator's command if
  // IC_TEST_MGM_RESTART gives one, else paused with SIGSTOP and let go.
  let victim = started[1];
  eprintln!("restarting data node {}", victim);
  mgm_client(&format!("{} RESTART", victim));
  // Until it is down it may still be dealt a write in turn, which then
  // fails with its link: wait for it to go, so that the writes below go
  // to the survivor, as the test means them to.
  let start = time::gethrtime();
  while cluster.global.started_nodes().contains(&victim) {
    std::thread::sleep(std::time::Duration::from_millis(100));
    let waited = time::millis_elapsed(start, time::gethrtime());
    if waited >= IC_IT_RESTART_WAIT_MS {
      panic!("data node {} did not go down", victim);
    }
  }
  eprintln!("data node {} is down", victim);
  match std::env::var("IC_TEST_MGM_RESTART") {
    Ok(command) => {
      eprintln!("restarting the management server: {}", command);
      run_shell(&command, "");
    }
    Err(_) => {
      let pid = match listener_pid(mgm_port()) {
        Some(pid) => pid,
        None => panic!("nothing listens on port {}", mgm_port()),
      };
      eprintln!("pausing the management server, process {}", pid);
      let paused = Stopped::stop(&pid);
      // The links to the data nodes do not go through it: work goes on.
      write_pk(&mut conn, &t, key_ops::IC_OP_WRITE, 61, "2", "sixty-one")
        .expect("a write while it is gone");
      let row = read_by_id(&mut conn, &t, 60).expect("a read while it is gone");
      assert_eq!(t.get(&row, "val"), "1");
      // Long enough for our node to give up on it more than once.
      std::thread::sleep(std::time::Duration::from_secs(6));
      drop(paused);
      eprintln!("the management server runs again");
    }
  }
  let start = time::gethrtime();
  while cluster.global.started_nodes().len() < started.len() {
    std::thread::sleep(std::time::Duration::from_millis(500));
    let waited = time::millis_elapsed(start, time::gethrtime());
    if waited >= IC_IT_RESTART_WAIT_MS {
      let show = mgm_client("show").unwrap_or_default();
      panic!("data node {} was not linked again:\n{}", victim, show);
    }
  }
  eprintln!("data node {} is linked again", victim);
  write_pk(&mut conn, &t, key_ops::IC_OP_WRITE, 61, "2", "sixty-one")
    .expect("a write after");
  write_pk(&mut conn, &t, key_ops::IC_OP_WRITE, 62, "3", "sixty-two")
    .expect("a write after");
  let row = read_by_id(&mut conn, &t, 61).expect("a read after");
  assert_eq!(t.get(&row, "name"), "'sixty-one'");
  let seen = mysql(&format!(
    "SELECT id, val FROM {}.pk WHERE id BETWEEN 60 AND 62 ORDER BY id",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("60\t1\n61\t2\n62\t3".to_string()));
}

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
      assert!(bytes.len() < size, "{} bytes do not fit", bytes.len());
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

/// Insert a row of `txt` with `columns` given as text, the others NULL.
fn insert_text(
  conn: &mut ApidConnection,
  t: &Table,
  id: u32,
  columns: &[(&str, &str)],
) {
  let mut names: Vec<&str> = vec!["id"];
  for (name, _) in columns {
    names.push(*name);
  }
  let rec = record_over(t, &names);
  let mut row = vec![0; rec.row_size() as usize];
  put_into(t, &rec, &mut row, "id", &id.to_string());
  for (name, text) in columns {
    put_into(t, &rec, &mut row, name, text);
  }
  key_op::write_key(conn, key_ops::IC_OP_INSERT, &rec, &row, &rec, &row)
    .expect("insert");
}

/// Bytes from MySQL's `HEX()`, two digits a byte.
fn unhex(text: &str) -> Vec<u8> {
  let mut bytes: Vec<u8> = Vec::new();
  let mut i: usize = 0;
  while i + 2 <= text.len() {
    bytes.push(u8::from_str_radix(&text[i..i + 2], 16).expect("hex"));
    i += 2;
  }
  bytes
}

/// A column of row `id` of `txt` as MySQL converts it to UTF-8.
fn mysql_text(column: &str, id: u32) -> String {
  let hex = mysql(&format!(
    "SELECT HEX(CONVERT({} USING utf8mb4)) FROM {}.txt WHERE id = {}",
    column, IC_IT_DB, id
  ))
  .expect("MySQL");
  String::from_utf8(unhex(&hex)).expect("UTF-8")
}

#[test]
fn charset_text_in_and_out_through_the_tables() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.txt;
     CREATE TABLE {db}.txt (
       id INT NOT NULL PRIMARY KEY,
       l1 VARCHAR(255) CHARACTER SET latin1,
       u4 VARCHAR(40) CHARACTER SET utf8mb4,
       u3 VARCHAR(40) CHARACTER SET utf8mb3,
       u16 VARCHAR(40) CHARACTER SET utf16,
       u16le VARCHAR(40) CHARACTER SET utf16le,
       uc2 VARCHAR(40) CHARACTER SET ucs2,
       u32 VARCHAR(40) CHARACTER SET utf32,
       c4 CHAR(10) CHARACTER SET utf8mb4,
       c16 CHAR(10) CHARACTER SET utf16,
       l2 VARCHAR(20) CHARACTER SET latin2,
       cp CHAR(4) CHARACTER SET cp1251) ENGINE=NDB;",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "txt");

  // Every byte of latin1 but 0, written by MySQL, reads as MySQL
  // converts it; written back as text, it is the same bytes.
  let mut every: Vec<u8> = Vec::new();
  for byte in 1..=255u8 {
    every.push(byte);
  }
  mysql(&format!(
    "INSERT INTO {}.txt (id, l1) VALUES (1, UNHEX('{}'))",
    IC_IT_DB,
    hex(&every)
  ));
  let row = read_by_id(&mut conn, &t, 1).expect("row 1");
  let latin1 = mysql_text("l1", 1);
  assert_eq!(t.get(&row, "l1"), format!("'{}'", latin1));
  insert_text(&mut conn, &t, 2, &[("l1", latin1.as_str())]);
  let seen = mysql(&format!(
    "SELECT LOWER(HEX(l1)) FROM {}.txt WHERE id = 2",
    IC_IT_DB
  ));
  assert_eq!(seen, Some(hex(&every)));

  // The Unicode sets, written by MySQL and read here, and written here
  // and read by MySQL, byte for byte as MySQL writes them.
  let full = "Aé Ωж 漢字 😀";
  let bmp = "Aé Ωж 漢字";
  let sets: [(&str, &str); 8] = [
    ("u4", full),
    ("u3", bmp),
    ("u16", full),
    ("u16le", full),
    ("uc2", bmp),
    ("u32", full),
    ("c4", "ab"),
    ("c16", "ab"),
  ];
  let mut names: Vec<&str> = Vec::new();
  let mut given: Vec<String> = Vec::new();
  for (column, text) in sets {
    names.push(column);
    given.push(format!(
      "CONVERT(UNHEX('{}') USING utf8mb4)",
      hex(text.as_bytes())
    ));
  }
  mysql(&format!(
    "INSERT INTO {}.txt (id, {}) VALUES (3, {})",
    IC_IT_DB,
    names.join(", "),
    given.join(", ")
  ));
  let row = read_by_id(&mut conn, &t, 3).expect("row 3");
  for (column, text) in sets {
    assert_eq!(t.get(&row, column), format!("'{}'", text), "{}", column);
  }
  insert_text(&mut conn, &t, 4, &sets);
  for (column, text) in sets {
    assert_eq!(mysql_text(column, 4), text, "{}", column);
  }
  let mut hexes: Vec<String> = Vec::new();
  for column in &names {
    hexes.push(format!("HEX({})", column));
  }
  let bytes_of = |id: u32| {
    mysql(&format!(
      "SELECT {} FROM {}.txt WHERE id = {}",
      hexes.join(", "),
      IC_IT_DB,
      id
    ))
  };
  assert_eq!(bytes_of(4), bytes_of(3));

  // A variable-sized value keeps its trailing spaces.
  insert_text(&mut conn, &t, 7, &[("u4", "a  ")]);
  let seen = mysql(&format!(
    "SELECT HEX(u4) FROM {}.txt WHERE id = 7",
    IC_IT_DB
  ));
  assert_eq!(seen, Some("612020".to_string()));
  let row = read_by_id(&mut conn, &t, 7).expect("row 7");
  assert_eq!(t.get(&row, "u4"), "'a  '");

  // Sets with no table here are shown and given in hexadecimal, a
  // CHAR without its padding.
  mysql(&format!(
    "INSERT INTO {}.txt (id, l2, cp) VALUES (5, \
     CONVERT(UNHEX('{}') USING utf8mb4), CONVERT(UNHEX('{}') USING utf8mb4))",
    IC_IT_DB,
    hex("Łódź".as_bytes()),
    hex("Да".as_bytes())
  ));
  let row = read_by_id(&mut conn, &t, 5).expect("row 5");
  let theirs = |id: u32| {
    mysql(&format!(
      "SELECT LOWER(HEX(l2)), LOWER(HEX(cp)) FROM {}.txt WHERE id = {}",
      IC_IT_DB, id
    ))
    .expect("MySQL")
  };
  let seen = theirs(5);
  let (l2, cp) = (t.get(&row, "l2"), t.get(&row, "cp"));
  assert_eq!(
    format!("{}\t{}", l2, cp),
    format!("0x{}", seen.replace('\t', "\t0x"))
  );
  insert_text(
    &mut conn,
    &t,
    6,
    &[("l2", l2.as_str()), ("cp", cp.as_str())],
  );
  assert_eq!(theirs(6), seen);
  assert_eq!(mysql_text("l2", 6), "Łódź");

  // What a set cannot hold is refused, as is text for a set with no
  // table.
  let mut scratch = t.row();
  for (column, text) in
    [("l1", "Ł"), ("u3", "😀"), ("uc2", "😀"), ("l2", "Łódź")]
  {
    let refused =
      text_row::put_value(&t.rec, t.position(column), text, &mut scratch);
    assert!(refused.is_err(), "{} took {:?}", column, text);
  }
}

// ---- big_rows ----

/// The tablespace the `big_rows` group's disk columns go in.
const IC_IT_TABLESPACE: &str = "ic_it_ts";
/// The log file group made for it when the cluster has none.
const IC_IT_LOGFILE_GROUP: &str = "ic_it_lg";
// The limits of a row's parts, in RonDB from 2026-10 (the author,
// 2026-10-12): the fixed-size part 16 280 bytes, the variable-size part
// of the in-memory columns 8 158 words, and of the disk columns as
// much, the whole row 72 000 bytes. A MySQL server makes no row over
// 65 535 bytes, BLOB and TEXT apart; only the inline parts of blob
// columns would take a row past that, and those are neither read nor
// written yet. So each part is filled, both variable parts
// at once, and the whole row is left to the encoder's unit tests.

/// Bytes of the BINARY column that, after 63 BINARY(255), the key, the
/// unique column and the VARBINARY's place, fills `big_mem`'s fixed
/// part to its last word.
const IC_IT_BIG_FIXED_TAIL: usize = 108;
/// The VARBINARY that fills the variable part of the in-memory columns:
/// with its two length bytes and the 16 bytes the part takes besides
/// (measured against the earlier limit, 8 008 words), 8 158 words.
const IC_IT_BIG_VAR: usize = 32614;
/// The VARBINARY that, beside an INT and two BINARY(255) and the 24
/// bytes the part takes besides (measured against the earlier limit,
/// 7 770 words), fills the disk part's 8 158 words. With
/// [`IC_IT_BIG_VAR`] in memory the row's columns come to 65 486 bytes,
/// next to the 65 535 a MySQL server allows.
const IC_IT_BIG_DISK_VAR: usize = 32090;

/// The tablespace the disk columns go in, made once and kept: a cluster
/// has at most one log file group, and one already there is used.
fn setup_tablespace() -> Option<()> {
  let files = mysql(
    "SELECT DISTINCT FILE_TYPE, LOGFILE_GROUP_NAME, TABLESPACE_NAME \
     FROM information_schema.FILES WHERE ENGINE = 'ndbcluster'",
  )?;
  let mut group = String::new();
  let mut have_space = false;
  for line in files.lines() {
    let fields: Vec<&str> = line.split('\t').collect();
    if fields.len() < 3 {
      continue;
    }
    if fields[0] == "UNDO LOG" {
      group = fields[1].to_string();
    }
    if fields[0] == "DATAFILE" && fields[2] == IC_IT_TABLESPACE {
      have_space = true;
    }
  }
  if group.is_empty() {
    group = IC_IT_LOGFILE_GROUP.to_string();
    mysql(&format!(
      "CREATE LOGFILE GROUP {} ADD UNDOFILE 'ic_it_undo.log' \
       INITIAL_SIZE 32M UNDO_BUFFER_SIZE 4M ENGINE NDB",
      group
    ))?;
  }
  if !have_space {
    mysql(&format!(
      "CREATE TABLESPACE {} ADD DATAFILE 'ic_it_data.dat' \
       USE LOGFILE GROUP {} INITIAL_SIZE 64M ENGINE NDB",
      IC_IT_TABLESPACE, group
    ))?;
  }
  Some(())
}

/// The `CREATE TABLE` of a table shaped as `big_mem`, named `name`: the
/// last fixed-size column `tail` bytes, the VARBINARY `var`.
fn big_mem_sql(name: &str, tail: usize, var: usize) -> String {
  let mut fixed = String::new();
  for i in 0..63 {
    fixed.push_str(&format!("f{} BINARY(255), ", i));
  }
  format!(
    "CREATE TABLE {db}.{name} (
       id INT NOT NULL PRIMARY KEY,
       code INT NOT NULL,
       {fixed}
       fr BINARY({tail}),
       v VARBINARY({var}),
       UNIQUE KEY {name}_code (code)
     ) ENGINE=NDB",
    db = IC_IT_DB,
    name = name,
    fixed = fixed,
    tail = tail,
    var = var
  )
}

/// The `CREATE TABLE` of a table shaped as `big_disk`, named `name`:
/// the VARBINARY in memory `var` bytes, the one on disk `disk_var`.
fn big_disk_sql(name: &str, var: usize, disk_var: usize) -> String {
  format!(
    "CREATE TABLE {db}.{name} (
       id INT NOT NULL PRIMARY KEY,
       code INT NOT NULL,
       m0 BINARY(255) STORAGE MEMORY,
       v VARBINARY({var}) STORAGE MEMORY,
       dn INT STORAGE DISK,
       d0 BINARY(255) STORAGE DISK,
       d1 BINARY(255) STORAGE DISK,
       d VARBINARY({disk_var}) STORAGE DISK,
       UNIQUE KEY {name}_code (code)
     ) TABLESPACE {space} STORAGE DISK ENGINE=NDB",
    db = IC_IT_DB,
    name = name,
    var = var,
    disk_var = disk_var,
    space = IC_IT_TABLESPACE
  )
}

/// The `big_mem` and `big_disk` tables, made afresh, each part of their
/// rows at its limit, and the tablespace. `None` without a MySQL
/// client.
fn setup_big_tables() -> Option<()> {
  setup_tablespace()?;
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.big_mem;
     {mem};
     DROP TABLE IF EXISTS {db}.big_disk;
     {disk};",
    db = IC_IT_DB,
    mem = big_mem_sql("big_mem", IC_IT_BIG_FIXED_TAIL, IC_IT_BIG_VAR),
    disk = big_disk_sql("big_disk", IC_IT_BIG_VAR, IC_IT_BIG_DISK_VAR)
  );
  mysql(&sql)?;
  Some(())
}

/// Run SQL through the MySQL client, going on past a statement that
/// fails, and give back whether every statement went and what the
/// client printed, warnings and errors included; `None` without a
/// client.
fn mysql_try(sql: &str) -> Option<(bool, String)> {
  let command = match std::env::var("IC_TEST_MYSQL") {
    Ok(command) => command,
    Err(_) => {
      eprintln!("skipped: IC_TEST_MYSQL is not set");
      return None;
    }
  };
  let command =
    format!("{} --batch --skip-column-names --raw --force", command);
  let mut child = Command::new("sh")
    .arg("-c")
    .arg(&command)
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
    .expect("write to the command");
  let out = child.wait_with_output().expect("wait for the command");
  let said = format!(
    "{}{}",
    String::from_utf8_lossy(&out.stdout),
    String::from_utf8_lossy(&out.stderr)
  );
  let went = out.status.success() && !said.contains("ERROR");
  Some((went, said))
}

/// The binary columns of a `big_rows` table, in the table's order:
/// every column but the key, the unique column and the INT on disk.
fn big_columns(t: &Table) -> Vec<String> {
  let mut names: Vec<String> = Vec::new();
  let mut id: u32 = 0;
  while id < t.def.num_fields() {
    let attr = t.def.field(id).expect("the column");
    if attr.name != "id" && attr.name != "code" && attr.name != "dn" {
      names.push(attr.name.clone());
    }
    id += 1;
  }
  names
}

/// True if the table has column `name` and `rec` holds it.
fn holds(t: &Table, rec: &Record, name: &str) -> bool {
  match t.def.field_id(name) {
    Ok(id) => rec.position_of(id).is_some(),
    Err(_) => false,
  }
}

/// Bytes for one column of a row: different from row to row, `seed`,
/// and from column to column, and running through every byte value.
fn big_bytes(seed: u32, column: u32, len: usize) -> Vec<u8> {
  let mut bytes: Vec<u8> = Vec::with_capacity(len);
  let mut x = seed.wrapping_mul(2_654_435_761) ^ column.wrapping_mul(40_503);
  let mut i: usize = 0;
  while i < len {
    x = x.wrapping_mul(1_103_515_245).wrapping_add(12_345);
    bytes.push((x >> 16) as u8);
    i += 1;
  }
  bytes
}

/// A row of a `big_rows` table laid out by `rec`: the key and the
/// unique column if `rec` holds them, the INT on disk set to `seed`,
/// and every binary column filled from `seed`, a VARBINARY `var_len`
/// bytes long or as long as it goes. Gives back the row and the bytes
/// of each binary column, in the order of [`big_columns`].
fn big_row(
  t: &Table,
  rec: &Record,
  id: i64,
  seed: u32,
  var_len: usize,
) -> (Vec<u8>, Vec<Vec<u8>>) {
  let mut row = vec![0; rec.row_size() as usize];
  if holds(t, rec, "id") {
    put_into(t, rec, &mut row, "id", &id.to_string());
  }
  if holds(t, rec, "code") {
    put_into(t, rec, &mut row, "code", &(100 * id).to_string());
  }
  if holds(t, rec, "dn") {
    put_into(t, rec, &mut row, "dn", &seed.to_string());
  }
  let mut values: Vec<Vec<u8>> = Vec::new();
  for name in big_columns(t) {
    let field_id = t.def.field_id(&name).expect("the column");
    let attr = t.def.field(field_id).expect("the column");
    let position = rec.position_of(field_id).expect("in the record");
    let size = rec.field(position).expect("the field").size() as usize;
    let len = match attr.array_kind {
      ic_signals::table_props::IC_ARRAY_KIND_LONG_VARSIZE => {
        var_len.min(size - 2)
      }
      ic_signals::table_props::IC_ARRAY_KIND_SHORT_VARSIZE => {
        var_len.min(size - 1)
      }
      _ => size,
    };
    let bytes = big_bytes(seed, field_id, len);
    put_raw(rec, &name, &bytes, &[0], &mut row);
    values.push(bytes);
  }
  (row, values)
}

fn hex(bytes: &[u8]) -> String {
  let mut text = String::with_capacity(2 * bytes.len());
  for byte in bytes {
    text.push_str(&format!("{:02x}", byte));
  }
  text
}

/// Where two long texts first differ, for a failure that does not
/// print them.
fn first_difference(a: &str, b: &str) -> usize {
  let (a, b) = (a.as_bytes(), b.as_bytes());
  let mut i: usize = 0;
  while i < a.len() && i < b.len() && a[i] == b[i] {
    i += 1;
  }
  i
}

/// Check a row read back in the table's own layout against what
/// [`big_row`] wrote from `seed`.
fn check_big_row(t: &Table, row: &[u8], values: &[Vec<u8>], seed: u32) {
  let names = big_columns(t);
  assert_eq!(names.len(), values.len());
  let mut i: usize = 0;
  while i < names.len() {
    let got = t.get(row, &names[i]);
    let want = format!("0x{}", hex(&values[i]));
    assert!(
      got == want,
      "{}: {} characters read, {} written, the first difference at {}",
      names[i],
      got.len(),
      want.len(),
      first_difference(&got, &want)
    );
    i += 1;
  }
  if t.def.field_id("dn").is_ok() {
    assert_eq!(t.get(row, "dn"), seed.to_string());
  }
}

/// Check that MySQL sees row `id` of `table` as [`big_row`] wrote it:
/// the INT on disk, if any, and every binary column end to end.
fn check_big_row_in_mysql(
  t: &Table,
  table: &str,
  id: i64,
  values: &[Vec<u8>],
  seed: u32,
) {
  let mut dn = String::from("0");
  if t.def.field_id("dn").is_ok() {
    dn = String::from("dn");
  }
  let seen = mysql(&format!(
    "SELECT {}, LOWER(HEX(CONCAT({}))) FROM {}.{} WHERE id = {}",
    dn,
    big_columns(t).join(", "),
    IC_IT_DB,
    table,
    id
  ))
  .expect("MySQL");
  let mut all: Vec<u8> = Vec::new();
  for value in values {
    all.extend_from_slice(value);
  }
  let mut want_dn: u32 = 0;
  if t.def.field_id("dn").is_ok() {
    want_dn = seed;
  }
  let want = format!("{}\t{}", want_dn, hex(&all));
  assert!(
    seen == want,
    "MySQL shows {} characters, {} written, the first difference at {}",
    seen.len(),
    want.len(),
    first_difference(&seen, &want)
  );
}

/// Define an insert of `row`, laid out by the table's record, as query
/// `qid` of transaction `tid`.
fn define_insert(
  conn: &mut ApidConnection,
  qid: QueryId,
  tid: TransId,
  row: &[u8],
) {
  let query = conn.query_mut(qid).expect("the query");
  query.key_row_mut().copy_from_slice(row);
  query.attr_row_mut().copy_from_slice(row);
  let insert = WriteKeyArgs {
    kind: WriteKind::Insert,
    ..WriteKeyArgs::default()
  };
  conn
    .write_key(qid, tid, &insert)
    .expect("define the insert");
}

/// The rows of a table, by key, as MySQL lists them.
fn ids_in_mysql(table: &str) -> String {
  mysql(&format!(
    "SELECT id FROM {}.{} ORDER BY id",
    IC_IT_DB, table
  ))
  .expect("MySQL")
}

/// Write a row as large as `table` holds by primary key and read it
/// back that way; update it through its unique index and read it back
/// that way; write it over with short VARBINARY columns, and grow them
/// back to full with an update by primary key. MySQL checks each.
fn check_big_table(conn: &mut ApidConnection, t: &Table, table: &str) {
  let index = bind_unique_index(conn, &format!("{}_code", table), table);

  let (row, values) = big_row(t, &t.rec, 1, 1, usize::MAX);
  key_op::write_key(conn, key_ops::IC_OP_INSERT, &t.rec, &row, &t.rec, &row)
    .expect("insert");
  let back = read_by_id(conn, t, 1).expect("the row is there");
  check_big_row(t, &back, &values, 1);
  check_big_row_in_mysql(t, table, 1, &values, 1);

  // Every column but the key and the indexed one, through the index.
  let key_rec = record_over(t, &["code"]);
  let mut key = vec![0; key_rec.row_size() as usize];
  put_into(t, &key_rec, &mut key, "code", "100");
  let mut columns = big_columns(t);
  if t.def.field_id("dn").is_ok() {
    columns.push(String::from("dn"));
  }
  let mut names: Vec<&str> = Vec::new();
  for column in &columns {
    names.push(column.as_str());
  }
  let val_rec = record_over(t, &names);
  let (vals, values) = big_row(t, &val_rec, 1, 2, usize::MAX);
  let qid = conn
    .create_unique_query(&index, &key_rec, &val_rec)
    .expect("a unique query");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let query = conn.query_mut(qid).expect("the query");
  query.key_row_mut().copy_from_slice(&key);
  query.attr_row_mut().copy_from_slice(&vals);
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn
    .write_key(qid, tid, &update)
    .expect("define the update");
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  let mut out = t.row();
  let found = key_op::read_unique_committed(
    conn, &index, &key_rec, &key, &t.rec, &mut out,
  )
  .expect("read through the index");
  assert!(found, "the row is found through the index");
  check_big_row(t, &out, &values, 2);
  check_big_row_in_mysql(t, table, 1, &values, 2);

  let (row, values) = big_row(t, &t.rec, 1, 3, 10);
  key_op::write_key(conn, key_ops::IC_OP_WRITE, &t.rec, &row, &t.rec, &row)
    .expect("write over");
  let back = read_by_id(conn, t, 1).expect("still there");
  check_big_row(t, &back, &values, 3);
  check_big_row_in_mysql(t, table, 1, &values, 3);

  // The variable parts grown from a few bytes to full in one update.
  let (row, values) = big_row(t, &t.rec, 1, 4, usize::MAX);
  key_op::write_key(conn, key_ops::IC_OP_UPDATE, &t.rec, &row, &t.rec, &row)
    .expect("update to full");
  let found = key_op::read_unique_committed(
    conn, &index, &key_rec, &key, &t.rec, &mut out,
  )
  .expect("read through the index");
  assert!(found, "the row is still found through the index");
  check_big_row(t, &out, &values, 4);
  check_big_row_in_mysql(t, table, 1, &values, 4);
}

#[test]
fn big_rows_the_largest_in_memory_row_by_primary_and_unique_key() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_big_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "big_mem");
  check_big_table(&mut conn, &t, "big_mem");
}

#[test]
fn big_rows_a_row_with_disk_columns_by_primary_and_unique_key() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_big_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "big_disk");
  for column in ["dn", "d0", "d1", "d"] {
    let id = t.def.field_id(column).expect("the column");
    let attr = t.def.field(id).expect("the column");
    assert_eq!(
      attr.storage_kind,
      ic_signals::table_props::IC_STORAGE_DISK,
      "{} is on disk",
      column
    );
  }
  check_big_table(&mut conn, &t, "big_disk");
}

#[test]
fn big_rows_the_tables_fill_each_part_to_its_limit() {
  let _cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  // The tables were made with each part at its limit; a byte more in
  // any part is refused, and by the data nodes, not by the MySQL
  // server's own limit on a row.
  if setup_big_tables().is_none() {
    return;
  }
  let over: [(&str, String); 3] = [
    (
      "the fixed part",
      big_mem_sql("big_probe", IC_IT_BIG_FIXED_TAIL + 1, IC_IT_BIG_VAR),
    ),
    (
      "the variable part",
      big_mem_sql("big_probe", IC_IT_BIG_FIXED_TAIL, IC_IT_BIG_VAR + 1),
    ),
    (
      "the disk part",
      big_disk_sql("big_probe", IC_IT_BIG_VAR, IC_IT_BIG_DISK_VAR + 1),
    ),
  ];
  for (part, create) in &over {
    let (went, said) = mysql_try(&format!(
      "DROP TABLE IF EXISTS {db}.big_probe;
       {create};
       SHOW WARNINGS;
       DROP TABLE IF EXISTS {db}.big_probe;",
      db = IC_IT_DB,
      create = create
    ))
    .expect("MySQL");
    assert!(!went, "a byte more in {} was taken", part);
    assert!(
      said.contains("from NDB"),
      "a byte more in {} was refused, but not by the data nodes: {}",
      part,
      said
    );
  }
}

#[test]
fn big_rows_rows_mysql_wrote_are_read_whole() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_big_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  for table in ["big_mem", "big_disk"] {
    let t = bind(&mut conn, table);
    let (_, values) = big_row(&t, &t.rec, 7, 7, usize::MAX);
    let mut columns = String::from("id, code");
    let mut given = String::from("7, 700");
    if t.def.field_id("dn").is_ok() {
      columns.push_str(", dn");
      given.push_str(", 7");
    }
    let names = big_columns(&t);
    let mut i: usize = 0;
    while i < names.len() {
      columns.push_str(&format!(", {}", names[i]));
      given.push_str(&format!(", UNHEX('{}')", hex(&values[i])));
      i += 1;
    }
    mysql(&format!(
      "INSERT INTO {}.{} ({}) VALUES ({})",
      IC_IT_DB, table, columns, given
    ))
    .expect("MySQL");
    let back = read_by_id(&mut conn, &t, 7).expect("the row is there");
    check_big_row(&t, &back, &values, 7);
    let index = bind_unique_index(&mut conn, &format!("{}_code", table), table);
    let key_rec = record_over(&t, &["code"]);
    let mut key = vec![0; key_rec.row_size() as usize];
    put_into(&t, &key_rec, &mut key, "code", "700");
    let mut out = t.row();
    let found = key_op::read_unique_committed(
      &mut conn, &index, &key_rec, &key, &t.rec, &mut out,
    )
    .expect("read through the index");
    assert!(found, "{}: the row is found through the index", table);
    check_big_row(&t, &out, &values, 7);
  }
}

#[test]
fn big_rows_trains_and_whole_requests_share_a_batch() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_big_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "big_mem");
  let mut queries: Vec<QueryId> = Vec::new();
  for _ in 0..3 {
    queries.push(conn.create_query(&t.def, &t.rec, &t.rec).expect("a query"));
  }
  let mut written: Vec<(i64, Vec<Vec<u8>>)> = Vec::new();

  // A train between two requests that go whole, in one transaction:
  // without its VARBINARY a row of big_mem fits one message.
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let rows: [(i64, usize); 3] = [(10, 0), (11, usize::MAX), (12, 100)];
  let mut i: usize = 0;
  while i < rows.len() {
    let (id, var_len) = rows[i];
    let (row, values) = big_row(&t, &t.rec, id, id as u32, var_len);
    define_insert(&mut conn, queries[i], tid, &row);
    written.push((id, values));
    i += 1;
  }
  conn.commit_transaction(tid).expect("ask to commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");

  // Three trains in one send: two in one transaction, one in another.
  let first = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let second = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let ids: [i64; 3] = [20, 21, 22];
  i = 0;
  while i < ids.len() {
    let (row, values) = big_row(&t, &t.rec, ids[i], ids[i] as u32, usize::MAX);
    let tid = if i < 2 { first } else { second };
    define_insert(&mut conn, queries[i], tid, &row);
    written.push((ids[i], values));
    i += 1;
  }
  conn.commit_transaction(first).expect("ask to commit");
  conn.commit_transaction(second).expect("ask to commit");
  assert_eq!(finish(&mut conn, first), CommitState::Committed);
  assert_eq!(finish(&mut conn, second), CommitState::Committed);
  conn.close_transaction(first).expect("close");
  conn.close_transaction(second).expect("close");

  for (id, values) in &written {
    let back = read_by_id(&mut conn, &t, *id).expect("the row is there");
    check_big_row(&t, &back, values, *id as u32);
    check_big_row_in_mysql(&t, "big_mem", *id, values, *id as u32);
  }
  assert_eq!(ids_in_mysql("big_mem"), "10\n11\n12\n20\n21\n22");
}

#[test]
fn big_rows_a_train_rolled_back_before_the_send_sends_nothing() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_big_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "big_mem");
  let mut queries: Vec<QueryId> = Vec::new();
  for _ in 0..3 {
    queries.push(conn.create_query(&t.def, &t.rec, &t.rec).expect("a query"));
  }

  // A train packed and its transaction rolled back before the send;
  // behind it a train and a whole request, committed in that send.
  let dropped = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let (row, _) = big_row(&t, &t.rec, 30, 30, usize::MAX);
  define_insert(&mut conn, queries[0], dropped, &row);
  conn
    .rollback_transaction(dropped)
    .expect("ask to roll back");
  let kept = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let (row, train) = big_row(&t, &t.rec, 31, 31, usize::MAX);
  define_insert(&mut conn, queries[1], kept, &row);
  let (row, whole) = big_row(&t, &t.rec, 32, 32, 0);
  define_insert(&mut conn, queries[2], kept, &row);
  conn.commit_transaction(kept).expect("ask to commit");

  assert_eq!(finish(&mut conn, dropped), CommitState::RolledBack);
  assert_eq!(finish(&mut conn, kept), CommitState::Committed);
  conn.close_transaction(dropped).expect("close");
  conn.close_transaction(kept).expect("close");
  assert!(read_by_id(&mut conn, &t, 30).is_none(), "never sent");
  let back = read_by_id(&mut conn, &t, 31).expect("the train went");
  check_big_row(&t, &back, &train, 31);
  let back = read_by_id(&mut conn, &t, 32).expect("the request went");
  check_big_row(&t, &back, &whole, 32);
  assert_eq!(ids_in_mysql("big_mem"), "31\n32");
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

// ---- scan ----

/// How long a scan may take before the test gives up on it.
const IC_IT_SCAN_WAIT_MS: u64 = 30_000;
/// Rows in the `scan` table: enough for several batches a fragment.
const IC_IT_SCAN_ROWS: i64 = 2_500;

/// Make the `scan` table afresh with `IC_IT_SCAN_ROWS` rows, id 1 up,
/// val ten times the id, name `r` and the id's last digit; 1 000 rows a
/// statement, within what MTR's data nodes take in one transaction.
fn setup_scan_table() -> Option<()> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.scan;
     CREATE TABLE {db}.scan (
       id INT NOT NULL PRIMARY KEY,
       val INT NOT NULL,
       name VARCHAR(32)
     ) ENGINE=NDB;",
    db = IC_IT_DB
  );
  mysql(&sql)?;
  let mut base: i64 = 0;
  while base < IC_IT_SCAN_ROWS {
    let count = (IC_IT_SCAN_ROWS - base).min(1000);
    let sql = format!(
      "INSERT INTO {db}.scan
         WITH RECURSIVE seq (n) AS
           (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < {count})
         SELECT n + {base}, (n + {base}) * 10, CONCAT('r', (n + {base}) % 10)
         FROM seq;",
      db = IC_IT_DB,
      count = count,
      base = base
    );
    mysql(&sql)?;
    base += count;
  }
  Some(())
}

/// Run a scan to its end, reading every row, handing every batch back
/// as soon as it is read. The ids read, in the order read; the batches;
/// and the run's error.
fn scan_ids(
  conn: &mut ApidConnection,
  t: &Table,
  scan: ScanId,
  args: &ScanArgs,
) -> (Vec<i64>, u32, Option<IcError>) {
  conn.start_scan(scan, args).expect("start the scan");
  let _ = conn.send_queries(true);
  let mut ids: Vec<i64> = Vec::new();
  let mut batches: u32 = 0;
  let start = time::gethrtime();
  while !conn.scan_done(scan) {
    conn.poll(100);
    while let Some(batch) = conn.get_next_scan_batch() {
      batches += 1;
      while conn.next_scan_row(batch).expect("read a row") {
        let id = t.get(conn.scan_row(scan), "id");
        ids.push(id.parse().expect("an id"));
      }
      conn.release_scan_batch(batch).expect("hand the batch back");
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(
      waited < IC_IT_SCAN_WAIT_MS,
      "the scan did not end: {:?}",
      conn.scan(scan)
    );
  }
  let error = match conn.scan(scan) {
    Some(scan) => scan.error(),
    None => None,
  };
  (ids, batches, error)
}

/// True if `ids`, in any order, are every id of the `scan` table once.
fn every_row_once(ids: &[i64]) -> bool {
  let mut sorted = ids.to_vec();
  sorted.sort_unstable();
  let mut expect: i64 = 1;
  for id in &sorted {
    if *id != expect {
      return false;
    }
    expect += 1;
  }
  expect == IC_IT_SCAN_ROWS + 1
}

#[test]
fn scan_reads_every_row_in_each_lock_mode() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  for lock in [ScanLock::Committed, ScanLock::Shared, ScanLock::Exclusive] {
    let args = ScanArgs {
      lock,
      ..ScanArgs::default()
    };
    let (ids, batches, error) = scan_ids(&mut conn, &t, scan, &args);
    assert!(error.is_none(), "{:?}: {:?}", lock, error);
    assert!(every_row_once(&ids), "{:?}: {} rows", lock, ids.len());
    println!("{:?}: {} rows in {} batches", lock, ids.len(), batches);
  }
  // Every row came to the scan it was for.
  assert_eq!(conn.unexpected(), 0);
  conn.free_scan(scan).expect("free the scan");
}

#[test]
fn scan_with_small_batches_and_few_receivers() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  for parallelism in [1u32, 2] {
    for lock in [ScanLock::Committed, ScanLock::Exclusive] {
      let args = ScanArgs {
        lock,
        parallelism,
        batch_rows: 100,
        ..ScanArgs::default()
      };
      let (ids, batches, error) = scan_ids(&mut conn, &t, scan, &args);
      assert!(error.is_none(), "{:?}", error);
      assert!(every_row_once(&ids), "{} rows", ids.len());
      // At most 100 rows a batch: at least 25 batches.
      assert!(batches >= 25, "{} batches", batches);
    }
  }
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn scan_closed_early_and_run_again() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  for lock in [ScanLock::Committed, ScanLock::Shared] {
    let args = ScanArgs {
      lock,
      batch_rows: 50,
      ..ScanArgs::default()
    };
    // Read one batch, keep it, and close with the others in flight or
    // held: the run stops at once, the batches in flight dropped.
    conn.start_scan(scan, &args).expect("start");
    let _ = conn.send_queries(true);
    let start = time::gethrtime();
    let mut first: Option<ScanBatchId> = None;
    while first.is_none() {
      conn.poll(100);
      first = conn.get_next_scan_batch();
      let waited = time::millis_elapsed(start, time::gethrtime());
      assert!(waited < IC_IT_SCAN_WAIT_MS, "no batch came");
    }
    let batch = first.expect("a batch");
    assert!(conn.next_scan_row(batch).expect("a row"));
    conn.close_scan(scan).expect("close");
    while !conn.scan_done(scan) {
      conn.poll(100);
      let waited = time::millis_elapsed(start, time::gethrtime());
      assert!(waited < IC_IT_SCAN_WAIT_MS, "the close did not end");
    }
    assert!(conn.scan(scan).expect("the scan").error().is_none());
    // The batch kept is let go of with the run.
    assert!(conn.next_scan_row(batch).is_err());
    conn
      .release_scan_batch(batch)
      .expect("a stale batch is passed over");
    // The record went back to its pool: the next runs read everything.
    let all = ScanArgs {
      lock,
      ..ScanArgs::default()
    };
    let mut run: u32 = 0;
    while run < 3 {
      let (ids, _, error) = scan_ids(&mut conn, &t, scan, &all);
      assert!(error.is_none(), "{:?}", error);
      assert!(every_row_once(&ids), "{} rows", ids.len());
      run += 1;
    }
  }
}

#[test]
fn scan_one_partition_at_a_time() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let fragments = t.def.info().fragment_count;
  assert!(fragments > 1, "{} fragments", fragments);
  let mut all: Vec<i64> = Vec::new();
  let mut partition: u32 = 0;
  while partition < fragments {
    let args = ScanArgs {
      partition: Some(partition),
      ..ScanArgs::default()
    };
    let (ids, _, error) = scan_ids(&mut conn, &t, scan, &args);
    assert!(error.is_none(), "partition {}: {:?}", partition, error);
    assert!(!ids.is_empty(), "partition {} is empty", partition);
    all.extend_from_slice(&ids);
    partition += 1;
  }
  // The partitions together are the table, each row in one of them.
  assert!(every_row_once(&all), "{} rows", all.len());
}

#[test]
fn scan_on_a_transaction_runs_at_its_coordinator() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  for lock in [ScanLock::Shared, ScanLock::Exclusive] {
    let tid = conn
      .start_transaction(TransactionHint::Any, None)
      .expect("start a transaction");
    let args = ScanArgs {
      lock,
      trans: Some(tid),
      ..ScanArgs::default()
    };
    let (ids, _, error) = scan_ids(&mut conn, &t, scan, &args);
    assert!(error.is_none(), "{:?}: {:?}", lock, error);
    assert!(every_row_once(&ids), "{} rows", ids.len());
    conn.commit_transaction(tid).expect("commit");
    assert_eq!(finish(&mut conn, tid), CommitState::Committed);
    conn.close_transaction(tid).expect("close");
  }
  // A committed read's rows cannot be taken over into a transaction.
  let tid = conn
    .start_transaction(TransactionHint::Any, None)
    .expect("start a transaction");
  let refused = ScanArgs {
    trans: Some(tid),
    ..ScanArgs::default()
  };
  let e = conn.start_scan(scan, &refused).expect_err("refused");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  conn.rollback_transaction(tid).expect("rollback");
  finish(&mut conn, tid);
}

#[test]
fn scan_reads_into_a_record_of_its_own() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let narrow = record_over(&t, &["id", "name"]);
  let scan = conn.create_scan(&t.def, &narrow).expect("create the scan");
  conn.start_scan(scan, &ScanArgs::default()).expect("start");
  let _ = conn.send_queries(true);
  let id_at = narrow.position_of(t.def.field_id("id").expect("id"));
  let name_at = narrow.position_of(t.def.field_id("name").expect("name"));
  let (id_at, name_at) = (id_at.expect("id"), name_at.expect("name"));
  let mut rows: i64 = 0;
  let mut row = vec![0u8; narrow.row_size() as usize];
  let start = time::gethrtime();
  while !conn.scan_done(scan) {
    conn.poll(100);
    while let Some(batch) = conn.get_next_scan_batch() {
      // Into a buffer of the caller's.
      while conn.next_scan_row_into(batch, &mut row).expect("a row") {
        let id: i64 = text_row::value_text(&narrow, id_at, &row)
          .parse()
          .expect("an id");
        let name = text_row::value_text(&narrow, name_at, &row);
        // Text as `value_text` gives it: quoted.
        assert_eq!(name, format!("'r{}'", id % 10));
        rows += 1;
      }
      conn.release_scan_batch(batch).expect("hand back");
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the scan did not end");
  }
  assert!(conn.scan(scan).expect("the scan").error().is_none());
  assert_eq!(rows, IC_IT_SCAN_ROWS);
}

#[test]
fn scan_waits_on_a_locked_row_until_the_deadlock_timeout() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  // Another transaction holds row 5 exclusively and does not end.
  let mut key = t.row();
  t.put(&mut key, "id", "5");
  let holder = start_for(&mut conn, &t, &key);
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("query");
  conn
    .query_mut(qid)
    .expect("query")
    .key_row_mut()
    .copy_from_slice(&key);
  let read = ReadKeyArgs {
    kind: ReadKind::Exclusive,
    ..ReadKeyArgs::default()
  };
  conn.read_key(qid, holder, &read).expect("read");
  complete(&mut conn, qid);
  // The exclusive scan meets it, waits, and is ended by the
  // coordinator with an error, the close it owes done.
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let args = ScanArgs {
    lock: ScanLock::Exclusive,
    ..ScanArgs::default()
  };
  let (_, _, error) = scan_ids(&mut conn, &t, scan, &args);
  let e = error.expect("the scan failed");
  assert!(e.code == 274 || e.code == 266, "error {}", e.code);
  assert!(e.is_temporary(), "error {} is temporary", e.code);
  conn.rollback_transaction(holder).expect("rollback");
  finish(&mut conn, holder);
  // With the lock gone, the same scan reads everything.
  let (ids, _, error) = scan_ids(&mut conn, &t, scan, &args);
  assert!(error.is_none(), "{:?}", error);
  assert!(every_row_once(&ids));
}

/// Every id a run of `scan` reads row by row with `next_row`, and how
/// it ended.
fn scan_rows(
  conn: &mut ApidConnection,
  t: &Table,
  scan: ScanId,
  args: &ScanArgs,
) -> (Vec<i64>, Option<IcError>) {
  conn.start_scan(scan, args).expect("start the scan");
  let mut ids: Vec<i64> = Vec::new();
  loop {
    match conn.next_row(scan) {
      Ok(true) => {
        let id = t.get(conn.scan_row(scan), "id");
        ids.push(id.parse().expect("an id"));
      }
      Ok(false) => return (ids, None),
      Err(e) => return (ids, Some(e)),
    }
  }
}

#[test]
fn scan_read_row_by_row_on_a_connection() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  // Not started: nothing to read.
  assert!(!conn.next_row(scan).expect("no run"));
  for lock in [ScanLock::Committed, ScanLock::Shared, ScanLock::Exclusive] {
    let args = ScanArgs {
      lock,
      batch_rows: 100,
      timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
      ..ScanArgs::default()
    };
    let (ids, error) = scan_rows(&mut conn, &t, scan, &args);
    assert!(error.is_none(), "{:?}: {:?}", lock, error);
    assert!(every_row_once(&ids), "{:?}: {} rows", lock, ids.len());
    assert_eq!(
      conn.scan(scan).expect("the scan").rows_read(),
      ids.len() as u64
    );
    // Read to the end, it stays so.
    assert!(!conn.next_row(scan).expect("the end again"));
  }
  // Into a buffer of the caller's.
  conn.start_scan(scan, &ScanArgs::default()).expect("start");
  let mut row = t.row();
  let mut ids: Vec<i64> = Vec::new();
  while conn.next_row_into(scan, &mut row).expect("a row") {
    ids.push(t.get(&row, "id").parse().expect("an id"));
  }
  assert!(every_row_once(&ids), "{} rows", ids.len());
  // A batch at a time, each handed back by the test.
  conn.start_scan(scan, &ScanArgs::default()).expect("start");
  let mut ids: Vec<i64> = Vec::new();
  let mut batches: u32 = 0;
  while let Some(batch) = conn.next_batch(scan).expect("a batch") {
    batches += 1;
    while conn.next_scan_row(batch).expect("a row") {
      ids.push(t.get(conn.scan_row(scan), "id").parse().expect("an id"));
    }
    conn.release_scan_batch(batch).expect("hand back");
  }
  assert!(every_row_once(&ids), "{} rows", ids.len());
  assert!(batches > 1, "{} batches", batches);
  assert_eq!(conn.unexpected(), 0);
}

/// A scan's callback that hands each batch back unread.
fn hand_back_at_once(conn: &mut ApidConnection, batch: ScanBatchId, _: usize) {
  conn.release_scan_batch(batch).expect("hand back");
}

#[test]
fn scan_row_by_row_beside_another_scan() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let first = conn.create_scan(&t.def, &t.rec).expect("the first scan");
  let second = conn.create_scan(&t.def, &t.rec).expect("the second scan");
  let small = ScanArgs {
    batch_rows: 100,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  // The second's batches pile up on the ready list while the first is
  // read row by row, which takes only its own.
  conn.start_scan(second, &small).expect("start the second");
  let (ids, error) = scan_rows(&mut conn, &t, first, &small);
  assert!(error.is_none(), "{:?}", error);
  assert!(every_row_once(&ids), "{} rows", ids.len());
  let mut ids: Vec<i64> = Vec::new();
  let start = time::gethrtime();
  while !conn.scan_done(second) {
    conn.poll(100);
    while let Some(batch) = conn.get_next_scan_batch() {
      assert_eq!(batch.scan(), second, "the first's batches were its own");
      while conn.next_scan_row(batch).expect("a row") {
        ids.push(t.get(conn.scan_row(second), "id").parse().expect("an id"));
      }
      conn.release_scan_batch(batch).expect("hand back");
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the second did not end");
  }
  assert!(every_row_once(&ids), "{} rows", ids.len());
  // A scan whose batches go to a callback is not read row by row.
  let called = ScanArgs {
    callback: Some(hand_back_at_once),
    ..ScanArgs::default()
  };
  conn
    .start_scan(first, &called)
    .expect("start with a callback");
  let e = conn.next_row(first).expect_err("refused");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  let start = time::gethrtime();
  while !conn.scan_done(first) {
    conn.poll(100);
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the callback scan did not end");
  }
  // Read ahead is for executors.
  let ahead = ScanArgs {
    read_ahead: true,
    ..ScanArgs::default()
  };
  let e = conn.start_scan(first, &ahead).expect_err("refused");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  assert_eq!(conn.unexpected(), 0);
}

/// The `scan` table bound through a submitter, and a scan shape over
/// it.
fn scan_through(submitter: &Submitter) -> (Table, Arc<ScanShape>) {
  let def = submitter.table_bind(IC_IT_DB, "scan").expect("bind scan");
  let rec = Record::default_for(&def, None).expect("a record");
  let shape = ScanShape::new(&def, &rec).expect("a scan shape");
  (Table { def, rec }, shape)
}

/// Every id a reader's run reads, and how it ended.
fn reader_ids(
  reader: &mut ScanReader,
  t: &Table,
) -> (Vec<i64>, Option<IcError>) {
  let mut ids: Vec<i64> = Vec::new();
  loop {
    match reader.next_row() {
      Ok(true) => ids.push(t.get(reader.row(), "id").parse().expect("an id")),
      Ok(false) => return (ids, None),
      Err(e) => return (ids, Some(e)),
    }
  }
}

/// A scan through an executor, closed by its deadline if it hangs.
fn bounded(lock: ScanLock, read_ahead: bool) -> ScanArgs {
  ScanArgs {
    lock,
    read_ahead,
    batch_rows: 100,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  }
}

#[test]
fn scan_through_an_executor() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  let mut reader = ScanReader::new(&shape);
  assert!(reader.is_done(), "no run yet");
  assert!(!reader.next_row().expect("no run"));
  let modes = [
    (ScanLock::Committed, false),
    (ScanLock::Committed, true),
    (ScanLock::Shared, false),
    (ScanLock::Exclusive, false),
  ];
  for (lock, read_ahead) in modes {
    // The same reader, run after run.
    let mut run: u32 = 0;
    while run < 2 {
      submitter
        .scan(&mut reader, &bounded(lock, read_ahead))
        .expect("submit the scan");
      let (ids, error) = reader_ids(&mut reader, &t);
      assert!(error.is_none(), "{:?} {}: {:?}", lock, read_ahead, error);
      assert!(every_row_once(&ids), "{:?}: {} rows", lock, ids.len());
      assert_eq!(reader.rows_read(), ids.len() as u64);
      assert!(reader.is_done());
      run += 1;
    }
  }
  // Into a buffer of the caller's.
  submitter
    .scan(&mut reader, &bounded(ScanLock::Committed, false))
    .expect("submit");
  let mut row = t.row();
  let mut ids: Vec<i64> = Vec::new();
  while reader.next_row_into(&mut row).expect("a row") {
    ids.push(t.get(&row, "id").parse().expect("an id"));
  }
  assert!(every_row_once(&ids), "{} rows", ids.len());
  assert_eq!(executor.stats().scans, 9);
}

#[test]
fn scan_through_an_executor_closed_early_dropped_and_refused() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  for lock in [ScanLock::Committed, ScanLock::Exclusive] {
    let mut reader = ScanReader::new(&shape);
    // A few rows, then a close, which waits for the executor.
    submitter
      .scan(&mut reader, &bounded(lock, false))
      .expect("submit");
    let mut read: u32 = 0;
    while read < 10 {
      assert!(reader.next_row().expect("a row"));
      read += 1;
    }
    // Not over: a second run is refused.
    let e = submitter
      .scan(&mut reader, &bounded(lock, false))
      .expect_err("refused");
    assert_eq!(e.code, err::IC_ERROR_SCAN_ACTIVE);
    reader.close();
    assert!(reader.is_done());
    assert!(reader.error().is_none(), "{:?}", reader.error());
    assert!(!reader.next_row().expect("closed"));
    // Then a whole run on the same reader.
    submitter
      .scan(&mut reader, &bounded(lock, false))
      .expect("submit");
    let (ids, error) = reader_ids(&mut reader, &t);
    assert!(error.is_none(), "{:?}", error);
    assert!(every_row_once(&ids), "{} rows", ids.len());
    // A reader dropped in the middle of a run: the executor closes its
    // scan, and the next reader reads everything.
    let mut dropped = ScanReader::new(&shape);
    submitter
      .scan(&mut dropped, &bounded(lock, false))
      .expect("submit");
    assert!(dropped.next_row().expect("a row"));
    drop(dropped);
    let mut next = ScanReader::new(&shape);
    submitter
      .scan(&mut next, &bounded(lock, false))
      .expect("submit");
    let (ids, error) = reader_ids(&mut next, &t);
    assert!(error.is_none(), "{:?}", error);
    assert!(every_row_once(&ids), "{} rows", ids.len());
  }
  // What an executor's scan cannot be.
  let mut reader = ScanReader::new(&shape);
  let ahead = bounded(ScanLock::Shared, true);
  let e = submitter.scan(&mut reader, &ahead).expect_err("refused");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  let called = ScanArgs {
    callback: Some(hand_back_at_once),
    ..ScanArgs::default()
  };
  let e = submitter.scan(&mut reader, &called).expect_err("refused");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  assert!(reader.is_done(), "a refused run never began");
  // A stopped executor refuses, and the reader is free to try again.
  drop(executor);
  let e = submitter
    .scan(&mut reader, &bounded(ScanLock::Committed, false))
    .expect_err("stopped");
  assert_eq!(e.code, err::IC_ERROR_EXECUTOR_STOPPED);
  assert!(reader.is_done());
  assert_eq!(
    reader.next_row().expect_err("the refusal").code,
    err::IC_ERROR_EXECUTOR_STOPPED
  );
}

#[test]
fn scan_a_slow_reader_stalls_only_its_own_scan() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  // Two scans at a time for six readers: four wait their turn.
  let options = ExecutorOptions {
    scans: 2,
    ..ExecutorOptions::default()
  };
  let executor = Executor::start(&cluster.global, &options).expect("executor");
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  // A reader that reads one row and then holds its batch for a while.
  let mut slow = ScanReader::new(&shape);
  submitter
    .scan(&mut slow, &bounded(ScanLock::Committed, false))
    .expect("submit the slow one");
  assert!(slow.next_row().expect("a row"));
  let held = time::gethrtime();
  // Meanwhile other threads' scans run to the end, one at a time on the
  // other place.
  std::thread::scope(|scope| {
    let mut handles = Vec::new();
    let mut k: u32 = 0;
    while k < 5 {
      let mine = executor.submitter();
      let (t, shape) = (&t, &shape);
      handles.push(scope.spawn(move || {
        let mut reader = ScanReader::new(shape);
        mine
          .scan(&mut reader, &bounded(ScanLock::Committed, k % 2 == 1))
          .expect("submit");
        let (ids, error) = reader_ids(&mut reader, t);
        assert!(error.is_none(), "{:?}", error);
        assert!(every_row_once(&ids), "{} rows", ids.len());
      }));
      k += 1;
    }
    for handle in handles {
      handle.join().expect("a reader thread");
    }
  });
  let others = time::millis_elapsed(held, time::gethrtime());
  println!("five scans while one was held: {} ms", others);
  // The slow one goes on where it stopped.
  let (ids, error) = reader_ids(&mut slow, &t);
  assert!(error.is_none(), "{:?}", error);
  assert_eq!(ids.len() as i64, IC_IT_SCAN_ROWS - 1);
  assert_eq!(executor.stats().scans, 6);
}

/// Queries over the `scan` table for takeovers, as many as `count`.
fn takeover_queries(
  conn: &mut ApidConnection,
  t: &Table,
  count: usize,
) -> Vec<QueryId> {
  let mut free: Vec<QueryId> = Vec::with_capacity(count);
  while free.len() < count {
    let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
    free.push(qid);
  }
  free
}

/// Takeovers that have completed, back to the free list, each checked.
fn take_back(conn: &mut ApidConnection, free: &mut Vec<QueryId>) {
  while let Some(done) = conn.get_next_executed_query() {
    let error = conn.query(done).expect("the query").error();
    assert!(error.is_none(), "a takeover failed: {:?}", error);
    free.push(done);
  }
}

/// A count from SQL.
fn count_of(sql: &str) -> i64 {
  match mysql(sql) {
    Some(text) => text.trim().parse().expect("a count"),
    None => -1,
  }
}

#[test]
fn scan_rows_taken_over_are_updated_and_deleted() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  for lock in [ScanLock::Exclusive, ScanLock::Shared] {
    if setup_scan_table().is_none() {
      return;
    }
    let mut conn = cluster.global.create_connection().expect("connection");
    let t = bind(&mut conn, "scan");
    let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
    let mut free = takeover_queries(&mut conn, &t, 600);
    let all = free.len();
    let tid = conn
      .start_transaction(TransactionHint::Any, Some(&t.def))
      .expect("start a transaction");
    let args = ScanArgs {
      lock,
      trans: Some(tid),
      batch_rows: 100,
      timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
      ..ScanArgs::default()
    };
    conn.start_scan(scan, &args).expect("start the scan");
    let mut rows: i64 = 0;
    // Row by row: every tenth row is updated, the one after it deleted.
    // Each batch asks for more only once its takeovers are done.
    while conn.next_row(scan).expect("a row") {
      rows += 1;
      let id: i64 = t.get(conn.scan_row(scan), "id").parse().expect("an id");
      take_back(&mut conn, &mut free);
      if id % 10 == 0 {
        let qid = free.pop().expect("a free query");
        let query = conn.query_mut(qid).expect("the query");
        t.put(query.attr_row_mut(), "id", &id.to_string());
        t.put(query.attr_row_mut(), "val", "-1");
        t.put(query.attr_row_mut(), "name", "taken");
        conn
          .update_scan_row(qid, scan, &TakeoverArgs::default())
          .expect("take the row over to update it");
      } else if id % 10 == 1 {
        let qid = free.pop().expect("a free query");
        conn
          .delete_scan_row(qid, scan, &TakeoverArgs::default())
          .expect("take the row over to delete it");
      }
    }
    assert_eq!(rows, IC_IT_SCAN_ROWS, "{:?}", lock);
    assert!(conn.scan(scan).expect("the scan").error().is_none());
    // The last takeovers, then the commit.
    let _ = conn.send_queries(true);
    let start = time::gethrtime();
    while free.len() < all {
      let _ = conn.flush(100, true);
      take_back(&mut conn, &mut free);
      let waited = time::millis_elapsed(start, time::gethrtime());
      assert!(waited < IC_IT_SCAN_WAIT_MS, "the takeovers did not end");
    }
    conn.commit_transaction(tid).expect("commit");
    assert_eq!(finish(&mut conn, tid), CommitState::Committed);
    conn.close_transaction(tid).expect("close");
    assert_eq!(conn.unexpected(), 0);
    let tenth = IC_IT_SCAN_ROWS / 10;
    let db = IC_IT_DB;
    assert_eq!(
      count_of(&format!("SELECT COUNT(*) FROM {}.scan", db)),
      IC_IT_SCAN_ROWS - tenth,
      "{:?}: a tenth deleted",
      lock
    );
    let updated = format!(
      "SELECT COUNT(*) FROM {}.scan WHERE val = -1 AND name = 'taken'",
      db
    );
    assert_eq!(count_of(&updated), tenth, "{:?}: a tenth updated", lock);
    let kept = format!(
      "SELECT COUNT(*) FROM {}.scan WHERE val = id * 10 AND id % 10 > 1",
      db
    );
    assert_eq!(count_of(&kept), IC_IT_SCAN_ROWS - 2 * tenth);
  }
}

#[test]
fn scan_a_lock_taken_over_is_held_to_commit() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("start a transaction");
  let args = ScanArgs {
    lock: ScanLock::Shared,
    trans: Some(tid),
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  // Before any row is read there is none to take over.
  conn.start_scan(scan, &args).expect("start the scan");
  let e = conn
    .lock_scan_row(qid, scan, &TakeoverArgs::default())
    .expect_err("no row yet");
  assert_eq!(e.code, err::IC_ERROR_NO_SCAN_ROW);
  // Row 5's lock is taken over, its columns read; the scan reads on to
  // the end and lets every other lock go.
  while conn.next_row(scan).expect("a row") {
    if t.get(conn.scan_row(scan), "id") == "5" {
      conn
        .lock_scan_row(qid, scan, &TakeoverArgs::default())
        .expect("take the lock over");
    }
  }
  complete(&mut conn, qid);
  let query = conn.query(qid).expect("the query");
  assert!(query.error().is_none(), "{:?}", query.error());
  assert_eq!(t.get(query.attr_row(), "val"), "50");
  // Another transaction cannot have row 5 exclusively while the lock is
  // the first's, and can once it has committed; row 6 it can at once.
  let reader = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let no_wait = ReadKeyArgs {
    kind: ReadKind::ExclusiveNoWait,
    ..ReadKeyArgs::default()
  };
  for (id, locked) in [("5", true), ("6", false)] {
    let mut key = t.row();
    t.put(&mut key, "id", id);
    let other = start_for(&mut conn, &t, &key);
    conn
      .query_mut(reader)
      .expect("the reader")
      .key_row_mut()
      .copy_from_slice(&key);
    conn
      .read_key(reader, other, &no_wait)
      .expect("define the read");
    complete(&mut conn, reader);
    let error = conn.query(reader).expect("the reader").error();
    if locked {
      assert_eq!(error.map(|e| e.code), Some(635), "row 5 is locked");
    } else {
      assert!(error.is_none(), "row 6 is free: {:?}", error);
    }
    conn
      .rollback_transaction(other)
      .expect("roll the reader back");
    finish(&mut conn, other);
    conn.close_transaction(other).expect("close");
  }
  conn.commit_transaction(tid).expect("commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  let mut key = t.row();
  t.put(&mut key, "id", "5");
  let other = start_for(&mut conn, &t, &key);
  conn
    .query_mut(reader)
    .expect("the reader")
    .key_row_mut()
    .copy_from_slice(&key);
  conn
    .read_key(reader, other, &no_wait)
    .expect("define the read");
  complete(&mut conn, reader);
  let error = conn.query(reader).expect("the reader").error();
  assert!(
    error.is_none(),
    "row 5 is free after the commit: {:?}",
    error
  );
  conn.rollback_transaction(other).expect("roll back");
  finish(&mut conn, other);
}

#[test]
fn scan_takeovers_a_batch_at_a_time_and_across_a_close() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let mut free = takeover_queries(&mut conn, &t, 200);
  let all = free.len();
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("start a transaction");
  let args = ScanArgs {
    lock: ScanLock::Exclusive,
    trans: Some(tid),
    batch_rows: 20,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start the scan");
  let _ = conn.send_queries(true);
  // Every row of the first five batches is deleted, each batch handed
  // back at once, its takeovers sent after; then the scan is closed with
  // takeovers still to be confirmed. The stop waits for them.
  let mut deleted: i64 = 0;
  let mut batches: u32 = 0;
  let start = time::gethrtime();
  while batches < 5 {
    conn.poll(100);
    take_back(&mut conn, &mut free);
    while let Some(batch) = conn.get_next_scan_batch() {
      if batches >= 5 {
        conn.release_scan_batch(batch).expect("hand back");
        continue;
      }
      batches += 1;
      while conn.next_scan_row(batch).expect("a row") {
        let qid = free.pop().expect("a free query");
        conn
          .delete_scan_row(qid, scan, &TakeoverArgs::default())
          .expect("delete the row");
        deleted += 1;
      }
      // Handed back with its takeovers unsent: it asks for more only
      // once they are confirmed.
      conn.release_scan_batch(batch).expect("hand back");
      let e = conn
        .delete_scan_row(free[0], scan, &TakeoverArgs::default())
        .expect_err("the batch is handed back");
      assert_eq!(e.code, err::IC_ERROR_NO_SCAN_ROW);
      let _ = conn.send_queries(true);
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "five batches did not come");
  }
  conn.close_scan(scan).expect("close");
  let start = time::gethrtime();
  while !conn.scan_done(scan) || free.len() < all {
    let _ = conn.flush(100, true);
    take_back(&mut conn, &mut free);
    while let Some(batch) = conn.get_next_scan_batch() {
      conn.release_scan_batch(batch).expect("hand back");
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the close did not end");
  }
  assert!(conn.scan(scan).expect("the scan").error().is_none());
  conn.commit_transaction(tid).expect("commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  assert!(deleted >= 5, "{} rows deleted", deleted);
  assert_eq!(
    count_of(&format!("SELECT COUNT(*) FROM {}.scan", IC_IT_DB)),
    IC_IT_SCAN_ROWS - deleted
  );
  // A scan with no transaction has no rows to take over.
  conn.start_scan(scan, &ScanArgs::default()).expect("start");
  assert!(conn.next_row(scan).expect("a row"));
  let e = conn
    .lock_scan_row(free[0], scan, &TakeoverArgs::default())
    .expect_err("no transaction");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  conn.close_scan(scan).expect("close");
  while !conn.scan_done(scan) {
    conn.poll(100);
  }
  assert_eq!(conn.unexpected(), 0);
}

// ---- Closing at once (chapter 06, phase 10) ----

/// Poll until the scan's run is over, handing back any batch that comes
/// meanwhile.
fn until_done(conn: &mut ApidConnection, scan: ScanId) {
  let start = time::gethrtime();
  while !conn.scan_done(scan) {
    conn.poll(100);
    while let Some(batch) = conn.get_next_scan_batch() {
      conn.release_scan_batch(batch).expect("hand back");
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(
      waited < IC_IT_SCAN_WAIT_MS,
      "the close did not end: {:?}",
      conn.scan(scan)
    );
  }
}

/// Poll until a batch of the scan comes, and give it.
fn first_batch(conn: &mut ApidConnection) -> ScanBatchId {
  let start = time::gethrtime();
  loop {
    conn.poll(100);
    if let Some(batch) = conn.get_next_scan_batch() {
      return batch;
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "no batch came");
  }
}

/// True if another transaction is refused row `id` of the `scan` table
/// exclusively, without waiting: 635, as while a lock on it is held.
/// `reader` is a free query over the table; the executed list is
/// emptied.
fn row_locked(
  conn: &mut ApidConnection,
  t: &Table,
  reader: QueryId,
  id: i64,
) -> bool {
  let mut key = t.row();
  t.put(&mut key, "id", &id.to_string());
  let other = start_for(conn, t, &key);
  conn
    .query_mut(reader)
    .expect("the reader")
    .key_row_mut()
    .copy_from_slice(&key);
  let no_wait = ReadKeyArgs {
    kind: ReadKind::ExclusiveNoWait,
    ..ReadKeyArgs::default()
  };
  conn
    .read_key(reader, other, &no_wait)
    .expect("define the read");
  complete(conn, reader);
  let error = conn.query(reader).expect("the reader").error();
  conn
    .rollback_transaction(other)
    .expect("roll the reader back");
  finish(conn, other);
  conn.close_transaction(other).expect("close");
  match error {
    None => false,
    Some(e) if e.code == 635 => true,
    Some(e) => panic!("reading row {}: {:?}", id, e),
  }
}

#[test]
fn scan_closed_at_once_with_every_batch_on_its_way() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  for lock in [ScanLock::Committed, ScanLock::Shared, ScanLock::Exclusive] {
    let args = ScanArgs {
      lock,
      ..ScanArgs::default()
    };
    // The request goes once the node it runs at has given a record,
    // which a poll may have to wait for: each run goes to the next node
    // in turn. The close follows at once, before any batch is taken, so
    // that every receiver's batch is on its way when the stop is.
    conn.start_scan(scan, &args).expect("start");
    let start = time::gethrtime();
    while conn.scan(scan).expect("the scan").state() == ScanState::Starting {
      conn.poll(10);
      let waited = time::millis_elapsed(start, time::gethrtime());
      assert!(waited < IC_IT_SCAN_WAIT_MS, "no record came");
    }
    assert_eq!(
      conn.scan(scan).expect("the scan").state(),
      ScanState::Running
    );
    conn.close_scan(scan).expect("close");
    let _ = conn.send_queries(true);
    until_done(&mut conn, scan);
    let closed = conn.scan(scan).expect("the scan");
    assert!(closed.error().is_none(), "{:?}: {:?}", lock, closed.error());
    assert_eq!(closed.rows_read(), 0, "{:?}: no row was read", lock);
    // Run again at once: every row once, none of them the closed run's.
    let mut run: u32 = 0;
    while run < 2 {
      let (ids, _, error) = scan_ids(&mut conn, &t, scan, &args);
      assert!(error.is_none(), "{:?}: {:?}", lock, error);
      assert!(every_row_once(&ids), "{:?}: {} rows", lock, ids.len());
      run += 1;
    }
  }
  println!(
    "signals of closed runs dropped as late: {}",
    conn.late_scan_signals()
  );
  assert_eq!(conn.unexpected(), 0);
  conn.free_scan(scan).expect("free the scan");
}

#[test]
fn scan_closed_at_once_through_an_executor() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  let mut reader = ScanReader::new(&shape);
  for lock in [ScanLock::Committed, ScanLock::Exclusive] {
    // Closed as soon as it is submitted, every batch still to come.
    submitter
      .scan(&mut reader, &bounded(lock, false))
      .expect("submit");
    reader.close();
    assert!(reader.is_done());
    assert!(reader.error().is_none(), "{:?}", reader.error());
    assert!(!reader.next_row().expect("closed"));
    // Then whole runs on the same reader.
    let mut run: u32 = 0;
    while run < 2 {
      submitter
        .scan(&mut reader, &bounded(lock, false))
        .expect("submit");
      let (ids, error) = reader_ids(&mut reader, &t);
      assert!(error.is_none(), "{:?}: {:?}", lock, error);
      assert!(every_row_once(&ids), "{:?}: {} rows", lock, ids.len());
      run += 1;
    }
  }
}

#[test]
fn scan_locking_closed_at_once_leaves_no_row_locked() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let reader = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  for lock in [ScanLock::Shared, ScanLock::Exclusive] {
    let args = ScanArgs {
      lock,
      batch_rows: 50,
      timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
      ..ScanArgs::default()
    };
    conn.start_scan(scan, &args).expect("start");
    let _ = conn.send_queries(true);
    // A batch held, its rows locked, the other batches on their way.
    let batch = first_batch(&mut conn);
    let mut held: Vec<i64> = Vec::new();
    while conn.next_scan_row(batch).expect("a row") {
      let id = t.get(conn.scan_row(scan), "id");
      held.push(id.parse().expect("an id"));
    }
    let first = held[0];
    let last = held[held.len() - 1];
    assert!(row_locked(&mut conn, &t, reader, first), "{:?}: held", lock);
    conn.close_scan(scan).expect("close");
    until_done(&mut conn, scan);
    let closed = conn.scan(scan).expect("the scan");
    assert!(closed.error().is_none(), "{:?}: {:?}", lock, closed.error());
    // The close let go of every lock the run held, at once.
    for id in [first, last] {
      assert!(
        !row_locked(&mut conn, &t, reader, id),
        "{:?}: row {} is free after the close",
        lock,
        id
      );
    }
  }
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn scan_closed_with_takeovers_pending_waits_for_them() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let reader = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("start a transaction");
  let args = ScanArgs {
    lock: ScanLock::Exclusive,
    trans: Some(tid),
    batch_rows: 20,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start the scan");
  let _ = conn.send_queries(true);
  // The batch's first row taken over to be deleted, the rest left to
  // the scan; the takeover sent, and the scan closed at once after, its
  // reply perhaps still to come: the stop waits for it.
  let batch = first_batch(&mut conn);
  assert!(conn.next_scan_row(batch).expect("a row"));
  let taken: i64 = t.get(conn.scan_row(scan), "id").parse().expect("an id");
  conn
    .delete_scan_row(qid, scan, &TakeoverArgs::default())
    .expect("take the row over");
  let mut left: Vec<i64> = Vec::new();
  while conn.next_scan_row(batch).expect("a row") {
    left.push(t.get(conn.scan_row(scan), "id").parse().expect("an id"));
  }
  assert!(!left.is_empty(), "a batch of more than one row");
  conn.release_scan_batch(batch).expect("hand back");
  let _ = conn.send_queries(true);
  conn.close_scan(scan).expect("close");
  let start = time::gethrtime();
  let mut done = false;
  while !conn.scan_done(scan) || !done {
    let _ = conn.flush(100, true);
    while let Some(executed) = conn.get_next_executed_query() {
      if executed == qid {
        done = true;
      }
    }
    while let Some(late) = conn.get_next_scan_batch() {
      conn.release_scan_batch(late).expect("hand back");
    }
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the close did not end");
  }
  let query = conn.query(qid).expect("the takeover");
  assert!(query.error().is_none(), "{:?}", query.error());
  assert!(conn.scan(scan).expect("the scan").error().is_none());
  // The rows the scan held and did not take over are free before the
  // commit; the one taken over goes with it.
  for id in [left[0], left[left.len() - 1]] {
    assert!(!row_locked(&mut conn, &t, reader, id), "row {} is free", id);
  }
  conn.commit_transaction(tid).expect("commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  assert_eq!(
    count_of(&format!("SELECT COUNT(*) FROM {}.scan", IC_IT_DB)),
    IC_IT_SCAN_ROWS - 1
  );
  assert_eq!(
    count_of(&format!(
      "SELECT COUNT(*) FROM {}.scan WHERE id = {}",
      IC_IT_DB, taken
    )),
    0
  );
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn scan_closed_on_a_transaction_rolling_back_waits_for_nothing() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let reader = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let mut free = takeover_queries(&mut conn, &t, 40);
  let all = free.len();
  // Closed, then rolled back; and rolled back, then closed.
  for close_first in [true, false] {
    let tid = conn
      .start_transaction(TransactionHint::Any, Some(&t.def))
      .expect("start a transaction");
    let args = ScanArgs {
      lock: ScanLock::Exclusive,
      trans: Some(tid),
      batch_rows: 20,
      timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
      ..ScanArgs::default()
    };
    conn.start_scan(scan, &args).expect("start the scan");
    let _ = conn.send_queries(true);
    // Every row of the first batch taken over to be deleted, the
    // takeovers sent, their replies still to come.
    let batch = first_batch(&mut conn);
    let mut deleted: Vec<i64> = Vec::new();
    while conn.next_scan_row(batch).expect("a row") {
      deleted.push(t.get(conn.scan_row(scan), "id").parse().expect("an id"));
      let q = free.pop().expect("a free query");
      conn
        .delete_scan_row(q, scan, &TakeoverArgs::default())
        .expect("take the row over");
    }
    conn.release_scan_batch(batch).expect("hand back");
    let _ = conn.send_queries(true);
    if close_first {
      // The transaction goes on: the stop waits for the takeovers, and
      // the rollback lets it go.
      conn.close_scan(scan).expect("close");
      conn.rollback_transaction(tid).expect("roll back");
    } else {
      conn.rollback_transaction(tid).expect("roll back");
      conn.close_scan(scan).expect("close");
    }
    let start = time::gethrtime();
    loop {
      let _ = conn.flush(100, true);
      // The takeovers end with the rollback; their errors are its.
      while let Some(executed) = conn.get_next_executed_query() {
        free.push(executed);
      }
      while let Some(late) = conn.get_next_scan_batch() {
        conn.release_scan_batch(late).expect("hand back");
      }
      let trans_done =
        conn.transaction(tid).expect("the transaction").is_done();
      if conn.scan_done(scan) && trans_done && free.len() == all {
        break;
      }
      let waited = time::millis_elapsed(start, time::gethrtime());
      assert!(waited < IC_IT_SCAN_WAIT_MS, "the close did not end");
    }
    let ended = conn
      .transaction(tid)
      .expect("the transaction")
      .commit_state();
    assert_eq!(
      ended,
      CommitState::RolledBack,
      "close first: {}",
      close_first
    );
    conn.close_transaction(tid).expect("close");
    // Nothing was deleted, and no row the scan or its takeovers held is
    // locked still.
    assert_eq!(
      count_of(&format!("SELECT COUNT(*) FROM {}.scan", IC_IT_DB)),
      IC_IT_SCAN_ROWS,
      "close first: {}",
      close_first
    );
    for id in [deleted[0], deleted[deleted.len() - 1]] {
      assert!(
        !row_locked(&mut conn, &t, reader, id),
        "close first {}: row {} is free",
        close_first,
        id
      );
    }
  }
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn scan_closed_by_its_deadline_or_a_stopping_executor() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  // A batch held past the run's deadline, the others on their way: the
  // run is closed at once with IC_ERROR_TIMEOUT.
  let args = ScanArgs {
    batch_rows: 50,
    timeout_ms: 300,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start");
  let _ = conn.send_queries(true);
  let batch = first_batch(&mut conn);
  let start = time::gethrtime();
  while !conn.scan_done(scan) {
    conn.poll(100);
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the deadline did not close it");
  }
  let error = conn.scan(scan).expect("the scan").error();
  assert_eq!(error.map(|e| e.code), Some(err::IC_ERROR_TIMEOUT));
  conn
    .release_scan_batch(batch)
    .expect("a stale batch is passed over");
  let (ids, _, error) = scan_ids(&mut conn, &t, scan, &ScanArgs::default());
  assert!(error.is_none(), "{:?}", error);
  assert!(every_row_once(&ids), "{} rows", ids.len());
  assert_eq!(conn.unexpected(), 0);
  // An executor stopped in the middle of a run closes it, and its
  // reader learns why once the rows it has are read.
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  let mut reader = ScanReader::new(&shape);
  submitter
    .scan(&mut reader, &bounded(ScanLock::Committed, false))
    .expect("submit");
  assert!(reader.next_row().expect("a row"));
  drop(executor);
  let (ids, error) = reader_ids(&mut reader, &t);
  assert!(ids.len() < IC_IT_SCAN_ROWS as usize, "{} rows", ids.len());
  assert_eq!(error.map(|e| e.code), Some(err::IC_ERROR_EXECUTOR_STOPPED));
  assert!(reader.is_done());
}

// ---- Index scans (chapter 06 §8.7) ----

/// Rows in the `rng` table.
const IC_IT_RNG_ROWS: i64 = 999;

/// Make the `rng` table afresh, `IC_IT_RNG_ROWS` rows: id 1 up, a the id
/// modulo 50, b the id modulo 7 or NULL for every eleventh, u the id and
/// v ten times the id; an ordered index over (a, b), and a unique one
/// over u.
fn setup_rng_table() -> Option<()> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.rng;
     CREATE TABLE {db}.rng (
       id INT NOT NULL PRIMARY KEY,
       a INT NOT NULL,
       b INT,
       u INT NOT NULL,
       v INT NOT NULL,
       KEY ix_ab (a, b),
       UNIQUE KEY uk_u (u)
     ) ENGINE=NDB;
     INSERT INTO {db}.rng
       WITH RECURSIVE seq (n) AS
         (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < {rows})
       SELECT n, n % 50, IF(n % 11 = 0, NULL, n % 7), n, n * 10 FROM seq;",
    db = IC_IT_DB,
    rows = IC_IT_RNG_ROWS
  );
  mysql(&sql)?;
  Some(())
}

/// A row of `key_rec` holding `values` in `columns`, in order; None is
/// NULL.
fn bound_row(
  t: &Table,
  key_rec: &Record,
  columns: &[&str],
  values: &[Option<&str>],
) -> Vec<u8> {
  let mut row = vec![0; key_rec.row_size() as usize];
  let mut i: usize = 0;
  while i < values.len() {
    match values[i] {
      Some(text) => put_into(t, key_rec, &mut row, columns[i], text),
      None => {
        let id = t.def.field_id(columns[i]).expect("no such column");
        let position = key_rec.position_of(id).expect("not in the record");
        key_rec
          .set_null(&mut row, position, true)
          .expect("set NULL");
      }
    }
    i += 1;
  }
  row
}

/// Every row a run of a scan reads with `next_row`, each one's id and
/// range number, and how the run ended.
fn index_rows(
  conn: &mut ApidConnection,
  t: &Table,
  scan: ScanId,
  args: &ScanArgs,
) -> (Vec<(i64, u32)>, Option<IcError>) {
  conn.start_scan(scan, args).expect("start the scan");
  let mut rows: Vec<(i64, u32)> = Vec::new();
  loop {
    match conn.next_row(scan) {
      Ok(true) => {
        let id: i64 = t.get(conn.scan_row(scan), "id").parse().expect("id");
        rows.push((id, conn.scan_range_no(scan)));
      }
      Ok(false) => return (rows, None),
      Err(e) => return (rows, Some(e)),
    }
  }
}

/// By id, the rows of `table` that `filter` selects, each with its
/// dense rank in MySQL's `ORDER BY order`, or with the number `rank`
/// gives it when that is a whole expression of its own.
fn mysql_ranks(table: &str, rank: &str, filter: &str) -> HashMap<i64, i64> {
  let sql = format!(
    "SELECT id, {rank} FROM {db}.{table} WHERE {filter}",
    rank = rank,
    db = IC_IT_DB,
    table = table,
    filter = filter
  );
  let text = mysql(&sql).expect("MySQL");
  let mut ranks: HashMap<i64, i64> = HashMap::new();
  for line in text.lines() {
    let mut parts = line.split('\t');
    let id: i64 = parts.next().expect("an id").parse().expect("an id");
    let value: i64 = parts.next().expect("a rank").parse().expect("a rank");
    ranks.insert(id, value);
  }
  ranks
}

/// The rank of each row in MySQL's order of `order`.
fn rank_by(order: &str) -> String {
  format!("DENSE_RANK() OVER (ORDER BY {})", order)
}

/// What is wrong with `rows`, read in this order, against `ranks`: a row
/// MySQL does not select, one read twice, rows not read; and for an
/// ordered run a row out of order, range by range, each range's rows by
/// rank, rising, or falling for a descending run.
fn order_faults(
  rows: &[(i64, u32)],
  ranks: &HashMap<i64, i64>,
  ordered: bool,
  descending: bool,
) -> Vec<String> {
  let mut faults: Vec<String> = Vec::new();
  let mut seen: HashMap<i64, bool> = HashMap::new();
  let mut last: Option<(u32, i64)> = None;
  for (id, range_no) in rows {
    let rank = match ranks.get(id) {
      Some(rank) => *rank,
      None => {
        faults.push(format!("{} is not selected", id));
        continue;
      }
    };
    if seen.insert(*id, true).is_some() {
      faults.push(format!("{} read twice", id));
    }
    if !ordered {
      continue;
    }
    if let Some((last_range, last_rank)) = last {
      let in_order = if *range_no != last_range {
        *range_no > last_range
      } else if descending {
        rank <= last_rank
      } else {
        rank >= last_rank
      };
      if !in_order {
        faults.push(format!(
          "{} (range {}, rank {}) after range {}, rank {}",
          id, range_no, rank, last_range, last_rank
        ));
      }
    }
    last = Some((*range_no, rank));
  }
  if seen.len() != ranks.len() {
    faults.push(format!("{} rows read of {}", seen.len(), ranks.len()));
  }
  faults
}

/// The first faults, for a message.
fn first_faults(mut faults: Vec<String>) -> String {
  faults.truncate(40);
  faults.join("\n")
}

/// A range of `ix_ab` with no number.
const fn ab(
  low_columns: u32,
  low_inclusive: bool,
  high_columns: u32,
  high_inclusive: bool,
) -> ScanRange {
  ScanRange {
    low_columns,
    low_inclusive,
    high_columns,
    high_inclusive,
    range_no: 0,
  }
}

/// A range of `ix_ab`, its bounds as text, with the condition MySQL
/// selects its rows by.
struct AbRange {
  range: ScanRange,
  low: &'static [Option<&'static str>],
  high: &'static [Option<&'static str>],
  filter: &'static str,
}

/// Every bound type, on one column and on two, open on either side,
/// bounds at NULL, and a range of no rows.
const IC_IT_AB_RANGES: [AbRange; 12] = [
  AbRange {
    range: ab(0, false, 0, false),
    low: &[],
    high: &[],
    filter: "TRUE",
  },
  AbRange {
    range: ab(1, true, 1, true),
    low: &[Some("7")],
    high: &[Some("7")],
    filter: "a = 7",
  },
  AbRange {
    range: ab(1, true, 1, false),
    low: &[Some("10")],
    high: &[Some("20")],
    filter: "a >= 10 AND a < 20",
  },
  AbRange {
    range: ab(1, false, 1, true),
    low: &[Some("10")],
    high: &[Some("20")],
    filter: "a > 10 AND a <= 20",
  },
  AbRange {
    range: ab(1, true, 0, false),
    low: &[Some("45")],
    high: &[],
    filter: "a >= 45",
  },
  AbRange {
    range: ab(0, false, 1, false),
    low: &[],
    high: &[Some("3")],
    filter: "a < 3",
  },
  AbRange {
    range: ab(2, true, 1, true),
    low: &[Some("5"), Some("3")],
    high: &[Some("5")],
    filter: "a = 5 AND b >= 3",
  },
  AbRange {
    range: ab(2, false, 2, false),
    low: &[Some("5"), Some("3")],
    high: &[Some("6"), Some("2")],
    filter: "(a = 5 AND b > 3) OR (a = 6 AND (b < 2 OR b IS NULL))",
  },
  AbRange {
    range: ab(2, true, 2, true),
    low: &[Some("9"), None],
    high: &[Some("9"), None],
    filter: "a = 9 AND b IS NULL",
  },
  AbRange {
    range: ab(2, true, 2, true),
    low: &[Some("9"), None],
    high: &[Some("9"), Some("2")],
    filter: "a = 9 AND (b IS NULL OR b <= 2)",
  },
  AbRange {
    range: ab(2, false, 1, true),
    low: &[Some("9"), None],
    high: &[Some("9")],
    filter: "a = 9 AND b IS NOT NULL",
  },
  AbRange {
    range: ab(1, false, 1, false),
    low: &[Some("20")],
    high: &[Some("20")],
    filter: "FALSE",
  },
];

/// The `ix_ab` index of `rng`, its key record and a scan of it reading
/// the whole row.
fn ab_scan(
  conn: &mut ApidConnection,
  t: &Table,
) -> (Arc<IndexDef>, Record, ScanId) {
  let index = conn
    .index_bind(IC_IT_DB, "ix_ab", "rng")
    .expect("bind the ordered index");
  assert!(index.is_ordered());
  let key_rec = record_over(t, &["a", "b"]);
  let scan = conn
    .create_index_scan(&index, &key_rec, &t.rec)
    .expect("create the index scan");
  (index, key_rec, scan)
}

#[test]
fn scan_index_ranges_of_every_bound_type() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_rng_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "rng");
  let (_, key_rec, scan) = ab_scan(&mut conn, &t);
  let columns = ["a", "b"];
  let mut faults: Vec<String> = Vec::new();
  for case in &IC_IT_AB_RANGES {
    let ranks = mysql_ranks("rng", &rank_by("a, b"), case.filter);
    let low = bound_row(&t, &key_rec, &columns, case.low);
    let high = bound_row(&t, &key_rec, &columns, case.high);
    // Unordered, ordered and ordered from the high end, in small
    // batches, so that the merge waits for one fragment after another.
    for (ordered, descending) in [(false, false), (true, false), (true, true)] {
      conn
        .add_scan_range(scan, &case.range, &low, &high)
        .expect("add the range");
      let args = ScanArgs {
        ordered,
        descending,
        batch_rows: 5,
        timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
        ..ScanArgs::default()
      };
      let (rows, error) = index_rows(&mut conn, &t, scan, &args);
      let what =
        format!("{} ordered {} desc {}", case.filter, ordered, descending);
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, ordered, descending) {
        faults.push(format!("{}: {}", what, fault));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
  // A run given no range reads every row; ordered into a buffer of the
  // caller's.
  let args = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start");
  let mut row = t.row();
  let mut rows: Vec<(i64, u32)> = Vec::new();
  while conn.next_row_into(scan, &mut row).expect("a row") {
    rows.push((t.get(&row, "id").parse().expect("an id"), 0));
  }
  let ranks = mysql_ranks("rng", &rank_by("a, b"), "TRUE");
  let faults = order_faults(&rows, &ranks, true, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(conn.unexpected(), 0);
  conn.free_scan(scan).expect("free the scan");
}

#[test]
fn scan_index_several_ranges_with_their_numbers() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_rng_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "rng");
  let (_, key_rec, scan) = ab_scan(&mut conn, &t);
  let columns = ["a", "b"];
  // Range 0: a = 3; range 1: a from 10 to 12; range 5: a above 47.
  let ranges: [(u32, ScanRange, &str, &str); 3] = [
    (0, ab(1, true, 1, true), "3", "3"),
    (1, ab(1, true, 1, true), "10", "12"),
    (5, ab(1, false, 0, false), "47", "0"),
  ];
  let filter = "a = 3 OR a BETWEEN 10 AND 12 OR a > 47";
  let numbers = mysql_ranks(
    "rng",
    "CASE WHEN a = 3 THEN 0 WHEN a <= 12 THEN 1 ELSE 5 END",
    filter,
  );
  let ranks = mysql_ranks("rng", &rank_by("a, b"), filter);
  let mut faults: Vec<String> = Vec::new();
  for (ordered, descending) in [(false, false), (true, false), (true, true)] {
    for (range_no, range, low, high) in &ranges {
      let numbered = ScanRange {
        range_no: *range_no,
        ..*range
      };
      let low_row = bound_row(&t, &key_rec, &columns, &[Some(*low)]);
      let high_row = bound_row(&t, &key_rec, &columns, &[Some(*high)]);
      conn
        .add_scan_range(scan, &numbered, &low_row, &high_row)
        .expect("add a range");
    }
    let args = ScanArgs {
      ordered,
      descending,
      batch_rows: 3,
      timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
      ..ScanArgs::default()
    };
    let (rows, error) = index_rows(&mut conn, &t, scan, &args);
    let what = format!("ordered {} desc {}", ordered, descending);
    if let Some(e) = error {
      faults.push(format!("{}: error {} {}", what, e.code, e.message()));
      continue;
    }
    for fault in order_faults(&rows, &ranks, ordered, descending) {
      faults.push(format!("{}: {}", what, fault));
    }
    // Each row came with the number of the range it is in.
    for (id, range_no) in &rows {
      let expected = numbers.get(id).copied().unwrap_or(-1);
      if expected != *range_no as i64 {
        faults.push(format!(
          "{}: row {} in range {}, not {}",
          what, id, range_no, expected
        ));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
  // An ordered run's numbers must rise in the order the ranges came;
  // the refused run leaves them for the next, an unordered one.
  let low = bound_row(&t, &key_rec, &columns, &[Some("4")]);
  for range_no in [4, 2] {
    let range = ScanRange {
      range_no,
      ..ab(1, true, 1, true)
    };
    conn.add_scan_range(scan, &range, &low, &low).expect("add");
  }
  let ordered = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  let e = conn.start_scan(scan, &ordered).expect_err("not rising");
  assert_eq!(e.code, err::IC_ERROR_BAD_RANGE);
  let (rows, error) = index_rows(&mut conn, &t, scan, &ScanArgs::default());
  assert!(error.is_none(), "{:?}", error);
  assert_eq!(
    rows.len() as i64,
    count_of(&format!(
      "SELECT COUNT(*) FROM {}.rng WHERE a = 4",
      IC_IT_DB
    )) * 2,
    "the rows of a = 4, once for each range"
  );
  // A number above 4095, or more columns than the index has.
  let too_big = ScanRange {
    range_no: 4096,
    ..ab(1, true, 1, true)
  };
  let e = conn
    .add_scan_range(scan, &too_big, &low, &low)
    .expect_err("number too big");
  assert_eq!(e.code, err::IC_ERROR_BAD_RANGE);
  let e = conn
    .add_scan_range(scan, &ab(3, true, 0, false), &low, &[])
    .expect_err("too many columns");
  assert_eq!(e.code, err::IC_ERROR_BAD_RANGE);
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn scan_index_pruned_to_one_partition() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_rng_table().is_none() {
    return;
  }
  let sql = format!(
    "DROP TABLE IF EXISTS {db}.rngd;
     CREATE TABLE {db}.rngd (
       k INT NOT NULL,
       id INT NOT NULL,
       v INT NOT NULL,
       PRIMARY KEY (k, id),
       KEY ix_kv (k, v)
     ) ENGINE=NDB PARTITION BY KEY (k);
     INSERT INTO {db}.rngd
       WITH RECURSIVE seq (n) AS
         (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 800)
       SELECT n % 40, n, n % 13 FROM seq;",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "rngd");
  let fragments = t.def.info().fragment_count;
  let index = conn
    .index_bind(IC_IT_DB, "ix_kv", "rngd")
    .expect("bind the ordered index");
  let columns = ["k", "v"];
  let key_rec = record_over(&t, &columns);
  let scan = conn
    .create_index_scan(&index, &key_rec, &t.rec)
    .expect("create the index scan");
  // Each case: its ranges as (k, v from, v to) with None for no bound
  // on v, the condition, and whether the run is pruned.
  struct Case {
    ranges: &'static [(&'static str, Option<(&'static str, &'static str)>)],
    filter: &'static str,
    pruned: bool,
  }
  let cases = [
    Case {
      ranges: &[("5", None)],
      filter: "k = 5",
      pruned: true,
    },
    Case {
      ranges: &[("5", Some(("2", "6")))],
      filter: "k = 5 AND v BETWEEN 2 AND 6",
      pruned: true,
    },
    Case {
      ranges: &[("7", Some(("0", "2"))), ("7", Some(("10", "12")))],
      filter: "k = 7 AND (v <= 2 OR v >= 10)",
      pruned: true,
    },
  ];
  let mut faults: Vec<String> = Vec::new();
  for case in &cases {
    let ranks = mysql_ranks("rngd", &rank_by("k, v"), case.filter);
    for ordered in [false, true] {
      // Each range numbered by its place.
      let mut range_no: u32 = 0;
      while (range_no as usize) < case.ranges.len() {
        let (k, v) = case.ranges[range_no as usize];
        let (range, low, high) = match v {
          Some((from, to)) => (
            ab(2, true, 2, true),
            bound_row(&t, &key_rec, &columns, &[Some(k), Some(from)]),
            bound_row(&t, &key_rec, &columns, &[Some(k), Some(to)]),
          ),
          None => {
            let row = bound_row(&t, &key_rec, &columns, &[Some(k)]);
            (ab(1, true, 1, true), row.clone(), row)
          }
        };
        let numbered = ScanRange { range_no, ..range };
        conn
          .add_scan_range(scan, &numbered, &low, &high)
          .expect("add a range");
        range_no += 1;
      }
      let args = ScanArgs {
        ordered,
        batch_rows: 4,
        timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
        ..ScanArgs::default()
      };
      let (rows, error) = index_rows(&mut conn, &t, scan, &args);
      let what = format!("{} ordered {}", case.filter, ordered);
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, ordered, false) {
        faults.push(format!("{}: {}", what, fault));
      }
      let parallelism = conn.scan(scan).expect("the scan").parallelism();
      let expected = if case.pruned { 1 } else { fragments };
      if parallelism != expected {
        faults.push(format!("{}: {} receivers", what, parallelism));
      }
    }
  }
  // k from 5 to 6 fixes no partition: every fragment is scanned.
  let low = bound_row(&t, &key_rec, &columns, &[Some("5")]);
  let high = bound_row(&t, &key_rec, &columns, &[Some("6")]);
  conn
    .add_scan_range(scan, &ab(1, true, 1, true), &low, &high)
    .expect("add a range");
  let (rows, error) = index_rows(&mut conn, &t, scan, &ScanArgs::default());
  assert!(error.is_none(), "{:?}", error);
  let ranks = mysql_ranks("rngd", &rank_by("k, v"), "k BETWEEN 5 AND 6");
  for fault in order_faults(&rows, &ranks, false, false) {
    faults.push(format!("k from 5 to 6: {}", fault));
  }
  let parallelism = conn.scan(scan).expect("the scan").parallelism();
  assert!(fragments < 2 || parallelism == fragments, "{}", parallelism);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  conn.free_scan(scan).expect("free the scan");
  // The table's own ordered index: each id alone finds its row, wherever
  // the hash places it.
  let r = bind(&mut conn, "rng");
  let primary = conn
    .index_bind(IC_IT_DB, "PRIMARY", "rng")
    .expect("bind the primary key's ordered index");
  let id_rec = record_over(&r, &["id"]);
  let scan = conn
    .create_index_scan(&primary, &id_rec, &r.rec)
    .expect("create the index scan");
  let mut id: i64 = 1;
  while id <= 40 {
    let text = id.to_string();
    let row = bound_row(&r, &id_rec, &["id"], &[Some(text.as_str())]);
    conn
      .add_scan_range(scan, &ab(1, true, 1, true), &row, &row)
      .expect("add a range");
    let (rows, error) = index_rows(&mut conn, &r, scan, &ScanArgs::default());
    assert!(error.is_none(), "id {}: {:?}", id, error);
    assert_eq!(rows, vec![(id, 0)], "id {}", id);
    assert_eq!(conn.scan(scan).expect("the scan").parallelism(), 1);
    id += 1;
  }
  assert_eq!(conn.unexpected(), 0);
}

/// Check an ordered scan of each index `ix_<column>` of `table`, its key
/// record over the column, both ways, against MySQL's `ORDER BY` of the
/// column, a continuous scan if `continuous`. The faults found, by
/// column.
fn ordered_faults(
  conn: &mut ApidConnection,
  t: &Table,
  table: &str,
  columns: &[&str],
  continuous: bool,
) -> Vec<String> {
  let mut faults: Vec<String> = Vec::new();
  for column in columns {
    let name = format!("ix_{}", column);
    let index = match conn.index_bind(IC_IT_DB, &name, table) {
      Ok(index) => index,
      Err(e) => {
        faults.push(format!("{}: no index: {}", column, e.message()));
        continue;
      }
    };
    let key_rec = record_over(t, &[*column]);
    let scan = conn
      .create_index_scan(&index, &key_rec, &t.rec)
      .expect("create the index scan");
    let ranks = mysql_ranks(table, &rank_by(column), "TRUE");
    for descending in [false, true] {
      let args = ScanArgs {
        ordered: true,
        descending,
        continuous,
        batch_rows: 3,
        timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
        ..ScanArgs::default()
      };
      let (rows, error) = index_rows(conn, t, scan, &args);
      let what = format!("{} desc {}", column, descending);
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, true, descending) {
        faults.push(format!("{}: {}", what, fault));
      }
    }
    conn.free_scan(scan).expect("free the scan");
  }
  faults
}

#[test]
fn scan_index_ordered_by_every_type() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let columns = [
    "ti", "su", "mi", "i", "bi", "bu", "f", "d", "de", "bn", "vb", "dt", "tm",
    "dtm", "ts", "yr",
  ];
  let mut keys = String::new();
  for column in &columns {
    keys.push_str(&format!(",\n KEY ix_{c} ({c})", c = column));
  }
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ordt;
     CREATE TABLE {db}.ordt (
       id INT NOT NULL PRIMARY KEY,
       ti TINYINT, su SMALLINT UNSIGNED, mi MEDIUMINT, i INT,
       bi BIGINT, bu BIGINT UNSIGNED, f FLOAT, d DOUBLE,
       de DECIMAL(12,3), bn BINARY(4), vb VARBINARY(8),
       dt DATE, tm TIME(2), dtm DATETIME(3), ts TIMESTAMP(1) NULL,
       yr YEAR{keys}
     ) ENGINE=NDB;
     INSERT INTO {db}.ordt
       WITH RECURSIVE seq (n) AS
         (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 600)
       SELECT n,
         IF(n % 13 = 0, NULL, n * 37 % 255 - 127),
         IF(n % 13 = 1, NULL, n * 7919 % 65536),
         IF(n % 13 = 2, NULL, n * 7919 % 16000001 - 8000000),
         IF(n % 13 = 3, NULL, n * 2654435761 % 4294967296 - 2147483648),
         IF(n % 13 = 4, NULL,
            (n * 2654435761 % 4294967296 - 2147483648) * 2147483647),
         IF(n % 13 = 5, NULL,
            CAST(n * 2654435761 % 4294967296 AS UNSIGNED) * 4294967291),
         IF(n % 13 = 6, NULL, (n * 7919 % 2001 - 1000) / 7),
         IF(n % 13 = 7, NULL, POW(10, n % 41 - 20) * (n % 7 - 3)),
         IF(n % 13 = 8, NULL, (n * 7919 % 2000001 - 1000000) * 0.997),
         IF(n % 13 = 9, NULL,
            UNHEX(LPAD(HEX(n * 2654435761 % 4294967296), 8, '0'))),
         IF(n % 13 = 10, NULL,
            LEFT(UNHEX(LPAD(HEX(n * 40503 % 65536 * 65536 + n % 3), 8,
                 '0')), n % 5)),
         IF(n % 13 = 11, NULL,
            DATE_ADD('1950-01-01', INTERVAL n * 7919 % 30000 DAY)),
         IF(n % 13 = 12, NULL,
            SEC_TO_TIME(n * 7919 % 200000 - 100000 + n % 100 / 100)),
         IF(n % 17 = 0, NULL,
            TIMESTAMPADD(MICROSECOND, n % 1000 * 1000,
              DATE_ADD('1990-01-01 00:00:00',
                       INTERVAL n * 104729 % 900000000 SECOND))),
         IF(n % 17 = 1, NULL,
            FROM_UNIXTIME(100000000 + n * 7919 % 1000000000 + n % 10 / 10)),
         IF(n % 17 = 2, NULL, 1901 + n * 37 % 255)
       FROM seq;",
    db = IC_IT_DB,
    keys = keys
  );
  if mysql(&sql).is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "ordt");
  let faults = ordered_faults(&mut conn, &t, "ordt", &columns, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(conn.unexpected(), 0);
}

/// Insert rows of `table` whose every column but the id holds the same
/// string, given in hexadecimal and put through `wrap` (`{}` for the
/// literal), ids from `first` up; and one row of NULLs after them.
fn insert_strings(
  table: &str,
  columns: usize,
  first: i64,
  hex: &[String],
  wrap: &str,
) -> Option<()> {
  let mut values: Vec<String> = Vec::new();
  let mut id = first;
  for bytes in hex {
    let literal = wrap.replace("{}", &format!("X'{}'", bytes));
    let mut row = id.to_string();
    let mut i: usize = 0;
    while i < columns {
      row.push_str(", ");
      row.push_str(&literal);
      i += 1;
    }
    values.push(format!("({})", row));
    id += 1;
  }
  let mut nulls = id.to_string();
  let mut i: usize = 0;
  while i < columns {
    nulls.push_str(", NULL");
    i += 1;
  }
  values.push(format!("({})", nulls));
  mysql(&format!(
    "INSERT INTO {}.{} VALUES {};",
    IC_IT_DB,
    table,
    values.join(", ")
  ))?;
  Some(())
}

/// Strings that sort apart in some collations and together in others:
/// case, accents, the German and Danish letters and their spellings,
/// trailing spaces and a tab, which comes before a space.
const IC_IT_LATIN1_WORDS: [&str; 33] = [
  "7373", "DF", "6165", "E4", "4145", "C4", "6F65", "F6", "7565", "FC", "6161",
  "E5", "6368", "6120", "2061", "", "6109", "7A7A", "5A7A", "4141", "C5", "6E",
  "F1", "6F", "6E6E", "4C4C", "6C6C", "41E4", "E4E4", "61626364", "41424344",
  "6162", "FF41",
];

/// Make the `ordl` table afresh: a CHAR or VARCHAR column for each
/// latin1 collation, each with an ordered index `ix_<column>`, holding
/// every byte but 0 alone, then the words, the same in every column, and
/// a row of NULLs. The columns' names, or `None` if there is no MySQL
/// client.
fn setup_latin1_table() -> Option<Vec<&'static str>> {
  let columns: [(&'static str, &str); 11] = [
    (
      "c_sw",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_swedish_ci",
    ),
    (
      "c_g1",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_german1_ci",
    ),
    (
      "c_da",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_danish_ci",
    ),
    (
      "c_g2",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_german2_ci",
    ),
    ("c_bin", "CHAR(4) CHARACTER SET latin1 COLLATE latin1_bin"),
    (
      "c_gci",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_general_ci",
    ),
    (
      "c_gcs",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_general_cs",
    ),
    (
      "c_sp",
      "CHAR(4) CHARACTER SET latin1 COLLATE latin1_spanish_ci",
    ),
    (
      "v_sw",
      "VARCHAR(4) CHARACTER SET latin1 COLLATE latin1_swedish_ci",
    ),
    (
      "v_g2",
      "VARCHAR(4) CHARACTER SET latin1 COLLATE latin1_german2_ci",
    ),
    (
      "v_bin",
      "VARCHAR(4) CHARACTER SET latin1 COLLATE latin1_bin",
    ),
  ];
  let mut defs = String::new();
  let mut keys = String::new();
  let mut names: Vec<&'static str> = Vec::new();
  for (name, def) in &columns {
    defs.push_str(&format!(",\n {} {}", name, def));
    keys.push_str(&format!(",\n KEY ix_{c} ({c})", c = name));
    names.push(*name);
  }
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ordl;
     CREATE TABLE {db}.ordl (
       id INT NOT NULL PRIMARY KEY{defs}{keys}
     ) ENGINE=NDB;",
    db = IC_IT_DB,
    defs = defs,
    keys = keys
  );
  mysql(&sql)?;
  // Every byte but 0 alone, then the words.
  let mut hex: Vec<String> = Vec::new();
  let mut byte: u32 = 1;
  while byte <= 0xFF {
    hex.push(format!("{:02X}", byte));
    byte += 1;
  }
  for word in &IC_IT_LATIN1_WORDS {
    hex.push(word.to_string());
  }
  insert_strings("ordl", columns.len(), 1, &hex, "{}")?;
  Some(names)
}

#[test]
fn scan_index_ordered_by_each_latin1_collation() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let names = match setup_latin1_table() {
    Some(names) => names,
    None => return,
  };
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "ordl");
  let faults = ordered_faults(&mut conn, &t, "ordl", &names, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  // A range in a collation's order: from 'ae' up to but not 'b' in
  // latin1_german2_ci, where 'ä' is 'ae'.
  let index = conn
    .index_bind(IC_IT_DB, "ix_c_g2", "ordl")
    .expect("bind the index");
  let key_rec = record_over(&t, &["c_g2"]);
  let scan = conn
    .create_index_scan(&index, &key_rec, &t.rec)
    .expect("create the index scan");
  let low = bound_row(&t, &key_rec, &["c_g2"], &[Some("ae")]);
  let high = bound_row(&t, &key_rec, &["c_g2"], &[Some("b")]);
  conn
    .add_scan_range(scan, &ab(1, true, 1, false), &low, &high)
    .expect("add the range");
  let args = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  let (rows, error) = index_rows(&mut conn, &t, scan, &args);
  assert!(error.is_none(), "{:?}", error);
  let ranks =
    mysql_ranks("ordl", &rank_by("c_g2"), "c_g2 >= 'ae' AND c_g2 < 'b'");
  let faults = order_faults(&rows, &ranks, true, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(conn.unexpected(), 0);
}

/// Strings in UTF-8, in hexadecimal: Latin letters with their cases and
/// accents, the German sharp s and its capital, ligatures, Greek and
/// Cyrillic, kana and Han characters, emoji, digits, punctuation, and
/// trailing spaces and a tab.
const IC_IT_UTF8_WORDS: [&str; 45] = [
  "61", "41", "C3A1", "C381", "C3A0", "62", "42", "C39F", "7373", "5353",
  "E1BA9E", "C3A6", "6165", "C386", "7A", "5A", "C3A4", "65", "C3A9", "45",
  "C3AA", "F09F9880", "F09F9881", "E4B8AD", "E697A5", "E38182", "E382A2",
  "6120", "612020", "2061", "", "6161", "6142", "4162", "6162", "6109", "C5A0",
  "73", "CEB1", "D0B0", "31", "39", "2D", "5F", "E282AC",
];

#[test]
fn scan_index_ordered_by_unicode_collations() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let columns: [(&str, &str); 6] = [
    (
      "v_ai",
      "VARCHAR(8) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci",
    ),
    (
      "v_as",
      "VARCHAR(8) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs",
    ),
    (
      "v_ub",
      "VARCHAR(8) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin",
    ),
    (
      "v_bin",
      "VARCHAR(8) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin",
    ),
    (
      "v_gen",
      "VARCHAR(8) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci",
    ),
    (
      "c_ai",
      "CHAR(4) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci",
    ),
  ];
  let mut defs = String::new();
  let mut keys = String::new();
  let mut names: Vec<&str> = Vec::new();
  for (name, def) in &columns {
    defs.push_str(&format!(",\n {} {}", name, def));
    keys.push_str(&format!(",\n KEY ix_{c} ({c})", c = name));
    names.push(*name);
  }
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ordu;
     CREATE TABLE {db}.ordu (
       id INT NOT NULL PRIMARY KEY{defs}{keys}
     ) ENGINE=NDB;",
    db = IC_IT_DB,
    defs = defs,
    keys = keys
  );
  if mysql(&sql).is_none() {
    return;
  }
  let mut hex: Vec<String> = Vec::new();
  for word in &IC_IT_UTF8_WORDS {
    hex.push(word.to_string());
  }
  let wrap = "CONVERT({} USING utf8mb4)";
  if insert_strings("ordu", columns.len(), 1, &hex, wrap).is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "ordu");
  let faults = ordered_faults(&mut conn, &t, "ordu", &names, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn scan_index_rows_taken_over_are_updated() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_rng_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "rng");
  let (_, key_rec, scan) = ab_scan(&mut conn, &t);
  // Queries whose update writes v alone.
  let id_rec = record_over(&t, &["id"]);
  let v_rec = record_over(&t, &["id", "v"]);
  let mut free: Vec<QueryId> = Vec::new();
  while free.len() < 300 {
    free.push(conn.create_query(&t.def, &id_rec, &v_rec).expect("a query"));
  }
  let all = free.len();
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("start a transaction");
  let low = bound_row(&t, &key_rec, &["a"], &[Some("10")]);
  let high = bound_row(&t, &key_rec, &["a"], &[Some("19")]);
  conn
    .add_scan_range(scan, &ab(1, true, 1, true), &low, &high)
    .expect("add the range");
  let args = ScanArgs {
    lock: ScanLock::Exclusive,
    trans: Some(tid),
    ordered: true,
    batch_rows: 10,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start the scan");
  let mut rows: i64 = 0;
  while conn.next_row(scan).expect("a row") {
    rows += 1;
    take_back(&mut conn, &mut free);
    let qid = free.pop().expect("a free query");
    let query = conn.query_mut(qid).expect("the query");
    put_into(&t, &v_rec, query.attr_row_mut(), "v", "-1");
    conn
      .update_scan_row(qid, scan, &TakeoverArgs::default())
      .expect("take the row over to update it");
  }
  assert!(conn.scan(scan).expect("the scan").error().is_none());
  let _ = conn.send_queries(true);
  let start = time::gethrtime();
  while free.len() < all {
    let _ = conn.flush(100, true);
    take_back(&mut conn, &mut free);
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the takeovers did not end");
  }
  conn.commit_transaction(tid).expect("commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  let db = IC_IT_DB;
  let in_range = count_of(&format!(
    "SELECT COUNT(*) FROM {}.rng WHERE a BETWEEN 10 AND 19",
    db
  ));
  assert_eq!(rows, in_range);
  let updated = count_of(&format!(
    "SELECT COUNT(*) FROM {}.rng WHERE v = -1 AND a BETWEEN 10 AND 19",
    db
  ));
  assert_eq!(updated, in_range, "every row of the range updated");
  let others =
    count_of(&format!("SELECT COUNT(*) FROM {}.rng WHERE v = -1", db));
  assert_eq!(others, in_range, "and no other");
  assert_eq!(conn.unexpected(), 0);
}

/// A scan's callback that is never called: an ordered run with one is
/// refused.
fn never_called(_: &mut ApidConnection, _: ScanBatchId, _: usize) {}

#[test]
fn scan_index_refusals() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_rng_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "rng");
  let not_supported = err::IC_ERROR_NOT_SUPPORTED;
  // A unique hash index has no order to scan.
  let unique = bind_unique_index(&mut conn, "uk_u", "rng");
  let u_rec = record_over(&t, &["u"]);
  let e = conn
    .create_index_scan(&unique, &u_rec, &t.rec)
    .expect_err("a hash index");
  assert_eq!(e.code, not_supported);
  // A key record without every column of the index.
  let index = conn.index_bind(IC_IT_DB, "ix_ab", "rng").expect("bind");
  let a_rec = record_over(&t, &["a"]);
  let e = conn
    .create_index_scan(&index, &a_rec, &t.rec)
    .expect_err("b is missing");
  assert_eq!(e.code, err::IC_ERROR_KEY_RECORD);
  // A table scan takes no range and has no order.
  let table_scan = conn.create_scan(&t.def, &t.rec).expect("a table scan");
  let e = conn
    .add_scan_range(table_scan, &ab(0, false, 0, false), &[], &[])
    .expect_err("a table scan");
  assert_eq!(e.code, not_supported);
  let ordered = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  let e = conn.start_scan(table_scan, &ordered).expect_err("no order");
  assert_eq!(e.code, not_supported);
  // An ordered run needs every index column in the rows it reads; an
  // unordered one does not.
  let key_rec = record_over(&t, &["a", "b"]);
  let narrow = record_over(&t, &["id"]);
  let scan = conn
    .create_index_scan(&index, &key_rec, &narrow)
    .expect("an index scan reading the id alone");
  let e = conn.start_scan(scan, &ordered).expect_err("no a or b read");
  assert_eq!(e.code, err::IC_ERROR_KEY_RECORD);
  let (rows, error) = index_rows(&mut conn, &t, scan, &ScanArgs::default());
  assert!(error.is_none(), "{:?}", error);
  assert_eq!(rows.len() as i64, IC_IT_RNG_ROWS);
  // An ordered run is read only row by row.
  let (_, _, scan) = ab_scan(&mut conn, &t);
  let with_callback = ScanArgs {
    ordered: true,
    callback: Some(never_called),
    ..ScanArgs::default()
  };
  let e = conn
    .start_scan(scan, &with_callback)
    .expect_err("a callback");
  assert_eq!(e.code, not_supported);
  conn.start_scan(scan, &ordered).expect("start");
  let e = conn.next_batch(scan).expect_err("batches are merged");
  assert_eq!(e.code, not_supported);
  // Read a few rows, then close it. Its batches went on no ready list.
  let mut read: u32 = 0;
  while read < 10 && conn.next_row(scan).expect("a row") {
    read += 1;
  }
  assert_eq!(read, 10);
  assert!(conn.get_next_scan_batch().is_none(), "on no ready list");
  conn.close_scan(scan).expect("close");
  assert!(!conn.next_row(scan).expect("closed"));
  assert!(conn.scan(scan).expect("the scan").error().is_none());
  assert_eq!(conn.unexpected(), 0);
}

/// Every row a reader's run reads, each one's id and range number, and
/// how the run ended.
fn reader_rows(
  reader: &mut ScanReader,
  t: &Table,
) -> (Vec<(i64, u32)>, Option<IcError>) {
  let mut rows: Vec<(i64, u32)> = Vec::new();
  loop {
    match reader.next_row() {
      Ok(true) => {
        let id: i64 = t.get(reader.row(), "id").parse().expect("an id");
        rows.push((id, reader.range_no()));
      }
      Ok(false) => return (rows, None),
      Err(e) => return (rows, Some(e)),
    }
  }
}

/// A table bound through an executor, with its default record.
fn bind_through(submitter: &Submitter, table: &str) -> Table {
  let def = submitter.table_bind(IC_IT_DB, table).expect("bind");
  let rec = Record::default_for(&def, None).expect("a record");
  Table { def, rec }
}

/// A shape over the index `name` of `table`, its key record over
/// `columns`, reading the whole row.
fn index_shape(
  submitter: &Submitter,
  t: &Table,
  table: &str,
  name: &str,
  columns: &[&str],
) -> Arc<ScanShape> {
  let index = submitter
    .index_bind(IC_IT_DB, name, table)
    .expect("bind the ordered index");
  let key_rec = record_over(t, columns);
  ScanShape::new_index(&index, &key_rec, &t.rec).expect("an index shape")
}

#[test]
fn scan_index_through_an_executor() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_rng_table().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let t = bind_through(&submitter, "rng");
  let columns = ["a", "b"];
  let shape = index_shape(&submitter, &t, "rng", "ix_ab", &columns);
  let key_rec = record_over(&t, &columns);
  let mut reader = ScanReader::new(&shape);
  // Unordered, ordered both ways, reading ahead, and with locks; the
  // same reader run after run, in small batches.
  let modes = [
    (false, false, false, ScanLock::Committed),
    (true, false, false, ScanLock::Committed),
    (true, true, false, ScanLock::Committed),
    (true, false, true, ScanLock::Committed),
    (true, true, true, ScanLock::Committed),
    (true, false, false, ScanLock::Shared),
    (true, true, false, ScanLock::Exclusive),
  ];
  let mut faults: Vec<String> = Vec::new();
  for case in &IC_IT_AB_RANGES {
    let ranks = mysql_ranks("rng", &rank_by("a, b"), case.filter);
    let low = bound_row(&t, &key_rec, &columns, case.low);
    let high = bound_row(&t, &key_rec, &columns, case.high);
    for (ordered, descending, read_ahead, lock) in modes {
      reader
        .add_range(&case.range, &low, &high)
        .expect("add the range");
      let args = ScanArgs {
        lock,
        ordered,
        descending,
        read_ahead,
        batch_rows: 5,
        timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
        ..ScanArgs::default()
      };
      submitter.scan(&mut reader, &args).expect("submit the scan");
      let (rows, error) = reader_rows(&mut reader, &t);
      let what = format!(
        "{} ordered {} desc {} ahead {} {:?}",
        case.filter, ordered, descending, read_ahead, lock
      );
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, ordered, descending) {
        faults.push(format!("{}: {}", what, fault));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
  // Numbered ranges, each row's number checked: range 0, a = 3; range
  // 1, a from 10 to 12; range 5, a above 47.
  let filter = "a = 3 OR a BETWEEN 10 AND 12 OR a > 47";
  let numbers = mysql_ranks(
    "rng",
    "CASE WHEN a = 3 THEN 0 WHEN a <= 12 THEN 1 ELSE 5 END",
    filter,
  );
  let ranks = mysql_ranks("rng", &rank_by("a, b"), filter);
  let ranges: [(u32, ScanRange, &str, &str); 3] = [
    (0, ab(1, true, 1, true), "3", "3"),
    (1, ab(1, true, 1, true), "10", "12"),
    (5, ab(1, false, 0, false), "47", "0"),
  ];
  for (ordered, descending) in [(false, false), (true, false), (true, true)] {
    for (range_no, range, low, high) in &ranges {
      let numbered = ScanRange {
        range_no: *range_no,
        ..*range
      };
      let low_row = bound_row(&t, &key_rec, &columns, &[Some(*low)]);
      let high_row = bound_row(&t, &key_rec, &columns, &[Some(*high)]);
      reader
        .add_range(&numbered, &low_row, &high_row)
        .expect("add a range");
    }
    let args = ScanArgs {
      ordered,
      descending,
      batch_rows: 3,
      timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
      ..ScanArgs::default()
    };
    submitter.scan(&mut reader, &args).expect("submit");
    let (rows, error) = reader_rows(&mut reader, &t);
    let what = format!("numbered, ordered {} desc {}", ordered, descending);
    assert!(error.is_none(), "{}: {:?}", what, error);
    for fault in order_faults(&rows, &ranks, ordered, descending) {
      faults.push(format!("{}: {}", what, fault));
    }
    for (id, range_no) in &rows {
      let expected = numbers.get(id).copied().unwrap_or(-1);
      if expected != *range_no as i64 {
        faults.push(format!("{}: row {} in range {}", what, id, range_no));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
  // An ordered run's numbers must rise; the refused run leaves its
  // ranges for the next, which reads the rows of a = 4 once a range.
  let four = bound_row(&t, &key_rec, &columns, &[Some("4")]);
  for range_no in [4, 2] {
    let range = ScanRange {
      range_no,
      ..ab(1, true, 1, true)
    };
    reader.add_range(&range, &four, &four).expect("add");
  }
  let ordered = ScanArgs {
    ordered: true,
    ..ScanArgs::default()
  };
  let e = submitter
    .scan(&mut reader, &ordered)
    .expect_err("numbers that do not rise");
  assert_eq!(e.code, err::IC_ERROR_BAD_RANGE);
  submitter
    .scan(&mut reader, &ScanArgs::default())
    .expect("unordered");
  let (rows, error) = reader_rows(&mut reader, &t);
  assert!(error.is_none(), "{:?}", error);
  let fours = count_of(&format!(
    "SELECT COUNT(*) FROM {}.rng WHERE a = 4",
    IC_IT_DB
  ));
  assert_eq!(rows.len() as i64, 2 * fours);
  // A shape over the table has no ranges and no order; an index shape
  // whose rows leave out an index column has no order either.
  let table_shape = ScanShape::new(&t.def, &t.rec).expect("a table shape");
  let mut table_reader = ScanReader::new(&table_shape);
  let e = table_reader
    .add_range(&ab(0, false, 0, false), &[], &[])
    .expect_err("a table shape");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  let e = submitter
    .scan(&mut table_reader, &ordered)
    .expect_err("no order");
  assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  let index = submitter
    .index_bind(IC_IT_DB, "ix_ab", "rng")
    .expect("bind the index");
  let narrow = record_over(&t, &["id"]);
  let narrow_shape =
    ScanShape::new_index(&index, &key_rec, &narrow).expect("a shape");
  let mut narrow_reader = ScanReader::new(&narrow_shape);
  let e = submitter
    .scan(&mut narrow_reader, &ordered)
    .expect_err("no a or b read");
  assert_eq!(e.code, err::IC_ERROR_KEY_RECORD);
  // Closed early, an ordered reader runs again.
  submitter.scan(&mut reader, &ordered).expect("submit");
  let mut read: u32 = 0;
  while read < 10 && reader.next_row().expect("a row") {
    read += 1;
  }
  assert_eq!(read, 10);
  reader.close();
  assert!(reader.is_done());
  submitter.scan(&mut reader, &ordered).expect("again");
  let (rows, error) = reader_rows(&mut reader, &t);
  assert!(error.is_none(), "{:?}", error);
  let all = mysql_ranks("rng", &rank_by("a, b"), "TRUE");
  let faults = order_faults(&rows, &all, true, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
}

#[test]
fn scan_index_ordered_through_an_executor_by_each_latin1_collation() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let names = match setup_latin1_table() {
    Some(names) => names,
    None => return,
  };
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let t = bind_through(&submitter, "ordl");
  let mut faults: Vec<String> = Vec::new();
  for column in &names {
    let name = format!("ix_{}", column);
    let shape = index_shape(&submitter, &t, "ordl", &name, &[*column]);
    let mut reader = ScanReader::new(&shape);
    let ranks = mysql_ranks("ordl", &rank_by(column), "TRUE");
    for descending in [false, true] {
      let args = ScanArgs {
        ordered: true,
        descending,
        batch_rows: 3,
        timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
        ..ScanArgs::default()
      };
      submitter.scan(&mut reader, &args).expect("submit the scan");
      let (rows, error) = reader_rows(&mut reader, &t);
      let what = format!("{} desc {}", column, descending);
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, true, descending) {
        faults.push(format!("{}: {}", what, fault));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
}

// ---- exit: the runs phase 8 is accepted by (chapter 06 §8.8) ----

/// Rows in the `big` table: the million a scan of the exit counts.
const IC_IT_BIG_ROWS: i64 = 1_000_000;
/// Rows each transaction writes while the table is filled.
const IC_IT_BIG_BATCH: i64 = 250;
/// Threads that fill it.
const IC_IT_BIG_WRITERS: i64 = 16;

/// Make the `big` table afresh, id and val, and fill it with
/// `IC_IT_BIG_ROWS` rows through `submitter`, sixteen threads a
/// transaction of 250 writes at a time; a million rows of two integers
/// take about 60 MB a data node (doc/dev-cluster.md). The table, and
/// its rows and the sum of val as MySQL counts them; `None` without a
/// MySQL client.
fn fill_big_table(submitter: &Submitter) -> Option<(Table, i64, i64)> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.big;
     CREATE TABLE {db}.big (
       id INT NOT NULL PRIMARY KEY,
       val INT NOT NULL
     ) ENGINE=NDB;",
    db = IC_IT_DB
  );
  mysql(&sql)?;
  let t = bind_through(submitter, "big");
  let shape = OpShape::new(&t.def, &t.rec, &t.rec).expect("a shape");
  let start = time::gethrtime();
  let batches = IC_IT_BIG_ROWS / IC_IT_BIG_BATCH;
  std::thread::scope(|scope| {
    let mut handles = Vec::new();
    let mut writer: i64 = 0;
    while writer < IC_IT_BIG_WRITERS {
      let mut mine = submitter.clone();
      let t = &t;
      let shape = &shape;
      let first = writer;
      handles.push(scope.spawn(move || {
        let mut request = Request::new();
        let mut batch = first;
        while batch < batches {
          request.clear();
          let mut id = batch * IC_IT_BIG_BATCH + 1;
          while id <= (batch + 1) * IC_IT_BIG_BATCH {
            let mut op = KeyOp::write(shape, WriteKind::Write);
            t.put(op.key_row_mut(), "id", &id.to_string());
            t.put(op.attr_row_mut(), "id", &id.to_string());
            t.put(op.attr_row_mut(), "val", &(id % 1000).to_string());
            request.push(op);
            id += 1;
          }
          if let Err(e) = mine.execute(&mut request) {
            panic!(
              "batch {}: {} ({}); a full table wants more data memory",
              batch,
              e.message(),
              e.code
            );
          }
          batch += IC_IT_BIG_WRITERS;
        }
      }));
      writer += 1;
    }
    for handle in handles {
      handle.join().expect("a writer");
    }
  });
  let wrote = time::millis_elapsed(start, time::gethrtime());
  println!("{} rows written in {} ms", IC_IT_BIG_ROWS, wrote);
  let db = IC_IT_DB;
  let counted = count_of(&format!("SELECT COUNT(*) FROM {}.big", db));
  let summed = count_of(&format!("SELECT SUM(val) FROM {}.big", db));
  assert_eq!(counted, IC_IT_BIG_ROWS, "what MySQL counts");
  Some((t, counted, summed))
}

#[test]
fn exit_a_scan_of_a_million_rows_counts_what_mysql_counts() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, counted, summed) = match fill_big_table(&submitter) {
    Some(filled) => filled,
    None => return,
  };
  let db = IC_IT_DB;

  // Parallel over every fragment, on a connection, row by row.
  let mut conn = cluster.global.create_connection().expect("connection");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let args = ScanArgs {
    timeout_ms: 300_000,
    ..ScanArgs::default()
  };
  let start = time::gethrtime();
  conn.start_scan(scan, &args).expect("start the scan");
  let mut rows: i64 = 0;
  let mut sum: i64 = 0;
  while conn.next_row(scan).expect("a row") {
    rows += 1;
    sum += t
      .get(conn.scan_row(scan), "val")
      .parse::<i64>()
      .expect("val");
  }
  let read = time::millis_elapsed(start, time::gethrtime());
  println!(
    "a scan of {} fragments read {} rows in {} ms",
    conn.scan(scan).expect("the scan").parallelism(),
    rows,
    read
  );
  assert_eq!(rows, counted, "the scan counts what MySQL counts");
  assert_eq!(sum, summed, "and sums what it sums");
  assert_eq!(
    conn.scan(scan).expect("the scan").parallelism(),
    t.def.info().fragment_count,
    "every fragment at once"
  );

  // And through the executor, with a reader.
  let scan_shape = ScanShape::new(&t.def, &t.rec).expect("a scan shape");
  let mut reader = ScanReader::new(&scan_shape);
  submitter.scan(&mut reader, &args).expect("submit");
  let mut through: i64 = 0;
  while reader.next_row().expect("a row") {
    through += 1;
  }
  assert_eq!(through, counted, "through an executor too");
  // The data nodes' memory back for the other groups.
  mysql(&format!("DROP TABLE IF EXISTS {}.big", db));
}

/// Application threads one executor serves at once.
const IC_IT_SCAN_THREADS: usize = 1_000;

#[test]
fn exit_one_executor_serves_1000_threads_scanning() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() || setup_rng_table().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (scan_t, table_shape) = scan_through(&submitter);
  let rng = bind_through(&submitter, "rng");
  let ab_shape = index_shape(&submitter, &rng, "rng", "ix_ab", &["a", "b"]);
  let key_rec = record_over(&rng, &["a", "b"]);
  // How many rows each value of a has, as MySQL counts them.
  let mut per_a: HashMap<i64, i64> = HashMap::new();
  let counts = mysql(&format!(
    "SELECT a, COUNT(*) FROM {}.rng GROUP BY a",
    IC_IT_DB
  ))
  .expect("MySQL");
  for line in counts.lines() {
    let mut parts = line.split('\t');
    let a: i64 = parts.next().expect("a").parse().expect("a");
    let count: i64 = parts.next().expect("a count").parse().expect("count");
    per_a.insert(a, count);
  }
  // Half the threads read the whole `scan` table, half a value of a in
  // the order of ix_ab; every thread has a reader of its own, and the
  // executor runs 64 scans at once, the rest waiting their turn.
  let start = time::gethrtime();
  let mut faults: Vec<String> = Vec::new();
  std::thread::scope(|scope| {
    let mut handles = Vec::new();
    let mut n: usize = 0;
    while n < IC_IT_SCAN_THREADS {
      let mine = submitter.clone();
      let scan_t = &scan_t;
      let rng = &rng;
      let key_rec = &key_rec;
      let table_shape = &table_shape;
      let ab_shape = &ab_shape;
      let per_a = &per_a;
      let thread = n;
      let spawned = std::thread::Builder::new()
        .stack_size(256 * 1024)
        .spawn_scoped(scope, move || -> Result<(), String> {
          if thread % 2 == 0 {
            let mut reader = ScanReader::new(table_shape);
            let args = bounded(ScanLock::Committed, thread % 4 == 0);
            mine.scan(&mut reader, &args).map_err(|e| e.message())?;
            let (ids, error) = reader_ids(&mut reader, scan_t);
            if let Some(e) = error {
              return Err(format!("thread {}: {}", thread, e.message()));
            }
            if !every_row_once(&ids) {
              return Err(format!("thread {}: {} rows", thread, ids.len()));
            }
            return Ok(());
          }
          let a = (thread % 50) as i64;
          let text = a.to_string();
          let row = bound_row(rng, key_rec, &["a"], &[Some(text.as_str())]);
          let mut reader = ScanReader::new(ab_shape);
          reader
            .add_range(&ab(1, true, 1, true), &row, &row)
            .map_err(|e| e.message())?;
          let args = ScanArgs {
            ordered: true,
            batch_rows: 4,
            timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
            ..ScanArgs::default()
          };
          mine.scan(&mut reader, &args).map_err(|e| e.message())?;
          let (rows, error) = reader_rows(&mut reader, rng);
          if let Some(e) = error {
            return Err(format!("thread {}: {}", thread, e.message()));
          }
          let expected = per_a.get(&a).copied().unwrap_or(0);
          if rows.len() as i64 != expected {
            return Err(format!(
              "thread {}: a = {}: {} rows of {}",
              thread,
              a,
              rows.len(),
              expected
            ));
          }
          Ok(())
        })
        .expect("an application thread");
      handles.push(spawned);
      n += 1;
    }
    for handle in handles {
      match handle.join() {
        Ok(Ok(())) => {}
        Ok(Err(fault)) => faults.push(fault),
        Err(_) => faults.push("a thread panicked".to_string()),
      }
    }
  });
  let took = time::millis_elapsed(start, time::gethrtime());
  println!(
    "{} threads, a scan each, through one executor in {} ms",
    IC_IT_SCAN_THREADS, took
  );
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(executor.stats().scans, IC_IT_SCAN_THREADS as u64);
}

#[test]
fn failure_a_data_node_restarted_during_a_scan_fails_it_and_a_retry_reads_all()
{
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
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
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  // A shared-lock scan reads each fragment at its primary replica, so
  // whichever node coordinates, half the fragments are read at the
  // victim. Read slowly, so that the run is still going when the victim
  // goes down.
  let args = ScanArgs {
    lock: ScanLock::Shared,
    batch_rows: 10,
    timeout_ms: 120_000,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start the scan");
  let mut rows: i64 = 0;
  while rows < 100 && conn.next_row(scan).expect("a row before") {
    rows += 1;
  }
  // Restarted with -a, at once, as a node killed goes: a graceful stop
  // could let the scan finish first.
  eprintln!("restarting data node {} at once during the scan", victim);
  let command = format!("{} RESTART -a", victim);
  let restart = std::thread::spawn(move || mgm_client(&command));
  let error = loop {
    match conn.next_row(scan) {
      Ok(true) => {
        rows += 1;
        std::thread::sleep(std::time::Duration::from_millis(10));
      }
      Ok(false) => panic!("the scan read {} rows to the end first", rows),
      Err(e) => break e,
    }
  };
  let _ = restart.join();
  eprintln!(
    "the scan failed after {} rows: {} ({})",
    rows,
    error.message(),
    error.code
  );
  assert!(error.is_temporary(), "temporary: {}", error.code);
  assert_ne!(error.code, err::IC_ERROR_TIMEOUT, "not by its deadline");

  // Started again, it reads every row once, at the survivor: tried
  // again while the cluster still handles the failure.
  let mut tries: u32 = 0;
  loop {
    tries += 1;
    let (ids, error) = scan_rows(&mut conn, &t, scan, &ScanArgs::default());
    match error {
      None => {
        assert!(every_row_once(&ids), "{} rows", ids.len());
        break;
      }
      Some(e) => {
        assert!(e.is_temporary(), "a retry failed: {}", e.code);
        assert!(tries < 60, "no retry read to the end: {}", e.code);
        std::thread::sleep(std::time::Duration::from_millis(500));
      }
    }
  }
  eprintln!("the scan run again read every row, try {}", tries);
  assert_eq!(
    conn.unexpected(),
    0,
    "the last no request expected: {:?}; {} late scan signals",
    conn.last_unexpected(),
    conn.late_scan_signals()
  );

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

// ---- where and assign: programs against MySQL (chapter 06 §9.1) ----

/// Rows of the `wh` table the `where` group fills.
const IC_IT_WH_ROWS: i64 = 400;
/// Rows of the twin tables of the `assign` group.
const IC_IT_ASG_ROWS: i64 = 200;
/// Conditions the `where` group draws, each run every way it runs.
const IC_IT_WHERE_CONDS: u32 = 100;
/// Statements the `assign` group draws.
const IC_IT_ASSIGN_STATEMENTS: u32 = 60;
/// Operations defined on one transaction, or put in one request.
const IC_IT_OPS_PER_TRANS: usize = 100;
/// Strings of a VARCHAR: cases a latin1 collation folds, letters beyond
/// ASCII, the empty one, and one with a trailing space, which pads.
const IC_IT_NAMES: [&str; 7] = ["abc", "ABC", "abd", "b", "åäö", "", "abc "];
/// Strings of a CHAR(4).
const IC_IT_CODES: [&str; 4] = ["c1", "C1", "c2", "zz"];
/// The inputs a drawn program reads, of the sixteen.
const IC_IT_INPUTS: usize = 5;

/// Numbers drawn the same way every run.
struct Draw {
  state: u64,
}

impl Draw {
  fn new(seed: u64) -> Draw {
    Draw {
      state: seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1,
    }
  }

  /// 0 to `n` − 1.
  fn below(&mut self, n: u32) -> u32 {
    self.state ^= self.state << 13;
    self.state ^= self.state >> 7;
    self.state ^= self.state << 17;
    ((self.state >> 32) as u32) % n
  }

  /// `low` to `high`, both in.
  fn range(&mut self, low: i64, high: i64) -> i64 {
    low + self.below((high - low + 1) as u32) as i64
  }

  /// True one time in `n`.
  fn one_in(&mut self, n: u32) -> bool {
    self.below(n) == 0
  }
}

/// What a column holds, for what its constants are drawn from and what
/// it is compared with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
  Int,
  Uint,
  Big,
  Name,
  Code,
}

/// A column of a table the groups draw over: its name, kind, and
/// whether it may be NULL.
#[derive(Clone, Copy)]
struct DrawColumn {
  name: &'static str,
  kind: Kind,
  nullable: bool,
}

const fn draw_column(
  name: &'static str,
  kind: Kind,
  nullable: bool,
) -> DrawColumn {
  DrawColumn {
    name,
    kind,
    nullable,
  }
}

/// A table the groups draw conditions over.
struct Schema {
  columns: &'static [DrawColumn],
  /// Its INT columns, which compare with each other and take sums.
  ints: &'static [&'static str],
  /// The signed columns compared with inputs.
  signed: &'static [&'static str],
  /// An unsigned column, summed with integers of no sign only, as MySQL
  /// refuses a negative unsigned value.
  unsigned: &'static str,
}

/// `wh`: what the `where` group filters.
const IC_IT_WH: Schema = Schema {
  columns: &[
    draw_column("id", Kind::Int, false),
    draw_column("a", Kind::Int, true),
    draw_column("b", Kind::Int, true),
    draw_column("n", Kind::Int, false),
    draw_column("u", Kind::Uint, true),
    draw_column("big", Kind::Big, true),
    draw_column("name", Kind::Name, true),
    draw_column("code", Kind::Code, true),
  ],
  ints: &["id", "a", "b", "n"],
  signed: &["id", "a", "b", "n", "big"],
  unsigned: "u",
};

/// The twins of the `assign` group.
const IC_IT_ASG: Schema = Schema {
  columns: &[
    draw_column("id", Kind::Int, false),
    draw_column("v", Kind::Int, true),
    draw_column("w", Kind::Int, true),
    draw_column("cnt", Kind::Uint, false),
    draw_column("name", Kind::Name, true),
  ],
  ints: &["id", "v", "w"],
  signed: &["id", "v", "w"],
  unsigned: "cnt",
};

/// A value of a column as text, as `text_row` takes it: NULL now and
/// then where the column may be.
fn draw_value(draw: &mut Draw, column: &DrawColumn) -> String {
  if column.nullable && draw.one_in(6) {
    return "NULL".to_string();
  }
  match column.kind {
    Kind::Int => draw.range(-20, 20).to_string(),
    Kind::Uint => draw.range(0, 40).to_string(),
    Kind::Big => {
      (draw.range(-3, 3) * (1i64 << 40) + draw.range(-5, 5)).to_string()
    }
    Kind::Name => IC_IT_NAMES[draw.below(7) as usize].to_string(),
    Kind::Code => IC_IT_CODES[draw.below(4) as usize].to_string(),
  }
}

/// A value's text as SQL: a string quoted, a number and NULL as they
/// are.
fn sql_literal(kind: Kind, text: &str) -> String {
  if text == "NULL" {
    return text.to_string();
  }
  match kind {
    Kind::Name | Kind::Code => format!("'{}'", text),
    _ => text.to_string(),
  }
}

/// `count` rows of `schema` as the values of an INSERT, ids from 1, the
/// other columns drawn; the INT columns of the `assign` twins drawn
/// wider, and their counter from 0 to 1000.
fn insert_values(draw: &mut Draw, schema: &Schema, count: i64) -> String {
  let mut sql = String::new();
  let mut id: i64 = 1;
  while id <= count {
    if id > 1 {
      sql.push(',');
    }
    sql.push_str(&format!("({}", id));
    let mut place: usize = 1;
    while place < schema.columns.len() {
      let column = &schema.columns[place];
      let mut text = draw_value(draw, column);
      if count == IC_IT_ASG_ROWS && text != "NULL" {
        if column.kind == Kind::Int {
          text = draw.range(-1000, 1000).to_string();
        } else if column.kind == Kind::Uint {
          text = draw.range(0, 1000).to_string();
        }
      }
      sql.push(',');
      sql.push_str(&sql_literal(column.kind, &text));
      place += 1;
    }
    sql.push(')');
    id += 1;
  }
  sql
}

/// Make `wh` afresh, filled through MySQL, or `None` without a client.
fn fill_wh() -> Option<()> {
  let mut draw = Draw::new(2026_10_20);
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.wh;
     CREATE TABLE {db}.wh (
       id INT NOT NULL PRIMARY KEY,
       a INT, b INT, n INT NOT NULL,
       u INT UNSIGNED, big BIGINT,
       name VARCHAR(16), code CHAR(4),
       KEY ix_a (a)
     ) ENGINE=NDB DEFAULT CHARSET=latin1;
     INSERT INTO {db}.wh VALUES {values};",
    db = IC_IT_DB,
    values = insert_values(&mut draw, &IC_IT_WH, IC_IT_WH_ROWS)
  );
  mysql(&sql)?;
  Some(())
}

/// Make the `assign` twins afresh, with the same rows, or `None`
/// without a client.
fn fill_asg() -> Option<()> {
  let mut draw = Draw::new(20_261_021);
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.asg_api;
     DROP TABLE IF EXISTS {db}.asg_sql;
     CREATE TABLE {db}.asg_api (
       id INT NOT NULL PRIMARY KEY,
       v INT, w INT, cnt BIGINT UNSIGNED NOT NULL,
       name VARCHAR(16)
     ) ENGINE=NDB DEFAULT CHARSET=latin1;
     CREATE TABLE {db}.asg_sql LIKE {db}.asg_api;
     INSERT INTO {db}.asg_api VALUES {values};
     INSERT INTO {db}.asg_sql SELECT * FROM {db}.asg_api;",
    db = IC_IT_DB,
    values = insert_values(&mut draw, &IC_IT_ASG, IC_IT_ASG_ROWS)
  );
  mysql(&sql)?;
  Some(())
}

/// The ids a query prints, one a line.
fn sql_ids(sql: &str) -> Vec<i64> {
  let text = mysql(sql).expect("a MySQL client");
  let mut ids: Vec<i64> = Vec::new();
  for line in text.lines() {
    if !line.is_empty() {
      ids.push(line.trim().parse().expect("an id"));
    }
  }
  ids
}

/// A condition drawn over a table: the program's tree in a builder, the
/// inputs it is given, and the same condition as SQL with the inputs'
/// values in their places.
struct CondDraw<'a> {
  draw: Draw,
  b: ProgramBuilder,
  t: &'a Table,
  schema: &'a Schema,
  inputs: [i64; 16],
}

impl<'a> CondDraw<'a> {
  /// A drawing over `t`, numbered `seed`, with its inputs drawn: the
  /// last of them never negative, for an unsigned column.
  fn new(t: &'a Table, schema: &'a Schema, seed: u64) -> CondDraw<'a> {
    let mut draw = Draw::new(seed);
    let mut inputs = [0i64; 16];
    let mut input: usize = 0;
    while input < IC_IT_INPUTS {
      inputs[input] = draw.range(-20, 20);
      input += 1;
    }
    inputs[IC_IT_INPUTS - 1] = draw.range(0, 20);
    CondDraw {
      draw,
      b: ProgramBuilder::new(&t.def),
      t,
      schema,
      inputs,
    }
  }

  fn id(&self, name: &str) -> u32 {
    self.t.def.field_id(name).expect("a column")
  }

  fn column(&mut self, name: &str) -> Value {
    let id = self.id(name);
    self.b.column(id)
  }

  fn pick(&mut self, names: &[&'static str]) -> &'static str {
    names[self.draw.below(names.len() as u32) as usize]
  }

  /// A constant of `column`, drawn from a row of it, and its SQL.
  fn constant(&mut self, column: &DrawColumn) -> (Value, String) {
    let text = draw_value(&mut self.draw, column);
    let mut row = self.t.row();
    self.t.put(&mut row, column.name, &text);
    let id = self.id(column.name);
    let k = self.b.constant(&self.t.rec, &row, id);
    (k, sql_literal(column.kind, &text))
  }

  /// One of the six comparisons, and its SQL.
  fn cmp(&mut self) -> (Cmp, &'static str) {
    let cmps = [
      (Cmp::Eq, "="),
      (Cmp::Ne, "<>"),
      (Cmp::Lt, "<"),
      (Cmp::Le, "<="),
      (Cmp::Gt, ">"),
      (Cmp::Ge, ">="),
    ];
    cmps[self.draw.below(6) as usize]
  }

  /// A term: a column against a constant of its own or another column,
  /// a NULL test, or a comparison worked out in registers.
  fn term(&mut self) -> (Cond, String) {
    let (cmp, op) = self.cmp();
    match self.draw.below(8) {
      0..=2 => {
        let columns = self.schema.columns;
        let column = columns[self.draw.below(columns.len() as u32) as usize];
        let c = self.column(column.name);
        let (k, literal) = self.constant(&column);
        if self.draw.one_in(2) {
          let sql = format!("{} {} {}", column.name, op, literal);
          (self.b.compare(cmp, c, k), sql)
        } else {
          let sql = format!("{} {} {}", literal, op, column.name);
          (self.b.compare(cmp, k, c), sql)
        }
      }
      3 => {
        let ints = self.schema.ints;
        let x = self.pick(ints);
        let y = self.pick(ints);
        let (cx, cy) = (self.column(x), self.column(y));
        (self.b.compare(cmp, cx, cy), format!("{} {} {}", x, op, y))
      }
      4 => {
        let columns = self.schema.columns;
        let name = columns[self.draw.below(columns.len() as u32) as usize].name;
        let c = self.column(name);
        if self.draw.one_in(2) {
          (self.b.is_null(c), format!("{} IS NULL", name))
        } else {
          (self.b.is_not_null(c), format!("{} IS NOT NULL", name))
        }
      }
      5 => {
        let signed = self.schema.signed;
        let name = self.pick(signed);
        let input = self.draw.below(IC_IT_INPUTS as u32 - 1);
        let c = self.column(name);
        let i = self.b.input(input);
        let sql = format!("{} {} {}", name, op, self.inputs[input as usize]);
        (self.b.compare(cmp, c, i), sql)
      }
      6 => {
        let ints = self.schema.ints;
        let x = self.pick(ints);
        let y = self.pick(ints);
        let k = self.draw.range(-10, 10);
        let (cx, cy) = (self.column(x), self.column(y));
        let kv = self.b.integer(k);
        let (sum, sql) = if self.draw.one_in(2) {
          (self.b.add(cx, kv), format!("{} + {}", x, k))
        } else {
          (self.b.subtract(cx, kv), format!("{} - {}", x, k))
        };
        let sql = format!("{} {} {}", sql, op, y);
        (self.b.compare(cmp, sum, cy), sql)
      }
      _ => {
        let name = self.schema.unsigned;
        let k = self.draw.range(0, 10);
        let input = self.draw.below(IC_IT_INPUTS as u32);
        let c = self.column(name);
        let kv = self.b.integer(k);
        let sum = self.b.add(c, kv);
        let i = self.b.input(input);
        let value = self.inputs[input as usize];
        let sql = format!("{} + {} {} {}", name, k, op, value);
        (self.b.compare(cmp, sum, i), sql)
      }
    }
  }

  /// A condition: terms joined by AND, OR and NOT, three deep at most.
  fn cond(&mut self, depth: u32) -> (Cond, String) {
    let ways = if depth >= 3 { 1 } else { 5 };
    match self.draw.below(ways) {
      0 | 1 => self.term(),
      2 => {
        let (x, sql) = self.cond(depth + 1);
        (self.b.not(x), format!("(NOT {})", sql))
      }
      3 => {
        let (x, sx) = self.cond(depth + 1);
        let (y, sy) = self.cond(depth + 1);
        (self.b.and(x, y), format!("({} AND {})", sx, sy))
      }
      _ => {
        let (x, sx) = self.cond(depth + 1);
        let (y, sy) = self.cond(depth + 1);
        (self.b.or(x, y), format!("({} OR {})", sx, sy))
      }
    }
  }
}

/// A program drawn over a table: compiled, its inputs, and its
/// condition and assignments as SQL.
struct Drawn {
  program: Arc<Program>,
  inputs: [i64; 16],
  /// The WHERE condition; "TRUE" for a program with none.
  sql: String,
  /// The SET list of an update, empty for a filter or a delete.
  sets: String,
}

/// Condition `index` of the `where` group, over `wh`.
fn draw_where(t: &Table, index: u32) -> Drawn {
  let mut d = CondDraw::new(t, &IC_IT_WH, 1000 + index as u64);
  let (top, sql) = d.cond(0);
  let program = match d.b.finish(Some(top)) {
    Ok(program) => program,
    Err(e) => {
      panic!("condition {}: {} ({}): {}", index, e.message(), e.code, sql)
    }
  };
  Drawn {
    program,
    inputs: d.inputs,
    sql,
    sets: String::new(),
  }
}

/// Where the ids we read differ from MySQL's, if they do.
fn id_faults(
  faults: &mut Vec<String>,
  way: &str,
  drawn: &Drawn,
  index: u32,
  want: &[i64],
  got: &[i64],
  error: Option<IcError>,
) {
  if let Some(e) = error {
    faults.push(format!(
      "{} {}: error {} ({}) for {}",
      way,
      index,
      e.code,
      e.message(),
      drawn.sql
    ));
    return;
  }
  if want != got {
    let mut extra: Vec<i64> = Vec::new();
    for id in got {
      if !want.contains(id) {
        extra.push(*id);
      }
    }
    let mut missing: Vec<i64> = Vec::new();
    for id in want {
      if !got.contains(id) {
        missing.push(*id);
      }
    }
    extra.truncate(8);
    missing.truncate(8);
    faults.push(format!(
      "{} {}: {} rows, MySQL's {}; ours alone {:?}, MySQL's alone {:?}, \
       inputs {:?}: {}",
      way,
      index,
      got.len(),
      want.len(),
      extra,
      missing,
      &drawn.inputs[..IC_IT_INPUTS],
      drawn.sql
    ));
  }
}

#[test]
fn where_scans_filter_as_mysql_does() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if fill_wh().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "wh");
  let scan = conn.create_scan(&t.def, &t.rec).expect("a scan");
  let index = conn.index_bind(IC_IT_DB, "ix_a", "wh").expect("ix_a");
  let a = t.def.field_id("a").expect("a");
  let key_rec = Record::default_for(&t.def, Some(&[a])).expect("a key");
  let ranged = conn
    .create_index_scan(&index, &key_rec, &t.rec)
    .expect("an index scan");
  let mut low = vec![0u8; key_rec.row_size() as usize];
  let mut high = vec![0u8; key_rec.row_size() as usize];
  text_row::put_value(&key_rec, 0, "-5", &mut low).expect("low");
  text_row::put_value(&key_rec, 0, "5", &mut high).expect("high");
  let between = ScanRange {
    low_columns: 1,
    low_inclusive: true,
    high_columns: 1,
    high_inclusive: true,
    range_no: 0,
  };
  let executor = executor_with(&cluster, 64, 256);
  let submitter = executor.submitter();
  let te = bind_through(&submitter, "wh");
  let plain = ScanShape::new(&te.def, &te.rec).expect("a scan shape");
  let db = IC_IT_DB;
  let mut faults: Vec<String> = Vec::new();
  let mut index_no: u32 = 0;
  while index_no < IC_IT_WHERE_CONDS {
    let drawn = draw_where(&t, index_no);
    let want = sql_ids(&format!(
      "SELECT id FROM {}.wh WHERE {} ORDER BY id",
      db, drawn.sql
    ));

    // A table scan on the connection.
    conn
      .set_scan_program(scan, Some(&drawn.program))
      .expect("the program");
    let mut input: u32 = 0;
    while input < IC_IT_INPUTS as u32 {
      let value = drawn.inputs[input as usize];
      conn.set_scan_input(scan, input, value).expect("an input");
      input += 1;
    }
    let (mut got, error) = scan_rows(&mut conn, &t, scan, &ScanArgs::default());
    got.sort();
    id_faults(&mut faults, "scan", &drawn, index_no, &want, &got, error);

    // Through an executor, with a reader.
    let shape = plain.with_program(&drawn.program).expect("a shape");
    let mut reader = ScanReader::new(&shape);
    input = 0;
    while input < IC_IT_INPUTS as u32 {
      let value = drawn.inputs[input as usize];
      reader.set_input(input, value).expect("an input");
      input += 1;
    }
    let args = ScanArgs::default();
    submitter.scan(&mut reader, &args).expect("submit the scan");
    let (rows, error) = reader_rows(&mut reader, &te);
    let mut got: Vec<i64> = Vec::new();
    for (id, _) in rows {
      got.push(id);
    }
    got.sort();
    id_faults(&mut faults, "reader", &drawn, index_no, &want, &got, error);

    // A range of the ordered index, filtered.
    let want = sql_ids(&format!(
      "SELECT id FROM {}.wh WHERE a BETWEEN -5 AND 5 AND {} ORDER BY id",
      db, drawn.sql
    ));
    conn
      .set_scan_program(ranged, Some(&drawn.program))
      .expect("the program");
    input = 0;
    while input < IC_IT_INPUTS as u32 {
      let value = drawn.inputs[input as usize];
      conn.set_scan_input(ranged, input, value).expect("an input");
      input += 1;
    }
    conn
      .add_scan_range(ranged, &between, &low, &high)
      .expect("the range");
    let (mut got, error) =
      scan_rows(&mut conn, &t, ranged, &ScanArgs::default());
    got.sort();
    id_faults(&mut faults, "range", &drawn, index_no, &want, &got, error);
    index_no += 1;
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
}

#[test]
fn where_key_reads_filter_as_mysql_does() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if fill_wh().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "wh");
  let mut queries: Vec<QueryId> = Vec::new();
  while queries.len() < IC_IT_OPS_PER_TRANS {
    let qid = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
    queries.push(qid);
  }
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let te = bind_through(&submitter, "wh");
  let plain = OpShape::new(&te.def, &te.rec, &te.rec).expect("a shape");
  // Every row, and a few that are not there, which no condition passes.
  let mut ids: Vec<i64> = Vec::new();
  let mut id: i64 = 1;
  while id <= IC_IT_WH_ROWS + 5 {
    ids.push(id);
    id += 1;
  }
  let read = ReadKeyArgs {
    kind: ReadKind::Committed,
    abort_option: AbortOption::IgnoreError,
    ..ReadKeyArgs::default()
  };
  let db = IC_IT_DB;
  let mut faults: Vec<String> = Vec::new();
  let mut index_no: u32 = 0;
  while index_no < IC_IT_WHERE_CONDS {
    let drawn = draw_where(&t, index_no);
    let want = sql_ids(&format!(
      "SELECT id FROM {}.wh WHERE {} ORDER BY id",
      db, drawn.sql
    ));

    // On the connection, a hundred reads a transaction.
    let mut got: Vec<i64> = Vec::new();
    let mut odd: Option<IcError> = None;
    for chunk in ids.chunks(IC_IT_OPS_PER_TRANS) {
      let tid = conn
        .start_transaction(TransactionHint::Any, Some(&t.def))
        .expect("a transaction");
      let mut slot: usize = 0;
      while slot < chunk.len() {
        let id = &chunk[slot];
        let qid = queries[slot];
        let query = conn.query_mut(qid).expect("the query");
        query
          .set_program(Some(&drawn.program))
          .expect("the program");
        let mut input: u32 = 0;
        while input < IC_IT_INPUTS as u32 {
          let value = drawn.inputs[input as usize];
          query.set_input(input, value).expect("an input");
          input += 1;
        }
        t.put(query.key_row_mut(), "id", &id.to_string());
        conn.read_key(qid, tid, &read).expect("define the read");
        slot += 1;
      }
      conn.commit_transaction(tid).expect("commit");
      assert_eq!(finish(&mut conn, tid), CommitState::Committed);
      conn.close_transaction(tid).expect("close");
      slot = 0;
      while slot < chunk.len() {
        let id = &chunk[slot];
        let query = conn.query(queries[slot]).expect("the query");
        match query.error() {
          None => got.push(*id),
          Some(e) if e.code == key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32 => {}
          Some(e) => odd = Some(e),
        }
        slot += 1;
      }
    }
    id_faults(&mut faults, "read", &drawn, index_no, &want, &got, odd);

    // Through an executor, a hundred reads a request.
    let shape = plain.with_program(&drawn.program).expect("a shape");
    let mut got: Vec<i64> = Vec::new();
    let mut odd: Option<IcError> = None;
    for chunk in ids.chunks(IC_IT_OPS_PER_TRANS) {
      let mut request = Request::new();
      for id in chunk {
        let mut op = KeyOp::read(&shape, ReadKind::Committed);
        op.set_abort_option(AbortOption::IgnoreError);
        let mut input: u32 = 0;
        while input < IC_IT_INPUTS as u32 {
          let value = drawn.inputs[input as usize];
          op.set_input(input, value).expect("an input");
          input += 1;
        }
        te.put(op.key_row_mut(), "id", &id.to_string());
        request.push(op);
      }
      submitter.execute(&mut request).expect("run the request");
      let mut place: usize = 0;
      while place < chunk.len() {
        let id = &chunk[place];
        match request.ops()[place].error() {
          None => got.push(*id),
          Some(e) if e.code == key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32 => {}
          Some(e) => odd = Some(e),
        }
        place += 1;
      }
    }
    id_faults(&mut faults, "op", &drawn, index_no, &want, &got, odd);
    index_no += 1;
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
}

/// Statement `index` of the `assign` group, over the `asg` twins: a
/// condition, or none, and assignments, or none for a delete. No
/// assignment reads a column another assigns, so that MySQL, which
/// assigns from left to right, sees what ours sees.
fn draw_statement(t: &Table, index: u32) -> Drawn {
  let mut d = CondDraw::new(t, &IC_IT_ASG, 5000 + index as u64);
  // A delete always has a condition, so that the twins keep rows.
  let deletes = d.draw.one_in(12);
  let mut top: Option<Cond> = None;
  let mut sql = "TRUE".to_string();
  if deletes || !d.draw.one_in(5) {
    let (c, s) = d.cond(0);
    top = Some(c);
    sql = s;
  }
  let mut sets: Vec<String> = Vec::new();
  if !deletes {
    let target = d.pick(&["v", "w"]);
    let other = if target == "v" { "w" } else { "v" };
    let (value, expr) = match d.draw.below(5) {
      0 => {
        let k = d.draw.range(-50, 50);
        let (c, kv) = (d.column(target), d.b.integer(k));
        (d.b.add(c, kv), format!("{} + {}", target, k))
      }
      1 => {
        let k = d.draw.range(-50, 50);
        let (c, kv) = (d.column(other), d.b.integer(k));
        (d.b.subtract(c, kv), format!("{} - {}", other, k))
      }
      2 => {
        let input = d.draw.below(IC_IT_INPUTS as u32 - 1);
        let (c, i) = (d.column(target), d.b.input(input));
        let sql = format!("{} + {}", target, d.inputs[input as usize]);
        (d.b.add(c, i), sql)
      }
      3 => {
        let column = draw_column(target, Kind::Int, true);
        d.constant(&column)
      }
      _ => {
        let (c, o) = (d.column(target), d.column(other));
        (d.b.add(c, o), format!("{} + {}", target, other))
      }
    };
    let target_id = d.id(target);
    if d.draw.one_in(3) {
      let (c, s) = d.cond(1);
      d.b.assign_if(target_id, value, c);
      sets.push(format!("{} = IF({}, {}, {})", target, s, expr, target));
    } else {
      d.b.assign(target_id, value);
      sets.push(format!("{} = {}", target, expr));
    }
    if d.draw.one_in(3) {
      let (k, literal) = d.constant(&draw_column("name", Kind::Name, true));
      let name_id = d.id("name");
      d.b.assign(name_id, k);
      sets.push(format!("name = {}", literal));
    }
    if d.draw.one_in(4) {
      let input = IC_IT_INPUTS as u32 - 1;
      let (c, i) = (d.column("cnt"), d.b.input(input));
      let sum = d.b.add(c, i);
      let cnt_id = d.id("cnt");
      d.b.assign(cnt_id, sum);
      let value = d.inputs[input as usize];
      sets.push(format!("cnt = cnt + {}", value));
    }
  }
  let program = match d.b.finish(top) {
    Ok(program) => program,
    Err(e) => panic!(
      "statement {}: {} ({}): {} SET {}",
      index,
      e.message(),
      e.code,
      sql,
      sets.join(", ")
    ),
  };
  Drawn {
    program,
    inputs: d.inputs,
    sql,
    sets: sets.join(", "),
  }
}

/// A table's rows as MySQL prints them, in id order.
fn rows_of(table: &str) -> String {
  let sql = format!("SELECT * FROM {}.{} ORDER BY id", IC_IT_DB, table);
  mysql(&sql).expect("a MySQL client")
}

#[test]
fn assign_updates_and_deletes_as_mysql_does() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if fill_asg().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "asg_api");
  let id_column = t.def.field_id("id").expect("id");
  // An update writes only what its program assigns: its record holds
  // the key, which an update leaves out.
  let key_rec = Record::default_for(&t.def, Some(&[id_column])).expect("key");
  let mut queries: Vec<QueryId> = Vec::new();
  while queries.len() < IC_IT_OPS_PER_TRANS {
    let qid = conn
      .create_query(&t.def, &key_rec, &key_rec)
      .expect("a query");
    queries.push(qid);
  }
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let te = bind_through(&submitter, "asg_api");
  let te_key = Record::default_for(&te.def, Some(&[id_column])).expect("key");
  let plain = OpShape::new(&te.def, &te_key, &te_key).expect("a shape");
  let mut ids: Vec<i64> = Vec::new();
  let mut id: i64 = 1;
  while id <= IC_IT_ASG_ROWS {
    ids.push(id);
    id += 1;
  }
  let db = IC_IT_DB;
  let mut index_no: u32 = 0;
  while index_no < IC_IT_ASSIGN_STATEMENTS {
    let drawn = draw_statement(&t, index_no);
    let deletes = drawn.sets.is_empty();
    let statement = if deletes {
      format!("DELETE FROM {}.asg_sql WHERE {}", db, drawn.sql)
    } else {
      format!(
        "UPDATE {}.asg_sql SET {} WHERE {}",
        db, drawn.sets, drawn.sql
      )
    };
    mysql(&statement).expect("MySQL runs it");
    let kind = if deletes {
      WriteKind::Delete
    } else {
      WriteKind::Update
    };
    // A row the condition refuses, or one gone, fails alone.
    let mut odd: Vec<String> = Vec::new();
    if index_no % 2 == 0 {
      let write = WriteKeyArgs {
        kind,
        abort_option: AbortOption::IgnoreError,
        ..WriteKeyArgs::default()
      };
      for chunk in ids.chunks(IC_IT_OPS_PER_TRANS) {
        let tid = conn
          .start_transaction(TransactionHint::Any, Some(&t.def))
          .expect("a transaction");
        let mut slot: usize = 0;
        while slot < chunk.len() {
          let id = &chunk[slot];
          let qid = queries[slot];
          let query = conn.query_mut(qid).expect("the query");
          query
            .set_program(Some(&drawn.program))
            .expect("the program");
          let mut input: u32 = 0;
          while input < IC_IT_INPUTS as u32 {
            let value = drawn.inputs[input as usize];
            query.set_input(input, value).expect("an input");
            input += 1;
          }
          let key = query.key_row_mut();
          text_row::put_value(&key_rec, 0, &id.to_string(), key).expect("id");
          conn.write_key(qid, tid, &write).expect("define the write");
          slot += 1;
        }
        conn.commit_transaction(tid).expect("commit");
        let state = finish(&mut conn, tid);
        if state != CommitState::Committed {
          odd.push(format!("{:?}", conn.transaction(tid)));
        }
        conn.close_transaction(tid).expect("close");
        slot = 0;
        while slot < chunk.len() {
          let query = conn.query(queries[slot]).expect("the query");
          if let Some(e) = query.error() {
            if e.code != key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32 {
              odd.push(format!(
                "id {}: {} ({})",
                chunk[slot],
                e.code,
                e.message()
              ));
            }
          }
          slot += 1;
        }
      }
    } else {
      let shape = plain.with_program(&drawn.program).expect("a shape");
      for chunk in ids.chunks(IC_IT_OPS_PER_TRANS) {
        let mut request = Request::new();
        for id in chunk {
          let mut op = KeyOp::write(&shape, kind);
          op.set_abort_option(AbortOption::IgnoreError);
          let mut input: u32 = 0;
          while input < IC_IT_INPUTS as u32 {
            let value = drawn.inputs[input as usize];
            op.set_input(input, value).expect("an input");
            input += 1;
          }
          let key = op.key_row_mut();
          text_row::put_value(&te_key, 0, &id.to_string(), key).expect("id");
          request.push(op);
        }
        if let Err(e) = submitter.execute(&mut request) {
          odd.push(format!("request: {} ({})", e.code, e.message()));
        }
        let mut place: usize = 0;
        while place < chunk.len() {
          let id = &chunk[place];
          if let Some(e) = request.ops()[place].error() {
            if e.code != key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32 {
              odd.push(format!("id {}: {} ({})", id, e.code, e.message()));
            }
          }
          place += 1;
        }
      }
    }
    let ours = rows_of("asg_api");
    let theirs = rows_of("asg_sql");
    if !odd.is_empty() || ours != theirs {
      let mut differ: Vec<String> = Vec::new();
      for (mine, sql) in ours.lines().zip(theirs.lines()) {
        if mine != sql {
          differ.push(format!("ours {} / MySQL's {}", mine, sql));
        }
      }
      differ.truncate(10);
      panic!(
        "statement {} ({}): {}\ninputs {:?}\n{}\n{}\n{} rows ours, {} MySQL's",
        index_no,
        if index_no % 2 == 0 {
          "connection"
        } else {
          "executor"
        },
        statement,
        &drawn.inputs[..IC_IT_INPUTS],
        odd.join("\n"),
        differ.join("\n"),
        ours.lines().count(),
        theirs.lines().count()
      );
    }
    index_no += 1;
  }
}

#[test]
fn assign_the_counter_reads_its_new_value_after() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if fill_asg().is_none() {
    return;
  }
  let db = IC_IT_DB;
  let start = count_of(&format!("SELECT cnt FROM {}.asg_api WHERE id = 1", db));
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "asg_api");
  let id_column = t.def.field_id("id").expect("id");
  let cnt = t.def.field_id("cnt").expect("cnt");
  let key_rec = Record::default_for(&t.def, Some(&[id_column])).expect("key");
  let after_rec = Record::default_for(&t.def, Some(&[cnt])).expect("cnt");
  // cnt = cnt + input 0, as the AUTO_INCREMENT counter takes values.
  let mut b = ProgramBuilder::new(&t.def);
  let c = b.column(cnt);
  let n = b.input(0);
  let sum = b.add(c, n);
  b.assign(cnt, sum);
  let program = b.finish(None).expect("the program");
  assert_eq!(program.inputs(), 1);

  // On the connection.
  let qid = conn
    .create_query(&t.def, &key_rec, &key_rec)
    .expect("a query");
  let query = conn.query_mut(qid).expect("the query");
  query.set_program(Some(&program)).expect("the program");
  query
    .set_reads_after(Some(&after_rec))
    .expect("reads after");
  text_row::put_value(&key_rec, 0, "1", query.key_row_mut()).expect("id");
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  let mut expected = start;
  let mut step: i64 = 1;
  while step <= 20 {
    conn
      .query_mut(qid)
      .expect("the query")
      .set_input(0, step * 3)
      .expect("the input");
    let tid = conn
      .start_transaction(TransactionHint::Any, Some(&t.def))
      .expect("a transaction");
    conn
      .write_key(qid, tid, &update)
      .expect("define the update");
    conn.commit_transaction(tid).expect("commit");
    assert_eq!(finish(&mut conn, tid), CommitState::Committed);
    conn.close_transaction(tid).expect("close");
    expected += step * 3;
    let query = conn.query(qid).expect("the query");
    assert!(query.error().is_none(), "{:?}", query.error());
    let seen = text_row::value_text(&after_rec, 0, query.after_row());
    assert_eq!(seen, expected.to_string(), "step {}", step);
    step += 1;
  }

  // Through an executor, read back into the operation's own row.
  let executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let te = bind_through(&submitter, "asg_api");
  let te_key = Record::default_for(&te.def, Some(&[id_column])).expect("key");
  let te_after = Record::default_for(&te.def, Some(&[cnt])).expect("cnt");
  let shape = OpShape::new(&te.def, &te_key, &te_key)
    .expect("a shape")
    .with_program(&program)
    .expect("with the program")
    .reading_after(&te_after)
    .expect("reading after");
  let mut request = Request::new();
  let mut op = KeyOp::write(&shape, WriteKind::Update);
  text_row::put_value(&te_key, 0, "1", op.key_row_mut()).expect("id");
  request.push(op);
  step = 1;
  while step <= 20 {
    request.ops_mut()[0].set_input(0, step).expect("the input");
    submitter.execute(&mut request).expect("run the request");
    let op = &request.ops()[0];
    assert!(op.error().is_none(), "{:?}", op.error());
    expected += step;
    let seen = text_row::value_text(&te_after, 0, op.after_row());
    assert_eq!(seen, expected.to_string(), "executor step {}", step);
    step += 1;
  }
  let now = count_of(&format!("SELECT cnt FROM {}.asg_api WHERE id = 1", db));
  assert_eq!(now, expected, "what MySQL reads");
}

#[test]
fn assign_a_missed_condition_aborts_only_where_asked() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if fill_asg().is_none() {
    return;
  }
  let db = IC_IT_DB;
  mysql(&format!(
    "UPDATE {db}.asg_api SET v = 1, w = 0 WHERE id = 1;
     UPDATE {db}.asg_api SET v = 2, w = 0 WHERE id = 2;",
    db = db
  ));
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "asg_api");
  let id_column = t.def.field_id("id").expect("id");
  let v = t.def.field_id("v").expect("v");
  let key_rec = Record::default_for(&t.def, Some(&[id_column])).expect("key");
  // WHERE v > 100, which no row meets; v = v + 1.
  let mut b = ProgramBuilder::new(&t.def);
  let c = b.column(v);
  let hundred = b.input(0);
  let top = b.compare(Cmp::Gt, c, hundred);
  let one = b.integer(1);
  let sum = b.add(c, one);
  b.assign(v, sum);
  let program = b.finish(Some(top)).expect("the program");
  let plain = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let guarded = conn
    .create_query(&t.def, &key_rec, &key_rec)
    .expect("a query");
  {
    let query = conn.query_mut(guarded).expect("the query");
    query.set_program(Some(&program)).expect("the program");
    query.set_input(0, 100).expect("the input");
    text_row::put_value(&key_rec, 0, "2", query.key_row_mut()).expect("id");
  }

  // With the writes' default, abort on error: the transaction rolls
  // back, the plain update with it, and says 6000.
  for (option, aborts, tried) in [
    (AbortOption::Default, true, 1i64),
    (AbortOption::IgnoreError, false, 2i64),
  ] {
    let mut row = t.row();
    t.put(&mut row, "id", "1");
    t.put(&mut row, "v", "1");
    t.put(&mut row, "w", &tried.to_string());
    t.put(&mut row, "cnt", "0");
    t.put(&mut row, "name", "NULL");
    let query = conn.query_mut(plain).expect("the query");
    query.key_row_mut().copy_from_slice(&row);
    query.attr_row_mut().copy_from_slice(&row);
    let tid = conn
      .start_transaction(TransactionHint::Any, Some(&t.def))
      .expect("a transaction");
    let write = WriteKeyArgs {
      kind: WriteKind::Update,
      ..WriteKeyArgs::default()
    };
    conn
      .write_key(plain, tid, &write)
      .expect("the plain update");
    let write = WriteKeyArgs {
      kind: WriteKind::Update,
      abort_option: option,
      ..WriteKeyArgs::default()
    };
    conn
      .write_key(guarded, tid, &write)
      .expect("the guarded update");
    conn.commit_transaction(tid).expect("commit");
    let state = finish(&mut conn, tid);
    let query_error = conn.query(guarded).expect("the query").error();
    let trans_error = conn.transaction(tid).expect("the transaction").error();
    conn.close_transaction(tid).expect("close");
    let w = count_of(&format!("SELECT w FROM {}.asg_api WHERE id = 1", db));
    let v2 = count_of(&format!("SELECT v FROM {}.asg_api WHERE id = 2", db));
    assert_eq!(v2, 2, "the guarded row is left as it was");
    if aborts {
      assert_eq!(state, CommitState::RolledBack, "{:?}", trans_error);
      let codes = [query_error, trans_error];
      let mut said: bool = false;
      for code in codes.into_iter().flatten() {
        if code.code == 6000 {
          said = true;
        }
      }
      assert!(said, "6000 said: {:?} {:?}", query_error, trans_error);
      assert_eq!(w, 0, "the plain update rolled back with it");
    } else {
      assert_eq!(state, CommitState::Committed, "{:?}", trans_error);
      assert_eq!(
        query_error.map(|e| e.code),
        Some(key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32),
        "626, as a row not there"
      );
      assert_eq!(w, tried, "the plain update committed");
    }
  }

  // A read that aborts on error, of a row its filter refuses.
  let mut b = ProgramBuilder::new(&t.def);
  let c = b.column(v);
  let hundred = b.input(0);
  let top = b.compare(Cmp::Gt, c, hundred);
  let filter = b.finish(Some(top)).expect("the filter");
  let reader = conn.create_query(&t.def, &t.rec, &t.rec).expect("a query");
  let query = conn.query_mut(reader).expect("the query");
  query.set_program(Some(&filter)).expect("the filter");
  query.set_input(0, 100).expect("the input");
  t.put(query.key_row_mut(), "id", "2");
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let read = ReadKeyArgs {
    kind: ReadKind::Locked,
    abort_option: AbortOption::AbortOnError,
    ..ReadKeyArgs::default()
  };
  conn.read_key(reader, tid, &read).expect("the read");
  conn.commit_transaction(tid).expect("commit");
  let state = finish(&mut conn, tid);
  let query_error = conn.query(reader).expect("the query").error();
  let trans_error = conn.transaction(tid).expect("the transaction").error();
  assert_eq!(state, CommitState::RolledBack, "{:?}", trans_error);
  let mut said: bool = false;
  for code in [query_error, trans_error].into_iter().flatten() {
    if code.code == 6000 {
      said = true;
    }
  }
  assert!(said, "6000 said: {:?} {:?}", query_error, trans_error);
}

// ---- AUTO_INCREMENT (phase 9.2a) ----

/// The values of `id` MySQL's inserts by `who` took, at or above `low`
/// and below `high`.
fn ids_between(who: &str, low: u64, high: u64) -> i64 {
  count_of(&format!(
    "SELECT COUNT(*) FROM {}.ai_api WHERE who = '{}' AND id >= {} \
     AND id < {}",
    IC_IT_DB, who, low, high
  ))
}

#[test]
fn auto_increment_values_come_from_the_counter_mysql_uses() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let made = mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ai_api;
     DROP TABLE IF EXISTS {db}.ai_plain;
     CREATE TABLE {db}.ai_api (
       id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
       who CHAR(1) NOT NULL
     ) ENGINE=NDB DEFAULT CHARSET=latin1;
     CREATE TABLE {db}.ai_plain (id INT NOT NULL PRIMARY KEY) ENGINE=NDB;
     INSERT INTO {db}.ai_api (who) VALUES ('m'), ('m'), ('m');",
    db = IC_IT_DB
  ));
  if made.is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "ai_api");
  let id_column = t.def.field_id("id").expect("id");
  assert_eq!(t.def.auto_increment_column(), Some(id_column));
  assert_eq!(t.def.hidden_key(), None);
  let plain = bind(&mut conn, "ai_plain");
  assert_eq!(
    conn.auto_increment(&plain.def),
    Err(IcError::new(err::IC_ERROR_NO_AUTO_INCREMENT))
  );

  // The counter is past every value MySQL reserved, and ours start
  // there: 4096 reserved at once, then handed out in turn.
  let mysqls = count_of(&format!("SELECT MAX(id) FROM {}.ai_api", IC_IT_DB));
  let before = conn.auto_increment_peek(&t.def).expect("peek");
  assert!(before > mysqls as u64, "{} after {}", before, mysqls);
  let first = conn.auto_increment(&t.def).expect("a value");
  assert!(first >= before, "{} below the counter {}", first, before);
  let after = conn.auto_increment_peek(&t.def).expect("peek");
  assert!(after >= first + 4096, "the counter at {}", after);
  let second = conn.auto_increment(&t.def).expect("a value");
  assert_eq!(second, first + 1);
  // Another connection of the process takes from the same range.
  let other = cluster.global.create_connection().expect("a connection");
  let third = other.auto_increment(&t.def).expect("a value");
  assert_eq!(third, second + 1);
  drop(other);

  // Our values go in, and MySQL's next take none of ours.
  mysql(&format!(
    "INSERT INTO {db}.ai_api VALUES ({a}, 'i'), ({b}, 'i'), ({c}, 'i');
     INSERT INTO {db}.ai_api (who) VALUES ('m'), ('m'), ('m'), ('m');",
    db = IC_IT_DB,
    a = first,
    b = second,
    c = third
  ))
  .expect("a MySQL client");
  assert_eq!(ids_between("m", first, first + 4096), 0);

  // A block that what is left cannot hold has a refill of its own.
  let block = conn.auto_increment_block(&t.def, 5000).expect("a block");
  assert!(block >= after, "{} below {}", block, after);
  let after_block = conn.auto_increment_peek(&t.def).expect("peek");
  assert!(
    after_block >= block + 5000,
    "the counter at {}",
    after_block
  );
  let next = conn.auto_increment(&t.def).expect("a value");
  assert!(next >= after_block, "{} below {}", next, after_block);

  // An explicit value beyond the counter moves it past; one below what
  // was handed out moves nothing.
  let high = conn.auto_increment_peek(&t.def).expect("peek") + 100_000;
  conn.auto_increment_seen(&t.def, high).expect("seen");
  let moved = conn.auto_increment_peek(&t.def).expect("peek");
  assert!(moved > high, "the counter at {} after {}", moved, high);
  conn.auto_increment_seen(&t.def, first).expect("seen");
  assert_eq!(conn.auto_increment_peek(&t.def).expect("peek"), moved);
  let beyond = conn.auto_increment(&t.def).expect("a value");
  assert!(beyond > high, "{} not past {}", beyond, high);

  // MySQL's next values are past what we said too, once its own
  // reserved values are used: none lands on ours.
  mysql(&format!(
    "INSERT INTO {db}.ai_api VALUES ({v}, 'i');
     INSERT INTO {db}.ai_api (who) VALUES ('m'), ('m');",
    db = IC_IT_DB,
    v = beyond
  ))
  .expect("a MySQL client");
  let total = count_of(&format!("SELECT COUNT(*) FROM {}.ai_api", IC_IT_DB));
  assert_eq!(total, 3 + 3 + 4 + 1 + 2);
}

// ---- AUTO_INCREMENT through an executor (phase 9.2b) ----

/// The submitter and table the callback of the test below asks with.
type AiCallbackWith = Option<(Submitter, Arc<TableDef>)>;

static AI_CALLBACK_WITH: std::sync::Mutex<AiCallbackWith> =
  std::sync::Mutex::new(None);
/// The value the callback was given, 0 for an error.
static AI_CALLBACK_VALUE: std::sync::atomic::AtomicU64 =
  std::sync::atomic::AtomicU64::new(0);
/// 0 until the callback has run, then 1, or 2 on the executor's thread.
static AI_CALLBACK_RAN: std::sync::atomic::AtomicUsize =
  std::sync::atomic::AtomicUsize::new(0);

/// Ask for a value on the executor's own thread, the first of the
/// table's, so that the refill goes from there. Nothing in it may
/// panic, as a panic stops the executor.
fn take_in_callback(_request: Request, _user_ref: usize) {
  use std::sync::atomic::Ordering;
  let name = std::thread::current().name().unwrap_or("").to_string();
  let mut value: u64 = 0;
  if let Ok(with) = AI_CALLBACK_WITH.lock() {
    if let Some((submitter, def)) = with.as_ref() {
      value = submitter.auto_increment(def).unwrap_or(0);
    }
  }
  AI_CALLBACK_VALUE.store(value, Ordering::Relaxed);
  let ran = if name.starts_with("ic_executor_") {
    2
  } else {
    1
  };
  AI_CALLBACK_RAN.store(ran, Ordering::Release);
}

#[test]
fn auto_increment_through_an_executor_shares_the_range() {
  use std::sync::atomic::Ordering;
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let made = mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ai_exec;
     CREATE TABLE {db}.ai_exec (
       id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
       who CHAR(1) NOT NULL
     ) ENGINE=NDB DEFAULT CHARSET=latin1;
     INSERT INTO {db}.ai_exec (who) VALUES ('m'), ('m');",
    db = IC_IT_DB
  ));
  if made.is_none() {
    return;
  }
  let mut executor = executor_with(&cluster, 64, 256);
  let mut submitter = executor.submitter();
  let def = submitter.table_bind(IC_IT_DB, "ai_exec").expect("bind");
  let rec = Record::default_for(&def, None).expect("a record");
  let shape = OpShape::new(&def, &rec, &rec).expect("a shape");
  let t = Table { def, rec };
  let mut conn = cluster.global.create_connection().expect("a connection");
  let on_conn = bind(&mut conn, "ai_exec");

  // The first value of the table is asked for in a callback on the
  // executor's own thread: its refill goes from there, on the
  // counter's connection, and the executor is not held up for good.
  let before = submitter.auto_increment_peek(&t.def).expect("peek");
  if let Ok(mut with) = AI_CALLBACK_WITH.lock() {
    *with = Some((executor.submitter(), Arc::clone(&t.def)));
  }
  submitter
    .submit_wait(Request::new(), Done::Callback(take_in_callback, 0))
    .expect("submitted");
  let start = time::gethrtime();
  while AI_CALLBACK_RAN.load(Ordering::Acquire) == 0 {
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_TRANS_WAIT_MS, "the callback did not come");
    time::microsleep(1_000);
  }
  if let Ok(mut with) = AI_CALLBACK_WITH.lock() {
    *with = None;
  }
  assert_eq!(
    AI_CALLBACK_RAN.load(Ordering::Acquire),
    2,
    "on the executor"
  );
  let first = AI_CALLBACK_VALUE.load(Ordering::Relaxed);
  assert!(first >= before, "{} below the counter {}", first, before);
  let after = submitter.auto_increment_peek(&t.def).expect("peek");
  assert!(after >= first + 4096, "the counter at {}", after);

  // A connection and the submitter take from the same range, the
  // table bound through each.
  let second = conn.auto_increment(&on_conn.def).expect("a value");
  assert_eq!(second, first + 1);
  let third = submitter.auto_increment(&t.def).expect("a value");
  assert_eq!(third, second + 1);
  let block = submitter.auto_increment_block(&t.def, 10).expect("a block");
  assert_eq!(block, third + 1);
  let fourth = conn.auto_increment(&on_conn.def).expect("a value");
  assert_eq!(fourth, block + 10);

  // The values go in through the executor, and MySQL's next take none
  // of them.
  let mut request = Request::new();
  for id in [first, second, third, block, fourth] {
    let mut op = KeyOp::write(&shape, WriteKind::Insert);
    t.put(op.key_row_mut(), "id", &id.to_string());
    t.put(op.attr_row_mut(), "id", &id.to_string());
    t.put(op.attr_row_mut(), "who", "e");
    request.push(op);
  }
  submitter.execute(&mut request).expect("the inserts commit");
  mysql(&format!(
    "INSERT INTO {}.ai_exec (who) VALUES ('m'), ('m'), ('m');",
    IC_IT_DB
  ))
  .expect("a MySQL client");
  let total = count_of(&format!("SELECT COUNT(*) FROM {}.ai_exec", IC_IT_DB));
  assert_eq!(total, 2 + 5 + 3);

  // An explicit value past the counter, said through the submitter,
  // moves it for the connection too.
  let high = submitter.auto_increment_peek(&t.def).expect("peek") + 50_000;
  submitter.auto_increment_seen(&t.def, high).expect("seen");
  let moved = conn.auto_increment_peek(&on_conn.def).expect("peek");
  assert!(moved > high, "the counter at {} after {}", moved, high);
  let beyond = conn.auto_increment(&on_conn.def).expect("a value");
  assert!(beyond > high, "{} not past {}", beyond, high);

  // A stopped executor's submitter refuses them, as it does everything.
  executor.stop();
  assert_eq!(
    submitter.auto_increment(&t.def),
    Err(IcError::new(err::IC_ERROR_EXECUTOR_STOPPED))
  );
  assert!(
    conn.auto_increment(&on_conn.def).is_ok(),
    "the connection's"
  );
}

// ---- AUTO_INCREMENT, the exit (phase 9.2c) ----

/// Values a refill reserves in the exit's table, few so that ours and
/// MySQL's take from the counter in turn many times over.
const IC_IT_AI_PREFETCH: u32 = 7;
/// Rows a transaction of ours inserts.
const IC_IT_AI_BATCH: usize = 20;
/// Rounds of MySQL's rows, then ours.
const IC_IT_AI_ROUNDS: i64 = 10;

/// How many rows of a table in the test database `filter` holds for.
fn count_where(table: &str, filter: &str) -> i64 {
  count_of(&format!(
    "SELECT COUNT(*) FROM {}.{} WHERE {}",
    IC_IT_DB, table, filter
  ))
}

/// `count` values of a table's AUTO_INCREMENT column, a call each.
fn take_ids(conn: &ApidConnection, t: &Table, count: usize) -> Vec<u64> {
  let mut ids: Vec<u64> = Vec::with_capacity(count);
  while ids.len() < count {
    ids.push(conn.auto_increment(&t.def).expect("a value"));
  }
  ids
}

/// Insert rows in one transaction, one on each query given, each row
/// its columns' values as text, and wait for it to commit.
fn insert_rows_as_text(
  conn: &mut ApidConnection,
  t: &Table,
  queries: &[QueryId],
  rows: &[Vec<(&str, String)>],
) {
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("a transaction");
  let insert = WriteKeyArgs {
    kind: WriteKind::Insert,
    ..WriteKeyArgs::default()
  };
  let mut i: usize = 0;
  while i < rows.len() {
    let query = conn.query_mut(queries[i]).expect("the query");
    for (column, text) in &rows[i] {
      t.put(query.key_row_mut(), column, text);
      t.put(query.attr_row_mut(), column, text);
    }
    conn
      .write_key(queries[i], tid, &insert)
      .expect("define the insert");
    i += 1;
  }
  conn.commit_transaction(tid).expect("ask to commit");
  let state = finish(conn, tid);
  let error = conn.transaction(tid).expect("the transaction").error();
  assert_eq!(state, CommitState::Committed, "{:?}", error);
  conn.close_transaction(tid).expect("close");
}

/// Rows of `ai_exit` for our side: `ids`, by `who`, `n` each.
fn exit_rows(
  ids: &[u64],
  who: &str,
  n: i64,
) -> Vec<Vec<(&'static str, String)>> {
  let mut rows: Vec<Vec<(&'static str, String)>> =
    Vec::with_capacity(ids.len());
  for id in ids {
    rows.push(vec![
      ("id", id.to_string()),
      ("who", who.to_string()),
      ("n", n.to_string()),
    ]);
  }
  rows
}

/// An insert of `count` rows by MySQL into `ai_exit`, the ids its own.
fn mysql_exit_rows(count: u32, n: i64) -> String {
  let mut values: Vec<String> = Vec::with_capacity(count as usize);
  while values.len() < count as usize {
    values.push(format!("('m', {})", n));
  }
  format!(
    "INSERT INTO {}.ai_exit (who, n) VALUES {};\n",
    IC_IT_DB,
    values.join(", ")
  )
}

#[test]
fn auto_increment_inserts_beside_mysqls_give_no_duplicate() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let made = mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ai_exit;
     CREATE TABLE {db}.ai_exit (
       id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
       who CHAR(1) NOT NULL,
       n INT NOT NULL
     ) ENGINE=NDB DEFAULT CHARSET=latin1;",
    db = IC_IT_DB
  ));
  if made.is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "ai_exit");
  let queries = takeover_queries(&mut conn, &t, IC_IT_AI_BATCH);
  t.def.set_auto_increment_prefetch(IC_IT_AI_PREFETCH);

  // In turn: a hundred rows of MySQL's, then twenty of ours, each value
  // asked for alone, ten times over.
  let mut lowest = u64::MAX;
  let mut round: i64 = 0;
  while round < IC_IT_AI_ROUNDS {
    mysql(&mysql_exit_rows(100, round)).expect("a MySQL client");
    let ids = take_ids(&conn, &t, IC_IT_AI_BATCH);
    for id in &ids {
      lowest = lowest.min(*id);
    }
    insert_rows_as_text(&mut conn, &t, &queries, &exit_rows(&ids, "i", round));
    round += 1;
  }

  // At once: five hundred rows of MySQL's, ten to a statement, from a
  // thread of their own while five hundred of ours go in.
  let mut script = String::new();
  let mut statements: u32 = 0;
  while statements < 50 {
    script.push_str(&mysql_exit_rows(10, 99));
    statements += 1;
  }
  let theirs = std::thread::spawn(move || mysql(&script));
  let mut batch: usize = 0;
  while batch < 25 {
    let ids = take_ids(&conn, &t, IC_IT_AI_BATCH);
    insert_rows_as_text(&mut conn, &t, &queries, &exit_rows(&ids, "i", 99));
    batch += 1;
  }
  theirs
    .join()
    .expect("MySQL's inserts")
    .expect("a MySQL client");
  assert_eq!(count_where("ai_exit", "who = 'i'"), 700);
  assert_eq!(count_where("ai_exit", "who = 'm'"), 1500);
  // MySQL refilled after our first values: theirs and ours interleave.
  let after = format!("who = 'm' AND id > {}", lowest);
  assert!(
    count_where("ai_exit", &after) > 0,
    "every value of MySQL's below ours"
  );

  // MySQL's explicit value past the counter moves it for us: a block
  // larger than the prefetch, a refill of its own, starts past it.
  let theirs_high = conn.auto_increment_peek(&t.def).expect("peek") + 100_000;
  mysql(&format!(
    "INSERT INTO {}.ai_exit VALUES ({}, 'x', 0);",
    IC_IT_DB, theirs_high
  ))
  .expect("a MySQL client");
  let past = conn
    .auto_increment_block(&t.def, IC_IT_AI_PREFETCH + 1)
    .expect("a block");
  assert!(
    past > theirs_high,
    "{} not past MySQL's {}",
    past,
    theirs_high
  );

  // Ours moves it for MySQL. Its own explicit value left its range
  // empty, so the next rows it inserts come from a refill past ours.
  let mine = conn.auto_increment_peek(&t.def).expect("peek") + 100_000;
  insert_rows_as_text(&mut conn, &t, &queries, &exit_rows(&[mine], "y", 0));
  conn.auto_increment_seen(&t.def, mine).expect("seen");
  mysql(&mysql_exit_rows(600, 100)).expect("a MySQL client");
  assert_eq!(
    count_where(
      "ai_exit",
      &format!("who = 'm' AND n = 100 AND id <= {}", mine)
    ),
    0
  );
  let total = count_of(&format!("SELECT COUNT(*) FROM {}.ai_exit", IC_IT_DB));
  assert_eq!(total, 700 + 1500 + 1 + 1 + 600);
}

#[test]
fn auto_increment_keeps_the_step_and_offset_but_not_for_a_hidden_key() {
  let options = GlobalOptions {
    auto_increment_step: 10,
    auto_increment_offset: 3,
    ..GlobalOptions::default()
  };
  let cluster = match cluster_with_options(&options, 30_000) {
    Some(cluster) => cluster,
    None => return,
  };
  let made = mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ai_step;
     DROP TABLE IF EXISTS {db}.ai_nopk;
     CREATE TABLE {db}.ai_step (
       id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
       v INT
     ) ENGINE=NDB;
     CREATE TABLE {db}.ai_nopk (a INT NOT NULL, b VARCHAR(10))
       ENGINE=NDB DEFAULT CHARSET=latin1;
     INSERT INTO {db}.ai_step (v) VALUES (1);
     INSERT INTO {db}.ai_nopk VALUES (1, 'm');",
    db = IC_IT_DB
  ));
  if made.is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "ai_step");
  let h = bind(&mut conn, "ai_nopk");

  // 10 k + 3, and a refill reserves 4096 steps.
  let before = conn.auto_increment_peek(&t.def).expect("peek");
  let first = conn.auto_increment(&t.def).expect("a value");
  assert_eq!(first % 10, 3, "{}", first);
  assert!(
    first >= before && first < before + 10,
    "{} at {}",
    first,
    before
  );
  let after = conn.auto_increment_peek(&t.def).expect("peek");
  assert!(after >= before + 40_960, "the counter at {}", after);
  let second = conn.auto_increment(&t.def).expect("a value");
  assert_eq!(second, first + 10);
  let block = conn.auto_increment_block(&t.def, 3).expect("a block");
  assert_eq!(block, second + 10);
  let next = conn.auto_increment(&t.def).expect("a value");
  assert_eq!(next, block + 30);
  mysql(&format!(
    "INSERT INTO {}.ai_step VALUES ({}, 2), ({}, 2);",
    IC_IT_DB, first, next
  ))
  .expect("a MySQL client");

  // The hidden key takes 1 and 1, as MySQL servers give it.
  assert!(h.def.hidden_key().is_some(), "no hidden key");
  let before = conn.auto_increment_peek(&h.def).expect("peek");
  let first = conn.auto_increment(&h.def).expect("a value");
  assert!(first >= before, "{} below the counter {}", first, before);
  let after = conn.auto_increment_peek(&h.def).expect("peek");
  assert!(after >= first + 4096, "the counter at {}", after);
  assert!(after < first + 40_960, "steps reserved: {}", after);
  let second = conn.auto_increment(&h.def).expect("a value");
  assert_eq!(second, first + 1);
  let block = conn.auto_increment_block(&h.def, 3).expect("a block");
  assert_eq!(block, second + 1);
  assert_eq!(conn.auto_increment(&h.def).expect("a value"), block + 3);
}

#[test]
fn auto_increment_a_table_without_a_primary_key_works_by_its_hidden_key() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let made = mysql(&format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.ai_nopk;
     CREATE TABLE {db}.ai_nopk (a INT NOT NULL, b VARCHAR(10))
       ENGINE=NDB DEFAULT CHARSET=latin1;
     INSERT INTO {db}.ai_nopk VALUES (1, 'm'), (2, 'm'), (3, 'm');",
    db = IC_IT_DB
  ));
  if made.is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "ai_nopk");

  // `$PK` as the dictionary has it: the last column, the key alone,
  // the AUTO_INCREMENT column, BIGINT UNSIGNED, never NULL.
  let pk = t.def.hidden_key().expect("a hidden key");
  let column = t.def.field(pk).expect("the column");
  assert_eq!(column.name, "$PK");
  assert!(column.primary_key && !column.nullable);
  assert_eq!(
    column.type_code,
    ic_signals::table_props::IC_TYPE_BIGUNSIGNED
  );
  assert_eq!(pk + 1, t.def.num_fields());
  assert_eq!(t.def.auto_increment_column(), Some(pk));

  // Inserts take their keys from the counter, as MySQL's do: neither
  // takes the other's.
  let queries = takeover_queries(&mut conn, &t, 40);
  let keys = take_ids(&conn, &t, 40);
  let mut rows: Vec<Vec<(&str, String)>> = Vec::with_capacity(keys.len());
  let mut i: usize = 0;
  while i < keys.len() {
    rows.push(vec![
      ("$PK", keys[i].to_string()),
      ("a", (100 + i).to_string()),
      ("b", "i".to_string()),
    ]);
    i += 1;
  }
  insert_rows_as_text(&mut conn, &t, &queries, &rows);
  mysql(&format!(
    "INSERT INTO {}.ai_nopk VALUES (4, 'm'), (5, 'm'), (6, 'm');",
    IC_IT_DB
  ))
  .expect("a MySQL client");
  assert_eq!(count_where("ai_nopk", "1 = 1"), 46);

  // A read, an update and a delete by the hidden key, as on any table.
  let mut key = t.row();
  t.put(&mut key, "$PK", &keys[5].to_string());
  let mut out = t.row();
  let found = key_op::read_committed(&mut conn, &t.rec, &key, &t.rec, &mut out)
    .expect("a read");
  assert!(found, "no row by its hidden key");
  assert_eq!(t.get(&out, "a"), "105");
  t.put(&mut out, "b", "k");
  key_op::write_key(
    &mut conn,
    key_ops::IC_OP_UPDATE,
    &t.rec,
    &key,
    &t.rec,
    &out,
  )
  .expect("an update");
  let mut gone = t.row();
  t.put(&mut gone, "$PK", &keys[6].to_string());
  key_op::write_key(
    &mut conn,
    key_ops::IC_OP_DELETE,
    &t.rec,
    &gone,
    &t.rec,
    &gone,
  )
  .expect("a delete");
  assert_eq!(count_where("ai_nopk", "a = 105 AND b = 'k'"), 1);
  assert_eq!(count_where("ai_nopk", "a = 106"), 0);

  // A scan takes our rows over, the key only it returns: even ones
  // updated, odd ones deleted. MySQL's are passed over.
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let mut free = takeover_queries(&mut conn, &t, 50);
  let all = free.len();
  let tid = conn
    .start_transaction(TransactionHint::Any, Some(&t.def))
    .expect("start a transaction");
  let args = ScanArgs {
    lock: ScanLock::Exclusive,
    trans: Some(tid),
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  conn.start_scan(scan, &args).expect("start the scan");
  let mut scanned: i64 = 0;
  while conn.next_row(scan).expect("a row") {
    scanned += 1;
    let row = conn.scan_row(scan);
    // A string comes back quoted.
    if t.get(row, "b") != "'i'" {
      continue;
    }
    let a: i64 = t.get(row, "a").parse().expect("a number");
    let hidden = t.get(row, "$PK");
    take_back(&mut conn, &mut free);
    let qid = free.pop().expect("a free query");
    if a % 2 == 0 {
      let query = conn.query_mut(qid).expect("the query");
      t.put(query.attr_row_mut(), "$PK", &hidden);
      t.put(query.attr_row_mut(), "a", &a.to_string());
      t.put(query.attr_row_mut(), "b", "u");
      conn
        .update_scan_row(qid, scan, &TakeoverArgs::default())
        .expect("take the row over to update it");
    } else {
      conn
        .delete_scan_row(qid, scan, &TakeoverArgs::default())
        .expect("take the row over to delete it");
    }
  }
  assert_eq!(scanned, 45);
  assert!(conn.scan(scan).expect("the scan").error().is_none());
  let _ = conn.send_queries(true);
  let start = time::gethrtime();
  while free.len() < all {
    let _ = conn.flush(100, true);
    take_back(&mut conn, &mut free);
    let waited = time::millis_elapsed(start, time::gethrtime());
    assert!(waited < IC_IT_SCAN_WAIT_MS, "the takeovers did not end");
  }
  conn.commit_transaction(tid).expect("commit");
  assert_eq!(finish(&mut conn, tid), CommitState::Committed);
  conn.close_transaction(tid).expect("close");
  assert_eq!(conn.unexpected(), 0);
  // Of ours, 100 to 139 less 105 and 106: 19 even, 19 odd.
  assert_eq!(count_where("ai_nopk", "b = 'u' AND a % 2 = 0"), 19);
  assert_eq!(count_where("ai_nopk", "b = 'i'"), 0);
  assert_eq!(count_where("ai_nopk", "b = 'm'"), 6);
  assert_eq!(count_where("ai_nopk", "1 = 1"), 6 + 1 + 19);

  // MySQL finds our rows by its own scans, and changes them as its own.
  mysql(&format!(
    "UPDATE {db}.ai_nopk SET b = 'v' WHERE b = 'u';
     DELETE FROM {db}.ai_nopk WHERE b = 'k';",
    db = IC_IT_DB
  ))
  .expect("a MySQL client");
  assert_eq!(count_where("ai_nopk", "b = 'v'"), 19);
  assert_eq!(count_where("ai_nopk", "1 = 1"), 25);
}

// ---- The continuous scan (chapter 06, phase 10) ----

/// A continuous committed read in batches of `batch_rows`, 0 for the
/// configuration's, closed by its deadline if it hangs.
fn continuous(batch_rows: u32) -> ScanArgs {
  ScanArgs {
    continuous: true,
    batch_rows,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  }
}

#[test]
fn continuous_scan_reads_every_row_on_a_connection() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  // Every fragment at once, and one or two at a time in small batches,
  // so that each fragment's batches go round its four receivers many
  // times, the fifth, the ninth and later in its first again, and a
  // receiver's four go on from one fragment of the table to the next.
  for (parallelism, batch_rows) in [(0u32, 0u32), (1, 100), (2, 100), (0, 7)] {
    let args = ScanArgs {
      parallelism,
      ..continuous(batch_rows)
    };
    let (ids, batches, error) = scan_ids(&mut conn, &t, scan, &args);
    let what = format!("parallelism {}, {} rows", parallelism, batch_rows);
    assert!(error.is_none(), "{}: {:?}", what, error);
    assert!(every_row_once(&ids), "{}: {} rows", what, ids.len());
    let run = conn.scan(scan).expect("the scan");
    assert!(run.is_continuous(), "{}", what);
    assert_eq!(run.receivers(), 4 * run.parallelism(), "{}", what);
    if batch_rows == 100 {
      // At most 100 rows a batch: at least 25 batches, more than four
      // for each set of four receivers.
      assert!(batches >= 25, "{}: {} batches", what, batches);
    }
    println!("{}: {} batches", what, batches);
  }
  // Row by row, as next_row reads it.
  conn.start_scan(scan, &continuous(50)).expect("start");
  let mut ids: Vec<i64> = Vec::new();
  while conn.next_row(scan).expect("a row") {
    ids.push(t.get(conn.scan_row(scan), "id").parse().expect("an id"));
  }
  assert!(every_row_once(&ids), "row by row: {} rows", ids.len());
  // Off by default.
  let (ids, _, error) = scan_ids(&mut conn, &t, scan, &ScanArgs::default());
  assert!(error.is_none(), "{:?}", error);
  assert!(every_row_once(&ids), "{} rows", ids.len());
  assert!(!conn.scan(scan).expect("the scan").is_continuous());
  assert_eq!(conn.unexpected(), 0);
  conn.free_scan(scan).expect("free the scan");
}

#[test]
fn continuous_scan_through_an_executor() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() || setup_rng_table().is_none() {
    return;
  }
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  let mut reader = ScanReader::new(&shape);
  for batch_rows in [100u32, 0] {
    // The same reader, run after run.
    let mut run: u32 = 0;
    while run < 2 {
      submitter
        .scan(&mut reader, &continuous(batch_rows))
        .expect("submit the scan");
      let (ids, error) = reader_ids(&mut reader, &t);
      assert!(error.is_none(), "{} rows: {:?}", batch_rows, error);
      assert!(every_row_once(&ids), "{}: {} rows", batch_rows, ids.len());
      run += 1;
    }
  }
  // An index's ranges, unordered and ordered both ways, in small
  // batches.
  let rng = bind_through(&submitter, "rng");
  let columns = ["a", "b"];
  let ab_shape = index_shape(&submitter, &rng, "rng", "ix_ab", &columns);
  let key_rec = record_over(&rng, &columns);
  let mut index_reader = ScanReader::new(&ab_shape);
  let mut faults: Vec<String> = Vec::new();
  for case in &IC_IT_AB_RANGES {
    let ranks = mysql_ranks("rng", &rank_by("a, b"), case.filter);
    let low = bound_row(&rng, &key_rec, &columns, case.low);
    let high = bound_row(&rng, &key_rec, &columns, case.high);
    for (ordered, descending) in [(false, false), (true, false), (true, true)] {
      index_reader
        .add_range(&case.range, &low, &high)
        .expect("add the range");
      let args = ScanArgs {
        ordered,
        descending,
        ..continuous(5)
      };
      submitter
        .scan(&mut index_reader, &args)
        .expect("submit the scan");
      let (rows, error) = reader_rows(&mut index_reader, &rng);
      let what =
        format!("{} ordered {} desc {}", case.filter, ordered, descending);
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, ordered, descending) {
        faults.push(format!("{}: {}", what, fault));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
}

#[test]
fn continuous_scan_ordered_by_each_latin1_collation() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let names = match setup_latin1_table() {
    Some(names) => names,
    None => return,
  };
  // On a connection: the merge reads each fragment's four receivers in
  // turn, in batches of three rows.
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "ordl");
  let faults = ordered_faults(&mut conn, &t, "ordl", &names, true);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(conn.unexpected(), 0);
  // Through an executor, the reader merging.
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let t = bind_through(&submitter, "ordl");
  let mut faults: Vec<String> = Vec::new();
  for column in &names {
    let name = format!("ix_{}", column);
    let shape = index_shape(&submitter, &t, "ordl", &name, &[*column]);
    let mut reader = ScanReader::new(&shape);
    let ranks = mysql_ranks("ordl", &rank_by(column), "TRUE");
    for descending in [false, true] {
      let args = ScanArgs {
        ordered: true,
        descending,
        ..continuous(3)
      };
      submitter.scan(&mut reader, &args).expect("submit the scan");
      let (rows, error) = reader_rows(&mut reader, &t);
      let what = format!("executor {} desc {}", column, descending);
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, true, descending) {
        faults.push(format!("{}: {}", what, fault));
      }
    }
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
}

#[test]
fn continuous_scan_closed_with_fragments_a_batch_ahead() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  let mut run: u32 = 0;
  while run < 3 {
    conn.start_scan(scan, &continuous(50)).expect("start");
    let _ = conn.send_queries(true);
    // A batch taken and kept, while each fragment goes on into its
    // next receiver; then the close.
    let batch = first_batch(&mut conn);
    let start = time::gethrtime();
    while time::millis_elapsed(start, time::gethrtime()) < 20 {
      conn.poll(5);
    }
    conn.close_scan(scan).expect("close");
    until_done(&mut conn, scan);
    let closed = conn.scan(scan).expect("the scan");
    assert!(closed.error().is_none(), "{:?}", closed.error());
    conn
      .release_scan_batch(batch)
      .expect("a stale batch is passed over");
    // Run again at once: every row once, none of the closed run's.
    let (ids, _, error) = scan_ids(&mut conn, &t, scan, &continuous(50));
    assert!(error.is_none(), "{:?}", error);
    assert!(every_row_once(&ids), "{} rows", ids.len());
    run += 1;
  }
  println!(
    "signals of closed runs dropped as late: {}",
    conn.late_scan_signals()
  );
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn continuous_scan_a_slow_reader_stalls_only_its_own_scan() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  // Two scans at a time for six readers: four wait their turn.
  let options = ExecutorOptions {
    scans: 2,
    ..ExecutorOptions::default()
  };
  let executor = Executor::start(&cluster.global, &options).expect("executor");
  let submitter = executor.submitter();
  let (t, shape) = scan_through(&submitter);
  // A reader that reads one row and then holds its batch for a while;
  // its fragments fill their other receivers and then wait.
  let mut slow = ScanReader::new(&shape);
  submitter
    .scan(&mut slow, &continuous(100))
    .expect("submit the slow one");
  assert!(slow.next_row().expect("a row"));
  let held = time::gethrtime();
  std::thread::scope(|scope| {
    let mut handles = Vec::new();
    let mut k: u32 = 0;
    while k < 5 {
      let mine = executor.submitter();
      let (t, shape) = (&t, &shape);
      handles.push(scope.spawn(move || {
        let mut reader = ScanReader::new(shape);
        mine.scan(&mut reader, &continuous(100)).expect("submit");
        let (ids, error) = reader_ids(&mut reader, t);
        assert!(error.is_none(), "{:?}", error);
        assert!(every_row_once(&ids), "{} rows", ids.len());
      }));
      k += 1;
    }
    for handle in handles {
      handle.join().expect("a reader thread");
    }
  });
  let others = time::millis_elapsed(held, time::gethrtime());
  println!("five continuous scans while one was held: {} ms", others);
  // The slow one goes on where it stopped.
  let (ids, error) = reader_ids(&mut slow, &t);
  assert!(error.is_none(), "{:?}", error);
  assert_eq!(ids.len() as i64, IC_IT_SCAN_ROWS - 1);
  assert_eq!(executor.stats().scans, 6);
}

#[test]
fn continuous_scan_refusals() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_scan_table().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let t = bind(&mut conn, "scan");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  // Committed reads only, never a scan that locks.
  for lock in [ScanLock::Shared, ScanLock::Exclusive] {
    let args = ScanArgs {
      lock,
      ..continuous(0)
    };
    let e = conn.start_scan(scan, &args).expect_err("a locking scan");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED, "{:?}", lock);
    assert_eq!(conn.scan(scan).expect("the scan").state(), ScanState::Idle);
  }
  // Through an executor: not with a lock, nor with read_ahead.
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (_, shape) = scan_through(&submitter);
  let mut reader = ScanReader::new(&shape);
  let locked = ScanArgs {
    lock: ScanLock::Shared,
    ..continuous(0)
  };
  let ahead = ScanArgs {
    read_ahead: true,
    ..continuous(0)
  };
  for args in [locked, ahead] {
    let e = submitter.scan(&mut reader, &args).expect_err("refused");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
    assert!(reader.is_done(), "a refused run never began");
  }
}

/// A scan of the million rows, the continuous scan `on` or off, with
/// time enough.
fn million(on: bool) -> ScanArgs {
  ScanArgs {
    continuous: on,
    timeout_ms: 300_000,
    ..ScanArgs::default()
  }
}

/// Print how much faster a scan was with the continuous scan on, from
/// the best times `[off, on]`; where every row is returned, a gain short
/// of 10% is to be looked into (chapter 06, phase 10's exit).
fn report_gain(what: &str, best: [u64; 2], every_row: bool) {
  let off = best[0].max(1);
  let faster = 100 - (best[1] * 100 / off).min(100);
  println!(
    "{}: continuous off {} ms, on {} ms: {}% faster",
    what, best[0], best[1], faster
  );
  if every_row && faster < 10 {
    println!("  less than 10% faster: to be looked into");
  }
}

#[test]
fn continuous_a_scan_of_a_million_rows_on_and_off() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let (t, counted, summed) = match fill_big_table(&submitter) {
    Some(filled) => filled,
    None => return,
  };
  let db = IC_IT_DB;
  // A filter that passes one row in a hundred, val below 10.
  let val = t.def.field_id("val").expect("val");
  let mut b = ProgramBuilder::new(&t.def);
  let column = b.column(val);
  let limit = b.input(0);
  let below = b.compare(Cmp::Lt, column, limit);
  let few = b.finish(Some(below)).expect("the filter");
  let passed =
    count_of(&format!("SELECT COUNT(*) FROM {}.big WHERE val < 10", db));
  let mut conn = cluster.global.create_connection().expect("connection");
  let scan = conn.create_scan(&t.def, &t.rec).expect("create the scan");
  conn.set_scan_input(scan, 0, 10).expect("the bound");
  // Right first, untimed: a continuous run counts and sums what MySQL
  // does.
  conn
    .start_scan(scan, &million(true))
    .expect("start the scan");
  let mut rows: i64 = 0;
  let mut sum: i64 = 0;
  while conn.next_row(scan).expect("a row") {
    rows += 1;
    sum += t
      .get(conn.scan_row(scan), "val")
      .parse::<i64>()
      .expect("val");
  }
  assert_eq!(rows, counted, "the scan counts what MySQL counts");
  assert_eq!(sum, summed, "and sums what it sums");
  // Then timed, the rows counted alone: the text of a row's column took
  // most of the time otherwise, and a client busy with its rows never
  // waits for a batch, which is what the continuous scan saves. Each
  // case off and on, three times, the best of each kept.
  let cases: [(&str, u32, bool, i64); 3] = [
    ("every row, every fragment at once", 0, false, counted),
    ("every row, one fragment at a time", 1, false, counted),
    ("one row in a hundred", 0, true, passed),
  ];
  for (what, parallelism, filtered, expected) in cases {
    let program = if filtered { Some(&few) } else { None };
    conn.set_scan_program(scan, program).expect("the program");
    let mut best: [u64; 2] = [u64::MAX; 2];
    let mut round: u32 = 0;
    while round < 3 {
      for on in [false, true] {
        let args = ScanArgs {
          parallelism,
          ..million(on)
        };
        let start = time::gethrtime();
        conn.start_scan(scan, &args).expect("start the scan");
        let mut rows: i64 = 0;
        while conn.next_row(scan).expect("a row") {
          rows += 1;
        }
        let took = time::millis_elapsed(start, time::gethrtime());
        assert_eq!(rows, expected, "{}", what);
        let k = on as usize;
        best[k] = best[k].min(took);
      }
      round += 1;
    }
    report_gain(what, best, !filtered);
  }
  // Through an executor, off and on.
  let scan_shape = ScanShape::new(&t.def, &t.rec).expect("a scan shape");
  let mut reader = ScanReader::new(&scan_shape);
  let mut best: [u64; 2] = [u64::MAX; 2];
  let mut round: u32 = 0;
  while round < 3 {
    for on in [false, true] {
      let start = time::gethrtime();
      submitter.scan(&mut reader, &million(on)).expect("submit");
      let mut through: i64 = 0;
      while reader.next_row().expect("a row") {
        through += 1;
      }
      let took = time::millis_elapsed(start, time::gethrtime());
      assert_eq!(through, counted, "through an executor too");
      let k = on as usize;
      best[k] = best[k].min(took);
    }
    round += 1;
  }
  report_gain("every row, through an executor", best, true);
  assert_eq!(conn.unexpected(), 0);
  // The data nodes' memory back for the other groups.
  mysql(&format!("DROP TABLE IF EXISTS {}.big", db));
}

// ---- Fanout tables (chapter 06, phase 10) ----

/// A fanout table of the group: its name, its key's columns in key
/// order, how many of them are the base, and how a row's key and id are
/// made from its number m, 0 up, in SQL.
struct FanTable {
  name: &'static str,
  key: &'static [&'static str],
  base: usize,
  rows: i64,
}

/// The group's tables: 1:1:4 over two integers, with an index whose
/// base column is not first; 2:1:4 with a character column in its
/// base; 1:2:4 over 8 partitions with a character column in its
/// detail.
const IC_IT_FAN_TABLES: [FanTable; 3] = [
  FanTable {
    name: "fan_ii",
    key: &["a", "b"],
    base: 1,
    rows: 1000,
  },
  FanTable {
    name: "fan_ci",
    key: &["a", "b", "c"],
    base: 2,
    rows: 1000,
  },
  FanTable {
    name: "fan_ic",
    key: &["a", "b", "c"],
    base: 1,
    rows: 1000,
  },
];

/// Fill `table` afresh with `rows` rows from `select`, an SQL row of
/// m, the row's number from 0, 1 000 rows a statement.
fn fill_fan_rows(table: &str, rows: i64, select: &str) -> Option<()> {
  let mut base: i64 = 0;
  while base < rows {
    let count = (rows - base).min(1000);
    let row = select.replace("{m}", &format!("(n + {} - 1)", base));
    mysql(&format!(
      "INSERT INTO {db}.{table}
         WITH RECURSIVE seq (n) AS
           (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < {count})
         SELECT {row} FROM seq;",
      db = IC_IT_DB,
      table = table,
      count = count,
      row = row
    ))?;
    base += count;
  }
  Some(())
}

/// Make the group's three fanout tables afresh, as the tree's own test
/// makes them (`COMMENT="NDB_TABLE=PARTITION_HASH=base:detail:fanout"`),
/// each row with an id, its number plus one.
fn setup_fan_tables() -> Option<()> {
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.fan_ii, {db}.fan_ci, {db}.fan_ic;
     CREATE TABLE {db}.fan_ii (
       a INT NOT NULL, b INT NOT NULL, id INT NOT NULL, v INT NOT NULL,
       PRIMARY KEY (a, b), KEY ix_va (v, a)
     ) ENGINE=NDB COMMENT=\"NDB_TABLE=PARTITION_HASH=1:1:4\";
     CREATE TABLE {db}.fan_ci (
       a VARCHAR(8) CHARACTER SET latin1 NOT NULL, b INT NOT NULL,
       c INT NOT NULL, id INT NOT NULL,
       PRIMARY KEY (a, b, c)
     ) ENGINE=NDB COMMENT=\"NDB_TABLE=PARTITION_HASH=2:1:4\";
     CREATE TABLE {db}.fan_ic (
       a INT NOT NULL, b INT NOT NULL,
       c CHAR(4) CHARACTER SET latin1 NOT NULL, id INT NOT NULL,
       PRIMARY KEY (a, b, c)
     ) ENGINE=NDB COMMENT=\"NDB_TABLE=PARTITION_HASH=1:2:4\"
       PARTITION BY KEY () PARTITIONS 8;",
    db = IC_IT_DB
  );
  mysql(&sql)?;
  fill_fan_rows(
    "fan_ii",
    1000,
    "{m} DIV 20, {m} MOD 20, {m} + 1, ({m} DIV 20) MOD 5",
  )?;
  fill_fan_rows(
    "fan_ci",
    1000,
    "CONCAT('k', {m} DIV 100), ({m} DIV 10) MOD 10, {m} MOD 10, {m} + 1",
  )?;
  fill_fan_rows(
    "fan_ic",
    1000,
    "{m} DIV 50, ({m} DIV 5) MOD 10, \
     ELT(1 + {m} MOD 5, 'x', 'y', 'z', 'w', 'q'), {m} + 1",
  )
}

/// The key of row m of a group table, column by column in key order, as
/// `setup_fan_tables` makes it.
fn fan_key(table: &str, m: i64) -> Vec<String> {
  let letters = ["x", "y", "z", "w", "q"];
  match table {
    "fan_ii" => vec![(m / 20).to_string(), (m % 20).to_string()],
    "fan_ci" => vec![
      format!("k{}", m / 100),
      ((m / 10) % 10).to_string(),
      (m % 10).to_string(),
    ],
    _ => vec![
      (m / 50).to_string(),
      ((m / 5) % 10).to_string(),
      letters[(m % 5) as usize].to_string(),
    ],
  }
}

#[test]
fn fanout_tables_are_placed_where_the_data_nodes_place_them() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_fan_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  for fan in &IC_IT_FAN_TABLES {
    let t = bind(&mut conn, fan.name);
    let info = t.def.info();
    assert_eq!(info.fanout, 4, "{}", fan.name);
    assert_eq!(info.fanout_base_keys as usize, fan.base, "{}", fan.name);
    assert_eq!(
      info.fanout_detail_keys as usize,
      fan.key.len() - fan.base,
      "{}",
      fan.name
    );
    // Every row where the data nodes put it, and every row of one base
    // in at most the fanout's partitions.
    let mut of_base: HashMap<String, Vec<u32>> = HashMap::new();
    let mut m: i64 = 0;
    while m < fan.rows {
      let key = fan_key(fan.name, m);
      let mut row = t.row();
      let mut k: usize = 0;
      while k < key.len() {
        t.put(&mut row, fan.key[k], &key[k]);
        k += 1;
      }
      let ours = ic_apid::hash::partition_of(&t.def, &t.rec, &row)
        .expect("a fanout key is placed");
      let theirs = key_op::read_partition(&mut conn, &t.rec, &row)
        .expect("read the row's partition");
      assert_eq!(theirs, Some(ours), "{} row {}: {:?}", fan.name, m, key);
      assert_eq!(
        conn.hint_for_key(&t.def, &t.rec, &row),
        TransactionHint::Partition(ours),
        "{} row {}",
        fan.name,
        m
      );
      let base = key[..fan.base].join("/");
      let partitions = of_base.entry(base).or_default();
      if !partitions.contains(&ours) {
        partitions.push(ours);
      }
      m += 1;
    }
    for (base, partitions) in &of_base {
      assert!(
        partitions.len() <= 4,
        "{} base {}: {:?}",
        fan.name,
        base,
        partitions
      );
    }
    println!(
      "{}: {} rows placed, {} bases",
      fan.name,
      fan.rows,
      of_base.len()
    );
  }
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn fanout_key_operations_do_what_mysql_does() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_fan_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  for fan in &IC_IT_FAN_TABLES {
    let t = bind(&mut conn, fan.name);
    // Twenty rows of a base no row has yet, written, read, updated and
    // deleted by key, each through a transaction hinted to its
    // partition.
    let mut keys: Vec<Vec<String>> = Vec::new();
    let mut k: i64 = 0;
    while k < 20 {
      let mut key = fan_key(fan.name, k);
      key[0] = match fan.name {
        "fan_ci" => "new".to_string(),
        _ => "7777".to_string(),
      };
      keys.push(key);
      k += 1;
    }
    let mut id: i64 = 100_000;
    for key in &keys {
      let mut row = t.row();
      let mut c: usize = 0;
      while c < key.len() {
        t.put(&mut row, fan.key[c], &key[c]);
        c += 1;
      }
      t.put(&mut row, "id", &id.to_string());
      if fan.name == "fan_ii" {
        t.put(&mut row, "v", "1");
      }
      key_op::write_key(
        &mut conn,
        key_ops::IC_OP_INSERT,
        &t.rec,
        &row,
        &t.rec,
        &row,
      )
      .expect("insert by key");
      id += 1;
    }
    let first = match fan.name {
      "fan_ci" => "'new'".to_string(),
      _ => "7777".to_string(),
    };
    let count = |filter: &str| {
      count_of(&format!(
        "SELECT COUNT(*) FROM {}.{} WHERE a = {}{}",
        IC_IT_DB, fan.name, first, filter
      ))
    };
    assert_eq!(count(""), 20, "{}: inserted", fan.name);
    // Updated: each id plus a million.
    for key in &keys {
      let mut row = t.row();
      let mut c: usize = 0;
      while c < key.len() {
        t.put(&mut row, fan.key[c], &key[c]);
        c += 1;
      }
      let mut found = t.row();
      assert!(
        key_op::read_committed(&mut conn, &t.rec, &row, &t.rec, &mut found)
          .expect("read by key"),
        "{}: {:?} is there",
        fan.name,
        key
      );
      let was: i64 = t.get(&found, "id").parse().expect("an id");
      t.put(&mut found, "id", &(was + 1_000_000).to_string());
      key_op::write_key(
        &mut conn,
        key_ops::IC_OP_UPDATE,
        &t.rec,
        &found,
        &t.rec,
        &found,
      )
      .expect("update by key");
    }
    assert_eq!(count(" AND id > 1000000"), 20, "{}: updated", fan.name);
    for key in &keys {
      let mut row = t.row();
      let mut c: usize = 0;
      while c < key.len() {
        t.put(&mut row, fan.key[c], &key[c]);
        c += 1;
      }
      key_op::write_key(
        &mut conn,
        key_ops::IC_OP_DELETE,
        &t.rec,
        &row,
        &t.rec,
        &row,
      )
      .expect("delete by key");
    }
    assert_eq!(count(""), 0, "{}: deleted", fan.name);
  }
  assert_eq!(conn.unexpected(), 0);
}

/// One case of a fanout scan: the index, its columns, the bound's
/// values, MySQL's condition, and how many partitions the run reads:
/// 4, the fanout's, 1, or 0 for every one.
struct FanScan {
  table: &'static str,
  index: &'static str,
  columns: &'static [&'static str],
  values: &'static [&'static str],
  filter: &'static str,
  order: &'static str,
  partitions: u32,
}

const IC_IT_FAN_SCANS: [FanScan; 8] = [
  // The base fixed: the fanout's four partitions.
  FanScan {
    table: "fan_ii",
    index: "PRIMARY",
    columns: &["a"],
    values: &["7"],
    filter: "a = 7",
    order: "a, b",
    partitions: 4,
  },
  // The base's column second in its index, the first fixed too.
  FanScan {
    table: "fan_ii",
    index: "ix_va",
    columns: &["v", "a"],
    values: &["2", "7"],
    filter: "v = 2 AND a = 7",
    order: "v, a",
    partitions: 4,
  },
  // The index's first column alone, the base open: every partition.
  FanScan {
    table: "fan_ii",
    index: "ix_va",
    columns: &["v"],
    values: &["2"],
    filter: "v = 2",
    order: "v, a",
    partitions: 0,
  },
  // The whole key: one partition.
  FanScan {
    table: "fan_ii",
    index: "PRIMARY",
    columns: &["a", "b"],
    values: &["7", "3"],
    filter: "a = 7 AND b = 3",
    order: "a, b",
    partitions: 1,
  },
  // A base of two columns, the first of them characters.
  FanScan {
    table: "fan_ci",
    index: "PRIMARY",
    columns: &["a", "b"],
    values: &["k3", "4"],
    filter: "a = 'k3' AND b = 4",
    order: "a, b, c",
    partitions: 4,
  },
  // Half of that base: every partition.
  FanScan {
    table: "fan_ci",
    index: "PRIMARY",
    columns: &["a"],
    values: &["k3"],
    filter: "a = 'k3'",
    order: "a, b, c",
    partitions: 0,
  },
  // A base of one column, the detail two, over 8 partitions.
  FanScan {
    table: "fan_ic",
    index: "PRIMARY",
    columns: &["a"],
    values: &["5"],
    filter: "a = 5",
    order: "a, b, c",
    partitions: 4,
  },
  FanScan {
    table: "fan_ic",
    index: "PRIMARY",
    columns: &["a", "b", "c"],
    values: &["5", "2", "z"],
    filter: "a = 5 AND b = 2 AND c = 'z'",
    order: "a, b, c",
    partitions: 1,
  },
];

#[test]
fn fanout_scans_by_the_base_read_the_fanouts_partitions() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_fan_tables().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("connection");
  let mut faults: Vec<String> = Vec::new();
  for case in &IC_IT_FAN_SCANS {
    let t = bind(&mut conn, case.table);
    let fragments = t.def.info().fragment_count;
    let index = conn
      .index_bind(IC_IT_DB, case.index, case.table)
      .expect("bind the ordered index");
    // The key record over the index's columns, as many as it has.
    let all: Vec<&str> = match case.index {
      "ix_va" => vec!["v", "a"],
      _ => match case.table {
        "fan_ii" => vec!["a", "b"],
        _ => vec!["a", "b", "c"],
      },
    };
    let key_rec = record_over(&t, &all);
    let scan = conn
      .create_index_scan(&index, &key_rec, &t.rec)
      .expect("create the index scan");
    let mut values: Vec<Option<&str>> = Vec::new();
    for value in case.values {
      values.push(Some(*value));
    }
    let bound = bound_row(&t, &key_rec, &all, &values);
    let width = case.columns.len() as u32;
    let ranks = mysql_ranks(case.table, &rank_by(case.order), case.filter);
    let mut expected = fragments;
    if case.partitions > 0 {
      expected = case.partitions.min(fragments);
    }
    for (ordered, continuous) in
      [(false, false), (true, false), (false, true), (true, true)]
    {
      conn
        .add_scan_range(scan, &ab(width, true, width, true), &bound, &bound)
        .expect("add the range");
      let args = ScanArgs {
        ordered,
        continuous,
        batch_rows: 4,
        timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
        ..ScanArgs::default()
      };
      let (rows, error) = index_rows(&mut conn, &t, scan, &args);
      let what = format!(
        "{} {} ordered {} continuous {}",
        case.table, case.filter, ordered, continuous
      );
      if let Some(e) = error {
        faults.push(format!("{}: error {} {}", what, e.code, e.message()));
        continue;
      }
      for fault in order_faults(&rows, &ranks, ordered, false) {
        faults.push(format!("{}: {}", what, fault));
      }
      let run = conn.scan(scan).expect("the scan");
      if run.parallelism() != expected {
        faults.push(format!(
          "{}: {} partitions, not {}",
          what,
          run.parallelism(),
          expected
        ));
      }
      let mut receivers = expected;
      if continuous {
        receivers *= 4;
      }
      if run.receivers() != receivers {
        faults.push(format!("{}: {} receivers", what, run.receivers()));
      }
    }
    conn.free_scan(scan).expect("free the scan");
  }
  assert!(faults.is_empty(), "{}", first_faults(faults));
  // Through an executor, the reader's ranges going with its run.
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let t = bind_through(&submitter, "fan_ii");
  let shape = index_shape(&submitter, &t, "fan_ii", "PRIMARY", &["a", "b"]);
  let key_rec = record_over(&t, &["a", "b"]);
  let mut reader = ScanReader::new(&shape);
  let bound = bound_row(&t, &key_rec, &["a", "b"], &[Some("7")]);
  let ranks = mysql_ranks("fan_ii", &rank_by("a, b"), "a = 7");
  reader
    .add_range(&ab(1, true, 1, true), &bound, &bound)
    .expect("add the range");
  let args = ScanArgs {
    ordered: true,
    batch_rows: 4,
    timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
    ..ScanArgs::default()
  };
  submitter.scan(&mut reader, &args).expect("submit");
  let (rows, error) = reader_rows(&mut reader, &t);
  assert!(error.is_none(), "{:?}", error);
  let faults = order_faults(&rows, &ranks, true, false);
  assert!(faults.is_empty(), "{}", first_faults(faults));
  assert_eq!(conn.unexpected(), 0);
}

#[test]
fn fanout_a_scan_by_its_base_skips_the_other_partitions() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  // The same 100 000 rows in a fanout table and in a plain one, each of
  // 32 partitions: an equality on a is the fanout table's base, and
  // fixes nothing of the plain one's distribution key, (a, b).
  let sql = format!(
    "CREATE DATABASE IF NOT EXISTS {db};
     DROP TABLE IF EXISTS {db}.fan_big, {db}.fan_plain;
     CREATE TABLE {db}.fan_big (
       a INT NOT NULL, b INT NOT NULL, id INT NOT NULL,
       PRIMARY KEY (a, b)
     ) ENGINE=NDB COMMENT=\"NDB_TABLE=PARTITION_HASH=1:1:4\"
       PARTITION BY KEY () PARTITIONS 32;
     CREATE TABLE {db}.fan_plain (
       a INT NOT NULL, b INT NOT NULL, id INT NOT NULL,
       PRIMARY KEY (a, b)
     ) ENGINE=NDB PARTITION BY KEY () PARTITIONS 32;",
    db = IC_IT_DB
  );
  if mysql(&sql).is_none() {
    return;
  }
  let rows: i64 = 100_000;
  for table in ["fan_big", "fan_plain"] {
    fill_fan_rows(table, rows, "{m} DIV 100, {m} MOD 100, {m} + 1")
      .expect("fill the table");
  }
  let tables = ["fan_big", "fan_plain"];
  // One at a time: a scan's time is mostly its round trips, and the
  // partitions it reads are read side by side, so the fanout's four
  // save little of it. The tables in turn, three times, the best of
  // each kept: the first runs also fill the record pools.
  let mut conn = cluster.global.create_connection().expect("connection");
  let mut scans: Vec<(Table, Record, ScanId)> = Vec::new();
  for table in tables {
    let t = bind(&mut conn, table);
    let index = conn
      .index_bind(IC_IT_DB, "PRIMARY", table)
      .expect("bind the primary key's ordered index");
    let key_rec = record_over(&t, &["a", "b"]);
    let scan = conn
      .create_index_scan(&index, &key_rec, &t.rec)
      .expect("create the index scan");
    scans.push((t, key_rec, scan));
  }
  let mut best: [u64; 2] = [u64::MAX; 2];
  let mut partitions: [u32; 2] = [0; 2];
  let mut round: u32 = 0;
  while round < 3 {
    let mut k: usize = 0;
    while k < scans.len() {
      let (t, key_rec, scan) = &scans[k];
      let start = time::gethrtime();
      let mut a: i64 = 0;
      while a < 300 {
        let text = (a * 3).to_string();
        let bound = bound_row(t, key_rec, &["a", "b"], &[Some(text.as_str())]);
        conn
          .add_scan_range(*scan, &ab(1, true, 1, true), &bound, &bound)
          .expect("add the range");
        let args = ScanArgs {
          timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
          ..ScanArgs::default()
        };
        let (found, error) = index_rows(&mut conn, t, *scan, &args);
        assert!(error.is_none(), "{} a = {}: {:?}", tables[k], text, error);
        assert_eq!(found.len(), 100, "{} a = {}", tables[k], text);
        partitions[k] = conn.scan(*scan).expect("the scan").parallelism();
        a += 1;
      }
      best[k] = best[k].min(time::millis_elapsed(start, time::gethrtime()));
      k += 1;
    }
    round += 1;
  }
  for k in [0usize, 1] {
    println!(
      "{}: 300 scans one at a time, {} partitions each, in {} ms",
      tables[k], partitions[k], best[k]
    );
  }
  for (_, _, scan) in &scans {
    conn.free_scan(*scan).expect("free the scan");
  }
  // Many at once, through an executor: 16 threads of 100 scans each.
  // Now the data nodes' threads limit, and a scan of four partitions
  // asks an eighth of a plain one's work of them.
  let executor = executor_with(&cluster, 400, 4000);
  let submitter = executor.submitter();
  let mut shapes: Vec<(Table, Record, Arc<ScanShape>)> = Vec::new();
  for table in tables {
    let t = bind_through(&submitter, table);
    let shape = index_shape(&submitter, &t, table, "PRIMARY", &["a", "b"]);
    let key_rec = record_over(&t, &["a", "b"]);
    shapes.push((t, key_rec, shape));
  }
  let mut many: [u64; 2] = [u64::MAX; 2];
  let mut round: u32 = 0;
  while round < 2 {
    let mut k: usize = 0;
    while k < shapes.len() {
      let (t, key_rec, shape) = &shapes[k];
      let start = time::gethrtime();
      std::thread::scope(|scope| {
        let mut handles = Vec::new();
        let mut thread: i64 = 0;
        while thread < 16 {
          let mine = executor.submitter();
          let first = thread * 100;
          handles.push(scope.spawn(move || {
            let mut reader = ScanReader::new(shape);
            let mut i: i64 = 0;
            while i < 100 {
              let text = ((first + i) % 1000).to_string();
              let bound =
                bound_row(t, key_rec, &["a", "b"], &[Some(text.as_str())]);
              reader
                .add_range(&ab(1, true, 1, true), &bound, &bound)
                .expect("add the range");
              let args = ScanArgs {
                timeout_ms: IC_IT_SCAN_WAIT_MS as u32,
                ..ScanArgs::default()
              };
              mine.scan(&mut reader, &args).expect("submit");
              let (found, error) = reader_rows(&mut reader, t);
              assert!(error.is_none(), "a = {}: {:?}", text, error);
              assert_eq!(found.len(), 100, "a = {}", text);
              i += 1;
            }
          }));
          thread += 1;
        }
        for handle in handles {
          handle.join().expect("a scanning thread");
        }
      });
      many[k] = many[k].min(time::millis_elapsed(start, time::gethrtime()));
      k += 1;
    }
    round += 1;
  }
  for k in [0usize, 1] {
    println!(
      "{}: 1 600 scans, 16 at a time, in {} ms: {} scans a second",
      tables[k],
      many[k],
      1600 * 1000 / many[k].max(1)
    );
  }
  let faster = 100 - (many[0] * 100 / many[1].max(1)).min(100);
  println!(
    "many at once, the fanout table's scans take {}% less time",
    faster
  );
  assert_eq!(conn.unexpected(), 0);
  mysql(&format!(
    "DROP TABLE IF EXISTS {db}.fan_big, {db}.fan_plain",
    db = IC_IT_DB
  ));
}

// ---- Review 3: failed synchronous operations clean up ----

#[test]
fn review_3_a_refused_unique_read_leaves_no_transaction_behind() {
  let cluster = match cluster() {
    Some(cluster) => cluster,
    None => return,
  };
  if setup_uk2().is_none() {
    return;
  }
  let mut conn = cluster.global.create_connection().expect("a connection");
  let t = bind(&mut conn, "uk2");
  let n = bind_unique_index(&mut conn, "uk2_n", "uk2");
  let key_rec = record_over(&t, &["n"]);
  let mut key = vec![0; key_rec.row_size() as usize];
  key_rec.set_null(&mut key, 0, true).expect("set NULL");
  let open = conn.transactions_open();
  // A NULL key is refused after the read's transaction has started, and
  // before anything goes. Each such transaction is rolled back and
  // closed, its coordinator record back in the pool: far more of them
  // than the coordinators keep records for one connection would
  // otherwise run them out.
  let mut tries: u32 = 0;
  while tries < 2000 {
    let mut out = t.row();
    let e = key_op::read_unique_committed(
      &mut conn, &n, &key_rec, &key, &t.rec, &mut out,
    )
    .expect_err("a NULL key is refused");
    assert_eq!(e.code, IC_NDB_ERROR_NULL_KEY, "try {}", tries);
    assert_eq!(conn.transactions_open(), open, "try {}", tries);
    tries += 1;
  }
  // A read that finds its row still works, and leaves nothing open.
  let row = read_through(&mut conn, &t, &n, &[("n", "0")])
    .expect("read through uk2_n")
    .expect("row 2");
  assert_eq!(t.get(&row, "id"), "2");
  let missing = read_through(&mut conn, &t, &n, &[("n", "12345")])
    .expect("read through uk2_n");
  assert!(missing.is_none(), "no row has n = 12345");
  assert_eq!(conn.transactions_open(), open);
  // Nothing of the refused reads is left on the executed list either.
  assert!(conn.get_next_executed_query().is_none());
  assert_eq!(conn.unexpected(), 0);
}
