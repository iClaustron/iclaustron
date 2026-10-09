# 01 — Goals and principles

## What we are building

iClaustron: a client library for RonDB data nodes ("the Data API"),
written in C-like Rust, MIT licensed, usable from

- Rust (native crate),
- C (a stable C ABI: `libiclaustron` shared + static library and a header),
- C++ (a thin header-only wrapper over the C ABI),
- other languages through the C ABI (Python, Go, Java, Node.js, .NET, ...).

The object model is the iClaustron Data API design (`include/ic_apid.h`),
updated where this plan says so. The wire protocol is the NDB protocol as
spoken by **RonDB 26.10** data nodes and management servers. The library is
an **API node** only: it reads cluster configuration from the management
server, it never writes configuration, and it never acts as a management
server.

## Scope of the first release (0.1)

- Connect to a RonDB 26.10 cluster through a stock connectstring, allocate
  a node id, fetch and decode the configuration.
- Connect to every data node, heartbeat, track node state, handle node
  failure and recovery protocols (`NODE_LOST_REP`, `NODE_LOST_DONE_REP`,
  reconnect), and report node failure to affected transactions correctly.
- Read table metadata for any table created through MySQL.
- Transactions with primary key and unique key operations: read (all
  lock modes), insert, update, delete, write; commit and rollback;
  asynchronous `send`/`poll`/`flush` model with callbacks.
- Record-style row binding that a C programmer uses with plain structs.
- Keys with character columns work in any character set; they are
  placed round robin until latin1 placement comes in 0.2.
- Every row size the data nodes accept can be written.
- A transaction whose reply never comes ends with 4012, outcome
  unknown, instead of waiting forever.
- Tables with BLOB columns: the other columns can be used; deletes are
  refused, since the BLOB parts would be left behind.
- C ABI with every function documented, a user guide, and the same
  examples in Rust and in C.

Later releases (replanned 2026-10-08, chapter 06): 0.2 the executor and
scans, ordered index ranges included, and latin1 placement and
ordering; 0.3 a basic interpreter,
AUTO_INCREMENT and tables without a primary key; then TLS, location
domains, BLOBs, UTF-8, the C++ header, events, DDL, pushdown joins and
aggregation pushdown.

## Out of scope for this project

- The iClaustron Cluster Server, Cluster Manager, Cluster Client, Process
  Controller, bootstrap, configurator, file server and replication server.
  All existing C code moves unchanged to `legacy-c/` in this repository as
  reference; only the parts named in chapter 03 are translated. (Possibly
  a future project.)
- Configuration file reading and writing (`.ini` readers, transactional
  config writer). The only configuration source is the management server.
- Windows. Linux and macOS are the targets.
- RonDB versions before 26.10; no version gating, no compatibility paths.

## Principles

1. **Keep the Linux-style module structure of iClaustron.** One Cargo crate
   per C directory that survives the scope cut (`port`, `util`, `comm`,
   `protocol`, `api` split into `apic` and `apid`). Each `.c`/`.ic` file
   becomes one `.rs` module with the same name.

2. **C-like Rust, explained for C readers.** Plain structs, enums and
   functions, explicit loops and state. The rules are in
   [07-style-guide.md](07-style-guide.md); the construct-by-construct
   translation table with explanations is in
   [11-c-to-rust-mappings.md](11-c-to-rust-mappings.md). Every crate's
   `MODULE.md` has a "Rust notes for C readers" section for anything
   non-obvious in that crate.

3. **The C header is the design document.** `ic_apid.h` sets object model
   and semantics. This plan records where it changes
   ([04-api-design.md](04-api-design.md)) and the header is rewritten to
   match before the code.

4. **Clean room against RonDB.** RonDB is GPLv2. We read its sources for
   protocol facts, write our own code, comments and error texts, and
   give everything names of our own: no RonDB name of a signal, block,
   field, function or class appears in our code or docs; the wire stays
   identical (decided 2026-10-08). See [09-licensing.md](09-licensing.md).

5. **Asynchronous first.** Define queries, `send`, do other work, `poll`,
   iterate completed queries. `flush` is a convenience on top.

6. **Receive threads receive; user threads execute.** Dedicated receive
   threads read sockets, split signals and route them to the owning user
   thread's queue. All signal execution (decoding rows, updating
   transaction state, callbacks) happens in the user thread that owns the
   query. This is the deliberate difference from the reference API, whose
   receive thread executes signals and then wakes the user thread; the
   iClaustron split removes that work, and the contention it causes, from
   the receive thread. User threads never act as receivers.

7. **One thread, one connection object.** An `ApidConnection` is owned by
   one thread. Mutex ordering levels from `api/ic_api_mutex_doc.txt` are
   completed and enforced in debug builds.

8. **Small dependency surface.** No glib. Standard library plus `libc`.
   Base64, MD5, XXH3, option parsing and the poll-set abstraction are
   written in-tree. `std::collections::HashMap` replaces the C hash table.

9. **Stable C ABI.** All cross-boundary types are `#[repr(C)]` or opaque
   handles; functions carry the `ic_` prefix; the header is generated by
   `cbindgen` and checked in.

10. **Tests are run by the developer.** Unit tests per crate and
    integration tests against a RonDB 26.10 cluster, each milestone naming
    the command that proves it. No step in this plan assumes automation
    beyond `cargo`.

11. **Translate module by module, design the whole.** Foundation crates are
    translated nearly one to one from the C. The Data API data path is new
    code built from [04-api-design.md](04-api-design.md) and
    [05-ndb-protocol.md](05-ndb-protocol.md), since the C stubs stop at the
    transport layer and the C design targeted NDB 7.2.9.

## Success criteria for 0.1

- Connects to a RonDB 26.10 cluster via connectstring, allocates a node
  id, stays connected through heartbeats, survives a data node restart
  and a management server restart.
- Describes any MySQL-created table (all column types, unique and ordered
  indexes) as the reference's own table description tool does.
- Primary key and unique key read/insert/update/delete/write with commit
  and rollback from Rust and from C; results verified through MySQL.
- A node killed mid-transaction yields the right temporary error class and
  the transaction can be retried on another node.
- A row of every size the data nodes accept is
  written and read back; a lost reply ends its transaction with 4012; a
  delete on a table with BLOB columns is refused; unique keys refuse
  NULL and are placed by the index (chapter 06 §6.5).
- `cargo xtask names` finds no RonDB name in the repository.
- Primary key read throughput within 20 % of the reference API using the
  asynchronous interface, one connection object per thread, 8 threads.
- Ships `libiclaustron.{so,dylib,a}` and `include/iclaustron/ic_apid.h`
  with every function documented, a user guide, and the same examples in
  Rust and in C, the C ones building with a plain compiler invocation.
