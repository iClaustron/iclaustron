// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_trans`: a run of rows inserted in one transaction, read back in
//! one batch, and deleted in one transaction.
//!
//! ```text
//!   ic_trans -c localhost:1186 -d ictest t9 --from 100 --count 10
//! ```
//!
//! This is the transaction API used as an application uses it: queries
//! made once, defined on a transaction, sent together, and taken from
//! the executed list as they complete. Every integer column of the
//! table other than the key gets ten times the key; a column of another
//! type is left NULL, and a table with such a column that cannot be
//! NULL is refused.
//!
//! Debug level 1024 traces every signal: one `TCKEYREQ` per row, the
//! confirmations packed together, and the rows coming back.

use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::TableDef;
use ic_apid::query::QueryId;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_apid::record::Record;
use ic_apid::text_row;
use ic_apid::transaction::CommitState;
use ic_apid::transaction::TransactionHint;
use ic_ndb_signals::dict_tab_info;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;

/// How long to wait for one transaction to finish.
const IC_TRANS_WAIT_MS: u64 = 10_000;

const OPTIONS: [OptionEntry; 6] = [
  OptionEntry {
    long_name: "ndb-connectstring",
    short_name: b'c',
    kind: OptionKind::Str,
    help: "Management servers, as host:port,host:port",
  },
  OptionEntry {
    long_name: "database",
    short_name: b'd',
    kind: OptionKind::Str,
    help: "Database the table is in; the default is test",
  },
  OptionEntry {
    long_name: "from",
    short_name: b'f',
    kind: OptionKind::Int,
    help: "The first key; the default is 1000",
  },
  OptionEntry {
    long_name: "count",
    short_name: b'n',
    kind: OptionKind::Int,
    help: "How many rows; the default is 10",
  },
  OptionEntry {
    long_name: "node-id",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Node id to ask for; 0 lets the server choose",
  },
  OptionEntry {
    long_name: "debug-level",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Debug level bits; 1024 traces every signal",
  },
];

fn main() {
  std::process::exit(run());
}

fn run() -> i32 {
  let mut parser = OptionParser::new(
    "ic_trans",
    "Insert, read back and delete a run of rows, one transaction each.",
  );
  parser.add_entries(&OPTIONS);
  if let Err(e) = parser.parse_env_args() {
    if e.code == ic_port::err::IC_ERROR_HELP_REQUESTED {
      return 0;
    }
    return 1;
  }
  let debug_level = parser.get_int_or("debug-level", 0) as u32;
  ic_port::debug::set_level(debug_level);
  ic_port::debug::set_screen(true);
  // Seconds since the start on every line, so that a pause can be seen.
  ic_port::debug::set_timestamp(debug_level != 0);
  let args: Vec<String> = parser.positional().to_vec();
  if args.len() != 1 {
    println!("Name one table, for example: ic_trans -d test t1 --count 10");
    return 1;
  }
  let database = parser.get_string_or("database", "test");
  let from = parser.get_int_or("from", 1000);
  let count = parser.get_int_or("count", 10);
  if !(1..=1000).contains(&count) {
    println!("The count must be between 1 and 1000");
    return 1;
  }
  let connect_text =
    parser.get_string_or("ndb-connectstring", "localhost:1186");
  let mut connect_string = match ic_util::connectstring::parse(&connect_text) {
    Ok(cs) => cs,
    Err(e) => {
      report("Could not read the connectstring", &e);
      return 1;
    }
  };
  let wanted = parser.get_int_or("node-id", 0) as u32;
  if wanted != 0 {
    connect_string.node_id = Some(wanted);
  }
  let fetched =
    mgm_client::fetch_configuration(&connect_string, 30_000, Some("ic_trans"));
  let (config, mgm) = match fetched {
    Ok(pair) => pair,
    Err(e) => {
      report("Could not fetch the configuration", &e);
      return 1;
    }
  };
  let mut global = match ApidGlobal::start(
    config,
    mgm,
    connect_string,
    30_000,
    Some("ic_trans"),
  ) {
    Ok(global) => global,
    Err(e) => {
      report("Could not start the Data API threads", &e);
      return 1;
    }
  };
  let code = run_all(&global, &database, &args[0], from, count as usize);
  global.stop();
  code
}

