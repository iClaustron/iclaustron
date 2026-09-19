// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_read`: read one row by its primary key, as a committed read.
//!
//! ```text
//!   ic_read -c localhost:1186 -d ictest t9 1
//!   ic_read -c localhost:1186 -d ictest t9 1 --debug-level 1024
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
//! Debug level 1024 traces every signal, which shows the seize of a
//! transaction record, the request, and the replies, packed or not.

use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::TableDef;
use ic_apid::key_op;
use ic_apid::record::Record;
use ic_apid::record::RecordField;
use ic_ndb_signals::dict_tab_info;
use ic_ndb_signals::dict_tab_info::AttributeInfo;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;

const OPTIONS: [OptionEntry; 4] = [
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
  let code = read_row(&global, &database, &args[0], &args[1..]);
  global.stop();
  code
}

fn read_row(
  global: &ApidGlobal,
  database: &str,
  table: &str,
  keys: &[String],
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
  let code =
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
  if conn.unexpected() > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      conn.unexpected()
    );
  }
  code
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
    let field = match rec.position_of(attr.attribute_id) {
      Some(position) => match rec.field(position) {
        Some(field) => field,
        None => return Err(format!("No field for {}", attr.name)),
      },
      None => return Err(format!("No field for {}", attr.name)),
    };
    put_value(attr, field, &keys[used], row)?;
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

/// One key value, from its text, into its field.
fn put_value(
  attr: &AttributeInfo,
  field: &RecordField,
  text: &str,
  row: &mut [u8],
) -> Result<(), String> {
  let at = field.offset() as usize;
  let size = field.size() as usize;
  let bytes = text.as_bytes();
  let too_long = format!("{} is too long for {}", text, attr.name);
  let is_integer = attr.ext_type >= dict_tab_info::IC_NDB_TYPE_TINYINT
    && attr.ext_type <= dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED;
  if is_integer && (size == 0 || size > 8) {
    return Err(format!("{} is an integer array, not a key", attr.name));
  }
  match attr.ext_type {
    dict_tab_info::IC_NDB_TYPE_TINYINT
    | dict_tab_info::IC_NDB_TYPE_SMALLINT
    | dict_tab_info::IC_NDB_TYPE_MEDIUMINT
    | dict_tab_info::IC_NDB_TYPE_INT
    | dict_tab_info::IC_NDB_TYPE_BIGINT => {
      let value: i64 = match text.parse() {
        Ok(value) => value,
        Err(_) => return Err(format!("{} is not a number", text)),
      };
      // What is above the column's sign bit must be all sign.
      let top = value >> (8 * size.min(8) - 1);
      if size < 8 && top != 0 && top != -1 {
        return Err(format!("{} does not fit {}", text, attr.name));
      }
      let le = value.to_le_bytes();
      row[at..at + size].copy_from_slice(&le[..size]);
    }
    dict_tab_info::IC_NDB_TYPE_TINYUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_SMALLUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_MEDIUMUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_UNSIGNED
    | dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED => {
      let value: u64 = match text.parse() {
        Ok(value) => value,
        Err(_) => return Err(format!("{} is not a number", text)),
      };
      if size < 8 && value >> (8 * size) != 0 {
        return Err(format!("{} does not fit {}", text, attr.name));
      }
      let le = value.to_le_bytes();
      row[at..at + size].copy_from_slice(&le[..size]);
    }
    dict_tab_info::IC_NDB_TYPE_CHAR | dict_tab_info::IC_NDB_TYPE_BINARY => {
      if bytes.len() > size {
        return Err(too_long);
      }
      // CHAR is kept padded with spaces, BINARY with zeros.
      let mut pad: u8 = 0;
      if attr.ext_type == dict_tab_info::IC_NDB_TYPE_CHAR {
        pad = b' ';
      }
      row[at..at + size].fill(pad);
      row[at..at + bytes.len()].copy_from_slice(bytes);
    }
    dict_tab_info::IC_NDB_TYPE_VARCHAR
    | dict_tab_info::IC_NDB_TYPE_VARBINARY => {
      if bytes.len() + 1 > size {
        return Err(too_long);
      }
      row[at] = bytes.len() as u8;
      row[at + 1..at + 1 + bytes.len()].copy_from_slice(bytes);
    }
    dict_tab_info::IC_NDB_TYPE_LONGVARCHAR
    | dict_tab_info::IC_NDB_TYPE_LONGVARBINARY => {
      if bytes.len() + 2 > size {
        return Err(too_long);
      }
      let len = (bytes.len() as u16).to_le_bytes();
      row[at..at + 2].copy_from_slice(&len);
      row[at + 2..at + 2 + bytes.len()].copy_from_slice(bytes);
    }
    _ => {
      return Err(format!(
        "A key of type {} cannot be given here",
        dict_tab_info::type_name(attr.ext_type)
      ));
    }
  }
  Ok(())
}

