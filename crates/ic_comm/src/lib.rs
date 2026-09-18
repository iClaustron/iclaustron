// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_comm`: everything between a socket and the protocol layers above
//! it. Translates `legacy-c/comm/*.c`.
//!
//! Modules:
//! - [`sock_buf`]: the pool of buffer pages the receive path reads into.
//! - [`poll_set`]: waiting on many sockets at once, over `epoll` on
//!   Linux and `kqueue` on macOS.
//! - [`connection`]: one TCP connection, with the options and the
//!   statistics the C `IC_CONNECTION` carried.
//! - [`line_proto`]: the line-oriented management protocol helpers.

pub mod connection;
pub mod line_proto;
pub mod poll_set;
pub mod sock_buf;