fn run_all(
  global: &ApidGlobal,
  database: &str,
  table: &str,
  from: i64,
  count: usize,
) -> i32 {
  if global.wait_for_first_started(15_000) == 0 {
    println!("No data node is started, so there is nobody to ask");
    return 1;
  }
  let mut conn = match global.create_connection() {
    Ok(conn) => conn,
    Err(e) => {
      report("Could not make a connection", &e);
      return 1;
    }
  };
  let def = match conn.table_bind(database, table) {
    Ok(def) => def,
    Err(e) => {
      report("Could not bind the table", &e);
      return 1;
    }
  };
  let rec = match Record::default_for(&def, None) {
    Ok(rec) => rec,
    Err(e) => {
      report("Could not make a record", &e);
      return 1;
    }
  };
  if let Err(text) = check_columns(&def) {
    println!("{}", text);
    return 1;
  }
  // One query per row, so that a whole run is in flight at once.
  let mut queries: Vec<QueryId> = Vec::with_capacity(count);
  let mut i: usize = 0;
  while i < count {
    match conn.create_query(&def, &rec, &rec) {
      Ok(id) => queries.push(id),
      Err(e) => {
        report("Could not make a query", &e);
        return 1;
      }
    }
    i += 1;
  }
  let steps: [(&str, WriteKind); 2] = [
    ("Inserted", WriteKind::Insert),
    ("Deleted", WriteKind::Delete),
  ];
  let mut code = write_run(&mut conn, &def, &rec, &queries, from, steps[0]);
  if code == 0 {
    code = read_run(&mut conn, &def, &rec, &queries, from);
  }
  if code == 0 {
    code = write_run(&mut conn, &def, &rec, &queries, from, steps[1]);
  }
  if conn.unexpected() > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      conn.unexpected()
    );
  }
  code
}

/// Every column other than the key must be an integer, or nullable.
fn check_columns(def: &Arc<TableDef>) -> Result<(), String> {
  for attr in &def.info().attributes {
    if attr.primary_key || attr.nullable || is_integer(attr.ext_type) {
      continue;
    }
    return Err(format!(
      "{} is neither an integer nor nullable, so no row can be made",
      attr.name
    ));
  }
  Ok(())
}

fn is_integer(ext_type: u32) -> bool {
  let integers =
    dict_tab_info::IC_NDB_TYPE_TINYINT..=dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED;
  integers.contains(&ext_type)
}

/// The key, and for an insert every other column, into a query's rows.
fn fill_query(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  id: QueryId,
  key: i64,
  values_too: bool,
) -> Result<(), String> {
  let query = match conn.query_mut(id) {
    Some(query) => query,
    None => return Err("The query is gone".to_string()),
  };
  for attr in &def.info().attributes {
    let position = match rec.position_of(attr.attribute_id) {
      Some(position) => position,
      None => continue,
    };
    if attr.primary_key {
      let text = key.to_string();
      text_row::put_value(rec, position, &text, query.key_row_mut())?;
      text_row::put_value(rec, position, &text, query.attr_row_mut())?;
      continue;
    }
    if !values_too {
      continue;
    }
    let text = if is_integer(attr.ext_type) {
      (key * 10).to_string()
    } else {
      text_row::IC_NULL_TEXT.to_string()
    };
    text_row::put_value(rec, position, &text, query.attr_row_mut())?;
  }
  Ok(())
}

