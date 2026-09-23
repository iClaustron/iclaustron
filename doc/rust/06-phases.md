# 06 — Phases, milestones and estimates

Estimates assume one senior developer full time with AI assistance. The
developer runs all builds and tests. Ranges are honest: the low end is
"translation goes as the code map suggests", the high end includes
protocol surprises found only against a live 26.10 cluster. Phases 1–2
are mostly translation; 3–6 are mostly new code; 7 is packaging. The 0.1
release is at the end of Phase 7.

| Phase | Name | Weeks | Cumulative |
|---|---|---|---|
| 0 | Bootstrap and header rewrite | 1–2 | 2 |
| 1 | Foundations (`ic_port`, `ic_util`, `ic_comm`, `ic_protocol`) | 3–4 | 6 |
| 2 | Configuration read (`ic_apic`) | 2 | 8 |
| 3 | Transport, threads, membership, node failure | 4 | 12 |
| 4 | Dictionary and records | 3 | 15 |
| 5 | Transactions and key operations | 4–5 | 20 |
| 6 | Interpreter: WHERE conditions and assignments | 3 | 23 |
| 7 | C ABI, C++ header, examples, benchmark, release 0.1 | 3 | 26 |
| 8 | 0.2: scans, ranges, takeover | 4–5 | 31 |
| 9 | 0.3: events, DDL, autoincrement, TLS | 6–7 | 38 |
| 10 | Later: BLOBs, SPJ, aggregation pushdown, ring buffers, TTL options | open | |

Roughly: 0.1 in 6 months, scans at 7–8 months, the rest within a year.

## Phase 0 — Bootstrap and header rewrite (1–2 weeks)

- Move every existing C directory and build file into `legacy-c/`
  (`git mv api bootstrap build_test cfg cluster_client cluster_mgr
  cluster_server comm doc/bootstrap.txt examples fileserver include pcntrl
  port protocol repserver scripts test util CMakeLists.txt Makefile.am
  configure.ac FindGLIB2.cmake bootstrap.sh.in setup.sh.in count_line.sh.in
  legacy-c/`), add `legacy-c/README.md` saying it is unbuilt reference
  code, replace the root `LICENSE` with MIT, keep `doc/rust/` at the root.
- `rustup` on the development machines; `rust-toolchain.toml`,
  `rustfmt.toml`, `clippy.toml`, `deny.toml`; `cargo xtask` with `check`,
  `style`, `header`, `test-integration` and `tags` (vim tags over Rust and
  C, replacing `git_tags.sh`).
- Workspace with all crates as empty libraries so the dependency graph is
  enforced from the first commit.
- **Rewrite `ic_apid.h`** to chapter 04: records, args structs, flat
  functions, no savepoints, no cluster ids, RonDB 26.10 types. This is the
  design review artefact; the Rust follows it.
- A RonDB 26.10 development cluster: build the tree at
  `/Users/mikael/mysql_trees/rondb_2604_main` with NDB, or use the docker
  images; document start/stop in `doc/dev-cluster.md`.
- Exit: `cargo build --workspace` on the empty workspace; header reviewed.

## Phase 1 — Foundations (3–4 weeks)

Translate the surviving parts of `port/`, `util/`, `comm/`, `protocol/`
per [03-module-map.md](03-module-map.md), in dependency order. Port
`test/test_unit.c` (types 1–6, 8) and `test/test_comm.c`. Each crate gets
its `MODULE.md` with the "Rust notes for C readers" section.

- Exit: unit tests pass on Linux (epoll) and macOS (kqueue); a loopback
  client/server exchange over `Connection` with a reader and a writer
  thread works; the option parser handles the `ic_apid_entries[]` set.
- Proof: `cargo test -p ic_port -p ic_util -p ic_comm -p ic_protocol`.

## Phase 2 — Configuration read (2 weeks)

- `mgm_client.rs`: connect, `get nodeid`, `get config_v2`, `get status`,
  `get connection parameter`; keep the connection for later use.
- `conf_blob.rs`: v2 decode into generic sections; `conf_param.rs`: the
  API-node subset table verified against 26.10
  `mgmapi_config_parameters.h`; `data.rs`: typed config.
