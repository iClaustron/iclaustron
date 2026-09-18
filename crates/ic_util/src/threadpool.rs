// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The thread pool (`IC_THREADPOOL_STATE`,
//! `legacy-c/util/ic_threadpool.c`): numbered threads with a stop flag
//! each, a way to wake one, and an optional handshake at startup.
//!
//! The Data API runs its send threads, receive threads and heartbeat
//! thread from here, and the thread id matters beyond bookkeeping: a
//! user thread's id becomes its NDB block number, which is what data
//! nodes address replies to.
//!
//! The lifetime of a thread:
//!
//! ```text
//!   pool.start_thread(...)  ->  id
//!       the new thread runs the given function with its ThreadState
//!       state.startup_done()   (only when started synchronised)
//!   pool.run_thread(id)        releases it to do its work
//!       ... state.stop_flag() is checked regularly ...
//!   pool.stop_thread(id)       asks it to stop and wakes it
//!   pool.join(id)              waits for it and frees the id
//! ```
//!
//! Rust notes for C readers: the C passed a `GThreadFunc` and a `void*`
//! object. Here the thread body is a boxed closure, which is how a Rust
//! thread receives anything: `Box::new(move |state| ...)` captures what
//! the thread needs, typically an `Arc` of shared state, and the
//! compiler checks that what is captured may safely cross threads. This
//! is the one place in the library that takes a closure; everywhere else
//! callbacks are plain function pointers.

use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::thread::JoinHandle;

use ic_port::debug::THREAD_LEVEL;
use ic_port::err;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_UNORDERED;
use ic_port::IcError;

/// Largest pool the C allowed, kept as the default ceiling.
pub const IC_DEFAULT_MAX_THREADPOOL_SIZE: u32 = 8192;

/// What a thread body is: a function that receives its state and runs
/// until it decides to return.
pub type ThreadFunc = Box<dyn FnOnce(&ThreadState) + Send + 'static>;

/// The parts of a thread's state that need the mutex.
struct ThreadSync {
  /// Set by `wake`, cleared by `wait`.
  woken: bool,
  /// The thread has finished its startup phase and is waiting.
  startup_done: bool,
  /// The pool has released the thread to do its work.
  run_allowed: bool,
  /// The thread body has returned.
  stopped: bool,
}

/// What a thread knows about itself. The pool holds one of these per
/// live thread and hands the thread an `Arc` of it.
pub struct ThreadState {
  thread_id: u32,
  stop_flag: AtomicBool,
  sync: IcMutex<ThreadSync>,
  cond: IcCond,
}

impl ThreadState {
  fn new(thread_id: u32) -> ThreadState {
    ThreadState {
      thread_id,
      stop_flag: AtomicBool::new(false),
      sync: IcMutex::new(
        IC_MUTEX_LEVEL_UNORDERED,
        ThreadSync {
          woken: false,
          startup_done: false,
          run_allowed: false,
          stopped: false,
        },
      ),
      cond: IcCond::new(),
    }
  }

  /// This thread's id, which for a Data API user thread is also its NDB
  /// block number offset (`ic_thread_get_id`).
  pub fn id(&self) -> u32 {
    self.thread_id
  }

  /// True once the pool has asked this thread to stop. Long running
  /// threads check it regularly (`ic_thread_get_stop_flag`).
  pub fn stop_flag(&self) -> bool {
    self.stop_flag.load(Ordering::Acquire)
  }

  /// Announce that the startup phase is over and wait until the pool
  /// releases the thread with [`ThreadPool::run_thread`]. Returns true
  /// if the thread was asked to stop while waiting
  /// (`ic_thread_startup_done`).
  pub fn startup_done(&self) -> bool {
    let mut guard = self.sync.lock();
    guard.startup_done = true;
    self.cond.broadcast();
    while !guard.run_allowed {
      if self.stop_flag() {
        return true;
      }
      guard = self.cond.wait(guard);
    }
    self.stop_flag()
  }

  /// Sleep until another thread calls [`wake`](Self::wake) or the stop
  /// flag is set (`ic_thread_wait`).
  pub fn wait(&self) {
    let mut guard = self.sync.lock();
    while !guard.woken && !self.stop_flag() {
      guard = self.cond.wait(guard);
    }
    guard.woken = false;
  }

  /// As [`wait`](Self::wait), but give up after `micros` microseconds.
  /// Returns true if woken, false on timeout.
  pub fn wait_timeout(&self, micros: u64) -> bool {
    let mut guard = self.sync.lock();
    if guard.woken || self.stop_flag() {
      guard.woken = false;
      return true;
    }
    let (mut guard2, timed_out) = self.cond.timed_wait(guard, micros);
    if timed_out && !guard2.woken {
      return false;
    }
    guard2.woken = false;
    true
  }

