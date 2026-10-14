# iClaustron user guide

iClaustron is a Data API for RonDB: a library an application links to
read and write rows of a RonDB cluster directly, as an API node, by
primary key and by unique key, in transactions. It is written in Rust
and has a C API; this guide covers both. It is for release 0.1, against
RonDB 26.10.

The examples in `examples/` are the same five programs in Rust and in
C, and the guide quotes them. The C API's reference is its header,
`include/iclaustron/ic_apid.h`; the Rust API's is `cargo doc`; the
design behind both is `doc/rust/04-api-design.md`.

## 1. What you need

- **A RonDB 26.10 cluster** with a free API slot for each program that
  connects: an `[api]` section in the cluster's configuration without a
  `NodeId`, or one with the node id the program asks for.
- **Network access** from the program to a management server (port
  1186 by default) and to every data node's port, which the management
  server tells the library at run time.
- **To build**: Rust (the toolchain in `rust-toolchain.toml`) for the
  library itself; a C compiler for the C API.

The tables are made through a MySQL server of the cluster, as usual;
iClaustron reads and writes their rows. `doc/dev-cluster.md` sets up a
development cluster on one machine.

### Building and linking

From Rust, depend on the crates by path or git:

```toml
[dependencies]
ic_apid = { git = "https://github.com/iClaustron/iclaustron", branch = "RUST-iclaustron" }
ic_port = { git = "https://github.com/iClaustron/iclaustron", branch = "RUST-iclaustron" }
```

From C, install the library, `libiclaustron`, with its header and a
pkg-config file, then build with pkg-config:

```sh
cargo xtask install /usr/local        # or any prefix
cc -o prog prog.c $(pkg-config --cflags --libs iclaustron)
```

```c
#include <iclaustron/ic_apid.h>
```

## 2. Connecting

One call starts the library: it asks a management server for a node id
and the cluster's configuration, starts the threads that keep the API
node in the cluster, and returns. The data nodes are connected in the
background; wait for them before the first transaction.

```rust
let global = ApidGlobal::connect("localhost:1186", Some("my_app"),
                                 &GlobalOptions::default())?;
if global.wait_for_all_started(30_000) == 0 {
  // no data node is up
}
let mut conn = global.create_connection()?;
```

```c
IC_GLOBAL_OPTIONS options;
IC_APID_GLOBAL *global;
IC_APID_CONNECTION *conn;
uint32_t started;
int32_t ret;            /* 0, or the error code, from every call */

ic_global_options_init(&options, sizeof options);
ret = ic_apid_global_connect("localhost:1186", "my_app", &options, &global);
ret = ic_apid_global_wait_for_started(global, 30000, 1, &started);
ret = ic_apid_global_create_connection(global, &conn);
```

**The connectstring** is a list of management servers, `host[:port]`
separated by commas, tried in turn; `nodeid=N` among them asks for that
node id rather than one the management server chooses, and
`bind-address=host` binds every socket to that local address first.
For example `mgm1:1186,mgm2:1186,nodeid=70`. The name the program gives
is what the management server shows for its node.

**Options**, fixed for the life of the global:

| Option | Default | Meaning |
|---|---|---|
| `receive_threads` | 1 | threads reading the data nodes' links; more when many threads send much |
| `reply_wait_ms` | 0: from the configuration | how long a transaction waits for its replies before it ends with 4012; at least two minutes |
| `mgm_timeout_ms` | 30 000 | how long a management server has to answer |

**One global per cluster.** A program that talks to several clusters
starts a global for each. Stopping the global (dropping it, or
`ic_apid_global_free`) stops its threads; free its connections first.

**One connection per thread.** A connection is one thread's view of the
cluster: its queries, its transactions, its replies. A thread makes its
own with `create_connection`; it may be handed to another thread, but
only one uses it at a time. A process has at most 4 711 connections at
once, as many as RonDB lets an API node have
(`IC_ERROR_TOO_MANY_USER_THREADS`, 7139).

## 3. Tables, indexes and records

**Binding.** `table_bind(db, table)` fetches a table's description from
the cluster, once for every connection of the global; `index_bind(db,
index, table)` a unique index's. A MySQL server names the hash index of
`UNIQUE KEY k` `k$unique`. When a data node says a table has changed
since it was bound (errors 241, 284), the library lets the description
go, and the next bind fetches it again: bind again and retry.

**A record** says how a row is laid out in the program's memory: for
each field, where its value is and where its null bit is. The values
are kept as the data nodes keep them, so nothing is converted on the
way in or out. In C a record is usually over a struct:

