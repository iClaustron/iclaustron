# 10 — Open questions

## Decided (2026-09-18)

| Topic | Decision |
|---|---|
| RonDB versions | 26.10 only; no gating for older versions |
| Scope | API node only: read configuration from `ndb_mgmd`, never write it; no cluster server, manager, client, process controller, bootstrap, configurator, file server, replication server |
| Name | iClaustron |
| License | MIT |
| Row binding | NdbRecord-like records over C structs (chapter 04); the 64-bit slot array is not implemented |
| Savepoints | removed from the API |
| Threading | dedicated receive threads that only receive and route; user threads execute signals; user threads never receive |
| 0.1 scope | key lookups (read all lock modes, insert, update, delete, write; unique keys) with commit/rollback, node failure protocols, dictionary, C ABI; scans, SPJ and the rest later. Revised 2026-10-08: the interpreter is no longer in 0.1 (see below) |
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
| Character sets | latin1 only in these phases, UTF-8 perhaps later; needed for hashing keys and for merging ordered scans. The author writes the latin1 collation module himself (chapter 06 §8.6, before ordered range scans); other character sets keep falling back to round-robin placement |
| Scans | synchronous and asynchronous, and fitting the executor model; the executor is built first in phase 8 |
| TLS (was Q8) | not in phases 6–9 |
| Location domains | not in phases 6–9 |
| C++ header, other language bindings | moved from phase 7 to phase 10 |
| AUTO_INCREMENT | phase 9, with tables without a primary key, after the interpreter and scans it uses |

## Still open

The recommendation is the default and the plan proceeds on it unless
you object.

| # | Question | Recommendation / how to resolve |
|---|---|---|
| Q2 | Does a 26.10 data node accept the legacy `ndbd`/`ndbd passwd` socket auth, or only the `ndbd TLS ...` line? | Test on day one of Phase 3; implement the TLS-capable line first. |
| Q3 | Multi-cluster: the C header carries `cluster_id` on every call (the "grid"). Keep it, or one cluster per `ApidGlobal`? | **Recommendation:** one cluster per `ApidGlobal`; applications needing several clusters create several globals. Removes a parameter from every call and every table in the C ABI. |
| Q4 | Joinable transactions (several user threads on one transaction): drop for good, or keep as a later item? | **Recommendation:** drop from the header; revisit only with a concrete user. |
| Q5 | Which version does the API announce in `get nodeid` and `API_REGREQ`: RonDB 26.10.0 exactly, or the running mgmd's version? | **Recommendation:** announce 26.10.0 (`0x1A0A00`) and our own MySQL version field as the same; confirm the compatibility check on a live node. |
| Q6 | Which non-C/C++ languages get a smoke-test binding (phase 10 since 2026-10-08, was phase 7)? | Suggest Python (`ctypes`) and Java (Panama) as the two that matter to RonDB users; Go and Node if cheap. |
| Q7 | Platforms for 0.1: Linux x86_64 and aarch64, macOS aarch64? | **Recommendation:** all three; RonDB ships on ARM. |
| Q9 | Should `ApidGlobal::connect` also support an explicit node id from the connectstring (`nodeid=N`) and the `bind-address` option, as `ndb_cluster_connection` does? | **Recommendation:** yes, both are cheap and operators expect them. |
| Q10 | Rows in the C API: the Rust query owns its rows; chapter 04 said C rows are the caller's memory. | **Recommendation:** C gets pointers to the query's own rows (`ic_apid_query_key_row`, `ic_apid_query_attr_row`), valid until the query is freed: no copy, nothing pointing into the caller's memory. Decide at the start of phase 7. |
| Q11 | The names of signals and blocks. | The first draft is in chapter 06 §6.1; the author reviews it before the rename. |
| Q12 | Writes through a unique index. | **Recommendation:** refuse an update through an index whose attribute record holds an indexed column, and keep refusing a write through an index (chapter 06 §6.5, item 8). |
| Q13 | AUTO_INCREMENT prefetch: how many values the client reserves at a time. | **Recommendation:** 512 by default, as MySQL servers use, settable per table binding; decide in phase 9. |
