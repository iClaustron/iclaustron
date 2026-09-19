# 04 — API design: Rust, C and C++

`include/ic_apid.h` remains the base. This chapter records how each C
construct appears in Rust and in the C ABI, the decisions that fill gaps,
and the changes decided for this project: a record-style row binding, no
savepoints, no cluster-server dependencies, RonDB 26.10 semantics. The C
header is rewritten to match this chapter before implementation starts
(Phase 0 deliverable).

## Three surfaces, one implementation

| | Rust (`ic_apid`) | C (`ic_capi`) | C++ (`iclaustron.hpp`) |
|---|---|---|---|
| Object | `struct ApidConnection` (fields private) | `IC_APID_CONNECTION*` opaque | `ic::Connection` RAII over the C pointer |
| Method | `conn.read_key(&mut q, &mut t, &args)` | `ic_apid_conn_read_key(conn, q, t, &args)` | `conn.read_key(q, t, args)` |
| Error | `Result<(), IcError>` | `int` code + `ic_apid_error_*` accessors | `ic::Error` value type |
| Callback | `Option<fn(&mut ApidConnection, &mut ApidQuery, usize)>` + `user_ref: usize` | `IC_APID_CALLBACK_FUNC` + `void*` | same as C, lambdas via trampoline |
| Enums | `#[repr(u32)] enum ReadKeyQueryType` | `IC_READ_KEY_QUERY_TYPE`, same values | `enum class`, same values |

The Rust API is the implementation. The C ABI is a mechanical wrapper:
one exported function per method, `Box::into_raw`/`from_raw` at create
and free, null checks, `catch_unwind` so a Rust panic never crosses into
C. The C++ header wraps the C ABI only. Other languages bind the C ABI.

**Flat functions instead of `_OPS` vtables.** `conn->apid_conn_ops->
ic_read_key(conn, ...)` becomes `ic_apid_conn_read_key(conn, ...)`.
Function pointer tables are awkward for `ctypes`, `cgo`, Panama and
P/Invoke and cost an indirect call. The header still offers the `_OPS`
structs as `static inline` wrappers for those who prefer the object
style; both are the same ABI.

**Opaque handles.** `ic_apid_hidden.h`'s struct-prefix trick is not used
across the C ABI; accessors are real functions (`ic_apid_query_is_failed`).
In Rust they are `#[inline]` methods, so Rust users pay nothing.

## Object model

| C type (header) | Rust type | Created by | Freed by |
|---|---|---|---|
| `IC_APID_GLOBAL` | `ApidGlobal` | `ApidGlobal::connect(connectstring, options)` | `drop` / `ic_apid_global_free` |
| `IC_APID_CONNECTION` | `ApidConnection` | `ApidGlobal::create_connection()` | `drop` / `ic_apid_conn_free` |
| `IC_TABLE_DEF`, `IC_INDEX_DEF` | `TableDef`, `IndexDef` | `ApidConnection::table_bind(db, table)` / `index_bind(db, index, table)` | `table_unbind` |
| **`IC_RECORD`** (new) | `Record` | `TableDef::create_record(&[FieldSpec])` | `drop` / `ic_record_free` |
| `IC_APID_QUERY` | `ApidQuery` | `ApidGlobal::create_apid_query(table_def, key_record, attr_record)` | `free` / `ic_apid_query_free` |
| `IC_TRANSACTION` | `Transaction` | `ApidConnection::start_transaction(hint)` | closed on commit/rollback completion |
| `IC_TRANSACTION_HINT` | `TransactionHint` | `TransactionHint::from_key(table_def, key_record, key_row)` or `::node(node_id)` | `drop` |
| `IC_WHERE_CONDITION` | `WhereCondition` | `ApidQuery::create_where_condition()` or `ApidGlobal::create_where_condition()` | as C |
| `IC_CONDITIONAL_ASSIGNMENT` | `ConditionalAssignment` | `ApidQuery::create_conditional_assignments(n)` | as C |
| `IC_APID_ERROR` | `IcError` (`#[repr(C)]`) | embedded | — |
| `IC_RANGE_CONDITION`, `IC_METADATA_TRANSACTION`, `IC_ALTER_TABLE`, `IC_ALTER_TABLESPACE` | kept in the header, marked "later release" | | |

Removed from the header: `ic_create_savepoint`, `ic_rollback_savepoint`,
`IC_SAVEPOINT_ID`, the savepoint commit states, `ic_external_connect`,
`ic_get_api_config_server`, the `cluster_id` parameter on every call
(see Q3 in [10-open-questions.md](10-open-questions.md); one cluster per
`ApidGlobal` in 0.1, multiple `ApidGlobal`s for multiple clusters).

## Row binding: records (replaces the 64-bit slot array)

Decision: the primary row model is a **record**, an NdbRecord-like
description of a C struct, so that a C programmer writes

```c
struct my_row {
  uint32_t a;            /* key, INT UNSIGNED NOT NULL          */
  uint32_t b;            /* INT UNSIGNED, nullable               */
  uint8_t  name_len;     /* VARCHAR(20): NDB 1-byte length + data */
  char     name[20];
  uint8_t  null_bits;    /* bit 0 = b is NULL                    */
};
```

