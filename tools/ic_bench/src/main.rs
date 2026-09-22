// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_bench`: how many primary key reads, or updates, one thread gets
//! through with a few batches of them in flight at a time.
//!
//! ```text
//!   ic_bench -c localhost:1186 -d ictest t9 --keys 1000 --batch 200
//!   ic_bench -c localhost:1186 -d ictest t9 --mode write --seconds 30
//!   ic_bench -c localhost:1186 -d ictest t9 --depth 1
//! ```
//!
//! The rows with keys from `--from` up to the key count are written
//! first, unless `--no-prepare`, so that every read finds its row. Then
//! for `--seconds` the thread keeps `--depth` batches in flight, each
//! of `--batch` committed reads, or updates, each a transaction of its
//! own hinted to the node holding its row. A batch is defined and sent
//! together, one socket write per node, and as soon as every one of
//! its operations is done it is defined again on the next keys, going
//! round them in order, while the other batches are still out. That
//! keeps the data nodes busy while this thread handles replies and
//! packs the next batch, and this thread busy while they execute. With
//! `--depth 1` the pipeline drains between batches, which is the plain
//! form and the lower number. It reports the operations per second and
//! the time a batch takes from send to done.
//!
//! The batch size matters more than the depth: a round costs both
//! sides a fixed part, the write, the receive, the execution round and
//! the reply packets, so 200 operations a packet cost the data nodes
//! far less per operation than 50, and the defaults are what measured
//! best on one thread (doc/rust/06, phase 5).
//!
//! `--force` makes each batch go at once rather than leaving it to the
//! adaptive send algorithm, which is what an application gets by
//! default; for one thread the two are the same. Measure a release
//! build: `cargo run --release -p ic_bench`.
//!
//! The table needs a primary key of one integer column; every other
//! integer column gets ten times the key, and any other column must be
//! nullable.

use std::collections::HashMap;
use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::dict_cache::TableDef;
use ic_apid::query::QueryId;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::TransId;
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

/// How long one batch may take before it counts as stuck.
const IC_BENCH_BATCH_WAIT_NANOS: u64 = 10_000_000_000;
/// The most operations in one batch.
const IC_BENCH_MAX_BATCH: i64 = 1000;
/// The most batches in flight at once.
const IC_BENCH_MAX_DEPTH: i64 = 16;

