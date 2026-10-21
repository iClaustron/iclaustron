# 05 — NDB protocol reference for the implementation

This chapter is the clean-room specification the `ic_signals` and
`ic_apid` crates are written from. Each item states what the protocol
is, in our words. Since 2026-10-08 a fact is learned first from the
wire, captures read with our Wireshark dissector (chapter 06 §6.7), then
from the author, and only then from the source (chapter 09); an item
names the capture it came from where there is one. The RonDB source is
not cited by file and line, which change from time to time (the author,
2026-10-21): a fact read there says which part of RonDB does it, the
coordinator or the reference's client, say, and a mend names its RonDB
commit. The source is read for checking, never for copying (see
[09-licensing.md](09-licensing.md)). Where the iClaustron C code already
implements the item, that is noted too; that code *can* be carried over.

Version in scope: **RonDB 26.10 only** (the tree is 26.10.0). No
compatibility paths for older releases. iClaustron's C code was written
against NDB 7.2.9; §14 lists what changed since then so no 7.2-era
assumption survives unexamined.

## Names

This chapter follows chapter 09's rule (author, 2026-10-08): nothing is
taken from RonDB, names included. What travels on the wire is kept
exactly: numbers, word layouts, error codes, parameter ids, the
management protocol's text and the stored names we look up. Everything
else has a name of our own that says what it is: signals, blocks, a
signal's fields, dictionary keys, pseudo-columns and configuration
parameters. RonDB is named only in this chapter, by the part that does a
thing and by commit, never by file and line. A signal we have not named
is referred to by its number.

Signals, by number:

| Group | Number and our name |
|---|---|
| Membership | 3 `HEARTBEAT_REQ`, 1 `HEARTBEAT_CONF`, 2 `HEARTBEAT_REF`; 26 `NODE_LOST_REP`; 27 `NODE_LOST_DONE_REP`; 163 `NODE_JOINED_REP` |
| Coordinator records | 39 `COORD_ALLOC_REQ`, 37 `COORD_ALLOC_CONF`, 38 `COORD_ALLOC_REF`; 36 `COORD_FREE_REQ`, 34 `COORD_FREE_CONF`, 35 `COORD_FREE_REF` |
| Key operations | 12 `KEY_OP_REQ`, 10 `KEY_OP_CONF`, 11 `KEY_OP_REF`; 519 `UNIQUE_OP_REQ`, 520 `UNIQUE_OP_CONF`, 521 `UNIQUE_OP_REF`; 5 `ROW_DATA_REP`; 6 `KEY_DATA_ORD`; 4 `COLUMN_DATA_ORD` |
| Ending a transaction | 19 `COMMIT_REQ`, 17 `COMMIT_CONF`, 18 `COMMIT_REF`; 15 `ROLLBACK_REQ`, 13 `ROLLBACK_CONF`, 14 `ROLLBACK_REF`; 16 `TRANS_ABORTED_REP`; 469 `COMMIT_MARKER_FREE_ORD`; 8 `TAKEOVER_CONF`, 9 `TAKEOVER_REF`; 980 `DEADLOCK_FOUND_REP` |
| Scans | 32 `SCAN_REQ`, 29 `SCAN_CONF`, 31 `SCAN_REF`; 28 `SCAN_MORE_REQ`; 33 `SCAN_ROW_KEY_REP` |
| Dictionary | 24 `TABLE_DESC_REQ`, 190 `TABLE_DESC_CONF`, 23 `TABLE_DESC_REF`; 193 `LIST_OBJECTS_REQ`, 194 `LIST_OBJECTS_CONF`; 606 `TABLE_CHANGED_REP`; 984 `COLLATION_DESC_REQ`, 985 `COLLATION_DESC_CONF`, 986 `COLLATION_DESC_REF` |

Blocks, by number (hex): F4 `ARCHIVE`, F5 `COORDINATOR` (the
transaction coordinator), F6 `DISTRIBUTION`, F7 `LOCAL_OPS` (runs
operations on a fragment), F8 `HASH_INDEX`, F9 `ROW_STORE`, FA
`DICTIONARY`, FB `START_CONTROL`, FC `MEMBERSHIP`, FD `FILE_IO`, FE
`NODE_ADMIN`, FF `INDEX_BUILD`, 100 `UTILITY`, 101 `CHANGE_FEED`, 102
`ORDERED_INDEX`, 109 `JOIN`, 10A `LINK_ADMIN`, 10B–111
`READ_WORKER_FIRST`…`READ_WORKER_LAST`; outside the data nodes 7FF
`BUNDLE`, FA2 `NODE_WATCH`, FA3 `CONFIG_SERVICE`, and the user threads
from 8000 (`IC_FIRST_USER_THREAD_BLOCK`). The constants are `IC_SIG_<NAME>` and
`IC_BLOCK_<NAME>`.

Configuration parameters are named by id and our name: 3 `node_id`,
5 `host`, 101 `replica_count`, 119 `api_heartbeat_ms`, 129
`timeout_check_period`, 130 `idle_transaction_limit`, 131
`lock_wait_limit`, 406 `port`, 800 `scan_batch_bytes_total`, 801
`scan_batch_bytes`, 802 `scan_batch_rows`, 806 `hash_map_buckets`.
Dictionary keys are named by meaning and number (constants `IC_TP_*`).

## 1. Connecting to a data node

### 1.1 TCP and socket authentication

Plain TCP to the data node's transporter port (from the config's TCP
connection section; if the port is 0 the port is dynamic and must be asked
from the management server with `get connection parameter`). Then a text handshake,
lines terminated by `\n`.

Two dialects exist on the client side:

- Legacy: client sends `ndbd` then `ndbd passwd`, reads one line,
  requires it to start with `ok`. (This is what iClaustron implements in
  `authenticate_client_connection`.)
- TLS-capable (MySQL 8.3+/RonDB): client sends one of `ndbd TLS
  required`, `ndbd TLS enabled`, `ndbd TLS disabled`, then an empty
  line. Server answers one of `ok`, `Cleartext ok`, `TLS ok`, `Cleartext
  required`, `TLS required`, `Error`. On `TLS ok` both sides start TLS
  on the same socket. A bare `ndbd` is classified as "too old" by the
  TLS-capable server.

Decision: send the TLS-capable form with `TLS disabled` in 0.1, accept
`ok` and `Cleartext ok`. **Open question Q2**: confirm on a live 26.10
node whether the legacy form is still accepted, so the fallback can be
dropped or kept.

### 1.2 Node id hello

After authentication the client sends one line with four decimal
integers: `<my node id> <transporter type> <remote node id> <multi
transporter instance>`, where transporter type is 1 for TCP and the
instance is always 0 for an API node. The whole line must be at most 23
characters. The server replies `<its node id> <transporter type>` and
the client checks both. On rejection the server may write `BYE`.
iClaustron sends only the two-integer form (`"<nodeid> 1"`); the server
still parses 2, 3 or 4 integers, but we send four.

### 1.2a When the node says `BYE`

Instead of the two numbers, a data node may answer the hello with the
single word `BYE`. It does so when its transporter towards us is not in
the connecting state, which is the ordinary condition of a restarting
node that has not yet opened up to API nodes. It is not an
authentication failure. The node then waits for the client to close
first, so that the lingering socket state lands on the client. We return
`IC_ERROR_NODE_NOT_READY` and redial on the usual delay. Seen live once
in every node restart, between "connection refused" and the first
accepted hello.

### 1.3 Via the management server

The management server can convert a management connection into a
transporter connection: the client sends `transporter connect` and an
empty line, and then performs 1.2 on the same socket. The C cluster
server (out of scope) implemented the *server* side of this; we do not
use the mgm path.

### 1.3a How long a node id stays ours

The management server reserves an id when it grants one, for a limited
time, and drops the reservation as soon as the id is held by a connected
transporter, and also on `NODE_LOST_REP` or `NODE_LOST_DONE_REP` naming
it. From then on a request for an id is decided by asking the data
nodes, which refuse one that is connected. So an API node's claim on its
id, once it is connected, is its connections and nothing else. **An API
node that loses every connection can lose its id**, if the id was chosen
for it rather than named in its connectstring, because the next node
asking for any id may be given it (pointed out by the author,
2026-09-19). The reference's client claims an id once and never again.
We claim it again after losing every link and before dialling; see
chapter 02, "Node failure flow". A session may ask for an id more than
once; nothing in the server ties a session to one request.

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
failure flow"); the reference's client, which never asks again, simply
redials under the old id and is answered `BYE` until the data nodes time
the old connection out. Seen live: a restarting cluster answers "not
ready" for several seconds, which an earlier revision counted towards
giving the id up. An earlier revision of this section also claimed the
code told the first two apart; it does not.

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

Connect attempts are rate limited with a backoff; RonDB additionally has
a node "active" flag in config (parameter 13, `IC_PARAM_NODE_ACTIVE`)
and refuses connections to/from inactive nodes.

## 2. Signal framing

Every message is a sequence of 32-bit words in the **sender's native
byte order**. There is no byte swapping: the sender stamps its
endianness in the header and the receiver rejects a mismatch.
(iClaustron implemented a swap; it is unnecessary and we drop it.)

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

Word 2: bits 0–19 signal number (16-bit number plus a 4-bit version id), bits 20–25
trace, bits 26–27 number of sections.

Word 3: bits 0–15 sender block reference **number** (block number only;
the receiver reconstructs the full reference from the connection's node
id), bits 16–31 receiver block number.

iClaustron's `fill_ndb_message_header` and `create_ndb_message`
implement this correctly and are carried over.

Limits: a message is at most 32 768 bytes, sent or received; the data
nodes split what they send above 7 400 words (§2.1), which is where they
split, not a limit on what they receive; the receive loop handles up to
1 024 signals per call. A data node drops a signal whose data words and
sections come to more than 25.

Checksum: XOR of all words except the checksum word itself.

### 2.1 Fragmented signals

When the sections exceed the per-signal limit the sender emits several
signals with the same signal number and fragment info 1 (first), 2
(middle), 3 (last). **Every fragment carries the full original signal
data**, followed by one word per section it carries giving that
section's original number, then one word with the fragment id; its
sections are the pieces of the original sections. Sections are split on
60-word boundaries, and the highest-numbered section is sent first. The
receiver appends each carried piece to the original section its number
names and tells trains apart by (sender, fragment id).

An earlier revision of this section said that only the final fragment
carried the real signal data. That is wrong, and the reference's own
client depends on it being wrong: its handler of `TABLE_DESC_CONF` reads
the request id from every fragment.

**But not every fragmented signal repeats its data.** A block may split
an answer by hand, with the same trailing section numbers and fragment
id, and put values belonging to each piece in the data. The dictionary
does this for `LIST_OBJECTS_CONF`: each piece's object count counts only
that piece's objects, and the reference's client adds them up. A piece
may also carry one section, or none. A generic reassembler keeps one
piece's data (ours, like the data nodes', the last piece's), so such
values must come from the joined sections.

The shapes of `LIST_OBJECTS_CONF` (the author, 2026-10-09): two words
of its own, the caller's data and the object count, and then:

| Data words | Sections | After the two words |
|---|---|---|
| 2 | 0 | nothing |
| 3 | 0 | a fragment id |
| 4 | 1 | the section's number (0 object data, 1 names), a fragment id |
| 5 | 2 | 0 and 1, a fragment id |

That is the fragment trailer, sent with whole answers too. The fragment
id is random. A long answer goes in pieces of 240 words with the
fragment bits set, so its pieces are joined like any fragmented
signal's, each piece's section appended to the one its number names. A
piece of 240 words holds whole objects (three words each), but a name may
run on into the next piece.

Seen live (2026-10-09, all 172 answers of a capture): only whole
answers, with fragment bits 0, of 3 words without sections and 5 with
two. The section lengths follow the 5 words, where every signal's go:
18 and 48 for 6 objects, 21 and 56 for 7. `ic_signals::list_objects`
reads the first two words and the joined sections, and counts objects
by the length of section 0, so it reads every shape.

**When a signal is split.** The fragmenting send sends a signal whole
when its data and sections come to at most 7400 words, and otherwise
splits it into pieces of 3840 words in a release build, 120 in a debug
build. The one exception is `ROW_DATA_REP`, which a debug build splits
above 240 words. The comment above the piece size's definition says
splitting starts above the piece size; the code says otherwise, and a
first draft of this section, written from the comment, was wrong in the
same way.

So a table description arrives whole unless it is very large, and row
data from a debug data node can arrive in pieces. The API side must
reassemble at least `TABLE_DESC_CONF` and `ROW_DATA_REP`, not only event
traffic as an earlier revision said. `ic_apid::fragments` does it, in
the user thread that executes the signal. The largest whole signal,
7400 words, fits in our 32 KB receive limit.

### 2.2 Sending a signal in fragments (phase 6.6, 2026-10-12)

A request whose sections are too large for one message goes as a train
of fragments, laid out as the reference's API node lays it out, which
differs from the data nodes' layout in §2.1. Seen live (2026-10-13): the
coordinator joins our trains of two and three fragments, several in one
send, with whole requests between them, and passes the operation on to
the node that runs it in fragments of its own. `header::encode_train`
writes it; the coordinator puts the train together before it runs the
request.

- **When.** A signal goes whole when its sections come to at most 8 040
  words, its own data not counted; above that it is split.
- **Pieces.** Each fragment carries up to 8 040 words of sections, the
  most a 32 KB message holds with room for its header. Sections go in
  order: a fragment carries the rest of one section, any sections that
  fit whole, and may begin the next. Every piece of a section but its
  last is a multiple of 60 words: a piece that begins part-way into a
  fragment is cut short to a multiple of 60, and when not even 60 words
  are left, the section waits for the next fragment; a fragment that
  begins with a piece takes 8 040 words, itself a multiple of 60. A
  section's last piece may be of any length, a whole section included.
- **Why 60.** A 26.10 data node keeps a section in segments of 60 words
  and joins pieces segment by segment; a debug data node stops on a
  ragged piece, a release one logs and keeps a corrupt section. Our
  segment size is one constant, `IC_SECTION_SEGMENT_WORDS`; any multiple
  of 60 satisfies 26.10.
- **Data.** The fragments before the last carry as data only the numbers
  of the sections they carry and the fragment id, n + 1 words for n
  sections; the last carries the signal's data, then its section
  numbers, then the fragment id. A data node takes the data words and
  the sections together up to 25 in one signal, which for the last
  fragment counts its section numbers twice: a `KEY_OP_REQ`, 8 words
  with two sections, comes to 13. The receiver keeps the last fragment's
  data, and so does `ic_apid::fragments`.
- **Fragment id.** A train is known by the sender's block reference and
  the fragment id. The reference counts ids per API connection from 0;
  the data nodes' own counter skips 0, and so does ours, one counter per
  user thread, which is one sender block. A section number of 3 or more
  is taken as malicious.
- **Room.** The coordinator has room to put together one train per API
  node at a time, never interleaved. Without room it drops the train and
  answers nothing, which the reply timer of §6.9 catches. Our trains
  stay whole on their link: a thread's batch is written with one append,
  so trains of different threads never interleave.
- **What a data node takes for malformed.** A key above 1 023 words or
  column data above 0x3FFFF words makes the coordinator take a request
  for malicious; the blocks that run the operation refuse column data of
  32 768 words or more. We refuse both before sending: a key above 1 023
  words with `IC_ERROR_KEY_TOO_LONG` (7156), column data of 32 768 words
  or more with `IC_ERROR_RECORD_SIZE_TOO_BIG` (7077).
- **Staged requests.** A request packed when its query is defined has
  its flags set at the send; in a train they are in the last fragment,
  and only its checksum is computed again.

The largest rows, and so the largest requests, are in §13. A write of
the largest row a MySQL server makes, both variable parts full, is
about 16 400 words of column data: three fragments with a one-word key.
The largest row there is, 18 000 words, with the largest key goes in
three as well.

## 3. Block numbers and references

- Kernel blocks: `ARCHIVE 0xF4, COORDINATOR 0xF5, DISTRIBUTION 0xF6,
  LOCAL_OPS 0xF7, HASH_INDEX 0xF8, ROW_STORE 0xF9, DICTIONARY 0xFA,
  START_CONTROL 0xFB, MEMBERSHIP 0xFC, FILE_IO 0xFD, NODE_ADMIN 0xFE,
  INDEX_BUILD 0xFF, UTILITY 0x100, CHANGE_FEED 0x101, ORDERED_INDEX
  0x102, ..., LINK_ADMIN 0x10A`, plus RonDB's read workers
  `READ_WORKER_FIRST`…`READ_WORKER_LAST` (`0x10B–0x111`), which can
  appear as senders of `ROW_DATA_REP`.
- API blocks: dynamic per client thread from `0x8000` upward (max 4 711
  clients); `BUNDLE = 0x07FF` (2047) for bundled signals; `NODE_WATCH =
  0x0FA2` (4002) fixed for the heartbeat client. iClaustron's
  `IC_NDB_MIN_MODULE_ID_FOR_THREADS 32768` and `IC_NDB_PACKED_MODULE_ID
  2047` match.
- Reference = `node | (block << 16)`. In multi-threaded kernels the
  block field also carries an instance number through a non-trivial
  mapping that must be reproduced exactly when decoding sender
  references.

## 4. Cluster membership and heartbeats

Version announced: `get nodeid` and `HEARTBEAT_REQ` carry an NDB version
word `(major << 16) | (minor << 8) | build`; for RonDB 26.10.0 that is
`0x1A0A00`, plus a MySQL version word. The data node checks API/DB
compatibility and answers `HEARTBEAT_REF` on mismatch. We announce
26.10.0 (Q5).

- `HEARTBEAT_REQ` (signal 3) from the API's `NODE_WATCH` block to
  `MEMBERSHIP` every `min(max interval, heartbeat check interval / 2)`;
  data: own reference, NDB version, MySQL version (3 words).

  **We send every third of the check interval, not every half.** The
  author judges half too seldom (2026-09-19). Sending more often than
  the reference's client does is safe by construction, since the data node only
  counts intervals in which it heard nothing. `ic_apid::apid_global`,
  `IC_HEARTBEATS_PER_INTERVAL`, with a compile-time check that it is
  never set below three.
- `HEARTBEAT_CONF` (signal 1), 22 words in this order: the sender's
  `MEMBERSHIP` reference, its NDB version, the heartbeat interval, its
  MySQL version, the lowest data node version, then the 16-word node
  state, then the lowest API version **after** it. The order matters and
  is easy to get wrong; the lowest API version is the last word, not the
  sixth.
  The node state holds start level, node group, dynamic id, start phase,
  single-user mode and a connected-nodes bitmap of 256 bits, which stays
  256 bits however far node ids rise because it only covers data nodes.
  It marks other nodes with ids below 256 as well: seen live
  (2026-10-09), data node 1's answer to the management server, node 3,
  had bits 1 and 3 set. So a bit says the node is connected, not that
  it is a data node.
  A data node counts as usable when started, or in single-user mode, or
  (RonDB) past the restart barrier while recovering.

  The heartbeat interval is in **hundredths of a second**, a tenth of
  parameter 119 (`api_heartbeat_ms`), which the configuration states in
  milliseconds: a cluster configured for 30000 ms answers 3000 here.
  Confirmed against a live RonDB 26.10 cluster, and matching the
  reference's client, which multiplies this field by ten.

- `HEARTBEAT_REF` (signal 2): rejection, e.g. version mismatch.
- Node declared dead after 4 missed heartbeats; then `NODE_LOST_REP`
  (signal 26) is sent to self and broadcast to all client blocks,
  followed by `NODE_LOST_DONE_REP` (signal 27) from the cluster when
  takeover is complete; only then may transactions be aborted with the
  right error and the node be reconnected.

  The detail, as implemented in `ic_apid` (`apid_global`,
  `rec_thread`, `heartbeat`, `connect_thread`):

  - **Counting missed heartbeats.** A counter per node goes up by one at
    the end of every check interval and back to zero on every
    `HEARTBEAT_CONF`, so at rest it moves between zero and one. The node
    is lost when it reaches 4, which is at least three whole intervals
    of silence. The check interval has a floor of 100 ms whatever the
    node reports.
  - **Every sign of a failure converges.** A disconnect of a node that
    had finished connecting is handled as if a `NODE_LOST_REP` naming
    that one node had arrived, unless one already has. So the socket
    closing, the heartbeats stopping and another node's report all run
    the same handling, and only the first for a given connection does
    anything.
  - **The reconnect gate.** A data node that is not connected is not
    dialled while its takeover report is outstanding. The stated reason
    is that cluster disconnect can then be detected reliably. The flag
    is cleared on the first handling of a failure, set by
    `NODE_LOST_DONE_REP`, and set again on connect.
  - **Who sends `NODE_LOST_DONE_REP` to an API node, and what is in
    it.** Every surviving data node, once all of its blocks have handled
    the failure, to every API node it holds as registered. Its block
    field carries the sender's `MEMBERSHIP` reference, **not zero**.
    Zero means "whole node" only between blocks inside a data node. The
    API side reads the failed node id and nothing else.
  - **When nobody is left to report.** If handling a failure leaves no
    data node alive, the API completes every outstanding takeover
    itself, since no `NODE_LOST_DONE_REP` can come.
  - **A lost link is not a failed node.** When only the API's link to a
    node drops and the node stays up, no `NODE_LOST_REP` arrives, and the
    data node has no special handling of the state (confirmed with the
    author, 2026-09-19). The reference rules above would leave that node
    undialled until it really restarts or every other node is lost,
    because nobody reports a takeover for a node that did not fail, and
    the reference's client tends to answer applications with 4009, "cluster
    failure", meanwhile. We differ deliberately: our own evidence of a
    loss (socket, send, heartbeat silence) only redials, with
    `IC_ERROR_LINK_LOST`; the reconnect gate applies only once a data
    node has actually sent `NODE_LOST_REP` for the node.

  The iClaustron C implements none of this. It has no handler for either
  signal and redials a lost node on a three second timer. **For protocol
  behaviour this chapter is the authority and the C is not**; the C is
  the authority for structure.
- Other membership signals: `NODE_JOINED_REP` (163), and, unnamed since
  we do not use them, 127 (close the links to failed nodes), 61 (a node
  id granted), 399 (a coordinator's takeover done), 465 (an order to
  dump state) and 247 (an event report). RonDB adds 900–905 (activate
  and deactivate a node), 906–908 (set a node's host name), 944–946 (set
  its location domain), 934–936 and 937–939 (get and list databases).

## 5. Management protocol and configuration

### 5.1 Text protocol

Request: a command line, then `name: value` lines (strings as
`name:"value"`), then an empty line. Reply: a line `<command> reply`,
then `name: value` lines, then an empty line. Lines are at most 512
bytes. iClaustron's `ic_protocol_support.c` and
`ic_apic_conf_read_proto.ic` implement both sides and are carried over.

