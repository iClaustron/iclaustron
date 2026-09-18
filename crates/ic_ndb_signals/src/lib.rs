// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_ndb_signals`: the NDB protocol as spoken by RonDB 26.10.
//!
//! Global signal numbers, block numbers and references, the Protocol6
//! signal header, sections, and one module per signal family with
//! `encode`/`decode` functions over 32-bit words. Written from
//! doc/rust/05-ndb-protocol.md; every constant carries a verification
//! pointer into the RonDB sources. This crate has no dependencies so it
//! can be fuzzed on its own.
//!
//! Module plan (doc/rust/03-module-map.md): `gsn`, `blocks`, `header`,
//! `sections`, `qmgr`, `tc`, `dict`, `interp`, `errors`; later `scan`,
//! `event`, `ddl`.
//!
//! Phase 0: empty. Modules are added from Phase 3.
