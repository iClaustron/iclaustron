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
//! - [`apid_global`]: the object every thread works from, and starting
//!   and stopping the threads.
//! - [`apid_conn`]: a user thread's connection: its inbox, and the
//!   replies it waits for.
//! - [`connect_thread`]: the thread per data node that dials it.
//! - [`rec_thread`]: the receive thread, which reads and routes.
//! - [`heartbeat`]: the heartbeat thread.
//! - [`dict_client`]: fetching a table's description.
//! - [`dict_cache`]: the descriptions every thread shares, and binding.
//! - [`record`]: how a table's fields lie in a row the application owns.
//! - [`row_codec`]: rows to and from the words of a key operation.
//! - [`key_op`]: the first key operations, by primary key.
//! - [`hash`]: where a key belongs, hashed as the data nodes hash it.
//! - [`query`]: the query object, one key operation defined once and
//!   used many times.
//! - [`transaction`]: defining queries on a transaction, sending them,
//!   and completing them as their replies come.
//! - [`text_row`]: values as text, in and out of a row, for the tools.
//! - [`fragments`]: putting a fragmented signal back together.
//! - [`handshake`]: becoming a transporter connection to a data node.
//! - [`signal_reader`]: turning the byte stream back into signals.
//! - [`node_connect`]: one connection to one data node.
//! - [`node_state`]: what every thread may know about a node, lock-free.
//! - [`thread_conn`]: handing signals from a receive thread to the user
//!   thread they are for.

pub mod apid_conn;
pub mod apid_global;
pub mod connect_thread;
pub mod dict_cache;
pub mod dict_client;
pub mod fragments;
pub mod handshake;
pub mod hash;
pub mod heartbeat;
pub mod key_op;
pub mod node_connect;
pub mod node_state;
pub mod query;
pub mod rec_thread;
pub mod record;
pub mod row_codec;
pub mod signal_reader;
pub mod text_row;
pub mod thread_conn;
pub mod transaction;
