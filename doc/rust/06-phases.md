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
  thread, a batch of committed reads or updates in flight at a time,
  each its own transaction hinted to the node holding its row; reports
  operations per second and the batch time's median, 99th percentile
  and worst. Plain on purpose: the pipeline drains between batches and
  every operation is its own socket write until step 4 of the thread
  plan gathers sends. Measure a release build.

  **Baseline, 2026-09-22**, release build, one thread, a two-node
  RonDB 26.10 cluster on the same machine as the client, a table of
  two INT columns, 100 operations in flight: committed reads 311 000
  per second, a batch taking 306 µs at the median and 534 µs at the
  99th percentile; updates 84 500 per second, a batch 1.18 ms at the
  median, 1.5 ms at the 99th and 11 ms at worst. About 3 µs per read
  on the API side with one socket write each. The comparison with the
  C++ NDB API on the same table and cluster is still to be made.
- Exit: integration groups `pk`, `uk`, `types`, `failure` pass; a 1-thread
  asynchronous PK read benchmark is within 2× of the C++ NDB API (the
  20 % target is Phase 7).

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
