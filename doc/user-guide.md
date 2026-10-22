# iClaustron user guide

iClaustron is a Data API for RonDB: a library an application links to
read and write rows of a RonDB cluster directly, as an API node: by
primary key and by unique key, in transactions, and by scans of tables
and of ordered indexes, with conditions and updates that run on the
data nodes. It is written in Rust and has a C API; this guide covers
both, against RonDB 26.10. Release 0.1 had the key operations; the
guide also covers what came after it, on the way to 0.2 (executors and
scans, section 5) and 0.3 (programs, section 6; AUTO_INCREMENT and
tables without a primary key, section 7).

The examples in `examples/` are the same eleven programs in Rust and in
C, and the guide quotes them. The C API's reference is its header,
`include/iclaustron/ic_apid.h`; the Rust API's is `cargo doc`; the
design behind both is `doc/rust/04-api-design.md`.

## 1. What you need

- **A RonDB 26.10 cluster** with a free API slot for each program that
  connects: an `[api]` section in the cluster's configuration that
  names no node id, or one that names the id the program asks for.
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

and name each item from its module, as the examples do:

```rust
use ic_apid::apid_global::ApidGlobal;
use ic_apid::apid_global::GlobalOptions;
use ic_port::IcError;
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
| `reply_wait_ms` | 0: six minutes | how long a transaction waits for its replies before it ends with 4012; less to give up sooner, never more than six minutes, after which a data node will never answer |
| `mgm_timeout_ms` | 30 000 | how long a management server has to answer |
| `auto_increment_prefetch` | 4 096 | AUTO_INCREMENT values a refill reserves for a table (section 7) |
| `auto_increment_step`, `auto_increment_offset` | 1, 1 | as MySQL's `auto_increment_increment` and `auto_increment_offset` (section 7) |

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
way in or out. In Rust a row is a slice of bytes, and the record says
where each field lies in it; here the layout of the C struct below,
field by field:

```rust
// id at 0; val at 4, null bit 0; code at 8; name at 12, its length
// byte and then 32 bytes, null bit 1; the null bits in byte 45
let spec = [
  FieldSpec::not_null(id_of_id, 0),
  FieldSpec::nullable(id_of_val, 4, 45, 0),
  FieldSpec::not_null(id_of_code, 8),
  FieldSpec::nullable(id_of_name, 12, 45, 1),
];
let rec = Record::new(&table, &spec, 48)?;
```

In C a record is usually over a struct:

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

(the field ids from `table.field_id(name)`, `ic_table_def_field_id`
in C). A program that would
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

**Tables without a primary key** have a last column of their own,
`$PK`, BIGINT UNSIGNED, which MySQL adds as the table's key and fills
from the table's AUTO_INCREMENT counter. iClaustron shows it as the
ordinary column it is: records hold it, key operations go by it, a
scan reads it. `table.hidden_key()` names it, and its `IC_FIELD_INFO`
says `hidden_key`, for tools that list columns and would leave it out.
An insert takes its value as section 7 says.

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

```c
IC_TRANSACTION_HINT hint;
IC_WRITE_KEY_ARGS args;
IC_TRANS_ID t;

hint = ic_apid_conn_hint_for_key(conn, table, rec, key, len);
ret = ic_apid_conn_start_transaction(conn, hint, table, &t);
ic_write_key_args_init(&args, sizeof args);
args.kind = IC_WRITE_KIND_INSERT;
ret = ic_apid_conn_write_key(conn, q, t, &args);
ret = ic_apid_conn_commit_transaction(conn, t);
while (!ic_apid_transaction_is_done(conn, t))  /* send and poll */
  ret = ic_apid_conn_flush(conn, 100, true, NULL);
ret = ic_apid_conn_close_transaction(conn, t);
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

## 5. Three ways to work

### Synchronously

The synchronous calls do one query as a transaction of its own, send
it, and wait: `key_op::read_committed`, `read_unique_committed` and
`write_key` in Rust, `ic_apid_conn_read_committed`,
`ic_apid_conn_read_unique_committed` and `ic_apid_conn_write` in C. They
take the program's own rows, and suit a thread that does one thing at
a time; a connection per thread is the way to scale them.

```rust
let mut row = vec![0u8; rec.row_size() as usize];
let found = key_op::read_committed(&mut conn, &rec, &key, &rec, &mut row)?;
```

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
its callback called (section 8). A program can keep many transactions
in flight on one connection, and that is where the throughput is: the
`pipeline` example keeps 32 single-row transactions in flight.

