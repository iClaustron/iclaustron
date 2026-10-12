# 10 — Open questions

## Decided (2026-09-18)

| Topic | Decision |
|---|---|
| RonDB versions | 26.10 only; no gating for older versions |
| Scope | API node only: read configuration from the management server, never write it; no cluster server, manager, client, process controller, bootstrap, configurator, file server, replication server |
| Name | iClaustron |
| License | MIT |
| Row binding | records over C structs, of the kind the reference API also uses (chapter 04); the 64-bit slot array is not implemented |
| Savepoints | removed from the API |
| Threading | dedicated receive threads that only receive and route; user threads execute signals; user threads never receive |
| 0.1 scope | key lookups (read all lock modes, insert, update, delete, write; unique keys) with commit/rollback, node failure protocols, dictionary, C ABI; scans, pushdown joins and the rest later. Revised 2026-10-08: the interpreter is no longer in 0.1 (see below) |
| Errors | NDB codes and classification kept, our own message texts |
| Hash table | `std::collections::HashMap` |
| Windows | not supported |
| Parsers | no bison; nothing in scope needs a parser generator |
| Tests | designed for the developer to run with `cargo test` and an env var for the cluster |
| Interpreter | the full RonDB 26.10 interpreter may be targeted; revised 2026-10-08: a basic interpreter in 0.3, after scans, the rest later |
| Option parsing | our own table-driven parser |
| Repository | this repository, branch `RUST-iclaustron`; **all** existing C code moves unchanged to `legacy-c/` and stays there for now as reference; the Rust workspace takes the root; root `LICENSE` becomes MIT |
| BLOBs | skipped for the moment; tables with BLOB columns can be bound, the BLOB columns themselves cannot be read or written (`IC_ERROR_NOT_SUPPORTED`); 2026-10-08: deletes on such tables are refused too, since the parts would be left behind; BLOBs are phase 10 |
| Receive threads | count fixed at connect (`GlobalOptions.receive_threads`) |
| Error texts | progressive: codes and classification first, texts added over time, fallback `"NDB error <code>"` |
| Interpreter spill | decision deferred; 0.1 compiles register-only programs and returns `IC_ERROR_CONDITION_TOO_COMPLEX` when more than 8 values are live |

## Decided (2026-10-08)

| Topic | Decision |
|---|---|
| Names | nothing is taken from RonDB, names included; whatever travels on the wire stays identical. Signals, blocks, configuration parameters, fields, keys and modules get names of our own (new stems; `_REQ`/`_CONF`/`_REF`/`_REP`, and `_ORD` or `_REP` where there would be no suffix). RonDB names appear nowhere in code or docs; chapter 05 cites by bare file:line and by signal number (chapter 09) |
| Order of work | phase 6: our own names, then tables with BLOB columns, the 4012 timeout, unique keys, every row size; phase 7: C API, documentation, examples in Rust and C, release 0.1; phase 8: the executor, scans and latin1 (moved from phase 6 the same day); phase 9: basic interpreter and AUTO_INCREMENT (chapter 06) |
| Character sets | needed for hashing keys and for merging ordered scans. Revised the same day: encodings, weights and orderings are retrieved from the data nodes through a dictionary request RonDB 26.04 main adds in a separate task, and kept in the global dictionary (chapter 06 §8.6); public standards may be compiled in. latin1 is tested first; until a character set is described, its keys are placed round robin |
| Protocol method | protocols are developed mainly by listening to the network, read with a Wireshark dissector of our own (chapter 06 §6.7); the author answers what the wire does not show; the RonDB source is read only to settle the rest (chapter 09) |
| Wireshark dissector (was Q14) | not in this repository: written in C in a Wireshark source tree of its own, under Wireshark's licence, built with Wireshark (chapter 06 §6.7) |
| Collation weights (was Q15) | `COLLATION_DESC_REQ` (984–986) with the weights flag returns every character's and every contraction's weight string, in as many round trips as the collation needs (chapter 05 §8.2); RonDB 26.04 main, the author's task, 2026-10-08 |
| Scans | synchronous and asynchronous, and fitting the executor model; the executor is built first in phase 8 |
| TLS (was Q8) | not in phases 6–9 |
| Location domains | not in phases 6–9 |
| C++ header, other language bindings | moved from phase 7 to phase 10 |
| AUTO_INCREMENT | phase 9, with tables without a primary key, after the interpreter and scans it uses |

