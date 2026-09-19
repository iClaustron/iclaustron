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
 ├─ DictCache                  (TableDef by name, shared by Arc, versioned;
 │                              built: ic_apid::dict_cache)
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
  "Node state has one writer" below. It also executes the other
  signals addressed to our cluster manager block rather than to a user
  thread: `NODE_FAILREP`, `NF_COMPLETEREP`, and `ALTER_TABLE_REP`, the
  dictionary's notice that a table changed, which lets go of the cached
  description under the dictionary cache's lock, holding no other.
- **Connect thread per node** (minimum stack size): resolves the port
  from the management server, connects, performs the transporter
  handshake and the node-id hello, then hands the connected socket to
  this node's receive thread. It retries on a growing delay for as long
  as the node is down. When a connected node is lost it does **not** go
  straight back to dialling: it waits until the node may be dialled
  again, see "Node failure flow".
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
- `NODE_FAILREP` and `NF_COMPLETEREP` name a node other than the one
  whose socket they arrived on, and that node usually belongs to a
  different receive thread. They therefore **must not write link
  state**, or the single-writer rule breaks. The failed node's own
  receive thread sees the close and publishes that.

**Membership state is a second, smaller thing, and it is not
single-writer.** Whether a lost node may be dialled again depends on
evidence that arrives on any socket: the node's own close, another
node's `NODE_FAILREP`, another node's `NF_COMPLETEREP`. It is a state
machine of three phases per node, connected, awaiting the takeover
report, and being dialled, and every transition in it is "the first
evidence wins and the rest do nothing". That is what compare-and-swap on
one atomic word gives, so it needs no mutex either, but it is shared
between threads in a way link state is not and the two must not be
merged. The rule that ends every wait when no data node is connected
looks at every node's membership, which is one atomic load each.

As built (`ic_apid::apid_global`, `NodeShared`): the membership word is
changed only by compare-and-swap, except that a failure report sets it
outright, since a report outranks whatever the link was doing. A connect
thread moves it from disconnected to connected when it claims a new
link, under the identity lock, so that a node id reclaim cannot happen
in between. A failure report from any receive thread asks the owning
receive thread to drop the link, through a per-node request word, rather
than touching the link itself.

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
it, which is small in absolute terms: heartbeats go out three times per
check interval per node, so even 144 nodes on a one second interval
produce under 450 `API_REGCONF` a second in total.

**Losing a link is not losing a node, and the two are kept apart.**
Our own evidence, the socket closing or a send failing or the heartbeats
stopping, says the link is gone. It says nothing about the node, which
may be up and serving everyone else. Only `NODE_FAILREP` from another
data node says a node failed. The C handles neither `NODE_FAILREP` nor
`NF_COMPLETEREP` and treats every closed socket alike, so this is new
work rather than a translation, and the reference for it is the data
nodes' protocol (`ClusterMgr.cpp`, `QmgrMain.cpp`), not the C.

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

### Building it in steps

Agreed with the author (2026-09-19) as a sequence of steps, each ending
somewhere testable against a live cluster:

1. **Shapes on one thread.** Published node state, inboxes, the thread
   table and the router, driven by a single polling loop. Done; the
   polling loop has since been replaced by step 2.
2. **Dedicated threads.** A connect thread per node, one receive thread
   that owns the poll set and the router and executes the signals about
   the node they came from, and one heartbeat thread. User threads send
   under a per-node mutex held for one write, and wait on their inbox.
   Done (`apid_global`, `connect_thread`, `rec_thread`, `heartbeat`).
3. **User threads for real.** An `ApidConnection` per user thread
   wrapping its inbox and block number, with a `poll` that executes what
   it takes, with **expected replies** and **fragments reassembled
   before anyone sees them** (both below). The first key
   lookup lands here, once a table can be fetched from the dictionary
   (phase 4), since an operation names its table's id and version.
   First cut done (`apid_conn`, `ApidGlobal::create_connection`): the
   inbox, `poll`, fragments joined, expected replies with one reply
   each, and requests failed when their link goes. The dictionary fetch
   and both tools run on it. Replies in several signals and the key
   lookup are still to come.
4. **The real send path.** The claim flag, user threads writing outside
   the mutex, the send thread pool taking overflow, adaptive send.
