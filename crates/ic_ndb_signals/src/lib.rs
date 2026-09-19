// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_ndb_signals`: the NDB protocol as RonDB 26.10 speaks it.
//!
//! Signals are how an API node and a data node talk once the transporter
//! handshake is done. A signal is a 12-byte header, up to 25 words of
//! data, and up to three sections of any length; what it means is
//! decided by its global signal number and the blocks it travels
//! between.
//!
//! This crate is only the wire format: no sockets, no threads, no state.
//! That makes it the piece that can be tested and fuzzed on its own, and
//! it is why it depends on nothing but the portability layer.
//!
//! Modules:
//! - [`blocks`]: block numbers and the references built from them.
//! - [`gsn`]: the signal numbers this library sends and receives.
//! - [`header`]: the signal header and the layout of a message.
//! - [`qmgr`]: heartbeats and what a data node says about itself.
//! - [`tc_seize`]: taking and giving back a transaction record.

pub mod blocks;
pub mod gsn;
pub mod header;
pub mod qmgr;
pub mod tc_seize;