const OPTIONS: [OptionEntry; 12] = [
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
    long_name: "mode",
    short_name: b'm',
    kind: OptionKind::Str,
    help: "read or write; the default is read",
  },
  OptionEntry {
    long_name: "seconds",
    short_name: b's',
    kind: OptionKind::Int,
    help: "How long to run; the default is 10",
  },
  OptionEntry {
    long_name: "batch",
    short_name: b'b',
    kind: OptionKind::Int,
    help: "Operations in one batch; the default is 200",
  },
  OptionEntry {
    long_name: "depth",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Batches in flight at once; the default is 2, 1 is lock step",
  },
  OptionEntry {
    long_name: "keys",
    short_name: b'k',
    kind: OptionKind::Int,
    help: "How many keys to go round; the default is 1000",
  },
  OptionEntry {
    long_name: "from",
    short_name: b'f',
    kind: OptionKind::Int,
    help: "The first key; the default is 1",
  },
  OptionEntry {
    long_name: "no-prepare",
    short_name: 0,
    kind: OptionKind::Flag,
    help: "Do not write the rows first; they are there already",
  },
  OptionEntry {
    long_name: "force",
    short_name: 0,
    kind: OptionKind::Flag,
    help: "Write each batch at once, not by the adaptive send algorithm",
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

/// What the command line asked for.
struct Run {
  write: bool,
  seconds: u64,
  batch: usize,
  depth: usize,
  keys: i64,
  from: i64,
  prepare: bool,
  force: bool,
}

fn main() {
  std::process::exit(run());
}

fn run() -> i32 {
  let mut parser = OptionParser::new(
    "ic_bench",
    "Measure primary key reads and writes from one thread.",
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
  ic_port::debug::set_timestamp(debug_level != 0);
  let args: Vec<String> = parser.positional().to_vec();
  if args.len() != 1 {
    println!("Name one table, for example: ic_bench -d test t1 --keys 1000");
    return 1;
  }
  let mode = parser.get_string_or("mode", "read");
  let write = match mode.as_str() {
    "read" => false,
    "write" => true,
    _ => {
      println!("The mode is read or write");
      return 1;
    }
  };
  let batch = parser.get_int_or("batch", 200);
  let depth = parser.get_int_or("depth", 2);
  let keys = parser.get_int_or("keys", 1000);
  let seconds = parser.get_int_or("seconds", 10);
  if !(1..=IC_BENCH_MAX_BATCH).contains(&batch)
    || !(1..=IC_BENCH_MAX_DEPTH).contains(&depth)
    || keys < 1
    || seconds < 1
  {
    println!(
      "The batch is between 1 and {}, the depth between 1 and {}, and \
       keys and seconds at least 1",
      IC_BENCH_MAX_BATCH, IC_BENCH_MAX_DEPTH
    );
    return 1;
  }
  let what = Run {
    write,
    seconds: seconds as u64,
    batch: batch as usize,
    depth: depth as usize,
    keys,
    from: parser.get_int_or("from", 1),
    prepare: !parser.get_flag("no-prepare"),
    force: parser.get_flag("force"),
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
    mgm_client::fetch_configuration(&connect_string, 30_000, Some("ic_bench"));
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
    Some("ic_bench"),
  ) {
    Ok(global) => global,
    Err(e) => {
      report("Could not start the Data API threads", &e);
      return 1;
    }
  };
  let code = bench(&global, &database, &args[0], &what);
  global.stop();
  code
}

fn bench(global: &ApidGlobal, database: &str, table: &str, what: &Run) -> i32 {
  if global.wait_for_all_started(5_000) == 0 {
    println!("No data node is started, so there is nobody to ask");
    return 1;
  }
  let started = global.started_nodes().len();
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
  let mut slots: Vec<Slot> = Vec::with_capacity(what.depth);
  let mut slot_of: HashMap<u32, usize> = HashMap::new();
  while slots.len() < what.depth {
    let mut queries: Vec<QueryId> = Vec::with_capacity(what.batch);
    while queries.len() < what.batch {
      match conn.create_query(&def, &rec, &rec) {
        Ok(id) => {
          slot_of.insert(id.as_u32(), slots.len());
          queries.push(id);
        }
        Err(e) => {
          report("Could not make a query", &e);
          return 1;
        }
      }
    }
    slots.push(Slot {
      queries,
      sent: Vec::new(),
      started: 0,
      done: 0,
    });
  }
  if what.prepare {
    let start = ic_port::time::gethrtime();
    let written = write_rows(&mut conn, &def, &rec, &slots[0].queries, what);
    let ms = ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    match written {
      Ok(()) => println!("Wrote {} row(s) in {} ms", what.keys, ms),
      Err(e) => {
        report("Could not write the rows", &e);
        return 1;
      }
    }
  }
  println!(
    "{} by primary key, {} in flight as {} batch(es) of {}, {} key(s) \
     from {}, {} data node(s), for {} s",
    if what.write {
      "Updates"
    } else {
      "Committed reads"
    },
    what.batch * what.depth,
    what.depth,
    what.batch,
    what.keys,
    what.from,
    started,
    what.seconds
  );
  let mut tally = Tally::default();
  let start = ic_port::time::gethrtime();
  let end_at = start + what.seconds * 1_000_000_000;
  let mut next_key = what.from;
  let mut running = true;
  let mut idle = slots.len();
  // Keep every slot in flight until the time is up, then let them
  // drain. A slot is defined again the moment its batch is done, so
  // the others are still out while this thread packs the next. The
  // completed queries the connection hands out say which slot has
  // its batch back; nothing in flight is looked at until then, so
  // that the cost of a poll does not grow with the depth.
  let mut errors: u64 = 0;
  loop {
    let now = ic_port::time::gethrtime();
    if running && now >= end_at {
      running = false;
    }
    let mut i: usize = 0;
    while i < slots.len() {
      if !slots[i].sent.is_empty() {
        let back = slots[i].done == slots[i].sent.len()
          && batch_done(&conn, &slots[i].sent);
        if !back {
          if now - slots[i].started >= IC_BENCH_BATCH_WAIT_NANOS {
            let e = IcError::new(ic_port::err::IC_ERROR_TIMEOUT);
            report("A batch did not finish", &e);
            return 1;
          }
          i += 1;
          continue;
        }
        let failed = close_batch(&mut conn, &slots[i].sent);
        tally.ops += slots[i].sent.len() as u64 - failed;
        tally.failed += failed;
        tally.note_batch(now - slots[i].started);
        slots[i].sent.clear();
        slots[i].done = 0;
        idle += 1;
      }
      if running {
        let defined = define_batch(
          &mut conn,
          &def,
          &rec,
          &slots[i].queries,
          what,
          next_key,
        );
        slots[i].sent = match defined {
          Ok(sent) => sent,
          Err(e) => {
            report("Could not define a batch", &e);
            return 1;
          }
        };
        slots[i].started = ic_port::time::gethrtime();
        idle -= 1;
        next_key += what.batch as i64;
        if next_key >= what.from + what.keys {
          next_key = what.from;
        }
      }
      i += 1;
    }
    if !running && idle == slots.len() {
      break;
    }
    conn.poll(1);
    while let Some(id) = conn.get_next_executed_query() {
      if let Some(query) = conn.query(id) {
        if let Some(e) = query.error() {
          if errors == 0 {
            println!("An operation failed: {} ({})", e.message(), e.code);
          }
          errors += 1;
        }
      }
      if let Some(slot) = slot_of.get(&id.as_u32()) {
        slots[*slot].done += 1;
      }
    }
  }
  let elapsed =
    ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
  tally.print(elapsed);
  if conn.unexpected() > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      conn.unexpected()
    );
  }
  if tally.failed > 0 {
    return 1;
  }
  0
}