5. **Several receive threads**, each given a share of the nodes at
   start. Everything shared is already per node, so this is assignment.
6. **Later.** Wake-up threads for rounds that wake hundreds of user
   threads, and load-based placement of nodes on receive threads.

### Fragmented signals never leave the library (decided)

Asked for by the author (2026-09-19): **the API reassembles fragmented
signals in the user thread, so that neither an application nor a
higher-level API built on this one ever deals with a fragment.** A
fragment is a transport detail, like the signal header, and stays below
the line the header already stays below.

What that means for step 3:

- Reassembly happens once, in the per-thread connection object's
  `poll`, as the first thing done with each signal taken from the inbox.
  Every handler, and everything above, receives only whole signals. A
  handler is never written with fragments in mind.
- It stays in the user thread, not the receive thread, for the reason
  the rest of this chapter gives: the receive thread only routes, and
  every fragment of a reply is addressed to the same block, so all of
  them land in the same inbox in order.
- The assembler's state belongs to the connection object, one per user
  thread, keyed by sending node, sending block and fragment id. When a
  node's link is lost, the fragments begun from that node can never be
  finished and are dropped with it.
- Expected replies (below) are matched against the whole signal. A
  reply that arrives in five fragments is still one reply.

Built with step 3: `ApidConnection::poll` hands each signal to
`ic_apid::fragments` first, and `dict_client` sees whole signals like
everything else. A lost link drops that node's unfinished fragments.

### Expected replies (decided; single replies built)

Asked for by the author (2026-09-19); built with step 3 for requests
answered by one signal.

**When a user thread sends a signal, it says which signal numbers it
expects back.** A `TCSEIZEREQ` expects `TCSEIZECONF` or `TCSEIZEREF`; a
`GET_TABINFOREQ` expects `GET_TABINFO_CONF` or `GET_TABINFOREF`; a
`TCKEYREQ` expects `TCKEYCONF`, `TCKEYREF` and `TRANSID_AI`. The
expectation is recorded with the request's own number (the `senderData`
or connect pointer the reply echoes) and the node it was sent to, and is
cleared when a final reply arrives.

What it gives, as seen from here:

- **A signal nobody expects is visible.** A late reply to a request
  already answered or abandoned, or a signal of the wrong kind, is
  caught at the point it arrives and counted, rather than being handled
  as if it belonged.
- **Several requests can wait on one inbox.** A signal is handed to the
  expectation it matches, not to whichever code happens to be reading
  the inbox.
- **A node failure fails exactly what was waiting on it.** The
  expectations recorded against the failed node are the requests to
  complete with a node-failure error, with no scan of every query.

As built (`ic_apid::apid_conn`):

- **The check is in the user thread only**, where the expectation is
  created and consumed and needs no lock. The receive thread stays a
  pure router, as the rest of this chapter wants; the question left
  open here before was settled that way for the first cut.
- A whole signal matches an expectation when its signal number is one
  the request named, it comes from the node the request went to, and
  its first data word is the request's number. Every reply used so far
  echoes the number there. A request does not always carry it first:
  `TCRELEASEREQ` leads with the coordinator's record and carries ours
  third (`DbtcMain.cpp`, `execTCRELEASEREQ`), so the number is given
  when sending, not read from the request.
- The first matching reply completes the request. `TCKEYREQ`, answered
  by several signals, needs the expectation to stay until the final
  one: `expect_several` keeps it until the request is forgotten, and
  `take_replies` hands out what has come. Such an expectation may also
  take replies from any node, as `TRANSID_AI` comes from the node that
  read the row; the loss of the link to the node the request went to
  still ends the wait. The key operation code decides when it is done.
- Each expectation records the link generation it was sent over. After
  every `poll`, a request whose node's link is down, or is a newer link
  than the one it went over, is completed with the node's error. Its
  reply cannot come, and waiting would only run into the timeout.
- `call` sends and waits in 100 ms slices, so a lost link is noticed
  within one slice. A request that times out is forgotten; its reply,
  if it comes later, is counted as unexpected.

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

1. **The link goes.** The node's socket closes, a send to it fails, or
   it has answered no heartbeat for four check intervals in a row. The
   node's own receive thread publishes the link as down, drops the
   socket from its poll set, and posts a "link lost" notice to every
   user thread queue. The connect thread starts dialling again on the
   usual delay, because at this point nothing says the node failed. If
   nothing more arrives this is the whole story: the node was up all
   along, the link comes back, and nobody ever reports a takeover
   because there was none. A data node does nothing special when it
   loses an API link.
