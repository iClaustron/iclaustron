// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_apic`: where the cluster configuration comes from.
//!
//! An API node knows nothing at startup but a connectstring. It asks a
//! management server for a node id and for the configuration, and from
//! the answer it learns which data nodes exist, how to reach each of
//! them, and the settings that govern its own behaviour. Translates the
//! read side of `legacy-c/api/ic_apic_*.ic`.
//!
//! Modules:
//! - [`conf_param`]: the parameter ids an API node cares about.
//! - [`conf_blob`]: decoding the binary configuration the management
//!   server sends.
//! - [`data`]: the typed configuration the rest of the library uses.
//! - `mgm_client`: the conversation with the management server, still
//!   to be written.

pub mod conf_blob;
pub mod conf_param;
pub mod data;
