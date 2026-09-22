# 03 — Module map: C tree → Rust workspace

Module-by-module translation table. Modes: **1:1** translate function for
function keeping names and structure; **Redesign** the C is the
specification but the Rust structure differs (written down in the crate's
`MODULE.md`); **New** the C is a stub or absent, written from
[04-api-design.md](04-api-design.md) and [05-ndb-protocol.md](05-ndb-protocol.md);
**Out** not part of this project (API-node focus).

Line counts are the C sizes and feed the estimates in
[06-phases.md](06-phases.md).

## Workspace layout

```
iclaustron/                    (this repository, branch RUST-iclaustron)
  legacy-c/                    # all existing C code, unchanged, unbuilt reference
  Cargo.toml                   # [workspace]
  LICENSE                      # MIT
  rustfmt.toml  clippy.toml  deny.toml  rust-toolchain.toml
  crates/
    ic_port/                   # port/         OS abstraction, replaces glib
    ic_util/                   # util/         containers, strings, threadpool, errors, debug
    ic_comm/                   # comm/         sockets, poll set, socket buffers, line protocol
    ic_protocol/               # protocol/     base64, mgm protocol strings
    ic_apic/                   # api/ic_apic_* mgm client, config blob decode, typed config
    ic_ndb_signals/            # NEW           GSNs, block numbers, signal structs, codecs
    ic_apid/                   # api/ic_apid_* the Data API
    ic_capi/                   # NEW           C ABI (cdylib + staticlib), cbindgen header
  cpp/include/iclaustron.hpp   # NEW           header-only C++ wrapper over the C ABI
  tools/
    ic_config_dump/            # NEW  print the cluster config fetched from ndb_mgmd
    ic_desc/                   # NEW  describe a table as the API sees it
    ic_bench/                  # NEW  PK read/write benchmark vs the C++ NDB API
    ic_sigdump/                # NEW  decode a captured signal stream
  tests/
    unit/                      # ports of test/test_unit.c
    integration/               # against a live RonDB 26.10 cluster
    examples/                  # Rust, C and C++ example programs (installed)
  doc/
```

Dependency order (no cycles):

```
ic_port → ic_util → ic_comm → ic_protocol → ic_apic → ic_ndb_signals → ic_apid → ic_capi
```

## Crate `ic_port` ← `port/ic_port.c` (2 166 lines), `include/ic_port.h`, `include/ic_base_header.h`

Everything that touches the OS or glib. Every glib function in the C tree
is replaced here and nowhere else.

| C | Rust module | Mode | Notes |
|---|---|---|---|
| `ic_base_header.h` constants (`IC_RNIL`, node/thread limits, module ids, ports) | `consts.rs` | 1:1 | `pub const`, same names; NDB version constants set to RonDB 26.10 |
| `ic_malloc/ic_calloc/ic_free*` per-module allocators with DEBUG leak tracking | `alloc.rs` | Redesign | Rust owns memory; keep only per-module byte counters (`AllocStats`) for diagnostics |
| `IC_MUTEX/IC_COND/IC_SPINLOCK`, `ic_mutex_*`, `ic_cond_*` | `sync.rs` | 1:1 | Newtypes over `std::sync::Mutex`/`Condvar`; debug-build mutex ordering-level check |
| `g_private_get/set` | `tls.rs` | 1:1 | `thread_local!` |
| `IC_TIMER`, `ic_gethrtime`, `ic_*_elapsed`, `ic_sleep_low`, `ic_microsleep`, `GTimer` | `time.rs` | 1:1 | `std::time::Instant`, nanoseconds as `u64` |
| socket start/stop, `MSG_NOSIGNAL` probe | `socket.rs` | 1:1 | `libc`; `std::net::ToSocketAddrs` for name resolution |
| `ic_start_process`, `ic_is_process_alive`, `ic_kill_process` | — | Out | only the process controller used them |
| file I/O helpers | `file.rs` | 1:1 (subset) | `std::fs`; only what `ic_util` still needs (debug output file, pid file) |
| `ic_daemonize`, pid files, umask, signal handlers | `daemon.rs` | 1:1 | `libc::fork/setsid/signal`; the one place with real `unsafe`. Kept because API programs may run as daemons |
| `ic_byte_order`, `ic_swap_endian_word`, `g_ntohl/g_htonl` | `endian.rs` | 1:1 | `u32::to_be/from_be` for the config blob; signal protocol needs only the byte-order flag |
| `ic_get_last_error`, `ic_get_strerror` | `oserr.rs` | 1:1 | `std::io::Error::last_os_error()` |
| `GOptionEntry`/`GOptionContext` parsing | `options.rs` | Redesign | In-house table-driven parser (~200 lines): `struct OptionEntry { long_name, short_name, kind, help }` arrays like the C tables; no `clap` |
| `g_snprintf` | — | Drop | `format!`/`write!` |
| Windows branches | — | Out | |

