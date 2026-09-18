# 02 — Architecture

## Layering

```
 applications (Rust)            C / C++ / Python / Go / Java / Node / .NET
        │                                    │
        │                          cpp/iclaustron.hpp (header only)
        │                                    │
        │                               ic_capi  (C ABI, cdylib + staticlib)
        │                                    │
        └──────────────► ic_apid ◄───────────┘        Data API
                            │
                     ic_ndb_signals                   GSNs, blocks, signal codecs
                            │
                         ic_apic                      config: mgm client, blob decode,
                            │                          typed config for API nodes
                       ic_protocol                    base64, mgm protocol strings
                            │
                         ic_comm                      sockets, poll set, page pool,
                            │                          line protocol
                         ic_util                      containers, threadpool, errors,
                            │                          debug, memory container
                         ic_port                      OS + replaces glib
```

Each layer only calls the layers below it. `ic_ndb_signals` has no
dependencies and is the only crate the fuzzers link.

## Runtime objects (Data API)

Names are the C names with `IC_` dropped. Indentation shows ownership.

```
ApidGlobal  (one per process; owns config, node table, threads, pools)
 ├─ ApiConfigServer            (from ic_apic: node list, ports, TCP parameters)
 ├─ NodeConnection[node_id]    (socket, send buffer chain, connection state,
 │                              heartbeat state, adaptive-send statistics)
 ├─ ReceiveThread[0..n]        (poll set over a subset of node sockets)
 ├─ SendThread[node_id]        (one per node connection, as in the C)
 ├─ HeartbeatThread            (API_REGREQ loop; owns its own ApidConnection)
 ├─ ThreadConnection[thread]   (per user thread: inbound signal queue + condvar)
 ├─ SockBuf                    (page pool for wire data)
 ├─ GlobalDictCache            (TableDef by name, ref-counted, versioned)
 └─ global WhereCondition / ConditionalAssignment pools

ApidConnection  (one per user thread; created from ApidGlobal)
 ├─ block number = 0x8000 + thread id   (what data nodes address replies to)
 ├─ ObjectMap                  (u32 object id → query / transaction owned by
 │                              this thread; generational, thread-local,
 │                              so late signals are rejected without locks)
 ├─ TcConnect[node_id]         (TC connect records seized by this thread)
 ├─ transaction id counter
 ├─ bound TableDefs
 ├─ queries: defined → sent → completed lists
 └─ iterator state for get_next_executed_query

Transaction  (from ApidConnection::start_transaction)
 ├─ TC node id, TC connect ptr, transaction id (u64)
 ├─ commit state
 └─ counts: ops sent, ops completed, start/execute/commit flags pending

Record  (from TableDef::create_record; describes a C struct layout)
 └─ per field: attribute id, byte offset, null bit, length encoding, type

ApidQuery  (from ApidGlobal::create_apid_query; reusable)
 ├─ TableDef, key Record + key row pointer, attribute Record + row pointer,
 │  field mask
 ├─ optional WhereCondition, ConditionalAssignment[]
 ├─ error object, user_reference, callback
 └─ per-execution state: object id, expected/received lengths
```

## Threading model

Fixed for the life of the project: **receive threads only receive and
route; user threads execute.**

- **User thread**: defines queries on its `ApidConnection`, calls `send`
  (or `flush`). `send` appends packed signals to each target node's send
  chain under that node's mutex and either wakes the node's send thread
  or, when the adaptive-send decision says so, writes to the socket
  inline. `poll` waits on the thread's `ThreadConnection` condvar for
  posted signals, then executes them: decodes `TCKEYCONF`/`TCKEYREF`/
  `TRANSID_AI`/`TC_COMMITCONF`/..., resolves the object id in its own
  `ObjectMap`, updates query and transaction state, writes row data into
  the user's row buffer through the `Record`, fires callbacks, and exposes
  completed queries via `get_next_executed_query`.
- **Receive threads** (dedicated; count set in `GlobalOptions` at connect
  and fixed for the life of the global, default 1; each owns a poll set
  over a subset of node sockets): read into `SockBuf` pages,
  split the byte stream into signal descriptors, unpack `API_PACKED`
  bundles into their sub-signals, look at each signal's receiver block
  number, map block number to user thread (block − 0x8000), append the
  descriptor to that thread's queue and signal its condvar. Signals for
  the cluster manager block go to the heartbeat thread's queue. They do
  nothing else with the signal content. They also run the adaptive-send
  timers so a buffered send never waits on an idle user thread.
- **Send thread per node**: drains that node's send chain to the socket;
  performs connect, the transporter handshake and the node-id hello;
  hands the connected socket to a receive thread; handles disconnect.
