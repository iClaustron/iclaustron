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
| 6 | Our own names; the Wireshark dissector; key operations completed: tables with BLOB columns, the reply timeout, unique keys, every row size | 5–6 | 26 |
| 7 | C API, documentation, examples in Rust and C, release 0.1 | 3–4 | 30 |
| 8 | 0.2: the executor, scans (asynchronous, synchronous, through the executor), character sets from the data nodes | 8–9 | 39 |
| 9 | 0.3: basic interpreter, AUTO_INCREMENT, tables without a primary key | 4–5 | 44 |
| 10 | Later: TLS, location domains, BLOBs, UTF-8, the C++ header, events, DDL, pushdown joins, … | open | |

**Replanned 2026-10-08** by the author. 0.1 is now the key operations
made complete and the C API with its documentation and examples; the
interpreter moves from before 0.1 to 0.3, after scans, and starts basic;
AUTO_INCREMENT comes with it; TLS and location domains are not in these
phases. Later the same day the author moved latin1 from phase 6 to phase
8, before the ordered range scans that first need it. Phases 0–5, sized at 20 weeks, were built between 2026-09-18 and
2026-09-25; the sizes of phases 6–9 keep the original unit so that the
phases compare with each other, not as a calendar.

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
  API-node subset table verified against 26.10's parameter ids
  (chapter 05 §5.2); `data.rs`: typed config.
- `tools/ic_config_dump`.
- Exit: node id allocated from a live 26.10 management server; every
  node and TCP section printed; the typed config matches the output of
  the reference's configuration tool for the parameters we type.
- Proof: `cargo run -p ic_config_dump -- localhost:1186`;
  `cargo test -p ic_apic`.

## Phase 3 — Transport, threads, membership, node failure (4 weeks)

- `ic_signals`: header codec, block references, signal table,
  `membership.rs`; fuzz target for the header decoder.
- `ic_apid`: translate send threads, receive threads (receive + route
  only), heartbeat thread, adaptive send, `global.rs`, `program.rs`,
  `connection.rs` (`poll`/`send`/`flush`). Implement the 4-integer hello
  and the TLS-capable auth line (cleartext).
- Node failure: missed heartbeats, `NODE_LOST_REP`, `NODE_LOST_DONE_REP`,
  per-thread node-down notices, reconnect with backoff, `HEARTBEAT_REF`
  handling.
- Exit: the port of `test_api_basic` connects to all data nodes of a
  2-node cluster, receives `HEARTBEAT_CONF` from each, stays connected
  for 10 minutes, survives a data node restarted from the management
  client and a restart of the management server, reconnects; unknown
  signal numbers are logged and dropped; `IC_DEBUG` signal tracing
  prints every signal.
- Proof: `IC_TEST_CONNECTSTRING=localhost:1186 cargo test -p ic_apid
  --features integration connect`.

## Phase 4 — Dictionary and records (3 weeks)

- `dict.rs` codecs: `TABLE_DESC_REQ/CONF/REF`, the property stream,
  TableProps keys and defaults for 26.10, `LIST_OBJECTS`.
- `dict_client.rs`: `table_bind`/`index_bind` send and parse; hash map
  fetch; global cache with ref counts and versions.
- `record.rs`: `FieldSpec` → `Record` with validation; default record;
  full `FieldType` list with sizes.
- `tools/ic_desc`.
- Exit: `ic_desc` matches the reference's table description tool for a
  table with every column type, a unique index and an ordered index;
  `ALTER TABLE` bumps the version and the next bind refetches.
- Proof: integration group `dict`.

## Phase 5 — Transactions and key operations (4–5 weeks)

- `hash.rs`: MD5 and XXH3 64-bit (not XXH64; see 05 §6.3), distribution
  key assembly from a record,
  fanout tables, hash map lookup, replica selection (read backup, fully
  replicated).
- `transaction.rs`: coordinator records allocated per thread per node,
  ids, start/execute/commit flags, completion accounting (05 §6.6),
  `COMMIT_REQ`, `ROLLBACK_REQ`, `COMMIT_MARKER_FREE_ORD`, node-failure
  outcomes, `DEADLOCK_FOUND_REP`.
- `coord.rs` codecs; `coord_handlers.rs`; `row_codec.rs` (record ↔
  `KEY_DATA_ORD`, `COLUMN_DATA_ORD`, `ROW_DATA_REP`) for every type;
  `query.rs`; unique key operations via `UNIQUE_OP_REQ`.
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

  **What that cost, 2026-09-22.** The reference's benchmark (a tool of
  the reference API) on the same cluster, two threads, 100 transactions
  in flight per thread and ten reads in each, did 1 425 000 reads per
  second,
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
  more: `top` showed the data node at 400% during the read run before and
  135% after, which is 12.9 µs of data-node CPU per read down to 1.8,
  seven times less per operation.

  **With the pipeline, 2026-09-22**, `--depth` batches of 100 in
  flight: depth 1 (lock step) 731 000 reads per second, a batch 97 µs
  from send to done at the median; depth 2 970 000–981 000, 145 µs;
  depth 4 859 000, 323 µs; updates at depth 2 288 700, 603 µs. Depth 2
  is where one client thread saturates with this build: `ic_bench`
  uses about the same CPU at depths 2 and 4, and depth 4 only
  lengthens the queue. The data nodes are not the limit: at depth 4
  the data node carried half its depth-2 load for 90% of the work, and
  at depth 1 the same load as at depth 4 for 85% of it, so the cost per
  operation there depends on how the traffic bunches, and more in
  flight per client is cheaper for them. Per thread the client rate is
  above the 713 000 of the reference's benchmark, which had ten reads
  per transaction. The like-for-like run of that benchmark (one thread,
  one read per transaction, 100 transactions in flight, one a round) is
  still to be made, and so is the split of the client's CPU between its
  user thread and its
  receive thread, which decides whether more user threads scale before
  step 5 of the thread plan (several receive threads).

  **Where the client's time went, 2026-09-22** (`sample` over ten
  seconds of the depth-2 read run, one sample a millisecond): the
  receive thread was idle 73% of the time, in `kevent`, and the user
  thread busy 92% of it. Of the user thread's time, 31% was in
  `coord_record`, which scanned every coordinator record this thread
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
  whole two-thread run of the reference's benchmark, which had ten
  reads a transaction against our one, so the phase-5 exit criterion
  holds for one thread pending the like-for-like run. Still to settle:
  the data nodes' CPU per operation rose with the load at batch 100 (4×
  the CPU for 2× the work at depth 4). The data nodes' CPU reading
  settled it: batch 200 at
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

  **Against the reference API, 2026-09-22.** The reference's benchmark,
  run like for like on the same cluster, one read per transaction and
  the same operations in flight, delivered slightly fewer reads per
  second than `ic_bench` and used slightly less CPU doing it: the same
  work at the same cost, within the noise of one machine. The phase-5
  exit criterion, within 2× of the reference API on one thread, is met
  at parity. Phase 7's 20% target is now a statement about CPU per
  operation, and the sample says where ours goes: the quarter in
  allocation.

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

  **Spinning in the receive thread, 2026-09-23, not kept.** Checking
  the sockets without waiting for up to 10, 50 or 200 µs after a
  round, before blocking in `kevent`, as the data nodes spin before
  they sleep, lowered performance in the author's runs, and was taken
  out again. Likely because, with the reads made again after a full
  read, the receive thread already takes what has arrived before it
  blocks, and a spin then only takes CPU from the data nodes, which
  share this machine's cores. The poll set keeps its event buffer
  between checks.

  **Several receive threads, 2026-09-23.** Built as step 5 of the
  thread plan (`GlobalOptions::receive_threads`), one by default. With
  two data nodes on this machine, two receive threads, one per node,
  against one: at 1, 4 and 8 user threads the rate was the same or a
  little lower and client CPU 10 to 14% higher (1: 730 against 660
  ns; 4: 770 to 810 against 711 to 717; 8: 864 to 874 against 757 to
  759), each user thread now getting a post from each receive thread,
  with the locks and wake-ups that go with them. One receive thread
  carried 6 110 000 to 6 180 000 committed reads a second from eight
  user threads without being the limit. Several are for machines with
  many data nodes and cores, measured the same way
  (`ic_bench --receive-threads`).

  On Linux, 32 CPUs, the same two data nodes, it is otherwise: one
  receive thread becomes the limit between four and eight user
  threads. One user thread: two receive threads cost 10% more CPU for
  the same rate, as on the Mac. Four: 5 050 000 and 5 560 000 reads a
  second against 4 750 000 and 4 930 000, CPU per read the same. Eight:
  with one receive thread the rate stopped at 5 320 000 to 5 480 000,
  the batch time doubled to 485 µs and each user thread fell to 670 000
  a second, the rounds queuing at the receive thread; with two, 7 120
  000 in one run and 5 620 000 in the other, where thread placement on
  32 cores, shared with the data nodes, likely made the spread. The
  Mac's faster cores were all busy, so its client never outran one
  receive thread. Linux counts voluntary switches: 20 to 45 sleeps per
  1 000 reads. With two data nodes, two receive threads is the most.

  The i9-13900K is hybrid: CPUs 0 to 15 are eight performance cores
  with two threads each, 16 to 31 sixteen efficiency cores. Pinned,
  the side on the efficiency cores sets the ceiling. The client on
  them, the data nodes on the performance cores: about 3.5 million
  reads a second, a single read costing 1 010 ns of client CPU against
  690. The other way round: 2.2 million whatever the user and receive
  threads, the batch time doubling with the user threads, and each data
  node with four threads busy, its receive thread at 93 to 100% and the
  others at 60%, the 270% `top` showed in total. The data nodes had
  sized their threads for a small machine when they started; more
  receive threads per data node, and the performance cores for them,
  are the fix on that side. Unpinned, the client taking whatever was
  free, did best, 5.3 to 7.1 million with eight user threads.

  **Checks by event, lists by work, lookups by index, 2026-09-23.** A
  Linux profile showed every poll walking all transactions in flight
  for lost links (7.5%), the send walking them all to find those with
  work, and the map from a coordinator's pointer to its transaction, a
  B-tree, costing 9 to 10% in searches, inserts and splits. Now a count
  of link and node changes, raised after each is published, lets a
  poll skip the failure checks when nothing has changed since it last
  ran them; a transaction with something to send is put on a list the
  send takes; a node is found by id through a table; and our pointer
  for a coordinator's record encodes the record's place, its top bit
  marking it and bit 0 left for the acknowledgement flag, so that
  `active` is a vector indexed by it and a reply finds its transaction
  in one load. Measured against data nodes that turned out to be a
  debug build, which halved every build's rate and doubled its system
  time alike; the client's user time does not depend on them: per read
  at depth two, 591 ns before the other session's failure-handling
  commits, 696 after them, 419 with these changes. Against release data
  nodes, one user thread: depth one 1 168 000 and 1 352 000 reads a
  second at 574 and 501 ns per read, depth two 1 788 000 at 548, depth
  four 2 613 000 and 2 548 000 at 544 and 550. Client CPU no longer
  grows with what is in flight, where it had gone from 682 to 776 ns
  between depths one and four, and a single thread now does 2.6
  million reads a second.

  **Unpacking a row by its present columns, 2026-09-24.** The same
  profile put `unpack_row` at 6.5%: it stepped through every bit of the
  packed row's mask, and found each present column by two searches,
  one over the record's fields and one over the table's columns. It now
  jumps over runs of absent columns with `trailing_zeros`, an absent
  column taking one bit, and the record and the table description each
  keep a table from column id to place, built when they are made, which
  also serves packing, the key codec and the text codec. `t9`'s two
  columns gain little from it; wide tables gain most.

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
  512-byte rows, a `ROW_DATA_REP` of about 138 words, copying and not
  copying gave the same at one thread and at four; at 1 KB rows, about
  266 words, not copying was ahead, 821 against 833 ns at one thread
  (every run), and at four threads 0.5% less CPU, 2% more rate and
  fewer page faults, the user threads' pages not growing to hold the
  rows. The best limit lies between those, and 256 is at its top; the
  spread of a page's release over threads that copying small signals
  avoids did not show at four threads at either size.