## Crate `ic_util` ← `util/*.c` (6 456 lines)

| C | Rust module | Mode | Notes |
|---|---|---|---|
| `ic_err.c/h` (codes 7000–7123, message table, `ic_assert`, `ic_require`) | `err.rs` | 1:1 + New | `pub const IC_ERROR_*: i32`, `struct IcError`, `ic_assert!`, `ic_require!`; built: `ErrorCategory` and `ErrorSeverity` as the C header has them, on every code |
| — | `ndb_err.rs` | New | built: the NDB codes the library meets, each with the class the reference gives it and a sentence of our own; a code not in the table is still reported, as "NDB error N" of unknown class |
| `ic_debug.c/h` (`DEBUG_ENTRY`/`DEBUG_RETURN_*`, level bits, indent tracking) | `debug.rs` | Redesign | `debug_entry!("name")` returns a guard whose `Drop` does the return print; level bits unchanged; zero cost in release |
| `ic_mc.c` memory container | `mc.rs` | 1:1 | Arena of chunks; `alloc`, `calloc`, `reset`, `free` semantics unchanged |
| `ic_dyn_array.c` `IC_DYNAMIC_ARRAY` (+ ordered) | `dyn_array.rs` | 1:1 | growable byte buffer; the spill-to-disk variant is Out (config writer only) |
| `ic_dyn_array.c` `IC_DYNAMIC_PTR_ARRAY` | `ptr_array.rs` | Redesign | Generational slot map `insert → u32`, `get`, `remove`; the object-reference map, one per user thread |
| `ic_bitmap.c` | `bitmap.rs` | 1:1 | |
| `ic_hashtable.c` + `_itr.c` | `hashtable.rs` | Redesign | `std::collections::HashMap` behind the `ic_hashtable_*` names; the third-party copyright disappears |
| `ic_string.c` `IC_STRING` | `string.rs` | 1:1 (subset) | `IcString`; the directory-layout helpers survive only as `set_config_dir`/`set_data_dir` for debug/pid files |
| `ic_threadpool.c` | `threadpool.rs` | 1:1 | `std::thread` + `sync.rs`; same ops |
| `ic_config_reader.c` (.ini reader) | — | Out | no config files are read |
| `ic_lex_support.c` | — | Out | served the bison parsers |
| `ic_hw_info.c` | — | Out | process controller |
| `ic_parse_connectstring.c` | `connectstring.rs` | 1:1 + New | stock `nodeid=N,host:port,...` syntax |
| `ic_readline.c` | — | Out | cluster client |
| `ic_linked_list.h` macros | — | Drop | `Vec`/`VecDeque`; intrusive index links only in send chains and page chains |
| `ic_util.c` misc | `misc.rs` | 1:1 | |
| `test/test_unit.c` types 1–6, 8 | `tests/unit/*.rs` | 1:1 | first `cargo test` targets |

## Crate `ic_comm` ← `comm/*.c` (5 319 lines)

