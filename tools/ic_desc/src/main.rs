// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_desc`: describe a table as the data nodes' dictionary sees it,
//! much as the reference's own table description tool does.
//!
//! ```text
//!   ic_desc -c localhost:1186 -d ictest t1
//!   ic_desc -c localhost:1186 -d ictest t1 t2 --debug-level 1024
//!   ic_desc -c localhost:1186 -d ictest t1 --watch 60
//!   ic_desc -c localhost:1186 -d ictest t1 --record
//!   ic_desc -c localhost:1186 --collation latin1_swedish_ci --weights
//!   ic_desc -c localhost:1186 --collation 8 --all-weights
//!   ic_desc -c localhost:1186 --charset utf8mb4 --weights
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
//!
//! `--collation` describes a collation instead, by name or by the number
//! a column's description carries, and `--charset` a character set's
//! default collation, as a data node's dictionary describes it.
//! `--weights` also fetches its weights, in as many round trips as it
//! takes, and prints how many there are and the weight strings of a few
//! telling characters; `--all-weights` prints every code point's and
//! every contraction's, one a line, to compare with another source.

use ic_apic::mgm_client;
use std::sync::Arc;

use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::collation_client;
use ic_apid::collation_client::Collation;
use ic_apid::dict_cache::TableDef;
use ic_apid::dict_client;
use ic_apid::record::Record;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;
use ic_signals::collation_desc;
use ic_signals::table_props;
use ic_signals::table_props::ColumnProps;
use ic_signals::table_props::HashMapProps;
use ic_signals::table_props::TableProps;

