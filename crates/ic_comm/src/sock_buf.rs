// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The socket buffer pool (`IC_SOCK_BUF`,
//! `legacy-c/comm/ic_sock_buf.c`): a fixed set of buffer pages that the
//! receive threads read into and the user threads read out of.
//!
//! Pages are pooled rather than allocated per read because they are the
//! hot path: every signal from every data node arrives in one. Taking a
//! page from the shared pool needs a lock, so a thread takes several at
//! a time into a free list of its own and returns them in batches; the
//! shared lock is then touched about once every `preallocate` pages, as
//! it was in the C.
//!
//! A page carries a reference count. One page can hold signals for
//! several user threads, and the last thread to finish with it returns
//! it. The count is maintained by whoever shares the page, which for now
//! is the Data API's receive path.

//
// Every page is its own allocation, held as Box<SockBufPage>, and clippy
// points out that a Vec of Box is one indirection more than a Vec of
// values. That indirection is the point here. A page is shared between
// the receive thread and the user threads it carries signals for, and
// they agree on it through the reference count inside the page, so the
// page has to keep one address for its whole life. A Vec of values would
// move a page whenever the Vec grew, and again when it was handed out.
// The C had the same property for the same reason, and sized the page to
// a cache line so that two pages in two threads never shared one.
#![allow(clippy::vec_box)]

use std::sync::atomic::AtomicI32;
use std::sync::atomic::Ordering;

use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_SOCK_BUF;

/// Words of space on every page that its owner may use for its own
/// bookkeeping, as the C `opaque_area` did.
pub const IC_SOCK_BUF_OPAQUE_WORDS: usize = 8;

/// One buffer page.
///
/// Rust notes for C readers: the C page was a 128 byte descriptor with a
/// pointer to a separate buffer, sized to a cache line so that pages
/// passed between threads did not share one. Here the page owns its
/// buffer as a `Vec<u8>`, and a page is passed between threads as a
/// `Box<SockBufPage>`, which is a pointer with an owner the compiler
/// tracks.
pub struct SockBufPage {
  buf: Vec<u8>,
  /// Bytes of valid data at the front of the buffer.
  data_len: u32,
  ref_count: AtomicI32,
  /// Scratch space for the page's current owner.
  pub opaque: [u32; IC_SOCK_BUF_OPAQUE_WORDS],
}

impl SockBufPage {
  fn new(page_size: usize) -> SockBufPage {
    SockBufPage {
      buf: vec![0u8; page_size],
      data_len: 0,
      ref_count: AtomicI32::new(1),
      opaque: [0; IC_SOCK_BUF_OPAQUE_WORDS],
    }
  }

  /// The whole buffer, for reading into from a socket.
  pub fn buf_mut(&mut self) -> &mut [u8] {
    &mut self.buf
  }

  /// The valid data, as set by [`set_data_len`](Self::set_data_len).
  pub fn data(&self) -> &[u8] {
    &self.buf[..self.data_len as usize]
  }

  /// The valid data, for modification in place.
  pub fn data_mut(&mut self) -> &mut [u8] {
    let end = self.data_len as usize;
    &mut self.buf[..end]
  }

  /// How many bytes of the page hold data.
  pub fn data_len(&self) -> u32 {
    self.data_len
  }

  /// Record how many bytes a read placed in the page.
  pub fn set_data_len(&mut self, len: u32) {
    ic_port::ic_assert!(len as usize <= self.buf.len());
    self.data_len = len;
  }

  /// Size of the whole buffer.
  pub fn page_size(&self) -> u32 {
    self.buf.len() as u32
  }

  /// Set the reference count before handing the page to several
  /// threads.
  pub fn set_ref_count(&self, count: i32) {
    self.ref_count.store(count, Ordering::Release);
  }

  /// The current reference count.
  pub fn ref_count(&self) -> i32 {
    self.ref_count.load(Ordering::Acquire)
  }

  /// Add one to the reference count.
  pub fn inc_ref_count(&self) {
    self.ref_count.fetch_add(1, Ordering::AcqRel);
  }

  /// Take one off the reference count. True when it reaches zero, which
  /// means the caller holds the last reference and should return the
  /// page to the pool.
  pub fn dec_ref_count(&self) -> bool {
    self.ref_count.fetch_sub(1, Ordering::AcqRel) == 1
  }

