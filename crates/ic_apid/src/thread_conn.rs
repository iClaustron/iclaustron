// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Handing signals from a receive thread to the user thread they are
//! for (`legacy-c/api/ic_apid_rec_thread.ic`, `post_ndb_messages`, and
//! `ic_apid_exec_message.ic`, `get_thread_messages`).
//!
//! This is the heart of the threading model. A receive thread does not
//! execute signals. It reads them, looks at the block each is addressed
//! to, and puts it in the inbox of the user thread that owns that block.
//! The user thread executes it in its own time and on its own core. The
//! C++ API executes in the receive thread and then wakes the user
//! thread, which makes the receive thread the bottleneck once there are
//! many user threads.
//!
//! Three things live here, as they do in the C:
//!
//! - [`ThreadConnection`], a user thread's inbox: a list of signals, a
//!   mutex and a condition (`IC_THREAD_CONNECTION`).
//! - [`ThreadTable`], which finds the inbox for a block number
//!   (`grid_comm->thread_conn_array`).
//! - [`Router`], a receive thread's own working state: the signals of
//!   the current round sorted per user thread, so that each inbox is
//!   locked once per round however many signals it gets
//!   (`temp_thd_conn` and `list_modules_received`).
//!
//! # The two things the C does that matter for speed
//!
//! **One lock per inbox per round.** Signals are collected per user
//! thread with no lock held, and the whole batch is appended under one
//! short lock. A user thread receiving a hundred signals in a round
//! costs the receive thread one lock, not a hundred.
//!
//! **The wake-up is sent after the lock is released.** Waking a thread
//! that then immediately blocks on the mutex we still hold would cost a
//! needless context switch. The price is that the user thread may wake,
//! take the signals and go back to waiting before our wake-up is even
//! sent, which then wakes it for nothing. That is harmless: the aim was
//! for the signals to be taken, and they were.
//!
//! # Finding the inbox without a lock
//!
//! The C reads `thread_conn_array` with no lock at all, which works
//! because a pointer store is atomic and threads come and go rarely.
//! Here the table has a mutex, but a receive thread does not take it per
//! signal or even per round. It keeps its own copy of the table and
//! takes the mutex only when the table's change counter, one atomic
//! load per round, says the copy is stale.
//!
//! **Note for C readers.** `Arc<ThreadConnection>` is a pointer with an
//! atomic reference count; the inbox is freed when the last holder lets
//! go. It is what makes it safe for a receive thread to still hold an
//! inbox whose user thread has just exited, where the C relies on the
//! user thread not freeing it too early.

use std::sync::atomic::AtomicU32;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_ndb_signals::blocks;
use ic_port::consts::IC_MAX_THREAD_CONNECTIONS;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_GLOBAL;
use ic_port::sync::IC_MUTEX_LEVEL_THREAD_CONN;
use ic_port::IcError;

use crate::node_connect::ReceivedSignal;

// A user thread's block number is the first API block number plus its
// thread id, so the ids have to fit in what is left of sixteen bits.
const _: () = assert!(
  IC_MAX_THREAD_CONNECTIONS <= (0x10000 - blocks::IC_MIN_API_BLOCK_NO as u32)
);

/// What the inbox mutex protects.
struct Inbox {
  /// Signals posted and not yet taken, oldest first.
  signals: Vec<ReceivedSignal>,
  /// True while the user thread is asleep waiting for signals
  /// (`thread_wait_cond`). It tells a poster whether a wake-up is
  /// needed, so that posting to a busy thread costs no system call.
  waiting: bool,
  /// True once the user thread has let go of its inbox. A receive
  /// thread may still hold it for a round; what it posts is dropped.
  closed: bool,
}

/// A user thread's inbox (`IC_THREAD_CONNECTION`).
pub struct ThreadConnection {
  thread_id: u32,
  inbox: IcMutex<Inbox>,
  cond: IcCond,
}

impl std::fmt::Debug for ThreadConnection {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ThreadConnection(thread {}, block {:#06x})",
      self.thread_id,
      self.block_number()
    )
  }
}