| C | Rust module | Mode | Notes |
|---|---|---|---|
| `ic_connection.c` (`IC_CONNECTION`, ~50 ops, read/write sessions, stats, background connect) | `connection.rs` | 1:1 API, Redesign inside | `struct Connection` with methods; `trait Transport` (plain TCP now, TLS later) is the one backend trait |
| SSL toggles / `ic_ssl.h` | `tls.rs` | New, later | `rustls` behind feature `tls`; not in 0.1 |
| `ic_poll_set.c` (epoll/kqueue/poll) | `poll_set.rs`, `poll_epoll.rs`, `poll_kqueue.rs`, `poll_posix.rs` | 1:1 | `libc` directly; Solaris event ports Out |
| `ic_sock_buf.c` (128-byte page descriptors, pool + thread-local free lists, refcount) | `sock_buf.rs` | 1:1 | `AtomicI32` refcount |
| `ic_protocol_support.c` (CR-terminated line protocol helpers) | `line_proto.rs` | 1:1 | used by the mgm client |
| `test/test_comm.c` | `tests/comm_*.rs` | 1:1 | |

## Crate `ic_protocol` ← `protocol/*.c` (720 lines)

| C | Rust module | Mode |
|---|---|---|
| `ic_base64.c` | `base64.rs` | 1:1 |
| `ic_proto_str.c` | `proto_str.rs` | 1:1, subset: only the mgm protocol keywords |
| `ic_pcntrl_proto.c` | — | Out |

## Crate `ic_apic` ← `api/ic_apic_*.ic` (read side only)

The C client side is complete but targets NDB 7.2.9 configuration. The
parameter table (3 056 lines, ~180 data-node fields) is not carried over
as is: an API node needs a small typed subset, and the rest is kept
generically.

| C | Rust module | Mode | Notes |
|---|---|---|---|
| `ic_apic_conf_read_proto.ic` (`get nodeid`, `get config_v2`, base64, keep-alive) | `mgm_client.rs` | 1:1 | plus `get status`, `get version`, `get connection parameter` (dynamic ports) |
| `ic_apic_conf_read_transl.ic` (blob → structs) | `conf_blob.rs` | Redesign | decode the v2 blob into a generic `ConfigBlob { sections: Vec<Section { type, entries: HashMap<key, Value> }> }`; v1 dropped (26.10 mgmd always serves v2) |
| `ic_apic_conf_param.ic` (parameter table) | `conf_param.rs` | Redesign | a small table of the parameter ids an API node uses: own node (BatchSize, BatchByteSize, MaxScanBatchSize, HeartbeatIntervalDbApi, ArbitrationRank, DefaultOperationRedoProblemAction, ...), data nodes (NodeId, HostName, NodeGroup, ServerPort, ...), TCP sections (NodeId1/2, HostName1/2, PortNumber, SendBufferMemory, ReceiveBufferMemory, Checksum, SendSignalId, TCP_SND/RCV_BUF_SIZE, TcpBind_INADDR_ANY, ...). Ids verified against `mgmapi_config_parameters.h` in 26.10 |
| `include/ic_apic_data.h` data model | `data.rs` | Redesign | `ClusterConfig { nodes: Vec<NodeConfig>, api: ApiNodeConfig, tcp: Vec<TcpLinkConfig> }`; the ~180-field data-server struct is Out |
| `ic_apic_cluster_config.ic` (raw blob key lookup) | folded into `conf_blob.rs` | 1:1 | |
| `ic_apic_if.ic`, `ic_apic_conf_net_read.ic` (`ic_get_configuration`) | `api_config_server.rs` | 1:1 | `ApiConfigServer::fetch(connectstring, node_type, node_id)` |
| `ic_apic.ic` protocol strings | `proto_strings.rs` | 1:1 subset | |
| `ic_apic_conf_reader.ic`, `ic_apic_grid_conf_reader.ic` (.ini) | — | Out | |
| `ic_apic_conf_writer.ic` | — | Out | |
| `ic_apic_run_cs.ic` (cluster server) | — | Out | |
| `ic_apic_proto_supp.ic` (cluster id in protocol) | — | Out | iClaustron cluster server extension |

## Crate `ic_ndb_signals` — New

Written from [05-ndb-protocol.md](05-ndb-protocol.md), against RonDB 26.10.