- `get nodeid` with `version`, `nodetype`, `nodeid`, `user:"mysqld"`,
  `password:"mysqld"`, `public key:"a public key"`,
  `endian:"little"|"big"`, optional `name`, `log_event`; reply `result:
  Ok` and `nodeid`.
- `get config_v2` (preferred) or `get config` with `version`,
  `nodetype`, and for v2 `node: <id>`; reply carries `Content-Length`,
  `Content-Type: ndbconfig/octet-stream`, `Content-Transfer-Encoding:
  base64`; then `Content-Length + 1` raw bytes of base64 follow the
  empty line.
- `get connection parameter` with `node1`, `node2` and `param`,
  answering `value` and `result`. The value is **signed**, and for
  parameter 406 (`port`) the sign is information: a negative value means
  the port was assigned when the node started rather than written in the
  configuration, and the port is its absolute value, so `-59733` means
  port 59733. Zero means the node has not been given a port yet. The
  listening side takes the same absolute value.
- Also
  `transporter connect` (1.3).

### 5.2 Binary configuration format

All words network byte order (big-endian), unlike the signal protocol.

- v1: magic `NDBCONFV`, then entries `key word = type<<28 | section<<14 |
  key`, types 1 a 32-bit number (1 word), 2 a string (length word incl.
  NUL, then padded bytes), 3 a section reference (1 word), 4 a 64-bit
  number (2 words, high then low); trailing XOR checksum.
- v2: magic `NDBCONF2`, then a 7-word header (total length, version=2,
  number of default sections=5, data nodes, api nodes, mgm nodes, comm
  sections), then default sections, system section, node sections,
  communication sections in that order; entry key word `type<<28 | key`
  (section implied by position); default sections supply inherited
  values; trailing XOR checksum. Section types: 1 a data node, 2 an API
  node, 3 a management server, 4 a TCP link, 5 a shared-memory link, 6
  the system section, 7 an RDMA link (RonDB).

iClaustron's `ic_apic_conf_read_transl.ic` decodes both. Parameter ids
are the management API's; our table of the API-node parameters is
checked against its list for 26.10.

## 6. Transactions and primary key operations

### 6.1 Allocating a coordinator record

Before the first operation to a given data node, send `COORD_ALLOC_REQ`
(signal 39) to `COORDINATOR` with: our pointer (our object id), our
block reference, the coordinator instance wanted. `COORD_ALLOC_CONF`
(signal 37) returns the coordinator's pointer to its record and the
coordinator's reference, to use in all later signals for that record.
Records are cached per node per client and released with
`COORD_FREE_REQ` (signal 36) at shutdown.

The words, as `ic_signals::coord_alloc` has them, none with sections:

| Signal | Number | Words |
|---|---|---|
| `COORD_ALLOC_REQ` | 39 | our pointer, our block reference, instance wanted (0 for any), release id |
| `COORD_ALLOC_CONF` | 37 | our pointer, the coordinator's pointer, the coordinator's block reference |
| `COORD_ALLOC_REF` | 38 | our pointer, NDB error code |
| `COORD_FREE_REQ` | 36 | **the coordinator's pointer**, our block reference, our pointer, release id |
| `COORD_FREE_CONF` | 34 | our pointer |
| `COORD_FREE_REF` | 35 | our pointer, NDB error code, source line of the refusal |

The release does not mirror the allocate: the coordinator's pointer
comes first, because it is the coordinator that has to find the record.
An allocation is refused when the node is not started or is shutting
down, or has no record free.

**The release id** (RonDB 26.10, added by the author on 2026-10-09 after
a doubled free stopped a debug data node, 17.10). The allocate's fourth
word is a random number the API node picks, never zero, which the
coordinator keeps with the record; the free carries it again as its
fourth word. A free with the record's id frees it as before. A free for
a record already freed, or being freed, is refused with
`COORD_FREE_REF`. A free with another id is ignored while a new
transaction uses the record, and refused otherwise (the author). A
request without the word, or with 0, is handled as before, so a
coordinator without release ids, which reads three words, takes ours
too. As built: each record gets its id when it is allocated
(`ApidConnection::next_release_id`), and is freed with it once at most.

**An allocate given up on** (review, 2026-10-21). A transaction's
allocate is waited for 5 s at most. A confirmation that comes after
that names our pointer for a place whose request is forgotten, and the
coordinator's pointer is in that confirmation alone, so nothing could
give the record back and it stayed until the link went. The place is
now kept as given up on, with its release id, and a confirmation that
comes late is answered with `COORD_FREE_REQ` at once, over the link it
came on; a late refusal needs nothing. A scan's allocate is never given
up on in that way: its pool waits for the answer, or for the link's
loss.

**The coordinator's block reference is echoed, never decoded.** Its
upper sixteen bits hold the block and the instance, and RonDB packs up
to 1024 instances into them with a transform of its own. An API node has
no use for the instance: it sends later signals to exactly the sixteen
bits it was given.

These are the first signals sent under a user thread's block number, so
their answers are the first to be routed to a user thread's inbox
instead of being executed where they arrive. `ic_node_ping` does the
exchange on every started node for that reason.

### 6.2 Transaction id

Client-side only: a 64-bit value whose high word is unique per client
block and whose low word is a counter. Sent in every signal as two
words, the low half and then the high half.

As built (`ApidConnection::next_transaction_id`): the block number in
bits 52–63, our node id in bits 40–51, the counter in the low word,
starting again at 0 after `0xFFFFFFFF`. The counter is kept per block
number when a connection goes, and the next connection with that block
number goes on from it, so no id is made twice.

### 6.3 Choosing the coordinator node (hinting)

1. Concatenate distribution-key column values in column-id order, each
   padded to a 4-byte boundary; charset columns are normalised first.
2. Hash, chosen per table by the hash function key (table property 163):
   0, or not sent, is MD5 producing 4 words; non-zero is **XXH3,
   64-bit** (`XXH3_64bits` of the xxHash library, seed 0), split into
   two words, low then high. **Not XXH64**, which an earlier revision of
   this plan named: the two are different functions, and the wrong one
   sends every key to the wrong fragment. The AVX2 path calls the same
   function. **The distribution hash is word 1 of the result, not word
   0**: the second word of the MD5 digest, or the high 32 bits of XXH3.
   A table gets the new hash when the API creating it is RonDB 22.10.1
   or later, so tables made by a 26.10 mysqld have the flag; an index
   uses its table's function.
3. RonDB fanout tables hash a base prefix and a detail suffix separately
   and combine `((base/fanout)*fanout) + (detail % fanout)`.
4. Hash → fragment: for hash-map tables `hash_map[hash % hash_map_len]`;
   linear-hash and plain key-hash variants exist for old tables.
5. Fragment → replica nodes from the table's fragment data; with RonDB
   dynamic primary replicas the primary may be recomputed from node
   state.
6. Node choice: location domain, then same host, then primary replica;
   fully-replicated tables: any node; read-backup tables: any replica of
   the fragment.

This whole chain lives in `ic_apid::hash` and is driven by the
`IC_TRANSACTION_HINT`. The MD5 and XXH3 implementations are written
in-tree (both are public algorithms). XXH3 has to match the xxHash
library bit for bit, including its handling of short inputs; its test
vectors are the check.

As built (`ic_util::md5`, `ic_util::xxh3`, `ic_apid::hash`), checked
against the reference:
- The key buffer is each distribution key column in column order, as the
  record holds it: a fixed-size value in full, a variable-sized one
  behind its one or two length bytes with only the bytes it has, each
  padded with zeros to a word. A character column is first put through
  its collation, from the weights the data nodes describe, fetched when
  the table is bound (below).
- **Binary columns** (the author, 2026-10-09): `BINARY` and `VARBINARY`
  have no character set. A `VARBINARY` goes into the hash as it is
  stored, its length byte or bytes and then its bytes, nothing
  stripped, padded with zeros at the end to a whole number of words: a
  value `abcd` behind one length byte is five bytes and three zeros. A
  `BINARY` goes in as its whole field, padded with zeros likewise to a
  whole number of words: a `BINARY(3)` is its three bytes and one zero.
  That buffer is what XXH3, with seed 0, or MD5 for an older table,
  hashes. They compare byte by
  byte, as `memcmp` does: two `BINARY` values over their whole fields,
  two `VARBINARY` values over their bytes up to the shorter one's
  length, the length bytes left out, and if those are equal the longer
  value is the larger. That is the order an ordered index scan's merge
  needs for them (chapter 06 §8.7). Seen live (2026-10-09, the `pk`
  group's binary-key test): our partition was the data nodes' for
  `VARBINARY` keys of every length from 0 to 20, trailing spaces kept,
  for keys of 250 to 300 bytes behind two length bytes, and for
  `BINARY(5)` keys.
- **Character columns** (step 8.6, done ahead of phase 8; seen live
  2026-10-12, the integration group `charset`; the rules from the
  author, 2026-10-09, where marked). The client and the coordinator hash
  a character column's data, a CHAR's whole field or a VARCHAR's bytes
  after its length bytes, put through its collation, and that part is
  padded with zeros to a word like any other column's.
  - *The characters* come from the bytes by the set's encoding (UTF-8,
    UTF-16 either way round, UCS-2, UTF-32, or for a set of one byte a
    character the table its weight runs give, 8.2), and *a string's
    weights* are its characters' weight strings in order. A contraction
    stands for the characters it joins, the longest one starting at a
    character first; failing that, a pair whose second character is
    weighed by the first, as a Japanese kana and the prolonged sound
    mark after it. Either kind's weight string is the whole sequence's,
    the first character's own weights included.
  - *Pad space collations*: the weights run together as a sort key, and
    if it is shorter than the collation's sort-key growth times the
    column's largest length in bytes, the sort key of a single space
    repeated after it to that length; cut to it if longer.
  - *No pad collations*, the Unicode 9.0.0 ones, utf8mb4_0900_bin aside:
    a CHAR's trailing spaces stripped, then 64-bit FNV-1a, from
    14695981039346656037, each 16-bit weight XORed in as one number and
    the product with 1099511628211 taken after (the author); the weights
    level by level, a zero weight hashed between levels (the author),
    zero weights within a level skipped (the author). The levels are
    those the collation compares: the first three always, separators and
    all, even when empty; a fourth, the kana level of
    utf8mb4_ja_0900_as_cs_ks, only when the string has a weight there,
    as the characters' own weight strings show it (seen live). The eight
    bytes of the result, little-endian, are the column's part.
  - *utf8mb4_0900_bin* (the author): trailing spaces stripped, a
    VARCHAR's too; then for each byte b, n1 = n1 XOR (((n1 AND 63) +
    n2) × b + (n1 << 8)) and n2 grows by 3, both from 0, n2's start
    being RonDB's seed; n1's eight bytes, little-endian.
  - *Not placed*, and so sent to the next node in turn, a hop and not a
    wrong answer: the collations that weigh a character by its
    neighbours (latin2_czech_cs, cp1250_czech_cs, tis620_thai_ci), and
    sets of several bytes a character that are not Unicode.
  - *Seen live to match the data nodes*, with CHAR and VARCHAR keys of
    ASCII, Latin, Greek, Cyrillic, CJK, Hangul, kana, combining and
    supplementary characters, each also in upper and lower case and
    with trailing spaces: latin1's eight collations; utf8mb4_general_ci,
    _bin, _unicode_ci, _unicode_520_ci, _0900_ai_ci, _0900_as_ci,
    _0900_as_cs, _0900_bin, _es_trad_0900_ai_ci, _de_pb_0900_ai_ci,
    _sv_0900_ai_ci, _ja_0900_as_cs, _ja_0900_as_cs_ks and
    _zh_0900_as_cs; utf8mb3_general_ci, _unicode_ci and _bin;
    ucs2_general_ci, utf16_general_ci, utf16le_general_ci,
    utf32_general_ci, latin2_general_ci and cp1251_general_ci; keys of
    several columns mixing collations and integers, with the
    distribution key a subset in another order, and with the key's
    columns in another order than the table's; and unique indexes over
    one and two character columns, against the index's own rows.
  - The latin1 collations and their numbers: 5 latin1_german1_ci, 8
    latin1_swedish_ci (latin1's default), 15 latin1_danish_ci, 31
    latin1_german2_ci, 47 latin1_bin, 48 latin1_general_ci, 49
    latin1_general_cs, 94 latin1_spanish_ci. MySQL's latin1 is the
    CP1252 code page as Unicode publishes it, with 0x81, 0x8D, 0x8F,
    0x90 and 0x9D, which CP1252 leaves unassigned, passed through as
    U+0081, U+008D, U+008F, U+0090 and U+009D.
  - Neither side counts characters. The row store checks on a write that
    a character column is well formed (744), and a key the coordinator
    cannot transform fails with 290. The client does not pad a CHAR with
    spaces; the application must, as a key padded with zeros is another
    key. A unique index's columns keep their character set and are
    hashed with the same transform.
- Which word places the key: word 1, the second word of the MD5 digest
  or the high half of XXH3, exactly as step 2 says.
- XXH3 is pinned by 37 published values of the algorithm, one on each
  side of every boundary between its paths, and MD5 by RFC 1321's own
  examples and the block boundaries. The XXH3 secret is the algorithm's
  standard 192 bytes.
- Fanout tables (step 3) were refused until phase 10, which does them
  (`hash::partition_of`, `hash::fanout_first_of`, 2026-10-21). Only the
  hash changes, worked out in two steps instead of one; a key operation
  is otherwise executed as on any table (the author, 2026-10-21). MySQL
  makes such a table with `COMMENT="NDB_TABLE= PARTITION_HASH=1:1:4"`,
  base columns : detail columns : fanout, which come in table properties
  166, 167 and 168, parsed already. The base columns are the primary
  key's first, in key order, the detail columns the next, and each part
  is hashed as a distribution key is.
- **Which nodes hold a fragment** comes with the description, in the
  replica map (table property 138): 16-bit values, big-endian, the
  replica count and the fragment count, then per fragment its log part
  and the node of each replica, primary first as the distribution
  handler placed it. The dictionary asks the distribution handler for it
  only when answering `TABLE_DESC_REQ`.
- **Which replica is primary is not fixed** (steps 5 and 6 above). With
  dynamic primary replicas, which RonDB 26.10 has, and more than one
  replica alive, the data nodes deal the primaries of a node group's
  fragments over the group's alive nodes in node id order, in fragment
  order, so many fragments to each that every alive node gets an equal
  share (the fragment count divided by the alive count, rounded up);
  with one replica, or one alive, that one. The reference computes the
  same rule for itself, and so does `ic_apid::hash::primary_of`.
- **Node choice as built** (`hash::choose_node`): a fully replicated
  table goes to any started node holding any fragment; a read-backup
  table, which every table a RonDB server makes is, to any started
  replica of the partition, taken in turn; any other table to its
  primary, or to a started replica if the primary is down. The reference
  first prefers a node in its own location domain, then one on its own
  host, and balances by a per-node count; none of that is done yet. A
  wrong choice only costs a hop: the coordinator forwards the operation
  to wherever the row is.
- The partition pseudo column (0xFFFE), read with a header of size zero,
  answers with one word: the partition the row is in. `ic_read
  --partition` compares it with the computed one, which is how the chain
  is checked live: seen to agree (2026-09-22) on a RonDB 26.10 table
  with an INT key, hashed by XXH3, over eight partitions.

### 6.4 KEY_OP_REQ (signal 12)

Long form only (sections). Signal data, 8 fixed words: the coordinator's
record (its pointer from the allocation), our query's id (our operation
object id), the column data length (total COLUMN_DATA_ORD words), the
table id, the flags word, the table's schema version, and the
transaction id's low and high halves. Optional words 9–12 depending on
flags: scan information, distribution hash value, distribution key size,
stored procedure id. Section 0 = KEY_DATA_ORD, section 1 =
COLUMN_DATA_ORD.

Flags word bits: 0 dirty, 1 no-disk, 2 distribution-key present, 3 via
the join block, 4 commit, 5–7 operation type (0 read, 1 update, 2
insert, 3 delete, 4 write, 5 read exclusive, 6 refresh, 7 unlock), 8
simple, 9 queue on redo, 10 execute, 11 start transaction, 12–13 abort
option, 14 scan takeover, 15 interpreted, 16–18 COLUMN_DATA_ORD words in
signal (0 for long form), 19 reorg, and RonDB additions 20
read-committed-base, 21 no-wait, 22 batch-safe, 23 batch-unsafe, 24
pass-queueing, plus replica-applier, TTL and ring-buffer flags.
iClaustron documents bits 0–15 in prose in `ic_apid_handle_messages.ic`
under the name `NDB_PRIM_KEYREQ`; the stock signal and the RonDB bits
above supersede it.

As built (`ic_signals::key_ops`), checked against the reference:

- **The optional words were wrong above.** After the eight fixed words
  come, each only if its flag is set and in this order: the user id and
  its version for RonDB's rate limits (flag bit 29, two words), the scan
  information (bit 14), the distribution key (bit 2). Nothing else is
  read from a long request.
- **A long request must be exactly as long as its flags make it.** The
  coordinator checks the length against eight plus the optional words,
  and refuses anything else as a malicious signal.
- A committed read sets both the simple and the dirty flag; a simple
  read sets only simple.
- Operation types: read 0, update 1, insert 2, delete 3, write 4, read
  exclusive 5, refresh 6, unlock 7.

KEY_DATA_ORD section: key column values in primary-key order, each
padded to 4-byte alignment.

COLUMN_DATA_ORD section. For a plain read: a list of column headers (one
word each) naming the columns. The reference's record path reads
**packed** instead, and so do we (`ic_apid::row_codec`, as the author
pointed out): one header, the read-every-column pseudo column (0xFFF0)
with the column count as its size when every column is read, otherwise
the read-by-mask pseudo column (0xFFF3) with a bitmask of the column ids
after it. The row comes back behind one 0xFFF3 header whose size is a
result bitmap's in bytes. The bitmap has a bit per column id, set if
read, and after each nullable column read one more, set if NULL. The
values follow in column order, NULLs taking no room: a byte-sized type
(characters, 8- and 16-bit elements) at the next byte, a 32- or 64-bit
one at the next word, bit columns packed together bit by bit from a
word, and whatever follows them after the last word they touched. A
variable-sized value starts with its one or two length bytes. There is
no packed form for writing: an update sends a header and value per
column.

  For a write, the section is a header and value per column, the value's
  length in bytes in the header and the value padded to words; a length
  of zero means NULL and no value follows. A key column is sent with the
  value the key section has, so that the two agree. A request with
  nothing to send, such as a delete, carries the key section alone. For
  a plain write: column header + value words per column. For interpreted
  operations and all scans the section starts with five length words:
  (1) reads before the program, (2) the program, (3) updates after, (4)
  reads after, (5) subroutines; each part is column headers plus data.

Column header word: bits 16–31 column id, bit 15 partial flag, bits 0–14
byte length; length 0 means NULL. Pseudo-columns, by number: 0xFFFE the
partition, 0xFFFD the row count, 0xFFFB the range number, 0xFFF3 read by
mask, 0xFFF0 read every column, 0xFFE7 the row's 64-bit global
checkpoint, 0xFFEE the lock reference, 0xFFED the operation's identity;
RonDB's 0xFF00 the aggregation result, 0xFC00 append to a column, 0xFC01
write part of a column.

### 6.5 Replies

- `KEY_OP_CONF` (signal 10): our pointer for the transaction, the global
  checkpoint's high word, the information word, the transaction id's low
  and high halves, then per operation `{our query's id, its row data
  length}`, then the global checkpoint's low word. The information word:
  bits 0–15 number of operations, bit 16 commit flag, bit 17 commit-ack
  marker. The row data length's bit 31 = dirty read (low bits then carry
  the responding node). Arrives to `BUNDLE` in bundled form (up to 6
  sub-signals, each sub-header `len = (w & 0x1F) + 3`, block in bits
  16–31) or alone to our block.
