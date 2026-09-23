# 08 — Testing, tooling and verification

The developer runs all builds and tests from the command line; the
framework is designed for that and nothing assumes a CI service. Each
milestone in [06-phases.md](06-phases.md) names the command that proves
it. A CI job can run the same commands later if wanted.

## Toolchain

- Rust stable via `rustup` (not installed on this machine yet:
  `curl https://sh.rustup.rs -sSf | sh`, then `rustup component add
  clippy rustfmt`). Pin with `rust-toolchain.toml` to the current stable at
  project start and bump quarterly.
- `cargo install cbindgen cargo-deny` (header generation, license and
  advisory checks). Plain `cargo test` is the test runner.
- macOS (this machine) and Linux are both first-class; aarch64 Linux
  because RonDB ships on ARM.
- A C compiler and CMake only for the C/C++ examples and the compatibility
  benchmark against the C++ NDB API.

## Test pyramid

### 1. Unit tests (per crate, `cargo test -p <crate>`)

- `ic_util`: direct ports of `test/test_unit.c` types 1–8 (memory
  container, simple/ordered dynamic array, ptr array, bitmap, hashtable,
  connectstring, socket buffer). These are the first tests to exist.
- `ic_comm`: loopback socket tests for `Connection` read/write/writev/
  sessions and for each `PollSet` backend; `test/test_comm.c` scenarios.
- `ic_ndb_signals`: encode/decode round-trips for every signal struct;
  golden-vector tests from captured traffic (below); `cargo fuzz` targets
  (nightly toolchain, run by the developer when a decoder changes) for
  `header::decode`, `DictTabInfo` property parsing, `TRANSID_AI`
  attribute parsing and the config-blob decoder. These decoders take
  bytes from the network, so they are fuzzed from day one.
- `ic_apic`: parameter-table invariants (unique ids, unique names), config
  blob v2 decoding from captured blobs, typed config extraction.
- `ic_apid`: state machines tested without a network: transaction
  bookkeeping (pending counts, TCKEYCONF/TRANSID_AI ordering, node-down
  notices), adaptive send statistics, the WHERE-condition compiler
  (source tree → expected interpreter words), record packing/unpacking
  for every column type.

### 2. Captured-traffic tests

A small tool `tools/ic_capture` records signal streams between the C++ NDB
API and a data node (via a TCP proxy that understands the transporter
handshake), stores them as `.bin` + `.json` (decoded by our own codec), and
the unit tests replay them. This gives regression vectors for exactly the
signals RonDB emits, independent of RonDB source code. Captures come from
running the RonDB `ndbapi-examples` (`ndbapi_simple`,
`ndbapi_simple_index`, `ndbapi_async`, later `ndbapi_scan`) against a
local 26.10 cluster.

### 3. Integration tests (`cargo xtask test-integration` = `IC_TEST_CONNECTSTRING=... cargo test -p ic_apid --features integration`)

Need a live cluster. Two ways to get one:

- **Local build**: the RonDB tree at
  `/Users/mikael/mysql_trees/rondb_2604_main` built with
  `-DWITH_NDB=1`; start with the MTR-provided cluster
  (`cd mysql-test && ./mtr --start ndb.ndb_basic` leaves a cluster running
  on well-known ports) or with a hand-written `config.ini` and
  `ndb_mgmd`/`ndbmtd`/`mysqld`. This is the primary development setup.
- **Docker**: RonDB's published images (`rondb-docker`), tag pinned to
  26.10.

Tests are ordinary `#[test]` functions gated by an env var
`IC_TEST_CONNECTSTRING`; when unset they are skipped, not failed. Tables
are created via MySQL (`mysql` client over TCP) so the tests exercise
exactly what `mysqld`-created tables look like, which is what users have.

As built (2026-09-22, `crates/ic_apid/tests/integration.rs`): three
variables, and a test that lacks what it needs says so on stderr and
returns. `IC_TEST_CONNECTSTRING` names the management server;
`IC_TEST_MYSQL` is a `mysql` client command with its connection
options, run through `sh -c`, through which the tests make their
tables in the database `ic_it` and check what they wrote;
`IC_TEST_NDB_MGM` is an `ndb_mgm` command for the `failure` group,
which stops a data node and starts it again. Each test is its own
program against the cluster, taking an API node id and letting it go,
with a retry on the id since the one the last test released may not be
free at once. The groups built are the phase-5 ones: `connect`,
`dict` (columns and key, index columns in key order, the cache letting
go after `ALTER TABLE`), `pk` (all five operations checked through
MySQL, 626 and 630, commit and rollback visibility), `uk` (read, update
and delete through `uk_code$unique`, the name a MySQL server gives a
unique hash index), `types` (the integer family at its edges, char,
varchar, binary, varbinary, and the long forms, written through the API
and read back both ways; float and double read from what MySQL wrote;
a row of NULLs; the date, time, decimal and bit types wait for the text
codec to format them) and `failure` (an update confirmed at a
coordinator that is then restarted: the commit fails with a temporary
error, the retry at the survivor commits, and the node is waited for
until it is started again, so that the next test finds the cluster
whole). Seen live 2026-09-22: the pending transaction ended rolled
back with 4010, the branch built for a coordinator that fails before
the commit is asked for. The `ndb_mgmd` restart is not yet covered.

