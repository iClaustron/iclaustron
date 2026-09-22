# 05 — NDB protocol reference for the implementation

This chapter is the clean-room specification the `ic_ndb_signals` and
`ic_apid` crates are written from. Each item states what the protocol is,
in our words, and gives a *verification pointer* (`file:line`) into the
RonDB tree at `/Users/mikael/mysql_trees/rondb_2604_main/storage/ndb` so
a reviewer can confirm the statement. Pointers are for checking, not for
copying (see [09-licensing.md](09-licensing.md)). Where the iClaustron C
code already implements the item, that is noted too; that code *can* be
carried over.

Version in scope: **RonDB 26.10 only** (the tree is 26.10.0). No
compatibility paths for older releases. iClaustron's C code was written
against NDB 7.2.9; §14 lists what changed since then so no 7.2-era
assumption survives unexamined.

## 1. Connecting to a data node

### 1.1 TCP and socket authentication

Plain TCP to the data node's transporter port (from the config's TCP
connection section; if the port is 0 the port is dynamic and must be asked
from `ndb_mgmd` with `get connection parameter`). Then a text handshake,
lines terminated by `\n`.

Two dialects exist on the client side:

- Legacy: client sends `ndbd` then `ndbd passwd`, reads one line, requires
  it to start with `ok`. (This is what iClaustron implements in
  `authenticate_client_connection`.)
  Verify: `src/common/util/SocketAuthenticator.cpp:61-127`.
- TLS-capable (MySQL 8.3+/RonDB): client sends one of `ndbd TLS required`,
  `ndbd TLS enabled`, `ndbd TLS disabled`, then an empty line. Server
  answers one of `ok`, `Cleartext ok`, `TLS ok`, `Cleartext required`,
  `TLS required`, `Error`. On `TLS ok` both sides start TLS on the same
  socket. A bare `ndbd` is classified as "too old" by the TLS-capable
  server. Verify: `SocketAuthenticator.cpp:133-236`.

Decision: send the TLS-capable form with `TLS disabled` in 0.1, accept
`ok` and `Cleartext ok`. **Open question Q2**: confirm on a live 26.10
node whether the legacy form is still accepted, so the fallback can be
dropped or kept.

### 1.2 Node id hello

After authentication the client sends one line with four decimal integers:
`<my node id> <transporter type> <remote node id> <multi transporter
instance>`, where transporter type is 1 for TCP and the instance is always
0 for an API node. The whole line must be at most 23 characters. The
server replies `<its node id> <transporter type>` and the client checks
both. On rejection the server may write `BYE`. Verify:
`src/common/transporter/Transporter.cpp:394-544` (client),
`TransporterRegistry.cpp:664-908` (server). iClaustron sends only the
two-integer form (`"<nodeid> 1"`); the server still parses 2, 3 or 4
integers, but we send four.

### 1.2a When the node says `BYE`

Instead of the two numbers, a data node may answer the hello with the
single word `BYE`. It does so when its transporter towards us is not in
the connecting state, which is the ordinary condition of a restarting
node that has not yet opened up to API nodes. It is not an
authentication failure. The node then waits for the client to close
first, so that the lingering socket state lands on the client. We
return `IC_ERROR_NODE_NOT_READY` and redial on the usual delay. Seen
live once in every node restart, between "connection refused" and the
first accepted hello. Verify: `TransporterRegistry.cpp:810-850`.

### 1.3 Via the management server

`ndb_mgmd` can convert a management connection into a transporter
connection: the client sends `transporter connect` and an empty line, and
then performs 1.2 on the same socket. The C cluster server (out of scope)
implemented the *server* side of this; we do not use the mgm path.
Verify: `src/mgmapi/mgmapi.cpp` (`ndb_mgm_convert_to_transporter`),
`TransporterRegistry.cpp:4230-4267`.

### 1.3a How long a node id stays ours

The management server reserves an id when it grants one, for a limited
time, and drops the reservation as soon as the id is held by a connected
transporter, and also on `NODE_FAILREP` or `NF_COMPLETEREP` naming it.
From then on a request for an id is decided by asking the data nodes,
which refuse one that is connected. So an API node's claim on its id,
once it is connected, is its connections and nothing else. **An API node
that loses every connection can lose its id**, if the id was chosen for
it rather than named in its connectstring, because the next node asking
for any id may be given it (pointed out by the author, 2026-09-19). The
C++ API claims an id once and never again. We claim it again after
losing every link and before dialling; see chapter 02, "Node failure
flow". A session may ask for an id more than once; nothing in the
server ties a session to one request. Verify: `MgmtSrvr.cpp:4067-4089`,
`:4145`, `:4206-4270`, `:4273-4320`; `Services.cpp:734-830`;
`ndb_cluster_connection.cpp:1399-1401`.

**What a refusal means.** A refused request carries `error_code` when
the request carried `log_event`, which ours does. The code separates
only two cases: 1102 is final (the id is not in the configuration, is of
another node type, or belongs to another host), and everything else,
1101 and 1103, "may succeed if asked again". In particular **"held by
another node" and "cluster not ready" are both 1101** and only the text
tells them apart: `Id N already allocated by another node.` against
`Cluster not ready for nodeid allocation.` The server's source notes
that the MySQL server matches on these texts as well, so they are
interface in practice. A third text, `already allocated by this
ndb_mgmd`, is a reservation on that server that times out, quite
possibly our own. We map the three kinds to `IC_ERROR_NODEID_NOT_ALLOWED`,
`IC_ERROR_NODEID_IN_USE` and `IC_ERROR_NO_NODEID`.