```c
struct t1_row {
  uint32_t id;        /* INT UNSIGNED NOT NULL, the primary key */
  int32_t  val;       /* INT, nullable: null bit 0 */
  uint32_t code;      /* INT UNSIGNED NOT NULL, the unique key */
  uint8_t  name_len;  /* VARCHAR(32) latin1, nullable: null bit 1 */
  char     name[32];
  uint8_t  null_bits;
};

IC_FIELD_SPEC spec[4] = {
  { id_of_id,   offsetof(struct t1_row, id),       IC_NO_NULL_BIT, 0 },
  { id_of_val,  offsetof(struct t1_row, val),
                offsetof(struct t1_row, null_bits), 0 },
  { id_of_code, offsetof(struct t1_row, code),     IC_NO_NULL_BIT, 0 },
  { id_of_name, offsetof(struct t1_row, name_len),
                offsetof(struct t1_row, null_bits), 1 },
};
ret = ic_record_create(table, spec, 4, sizeof(struct t1_row), &rec);
```

(the field ids from `ic_table_def_field_id`). A program that would
rather not write the struct asks the library to lay the row out,
`Record::default_for(&table, None)` or `ic_record_create_default(table,
NULL, 0, &rec)`, and finds each field with `ic_record_field` (offset
and size). The examples' `describe` prints both for a table.

How values are kept:

| Type | In the row |
|---|---|
| TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT, signed or not | 1, 2, 3, 4, 8 bytes, little-endian |
| FLOAT, DOUBLE | 4, 8 bytes, IEEE, little-endian |
| CHAR(n), BINARY(n) | the field's bytes; CHAR padded with the character set's spaces, BINARY with zeros |
| VARCHAR, VARBINARY up to 255 bytes | one length byte, then the bytes |
| longer VARCHAR, VARBINARY | two length bytes, little-endian, then the bytes |
| DECIMAL | MySQL's packed binary decimal |
| DATE, TIME, DATETIME, TIMESTAMP, YEAR, and their fractional forms | as the data nodes keep them (`doc/rust/05-ndb-protocol.md` §8.1) |
| BIT(n) | n bits, in 32-bit words |

The field's size in the row is the longest value with its length bytes:
`VARCHAR(32)` in latin1 is 33 bytes, in utf8mb4 129. A null bit is set
for NULL; a field that is NOT NULL has none.

**Key records.** A query has two records: one for its key and one for
the values it reads or writes. The key record holds every primary key
field, or for a query through a unique index every indexed field. One
record covering every field serves as both.

**Blob columns** (BLOB, TEXT, JSON, GEOMETRY) are not read or written in
0.1: a table that has one can be bound and used, but records leave the
column out (a default record does so by itself), and deleting a row of
such a table is refused (`IC_ERROR_DELETE_WITH_BLOBS`, 7153), since
its blob's parts would be left behind.

## 4. Queries and transactions

### Queries

A query is made once on a connection and used again and again:
`create_query(table, key_rec, attr_rec)`, or `create_unique_query(index,
key_rec, attr_rec)` through a unique index. **It owns its rows**,
sized by its records. For each use the program fills the key row (and
for a write the attribute row), defines the query on a transaction, and
once it has completed reads the attribute row. Between being defined
and completing, the rows are the library's.

```rust
let q = conn.create_query(&table, &rec, &rec)?;
conn.query_mut(q).expect("just made").key_row_mut().copy_from_slice(&key);
```

```c
IC_QUERY_ID q;
uint8_t *key;
size_t len;

ret = ic_apid_query_create(conn, table, rec, rec, &q);
key = ic_apid_query_key_row(conn, q, &len);   /* valid until freed */
memcpy(key, &row, sizeof row);
```

A query is idle, defined, sent or completed. Once completed it is idle
again when it is taken (below), and can be defined anew; `free_query`
lets it go. In C a query and a transaction are named by a `uint32_t`;
one freed or closed is refused if used again, never followed.

**Reads** (`read_key`), by kind:

| Kind | Lock |
|---|---|
| `Simple` | a shared lock, let go of when the read is done |
| `Locked` | a shared lock, held until the transaction ends |
| `Exclusive` | an exclusive lock, held until the transaction ends |
| `Committed` | none: the last committed value |
| `ExclusiveNoWait` | as `Exclusive`, failing at once rather than waiting for another's lock |

Through a unique index the data nodes take a shared lock for a simple
or committed read too, held until the transaction ends.

**Writes** (`write_key`): `Insert` (the row must not be there), `Update`
(it must), `Write` (insert, or overwrite what is there), `Delete`.
Through a unique index there is no insert or write, and an update may
not set an indexed column: the row is found by it
(`IC_ERROR_INDEXED_COLUMN_UPDATE`, 7155). Every row size the table
allows can be written: a request too large for one message goes in
several.