and declares it once:

```c
IC_FIELD_SPEC spec[3] = {
  { .field_id = ID_A,    .offset = offsetof(struct my_row, a),        .null_bit = IC_NOT_NULLABLE },
  { .field_id = ID_B,    .offset = offsetof(struct my_row, b),        .null_bit = IC_NULL_BIT(offsetof(struct my_row, null_bits), 0) },
  { .field_id = ID_NAME, .offset = offsetof(struct my_row, name_len), .null_bit = IC_NOT_NULLABLE },
};
IC_RECORD *rec = ic_table_def_create_record(table, spec, 3, sizeof(struct my_row));
```

Rules of the record format (identical to what NDB stores, so no
conversion is needed on the hot path):

- Fixed-size fields are stored inline at `offset` with their NDB size
  (Int 4 bytes, Bigint 8, Char(n) n bytes, Date 3, Datetime2 5+frac, ...).
- Varchar/Varbinary: 1 length byte then data, inline; Longvarchar/
  Longvarbinary: 2 little-endian length bytes then data, inline. The
  struct reserves the maximum size.
- Nullable fields name a byte offset and bit number; a set bit means NULL.
- Decimal: packed binary inline. Bit(n): `ceil(n/32)` words inline.
- BLOB/TEXT: skipped for the moment. A table with BLOB columns can be
  bound and used as long as the record and mask leave those columns out;
  a record naming a BLOB column is rejected with `IC_ERROR_NOT_SUPPORTED`.
- The record also carries a field mask; a query can use a subset of the
  record's fields per call (`mask` argument) without a new record.
- Alignment is the user's business: the API reads and writes bytes at the
  offsets given; it validates that offsets do not overlap and fit in
  `row_size`. Helper `ic_table_def_default_record(table)` builds a record
  with natural alignment and reports the offsets, for users who do not
  want to write the struct by hand (this is `ic_get_buf`/
  `ic_get_buf_offset` from the C header, now returning a record).
- Key records: a record over the key fields only (for `read_key` etc.).
  A record covering all key fields of the table can serve as both key and
  attribute record.

Rationale: the design's 64-bit slot array needed pointer chasing for every
string and a second allocation model; the record is one memcpy per field
in and out of `TRANSID_AI`/`ATTRINFO`, maps directly onto C structs and
mysqld rows, and is what RDRS and every heavy NDB API user already use.
The slot array can be added later as a convenience record type if wanted.

Types: `IC_FIELD_TYPE` grows to the full RonDB list (05 §8.1). Temporal
types are stored in their raw NDB encoding; helper functions
(`ic_datetime2_unpack` etc.) convert to broken-down time. Char/Varchar are
bytes; the charset id is exposed on `TableDef`; no transcoding.

## Query definition and execution

```
ApidQuery::create(table_def, key_record, attr_record)      once
conn.read_key(query, trans, ReadKeyArgs {
    query_type: KeyRead | SimpleKeyRead | ExclusiveKeyRead | CommittedKeyRead
              | ExclusiveKeyReadNoWait,
    key_row, attr_row, attr_mask, abort_option, callback, user_ref })
conn.write_key(query, trans, WriteKeyArgs {
    query_type: KeyInsert | KeyUpdate | KeyDelete | KeyWrite,
    key_row, attr_row, attr_mask, abort_option, batch_hint, callback, user_ref })
conn.unique_read_key / conn.unique_write_key   same, with an IndexDef-based query
conn.send(force) ; conn.poll(ms) ; conn.flush(ms, force)
conn.get_next_executed_query() -> Option<&mut ApidQuery>
query.is_failed() ; query.error() ; query.reset()
```

Arguments are passed in one `#[repr(C)]` struct per call (`ReadKeyArgs`,
`WriteKeyArgs`) so the C ABI has a fixed signature and new options can be
appended without breaking callers (the struct starts with its own size).
`define_field`/`define_pos`/`transfer_ownership` from the C header are
superseded by records; `set_partition_id(s)` stays (scans, later).

- `abort_option`: `AbortOnError` (default for writes), `IgnoreError`
  (default and only option for committed reads).
- `batch_hint`: `None | BatchSafe | BatchUnsafe` → TCKEYREQ bits 22/23.
- Callbacks fire on the thread calling `poll`/`flush`; a `None` callback
  leaves the query for `get_next_executed_query`.
