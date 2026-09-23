// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_bench`: how many primary key reads, or updates, a thread gets
//! through with a few batches of them in flight at a time, and how
//! that scales over threads.
//!
//! ```text
//!   ic_bench -c localhost:1186 -d ictest t9 --keys 1000 --batch 200
//!   ic_bench -c localhost:1186 -d ictest t9 --mode write --seconds 30
//!   ic_bench -c localhost:1186 -d ictest t9 --depth 1
//!   ic_bench -c localhost:1186 -d ictest t9 --threads 4 --keys 4000
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
//! With `--threads` there are that many user threads, each with a
//! connection of its own on the one global and its own share of the
//! keys, all running the same pipeline; the report is the total and
//! the per-thread average. This is where the adaptive send algorithm
//! and the send pool see contention, and where one receive thread's
//! ceiling shows.
//!
//! `--force` makes each batch go at once rather than leaving it to the
//! adaptive send algorithm, which is what an application gets by
//! default; for one thread the two are the same. `--callbacks` has
//! each query report its completion through a callback, carrying its
//! slot in `user_ref`, rather than through the executed list: the same
//! work, and a live check of the callback path. Measure a release
//! build: `cargo run --release -p ic_bench`.
//!
//! The table needs a primary key of one integer column; every other
//! integer column gets ten times the key, a binary or character column
//! gets `--bytes` bytes, and any other column must be nullable.
//!
//! **Large rows.** To find from what size a signal is better handed to
//! the user thread where it lies than copied (`--large-words`, see
//! `ic_apid::signal_page`), use a table with a wide column and vary
//! `--bytes`:
//!
//! ```text
//!   CREATE TABLE ictest.tbig (id INT NOT NULL PRIMARY KEY, v INT,
//!                             data VARBINARY(29000)) ENGINE=NDB;
//!   ic_bench -d ictest tbig --keys 10000 --bytes 4000
//!   ic_bench -d ictest tbig --keys 10000 --no-prepare --large-words 0
//! ```
//!
//! The rows are written with `--bytes` bytes, so that the reads that
//! follow bring that much back; mind the data memory, which is keys
//! times bytes.

use std::collections::HashMap;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_apic::mgm_client;
use ic_apid::apid_conn::ApidConnection;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::apid_global::GlobalOptions;
use ic_apid::apid_global::IC_EXTRA_READS;
use ic_apid::apid_global::IC_LARGE_SIGNAL_WORDS;
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
/// The most user threads.
const IC_BENCH_MAX_THREADS: i64 = 64;
/// One counter per slot of every thread, for `--callbacks`.
const IC_BENCH_MAX_SLOTS: usize =
  (IC_BENCH_MAX_THREADS * IC_BENCH_MAX_DEPTH) as usize;

/// For `--callbacks`: per slot of every thread, how many queries have
/// completed through the callback since the loop last looked, and how
/// many of them failed. A callback is a plain function with only its
/// `user_ref` for context, so the table is static and the reference
/// is the slot's number across all threads; each entry is used by one
/// thread, so the atomics are never contended.
static DONE_BY_CALLBACK: [AtomicUsize; IC_BENCH_MAX_SLOTS] =
  [const { AtomicUsize::new(0) }; IC_BENCH_MAX_SLOTS];
static FAILED_BY_CALLBACK: [AtomicUsize; IC_BENCH_MAX_SLOTS] =
  [const { AtomicUsize::new(0) }; IC_BENCH_MAX_SLOTS];