2. **The cluster says the node failed.** A data node sends
   `NODE_FAILREP` naming it, usually a moment after step 1 and
   sometimes before it. Every surviving node sends one and only the
   first does anything. The link is torn down if it still stood, the
   node's membership state moves to awaiting takeover, and the connect
   thread **stops dialling**. Dialling between steps 1 and 2 did no
   harm, because a dead node refuses the connection.
3. Each user thread, on its next `poll`, fails every sent query whose TC
   node is the dead node with a temporary node-failure error and completes
   the transaction as aborted, or as committed if `TCKEY_FAILCONF` says
   the commit happened.
4. **A node reported failed is not dialled until its failure is
   reported handled.** Every surviving data node sends `NF_COMPLETEREP`
   to every registered API node once all its blocks have finished
   handling the failure. The first to arrive moves the node to being
   dialled, and the connect thread starts. The block field of that
   report carries the sender's cluster manager reference, not zero, and
   is ignored; the failed node id is what counts.
5. If no data node is connected at all, nobody can send that report, so
   every wait ends at once. That state means the whole cluster is gone
   from where we stand, and it is the state the wait exists to make
   visible: it is where whatever a cluster restart invalidates, such as
   the dictionary cache, gets invalidated. Built for the cache:
   `note_link_lost` lets go of every cached description, as the
   reference does when its count of connected nodes reaches zero.
6. **In that state our node id may no longer be ours.** Once connected,
   our only claim on the id is our connections; the management server
   drops its own reservation as soon as a transporter holds the id. An
   id named in the connectstring is asked for by nobody else. An id the
   management server chose for us can go to the next API node that asks
   for any id. So a node with a chosen id returns to the management
   server before dialling anyone. It asks for the same id first, asks
   again for as long as the answer is "not now", and takes another id
   the moment the answer is that the id is held. A new id means a new
   identity: the configuration is fetched again as that node, and every
   block reference we hand out changes with it. With threads, that is a
   stop-the-world event for user threads, though one that comes when
   they have nothing in flight.

   **Decided (author, 2026-09-19): take a new id at once, but only if
   any id will do.** After a link loss the "other node" holding our id
   is usually our own old connection, which the data nodes have not yet
   timed out (seen live; chapter 05, 1.3a). Waiting for it means being
   away for four heartbeat intervals, two minutes on a cluster with a
   30 second interval, which is what the C++ API amounts to. Taking
   another id has us back in seconds, and with every link lost nothing
   is in flight, so identity is never cheaper to change. The old id
   occupies an API slot until it times out; if a flapping network ever
   uses the slots up, the request for any id is refused as "not now"
   and asked again until one frees.

   **An application started with a stated node id never takes
   another.** Startup parameters will let the node id be given when the
   application starts, as `nodeid=` in the connectstring already does.
   That id is then the only one the node may run under. It skips the
   management server entirely after an outage, redials under its id,
   and is answered `BYE` until the data nodes have let the old
   connection go.

Why wait at step 4 for a node the cluster has reported failed, rather
than redial at once as the C does. Two reasons, both from the data nodes' side of the protocol.
The report is what tells an API node that nothing more will be heard
about transactions that were running on the failed node. And redialling
early lets the API reach the restarted node before it has noticed that
the other nodes went down too, so a whole cluster restart passes for one
node bouncing and step 5 never happens. An earlier revision of this
chapter said the report "does not gate reconnection". That was wrong,
and came from reading only the C.

**Where we differ from the C++ API, on purpose.** It treats its own
disconnect as a failure report and then waits for a takeover report. For
a link-only loss that report never comes, since no data node reports a
takeover for a node that did not fail, and the data node has no handling
that would resolve it (confirmed with the author of the protocol,
2026-09-19). In that state the C++ API tends to give applications error
4009, "cluster failure", which it is not. Here step 1 and step 2 are
separate events with separate errors, `IC_ERROR_LINK_LOST` and
`IC_ERROR_NODE_DOWN`, and neither is ever presented as the cluster
failing while another data node is connected. "The whole cluster is
gone" is step 5 and nothing else.

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