impl ThreadConnection {
  fn new(thread_id: u32) -> ThreadConnection {
    ThreadConnection {
      thread_id,
      inbox: IcMutex::new(
        IC_MUTEX_LEVEL_THREAD_CONN,
        Inbox {
          signals: Vec::new(),
          waiting: false,
          closed: false,
        },
      ),
      cond: IcCond::new(),
    }
  }

  /// Which user thread this is.
  pub fn thread_id(&self) -> u32 {
    self.thread_id
  }

  /// The block number data nodes address this thread's replies to.
  pub fn block_number(&self) -> u16 {
    blocks::api_block_of_thread(self.thread_id)
  }

  /// Put a round's worth of signals in the inbox and wake the thread if
  /// it is asleep (`post_ndb_messages`, the part inside the loop).
  ///
  /// `batch` is left empty, keeping its allocation for the next round.
  /// Returns false if the thread has gone and the signals were dropped.
  pub fn post(&self, batch: &mut Vec<ReceivedSignal>) -> bool {
    if batch.is_empty() {
      return true;
    }
    let mut inbox = self.inbox.lock();
    if inbox.closed {
      batch.clear();
      return false;
    }
    inbox.signals.append(batch);
    let wake = inbox.waiting;
    inbox.waiting = false;
    // Unlock, and only then wake; see the note at the top of the file.
    drop(inbox);
    if wake {
      self.cond.signal();
    }
    true
  }

  /// Take everything in the inbox, waiting up to `wait_ms` for
  /// something to arrive if it is empty (`get_thread_messages`).
  ///
  /// An empty result means the wait ran out, or the thread was woken
  /// for signals it had already taken, which the design allows. Either
  /// way the caller goes round again.
  pub fn take(&self, wait_ms: u32) -> Vec<ReceivedSignal> {
    let mut inbox = self.inbox.lock();
    if inbox.signals.is_empty() && wait_ms != 0 {
      inbox.waiting = true;
      let micros = (wait_ms as u64) * 1000;
      let (woken, _timed_out) = self.cond.timed_wait(inbox, micros);
      inbox = woken;
      // A poster clears this when it wakes us. After a timeout nobody
      // has, and leaving it set would cost the next poster a wake-up
      // sent to a thread that is not asleep.
      inbox.waiting = false;
    }
    std::mem::take(&mut inbox.signals)
  }

  /// How many signals are waiting to be taken.
  pub fn pending(&self) -> usize {
    self.inbox.lock().signals.len()
  }

  /// Refuse anything more and drop what is there. Called when the user
  /// thread lets go of its inbox.
  fn close(&self) {
    let mut inbox = self.inbox.lock();
    inbox.closed = true;
    inbox.signals.clear();
  }
}

/// What the table mutex protects.
struct TableSlots {
  /// One slot per thread id; `None` is a free slot.
  slots: Vec<Option<Arc<ThreadConnection>>>,
}

/// Finds a user thread's inbox by its thread id
/// (`grid_comm->thread_conn_array`).
pub struct ThreadTable {
  slots: IcMutex<TableSlots>,
  /// Counts every change to the table. A receive thread compares it
  /// with the count its own copy was made at, once per round.
  changes: AtomicU32,
  /// Signals dropped because they were for a block nobody owns. Never
  /// silent: a routing mistake has to be visible in production.
  unroutable: AtomicU64,
}

impl std::fmt::Debug for ThreadTable {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ThreadTable({} change(s), {} unroutable)",
      self.changes(),
      self.unroutable()
    )
  }
}

impl Default for ThreadTable {
  fn default() -> ThreadTable {
    ThreadTable::new()
  }
}

impl ThreadTable {
  /// A table with every slot free.
  pub fn new() -> ThreadTable {
    let mut slots: Vec<Option<Arc<ThreadConnection>>> = Vec::new();
    let mut i: u32 = 0;
    while i < IC_MAX_THREAD_CONNECTIONS {
      slots.push(None);
      i += 1;
    }
    ThreadTable {
      slots: IcMutex::new(IC_MUTEX_LEVEL_GLOBAL, TableSlots { slots }),
      changes: AtomicU32::new(0),
      unroutable: AtomicU64::new(0),
    }
  }