- `tools/ic_config_dump`.
- Exit: node id allocated from a live 26.10 `ndb_mgmd`; every node and
  TCP section printed; the typed config matches `ndb_config` output for
  the parameters we type.
- Proof: `cargo run -p ic_config_dump -- localhost:1186`;
  `cargo test -p ic_apic`.

## Phase 3 — Transport, threads, membership, node failure (4 weeks)

- `ic_ndb_signals`: header codec, block references, GSN table,
  `qmgr.rs`; fuzz target for the header decoder.
- `ic_apid`: translate send threads, receive threads (receive + route
  only), heartbeat thread, adaptive send, `global.rs`, `program.rs`,
  `connection.rs` (`poll`/`send`/`flush`). Implement the 4-integer hello
  and the TLS-capable auth line (cleartext).
- Node failure: missed heartbeats, `NODE_FAILREP`, `NF_COMPLETEREP`,
  per-thread node-down notices, reconnect with backoff, `API_REGREF`
  handling.
- Exit: the port of `test_api_basic` connects to all data nodes of a
  2-node cluster, receives `API_REGCONF` from each, stays connected for 10
  minutes, survives `ndb_mgm -e "<id> RESTART"` and an `ndb_mgmd` restart,
  reconnects; unknown GSNs are logged and dropped; `IC_DEBUG` signal
  tracing prints every signal.
- Proof: `IC_TEST_CONNECTSTRING=localhost:1186 cargo test -p ic_apid
  --features integration connect`.

## Phase 4 — Dictionary and records (3 weeks)

- `dict.rs` codecs: `GET_TABINFOREQ/CONF/REF`, SimpleProperties,
  DictTabInfo keys and defaults for 26.10, `LIST_TABLES`.
- `dict_client.rs`: `table_bind`/`index_bind` send and parse; hash map
  fetch; global cache with ref counts and versions.
- `record.rs`: `FieldSpec` → `Record` with validation; default record;
  full `FieldType` list with sizes.
- `tools/ic_desc`.
- Exit: `ic_desc` matches `ndb_desc` for a table with every column type, a
  unique index and an ordered index; `ALTER TABLE` bumps the version and
  the next bind refetches.
- Proof: integration group `dict`.

## Phase 5 — Transactions and key operations (4–5 weeks)

- `hash.rs`: MD5 and XXH3 64-bit (not XXH64; see 05 §6.3), distribution
  key assembly from a record,
  fanout tables, hash map lookup, replica selection (read backup, fully
  replicated).
- `transaction.rs`: TC seize per thread per node, ids, start/execute/
  commit flags, completion accounting (05 §6.6), `TC_COMMITREQ`,
  `TCROLLBACKREQ`, `TC_COMMIT_ACK`, node-failure outcomes,
  `TC_DEADLOCK_REP`.
- `tc.rs` codecs; `tc_handlers.rs`; `row_codec.rs` (record ↔ KEYINFO,
  ATTRINFO, `TRANSID_AI`) for every type; `query.rs`; unique key
  operations via `TCINDXREQ`.
