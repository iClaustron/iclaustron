// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_util`: containers, strings, errors, debug tracing, thread pool.
//!
//! Translates `legacy-c/util/*.c`. Module plan
//! (doc/rust/03-module-map.md): `err`, `debug`, `mc` (memory container),
//! `dyn_array`, `ptr_array`, `bitmap`, `hashtable`, `string`,
//! `threadpool`, `connectstring`, `misc`.
//!
//! Phase 0: empty. Modules are added in Phase 1, together with the ports
//! of `legacy-c/test/test_unit.c`.