**`force`**: without it, a send may be held back for a moment to go
with other threads' signals on the same link, never longer than the
adaptive send allows; a thread that goes on to poll loses nothing by
leaving it off. With it, the send happens now.

### Through an executor

A program whose work arrives on threads of its own, such as a server
with a thread per request, cannot give each its own connection: there
are at most 4 711, and a thread that sends one call at a time pays a
whole round's costs for each. An **executor** is a thread of the
library's, with a connection, that runs the requests of any number of
threads, and sends together whatever it admits at once.

- Start one with `Executor::start(&global, &options)`
  (`ic_apid_executor_start`), and give each thread its own submitter:
  `executor.submitter()`, or a clone of one
  (`ic_apid_executor_submitter`).
- Bind tables through the submitter (`table_bind`,
  `ic_apid_submitter_table_bind`), and describe each kind of operation
  once as a shape: a table, or a unique index of it, and its records
  (`OpShape::new`, `ic_op_shape_create`).
- A request is a transaction of operations of those shapes, with their
  rows (`Request`, `ic_request_create`, `ic_request_add_read`,
  `ic_request_add_write`, and `ic_request_key_row` to fill a key).
  `execute` (`ic_apid_submitter_execute`) runs it and blocks until it
  has ended: the rows read are in its operations, each operation says
  how it went, and the call returns 0 if the transaction committed.
  Keep the request and run it again with new keys.

```rust
let mut req = Request::with_op(KeyOp::read(&shape, ReadKind::Committed));
req.ops_mut()[0].key_row_mut().copy_from_slice(&key);
sub.execute(&mut req)?;
match req.ops()[0].error() {
  Some(e) if e.code as u32 == key_op::IC_NDB_ERROR_NO_SUCH_ROW => {
    // no such row
  }
  Some(e) => return Err(e),
  None => row.copy_from_slice(req.ops()[0].attr_row()),
}
```

```c
IC_REQUEST *req;
uint32_t op;
size_t len;

ic_request_create(&req);
ic_request_add_read(req, shape, IC_READ_KIND_COMMITTED, &op);
memcpy(ic_request_key_row(req, op, &len), &key, sizeof key);
if ((ret = ic_apid_submitter_execute(sub, req)) != 0)
  goto error;
if (ic_request_op_error(req, op) == IC_NDB_ERROR_NO_SUCH_ROW)
  /* no such row */;
memcpy(&row, ic_request_attr_row(req, op, &len), sizeof row);
```

A full executor makes `execute` wait for room; a stopped one refuses
with `IC_ERROR_EXECUTOR_STOPPED` and leaves the request as it was. A
request may carry a deadline (`set_timeout_ms`,
`ic_request_set_timeout_ms`): one that waited past it comes back with
`IC_ERROR_TIMEOUT`, unsent. In Rust a request can also be handed to a
function on the executor's thread when it ends, rather than waited for
(`Submitter::submit` with `Done::Callback`).

Measured on a Mac with two data nodes, 64 threads calling through one
executor read 268 000 rows a second, 36 % more than 64 threads with a
connection each, at half the CPU a call. Both are far from what one
thread does asynchronously, two million a second: a blocking call
costs a thread's sleep and wake-up, about 10 µs of CPU, however it
reaches the cluster.

### Scans

A scan reads a whole table, one partition of it, or ranges of an
ordered index, in batches: one receiver per fragment scanned at once,
each bringing a batch of rows at a time. A receiver is asked for its
next batch only once the program has handed back the last, so the
program sets the pace, and a locking scan's rows stay locked until
their batch is handed back.

- Make a scan once over a table and a record (`create_scan`,
  `ic_apid_scan_create`), and start a run of it as often as you like
  (`start_scan`, `ic_apid_conn_start_scan`), with its arguments
  (`ScanArgs`, `ic_scan_args_init`): the lock (committed, shared or
  exclusive), the batch size, one partition, a deadline. A program
  run on the data nodes (section 6) can pass over the rows the
  application does not want, so that only the others travel.
- **Row by row**: `next_row` (`ic_apid_conn_next_row`) reads the next
  row into the scan's row, handing each batch back as it goes and
  polling while it waits; it says false at the end, or gives the
  scan's error. Other queries and scans of the connection are served
  meanwhile.