- `KEY_OP_REF` (signal 11): our query's id, the transaction id's low and
  high halves, the error code and the error data. RonDB rate-limit
  errors 243 and 2203.

  **A refusal is sent only for an operation that is allowed to fail**,
  that is one whose abort option says to ignore an error, as a committed
  read's does. An operation sent to abort on error, as a write is, gets
  none: the coordinator aborts the transaction, and the error comes in
  `TRANS_ABORTED_REP` (signal 16) instead, five words naming the
  transaction: our pointer, the transaction id, the error and its data.
  A transaction therefore waits for that report as well as for the
  confirmation. Found live (2026-09-22): an insert of a key that was
  already there waited for a refusal that never came.

  **A table or index out of date is found before an operation can be
  allowed to fail.** The coordinator checks the version a key request
  names as the request arrives, and on a mismatch aborts the whole
  transaction, whatever the abort option: 241 for a wrong version, 284
  for a table it does not know, 283 while a drop is in progress, in
  `TRANS_ABORTED_REP`. A dropped index's request gets 284 the same way.
  So these are read in the report as in a refusal: the tables and
  indexes of the transaction's operations still waiting are let go of
  before their queries fail, since the report does not say which
  operation it was (review, 2026-10-21). In the report, a refusal and a
  scan's refusal alike, every code the error table classes as a schema
  error lets go, 283 among them, as MySQL's handler drops its cached
  table on any schema error (the author, 2026-10-21; a path from the
  tree's root).
- `ROW_DATA_REP` (signal 5): row data (column header + data words) for one
  operation, first word = our operation object id; long form in section 0.
  May arrive **before or after** the matching `KEY_OP_CONF`. The long
  form's data is four words: the operation, the transaction id's two
  words, and the row's length in words, which is section 0's (seen live
  2026-10-14, all 19 long rows of a `big_rows` capture, joined from up
  to 136 fragments).

  Seen live (2026-09-21) with `ic_read` against RonDB 26.10: a
  committed read of a row by its key, of a row with a NULL column, and
  of a key with no row, each answered as this section says. With
  `ic_write` (2026-09-22): an insert, an update and a delete of a row,
  each confirmed, and a key already there refused with 630 through the
  rollback report.
- `SCAN_ROW_KEY_REP` (signal 33): key of a scanned row when scan takeover was
  requested.
- Commit-ack: when `KEY_OP_CONF` bit 17 is set, send
  `COMMIT_MARKER_FREE_ORD` (signal 469) back to coordinator.

As built (`ic_signals::key_ops`, `bundle`, and the receive thread),
with what the reference showed on the way:

- **Every reply names its receiver in its first word**, which the
  reference states as a rule for all traffic signals. `KEY_OP_CONF`
  names the transaction by the pointer given at allocate time, or is
  IC_NULL_PTR, and the transaction is then found from the operations it
  lists. `KEY_OP_REF` and `ROW_DATA_REP` name the operation by the
  pointer the request gave it.
- **`ROW_DATA_REP` comes from the node that read the row**, which need
  not be the coordinator's node. Its three header words are the
  operation pointer and the transaction id; a long one has the row in
  section 0, and one sent in fragments has a fourth word, the row's
  whole length.
- **Per operation, `KEY_OP_CONF` gives the words of row data to
  expect.** With the top bit set it is a dirty read, and the low bits
  name the reading node. An operation is complete when its
  `ROW_DATA_REP` words reach the count, or at the first `ROW_DATA_REP`
  of a dirty read; with no words expected, `KEY_OP_CONF` alone completes
  it. A dirty read whose reading node fails before its row arrives fails
  with 4119.
- The commit marker counts only when the commit bit is set too, and the
  low word of the global checkpoint follows the operations only when the
  signal is long enough to hold it.
- **`COMMIT_MARKER_FREE_ORD` is two words, the transaction id,** sent to
  the block that sent the `KEY_OP_CONF`, and sent even when the
  confirmation is otherwise rejected.
- **Both `KEY_OP_CONF` and short `ROW_DATA_REP` arrive bundled.** A
  signal to `BUNDLE` (2047) holds several signals of its own signal
  number, each behind a header word with its length less three in bits
  0–4 and its block in bits 16–31. The receive thread takes it apart and
  routes each part by its own block.

### 6.6 Completion accounting (the rule that makes `poll` correct)

Per operation keep `expected_len` (from KEY_OP_CONF) and `received_len`
(from ROW_DATA_REP), both initially unknown. The operation is complete
when both are known, regardless of arrival order. A KEY_OP_REF completes
it with an error. A transaction's `execute` is complete when completed
operations equals sent operations, and commit state is taken from the
KEY_OP_CONF commit flag.

### 6.7 Commit and abort without operations

`COMMIT_REQ` (signal 19) `{the coordinator's record, the transaction
id's low and high halves}` → `COMMIT_CONF` (signal 17) `{our pointer
(bit 0 = ack expected), the transaction id's two halves, the global
checkpoint's high and low words}` or `COMMIT_REF` (18). `ROLLBACK_REQ`
(signal 15) → `ROLLBACK_CONF` (13) or `ROLLBACK_REF` (14).
`TRANS_ABORTED_REP` (signal 16) `{our pointer, the transaction id, the
return code, the error data}` is a coordinator-initiated abort. As built
(`ic_signals::key_ops`): the two requests are three words, our pointer
and the transaction id; `ROLLBACK_CONF` is those three back;
`COMMIT_REF` adds the error code, and `ROLLBACK_REF` that and the
coordinator's state.

**The takeover replies, as built** (`ic_apid::transaction`): both are
three words, our pointer for the transaction and its id, sent by the
coordinator that took over from the failed one, so from another node
than the request went to. `TAKEOVER_CONF` has the commit-ack marker in
its pointer's low bit, and is acknowledged with `COMMIT_MARKER_FREE_ORD`
to its sender whatever became of the transaction here; it means
committed when a commit had been asked for, and otherwise a commit
nobody asked for, which the reference reports as 4115. `TAKEOVER_REF`
means aborted: the outcome wanted if a rollback had been asked for, and
error 4031 otherwise. The reference does nothing to the transactions of
a failed coordinator on `NODE_LOST_REP`; on `NODE_LOST_DONE_REP` it
aborts those still open with 4010, since the takeover would have
answered for any that committed. Not yet seen live: that needs a
coordinator killed with a commit in flight, which is for the failure
group of the integration tests. Node failure while a transaction is in
flight: `TAKEOVER_CONF` (8) / `TAKEOVER_REF` (9). RonDB sends
`DEADLOCK_FOUND_REP` (980) before an abort caused by deadlock detection.

Execute semantics we must reproduce: the first operation of a transaction
carries the start flag; the last operation in an execute that commits
carries the commit flag; the last operation of every send train carries
the execute flag. Abort option: abort on error by default for writes,
ignore the error by default for reads; committed reads only allow
ignoring it.

### 6.8 Unique index operations

`UNIQUE_OP_REQ` (signal 519), `UNIQUE_OP_CONF` (520) and `UNIQUE_OP_REF`
(521) have exactly the `KEY_OP_REQ` family layout; the key sent is the
unique index key; signals 522 and 523 are the short-form trains of key
and column data (unused by us).

As built (`ic_apid::transaction`, a query made with
`create_unique_query`): the request names the **index's own table**, its
id and version, not the base table's; the key is the indexed columns in
the index's order, which are the primary key of the index's own table,
taken from a record over the base table by name; the column data is over
the base table as for any read or write. The coordinator finds the row's
primary key through the index and runs the operation on the table. It
never sends `UNIQUE_OP_CONF`: an operation through an index is confirmed
by `KEY_OP_CONF` like any other, and refused by `UNIQUE_OP_REF` in the
key refusal's layout with error data 0; the row comes as `ROW_DATA_REP`.
(An earlier revision said both answers came as the unique family's
signals.) An insert cannot go through an index; the reference refuses
one with
4200. **A read through an index takes the shared lock, always**: the
      reference sends its committed and simple index reads as plain
      locked reads, and the coordinator asserts that an index request
      carries neither the dirty nor the simple flag. Found live
      (2026-09-22): a committed read sent through an index with both
      flags lost the link to the data node at once, which is what a
      failed assertion in a debug data node looks like from here.

**Unique keys done properly** (step 6.5, 2026-10-09; what was gathered
in 16.3, folded in here). Seen live, RonDB 26.10 debug build, the
integration group `uk`, unless marked otherwise:

- **NULL.** A row with NULL in an indexed column is never put in the
  index, so a lookup by NULL can find nothing; the bytes under a NULL
  would name some other row. The reference's client refuses a NULL key
  column with 4316, and so do we, before sending.
- **Placement.** The coordinator hashes the index columns in the index's
  order over the index's own table, which inherits read backup, the hash
  function and full replication from its table, and when the fragment
  types match its hash map and fragment count; the index's description
  carries its own replica data and hash map, and every index column is a
  distribution key. We hash the key a unique request carries, the
  indexed columns padded to words, with the index's own hash map and
  replicas. Seen live: for twenty keys the partition we worked out was
  the one the data nodes report for the index's row, read back with the
  partition pseudo column from the index's own table by its key (the
  coordinator lets a client read an index's table directly), and a
  transaction so hinted started at a node holding it. An index over a
  character column is placed in turn until its collation's transform is
  known (chapter 06 §8.6).
- **Locks.** A committed read through an index is sent as a shared read
  with bit 20 of the flags word set, which lets both reads use a backup
  replica of a read-backup table, or any node of a fully replicated one;
  the lock is held to commit, and nothing releases it early. The index
  read is shared for a read and exclusive otherwise. Seen live: a locked
  and an exclusive read through an index wait for an update's lock and
  see what it committed; a no-wait read is refused at once with 635; 500
  committed reads through an index in one batch all completed.
- **Duplicates.** A value taken already gives 893 with the index id as
  its error data. Seen live: an operation that aborts on error, as a
  write does, gets it in `TRANS_ABORTED_REP`, not in a refusal (6.4),
  and the data names the index: by primary key, the index whose value an
  insert duplicated; through one index, the other index whose column an
  update set to a value taken. A deferred check is bit 17 of the flags
  word, honoured on unique requests too and checked at commit; we do not
  ask for it.
- **The index's state.** Neither the coordinator nor the reference's
  client checks it, and a lookup through an index still being built
  finds nothing; we refuse one that is not online at bind. Seen live: an
  online index's description carries state 4, a table's carries none.
- **A dropped index.** An operation through it is refused with 284, or
  241 for a wrong version, and `TABLE_CHANGED_REP` tells API nodes of a
  new enough version. Seen live: an index dropped and made again by a
  MySQL server while bound was refused with one of the two, let go of,
  and bound afresh under its new id.
- **Updates.** The reference takes the indexed columns of an update
  through an index from the key row, so it cannot change them; we refuse
  such an update outright, and an insert or write through an index as
  before.
- Out of index operation records gives 288 (not seen live).

### 6.9 A reply that never comes (4012)

A transaction can wait for ever while its link stays up: a data node
that has stopped, or a coordinator that dropped a request train for
lack of room (16.1), sends nothing, and the heartbeats take minutes to
fail the node, if they fail it at all.

- **How long the client waits.** The larger of 120 000 ms and, over the
  node sections of the configuration, parameter 129 plus parameter 131
  (our `timeout_check_period` and `lock_wait_limit`), a section without
  them counting as 0.
- **What the reference does then.** It fails the transaction with 4012,
  whose class is unknown result, and never uses its coordinator record
  again; in its synchronous path it also tries a rollback.
- **What we do** (step 6.3, `ApidConnection::expire_transactions`). A
  transaction waits when it has a query sent and not complete, or a
  commit or rollback asked of the coordinator and not answered; the
  wait runs from its last send. A poll looks at most once a second.
  The transaction ends rolled back with 4012 and its unfinished queries
  fail with 4012. Its record is never handed out again and is freed
  with `COORD_FREE_REQ`, which makes the coordinator abort the
  transaction or conclude it without telling us, and free the record;
  a record is freed once at most (17.10). A reply that comes later
  finds no transaction of that id and is dropped.
- **Seen live (2026-10-09, RonDB 26.10 debug build, the integration
  group `timeout`).** With a wait of one second, a data node stopped
  with SIGSTOP after the transaction's record was allocated and before
  its insert and commit were sent: the transaction ended with 4012
  within the test's bounds of one to four seconds, the node still
  counted as started, and after SIGCONT the next transaction at the
  same node committed. The first version freed the record a second
  time when the connection let go of its records, and the node stopped
  with error 2343 (17.10).

## 7. Scans

### 7.1 The exchange (the author, 2026-10-15)

- **One request starts a scan.** `SCAN_REQ` goes to the coordinator,
  either for a scan of a whole table, naming the table, or for a range
  scan of an ordered index, naming the index. It carries the ranges'
  bounds and a column section: what to read, which may also hold an
  interpreted program.
- **Three lock modes:** committed reads, shared locks and exclusive
  locks. With a lock, a row is locked when it is handed to the API, so
  the API can take the lock over before the scan goes on. It does so
  with an ordinary `KEY_OP_REQ` that has the takeover flag set and
  names the scanned row.
- **The coordinator runs the parallelism.** It scans the fragments in
  parallel itself; the API waits for `SCAN_CONF`, which reports how
  many rows have been returned. A `SCAN_CONF` may be long, a section
  carrying many counts in one signal.
- **Rows come one signal each,** as `ROW_DATA_REP`, before or after the
  `SCAN_CONF` that counts them. Each names a receiver: an object the
  API made before the scan started, to receive rows. `SCAN_CONF` lists
  the receivers it reports on, by the ids the API gave them, which the
  API turns back into its objects.
- **The API sets the pace.** `SCAN_CONF` says whether the scan has
  ended. If it has not, the coordinator waits for `SCAN_MORE_REQ` to go
  on; by sending it the API agrees that the data nodes release the
  locks on the rows it has received. `SCAN_MORE_REQ` may instead ask
  for the scan to close. On `SCAN_MORE_REQ` the coordinator may answer
  at once with another `SCAN_CONF`, if rows are ready already.
- **Half-duplex, mostly.** Each side speaks in turn, but on an error
  either may close the scan when it is not its turn.
- Join pushdown uses the same protocol; it is not part of phase 8.

### 7.2 The signals, as read (2026-10-08; to be seen live in 8.2)

Read from the 26.10 sources before the author's account, and folded in
from §16.4; the captures of 8.2 confirm or correct each point. Words
count from 0: word 0 is a signal's first.

- **`SCAN_REQ` (32)**, eleven fixed words (the author, 2026-10-15):
  - 0: the scan's coordinator record, the one allocated with
    `COORD_ALLOC_REQ`;
  - 1: a spare, always 0;
  - 2: the flags;
  - 3: the table, or the index for a range scan;
  - 4: its schema version;
  - 5: a word with bits added lately;
  - 6–7: the transaction id;
  - 8: the owning transaction's coordinator record, only for a scan
    that updates, whose rows are taken over; else 0xFFFFFFFF, no owner,
    as for every committed-read scan (the author, 2026-10-15). Ours
    names one when the application starts a locking scan on a
    transaction, saying its rows may be taken over (the author,
    2026-10-16, chapter 04);
  - 9: the batch byte size, the whole word: parameter 801 goes up to 1M
    and the coordinator reads all 32 bits;
  - 10: the first batch's rows, in its low 16 bits.

  The high halves of words 5 and 10 carry further flags, allocated down
  from bit 31. RonDB's description of the request names word 9 as well,
  wrongly (the author, 2026-10-15). Word 10 has none yet, and the
  coordinator reads it whole. Word 5's low half is 0xFFFF as the
  reference sends it. Its high bits (the author, 2026-10-15):
  - 16–17: fragments per worker, for pushed-down joins; the coordinator
    reads them only with the join aggregation flag;
  - 30: the scan covers one partition, word 11 being its id;
  - 31: a scan of a table with a partition hash fanout. Such a table
    hashes in two levels: the first part of the primary key gives a
    first hash value, word 11, and the scan covers the fanout's number
    of partitions from it, which are contiguous.

  Each of 30 and 31 needs flag bit 26, and they are never set together.

  Optional words follow: the distribution key (11), a TTL window (12), a
  user id and its version (13–14).
- **Its flags**: 8 exclusive lock, 9 no disk, 10 hold lock, 11 read
  committed, 12 return each row's key, 13 storage order, 14 descending,
  15 range, 16–25 batch rows (0: use word 10, the reference puts the
  count in both up to 992 rows, and 0 here above that), 26 distribution
  key given, 27 join, 28 pass every confirmation, 29 extended
  confirmation (always ours), 30 read committed base, 31 several
  fragments per receiver (joins only, else 4003). (trust the constants,
  not the drawing beside them). Bit 9, no disk, says the scan reads
  memory alone: it is set only when neither the record nor the program
  touches a column kept on disk. The reference clears it for a program
  that reads, writes or branches on one; a filter on a disk column with
  it set fails with 927, "flag set to not use disk columns, but still
  done". Ours did not look at the program until the review of
  2026-10-21; a key operation never sets it. Bits 0–7 are RonDB's
  extensions, all 0 from us in phase 8 (the author, 2026-10-15):
  - 0: ring buffer metadata. 3 and 4: TTL (ignore it; expired rows
    only). 5: the user id for rate limits, with words 13–14. All later.
  - 1 and 7: join aggregation and aggregation, for pushed-down joins,
    which we do not support.
  - 2: continuous scan. Section 0 holds four receivers for each
    partition scanned in parallel, so that a committed-read scan goes on
    after reporting rows to the API, which raises the scan rate.
    Committed reads only (else 2202), receivers a multiple of 4 (else
    2201). Phase 10. How the four are used (the author, 2026-10-20): the
    coordinator asks the fragment for its first batch into the first
    receiver; once the fragment has reported that batch to the
    coordinator, it goes on at once with the second receiver, without
    waiting for the API, and so on round the four. That request and its
    confirmation pass between data nodes only and never reach the API,
    which needs no names for them. As the API sees it, a fragment's
    batches come in its receivers in turn: the first batch in the first
    receiver, the second in the second, the fifth in the first again.
    Why four (the author, 2026-10-21): one receiver whose batch is being
    merged, or read; the confirmed batches waiting behind it; one the
    prefetch is filling; and one kept cleared for the next prefetch.

    What the API gives back is the fragment record, not a receiver
    (confirmed by the author, 2026-10-21). The coordinator's fragment
    record holds the four receiver ids and an index into them. Each
    `SCAN_CONF` entry names the receiver at the index, then the record's
    pointer, which is the same for the four. Naming that pointer in
    `SCAN_MORE_REQ`, once for each entry and only while its batch is
    delivered, moves the index on to the next receiver. The fragment
    runs one batch ahead: once it has reported batch k it scans batch k +
    1 into the next receiver at once, sending its rows to the API as it
    goes. If it is done before k is named back it stops and holds k +
    1's confirmation; when k is named back it reports k + 1 at once and
    goes on with k + 2. So naming back batch k lets batch k + 2 come, in
    the receiver two on, and nothing in the data nodes waits for that
    receiver to be free: the API names k back only once the receiver is
    cleared, its batch k − 2 done with. Naming back does not wait for
    the batch's rows to be used: a committed read leaves no state in the
    data nodes for the rows it has delivered (the author, 2026-10-20).
    Ours names batch k back once batch k − 2 of the fragment has been
    handed back (chapter 04), which gives the four roles: k − 1 being
    read, k waiting behind it, k + 1 being filled, and k + 2's receiver,
    k − 2's, cleared. A slower reader still holds k − 2: then k − 1 and
    k wait behind it, k + 1 is filled, and the fragment waits until k −
    2 is handed back.

    A fragment's four receivers are four in a row in section 0 (the
    author, 2026-10-20): ids 0 to 3 are the first fragment scanned at
    once, 4 to 7 the second, and so on. When the fragment has no more
    rows, only the receiver whose batch is its last says done, with the
    null pointer (a fragment that reaches its end while it is a batch
    ahead waits to be named back, then reports that batch as its last),
    and the API counts the other three as done with it and names none of
    them back; rows it still holds from them are its own to use, as the
    data nodes hold nothing for them (the author, 2026-10-20). A
    continuous scan is a committed read and never a scan that locks (the
    author, 2026-10-20), so it has no takeovers. Which scans set it is
    the API's choice. Ours is `ScanArgs::continuous`, off by default
    (chapter 04; the author, 2026-10-21). The reference turns it on for
    a whole cluster connection by the API node's parameter 899,
    `ContinousScan`, false by default (whose text still says two
    receivers), for every committed read but one whose options ask for a
    standard scan. It then multiplies the parallelism by four, an
    ordered run's to four times the fragments, and sizes each receiver's
    batch by that count, so the bytes of all the receivers together stay
    within parameter 800. We size them the same way (the author,
    2026-10-21), and do not read 899: a scan's arguments alone decide.
    The reference asks for it only from data nodes of 25.10.1 and later;
    every 26.10 cluster has it, so we check no version.
  - 6: pass queueing. An API never sets it; doing so is an error.

  A committed read sets 11; shared and simple reads set hold lock; an
  exclusive read sets 8, hold lock and 12. The reference always sets 13
  for a table scan, and never for a scan of an ordered index, which sets
  15 in its place: 13 would read the table's fragment in storage order
  and leave the index out.
- **Its sections**: 0 the receiver ids, whose count is the parallelism;
  1 the column section, required (empty: 269), beginning with five size
  words: the read before the program, the program, then 0 and 0 (a scan
  does not update, and we send no final read; the author, 2026-10-15),
  and the subroutines; 2 the bounds. A large request is sent in
  fragments.
- **A record of its own.** A scan has a coordinator record of its own,
  with the transaction id. A scan started on a transaction, whose rows
  may be taken over, takes its record on the node and coordinator
  instance of the transaction's record, runs under the transaction's id
  and names that record in word 8. Any other scan, every committed read
  among them, names none, 0xFFFFFFFF (the author, 2026-10-15), runs
  under an id of its own and may go to any node. The reference instead
  names the transaction's record for every scan. One scan per record; a
  second gives 202.

  What the owner gives a scan, as read: the transaction's idle timer is
  kept fresh while the scan runs, and the scan takes the transaction's
  savepoint and fails at the start if it is aborting. The link is
  dropped quietly when the ids differ, as for a transaction not yet
  begun there. A takeover is matched at the data node by the row word
  and the transaction id, which is why such a scan runs under its
  transaction's id.

  The coordinator treats word 8 as none when it is 0xFFFFFFFF, or when
  it is not a valid record of the same transaction, and later requires a
  valid owner for every scan. Before RonDB d9f777a6847 (2026-10-16) it
  kept the word as it came, so a scan that names none could stop the
  data node. The author's mend has a scan with no owner named take its
  own record as its owner, so the check passes. Every scan of the
  `scan_` group ran on a tree with the mend.
- **Receivers.** Receiver i serves fragment record i, and min(receivers,
  fragments) are used. In a continuous scan, receivers 4i to 4i + 3
  serve fragment record i (flag bit 2, above). The receiver id is word 0
  of `ROW_DATA_REP` and of `SCAN_ROW_KEY_REP`, and is named per receiver
  in `SCAN_CONF`.
- **`SCAN_CONF` (29)**: our pointer for the scan's record (the one sent
  in `COORD_ALLOC_REQ`), a word with the receiver count and bit 31 end
  of data, the transaction id; then an entry per receiver. Its first two
  words are the receiver's id and the coordinator's pointer for it
  (0xFFFFFF00, the null record pointer,: that receiver is done). The
  rest depends on flag bit 29 of the request:
  - Bit 29 off: len << 10 | rows, and when rows reach 1023 that word
    holds 1023 and a fourth word the count.
  - Bit 29 on: rows, len, and the join's active mask, five words in all.
    An API older than 8.0.20 gets four words, without the mask.

  We always set bit 29, take the five-word entry and ignore its last
  word, so batches of up to 4 000 rows need no special case (the author,
  2026-10-15). The reference sets it only for pushed-down joins. The
  entries are inline while the signal stays within 25 words, else in
  section 0. len counts row words and key words.
- **A receiver's batch is complete** when its confirmation has come and
  the words received equal len; rows and confirmation come from
  different nodes in either order. len 0 with rows means an empty
  projection.
- **The end.** The last confirmation carries end of data with the
  counts, and the record is then free again; a confirmation that is
  exactly end of data confirms a close. A fragment that ends with no
  rows while others remain is not reported: the coordinator starts the
  next fragment on the same receiver. In a continuous scan the receiver
  with a fragment's last batch alone says done, and its three companions
  are done with it (flag bit 2, above).
- **`SCAN_ROW_KEY_REP` (33)**, one per row when keys are asked for: the
  receiver id, the key length, a word of operation number << 8 | scan
  number | fragment << 20, the transaction id, then the key, inline up
  to 20 words, else in section 0.
- **`SCAN_REF` (31)**: the record, the transaction id, the error, and
  whether a close is owed: 0 for errors at the start, 1 from errors
  during the scan. On 0 the scan is over and the record free. On 1 we
  send `SCAN_MORE_REQ` with stop and wait for the bare end of data. That
  stop names one pointer, 0xFFFFFFFF, as the reference's does: the full
  five words, though the coordinator ignores the list (the author,
  2026-10-15).