  fn reset(&mut self) {
    self.data_len = 0;
    self.ref_count.store(1, Ordering::Release);
    self.opaque = [0; IC_SOCK_BUF_OPAQUE_WORDS];
  }
}

struct PoolInner {
  free_pages: Vec<Box<SockBufPage>>,
  num_pages: u64,
}

/// A pool of buffer pages shared by every thread that touches a socket.
pub struct SockBufPool {
  inner: IcMutex<PoolInner>,
  cond: IcCond,
  page_size: usize,
}

/// A thread's own list of pages taken from the pool, so that the shared
/// lock is not needed for every page.
pub type LocalFreeList = Vec<Box<SockBufPage>>;

impl SockBufPool {
  /// A pool of `num_pages` pages of `page_size` bytes each
  /// (`ic_create_sock_buf`).
  pub fn new(page_size: usize, num_pages: u64) -> SockBufPool {
    let mut free_pages: LocalFreeList = Vec::with_capacity(num_pages as usize);
    let mut i: u64 = 0;
    while i < num_pages {
      free_pages.push(Box::new(SockBufPage::new(page_size)));
      i += 1;
    }
    SockBufPool {
      inner: IcMutex::new(
        IC_MUTEX_LEVEL_SOCK_BUF,
        PoolInner {
          free_pages,
          num_pages,
        },
      ),
      cond: IcCond::new(),
      page_size,
    }
  }

  /// Size of every page in the pool.
  pub fn page_size(&self) -> usize {
    self.page_size
  }

  /// How many pages the pool owns in total.
  pub fn num_pages(&self) -> u64 {
    self.inner.lock().num_pages
  }

  /// How many pages are free right now.
  pub fn num_free(&self) -> usize {
    self.inner.lock().free_pages.len()
  }

  /// Take a page, refilling `local` with `preallocate` pages from the
  /// shared pool when it is empty (`ic_get_sock_buf_page`).
  pub fn get_page(
    &self,
    local: &mut LocalFreeList,
    preallocate: u32,
  ) -> Option<Box<SockBufPage>> {
    if let Some(page) = local.pop() {
      return Some(page);
    }
    let mut want = preallocate;
    if want == 0 {
      want = 1;
    }
    let mut guard = self.inner.lock();
    let mut taken: u32 = 0;
    while taken < want {
      match guard.free_pages.pop() {
        Some(page) => {
          local.push(page);
          taken += 1;
        }
        None => break,
      }
    }
    drop(guard);
    local.pop()
  }

  /// As [`get_page`](Self::get_page), but wait up to `wait_ms`
  /// milliseconds for a page when the pool is empty
  /// (`ic_get_sock_buf_page_wait`).
  pub fn get_page_wait(
    &self,
    local: &mut LocalFreeList,
    preallocate: u32,
    wait_ms: u32,
  ) -> Option<Box<SockBufPage>> {
    if let Some(page) = self.get_page(local, preallocate) {
      return Some(page);
    }
    let mut guard = self.inner.lock();
    let mut waited: u32 = 0;
    while guard.free_pages.is_empty() && waited < wait_ms {
      let (next_guard, timed_out) = self.cond.timed_wait(guard, 1000);
      guard = next_guard;
      if timed_out {
        waited += 1;
      }
    }
    let page = guard.free_pages.pop();
    drop(guard);
    page
  }

  /// Give a page back (`ic_return_sock_buf_page`).
  pub fn return_page(&self, mut page: Box<SockBufPage>) {
    page.reset();
    let mut guard = self.inner.lock();
    guard.free_pages.push(page);
    self.cond.signal();
  }

  /// Give back every page in `pages`, emptying it. The C passed a linked
  /// list of pages for the same reason: one lock for many pages.
  pub fn return_pages(&self, pages: &mut LocalFreeList) {
    if pages.is_empty() {
      return;
    }
    let mut guard = self.inner.lock();
    while let Some(mut page) = pages.pop() {
      page.reset();
      guard.free_pages.push(page);
    }
    self.cond.broadcast();
  }

