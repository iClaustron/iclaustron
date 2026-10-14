// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The shared library's name as a program linked with it records it:
//! `libiclaustron.so.0` on Linux, the major version in the soname, and
//! `@rpath/libiclaustron.dylib` on macOS, which `cargo xtask install`
//! turns into the installed path.

fn main() {
  let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
  if os == "linux" {
    println!("cargo:rustc-cdylib-link-arg=-Wl,-soname,libiclaustron.so.0");
  } else if os == "macos" {
    println!(concat!(
      "cargo:rustc-cdylib-link-arg=-Wl,-install_name,",
      "@rpath/libiclaustron.dylib"
    ));
  }
  println!("cargo:rerun-if-changed=build.rs");
}