- **A batch at a time**: complete batches go on the ready list
  (`get_next_scan_batch`), or to the scan's callback; read a batch's
  rows with `next_scan_row` and hand it back with `release_scan_batch`.
- **Through an executor**: a thread with no connection describes the
  scan as a shape (`ScanShape::new`, `ic_scan_shape_create`), makes a
  reader of it (`ScanReader::new`, `ic_scan_reader_create`), runs it
  with `submitter.scan` (`ic_apid_submitter_scan`) and reads the rows
  with the reader's `next_row` (`ic_scan_reader_next_row`). A slow
  reader stalls only its own scan. A committed read may set
  `read_ahead`, so that a receiver fetches its next batch while the
  reader reads the last, at the cost of twice the memory.

```rust
conn.start_scan(scan, &ScanArgs::default())?;
while conn.next_row(scan)? {
  let row = conn.scan_row(scan);
  // ... use the row ...
}
```

```c
IC_SCAN_ARGS args;
bool more;
size_t len;

ic_scan_args_init(&args, sizeof args);
if ((ret = ic_apid_conn_start_scan(conn, scan, &args)) != 0)
  goto error;
while ((ret = ic_apid_conn_next_row(conn, scan, &more)) == 0 && more)
{
  const uint8_t *row = ic_apid_scan_row(conn, scan, &len);
  /* ... use the row ... */
}
if (ret != 0)
  goto error;
```

**Index scans.** A scan of an ordered index reads the rows whose
indexed columns lie in the ranges given. Make it once with the index, a
key record holding the index's columns, and the record the rows are
read into (`create_index_scan`, `ic_apid_index_scan_create`), and add
each run's ranges before starting it (`add_scan_range`,
`ic_apid_conn_add_scan_range`). A range is a low and a high row, laid
out by the key record, and a `ScanRange` (`IC_SCAN_RANGE`, filled by
`ic_scan_range_init`) that says how many of the index's leading
columns each bound takes from its row (0 for no bound on that side),
whether each bound is in the range, and the range's number, 0 to
4 095. Bounds compare as the index orders its rows, column by column,
and a NULL in a bound comes before every value. A run given no range
reads the whole index.

- **In order**: `ordered` in the arguments merges the fragments' rows
  into the index's order, `descending` from the high end. Character
  columns compare in their collation's order, as the data nodes
  describe it. An ordered run is read row by row.
- **Several ranges**: `scan_range_no` (`ic_apid_scan_range_no`) gives
  the number of the range the row last read is in. An ordered run
  gives its rows range by range, so the numbers must rise in the order
  the ranges were added (`IC_ERROR_BAD_RANGE`, 7163, as for a range
  that cannot be sent).
- **One partition**: a run whose every range fixes the table's
  distribution key reads that partition alone. On a table made with
  `COMMENT="NDB_TABLE=PARTITION_HASH=base:detail:fanout"`, ranges
  that fix only the first `base` columns of the primary key, the same
  in every range, read the fanout's partitions for that base alone.
- **Through an executor**: a shape over the index
  (`ScanShape::new_index`, `ic_scan_shape_create_index`), and the
  ranges added to the reader (`add_range`, `ic_scan_reader_add_range`;
  `range_no`, `ic_scan_reader_range_no`).

The `range_scan` example reads ranges of an index in order, from either
end; `scan_executor` does so from four threads through an executor.

**Updating and deleting rows a scan finds.** Start a locking scan on a
transaction (`ScanArgs::trans`, `IC_SCAN_ARGS`' `trans`), and act on
the row last read: `lock_scan_row`, `update_scan_row` or
`delete_scan_row` (`ic_apid_conn_lock_scan_row` and so on) define a
query on the transaction that takes the row's lock over and locks,
updates or deletes it. The query is any query over the scanned table:
an update takes its values from the query's attribute row, and its key
from the scan. A batch handed back asks for more only once its rows'
takeovers have completed, so the locks never go before the changes are
in. Commit the transaction once the scan is done.

Close a run early with `close_scan` (`ic_apid_conn_close_scan`), or a
reader's with `close` (`ic_scan_reader_close`). The run stops at once,
without waiting for the batches on their way, whose rows are dropped;
a locking scan on a transaction that goes on first waits for its
takeovers to be confirmed, so that each holds its lock. A batch kept
too long ends the scan with 296; a lock waited for too long with 274
or 266; the failure of the coordinator's node with 4028. All are
temporary: start the scan again.

## 6. Programs: conditions and updates on the data nodes

A program, in this section and in the API's names (`Program`,
`IC_PROGRAM`), is not the application but a piece of code it builds,
which runs in the data node on the row an operation holds there:

