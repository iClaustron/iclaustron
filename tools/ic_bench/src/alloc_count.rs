// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! How many allocations a run makes, with the feature `count-allocs`:
//! a global allocator that counts each allocation, reallocation and the
//! bytes asked for, and hands the work to the system's. Without the
//! feature nothing is counted and nothing is in the way.
//!
//! The counters are shared atomics, so with several threads the counting
//! itself costs a little: read the counts from such a run, and the rates
//! from one without the feature.

/// Allocations and bytes so far, if they are counted.
#[cfg(feature = "count-allocs")]
pub fn counts() -> Option<(u64, u64)> {
  Some(counting::counts())
}

/// Allocations and bytes so far, if they are counted.
#[cfg(not(feature = "count-allocs"))]
pub fn counts() -> Option<(u64, u64)> {
  None
}

#[cfg(feature = "count-allocs")]
mod counting {
  use std::alloc::GlobalAlloc;
  use std::alloc::Layout;
  use std::alloc::System;
  use std::sync::atomic::AtomicU64;
  use std::sync::atomic::Ordering;

  static ALLOCATIONS: AtomicU64 = AtomicU64::new(0);
  static BYTES: AtomicU64 = AtomicU64::new(0);

  /// The system's allocator, counted.
  struct Counting;

  #[global_allocator]
  static COUNTING: Counting = Counting;

  fn note(bytes: usize) {
    ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
    BYTES.fetch_add(bytes as u64, Ordering::Relaxed);
  }

  pub fn counts() -> (u64, u64) {
    (
      ALLOCATIONS.load(Ordering::Relaxed),
      BYTES.load(Ordering::Relaxed),
    )
  }

  // SAFETY: every call is handed to the system's allocator unchanged;
  // counting touches nothing the allocator relies on.
  unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
      note(layout.size());
      // SAFETY: the caller's contract, passed on.
      unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
      note(layout.size());
      // SAFETY: the caller's contract, passed on.
      unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(
      &self,
      ptr: *mut u8,
      layout: Layout,
      new_size: usize,
    ) -> *mut u8 {
      note(new_size);
      // SAFETY: the caller's contract, passed on.
      unsafe { System.realloc(ptr, layout, new_size) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
      // SAFETY: the caller's contract, passed on.
      unsafe { System.dealloc(ptr, layout) }
    }
  }
}
