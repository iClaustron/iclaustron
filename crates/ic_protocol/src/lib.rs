// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_protocol`: the pieces of the NDB management protocol that are
//! neither sockets nor configuration. Translates
//! `legacy-c/protocol/ic_base64.c` and the management half of
//! `legacy-c/protocol/ic_proto_str.c`.
//!
//! Modules:
//! - [`base64`]: the encoding the configuration blob travels in.
//! - [`proto_str`]: the protocol's vocabulary, as RonDB 26.10 spells it.

pub mod base64;
pub mod proto_str;
