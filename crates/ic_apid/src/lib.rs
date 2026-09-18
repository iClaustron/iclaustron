// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_apid`: the iClaustron Data API.
//!
//! The library proper. Translates the transport substrate of
//! `legacy-c/api/ic_apid_*.ic` (send threads, receive threads, heartbeat,
//! adaptive send) and implements the data path (dictionary, records,
//! transactions, key operations, interpreter programs) from
//! doc/rust/04-api-design.md and doc/rust/05-ndb-protocol.md.
//!
//! Threading model (doc/rust/02-architecture.md): receive threads only
//! receive and route signals to the owning user thread; user threads
//! execute them in `poll`.
//!
//! Phase 0: empty. Modules are added from Phase 3.
