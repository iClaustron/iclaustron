// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_apid`: the iClaustron Data API.
//!
//! The library proper: connecting to the data nodes of a RonDB cluster,
//! keeping those connections alive, and running transactions over them.
//!
//! # Using it
//!
//! Start the library with
//! [`ApidGlobal::connect`](apid_global::ApidGlobal::connect), make a
//! connection per thread, bind a table, describe its rows with a
//! [`Record`](record::Record), and read and write rows by key: one at a
//! time with the synchronous calls of [`key_op`], or many at once with
//! queries defined on transactions, sent together and completed by
//! [`poll`](apid_conn::ApidConnection::poll), each through the executed
//! list or a callback ([`transaction`], [`query`]). Threads that have no
//! connection of their own hand their key operations to an
//! [executor](executor), which runs them together with every other
//! thread's.
//!
//! ```no_run
//! use ic_apid::apid_global::ApidGlobal;
//! use ic_apid::apid_global::GlobalOptions;
//! use ic_apid::key_op;
//! use ic_apid::record::Record;
//! use ic_port::IcError;
//!
//! fn main() -> Result<(), IcError> {
//!   let global = ApidGlobal::connect(
//!     "localhost:1186",
//!     Some("my_app"),
//!     &GlobalOptions::default(),
//!   )?;
//!   global.wait_for_all_started(30_000);
//!   let mut conn = global.create_connection()?;
//!   let table = conn.table_bind("ic_ex", "t1")?;
//!   let rec = Record::default_for(&table, None)?;
//!   // The key goes into its field, at the offset the record gives.
//!   let key = vec![0u8; rec.row_size() as usize];
//!   let mut row = vec![0u8; rec.row_size() as usize];
//!   let found =
//!     key_op::read_committed(&mut conn, &rec, &key, &rec, &mut row)?;
//!   println!("found: {}", found);
//!   Ok(())
//! }
//! ```
//!
//! The user guide, `doc/user-guide.md`, explains records, queries,
//! transactions, callbacks, errors and when to try again, and node
//! failure; `examples/rust` has five whole programs.
//!
//! Threading model (doc/rust/02-architecture.md): receive threads only
//! receive signals and route them to the user thread that owns the
//! query; user threads execute them in `poll`. That is the deliberate
//! difference from the reference API, whose receive thread executes
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
//! - [`collation_client`]: what a collation is, and its weights.
//! - [`collation_map`]: a collation as placing a key with a character
//!   column uses it.
//! - [`record`]: how a table's fields lie in a row the application owns.
//! - [`row_codec`]: rows to and from the words of a key operation.
//! - [`key_op`]: the first key operations, by primary key.
//! - [`hash`]: where a key belongs, hashed as the data nodes hash it.
//! - [`query`]: the query object, one key operation defined once and
//!   used many times.
//! - [`transaction`]: defining queries on a transaction, sending them,
//!   and completing them as their replies come.
//! - [`scan`]: scans, a table read fragment by fragment in batches,
//!   on a connection.
//! - [`executor`]: a user thread that runs key operations and scans for
//!   any number of application threads, and the blocking calls over it.
//! - [`scan_reader`]: an application thread's end of a scan an
//!   executor runs.
//! - [`index_order`]: rows in an ordered index's order, for the merge
//!   of an ordered range scan.
//! - [`index_scan`]: the ranges of a scan of an ordered index.
//! - [`program`]: conditions and assignments the data node runs on a
//!   row, built as a tree and compiled to its interpreter's words.
//! - [`text_row`]: values as text, in and out of a row, for the tools.
//! - [`fragments`]: putting a fragmented signal back together.
//! - [`handshake`]: becoming a transporter connection to a data node.
//! - [`signal_reader`]: turning the byte stream back into signals.
//! - [`node_connect`]: one connection to one data node.
//! - [`node_state`]: what every thread may know about a node, lock-free.
//! - [`thread_conn`]: handing signals from a receive thread to the user
//!   thread they are for, and requests from application threads to an
//!   executor.

pub mod adaptive_send;
pub mod apid_conn;
pub mod apid_global;
pub mod auto_increment;
pub mod collation_client;
pub mod collation_map;
pub mod connect_thread;
pub mod dict_cache;
pub mod dict_client;
pub mod executor;
pub mod fragments;
pub mod handshake;
pub mod hash;
pub mod heartbeat;
pub mod index_order;
pub mod index_scan;
pub mod key_op;
pub mod node_connect;
pub mod node_state;
pub mod program;
pub mod query;
pub mod rec_thread;
pub mod record;
pub mod row_codec;
pub mod scan;
pub mod scan_reader;
pub mod send_pool;
pub mod signal_page;
pub mod signal_reader;
pub mod text_row;
pub mod thread_conn;
pub mod transaction;
