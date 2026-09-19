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
 ├─ ReceiveThread[0..n]        (poll set over a subset of node sockets;
 │                              a node is assigned at connect and stays)
 ├─ ConnectThread[node_id]     (one per node: connect, handshake, retry)
 ├─ SendThreadPool             (small, shared; absorbs send overflow only)
 ├─ HeartbeatThread            (API_REGREQ loop; reads node state, never writes)
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
  descriptor to that thread's queue and signal its condvar. They do
  nothing else with the signal content, with one deliberate exception
  below. They also run the adaptive-send timers so a buffered send never
  waits on an idle user thread.

  **The exception: a receive thread executes `API_REGCONF` and
  `API_REGREF` for the nodes it owns**, rather than routing them. This
  is what makes node state lock-free, and it is the one place where
  reading signal content in a receive thread pays for itself. See
  "Node state has one writer" below.
- **Connect thread per node** (minimum stack size): resolves the port
  from the management server, connects, performs the transporter
  handshake and the node-id hello, then hands the connected socket to
  this node's receive thread. It retries on a growing delay for as long
  as the node is down, and goes back to retrying when the node is lost.
  One thread per node rather than one shared thread keeps a slow or
  unreachable node from delaying every other node's reconnect, and a
  thread asleep on a retry timer costs little. The C merges this role
  into the send thread (`ic_apid_send_thread.ic`, `connect_by_send_thread`);
  we keep the roles apart so the send side can be a pool.
- **Send thread pool** (small, shared by all nodes): absorbs send
  overflow only. The common case is that a user thread does its own
  send: it claims the node's send chain with a flag, writes outside the
  mutex, and on finishing hands the remainder to the pool if more
  buffers arrived meanwhile. A pool rather than the C's thread per node
  keeps the thread count off the node count, which matters at 144 data
  nodes.
- **Heartbeat thread** (one): walks the node table on its schedule and
  sends `API_REGREQ` to every node that is up. It reads node state and
  never writes it. It times a node out when the last `API_REGCONF`
  timestamp gets too old, by telling that node's receive thread, not by
  changing the state itself. It owns no node list, so it needs no mutex:
  it walks the fixed node array and skips what is not up.

  The C instead gives the heartbeat thread its own `ApidConnection` and
  a mutex-protected linked list of nodes, and routes `API_REGCONF` to it
  as an ordinary signal (`ic_apid_handle_message_array.ic` registers
  `execAPI_REGCONF_v0` in the same table user threads use). We move that
  work to the receive thread for the reason in the next section.

### Node state has one writer

Every node is assigned to one receive thread at connect and stays there.
That thread is also the one that sees the socket close. If it also
executes `API_REGCONF`, then **one thread writes a node's state and
nobody else does**, so the state needs no mutex at all. Readers get
atomics.

The rules that keep it true:

- The **connect thread** does not write node state. It hands the
  connected socket to the receive thread, which installs it and
  publishes the state.
- The **heartbeat thread** and **user threads** only read, through
  atomics: `node_up`, `start_level`, `node_group`, and the timestamp of
  the last `API_REGCONF`. The full node state blob stays private to the
  receive thread, because a 16-word structure cannot be read atomically
  and a torn read would be worse than a stale scalar.
- `NODE_FAILREP` names a node other than the one whose socket it arrived
  on, and that node usually belongs to a different receive thread. It
  therefore **must not write connection state**, or the single-writer
  rule breaks. It is used only to post "node failed" notices to user
  threads so they can fail their in-flight queries early. The failed
  node's own receive thread will see the close and publish the state.

What this buys, measured against the C: `node_failure_handling` in
`ic_apid_rec_thread.ic` takes the heartbeat mutex, a receive state mutex
and the node mutex at once, while `check_node_started` reads `node_up`
and the start state with no mutex at all, which is already a data race
the C gets away with. Single writer plus atomics removes the first and
legalises the second.

Two more locks fall out for free, both because receive-thread assignment
is fixed at connect. Nodes never move between receive threads, so the
add and remove lists and their mutex are not needed. The heartbeat
thread keeps no list, so its mutex is not needed either.

**What stays locked**, because it is genuinely written by many threads:
a node's send chain, and each user thread's inbound queue. Heartbeat
handling in the receive thread does not remove the per-node mutex, since
that mutex exists for the send chain. It removes the state contention on
it, which is small in absolute terms: at 144 nodes and a six second
period, `API_REGCONF` arrives about 24 times a second in total.

**Two paths report a node going away, and they stay separate.** A socket
that closes is seen by the receive thread, which drops the link at once
and tells the connect thread to start again. That involves no signal and
no other thread. `NODE_FAILREP` is the cluster's opinion, and it arrives
earlier than the close in some failures, so it is worth having as a head
start on failing queries. The C handles neither `NODE_FAILREP` nor
`NF_COMPLETEREP` at all and relies only on the dropped socket, so this is
new work rather than a translation.

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
  receive thread with a debug log, and counted. The C drops a signal
  addressed to a block it does not route with only a debug print
  (`ic_apid_rec_thread.ic`, "Message to module id %u not allowed"),
  which makes a routing mistake invisible in a production build.

Mutex ordering levels (lock lower before higher, never the reverse):

| Level | Mutex | Protects |
|---|---|---|
| 1 | `ApidGlobal.dict_mutex` | global dictionary cache |
| 2 | `ApidGlobal.mutex` | thread table, stop flag; start and stop only |
| 3 | `ThreadConnection.mutex` | one user thread's inbound queue |
| 5 | `SockBuf.mutex` | global page pool (thread-local free lists avoid it) |
| 6 | `NodeConnection.mutex` | one node's send chain |

Levels 4 and 7 are deliberately empty. The C has a receive state mutex
for moving nodes between receive threads and a heartbeat mutex for the
heartbeat node list; fixed assignment at connect removes the first and a
list-free heartbeat thread removes the second. Node state itself is not
in the table because it has a single writer and is published through
atomics.

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

1. The node's own receive thread sees the socket close, or the heartbeat
   thread finds the last `API_REGCONF` too old and tells that receive
   thread. Either way the node's receive thread is the one that acts: it
   publishes `node_up = false`, drops the socket out of its poll set,
   posts a "node down" notice to every user thread queue, and wakes the
   node's connect thread.
2. `NODE_FAILREP` about that node may arrive first, on a different
   node's socket. The receive thread that reads it posts the notices
   early so user threads can start failing queries, and changes no
   connection state. Step 1 still happens when the close is seen.
3. Each user thread, on its next `poll`, fails every sent query whose TC
   node is the dead node with a temporary node-failure error and completes
   the transaction as aborted, or as committed if `TCKEY_FAILCONF` says
   the commit happened.
4. The connect thread has been retrying since step 1 and reconnects when
   the node accepts again. `NF_COMPLETEREP` says the cluster finished
   taking the node's work over; it settles recovering transactions and
   does not gate reconnection.

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