- Error table with classification and our own messages.
- `tools/ic_bench` (PK read, PK write). As built (2026-09-22): one
  thread, `--depth` batches (two by default) of `--batch` (200 by
  default) committed reads or updates in flight, each operation its own transaction
  hinted to the node holding its row; a batch is defined again the
  moment it is done, while the others are still out, so the data nodes
  have work while the thread packs and unpacks. `--depth 1` is the
  lock-step form, which the numbers below were measured with.
  `--threads` runs that many user threads on the one global, each with
  its own connection and share of the keys; `--callbacks` completes
  the queries through callbacks instead of the executed list, the same
  work by the other path. Reports operations per
  second, per thread too, and the time from send to done of a batch,
  at the median, 99th percentile and worst. Measure a release build.

  **Baseline, 2026-09-22**, release build, one thread, a two-node
  RonDB 26.10 cluster on the same machine as the client, a table of
  two INT columns, 100 operations in flight: committed reads 311 000
  per second, a batch taking 306 µs at the median and 534 µs at the
  99th percentile; updates 84 500 per second, a batch 1.18 ms at the
  median, 1.5 ms at the 99th and 11 ms at worst. About 3 µs per read
  on the API side, with one socket write per operation, which was the
  send path before step 4 of the thread plan.

  **What that cost, 2026-09-22.** `flexAsynch` (the NDB API's own
  tool) on the same cluster, two threads, 100 transactions in flight
  per thread and ten reads in each, did 1 425 000 reads per second,
  and the author saw the data nodes use about four times the CPU for
  `ic_bench` at a quarter of the rate. One packet per operation is the
  reason: each packet costs a data node a receive, an execution round
  of one signal, and a reply packet, where a hundred operations in one
  packet cost it one of each. Step 4 gathers a batch into one write
  per node.

  **After step 4, 2026-09-22**, same cluster, table and settings:
  committed reads 734 650 per second, a batch of 100 taking 133 µs at
  the median, 176 µs at the 99th percentile and 291 µs at most;
  updates 179 940 per second, a batch 547 µs at the median, 750 µs at
  the 99th and 11.7 ms at worst. The adaptive send and `--force` give
  the same figures, as a thread sending alone in lock step never has
  its sends held. That is 2.4× the reads and 2.1× the updates from one
  write per node instead of one per operation. The data nodes felt it
  more: `top` showed `ndbmtd` at 400% during the read run before and
  135% after, which is 12.9 µs of data-node CPU per read down to 1.8,
  seven times less per operation.

  **With the pipeline, 2026-09-22**, `--depth` batches of 100 in
  flight: depth 1 (lock step) 731 000 reads per second, a batch 97 µs
  from send to done at the median; depth 2 970 000–981 000, 145 µs;
  depth 4 859 000, 323 µs; updates at depth 2 288 700, 603 µs. Depth 2
  is where one client thread saturates with this build: `ic_bench`
  uses about the same CPU at depths 2 and 4, and depth 4 only
  lengthens the queue. The data nodes are not the limit: at depth 4
  `ndbmtd` carried half its depth-2 load for 90% of the work, and at
  depth 1 the same load as at depth 4 for 85% of it, so the cost per
  operation there depends on how the traffic bunches, and more in
  flight per client is cheaper for them. Per thread the client rate is
  above the `flexAsynch` run's 713 000, which had ten reads per
  transaction. The like-for-like `flexAsynch` run (one thread, one read
  per transaction, `-t 1 -p 100 -c 1 -o 1`) is still to be made, and
  so is the split of the client's CPU between its user thread and its
  receive thread, which decides whether more user threads scale before
  step 5 of the thread plan (several receive threads).

  **Where the client's time went, 2026-09-22** (`sample` over ten
  seconds of the depth-2 read run, one sample a millisecond): the
  receive thread was idle 73% of the time, in `kevent`, and the user
  thread busy 92% of it. Of the user thread's time, 31% was in
  `tc_record`, which scanned every coordinator record this thread
  held, and checked every record's link, to find a free one; about
  13% in freeing a transaction, which scanned the active list to take
  it out, and in naming the transaction a reply is for, which scanned
  it again; about 23% in malloc, free and memmove, mostly the vectors
  a signal arrives in, allocated by the receive thread and freed by
  the user thread, and the key, attribute and row vectors a query is
  defined with; 5% in the socket writes; 3% unpacking rows. The three
  scans were made constant time on the spot (free records kept per
  node, the active list a map by coordinator record, the row buffer
  kept across executions); the allocations are phase 7's "allocation
  removal", whose largest part is giving signals pages of their own as
  the C does (`IC_SOCK_BUF_PAGE`) rather than a vector each.

  **With the scans gone, 2026-09-22**: at batch 100, depth 1 gained
  10% and depth 2 5% for 5% and 20% less client CPU, and depth 4 went
  from falling behind depth 2 to twice depth 1, the user thread no
  longer the ceiling. The batch size then turned out to matter more
  than the depth: `--batch 400 --depth 1` 1 407 800 reads per second,
  182 µs a batch; `--batch 200 --depth 2` 1 746 440, 158 µs a batch,
  from one thread, with the same 400 in flight as batch 100 at depth
  4. A round costs both sides a fixed part, the write, the receive,
  the execution round and the reply packets, and 200 operations a
  packet amortise it far better than 50. That one thread is above the
  whole two-thread `flexAsynch` run, which had ten reads a transaction
  against our one, so the phase-5 exit criterion holds for one thread
  pending the like-for-like run. Still to settle: the data nodes' CPU
  per operation rose with the load at batch 100 (4× the CPU for 2× the
  work at depth 4). The `ndbmtd` reading settled it: batch 200 at
  depth 2, half the packets of batch 100 at depth 4 for the same 400
  in flight, took the nodes from 260% to 180% while the rate rose 20%,
  about 40% less data-node CPU per read. The nodes' cost is per round,
  not per operation, and operations per packet is the lever; the
  tool's defaults are batch 200, depth 2. Updates at those defaults:
  481 800 per second, 721 µs a batch, measured through callbacks
  (`--callbacks`), which read the same as the executed list for reads.

  **Over threads, 2026-09-22**, at those defaults, 400 in flight per
  thread: one thread 1 730 000 reads per second, 160 µs a batch; two
  threads 2 345 000, 1 172 000 each, 264 µs; four threads 2 816 000,
  704 000 each, 491 µs. The batch time growing with the thread count
  at the same depth says the rounds queue at something shared. The
  four-thread sample said which: the receive thread was still 31%
  idle, and the user threads 54% idle, waiting for replies, so nobody
  on the client was saturated and the queue was at the data nodes,
  which share this laptop's cores with the client. Step 5 of the
  thread plan is not called for at this rate; the receive thread
  would carry about 4 million reads a second before it is. The send
  pool never ran: with four threads the sends to a node were still
  about 120 µs apart, above the adaptive send's limit, so nothing was
  held. Of the client's remaining CPU per operation, a quarter is
  malloc, free and memmove, phase 7's allocation removal; the vector
  of started nodes that every transaction start allocated was made a
  buffer the connection keeps.

  **Against the C++ NDB API, 2026-09-22.** `flexAsynch` run like for
  like on the same cluster, one read per transaction and the same
  operations in flight, delivered slightly fewer reads per second than
  `ic_bench` and used slightly less CPU doing it: the same work at the
  same cost, within the noise of one machine. The phase-5 exit
  criterion, within 2× of the C++ API on one thread, is met at parity.
  Phase 7's 20% target is now a statement about CPU per operation, and
  the sample says where ours goes: the quarter in allocation.

  **Allocation removal, first try, 2026-09-23, rolled back.** Taking
