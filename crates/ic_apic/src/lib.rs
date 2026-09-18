// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_apic`: configuration client for API nodes.
//!
//! Translates the read side of `legacy-c/api/ic_apic_*.ic`: the
//! `ndb_mgmd` text protocol client (`get nodeid`, `get config_v2`), the
//! version 2 configuration blob decoder, and a small typed configuration
//! for the API node itself, the data nodes and the TCP links. Module plan
//! (doc/rust/03-module-map.md): `mgm_client`, `conf_blob`, `conf_param`,
//! `data`, `api_config_server`, `proto_strings`.
//!
//! Phase 0: empty. Modules are added in Phase 2.