  /// Wake a thread sleeping in [`wait`](Self::wait) (`ic_thread_wake`).
  pub fn wake(&self) {
    let mut guard = self.sync.lock();
    guard.woken = true;
    self.cond.broadcast();
  }

  fn set_stop_flag(&self) {
    self.stop_flag.store(true, Ordering::Release);
    self.wake();
  }

  fn mark_stopped(&self) {
    let mut guard = self.sync.lock();
    guard.stopped = true;
    guard.startup_done = true;
    self.cond.broadcast();
  }

  fn has_stopped(&self) -> bool {
    self.sync.lock().stopped
  }

  fn wait_startup_done(&self) {
    let mut guard = self.sync.lock();
    while !guard.startup_done {
      guard = self.cond.wait(guard);
    }
  }

  fn allow_run(&self) {
    let mut guard = self.sync.lock();
    guard.run_allowed = true;
    self.cond.broadcast();
  }
}

struct Slot {
  in_use: bool,
  state: Option<Arc<ThreadState>>,
  handle: Option<JoinHandle<()>>,
}

/// A pool of numbered threads.
pub struct ThreadPool {
  name: String,
  slots: Vec<Slot>,
  free_ids: Vec<u32>,
  stop_flag: AtomicBool,
}

impl ThreadPool {
  /// A pool that can run `pool_size` threads at once. Ids run from 0 to
  /// `pool_size - 1` (`ic_create_threadpool`).
  pub fn new(pool_size: u32, pool_name: &str) -> ThreadPool {
    let mut size = pool_size;
    if size == 0 {
      size = 1;
    }
    if size > IC_DEFAULT_MAX_THREADPOOL_SIZE {
      size = IC_DEFAULT_MAX_THREADPOOL_SIZE;
    }
    let mut slots: Vec<Slot> = Vec::with_capacity(size as usize);
    let mut free_ids: Vec<u32> = Vec::with_capacity(size as usize);
    let mut i: u32 = 0;
    while i < size {
      slots.push(Slot {
        in_use: false,
        state: None,
        handle: None,
      });
      /* Hand out low ids first, so pop() takes the smallest. */
      free_ids.push(size - 1 - i);
      i += 1;
    }
    ThreadPool {
      name: pool_name.to_string(),
      slots,
      free_ids,
      stop_flag: AtomicBool::new(false),
    }
  }

  /// The pool's name, used to name the threads it starts.
  pub fn name(&self) -> &str {
    &self.name
  }

  /// How many threads the pool can run at once.
  pub fn capacity(&self) -> u32 {
    self.slots.len() as u32
  }

  /// How many ids are handed out.
  pub fn num_threads(&self) -> u32 {
    let mut count: u32 = 0;
    for slot in &self.slots {
      if slot.in_use {
        count += 1;
      }
    }
    count
  }

  /// Take a free thread id (`ic_threadpool_get_thread_id`).
  pub fn get_thread_id(&mut self) -> Result<u32, IcError> {
    match self.free_ids.pop() {
      Some(id) => {
        self.slots[id as usize].in_use = true;
        Ok(id)
      }
      None => Err(IcError::new(err::IC_ERROR_THREADPOOL_FULL)),
    }
  }

  /// Take a free thread id, reaping finished threads and waiting up to
  /// `timeout_seconds` for one to appear
  /// (`ic_threadpool_get_thread_id_wait`).
  pub fn get_thread_id_wait(
    &mut self,
    timeout_seconds: u32,
  ) -> Result<u32, IcError> {
    let mut waited: u32 = 0;
    loop {
      self.check_threads();
      if let Ok(id) = self.get_thread_id() {
        return Ok(id);
      }
      if waited >= timeout_seconds {
        return Err(IcError::new(err::IC_ERROR_THREADPOOL_FULL));
      }
      ic_port::time::sleep_low(1);
      waited += 1;
    }
  }

  /// Give back an id taken with [`get_thread_id`](Self::get_thread_id)
  /// but never used to start a thread
  /// (`ic_threadpool_free_thread_id`).
  pub fn free_thread_id(&mut self, thread_id: u32) {
    let index = thread_id as usize;
    if index >= self.slots.len() || !self.slots[index].in_use {
      return;
    }
    self.slots[index].in_use = false;
    self.slots[index].state = None;
    self.slots[index].handle = None;
    self.free_ids.push(thread_id);
  }