the send side's allocations away (the sections into buffers kept per
query, then per connection; the distribution key on the stack; a
transaction's queries inline) cut the client's CPU and still lost:
1.60 to 1.64 million reads a second against 1.73, the batch 194 µs
against 166, at the same rate with `--force`. Spinning before sleeping
in the user and receive threads, the data nodes' answer to a thread
that goes idle sooner, won part of it back. What stayed unexplained is
likely cache: 400 query objects' buffers are colder than the
allocator's just-freed blocks, and the transaction grew a cache line.
The 1 000-row benchmark magnifies every client microsecond, as the
data nodes serve it from cache; the next try measures on a million
rows. The code went back to the version measured at 1.73 million.

  **Signals in pages, 2026-09-23.** The measured costs were on the
  receive side: the read buffer moved its whole remainder to the front
  once per signal consumed, and every signal became a 64-byte structure
  with its words in an allocation of their own, made on the receive
  thread and freed on the user thread. Now the reader consumes by
  offset and compacts once per read; the receive thread copies a small
  signal straight from the read buffer into the user thread's page for
  the round, which goes through the inbox whole and comes back emptied;
  the user thread reads every signal in place (chapter 02, "Signals in
  pages"). On `t9` at the defaults, a million rows: 1 757 000 reads a
  second at 665 ns of client CPU per read, against 1 730 000 at 753 ns
  before, 12% less. A step before, taking the transaction's two query
  lists off the heap into the queries (the C's intrusive list) measured
  within the noise, 626 to 782 ns against 657 to 756 ns across depths,
  and was rolled back: the send side's small, same-thread allocations
  are cheap, and the object that grows by their removal costs more.

  **A correction, 2026-09-23.** The million-key runs of `t9` above,
  E's 1 757 000 at 665 ns among them, read rows that were never
  written: the table had been prepared with a thousand keys. A committed
  read of a missing row fails its query and commits its transaction,
  and the bench counted transactions, so the runs passed; updates
  failing to a man gave it away. The bench now counts a failed query
  as a failed operation. The comparisons between builds hold, each
  having done the same work, but the workload was not the one meant.

  **Requests packed at define, 2026-09-23.** A request is encoded whole
  when its query is defined, into a staged buffer per node the
  connection keeps, its sections built in two scratch buffers the
  connection keeps; the send only sets the start, execute and commit
  bits in its flags word and hands the staged buffer over as the
  outgoing one, without a copy. A query's execution state shrinks by
  36 bytes, the two section allocations and the encoder's are gone, and
  the send loop touches one word a query. A request that does not go
  after all, rolled back or lost before the send, is cut out when the
  send happens. On a million real rows, four threads, against the same
  build without it: 4 336 000 reads a second at 754 ns per read against
  4 318 000 at 785 ns; one thread with it, 1 770 000 and 1 789 000 at
  676 and 670 ns. Reads of missing rows, one thread, the same
  comparison: 638 ns against 665. About 4% less client CPU either way,
  and the first change to
  the send side that measured better: it keeps the layout that had
  won, the sections built together at define and read in order, and
  removes a pass over them.

  **The hint's key on the stack, 2026-09-23.** `hint_for_key` builds
  the distribution key in a 128-byte array on the stack, falling back
  to the heap for a longer key, instead of in a vector per transaction.
  The hint comes before the transaction and its request, so the key
  section packed at define cannot serve it; the bytes and the rules
  are the same. One allocation less a transaction, kept by the author
  after measuring; its share is below what two runs resolve. With it,
  the order set for the allocation work is done: the intrusive query
  lists (D) measured no better and were rolled back, the arena (A) is
  what packing at define (B) became, and signals in pages (E, E2) and
  packing at define (B) are the ones that paid.

  **A whole row unpacked from its signal, 2026-09-23.** A read's row
  went from its signal into a buffer each query keeps, then into the
  attribute row. When it comes whole, as a committed read's always does
  and another's does when the confirmation has already given its
  length, it is unpacked straight from the page into the attribute row;
  a large row is then copied once after `recv`, into its final place.
  On the wide table, by reference at the default limit: 16 KB rows 2 426
  and 2 428 ns per read against 2 718 and 2 724, user time 1 320 against
  1 660; 29 KB rows 4 821 and 5 166 against 5 239 and 5 546, user time
  2 060 against 2 580, the difference a row's copy at about 20 GB/s.
  The rate at 16 KB came out lower than on the earlier day, 566 000
  against 620 000, with the client using less, so the limit there is
  not the client; the effect on `t9`'s small rows was not measured
  apart.

  **Reading a socket again, 2026-09-23.** After a read, the receive
  thread reads the same socket again without waiting (`recv` with
  `MSG_DONTWAIT`, the socket left blocking for the writes), up to four
  times, but only while a read fills all the room it was given, the
  author's rule: a read that comes short took all there was. Reading
  again after every read found nothing nine times in ten and saved
  nothing, `kevent` already waking the thread as data came. With the
  rule, on `t9` no read ever filled its room, so nothing changes and
  nothing is paid (732 to 738 ns at four threads either way); on 29 KB
  rows the reads made again found data all but once or twice in a
  hundred thousand, user time fell 12% (1 800 against 2 040 ns), the
  preemptions by a third, and CPU per read to 4 595 and 4 655 ns
  against 4 705 and 5 115. The default is four
  (`ApidGlobal::set_extra_reads`, `--extra-reads`).

  **Commit acknowledgements go with the next send, 2026-09-23.** An
  update cost about 1 150 ns of client CPU against 670 for a read, half
  of it system time. A profile of updates showed the client waiting
  most of the time, the user thread 65% idle and the receive thread
  91%, and of the user thread's work 27% in `sendto`, four fifths of
  it at the end of `poll`: every committed write whose confirmation
  carries a marker is acknowledged, and each poll wrote its
  acknowledgements as it made them. They now wait in the node's
  outgoing buffer and go with the next send's requests, in one write; a
  poll writes them itself once they have waited 1 ms or when nothing is
  in flight, and closing a connection writes what waits. Writes per
  1 000 updates fell from 44 to 13, a little over one per node per
  batch, and client CPU per update from 1 150 to 1 007 and 1 089 ns;
  the rate, set by the data nodes' commit, stayed at about 460 000.

  Large signals stay in the receive page and are read there, the page
  counted by an `Arc` (the C page's atomic) and sealed while held.
  Reading a table with a `VARBINARY(29000)` column filled to a given
  size, 10 000 rows, by reference against copying, client CPU per read:
  equal to 1 KB, 2 to 7% less from 2 to 16 KB, and at 29 KB within
  the noise: two runs each gave 5 077 and 5 529 ns copying, 5 546 and
  5 239 by reference, where single runs had seemed to show by
  reference 7% behind. Single five-second runs at these sizes spread
  by 5%, so a comparison takes two or more. Counters on the readers
  (`signal_reader::reader_stats`, printed by the bench with the page
  faults) show the trade: by reference saves the receive thread's copy
  of each row, and pays with the part of a row a read ends in, copied
  into the next page since a page someone holds is not written (13% of
  the bytes, 2.2 KB a read at 16 KB rows), and with `recv` writing into
  whichever page is free rather than into one page that stays in cache
  (150 ns more system time at 16 KB). No page is allocated after the
  first few dozen, and page faults are nil in both modes; reads average
  100 KB in both. Pages of 64 KB instead of 128 made both modes slower,
  twice the reads for large rows. The first release kept too few free
  pages and let them all go at once, which doubled system time at 29
  KB until fixed; free pages are now taken most recently sealed first.
  Signals of 256 words and up go by reference
  (`set_large_signal_words`). The limit, measured, 2026-09-23: at
  512-byte rows, a `TRANSID_AI` of about 138 words, copying and not
  copying gave the same at one thread and at four; at 1 KB rows, about
  266 words, not copying was ahead, 821 against 833 ns at one thread
  (every run), and at four threads 0.5% less CPU, 2% more rate and
  fewer page faults, the user threads' pages not growing to hold the
  rows. The best limit lies between those, and 256 is at its top; the
  spread of a page's release over threads that copying small signals
  avoids did not show at four threads at either size.
- Exit: integration groups `pk`, `uk`, `types`, `failure` pass; a 1-thread
  asynchronous PK read benchmark is within 2× of the C++ NDB API (the
  20 % target is Phase 7). Met as of 2026-09-23: the benchmark at
  parity on 2026-09-22, and the integration groups `connect`, `dict`,
  `pk`, `uk`, `types` and `failure` passing against the two-node
  cluster (chapter 08). Writing them found one protocol fault the
  tools had never reached: `TC_COMMITREQ` and `TCROLLBACKREQ` carried
  our pointer for the transaction where the coordinator wants its own,
  so a commit or rollback with nothing left to send got no reply; every
  commit until then had ridden on a query's commit flag. The `failure`
  group is the takeover path seen live: a pending transaction at a
  restarted coordinator ended rolled back with 4010 and its retry at
  the survivor committed.

## Phase 6 — Interpreter (3 weeks)

- Record the RonDB 26.10 interpreter instruction encoding into 05 §9 from
  reading the sources (clean room); implement `interp.rs` with the full
  instruction set.
- `where_cond.rs` + `interp_gen.rs`: builder → program; register
  allocation over the 8 registers, `IC_ERROR_CONDITION_TOO_COMPLEX` when
  more are live (spill strategy deferred); short-circuit AND/OR/XOR; LIKE;
  API-side regexp.
- `cond_assign.rs`: arithmetic and conditional assignments on
  `write_key`.
- Exit: integration groups `where`, `assign` pass on generated data
  compared with MySQL.

## Phase 7 — C ABI, C++ header, examples, release 0.1 (3 weeks)

- `ic_capi` with every public method; cbindgen header checked in and
  diffed against the Phase 0 hand-written header; `pkg-config` file;
  `cargo xtask install`.
- `cpp/include/iclaustron.hpp`.
- Examples in Rust, C and C++; smoke bindings in the languages chosen in
  Q6.
- Performance pass with the benchmark against the C++ NDB API: batching,
  adaptive send tuning, allocation removal, receive thread count.
- **Measure the key hash against the C** (author, 2026-09-22), in a
  release build, over the key sizes RonDB really hashes: 4, 8, 16 and 32
  bytes, and one long key. Up to 240 bytes XXH3 runs no accumulator
  loop, so both sides are the same few multiplies and rotates and ought
  to come out level; above that the data nodes use a vector path
  (`rondb_xxhash_avx2`, and the bundled xxHash's SSE2 or NEON) while
  `ic_util::xxh3` is scalar, so long keys will be slower until a vector
  path is added behind a target-feature check, with the scalar one as
  fallback and the same test vectors pinning both. Watch for bounds
  checks on the key buffer, and never measure a debug build: its
  overflow checks and absent inlining make the comparison meaningless.
- Documentation: user guide, `cargo doc`, per-crate `MODULE.md`.
- Exit: the 0.1 success criteria in
  [01-goals-and-principles.md](01-goals-and-principles.md).

## Phase 8 — 0.2: scans (4–5 weeks)

`scan.rs` codecs, `scan_handlers.rs`, receiver ids per fragment, batch
sizing from config, `SCAN_NEXTREQ`, `scan_next_row`, `scan_close`;
`range.rs` (`RangeCondition` → bound sections, multi-range); takeover
updates/deletes; interpreter filters on scans. Exit: scan of 1 M rows with
parallelism = fragments counts match MySQL; range scans with every bound
type.

## Phase 9 — 0.3 (6–7 weeks)

Event API with fragmented signal reassembly and epochs; DDL through `MetadataTransaction` (carry over the C
schema-transaction code); autoincrement (`SYSTAB_0`); TLS to data nodes and
`ndb_mgmd` behind the `tls` feature.

## Phase 10 — Later

BLOB/TEXT through the parts table (skipped for now; tables with BLOB
columns can be used as long as those columns are not touched), SPJ
pushdown joins, aggregation pushdown, ring buffer tables, TTL options,
rate-limit/user-id flags, location-domain node selection.

## Working method per module (all phases)

1. Read the C header and source; copy the header prose into Rust doc
   comments (it is our text). Check every protocol constant against 26.10.
2. Write the Rust module with the same file name and function names.
3. Port or write unit tests; for protocol code add a golden vector from a
   capture.
4. Write the crate `MODULE.md`: source files, deviations, glib
   replacements, open items, and a "Rust notes for C readers" section
   explaining any construct not covered by chapter 11.
5. `cargo fmt`, `cargo clippy`, `cargo xtask style`; review against
   [07-style-guide.md](07-style-guide.md).

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| iClaustron's protocol knowledge is NDB 7.2.9; anything not re-verified against 26.10 is suspect | every constant in `ic_ndb_signals` carries a 26.10 verification pointer; Phase 3 signal tracing shows mismatches immediately |
| Legacy vs TLS-capable socket auth (Q2) | test both on day one of Phase 3 |
| Interpreter encoding is large | Phase 6 starts by writing the spec section; key-lookup use only in 0.1; register-only programs, spill strategy decided later |
| Performance below target with dedicated receive threads | benchmark at Phase 5 and 7; tune receive thread count, batching, page sizes; the model itself is fixed |
| Clean-room slips | review checklist item; `MODULE.md` cites, never quotes |
| Scope creep into RonDB-only features | Phase 10 by decision |