- a **scan's filter**, which passes over the rows it does not want, so
  that only the others travel;
- a **key operation's condition**, which refuses the operation when the
  row does not meet it, such as an update that only happens while a
  column still has the value the application read;
- an **update's assignments**, worked out from the row, such as adding
  to a counter without reading it first.

**Building one.** A builder over a table makes values and conditions
bottom-up, each giving an id, and compiles them into a program:

- values: a column, `column(id)`; a constant, `constant(&rec, row, id)`,
  the value column `id` has in a row laid out by `rec`; an integer,
  `integer(n)`; an input, `input(n)`; and `add(a, b)` and
  `subtract(a, b)`;
- conditions: `compare(cmp, a, b)` with `Cmp::Eq`, `Ne`, `Lt`, `Le`,
  `Gt` and `Ge` (`IC_CMP_EQ` and so on), `is_null(a)` and
  `is_not_null(a)`, joined by `and(x, y)`, `or(x, y)` and `not(x)`;
- assignments, for an update: `assign(column, value)`, and
  `assign_if(column, value, cond)` for one that happens only where its
  own condition holds;
- then `finish(Some(top))`, or `finish(None)` for a program that only
  assigns (`ic_program_builder_finish`,
  `ic_program_builder_finish_no_condition`).

```rust
// val > input 0 AND name IS NOT NULL, as a scan's filter
let mut b = ProgramBuilder::new(&table);
let val = b.column(val_id);
let limit = b.input(0);
let above = b.compare(Cmp::Gt, val, limit);
let name = b.column(name_id);
let named = b.is_not_null(name);
let both = b.and(above, named);
let filter = b.finish(Some(both))?;
conn.set_scan_program(scan, Some(&filter))?;
conn.set_scan_input(scan, 0, 100)?;
```

```c
IC_PROGRAM_BUILDER *b;
IC_PROGRAM *filter;
IC_PROGRAM_COND above, named;

ret = ic_program_builder_create(table, &b);
above = ic_program_compare(b, IC_CMP_GT, ic_program_column(b, val_id),
                           ic_program_input(b, 0));
named = ic_program_is_not_null(b, ic_program_column(b, name_id));
ret = ic_program_builder_finish(b, ic_program_and(b, above, named),
                                &filter);
ic_program_builder_free(b);
ret = ic_apid_conn_set_scan_program(conn, scan, filter);
ret = ic_apid_conn_set_scan_input(conn, scan, 0, 100);
```

In C a value or condition the builder could not make, from a null
handle or an unknown comparison, is `IC_PROGRAM_NONE`, which the finish
refuses with `IC_ERROR_BAD_PROGRAM` (7165), so the steps need no check
each.

**Made once, run many times.** A program is shared by any number of
operations; only its inputs change from one to the next, up to 16
integers (`IC_PROGRAM_INPUTS`), compared with an integer column or added
to one. Each query, operation or scan holds its own 16, 0 until set and
kept until changed. A constant from a row stays in the program, so a
value that changes between operations belongs in an input.

**Where a program goes.**

- On a connection a query or a scan keeps it until it is changed:
  `query.set_program(Some(&p))` and `conn.set_scan_program(scan,
  Some(&p))` (`ic_apid_query_set_program`,
  `ic_apid_conn_set_scan_program`), `None` or NULL to take it off; the
  inputs with `set_input` and `set_scan_input`
  (`ic_apid_query_set_input`, `ic_apid_conn_set_scan_input`).
- Through an executor a shape takes it: `shape.with_program(&p)`
  (`ic_op_shape_with_program`, `ic_scan_shape_with_program`) makes a
  shape of the same records that runs it. The inputs are set on each
  operation (`KeyOp::set_input`, `ic_request_set_input`) or on the
  reader (`ScanReader::set_input`, `ic_scan_reader_set_input`).
- A program goes on key reads, updates and deletes and on scans, and
  one that assigns only on an update. A program on an insert or a
  write, through a unique index, or on a scanned row taken over is
  refused when the operation is defined (`IC_ERROR_NOT_SUPPORTED`,
  7127).

