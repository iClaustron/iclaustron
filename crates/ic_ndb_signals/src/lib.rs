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
//! - [`simple_properties`]: the key-value encoding of descriptions.
//! - [`dict_tab_info`]: what a table looks like to the dictionary.
//! - [`get_tab_info`]: asking the dictionary for a table.
//! - [`list_tables`]: asking the dictionary what objects exist.
//! - [`alter_table_rep`]: the notice that a table was altered or
//!   dropped.
//! - [`tc_seize`]: taking and giving back a transaction record.

pub mod alter_table_rep;
pub mod blocks;
pub mod dict_tab_info;
pub mod get_tab_info;
pub mod gsn;
pub mod header;
pub mod list_tables;
pub mod qmgr;
pub mod simple_properties;
pub mod tc_seize;
