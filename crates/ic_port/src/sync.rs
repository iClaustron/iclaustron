// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Mutex and condition variable (`IC_MUTEX`, `IC_COND`, `ic_mutex_*`,
//! `ic_cond_*`) with lock ordering levels.
//!
//! Every [`IcMutex`] is created with a level from the table in
//! doc/rust/02-architecture.md. In debug builds a thread that already
//! holds a mutex of level N and tries to lock one of level <= N aborts
//! with a message; this completes the discipline that
//! `legacy-c/api/ic_api_mutex_doc.txt` started. Level 0 means unordered
//! (never checked).
//!
//! Rust notes for C readers: `IcMutex<T>` *contains* the data it
//! protects. `lock()` returns a guard; the data is reached through the
//! guard (`guard.field`) and the mutex is unlocked when the guard goes out
//! of scope. There is no `unlock` call to forget. The `<T>` is the type
//! of the protected data, like `Vec<T>` is a vector of `T`.

use std::cell::RefCell;
use std::ops::Deref;
use std::ops::DerefMut;
use std::sync::Condvar;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::time::Duration;

/// Unordered: the mutex takes no part in ordering checks.
pub const IC_MUTEX_LEVEL_UNORDERED: u32 = 0;
/// Global dictionary cache.
pub const IC_MUTEX_LEVEL_DICT: u32 = 1;
/// `ApidGlobal`: thread table and stop flag. Taken at start and stop,
/// not on the signal path.
pub const IC_MUTEX_LEVEL_GLOBAL: u32 = 2;
/// One user thread's inbound signal queue.
pub const IC_MUTEX_LEVEL_THREAD_CONN: u32 = 3;
// Level 4 is deliberately unused. In the C it is the receive state
// mutex, which protects the lists that move a node from one receive
// thread to another. Here a node is assigned to a receive thread at
// connect and stays, so there is nothing to protect.
/// Global socket buffer page pool.
pub const IC_MUTEX_LEVEL_SOCK_BUF: u32 = 5;
/// One node connection's send chain. Not its state: that has a single
/// writer and is published through atomics, see `ic_apid::node_state`.
pub const IC_MUTEX_LEVEL_NODE_CONN: u32 = 6;
// Level 7 is deliberately unused. In the C it is the heartbeat mutex,
// which protects the heartbeat thread's linked list of nodes. Here the
// heartbeat thread walks the fixed node table and keeps no list.

thread_local! {
    static HELD_LEVELS: RefCell<Vec<u32>> = const { RefCell::new(Vec::new()) };
}

fn check_order(level: u32) {
  if level == IC_MUTEX_LEVEL_UNORDERED || !cfg!(debug_assertions) {
    return;
  }
  let mut violated: u32 = 0;
  HELD_LEVELS.with(|cell| {
    let held = cell.borrow();
    for h in held.iter() {
      if *h >= level {
        violated = *h;
      }
    }
  });
  if violated != 0 {
    crate::ic_printf!(
      "Mutex ordering violation: locking level {} while holding \
             level {}",
      level,
      violated
    );
    std::process::abort();
  }
}

fn push_level(level: u32) {
  if level == IC_MUTEX_LEVEL_UNORDERED || !cfg!(debug_assertions) {
    return;
  }
  HELD_LEVELS.with(|cell| {
    cell.borrow_mut().push(level);
  });
}

fn pop_level(level: u32) {
  if level == IC_MUTEX_LEVEL_UNORDERED || !cfg!(debug_assertions) {
    return;
  }
  HELD_LEVELS.with(|cell| {
    let mut held = cell.borrow_mut();
    let mut i = held.len();
    while i > 0 {
      i -= 1;
      if held[i] == level {
        held.remove(i);
        return;
      }
    }
  });
}

/// A mutex with an ordering level, protecting a value of type `T`.
pub struct IcMutex<T> {
  level: u32,
  inner: Mutex<T>,
}

/// Proof that an [`IcMutex`] is locked; gives access to the value.
pub struct IcMutexGuard<'a, T> {
  level: u32,
  guard: Option<MutexGuard<'a, T>>,
}

impl<T> IcMutex<T> {
  /// A new mutex at the given ordering level holding `value`.
  pub const fn new(level: u32, value: T) -> IcMutex<T> {
    IcMutex {
      level,
      inner: Mutex::new(value),
    }
  }