- **`SCAN_MORE_REQ` (28)**: the record, a stop word, the transaction id,
  the coordinator's pointers of the receivers to go on (inline up to 21,
  else section 0); each named fragment goes on by itself, one not named
  waits. We name subsets as the reference does (the author, 2026-10-15):
  an ordered scan refills the one receiver its merge has emptied; an
  unordered scan names every receiver whose batch it has used, and those
  whose batches came meanwhile wait for the next round. A receiver
  reported done (the null pointer) is never named. In a continuous scan
  (flag bit 2, above) the pointer named is the fragment record's, the
  same for its four receivers, named once for each batch reported: ours
  names batch k once the fragment's batch k − 2 is done with, whether or
  not k's rows have been used. A pointer named must be one whose batch
  the API holds, named once: before RonDB 99ea088977e (2026-10-16) the
  coordinator did not check it and stopped the data node if it was
  wrong; since then it refuses it with 2206, "Invalid scan continuation
  request". Stop set: the coordinator closes at once, ignoring the list,
  closes the fragments, and sends the bare end of data confirmation.
  While a scan closes after an error, a `SCAN_MORE_REQ` without stop is
  ignored. In phase 8 we close as the reference does: first wait for
  every batch asked for, then send stop naming the receivers held, then
  wait for the bare end of data. A stop at once, with batches on their
  way, should work too and is likely better, but waiting is the safer
  (the author, 2026-10-15); it is left for phase 10. In a continuous
  scan a fragment that is a batch ahead when the stop comes drops that
  batch. Its rows already sent never have a confirmation, and since they
  come straight from the fragment while the end of data comes through
  the coordinator, some may reach the API after it. They are dropped, as
  is any row that names no receiver of a live run with that run's
  transaction id, and nothing more is waited for (the author,
  2026-10-21). Each run takes new receiver ids, whose generation tells
  them from an earlier run's in the same slot
  (`crates/ic_util/src/ptr_array.rs`), and a run under an id of its own
  a new transaction id, so no later run takes them in. The same holds
  for any batch on its way when a stop is sent at once.

  Phase 10 sends the stop at once, without waiting for the batches
  asked for (the author, 2026-10-21), for a committed read and for a
  locking scan whose transaction is rolling back; a locking scan with
  no transaction takes over nothing, and stops at once too. A locking
  scan on a transaction still going first waits until its pending
  takeovers have been received, then stops at once: the stop releases
  the locks of the rows the scan holds, and a takeover must reach them
  first. A `SCAN_CONF` the coordinator sent before the stop reached it
  is let go of, its receivers named back no more.
- **Batches.** A batch ends at the row count or the byte count, checked
  after a row is added, so one row may overshoot. The reference asks for
  bytes = min(parameter 801, parameter 800 / parallelism) and rows =
  min(parameter 802 or the scan's own, 992 for an exclusive scan or 4
  000 for any other, bytes); defaults 384 rows, 32 KB, 512 KB. The 992
  comes from the ten-bit operation number used by takeover. The data
  node requires a batch of 1 to 992 rows on an exclusive scan and 1 to 4
  000 on any other, and keys on an exclusive scan, and stops otherwise;
  the coordinator passes the rows on unchecked. A shared-lock scan that
  asks for keys may thus have 4 000 rows a batch, and rows past 1 023
  get an operation number that wraps. Mended by the author in RonDB
  c2f5eff326b (2026-10-16): the coordinator refuses a batch of 0 rows or
  of more than 992 for any locking scan, shared ones included, or 4 000
  for committed reads, with 1230, "Invalid scan batch size", and an
  exclusive scan without keys with 2205, and the data node holds every
  locking scan to 992. Our locking scans, shared ones included, ask for
  at most 992 rows a batch; committed reads for up to 4 000 (the author,
  2026-10-15). The reference asks for more only when every cached row is
  consumed: its policy, not the protocol's.
- **Timeouts.** The coordinator's inactivity timer runs only while no
  fragment is running; past parameter 130 (`idle_transaction_limit`) the
  scan fails with 296 and a close owed (the default in 26.10 is 3 600
  000 ms). A fragment stalled past the deadlock timeout gives 274;
  RonDB's deadlock detection may end a scan with 296. The reference's
  client gives up with 4008 after three times the check interval plus
  the deadlock timeout.
- **Takeover**: a `KEY_OP_REQ` on the owning transaction whose key is
  the row's key from `SCAN_ROW_KEY_REP`, with flags bit 14 (scan
  takeover) and bit 2 (distribution key given), and optional words after
  the user id: 1 | (the row word & 0x3FFFF) << 1 | fragment << 20, then
  the fragment as distribution key. Its `KEY_OP_CONF` must have come
  before the `SCAN_MORE_REQ` that names the row's receiver: that request
  releases the scan's locks on the batch, and only the confirmation
  shows that the update, write or delete is in place before the lock
  goes. Sending it first is not enough (the author, 2026-10-15). It must
  also come after the batch's `SCAN_CONF`, though its rows may come
  earlier: until then the fragment's scan is not waiting for more, and
  the takeover is refused.
- **Node failure**: a reading node lost gives `SCAN_REF` 270, temporary,
  with a close owed; the coordinator's node lost: the reference reports
  4028, no close is sent, the record is gone; our API node lost: the
  coordinator closes the scan. Nothing resumes a scan; it starts again
  and rows come again.
- **Range scans**: per bound column a word of bound type (0 ≤, 1 <, 2 ≥,
  3 >, 4 =; low bounds use 0 and 1, high bounds 2 and 3), a column
  header (index column id, byte length) and the value padded to words;
  each range's first word also holds len << 16 | range number << 4,
  range numbers up to 0xFFF and rising strictly in an ordered scan with
  several ranges; an open range is type 0 with a header of (0, 0). The
  header names the column by its id in the index's own table, and its
  length counts a variable-sized value's length bytes, which are sent
  with it. A range's bounds go column by column, a column's low bound
  before its high one. Every column of a bound but its last is sent
  inclusive (0 or 2), so a bound is a prefix compared as the index
  orders its rows, and only the last column says whether the bound's own
  values are in the range. A range whose two bounds are the same row,
  with as many columns, both inclusive, goes as type 4 alone.
- **The range number in the row.** A scan that reads range numbers puts
  the pseudo-column 0xFFFB, size 0, before the read in its column
  section. Each row then begins with 0xFFFB, size 4, and one word, the
  number, before the packed row.
- **Ordered range scans** (the author, 2026-10-15). `SCAN_REQ` has no
  ordering flag: an ordered range scan scans every fragment in
  parallel, a receiver each, and the client merges.
  - Without the continuous scan, once the first batches have filled the
    buffers, rows are fetched from one fragment at a time, as the merge
    empties its receiver.
  - The continuous scan (phase 10) would fetch further rows ahead
    through a fragment's other receivers. It is for committed reads
    only, never for a locked ordered scan.
  - With several ranges, each fragment returns one range at a time, in
    the order sent, and the fragments are not kept in step: one may be
    in its second range while another is still in its first. So the
    merge orders by range number, then by the index. The reference reads
    each row's range number as a pseudo-column and compares it first.
  - The descending flag (14) reverses each fragment's order, and the
    merge's comparison of keys with it; ranges keep the order sent.

  The reference asks for as many receivers as the index has fragments,
  and the client needs a row or the end from each before returning one.
  The coordinator moves a receiver on to a further fragment only when it
  has fewer receivers than fragments. An ordered scan must therefore
  never have fewer.
- **Pruning**: flag bit 26 and word 11, with word 5's bits 30 and 31.
  - Bit 26 alone: word 11 is a hash, mapped to one fragment through the
    hash map.
  - With bit 30: word 11 is a fragment id, checked against the table's
    fragment count.
  - With bit 31: word 11 is a base hash, and the fanout's number of hash
    values from it are scanned.

  The coordinator refuses these with 241:
  - bit 30 or 31 without bit 26;
  - bit 31 on a table without a fanout;
  - bits 30 and 31 together.

  On a fanout table bit 26 alone is refused with 2204, a permanent
  application error. Until 90a51a1a331 (2026-10-15) it was 2203, which
  also means read rate overload, and the reference's client, which takes
  a code's first entry, reported it as that: temporary. A tree from
  before the mend still sends 2203. The reference prunes a range scan
  only when both bounds fix every distribution key column, and sends the
  key's hash, bit 26 alone. Ours names the partition by its id, bits 26
  and 30, as for a table, which the coordinator checks against the
  scanned object's fragment count: an ordered index has a fragment for
  each of its table's. Seen to work in 8.7 (2026-10-17): runs pruned so,
  by a table's distribution key and by its primary key's ordered index,
  read every row MySQL selects, with one receiver.

  Pruning to the fanout's partitions is a pure optimisation and not in
  phase 8: a fanout table's scans cover every partition, which is
  correct (the author, 2026-10-15). Phase 10 does it (the author,
  2026-10-21): when the base columns are fixed by an equality, the API
  hashes them, and the scan covers only the fanout's fragments. In a
  1:1:4 table an equality on the primary key's first column leaves 4
  fragments, and the API sets up the scan for those 4 alone. It
  combines with the continuous scan, four receivers for each.

  Which fragments those are the coordinator works out, not the API (the
  author was unsure; read from the tree, 2026-10-21, for the captures to
  confirm). With bit 31 it takes word 11 as the first of the fanout's
  hash values, scans that many fragments, and maps each hash value, word
  11 plus the fragment's place, through the hash map as it sends the
  fragment its request. It uses at most the fanout's number of
  receivers, so the API sends that many, or four times that many in a
  continuous scan. A whole distribution key fixed, base and detail,
  places the run in one partition by the full hash, which goes by its id
  with bits 26 and 30, as on any table: the coordinator takes that as an
  exact fragment on a fanout table too. We never send bit 26 alone on a
  fanout table, so its 2204 does not arise; our table classes 2203 as
  the overload and 2204 as the refusal, as the kernel now does.

### 7.3 Asked of the author (2026-10-15)

Each answer goes into 7.2, and the captures check it. All fourteen
were answered on 2026-10-15; what the captures still have to show is
said with each.

1. `SCAN_REQ` word 1: a spare, sent as 0? Yes, always 0 (answered
   2026-10-15).
2. Word 5 for a plain scan, neither pruned nor a join: 0x0000FFFF? When
   are the fragments-per-worker bits (16–17) set? For pushed-down joins;
   30 is one partition, 31 the fanout, each needing flag bit 26 and
   never the two together, as the tree has it (answered 2026-10-15).
   The low half's 0xFFFF is for the captures to show. The double 2203
   was the author's to mend: the fanout refusal is 2204 since
   90a51a1a331 (2026-10-15), §7.2.
3. The flags' bits 0–7: may a plain scan leave them 0? Yes, all of them
   in phase 8; bit 2 is the continuous scan, four receivers per
   partition, for phase 10; bit 6 is never set by an API (answered
   2026-10-15). How a partition's four receivers are used is for
   phase 10 to ask: asked 2026-10-20, a fragment's batches come in its
   four receivers in turn, the fifth in the first again, the fragment
   going on to the next receiver as soon as a batch is reported (§7.2,
   flag bit 2). Confirmed 2026-10-21: the API names back the fragment
   record, once for each batch, and the fragment runs one batch ahead
   of it; the four receivers' roles are recorded there.
4. The batch's rows: flags bits 16–25 or word 10? Is 0 in the bits and
   the count in word 10 right? The coordinator takes the bits unless
   they are 0, then word 10. Word 9's bytes take the whole word, not 16
   bits (answered 2026-10-15): the reference sends min(801, 800 /
   parallelism) as it is.
5. The column section's five size words: what each is, and for a plain
   read of columns, is it [n, 0, 0, 0, 0] then the n words of the list?
   They are as for an interpreted key operation (§6.4); the reference's
   scan sends [read, program, 0, 0, subroutines]. A scan cannot update,
   and a final read is of no value, so ours are [read, program, 0, 0,
   subroutines] with the read part the words a key read sends (answered
   2026-10-15). The captures check the read part.
6. `SCAN_CONF`: is the three-word entry the one to expect, and when does
   the fourth word or the extended form (bit 29) come? Should a client
   ask for the extended form? Yes: always set bit 29 and take the
   five-word entry, ignoring its last word (answered 2026-10-15).
7. "A new `SCAN_CONF` at once if rows are ready": do fragments fetch
   their next batch ahead while the API holds the last, or is it only
   receivers not named before that have finished meanwhile? As read: the
   coordinator sends a `SCAN_CONF` whenever a fragment's batch is done,
   so several may come between two `SCAN_MORE_REQ`s. Without the
   continuous scan a fragment does not scan ahead; it waits for a
   `SCAN_MORE_REQ` naming its receiver. The rows "ready" are another
   fragment's batch already on its way. The captures show the timing.
8. May `SCAN_MORE_REQ` name a subset of the receivers holding batches,
   each named one going on alone? Must every receiver with a batch be
   named eventually before the end of data? As read: yes to both. End of
   data comes only when no receiver holds a batch or runs, so each must
   be named again or the scan closed. The author (2026-10-15): name
   every receiver a `SCAN_CONF` reported; the protocol may allow a
   subset, but the coordinator may not handle one. Yet the reference
   names subsets: an ordered scan refills one receiver at a time, and an
   unordered one names the receivers whose rows it has used, leaving
   batches that came meanwhile for the next round. Asked again, the
   author agreed: we do as the reference does (2026-10-15).
9. Closing: after `SCAN_MORE_REQ` with stop, does the API wait for the
   bare end of data `SCAN_CONF`, or a `SCAN_REF`, before it reuses the
   scan's record and receivers, dropping rows that come meanwhile? As
   read: the coordinator takes a stop at any time. It closes running,
   held and queued fragments, sends the API nothing from fragments that
   report while it closes, and then sends the bare end of data. The
   reference instead first waits for every batch it asked for, then
   sends stop naming the receivers it holds. May we stop at once?
   Waiting is safest; an immediate close should work and is likely
   desirable (answered 2026-10-15). Phase 8 waits; the immediate close
   is for phase 10. Rows that come after the end of data are dropped by
   the run's transaction id and receiver ids, with nothing more waited
   for (answered 2026-10-21, for a continuous scan's batch ahead and for
   any batch on its way; §7.2, `SCAN_MORE_REQ`). Phase 10 stops at once,
   but a locking scan on a transaction still going first waits for its
   pending takeovers (answered 2026-10-21).
10. `SCAN_REF`'s close-owed word: 1 means the API sends `SCAN_MORE_REQ`
    with stop and waits as in 9; 0 means the scan is over and its record
    free? As read: yes. On 1 the reference sends stop naming one made-up
    pointer, 0xFFFFFFFF, which the coordinator ignores, as it ignores
    the whole list with stop. May our stop name no pointer at all, a
    four-word signal? No: send the full signal as the reference does,
    five words with that pointer, though the coordinator ignores it
    (answered 2026-10-15).
11. The scan's own record: word 0 is a record allocated with
    `COORD_ALLOC_REQ`, and word 8 the transaction's record or the null
    record (answered 2026-10-15). Still open: must the scan's record be
    on the node and coordinator instance of the transaction's record?
    May a transaction run several scans at once, a record each? With the
    null record in word 8, does the scan begin a transaction of its own?
    As read: every reference scan gets a record of its own, on the node
    and instance of the transaction's record, with the same transaction
    id, naming it in word 8. Word 8 as 0xFFFFFFFF reads as none, and one
    that is not a valid record of the same transaction is quietly taken
    as none. But the coordinator keeps word 8 and later requires it to
    be a valid record for every scan, so a scan without one would stop
    the data node. The author: 0xFFFFFFFF is no owner, and only a scan
    that updates names one; committed-read scans send 0xFFFFFFFF
    (answered 2026-10-15). The check is the author's, in a session of
    its own. Several scans at once in a transaction, each on its own
    record, follow from one scan per record.
12. Takeover: flags bit 14, bit 2, the information word and the fragment
    as the distribution key, as above? Must it reach the coordinator
    before the `SCAN_MORE_REQ` that releases the row, or does sending it
    first on the same link suffice? The words as above. Wait for its
    `KEY_OP_CONF` before naming the receiver again: only that guarantees
    the operation came before the lock was released (answered
    2026-10-15).
13. The 992 rows of a locked batch: a limit of the protocol? As read:
    yes for exclusive scans, where the data node stops on more;
    shared-lock scans may have 4 000, and with keys their rows past 1
    023 get a wrapped operation number. We cap every locking scan at 992.
    Is the shared-lock case a RonDB bug? Takeover past 992 sounds like a
    bug, for the author in a session of its own. Every locking scan
    stays at 992 rows a batch, shared ones included; committed reads may
    go to 4 000 (answered 2026-10-15).
14. Ordered range scans: do each receiver's rows come in index order,
    for the client to merge, in every case? Yes: every fragment in
    parallel, range by range with the fragments not in step, reversed
    by the descending flag; after the first batches, one fragment is
    fetched at a time unless the continuous scan is used, which a
    locked ordered scan cannot (answered 2026-10-15).

For the author, in sessions of their own (raised 2026-10-15):
- error 2203 had two meanings, the fanout scan refusal and read rate
  overload: mended in RonDB 90a51a1a331 (2026-10-15), the refusal now
  2204;
- RonDB's description of `SCAN_REQ` counts word 9 among the words whose
  high half is free;
- the coordinator's check that every scan names a valid owning record,
  which a scan naming none would fail: mended in RonDB d9f777a6847
  (2026-10-16);
- takeover past row 1 023 of a shared-lock batch, whose operation number
  wraps: mended in c2f5eff326b, locking batches held to 992;
- two things the data node stops on that a refusal would serve: a
  `SCAN_MORE_REQ` naming a pointer not held, and a batch's rows outside
  1 to 992 or 4 000, or an exclusive scan without keys: mended in
  99ea088977e (2206) and c2f5eff326b (1230, 2205).

### 7.4 Seen on the wire (2026-10-16)

A capture of `tools/scan_capture.sh` against the release cluster (kept as
`~/captures/scans1.pcap`, the second as `scans2.pcap`): 1 000
rows over 8 fragments, 100 to 160 a fragment, so every fragment fitted
in one batch of the reference's 384 rows. Read with the dissector; it
confirms 7.2 where it says nothing below.

- **`SCAN_REQ`.** Word 1 is 0, word 5 0x0000FFFF, flag bits 0–7 and 29
  are 0, and the batch rows (384) are in both flag bits 16–25 and word
  10. Word 8 names the transaction's record in every scan of the
  reference, committed reads included; each scan's own record comes
  from a `COORD_ALLOC_REQ` before it and goes back with a
  `COORD_FREE_REQ` after it.
- **Word 9 may be 0.** The reference lowers the bytes to the rows times
  its estimate of a row: 0 when nothing is read (`--nodata`), 3 072 for
  keys alone. The data node then limits by rows alone.
- **Flags by lock mode:** committed reads 11; shared locks 10, without
  keys; exclusive locks 8, 10 and 12. Bits 9 (no disk) and 13 (storage
  order) are always set on a table scan, so the reference's storage
  order option changes nothing on the wire.
- **The column section.** The read part is the read-by-bitmap words of
  a key read (0xFFF3 and the bitmap), [0, 0, 0, 0, 0] when nothing is
  read. MySQL's range scan reads the range number first (0xFFFB, no
  value). A pushed condition is the program part (5 and 8 words).
  Parts 3 and 4 were always 0.
- **Committed reads.** Each `SCAN_CONF` reports one receiver, done when
  its fragment ended within the batch. The last has end of data, with
  no `SCAN_MORE_REQ` at all. With parallelism 1, the one receiver went
  through all 8 fragments, a `SCAN_MORE_REQ` naming it after each, its
  pointer unchanged.
- **Locking scans.** No receiver is done in its first `SCAN_CONF`, even
  when the batch held the fragment's every row. After the
  `SCAN_MORE_REQ` that releases the locks, each comes back with 0 rows
  and done.
- **`SCAN_MORE_REQ`** named subsets as the confirmations came: [06 01
  03], then [05], then [00 07 04], with several `SCAN_CONF`s in between
  (7.3 questions 7 and 8).
- **Rows and their confirmation.** Rows and keys come straight from the
  data node scanning the fragment, over its own link; `SCAN_CONF` comes
  from the coordinator. Rows may come after their `SCAN_CONF`, even
  after end of data on the same link. A batch is complete only when
  its words are all in.