**When the condition does not hold**, a scan passes the row over. A key
operation is refused: with 6000 where it aborts on error, which rolls
its transaction back, and with 626, as if there were no row, where it
ignores errors. A comparison with a NULL is unknown, as in SQL, and only
a condition that is true lets the row through. A conditional update is
a program with a condition only, the new values in the attribute row
as for any update.

**Reading after an update.** An update can read columns back after its
program has run, into a row of their own laid out by a record of their
own: `query.set_reads_after(Some(&rec))` and `query.after_row()`
(`ic_apid_query_set_reads_after`, `ic_apid_query_after_row`); through
an executor, `shape.reading_after(&rec)` (`ic_op_shape_reading_after`)
and the operation's `after_row` (`ic_request_after_row`). A counter is
then one round trip: a program that assigns the column plus input 0,
on an update that reads the column back, the row locked only inside
the data node.

**What the finish refuses**: a comparison or assignment of values of
different types (`IC_ERROR_NOT_COMPARABLE`, 7164), since a constant
must have its column's type, length, precision, scale and collation;
more than eight values live at once (`IC_ERROR_CONDITION_TOO_COMPLEX`,
7130); a column the table does not have, or an input beyond 15. A
program is for the table its builder was made over, and is refused on
another's operations (`IC_ERROR_BAD_PROGRAM`).

The `conditional_update` example runs an optimistic update, once
committed and once refused with 6000, a counter read back after its
update, and a scan with a filter.

## 7. AUTO_INCREMENT and tables without a primary key

**Values are asked for, not filled in.** An insert writes the row as
given. For a table's AUTO_INCREMENT column the application asks for
the next value and puts it in its rows:

```rust
let id = conn.auto_increment(&table)?;
// put id in the key row and the attribute row, then insert as usual
```

```c
uint64_t id;

ret = ic_apid_conn_auto_increment(conn, table, &id);
```

- **Unique across the cluster.** The values come from the table's
  counter in the data nodes, the one MySQL servers take theirs from, so
  no client hands out a value another has, MySQL servers included. They
  are not in order across clients, and values not used are gaps, as
  with MySQL's own.
- **A range per table for the process.** A refill reserves
  `auto_increment_prefetch` values at once, 4 096 by default, in one
  round trip, and the values after it need none. A table may have a
  size of its own (`table.set_auto_increment_prefetch(n)`,
  `ic_table_def_set_auto_increment_prefetch`). Every connection and
  executor of the process takes from the table's one range. The refill
  goes on a connection the library keeps for the counters, so the call
  never sends the application's queries and never runs a callback.
- **Many rows at once**: `auto_increment_block(&table, n)`
  (`ic_apid_conn_auto_increment_block`) gives the first of `n` values a
  step apart, reserved together.
- **A value of the application's own**: after inserting a row with an
  explicit value, `auto_increment_seen(&table, value)`
  (`ic_apid_conn_auto_increment_seen`) moves the counter past it, only
  ever up, so that no client hands it out. MySQL servers do the same
  after theirs. Another client's reserved range is not told, with MySQL
  as with iClaustron, so an explicit value inside one may later be
  handed out again by that client: keep explicit values above the
  counter.
- **The counter as it stands**: `auto_increment_peek(&table)`
  (`ic_apid_conn_auto_increment_peek`), the next value no client has
  reserved; only advisory, out of date once any
  client refills.
