// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_read`: read one row by its primary key, as a committed read.
//!
//! ```text
//!   ic_read -c localhost:1186 -d ictest t9 1
//!   ic_read -c localhost:1186 -d ictest t9 1 --debug-level 1024
//!   ic_read -c localhost:1186 -d ictest t9 1 --partition
//! ```
//!
//! After the table come the key's values, one per primary key column
//! in the table's column order. The table is bound through the
//! dictionary cache, the default record is made over it, and the row is
//! read into it and printed a column at a time.
//!
//! Key columns may be integers, or character and binary strings. The
//! row may hold any type a record can: numbers and strings are printed
//! as such, anything else as its bytes in hexadecimal.
//!
//! Integers are put in the key, and read from the row, in little-endian
//! order: the data node's own, on the machines RonDB runs on.
//!
//! `--partition` also works out which partition the key belongs to,
//! hashing it as the data nodes do, and asks a data node which
//! partition the row is really in, so that the two can be compared.
//!
//! Debug level 1024 traces every signal, which shows the seize of a
//! transaction record, the request, and the replies, packed or not.

use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::TableDef;
use ic_apid::hash;
use ic_apid::key_op;
use ic_apid::record::Record;
use ic_apid::text_row;
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
  OptionEntry {
    long_name: "partition",
    short_name: b'p',
    kind: OptionKind::Flag,
    help: "Also compare the key's computed partition with the row's",
  },
];

fn main() {
  std::process::exit(run());
}

fn run() -> i32 {
  let mut parser =
    OptionParser::new("ic_read", "Read one row of a table by its key.");
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
    println!("Name a table and its key, for example: ic_read -d test t1 7");
    return 1;
  }
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
    mgm_client::fetch_configuration(&connect_string, 30_000, Some("ic_read"));
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
    Some("ic_read"),
  ) {
    Ok(global) => global,
    Err(e) => {
      report("Could not start the Data API threads", &e);
      return 1;
    }
  };
  let partition = parser.get_flag("partition");
  let code = read_row(&global, &database, &args[0], &args[1..], partition);
  global.stop();
  code
}

fn read_row(
  global: &ApidGlobal,
  database: &str,
  table: &str,
  keys: &[String],
  partition: bool,
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
  let mut key_row = vec![0u8; rec.row_size() as usize];
  if let Err(text) = put_key(&def, &rec, keys, &mut key_row) {
    println!("{}", text);
    return 1;
  }
  let mut row = vec![0u8; rec.row_size() as usize];
  let mut code =
    match key_op::read_committed(&mut conn, &rec, &key_row, &rec, &mut row) {
      Ok(true) => {
        print_row(&def, &rec, &row);
        0
      }
      Ok(false) => {
        println!("No row with that key");
        1
      }
      Err(e) => {
        report("Could not read the row", &e);
        1
      }
    };
  if partition && code == 0 {
    code = compare_partition(&mut conn, &def, &rec, &key_row);
  }
  if conn.unexpected() > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      conn.unexpected()
    );
  }
  code
}

/// The partition the key hashes to here against the one the data node
/// keeps the row in.
fn compare_partition(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  key_row: &[u8],
) -> i32 {
  let computed = match hash::partition_of(def, rec, key_row) {
    Ok(partition) => partition,
    Err(e) => {
      report("Could not work out the partition", &e);
      return 1;
    }
  };
  let actual = match key_op::read_partition(conn, rec, key_row) {
    Ok(Some(partition)) => partition,
    Ok(None) => {
      println!("The row went away before its partition was read");
      return 1;
    }
    Err(e) => {
      report("Could not read the row's partition", &e);
      return 1;
    }
  };
  println!(
    "Partition: computed {}, the data node says {}{}",
    computed,
    actual,
    if computed == actual {
      ""
    } else {
      " -- THEY DIFFER"
    }
  );
  if computed == actual {
    0
  } else {
    1
  }
}

/// Put the key's values into the key columns of `row`.
fn put_key(
  def: &Arc<TableDef>,
  rec: &Record,
  keys: &[String],
  row: &mut [u8],
) -> Result<(), String> {
  let mut used: usize = 0;
  for attr in &def.info().attributes {
    if !attr.primary_key {
      continue;
    }
    if used >= keys.len() {
      return Err(format!("The key needs a value for {}", attr.name));
    }
    let position = match rec.position_of(attr.attribute_id) {
      Some(position) => position,
      None => return Err(format!("No field for {}", attr.name)),
    };
    text_row::put_value(rec, position, &keys[used], row)?;
    used += 1;
  }
  if used != keys.len() {
    return Err(format!(
      "The key has {} column(s), not {}",
      used,
      keys.len()
    ));
  }
  Ok(())
}

/// Each column of the row, a line each.
fn print_row(def: &Arc<TableDef>, rec: &Record, row: &[u8]) {
  let mut position: u32 = 0;
  while position < rec.num_fields() {
    if let Some(field) = rec.field(position) {
      if let Some(attr) = def.field(field.field_id()) {
        println!(
          "{}: {}",
          attr.name,
          text_row::value_text(rec, position, row)
        );
      }
    }
    position += 1;
  }
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
