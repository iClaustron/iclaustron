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
| 0.1 scope | key lookups (read all lock modes, insert, update, delete, write; unique keys) with commit/rollback, node failure protocols, dictionary, interpreter on key operations, C ABI; scans, SPJ and the rest later |
| Errors | NDB codes and classification kept, our own message texts |
| Hash table | `std::collections::HashMap` |
| Windows | not supported |
| Parsers | no bison; nothing in scope needs a parser generator |
| Tests | designed for the developer to run with `cargo test` and an env var for the cluster |
| Interpreter | the full RonDB 26.10 interpreter may be targeted |
| Option parsing | our own table-driven parser |
| Repository | this repository, branch `RUST-iclaustron`; **all** existing C code moves unchanged to `legacy-c/` and stays there for now as reference; the Rust workspace takes the root; root `LICENSE` becomes MIT |
| BLOBs | skipped for the moment; tables with BLOB columns can be bound, the BLOB columns themselves cannot be read or written (`IC_ERROR_NOT_SUPPORTED`); not scheduled |
| Receive threads | count fixed at connect (`GlobalOptions.receive_threads`) |
| Error texts | progressive: codes and classification first, texts added over time, fallback `"NDB error <code>"` |
| Interpreter spill | decision deferred; 0.1 compiles register-only programs and returns `IC_ERROR_CONDITION_TOO_COMPLEX` when more than 8 values are live |

## Still open

The recommendation is the default and the plan proceeds on it unless
you object.

| # | Question | Recommendation / how to resolve |
|---|---|---|
| Q2 | Does a 26.10 data node accept the legacy `ndbd`/`ndbd passwd` socket auth, or only the `ndbd TLS ...` line? | Test on day one of Phase 3; implement the TLS-capable line first. |
| Q3 | Multi-cluster: the C header carries `cluster_id` on every call (the "grid"). Keep it, or one cluster per `ApidGlobal`? | **Recommendation:** one cluster per `ApidGlobal`; applications needing several clusters create several globals. Removes a parameter from every call and every table in the C ABI. |
| Q4 | Joinable transactions (several user threads on one transaction): drop for good, or keep as a later item? | **Recommendation:** drop from the header; revisit only with a concrete user. |
| Q5 | Which version does the API announce in `get nodeid` and `API_REGREQ`: RonDB 26.10.0 exactly, or the running mgmd's version? | **Recommendation:** announce 26.10.0 (`0x1A0A00`) and our own MySQL version field as the same; confirm the compatibility check on a live node. |
| Q6 | Which non-C/C++ languages get a smoke-test binding in Phase 7? | Suggest Python (`ctypes`) and Java (Panama) as the two that matter to RonDB users; Go and Node if cheap. |
| Q7 | Platforms for 0.1: Linux x86_64 and aarch64, macOS aarch64? | **Recommendation:** all three; RonDB ships on ARM. |
| Q8 | TLS to data nodes and `ndb_mgmd` in 0.1? | **Recommendation:** no; cleartext in 0.1, `rustls` behind a feature in 0.3. |
| Q9 | Should `ApidGlobal::connect` also support an explicit node id from the connectstring (`nodeid=N`) and the `bind-address` option, as `ndb_cluster_connection` does? | **Recommendation:** yes, both are cheap and operators expect them. |