- Exit: integration groups `pk`, `uk`, `types`, `failure` pass; a 1-thread
  asynchronous PK read benchmark is within 2× of the reference API (the
  20 % target is Phase 7). Met as of 2026-09-23: the benchmark at
  parity on 2026-09-22, and the integration groups `connect`, `dict`,
  `pk`, `uk`, `types` and `failure` passing against the two-node
  cluster (chapter 08). Writing them found one protocol fault the
  tools had never reached: `COMMIT_REQ` and `ROLLBACK_REQ` carried
  our pointer for the transaction where the coordinator wants its own,
  so a commit or rollback with nothing left to send got no reply; every
  commit until then had ridden on a query's commit flag. The `failure`
  group is the takeover path seen live: a pending transaction at a
  restarted coordinator ended rolled back with 4010 and its retry at
  the survivor committed.

## Phase 6 — Our own names, and key operations completed (5–6 weeks)

Replanned 2026-10-08. What a key-operation user needs before the C API
fixes the surface, with the rename first: the code is 25 000 lines
today, and nothing has been published under the old names. Step 6.7,
the dissector, comes second, right after the rename: every later step
learns its protocol from the wire through it.

### 6.1 Our own names (1 week)

The rule (author, 2026-10-08; chapter 09): nothing is taken from RonDB,
names included. The wire stays identical: signal numbers, block
numbers, word layouts and error codes are the interface and do not
change. Every name we give a signal, a block, a signal's fields, a
dictionary property key, a pseudo-column or a module is our own and
says what the thing does. RonDB names appear nowhere in code or docs:
chapter 05 cites RonDB by bare file:line and refers to signals by
number, and code comments cite chapter 05's sections, not RonDB.

The style (author): new stems with the suffixes `_REQ` (request),
`_CONF` (confirm), `_REF` (refuse) and `_REP` (report); a signal that
would have no suffix gets `_ORD` for an order, to which nothing comes
back, or `_REP` for a report. Constants are `IC_SIG_<NAME>`, Rust types
CamelCase (`KeyOpReq`), blocks `IC_BLOCK_<NAME>`.

The names, by number, drafted and applied on 2026-10-08 (Q11: the
author may still change any of them, which is one more rename):

| Group | Number → our name |
|---|---|
| Membership | 3 `HEARTBEAT_REQ`, 1 `HEARTBEAT_CONF`, 2 `HEARTBEAT_REF`, 26 `NODE_LOST_REP`, 27 `NODE_LOST_DONE_REP`, 163 `NODE_JOINED_REP` |
| Coordinator records | 39, 37, 38 `COORD_ALLOC_REQ`/`_CONF`/`_REF`; 36, 34, 35 `COORD_FREE_REQ`/`_CONF`/`_REF` |
| Key operations | 12, 10, 11 `KEY_OP_REQ`/`_CONF`/`_REF`; 519, 520, 521 `UNIQUE_OP_REQ`/`_CONF`/`_REF`; 5 `ROW_DATA_REP`; 6 `KEY_DATA_ORD`; 4 `COLUMN_DATA_ORD` |
| Ending a transaction | 19, 17, 18 `COMMIT_REQ`/`_CONF`/`_REF`; 15, 13, 14 `ROLLBACK_REQ`/`_CONF`/`_REF`; 16 `TRANS_ABORTED_REP`; 469 `COMMIT_MARKER_FREE_ORD`; 8, 9 `TAKEOVER_CONF`/`_REF`; 980 `DEADLOCK_FOUND_REP` |
| Scans | 32 `SCAN_REQ`, 29 `SCAN_CONF`, 31 `SCAN_REF`, 28 `SCAN_MORE_REQ`, 33 `SCAN_ROW_KEY_REP` |
| Dictionary | 24, 190, 23 `TABLE_DESC_REQ`/`_CONF`/`_REF`; 193, 194 `LIST_OBJECTS_REQ`/`_CONF`; 606 `TABLE_CHANGED_REP`; 984, 985, 986 `COLLATION_DESC_REQ`/`_CONF`/`_REF` |
| Blocks (hex) | F5 `COORDINATOR`, F7 `LOCAL_OPS`, F9 `ROW_STORE`, F8 `HASH_INDEX`, 102 `ORDERED_INDEX`, F6 `DISTRIBUTION`, FA `DICTIONARY`, FB `START_CONTROL`, FC `MEMBERSHIP`, FD `FILE_IO`, FE `NODE_ADMIN`, FF `INDEX_BUILD`, 100 `UTILITY`, 101 `CHANGE_FEED`, 109 `JOIN`, 10A `LINK_ADMIN`, F4 `ARCHIVE`, 10B–111 `READ_WORKER_FIRST`…`_LAST`, 7FF `BUNDLE`, FA2 `NODE_WATCH`, FA3 `CONFIG_SERVICE`; 8000 is `IC_FIRST_USER_THREAD_BLOCK` |
| Configuration (parameter id) | 129 `timeout_check_period`, 130 `idle_transaction_limit`, 131 `lock_wait_limit`, 800 `scan_batch_bytes_total`, 801 `scan_batch_bytes`, 802 `scan_batch_rows`, 119 `api_heartbeat_ms`, 101 `replica_count`; every parameter `ic_apic` decodes has one, constants `IC_PARAM_*` |

Beyond signals and blocks the same holds, as built:
- **Crate and modules:** `ic_signals`, with `numbers`, `key_ops`,
  `coord_alloc`, `membership`, `table_props`, `table_desc`,
  `list_objects`, `table_change`, `prop_stream`, `column_header`,
  `bundle`, `blocks` and `header`.
- **Types:** `KeyOpReq`, `KeyOpConf`, `KeyOpRef`, `KeyOpFlags`,
  `KeyOpResult`, `TransCommitConf`, `TransEndRef`, `RollbackConf`,
  `TakeoverConf`, `TakeoverRef`, `TransAbortedRep`, `RowDataRep`,
  `CoordAllocReq`/`Conf`/`Ref`, `CoordFreeReq`/`Conf`/`Ref`,
  `HeartbeatReq`/`Conf`/`Ref`, `NodeLostRep`, `NodeLostDoneRep`,
  `TableDescReq`/`Conf`/`Ref`, `ListObjectsReq`/`Conf`,
  `TableChangedRep`, `BundlePart`, `TableProps`, `ColumnProps`,
  `HashMapProps`, `FragmentState`; in `ic_apid` `CoordRecord`; in
  `ic_port` the error table's `ErrorEntry` and `ErrorClass`.