| Module | Content |
|---|---|
| `gsn.rs` | `pub const GSN_*: u32` for every signal the API sends or receives |
| `blocks.rs` | block numbers, block reference pack/unpack incl. the instance mapping |
| `header.rs` | Protocol6 header encode/decode; sections; checksum; fragment flags. Ported from `fill_ndb_message_header`/`create_ndb_message` (complete in C) minus the byte swap |
| `sections.rs` | section slices, `AttributeHeader` codec, key alignment rules |
| `qmgr.rs` | `API_REGREQ/CONF/REF`, `NODE_FAILREP`, `NF_COMPLETEREP`, `CONNECT_REP`, node state (ported from `ic_apid_general_signals.h`, re-verified for 26.10) |
| `tc.rs` | `TCKEYREQ/CONF/REF`, `TCINDXREQ/CONF/REF`, `TCSEIZEREQ/CONF/REF`, `TCRELEASEREQ/CONF/REF`, `TC_COMMITREQ/CONF/REF`, `TCROLLBACKREQ/CONF/REF/REP`, `TCKEY_FAILCONF/REF`, `TC_COMMIT_ACK`, `TRANSID_AI`, `TC_DEADLOCK_REP` |
| `dict.rs` | `GET_TABINFOREQ/CONF/REF`, `LIST_TABLES_REQ/CONF`, DictTabInfo keys + defaults, SimpleProperties codec (ported from `ic_apid_dict_signals.h` and `fill_create_table_info_properties`, extended with the 26.10 keys) |
| `interp.rs` | RonDB 26.10 interpreter instruction encoding (full set) |
| `errors.rs` | NDB error code table: code, classification, status, our own message text added progressively (fallback `"NDB error <code>"`) |
| `scan.rs`, `event.rs`, `ddl.rs` | later releases |

All structs are `#[repr(C)]` over `u32` words with `encode`/`decode`
functions; explicit shifts and masks, no bit-field macros.

## Crate `ic_apid` ← `api/ic_apid_*.ic` (13 500 lines, ~30 % implemented)