## Decided (2026-10-09)

| Topic | Decision |
|---|---|
| `LIST_OBJECTS_CONF` (was Q17) | Answered by the author: two words of its own, then the fragment trailer even when sent whole (none, a fragment id, or the number of each carried section and a fragment id: 2 to 5 words, 0 to 2 sections); a long answer goes in 240-word pieces with the fragment bits set, which `ic_apid::fragments` joins. Nothing to change in the client (chapter 05 §2.1). |

## Decided (2026-10-12)

| Topic | Decision |
|---|---|
| The hash of a key in a no-pad collation (was Q16) | Described by the author and seen live: 64-bit FNV-1a over the collation's 16-bit weights, level by level with a zero weight between levels and zero weights within a level skipped, the first three levels always and a fourth only where the string has a weight there; utf8mb4_0900_bin folds its bytes. The result's eight bytes are the column's part of the hashed key (chapter 05 §6.3). Every collation in the `charset` group places keys as the data nodes do; the neighbour-weighted ones are placed in turn. |

## Still open

The recommendation is the default and the plan proceeds on it unless
you object.

| # | Question | Recommendation / how to resolve |
|---|---|---|
| Q2 | Does a 26.10 data node accept the legacy `ndbd`/`ndbd passwd` socket auth, or only the `ndbd TLS ...` line? | Test on day one of Phase 3; implement the TLS-capable line first. |
| Q3 | Multi-cluster: the C header carries `cluster_id` on every call (the "grid"). Keep it, or one cluster per `ApidGlobal`? | **Recommendation:** one cluster per `ApidGlobal`; applications needing several clusters create several globals. Removes a parameter from every call and every table in the C ABI. |
| Q4 | Joinable transactions (several user threads on one transaction): drop for good, or keep as a later item? | **Recommendation:** drop from the header; revisit only with a concrete user. |
| Q5 | Which version does the API announce in `get nodeid` and `HEARTBEAT_REQ`: RonDB 26.10.0 exactly, or the running management server's version? | **Recommendation:** announce 26.10.0 (`0x1A0A00`) and our own MySQL version field as the same; confirm the compatibility check on a live node. |
| Q6 | Which non-C/C++ languages get a smoke-test binding (phase 10 since 2026-10-08, was phase 7)? | Suggest Python (`ctypes`) and Java (Panama) as the two that matter to RonDB users; Go and Node if cheap. |
| Q7 | Platforms for 0.1: Linux x86_64 and aarch64, macOS aarch64? | **Recommendation:** all three; RonDB ships on ARM. |
| Q9 | Should `ApidGlobal::connect` also support an explicit node id from the connectstring (`nodeid=N`) and the `bind-address` option, as the reference API's connection object does? | **Recommendation:** yes, both are cheap and operators expect them. |
| Q10 | Rows in the C API: the Rust query owns its rows; chapter 04 said C rows are the caller's memory. | **Recommendation:** C gets pointers to the query's own rows (`ic_apid_query_key_row`, `ic_apid_query_attr_row`), valid until the query is freed: no copy, nothing pointing into the caller's memory. Decide at the start of phase 7. |
| Q11 | The names of signals and blocks. | The first draft is in chapter 06 §6.1; the author reviews it before the rename. |
| Q12 | Writes through a unique index. | **Recommendation:** refuse an update through an index whose attribute record holds an indexed column, and keep refusing a write through an index (chapter 06 §6.5, item 8). |
| Q13 | AUTO_INCREMENT prefetch: how many values the client reserves at a time. | **Recommendation:** 512 by default, as MySQL servers use, settable per table binding; decide in phase 9. |
