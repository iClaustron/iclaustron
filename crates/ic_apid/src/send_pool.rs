// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The send thread pool (`legacy-c/api/ic_apid_send_thread.ic`,
//! `active_send_thread`; chapter 02, "Send thread pool"): the thread
//! that writes what user threads leave behind.
//!
//! A user thread writes its own signals: it claims the node's send
//! chain and writes outside the node's mutex
//! ([`NodeShared::send_words`](crate::apid_global::NodeShared::send_words)).
//! Two things it does not do. When more signals were queued while it
//! wrote, it does not write again on their senders' behalf but hands
//! the node to the pool and goes back to its own work. And when the
//! adaptive send algorithm held its signals back for company that
//! never came, some thread has to write them once the wait is over;
//! the pool is that thread.
//!
//! The C has a send thread per node and lets the receive thread's
//! wake-ups end the waits. A pool keeps the thread count off the node
//! count, which matters at 144 data nodes, and a thread that sleeps
//! until the earliest deadline ends a wait when it is due rather than
//! when the next signal happens to arrive; our receive thread sleeps
//! for milliseconds when nothing comes, where a wait is bounded in
//! microseconds. Decided 2026-09-22, over chapter 02's "receive threads
//! run the adaptive-send timers".
//!
//! One thread for now. It never holds the pool's mutex while it writes,
//! and a node with more to write after one write is queued again
//! behind the others rather than written until empty, so that one busy
//! node cannot hold the rest.

use std::collections::VecDeque;
use std::sync::Arc;

use ic_port::debug::IC_THREAD_LEVEL;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_SEND_POOL;
use ic_port::time;
use ic_port::time::IcTimer;
use ic_util::threadpool::ThreadState;

use crate::apid_global::ApidShared;

/// Stack for a send thread.
pub const IC_SEND_THREAD_STACK: usize = 256 * 1024;
/// How long the thread sleeps with nothing due, so that a stop is seen.
pub const IC_SEND_POOL_IDLE_MICROS: u64 = 10_000;

struct SendQueue {
  /// Nodes with signals waiting that the pool is to write, in the order
  /// they were handed over.
  ready: VecDeque<u32>,
  /// Nodes holding signals back for company, each with when it must
  /// write them.
  deferred: Vec<(u32, IcTimer)>,
}

/// What user threads hand the pool. Held in [`ApidShared`] and by every
/// node.
pub(crate) struct SendPool {
  queue: IcMutex<SendQueue>,
  cond: IcCond,
}

impl SendPool {
  pub(crate) fn new() -> SendPool {
    SendPool {
      queue: IcMutex::new(
        IC_MUTEX_LEVEL_SEND_POOL,
        SendQueue {
          ready: VecDeque::new(),
          deferred: Vec::new(),
        },
      ),
      cond: IcCond::new(),
    }
  }

  /// Ask the pool to write what waits at a node, whose send chain the
  /// caller has left claimed for it (`send_done_handling`: "we give
  /// this mission to the send thread").
  pub(crate) fn ask(&self, node_id: u32) {
    let mut queue = self.queue.lock();
    if !queue.ready.contains(&node_id) {
      queue.ready.push_back(node_id);
    }
    drop(queue);
    self.cond.signal();
  }

  /// Say that a node holds signals back until `deadline`, when the
  /// pool is to write them if no one else has.
  pub(crate) fn defer(&self, node_id: u32, deadline: IcTimer) {
    let mut queue = self.queue.lock();
    for (id, _) in &queue.deferred {
      if *id == node_id {
        return;
      }
    }
    let was_idle = queue.deferred.is_empty();
    queue.deferred.push((node_id, deadline));
    drop(queue);
    // A deadline is always later than those already known, so a thread
    // sleeping towards one of them needs no waking.
    if was_idle {
      self.cond.signal();
    }
  }
}

/// A node whose held signals are due, taken out of the list.
fn take_due(queue: &mut SendQueue, now: IcTimer) -> Option<u32> {
  let mut i = 0;
  while i < queue.deferred.len() {
    if queue.deferred[i].1 <= now {
      return Some(queue.deferred.remove(i).0);
    }
    i += 1;
  }
  None
}

/// The earliest deadline among the nodes holding signals back.
fn earliest(queue: &SendQueue) -> Option<IcTimer> {
  let mut earliest: Option<IcTimer> = None;
  for (_, deadline) in &queue.deferred {
    let sooner = match earliest {
      Some(known) => *deadline < known,
      None => true,
    };
    if sooner {
      earliest = Some(*deadline);
    }
  }
  earliest
}

/// The send thread's body.
pub(crate) fn run_send_thread(shared: Arc<ApidShared>, state: &ThreadState) {
  ic_port::debug_print!(IC_THREAD_LEVEL, "Send thread started");
  let pool = &shared.send_pool;
  while !state.stop_flag() {
    let now = time::gethrtime();
    let mut queue = pool.queue.lock();
    if let Some(node_id) = queue.ready.pop_front() {
      drop(queue);
      if let Some(node) = shared.node(node_id) {
        node.send_for_pool(false);
      }
      continue;
    }
    if let Some(node_id) = take_due(&mut queue, now) {
      drop(queue);
      if let Some(node) = shared.node(node_id) {
        node.send_for_pool(true);
      }
      continue;
    }
    let wait_micros = match earliest(&queue) {
      Some(deadline) => (deadline - now) / 1000 + 1,
      None => IC_SEND_POOL_IDLE_MICROS,
    };
    let _ = pool.cond.timed_wait(queue, wait_micros);
  }
  ic_port::debug_print!(IC_THREAD_LEVEL, "Send thread stopped");
}