| C | Rust module | Mode | Notes |
|---|---|---|---|
| `ic_apid_int.h`, `ic_apid_impl.h` | `int_types.rs` | 1:1 | Rust privacy replaces the public/hidden/internal cast tiers; inline accessors become `#[inline]` methods |
| `ic_apid_static.ic`, `ic_apid_common.ic` | `refs.rs` | 1:1 | block reference math (`0x8000 + thread_id`) |
| `ic_apid_error.ic` | `error.rs` | 1:1 | |
| `ic_apid_send_message.ic` | `apid_global.rs` (`NodeShared::send_words`, the send chain), `apid_conn.rs` (`queue_signal`, `send_queued`) | 1:1 | header codec moves to `ic_ndb_signals::header`; the chain is one buffer per node rather than pages |
| `ic_apid_send_thread.ic` | `send_pool.rs`, `connect_thread.rs`, `handshake.rs` | 1:1 + change | one pool thread rather than a thread per node, and it ends the adaptive-send waits; 4-integer hello, TLS-capable auth line; the listen (server-side) thread is Out: API nodes only connect |
| `ic_apid_adaptive_send.ic` | `adaptive_send.rs` | 1:1 | adjusted on the send path once an interval, not from the receive thread |
| `ic_apid_rec_thread.ic` | `rec_thread.rs` | 1:1 | receive + route only; fix the close-down TODO and the `abort()` at disconnect |
| `ic_apid_exec_message.ic` | `exec_message.rs` | 1:1 | runs in the user thread; unknown GSN → log and drop; no endian swap |
| `ic_apid_handle_message_array.ic` | `dispatch.rs` | 1:1 | single protocol version (26.10) |
| `ic_apid_heartbeat.ic` | `heartbeat.rs` | 1:1 + New | add missed-heartbeat death, `NODE_FAILREP`/`NF_COMPLETEREP`, per-thread node-down notices |
| `ic_apid_handle_messages.ic` (stubs) | `tc_handlers.rs`, `node_handlers.rs` | **New** | `TCKEYCONF`, `TCKEYREF`, `TRANSID_AI`, commit/rollback replies, `TCKEY_FAIL*`, `TC_DEADLOCK_REP` |
| `ic_apid_handle_dict_messages.ic` | `dict_client.rs` | New + 1:1 | `GET_TABINFOREQ` send + DictTabInfo parse (new); the DDL path is carried over but parked until the DDL release |
| `ic_apid_global.ic` | `global.rs` | 1:1 | `external_connect` (cluster server hand-off) Out |
| `ic_apid_start.ic` | `program.rs` | 1:1 | option groups via `ic_port::options`; the iClaustron-cluster-server options Out |
| `ic_apid_conn.ic` | `apid_conn.rs` | 1:1 + New | first cut built: inbox, `poll`, fragments joined, expected replies; `read_key`, `write_key`, transaction start/commit/rollback get real bodies; savepoint functions removed |
| `ic_apid_op.ic` | `query.rs` | Redesign | built: record-based binding (04), rows owned by the query, ids from the object map; conditions and assignments still to come |
| `ic_apid_table.ic` | `dict_cache.rs`, `record.rs` | 1:1 + New | binding via the dictionary built in `dict_cache.rs` (`TableDef`, `IndexDef`, the shared cache, invalidation by `ALTER_TABLE_REP`); `Record` from a field spec and the default record built in `record.rs`; index key records still to come |
| `ic_apid_trans.ic` (stubs) | `transaction.rs` | New | built: TC records, ids, start/execute/commit flags, completion accounting, commit and rollback, unique-key queries through `TCINDXREQ`, a lost coordinator link; still to come: `TCKEY_FAILCONF`/`REF`, callbacks |
| `ic_apid_where.ic`, `ic_apid_cond_assign.ic` (stubs) | `where_cond.rs`, `cond_assign.rs`, `interp_gen.rs` | New | builder → RonDB 26.10 interpreter program |
| — | `hash.rs` | New | distribution key, hash choice per table, hash map → partition → node, with the data nodes' dynamic primary rule; location domain and host proximity still to come. The digests themselves are `ic_util::md5` and `ic_util::xxh3`, written from the public algorithms |
| — | `row_codec.rs` | New | `TRANSID_AI` ↔ record row; ATTRINFO/KEYINFO from record rows; first cut built: key, packed read request, packed row unpacked |
| — | `key_op.rs` | New | the first key operation, a committed read waited for; a stepping stone to `transaction.rs` |
| `ic_apid_range.ic`, `ic_apid_tablespace.ic` | `range.rs`, `tablespace.rs` | later releases | |

## Crate `ic_capi` — New

`#[no_mangle] pub extern "C" fn ic_apid_conn_read_key(...)` for every
public method; opaque `typedef struct ic_apid_connection
IC_APID_CONNECTION;`; `#[repr(C)]` enums mirroring `ic_apid_datatypes.h`;
`cbindgen` generates `include/iclaustron/ic_apid.h`. Built as `cdylib`
and `staticlib`; a `pkg-config` file is generated.

## `cpp/include/iclaustron.hpp` — New

Header-only RAII wrappers over the C ABI; no build step, no ABI of its own.

## Out of this project (kept unchanged under `legacy-c/`)

`cluster_server/`, `cluster_mgr/`, `cluster_client/`, `pcntrl/`,
`bootstrap/`, `cfg/`, `fileserver/`, `repserver/`, `scripts/`, the bison
grammars, `ic_apic_run_cs.ic`, the config writer and `.ini` readers.
Nothing in the API scope needs a parser generator.

## Build system

Cargo only. A tiny `CMakeLists.txt` at the top level lets C/C++ consumers
`find_package(iclaustron)` against installed artefacts; it does not build
Rust. `cargo xtask` (an in-tree binary) provides `check`, `style`, `header`
(runs cbindgen and diffs against the checked-in header),
`test-integration` (runs the gated tests against a connectstring),
`tags` (vim tags over Rust and C sources, the successor of
`git_tags.sh`), and later `install` and `bench`.
