// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_write`: insert, update, delete or write one row by its primary
//! key, each as a transaction of its own.
//!
//! ```text
//!   ic_write -c localhost:1186 -d ictest t9 3 30
//!   ic_write -c localhost:1186 -d ictest --op update t9 3 31
//!   ic_write -c localhost:1186 -d ictest --op delete t9 3
//! ```
//!
//! After the table come the row's values, one per column in the table's
//! column order, the key columns among them. `NULL` writes no value. A
//! delete takes the key columns only.
//!
//! An update writes every column but the key's, which it may not
//! change; a write inserts the row or updates it if it is there.
//!
//! Values are given as text: integers, and character and binary
//! strings. A column of another type can only be given as `NULL` here;
//! an application writes such a column in the bytes the data node
//! keeps it in.
//!
//! Debug level 1024 traces every signal, which shows the request with
//! its key and values, and the confirmation or refusal.

use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::TableDef;
use ic_apid::key_op;
use ic_apid::record::Record;
use ic_apid::text_row;
use ic_ndb_signals::tc_key;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;

const OPTIONS: [OptionEntry; 5] = [
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
    long_name: "op",
    short_name: b'o',
    kind: OptionKind::Str,
    help: "insert, update, delete or write; the default is insert",
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
    "ic_write",
    "Insert, update or delete one row of a table by its key.",
  );
  parser.add_entries(&OPTIONS);
  if let Err(e) = parser.parse_env_args() {
    if e.code == ic_port::err::IC_ERROR_HELP_REQUESTED {
      return 0;
    }
    return 1;
  }
  ic_port::debug::set_level(parser.get_int_or("debug-level", 0) as u32);
  ic_port::debug::set_screen(true);
  let args: Vec<String> = parser.positional().to_vec();
  if args.len() < 2 {
    println!("Name a table and a row, for example: ic_write -d test t1 7 70");
    return 1;
  }
  let operation = match operation_of(&parser.get_string_or("op", "insert")) {
    Some(operation) => operation,
    None => {
      println!("The operation is insert, update, delete or write");
      return 1;
    }
  };
  let database = parser.get_string_or("database", "test");
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
    mgm_client::fetch_configuration(&connect_string, 30_000, Some("ic_write"));
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
    Some("ic_write"),
  ) {
    Ok(global) => global,
    Err(e) => {
      report("Could not start the Data API threads", &e);
      return 1;
    }
  };
  let code = write_row(&global, &database, operation, &args[0], &args[1..]);
  global.stop();
  code
}

/// The operation an option names.
fn operation_of(text: &str) -> Option<u32> {
  match text {
    "insert" => Some(tc_key::IC_OP_INSERT),
    "update" => Some(tc_key::IC_OP_UPDATE),
    "delete" => Some(tc_key::IC_OP_DELETE),
    "write" => Some(tc_key::IC_OP_WRITE),
    _ => None,
  }
}

fn write_row(
  global: &ApidGlobal,
  database: &str,
  operation: u32,
  table: &str,
  values: &[String],
) -> i32 {
  if global.wait_for_started(15_000) == 0 {
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
  let mut row = vec![0u8; rec.row_size() as usize];
  let keys_only = operation == tc_key::IC_OP_DELETE;
  if let Err(text) = put_row(&def, &rec, values, keys_only, &mut row) {
    println!("{}", text);
    return 1;
  }
  let done = key_op::write_key(&mut conn, operation, &rec, &row, &rec, &row);
  if conn.unexpected() > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      conn.unexpected()
    );
  }
  match done {
    Ok(()) => {
      println!("{}", what_happened(operation));
      0
    }
    Err(e) => {
      if e.code == key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32 {
        println!("There is no row with that key");
        return 1;
      }
      if e.code == key_op::IC_NDB_ERROR_ROW_EXISTS as i32 {
        println!("There is already a row with that key");
        return 1;
      }
      report("Could not write the row", &e);
      1
    }
  }
}

fn what_happened(operation: u32) -> &'static str {
  match operation {
    tc_key::IC_OP_INSERT => "Inserted",
    tc_key::IC_OP_UPDATE => "Updated",
    tc_key::IC_OP_DELETE => "Deleted",
    _ => "Written",
  }
}

/// The values into the row: the key columns alone for a delete, every
/// column otherwise, in the table's column order.
fn put_row(
  def: &Arc<TableDef>,
  rec: &Record,
  values: &[String],
  keys_only: bool,
  row: &mut [u8],
) -> Result<(), String> {
  let mut used: usize = 0;
  for attr in &def.info().attributes {
    if keys_only && !attr.primary_key {
      continue;
    }
    let position = match rec.position_of(attr.attribute_id) {
      Some(position) => position,
      None => return Err(format!("No field for {}", attr.name)),
    };
    if used >= values.len() {
      return Err(format!("There is no value for {}", attr.name));
    }
    text_row::put_value(rec, position, &values[used], row)?;
    used += 1;
  }
  if used != values.len() {
    return Err(format!(
      "The row takes {} value(s), not {}",
      used,
      values.len()
    ));
  }
  Ok(())
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