  /// Lock the mutex, checking the ordering level in debug builds.
  pub fn lock(&self) -> IcMutexGuard<'_, T> {
    check_order(self.level);
    let guard = match self.inner.lock() {
      Ok(g) => g,
      Err(poisoned) => poisoned.into_inner(),
    };
    push_level(self.level);
    IcMutexGuard {
      level: self.level,
      guard: Some(guard),
    }
  }

  /// The ordering level.
  pub fn level(&self) -> u32 {
    self.level
  }
}

impl<T> Deref for IcMutexGuard<'_, T> {
  type Target = T;

  fn deref(&self) -> &T {
    match self.guard.as_ref() {
      Some(g) => g,
      None => std::process::abort(),
    }
  }
}

impl<T> DerefMut for IcMutexGuard<'_, T> {
  fn deref_mut(&mut self) -> &mut T {
    match self.guard.as_mut() {
      Some(g) => g,
      None => std::process::abort(),
    }
  }
}

impl<T> Drop for IcMutexGuard<'_, T> {
  fn drop(&mut self) {
    pop_level(self.level);
  }
}

/// A condition variable used together with an [`IcMutex`].
pub struct IcCond {
  inner: Condvar,
}

impl IcCond {
  /// A new condition variable.
  pub const fn new() -> IcCond {
    IcCond {
      inner: Condvar::new(),
    }
  }

  /// Wake one waiter (`ic_cond_signal`).
  pub fn signal(&self) {
    self.inner.notify_one();
  }

  /// Wake all waiters (`ic_cond_broadcast`).
  pub fn broadcast(&self) {
    self.inner.notify_all();
  }

  /// Release the mutex, wait for a signal, re-acquire the mutex and
  /// return the guard (`ic_cond_wait`).
  pub fn wait<'a, T>(&self, guard: IcMutexGuard<'a, T>) -> IcMutexGuard<'a, T> {
    let mut g = guard;
    let inner = match g.guard.take() {
      Some(i) => i,
      None => std::process::abort(),
    };
    let inner = match self.inner.wait(inner) {
      Ok(i) => i,
      Err(poisoned) => poisoned.into_inner(),
    };
    g.guard = Some(inner);
    g
  }

  /// As [`wait`](IcCond::wait) but gives up after `micros`
  /// microseconds. The second value is true on timeout
  /// (`ic_cond_timed_wait`).
  pub fn timed_wait<'a, T>(
    &self,
    guard: IcMutexGuard<'a, T>,
    micros: u64,
  ) -> (IcMutexGuard<'a, T>, bool) {
    let mut g = guard;
    let inner = match g.guard.take() {
      Some(i) => i,
      None => std::process::abort(),
    };
    let duration = Duration::from_micros(micros);
    let result = self.inner.wait_timeout(inner, duration);
    let (inner, timeout) = match result {
      Ok((i, t)) => (i, t.timed_out()),
      Err(poisoned) => {
        let (i, t) = poisoned.into_inner();
        (i, t.timed_out())
      }
    };
    g.guard = Some(inner);
    (g, timeout)
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::Arc;

  #[test]
  fn lock_in_ascending_order() {
    let a = IcMutex::new(IC_MUTEX_LEVEL_GLOBAL, 1u32);
    let b = IcMutex::new(IC_MUTEX_LEVEL_NODE_CONN, 2u32);
    let ga = a.lock();
    let gb = b.lock();
    assert_eq!(*ga + *gb, 3);
    drop(gb);
    drop(ga);
    let mut gb = b.lock();
    *gb += 1;
    assert_eq!(*gb, 3);
  }

  #[test]
  fn cond_signal_wakes_waiter() {
    let shared = Arc::new((IcMutex::new(0, false), IcCond::new()));
    let for_thread = Arc::clone(&shared);
    let handle = std::thread::spawn(move || {
      let mut g = for_thread.0.lock();
      *g = true;
      for_thread.1.signal();
    });
    let mut g = shared.0.lock();
    while !*g {
      g = shared.1.wait(g);
    }
    assert!(*g);
    drop(g);
    handle.join().expect("join");
  }

  #[test]
  fn timed_wait_times_out() {
    let m = IcMutex::new(0, 0u32);
    let c = IcCond::new();
    let g = m.lock();
    let (g, timed_out) = c.timed_wait(g, 2000);
    assert!(timed_out);
    assert_eq!(*g, 0);
  }
}
