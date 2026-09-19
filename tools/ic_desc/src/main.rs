// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_desc`: describe a table as the data nodes' dictionary sees it,
//! the way `ndb_desc` does.
//!
//! ```text
//!   ic_desc -c localhost:1186 -d ictest t1
//!   ic_desc -c localhost:1186 -d ictest t1 t2 --debug-level 1024
//!   ic_desc -c localhost:1186 -d ictest t1 --watch 60
//!   ic_desc -c localhost:1186 -d ictest t1 --record
//! ```
//!
//! It starts the Data API, makes one connection as a user thread does,
//! and asks a started data node for each table in turn. The table has
//! to exist: the API never creates one, that is done through a MySQL
//! server.
//!
//! Character sets are shown by number. The names live in the MySQL
//! server's tables, which this library does not carry.
//!
//! Debug level 1024 traces every signal in and out, which shows the
//! request, and the answer in fragments if the data node split it.
//!
//! Tables and indexes are bound as an application binds them, through
//! the dictionary cache. `--watch` then binds the tables again every
//! second and says when that gives a new description: alter or drop a
//! table through a MySQL server meanwhile, and the data nodes' notice
//! should let the cached one go at once. Debug level 1024 shows the
//! notices, among every other signal.
//!
//! `--record` also prints the default record over the table: where each
//! field lies in the row, every one on a word, and its null bit.

use ic_apic::mgm_client;
use std::sync::Arc;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::TableDef;
use ic_apid::dict_client;
use ic_apid::record::Record;
use ic_ndb_signals::dict_tab_info;
use ic_ndb_signals::dict_tab_info::AttributeInfo;
use ic_ndb_signals::dict_tab_info::HashMapInfo;
use ic_ndb_signals::dict_tab_info::TableInfo;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;

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
    help: "Database the tables are in; the default is test",
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
    long_name: "watch",
    short_name: b'w',
    kind: OptionKind::Int,
    help: "Then bind the tables every second for this many seconds",
  },
  OptionEntry {
    long_name: "record",
    short_name: b'r',
    kind: OptionKind::Flag,
    help: "Also print the default record over each table",
  },
];

fn main() {
  std::process::exit(run());
}

fn run() -> i32 {
  let mut parser = OptionParser::new(
    "ic_desc",
    "Describe tables as the data nodes' dictionary sees them.",
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
  let tables: Vec<String> = parser.positional().to_vec();
  if tables.is_empty() {
    println!("Name at least one table, for example: ic_desc -d test t1");
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
    mgm_client::fetch_configuration(&connect_string, 30_000, Some("ic_desc"));
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
    Some("ic_desc"),
  ) {
    Ok(global) => global,
    Err(e) => {
      report("Could not start the Data API threads", &e);
      return 1;
    }
  };
  let watch = parser.get_int_or("watch", 0) as u32;
  let record = parser.get_flag("record");
  let code = describe_all(&global, &database, &tables, watch, record);
  global.stop();
  code
}

fn describe_all(
  global: &ApidGlobal,
  database: &str,
  tables: &[String],
  watch: u32,
  record: bool,
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
  let mut code = 0;
  for table in tables {
    match conn.table_bind(database, table) {
      Ok(def) => {
        print_table(def.info(), def.hash_map());
        print_indexes(&mut conn, database, table, def.info());
        print_own_lines(def.info(), def.hash_map());
        if record {
          print_default_record(&def);
        }
      }
      Err(e) => {
        println!("-- {}.{} --", database, table);
        report("Could not describe it", &e);
        code = 1;
      }
    }
    println!();
  }
  if watch > 0 {
    watch_tables(&mut conn, database, tables, watch);
  }
  if conn.unexpected() > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      conn.unexpected()
    );
  }
  code
}

/// The default record over the table: offset, size and null bit of each
/// field.
fn print_default_record(table: &Arc<TableDef>) {
  let rec = match Record::default_for(table, None) {
    Ok(rec) => rec,
    Err(e) => {
      report("Could not make its default record", &e);
      return;
    }
  };
  println!("-- Default record, {} bytes --", rec.row_size());
  let mut position: u32 = 0;
  while position < rec.num_fields() {
    if let Some(field) = rec.field(position) {
      let name = match table.field(field.field_id()) {
        Some(attr) => attr.name.as_str(),
        None => "?",
      };
      let mut null = String::new();
      if field.is_nullable() {
        null = format!(
          ", null bit {} of byte {}",
          field.null_bit(),
          field.null_byte_offset()
        );
      }
      println!(
        "{} {}: offset {}, {} bytes{}",
        name,
        dict_tab_info::type_name(field.field_type()),
        field.offset(),
        field.size(),
        null
      );
    }
    position += 1;
  }
}

