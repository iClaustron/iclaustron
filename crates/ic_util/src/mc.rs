// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The memory container (`IC_MEMORY_CONTAINER`, `legacy-c/util/ic_mc.c`):
//! many small allocations grouped into a few large buffers, all released
//! together.
//!
//! It is used where an object graph has one clear lifetime: a parsed
//! configuration, the signals built for one send, the state of one
//! metadata transaction. [`reset`](MemoryContainer::reset) empties the
//! container but keeps one buffer, so the next round of allocation costs
//! nothing.
//!
//! Rust notes for C readers: `mc_alloc` returned a `gchar*` that the
//! caller stored in a struct. Rust cannot hand out a pointer into a
//! buffer it may later move or free, so [`alloc`](MemoryContainer::alloc)
//! returns an [`McHandle`], an index, and [`bytes`](MemoryContainer::bytes)
//! turns the handle back into a slice. A handle is a pointer that the
//! compiler can check: using one after a `reset` cannot corrupt memory,
//! it panics in a debug build and returns an empty slice otherwise.

use ic_port::consts::ic_align;

/// Smallest buffer the container will allocate.
pub const IC_MC_MIN_BASE_SIZE: u32 = 128;
/// Buffer size used when the caller has no preference.
pub const IC_MC_DEFAULT_BASE_SIZE: u32 = 8180;

/// A piece of memory handed out by a [`MemoryContainer`]. Copyable, like
/// the pointer it replaces.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct McHandle {
  buf_index: u32,
  offset: u32,
  len: u32,
}

impl McHandle {
  /// Length in bytes of the piece of memory.
  pub fn len(&self) -> u32 {
    self.len
  }

  /// True if the handle covers no bytes.
  pub fn is_empty(&self) -> bool {
    self.len == 0
  }
}

/// An arena: allocations are cheap, individual frees do not exist, and
/// the whole container is released or reset in one call.
pub struct MemoryContainer {
  buffers: Vec<Vec<u8>>,
  /// Bytes already used in the buffer currently being filled.
  current_used: u32,
  /// Index of the buffer currently being filled.
  current_index: usize,
  base_size: u32,
  max_size: u64,
  total_size: u64,
}

impl MemoryContainer {
  /// A container whose buffers are `base_size` bytes (at least
  /// [`IC_MC_MIN_BASE_SIZE`]). `max_size` of 0 means no limit; otherwise an
  /// allocation that would take the total past it fails.
  ///
  /// The C version took a `use_mutex` flag. A container shared between
  /// threads is wrapped in an `IcMutex` by its owner instead.
  pub fn new(base_size: u32, max_size: u64) -> MemoryContainer {
    let mut size = base_size;
    if size < IC_MC_MIN_BASE_SIZE {
      size = IC_MC_MIN_BASE_SIZE;
    }
    size = ic_align(size as usize, 8) as u32;
    let mut limit = max_size;
    if limit > 0 && limit < size as u64 {
      limit = size as u64;
    }
    MemoryContainer {
      buffers: vec![vec![0u8; size as usize]],
      current_used: 0,
      current_index: 0,
      base_size: size,
      max_size: limit,
      total_size: 0,
    }
  }

  /// A container with the default buffer size and no limit.
  pub fn with_default_size() -> MemoryContainer {
    MemoryContainer::new(IC_MC_DEFAULT_BASE_SIZE, 0)
  }

  /// Reserve `size` bytes, rounded up to a multiple of 8 as the C did.
  /// Returns `None` when `max_size` would be exceeded.
  ///
  /// The bytes are zero; the container never hands out stale data, so
  /// there is no separate `calloc`.
  pub fn alloc(&mut self, size: u32) -> Option<McHandle> {
    let want = ic_align(size as usize, 8) as u32;
    let new_total = self.total_size + want as u64;
    if self.max_size > 0 && new_total > self.max_size {
      return None;
    }
    let current_len = self.buffers[self.current_index].len() as u32;
    if current_len - self.current_used >= want {
      let handle = McHandle {
        buf_index: self.current_index as u32,
        offset: self.current_used,
        len: size,
      };
      self.current_used += want;
      self.total_size = new_total;
      return Some(handle);
    }
    //
    // The rest of the current buffer is given up. A request larger than
    // the base size gets a buffer of its own, and the buffer being filled
    // stays current so its remaining space is not wasted.
    if want > self.base_size {
      self.buffers.push(vec![0u8; want as usize]);
      let handle = McHandle {
        buf_index: (self.buffers.len() - 1) as u32,
        offset: 0,
        len: size,
      };
      self.total_size = new_total;
      return Some(handle);
    }
    self.buffers.push(vec![0u8; self.base_size as usize]);
    self.current_index = self.buffers.len() - 1;
    self.current_used = want;
    self.total_size = new_total;
    Some(McHandle {
      buf_index: self.current_index as u32,
      offset: 0,
      len: size,
    })
  }

  /// Reserve room for `text` and copy it in.
  pub fn alloc_bytes(&mut self, text: &[u8]) -> Option<McHandle> {
    let handle = self.alloc(text.len() as u32)?;
    self.bytes_mut(handle).copy_from_slice(text);
    Some(handle)
  }

