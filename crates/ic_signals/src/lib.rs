// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_signals`: the NDB protocol as RonDB 26.10 speaks it.
//!
//! Signals are how an API node and a data node talk once the connection
//! handshake is done. A signal is a 12-byte header, up to 25 words of
//! data, and up to three sections of any length; what it means is
//! decided by its signal number and the blocks it travels between.
//!
//! This crate is only the wire format: no sockets, no threads, no state.
//! That makes it the piece that can be tested and fuzzed on its own, and
//! it is why it depends on nothing but the portability layer.
//!
//! Modules:
//! - [`blocks`]: block numbers and the references built from them.
//! - [`numbers`]: the signal numbers this library sends and receives.
//! - [`header`]: the signal header and the layout of a message.
//! - [`membership`]: heartbeats and what a data node says about itself.
//! - [`prop_stream`]: the key-value encoding of descriptions.
//! - [`table_props`]: what a table looks like to the dictionary.
//! - [`table_desc`]: asking the dictionary for a table.
//! - [`list_objects`]: asking the dictionary what objects exist.
//! - [`table_change`]: the notice that a table was altered or
//!   dropped.
//! - [`collation_desc`]: asking the dictionary what a collation is.
//! - [`coord_alloc`]: taking and giving back a transaction record.
//! - [`key_ops`]: key operations, their replies, and the commit
//!   acknowledgement.
//! - [`column_header`]: the word in front of each column's value.
//! - [`bundle`]: several small signals sent to an API node as one.

pub mod blocks;
pub mod bundle;
pub mod collation_desc;
pub mod column_header;
pub mod coord_alloc;
pub mod header;
pub mod key_ops;
pub mod list_objects;
pub mod membership;
pub mod numbers;
pub mod prop_stream;
pub mod table_change;
pub mod table_desc;
pub mod table_props;