  /// Give a new user thread an inbox, in the first free slot.
  pub fn allocate(&self) -> Result<Arc<ThreadConnection>, IcError> {
    let mut table = self.slots.lock();
    let mut thread_id: usize = 0;
    while thread_id < table.slots.len() {
      if table.slots[thread_id].is_none() {
        let conn = Arc::new(ThreadConnection::new(thread_id as u32));
        table.slots[thread_id] = Some(Arc::clone(&conn));
        self.changes.fetch_add(1, Ordering::Release);
        return Ok(conn);
      }
      thread_id += 1;
    }
    Err(IcError::new(err::IC_ERROR_TOO_MANY_USER_THREADS))
  }

  /// Take a user thread's inbox out of the table. Signals still on
  /// their way to it are dropped from here on.
  pub fn release(&self, conn: &ThreadConnection) {
    let mut table = self.slots.lock();
    let thread_id = conn.thread_id as usize;
    if thread_id < table.slots.len() {
      table.slots[thread_id] = None;
    }
    self.changes.fetch_add(1, Ordering::Release);
    // Under the table lock, so that a receive thread refreshing its
    // copy right now sees either the old table and a closed inbox, or
    // the new table.
    conn.close();
  }

  /// How many times the table has changed.
  pub fn changes(&self) -> u32 {
    self.changes.load(Ordering::Acquire)
  }

  /// How many signals have been dropped for want of an owner.
  pub fn unroutable(&self) -> u64 {
    self.unroutable.load(Ordering::Relaxed)
  }

  /// Copy the table into a receive thread's own copy, returning the
  /// change count the copy corresponds to.
  fn copy_into(&self, copy: &mut Vec<Option<Arc<ThreadConnection>>>) -> u32 {
    let table = self.slots.lock();
    copy.clear();
    for slot in &table.slots {
      copy.push(slot.clone());
    }
    // Read under the lock, so the count and the copy agree.
    self.changes.load(Ordering::Acquire)
  }
}

/// A receive thread's own state for sorting a round's signals by user
/// thread (`temp_thd_conn` and `list_modules_received`).
///
/// One per receive thread, never shared, so nothing in it is locked.
pub struct Router {
  table: Arc<ThreadTable>,
  /// This thread's copy of the table, and the change count it was made
  /// at.
  copy: Vec<Option<Arc<ThreadConnection>>>,
  copy_changes: u32,
  /// This round's signals, per thread id.
  batches: Vec<Vec<ReceivedSignal>>,
  /// The thread ids that have signals this round, so that posting does
  /// not have to look at every slot.
  touched: Vec<u32>,
}

impl std::fmt::Debug for Router {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "Router({} thread(s) with signals)", self.touched.len())
  }
}

impl Router {
  /// A router for one receive thread.
  pub fn new(table: Arc<ThreadTable>) -> Router {
    let mut batches: Vec<Vec<ReceivedSignal>> = Vec::new();
    let mut i: u32 = 0;
    while i < IC_MAX_THREAD_CONNECTIONS {
      batches.push(Vec::new());
      i += 1;
    }
    let mut router = Router {
      table,
      copy: Vec::new(),
      copy_changes: 0,
      batches,
      touched: Vec::new(),
    };
    router.copy_changes = router.table.copy_into(&mut router.copy);
    router
  }