**On error**: by default a failed write rolls its transaction back and
a failed read only fails itself; `AbortOnError` and `IgnoreError` say
otherwise (a committed read always ignores). **Batching hint**: `Safe`
or `Unsafe` tells the data node whether a write depends on those before
it in its batch.

### Transactions

A transaction is started on a connection, given queries, and asked to
commit or roll back:

```rust
let hint = conn.hint_for_key(&table, &rec, &key);
let t = conn.start_transaction(hint, Some(&table))?;
conn.write_key(q, t, &WriteKeyArgs { kind: WriteKind::Insert,
                                     ..WriteKeyArgs::default() })?;
conn.commit_transaction(t)?;
// send and poll until conn.transaction(t) is done, then
conn.close_transaction(t)?;
```

**The hint** chooses the data node that coordinates the transaction.
`hint_for_key` hashes the key as the data nodes do, character columns
in their collation included, and chooses the node that holds the row,
so the transaction's first round trip stays on one node; for a
transaction that starts through a unique index, `hint_for_unique_key`,
with the index's own table (`start_transaction_on_index` in C). `Any`
takes the next started node in turn, `Node(id)` that node. A hint is
only a choice of node: a transaction may touch any row.

Starting is local: the data node hears of the transaction with its
first query. A commit or rollback goes with the next send, on the last
query not yet sent or alone. **A transaction is done** when it has
ended and every query in it has completed; it then says how it ended
(`Committed` or `RolledBack`), with what error, and for a commit the
global checkpoint it belongs to. `close_transaction` lets it go.

## 5. Two ways to work

### Synchronously

The synchronous calls do one query as a transaction of its own, send
it, and wait: `key_op::read_committed`, `read_unique_committed` and
`write_key` in Rust, `ic_apid_conn_read_committed`,
`ic_apid_conn_read_unique_committed` and `ic_apid_conn_write` in C. They
take the program's own rows, and suit a thread that does one thing at
a time; a connection per thread is the way to scale them.

```c
struct t1_row key, row;
bool found;

ret = ic_apid_conn_read_committed(conn, rec, (const uint8_t *)&key,
                                  sizeof key, rec, (uint8_t *)&row,
                                  sizeof row, &found);
```

### Asynchronously

A thread defines queries on transactions, as many as it likes, sends
them all at once, and polls for the replies:

- `send_queries(force)` (`ic_apid_conn_send`) sends everything defined
  and every commit and rollback asked for, one write per data node.
- `poll(ms)` (`ic_apid_conn_poll`) waits up to `ms` for replies and
  completes the queries they finish.
- `flush(ms, force)` is both.

A completed query goes to the connection's executed list, taken with
`get_next_executed_query`, or, if it was defined with a callback, has
its callback called (section 6). A program can keep many transactions
in flight on one connection, and that is where the throughput is: the
`pipeline` example keeps 32 single-row transactions in flight.

**`force`**: without it, a send may be held back for a moment to go
with other threads' signals on the same link, never longer than the
adaptive send allows; a thread that goes on to poll loses nothing by
leaving it off. With it, the send happens now.

## 6. Callbacks

A query defined with a callback has it called when it completes:
**on the thread that polls, from inside the poll**, with the connection,
the query and the `user_ref` it was defined with.

```c
static void on_written(IC_APID_CONNECTION *conn, IC_QUERY_ID query,
                       void *user_ref)
{
  struct slot *s = user_ref;
  int32_t err = ic_apid_query_error(conn, query);
  ...
}

args.callback = on_written;
args.user_ref = slot;
ret = ic_apid_conn_write_key(conn, query, trans, &args);
```

Inside the callback the query is complete, with its error or its row,
and its transaction's state is settled as far as this query decides it:
a single-query transaction that committed is done, and can be closed
there. The callback may read the query's rows, define queries, start,
commit, roll back and close transactions, and send; a poll or flush
inside it does nothing. When it returns the query is idle again, unless
the callback defined it anew, which is how a pipeline keeps itself full.
It must not free the connection.

In Rust a callback is a plain function, `fn(&mut ApidConnection,
QueryId, usize)`, with `user_ref` its only context; state it shares with
the rest of the program lives where a plain function can reach it, as
in the `pipeline` example's statics.

## 7. Errors

### What an error says

An error is a code. 7000 and up are iClaustron's own; small positive
codes are the operating system's; the rest are NDB's, as the data nodes
report them. Each has:

- a **category**: `User` (the request was wrong: no such row, a
  duplicate key), `Internal`, or `External` (a node failure, an
  overload, a timeout, a lost link);
- a **severity**: `Temporary` (the same request may well succeed if
  tried again), `Error` (it will not; something must change), `Stop`
  (the library can no longer be used);