/// Bind the tables again once a second, and say whenever that gives a
/// description other than the one before: after the dictionary's notice
/// has let the cached one go, or once the table can no longer be bound.
fn watch_tables(
  conn: &mut ApidConnection,
  database: &str,
  tables: &[String],
  seconds: u32,
) {
  let mut bound: Vec<Option<Arc<TableDef>>> = Vec::new();
  for table in tables {
    bound.push(conn.table_bind(database, table).ok());
  }
  println!("Binding every second for {} seconds", seconds);
  let mut elapsed: u32 = 0;
  while elapsed < seconds {
    ic_port::time::microsleep(1_000_000);
    elapsed += 1;
    let mut i: usize = 0;
    while i < tables.len() {
      let now = match conn.table_bind(database, &tables[i]) {
        Ok(def) => Some(def),
        Err(e) => {
          if bound[i].is_some() {
            println!("{:>4} s  {}: {}", elapsed, tables[i], e.message());
          }
          None
        }
      };
      let changed = match (&bound[i], &now) {
        (Some(was), Some(def)) => !Arc::ptr_eq(was, def),
        (None, Some(_)) => true,
        _ => false,
      };
      if let (true, Some(def)) = (changed, &now) {
        println!(
          "{:>4} s  {}: version upper {}, lower {}, {} column(s)",
          elapsed,
          tables[i],
          def.info().version_upper(),
          def.info().version_lower(),
          def.info().attributes.len()
        );
      }
      bound[i] = now;
      i += 1;
    }
  }
}

fn print_table(table: &TableInfo, map: Option<&HashMapInfo>) {
  // The lines and their order are ndb_desc's, so that the two can be
  // compared line by line. Ours alone come at the end.
  println!("-- {} --", table.table_name());
  // ndb_desc prints the whole word. Its two halves count different
  // things, online changes above and offline changes below, so they
  // are shown apart.
  println!(
    "Version: upper {}, lower {}",
    table.version_upper(),
    table.version_lower()
  );
  println!("Fragment type: {}", fragment_type_name(table.fragment_type));
  println!("K Value: {}", table.kvalue);
  println!("Min load factor: {}", table.min_load_factor);
  println!("Max load factor: {}", table.max_load_factor);
  println!("Temporary table: {}", yes_no(table.temporary));
  println!("Number of attributes: {}", table.no_of_attributes);
  println!("Number of primary keys: {}", table.no_of_key_attr);
  println!("Length of frm data: {}", table.mysql_metadata_len);
  println!("Max Rows: {}", table.max_rows);
  println!("Row Checksum: {}", table.row_checksum as u32);
  println!("Row GCI: {}", table.row_gci as u32);
  println!("SingleUserMode: {}", table.single_user_mode);
  println!("ForceVarPart: {}", table.force_var_part as u32);
  println!("PartitionCount: {}", table.partition_count);
  println!("FragmentCount: {}", table.fragment_count);
  println!(
    "PartitionBalance: {}",
    partition_balance_name(table.partition_balance)
  );
  println!("ExtraRowGciBits: {}", table.extra_row_gci_bits);
  println!("ExtraRowAuthorBits: {}", table.extra_row_author_bits);
  // What a table is once fetched; the reference prints its own state.
  println!("TableStatus: Retrieved");
  let mut options: Vec<&str> = Vec::new();
  if table.read_backup {
    options.push("readbackup");
  }
  if table.fully_replicated {
    options.push("fullyreplicated");
  }
  if !table.logged {
    options.push("nologging");
  }
  println!("Table options: {}", options.join(", "));
  if let Some(map) = map {
    println!("HashMap: {}", map.name);
  }
  if table.ttl_sec != dict_tab_info::IC_RNIL {
    println!("TTL: {} s on column {}", table.ttl_sec, table.ttl_column_no);
  }
  println!("-- Attributes --");
  for attr in &table.attributes {
    println!("{}", describe_attribute(attr));
  }
}

/// What ndb_desc does not show, after everything it does.
fn print_own_lines(table: &TableInfo, map: Option<&HashMapInfo>) {
  println!("-- iClaustron --");
  println!("Database: {}", table.database());
  println!("Table id: {}", table.table_id);
  let hash = if table.hash_function == 0 {
    "MD5"
  } else {
    "XXH3 64-bit"
  };
  println!("Hash function: {}", hash);
  if let Some(map) = map {
    println!(
      "Hash map: id {}, version {}, {} buckets over {} fragments",
      map.object_id,
      map.version,
      map.fragments.len(),
      map.fragment_count()
    );
  }
}