  /// Start a thread on an id already taken
  /// (`ic_threadpool_start_thread_with_thread_id`).
  ///
  /// With `synch_startup` the new thread runs until it calls
  /// [`ThreadState::startup_done`] and then waits; this call returns once
  /// it is waiting, so the caller knows the startup phase is over.
  /// [`run_thread`](Self::run_thread) then releases it.
  pub fn start_thread_with_id(
    &mut self,
    thread_id: u32,
    thread_func: ThreadFunc,
    stack_size: usize,
    synch_startup: bool,
  ) -> Result<(), IcError> {
    let index = thread_id as usize;
    if index >= self.slots.len() || !self.slots[index].in_use {
      return Err(IcError::new(err::IC_ERROR_WRONG_NODE_ID));
    }
    let state = Arc::new(ThreadState::new(thread_id));
    if !synch_startup {
      state.allow_run();
    }
    let thread_state = Arc::clone(&state);
    let mut builder =
      std::thread::Builder::new().name(format!("{}-{}", self.name, thread_id));
    if stack_size > 0 {
      builder = builder.stack_size(stack_size);
    }
    let spawned = builder.spawn(move || {
      thread_func(&thread_state);
      thread_state.mark_stopped();
    });
    let handle = match spawned {
      Ok(h) => h,
      Err(_) => {
        self.free_thread_id(thread_id);
        return Err(IcError::new(err::IC_ERROR_START_THREAD_FAILED));
      }
    };
    self.slots[index].state = Some(Arc::clone(&state));
    self.slots[index].handle = Some(handle);
    ic_port::debug_print!(
      THREAD_LEVEL,
      "Started thread {} in pool {}",
      thread_id,
      self.name
    );
    if synch_startup {
      state.wait_startup_done();
    }
    Ok(())
  }

  /// Take an id and start a thread on it
  /// (`ic_threadpool_start_thread`).
  pub fn start_thread(
    &mut self,
    thread_func: ThreadFunc,
    stack_size: usize,
    synch_startup: bool,
  ) -> Result<u32, IcError> {
    let thread_id = self.get_thread_id()?;
    self.start_thread_with_id(
      thread_id,
      thread_func,
      stack_size,
      synch_startup,
    )?;
    Ok(thread_id)
  }

  /// The state of a running thread, for waking it or reading its id
  /// (`ic_threadpool_get_thread_state`).
  pub fn thread_state(&self, thread_id: u32) -> Option<Arc<ThreadState>> {
    let slot = self.slots.get(thread_id as usize)?;
    slot.state.clone()
  }

  /// Release a thread that is waiting in
  /// [`ThreadState::startup_done`] (`ic_threadpool_run_thread`).
  pub fn run_thread(&self, thread_id: u32) {
    if let Some(state) = self.thread_state(thread_id) {
      state.allow_run();
    }
  }

  /// Ask a thread to stop and wake it, without waiting
  /// (`ic_threadpool_stop_thread`).
  pub fn stop_thread(&self, thread_id: u32) {
    if let Some(state) = self.thread_state(thread_id) {
      state.set_stop_flag();
      /* A thread still waiting for the run command has to be let go. */
      state.allow_run();
    }
  }

  /// Ask a thread to stop and wait for it
  /// (`ic_threadpool_stop_thread_wait`).
  pub fn stop_thread_wait(&mut self, thread_id: u32) {
    self.stop_thread(thread_id);
    self.join(thread_id);
  }

  /// Wait for a thread to finish and free its id
  /// (`ic_threadpool_join`).
  pub fn join(&mut self, thread_id: u32) {
    let index = thread_id as usize;
    if index >= self.slots.len() {
      return;
    }
    let handle = self.slots[index].handle.take();
    if let Some(h) = handle {
      let _ = h.join();
    }
    ic_port::debug_print!(
      THREAD_LEVEL,
      "Joined thread {} in pool {}",
      thread_id,
      self.name
    );
    self.free_thread_id(thread_id);
  }

  /// Join every thread that has finished, freeing its id
  /// (`ic_threadpool_check_threads`).
  pub fn check_threads(&mut self) {
    let mut finished: Vec<u32> = Vec::new();
    for slot in &self.slots {
      if !slot.in_use {
        continue;
      }
      if let Some(state) = slot.state.as_ref() {
        if state.has_stopped() {
          finished.push(state.id());
        }
      }
    }
    for id in finished {
      self.join(id);
    }
  }

  /// True once the pool has been asked to stop
  /// (`ic_threadpool_get_stop_flag`).
  pub fn get_stop_flag(&self) -> bool {
    self.stop_flag.load(Ordering::Acquire)
  }

  /// Ask every thread in the pool to stop, and wake them all
  /// (`ic_threadpool_set_stop_flag`).
  pub fn set_stop_flag(&self) {
    self.stop_flag.store(true, Ordering::Release);
    for slot in &self.slots {
      if let Some(state) = slot.state.as_ref() {
        state.set_stop_flag();
        state.allow_run();
      }
    }
  }