/// Each column of the row, a line each.
fn print_row(def: &Arc<TableDef>, rec: &Record, row: &[u8]) {
  let mut position: u32 = 0;
  while position < rec.num_fields() {
    if let Some(field) = rec.field(position) {
      if let Some(attr) = def.field(field.field_id()) {
        let value = if rec.is_null(row, position) {
          "NULL".to_string()
        } else {
          let at = field.offset() as usize;
          format_value(attr, &row[at..at + field.size() as usize])
        };
        println!("{}: {}", attr.name, value);
      }
    }
    position += 1;
  }
}

/// A value as text: numbers and strings as such, the rest as bytes.
fn format_value(attr: &AttributeInfo, bytes: &[u8]) -> String {
  match attr.ext_type {
    dict_tab_info::IC_NDB_TYPE_TINYINT
    | dict_tab_info::IC_NDB_TYPE_SMALLINT
    | dict_tab_info::IC_NDB_TYPE_MEDIUMINT
    | dict_tab_info::IC_NDB_TYPE_INT
    | dict_tab_info::IC_NDB_TYPE_BIGINT => signed_of(bytes).to_string(),
    dict_tab_info::IC_NDB_TYPE_TINYUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_SMALLUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_MEDIUMUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_UNSIGNED
    | dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED
    | dict_tab_info::IC_NDB_TYPE_YEAR => unsigned_of(bytes).to_string(),
    dict_tab_info::IC_NDB_TYPE_FLOAT if bytes.len() == 4 => {
      f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]).to_string()
    }
    dict_tab_info::IC_NDB_TYPE_DOUBLE if bytes.len() == 8 => {
      let mut eight = [0u8; 8];
      eight.copy_from_slice(bytes);
      f64::from_le_bytes(eight).to_string()
    }
    dict_tab_info::IC_NDB_TYPE_CHAR => {
      let text = String::from_utf8_lossy(bytes);
      format!("'{}'", text.trim_end_matches(' '))
    }
    dict_tab_info::IC_NDB_TYPE_VARCHAR if !bytes.is_empty() => {
      let len = (bytes[0] as usize).min(bytes.len() - 1);
      format!("'{}'", String::from_utf8_lossy(&bytes[1..1 + len]))
    }
    dict_tab_info::IC_NDB_TYPE_LONGVARCHAR if bytes.len() >= 2 => {
      let len =
        (bytes[0] as usize + 256 * bytes[1] as usize).min(bytes.len() - 2);
      format!("'{}'", String::from_utf8_lossy(&bytes[2..2 + len]))
    }
    _ => hex_of(bytes),
  }
}

/// A little-endian integer of any size up to eight bytes, sign extended.
fn signed_of(bytes: &[u8]) -> i64 {
  let unsigned = unsigned_of(bytes);
  let bits = 8 * bytes.len().min(8);
  if bits == 0 || bits == 64 {
    return unsigned as i64;
  }
  let shift = 64 - bits;
  ((unsigned << shift) as i64) >> shift
}

/// A little-endian unsigned integer of up to eight bytes.
fn unsigned_of(bytes: &[u8]) -> u64 {
  let mut value: u64 = 0;
  let mut i = bytes.len().min(8);
  while i > 0 {
    i -= 1;
    value = (value << 8) | bytes[i] as u64;
  }
  value
}

fn hex_of(bytes: &[u8]) -> String {
  let mut text = String::from("0x");
  for byte in bytes {
    text.push_str(&format!("{:02x}", byte));
  }
  text
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