**"Another node" is usually ourselves.** Seen live (2026-09-19, network
cut on the API side for about 15 s): every link was lost, and on
returning the server refused our old id as "already allocated by another
node" three times running. Nobody had taken it. The data nodes had never
seen our sockets close, so to them the old connection was still up, and
it stays up until they have gone four heartbeat intervals without
hearing from it, two minutes on that cluster. The refusal therefore
means "the cluster counts this id as connected" and nothing more. We
take another id at once if any id will do, and never if the application
was started with a stated one (decided by the author; chapter 02, "Node
failure flow"); the C++ API,
which never asks again, simply redials under the old id and is answered
`BYE` until the data nodes time the old connection out. Seen live: a
restarting cluster answers "not ready" for several seconds, which an
earlier revision counted towards giving the id up. An earlier revision
of this section also claimed the code told the first two apart; it does
not. Verify: `mgmapi_error.h:80-87`, `MgmtSrvr.cpp:5040-5080`,
`Services.cpp:753`, `:816`.

**How a pulled cable shows, seen live on macOS (2026-09-19).** Pulling
the API host's Ethernet cable for about 10 s did nothing at all: the
links carried on, TCP resent what had queued, and neither side's
heartbeat count came near its limit. Pulling it for about 20 s lost
every link at once with "Can't assign requested address" (errno 49),
reported on the reading side of the sockets by the receive thread, with
no heartbeat write failing first. So macOS, once it has removed the
interface's address, reports the error on every socket bound to it, and
the difference between the two pulls is whether it got that far. It is
not a matter of when the next heartbeat happens to be written. A cut
that leaves our address in place is found only by heartbeat silence,
after three to four check intervals.

### 1.4 Reconnect policy

Connect attempts are rate limited with a backoff; RonDB additionally has a
node "active" flag in config (`IC_CFG_NODE_ACTIVE`) and refuses connections
to/from inactive nodes. Verify: `Transporter.cpp:290`, `:517`,
`TransporterRegistry.cpp:737`.

## 2. Signal framing ("Protocol6")

Every message is a sequence of 32-bit words in the **sender's native byte
order**. There is no byte swapping: the sender stamps its endianness in
the header and the receiver rejects a mismatch. Verify:
`src/common/transporter/Packer.cpp:160`, `:430`. (iClaustron implemented a
swap; it is unnecessary and we drop it.)

Layout, in order:

1. Header word 1
2. Header word 2
3. Header word 3
4. Optional signal id word (if bit 2 of word 1 is set)
5. Signal data: 0 to 25 words
6. One length word per section (0 to 3 sections)
7. The section payloads, concatenated
8. Optional checksum word: XOR of all preceding words

Word 1 bit fields (mask, shift):

| Field | Mask | Shift |
|---|---|---|
| byte order flag, replicated in 4 bits | `0x81000081` | bits 0, 7, 24, 31 |
| fragment info, low bit | `0x00000002` | 1 |
| signal id present | `0x00000004` | 2 |
| compressed (never set by us; receiver rejects) | `0x00000008` | 3 |
| checksum present | `0x00000010` | 4 |
| priority (2 bits) | `0x00000060` | 5 |
| total message length in words (16 bits) | `0x00FFFF00` | 8 |
| fragment info, high bit | `0x02000000` | 25 |
| signal data length in words (5 bits) | `0x7C000000` | 26 |

Word 2: bits 0–19 GSN (16-bit GSN plus a 4-bit version id), bits 20–25
trace, bits 26–27 number of sections.

Word 3: bits 0–15 sender block reference **number** (block number only;
the receiver reconstructs the full reference from the connection's node
id), bits 16–31 receiver block number.

Verify: `src/common/transporter/TransporterInternalDefinitions.hpp:54-158`,
`Packer.cpp:151-254` (unpack), `:507-558` (pack). iClaustron's
`fill_ndb_message_header` and `create_ndb_message` implement this correctly
and are carried over.

Limits: a message is at most 32 768 bytes (`MAX_SEND_MESSAGE_BYTESIZE`,
`MAX_RECV_MESSAGE_BYTESIZE`); a single section is at most 7 400 words in
one signal; the receive loop handles up to 1 024 signals per call. Verify:
`include/transporter/TransporterDefinitions.hpp:85-86`,
`include/kernel/ndb_limits.h:298-305`, `Packer.cpp:40`.

Checksum: XOR of all words except the checksum word itself. Verify:
`TransporterInternalDefinitions.hpp:50`, `include/util/Checksum.hpp:93`.

### 2.1 Fragmented signals

When the sections exceed the per-signal limit the sender emits several
signals with the same GSN and fragment info 1 (first), 2 (middle),
3 (last). **Every fragment carries the full original signal data**,
followed by one word per section it carries giving that section's
original number, then one word with the fragment id; its sections are
the pieces of the original sections. Sections are split on 60-word
boundaries, and the highest-numbered section is sent first. The receiver
appends each carried piece to the original section its number names and
tells trains apart by (sender, fragment id). Verify: `SimulatedBlock.cpp`,
`sendFirstFragment` and `sendNextLinearFragment`; `NdbApiSignal.hpp`,
`getFragmentId` and `getFragmentSectionNumber`.

An earlier revision of this section said that only the final fragment
carried the real signal data. That is wrong, and the reference's own
`execGET_TABINFO_CONF` depends on it being wrong: it reads the request
id from every fragment.

**But not every fragmented signal repeats its data.** A block may split
an answer by hand, with the same trailing section numbers and fragment
id, and put values belonging to each piece in the data. `Dbdict` does
this for `LIST_TABLES_CONF`: each piece's `noOfTables` counts only that
piece's objects, and the API adds them up (`execLIST_TABLES_CONF`). A
piece may also carry one section, or none. A generic reassembler keeps
the first piece's data, so such values must come from the joined
sections. Verify: `Dbdict.cpp`, `sendLIST_TABLES_CONF`.

**When a signal is split.** The fragmenting send sends a signal whole
when its data and sections come to at most `MAX_SIZE_SINGLE_SIGNAL`,
7400 words, and otherwise splits it into pieces of `FRAGMENT_WORD_SIZE`
words: 3840 in a release build, 120 in a debug build (`VM_TRACE`). The
one exception is `TRANSID_AI`, which a debug build splits above 240
words (`DEB_MAX_SIZE_SINGLE_SIGNAL`). Verify: `SimulatedBlock.cpp`,
`sendFirstFragment`; `ndb_limits.h:305-306`; `SimulatedBlock.hpp:1003-1011`.
The comment above `FRAGMENT_WORD_SIZE` says splitting starts above the
piece size; the code says otherwise, and a first draft of this section,
written from the comment, was wrong in the same way.

So a table description arrives whole unless it is very large, and row
data from a debug data node can arrive in pieces. The API side must
reassemble at least `GET_TABINFO_CONF` and `TRANSID_AI`, not only event
traffic as an earlier revision said. `ic_apid::fragments` does it, in
the user thread that executes the signal. The largest whole signal,
7400 words, fits in our 32 KB receive limit.

## 3. Block numbers and references

- Kernel blocks: `BACKUP 0xF4, DBTC 0xF5, DBDIH 0xF6, DBLQH 0xF7,
  DBACC 0xF8, DBTUP 0xF9, DBDICT 0xFA, NDBCNTR 0xFB, QMGR 0xFC, NDBFS 0xFD,
  CMVMI 0xFE, TRIX 0xFF, DBUTIL 0x100, SUMA 0x101, DBTUX 0x102, ...,
  TRPMAN 0x10A`, plus RonDB query blocks `0x10B–0x111` which can appear as
  senders of `TRANSID_AI`. Verify: `include/kernel/BlockNumbers.h:49-79`.
- API blocks: dynamic per client thread from `0x8000` upward (max 4 711
  clients); `API_PACKED = 0x07FF` (2047) for packed signals;
  `API_CLUSTERMGR = 0x0FA2` (4002) fixed for the heartbeat client. Verify:
  `BlockNumbers.h:34-47`. iClaustron's `IC_NDB_MIN_MODULE_ID_FOR_THREADS
  32768` and `IC_NDB_PACKED_MODULE_ID 2047` match.
- Reference = `node | (block << 16)`. In multi-threaded kernels the block
  field also carries an instance number through a non-trivial mapping that
  must be reproduced exactly when decoding sender references. Verify:
  `include/kernel/RefConvert.hpp:96-184`.

## 4. Cluster membership and heartbeats

Version announced: `get nodeid` and `API_REGREQ` carry an NDB version
word `(major << 16) | (minor << 8) | build`; for RonDB 26.10.0 that is
`0x1A0A00`, plus a MySQL version word. The data node checks API/DB
compatibility and answers `API_REGREF` on mismatch. We announce 26.10.0
(Q5). Verify: `include/ndb_version.h.in:48-61`, `ClusterMgr.cpp:374-383`.

- `API_REGREQ` (GSN 3) from the API's cluster-manager block to `QMGR`
  every `min(max interval, heartbeat check interval / 2)`; data: own
  reference, NDB version, MySQL version (3 words). Verify:
  `include/kernel/signaldata/ApiRegSignalData.hpp:34-51`,
  `src/ndbapi/ClusterMgr.cpp:371-561`.

  **We send every third of the check interval, not every half.** The
  author judges half too seldom (2026-09-19). Sending more often than
  the C++ API does is safe by construction, since the data node only
  counts intervals in which it heard nothing. `ic_apid::apid_global`,
  `IC_HEARTBEATS_PER_INTERVAL`, with a compile-time check that it is
  never set below three.
- `API_REGCONF` (GSN 1), 22 words in this order: `qmgrRef, version,
  apiHeartbeatInterval, mysql_version, minDbVersion`, then the 16-word
  `nodeState`, then `minApiVersion` **after** it. The order matters and
  is easy to get wrong; `minApiVersion` is the last word, not the sixth.
  The node state holds start level, node group, dynamic id, start phase,
  single-user mode and a connected-nodes bitmap of 256 bits, which stays
  256 bits however far node ids rise because it only covers data nodes.
  A data node counts as usable when started, or in single-user mode, or
  (RonDB) past the restart barrier while recovering.

  `apiHeartbeatInterval` is in **hundredths of a second**, a tenth of the
  `HeartbeatIntervalDbApi` the configuration states in milliseconds: a
  cluster configured for 30000 ms answers 3000 here. Confirmed against a
  live RonDB 26.10 cluster, and matching the C++ cluster manager, which
  multiplies this field by ten.

  Verify: `ApiRegSignalData.hpp:87-108`, `NodeState.hpp:113`,
  `ClusterMgr.cpp:1676-1905`.
- `API_REGREF` (GSN 2): rejection, e.g. version mismatch.
- Node declared dead after 4 missed heartbeats; then `NODE_FAILREP`
  (GSN 26) is sent to self and broadcast to all client blocks, followed by
  `NF_COMPLETEREP` (GSN 27) from the cluster when takeover is complete;
  only then may transactions be aborted with the right error and the node
  be reconnected. Verify: `ClusterMgr.cpp:2304-2404`,
  `src/ndbapi/Ndbif.cpp:1346-1372`, `Ndb.cpp:272-314`.

  The detail, as implemented in `ic_apid` (`apid_global`,
  `rec_thread`, `heartbeat`, `connect_thread`):

  - **Counting missed heartbeats.** A counter per node goes up by one at
    the end of every check interval and back to zero on every
    `API_REGCONF`, so at rest it moves between zero and one. The node is
    lost when it reaches 4, which is at least three whole intervals of
    silence. The check interval has a floor of 100 ms whatever the node
    reports. Verify: `ClusterMgr.cpp:485-540`, `:1844-1861`;
    `ClusterMgr.hpp:99`.
  - **Every sign of a failure converges.** A disconnect of a node that
    had finished connecting is handled as if a `NODE_FAILREP` naming
    that one node had arrived, unless one already has. So the socket
    closing, the heartbeats stopping and another node's report all run
    the same handling, and only the first for a given connection does
    anything. Verify: `ClusterMgr.cpp:2200-2300` (disconnect),
    `:2302-2375` (the report).
  - **The reconnect gate.** A data node that is not connected is not
    dialled while its takeover report is outstanding. The stated reason
    is that cluster disconnect can then be detected reliably. The flag
    is cleared on the first handling of a failure, set by
    `NF_COMPLETEREP`, and set again on connect. Verify:
    `ClusterMgr.cpp:448-461`, `:2092-2106`, `:2165-2178`, `:2355-2364`.
  - **Who sends `NF_COMPLETEREP` to an API node, and what is in it.**
    Every surviving data node, once all of its blocks have handled the
    failure, to every API node it holds as registered. Its block field
    carries the sender's `QMGR` reference, **not zero**. Zero means
    "whole node" only between blocks inside a data node. The API side
    reads the failed node id and nothing else. Verify:
    `QmgrMain.cpp:4860-4880`, `ClusterMgr.cpp:2092-2106`.
  - **When nobody is left to report.** If handling a failure leaves no
    data node alive, the API completes every outstanding takeover
    itself, since no `NF_COMPLETEREP` can come. Verify:
    `ClusterMgr.cpp:2383-2401`.
  - **A lost link is not a failed node.** When only the API's link to a
    node drops and the node stays up, no `NODE_FAILREP` arrives, and the
    data node has no special handling of the state (confirmed with the
    author, 2026-09-19). The reference rules above would leave that node
    undialled until it really restarts or every other node is lost,
    because nobody reports a takeover for a node that did not fail, and
    the C++ API tends to answer applications with 4009, "cluster
    failure", meanwhile. We differ deliberately: our own evidence of a
    loss (socket, send, heartbeat silence) only redials, with
    `IC_ERROR_LINK_LOST`; the reconnect gate applies only once a data
    node has actually sent `NODE_FAILREP` for the node.

  The iClaustron C implements none of this. It has no handler for either
  signal and redials a lost node on a three second timer. **For protocol
  behaviour this chapter is the authority and the C is not**; the C is
  the authority for structure.
- Other membership GSNs: `CONNECT_REP 163`, `CLOSE_COMREQ 127`,
  `ALLOC_NODEID_CONF 61`, `TAKE_OVERTCCONF 399`, `DUMP_STATE_ORD 465`,
  `EVENT_REP 247`. RonDB-only: `ACTIVATE_*`, `DEACTIVATE_*`,
  `SET_HOSTNAME_*`, `SET_DOMAIN_ID_*`, `GET_DATABASE_*`,
  `LIST_DATABASE_*`. Verify: `include/kernel/GlobalSignalNumbers.h`.

## 5. Management protocol and configuration

### 5.1 Text protocol

Request: a command line, then `name: value` lines (strings as
`name:"value"`), then an empty line. Reply: a line `<command> reply`, then
`name: value` lines, then an empty line. Lines are at most 512 bytes.
Verify: `src/mgmapi/mgmapi.cpp:470`. iClaustron's `ic_protocol_support.c`
and `ic_apic_conf_read_proto.ic` implement both sides and are carried
over.

- `get nodeid` with `version`, `nodetype`, `nodeid`, `user:"mysqld"`,
  `password:"mysqld"`, `public key:"a public key"`, `endian:"little"|"big"`,
  optional `name`, `log_event`; reply `result: Ok` and `nodeid`. Verify:
  `mgmapi.cpp:3268-3329`.
- `get config_v2` (preferred) or `get config` with `version`, `nodetype`,
  and for v2 `node: <id>`; reply carries `Content-Length`,
  `Content-Type: ndbconfig/octet-stream`,
  `Content-Transfer-Encoding: base64`; then `Content-Length + 1` raw bytes
  of base64 follow the empty line. Verify: `mgmapi.cpp:3090-3214`.
- `get connection parameter` with `node1`, `node2` and `param`,
  answering `value` and `result`. The value is **signed**, and for
  `CFG_CONNECTION_SERVER_PORT` (406) the sign is information: a negative
  value means the port was assigned when the node started rather than
  written in the configuration, and the port is its absolute value, so
  `-59733` means port 59733. Zero means the node has not been given a
  port yet. Verify: `TransporterRegistry.cpp:3997`, which takes the
  same absolute value on the listening side.
- Also
  `transporter connect` (1.3).

### 5.2 Binary configuration format

All words network byte order (big-endian), unlike the signal protocol.

- v1: magic `NDBCONFV`, then entries `key word = type<<28 | section<<14 |
  key`, types `Int=1` (1 word), `String=2` (length word incl. NUL, then
  padded bytes), `Section=3` (1 word), `Int64=4` (2 words, high then low);
  trailing XOR checksum. Verify: `src/common/mgmcommon/ConfigObject.cpp:372-620`,
  `include/util/ConfigSection.hpp:330-336`.
- v2: magic `NDBCONF2`, then a 7-word header (total length, version=2,
  number of default sections=5, data nodes, api nodes, mgm nodes, comm
  sections), then default sections, system section, node sections,
  communication sections in that order; entry key word `type<<28 | key`
  (section implied by position); default sections supply inherited values;
  trailing XOR checksum. Section type ids `DataNode 1, ApiNode 2, MgmNode 3,
  Tcp 4, Shm 5, System 6, Rdma 7 (RonDB)`. Verify:
  `ConfigObject.cpp:850-877`, `:1032-1100`, `ConfigSection.hpp:53-60`,
  `:338-341`.

iClaustron's `ic_apic_conf_read_transl.ic` decodes both. Parameter ids are
in `include/mgmapi/mgmapi_config_parameters.h`; iClaustron's own table in
the new API-node parameter table is verified against it for 26.10.

## 6. Transactions and primary key operations

### 6.1 Seizing a TC record

Before the first operation to a given data node, send `TCSEIZEREQ`
(GSN 39) to `DBTC` with: API connect pointer (our object id), our block
reference, requested TC instance. `TCSEIZECONF` (GSN 37) returns the TC
connect pointer and TC reference to use in all later signals for that
record. Records are cached per node per client and released with
`TCRELEASEREQ` (GSN 36) at shutdown. Verify: `src/ndbapi/Ndb.cpp:112-169`,
`Ndbif.cpp:1014`, `Ndblist.cpp:418`.

The words, as `ic_ndb_signals::tc_seize` has them, none with sections:

| Signal | GSN | Words |
|---|---|---|
| `TCSEIZEREQ` | 39 | our pointer, our block reference, instance wanted (0 for any) |
| `TCSEIZECONF` | 37 | our pointer, the coordinator's pointer, the coordinator's block reference |
| `TCSEIZEREF` | 38 | our pointer, NDB error code |
| `TCRELEASEREQ` | 36 | **the coordinator's pointer**, our block reference, our pointer |
| `TCRELEASECONF` | 34 | our pointer |
| `TCRELEASEREF` | 35 | our pointer, NDB error code, source line of the refusal |

The release does not mirror the seize: the coordinator's pointer comes
first, because it is the coordinator that has to find the record. A
seize is refused when the node is not started or is shutting down, or
has no record free. Verify: `DbtcMain.cpp:2366-2500`,
`NdbApiSignal.cpp:134-138`.

**The coordinator's block reference is echoed, never decoded.** Its
upper sixteen bits hold the block and the instance, and RonDB packs up
to 1024 instances into them with a transform of its own
(`RefConvert.hpp`, `blockToMain` and `blockToInstance`). An API node has
no use for the instance: it sends later signals to exactly the sixteen
bits it was given.

These are the first signals sent under a user thread's block number, so
their answers are the first to be routed to a user thread's inbox
instead of being executed where they arrive. `ic_node_ping` does the
exchange on every started node for that reason.

### 6.2 Transaction id

Client-side only: a 64-bit value whose high word is unique per client
block and whose low word is a counter. Sent as `transId1` (low) and
`transId2` (high) in every signal. Verify: `Ndb.cpp:2219`.

As built (`ApidConnection::next_transaction_id`): the block number in
bits 52–63, our node id in bits 40–51, the counter in the low word,
starting again at 0 after `0xFFFFFFFF`. The counter is kept per block
number when a connection goes, and the next connection with that block
number goes on from it, so no id is made twice. Verify: `Ndbif.cpp`,
where the first id of an `Ndb` is made; `Ndbinit.cpp`, where its counter
is kept for the block; `ndb_cluster_connection.cpp`,
`get_next_transid`.

### 6.3 Choosing the TC node (hinting)

1. Concatenate distribution-key column values in attribute-id order, each
   padded to a 4-byte boundary; charset columns are normalised first.
2. Hash, chosen per table by `HashFunctionFlag` (DictTabInfo key 163):
   0, or not sent, is MD5 producing 4 words; non-zero is **XXH3, 64-bit**
   (`XXH3_64bits` of the xxHash library, seed 0), split into two words,
   low then high. **Not XXH64**, which an earlier revision of this plan
   named: the two are different functions, and the wrong one sends every
   key to the wrong fragment. The AVX2 path calls the same function.
   **The distribution hash is word 1 of the result, not word 0**: the
   second word of the MD5 digest, or the high 32 bits of XXH3. A table
   gets the new hash when the API creating it is RonDB 22.10.1 or later,
   so tables made by a 26.10 mysqld have the flag; an index uses its
   table's function. Verify: `include/util/rondb_hash.hpp:52`,
   `src/common/util/rondb_hash.cpp:33-60`, `xxhash_std.cpp:31`,
   `xxhash_avx2.cpp:38`, `ndb_version.h.in`,
   `ndbd_support_new_hash_function`, `NdbDictionaryImpl.cpp:4014`,
   `Ndb.cpp:431-611`, `:565`.
3. RonDB fanout tables hash a base prefix and a detail suffix separately
   and combine `((base/fanout)*fanout) + (detail % fanout)`. Verify:
   `Ndb.cpp:357-425`.
4. Hash → fragment: for hash-map tables `hash_map[hash % hash_map_len]`;
   linear-hash and `DistrKeyHash` variants exist for old tables. Verify:
   `src/ndbapi/NdbDictionary.cpp:778-794`.
5. Fragment → replica nodes from the table's fragment data; with RonDB
   dynamic primary replicas the primary may be recomputed from node state.
   Verify: `NdbDictionaryImpl.cpp:2455`.
6. Node choice: location domain, then same host, then primary replica;
   fully-replicated tables: any node; read-backup tables: any replica of
   the fragment. Verify: `Ndb.cpp:786-873`.

This whole chain lives in `ic_apid::hash` and is driven by the
`IC_TRANSACTION_HINT`. The MD5 and XXH3 implementations are written
in-tree (both are public algorithms). XXH3 has to match the xxHash
library bit for bit, including its handling of short inputs; its test
vectors are the check.

As built (`ic_util::md5`, `ic_util::xxh3`, `ic_apid::hash`), checked
against the reference:
- The key buffer is each distribution key column in attribute order,
  as the record holds it: a fixed-size value in full, a variable-sized
  one behind its one or two length bytes with only the bytes it has,
  each padded with zeros to a word. A character column is first put
  through its collation's transform (`strnxfrm_hash`), which needs the
  MySQL collation tables; such a key is refused for now with
  `IC_ERROR_NOT_SUPPORTED`. Verify: `Ndb.cpp`, `computeHash` over an
  `NdbRecord`.
- Which word places the key: `values[1]`, the second word of the MD5
  digest or the high half of XXH3, exactly as step 2 says. Verify:
  `Ndb.cpp`, after `rondb_calc_hash`; `rondb_hash.cpp`.
- XXH3 is pinned by 37 published values of the algorithm, one on each
  side of every boundary between its paths, and MD5 by RFC 1321's own
  examples and the block boundaries. The XXH3 secret is the algorithm's
  standard 192 bytes.
- Fanout tables (step 3) are refused for now; the fields to do them
  (`PartitionHashBaseKeyCount`, `DetailKeyCount`, `Fanout`) are parsed.
- **Which nodes hold a fragment** comes with the description, in
  `ReplicaData` (key 138): 16-bit values, big-endian, the replica
  count and the fragment count, then per fragment its log part and the
  node of each replica, primary first as the distribution handler
  placed it. The dictionary asks the distribution handler for it only
  when answering `GET_TABINFOREQ`. Verify: `Dbdict.cpp`,
  `packTableIntoPages`; `DbdihMain.cpp`, `execCREATE_FRAGMENTATION_REQ`;
  `NdbDictionaryImpl.cpp`, where the log part is passed over.
- **Which replica is primary is not fixed** (steps 5 and 6 above). With
  dynamic primary replicas, which RonDB 26.10 has, and more than one
  replica alive, the data nodes deal the primaries of a node group's
  fragments over the group's alive nodes in node id order, in fragment
  order, so many fragments to each that every alive node gets an equal
  share (the fragment count divided by the alive count, rounded up);
  with one replica, or one alive, that one. The reference computes the
  same rule for itself, and so does `ic_apid::hash::primary_of`.
  Verify: `NdbDictionaryImpl.cpp`, `get_nodes` and
  `calculate_primary_replicas`.
- **Node choice as built** (`hash::choose_node`): a fully replicated
  table goes to any started node holding any fragment; a read-backup
  table, which every table a RonDB server makes is, to any started
  replica of the partition, taken in turn; any other table to its
  primary, or to a started replica if the primary is down. The
  reference first prefers a node in its own location domain, then one
  on its own host, and balances by a per-node count; none of that is
  done yet. A wrong choice only costs a hop: the coordinator forwards
  the operation to wherever the row is. Verify: `NdbImpl::select_node`;
  `ndb_cluster_connection.cpp`, `select_node`.
- The `FRAGMENT` pseudo column (0xFFFE), read with a header of size
  zero, answers with one word: the partition the row is in. `ic_read
  --partition` compares it with the computed one, which is how the
  chain is checked live: seen to agree (2026-09-22) on a RonDB 26.10
  table with an INT key, hashed by XXH3, over eight partitions. Verify:
  `DbtupRoutines.cpp`, `read_pseudo`.

### 6.4 TCKEYREQ (GSN 12)

Long form only (sections). Signal data, 8 fixed words: API connect ptr
(TC connect ptr from seize), sender data (our operation object id),
attrLen (total ATTRINFO words), tableId, requestInfo, tableSchemaVersion,
transId1, transId2. Optional words 9–12 depending on flags: scanInfo,
distribution hash value, distribution key size, stored procedure id.
Section 0 = KEYINFO, section 1 = ATTRINFO. Verify:
`include/kernel/signaldata/TcKeyReq.hpp:57-160`.

requestInfo bits: 0 dirty, 1 no-disk, 2 distribution-key present,
3 via SPJ, 4 commit, 5–7 operation type (0 read, 1 update, 2 insert,
3 delete, 4 write, 5 read exclusive, 6 refresh, 7 unlock), 8 simple,
9 queue on redo, 10 execute, 11 start transaction, 12–13 abort option,
14 scan takeover, 15 interpreted, 16–18 ATTRINFO words in signal (0 for
long form), 19 reorg, and RonDB additions 20 read-committed-base,
21 no-wait, 22 batch-safe, 23 batch-unsafe, 24 pass-queueing, plus
replica-applier, TTL and ring-buffer flags. Verify: `TcKeyReq.hpp:279-448`.
iClaustron documents bits 0–15 in prose in `ic_apid_handle_messages.ic`
under the name `NDB_PRIM_KEYREQ`; the stock GSN and the RonDB bits above
supersede it.

As built (`ic_ndb_signals::tc_key`), checked against the reference:

- **The optional words were wrong above.** After the eight fixed words
  come, each only if its flag is set and in this order: the user id and
  its version for RonDB's rate limits (flag bit 29, two words), the scan
  information (bit 14), the distribution key (bit 2). Nothing else is
  read from a long request. Verify: `NdbOperationExec.cpp`,
  `fillTcKeyReqHdr`; `DbtcMain.cpp`, `execTCKEYREQ`, where the words are
  found.
- **A long request must be exactly as long as its flags make it.** The
  coordinator checks the length against eight plus the optional words,
  and refuses anything else as a malicious signal. Verify:
  `DbtcMain.cpp`, `execTCKEYREQ`, the length check after the optional
  words.
- A committed read sets both the simple and the dirty flag; a simple
  read sets only simple. Verify: `NdbOperationDefine.cpp`, the lock mode
  switch.
- Operation types: read 0, update 1, insert 2, delete 3, write 4, read
  exclusive 5, refresh 6, unlock 7. Verify: `kernel_types.h`.

KEYINFO section: key column values in primary-key order, each padded to
4-byte alignment. Verify: `include/kernel/signaldata/KeyInfo.hpp:71-95`.

ATTRINFO section. For a plain read: a list of attribute headers (one word
each) naming the columns. The record path of the reference reads
**packed** instead, and so do we (`ic_apid::row_codec`, as the author
pointed out): one header, `READ_ALL` (0xFFF0) with the column count as
its size when every column is read, otherwise `READ_PACKED` (0xFFF3)
with a bitmask of the attribute ids after it. The row comes back behind
one `READ_PACKED` header whose size is a result bitmap's in bytes. The
bitmap has a bit per attribute id, set if read, and after each nullable
column read one more, set if NULL. The values follow in attribute
order, NULLs taking no room: a byte-sized type (characters, 8- and
16-bit elements) at the next byte, a 32- or 64-bit one at the next
word, bit columns packed together bit by bit from a word, and whatever
follows them after the last word they touched. A variable-sized value
starts with its one or two length bytes. There is no packed form for
writing: an update sends a header and value per column. Verify:
`NdbOperationExec.cpp`, `buildSignalsNdbRecord`; `DbtupRoutines.cpp`,
`read_packed` and `updateAttributes`; `NdbReceiver.cpp`,
`unpackNdbRecord` and `pad_pos`.

  For a write, the section is a header and value per column, the
  value's length in bytes in the header and the value padded to words;
  a length of zero means NULL and no value follows. A key column is
  sent with the value the key section has, so that the two agree. A
  request with nothing to send, such as a delete, carries the key
  section alone. Verify: `NdbOperationExec.cpp`,
  `buildSignalsNdbRecord`, its final update words, and `doSendKeyReq`,
  which sends one section or two. For a plain write: attribute header + value
words per column. For interpreted operations and all scans the section
starts with five length words: (1) reads before the program, (2) the
program, (3) updates after, (4) reads after, (5) subroutines; each part is
attribute headers plus data. Verify:
`include/kernel/signaldata/AttrInfo.hpp:33-97`,
`src/ndbapi/NdbOperationExec.cpp:1345-1400`.

Attribute header word: bits 16–31 attribute id, bit 15 partial flag,
bits 0–14 byte length; length 0 means NULL. Pseudo-columns
`FRAGMENT 0xFFFE`, `ROW_COUNT`, `RANGE_NO 0xFFFB`, `READ_PACKED 0xFFF3`,
`READ_ALL 0xFFF0`, `ROW_GCI64`, `LOCK_REF`, `OP_ID`, RonDB `AGG_RESULT
0xFF00`, `APPEND_COLUMN 0xFC00`, `SET_PARTIAL_COLUMN 0xFC01`. Verify:
`include/kernel/AttributeHeader.hpp:39-196`.

### 6.5 Replies

- `TCKEYCONF` (GSN 10): API connect ptr, gci_hi, confInfo, transId1,
  transId2, then per operation `{operation object id, attrInfoLen}`, then
  gci_lo. confInfo: bits 0–15 number of operations, bit 16 commit flag,
  bit 17 commit-ack marker. `attrInfoLen` bit 31 = dirty read (low bits
  then carry the responding node). Arrives to `API_PACKED` in packed form
  (up to 6 sub-signals, each sub-header `len = (w & 0x1F) + 3`, block in
  bits 16–31) or unpacked to our block. Verify:
  `include/kernel/signaldata/TcKeyConf.hpp:36-116`,
  `src/ndbapi/TransporterFacade.cpp:288-321`.
- `TCKEYREF` (GSN 11): connect ptr, transId1, transId2, errorCode,
  errorData. RonDB rate-limit errors 243 and 2203. Verify:
  `TcKeyRef.hpp:34-59`.

  **A refusal is sent only for an operation that is allowed to fail**,
  that is one sent with `IgnoreError`, as a committed read is. An
  operation sent with `AbortOnError`, as a write is, gets none: the
  coordinator aborts the transaction, and the error comes in
  `TCROLLBACKREP` (GSN 16) instead, five words naming the transaction:
  our pointer, the transaction id, the error and its data. A
  transaction therefore waits for that report as well as for the
  confirmation. Found live (2026-09-22): an insert of a key that was
  already there waited for a refusal that never came. Verify:
  `DbtcMain.cpp`, where a refusal is sent only when an error is allowed
  and `TCKEY_abort` is taken otherwise, and where the rollback report is
  sent with the transaction's return code; `TcRollbackRep.hpp`.
- `TRANSID_AI` (GSN 5): row data (attribute header + data words) for one
  operation, first word = our operation object id; long form in section 0.
  May arrive **before or after** the matching `TCKEYCONF`.

  Seen live (2026-09-21) with `ic_read` against RonDB 26.10: a
  committed read of a row by its key, of a row with a NULL column, and
  of a key with no row, each answered as this section says. With
  `ic_write` (2026-09-22): an insert, an update and a delete of a row,
  each confirmed, and a key already there refused with 630 through the
  rollback report.
- `KEYINFO20` (GSN 33): key of a scanned row when scan takeover was
  requested.
- Commit-ack: when `TCKEYCONF` bit 17 is set, send `TC_COMMIT_ACK`
  (GSN 469) back to TC. Verify: `Ndbif.cpp:493-520`.

As built (`ic_ndb_signals::tc_key`, `packed`, and the receive thread),
with what the reference showed on the way:

- **Every reply names its receiver in its first word**, which the
  reference states as a rule for all traffic signals. `TCKEYCONF` names
  the transaction by the pointer given at seize time, or is RNIL, and
  the transaction is then found from the operations it lists.
  `TCKEYREF` and `TRANSID_AI` name the operation by the pointer the
  request gave it. Verify: `Ndbif.cpp`, the dispatch of each.
- **`TRANSID_AI` comes from the node that read the row**, which need
  not be the coordinator's node. Its three header words are the
  operation pointer and the transaction id; a long one has the row in
  section 0, and one sent in fragments has a fourth word, the row's
  whole length. Verify: `TransIdAI.hpp`.
- **Per operation, `TCKEYCONF` gives the words of row data to expect.**
  With the top bit set it is a dirty read, and the low bits name the
  reading node. An operation is complete when its `TRANSID_AI` words
  reach the count, or at the first `TRANSID_AI` of a dirty read; with no
  words expected, `TCKEYCONF` alone completes it. A dirty read whose
  reading node fails before its row arrives fails with 4119. Verify:
  `NdbReceiver.hpp`, `execTCOPCONF`; `NdbReceiver.cpp`,
  `execTRANSID_AI`; `NdbTransaction.cpp`, `receiveTCKEYCONF`.
- The commit marker counts only when the commit bit is set too, and
  the low word of the global checkpoint follows the operations only
  when the signal is long enough to hold it. Verify: `TcKeyConf.hpp`,
  `getMarkerFlag`; `receiveTCKEYCONF`.
- **`TC_COMMIT_ACK` is two words, the transaction id,** sent to the
  block that sent the `TCKEYCONF`, and sent even when the confirmation
  is otherwise rejected. Verify: `NdbTransaction.cpp`,
  `sendTC_COMMIT_ACK`; `Ndbif.cpp`.
- **Both `TCKEYCONF` and short `TRANSID_AI` arrive packed.** A signal
  to `API_PACKED` (2047) holds several signals of its own signal number,
  each behind a header word with its length less three in bits 0–4 and
  its block in bits 16–31. The receive thread takes it apart and routes
  each part by its own block. Verify: `TransporterFacade.cpp`, where
  `API_PACKED` is taken apart; `DbtcMain.cpp`, `sendPackedTCKEYCONF`;
  `DbtupBuffer.cpp`, `sendAPI_TRANSID_AI`.

### 6.6 Completion accounting (the rule that makes `poll` correct)

Per operation keep `expected_len` (from TCKEYCONF) and `received_len` (from
TRANSID_AI), both initially unknown. The operation is complete when both
are known, regardless of arrival order. A TCKEYREF completes it with an
error. A transaction's `execute` is complete when completed operations
equals sent operations, and commit state is taken from the TCKEYCONF
commit flag. Verify: `include/ndbapi/NdbReceiver.hpp:400`,
`src/ndbapi/NdbTransaction.cpp:2785-2853`, `NdbOperationExec.cpp:1698`.

### 6.7 Commit and abort without operations

`TC_COMMITREQ` (GSN 19) `{TC connect ptr, transId1, transId2}` →
`TC_COMMITCONF` (GSN 17) `{connect ptr (bit 0 = ack expected), transId1,
transId2, gci_hi, gci_lo}` or `TC_COMMITREF`. `TCROLLBACKREQ` (GSN 15) →
`TCROLLBACKCONF` (13) or `TCROLLBACKREF` (14). `TCROLLBACKREP` (GSN 16) `{connect ptr, transId,
returnCode, errorData}` is a TC-initiated abort. As built
(`ic_ndb_signals::tc_key`): the two requests are three words, our
pointer and the transaction id; `TCROLLBACKCONF` is those three back;
`TC_COMMITREF` adds the error code, and `TCROLLBACKREF` that and the
coordinator's state. Verify: `DbtcMain.cpp`, `execTC_COMMITREQ`,
`execTCROLLBACKREQ` and where each reply is sent. Node failure while a
transaction is in flight: `TCKEY_FAILCONF` (8) / `TCKEY_FAILREF` (9).
RonDB sends `TC_DEADLOCK_REP` before an abort caused by deadlock
detection. Verify: `TcCommit.hpp:37-70`, `TcRollbackRep.hpp:33`,
`NdbTransaction.cpp:1748-1800`, `:2876-2940`, `Ndbif.cpp:1400`.

Execute semantics we must reproduce: the first operation of a transaction
carries the start flag; the last operation in a `Commit` execute carries
the commit flag; the last operation of every send train carries the
execute flag. Abort option: `AbortOnError` default for writes,
`IgnoreError` default for reads; committed reads only allow
`IgnoreError`.

### 6.8 Unique index operations

`TCINDXREQ` (GSN 519), `TCINDXCONF` (520) and `TCINDXREF` (521) have exactly the `TCKEYREQ` family
layout; the key sent is the unique index key; `INDXKEYINFO`/`INDXATTRINFO`
are the short-form trains (unused by us). Verify:
`include/kernel/signaldata/TcIndx.hpp:34`,
`src/ndbapi/NdbIndexOperation.cpp:35-178`.

## 7. Scans

- `SCAN_TABREQ` (GSN 32), 11 fixed words: API connect ptr, attr/key
  length word, requestInfo, tableId (the **index** id for ordered index
  scans), schema version, stored procedure id, transId1, transId2, buddy
  connect ptr, batch byte size, first batch size; optional distribution
  key, RonDB TTL purge window, user id, user id version, parallelism.
  Sections: 0 = receiver ids (one per parallel fragment scan, which is how
  parallelism is expressed), 1 = ATTRINFO, 2 = KEYINFO (bounds). Verify:
  `include/kernel/signaldata/ScanTab.hpp:39-133`.
- requestInfo bits: 8 lock mode, 9 no-disk, 10 hold lock, 11 read
  committed, 12 keyinfo (request KEYINFO20 for takeover), 13 tuple scan,
  14 descending, 15 range scan, 16–25 batch size (rows, max 1 023),
  26 distribution key, 27 via SPJ, 28 pass all confs, 29 extended conf,
  30 read-committed-base, 31 multi-fragment; RonDB bits 0–7 for ring
  buffer meta, join aggregation, parallel ordered scan, TTL, user id,
  pass queueing, aggregation. Verify: `ScanTab.hpp:210-250`.
- `SCAN_TABCONF` (GSN 29): connect ptr, requestInfo (bit 31 = end of
  data), transId1, transId2, then per fragment `{apiPtrI, tcPtrI, rows,
  len}` (or a 3-word compact form); `tcPtrI == RNIL` means that fragment is
  finished. Verify: `ScanTab.hpp:653-688`.
- `SCAN_TABREF` (GSN 31): connect ptr, transId, errorCode, closeNeeded.
- `SCAN_NEXTREQ` (GSN 28): connect ptr, stopScan, transId1, transId2, then
  the `tcPtrI`s being acknowledged (inline if ≤ 21 words, else section 0).
  Acknowledging releases the previous batch's locks unless taken over.
  Verify: `ScanTab.hpp:730-786`.
- Row data arrives as `TRANSID_AI` addressed to the receiver ids.
- Batch sizing from config `BatchSize`, `BatchByteSize`,
  `MaxScanBatchSize` and the row width. Verify:
  `include/ndbapi/NdbReceiver.hpp:132-159`.
- Ordered index bounds (KEYINFO section): per bound part a bound-type
  word (`LE 0, LT 1, GE 2, GT 3, EQ 4`), then an attribute header using
  the column's *index* attribute id, then the value words. An open bound
  is `LE` with a NULL header for attribute 0. For multi-range scans the
  first word of each range also carries the bound length (bits 16–31) and
  the range number (bits 4–15, max 0xFFF). Verify:
  `include/ndbapi/NdbIndexScanOperation.hpp:89-178`,
  `src/ndbapi/NdbScanOperation.cpp:3900-3951`, `KeyInfo.hpp:71-95`.
  This is the target of `IC_RANGE_CONDITION`.
- Takeover (update/delete a scanned row): a `TCKEYREQ` whose KEYINFO is the
  raw `KEYINFO20` payload, with the scan-takeover flag and scanInfo word
  (take-over info and fragment) set. Verify:
  `NdbScanOperation.cpp:3295-3358`.

## 8. Dictionary

- `GET_TABINFOREQ` (GSN 24): senderData, senderRef, requestType (0 by id,
  1 by name, +2 for long-signal conf), tableId or name length,
  schemaTransId; name in section 0. `GET_TABINFO_CONF` (GSN 190):
  senderData, tableId, gci, totalLen, and the table description in
  section 0 (fragmented if large). `GET_TABINFOREF` (GSN 23) with error
  codes 709, 723, 701. Verify:
  `include/kernel/signaldata/GetTabInfo.hpp:38-110`,
  `src/ndbapi/NdbDictionaryImpl.cpp:3605-3629`. iClaustron has the
  structs and GSNs but never sends the request.

  As built (`ic_ndb_signals::get_tab_info`, `ic_apid::dict_client`),
  each point checked against the reference:
  - The name is the internal `database/def/table`. The length word
    counts a terminating NUL, and section 0 is the name, the NUL and
    zero padding to whole words, in native byte order since the receiver
    reads it as bytes. The request goes to `DBDICT` on any started node,
    with requestType 3 and schemaTransId 0.
  - The answer's six words are senderData, tableId, gci, totalLen in
    words, tableType, senderRef. `GET_TABINFOREF` is seven words, with
    the error code sixth and the source line seventh; an older data node
    sends five with the code last.
  - The reference asks up to a hundred times, 50 to 100 ms apart with a
    random spread, on Busy (701), on the node failing mid-request, and
    on a timeout, each time to an alive node. 723 and 709 mean no such
    table and are final.
  - After parsing, a table placed by hash map needs its hash map, fetched
    by id the same way (requestType 2, the id in word 4, no section).
    As built in `dict_client::get_hash_map` and
    `dict_tab_info::parse_hash_map_info`:
    - Keys: name 1, bucket array size 2, bucket array 3; the map's id and
      version reuse the table keys `HashMapObjectId` (153) and
      `HashMapVersion` (154). Every value defaults to zero, and there is
      no end marker.
    - **The bucket count is sent as a size in bytes**, two per bucket;
      the reference halves it after reading. The buckets are 16-bit
      fragment numbers copied as the data node holds them, so they are
      read in the receiver's own byte order.
    - A row goes to fragment `buckets[hash % buckets.len()]`, the hash
      taken over the distribution key. `DEFAULT-HASHMAP-3840-8` is 3840
      buckets over 8 fragments.
    Verify: `NdbDictionaryImpl.cpp`, `get_hashmap` and
    `parseHashMapInfo`; `DictTabInfo.cpp`, `DictHashMapInfo::Mapping`;
    `NdbDictionary.cpp`, `Table::getPartitionId`.
- SimpleProperties encoding (network byte order): head word
  `(valueType << 16) | key`; `Uint32 = 0` one word, `String = 1` and
  `Binary = 2` length word then padded bytes, `Uint64 = 4` two words low
  then high. Verify: `include/util/SimpleProperties.hpp:47-95`,
  `src/common/util/SimpleProperties.cpp:36-204`. iClaustron's
  `fill_create_table_info_properties` writes this format.

  **Two byte orders in one buffer.** Heads, numbers and lengths are
  stored with `htonl` into words of their own; string bytes are copied
  in raw. The words then travel in the sender's order like any signal
  word. So a receiver of the same byte order reads a number as
  `from_be` of the native word and a string as the native bytes of its
  words. A string's length counts its terminating NUL. The reader skips
  keys it does not know (`unpack`, `ignoreUnknownKeys`).
- DictTabInfo keys: table keys 1–30 and 127–171 (`TableName 1, TableId 2,
  TableVersion 3, NoOfKeyAttr 5, NoOfAttributes 6, KeyLength 12,
  FragmentTypeVal 13, TableTypeVal 18, PrimaryTableId 20,
  FragmentCount 128, FragmentData 130, HashMapObjectId 153,
  HashMapVersion 154, ReadBackupFlag 158, FullyReplicatedFlag 159,
  PartitionCount 160`; RonDB `HashFunctionFlag 163, TTLSec 164,
  TTLColumnNo 165, PartitionHashBaseKeyCount 166,
  PartitionHashDetailKeyCount 167, PartitionHashFanout 168,
  RingBufferSize 169, RingIdxColumnNo 170, RingMetaColumnNo 171`),
  `TableEnd 999`; attribute keys 1000–1021 (`AttributeName, AttributeId,
  AttributeSize 1003, AttributeArraySize 1005, AttributeKeyFlag 1006,
  AttributeStorageType 1007, AttributeNullableFlag 1008, AttributeDKey
  1010, AttributeExtType 1013, ExtPrecision/Scale/Length 1014–1016,
  AutoIncrement 1017, ArrayType 1019, DefaultValueLen/Value 1020–1021`),
  `AttributeEnd 1999`. **Only non-default values are sent, so the default
  table is part of the protocol.** Verify:
  `include/kernel/signaldata/DictTabInfo.hpp:97-663`. iClaustron's
  `ic_apid_dict_signals.h` already lists keys and defaults.

  How the reference reads it, and so how `ic_ndb_signals::dict_tab_info`
  does:
  - **The table part ends at the first `AttributeName`**, which is the
    break key of the table mapping; `TableEnd` is not in it and is
    skipped like any unknown key. Each attribute then runs from its
    `AttributeName` to `AttributeEnd`, and there must be exactly
    `NoOfAttributes` of them.
  - **The defaults are the ones `Table::init` and `Attribute::init` set,
    not the ones the comments beside the keys name.** The comments are
    out of date in several places: the default fragment type is
    hash-map partitioning (9), not a small table; `NoOfKeyAttr` starts
    at 0, not 1; `MinLoadFactor` is 78, not 70. An attribute defaults to
    type Unsigned, 32-bit elements, one element, fixed array, in memory.
  - **Sizes are worked out, not read.** `translateExtType` sets the
    element size and count from type, length, precision and scale, and
    the parser calls it for every attribute. Decimals use MySQL's
    `decimal_bin_size` (four bytes per nine digits either side of the
    point, less for the rest), with precision at most 65 and scale below
    31.
  - The character set number is the high half of `AttributeExtPrecision`.
    Char, Varchar, Longvarchar and Text must have one and nothing else
    may, or the table is refused.
  - A default value is binary: a four-byte attribute header in network
    order whose low 15 bits are the value's byte size, then the value.
    **Every column is sent one; a size of zero means no default.** The
    value is not in network order, whatever the comment beside the code
    says: `convertByteOrder` only acts on a big-endian host (or twice,
    cancelling out, under `VM_TRACE`), so on a little-endian API the
    bytes are used as the data node stored them. Verify:
    `AttributeHeader.hpp`, `getByteSize`; `NdbSqlUtil.cpp`,
    `convertByteOrder`. Found live: a first `ic_desc` printed a default
    on every column.
  - **No column marked as distribution key means the whole primary key
    is.** A table created without a partitioning clause sends no
    `AttributeDKey` at all; the reference then marks every primary key
    column ("none is all"), and does the same when all of them are
    marked. Key hashing depends on it. Verify: `NdbDictionaryImpl.cpp`,
    `NdbTableImpl::computeAggregates`. Found live: `ndb_desc` showed
    `DISTRIBUTION KEY` where a first `ic_desc` did not.
  - The "frm data" `ndb_desc` counts is whichever of `FrmData` (27) or
    `MysqlDictMetadata` (30) arrived; the reference keeps either as the
    same blob. Verify: `NdbTableImpl::IndirectReader`.
- **A table's schema version is two counters** (author, 2026-09-19):
  bits 24–31 go up by one for every online change, bits 0–23 for every
  offline change. Operations are checked against bits 0–23 only, so an
  online change leaves operations prepared with the previous version
  working, while an offline change makes them fail with a wrong schema
  version. `ndb_desc` prints the whole word: seen live, a table altered
  once online reads 16777217, which is 0x01000001. `ic_desc` prints the
  two apart, as upper 1, lower 1 (`TableInfo::version_upper` and
  `version_lower`). For the table cache
  (below) this means an online change is not noticed through errors;
  the API learns of it from the dictionary's notice (below) and
  refetches. Verify: `Dbdict.cpp`,
  `alter_obj_inc_schema_version` and `create_obj_inc_schema_version`;
  `kernel_types.h`, `table_version_major`; `DbtcMain.cpp`,
  `TableRecord::getErrorCode`; `DbspjMain.cpp` checks the same way.
- After parsing a hash-map table, fetch its hash map object (also via
  `GET_TABINFOREQ` by id) to fill the hash → fragment array. Verify:
  `NdbDictionaryImpl.cpp:3703-3714`.
- Cache: per-connection local cache plus a global, mutex-protected,
  ref-counted cache keyed by internal name `db/schema/table`; schema
  change detected by table version mismatch (errors 241/284) forcing a
  refetch. Verify: `src/ndbapi/DictCache.hpp:64-95`.

  As built (`ic_apid::dict_cache`, `ApidConnection::table_bind` and
  `index_bind`), with what was found in the reference on the way:
  - **The dictionary tells API nodes when a table changes.**
    `ALTER_TABLE_REP` (GSN 606) goes to every API node's cluster
    manager block (0x0FA2), unasked. Three words, table id, table
    version and kind of change (1 altered, 2 dropped), and the internal
    name in section 0, NUL-padded to 128 bytes. The version is the one
    that no longer holds, the one before an alteration, and the name is
    the one before a rename. Verify: `AlterTable.hpp`, `AlterTableRep`.
  - An alteration is sent by every data node, through its own cluster
    manager, to every API node registered there, so it arrives once per
    data node. Verify: `Dbdict.cpp`, `alterTable_fromCommitComplete`;
    `QmgrMain.cpp`, `execAPI_BROADCAST_REP`.
  - A drop is sent by the master alone, and only to API nodes whose
    reported version is 26.05 or later, or 26.02.5 and 25.10.14 in
    their series. The code, not its comment, decides: 26.04 does not
    get it. We report 26.10. Verify: `Dbdict.cpp`, `dropTable_commit`;
    `ndb_version.h.in`, `ndbd_support_drop_table_notification`.
  - The reference lets go of the cached object whose name, id and
    version all match, and marks a name being fetched so that what the
    fetch brings back is not trusted. Verify: `ClusterMgr.cpp`,
    `GSN_ALTER_TABLE_REP`; `DictCache.cpp`, `alter_table_rep`. Here the
    fetch is repeated only if it brought back the version a notice
    named, so the copies from the other data nodes cost nothing.
  - When the last data node link goes, the reference lets go of
    everything cached except fetches under way. Verify:
    `ClusterMgr.cpp`, where the count of connected nodes reaches zero;
    `DictCache.cpp`, `invalidate_all`.
  - An index's internal name is `sys/def/<table id>/<index>`, or
    `<database>/<schema>/<table id>/<index>` for one made by an older
    version, which is tried when the first is not found. The index
    object remembers the table id and version it was made for, and one
    found for another version is fetched again. Here the index holds
    that table object itself, so the version lives as long as the index
    does, and the cache lets go of a version's indexes with it. Verify:
    `NdbDictionaryImpl.hpp`, `getIndexGlobal`; `NdbDictionaryImpl.cpp`,
    `internal_index_name`, `old_internal_index_name`,
    `create_index_obj_from_table`.
- `LIST_TABLES_REQ` (GSN 193) / `CONF` (194) for listing.

  As built (`ic_ndb_signals::list_tables`, `dict_client::list_dependents`):
  the request is five words, senderData, senderRef, a flags word, table
  id, table type. The flags word asks for names (bit 28), indexes only
  (29) or the dependents of one table (30), and repeats the table id in
  its low 12 bits for data nodes that only read the older form. The
  answer is senderData and a count, with three words per object in
  section 0 (flags with store, temporary and state, then id, then type)
  and, when asked for, the names in section 1, each a byte length
  counting its NUL and then the name. Every current data node answers
  in this long form (`listObjects` always expects it). The dictionary
  splits a long answer by hand; see §2.1.

  A table's indexes are its dependents of type unique hash index (3) or
  ordered index (6). Each is fetched as a table of its own; its last
  column is a hidden reference to the table's row (`NDB$PK` or
  `NDB$TNODE`) and is not one of the index's columns. Index names are
  `sys/def/<table id>/<name>`, or `<db>/<schema>/<table id>/<name>` for
  indexes made before that form; the name is the part after the last
  separator. The primary key is not an index of its own: `ndb_desc`
  prints it as a unique hash index made from the table's key columns.
  Verify: `NdbDictionary.cpp`, the `-- Indexes --` section of `print`;
  `NdbDictionaryImpl.cpp`, `create_index_obj_from_table` and
  `internal_index_name`; `Ndb.cpp`, `externalizeIndexName`.
- DDL: `SCHEMA_TRANS_BEGIN_REQ/CONF` → `CREATE_TABLE_REQ` (+ DictTabInfo
  section) / `CREATE_HASH_MAP_REQ` / `CREATE_INDX_REQ` / `DROP_TABLE_REQ`
  / `ALTER_TABLE_REQ` → `SCHEMA_TRANS_END_REQ/CONF`. Verify:
  `include/kernel/signaldata/SchemaTrans.hpp:34-76`,
  `CreateTable.hpp:34-62`. iClaustron implements this path end to end
  (`execute_meta_data_transaction`) and it is carried over.

### 8.1 Column types and storage encodings

Types (`NDB_TYPE_*`): Tinyint, Tinyunsigned, Smallint, Smallunsigned,
Mediumint, Mediumunsigned, Int, Unsigned, Bigint, Bigunsigned, Float,
Double, Olddecimal, Olddecimalunsigned, Decimal, Decimalunsigned, Char,
Varchar, Binary, Varbinary, Datetime, Date, Blob, Text, Bit, Longvarchar,
Longvarbinary, Time, Year, Timestamp, Time2, Datetime2, Timestamp2.
Encodings: Char/Binary fixed; Varchar/Varbinary 1 length byte + data;
Longvarchar/Longvarbinary 2 little-endian length bytes + data; Decimal
packed binary of `decimal_bin_size(precision, scale)` bytes; Bit as a bit
array of `array_size` bits; Date 3 bytes, Time 3, Datetime 8, Timestamp 4,
Year 1; Time2 3 + frac, Datetime2 5 + frac, Timestamp2 4 + frac where frac
= `(scale + 1) / 2` bytes and scale is carried in precision. Verify:
`include/ndbapi/NdbDictionary.hpp:296-356`, `DictTabInfo.hpp:500-635`.
`IC_FIELD_TYPE` in `ic_apid_datatypes.h` must be extended to cover all of
these.

BLOB/TEXT: the main table holds a head plus inline data (V2 head 16 bytes:
u16 varsize, u16 reserved, u32 pkid, u64 length, little-endian); the rest
is in a parts table `NDB$BLOB_<tableid>_<colno>` with columns = main PK,
`NDB$PART`, `NDB$PKID`, `NDB$DATA`, sharing the main table's partitioning.
Verify: `include/ndbapi/NdbBlob.hpp:166`, `src/ndbapi/NdbBlob.cpp:72-735`.
BLOB support is not scheduled; records must leave BLOB columns out.

## 9. Interpreted programs

The kernel interpreter is a register machine with 8 registers, labels,
subroutines, `read_attr`/`write_attr`, `load_const_*`, arithmetic,
`branch_*` (register vs register) and `branch_col_*` (column vs constant,
including LIKE, NOT LIKE and bit masks), `interpret_exit_ok/nok/last_row`.
RonDB adds memory-region instructions, string and integer conversions,
searches and sorting, and `write_interpreter_output`/
`read_interpreter_input`. Verify: `include/ndbapi/NdbInterpretedCode.hpp:80-757`,
`include/ndbapi/NdbScanFilter.hpp:42-105`.

The `IC_WHERE_CONDITION` and `IC_CONDITIONAL_ASSIGNMENT` builders map
onto the **full 26.10 instruction set**: memory addresses → the 8
registers (more than 8 live values is an error in 0.1; a spill strategy
using memory regions `load_const_mem`, `read_*_to_reg_*`,
`write_*_reg_to_mem_*` is decided later); subroutine ids →
labels/subroutines; `ic_define_boolean` → short-circuit branches;
comparators → `branch_col_*` when one side is a constant and `branch_*`
after `read_attr` otherwise; `ic_define_like` → `branch_col_like`; string
operations → `string_search` and the conversion instructions. Regexp has
no kernel counterpart and is evaluated API-side after the row is received
(documented limitation). The exact opcode encodings are read from
`src/ndbapi/NdbInterpretedCode.cpp` and `include/kernel/Interpreter.hpp`
when Phase 6 starts and recorded here as §9.1.

## 10. Errors

Error records have: code, MySQL code, classification (18 values:
NoError, ApplicationError, NoDataFound, ConstraintViolation, SchemaError,
UserDefinedError, InsufficientSpace, TemporaryResourceError,
NodeRecoveryError, OverloadError, TimeoutExpired, UnknownResultError,
InternalError, FunctionNotImplemented, UnknownErrorCode, NodeShutdown,
SchemaObjectExists, InternalTemporary) and a derived status (Success,
TemporaryError, PermanentError, UnknownResult). Verify:
`src/ndbapi/ndberror.cpp:34-1249`, `include/ndbapi/ndberror.h:64`.
Our table keeps code, MySQL code and classification (interface data) and
carries our own message text. `IC_ERROR_SEVERITY_LEVEL` maps from status
and `IC_ERROR_CATEGORY` from classification.

**Never report more than is known.** The C++ API answers 4009, "cluster
failure", in states that are not one, such as a single broken link to a
node that is still up, and an application cannot act sensibly on that.
Our errors name what we actually know: `IC_ERROR_LINK_LOST` when our
connection to one node broke, `IC_ERROR_NODE_DOWN` when a data node has
reported a node failed, and a cluster-level error only when no data node
is connected at all. The first two are temporary errors that invite a
retry on another node.

## 11. Events (0.3)

`CREATE_EVNT_REQ`, `SUB_START_REQ` `{senderRef, senderData,
subscriptionId, subscriptionKey, part, subscriberData, subscriberRef,
requestInfo}` → `SUB_START_CONF` with first GCI and bucket count; data as
`SUB_TABLE_DATA` (GSN 586) with sections attr info, after values, before
values; epoch boundaries `SUB_GCP_COMPLETE_REP` (GSN 593) acknowledged with
`SUB_GCP_COMPLETE_ACK`. Verify: `include/kernel/signaldata/SumaImpl.hpp:98-461`,
`src/ndbapi/Ndbif.cpp:1240-1260`. Fragmented signals are used here.

## 12. Autoincrement (0.3)

System table `sys/def/SYSTAB_0` keyed by table id, column `NEXTID`;
ranges are allocated with an interpreted update (`incValue`) inside a
transaction hinted with the table id as key. Verify: `Ndb.cpp:1646-1663`.

## 13. Hard limits to encode as constants

`NDB_MAX_NO_OF_ATTRIBUTES_IN_KEY 32`, `NDB_MAX_ATTRIBUTES_IN_TABLE 4096`,
`NDB_MAX_TUPLE_SIZE_IN_WORDS 18000`, `NDB_MAX_KEYSIZE_IN_WORDS 1023`,
`NDB_MAX_ACTIVE_EVENTS 100`, `NDB_MAX_SCANFILTER_SIZE_IN_WORDS 15359`,
`MAX_GSN 982`. Verify: `include/ndbapi/ndbapi_limits.h:30-45`,
`GlobalSignalNumbers.h:40`.

## 14. Node ids, and why nothing is sized by them

The highest node id a cluster allows has risen with every era of the
product: 255 in NDB 7.2, which is what the C code assumed, 2039 in
RonDB 26.10 (`ndb_limits.h:70`, `MAX_NODES_ID`), and 8191 in a coming
release. Data nodes stay in the low numbers (`MAX_DATA_NODE_ID 144`),
but API nodes are routinely given large ids: a stock test cluster hands
out 1271 and 1600.

Two consequences for this library.

- **Nothing is sized by the limit.** Anything indexed by node id is a
  map, or a vector sized from the configuration actually received. A
  cluster with larger ids then needs no code change and no rebuild.
  `IC_MAX_NODE_ID` exists only as a sanity bound where no configuration
  is available yet, such as reading a connectstring, and even there the
  check is against `IC_MAX_NODE_ID_WIRE`, the 16 bits a block reference
  gives the node id, which cannot change without changing the protocol.
- **Node bitmaps in signals grow with it.** The connected-node bitmap in
  `API_REGCONF` and the failed-node bitmap in `NODE_FAILREP` are sized
  by the cluster's node id limit, so they long ago outgrew the 25 data
  words a signal carries and travel in a section. Any decoder for them
  must take the length from the signal rather than assume one.
  Verify when Phase 3 reaches them: `ClusterMgr.cpp:2304`, which already
  handles three different encodings of the same bitmap.


## 15. Delta from NDB 7.2.9 (iClaustron's baseline) to RonDB 26.10

Everything the C code "knows" about the protocol dates from 7.2.9 and must
be treated as a hypothesis. Known differences the port must apply:

| Area | 7.2.9 as coded in iClaustron | RonDB 26.10 |
|---|---|---|
| Socket auth | `ndbd` / `ndbd passwd` → `ok` | TLS-capable line `ndbd TLS disabled` + empty line → `ok`/`Cleartext ok` (legacy possibly still accepted, Q2) |
| Node id hello | `"<nodeid> 1"` (2 integers) | `"<nodeid> 1 <remote nodeid> 0"` (4 integers), max 23 chars; reply `"<nodeid> 1"` |
| Byte order | swap on receive | no swap; reject mismatch |
| Signals | private renamed GSNs for a patched kernel | stock GSNs (§3–8) |
| Key operations | `NDB_PRIM_KEYREQ` prose, short signal with inline KEYINFO/ATTRINFO trains | long-form `TCKEYREQ` with sections 0/1 only; new requestInfo bits 20–24 and RonDB flags |
| Config | `get config` v1, iClaustron cluster ids | `get config_v2` with `node:` argument; v2 blob with default sections; `Rdma` section type 7 |
| Config parameters | full 7.2 data-node table | API-node subset re-verified against 26.10 ids; new parameters (TLS, RDMA, location domains, active flag) |
| Heartbeat | `API_REGREQ` 3 words | same 3 words; `API_REGCONF` gained `minDbVersion`/`minApiVersion` and RonDB restart-barrier liveness rule |
| Node failure | `NODE_FAILREP` fixed bitmap | three formats incl. a long section; `NF_COMPLETEREP` gating of reconnect |
| Block numbers | `IC_NDB_MIN_MODULE_ID_FOR_THREADS 32768`, `IC_NDB_PACKED_MODULE_ID 2047` | unchanged; plus fixed `API_CLUSTERMGR 4002`; kernel blocks extended to `0x111` (query blocks) |
| Partitioning | hash maps (introduced 7.2) with MD5 | MD5 or XXH3 64-bit per table (`HashFunctionFlag`), fanout tables, read backup, fully replicated, dynamic primary replicas |
| Dictionary | DictTabInfo keys up to ~160 | keys to 171 (TTL, hash function, fanout, ring buffer); `GET_TABINFO_CONF` long signal; `MysqlDictMetadata` replaces frm data |
| Column types | up to Datetime/Timestamp | `Time2`, `Datetime2`, `Timestamp2` with fractional seconds; `Longvarchar` in indexes |
| Interpreter | 7.2 instruction set (registers, branches, LIKE) | plus memory regions, searches, sorting, conversions, interpreter I/O, partial column writes |
| Transporters | TCP, SHM, SCI | TCP, SHM, RDMA; multi-transporters (DB↔DB only); API uses TCP only |
| Version words | `IC_NDB_VERSION 0x080014` in `ic_base_header.h` | `0x1A0A00` |