  /// Add more pages to the pool (`ic_inc_sock_buf`).
  pub fn inc_pages(&self, num_pages: u64) {
    let mut guard = self.inner.lock();
    let mut i: u64 = 0;
    while i < num_pages {
      guard
        .free_pages
        .push(Box::new(SockBufPage::new(self.page_size)));
      i += 1;
    }
    guard.num_pages += num_pages;
    self.cond.broadcast();
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::Arc;

  #[test]
  fn pages_are_taken_and_returned() {
    let pool = SockBufPool::new(1024, 4);
    assert_eq!(pool.num_pages(), 4);
    assert_eq!(pool.num_free(), 4);
    assert_eq!(pool.page_size(), 1024);
    let mut local: LocalFreeList = Vec::new();
    let page = pool.get_page(&mut local, 2).expect("page");
    assert_eq!(page.page_size(), 1024);
    /* Two were taken from the pool, one handed out, one left local. */
    assert_eq!(pool.num_free(), 2);
    assert_eq!(local.len(), 1);
    pool.return_page(page);
    assert_eq!(pool.num_free(), 3);
    pool.return_pages(&mut local);
    assert_eq!(pool.num_free(), 4);
    assert!(local.is_empty());
  }

  #[test]
  fn the_local_list_avoids_the_pool() {
    let pool = SockBufPool::new(64, 10);
    let mut local: LocalFreeList = Vec::new();
    let first = pool.get_page(&mut local, 5).expect("page");
    assert_eq!(pool.num_free(), 5);
    /* The next four come from the local list without touching the pool. */
    let mut pages: LocalFreeList = vec![first];
    let mut i = 0;
    while i < 4 {
      pages.push(pool.get_page(&mut local, 5).expect("page"));
      i += 1;
    }
    assert_eq!(pool.num_free(), 5);
    assert!(local.is_empty());
    pool.return_pages(&mut pages);
    assert_eq!(pool.num_free(), 10);
  }

  #[test]
  fn data_and_reference_counts() {
    let pool = SockBufPool::new(32, 1);
    let mut local: LocalFreeList = Vec::new();
    let mut page = pool.get_page(&mut local, 1).expect("page");
    page.buf_mut()[..5].copy_from_slice(b"hello");
    page.set_data_len(5);
    assert_eq!(page.data(), b"hello");
    assert_eq!(page.data_len(), 5);
    page.data_mut()[0] = b'H';
    assert_eq!(page.data(), b"Hello");
    assert_eq!(page.ref_count(), 1);
    page.set_ref_count(3);
    page.inc_ref_count();
    assert_eq!(page.ref_count(), 4);
    assert!(!page.dec_ref_count());
    assert!(!page.dec_ref_count());
    assert!(!page.dec_ref_count());
    assert!(page.dec_ref_count());
    /* Returning resets the page for its next user. */
    pool.return_page(page);
    let page = pool.get_page(&mut local, 1).expect("page");
    assert_eq!(page.data_len(), 0);
    assert_eq!(page.ref_count(), 1);
  }

  #[test]
  fn an_empty_pool_reports_it() {
    let pool = SockBufPool::new(16, 1);
    let mut local: LocalFreeList = Vec::new();
    let page = pool.get_page(&mut local, 1).expect("page");
    assert!(pool.get_page(&mut local, 1).is_none());
    assert!(pool.get_page_wait(&mut local, 1, 2).is_none());
    pool.inc_pages(2);
    assert_eq!(pool.num_pages(), 3);
    let extra = pool.get_page(&mut local, 1).expect("page after inc");
    pool.return_page(page);
    pool.return_page(extra);
  }

  #[test]
  fn a_waiting_thread_gets_a_returned_page() {
    let pool = Arc::new(SockBufPool::new(16, 1));
    let mut local: LocalFreeList = Vec::new();
    let page = pool.get_page(&mut local, 1).expect("page");
    let returner = Arc::clone(&pool);
    let handle = std::thread::spawn(move || {
      ic_port::time::microsleep(20_000);
      returner.return_page(page);
    });
    let mut waiter: LocalFreeList = Vec::new();
    let got = pool.get_page_wait(&mut waiter, 1, 2000);
    assert!(got.is_some());
    handle.join().expect("join");
  }
}