  /// Stop every thread and wait for all of them (`ic_threadpool_stop`).
  pub fn stop(&mut self) {
    self.set_stop_flag();
    let mut id: u32 = 0;
    while id < self.capacity() {
      if self.slots[id as usize].in_use {
        self.join(id);
      }
      id += 1;
    }
  }
}

impl Drop for ThreadPool {
  fn drop(&mut self) {
    self.stop();
  }
}

/// Sleep for a number of seconds, giving up early if the process stop
/// flag is set. Checked once a second, as the C `ic_sleep` did.
pub fn sleep_seconds(seconds_to_sleep: u32) {
  let mut slept: u32 = 0;
  while slept < seconds_to_sleep {
    if ic_port::stop::get_stop_flag() != 0 {
      return;
    }
    ic_port::time::sleep_low(1);
    slept += 1;
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::atomic::AtomicU32;

  #[test]
  fn ids_are_taken_and_returned() {
    let mut pool = ThreadPool::new(3, "test");
    assert_eq!(pool.capacity(), 3);
    let a = pool.get_thread_id().expect("id a");
    let b = pool.get_thread_id().expect("id b");
    let c = pool.get_thread_id().expect("id c");
    assert_eq!(a, 0);
    assert_eq!(b, 1);
    assert_eq!(c, 2);
    assert_eq!(pool.num_threads(), 3);
    assert!(pool.get_thread_id().is_err());
    pool.free_thread_id(b);
    assert_eq!(pool.num_threads(), 2);
    let again = pool.get_thread_id().expect("id again");
    assert_eq!(again, 1);
  }

  #[test]
  fn a_thread_runs_and_stops() {
    let counter = Arc::new(AtomicU32::new(0));
    let mut pool = ThreadPool::new(2, "run");
    let shared = Arc::clone(&counter);
    let id = pool
      .start_thread(
        Box::new(move |state| {
          while !state.stop_flag() {
            shared.fetch_add(1, Ordering::Relaxed);
            state.wait();
          }
        }),
        0,
        false,
      )
      .expect("start");
    let state = pool.thread_state(id).expect("state");
    assert_eq!(state.id(), id);
    let mut spins = 0;
    while counter.load(Ordering::Relaxed) == 0 && spins < 1000 {
      ic_port::time::microsleep(1000);
      spins += 1;
    }
    assert!(counter.load(Ordering::Relaxed) >= 1);
    state.wake();
    pool.stop_thread_wait(id);
    assert_eq!(pool.num_threads(), 0);
  }

  #[test]
  fn synchronised_startup_waits_for_run() {
    let progress = Arc::new(AtomicU32::new(0));
    let mut pool = ThreadPool::new(1, "synch");
    let shared = Arc::clone(&progress);
    let id = pool
      .start_thread(
        Box::new(move |state| {
          shared.store(1, Ordering::Release);
          if state.startup_done() {
            return;
          }
          shared.store(2, Ordering::Release);
        }),
        0,
        true,
      )
      .expect("start");
    /* start_thread returned only after the startup phase finished. */
    assert_eq!(progress.load(Ordering::Acquire), 1);
    pool.run_thread(id);
    pool.join(id);
    assert_eq!(progress.load(Ordering::Acquire), 2);
  }

  #[test]
  fn check_threads_reaps_finished_ones() {
    let mut pool = ThreadPool::new(4, "reap");
    let mut i = 0;
    while i < 4 {
      pool
        .start_thread(Box::new(|_state| {}), 0, false)
        .expect("start");
      i += 1;
    }
    assert_eq!(pool.num_threads(), 4);
    let mut spins = 0;
    while pool.num_threads() > 0 && spins < 1000 {
      pool.check_threads();
      ic_port::time::microsleep(1000);
      spins += 1;
    }
    assert_eq!(pool.num_threads(), 0);
    pool.get_thread_id().expect("id reused");
  }

  #[test]
  fn stop_flag_reaches_every_thread() {
    let mut pool = ThreadPool::new(3, "stop");
    let mut i = 0;
    while i < 3 {
      pool
        .start_thread(
          Box::new(|state| {
            while !state.stop_flag() {
              state.wait_timeout(1000);
            }
          }),
          0,
          false,
        )
        .expect("start");
      i += 1;
    }
    assert!(!pool.get_stop_flag());
    pool.stop();
    assert!(pool.get_stop_flag());
    assert_eq!(pool.num_threads(), 0);
  }

  #[test]
  fn wait_timeout_returns_on_wake_and_on_timeout() {
    let state = ThreadState::new(7);
    assert!(!state.wait_timeout(2000));
    state.wake();
    assert!(state.wait_timeout(2000));
  }
}