Test groups, mirroring the phases:

| Group | Proves |
|---|---|
| `connect` | node id allocation, handshake with every data node, API_REGCONF received, `wait_first_node_connect` returns; survives `ndb_mgm -e "2 RESTART"` |
| `dict` | every column type in one table decodes to the right `FieldType`/length; indexes and their key order; table version bump after `ALTER TABLE` invalidates the cached def |
| `pk` | read/insert/update/delete/write; error 626 (no such row) and 630 (duplicate) mapping; commit and rollback visibility checked through MySQL |
| `uk` | unique index read/update/delete |
| `scan` | (0.2) full table scan count equals `SELECT COUNT(*)`; range scans with each bound type; multi-range; takeover update/delete |
| `where` | pushed conditions match MySQL results on a randomly generated table |
| `assign` | conditional assignments `a = a * (b + 1)` match MySQL arithmetic |
| `types` | round-trip of every type incl. Decimal, Datetime2/Timestamp2 fractional seconds, Bit, Varbinary, Char with charsets |
| `blob`, `event`, `ddl` | 0.3 |
| `failure` | kill a data node mid-transaction: correct temporary error class, transaction retried by the test on another node; restart `ndb_mgmd` while connected |

### 4. Conformance against the C++ NDB API

`tools/ic_bench` and a matching C++ program (built in the RonDB tree using
its NDB API) run the same workload (PK reads of 8 columns, 1 000 rows,
N threads, async batches of 100). We record throughput and p99 latency
for both and keep the numbers in `doc/bench/`. The 0.1 criterion is
within 20 % of the C++ API.

### 5. Multi-language smoke tests

Under `tests/examples/`: `c/simple.c`, `cpp/simple.cpp`, plus the
languages chosen in Q6 (Python `ctypes`, Java Panama suggested). Each does
connect → bind table → PK insert/read → commit and is run by
`cargo xtask test-examples` against the development cluster. Their purpose is to catch ABI mistakes
(struct layout, ownership, string lifetime), not to be real bindings.

## Static checks (`cargo xtask check`, run by the developer before a commit)

```
cargo fmt --check
cargo clippy --workspace --all-targets -- -D warnings
cargo deny check licenses advisories
cargo test --workspace
cargo doc --no-deps -D rustdoc::broken_intra_doc_links
cargo xtask header --check       # cbindgen output == checked-in header
```

Plus a custom check `cargo xtask style`, which does two things clippy
and rustfmt cannot:

- **Line width.** Every line of every `.rs` file must fit in 80 columns.
  rustfmt keeps code within the limit but never reflows a comment or a
  string literal, so those are only caught here.
- **Forbidden constructs.** The items from
  [07-style-guide.md](07-style-guide.md) that clippy cannot express
  (`impl Trait`, `where `, `async `, `Rc<`, `RefCell<`, `.iter().map(`,
  `macro_rules!`, `.unwrap()`), skipped inside `#[cfg(test)]` modules,
  with `xtask/style-allow.txt` listing the few sanctioned places and the
  reason for each.

## Editor navigation: `cargo xtask tags`

The successor of `legacy-c/git_tags.sh`. It lists every source file git
knows about (tracked, or new and not ignored), Rust and C, and feeds
them to Universal Ctags with the Rust, C and C++ parsers, producing a
sorted `./tags` file for vim/gvim (`tags` is git-ignored). `legacy-c/` is
included by default so that a C function and its Rust translation are
both one `Ctrl-]` away; `--no-legacy` leaves it out. Needs Universal
Ctags (`brew install universal-ctags` or `sudo port install
universal-ctags`); the BSD `ctags` that ships with macOS has no Rust
parser and the command says so. Suggested `~/.vimrc` lines:

```
set tags=./tags;,tags;
" :tag name, Ctrl-] jump, Ctrl-T back, :tselect for several matches
```

Re-run after adding files; a `BufWritePost` autocommand or a git
`post-commit` hook calling `cargo xtask tags` keeps it current. For tags
into the Rust standard library and dependencies, `rusty-tags`
(`cargo install rusty-tags`, `rusty-tags vi`) can be layered on later.

## Debugging aids to build early

- `IC_DEBUG=level_bits` env var driving the ported `DEBUG_*` machinery,
  same 17 level bits as the C.
- `IC_NDB_MESSAGE_LEVEL` prints every signal in and out as
  `gsn name, sender/receiver block refs, words, sections` using the
  `ic_ndb_signals` decoders. This is the single most useful tool during
  phases 3–6.
- `tools/ic_config_dump`: fetch and print a cluster config from
  `ndb_mgmd` (phase 2 deliverable), also used to diff parameter tables
  between RonDB versions.
- `tools/ic_desc db.table`: print a table definition as the API sees it
  (phase 4 deliverable).
- `tools/ic_sigdump file.bin`: decode a captured signal stream.

## Documentation

- `cargo doc` for the Rust API; the doc comments are ported from the
  `ic_apid.h` prose (they are the specification).
- The C header carries the same prose (cbindgen copies doc comments).
- `doc/` in the new repo: this plan, a user guide with the C, C++ and Rust
  examples, and `MODULE.md` per crate.