/// One batch's worth of query objects, and its transactions while it
/// is in flight.
struct Slot {
  queries: Vec<QueryId>,
  sent: Vec<TransId>,
  /// When the batch was sent.
  started: u64,
  /// How many of its queries have completed since.
  done: usize,
}

/// What was measured.
#[derive(Default)]
struct Tally {
  ops: u64,
  failed: u64,
  batches: u64,
  /// Nanoseconds each batch took, for the spread.
  batch_nanos: Vec<u64>,
}

impl Tally {
  fn note_batch(&mut self, nanos: u64) {
    self.batches += 1;
    self.batch_nanos.push(nanos);
  }

  fn print(&mut self, elapsed_ms: u64) {
    if elapsed_ms == 0 {
      return;
    }
    let per_second = self.ops * 1000 / elapsed_ms;
    println!(
      "{} operation(s) in {} batch(es) over {} ms: {} per second",
      self.ops, self.batches, elapsed_ms, per_second
    );
    if self.failed > 0 {
      println!("{} operation(s) failed", self.failed);
    }
    if self.batch_nanos.is_empty() {
      return;
    }
    self.batch_nanos.sort_unstable();
    let n = self.batch_nanos.len();
    let median = self.batch_nanos[n / 2];
    let p99 = self.batch_nanos[(n * 99) / 100];
    let slowest = self.batch_nanos[n - 1];
    println!(
      "A batch took {} us from send to done at the median, {} us at the \
       99th percentile, {} us at most",
      median / 1000,
      p99 / 1000,
      slowest / 1000
    );
  }
}

/// Every column other than the key must be an integer, or nullable, and
/// the key one integer column.
fn check_columns(def: &Arc<TableDef>) -> Result<(), String> {
  let mut key_columns: u32 = 0;
  for attr in &def.info().attributes {
    if attr.primary_key {
      key_columns += 1;
      if !is_integer(attr.ext_type) {
        return Err(format!("The key column {} is not an integer", attr.name));
      }
      continue;
    }
    if attr.nullable || is_integer(attr.ext_type) {
      continue;
    }
    return Err(format!(
      "{} is neither an integer nor nullable, so no row can be made",
      attr.name
    ));
  }
  if key_columns != 1 {
    return Err("The primary key must be one column".to_string());
  }
  Ok(())
}

fn is_integer(ext_type: u32) -> bool {
  let integers =
    dict_tab_info::IC_NDB_TYPE_TINYINT..=dict_tab_info::IC_NDB_TYPE_BIGUNSIGNED;
  integers.contains(&ext_type)
}

/// The key, and for a write every other column, into a query's rows.
fn fill_query(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  id: QueryId,
  key: i64,
  values_too: bool,
) -> Result<(), IcError> {
  let query = match conn.query_mut(id) {
    Some(query) => query,
    None => return Err(IcError::new(ic_port::err::IC_ERROR_NO_SUCH_FIELD)),
  };
  for attr in &def.info().attributes {
    let position = match rec.position_of(attr.attribute_id) {
      Some(position) => position,
      None => continue,
    };
    let text = if attr.primary_key {
      key.to_string()
    } else if !values_too {
      continue;
    } else if is_integer(attr.ext_type) {
      (key * 10).to_string()
    } else {
      text_row::IC_NULL_TEXT.to_string()
    };
    let bad = IcError::new(ic_port::err::IC_ERROR_VALUE_TOO_LONG);
    if attr.primary_key {
      let put = text_row::put_value(rec, position, &text, query.key_row_mut());
      if put.is_err() {
        return Err(bad);
      }
    }
    let put = text_row::put_value(rec, position, &text, query.attr_row_mut());
    if put.is_err() {
      return Err(bad);
    }
  }
  Ok(())
}