/// Every row written in one transaction.
fn write_run(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  queries: &[QueryId],
  from: i64,
  step: (&str, WriteKind),
) -> i32 {
  let (what, kind) = step;
  let trans = match conn.start_transaction(TransactionHint::Any, None) {
    Ok(trans) => trans,
    Err(e) => {
      report("Could not start a transaction", &e);
      return 1;
    }
  };
  let mut i: usize = 0;
  while i < queries.len() {
    let key = from + i as i64;
    let insert = kind == WriteKind::Insert;
    if let Err(text) = fill_query(conn, def, rec, queries[i], key, insert) {
      println!("{}", text);
      return 1;
    }
    let args = WriteKeyArgs {
      kind,
      user_ref: i,
      ..WriteKeyArgs::default()
    };
    if let Err(e) = conn.write_key(queries[i], trans, &args) {
      report("Could not define a write", &e);
      return 1;
    }
    i += 1;
  }
  if let Err(e) = conn.commit_transaction(trans) {
    report("Could not ask for the commit", &e);
    return 1;
  }
  let outcome = wait_for(conn, trans);
  let failed = count_failures(conn, "write");
  match outcome {
    Some(CommitState::Committed) => {
      let gci = match conn.transaction(trans) {
        Some(t) => t.gci(),
        None => 0,
      };
      println!(
        "{} {} row(s) in one transaction, global checkpoint {}",
        what,
        queries.len(),
        gci
      );
    }
    Some(other) => {
      let why = match conn.transaction(trans) {
        Some(t) => t.error(),
        None => None,
      };
      println!("The transaction ended {:?}{}", other, error_text(why));
    }
    None => println!("The transaction did not finish in time"),
  }
  let _ = conn.close_transaction(trans);
  if outcome == Some(CommitState::Committed) && failed == 0 {
    0
  } else {
    1
  }
}

/// Every row read back with a committed read, all in one batch.
fn read_run(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  queries: &[QueryId],
  from: i64,
) -> i32 {
  let trans = match conn.start_transaction(TransactionHint::Any, None) {
    Ok(trans) => trans,
    Err(e) => {
      report("Could not start a transaction", &e);
      return 1;
    }
  };
  let mut i: usize = 0;
  while i < queries.len() {
    let key = from + i as i64;
    if let Err(text) = fill_query(conn, def, rec, queries[i], key, false) {
      println!("{}", text);
      return 1;
    }
    let args = ReadKeyArgs {
      kind: ReadKind::Committed,
      user_ref: i,
      ..ReadKeyArgs::default()
    };
    if let Err(e) = conn.read_key(queries[i], trans, &args) {
      report("Could not define a read", &e);
      return 1;
    }
    i += 1;
  }
  if let Err(e) = conn.commit_transaction(trans) {
    report("Could not ask for the commit", &e);
    return 1;
  }
  let outcome = wait_for(conn, trans);
  let mut shown: usize = 0;
  let mut failed: usize = 0;
  while let Some(id) = conn.get_next_executed_query() {
    let query = match conn.query(id) {
      Some(query) => query,
      None => continue,
    };
    if let Some(e) = query.error() {
      println!("row {}: {} ({})", query.user_ref(), e.message(), e.code);
      failed += 1;
      continue;
    }
    let mut parts: Vec<String> = Vec::new();
    let mut position: u32 = 0;
    while position < rec.num_fields() {
      parts.push(text_row::value_text(rec, position, query.attr_row()));
      position += 1;
    }
    println!("row {}: {}", query.user_ref(), parts.join(", "));
    shown += 1;
  }
  println!("Read {} row(s) back in one batch", shown);
  let _ = conn.close_transaction(trans);
  if outcome == Some(CommitState::Committed) && failed == 0 {
    0
  } else {
    1
  }
}

/// Send, then poll until the transaction is done or the wait runs out.
fn wait_for(
  conn: &mut ApidConnection,
  trans: ic_apid::query::TransId,
) -> Option<CommitState> {
  let start = ic_port::time::gethrtime();
  loop {
    if let Err(e) = conn.flush(100, false) {
      report("Could not send", &e);
    }
    if let Some(t) = conn.transaction(trans) {
      if t.is_done() {
        return Some(t.commit_state());
      }
    }
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if waited >= IC_TRANS_WAIT_MS {
      return None;
    }
  }
}

/// Take every executed query, reporting the failed ones.
fn count_failures(conn: &mut ApidConnection, what: &str) -> usize {
  let mut failed: usize = 0;
  while let Some(id) = conn.get_next_executed_query() {
    if let Some(query) = conn.query(id) {
      if let Some(e) = query.error() {
        println!(
          "{} {}: {} ({})",
          what,
          query.user_ref(),
          e.message(),
          e.code
        );
        failed += 1;
      }
    }
  }
  failed
}

fn error_text(error: Option<IcError>) -> String {
  match error {
    Some(e) => format!(": {} ({})", e.message(), e.code),
    None => String::new(),
  }
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
