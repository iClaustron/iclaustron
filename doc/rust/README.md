# Plan: iClaustron, a Rust NDB API

A months-long project to build an MIT-licensed client library for RonDB
26.10 data nodes in C-like Rust, following the iClaustron module structure
and the iClaustron Data API design, with a C ABI for C, C++ and every
language that can call C. The library is an API node only; the
management side of iClaustron is out of scope.

Read in order the first time; afterwards each chapter stands alone.

| Chapter | What it answers |
|---|---|
| [01 Goals and principles](01-goals-and-principles.md) | Scope of 0.1, what is out, the eleven rules, success criteria |
| [02 Architecture](02-architecture.md) | Crate layering, runtime objects, the fixed threading model, data flows, mutex levels |
| [03 Module map](03-module-map.md) | Every surviving C file → Rust module with mode (1:1 / redesign / new / out) |
| [04 API design](04-api-design.md) | Rust, C and C++ surfaces; record-style rows; transactions; queries; what changes in `ic_apid.h` |
| [05 NDB protocol](05-ndb-protocol.md) | Clean-room protocol spec for 26.10 with verification pointers; delta from the 7.2.9 baseline |
| [06 Phases](06-phases.md) | Eleven phases with exit criteria, proof commands, estimates (0.1 in about 6 months) |
| [07 Style guide](07-style-guide.md) | Allowed and forbidden Rust; naming; memory; errors |
| [08 Testing and tooling](08-testing-and-tooling.md) | Toolchain, developer-run test pyramid, captured vectors, integration cluster, benchmark |
| [09 Licensing](09-licensing.md) | Relicensing to MIT, clean room against RonDB, dependency policy |
| [10 Open questions](10-open-questions.md) | Decisions taken and the thirteen still open |
| [11 C to Rust mappings](11-c-to-rust-mappings.md) | Construct-by-construct translation table with explanations and worked examples |
| [12 Performance model](12-performance-model.md) | Operational laws and the `ic_model` simulation for what-if analysis of latency and throughput; its calibration |

## State of the C code this plan starts from

- Configuration client (`ic_apic` read side): complete but written for NDB
  7.2.9 configuration; the mgm client translates 1:1, the parameter table
  is rebuilt as a small API-node subset for 26.10.
- Data API transport substrate (send/receive/heartbeat threads, adaptive
  send, handshake, wire framing): complete; translates 1:1 with the
  26.10 handshake changes.
- Data API data path (transactions, key operations, interpreter): fully
  designed in `include/ic_apid.h`, implemented as stubs; the key signal
  header is empty. Built new from chapters 04 and 05.
- Dictionary: reading table definitions was never implemented; built new.
- glib: about 50 functions behind `ic_port.h`; removed by the `ic_port` crate.
- Everything management-side (cluster server, manager, client, process
  controller, bootstrap, file server, replication server, config files):
  not translated.

## Reference trees

- iClaustron C code: this repository, branch `RUST-iclaustron`, to be
  moved unchanged to `legacy-c/` in Phase 0.
- RonDB 26.10: `/Users/mikael/mysql_trees/rondb_2604_main/storage/ndb`. Read for protocol facts; never copy.