- **Takeover** (MySQL's FOR UPDATE, UPDATE and DELETE). MySQL took over
  the rows of the receivers whose `SCAN_CONF` had come and whose rows it
  had used, waited for every `KEY_OP_CONF`, then named exactly those
  receivers; then the next round, then `COMMIT_REQ` (7.3 question 12).
  The takeover goes to the record of word 8, with flags 14 and 2, the
  information word (0x181 for row word 0xC0), the fragment as the
  distribution key, and the operation: read exclusive for FOR UPDATE,
  with the key alone. The first carries Start.

Not seen, because no fragment needed a second batch: a close
(`LIMIT 5` ended by itself), the merge's refill of one receiver, more
than one range, a fourth `SCAN_CONF` word. No `SCAN_REF` either. So a
second capture was taken the same day, with 20 000 rows, about 2 500
a fragment:

- **The ordered merge.** After the first batches, the reference's
  ordered scan names one receiver at a time, the one its merge emptied,
  and waits for that receiver's `SCAN_CONF` before naming the next.
  MySQL's ordered range scans do the same (7.3 question 14).
- **Two ranges.** Each fragment returns all of range 0, then all of
  range 1, and the fragments come to range 1 at different times; a
  fragment whose range 0 fits in a batch sends both in that batch. The
  range number comes first in every row. The descending range sets flag
  14 alone.
- **A fragment's last batch** comes back done; when the batch before
  happened to end the fragment, the last is 0 rows and done.
- **Closing.** For `LIMIT 5`, with and without locks, MySQL waited
  until every batch it had asked for had come, took over its five rows
  in the locking case and had their `KEY_OP_CONF`, then sent stop
  naming all eight receivers it held. The coordinator answered with the
  bare end of data (no entries), and the locking transaction committed
  (7.3 question 9).
- **`SCAN_REF`.** A locked scan that met a row another transaction
  held: the other seven receivers ended, and 3.2 s later, the deadlock
  timeout under MTR, came `SCAN_REF` 274 with a close owed. The
  reference sent stop, five words naming 0xFFFFFFFF, had the bare end of
  data, and rolled back (7.3 question 10).

Still not seen: a `SCAN_CONF` with more than one entry (every one had
one), and a fourth word, since no batch reached 1 023 rows.

Read again for takeover (phase 8.5, 2026-10-16), the second capture:

- **A key's operation number is its row's place in the batch.** Over the
  30 849 `SCAN_ROW_KEY_REP` of the capture, in 67 batches, the number
  (bits 8 to 17 of the row word) starts at 0 in every batch and rises by
  one per key, with no gap; the row word's low byte was 0xC0 throughout.
  The keys of a batch come before its rows, a few at a time, and the
  data node packs several rows in one `ROW_DATA_REP`; each kind keeps
  the scan's order. The reference pairs the n-th key with the n-th row
  as they come, and so do we.
- **A locking scan's last batch carries no rows.** All 81 entries that
  named the null record for a locking scan reported 0 rows; those of
  committed reads carried rows in 199 of 225. So every batch whose rows
  may be taken over is followed by a request for more, which is what
  waits for the takeovers.
- **The reference takes over in the scan's lock**: a lock taken over
  reads shared from a shared-lock scan and exclusively from an exclusive
  one; updates and deletes from either; on a base table's record only,
  aborting on error.

Closing at once (phase 10, 2026-10-21), a capture of our own runs of
`scan_closed_at_once_with_every_batch_on_its_way` and
`scan_closed_with_takeovers_pending_waits_for_them` (kept as
`~/captures/close.pcap`):

- **The stop goes right behind the request.** For each of the three
  runs closed at once, committed, shared and exclusive, the stop went
  with `SCAN_REQ`, in the same frame or 8 µs after it, and the bare end
  of data came back 90 to 200 µs later. No `SCAN_CONF` reported a
  batch in between. The fragments had begun all the same: each run
  had part of a batch on its way, 149 rows from one node for the
  committed read, rows and keys from both for the exclusive one, which
  no confirmation ever counted.
- **Those rows all came before the end of data**, on the same links,
  so the closing run dropped them itself; none came after it, and the
  late count had nothing to count in this capture.
- **A close on a transaction that goes on waits for its takeover.**
  The takeover's `KEY_OP_REQ` went out, its `KEY_OP_CONF` came back,
  and the stop followed 38 µs later, then the bare end of data. No
  request for more went between the batch handed back and the stop,
  since the batch's receiver waited for its takeover. Reads of the
  rows the scan had held and not taken over, by another transaction
  without waiting, each found its row free before the commit.

The continuous scan (phase 10, 2026-10-21), a capture of our own run of
`continuous_scan_reads_every_row_on_a_connection` (kept as
`~/captures/continuous.pcap`), 2 500 rows over 8 fragments:

- **The request.** Flag bit 2 set (the extensions byte 0x04) on every
  continuous run, and not on the plain one after them. With every
  fragment at once, section 0 held 32 receivers, four for each, and the
  batch bytes were 16 384, parameter 800 over the 32; with one or two
  fragments at once, 4 or 8 receivers, the bytes stayed at parameter
  801's 32 768.
- **A fragment's batches go round its four receivers.** In the run of
  7-row batches over every fragment, 363 batches came, each fragment
  record's strictly in turn from its first receiver, round and round
  (41 to 48 batches a record). Every entry of a record carried the same
  coordinator pointer, its fragment record; each of the 355 batches not
  last was named back once, by that pointer, and the 8 last, with the
  null pointer, were not.
- **One record, the table's fragments one after another.** With one
  fragment at once in batches of 100, the 30 batches went round the
  four receivers seven and a half times, the record going on from one
  fragment of the table to the next without a break in the turn: a
  fragment's short last batch, 25 rows, was followed by the next
  fragment's first in the next receiver. The last batch, 81 rows, came
  in the record's second receiver with the null pointer.
- **A fragment whose rows fit one batch** ends in its first receiver:
  in the run of 384-row batches each of the 8 fragments, about 310
  rows, came in one batch that was its last, at place 0 of its four,
  and nothing was named back.

Fanout tables (phase 10, 2026-10-21), a capture of our own run of
`fanout_scans_by_the_base_read_the_fanouts_partitions` (kept as
`~/captures/fanout.pcap`), its 33 runs over the 1:1:4, 2:1:4 and
1:2:4 tables:

- **A base fixed**: flag bit 26, word 5's bit 31, and word 11 the
  base's first hash value, a multiple of 4 each time, with 4 receivers,
  16 in a continuous scan. At most 4 fragment records reported. The
  same base through two indexes, `a = 7` through the primary key's
  and through `(v, a)`, named the same word 11, 0x57ae4480: the base is
  hashed the same wherever its columns are.
- **The whole key fixed**: bits 26 and 30, word 11 the partition's id,
  one receiver, or four continuous, one entry.
- **A base column left open**: word 5 plain, every fragment, 8
  receivers or 32, each fragment reporting.

## 8. Dictionary

- `TABLE_DESC_REQ` (signal 24): the caller's data, the caller's
  reference, the request type (0 by id, 1 by name, +2 for a long
  confirmation), the table id or the name's length, the schema
  transaction id; name in section 0. `TABLE_DESC_CONF` (signal 190): the
  caller's data, the table id, a global checkpoint, the total length,
  and the table description in section 0 (fragmented if large).
  `TABLE_DESC_REF` (signal 23) with error codes 709, 723, 701.
  iClaustron's C code has the structures and signal numbers but never
  sends the request.

  As built (`ic_signals::table_desc`, `ic_apid::dict_client`),
  each point checked against the reference:
  - The name is the internal `database/def/table`. The length word
    counts a terminating NUL, and section 0 is the name, the NUL and
    zero padding to whole words, in native byte order since the receiver
    reads it as bytes. The request goes to `DICTIONARY` on any started node,
    with request type 3 and schema transaction id 0.
  - The answer's six words are the caller's data, the table id, the
    global checkpoint, the total length in words, the table's type, and
    the sender's reference. `TABLE_DESC_REF` is seven words, with
    the error code sixth and the source line seventh; an older data node
    sends five with the code last.
  - The reference asks up to a hundred times, 50 to 100 ms apart with a
    random spread, on Busy (701), on the node failing mid-request, and
    on a timeout, each time to an alive node. 723 and 709 mean no such
    table and are final.
  - After parsing, a table placed by hash map needs its hash map, fetched
    by id the same way (request type 2, the id in word 4, no section).
    As built in `dict_client::get_hash_map` and
    `table_props::parse_hash_map_info`:
    - Keys: name 1, bucket array size 2, bucket array 3; the map's id and
      version reuse the table keys 153 (hash map id) and 154 (hash map
      version). Every value defaults to zero, and there is
      no end marker.
    - **The bucket count is sent as a size in bytes**, two per bucket;
      the reference halves it after reading. The buckets are 16-bit
      fragment numbers copied as the data node holds them, so they are
      read in the receiver's own byte order.
    - A row goes to fragment `buckets[hash % buckets.len()]`, the hash
      taken over the distribution key. `DEFAULT-HASHMAP-3840-8` is 3840
      buckets over 8 fragments.
- The property stream (network byte order): head word `(value type << 16)
  | key`; type 0 a 32-bit number, one word; 1 a string and 2 binary
  data, a length word then padded bytes; 4 a 64-bit number, two words
  low then high. iClaustron's `fill_create_table_info_properties` writes
  this format.

  **Two byte orders in one buffer.** Heads, numbers and lengths are
  stored with `htonl` into words of their own; string bytes are copied
  in raw. The words then travel in the sender's order like any signal
  word. So a receiver of the same byte order reads a number as `from_be`
  of the native word and a string as the native bytes of its words. A
  string's length counts its terminating NUL. The reader skips keys it
  does not know.
- Table property keys (`IC_TP_*`): table keys 1–30 and 127–171 (1 name,
  2 id, 3 version, 5 key column count, 6 column count, 12 key length, 13
  fragment type, 18 table type, 20 base table id, 128 fragment count,
  130 fragment data, 153 hash map id, 154 hash map version, 158 read
  backup, 159 fully replicated, 160 partition count; RonDB's 163 hash
  function, 164 TTL in seconds, 165 TTL column, 166 fanout base key
  count, 167 fanout detail key count, 168 fanout, 169 ring buffer size,
  170 ring index column, 171 ring meta column), 999 table end; column
  keys 1000–1021 (1000 name, 1001 id, 1003 element size, 1005 array
  size, 1006 in the key, 1007 storage, 1008 nullable, 1010 in the
  distribution key, 1013 type, 1014–1016 precision, scale and length,
  1017 auto-increment, 1019 array kind, 1020–1021 default value length
  and value), 1999 column end. **Only non-default values are sent, so
  the default table is part of the protocol.** iClaustron's
  `ic_apid_dict_signals.h` already lists keys and defaults.

  How the reference reads it, and so how `ic_signals::table_props`
  does:
  - **The table part ends at the first column name key (1000)**, which
    is where the reference's table reading stops; table end (999) is not
    among the keys it reads and is skipped like any unknown key. Each column then
    runs from its name key (1000) to column end (1999), and there must
    be exactly as many as the column count (key 6) says.
  - **The defaults are the ones the reference's reader sets before
    reading, not the ones the comments beside the keys name**. The
    comments are out of date in several places: the default fragment
    type is hash-map partitioning (9), not a small table; the key column
    count (key 5) starts at 0, not 1; the minimum load factor (key 10)
    is 78, not 70. A column defaults to type Unsigned, 32-bit elements,
    one element, fixed array, in memory.
  - **Sizes are worked out, not read.** The reference works out the
    element size and count from type, length, precision and scale, and
    its reader does so for every column. Decimals take MySQL's binary
    decimal size (four bytes per nine digits either side of the point,
    less for the rest), with precision at most 65 and scale below 31.
  - The character set number is the high half of the precision key
    (1014).
    Char, Varchar, Longvarchar and Text must have one and nothing else
    may, or the table is refused.
  - A default value is binary: a four-byte column header in network
    order whose low 15 bits are the value's byte size, then the value.
    **Every column is sent one; a size of zero means no default.** The
    value is not in network order, whatever the comment beside the code
    says: the reference's byte order conversion only acts on a
    big-endian host (or twice, cancelling out, in a debug build), so on
    a little-endian API the bytes are used as the data node stored them.
    Found live: a first `ic_desc` printed a default on every column.
  - **No column marked as distribution key means the whole primary key
    is.** A table created without a partitioning clause sends no
    distribution key flag (key 1010) at all; the reference then marks
    every primary key column ("none is all"), and does the same when all
    of them are marked. Key hashing depends on it. Found live: the
    reference's table description tool showed the key columns as
    distribution key where a first `ic_desc` did not.
  - The "frm data" the reference's table description tool counts is
    whichever of key 27 (the old frm image) or key 30 (MySQL's
    dictionary metadata) arrived; the reference keeps either as the same
    blob.
- **A table's schema version is two counters** (author, 2026-09-19):
  bits 24–31 go up by one for every online change, bits 0–23 for every
  offline change. Operations are checked against bits 0–23 only, so an
  online change leaves operations prepared with the previous version
  working, while an offline change makes them fail with a wrong schema
  version. The reference's table description tool prints the whole word:
  seen live, a table altered once online reads 16777217, which is
  0x01000001. `ic_desc` prints the two apart, as upper 1, lower 1
  (`TableProps::version_upper` and `version_lower`). For the table cache
  (below) this means an online change is not noticed through errors; the
  API learns of it from the dictionary's notice (below) and refetches.
- After parsing a hash-map table, fetch its hash map object (also via
  `TABLE_DESC_REQ` by id) to fill the hash → fragment array.
- Cache: per-connection local cache plus a global, mutex-protected,
  ref-counted cache keyed by internal name `db/schema/table`; schema
  change detected by table version mismatch (errors 241/284) forcing a
  refetch.

  As built (`ic_apid::dict_cache`, `ApidConnection::table_bind` and
  `index_bind`), with what was found in the reference on the way:
  - **The dictionary tells API nodes when a table changes.**
    `TABLE_CHANGED_REP` (signal 606) goes to every API node's
    `NODE_WATCH` block (0x0FA2), unasked. Three words, table id, table
    version and kind of change (1 altered, 2 dropped), and the internal
    name in section 0, NUL-padded to 128 bytes. The version is the one
    that no longer holds, the one before an alteration, and the name is
    the one before a rename.
  - An alteration is sent by every data node, through its own
    `MEMBERSHIP` block, to every API node registered there, so it
    arrives once per data node.
  - A drop is sent by the master alone, and only to API nodes whose
    reported version is 26.05 or later, or 26.02.5 and 25.10.14 in their
    series. The code, not its comment, decides: 26.04 does not get it.
    We report 26.10.
  - The reference lets go of the cached object whose name, id and
    version all match, and marks a name being fetched so that what the
    fetch brings back is not trusted. Here the fetch is repeated only if
    it brought back the version a notice named, so the copies from the
    other data nodes cost nothing.
  - When the last data node link goes, the reference lets go of
    everything cached except fetches under way.
  - An index's internal name is `sys/def/<table id>/<index>`, or
    `<database>/<schema>/<table id>/<index>` for one made by an older
    version, which is tried when the first is not found. The index
    object remembers the table id and version it was made for, and one
    found for another version is fetched again. Here the index holds
    that table object itself, so the version lives as long as the index
    does, and the cache lets go of a version's indexes with it.
- `LIST_OBJECTS_REQ` (signal 193) / `LIST_OBJECTS_CONF` (194) for listing.

  As built (`ic_signals::list_objects`, `dict_client::list_dependents`):
  the request is five words: the caller's data, the caller's reference,
  a flags word, a table id and a table type. The flags word asks for
  names (bit 28), indexes only (29) or the dependents of one table (30),
  and repeats the table id in its low 12 bits for data nodes that only
  read the older form. The answer is the caller's data and a count, with
  three words per object in section 0 (flags with store, temporary and
  state, then id, then type) and, when asked for, the names in section
  1, each a byte length counting its NUL and then the name. Every
  current data node answers in this long form (the reference's client
  always expects it). The dictionary splits a long answer by hand; see
  §2.1.

  A table's indexes are its dependents of type unique hash index (3) or
  ordered index (6). Each is fetched as a table of its own; its last
  column is a hidden reference to the table's row (`NDB$PK` or
  `NDB$TNODE`) and is not one of the index's columns. Index names are
  `sys/def/<table id>/<name>`, or `<db>/<schema>/<table id>/<name>` for
  indexes made before that form; the name is the part after the last
  separator. The primary key is not an index of its own: the reference's
  table description tool prints it as a unique hash index made from the
  table's key columns.
- DDL, by signal number: begin a schema transaction (731, confirmed by 732)
  → create a table (587, with a table properties section) / create a
  hash map (297) / create an index (510) / drop a table (82) / alter a
  table (624) → end the schema transaction (734, confirmed by 735).
  iClaustron implements this path end to end
  (`execute_meta_data_transaction`) and it is carried over.

### 8.1 Column types and storage encodings

Types (our `IC_TYPE_*`): Tinyint, Tinyunsigned, Smallint, Smallunsigned,
Mediumint, Mediumunsigned, Int, Unsigned, Bigint, Bigunsigned, Float,
Double, Olddecimal, Olddecimalunsigned, Decimal, Decimalunsigned, Char,
Varchar, Binary, Varbinary, Datetime, Date, Blob, Text, Bit,
Longvarchar, Longvarbinary, Time, Year, Timestamp, Time2, Datetime2,
Timestamp2. Encodings: Char/Binary fixed; Varchar/Varbinary 1 length
byte + data; Longvarchar/Longvarbinary 2 little-endian length bytes +
data; Decimal packed binary of MySQL's binary decimal size for its
precision and scale; Bit as an array of as many bits as the column's
array size; Date 3 bytes, Time 3, Datetime 8, Timestamp 4, Year 1; Time2
3 + frac, Datetime2 5 + frac, Timestamp2 4 + frac where frac = `(scale + 1)
/ 2` bytes and scale is carried in precision. `IC_FIELD_TYPE` in
`ic_apid_datatypes.h` must be extended to cover all of these.

BLOB/TEXT: the main table holds a head plus inline data (V2 head 16
bytes: u16 varsize, u16 reserved, u32 pkid, u64 length, little-endian);
the rest is in a parts table `NDB$BLOB_<tableid>_<colno>` with columns =
main PK, `NDB$PART`, `NDB$PKID`, `NDB$DATA`, sharing the main table's
partitioning. BLOB support is not scheduled; records must leave BLOB
columns out.

### 8.2 Describing a collation, and its weights (signals 984–986, 2026-10-08)

New in RonDB 26.04 main, in the author's separate task (Q15). Read from
his working tree, as the change is not committed yet; to be checked on
the wire with the dissector and the reference's test program for it.
Paths below are into that tree.

- **Who and where.** Any node, in practice an API node, sends
  `COLLATION_DESC_REQ` (984) to the dictionary block (0xFA) of a data
  node, which answers `COLLATION_DESC_CONF` (985) or refuses with
  `COLLATION_DESC_REF` (986). Only a data node of version 26.10.0 or
  later may be asked; an older one takes the unknown signal number as a
  fatal error.
- **The request, seven words**, all of them required (a shorter request
  is refused as malformed): the block reference to reply to, which must
  be on the sending node; the caller's data, given back; the kind of
  lookup, 0 by collation number, 1 by collation name, 2 by character set
  name meaning that set's default collation; the collation number (kind
  0, from 1 to 2047; the number a column's description keeps in the
  upper half of its precision word); the name's length in bytes (kinds 1
  and 2, from 1 to 64, no NUL); flags, bit 0 asking for the weights too;
  and with weights the position to start from, 0 at first. Kinds 1 and 2
  carry the name in section 0, padded to whole words, at most 17 words;
  kind 0 carries no section. Names are matched without regard to case.
  The request is never sent in fragments, and one that arrives
  fragmented is refused.
- **The confirmation, nineteen words**, in order: the caller's data; the
  collation number found; the number of its character set's default
  collation; the number of its character set's binary collation, 0 if it
  has none; flags (0x1 the set's default, 0x2 binary order, 0x4 a
  Unicode set in a standard encoding, 0x8 case-sensitive order, 0x10 a
  pure ASCII set, 0x20 "no pad": trailing spaces count when comparing);
  the fewest and the most bytes a character takes; the most sort-key
  bytes one string byte becomes; a word with the most bytes upper-casing
  makes of one byte in bits 0-7 and lower-casing in bits 8-15; the byte
  a CHAR value is padded with; how many weight levels are compared; the
  lowest- and the highest-sorting code point; a word with the byte
  length of the character set's name in bits 0-15 and of the collation's
  name in bits 16-31; then, for the weights, the number of weight runs,
  their bytes, the number of contractions, their bytes, and the position
  the next request continues from, or 0xFFFFFFFF when all have been sent
  (and always without weights). Section 0 holds the character set's
  name, a NUL, the collation's name and a NUL, padded to whole words
  with NULs.
- **The weights, in section 1** of a confirmation asked with weights,
  only when it holds an entry: the weight runs, padded with zeros to a
  whole word, then the contractions, padded likewise. Every number in it
  is big-endian.
  - A weight run covers consecutive code points: three bytes the first
    code point, three bytes how many (at least 1), one byte the step
    offset, one byte the length of the character's encoding, two bytes
    the length of its weight string, then the first code point's bytes
    in the character set and its weight string. Code point first + k has
    the first one's weight string with the 16-bit number at the step
    offset increased by k, or the same weight string when the offset is
    0xFF; its bytes in the set are the first one's, read as a big-endian
    number, increased by k. A Unicode set's encoding is the standard one
    (utf8, utf16, utf16le, utf32 or ucs2), and its runs carry no bytes.
  - The characters of a set are the code points it encodes and decodes
    back to the same code point; each is in exactly one run, the runs in
    increasing order.
  - A character's weight string is the sort key of a string of that one
    character, without padding. How weight strings combine into a longer
    string's depends on the collation: a Unicode collation of the 9.0.0
    algorithm lists the weights of each level in turn, separated by
    0x0000, and contractions stand in for their characters; a few
    collations (czech, win1250ch, tis620) also weigh a character by its
    neighbours, which per-character weights cannot reproduce.
  - A contraction is a sequence of code points weighed as one, such as
    "ch" in Czech: one byte the number of code points (2 to 6), one byte
    flags (bit 0: the second code point is weighed by the first, which
    keeps its own weights, as for the Japanese prolonged sound mark), two
    bytes the weight string's length, three bytes per code point in
    string order, then the weight string of the whole sequence. One
    holding a code point the set does not encode has no entry but keeps
    its position.
- **Asking in several round trips.** One confirmation with weights is
  one signal, sent in fragments, of at most 1 MB of sections (4 KB from
  a debug data node, so that a collation takes many). The first request
  asks from position 0; each next one from the position the last
  confirmation gave, until that is 0xFFFFFFFF. Positions below 0x110000
  are code points; 0x110000 + n is the nth contraction. The data node
  builds an answer in slices before sending it, serves one request with
  weights at a time, and gives up building one when the asking node's
  link goes.
- **The refusal, four words:** the caller's data, the error, and the
  request's kind and collation number as sent (0 where the request was
  too short to carry them). Errors: 1 malformed, or sections that do not
  fit the kind; 2 the reply address on another node than the sender's; 3
  an unknown kind or unknown flags; 4 a collation number of 0 or 2048
  and above; 5 a bad name (empty, too long, longer than its section, or
  holding a NUL); 6 no such collation; 7 no such character set; 8
  weights asked for from 0xFFFFFFFF; 9 busy with another request with
  weights, to be sent again; 10 out of memory to build the answer in. A
  request too short to name its reply address, or naming one on another
  node, is refused to the block that sent it.
- Looking a collation up makes the data node initialise it, as creating
  a table that uses it does.
- **Seen live, 2026-10-09**, through `ic_desc --collation` against the
  development cluster's debug data nodes, and the integration group
  `collation`: latin1_swedish_ci is number 8, its set's binary collation
  47, one byte a character, pad byte 0x20, one level; its weights came
  in one round trip as 226 runs covering all 256 characters; A and a
  weigh 0x41, é 0x45 (as E), Ä and ä 0x5C (after Z, as Swedish has it).
  utf8mb4's default is utf8mb4_0900_ai_ci, number 255, binary
  collation 46, no pad, one to four bytes a character, one level,
  sorting from U+0009 to U+10FFFF, and gives **0** as its sort-key
  growth, so that word cannot bound a sort key's length for such a
  collation. Its weights took 30 round trips of about 4.5 KB, the debug
  node's limit, as 9 112 runs covering 1 112 064 code points, every
  Unicode scalar value, and no contractions; A, a, Ä and ä all weigh
  0x1C47, B and b 0x1C60, é 0x1CAA, the space 0x0209.

## 9. Interpreted programs

A program runs in the data node that holds the row: as a scan's
filter, or as a key operation's condition and the updates it computes
from the row. What follows is the author's account (2026-10-19), in our
words and with names of our own, checked against the six programs
MySQL sent in `scans2.pcap` (§9.6) and against the row store's code
where the two leave a gap. The constants are `IC_INSN_<NAME>`.

### 9.1 The machine

- Eight registers, and a memory of a little more than 64 KB that the
  memory instructions work on (later).
- A register is four words: a type word; a word that only the column
  read uses; and the 64-bit value in the last two, read as the type word
  says. The type word has a byte per property: byte 0 is 1 for a value
  and 0 for NULL, byte 1 is 1 for unsigned and 0 for signed, byte 2 is 1
  for a double and 0 for an integer, byte 3 is 0. So 0 is NULL, 1 a
  signed integer, 0x101 an unsigned one, 0x10001 a double.