- **Step and offset** for the process (`auto_increment_step`,
  `auto_increment_offset` in the global's options), as MySQL's
  `auto_increment_increment` and `auto_increment_offset`: for clusters
  in a replication ring, set as the cluster's MySQL servers are.
- **Through an executor** the submitter has the same calls
  (`ic_apid_submitter_auto_increment` and so on), which may be made
  from a callback on the executor's thread too.
- **Errors.** A temporary error on the counter is tried again inside
  the call, for up to 3.6 s, and then returned. A table without such a
  column is refused (`IC_ERROR_NO_AUTO_INCREMENT`, 7166). The value is
  not checked against the column's type: an AUTO_INCREMENT column wants
  64 bits.

**Tables without a primary key.** The hidden key `$PK` (section 3) is
the table's AUTO_INCREMENT column: an insert takes its value with
`auto_increment`, always one apart whatever the step, and puts it in
its key row. A scan returns it with each row, so a row a scan finds
can be read, updated or deleted by it afterwards, or taken over at
once (section 5).

The `auto_increment` example takes values one at a time and as a
block, inserts an id of its own, and inserts, scans and deletes rows of
a table without a primary key.

## 8. Callbacks

A query defined with a callback has it called when it completes:
**on the thread that polls, from inside the poll**, with the connection,
the query and the `user_ref` it was defined with.

```rust
fn on_written(conn: &mut ApidConnection, query: QueryId, user_ref: usize) {
  let error = conn.query(query).and_then(|q| q.error());
  // ...
}

let args = WriteKeyArgs {
  kind: WriteKind::Write,
  callback: Some(on_written),
  user_ref: slot,
  ..WriteKeyArgs::default()
};
conn.write_key(query, trans, &args)?;
```

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

## 9. Errors

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
| 626 | no row with that key; or a program's condition did not hold, on an operation that ignores errors | user |
| 630 | a row with that key is there already | user |
| 893 | the row would break a unique index; the error data names it | user |
| 6000 | a program's condition did not hold, on an operation that aborts on error | user |
| 241, 284 | the table or index changed or went since it was bound | user: bind again |
| 266 | a lock wait timed out, probably a deadlock | temporary |
| 274, 296 | a scan waited too long for a lock; a scan's batch was kept too long | temporary |
| 233, 4006, 899 and others | a node is out of a resource for the moment | temporary |
| 4010, 4025, 4028, 4029, 4031 | a node failure aborted the transaction | temporary |
| 4119 | a node failure lost the reply to a committed read | temporary |
| 4012 | the replies stopped coming; the outcome is unknown | temporary, see below |
| 7135 | the link to a data node was lost; the node may be up | temporary |
| 7142 | no data node is started | temporary |
| 7159 | an executor's list of requests is full (`submit` only) | temporary |
| 7160 | the executor has stopped | error |
| 7127 | something the library does not do (yet), such as a program on an insert | error |
| 7130, 7164, 7165 | a program needs more than eight values at once; compares different types; names what it does not have | error |
| 7163 | a range of an index scan that cannot be sent | error |
| 7166 | the table has no AUTO_INCREMENT column | error |
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

## 10. Node failure

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

## 11. Performance

- **Batch.** One send per data node carries every query defined since
  the last; many queries per transaction, and many transactions in
  flight, cost few round trips.
- **Keep queries.** Make a query once and define it again for each use:
  its rows and buffers are made with it and used again.
- **Hint.** A transaction started by its key's hint runs its first
  query on the node that holds the row.
- **Use committed reads** where no lock is needed: they take none.
- **Work where the rows are.** A scan with a program sends only the
  rows that pass it; a conditional update checks and writes in one
  round trip, and an update that adds to a column needs no read first.
- **AUTO_INCREMENT**: each refill is a round trip, under the table's
  lock, so a table inserted into fast wants a larger prefetch, and an
  insert of many rows a block.
- **Threads.** A connection per thread scales with cores; more receive
  threads help when many threads together read a lot.
- **Many threads, blocking calls:** an executor gathers their calls
  into shared sends. Its own thread wakes every caller, which is what
  limits one executor; a second helps while there are cores for it. A
  blocking call still costs a sleep and a wake-up; keep many calls in
  flight on one thread where the work allows it.
- **Measure release builds.** The library's checks in a debug build
  cost time a release build does not spend.

## 12. What is not done yet

- Programs are the basic set: comparisons, IS NULL, AND, OR and NOT,
  adding and subtracting integers, and assignments with a condition of
  their own, with at most eight values live at once; none on inserts
  or writes, through a unique index, or on a row a scan hands over.
- Inserts do not fill an AUTO_INCREMENT column by themselves: the
  application asks for the value (section 7).
- Rows a scan has locked are taken over on a connection only, not
  through an executor.
- No blob columns (BLOB, TEXT, JSON, GEOMETRY): records leave them out,
  and deletes on their tables are refused.
- No schema changes: tables and indexes are made through a MySQL
  server.
- No TLS between the API node and the cluster, and no choosing data
  nodes by location domain.
- At most 4 711 connections per process, RonDB's limit for an API
  node.
- Keys in three collations whose keys cannot be placed from their
  weights, `latin2_czech_cs`, `cp1250_czech_cs` and `tis620_thai_ci`,
  are not hinted: such a transaction goes to the next node in turn,
  which works and costs a hop. An ordered run over an index with such a
  column is refused.
- No events, no pushdown joins, no aggregation on the data nodes.
- Linux on x86_64 and aarch64 and macOS on aarch64; no Windows.
