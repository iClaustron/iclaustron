// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_util`: the containers and helpers the rest of the library builds
//! on. Translates `legacy-c/util/*.c` apart from the error codes and the
//! debug tracer, which had to move down into `ic_port` because the C port
//! layer used them.
//!
//! Modules:
//! - [`mc`]: the memory container, an arena freed in one go.
//! - [`bitmap`]: a bitmap of fixed width.
//! - [`dyn_array`]: a growable byte buffer with positioned reads and
//!   writes.
//! - [`ptr_array`]: the object map, turning objects into the 32-bit ids
//!   that travel in NDB signals and back.
//! - [`connectstring`]: parsing `host:port,host:port` and the NDB
//!   `nodeid=` and `bind-address=` options.
//! - [`string`]: the few string helpers with no direct equivalent in the
//!   Rust standard library.

pub mod bitmap;
pub mod connectstring;
pub mod dyn_array;
pub mod mc;
pub mod ptr_array;
pub mod string;