const OPTIONS: [OptionEntry; 18] = [
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
    long_name: "threads",
    short_name: b't',
    kind: OptionKind::Int,
    help: "User threads, each with its own share of the keys; default 1",
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
    long_name: "bytes",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Bytes written into each binary or character column; default 0",
  },
  OptionEntry {
    long_name: "large-words",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Signals of this many words or more are not copied; 0 copies all",
  },
  OptionEntry {
    long_name: "extra-reads",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Times the receive thread reads a socket again before waiting",
  },
  OptionEntry {
    long_name: "receive-threads",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Receive threads, each reading a share of the data nodes",
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
    long_name: "callbacks",
    short_name: 0,
    kind: OptionKind::Flag,
    help: "Complete queries through a callback, not the executed list",
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
  threads: usize,
  keys: i64,
  from: i64,
  prepare: bool,
  bytes: usize,
  large_words: u32,
  extra_reads: u32,
  receive_threads: u32,
  force: bool,
  callbacks: bool,
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
  let threads = parser.get_int_or("threads", 1);
  let keys = parser.get_int_or("keys", 1000);
  let seconds = parser.get_int_or("seconds", 10);
  if !(1..=IC_BENCH_MAX_BATCH).contains(&batch)
    || !(1..=IC_BENCH_MAX_DEPTH).contains(&depth)
    || !(1..=IC_BENCH_MAX_THREADS).contains(&threads)
    || keys < threads
    || seconds < 1
  {
    println!(
      "The batch is between 1 and {}, the depth between 1 and {}, the \
       threads between 1 and {}, keys at least the threads, and seconds \
       at least 1",
      IC_BENCH_MAX_BATCH, IC_BENCH_MAX_DEPTH, IC_BENCH_MAX_THREADS
    );
    return 1;
  }
  let what = Run {
    write,
    seconds: seconds as u64,
    batch: batch as usize,
    depth: depth as usize,
    threads: threads as usize,
    keys,
    from: parser.get_int_or("from", 1),
    prepare: !parser.get_flag("no-prepare"),
    bytes: parser.get_int_or("bytes", 0).max(0) as usize,
    receive_threads: parser.get_int_or("receive-threads", 1).max(1) as u32,
    extra_reads: parser
      .get_int_or("extra-reads", IC_EXTRA_READS as i64)
      .max(0) as u32,
    large_words: parser
      .get_int_or("large-words", IC_LARGE_SIGNAL_WORDS as i64)
      .max(0) as u32,
    force: parser.get_flag("force"),
    callbacks: parser.get_flag("callbacks"),
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
  let options = GlobalOptions {
    receive_threads: what.receive_threads,
  };
  let mut global = match ApidGlobal::start_with_options(
    config,
    mgm,
    connect_string,
    30_000,
    Some("ic_bench"),
    &options,
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
  global.set_large_signal_words(what.large_words);
  global.set_extra_reads(what.extra_reads);
  let mut conns: Vec<ApidConnection> = Vec::with_capacity(what.threads);
  while conns.len() < what.threads {
    match global.create_connection() {
      Ok(conn) => conns.push(conn),
      Err(e) => {
        report("Could not make a connection", &e);
        return 1;
      }
    }
  }
  let def = match conns[0].table_bind(database, table) {
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
  if what.prepare {
    let queries = match make_queries(&mut conns[0], &def, &rec, what.batch) {
      Ok(queries) => queries,
      Err(e) => {
        report("Could not make a query", &e);
        return 1;
      }
    };
    let start = ic_port::time::gethrtime();
    let written = write_rows(&mut conns[0], &def, &rec, &queries, what);
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
    "{} by primary key, {} thread(s) each with {} in flight as {} \
     batch(es) of {}, {} key(s) from {}, {} data node(s), for {} s",
    if what.write {
      "Updates"
    } else {
      "Committed reads"
    },
    what.threads,
    what.batch * what.depth,
    what.depth,
    what.batch,
    what.keys,
    what.from,
    started,
    what.seconds
  );
  println!(
    "{} byte(s) in each binary or character column; signals of {} word(s) \
     or more not copied; {} receive thread(s)",
    what.bytes, what.large_words, what.receive_threads
  );
  // Each thread goes round its own share of the keys, the last taking
  // the remainder.
  let per_thread = what.keys / what.threads as i64;
  let usage_before = ic_port::time::process_usage();
  let reader_before = ic_apid::signal_reader::reader_stats();
  let send_before = ic_apid::apid_global::send_stats();
  let start = ic_port::time::gethrtime();
  let outcomes: Vec<Result<Tally, String>> = std::thread::scope(|scope| {
    let mut handles = Vec::with_capacity(what.threads);
    for (position, conn) in conns.drain(..).enumerate() {
      let index = position as i64;
      let first = what.from + index * per_thread;
      let mut span = per_thread;
      if position + 1 == what.threads {
        span = what.keys - index * per_thread;
      }
      let range = KeyRange {
        start: first,
        end: first + span,
      };
      let def = &def;
      let rec = &rec;
      handles.push(
        scope.spawn(move || run_thread(conn, def, rec, what, range, position)),
      );
    }
    let mut outcomes = Vec::with_capacity(handles.len());
    for handle in handles {
      outcomes.push(match handle.join() {
        Ok(outcome) => outcome,
        Err(_) => Err("A thread panicked".to_string()),
      });
    }
    outcomes
  });
  let elapsed =
    ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
  let mut tally = Tally::default();
  let mut code = 0;
  for outcome in outcomes {
    match outcome {
      Ok(one) => tally.merge(one),
      Err(text) => {
        println!("{}", text);
        code = 1;
      }
    }
  }
  tally.print(elapsed, what.threads);
  print_usage(usage_before, ic_port::time::process_usage(), tally.ops);
  print_reader(reader_before, ic_apid::signal_reader::reader_stats());
  let (writes, bytes) = ic_apid::apid_global::send_stats();
  let writes = writes - send_before.0;
  if writes > 0 && tally.ops > 0 {
    println!(
      "Send: {} writes of {} bytes on average, {} writes per 1000 \
       operations",
      writes,
      (bytes - send_before.1) / writes,
      writes * 1000 / tally.ops
    );
  }
  if tally.unexpected > 0 {
    println!(
      "{} signal(s) came that nothing waited for",
      tally.unexpected
    );
  }
  if tally.failed > 0 {
    return 1;
  }
  code
}

/// The keys one thread goes round: from `start` up to `end`.
#[derive(Clone, Copy)]
struct KeyRange {
  start: i64,
  end: i64,
}

/// One thread's run: the pipeline over its keys until the time is up.
fn run_thread(
  mut conn: ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  what: &Run,
  range: KeyRange,
  thread_index: usize,
) -> Result<Tally, String> {
  // This thread's slots in the callback table.
  let first_slot = thread_index * IC_BENCH_MAX_DEPTH as usize;
  let mut slots: Vec<Slot> = Vec::with_capacity(what.depth);
  let mut slot_of: HashMap<u32, usize> = HashMap::new();
  while slots.len() < what.depth {
    let queries = match make_queries(&mut conn, def, rec, what.batch) {
      Ok(queries) => queries,
      Err(e) => return Err(describe("Could not make a query", &e)),
    };
    for id in &queries {
      slot_of.insert(id.as_u32(), slots.len());
    }
    slots.push(Slot {
      queries,
      sent: Vec::new(),
      started: 0,
      done: 0,
    });
  }
  let mut tally = Tally::default();
  let start = ic_port::time::gethrtime();
  let end_at = start + what.seconds * 1_000_000_000;
  let mut next_key = range.start;
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
            return Err(describe("A batch did not finish", &e));
          }
          i += 1;
          continue;
        }
        let failed = close_batch(&mut conn, &slots[i].sent, &slots[i].queries);
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
          def,
          rec,
          &slots[i].queries,
          what,
          range,
          next_key,
          first_slot + i,
        );
        slots[i].sent = match defined {
          Ok(sent) => sent,
          Err(e) => return Err(describe("Could not define a batch", &e)),
        };
        slots[i].started = ic_port::time::gethrtime();
        idle -= 1;
        next_key += what.batch as i64;
        if next_key >= range.end {
          next_key = range.start;
        }
      }
      i += 1;
    }
    if !running && idle == slots.len() {
      break;
    }
    conn.poll(1);
    if what.callbacks {
      // The callbacks ran inside that poll and counted into this
      // thread's entries of the table.
      let mut slot: usize = 0;
      while slot < slots.len() {
        let entry = first_slot + slot;
        slots[slot].done += DONE_BY_CALLBACK[entry].swap(0, Ordering::Relaxed);
        let failed = FAILED_BY_CALLBACK[entry].swap(0, Ordering::Relaxed);
        if failed > 0 && errors == 0 {
          println!("An operation failed");
        }
        errors += failed as u64;
        slot += 1;
      }
      continue;
    }
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
  tally.unexpected = conn.unexpected();
  Ok(tally)
}