/// The indexes, as ndb_desc lists them: first the primary key, which
/// is the table itself rather than an index of its own, then every
/// index in id order, each with its columns. The list comes from the
/// dictionary; each index is then bound by name.
fn print_indexes(
  conn: &mut ApidConnection,
  database: &str,
  table_name: &str,
  table: &TableInfo,
) {
  // The reference prints the heading with a space at its end.
  println!("-- Indexes -- ");
  let mut key_names: Vec<&str> = Vec::new();
  for attr in &table.attributes {
    if attr.primary_key {
      key_names.push(&attr.name);
    }
  }
  println!("PRIMARY KEY({}) - UniqueHashIndex", key_names.join(", "));
  let listed = dict_client::list_dependents(conn, table.table_id);
  let mut objects = match listed {
    Ok(objects) => objects,
    Err(e) => {
      report("Could not list its indexes", &e);
      return;
    }
  };
  objects.sort_by_key(|object| object.id);
  for object in &objects {
    let kind = index_type_name(object.object_type);
    if kind.is_empty() {
      // A trigger, a large-object table or another dependent.
      continue;
    }
    let bound = conn.index_bind(database, object.short_name(), table_name);
    let index = match bound {
      Ok(index) => index,
      Err(e) => {
        report("Could not bind an index", &e);
        continue;
      }
    };
    // The last column is a hidden reference back to the table's row,
    // not one of the index's own.
    let attributes = &index.info().attributes;
    let mut columns: Vec<&str> = Vec::new();
    let mut i: usize = 0;
    while i + 1 < attributes.len() {
      columns.push(&attributes[i].name);
      i += 1;
    }
    println!("{}({}) - {}", object.short_name(), columns.join(", "), kind);
  }
}

/// What ndb_desc calls an index type, or nothing for other objects.
fn index_type_name(object_type: u32) -> &'static str {
  match object_type {
    dict_tab_info::IC_TABLE_TYPE_UNIQUE_HASH_INDEX => "UniqueHashIndex",
    dict_tab_info::IC_TABLE_TYPE_ORDERED_INDEX => "OrderedIndex",
    _ => "",
  }
}

fn yes_no(on: bool) -> &'static str {
  if on {
    return "yes";
  }
  "no"
}

/// One column as `ndb_desc` shows it: name, type with its sizes, then
/// flags.
fn describe_attribute(attr: &AttributeInfo) -> String {
  let mut line = format!("{} {}", attr.name, type_with_sizes(attr));
  // A primary key column cannot be NULL, so the reference says only
  // that it is a key.
  if attr.primary_key {
    line.push_str(" PRIMARY KEY");
  } else if !attr.nullable {
    line.push_str(" NOT NULL");
  } else {
    line.push_str(" NULL");
  }
  if attr.distribution_key {
    line.push_str(" DISTRIBUTION KEY");
  }
  line.push_str(" AT=");
  line.push_str(array_type_name(attr.array_type));
  line.push_str(" ST=");
  if attr.storage_type == dict_tab_info::IC_STORAGE_DISK {
    line.push_str("DISK");
  } else {
    line.push_str("MEMORY");
  }
  if attr.auto_increment {
    line.push_str(" AUTO_INCR");
  }
  if attr.dynamic {
    line.push_str(" DYNAMIC");
  }
  if !attr.default_value.is_empty() {
    line.push_str(" DEFAULT ");
    line.push_str(&format_default(attr));
  }
  line
}

