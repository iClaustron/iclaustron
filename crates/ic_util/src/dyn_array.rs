// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! A growable byte buffer with positioned reads and writes
//! (`IC_DYNAMIC_ARRAY`, `legacy-c/util/ic_dyn_array.c`). Used to collect
//! a configuration blob or a protocol reply whose length is not known in
//! advance.
//!
//! The C version chained fixed 1 kByte buffers so that earlier pointers
//! stayed valid, and had a second "ordered" variant with an index tree
//! for spilling to disk. Neither is needed here: this is one `Vec<u8>`,
//! and the disk variant served only the configuration writer, which is
//! out of scope.

use ic_port::err;
use ic_port::IcError;

/// A growable byte buffer.
#[derive(Clone, Debug, Default)]
pub struct DynArray {
  buf: Vec<u8>,
}

impl DynArray {
  /// An empty buffer.
  pub fn new() -> DynArray {
    DynArray { buf: Vec::new() }
  }

  /// An empty buffer with room for `capacity` bytes reserved.
  pub fn with_capacity(capacity: usize) -> DynArray {
    DynArray {
      buf: Vec::with_capacity(capacity),
    }
  }

  /// Append bytes to the end (`ic_insert_dynamic_array`).
  pub fn insert(&mut self, bytes: &[u8]) {
    self.buf.extend_from_slice(bytes);
  }

  /// Overwrite `bytes.len()` bytes at `position`, which must lie within
  /// the current contents (`ic_write_dynamic_array`).
  pub fn write_at(
    &mut self,
    position: u64,
    bytes: &[u8],
  ) -> Result<(), IcError> {
    let start = position as usize;
    let end = start + bytes.len();
    if end > self.buf.len() {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    self.buf[start..end].copy_from_slice(bytes);
    Ok(())
  }

  /// Copy out `out.len()` bytes from `position`
  /// (`ic_read_dynamic_array`).
  pub fn read_at(&self, position: u64, out: &mut [u8]) -> Result<(), IcError> {
    let start = position as usize;
    let end = start + out.len();
    if end > self.buf.len() {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    out.copy_from_slice(&self.buf[start..end]);
    Ok(())
  }

  /// Number of bytes held (`ic_get_current_size`).
  pub fn size(&self) -> u64 {
    self.buf.len() as u64
  }

  /// True if nothing has been inserted.
  pub fn is_empty(&self) -> bool {
    self.buf.is_empty()
  }

  /// All the bytes.
  pub fn as_bytes(&self) -> &[u8] {
    &self.buf
  }

  /// All the bytes, for writing.
  pub fn as_bytes_mut(&mut self) -> &mut [u8] {
    &mut self.buf
  }

  /// Forget the contents, keeping the memory for the next round.
  pub fn reset(&mut self) {
    self.buf.clear();
  }

  /// Hand the bytes over to the caller, emptying the array.
  pub fn take(&mut self) -> Vec<u8> {
    std::mem::take(&mut self.buf)
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn insert_read_write() {
    let mut a = DynArray::new();
    assert!(a.is_empty());
    a.insert(b"hello ");
    a.insert(b"world");
    assert_eq!(a.size(), 11);
    assert_eq!(a.as_bytes(), b"hello world");
    let mut out = [0u8; 5];
    a.read_at(6, &mut out).expect("read");
    assert_eq!(&out, b"world");
    a.write_at(0, b"HELLO").expect("write");
    assert_eq!(a.as_bytes(), b"HELLO world");
  }

  #[test]
  fn out_of_range_is_an_error() {
    let mut a = DynArray::new();
    a.insert(b"12345");
    let mut out = [0u8; 3];
    assert!(a.read_at(3, &mut out).is_err());
    assert!(a.write_at(3, b"abc").is_err());
    assert!(a.read_at(2, &mut out).is_ok());
  }

  // The C unit test, test type 2: insert many blocks and read them all
  // back at their recorded positions.
  #[test]
  fn many_inserts_read_back() {
    let mut a = DynArray::with_capacity(1024);
    let mut positions: Vec<(u64, u8, usize)> = Vec::new();
    let mut i: u32 = 0;
    while i < 500 {
      let len = ((i % 37) + 1) as usize;
      let value = (i & 0xFF) as u8;
      let block = vec![value; len];
      positions.push((a.size(), value, len));
      a.insert(&block);
      i += 1;
    }
    for (position, value, len) in &positions {
      let mut out = vec![0u8; *len];
      a.read_at(*position, &mut out).expect("read");
      for byte in &out {
        assert_eq!(*byte, *value);
      }
    }
    let total = a.size();
    let taken = a.take();
    assert_eq!(taken.len() as u64, total);
    assert!(a.is_empty());
  }
}