/// The callback of `--callbacks`: count the query as done for its
/// slot, whose number across all threads is the `user_ref`.
fn note_done(conn: &mut ApidConnection, id: QueryId, slot: usize) {
  let failed = match conn.query(id) {
    Some(query) => query.is_failed(),
    None => false,
  };
  if slot >= IC_BENCH_MAX_SLOTS {
    return;
  }
  DONE_BY_CALLBACK[slot].fetch_add(1, Ordering::Relaxed);
  if failed {
    FAILED_BY_CALLBACK[slot].fetch_add(1, Ordering::Relaxed);
  }
}

/// A batch's worth of query objects on a connection.
fn make_queries(
  conn: &mut ApidConnection,
  def: &Arc<TableDef>,
  rec: &Record,
  count: usize,
) -> Result<Vec<QueryId>, IcError> {
  let mut queries: Vec<QueryId> = Vec::with_capacity(count);
  while queries.len() < count {
    queries.push(conn.create_query(def, rec, rec)?);
  }
  Ok(queries)
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

/// What was measured, by one thread or by all of them together.
#[derive(Default)]
struct Tally {
  ops: u64,
  failed: u64,
  batches: u64,
  /// Nanoseconds each batch took, for the spread.
  batch_nanos: Vec<u64>,
  /// Signals that came which nothing waited for.
  unexpected: u64,
}

impl Tally {
  fn note_batch(&mut self, nanos: u64) {
    self.batches += 1;
    self.batch_nanos.push(nanos);
  }

  fn merge(&mut self, other: Tally) {
    self.ops += other.ops;
    self.failed += other.failed;
    self.batches += other.batches;
    self.batch_nanos.extend_from_slice(&other.batch_nanos);
    self.unexpected += other.unexpected;
  }

  fn print(&mut self, elapsed_ms: u64, threads: usize) {
    if elapsed_ms == 0 {
      return;
    }
    let per_second = self.ops * 1000 / elapsed_ms;
    if threads > 1 {
      println!(
        "{} operation(s) in {} batch(es) over {} ms: {} per second, {} \
         per thread",
        self.ops,
        self.batches,
        elapsed_ms,
        per_second,
        per_second / threads as u64
      );
    } else {
      println!(
        "{} operation(s) in {} batch(es) over {} ms: {} per second",
        self.ops, self.batches, elapsed_ms, per_second
      );
    }
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
    if attr.nullable || is_integer(attr.ext_type) || is_bytes(attr) {
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

/// A binary or character column, which takes `--bytes` bytes.
fn is_bytes(attr: &dict_tab_info::AttributeInfo) -> bool {
  matches!(
    attr.ext_type,
    dict_tab_info::IC_NDB_TYPE_CHAR
      | dict_tab_info::IC_NDB_TYPE_BINARY
      | dict_tab_info::IC_NDB_TYPE_VARCHAR
      | dict_tab_info::IC_NDB_TYPE_VARBINARY
      | dict_tab_info::IC_NDB_TYPE_LONGVARCHAR
      | dict_tab_info::IC_NDB_TYPE_LONGVARBINARY
  )
}

/// `bytes` bytes into a binary or character field of a row, with the
/// length in front for a variable-length one. The bytes are written
/// only when the length changes, so that a row used again and again is
/// filled once.
fn put_bytes(
  rec: &Record,
  attr: &dict_tab_info::AttributeInfo,
  position: u32,
  bytes: usize,
  row: &mut [u8],
) -> Result<(), IcError> {
  let field = match rec.field(position) {
    Some(field) => field,
    None => return Err(IcError::new(ic_port::err::IC_ERROR_NO_SUCH_FIELD)),
  };
  let at = field.offset() as usize;
  let size = field.size() as usize;
  let head = match attr.array_type {
    dict_tab_info::IC_ARRAY_TYPE_SHORT_VAR => 1,
    dict_tab_info::IC_ARRAY_TYPE_MEDIUM_VAR => 2,
    _ => 0,
  };
  if head + bytes > size {
    return Err(IcError::new(ic_port::err::IC_ERROR_VALUE_TOO_LONG));
  }
  if field.is_nullable() {
    rec.set_null(row, position, false)?;
  }
  if head == 0 {
    // A fixed-size column is filled whole.
    if row[at] != b'x' {
      row[at..at + size].fill(b'x');
    }
    return Ok(());
  }
  let current = if head == 1 {
    row[at] as usize
  } else {
    row[at] as usize | (row[at + 1] as usize) << 8
  };
  if current == bytes && (bytes == 0 || row[at + head] == b'x') {
    return Ok(());
  }
  row[at] = bytes as u8;
  if head == 2 {
    row[at + 1] = (bytes >> 8) as u8;
  }
  row[at + head..at + head + bytes].fill(b'x');
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
  bytes: usize,
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
    if values_too && !attr.primary_key && is_bytes(attr) {
      put_bytes(rec, attr, position, bytes, query.attr_row_mut())?;
      continue;
    }
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
      fill_query(conn, def, rec, queries[i], key, true, what.bytes)?;
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
    if close_batch(conn, &sent, queries) > 0 {
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
  range: KeyRange,
  first_key: i64,
  slot: usize,
) -> Result<Vec<TransId>, IcError> {
  let mut sent: Vec<TransId> = Vec::with_capacity(queries.len());
  let mut callback: Option<ic_apid::query::QueryCallback> = None;
  if what.callbacks {
    callback = Some(note_done);
  }
  let mut key = first_key;
  for id in queries {
    if key >= range.end {
      key = range.start;
    }
    fill_query(conn, def, rec, *id, key, what.write, what.bytes)?;
    let trans = start_for(conn, def, rec, *id)?;
    if what.write {
      let args = WriteKeyArgs {
        kind: WriteKind::Update,
        user_ref: slot,
        callback,
        ..WriteKeyArgs::default()
      };
      conn.write_key(*id, trans, &args)?;
    } else {
      let args = ReadKeyArgs {
        kind: ReadKind::Committed,
        user_ref: slot,
        callback,
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
///
/// An operation failed if its transaction did not commit, or if its
/// query failed in a transaction that did: a committed read of a row
/// that is not there fails its query and commits its transaction, and
/// counting transactions alone let a run of reads of missing rows pass
/// for a run of reads. `queries[i]` is the query of `sent[i]`.
fn close_batch(
  conn: &mut ApidConnection,
  sent: &[TransId],
  queries: &[QueryId],
) -> u64 {
  let mut failed: u64 = 0;
  let mut i: usize = 0;
  while i < sent.len() {
    let mut ok = match conn.transaction(sent[i]) {
      Some(t) => t.commit_state() == CommitState::Committed,
      None => true,
    };
    if let Some(query) = queries.get(i).and_then(|id| conn.query(*id)) {
      if query.is_failed() {
        ok = false;
      }
    }
    if !ok {
      failed += 1;
    }
    let _ = conn.close_transaction(sent[i]);
    i += 1;
  }
  failed
}

/// The client's CPU per operation and its sleeps over the run: what
/// taking work off the client should lower, whatever the rate does.
fn print_usage(
  before: ic_port::time::ProcessUsage,
  after: ic_port::time::ProcessUsage,
  ops: u64,
) {
  if ops == 0 {
    return;
  }
  let user = after.user_micros - before.user_micros;
  let system = after.system_micros - before.system_micros;
  let voluntary = after.voluntary_switches - before.voluntary_switches;
  let involuntary = after.involuntary_switches - before.involuntary_switches;
  let faults = after.minor_faults - before.minor_faults;
  println!(
    "Client CPU {} ns per operation ({} ns user, {} ns system); {} \
     sleeps and {} preemptions, {} sleeps per 1000 operations; {} page \
     faults, {} per 1000 operations",
    (user + system) * 1000 / ops,
    user * 1000 / ops,
    system * 1000 / ops,
    voluntary,
    involuntary,
    voluntary * 1000 / ops,
    faults,
    faults * 1000 / ops
  );
}

/// What the receive side did over the run: how large a read was, and
/// how often a reader went on in another page.
fn print_reader(
  before: ic_apid::signal_reader::ReaderStats,
  after: ic_apid::signal_reader::ReaderStats,
) {
  let reads = after.reads - before.reads;
  if reads == 0 {
    return;
  }
  let bytes = after.bytes_read - before.bytes_read;
  println!(
    "Receive: {} reads of {} bytes on average; {} page switches, {} \
     pages allocated, {} KB of tails copied; {} reads again, {} of them \
     empty",
    reads,
    bytes / reads,
    after.page_switches - before.page_switches,
    after.pages_allocated - before.pages_allocated,
    (after.tail_bytes_copied - before.tail_bytes_copied) / 1024,
    after.extra_reads - before.extra_reads,
    after.empty_reads - before.empty_reads
  );
}

fn describe(what: &str, error: &IcError) -> String {
  format!("{}: {} ({})", what, error.message(), error.code)
}

fn report(what: &str, error: &IcError) {
  println!("{}", describe(what, error));
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