- **Fields** by meaning: `coord_ptr` (the coordinator's record),
  `client_ptr` (our pointer for it), `query_ref`, `flags_word`,
  `info_word`, `trans_id_lo` and `trans_id_hi`, `caller_data` and
  `caller_ref`; a table's and a column's properties likewise
  (`column_count`, `key_column_count`, `type_code`, `array_kind`, ...).
- **Constants:** dictionary keys `IC_TP_*`, column types `IC_TYPE_*`
  (the names the C header already used), kinds of array, object and
  fragment `IC_ARRAY_KIND_*`, `IC_OBJECT_KIND_*`, `IC_FRAGMENT_KIND_*`,
  configuration parameters `IC_PARAM_*` with snake_case display names,
  which `ic_config_dump` prints. `ic_desc` prints labels of our own.

What stays is what travels on the wire (author, 2026-10-08): numbers,
word layouts, error codes, configuration parameter ids and section
types, the management protocol's text, the handshake's strings, and
the stored names we look up (the counter table `sys/def/SYSTAB_0`, the
hidden key column `$PK`). SQL names an application writes (collations,
`AUTO_INCREMENT`, partition-balance options) are MySQL's language and
are used as they are. "RonDB" and "NDB" name what we interoperate with.
`legacy-c/` is not touched: it is iClaustron's old code, reference
only, never built.

Citations: the code's comments cite chapter 05, and chapter 05 §17
holds, module by module, the facts the code relies on with their bare
RonDB file:line. Chapter 05 is the only file that cites RonDB.

`cargo xtask names` (`xtask/src/names.rs`) reads, when it runs, the
names of signals, blocks, signal layouts, dictionary keys,
configuration constants and parameters, the reference API's classes
and the tree's programs from a RonDB tree given by `IC_RONDB_TREE`, and
finds none of them in `crates/`, `tools/`, `xtask/`, `include/`, `doc/`
or `README.md`; words that are ordinary English or Rust, or travel on
the wire, are listed with reasons in `xtask/names-allow.txt`. The list
of RonDB names is never stored here.

- Exit: `cargo xtask names` is clean; every unit test and integration
  group passes unchanged; a traffic capture taken before the rename
  replays byte for byte after it.

The wire check, as made (2026-10-09): the same thirteen tool runs
(`ic_desc`; inserts, an update, a write, reads, deletes and a read of a
deleted row with `ic_write` and `ic_read`; a read through a unique
index; a ten-row `ic_trans`) were captured from a build of `a9b9d19`,
before the rename, and of `3464a98`, after it, and compared with the
dissector's help, run by run and link by link. What the API sent
matched in every run: the management session's lines, the handshake,
and 111 signals (table descriptions, object lists, coordinator records
taken and freed, key operations, a unique-index operation, commit
marker frees) with their headers, data and sections, and every kind of
heartbeat. Masked, as differing from run to run whatever the code: the
API node id, signal ids, checksums, transaction ids, the coordinator's
records and the instance of the receiving block. Not covered, as these
tools do not send them: `COMMIT_REQ` and `ROLLBACK_REQ` on their own,
the short form's `KEY_DATA_ORD` and `COLUMN_DATA_ORD`, and a fragmented
send. The scripts are kept outside the tree (`target/rename/`).

### 6.2 Tables with BLOB columns (2 days)

- BLOB, TEXT, JSON and GEOMETRY are all blob columns in the dictionary:
  the row holds a head and the first bytes, and the rest lives in rows
  of a parts table that the client keeps, not the data node. A delete
  by our client would remove the row and leave its parts behind for
  good.
- Until BLOBs are built (phase 10), a delete by primary or unique key
  on a table with any blob column is refused before it is sent
  (`IC_ERROR_NOT_SUPPORTED`, with a text of its own saying why). Reads,
  inserts, updates and writes that leave the blob columns out stay
  allowed: a new row's blob is NULL, and an update leaves an existing
  one as it was.
- Exit: integration group `blob_guard` on a table with a TEXT column:
  the delete is refused; insert, update and read work, and MySQL reads
  the rows; a write over a row whose TEXT holds parts leaves the TEXT
  readable through MySQL.

As built (2026-10-09; `blob_guard` and `uk` pass): `ColumnProps::is_blob` (BLOB
and TEXT, types 20 and 21) is the one test, which records use too;
`TableDef::has_blob_columns` is worked out once when the table is
bound. A delete is refused with `IC_ERROR_DELETE_WITH_BLOBS` (7153) in
`key_op::write_key` and when a delete is defined on a transaction, by
primary key or through a unique index, before anything is packed. The
group `blob_guard` checks that BLOB, TEXT, JSON and GEOMETRY columns
all read as blobs and stay out of the default record; that the three
ways of deleting are refused and the rows, a 5000-byte TEXT among
them, are still there; and that an insert, an update, a read, and a
write and an update over a row whose 9000-byte TEXT is in parts leave
the TEXT as MySQL wrote it.

### 6.3 A reply that never comes: 4012 (3 days)

- Today a transaction whose reply never comes while its link stays up
  waits forever and keeps its coordinator record. The reference gives
  up after a time and reports 4012, outcome unknown; so will we.
- The time is the larger of 120 s and, over the data nodes in the
  configuration, the largest `timeout_check_period` (parameter 129) +
  `lock_wait_limit` (131) (chapter 05 §6.9). `ic_apic`
  decodes both parameters; `GlobalOptions` may override (tests use a
  second).
- A transaction notes when it last sent with replies outstanding.
  `poll` reads the clock once and walks the transactions with replies
  outstanding at most once a second.
- On expiry the transaction fails with 4012, class unknown result; each
  of its sent queries not yet complete completes with 4012, through its
  callback or the executed list; its coordinator record is lost, never
  reused, and freed with `COORD_FREE_REQ`, which also makes the
  coordinator clean up; a reply that comes late is dropped by the
  transaction id check, as today. Synchronous waits already time out.
- The same timer catches a request train the coordinator dropped for
  lack of room, to which it sends no reply (6.6), and later a scan
  (phase 8).
- Exit: a unit test with an injected clock; integration group
  `timeout`: a data node stopped with SIGSTOP, its link up, a timeout
  of one second, the transaction ends with 4012 before the heartbeats
  fail the node; after SIGCONT the next transaction works.

As built (2026-10-09; `timeout` and the whole suite pass): `ic_apic` decodes parameters
129 and 131 into `DataNodeConfig`, and `ClusterConfig::reply_wait_ms`
gives the wait; `GlobalOptions::reply_wait_ms` overrides it when not
zero, and `ApidGlobal::reply_wait_ms` says which applies. A send reads
the clock once and notes it on every transaction it sends for. A poll
reads the clock once, after the replies it took, and at most once a
second walks the transactions in flight
(`ApidConnection::expire_transactions`). One that waits for a reply (a
query sent and not complete, or a commit or rollback asked) longer than
the wait since it last sent ends rolled back with 4012, its class
unknown result, as a lost link's transaction ends rolled back with the
link's error; its queries fail with 4012; its record is lost and freed
with `COORD_FREE_REQ`, whose answer is taken quietly. A record is freed
once at most, and only one the coordinator confirmed: the first run
freed the timed-out record again when the connection let go of its
records, and the debug data node holding it stopped with error 2343
(chapter 05 §17.10), so the group now checks that the node outlives
the connection. RonDB answered the same day with release ids:
every allocate now carries a random id per record, never zero, and the
record's free carries it again (chapter 05 §6.1), so a stale free can no
longer free a record the coordinator has handed on. One already ended
that waits only for its reads' rows fails those queries alone. The
one-shot calls of `key_op` keep their own ten-second wait. Unit tests:
the wait from the configuration, the once-a-second pacing, and which
transactions count as waiting, with the clock given. The group
`timeout` finds the node's process by the port it listens on (`lsof`)
and needs `IC_TEST_NDB_MGM` set, as the `failure` group does.

### 6.4 latin1: moved to phase 8

Moved by the author, 2026-10-08: character sets wait until later in the
process. They are now step 8.6, and come from the data nodes rather than
from a module written here. Until then a key with a character column
works in any character set and is placed round robin, which costs a hop,
not a wrong answer.

### 6.5 Unique keys done properly (1.5 weeks)

From reading the reference (chapter 05 §6.8), most important first:
1. **NULL in a unique key (a bug).** A row with NULL in an indexed
   column is not in the index, so a lookup with NULL finds nothing.
   Today we send whatever bytes lie under the NULL and can find the row
   whose value happens to equal them. Refused before sending (the
   reference's code is 4316).
2. **Placement.** A transaction whose first operation goes through a
   unique index is placed by the index's own table: its columns hashed
   in the index's order, as the coordinator does, through the index's
   hash map and replica data (the index inherits read backup and full
   replication from its table; a character column falls back to round
   robin until 8.6). `IndexDef` gets them at bind, the hint
   takes an index, and `key_op` stops starting such transactions
   anywhere.
3. A committed read through an index sets the request's "read
   committed base" bit (bit 20), which lets the index read and the row
   read go to a backup replica of a read-backup table; the lock is
   still held to commit, as now.
4. A simple read through an index holds its shared lock to commit, the
   coordinator not allowing the simple flag on one: documented on
   `ReadKind::Simple`.
5. The refusal's error data is kept: for 893, a duplicate key, it names
   the index. Today it is dropped.
6. The index's state is checked at bind: an index that is not online is
   refused. The reference does not check, and a lookup through an
   offline index silently finds nothing.
7. A 241 or 284 on an index operation invalidates the index in the
   cache, as one on a table does.
8. Our rules, unless the author objects (Q12): an update through an
   index whose attribute record holds an indexed column is refused (the
   reference takes indexed columns from the key row, so it cannot change
   them either); a write through an index stays refused (the reference
   lets it act as an update; not seen live).
9. Chapter 05 §6.8 corrected: the coordinator never sends
   `UNIQUE_OP_CONF`; the confirmation comes as `KEY_OP_CONF`.
- Exit: the `uk` group grown to cover a nullable unique key and a NULL
  lookup; a key of several columns in an order other than the table's;
  CHAR and VARCHAR keys, placed round robin until 8.6; 893 on an insert by primary key and on
  an update through the index; locked, exclusive and no-wait reads
  through an index against a conflicting lock; primary and unique key
  operations in one transaction, and 500 unique key operations in one
  batch; 626 with abort on error; an index dropped and made again while
  bound; the refusals; placement, seen as the node the index read ran on.

The deferred unique check (bit 17) waits for an application that asks
for it.

As built (2026-10-09; the `uk` group and the whole suite pass), item by item:
1. `row_codec::index_key_info_into` refuses a NULL indexed column with
   4316 (`IC_NDB_ERROR_NULL_KEY`) before anything is packed.
2. `IndexDef` keeps the index's own table (`as_table`), with its
   replica data and its hash map, fetched at bind as a table's is;
   `hash::index_partition_of` hashes the key a unique request carries
   over that table, and `ApidConnection::hint_for_unique_key` gives the
   partition, built in the connection's key buffer. A character column
   gives no partition, so such a transaction goes to the next node in
   turn. `key_op::read_unique_committed` starts where the hint says.
3. A committed read through an index sets bit 20 (`committed_base`).
4. `ReadKind::Simple` and `ReadKind::Committed` say what they do
   through an index.
5. `ApidQuery::error_data` keeps what a refusal says beyond its code,
   and `Transaction::error_data` what an abort report does: seen live,
   an 893 on an operation that aborts on error comes in the report.
6. An index whose description gives a state other than online (4) is
   refused at bind with `IC_ERROR_INDEX_NOT_ONLINE` (7154).
7. A 241 or 284 lets go of the query's index, or its table when it has
   none (`ApidConnection::index_changed`, `table_changed`).
8. An update through an index whose values hold an indexed column is
   refused with `IC_ERROR_INDEXED_COLUMN_UPDATE` (7155); an insert or
   write through one stays refused.
9. Chapter 05 §6.8 corrected.

The group `uk` gains a table `uk2` and eight tests for the exit above.
The dissector names bit 20.

### 6.6 Every row size can be written (1.5 weeks)

Today nothing above one 32 KB message can be sent (`header.rs`), while
a row may be 72 000 bytes. From reading (chapter 05 §16.1):
- A request is split when its sections together exceed 8 040 words,
  and each fragment then carries up to 8 040 words, the most a 32 KB
  message holds with room for its header. A fragment carries whole
  sections and may begin the next.
- The cut granularity is not the fragment size. A 26.10 data node keeps
  a section in segments of 60 words and checks that every piece of a
  section but the last ends on a segment boundary; a release data node
  that gets a ragged piece keeps a corrupt section without a word. So a
  section that begins part-way into a fragment is cut after a multiple
  of 60 words (at least 60). The encoder takes the segment size from
  one constant: any multiple of 60 satisfies 26.10, and a RonDB with
  larger segments is a change of that constant.
- Fragments before the last carry only their section numbers and the
  fragment id; the last carries the signal's data, its section numbers
  and the fragment id. A data node takes data words plus sections up to
  25 in one signal.
- Fragment ids count per sender block, from 1, skipping 0; a train
  stays together on its link, which staging already gives.
- A row is at most 18 000 words, of which the fixed in-memory part
  4 070, the variable part 8 008 and the disk part 7 770: about 48 KB
  without disk columns. From 2026-10 (the author, 2026-10-12): the
  variable parts of the in-memory and of the disk columns 8 158 words
  each, the fixed part and the row as before. A key is at most 1 023 words, column data under
  32 768 words.
- A coordinator without room to put a train together drops it and
  answers nothing, which 6.3's timer catches.
- Disk columns are in scope (author, 2026-10-08): the development
  cluster gets a log file group and a tablespace (`doc/dev-cluster.md`),
  and the `types` group a table with disk columns, read and written by
  primary and by unique key.

Changes:
- `header.rs`: an encoder that splits as above, returning the train's
  length and where its last fragment lies, `out` cut back on error;
  the 32 KB check per message and the 25-word check.
- `apid_conn.rs`: `Staged` knows its last fragment, the send writes the
  flags into the last fragment and recomputes only its checksum when
  checksums are on, `stage_signal` and the queued path use the encoder,
  and the connection counts fragment ids.
- `build_sections` refuses a key above 1 023 words and column data of
  32 768 words or more before sending, so that no data node ever sees a
  request it takes for a malformed one.
- `fragments.rs`: the signal's data taken from the last fragment, where
  we put it (the data nodes put it in every fragment), so that our own
  sender's trains reassemble in a loopback test.
- Exit: unit tests at each boundary: one signal at 8 040 words of
  sections; two fragments; two against three at 16 020 and 16 021 words
  with a one-word key, and at 15 000 and 15 001 with a 1 023-word key;
  a head piece that is not a multiple of 60; with checksums and signal
  ids on. Integration group `big_rows`: the largest in-memory row, and
  on a table with disk columns a row near 72 000 bytes, written and
  read back by primary and by unique key and checked through MySQL; a
  fragmented request between plain ones in one batch, two in one
  batch, one rolled back before it was sent.

As built (2026-10-13; the `big_rows` group passes), item by item:
1. `header::encode_train` splits as above and returns a `Train`: its
   length, its number of messages, where the last message and the
   signal's data lie; `out` is cut back on error. `encode` refuses data
   and sections over 25 together. Segment and fragment sizes are
   `IC_SECTION_SEGMENT_WORDS` and `IC_FRAGMENT_SECTION_WORDS`.
2. `Staged` has `last_at` and `data_at`; `patch_staged` writes the
   flags there and computes the last message's checksum again.
   `stage_signal` and `queue_signal_for_generation` use the encoder, and
   the connection counts fragment ids from 1, skipping 0, advanced only
   by a train. The two direct sends (`send_for_generation`,
   `NodeConnection::send_signal`) carry control and dictionary
   signals, never a key request, and stay whole.
3. `build_sections` refuses a key above 1 023 words with
   `IC_ERROR_KEY_TOO_LONG` (7156, new) and column data of 32 768 words
   or more with `IC_ERROR_RECORD_SIZE_TOO_BIG` (7077).
4. `fragments` takes the joined data from the last fragment; a unit
   test joins a train of our own encoder's.
5. Unit tests at every boundary of the exit, with checksums and signal
   ids, and the patch of a staged train.
6. The disk columns' exit is met by the `big_rows` group rather than
   `types`. Its tables fill each part of a row exactly, at the limits
   of 2026-10 (chapter 05 §13): `big_mem` the fixed and the in-memory
   variable part, about 48 KB; `big_disk` both variable parts, 65 486
   bytes, next to the 65 535 a MySQL server allows. A row past that,
   up to 72 000 bytes, needs the inline parts of blob columns, which
   iClaustron neither reads nor writes before phase 10; the encoder's
   unit tests cover the largest row. Tests: each table by primary and
   unique key, shrunk and grown back by an update; a byte more in any
   part refused by the data nodes; rows MySQL wrote read whole; trains
   and whole requests in one batch; a train rolled back unsent. The
   group makes the log file group `ic_it_lg` and tablespace `ic_it_ts`
   when the cluster has none, and keeps them. First run, on the
   earlier limits: a full in-memory variable part stopped both data
   nodes in TUP's allocator, a RonDB bug mended with the new limits;
   the trains had arrived and been joined.
7. Chapter 05 §16.1 folded into §2.2 and §13; §2's 7 400-word "limit"
   corrected.

### 6.7 The wire made readable: a Wireshark dissector (2 weeks, second in order)

Decided by the author, 2026-10-08: protocols are developed mainly by
listening to the network, not by reading the reference's source. A
dissector of our own turns what a capture holds into text a person
reads, in our names.

- **What it decodes:**
  - the management protocol (port 1186 by default): requests and
    replies line by line, and the configuration blob's header;
  - the signal protocol, recognised by its handshake (the `ndbd` lines
    and the node-id hello) on whatever port a data node was given: the
    three header words, the byte-order and fragment bits, the signal id
    and checksum when the link has them, the data words and the
    sections; bundled signals (block 0x7FF) taken apart;
  - each signal by number with our name, and the fields we know by
    meaning (the coordinator's record, our query's id, the flags word
    bit by bit, the transaction id, the row data); a signal we have no
    name for is shown as its number and words.
- **Where:** not in this repository (author, 2026-10-08). The
  dissector is written in C, as Wireshark's own dissectors are, in a
  Wireshark source tree of its own beside this one, under Wireshark's
  licence, and Wireshark is built from that tree. Its tables of signal
  and block names use our names, kept in step with `ic_signals` by hand,
  since chapter 05 quotes what it prints. Its protocol filter name is
  `ndb`.
- **How it is used:** a capture on the loopback device of the
  development cluster (`tcpdump -i lo0 -w run.pcapng` on the Mac, `-i
  lo` on Linux) while the reference API, a MySQL server or our own tools
  do the operation; then `tshark -r run.pcapng -O ndb` for text
  that can be compared and quoted. A fact learned this way goes into
  chapter 05 with the name of the capture it came from; small captures
  are kept under `doc/rust/captures/`. What the wire does not show, the
  author is asked; the reference's source is read only to settle what
  neither shows.
- **With chapter 08's captured-traffic tests:** `tools/ic_capture` reads
  such capture files and writes the replay vectors from them, instead
  of being a proxy of its own.
- Exit: a capture of `ic_bench` and one of the reference's benchmark
  against the development cluster, decoded with every signal named and
  every known field shown; the rename's byte-for-byte check (6.1) made
  with it, our tools from before and after the rename sending the same
  bytes.

As built, first version (2026-10-08, not yet compiled): the tree is
`/Users/mikael/github/wireshark`, branch `ndb-dissector`, and the
dissector `epan/dissectors/packet-ndb.c`. It decodes text lines on both
kinds of connection, a signal link found by its first line, and from
each side's hello the messages, reassembled across TCP segments: the
three header words bit by bit, signal id, data, sections and checksum
(checked); bundles taken apart; fragments shown with their fragment id
but not yet put together. Fields are named for the key operation,
transaction, takeover, heartbeat (with the data node's state),
coordinator record, table description, object list and collation
signals, and column data is taken apart header by header (id, size,
value; reading every column, columns by bitmap, the packed row that
comes back); the others show their words. Still to come: putting
fragments together, the scan signals' fields, the table description's
properties, and the management protocol's configuration blob.

First capture (2026-10-09, the development cluster, 51,682 frames): the
messages start on each side right after its hello, and the first byte
of a message is text whenever the link carries signal ids and checksums
(0x34 is `4`). So the dissector ends a side's text at its hello: a line
of numbers after the client asked for a link (`ndbd …` toward a data
node, `transporter connect` toward the management server). A capture
that misses the hello falls back to the first line holding bytes that
are not text. Framed this way, all 46 directions of the capture's 23
links, 26,587 messages, frame without a gap and every checksum holds.
Data node to data node links carry the link instance in the hello's
fourth number (`2 1 1 0` to `2 1 1 4` for the five links from node 2
to node 1); both data nodes link to the management server (node 3)
through port 1186 with `transporter connect`.

Fragments put together (2026-10-14). On the first pass each fragment's
pieces are appended to the section their number names, in a train kept
per link direction by sender block and fragment id; at the last
fragment the joined signal is made as a whole message, the last
fragment's data and the joined sections, shown as a data source of its
own and decoded as any whole signal, with the frames its fragments came
in (`ndb.train.fragments`, `ndb.train.frame`). Earlier fragments link
to the frame that joins them (`ndb.joined_in`); one whose train began
before the capture is flagged (`ndb.fragment.unjoined`). Seen live on a
capture of the `big_rows` group: all 30 trains joined, none left over:
9 `KEY_OP_REQ` and 2 `UNIQUE_OP_REQ` of ours, of two and three
fragments, the largest 16 394 words with every column decoded; and 19
`ROW_DATA_REP` from a debug data node, in pieces of 120 words, up to
136 fragments, each row what the request before it wrote. The long
`ROW_DATA_REP`'s fourth data word, the row's length, is now shown as
such (`ndb.row_len`) rather than read as a column.

Exit for the phase: the groups `connect`, `dict`, `pk`, `uk`, `types`,
`failure`, `blob_guard`, `timeout` and `big_rows` pass, `cargo xtask
names` is clean, and the dissector decodes every signal phase 6 sends.

## Phase 7 — C API, documentation, examples, release 0.1 (3–4 weeks)

- **The header from what was built.** Chapter 04 first. The Phase 0
  header (`include/iclaustron/ic_apid.h`) was written before the code
  and differs from it: queries and transactions live in the connection
  and are named by id, a query owns its rows, hints are values, and
  neither the interpreter nor scans are in 0.1. Rows in C (Q10,
  recommended): the query owns its rows and C gets pointers to them,
  `ic_apid_query_key_row(query, &len)` and
  `ic_apid_query_attr_row(query, &len)`, valid until the query is
  freed; no copy, and no pointer into the caller's memory. That replaces
  chapter 04's "row buffers are the caller's memory".
- **`ic_capi`**: one exported function per public method; opaque
  handles; `catch_unwind` on every entry (`IC_ERROR_INTERNAL_PANIC`, the
  connection then unusable); errors as an `int` and the error object
  (code, category, our text); a C callback, a function pointer and a
  `void*`, through one trampoline beside the Rust callback; the header
  generated by cbindgen and checked in, `cargo xtask header` diffing it;
  shared and static libraries, a `pkg-config` file, `cargo xtask
  install`.
- **Documentation**: every C function documented in the header: what it
  does, who owns what, how long a returned pointer lives, which thread
  may call it, its errors. `cargo doc` for the Rust crates with a guide
  at the crate level. A user guide, `doc/user-guide.md`: connecting,
  records, queries and transactions, the asynchronous and synchronous
  styles, callbacks, errors and when to retry (temporary errors, 4012's
  unknown outcome), node failure, and the limits of 0.1 (keys with
  character columns placed round robin,
  deletes refused on tables with blob columns, no TLS, the user-thread
  limit). Per-crate `MODULE.md`.
- **Examples in Rust and in C**, the same programs in both: describe a
  table; read, insert, update and delete by primary key, synchronously;
  an asynchronous pipeline with callbacks; read and update by unique
  key; a transaction retried after a temporary error. Rust in
  `examples/rust` as cargo examples, C in `examples/c` with a Makefile
  and the one-line `cc` command; each runs against the development
  cluster (`doc/dev-cluster.md`).
- Kept from the earlier phase 7: the performance pass (allocation,
  adaptive send, receive thread count); measuring the key hash against
  the C; raising the user threads per process from 256 to 4 711, which
  the synchronous style with a connection per thread needs, and lifting
  the limit of four management servers with it. Before the limit passes
  4 096, the transaction id must carry bit 12 of the block index in bit
  38 and bit 12 of the node id in bit 39, as the reference does
  (chapter 05 §17.12); ours keeps 12 bits of each, so two threads would
  make the same ids (found 2026-10-08).
- Moved to phase 10: the C++ header and the smoke bindings in other
  languages (Q6), which were in this phase before.
- Exit: the 0.1 success criteria of chapter 01, as revised 2026-10-08.

As built, the header and `ic_capi` (2026-10-14; `cargo xtask check`
and `header` pass, the header compiles as C and as C++, the `c_api`
group passes, and `cargo xtask install` installs a library pkg-config
finds):
1. Chapter 04 rewritten from what was built, Rust and C. Q3, Q4, Q7,
   Q9 and Q10 applied as recommended (chapter 10).
2. `ic_apid` gained what the C ABI needs: `ApidGlobal::connect`
   (connectstring to running global, `GlobalOptions::mgm_timeout_ms`),
   `QueryId::from_u32` and `TransId::{as_u32, from_u32}`, a `binding`
   word in the read and write arguments for a binding's trampoline,
   and `ApidConnection::set_unusable`. `bind-address=` is now applied:
   the management and data node sockets bind to it before they connect
   (`MgmClient::connect_from`, `NodeConnection::connect_to_port_from`).
   New errors 7157 `IC_ERROR_NULL_POINTER` and 7158 `IC_ERROR_BAD_TEXT`.
3. `ic_capi`: about seventy `ic_*` functions over opaque handles and
   `u32` ids, `catch_unwind` on every entry, a connection marked
   unusable after a panic, C callbacks through one trampoline, rows
   owned by the query and given to C as pointers.
4. The header generated by cbindgen and checked in, replacing the Phase
   0 one; `cargo xtask header` diffs it.
5. The library named `libiclaustron`, with a soname carrying the major
   version, `iclaustron.pc`, and `cargo xtask install [prefix]`.
6. The `c_api` integration group: insert, a read whose callback reads
   the row from inside, a unique-key read, update, delete and a
   refused second insert, all through the C functions; stale ids and
   bad text refused. `cargo xtask test-integration` runs it with the
   `ic_apid` groups.

As built, the examples (2026-10-14; both languages run as meant and
print the same lines, the C ones compiled with `-Wall -Wextra` without
a warning; the pipeline at 21 000 rows a second in Rust from a debug
build, 26 600 in C on the release library): five
programs in Rust and in C, `examples/README.md`, on a table
`examples/setup.sql` makes (`ic_ex.t1`: an INT UNSIGNED key, a nullable
INT, a latin1 VARCHAR and a unique INT). `describe` prints a table's
fields and a default record's layout; `key_ops` inserts, reads, updates
and deletes by primary key with the synchronous calls, a refused second
insert included; `pipeline` writes rows with a number of transactions
in flight, each query's callback closing its transaction and starting
the next write; `unique` reads and updates through the unique index;
`retry` holds a row with an exclusive read so that an update waits out
the lock wait limit and ends with 266, then lets it go and tries the
update again. Rust: the package `ic_examples` (`examples/rust`), its
library the shared helpers, the programs cargo examples; C: one file
each with `t1.h`, the row as a C struct, a Makefile building against
the installed library or the tree's own build, and `compile_flags.txt`
for an editor. `ic_field_type_name` added to the C API for `describe`;
`cargo xtask style` now checks `examples/` too.

As built, the user guide (2026-10-14): `doc/user-guide.md` covers
what a program needs, connecting, tables, indexes and records with how
each type is kept in a row, queries and transactions, the synchronous
and asynchronous styles, callbacks, errors with a table of those to
know and when to try again, node failure, performance and the limits
of 0.1, quoting the examples in both languages. `ic_apid`'s crate docs
gained a "Using it" section with an example `cargo test` compiles; the
README points to the guide. Found while writing it: 4012 and the other
unknown outcomes are temporary, as NDB classes them; chapter 04 and the
header said otherwise, and now say that a write is tried again only
once it is known not to have committed.

The header reviewed (2026-10-14): an overview at its top gives the
rules every function keeps (results and error codes, handles and who
frees them, ids, rows owned by queries and their alignment, strings,
structs with a size, threads, panics, what a "Safety" paragraph is);
every function that can fail names the errors particular to it, with
where it blocks, what it hands over and how long that lives; the
error codes a program checks are defines of their own
(`IC_ERROR_NO_SUCH_TABLE`, `IC_ERROR_TRANSACTION_ACTIVE`,
`IC_NDB_ERROR_LOCK_TIMEOUT` and so on), each checked against the
library's by a unit test.

As built, the user-thread limit (2026-10-14; `cargo xtask check` and
the `pk` group pass): a process may have 4 711 connections, as many as RonDB lets an
API node have (`IC_MAX_THREAD_CONNECTIONS`, checked at compile time
against the block numbers and the transaction id's room). A
transaction id now carries bit 12 of the block index in bit 38 and of
the node id in bit 39, as the reference does, so threads 4 096 apart,
and nodes 4 096 apart, make different ids; a unit test checks every
thread index up to the limit, and the `pk` group gains a test that
takes every user thread, is refused one more with 7139, and writes and
reads from threads 4 096 apart and from the last. The connectstring
takes any number of management servers, not four.

The performance pass, the method (2026-10-14). The September baseline
cannot be run against today's data nodes, which want a release id in
`COORD_ALLOC_REQ` that it does not send, so the yardstick is the
reference API on the same cluster, as the 0.1 criterion puts it: read
throughput within 20% of the reference's, asynchronous, a connection
per thread, 8 threads. `tools/perf_pass.sh` runs it all against one
release-built cluster into one log: `ic_hashbench` against
`xxh3_bench.c`, the data nodes' own xxHash, at keys of 4 to 1 024
bytes, whose checks must agree; `ic_bench` reads at 1 and 8 threads,
8 with `--force` and with two receive threads, and updates; `ic_bench`
built with `count-allocs`, allocations per operation; and
the reference's benchmark at eight threads, 400 one-read transactions
in flight each, like for like with the 8-thread run.

The first run (2026-10-14, the author's Mac, two release data nodes,
100 000 keys, 10 s each):
- **The key hash.** XXH3 agrees with the data nodes' xxHash at every
  size, 20 million keys each; up to 240 bytes it is level with it,
  2.4 to 10 ns either way (16 and 32 bytes half a nanosecond slower,
  4 and 8 faster); at 1 024 bytes 59 ns against 20, the C's vector
  path against our scalar one, as foreseen. Distribution keys are
  short; the vector path is not needed for 0.1. MD5, for older tables,
  130 ns per 64-byte block.
- **Throughput and CPU.** One thread: 2 011 000 reads a second at 497
  ns of client CPU each. Eight: 6 098 000, 631 ns. Eight with
  `--force`: 5 560 000, 686 ns, so the adaptive send pays its way. Eight
  with two receive threads: 5 958 000, 760 ns, so one is right with two
  data nodes here. Updates, one thread: 467 000 at 850 ns.
- **The reference.** Its benchmark, eight threads, 400 in flight each:
  2 565 000 reads a second, against our 6 098 000. Its rounds of 400 are
  sent and waited for whole where ours overlap two batches of 200, so
  the like-for-like number is ours at `--batch 400 --depth 1`, added to
  the script with the reference's CPU.
- **Allocations.** 5 per 1 000 reads and 6 per 1 000 updates, one per
  batch: the benchmark's own list of a batch's transactions, now kept
  per slot. The library's path allocates nothing per operation.

The second run (2026-10-14, the same machine and cluster), with the
like-for-like run and the reference's CPU:
- **Like for like.** Ours at eight threads in rounds of 400, sent and
  waited for as the reference's benchmark does (`--batch 400 --depth
  1`): 5 617 000 reads a second at 598 ns of client CPU each. The
  reference's: 2 639 000, with 60.8 s of CPU over its 15 s of reads,
  about 1 500 ns each. Twice
  the reads for about two fifths of the CPU per read; each side used
  some 3.8 cores. The 0.1 criterion, within 20% of the reference, is
  met with room to spare.
- **Adaptive send.** `--force` at eight threads was 5% faster this time
  and 9% slower the first: within what two runs on one machine resolve.
  The adaptive send stays the default, for the same CPU and 4 socket
  writes per 1 000 reads against 7.
- **Receive threads.** Two at eight user threads: 5 277 000 at 807 ns,
  worse in both runs, by 5 to 12% in rate and 20 to 28% in CPU. One
  receive thread is right with two data nodes.
- **Allocations.** 6 391 over a run of 20 million reads, 5 969 over 4.8
  million updates: setup alone, none per operation.
- **The key hash.** As in the first run.

Nothing the pass found called for a change to the library.

The 0.1 criteria (chapter 01), where each stands (2026-10-14):

| Criterion | Evidence | Stands |
|---|---|---|
| Connects, takes a node id, heartbeats, survives a data node restart | `connect` and `failure` groups | met |
| ... and a management server restart | `failure_a_management_server_gone_for_a_while_is_found_again`, new: a data node restarts and the management server is gone meanwhile, paused, or restarted by `IC_TEST_MGM_RESTART`; work goes on, and our node links to the data node again only by finding the management server again. (First tried with the management client restarting the server: under MTR it exits and is not started again.) | met |
| Describes any MySQL table as the reference's tool does | `tools/desc_compare.sh`: every column type, a unique and an ordered index, the reference's tool and `ic_desc` diffed with our labels mapped onto theirs. Every fact agreed but collation names, shown by number, and blob columns' sizes, version and parts table, not shown; `ic_desc` now asks the data nodes for collation names and prints the blob details (inline size, part size and stripe from precision, scale and length), and the two agree on every line | met |
| Primary and unique key operations, commit and rollback, from Rust and C, checked through MySQL | `pk`, `uk`, `c_api` groups | met |
| A node killed mid-transaction: a temporary error, retried elsewhere | `failure_a_stopped_coordinator_fails_the_transaction_and_a_retry_commits` | met |
| Every row size; 4012; blob deletes refused; unique keys refuse NULL and are placed by the index | `big_rows`, `timeout`, `blob_guard`, `uk` groups | met |
| `cargo xtask names` finds no RonDB name | the run of 2026-10-14, after naming the reference's programs in our own words | met |
| Read throughput within 20% of the reference, 8 threads | the performance pass: twice the reference's | met |
| Libraries, documented header, user guide, examples in Rust and C | `ic_capi`, the header review, `doc/user-guide.md`, `examples/` | met |

The full integration run (2026-10-15): every group passed. It ran on a
release cluster with 100 MB of data memory on each data node, and with
the restore of rows longer than a page mended (chapter 05 §13).

## Phase 8 — 0.2: the executor, scans and character sets (8–9 weeks)

### 8.1 The executor (2 weeks)

The architecture page (sections 3 and 4) proposes it. A scan can only
be shown to fit it once it exists, so it comes first:
- the inbox's second list, for requests from application threads, under
  the mutex, condition and waiting flag the signal pages use; a
  `Submitter`, a handle that can be cloned; `submit` bounded, answering
  busy or blocking;
- the loop: replies first, completions delivered (a slot and a wake-up,
  a callback on the executor, later a Rust waker), requests admitted up
  to the in-flight limit, one send per node;
- deadlines checked at admission; node failure completes requests with
  their error;
- the blocking façade (form B) for key operations, and its C ABI;
- measured with `ic_bench` through executors against threads with
  connections of their own, and against `ic_model`'s prediction
  (chapter 12).

**As built (2026-10-15).** Every item above; `cargo xtask check`, the
`executor_` integration group and the C API's executor test pass.

- `thread_conn.rs`: the inbox's request list, bounded (`queue`, 4 000
  by default), taken whole in the same lock as the pages; a request
  wakes the executor only if it sleeps willing to take one, so an
  executor at its in-flight limit sleeps for replies alone; a full list
  refuses with `IC_ERROR_EXECUTOR_BUSY` (7159) or waits for room on a
  second condition; `close_requests` for stopping
  (`IC_ERROR_EXECUTOR_STOPPED`, 7160).
- `executor.rs`: `Executor` (a user thread with a connection of its
  own), `Submitter`, `OpShape` (a table or unique index and the records
  of an operation's rows, made once), `KeyOp`, `Request` (a transaction
  of operations, committed on the last), `Done` (a `Slot`, or a
  `RequestCallback` on the executor's thread, a `fn` and a `usize` as
  every callback is), `ExecutorStats`. The loop takes replies and
  requests in one lock, hands back what finished, admits up to
  `in_flight` (400 by default), and sends once. Each operation runs as
  a query of its shape, from a pool the executor keeps per shape and
  lets go of once nobody holds the shape; rows are copied in and out,
  so nothing is allocated per request. Deadlines are checked at
  admission; a request that cannot be defined is rolled back before it
  is sent; a stopping executor fails what waits and finishes what is
  out; a request dropped undelivered, which only a panicking executor
  does, tells its slot (`IC_ERROR_INTERNAL_PANIC`). Tables and indexes
  are bound through the submitter, on the executor's connection.
- `Submitter::execute`, the blocking form, waits on the submitter's own
  slot: one submitter per thread.
- The C ABI: `IC_APID_EXECUTOR`, `IC_APID_SUBMITTER`, `IC_OP_SHAPE`,
  `IC_REQUEST`, and `ic_apid_submitter_execute` (chapter 04).
- `ic_bench --executors E --clients N`: N application threads, each one
  blocking call at a time through executor *i* mod E;
  `tools/perf_pass.sh` section 5. `ic_model executors=E clients=N`.

Measured (2026-10-15, the Mac, two release data nodes, `t9` by primary
key, 100 000 keys, 10 s; `target/perf-20261015-115301.log`):

| Run | Per second | Median call | 99th | Client CPU a call | Writes per 1000 | Requests a send |
|---|---|---|---|---|---|---|
| A: 64 threads, a connection each, one call at a time | 198 000 reads | 319 µs | 487 µs | 18.3 µs | 176 | 1 |
| B: 64 callers, one executor | 268 000 reads | 231 µs | 475 µs | 9.8 µs | 86 | 21.0 |
| B: 256 callers, two executors | 307 000 reads | 821 µs | 1 455 µs | 18.1 µs | 47 | 38.7 |
| B: 64 callers, one executor | 68 500 updates | 918 µs | 1 392 µs | 29.6 µs | 416 | 4.0 |
| Pipelined, one thread, batches of 200, for scale | 1.99 million reads | 158 µs a batch | 236 µs | 0.53 µs | 10 | 200 |

- **Form B against form A**, 64 threads either way: 36 % more reads, a
  median call 28 % shorter, half the client CPU. Twenty-one requests a
  send halve the writes per read.
- **Both are bound by latency.** Each caller has one call out, so the
  rate is the callers over a call's time: 64 over 231 µs is 277 000.
- **One executor is bound by its own thread.** It wakes every caller,
  and every caller goes to sleep and wakes: about 10 µs of CPU a call
  in all, against 0.5 µs pipelined, most of it the kernel's. At 268 000
  calls a second one thread has 3.7 µs for each, waking its caller
  among them; the model puts the limit there too (not measured on the
  executor's thread itself). The
  architecture page had predicted that batching would bring a call's
  cost near the pipelined figure; the wake-up is per call, not per
  batch, and it does not.
- **More executors scale as far as the cores do.** Two with 256 callers
  gave 15 % more, not twice: 256 runnable threads doubled each call's
  kernel time (8.3 to 16.2 µs) and preempted 3.45 million times in 10 s.
- **Updates** come back spread out, the commit taking each its own
  time, so a send carries four.

Against `ic_model`'s prediction, made before the measurement:

| Run | Model, as predicted | Model, caller's cost fitted | Measured |
|---|---|---|---|
| A, 64 threads | 238 000, 259 µs, 16.4 µs | — | 198 000, 319 µs, 18.3 µs |
| B, 64 callers, one executor | 193 000, 329 µs, 4.5 µs | 193 000, 328 µs, 9.2 µs | 268 000, 231 µs, 9.8 µs |
| B, 256 callers, two executors | 451 000, 562 µs, 4.3 µs | 391 000, 647 µs, 9.0 µs | 307 000, 821 µs, 18.1 µs |

The model put the limit where it is, on the executor's thread waking
its callers, and the batching where it is (63 writes per 1000 against
86). It counted half the CPU: what a caller spends going to sleep and
waking, which it had no part for. Fitted to the measurement as
`submit_ns`, 5 µs a call, now its default, the CPU matches. What it
still misses is the scheduler's cost of hundreds of runnable threads,
which a mean-field stretch does not see (chapter 12, "Executors").

What this leaves for later, kept in chapter 04: the wake-ups moved off
the executor's thread, to helpers or to a caller that spins briefly;
asynchronous C callbacks and Rust futures over executors, which a
service that wants the pipelined figures uses instead of blocking.

### 8.2 The protocol, seen and written down (1 week)

Chapter 05 §7 rewritten from captures of the reference's scans read with
the dissector (6.7) and from the author's answers; §16.4 holds what an
earlier reading of the source gathered, and what in §7 is wrong: `SCAN_REQ` (32), its flags and sections;
`SCAN_CONF` (29), per receiver; `SCAN_REF` (31), and when a close is
still owed; `SCAN_MORE_REQ` (28), per receiver and to close;
`SCAN_ROW_KEY_REP` (33) for takeover; batch sizes, timeouts, node
failure.

### 8.3 The scan core, on a connection (2 weeks)

- A scan runs on a coordinator record of its own. A locking scan
  started on a transaction, so that its rows may be taken over, takes
  it on the node and coordinator instance of the transaction's record
  and names that record; every other scan names none (the author,
  2026-10-16; chapter 04, chapter 05 §7.2). A scan naming none needs
  the author's mend of the coordinator's check on that word, in
  `rondb_1107_cte_test` (d9f777a6847); on a tree without it such a
  scan may stop a data node.
  Records for scans are pooled per node and instance and taken ahead,
  so that nothing waits for one, an executor least of all.
- Receivers, one per fragment scanned in parallel, get ids in the
  object map that the routing of `ROW_DATA_REP` and `SCAN_ROW_KEY_REP`
  tells apart from queries; the transaction id and the generation are
  checked as for queries.
- Batch sizes from the API's configuration (`scan_batch_rows` 802,
  `scan_batch_bytes` 801, `scan_batch_bytes_total` 800) or the scan's
  own; at most 992
  rows a batch when locking, for takeover, and 4 000 for committed
  reads.
- The consumer sets the pace: a receiver is asked for more only when
  room for its next batch is reserved, each receiver on its own. Rows
  are copied out of the receive pages into the scan's batch buffers, so
  a parked scan holds no page. A batch is complete when its `SCAN_CONF`
  has come and as many words as it says. Every scan asks for the
  extended confirmation, five words per receiver (chapter 05 §7.2).
- Close at any time, as the reference does: wait for the batches asked
  for, then stop, then wait for the bare end of data. A `SCAN_REF` that
  owes a close gets one; the record is kept until the close is
  confirmed; rows that come late are dropped by generation.
- Errors: 296 when a parked scan passes `idle_transaction_limit` (130);
  274 and 266 for locks; 270 when a reading node fails (temporary: the
  scan starts again and rows come again); 4028 when the coordinator's
  node fails; a deadline of the client's closes the scan.
- Full table scans in every lock mode; a scan pruned to one partition
  by its distribution key (with 8.6's character-set hash) or by partition id.
  On a table with a partition hash fanout, a scan by distribution key
  covers every partition. Pruning it to the fanout's partitions is a
  later optimisation (the author, 2026-10-15; chapter 05 §7.2).

### 8.4 Three ways to consume a scan (1 week)

- **Asynchronous**, on a connection: batches complete in `poll`,
  through a callback per batch or a ready list, as queries do; the
  application reads a batch's rows and hands the batch back, which lets
  that receiver go on.
- **Synchronous**: `next_row` and `next_batch` with a wait, built on
  `poll`; a thread may have scans and queries in flight together.
- **Through an executor**: a scan goes to the executor with a reader,
  the application thread's end. A complete batch's buffer moves to the
  reader, and its receiver is asked for more only once the reader has
  handed that buffer back, whatever the lock (the author, 2026-10-16):
  a locking scan's rows stay locked while they are read, and memory
  stays within the receivers times the batch bytes. A flag of our own,
  `read_ahead`, off by default, lets a committed read's receiver go on
  at once instead, as the continuous scan of phase 10 would do at the
  data nodes. A slow reader parks only its own scan, never the
  executor's loop. The reader's
  synchronous `next_row` waits on a condition of its own; a Rust stream
  comes later over the same.
- Chapter 04, "Consuming scans", has the design, and the C ABI for
  scans on a connection and through an executor.
- **Built 2026-10-16**; the `scan_` integration group passes against
  `rondb_1107_cte_test`: the 7 tests of 8.3 and 5 new ones (`next_row`
  on a connection; a scan read row by row beside another; readers on an
  executor in every lock mode and with `read_ahead`; readers closed
  early, dropped and refused; a slow reader), and the C ABI's test.
  While one reader held a batch, five other readers on the same
  executor each read the whole table, in 7 ms together.
- Left for later: a Rust stream over the reader; scans whose
  coordinator stays up but never answers still wait for the run's own
  `timeout_ms` or the coordinator's timers (from 8.3).

### 8.5 Updating and deleting what a scan locked (3 days)

Takeover: rows with their keys (`SCAN_ROW_KEY_REP`), and a `KEY_OP_REQ`
on the owning transaction carrying the takeover words. A receiver is
asked for more only once every takeover of its batch has its
`KEY_OP_CONF`, since that request releases the batch's locks (chapter
05 §7.2).

### 8.6 Character sets from the data nodes (1 week, with RonDB's new request)

Decided by the author, 2026-10-08, in place of the hand-written latin1
module planned earlier the same day:
- **What a character set is needed for:** hashing a distribution key
  with a character column, for exact placement, and merging the rows of
  an ordered index scan in order (8.7). Values pass as bytes; the
  coordinator hashes a key itself, so a key operation is right with any
  character set, and only our choice of node and our merge need the
  collation.
- **Where it comes from:** the data nodes. RonDB 26.04 main gets, in a
  separate task, a request that describes a character set and its
  collations as the table description request describes a table: the
  encoding, the weights and the ordering. iClaustron asks for one the
  first time a column names a character set it has not seen, as
  `table_bind` does for a table, and keeps it in the global dictionary
  (the shared dictionary cache) for every thread.
- **What may be compiled in:** character sets whose tables are public
  standards, with the source named in the code: the binary set (each
  byte is its own weight), the CP1252 code page as Unicode publishes it
  (MySQL's latin1 encoding, for text conversion), Unicode's own data.
  What MySQL alone defines, the weights and order of its collations,
  comes only from the data nodes.
- **The request, as RonDB 26.04 main has it (2026-10-08):**
  `COLLATION_DESC_REQ` (984) to the dictionary block, by collation
  number, collation name or character set name, answered by
  `COLLATION_DESC_CONF` (985) or `COLLATION_DESC_REF` (986), only by
  data nodes of 26.10.0 or later (chapter 05 §8.2). The answer gives the
  collation's names, its set's default and binary collations, flags (no
  pad, case sensitive, Unicode, ...), the bytes a character takes, how
  much a string grows as a sort key, the pad byte and the weight levels.
  Asked with weights, it also carries the weight string of every
  character and every contraction, at most 1 MB an answer, in as many
  round trips as the collation needs, each from the position the last
  gave; the data node serves one such request at a time and answers
  others busy. The codec, with the weight runs and contractions read and
  stepped, is in `ic_signals::collation_desc`; the dissector decodes the
  three signals and the weights of an answer that comes whole.
- **What the weights do not cover:** a few collations (czech,
  win1250ch, tis620) weigh a character by its neighbours, which
  per-character weights cannot rebuild, so their keys stay placed round
  robin. A no-pad collation's key is hashed with the collation's own
  function, which the author described (was Q16; chapter 05 §6.3).
- **Work:**
  - the request and its reply in `ic_signals`, and the round trips in
    `ic_apid::collation_client`, shown by `ic_desc --collation` and
    `--charset` (done 2026-10-09: the integration group `collation`
    passes against the development cluster; what was seen is in chapter
    05 §8.2). A sort key's length is not bounded by the sort-key growth
    word, which the 9.0.0 Unicode collations give as 0;
  - a character-set entry in the dictionary cache, fetched on first
    use and kept while the cluster connection lasts;
  - `hash.rs`: a character column in a distribution key, of a table or
    of a unique index, hashed with the fetched weights; until its
    character set is described, placement falls back to round robin, a
    hop, not a wrong answer;
  - the merge of 8.7 compares character columns by the fetched order;
  - `text_row.rs`, for the tools: text in and out through the
    compiled-in tables, hex where there is none.
- **Tests:** for each collation the data nodes describe, CHAR and
  VARCHAR keys covering every byte of a single-byte set, case variants
  and trailing spaces, inserted through MySQL into a table of many
  partitions: our hash's partition equals the one the data node reports
  (`key_op::read_partition`), and keys the collation calls equal land
  together; ordered scans over such columns return MySQL's `ORDER BY`
  order.
- **Exit:** placement and merge are exact for latin1 in all eight of
  its collations; another character set the data nodes describe works
  the same way and gets its own test when it is first used.

Placement done ahead of phase 8 (2026-10-12; the integration group
`charset` passes): `ic_apid::collation_map` turns a collation the data
nodes describe into the transform its keys are hashed through (pad
space: the padded sort key; no pad: FNV-1a over the weights;
utf8mb4_0900_bin: a fold of the bytes), decoding UTF-8, UTF-16, UCS-2,
UTF-32 and any set of one byte a character. Binding a table, or an
index, fetches the collation of each character column a key is hashed
by into the dictionary cache, once per collation for the process; a
collation that cannot place keys leaves them placed in turn.
`hash::partition_of` and `hash::index_partition_of` put character
columns through it, so tables and unique indexes with character keys
are placed exactly. Seen live to match the data nodes for latin1's
eight collations, utf8mb4's pad-space and fourteen of its 9.0.0
collations including Spanish, German, Swedish, Japanese and Chinese
tailorings, utf8mb3, ucs2, utf16, utf16le, utf32, latin2 and cp1251,
for keys of several columns and for unique indexes (chapter 05 §6.3).
Still to do in this step: text in and out through compiled-in tables
in `text_row`; the merge is 8.7's.

### 8.7 Ordered index range scans (1.5 weeks)

Bounds of every type, several ranges with their numbers; every fragment
scanned in parallel and the rows merged in order in the client, which
needs a row or the end from every fragment before it returns one;
descending order; character columns compared in their collation's
order (8.6), other types by type; a range pruned to one partition when it
fixes the distribution key.

### 8.8 C ABI and examples (3 days)

Scans in `ic_capi`, documented as in phase 7; examples in Rust and C: a
full scan, a range scan, a scan that updates, a scan through an
executor.

Exit: a scan of a million rows, parallel over every fragment, counts
what MySQL counts; range scans with every bound type return what MySQL
returns, and in MySQL's `ORDER BY` order for latin1 columns in each
collation; updates and deletes by takeover; a scan through an executor
serving 1 000 application threads; a data node killed during a scan
gives a temporary error, and the scan started again completes.

## Phase 9 — 0.3: basic interpreter, AUTO_INCREMENT (4–5 weeks)

### 9.1 A basic interpreter (3 weeks)

- The encoding learned from captures of the reference's interpreted
  operations, read with the dissector, and from the author, and written
  into chapter 05 §9 in our words, with names of our own for the
  instructions.
- Basic first (author, 2026-10-08): conditions comparing a column with
  a constant or with another column (=, <>, <, <=, >, >=; latin1
  strings by their collation), IS NULL and IS NOT NULL, AND, OR, NOT;
  on key reads, updates and deletes, and as scan filters. Assignments:
  a column set to a constant, or to itself plus or minus a constant. A
  conditional update or delete. A column's value read after an update,
  which AUTO_INCREMENT uses.
- Registers allocated over the eight there are, and
  `IC_ERROR_CONDITION_TOO_COMPLEX` beyond them, as decided.
- Later (phase 10): LIKE, regular expressions, the memory regions for
  spilling, and the rest of the builder in `ic_apid.h`.
- Exit: integration groups `where` and `assign` match MySQL on
  generated data, on key operations and on scans.

### 9.2 AUTO_INCREMENT and tables without a primary key (1.5 weeks)

From reading (chapter 05 §16.5):
- The counter is one row per table in the counter table, looked up by
  its stored name (`sys/def/SYSTAB_0`), keyed by the table id and
  holding the next value not handed out. The client that creates a
  table writes its row; MySQL servers do.
- Values are taken by one interpreted update that adds n × step and
  reads the new value back, in a transaction of its own placed by the
  counter row's key: one round trip, the lock held only inside the data
  node. They are handed out from a range the client keeps per table;
  the prefetch is ours to choose (Q13), step and offset follow MySQL's
  `auto_increment_increment` and `auto_increment_offset`. Values are
  unique across API nodes, not ordered, and unused ones are gaps.
- An explicit value above the counter moves the counter past it, only
  ever upwards, by an interpreted update that writes only if larger; a
  value inside our own cached range moves only the cache. The counter
  can be read without reserving.
- A table without a primary key has a hidden key column, last, stored
  as `$PK`, BIGUNSIGNED, taking values from the same counter. Inserts
  need only that. Reads, updates and deletes need the key, which only a
  scan returns (phase 8).
- Temporary errors are retried, as MySQL servers do.
- Exit: inserts from `ic_trans` and from a MySQL server, interleaved
  into one AUTO_INCREMENT table, give no duplicate; an explicit value
  moves the counter for both; on a table without a primary key, inserts,
  then a scan with updates and deletes by takeover.

## Phase 10 — Later

TLS to the data nodes and the management server; choosing nodes by
location domain; BLOB, TEXT and JSON through the parts table, and with
them deletes on such tables; UTF-8 (utf8mb4) collations for hashing and
merging; the C++ header and bindings for other languages; events; DDL
(a client creating a table with an AUTO_INCREMENT column writes its
counter row); the full interpreter; pushdown joins; aggregation
pushdown; ring buffer tables; TTL options; rate-limit and user-id flags;
the deferred unique check; scans with four receivers per fragment for
committed reads, which suit an executor; a fanout table's scans pruned
to its partitions; closing a scan at once, with batches on their way.

## Working method per module (all phases)

1. Read the C header and source; copy the header prose into Rust doc
   comments (it is our text). Check every protocol constant against 26.10.
   Each protocol fact goes into chapter 05 in our words, with a bare
   RonDB file:line, before the code that uses it; code comments cite the
   chapter 05 section.
2. Write the Rust module with the same file name and function names as
   the C, and give anything the C does not have a name of our own that
   says what it does, never a RonDB name (chapter 09).
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
| iClaustron's protocol knowledge is NDB 7.2.9; anything not re-verified against 26.10 is suspect | every protocol constant is backed by a chapter 05 section with a 26.10 file:line; Phase 3 signal tracing shows mismatches immediately |
| Legacy vs TLS-capable socket auth (Q2) | test both on day one of Phase 3 |
| The rename (6.1) changes something on the wire | a pure rename: a capture taken before replays byte for byte after; every test passes unchanged |
| A RonDB name creeps back in | `cargo xtask names` reads the names from a RonDB tree at check time; review checklist item |
| character-set weights wrong | the end-to-end check against the partitions the data nodes report (8.6); a wrong weight costs a hop, never a wrong answer, until the ordered merge of phase 8, whose exit compares with MySQL's `ORDER BY` |
| A fragment train the coordinator drops gets no reply | the 4012 timer of 6.3 comes before the fragmenting sender of 6.6 |
| Scan flow control starves an executor | credit per receiver, rows copied out of receive pages (8.3, 8.4); measured with many consumers |
| Interpreter encoding is large | phase 9 starts by writing chapter 05 §9; basic first; register-only programs, spill strategy decided later |
| Performance below target with dedicated receive threads | benchmark at Phase 5 and 7; tune receive thread count, batching, page sizes; the model itself is fixed |
| Clean-room slips | review checklist item; `MODULE.md` cites, never quotes; RonDB is cited only in chapter 05, by file:line |
| Scope creep into RonDB-only features | Phase 10 by decision |
