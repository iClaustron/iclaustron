// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_comm`: sockets, poll sets, socket buffer pages, line protocol.
//!
//! Translates `legacy-c/comm/*.c`. Module plan
//! (doc/rust/03-module-map.md): `connection`, `poll_set` with the
//! `poll_epoll`, `poll_kqueue` and `poll_posix` backends, `sock_buf`,
//! `line_proto`; `tls` comes in a later release.
//!
//! Phase 0: empty. Modules are added in Phase 1.
