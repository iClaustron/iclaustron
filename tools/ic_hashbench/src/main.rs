// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_hashbench`: how long a key's hash takes, XXH3 and MD5 as
//! `ic_util` computes them, over the key sizes RonDB hashes: 4, 8, 16
//! and 32 bytes, and longer ones up to where XXH3 runs its accumulator
//! loop (doc/rust/06-phases.md, phase 7).
//!
//! `xxh3_bench.c` beside this crate does the same with the xxHash the
//! data nodes are built with, the same keys in the same order, so the
//! two can be set side by side; both print an XOR of every hash, which
//! must agree, or our XXH3 is not the data nodes'. Measure release
//! builds only:
//!
//! ```text
//! cargo run --release -p ic_hashbench
//! cc -O3 -I$RONDB/extra/xxhash/xxHash-0.8.3 \
//!   -o target/xxh3_bench tools/ic_hashbench/xxh3_bench.c
//! target/xxh3_bench
//! ```
//!
//! On x86-64 add `-mavx2` to the C, as the data nodes use AVX2 when the
//! CPU has it.

use std::time::Instant;

use ic_util::md5;
use ic_util::xxh3;

/// The key sizes, in bytes.
const SIZES: [usize; 8] = [4, 8, 16, 32, 64, 128, 240, 1024];

/// How many hashes are timed for a key of `len` bytes.
fn iterations(len: usize) -> u64 {
  if len <= 64 {
    20_000_000
  } else if len <= 240 {
    5_000_000
  } else {
    1_000_000
  }
}

/// The key: byte i is i times 7 plus 1.
fn key(len: usize) -> Vec<u8> {
  let mut key = vec![0u8; len];
  let mut i: usize = 0;
  while i < len {
    key[i] = (i as u8).wrapping_mul(7).wrapping_add(1);
    i += 1;
  }
  key
}

/// Each hash's key differs: its first bytes, up to four, are the
/// iteration's number, little-endian.
fn stamp(key: &mut [u8], i: u64) {
  let bytes = (i as u32).to_le_bytes();
  let n = key.len().min(4);
  key[..n].copy_from_slice(&bytes[..n]);
}

/// Nanoseconds per XXH3 hash of a key of `len` bytes, and the XOR of
/// every hash.
fn time_xxh3(len: usize) -> (f64, u64) {
  let mut bytes = key(len);
  let n = iterations(len);
  let mut sink: u64 = 0;
  let start = Instant::now();
  let mut i: u64 = 0;
  while i < n {
    stamp(&mut bytes, i);
    sink ^= xxh3::hash(&bytes);
    i += 1;
  }
  let nanos = start.elapsed().as_nanos() as f64;
  (nanos / n as f64, sink)
}

/// Nanoseconds per MD5 digest of a key of `len` bytes.
fn time_md5(len: usize) -> (f64, u32) {
  let mut bytes = key(len);
  let n = iterations(len) / 4;
  let mut sink: u32 = 0;
  let start = Instant::now();
  let mut i: u64 = 0;
  while i < n {
    stamp(&mut bytes, i);
    sink ^= md5::digest(&bytes)[1];
    i += 1;
  }
  let nanos = start.elapsed().as_nanos() as f64;
  (nanos / n as f64, sink)
}

fn main() {
  println!(
    "{:>6} {:>9} {:>18} {:>9}",
    "bytes", "XXH3 ns", "XXH3 check", "MD5 ns"
  );
  for len in SIZES {
    let (xxh3_ns, check) = time_xxh3(len);
    let (md5_ns, _) = time_md5(len);
    println!(
      "{:>6} {:>9.2} {:>18} {:>9.2}",
      len,
      xxh3_ns,
      format!("{:016x}", check),
      md5_ns
    );
  }
}