const OPTIONS: [OptionEntry; 10] = [
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
  OptionEntry {
    long_name: "collation",
    short_name: 0,
    kind: OptionKind::Str,
    help: "Describe this collation, by name or number, not tables",
  },
  OptionEntry {
    long_name: "charset",
    short_name: 0,
    kind: OptionKind::Str,
    help: "Describe this character set's default collation",
  },
  OptionEntry {
    long_name: "weights",
    short_name: 0,
    kind: OptionKind::Flag,
    help: "With a collation: fetch its weights and show a few",
  },
  OptionEntry {
    long_name: "all-weights",
    short_name: 0,
    kind: OptionKind::Flag,
    help: "With a collation: print every weight string",
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
  let debug_level = parser.get_int_or("debug-level", 0) as u32;
  ic_port::debug::set_level(debug_level);
  ic_port::debug::set_screen(true);
  // Seconds since the start on every line, so that a pause can be seen.
  ic_port::debug::set_timestamp(debug_level != 0);
  let tables: Vec<String> = parser.positional().to_vec();
  let collation = parser.get_string_or("collation", "");
  let charset = parser.get_string_or("charset", "");
  let asking_collation = !collation.is_empty() || !charset.is_empty();
  if tables.is_empty() && !asking_collation {
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
  let all = parser.get_flag("all-weights");
  let weights = parser.get_flag("weights") || all;
  let code = if asking_collation {
    describe_collation(&global, &collation, &charset, weights, all)
  } else {
    describe_all(&global, &database, &tables, watch, record)
  };
  global.stop();
  code
}

/// Ask a data node for a collation, and print what it says.
fn describe_collation(
  global: &ApidGlobal,
  collation: &str,
  charset: &str,
  weights: bool,
  all: bool,
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
  let by_charset = collation_desc::IC_COLLATION_BY_CHARSET;
  let by_name = collation_desc::IC_COLLATION_BY_NAME;
  let fetched = if !charset.is_empty() {
    collation_client::describe_by_name(&mut conn, by_charset, charset, weights)
  } else {
    match collation.parse::<u32>() {
      Ok(number) => {
        collation_client::describe_by_number(&mut conn, number, weights)
      }
      Err(_) => collation_client::describe_by_name(
        &mut conn, by_name, collation, weights,
      ),
    }
  };
  match fetched {
    Ok(found) => {
      print_collation(&found, weights, all);
      0
    }
    Err(e) => {
      report("Could not describe the collation", &e);
      1
    }
  }
}

fn print_collation(found: &Collation, weights: bool, all: bool) {
  let desc = &found.desc;
  println!("-- collation {} ({}) --", desc.collation_name, desc.number);
  println!("Character set: {}", desc.charset_name);
  println!("Default collation of the set: {}", desc.default_number);
  println!("Binary collation of the set: {}", desc.binary_number);
  println!("Flags: {}", collation_flags(desc.flags));
  println!(
    "Bytes a character: {} to {}",
    desc.min_char_bytes, desc.max_char_bytes
  );
  println!("Sort-key bytes per byte: {}", desc.sort_key_growth);
  println!(
    "Case growth: upper {}, lower {}",
    desc.upper_case_growth, desc.lower_case_growth
  );
  println!("Pad byte: 0x{:02x}", desc.pad_byte);
  println!("Weight levels compared: {}", desc.weight_levels);
  println!(
    "Sorts from U+{:04X} to U+{:04X}",
    desc.min_sort_char, desc.max_sort_char
  );
  println!(
    "Answered by node {} in {} round trip(s)",
    found.node_id, found.round_trips
  );
  if !weights {
    return;
  }
  let mut covered: u64 = 0;
  for run in &found.runs {
    covered += run.count as u64;
  }
  println!(
    "Weights: {} runs covering {} code points, {} contractions",
    found.runs.len(),
    covered,
    found.contractions.len()
  );
  // A few telling characters: the space, A and a, B and b, Ä and ä, é.
  let samples: [u32; 8] = [0x20, 0x41, 0x61, 0x42, 0x62, 0xC4, 0xE4, 0xE9];
  let mut out: Vec<u8> = Vec::new();
  for code_point in samples {
    let shown = char::from_u32(code_point).unwrap_or('?');
    if weight_of(found, code_point, &mut out) {
      println!("  U+{:04X} '{}': {}", code_point, shown, hex(&out));
    } else {
      println!("  U+{:04X} '{}': not in the set", code_point, shown);
    }
  }
  if !all {
    return;
  }
  for run in &found.runs {
    let mut k: u32 = 0;
    while k < run.count {
      run.weight_of(k, &mut out);
      println!("U+{:04X} {}", run.first + k, hex(&out));
      k += 1;
    }
  }
  for contraction in &found.contractions {
    let mut points = String::new();
    for code_point in &contraction.code_points {
      if !points.is_empty() {
        points.push(' ');
      }
      points.push_str(&format!("U+{:04X}", code_point));
    }
    let context = if contraction.previous_context {
      " (second weighed by the first)"
    } else {
      ""
    };
    println!("{} {}{}", points, hex(&contraction.weight), context);
  }
}

/// The weight string of one code point, if the set has it.
fn weight_of(found: &Collation, code_point: u32, out: &mut Vec<u8>) -> bool {
  for run in &found.runs {
    if code_point >= run.first && code_point - run.first < run.count {
      run.weight_of(code_point - run.first, out);
      return true;
    }
  }
  false
}

fn hex(bytes: &[u8]) -> String {
  let mut text = String::new();
  for byte in bytes {
    text.push_str(&format!("{:02x}", byte));
  }
  if text.is_empty() {
    text.push_str("(empty)");
  }
  text
}

fn collation_flags(flags: u32) -> String {
  let names: [(u32, &str); 6] = [
    (collation_desc::IC_COLLATION_DEFAULT, "the set's default"),
    (collation_desc::IC_COLLATION_BINARY, "binary order"),
    (collation_desc::IC_COLLATION_UNICODE, "Unicode"),
    (
      collation_desc::IC_COLLATION_CASE_SENSITIVE,
      "case sensitive",
    ),
    (collation_desc::IC_COLLATION_PURE_ASCII, "pure ASCII"),
    (collation_desc::IC_COLLATION_NO_PAD, "no pad"),
  ];
  let mut text = String::new();
  for (bit, name) in names {
    if (flags & bit) != 0 {
      if !text.is_empty() {
        text.push_str(", ");
      }
      text.push_str(name);
    }
  }
  if text.is_empty() {
    text.push_str("none");
  }
  text
}

fn describe_all(
  global: &ApidGlobal,
  database: &str,
  tables: &[String],
  watch: u32,
  record: bool,
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
        table_props::type_name(field.field_type()),
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

fn print_table(table: &TableProps, map: Option<&HashMapProps>) {
  // The lines follow the reference's own table description tool in
  // what they show and in which order, in our words, so that the two
  // can be compared by meaning. Ours alone come at the end.
  println!("-- {} --", table.table_name());
  // The reference prints the whole word. Its two halves count different
  // things, online changes above and offline changes below, so they
  // are shown apart.
  println!(
    "Version: upper {}, lower {}",
    table.version_upper(),
    table.version_lower()
  );
  println!("Fragment type: {}", fragment_kind_name(table.fragment_kind));
  println!("Linear hash k: {}", table.linear_hash_k);
  println!("Min fill: {}", table.min_fill);
  println!("Max fill: {}", table.max_fill);
  println!("Temporary table: {}", yes_no(table.temporary));
  println!("Number of attributes: {}", table.column_count);
  println!("Number of primary keys: {}", table.key_column_count);
  println!("SQL metadata length: {}", table.sql_metadata_len);
  println!("Max Rows: {}", table.max_rows);
  println!("Row Checksum: {}", table.row_checksum as u32);
  println!("Row GCI: {}", table.row_gci as u32);
  println!("Single user: {}", table.single_user);
  println!("Force varsize part: {}", table.force_varsize_part as u32);
  println!("Partitions: {}", table.partition_count);
  println!("Fragments: {}", table.fragment_count);
  println!(
    "Partition balance: {}",
    partition_balance_name(table.partition_balance)
  );
  println!("Extra GCI bits: {}", table.extra_gci_bits);
  println!("Extra author bits: {}", table.extra_author_bits);
  // What a table is once fetched; the reference prints its own state.
  println!("Status: fetched");
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
    println!("Hash map: {}", map.name);
  }
  if table.ttl_seconds != table_props::IC_NULL_PTR {
    println!(
      "TTL: {} s on column {}",
      table.ttl_seconds, table.ttl_column
    );
  }
  println!("-- Attributes --");
  for attr in &table.attributes {
    println!("{}", describe_attribute(attr));
  }
}

/// What the reference's tool does not show, after everything it does.
fn print_own_lines(table: &TableProps, map: Option<&HashMapProps>) {
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

/// The indexes, as the reference's tool lists them: first the primary
/// key, which is the table itself rather than an index of its own, then
/// every index in id order, each with its columns. The list comes from
/// the dictionary; each index is then bound by name.
fn print_indexes(
  conn: &mut ApidConnection,
  database: &str,
  table_name: &str,
  table: &TableProps,
) {
  // The reference prints the heading with a space at its end.
  println!("-- Indexes -- ");
  let mut key_names: Vec<&str> = Vec::new();
  for attr in &table.attributes {
    if attr.primary_key {
      key_names.push(&attr.name);
    }
  }
  println!("PRIMARY KEY({}) - unique hash index", key_names.join(", "));
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

/// An index's kind, or nothing for other objects.
fn index_type_name(object_type: u32) -> &'static str {
  match object_type {
    table_props::IC_OBJECT_KIND_UNIQUE_INDEX => "unique hash index",
    table_props::IC_OBJECT_KIND_ORDERED_INDEX => "ordered index",
    _ => "",
  }
}

fn yes_no(on: bool) -> &'static str {
  if on {
    return "yes";
  }
  "no"
}

/// One column as the reference's tool shows it: name, type with its
/// sizes, then flags.
fn describe_attribute(attr: &ColumnProps) -> String {
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
  line.push_str(" array=");
  line.push_str(array_kind_name(attr.array_kind));
  line.push_str(" storage=");
  if attr.storage_kind == table_props::IC_STORAGE_DISK {
    line.push_str("DISK");
  } else {
    line.push_str("MEMORY");
  }
  if attr.auto_increment {
    line.push_str(" AUTO_INCREMENT");
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
fn format_default(attr: &ColumnProps) -> String {
  let bytes = &attr.default_value;
  let t = attr.type_code;
  if t == table_props::IC_TYPE_TINYINT
    || t == table_props::IC_TYPE_SMALLINT
    || t == table_props::IC_TYPE_MEDIUMINT
    || t == table_props::IC_TYPE_INT
    || t == table_props::IC_TYPE_BIGINT
  {
    return signed_of(bytes).to_string();
  }
  if t == table_props::IC_TYPE_TINYUNSIGNED
    || t == table_props::IC_TYPE_SMALLUNSIGNED
    || t == table_props::IC_TYPE_MEDIUMUNSIGNED
    || t == table_props::IC_TYPE_UNSIGNED
    || t == table_props::IC_TYPE_BIGUNSIGNED
  {
    return unsigned_of(bytes).to_string();
  }
  if t == table_props::IC_TYPE_FLOAT && bytes.len() == 4 {
    let value = f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    return value.to_string();
  }
  if t == table_props::IC_TYPE_DOUBLE && bytes.len() == 8 {
    let mut raw: [u8; 8] = [0; 8];
    raw.copy_from_slice(bytes);
    return f64::from_le_bytes(raw).to_string();
  }
  if t == table_props::IC_TYPE_CHAR {
    let text = String::from_utf8_lossy(bytes);
    return text.trim_end_matches(' ').to_string();
  }
  if t == table_props::IC_TYPE_VARCHAR && !bytes.is_empty() {
    return String::from_utf8_lossy(&bytes[1..]).into_owned();
  }
  if t == table_props::IC_TYPE_LONGVARCHAR && bytes.len() >= 2 {
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

fn type_with_sizes(attr: &ColumnProps) -> String {
  let name = table_props::type_name(attr.type_code);
  if attr.is_char_type() {
    return format!("{}({};charset {})", name, attr.length, attr.charset);
  }
  let t = attr.type_code;
  if t == table_props::IC_TYPE_DECIMAL
    || t == table_props::IC_TYPE_DECIMALUNSIGNED
    || t == table_props::IC_TYPE_OLDDECIMAL
    || t == table_props::IC_TYPE_OLDDECIMALUNSIGNED
  {
    return format!("{}({},{})", name, attr.precision, attr.scale);
  }
  if t == table_props::IC_TYPE_BINARY
    || t == table_props::IC_TYPE_VARBINARY
    || t == table_props::IC_TYPE_LONGVARBINARY
    || t == table_props::IC_TYPE_BIT
  {
    return format!("{}({})", name, attr.length);
  }
  if t == table_props::IC_TYPE_TIME2
    || t == table_props::IC_TYPE_DATETIME2
    || t == table_props::IC_TYPE_TIMESTAMP2
  {
    return format!("{}({})", name, attr.precision);
  }
  name.to_string()
}

fn array_kind_name(array_kind: u32) -> &'static str {
  match array_kind {
    table_props::IC_ARRAY_KIND_SHORT_VARSIZE => "short varsize",
    table_props::IC_ARRAY_KIND_LONG_VARSIZE => "long varsize",
    _ => "fixed",
  }
}

fn fragment_kind_name(fragment_kind: u32) -> &'static str {
  match fragment_kind {
    table_props::IC_FRAGMENT_KIND_HASH_MAP => "hash map",
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