- **Heartbeat thread**: sends `API_REGREQ` to each data node on its
  schedule, executes `API_REGCONF`/`API_REGREF`, counts missed heartbeats,
  declares nodes dead, drives `NODE_FAILREP`/`NF_COMPLETEREP` processing
  and broadcasts node-state changes to user threads (a per-thread
  "node failed" notice in their queue, so each user thread fails its own
  in-flight queries on that node in its own context).

Why this split: the C++ NDB API's receive thread both receives and
executes signals, then wakes the target thread. Under many client threads
the receive thread becomes the bottleneck and every user thread contends
on it. Routing only the descriptor to the owning thread keeps the receive
thread's per-signal work to a table lookup and a queue append, and moves
all decoding to threads that scale with the application. The cost is one
hand-off per batch of signals, which is amortised because a `poll` drains
the whole queue.

Consequences designed in:

- Per-thread `ObjectMap` and per-thread TC connect records mean no shared
  mutable state on the reply path other than the queue.
- A `SockBuf` page is refcounted; the last user thread to execute a
  signal on it returns it to the pool (as in the C code).
- Signals for a thread that has exited (late replies) are dropped by the
  receive thread with a debug log.

Mutex ordering levels (lock lower before higher, never the reverse):

| Level | Mutex | Protects |
|---|---|---|
| 1 | `ApidGlobal.dict_mutex` | global dictionary cache |
| 2 | `ApidGlobal.mutex` | thread table, stop flag, node state table |
| 3 | `ThreadConnection.mutex` | one user thread's inbound queue |
| 4 | `ReceiveState.mutex` | receive thread's node add/remove lists |
| 5 | `SockBuf.mutex` | global page pool (thread-local free lists avoid it) |
| 6 | `NodeConnection.mutex` | one node's send chain, connection state |
| 7 | `HeartbeatState.mutex` | heartbeat node list |

Debug builds check the ordering at every lock (`ic_port::sync`).

## Data flow for a primary key read

1. User: once, `TableDef::create_record(&spec)` for the key struct and the
   row struct; `ApidGlobal::create_apid_query(table, key_record,
   attr_record)`. Per call: fill the key struct, `conn.read_key(query,
   trans, KeyRead, key_row_ptr, attr_row_ptr, mask, cb, user_ref)`.
2. `read_key`: allocate an object id in the thread's `ObjectMap`; if
   `trans` has no TC node yet, hash the distribution key from the key row
   (`hash.rs`), pick the node, seize a TC connect record for this thread on
   that node if none is cached; build `TCKEYREQ` + KEYINFO + ATTRINFO into
   the thread's signal buffer; set start/execute flags as required; move
   the query to "defined".
3. `send`: append signals to node send chains, set the execute flag on the
   last per node, adaptive-send decision, queries → "sent".
4. Data node replies with `TCKEYCONF` (packed) and `TRANSID_AI`.
5. Receive thread posts both descriptors to this thread's queue.
6. `poll`: `TRANSID_AI` → decode attribute headers, write values into the
   attribute row through the `Record` (`row_codec.rs`), record received
   length; `TCKEYCONF` → record expected length and commit state; when
   both are known → query complete → callback → "completed".
7. User iterates `get_next_executed_query`, reads the row struct, resets
   the query.

## Node failure flow

1. Heartbeat thread misses 4 intervals or receives `NODE_FAILREP` → marks
   node dead, starts disconnect on its send thread, posts a "node down"
   notice to every user thread queue.
2. Each user thread, on its next `poll`, fails every sent query whose TC
   node is the dead node with a temporary node-failure error and completes
   the transaction as aborted (or, if `TCKEY_FAILCONF` says the commit
   happened, as committed).
3. `NF_COMPLETEREP` arrives → node may be reconnected; the send thread
   retries with backoff.

## Memory

- Wire data lives in `SockBuf` pages (128-byte descriptors, cache-line
  aligned, refcounted) exactly as in the C.
- Signal construction uses a per-thread `MemoryContainer` reset after each
  `send`.
- `ApidQuery`, `Transaction`, `TableDef`, `Record` are `Box`ed objects
  handed out as opaque handles. Lifetimes are the ones documented in
  `ic_apid.h`.
- No allocation on the hot path after warm-up.

## Configuration source

Stock RonDB only: connectstring → `ndb_mgmd` → `get nodeid`,
`get config_v2` → typed structs for the API node itself, the data node
list and the TCP connection parameters. Everything else in the blob is
kept as a generic key/value map for diagnostics.

## Error propagation

Signal-level errors (`TCKEYREF`, `TCROLLBACKREP`, `GET_TABINFOREF`,
`API_REGREF`) become `IcError { code, category, severity, message }` on the
query or transaction. Node failure flips in-flight queries to a temporary
error class. Internal errors keep the 7000–7123 iClaustron code space. All
public functions return `Result`; the C ABI returns the code.