/// A default value as text: numbers and text as themselves, anything
/// else as hex. The reference also formats decimals and times; we show
/// those in hex for now.
fn format_default(attr: &AttributeInfo) -> String {
  let bytes = &attr.default_value;
  let t = attr.ext_type;
  if t == dict_tab_info::IC_NDB_TYPE_TINYINT
    || t == dict_tab_info::IC_NDB_TYPE_SMALLINT
    || t == dict_tab_info::IC_NDB_TYPE_MEDIUMINT
    || t == dict_tab_info::IC_NDB_TYPE_INT
    || t == dict_tab_info::IC_NDB_TYPE_BIGINT
  {
    return signed_of(bytes).to_string();
  }
  if t == dict_tab_info::IC_NDB_TYPE_TINYUNSIGNED
    || t == dict_tab_info::IC_NDB_TYPE_SMALLUNSIGNED
    || t == dict_tab_info::IC_NDB_TYPE_MEDIUMUNSIGNED
    || t == dict_tab_info::IC_NDB_TYPE_UNSIGNED
    || t == dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED
  {
    return unsigned_of(bytes).to_string();
  }
  if t == dict_tab_info::IC_NDB_TYPE_FLOAT && bytes.len() == 4 {
    let value = f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    return value.to_string();
  }
  if t == dict_tab_info::IC_NDB_TYPE_DOUBLE && bytes.len() == 8 {
    let mut raw: [u8; 8] = [0; 8];
    raw.copy_from_slice(bytes);
    return f64::from_le_bytes(raw).to_string();
  }
  if t == dict_tab_info::IC_NDB_TYPE_CHAR {
    let text = String::from_utf8_lossy(bytes);
    return text.trim_end_matches(' ').to_string();
  }
  if t == dict_tab_info::IC_NDB_TYPE_VARCHAR && !bytes.is_empty() {
    return String::from_utf8_lossy(&bytes[1..]).into_owned();
  }
  if t == dict_tab_info::IC_NDB_TYPE_LONGVARCHAR && bytes.len() >= 2 {
    return String::from_utf8_lossy(&bytes[2..]).into_owned();
  }
  let mut hex = String::from("0x");
  for byte in bytes {
    hex.push_str(&format!("{:02X}", byte));
  }
  hex
}

/// Little-endian bytes as an unsigned number.
fn unsigned_of(bytes: &[u8]) -> u64 {
  let mut value: u64 = 0;
  let mut i = bytes.len().min(8);
  while i > 0 {
    i -= 1;
    value = (value << 8) | bytes[i] as u64;
  }
  value
}

/// Little-endian bytes as a signed number of that many bytes.
fn signed_of(bytes: &[u8]) -> i64 {
  let len = bytes.len().min(8);
  if len == 0 {
    return 0;
  }
  let raw = unsigned_of(bytes);
  let shift = 64 - 8 * len as u32;
  ((raw << shift) as i64) >> shift
}

fn type_with_sizes(attr: &AttributeInfo) -> String {
  let name = dict_tab_info::type_name(attr.ext_type);
  if attr.is_char_type() {
    return format!("{}({};charset {})", name, attr.length, attr.charset);
  }
  let t = attr.ext_type;
  if t == dict_tab_info::IC_NDB_TYPE_DECIMAL
    || t == dict_tab_info::IC_NDB_TYPE_DECIMALUNSIGNED
    || t == dict_tab_info::IC_NDB_TYPE_OLDDECIMAL
    || t == dict_tab_info::IC_NDB_TYPE_OLDDECIMALUNSIGNED
  {
    return format!("{}({},{})", name, attr.precision, attr.scale);
  }
  if t == dict_tab_info::IC_NDB_TYPE_BINARY
    || t == dict_tab_info::IC_NDB_TYPE_VARBINARY
    || t == dict_tab_info::IC_NDB_TYPE_LONGVARBINARY
    || t == dict_tab_info::IC_NDB_TYPE_BIT
  {
    return format!("{}({})", name, attr.length);
  }
  if t == dict_tab_info::IC_NDB_TYPE_TIME2
    || t == dict_tab_info::IC_NDB_TYPE_DATETIME2
    || t == dict_tab_info::IC_NDB_TYPE_TIMESTAMP2
  {
    return format!("{}({})", name, attr.precision);
  }
  name.to_string()
}

fn array_type_name(array_type: u32) -> &'static str {
  match array_type {
    dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR => "SHORT_VAR",
    dict_tab_info::IC_ARRAY_TYPE_MEDIUM_VAR => "MEDIUM_VAR",
    _ => "FIXED",
  }
}

fn fragment_type_name(fragment_type: u32) -> &'static str {
  match fragment_type {
    dict_tab_info::IC_FRAGMENT_TYPE_HASH_MAP => "HashMapPartition",
    _ => "Other",
  }
}

fn partition_balance_name(balance: u32) -> &'static str {
  // The special values count down from the top of the range.
  match !balance {
    0 => "SPECIFIC",
    1 => "FOR_RP_BY_LDM",
    2 => "FOR_RA_BY_LDM",
    3 => "FOR_RP_BY_NODE",
    4 => "FOR_RA_BY_NODE",
    5 => "FOR_RA_BY_LDM_X_2",
    6 => "FOR_RA_BY_LDM_X_3",
    7 => "FOR_RA_BY_LDM_X_4",
    8 => "FOR_RP_BY_LDM_X_2",
    9 => "FOR_RP_BY_LDM_X_4",
    _ => "a fixed count",
  }
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