- An instruction is a word, some with words after it. Its number is bits
  0–5, plus 64 when bit 15 is set, so 0 to 127. A first register is in
  bits 6–8, a second in 9–11, a third in 12–14, a fourth in 16–18. Bits
  16–31, "the field" below, otherwise hold what the instruction needs: a
  jump's distance, a column id, a 16-bit constant, a memory offset.

### 9.2 The instructions

R1 to R4 are the registers of bits 6–8, 9–11, 12–14 and 16–18. The
basic interpreter of phase 9.1 needs these:

| # | Our name | What it does |
|---|---|---|
| 1 | `READ_COLUMN` | R1 = the row's value of the column in the field |
| 2 | `WRITE_COLUMN` | the column in the field = R1 |
| 3 | `SET_NULL` | R1 = NULL |
| 4 | `SET_16` | R1 = the field |
| 5 | `SET_32` | R1 = the next word |
| 6 | `SET_64` | R1 = the next two words |
| 7 | `ADD` | R4 = R1 + R2 |
| 8 | `SUBTRACT` | R4 = R1 − R2 |
| 9 | `JUMP` | jump |
| 10 | `JUMP_IF_NULL` | jump if R1 is NULL |
| 11 | `JUMP_IF_VALUE` | jump if R1 is not NULL |
| 12–17 | `JUMP_IF_EQ`, `_NE`, `_LT`, `_LE`, `_GT`, `_GE` | jump if R1 =, ≠, <, ≤, >, ≥ R2 |
| 18 | `ACCEPT` | end; the row passes |
| 19 | `REFUSE` | end; the row fails, with the error code in the field |
| 20 | `CALL` | call a subroutine |
| 21 | `RETURN` | return from it |
| 22 | `ACCEPT_LAST` | end; the row passes and is the fragment's last |
| 23 | `TEST_COLUMN_VALUE` | jump on a column compared with a value in the program (§9.3) |
| 24 | `JUMP_IF_COLUMN_NULL` | jump if the column in word 2's field is NULL |
| 25 | `JUMP_IF_COLUMN_VALUE` | jump if it is not NULL |
| 26 | `TEST_COLUMN_PARAM` | as 23, against a parameter |
| 27 | `TEST_COLUMN_COLUMN` | as 23, against another column of the row |
| 71 | `ADD_IMM` | R3 = R1 + the field |
| 72 | `SUBTRACT_IMM` | R3 = R1 − the field |
| 121 | `READ_INPUT` | R1 = the input in the field, one of 16 opaque 8-byte values (§9.5) |

Described, for later:

| # | Our name | What it does |
|---|---|---|
| 28, 29 | `SHIFT_LEFT`, `SHIFT_RIGHT` | R3 = R1 shifted by R2 bits |
| 30–35 | `MULTIPLY`, `DIVIDE`, `BIT_AND`, `BIT_OR`, `BIT_XOR`, `REMAINDER` | R3 = R1 with R2 |
| 36 | `BIT_NOT` | R3 = R1 inverted |
| 45 | `SET_DOUBLE` | R1 = the double in the next two words |
| 48 | `READ_COLUMN_TO_MEMORY` | a column into memory |
| 49–52 | `LOAD_8`, `LOAD_16`, `LOAD_32`, `LOAD_64` | R1 = memory at the field: unsigned 8, 16 or 32 bits, or signed 64 |
| 53–56 | `STORE_8`, `STORE_16`, `STORE_32`, `STORE_64` | memory at the field = R1, in those sizes |
| 57 | `WRITE_COLUMN_FROM_MEMORY` | a column = R2 bytes of memory at R1 |
| 58 | `APPEND_COLUMN_FROM_MEMORY` | the same appended to a column |
| 59 | `COPY_TO_MEMORY` | bytes from the program to memory at R1 |
| 62 | `STORE_REGISTER` | 8 bytes of memory at the field = R1 |
| 76–81 | `JUMP_IF_EQ_IMM`, `_NE_IMM`, `_LT_IMM`, `_LE_IMM`, `_GT_IMM`, `_GE_IMM` | jump if R1, unsigned, compares so with bits 9–14 (0 to 63) |
| 92–99 | `SHIFT_LEFT_IMM`, `SHIFT_RIGHT_IMM`, `MULTIPLY_IMM`, `DIVIDE_IMM`, `BIT_AND_IMM`, `BIT_OR_IMM`, `BIT_XOR_IMM`, `REMAINDER_IMM` | R3 = R1 with the field (a shift below 64) |
| 113–116 | `LOAD_8_AT` … `LOAD_64_AT` | R2 = memory at R1 |
| 117–120 | `STORE_8_AT` … `STORE_64_AT` | memory at R2 = R1 |
| 123 | `WRITE_OUTPUT` | the output in the field, one of 16, = R1 |
| 125 | `ZERO_MEMORY` | memory zeroed, R1 and R2 the size and the offset |

Not described, for now or for pushed joins: 37, 38–44, 46, 47, 60,
61, 63, 65–70, 73–75, 82, 83, 101, 122 and 124. Unused: 0, 84–86,
88, 89, 100, 102–112, 126, 127. Numbers 87, 90 and 91 are 23, 26 and
27 with bit 15 set, which a comparison of 8 or more sets (§9.3).

As read in the code where the account leaves a choice: `SUBTRACT_IMM` is
R1 minus the field, not the field minus R1, and both immediates are
unsigned, 0 to 65 535; `ADD` and `SUBTRACT` with a NULL operand give
NULL; the jumps of 12–17 compare by the type words, signed, unsigned or
double, and a NULL in either register is error 878, so a program tests
for NULL first. `READ_COLUMN` gives the register the column's type: a
signed integer column a signed value, the smaller ones sign-extended, an
unsigned column an unsigned one, FLOAT and DOUBLE a double; the jumps of
12–17 and the arithmetic go by the types, so a signed and an unsigned
value compare as numbers. That came with bab40b92982 (2026-05-02).
Before it a column of 32 bits or fewer was read as its word, filled out
with zeros and taken as signed, so a negative value of such a column
compared through a register as a large positive one; the reference
tree's history shows it (`git show bab40b92982^`). Our programs read
integer columns into registers only where they meet an input, an integer
or a sum (`ic_apid::program`), and rely on the typed reads: every data
node is a released, stable 26.10 or later, none older (the author,
2026-10-20; chapter 01), and the 26.10 branch has the commit (seen in
its tree at d3ba310b593, 2026-10-20). For later: the code reads
`ZERO_MEMORY`'s offset from R1 and its size from R2, the other way round
from the account.

### 9.3 Column tests (23, 26, 27)

Two words, and for 23 the value after them:

- **Word 1**: the instruction; the NULL rule in bits 6–7; the
  comparison in bits 12–15; the jump's distance in the field.
- **Word 2**: the column's id in bits 16–31; in bits 0–15 the value's
  length in bytes (23), the parameter's number (26), or the other
  column's id (27).
- **The value** (23 only), padded to whole words with zero bytes, in the
  column's own format. A fixed-size type sends the column's whole size;
  a variable one its length bytes and its bytes, the length counting
  both (`'r3'` is 3 bytes, 02 72 33); a LIKE pattern is sent bare, as
  long as it is. A bit column's unused bits are zero. As the reference's
  builder writes it; the wire has shown the variable one.

The comparisons: 0 equal, 1 not equal, 2 less, 3 less or equal, 4
greater, 5 greater or equal, 6 LIKE, 7 NOT LIKE, 8–11 bit masks (later).
**The value comes first**: 2 jumps when the value is less than the
column, that is, when the column is greater. For 27 the other column, in
bits 0–15, stands where the value does. Strings compare by the column's
collation, for 27 by the first column's. For 27 the data node reads the
other column as though it had the first one's type and checks nothing,
so only columns of one type, length, precision, scale and collation may
be compared, and no blobs; the reference's builder refuses others with 4557.
Ours refuses them too.

The NULL rules, when the column or the value is NULL (a value of
length 0 is): 0, compare anyway, NULL equal to NULL and less than any
value; 2, jump; 3, go on to the next instruction. MySQL's filters use
2 with jumps to a `REFUSE`, so that a row whose column is NULL fails,
as SQL has it.

Seen: comparisons 1, 2, 4 and 5 with rules 0 and 2 (§9.6).

### 9.4 Jumps and ends

- **A jump's distance** is in words, counted from the jump's first word.
  The account gives it bits 16–31; the code reads bits 16–30 as the
  distance and bit 31 as a jump backwards. Every jump seen goes forward.
- **`ACCEPT`**: a scan returns the row; a key operation goes on to its
  final updates and reads (§9.5).
- **`REFUSE`** ends with the code in the field. In a scan, 626 and 899
  skip the row and the scan goes on; any other code ends the scan with
  it. A key operation fails with the code. MySQL refuses with 626, the
  reference's default, which advises 626 or a code from 6000 to 6999.
  That range is kept for the programs' own codes; the data nodes define
  none in it.
- **Ours** (the author, 2026-10-19): the refusal's code says whether a
  missed filter is to abort: 6000 where it is, 626 where it is not. A
  scan's filter refuses with 626, so the row is skipped; 6000 would end
  the scan. A key operation's abort option keeps its meaning and its
  defaults (§6.7: writes abort on error, reads ignore errors), and its
  filter follows it: with abort on error it refuses with 6000, so that
  the transaction's abort says that the condition failed, not that the
  row was missing; with errors ignored it refuses with 626, and the
  operation fails alone as a row not found. On a key operation the
  code does not decide the abort, the abort option does (§9.8,
  questions 1–3). Our error table knows 6000 as an error the program
  raised, permanent: "The row did not meet the operation's condition".
  The rest of the range, 6001–6999, is left to applications' own
  exits.
- **`ACCEPT_LAST`**: as `ACCEPT`, and the fragment's scan ends after
  the row.

### 9.5 Where a program travels

- **A key operation** sets flag bit 15 (§6.4), and its column section
  begins with five sizes: the reads before the program, the program,
  the final updates, the final reads, the subroutines. The final updates
  are column headers and values written after the program; the final
  reads name columns read after it, which is how a counter's new value
  comes back (§16.5; seen, §9.6).
- **A scan** has the same five sizes; the reference sends [read,
  program, 0, 0, subroutines] (§7.2).
- Subroutines, for `CALL`, and the parameters of 26 are in the fifth
  part; their layout is not needed by the basic interpreter.
- **Inputs** (the author, 2026-10-20): a program can be used again and
  again with only its inputs set anew for each operation. There are 16,
  each an opaque 8-byte value, and they go at the front of the reads
  before the program: a word with 0xFFFF in bits 16–31 and in bits 0–15
  the inputs' length in words, this word with them (1 and 3 for each
  input, so 4 to 49); then for each input a column header naming 0xFFA0
  plus its number, of 8 bytes, and its value, low word first. The reads,
  if any, follow, and the part's size counts both. `READ_INPUT` (121)
  puts input n, the field, in R1 as a signed integer, never NULL. An
  input the operation does not set is not cleared: it holds what an
  earlier operation in the same thread of the data node left there, so
  ours sends every input its program reads. The data node refuses inputs
  whose sizes do not add up or one not of 8 bytes (937), more than 16
  (938), a header naming no input (939), and `READ_INPUT` of 16 or more
  (846). As read in the code; not yet seen on the wire.

### 9.6 Seen on the wire (`scans2.pcap`, read again 2026-10-19)

