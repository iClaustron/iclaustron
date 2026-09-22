// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

#![cfg_attr(test, allow(clippy::expect_used, clippy::unwrap_used))]

//! `ic_port`: the portability layer.
//!
//! Translates `legacy-c/port/ic_port.c`, `legacy-c/include/ic_port.h`,
//! the constants of `legacy-c/include/ic_base_header.h`, and, because the
//! C port layer used them and Rust crates cannot depend upwards, also
//! `legacy-c/util/ic_err.c` and `legacy-c/util/ic_debug.c`. Everything
//! that touches the operating system or that glib used to provide lives
//! here and nowhere else.
//!
//! Modules:
//! - [`consts`]: sizes, limits, ports, NDB block numbers, versions.
//! - [`err`]: error codes 7000.., [`IcError`], `ic_assert!`, `ic_require!`.
//! - [`ndb_err`]: the codes the data nodes report, their kinds, and our
//!   own words for them.
//! - [`stop`]: the process-wide stop flag.
//! - [`oserr`]: last OS error and its text.
//! - [`endian`]: byte order detection and word swapping.
//! - [`time`]: nanosecond timers and sleeps.
//! - [`output`]: `ic_printf!` and redirection of standard output.
//! - [`debug`]: the `DEBUG_ENTRY`/`DEBUG_PRINT` tracing facility.
//! - [`sync`]: mutex and condition variable with lock ordering levels.
//! - [`file`]: file helpers.
//! - [`socket`]: socket helpers below `ic_comm`.
//! - [`daemon`]: daemonising, pid files, signal handlers.
//! - [`options`]: table driven command line parsing.

pub mod consts;
pub mod daemon;
pub mod debug;
pub mod endian;
pub mod err;
pub mod file;
pub mod ndb_err;
pub mod options;
pub mod oserr;
pub mod output;
pub mod socket;
pub mod stop;
pub mod sync;
pub mod time;

pub use err::IcError;
