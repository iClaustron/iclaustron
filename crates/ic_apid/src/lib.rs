// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_apid`: the iClaustron Data API.
//!
//! The library proper: connecting to the data nodes of a RonDB cluster,
//! keeping those connections alive, and running transactions over them.
//!
//! Threading model (doc/rust/02-architecture.md): receive threads only
//! receive signals and route them to the user thread that owns the
//! query; user threads execute them in `poll`. That is the deliberate
//! difference from the C++ NDB API, whose receive thread executes
//! signals itself and then wakes the user thread.
//!
//! Modules:
//! - [`handshake`]: becoming a transporter connection to a data node.
//! - [`signal_reader`]: turning the byte stream back into signals.
//! - [`node_connect`]: one connection to one data node.

pub mod handshake;
pub mod node_connect;
pub mod signal_reader;