  /// Sort one signal into this round's batch for the user thread it is
  /// addressed to.
  ///
  /// A signal that is not for a user thread is handed back, because it
  /// is for one of our fixed blocks and the caller executes those
  /// itself: `API_REGCONF` and its kind, which describe the node they
  /// came from, belong to the receive thread that owns that node.
  pub fn route(&mut self, signal: ReceivedSignal) -> Option<ReceivedSignal> {
    let thread_id = match blocks::thread_of_api_block(signal.receiver_block) {
      Some(thread_id) => thread_id as usize,
      None => return Some(signal),
    };
    let owned = thread_id < self.copy.len() && self.copy[thread_id].is_some();
    if !owned {
      // A reply to a thread that has exited, or a routing mistake.
      self.table.unroutable.fetch_add(1, Ordering::Relaxed);
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "Dropped signal {} from node {} for block {:#06x}, which nobody \
         owns",
        signal.gsn,
        signal.sender_node_id,
        signal.receiver_block
      );
      return None;
    }
    if self.batches[thread_id].is_empty() {
      self.touched.push(thread_id as u32);
    }
    self.batches[thread_id].push(signal);
    None
  }

  /// Post every batch of this round to its inbox (`post_ndb_messages`),
  /// then bring this thread's copy of the table up to date if user
  /// threads have come or gone.
  pub fn post_all(&mut self) {
    let mut i: usize = 0;
    while i < self.touched.len() {
      let thread_id = self.touched[i] as usize;
      let posted = match &self.copy[thread_id] {
        Some(conn) => conn.post(&mut self.batches[thread_id]),
        None => false,
      };
      if !posted {
        // The thread went between our copy being made and now.
        self.table.unroutable.fetch_add(1, Ordering::Relaxed);
        self.batches[thread_id].clear();
      }
      i += 1;
    }
    self.touched.clear();
    // After posting, not before: the batches were sorted against the
    // copy we had, and must be posted against the same one.
    if self.table.changes() != self.copy_changes {
      self.copy_changes = self.table.copy_into(&mut self.copy);
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::thread;

  fn signal_for(conn: &ThreadConnection, gsn: u16) -> ReceivedSignal {
    ReceivedSignal {
      gsn,
      receiver_block: conn.block_number(),
      sender_node_id: 2,
      ..ReceivedSignal::default()
    }
  }

  #[test]
  fn threads_get_the_block_numbers_data_nodes_reply_to() {
    let table = ThreadTable::new();
    let first = table.allocate().expect("first");
    let second = table.allocate().expect("second");
    assert_eq!(first.thread_id(), 0);
    assert_eq!(second.thread_id(), 1);
    assert_eq!(first.block_number(), blocks::IC_MIN_API_BLOCK_NO);
    assert_eq!(second.block_number(), blocks::IC_MIN_API_BLOCK_NO + 1);
  }

  #[test]
  fn a_released_slot_is_used_again() {
    // Thread ids are block numbers, of which there is a fixed supply,
    // so an application that starts and stops threads must get them
    // back.
    let table = ThreadTable::new();
    let first = table.allocate().expect("first");
    let _second = table.allocate().expect("second");
    table.release(&first);
    let third = table.allocate().expect("third");
    assert_eq!(third.thread_id(), 0);
  }

  #[test]
  fn the_table_fills_up_and_says_so() {
    let table = ThreadTable::new();
    let mut held: Vec<Arc<ThreadConnection>> = Vec::new();
    let mut i: u32 = 0;
    while i < IC_MAX_THREAD_CONNECTIONS {
      held.push(table.allocate().expect("room"));
      i += 1;
    }
    let e = table.allocate().expect_err("full");
    assert_eq!(e.code, err::IC_ERROR_TOO_MANY_USER_THREADS);
  }

  #[test]
  fn signals_reach_the_thread_they_are_addressed_to() {
    let table = Arc::new(ThreadTable::new());
    let first = table.allocate().expect("first");
    let second = table.allocate().expect("second");
    let mut router = Router::new(Arc::clone(&table));
    assert!(router.route(signal_for(&first, 10)).is_none());
    assert!(router.route(signal_for(&second, 11)).is_none());
    assert!(router.route(signal_for(&first, 12)).is_none());
    // Nothing is visible until the round is posted.
    assert_eq!(first.pending(), 0);
    router.post_all();
    let got = first.take(0);
    assert_eq!(got.len(), 2);
    // In the order they arrived, which the protocol relies on.
    assert_eq!(got[0].gsn, 10);
    assert_eq!(got[1].gsn, 12);
    let got = second.take(0);
    assert_eq!(got.len(), 1);
    assert_eq!(got[0].gsn, 11);
    // Taking empties the inbox.
    assert!(first.take(0).is_empty());
  }

  #[test]
  fn a_signal_for_a_fixed_block_is_handed_back() {
    // The receive thread executes these itself; see the module note.
    let table = Arc::new(ThreadTable::new());
    let mut router = Router::new(Arc::clone(&table));
    let signal = ReceivedSignal {
      gsn: 1,
      receiver_block: blocks::IC_BLOCK_API_CLUSTERMGR,
      ..ReceivedSignal::default()
    };
    let back = router.route(signal).expect("handed back");
    assert_eq!(back.gsn, 1);
    assert_eq!(table.unroutable(), 0);
  }

  #[test]
  fn a_signal_for_nobody_is_dropped_and_counted() {
    let table = Arc::new(ThreadTable::new());
    let mut router = Router::new(Arc::clone(&table));
    let signal = ReceivedSignal {
      gsn: 10,
      receiver_block: blocks::IC_MIN_API_BLOCK_NO + 7,
      ..ReceivedSignal::default()
    };
    assert!(router.route(signal).is_none());
    assert_eq!(table.unroutable(), 1);
  }

  #[test]
  fn a_router_notices_threads_that_come_later() {
    // The router copied the table when it was made. A thread created
    // afterwards must still be reachable, from the next round on.
    let table = Arc::new(ThreadTable::new());
    let mut router = Router::new(Arc::clone(&table));
    let late = table.allocate().expect("late");
    router.post_all();
    assert!(router.route(signal_for(&late, 10)).is_none());
    router.post_all();
    assert_eq!(late.take(0).len(), 1);
    assert_eq!(table.unroutable(), 0);
  }

  #[test]
  fn a_late_reply_to_a_thread_that_has_gone_is_dropped() {
    let table = Arc::new(ThreadTable::new());
    let conn = table.allocate().expect("conn");
    let mut router = Router::new(Arc::clone(&table));
    assert!(router.route(signal_for(&conn, 10)).is_none());
    // The thread goes between the signal being sorted and posted.
    table.release(&conn);
    router.post_all();
    assert_eq!(conn.pending(), 0);
    assert_eq!(table.unroutable(), 1);
  }

  #[test]
  fn a_waiting_thread_is_woken_by_a_post() {
    let table = Arc::new(ThreadTable::new());
    let conn = table.allocate().expect("conn");
    let waiter = Arc::clone(&conn);
    let handle = thread::spawn(move || {
      // Long enough that only a wake-up explains a prompt return.
      let start = ic_port::time::gethrtime();
      let mut got = waiter.take(10_000);
      // The design allows a wake-up with nothing to take; go round.
      while got.is_empty() {
        got = waiter.take(10_000);
      }
      let waited =
        ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
      (got.len(), waited)
    });
    // Give the waiter time to fall asleep, so the wake-up path is the
    // one tested.
    ic_port::time::microsleep(50_000);
    let mut router = Router::new(Arc::clone(&table));
    assert!(router.route(signal_for(&conn, 10)).is_none());
    router.post_all();
    let (count, waited) = handle.join().expect("joined");
    assert_eq!(count, 1);
    assert!(waited < 5_000, "woken after {} ms", waited);
  }

  #[test]
  fn an_empty_inbox_gives_up_after_the_wait() {
    let table = ThreadTable::new();
    let conn = table.allocate().expect("conn");
    let start = ic_port::time::gethrtime();
    assert!(conn.take(30).is_empty());
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    assert!(waited >= 25, "returned after {} ms", waited);
    // And does not wait at all when told not to.
    assert!(conn.take(0).is_empty());
  }

  #[test]
  fn many_posters_lose_nothing() {
    // Several receive threads post to one user thread at once. Every
    // signal must arrive exactly once.
    let table = Arc::new(ThreadTable::new());
    let conn = table.allocate().expect("conn");
    let mut posters = Vec::new();
    let mut p: u16 = 0;
    while p < 4 {
      let table = Arc::clone(&table);
      let conn = Arc::clone(&conn);
      posters.push(thread::spawn(move || {
        let mut router = Router::new(table);
        let mut round: u16 = 0;
        while round < 100 {
          let mut i: u16 = 0;
          while i < 10 {
            let gsn = p * 1000 + round;
            assert!(router.route(signal_for(&conn, gsn)).is_none());
            i += 1;
          }
          router.post_all();
          round += 1;
        }
      }));
      p += 1;
    }
    let mut total: usize = 0;
    while total < 4000 {
      total += conn.take(1000).len();
    }
    for handle in posters {
      handle.join().expect("poster");
    }
    assert_eq!(total, 4000);
    assert!(conn.take(0).is_empty());
  }
}