/// Write every key's row, a batch at a time, before measuring.
fn write_rows(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  queries: &[QueryId],
  what: &Run,
) -> Result<(), IcError> {
  let mut key = what.from;
  let end = what.from + what.keys;
  while key < end {
    let mut sent: Vec<TransId> = Vec::new();
    let mut i: usize = 0;
    while i < queries.len() && key < end {
      fill_query(conn, def, rec, queries[i], key, true)?;
      let trans = start_for(conn, def, rec, queries[i])?;
      let args = WriteKeyArgs {
        kind: WriteKind::Write,
        ..WriteKeyArgs::default()
      };
      conn.write_key(queries[i], trans, &args)?;
      conn.commit_transaction(trans)?;
      sent.push(trans);
      key += 1;
      i += 1;
    }
    conn.send_queries(true)?;
    let start = ic_port::time::gethrtime();
    while !batch_done(conn, &sent) {
      conn.poll(1);
      if ic_port::time::gethrtime() - start >= IC_BENCH_BATCH_WAIT_NANOS {
        return Err(IcError::new(ic_port::err::IC_ERROR_TIMEOUT));
      }
    }
    let mut first_error: Option<IcError> = None;
    while let Some(id) = conn.get_next_executed_query() {
      if let Some(query) = conn.query(id) {
        if let Some(e) = query.error() {
          if first_error.is_none() {
            first_error = Some(e);
          }
        }
      }
    }
    if let Some(e) = first_error {
      println!("An operation failed: {} ({})", e.message(), e.code);
    }
    if close_batch(conn, &sent) > 0 {
      return Err(IcError::new(ic_port::err::IC_ERROR_TRANSACTION_ROLLED_BACK));
    }
  }
  Ok(())
}

/// A transaction hinted to the node holding the query's key.
fn start_for(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  id: QueryId,
) -> Result<TransId, IcError> {
  let hint = match conn.query(id) {
    Some(query) => conn.hint_for_key(def, rec, query.key_row()),
    None => TransactionHint::Any,
  };
  conn.start_transaction(hint, Some(def))
}

/// Define one batch, one transaction per operation, and send it.
fn define_batch(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  queries: &[QueryId],
  what: &Run,
  first_key: i64,
) -> Result<Vec<TransId>, IcError> {
  let mut sent: Vec<TransId> = Vec::with_capacity(queries.len());
  let mut key = first_key;
  let end = what.from + what.keys;
  for id in queries {
    if key >= end {
      key = what.from;
    }
    fill_query(conn, def, rec, *id, key, what.write)?;
    let trans = start_for(conn, def, rec, *id)?;
    if what.write {
      let args = WriteKeyArgs {
        kind: WriteKind::Update,
        ..WriteKeyArgs::default()
      };
      conn.write_key(*id, trans, &args)?;
    } else {
      let args = ReadKeyArgs {
        kind: ReadKind::Committed,
        ..ReadKeyArgs::default()
      };
      conn.read_key(*id, trans, &args)?;
    }
    conn.commit_transaction(trans)?;
    sent.push(trans);
    key += 1;
  }
  conn.send_queries(what.force)?;
  Ok(sent)
}

/// True once every transaction of the batch is done.
fn batch_done(conn: &ApidConnection, sent: &[TransId]) -> bool {
  for trans in sent {
    if let Some(t) = conn.transaction(*trans) {
      if !t.is_done() {
        return false;
      }
    }
  }
  true
}

/// Close a batch's transactions, once it is done. Returns how many did
/// not commit.
fn close_batch(conn: &mut ApidConnection, sent: &[TransId]) -> u64 {
  let mut failed: u64 = 0;
  for trans in sent {
    if let Some(t) = conn.transaction(*trans) {
      if t.commit_state() != CommitState::Committed {
        failed += 1;
      }
    }
    let _ = conn.close_transaction(*trans);
  }
  failed
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