- a **text**: `message()`, `ic_error_message`.

Some refusals say more than their code: `error_data()` on the query or
the transaction (`ic_apid_query_error_data`). For 893, a duplicate in a
unique index, it is the id of the index.

In C every call that can fail returns 0 or the code. In Rust it returns
`Result<_, IcError>`. A query's own error, and a transaction's, are
asked for once it has completed.

### Errors to know

| Code | What happened | Kind |
|---|---|---|
| 626 | no row with that key | user |
| 630 | a row with that key is there already | user |
| 893 | the row would break a unique index; the error data names it | user |
| 241, 284 | the table or index changed or went since it was bound | user: bind again |
| 266 | a lock wait timed out, probably a deadlock | temporary |
| 233, 4006, 899 and others | a node is out of a resource for the moment | temporary |
| 4010, 4025, 4028, 4029, 4031 | a node failure aborted the transaction | temporary |
| 4119 | a node failure lost the reply to a committed read | temporary |
| 4012 | the replies stopped coming; the outcome is unknown | temporary, see below |
| 7135 | the link to a data node was lost; the node may be up | temporary |
| 7142 | no data node is started | temporary |
| 7139 | too many connections in this process | error |
| 7153, 7155 | a delete on a table with blobs; an indexed column updated through the index | error |
| 7154 | the index is not online | error |
| 7077, 7156 | the row or the key is larger than RonDB allows | error |
| 7129 | a panic inside the library; the connection is unusable | stop |

### When to try again

**A temporary error ends the transaction.** Trying again means a new
transaction from the start, every query in it again, preferably after a
short pause that grows with each attempt; the `retry` example does
this. Locks and deadlocks, overloads and node failures all clear up
with time.

**An unknown outcome needs care.** 4012, and the other errors of NDB's
unknown-result class (4008, 4009, 4035), mean the library stopped
hearing about the transaction: it may have committed, or not. They
count as temporary, but before a write is tried again the program has
to know which it was, for instance by reading the row; or the write must
be one that does no harm done twice, such as `Write` with the same
values. A read may simply be tried again.

**A user error does not clear up by itself**: the request has to
change, or the condition is the answer (626 means there is no row).

## 8. Node failure

The library keeps the API node in the cluster on its own: it exchanges
heartbeats with every data node, notices a link that goes quiet,
reconnects with backoff, and after losing every link asks the
management server for its node id again. A program does not restart
anything.

When a data node fails, what the program sees depends on what it had
there:

- **A transaction whose coordinator failed** waits while another data
  node takes over. The new coordinator finishes what can be finished:
  a transaction that had committed is reported committed, one it
  aborts ends with a node-failure error (4010, 4031), temporary. A
  rollback that was asked for ends rolled back.
- **A link lost with no failure reported** ends the transactions that
  used it at once, their outcome unknown to us; the data node aborts
  what an API node it lost had open.
- **A read** whose replica failed ends with 4119 or a node-failure
  error, and can be tried again.
- **New transactions** go to the nodes still started; a hint that names
  a failed node's partition chooses a replica that is up.

In short: retry temporary errors, and treat 4012 as above. A
transaction that goes to a cluster with no started node fails with
7142 until one is back.

## 9. Performance

- **Batch.** One send per data node carries every query defined since
  the last; many queries per transaction, and many transactions in
  flight, cost few round trips.
- **Keep queries.** Make a query once and define it again for each use:
  its rows and buffers are made with it and used again.
- **Hint.** A transaction started by its key's hint runs its first
  query on the node that holds the row.
- **Use committed reads** where no lock is needed: they take none.
- **Threads.** A connection per thread scales with cores; more receive
  threads help when many threads together read a lot.
- **Measure release builds.** The library's checks in a debug build
  cost time a release build does not spend.

## 10. What 0.1 does not do

- No scans: rows are read and written by primary key and unique key
  only. Scans, with conditions run on the data nodes, are next.
- No blob columns (BLOB, TEXT, JSON, GEOMETRY): records leave them out,
  and deletes on their tables are refused.
- No schema changes: tables and indexes are made through a MySQL
  server.
- No TLS between the API node and the cluster.
- At most 4 711 connections per process, RonDB's limit for an API
  node.
- Keys in three collations whose keys cannot be placed from their
  weights, `latin2_czech_cs`, `cp1250_czech_cs` and `tis620_thai_ci`,
  are not hinted: such a transaction goes to the next node in turn,
  which works and costs a hop.
- No events, no AUTO_INCREMENT from the API, no tables without a
  primary key.
- Linux on x86_64 and aarch64 and macOS on aarch64; no Windows.