- **No protocol detail reaches the caller.** A reply the data node sent
  in fragments is put back together inside `poll` before it is executed,
  so an application, or an API layered on this one, sees a completed
  query and never a fragment, a signal or a signal number. (Decided by
  the author, 2026-09-19; mechanism in chapter 02, "Fragmented signals
  never leave the library".)

## Transactions

- `start_transaction(hint)` is local; the kernel transaction starts with
  the first key operation (start flag).
- `TransactionHint::from_key(table, key_record, key_row)` hashes the
  distribution key and selects the primary replica (or any replica for
  read-backup / fully-replicated tables); `::node(id)` forces a node;
  no hint = round robin over live data nodes.
- Transaction id: high 32 bits = this connection's block number,
  low 32 bits = a per-connection counter; unique in the process.
- `commit_transaction(trans, cb, user_ref)` piggybacks the commit flag on
  the last defined unsent query, else sends `TC_COMMITREQ`.
  `rollback_transaction` likewise with `TCROLLBACKREQ`.
- Completed transactions report `CommitState::{Committed, RolledBack}`
  and the GCI.
- Node failure: queries on a dead TC node fail with a temporary
  node-failure error; `TCKEY_FAILCONF` reports "committed anyway".
- **Savepoints removed** (no kernel counterpart; no use case).
- **Joinable transactions** (`joinable` flag, `join_transaction`) are
  dropped from 0.1. If needed later they require routing replies by
  object id across threads; see Q4.

## WHERE conditions and conditional assignments

The register-machine builder in the C header is kept as is. The
implementation compiles the recorded program into RonDB 26.10 interpreter
words at `read_key`/`write_key` time and caches the compiled program on
the condition object. Every builder construct including LIKE maps to the
kernel; regexp is the one construct evaluated API-side after the row
arrives, documented as such. 0.1 allocates the 8 interpreter registers
only and returns `IC_ERROR_CONDITION_TOO_COMPLEX` for trees needing more
live values; the spill strategy (26.10 memory regions) is decided later. Compilation and validation
happen in `ic_evaluate_where`.

## Metadata

`table_bind`/`index_bind`/`table_unbind` perform `GET_TABINFOREQ` and fill
`TableDef`/`FieldDef`/`IndexDef` (field ids, types, sizes, nullability,
key order, distribution keys, hash map, read-backup/fully-replicated
flags, charsets, defaults, autoincrement flag). `list_tables(db)`.
`MetadataTransaction`/`AlterTable`/`AlterTablespace` stay in the header
for the DDL release; the working C DDL code is carried over then.

## Program scaffolding

`ApidGlobal::connect(connectstring, GlobalOptions)` does connect, node id
allocation, config fetch and thread start in one call (mirroring
`Ndb_cluster_connection::connect` + `wait_until_ready`). `GlobalOptions`
covers node id, receive thread count (fixed for the life of the global),
heartbeat interval, adaptive-send limits, connect timeouts. The `ic_start_program`/`ic_start_apid_program`/
`ic_run_apid_program`/`ic_stop_apid_program` helpers stay for programs
that want the thread-pool driven style, built on top of the above and on
`ic_port::options`.

## C ABI rules

- Every exported symbol starts with `ic_`; every exported type is
  `#[repr(C)]` or opaque.
- Strings in: `const char*` NUL-terminated, copied. Strings out: valid
  until the owning object is freed or reset; documented per function.
- Row buffers are the caller's memory; the API never allocates row
  storage. It reads them during `send` and writes them during `poll`.
  Between `send` and completion the rows must not be touched (same rule as
  the C header).
- Callbacks run on the `poll`/`flush` caller's thread only.
- A panic inside the library becomes `IC_ERROR_INTERNAL_PANIC`; the
  connection is marked unusable.
- `ic_api_version(&major, &minor, &patch)`; soname follows semver.
- Header generated by `cbindgen`, checked in; enum values are those of
  `ic_apid_datatypes.h`, appended never inserted.

## C++ header

`namespace ic`: `Global`, `Connection`, `TableDef`, `Record`, `Query`,
`Transaction`, `Where`, `Assignment`, `Error`. Move-only; destructors call
the C free functions; methods return `ic::Error` (`explicit operator
bool`). Lambdas allowed as callbacks through a stored `std::function` and
a C trampoline. Optional `IC_CPP_EXCEPTIONS`. C++17, no dependencies.

## Example (Rust, asynchronous style)

```rust
let global = ApidGlobal::connect("localhost:1186", &GlobalOptions::default())?;
let mut conn = global.create_connection()?;
let table = conn.table_bind("test", "t1")?;

#[repr(C)] struct Row { a: u32, b: u32, null_bits: u8 }
let rec = table.create_record(&[
    FieldSpec::not_null(table.field_id("a")?, offset_of!(Row, a)),
    FieldSpec::nullable(table.field_id("b")?, offset_of!(Row, b),
                        offset_of!(Row, null_bits), 0),
], size_of::<Row>())?;
let mut q = global.create_apid_query(&table, &rec, &rec)?;

let mut row = Row { a: 42, b: 0, null_bits: 0 };
let mut t = conn.start_transaction(Some(&TransactionHint::from_key(&table, &rec, &row)))?;
conn.read_key(&mut q, &mut t, &ReadKeyArgs::committed(&mut row, None, 0))?;
conn.commit_transaction(&mut t, None, 0)?;
conn.flush(1000, true)?;
while let Some(done) = conn.get_next_executed_query() {
    if done.is_failed() { return Err(done.error().clone()); }
}
println!("b = {}", row.b);
```

The same program in C is the same calls with `ic_apid_conn_` prefixes,
`int` returns and a `goto error` chain; it is the first file in
`tests/examples/c/`.