  /// The bytes of a handle.
  ///
  /// A handle from a container that has been reset since is not valid:
  /// this panics in a debug build and returns an empty slice otherwise.
  pub fn bytes(&self, handle: McHandle) -> &[u8] {
    if !self.is_valid(handle) {
      ic_port::ic_assert!(false);
      return &[];
    }
    let start = handle.offset as usize;
    let end = start + handle.len as usize;
    &self.buffers[handle.buf_index as usize][start..end]
  }

  /// The bytes of a handle, for writing. See [`bytes`](Self::bytes).
  pub fn bytes_mut(&mut self, handle: McHandle) -> &mut [u8] {
    if !self.is_valid(handle) {
      ic_port::ic_assert!(false);
      return &mut [];
    }
    let start = handle.offset as usize;
    let end = start + handle.len as usize;
    &mut self.buffers[handle.buf_index as usize][start..end]
  }

  fn is_valid(&self, handle: McHandle) -> bool {
    let index = handle.buf_index as usize;
    if index >= self.buffers.len() {
      return false;
    }
    let end = handle.offset as usize + handle.len as usize;
    end <= self.buffers[index].len()
  }

  /// Release everything but one buffer and start again. Handles issued
  /// before the reset must not be used afterwards.
  pub fn reset(&mut self) {
    self.buffers.truncate(1);
    if self.buffers[0].len() != self.base_size as usize {
      self.buffers[0] = vec![0u8; self.base_size as usize];
    } else {
      self.buffers[0].fill(0);
    }
    self.current_index = 0;
    self.current_used = 0;
    self.total_size = 0;
  }

  /// Bytes handed out since the last reset, before rounding.
  pub fn total_size(&self) -> u64 {
    self.total_size
  }

  /// Bytes actually held, including the unused tail of each buffer.
  pub fn allocated_size(&self) -> u64 {
    let mut total: u64 = 0;
    for buf in &self.buffers {
      total += buf.len() as u64;
    }
    total
  }

  /// Number of buffers in use.
  pub fn num_buffers(&self) -> usize {
    self.buffers.len()
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn alloc_and_read_back() {
    let mut mc = MemoryContainer::new(128, 0);
    let a = mc.alloc(10).expect("alloc a");
    let b = mc.alloc(10).expect("alloc b");
    assert_eq!(a.len(), 10);
    assert!(!a.is_empty());
    mc.bytes_mut(a).copy_from_slice(b"0123456789");
    mc.bytes_mut(b).copy_from_slice(b"abcdefghij");
    assert_eq!(mc.bytes(a), b"0123456789");
    assert_eq!(mc.bytes(b), b"abcdefghij");
    /* 10 bytes rounds up to 16, so both fit one 128 byte buffer. */
    assert_eq!(mc.num_buffers(), 1);
    assert_eq!(mc.total_size(), 32);
  }

  #[test]
  fn allocations_are_zeroed() {
    let mut mc = MemoryContainer::new(128, 0);
    let a = mc.alloc(8).expect("alloc");
    assert_eq!(mc.bytes(a), &[0u8; 8]);
    mc.bytes_mut(a).fill(0xFF);
    mc.reset();
    let b = mc.alloc(8).expect("alloc after reset");
    assert_eq!(mc.bytes(b), &[0u8; 8]);
  }

  #[test]
  fn grows_with_more_buffers() {
    let mut mc = MemoryContainer::new(128, 0);
    let mut i = 0;
    while i < 100 {
      mc.alloc(64).expect("alloc");
      i += 1;
    }
    assert!(mc.num_buffers() > 1);
    assert_eq!(mc.total_size(), 100 * 64);
    mc.reset();
    assert_eq!(mc.num_buffers(), 1);
    assert_eq!(mc.total_size(), 0);
  }

  #[test]
  fn large_allocation_gets_its_own_buffer() {
    let mut mc = MemoryContainer::new(128, 0);
    let small = mc.alloc(16).expect("small");
    let large = mc.alloc(4096).expect("large");
    assert_eq!(mc.bytes(large).len(), 4096);
    /* The small buffer is still the current one, so this fits in it. */
    let another = mc.alloc(16).expect("another small");
    assert_eq!(another.buf_index, small.buf_index);
    assert_ne!(large.buf_index, small.buf_index);
  }

  #[test]
  fn max_size_is_honoured() {
    let mut mc = MemoryContainer::new(128, 256);
    assert!(mc.alloc(128).is_some());
    assert!(mc.alloc(128).is_some());
    assert!(mc.alloc(8).is_none());
    mc.reset();
    assert!(mc.alloc(128).is_some());
  }

  #[test]
  fn alloc_bytes_copies() {
    let mut mc = MemoryContainer::with_default_size();
    let h = mc.alloc_bytes(b"localhost:1186").expect("alloc_bytes");
    assert_eq!(mc.bytes(h), b"localhost:1186");
  }

  // The C unit test, test type 1: many random allocations over many
  // containers with resets in between.
  #[test]
  fn many_random_allocations() {
    let mut seed: u32 = 1;
    let mut next = move || {
      seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
      (seed >> 16) & 0x7FFF
    };
    let mut i: u32 = 1;
    while i < 200 {
      let mut mc = MemoryContainer::new(313 * i, 0);
      let num_allocs = next() & 255;
      let mut j = 0;
      while j < 4 {
        let mut k = 0;
        while k < num_allocs {
          let large = (next() & 3) == 1;
          let max = if large { 32767 } else { 511 };
          mc.alloc(next() & max).expect("alloc");
          k += 1;
        }
        mc.reset();
        j += 1;
      }
      i += 1;
    }
  }
}