The programs of MySQL's scans of `scan_t` (columns 0 `id INT`, 1 `val
INT`, 2 `name VARCHAR(32)`), by word:

`WHERE val > 190000` (frame 62075), "refuse if 190000 ≥ val, or val is
NULL":

```
0  0x00045097  TEST_COLUMN_VALUE, NULL rule 2, ≥, jump +4 to 4
1  0x00010004  column 1, 4 bytes
2  0x0002e630  190000
3  0x00000012  ACCEPT
4  0x02720013  REFUSE 626
```

`WHERE val BETWEEN 100 AND 30000` (62918, sent along with the index
range):

```
0  0x00074097  TEST_COLUMN_VALUE, rule 2, >, +7 to 7    100 > val
1  0x00010004  column 1, 4 bytes
2  0x00000064  100
3  0x00042097  TEST_COLUMN_VALUE, rule 2, <, +4 to 7    30000 < val
4  0x00010004  column 1, 4 bytes
5  0x00007530  30000
6  0x00000012  ACCEPT
7  0x02720013  REFUSE 626
```

Two such ranges joined by OR (63781, 16 words): the first range's tests
jump to the second range at word 7 when they fail, and a `JUMP` +9
(0x00090009) at word 6 goes to the `ACCEPT` at word 15 when they pass;
the second range's tests jump to the `REFUSE` at word 14, and its
`JUMP` +2 (0x00020009) at word 13 to the `ACCEPT`. So a condition is
compiled with its terms negated: AND falls through, OR jumps forward.

`WHERE name = 'r3'` (65578): `0x00041097` (test, rule 2, ≠, +4),
`0x00020003` (column 2, 3 bytes), `0x00337202` (02 72 33 and a pad
byte), `ACCEPT`, `REFUSE` 626.

MySQL's scan of a table of its own (table 6, frame 13131) used NULL
rule 0 and ≠ with jumps to the `REFUSE`: column 0 = 494 and column 1 =
1. Its sixteen statistics reads of `scan_t` (frame 14199 on) send a
one-word program, `ACCEPT_LAST` (0x00000016), after reading the
pseudo-columns 0xFFFD, 0xFFFA, 0xFFF9, 0xFFF4, 0xFFEC, 0xFFEB and the
partition.

Read again on 2026-10-19, with `tools/interp_capture.sh`'s capture
(`~/captures/interp.pcap`, steps in `interp-steps.txt`). It began
after the cluster, so it holds no handshakes, and was read with the
data nodes' ports decoded as NDB (`-d tcp.port==11861,ndb -d
tcp.port==11862,ndb`): without a handshake, bytes that are not text
are taken as messages. Its table `interp_t` has columns 0 `id INT`, 1
`val INT`, 2 `name VARCHAR(32)` (NULL on every seventh row), 3 `code
CHAR(4)` and 4 `big BIGINT UNSIGNED`. MySQL's scans:

| Condition | Program |
|---|---|
| `name IS NULL` | `JUMP_IF_COLUMN_VALUE` column 2 to the `REFUSE`; `ACCEPT` |
| `name IS NOT NULL AND val < 500` | `JUMP_IF_COLUMN_NULL` column 2 to the `REFUSE`; test 500 ≤ val (3), rule 2, to the `REFUSE`; `ACCEPT` |
| `val > id` | `TEST_COLUMN_COLUMN` ≥ (5), rule 2, column 1 in bits 16–31 and column 0 in bits 0–15: refuse if id ≥ val |
| `code = 'c3'` | test ≠, the value `'c3  '`: 4 bytes, the `CHAR(4)`'s whole size, padded with spaces by the client |
| `big >= 18446744073709551500` | test > on 8 bytes: refuse if the value > big |
| `NOT (val BETWEEN 100 AND 200)` | tests 100 > val and 200 < val, rule 3, jumping to the `ACCEPT`; then `JUMP` to the `REFUSE` |
| `val = 10 OR name = 'r3'` | tests =, rule 3, jumping to the `ACCEPT`; then the `REFUSE` |
| `name LIKE 'r1%'` | test NOT LIKE (7), rule 2, the pattern bare (3 bytes, no length byte): refuse if it does not match |

So the other column of a column pair stands first, as the value does,
and MySQL compiles SQL's NULL logic with the two rules: a term of an
AND is negated and jumps to the refusal with rule 2, a term of an OR
jumps to the acceptance with rule 3, and either way a NULL makes the
term fail. The scans of MySQL's statistics read pseudo-columns 0xFFFD
or 0xFFFC with the program `ACCEPT_LAST`.

**MySQL sends no program with a key operation.** A read by primary key
with a condition is a committed read and the server tests the row; an
update or delete with one is an exclusive read and then the update or
delete, two round trips; so is `val = val + 1`. A refused key
operation (§9.4) is therefore first seen from our own client.

**The AUTO_INCREMENT counter** (§16.5, seen live). Each operation is a
`KEY_OP_REQ` of its own transaction on the counter table (id 2), keyed
by the table id, an update with flags 0x8E32 (no disk, commit, execute,
start, abort on error, interpreted):

- Taking values, sizes [0, 7, 0, 1, 0]: `READ_COLUMN` r6 = column 1,
  `SET_64` r7 = 512 (the constant's low word first), `ADD` r7 = r6 +
  r7, `WRITE_COLUMN` column 1 = r7, `ACCEPT`; the final read names
  column 1. The `ROW_DATA_REP` brought column 1, 8 bytes: 513, so the
  first insert reserved 1 to 512; `KEY_OP_CONF` gave the row's length
  as 3 words. The next three inserts sent nothing.
- An explicit value of 100000, sizes [0, 9, 0, 1, 0]: `SET_64` r1 =
  100001, `READ_COLUMN` r2 = column 1, `JUMP_IF_LE` to 7 if r1 ≤ r2,
  `WRITE_COLUMN` column 1 = r1, then three `ACCEPT`s (5, 7 and 8); read
  back 100001. The register jump compares bits 6–8 with bits 9–11, as
  §9.2 has it. The next insert took 512 more and read back 100513.
- `ALTER TABLE … AUTO_INCREMENT = 200000` copied the table, to id 24,
  moved its counter up with the constant 3 (not looked into), and the
  next insert read back 200512.

Not seen yet: a refused key operation, final updates, `CALL` and
subroutines, `SET_16`, `SET_32`, `SUBTRACT`, the immediates.

### 9.7 Our builders

The `IC_WHERE_CONDITION` and `IC_CONDITIONAL_ASSIGNMENT` builders of
`ic_apid.h` map onto these. A column compared with a constant is
`TEST_COLUMN_VALUE`, with another column `TEST_COLUMN_COLUMN`; IS NULL
and IS NOT NULL are 24 and 25; AND, OR and NOT are jumps, as MySQL's
two ranges show. NULLs follow SQL's logic, compiled as MySQL compiles
it (§9.6): a term of an AND jumps to the refusal with rule 2, a term of
an OR to the acceptance with rule 3, since phase 9.1's exit compares
our answers with MySQL's. An assignment reads the column into a
register, sets or adds, and writes it back. More than eight live
values is
`IC_ERROR_CONDITION_TOO_COMPLEX` in 0.3, spilling to memory comes
later, and LIKE (comparison 6) too. Regular expressions have
no instruction and are evaluated in the API after the row comes, a
documented limitation.

As built (2026-10-20): the encoding is `ic_signals::interp`, a writer
of programs with labels for the jumps, tested against the programs of
§9.6 word for word, both captures' scans and the counter's updates.

### 9.8 Asked of the author

1. **A key operation's failed condition (2026-10-19).** A conditional
   update or delete whose row fails its condition ends in `REFUSE`,
   and the operation fails with the code; the reference and MySQL use
   626, so a failed condition reads as "no such row". Shall ours use
   626 too, or a code of 6000–6999 so that an application can tell a
   failed condition from a missing row? Answered 2026-10-19: a code in
   the 6000 range for key operations; we took 6000 (§9.4).
2. **What a failed condition does to the transaction (2026-10-19).** A
   key operation that fails takes the transaction with it when its
   abort option is "abort on error", which our writes default to
   (§6.7; the coordinator acts on bits 12–13 of the flags); our reads
   default to ignoring errors. So by our present defaults a
   conditional update or delete whose row fails its condition rolls
   the transaction back, while MySQL's `UPDATE … WHERE id = 5 AND val
   > 100` matches no row and the transaction goes on. Shall a
   conditional write default to ignoring its error, so that 6000 is
   reported on the operation and the transaction continues, or keep the
   writes' default? Answered 2026-10-19: as the reference's abort
   option, a flag: set, the transaction aborts if anything fails to read
   or write; not set, it does not abort when 626 or 6000 happens. Ours
   is the abort option, `IC_ABORT_ON_ERROR` set; question 3 settles the
   default. On the wire the coordinator knows only the two: an operation
   that aborts on error takes the transaction with it, one that ignores
   errors fails alone whatever its code, unless the error came from a
   backup replica or from an operation a trigger made, which abort in
   any case. For an update or a delete with a condition the errors left
   are 626 and 6000.
3. **Whether "not set" is every write's default (2026-10-19).** Our
   writes abort on error unless asked to ignore errors; should "not
   set" be every write's default, so that an insert's 630 would fail
   alone too, or only a conditional write's? Answered 2026-10-19:
   neither; the abort option keeps today's meaning and defaults for
   every operation, conditional or not, and the filter's code follows
   it instead: 6000 where a missed filter is to abort, 626 where it is
   not (§9.4).

## 10. Errors

Error records have: code, MySQL code, classification (18 values: no
error, the application's error, no such row, a constraint broken, a
schema error, an error the user's program raised, out of space, a
temporary lack of resources, node recovery, overload, a timeout, an
unknown result, an internal error, not implemented, an unknown code, a
node shutting down, an object that already exists, and a temporary
internal condition) and a derived status (success, temporary error,
permanent error, unknown result). Our table keeps code, MySQL code and
classification (interface data) and carries our own message text.
`IC_ERROR_SEVERITY_LEVEL` maps from status and `IC_ERROR_CATEGORY` from
classification.

**Never report more than is known.** The reference's client answers 4009, "cluster
failure", in states that are not one, such as a single broken link to a
node that is still up, and an application cannot act sensibly on that.
Our errors name what we actually know: `IC_ERROR_LINK_LOST` when our
connection to one node broke, `IC_ERROR_NODE_DOWN` when a data node has
reported a node failed, and a cluster-level error only when no data node
is connected at all. The first two are temporary errors that invite a
retry on another node.

## 11. Events (0.3)

Signals by number, none named by us yet: create an event (700), then
start a subscription (579) `{the caller's reference, the caller's data,
the subscription's id, the subscription's key, the part, the
subscriber's data, the subscriber's reference, a flags word}` →
confirmed (581) with the first global checkpoint and the bucket count;
row changes arrive as signal 586 with sections column information, after
values, before values; epoch boundaries as signal 593, acknowledged with
signal 699. Fragmented signals are used here.

## 12. Autoincrement (0.3)

System table `sys/def/SYSTAB_0` keyed by table id, column `NEXTID`;
ranges are allocated with an interpreted update that adds to the column
inside a transaction hinted with the table id as key.

## 13. Hard limits to encode as constants

At most 32 key columns, 4096 columns in a table, a row of 18000 words, a
key of 1023 words, 100 active events, a scan filter of 15359 words, and
signal numbers up to 982.

A row's parts have limits of their own: the fixed-size part in memory at
most 4 070 words, the variable-size part 8 008, the disk part 7 770
(enforced when a table is made). Seen live (2026-10-12, by making tables
through a MySQL server and growing a column a byte at a time): a table
one byte over is refused with 944 "Fixed size part of row can at most be
16280 bytes" or 943 "On-disk part of row can at most be 31080 bytes".
One VARBINARY of 32 014 bytes fills the variable part; one of 31 054 the
disk part; a VARBINARY costs the fixed part a word. A MySQL server
allows a row of at most 65 535 bytes, BLOB and TEXT apart (which the NDB
handler does not raise), so the largest row it makes is about 65 KB: in
memory alone about 48 KB. Only the inline parts of blob columns can take
a row made through MySQL past that; we neither read nor write blob
columns yet.

**The limits from 2026-10** (the author, 2026-10-12, with the mend
below): the fixed-size part 16 280 bytes, as before; the variable-size
part 8 158 words, for the in-memory columns and for the disk columns
alike; the whole row 72 000 bytes. The overheads measured above hold:
16 bytes beside one in-memory VARBINARY and 24 beside the disk columns
of `big_disk`. Seen live (2026-10-13, the `big_rows` group): a
VARBINARY of 32 614 bytes fills the in-memory variable part, one of
32 090 beside an INT and two BINARY(255) the disk part, both in one row
made through MySQL (65 486 bytes); a byte more in any part is refused
by the data nodes; rows with every part full are written, grown,
shrunk and read by primary and unique key.

**A full variable part stopped the data nodes: a RonDB bug.** Seen live
(2026-10-12, release build): an insert whose in-memory VARBINARY(32 014)
was full, a variable part of about 8 005 words, stopped the data node
that ran it with error 2301, an assertion in the row store's allocation
of variable parts, and the other node the same way on the next such
insert: the whole cluster. The request itself had arrived whole: the
coordinator had joined our train and handed the operation on (§2.2). The
allocation of the row's variable part asked for more than 7 784 words,
the least free space of the fullest list of pages, and finding no list
for it the node stopped. The author (2026-10-12): the variable part of
in-memory columns may grow beyond that, up to what a table may declare.
So the stop is a bug in RonDB, not a limit, mended with the limits
above; the `big_rows` tests fill every part, and stop a data node that
is not mended.

**A full row in a checkpoint stops a restarting node: a RonDB bug.**
Seen live (2026-10-14, release build, a full integration run): the
`big_rows` group left rows of `big_mem` with both parts full, about 12
230 words, in the table; the local checkpoint taken during the `failure`
group's first restart of a data node wrote them; its second restart of
that node stopped the node in start phase 5 with error 2341, a failed
requirement of the restore. Restoring a checkpoint requires each record
in the file to be shorter than a page of 8 192 words, and joins a record
that crosses into the next page by copying it to the start of the first;
only a table definition may be longer, and is copied to a buffer of its
own. So a row over 32 KB, which the limits above allow, is checkpointed
but cannot be restored. A system restart restores checkpoints the same
way, so it would stop too (not seen). A bug in RonDB, not a limit,
mended by the author (2026-10-15): restore now reads records longer than
a page. In the full run since, the `failure` group restarted the node
twice after `big_rows`, and both restarts completed.

## 14. Node ids, and why nothing is sized by them

The highest node id a cluster allows has risen with every era of the
product: 255 in NDB 7.2, which is what the C code assumed, 2039 in RonDB
26.10, and 8191 in a coming release. Data nodes stay in the low numbers
(at most 144), but API nodes are routinely given large ids: a stock test
cluster hands out 1271 and 1600.

Two consequences for this library.

- **Nothing is sized by the limit.** Anything indexed by node id is a
  map, or a vector sized from the configuration actually received. A
  cluster with larger ids then needs no code change and no rebuild.
  `IC_MAX_NODE_ID` exists only as a sanity bound where no configuration
  is available yet, such as reading a connectstring, and even there the
  check is against `IC_MAX_NODE_ID_WIRE`, the 16 bits a block reference
  gives the node id, which cannot change without changing the protocol.
- **Node bitmaps in signals grow with it.** The connected-node bitmap in
  `HEARTBEAT_CONF` and the failed-node bitmap in `NODE_LOST_REP` are
  sized by the cluster's node id limit, so they long ago outgrew the 25
  data words a signal carries and travel in a section. Any decoder for
  them must take the length from the signal rather than assume one. The
  reference's client handles three different encodings of the same
  bitmap.


## 15. Delta from NDB 7.2.9 (iClaustron's baseline) to RonDB 26.10

Everything the C code "knows" about the protocol dates from 7.2.9 and must
be treated as a hypothesis. Known differences the port must apply:

| Area | 7.2.9 as coded in iClaustron | RonDB 26.10 |
|---|---|---|
| Socket auth | `ndbd` / `ndbd passwd` → `ok` | TLS-capable line `ndbd TLS disabled` + empty line → `ok`/`Cleartext ok` (legacy possibly still accepted, Q2) |
| Node id hello | `"<nodeid> 1"` (2 integers) | `"<nodeid> 1 <remote nodeid> 0"` (4 integers), max 23 chars; reply `"<nodeid> 1"` |
| Byte order | swap on receive | no swap; reject mismatch |
| Signals | private signal numbers for a patched kernel | the stock signal numbers (§3–8) |
| Key operations | `NDB_PRIM_KEYREQ` prose, short signal with inline KEY_DATA_ORD/COLUMN_DATA_ORD trains | long-form `KEY_OP_REQ` with sections 0/1 only; new flags-word bits 20–24 and RonDB flags |
| Config | `get config` v1, iClaustron cluster ids | `get config_v2` with `node:` argument; v2 blob with default sections; RDMA section type 7 |
| Config parameters | full 7.2 data-node table | API-node subset re-verified against 26.10 ids; new parameters (TLS, RDMA, location domains, active flag) |
| Heartbeat | `HEARTBEAT_REQ` 3 words | same 3 words; `HEARTBEAT_CONF` gained the lowest data node and API versions and RonDB restart-barrier liveness rule |
| Node failure | `NODE_LOST_REP` fixed bitmap | three formats incl. a long section; `NODE_LOST_DONE_REP` gating of reconnect |
| Block numbers | `IC_NDB_MIN_MODULE_ID_FOR_THREADS 32768`, `IC_NDB_PACKED_MODULE_ID 2047` | unchanged; plus fixed `NODE_WATCH 4002`; kernel blocks extended to `0x111` (query blocks) |
| Partitioning | hash maps (introduced 7.2) with MD5 | MD5 or XXH3 64-bit per table (table property 163), fanout tables, read backup, fully replicated, dynamic primary replicas |
| Dictionary | table property keys up to ~160 | keys to 171 (TTL, hash function, fanout, ring buffer); `TABLE_DESC_CONF` long signal; MySQL's dictionary metadata (key 30) replaces frm data |
| Column types | up to Datetime/Timestamp | `Time2`, `Datetime2`, `Timestamp2` with fractional seconds; `Longvarchar` in indexes |
| Interpreter | 7.2 instruction set (registers, branches, LIKE) | plus memory regions, searches, sorting, conversions, interpreter I/O, partial column writes |
| Transporters | TCP, SHM, SCI | TCP, SHM, RDMA; multi-transporters (DB↔DB only); API uses TCP only |
| Version words | `IC_NDB_VERSION 0x080014` in `ic_base_header.h` | `0x1A0A00` |

## 16. Gathered for phases 6–9 (2026-10-08, read, not yet seen live)

Read from the 26.10 sources for the replanned phases 6–9 (chapter 06).
Each step folds its part into the proper section above when it starts,
verifies it on a live cluster, and then removes it from here. Signals
are named as in "Names" at the start of this chapter, and RonDB is not
cited by file and line (the author, 2026-10-21).

Configuration parameters are named by id, with our names: 129
`timeout_check_period`, 130 `idle_transaction_limit`, 131
`lock_wait_limit`, 800 `scan_batch_bytes_total`, 801
`scan_batch_bytes`, 802 `scan_batch_rows`.

### 16.1 Sending a request in fragments (phase 6.6)

Folded into §2.2 and §13, seen live, 2026-10-13.

### 16.2 Hashing a latin1 key (phase 8.6)

Folded into 6.3, seen live, 2026-10-12.

### 16.3 Unique keys (phase 6.5)

Folded into 6.8, seen live, 2026-10-09.

### 16.4 Scans (phase 8)

Folded into §7.2, 2026-10-15, with the author's account in §7.1; to be
seen live in 8.2.

### 16.5 AUTO_INCREMENT and the hidden key (phase 9.2)

- The counter table is stored as `sys/def/SYSTAB_0`, logged, with two
  columns: id 0, 32-bit unsigned, the key; id 1, 64-bit unsigned, not
  NULL, the next value not yet handed out. The data nodes create it at
  initial start and put no rows in it.
- The key is the table id; the backup's sequence uses 0x1F000000. The
  client that creates a table with an auto-increment column writes the
  row, as a write of the initial value. A table may have one
  auto-increment column (4335). Dropping a table leaves its row; the
  next table with that id overwrites it.
- Each counter operation is a transaction of its own, placed by the
  counter row's key and committed with its operation:
  - taking n values: an interpreted update adding n to the counter, then
    reading it back; exclusive lock, abort on error; the values are new
    − n to new − 1;
  - setting: a write, used at create;
  - moving up only: an interpreted update that writes the value only if
    it is larger, then reads the counter back;
  - peeking: a shared read. The reference's client never retries; MySQL
    servers retry temporary errors up to 100 times with a 30–36 ms
    pause.
- The client keeps a range per table: the last value handed out, the
  last reserved, the highest seen. A refill reserves cache size × step;
  the next value is the least k × step + offset above the last handed
  out (an offset above the step counts as 1). MySQL servers pass
  `auto_increment_increment` and `auto_increment_offset`, and a cache
  size of `ndb_autoincrement_prefetch_sz` (default 512).
- A value that does not fit the column fails the insert in MySQL's
  strict mode, and outside it is cut to the column's largest, so that
  the next insert finds it taken.
- After an insert with an explicit value, MySQL servers move the counter
  past it, upwards only, unless their cache or highest seen is already
  above. Other nodes' cached ranges are not told.
- The interpreted update is a `KEY_OP_REQ` with flags bit 15 and an
  update (seen on the wire, §9.6); its column section begins with five
  size words (initial read, program, final update, final read,
  subroutines), here 0, 7, 0, 1, 0: read the counter into a register,
  load the 64-bit constant, add, write the counter from the sum, exit
  with success; the final read names the counter column; the row store
  runs it. The conditional branch compares its operands in the order the
  row store reads them, not the order the reference's builder takes
  them.
- Without the interpreter the same is an exclusive read and an update
  in one transaction: two round trips, the row locked across the
  client's round trip.
- The dictionary has only the column's auto-increment flag; the start
  value is not stored there.
- A table without a primary key gets a last column stored as `$PK`:
  64-bit unsigned, NOT NULL, primary key, auto-increment, no default;
  its first value is 1; MySQL servers take its values from the same
  counter, 1 000 at a time, and learn a row's key by scanning.
- TRUNCATE drops and creates the table, so its counter starts again; an
  in-place `ALTER TABLE … AUTO_INCREMENT=n` only moves it up.
- Correction to §12: it lacks the creating client's write, moving up
  only, peeking, and the hidden key.

### 16.6 When the reference gives up waiting (phase 6.3)

Folded into 6.9, seen live, 2026-10-09.

## 17. Where the code's facts are checked (2026-10-08)

The code cites this section, never RonDB (chapter 09, "Names"): a
comment that rests on a protocol fact ends with "Verify: chapter 05
§17.N". Module by module, each fact is given in our words, checked
against the 26.10 tree when written. Where a section above treats the
same fact, that section is the fuller account; this one is the index the
code points into.

### 17.1 Portability layer (`ic_port`)

`crates/ic_port/src/consts.rs`

- Block numbers from 0x8000 (32768) up belong to the user threads of an API node.
- Signals bundled into one are addressed to block 0x7FF (2047), BUNDLE.
- Every API node has the fixed block 0xFA2 (4002), NODE_WATCH, which keeps track of the state of the other nodes.
- The kernel blocks this library addresses are COORDINATOR 0xF5, DICTIONARY 0xFA, MEMBERSHIP 0xFC, NODE_ADMIN 0xFE, CHANGE_FEED 0x101, INFO_TABLES 0x107 and LINK_ADMIN 0x10A.
- A version is encoded as `(major << 16) | (minor << 8) | build`, so 26.10.0 is 0x1A0A00.
- A block reference keeps the node id in its low 16 bits, so no node id above 65535 can travel.
- The largest node id a cluster allows is 8191; the limit before it was 2039.
- A data node's id is at most 144.
- A cluster has at most 144 data nodes: the limit of 145 counts node id 0, which is never used.
- A row holds at most 18000 words, which is 72000 bytes.

`crates/ic_port/src/ndb_err.rs`

- Every error code the data nodes and the reference's client report has a class: application, configuration, no data, constraint, schema, schema object exists, user defined, out of space, temporary resource, node recovery, overload, timeout, node stopping, temporary internal, unknown result, unknown code, internal or not implemented.
- Each class is permanent, temporary or of unknown outcome: the temporary resource, node recovery, overload, timeout, node stopping and temporary internal classes are temporary, unknown result and unknown code are of unknown outcome, the rest permanent.
- The schema object exists class has no entry in that table, so the reference's client reports its codes, 721, 746 and 4244, as of unknown outcome, one to try again (MySQL 8.4's table lacks it too). Ours takes it as permanent: a name taken stays taken until something drops it (the author, 2026-10-21). Mended in RonDB 9021f71df44 (2026-10-21), on 26.10-main, where the class is now permanent too.

### 17.2 Sockets and the management protocol (`ic_comm`, `ic_protocol`)

`crates/ic_protocol/src/proto_str.rs`

- The reference's client writes a request as the command on one line, one `name: value` line per argument and an empty line, and reads the reply against a table naming its first line and its fields.
- "get nodeid" asks for a node id.
- The reply to it starts with "get nodeid reply".
- "get config_v2" asks for the configuration in the version 2 format.
- "get config" is the version 1 form of the same request.
- Both forms are answered with "get config reply".
- "get version" asks for the server's version, and the client asks for the version 2 configuration only when that version allows it.
- The version 2 configuration is served from version 8.0.18 on.
- The reply to "get version" starts with "version", not the command and " reply".
- "get status" asks for the state of every node.
- The reply to it starts with "node status", again not the command and " reply".
- "get connection parameter" asks for one parameter of a link, which is how a data node's dynamically assigned port is found.
- The reply to it starts with "get connection parameter reply".
- "transporter connect" and an empty line end the management protocol on the socket, and the signal protocol takes it over.
- A configuration blob is announced with the content type "ndbconfig/octet-stream".
- A configuration blob is announced with the transfer encoding "base64".
- The node id request carries the user name "mysqld".
- The node id request carries the public key "a public key".
- Node types are numbered 0 for a data node, 1 for an API node and 2 for a management server.
- The data nodes number node types with the same values.

`crates/ic_comm/src/line_proto.rs`

- The management server's parser reads at most 512 bytes of a line, and the reference's client checks a line against that limit.

### 17.3 Configuration (`ic_apic`)

`crates/ic_apic/src/conf_param.rs`

- In a node section, ids 3 to 14 are common to every kind of node.
- Parameter 3, node_id, is the node's id in the cluster.
- Parameter 5, host, is the host the node runs on.
- Parameter 7, data_dir, is the node's data directory.
- Parameter 11, dedicated, says whether the node id is reserved for one host.
- Parameter 9, send_buffer_total, is the send buffer shared by all the node's links.
- Parameter 999, section_kind, is written in every section and says which kind of section it is.
- In the system section the ids 1 to 4 and 8 mean something else than in a node section: 3 is the cluster's name there.
- Parameter 1, first_mgm_node, is the management node that holds the master copy of the configuration.
- Parameter 2, config_generation, is the configuration's generation.
- Parameter 3, cluster_name, is the cluster's name.
- Parameter 4, api_rdma_allowed, says whether API nodes may reach data nodes over RDMA.
- Parameter 8, port_base, is the port the cluster allocates ports from.
- Parameter 101, replica_count, is the number of replicas per fragment.
- Parameter 119, api_heartbeat_ms, is how often a data node expects to hear from an API node, in milliseconds.
- Parameter 185, node_group, is the node group of a data node.
- Parameter 129, timeout_check_period, is how often a data node checks its transactions for ones that have waited too long, in milliseconds.
- Parameter 131, lock_wait_limit, is how long a data node lets an operation wait for a lock before it aborts the transaction, in milliseconds.
- The reference's client waits for a transaction's replies for the larger of 120 000 ms and, over the node sections of the configuration, parameter 129 plus parameter 131, a section without them counting as 0; then it gives the transaction up with 4012.
- The parameters of an API node itself are 800 to 809, with RonDB's own additions at 898 and 899.
- Parameter 800, scan_batch_bytes_total, is the largest total size of a scan batch in bytes.
- Parameter 801, scan_batch_bytes, is the most bytes of one scan's batch.
- Parameter 802, scan_batch_rows, is the most rows a scan batch may hold.
- Parameter 803, auto_reconnect, says whether to reconnect by itself after losing a data node.
- Parameter 805, redo_full_action, says what to do when the redo log is under pressure.
- Parameter 806, hash_map_buckets, is the default number of hash map buckets for a new table.
- Parameter 200, arbiter_rank, is a node's rank as an arbitrator, 0 meaning never.
- Parameter 201, arbiter_delay, is the delay before a node acts as arbitrator.
- A link section uses the ids 400 to 414.
- Parameter 400, node_1, is the lower numbered node of the pair.
- Parameter 401, node_2, is the higher numbered node of the pair.
- Parameter 402, signal_ids, says whether every signal carries an id.
- Parameter 403, checksum, says whether every signal carries a checksum.
- Parameter 406, port, is the port the listening side uses; 0 means it is assigned at start and must be asked for.
- Parameter 407, host_1, is the host name of node 1 of the pair.
- Parameter 408, host_2, is the host name of node 2 of the pair.
- Parameter 410, server_node, is the node of the pair that listens.
- Parameter 411, overload_bytes, is the send buffer use at which the link counts as overloaded.
- Parameter 412, checksum_before_send, says whether the checksum is computed before sending.
- Parameter 414, ip_version, is the IP version to prefer when a host resolves to both.
- Parameter 409, group, is the group of links a link belongs to when a pair has more than one.
- A TCP link adds the ids 452 to 499.
- Parameter 454, send_buffer, is the link's send buffer in bytes.
- Parameter 455, recv_buffer, is the link's receive buffer in bytes.
- Parameter 457, socket_recv_buffer, is the socket's `SO_RCVBUF`.
- Parameter 458, socket_send_buffer, is the socket's `SO_SNDBUF`.
- Parameter 459, socket_max_segment, is the socket's `TCP_MAXSEG`.
- Parameter 460, bind_any, says whether the listening side binds every address.
- Parameter 462, require_tls, says whether the link must use TLS.
- Parameter 499, ipv4_only, says whether the link uses IPv4 only.

`crates/ic_apic/src/conf_blob.rs`

- A version 2 configuration starts with the eight bytes "NDBCONF2" (version 1 used "NDBCONFV") and a header of seven words: the total length in words, the version, which is 2, the number of default sections, which is 5, and the numbers of data nodes, API nodes, management nodes and links. Then come the five default sections (data node, API node, management node, TCP, shared memory), the system section, the node sections, the link sections and a checksum, every word in network byte order.
- The checksum is every word but the last exclusive-ored together, and equals the last word.
- A section starts with three words, its length, its number of entries and its type. An entry is a word with the value type in its top 4 bits and the id in its low 28, then one word for an integer, two words high half first for a 64-bit integer, or for a string a length that counts the closing NUL and the bytes padded with zeros to whole words.
- A value a section does not state is read from the default section for its type; an RDMA section has no default section.
- Section types are 0 invalid, 1 data node, 2 API node, 3 management node, 4 TCP, 5 shared memory, 6 system and 7 RDMA.

`crates/ic_apic/src/mgm_client.rs`

- An argument is written as `name: value` for a number and `name:"value"` for text, with no space before the quote, and a text argument's line may not exceed 512 bytes.
- A refused node id request comes with code 1101 when asking again may succeed, 1102 when it will not, and 1103 when the server could not match a host name and the node should ask again.
- A wanted node id that another node holds is refused with "Id N already allocated by another node." and code 1101, the same code as the refusals that time may cure; the server notes that the MySQL server reads its node id refusal texts.
- The node id request sends version, nodetype, nodeid, user, password, public key, endian, an optional name and log_event, and its reply carries result, nodeid and, on a refusal, error_code.
- The configuration request sends version, nodetype and node; its reply carries result, Content-Length, Content-Type and Content-Transfer-Encoding, and the base64 text that follows is one byte longer than Content-Length says, for its closing newline.
- The link parameter request sends node1, node2 and param, and its reply carries value and result.
- Handing the socket over sends "transporter connect" and an empty line, and the reference's client gives the socket up without closing it.

### 17.4 Block numbers (`ic_signals::blocks`)

- The data node's blocks are numbered: archive 0xF4, coordinator 0xF5, distribution 0xF6, the fragment's operation block 0xF7, hash index 0xF8, row store 0xF9, dictionary 0xFA, start control 0xFB, membership 0xFC, file I/O 0xFD, node administration 0xFE, index build 0xFF, utility 0x100, change feed 0x101, ordered index 0x102, the join block 0x108, a thread administration block 0x109, link administration 0x10A, and the read workers 0x10B to 0x111.
- A block reference is one word: the node id in the low 16 bits, the block number in the high 16.
- The first block number an API node's threads may use is 0x8000.
- An API node may have at most 4711 threads that talk to the cluster.
- The bundle block, to which several small replies are sent as one signal, is 0x7FF (2047).
- An API node's fixed node watch block, which takes heartbeat answers and node state, is 0xFA2.
- The management server's configuration service block is 0xFA3.
- A reference is built as the node id or-ed with the block number shifted up 16 bits.
- The node of a reference is its low 16 bits.
- The block of a reference is its high 16 bits.
- A data node's block number carries the instance of its block, as a coordinator record's block does: the high 7 bits are the instance's low 7 bits, and the low 9 bits hold the main block, counted from 244, plus the instance's higher bits in steps of 64, the sum wrapping at 512 and skipping 0. Up to instance 127 the low 9 bits are the block itself, 0xF5 for the coordinator, of which there may be up to 360 instances.

### 17.5 Signal numbers (`ic_signals::numbers`)

- HEARTBEAT_CONF is signal 1.
- HEARTBEAT_REF is signal 2.
- HEARTBEAT_REQ is signal 3.
- NODE_LOST_REP is signal 26.
- NODE_LOST_DONE_REP is signal 27.
- NODE_JOINED_REP is signal 163.
- COORD_ALLOC_REQ is signal 39.
- COORD_ALLOC_CONF is signal 37.
- COORD_ALLOC_REF is signal 38.
- COORD_FREE_REQ is signal 36.
- COORD_FREE_CONF is signal 34.
- COORD_FREE_REF is signal 35.
- COMMIT_REQ is signal 19.
- COMMIT_CONF is signal 17.
- COMMIT_REF is signal 18.
- ROLLBACK_REQ is signal 15.
- ROLLBACK_CONF is signal 13.
- ROLLBACK_REF is signal 14.
- TRANS_ABORTED_REP is signal 16.
- COMMIT_MARKER_FREE_ORD is signal 469.
- TAKEOVER_CONF is signal 8.
- TAKEOVER_REF is signal 9.
- DEADLOCK_FOUND_REP is signal 980.
- KEY_OP_REQ is signal 12.
- KEY_OP_CONF is signal 10.
- KEY_OP_REF is signal 11.
- UNIQUE_OP_REQ is signal 519.
- UNIQUE_OP_CONF is signal 520.
- UNIQUE_OP_REF is signal 521.
- ROW_DATA_REP is signal 5.
- KEY_DATA_ORD is signal 6.
- COLUMN_DATA_ORD is signal 4.
- SCAN_ROW_KEY_REP is signal 33.
- SCAN_REQ is signal 32.
- SCAN_CONF is signal 29.
- SCAN_REF is signal 31.
- SCAN_MORE_REQ is signal 28.
- TABLE_DESC_REQ is signal 24.
- TABLE_DESC_CONF is signal 190.
- TABLE_DESC_REF is signal 23.
- LIST_OBJECTS_REQ is signal 193.
- LIST_OBJECTS_CONF is signal 194.
- TABLE_CHANGED_REP is signal 606.

### 17.6 Message framing and bundles (`ic_signals::header`, `ic_signals::bundle`)

- A message is three header words (byte order and flags, lengths; signal number, trace, section count; receiver and sender block), then an optional signal id, the data, the section lengths and sections, and an optional checksum.
- A signal carries at most 25 words of data; longer data goes in a section.
- The largest message is 32768 bytes, sent or received.
- The byte order value is 1 on a big-endian machine and 0 on a little-endian one.
- The comment that describes the header words gives these two values the other way round.
- The checksum is the exclusive or of every word of the message before the checksum word.
- The one significant byte order bit is repeated into bits 0, 7, 24 and 31 of the first word.
- A bundled signal goes to block 2047. The receiver walks its data part by part: a header word with the block in bits 16-31 and the length less three in bits 0-4, at most 25 words, and each part is delivered to its own block as if it had come alone.
- The coordinator sends KEY_OP_CONF (10) to an API node bundled this way.
- The node that reads a row bundles a short ROW_DATA_REP (5), the part's header word being the block shifted up 16 bits plus the row data's length.
- A data node drops a signal whose data words and sections come to more than 25; `encode` refuses one.
- `encode_train` sends a signal whole up to 8 040 words of sections and as a train of fragments above, laid out as §2.2 says: pieces of whole 60-word segments but a section's last, the data in the last fragment only.

### 17.7 Membership and heartbeats (`ic_signals::membership`)

- HEARTBEAT_REQ (3) is three words, our block reference, our version and our MySQL version, and is answered by the membership block.
- HEARTBEAT_CONF (1) is the membership block's reference, its version, the heartbeat interval, its MySQL version, the oldest data node version, the node state, and the oldest API node version.
- The node state is 16 words.
- The connected-node bitmap inside it is 8 words, 256 bits, and the data node fills it with the nodes it is connected to.
- The start levels: nothing 0, process up 1, starting 2, started 3, single user 4, and stopping in four phases 5 to 8.
- HEARTBEAT_REF (2) is four words, the last but one the reason: wrong node type 1, unsupported version 2, unconfigured node 3.
- The data node sends the heartbeat interval in hundredths of a second, a tenth of the configured milliseconds, and the reference multiplies it by ten.
- NODE_LOST_DONE_REP (27) is five words: block, reporting node, failed node, and two more.
- NODE_LOST_REP (26) in its modern form is three words with the bitmap of failed nodes in a section; the two older forms carry the bitmap inline, sized for data nodes or for every node.
- The bitmap's length is the signal data length less three.
- The membership block sends NODE_LOST_DONE_REP to every registered API node with its own reference in the block field; zero there means the whole node only inside a data node.
- The reference's receiver reads only the failed node id.

### 17.8 Properties and table descriptions (`ic_signals::prop_stream`, `ic_signals::table_props`)

- A property's head word is the value type shifted up 16 bits plus the key. The head, numbers and length words are written in network byte order; a two-word number goes low word first; text and bytes are copied as they are, padded to whole words.
- The reader converts the head, numbers and length words from network byte order, steps over the bytes rounded up to words, and refuses a value type it does not know.
- Keys a reader does not know are passed over.
- The value types: one-word number 0, text 1, bytes 2, two-word number 4.
- Text is sent with a length that counts its terminating NUL.
- The table keys and the column keys, with their numbers.
- The table part ends at the first column name; a column ends at its end marker; the table end marker is not a table key.
- The defaults are what the table and column structures are initialised with.
- A column's element size and count are worked out from its type, length, precision and scale.
- The reference reads a description into a table, column by column, this way.
- The column type, length storage and storage numbers.
- The offline part of a table version is its low 24 bits, which is what operations are checked against.
- The replica data is 16-bit values in network byte order: replica count, fragment count, then per fragment its log part and the node of each replica. The dictionary gets it from the distribution block and adds it only when answering TABLE_DESC_REQ (24).
- The reference reads the replica data and passes over each log part.
- In MySQL the largest decimal precision is 65 and the first scale that means "not specified" is 31.
- A table placed by hash map sends a key hash to the fragment its map gives for the hash modulo the map's size.
- A hash map is described by its name (key 1), its bucket array's length in bytes (2), the buckets as 16-bit values (3), and the table keys for id and version; every value starts at zero.
- The reference halves the byte length to get the bucket count.
- When no column is marked as part of the distribution key, every primary key column is.
- A default value is a column header in network byte order, whose low 15 bits are the size in bytes, then the value; size zero means no default.
- The byte order conversion the reference applies to a default changes nothing on a little-endian host.

### 17.9 Dictionary requests and notices (`ic_signals::table_desc`, `ic_signals::list_objects`, `ic_signals::table_change`)

- The reference asks for a table by name with a long answer; the name length counts the terminating NUL, and section 0 is the name padded to whole words.
- The dictionary answers with the description in section 0, through the send that fragments when it must.
- A data node sends a signal whole when its data and sections come to at most 7400 words, and in fragments beyond that.
- The reference gathers the fragments of the answer into one buffer sized by the total length.
- TABLE_DESC_REQ (24) is five words: caller data, caller reference, request type, table id or name length, schema transaction. Request types: by id 0, by name 1, long answer 2. The name goes in section 0.
- TABLE_DESC_CONF (190) is six words: caller data, table id, global checkpoint, total length in words, object type, the dictionary's reference.
- TABLE_DESC_REF (23) is seven words with the error code sixth and the source line seventh; an older node sent five, the error code last. Codes: 701 busy, 702 name too long, 709 no such id, 710 not fetchable by name, 723 no such table.
- LIST_OBJECTS_REQ (193) is five words: caller data, caller reference, flags, table id, object type.
- LIST_OBJECTS_CONF (194) is two words, caller data and an object count, with three words per object in section 0 and the names in section 1.
- Each name is a length word in bytes, counting the NUL, then the name padded to whole words.
- The dictionary sends a long list in pieces, each with the count of its own objects, and the reference adds the counts up.
- The reference expects the long form from every data node; the older form packed each object into one word.
- Request flags: names wanted is bit 28, dependents only is bit 30.
- The older form keeps the table id in the low 12 bits of the flags word, and the reference fills it in.
- The reference lists a table's dependents, names included, to find its indexes.
- In each object's first word the store is bits 20-22, temporary bit 23 and state bits 24-27; the reference reads one name per object.
- TABLE_CHANGED_REP (606) goes to the API node's node watch block.
- For an alteration, each data node's dictionary hands it to its membership block, which sends it to every API node registered there.
- The version it carries is the one being replaced, and the name is the old name after a rename.
- For a drop it is sent once, by the master, and only to API nodes whose version is 26.05 or later, 26.02.5 or later in 26.02, or 25.10.14 or later in 25.10.
- It is three words, table id, table version and kind of change (1 altered, 2 dropped), with the name in section 0.
- The name is padded with NULs to 128 bytes.
- The reference hands it to its dictionary cache.

### 17.10 Coordinator records (`ic_signals::coord_alloc`)

- The six signal numbers: COORD_FREE_CONF 34, COORD_FREE_REF 35, COORD_FREE_REQ 36, COORD_ALLOC_CONF 37, COORD_ALLOC_REF 38, COORD_ALLOC_REQ 39.
- The reference sends COORD_ALLOC_REQ (39) and COORD_FREE_REQ (36) as three plain words each, with no sections.
- COORD_ALLOC_REQ carries our pointer, our block reference and the coordinator instance wanted, and from RonDB 26.10 a fourth word, the release id: random, never zero, kept by the coordinator with the record.
- COORD_ALLOC_CONF (37) is three words: our pointer, the coordinator's pointer, the coordinator's reference.
- COORD_ALLOC_REF (38) is two words, our pointer and an error code, sent when the node is not started, is shutting down, or has no record free.
- COORD_FREE_REQ puts the coordinator's pointer first, then our block reference, then our pointer, and from RonDB 26.10 the record's release id fourth.
- COORD_FREE_CONF (34) is one word, our pointer.
- COORD_FREE_REF (35) is our pointer, an error code and the source line, three words, or five when the sender's reference does not match the record's.
- COORD_FREE_REQ for a record whose transaction is still in progress makes the coordinator abort it, or conclude it without telling the client, and then free the record and answer COORD_FREE_CONF; replies of that transaction may still reach the client, which must check them against the transaction id.
- A record is freed once. A COORD_FREE_REQ that names a record no longer allocated, or allocated to another block, is refused with COORD_FREE_REF, and a debug data node stops on it first (error 2343, a failed assertion). Since the coordinator may hand a freed record to another transaction, of any API node, a second free must never be sent. With release ids (RonDB 26.10, 6.1) a free for a record already freed is refused, and one with another record's id is ignored while a transaction uses that record and refused otherwise, so a stale free no longer frees a record handed on.
- Seen live (2026-10-09, debug build): the first version of step 6.3 freed a timed-out transaction's record, and freed it again when the connection let go of its records; each time the data node that held it stopped with error 2343, the check above, about 0.3 s after our node disconnected. The author added release ids the same day.

### 17.11 Key operations and column headers (`ic_signals::key_ops`, `ic_signals::column_header`)

- KEY_OP_REQ (12) has eight fixed words; in the long form the key is section 0 and the column data section 1.
- After the eighth word come, only when their flags say so and in this order, the user id and its version, the scan information, and the distribution key.
- The coordinator refuses a long request that is not exactly as long as its flags make it, as a malicious signal.
- The reference finds a KEY_OP_CONF's transaction by its first word, or from the operations when that word is the null value 0xFFFFFF00.
- KEY_OP_REF (11) and ROW_DATA_REP (5) name the operation by our pointer in the first word.
- KEY_OP_CONF (10) is our transaction pointer, the global checkpoint's high word, an info word and the transaction id, then two words per operation (our pointer, row data length), then the checkpoint's low word. The info word holds the operation count in its low 16 bits, the commit bit at 16, and the marker bit at 17, which counts only with the commit bit.
- A row data length above bit 31 marks a dirty read, its low bits naming the reading node, and one ROW_DATA_REP then completes the operation whatever its length.
- The checkpoint's low word is read only when the signal is long enough to hold it.
- ROW_DATA_REP is three words, our pointer and the transaction id, then the row: inline when short, section 0 when long. A long one in fragments has a fourth word, the row's whole length.
- COMMIT_MARKER_FREE_ORD (469) is the two words of the transaction id, sent to the block that sent the KEY_OP_CONF, even when the confirmation is otherwise rejected.
- Operation types: read 0, update 1, insert 2, delete 3, write 4, exclusive read 5.
- Abort options: abort on error 0, ignore error 2.
- Request flag bits: dirty 0, no disk 1, commit 4, operation 5-7, simple 8, execute 10, start 11, abort option 12-13, read committed base 20, no wait 21, batch safe 22, batch unsafe 23.
- In the long form the third word, the column data length, is zero.
- The coordinator puts at most three operations in one KEY_OP_CONF: six words.
- KEY_OP_REF is five words: our operation pointer, the transaction id, the error code, more about the error.
- COMMIT_REQ (19) and ROLLBACK_REQ (15) are the coordinator's pointer and the transaction id. The coordinator checks the id against the record, and a bad pointer or a wrong id gets no reply.
- The reference puts the coordinator's pointer first in both.
- COMMIT_CONF (17) is five words: our pointer, with its low bit set when a commit marker was kept, the transaction id, and the global checkpoint high then low.
- COMMIT_REF (18) is four words, the error code last; ROLLBACK_REF (14) adds a fifth, the coordinator's state.
- ROLLBACK_CONF (13) is three words, our pointer and the transaction id.
- TAKEOVER_CONF (8) is three words; the low bit of our pointer asks for COMMIT_MARKER_FREE_ORD, which goes to the sender.
- TAKEOVER_REF (9) is three words, our pointer and the transaction id.
- TRANS_ABORTED_REP (16) is five words: our pointer, the transaction id, the transaction's error code, and more about the error.
- A column header holds the column id in bits 16-31, the partial read or write flag in bit 15, and the value's size in bytes in bits 0-14; the value takes that many bytes rounded up to words, and size zero is NULL.

### 17.12 Links, threads and signals (`ic_apid::apid_conn`, `apid_global`, `handshake`, `heartbeat`, `fragments`, `node_connect`, `node_state`, `rec_thread`, `signal_reader`, `signal_page`, `thread_conn`, `connect_thread`, `adaptive_send`, `send_pool`, `lib`)

- A transaction id holds the low 12 bits of the user thread's block number less 0x8000 in bits 52 to 63 and the low 12 bits of the API node's id in bits 40 to 51; bit 12 of the node id goes in bit 39 and bit 12 of the block index in bit 38; the low 32 bits are a count that wraps without carrying into bit 32. Ours is laid out the same since 2026-10-14 (`apid_conn::transaction_id`), with up to 4 711 user threads.
- The count of a newly opened block goes on from where the last holder of the same block number left it; a holder stores its count for the block before it lets the block go.
- The reference API sends a heartbeat every half check interval, or at its own longest registration interval if that is shorter.
- The check interval in `HEARTBEAT_CONF` (signal 1) is in hundredths of a second, and one shorter than 100 ms is raised to 100 ms.
- The missed count goes up at the end of every check interval, and a node whose count reaches four, with its interval known, is lost.
- The client sends a line saying what it can do about encryption ("ndbd TLS disabled" for cleartext) and an empty line, and reads one reply: "ok" from a server too old to know encryption, "Cleartext ok" when cleartext is accepted, "TLS required" when it is not.
- A server of the older kind reads the two lines as a user name and a password and answers "ok".
- The hello is four numbers, our node id, the transporter type, the far node's id and the link instance; the reply is two, the far node's id and its type, and both are checked.
- A hello is at most 23 bytes before its newline, the limit servers before 8.0.20 read.
- The data node accepts a hello of two, three or four numbers.
- A TCP link is transporter type 1; 3 is shared memory and 4 RDMA.
- A data node not expecting a connection from that node answers the hello with "BYE", waits up to a second for the client to close, and resets the socket if it does not.
- A missed-heartbeat counter goes up at the end of every check interval and back to zero on every reply, and the node is lost when it reaches four.
- A data node splits a signal whose data and sections together pass 7400 words; a debug build splits `ROW_DATA_REP` (signal 5) above 240 words.
- The pieces are 3840 words, or 120 from a debug build; the note beside them says splitting starts above that size, but the code splits only above 7400 words.
- Every piece carries the original's whole signal data, then the number of each section it carries, highest first, then the fragment id; the first piece is marked 1, middle ones 2 and the last 3.
- A receiver reads the fragment id from the last data word and the section numbers from the words before it.
- The reference's client checks the request number in the data of every fragment of a table description, so it relies on each fragment carrying the original data.
- A data node joining a train keeps the last fragment's data, and reads only the trailing section numbers and fragment id of the others; `fragments` keeps the last's too.
- Fragment ids are counted per sender, skipping 0; `apid_conn` counts them per user thread.
- The reference API's send thread sends a held-back send after a grace period of 200 µs.

### 17.13 Dictionary and records (`ic_apid::dict_cache`, `dict_client`, `record`, `text_row`)

- The first thread to ask for a table name leaves an entry saying it is being fetched, and threads asking meanwhile wait for that fetch.
- The fetching thread stores what it got and wakes the waiters; a change notice that came during the fetch makes the stored version one that no longer holds.
- `TABLE_CHANGED_REP` (signal 606) names the table and the version that no longer holds; that version is marked altered or dropped, and let go of once nobody uses it.
- When the last data node link goes, every cached entry not being fetched is marked invalid.
- An index is looked up by its current internal name, then by the older one; one bound for another version of its table is let go of and fetched again.
- An index's internal name is `sys/def/<table id>/<index>`; an older version made `<database>/<schema>/<table id>/<index>`.
- An index is described as a table of its own, with its own id and version, its columns matched to the base table's by name and the last, hidden column left out.
- A table description (`TABLE_DESC_REQ`, signal 24) is asked of any started node, each attempt with a request number of its own, up to 100 attempts, and a reply that the dictionary is busy is asked again.
- Before each new attempt the pause is 50 ms plus ten times a random number below 5, below 10 after half the attempts, and from 100 ms after three quarters; a failed send or a lost node is asked again too.
- A table placed by hash map has its hash map fetched as part of fetching the table.
- The dictionary answers `LIST_OBJECTS_REQ` (signal 193) only with `LIST_OBJECTS_CONF` (signal 194); it has no refusal.
- A record is checked so that no two values or null bits overlap: each is a range of bits, the ranges are sorted by start and each must begin after the one before ends; a NOT NULL column has no null bit to check.
- A column's room in a record is its size in bytes, a blob's a pointer.
- The same column twice in a record is refused, with error 4291.
- The reference's default record puts the null bits first, one per nullable column in attribute order, then each column in attribute order right after the one before, with no padding.
- A value's size in words is its byte size rounded up.
- The row store reads a fixed-size column from a word offset in the row.

### 17.14 Key operations (`ic_apid::hash`, `key_op`, `query`, `row_codec`, `transaction`)

- The bytes hashed are the distribution key columns in attribute order: a fixed-size value in full, a variable-sized one behind its one or two length bytes with only the bytes it has, a character column first transformed by its collation, each padded to a word.
- The value that places a key is the second word of the hash; a table with fanout partitioning hashes a base part and a detail part apart.
- The hash is XXH3, its low and high halves the first two words, for a table that asks for the new hash, and MD5 otherwise.
- A transaction started with a key goes to the partition of the key's hash.
- The partition of a table placed by hash map is the map's entry at the hash modulo the map's length.
- A table that is neither read-backup nor fully replicated goes to the primary replica, a node in the client's own location domain first; a fully replicated one to any node holding any fragment; a read-backup one to any replica of the fragment.
- A fragment's primary is its only live replica, or with several alive, a node group's alive nodes, sorted by id, take its fragments in turn in batches of the fragment count divided by the alive count, rounded up; this is worked out again when a node comes or goes.
- Among candidate nodes the nearest is preferred, then the one this client has chosen least, the primary winning a tie.
- Pseudo column 0xFFFE reads back the partition of the row's fragment, as one word.
- A read set to read committed is a simple and dirty read; a simple read is simple and not dirty.
- A simple read's abort option is always to ignore errors; any other operation takes the one asked for.
- `KEY_OP_CONF` (signal 10) gives, for each operation, the operation and the length of row data to expect; for a dirty read that word is a marker bit with the reading node's id, and a dirty read whose reading node has failed completes with error 4119.
- `ROW_DATA_REP` (signal 5) completes an operation when what has come reaches the length the confirmation gave, or at once for a dirty read.
- The key a request carries is each key column in attribute order, a fixed-size value in full, a variable-sized one with its length bytes and only the bytes it has, each padded to a word.
- Key positions are numbered in attribute order.
- A read asks with pseudo column 0xFFF0 and the column count when it wants every column, otherwise with 0xFFF3 followed by a bitmask of the attribute ids wanted.
- The row comes back behind one 0xFFF3 header whose size is the result bitmap's length in bytes: a bit for each column read and, after each nullable one, a bit set if it is NULL; then the values in attribute order, NULLs taking no room, an 8- or 16-bit column after the last word bits touched, a 32-bit or wider one at the next word, and bit columns packed together starting on a word.
- The client unpacks the row by the same rules, a variable-sized value saying its length in its first one or two bytes.
- An operation through a unique index accesses the index's own table, whose primary key is the index columns.
- `UNIQUE_OP_REQ` (signal 519) is laid out as `KEY_OP_REQ` (signal 12), and `UNIQUE_OP_CONF` (signal 520) as `KEY_OP_CONF` (signal 10).
- The coordinator reads the index's own table with the key the request carries, asking for the stored primary key, then runs the operation on the base table with that key.
- Both the read of the index and the operation on the table assert that the request carries neither the dirty nor the simple flag.
- The reference's client sends a committed or simple read through an index as a shared-lock read, the committed one with bit 20 of the flags word set, which lets a backup replica of a read-backup table serve both reads; the lock is held to commit. It refuses an insert through an index with error 4200.
- When a node's failure is reported handled, a transaction coordinated there that was sending operations or a commit fails with error 4010, and one that was rolling back counts as a success.
- `TAKEOVER_CONF` (signal 8) is taken by a transaction sending operations or a commit, and `TAKEOVER_REF` (signal 9) by one sending operations or a rollback.
- `TAKEOVER_CONF` says the transaction committed; one that had reads fails with error 4115, as their rows may be lost.
- `TAKEOVER_REF` says the transaction was aborted: a success if a rollback was being sent, error 4031 otherwise.
- The coordinator that takes over sends `TAKEOVER_CONF` with the transaction id, the low bit of its first word set when a commit acknowledgement is owed, routed through another node if it has no link to the API node.

### 17.15 The C header, `ic_capi`, `xtask` and the tools



- The column type numbers 0 to 33 in the C header are the numbers the data nodes send for a column's type.