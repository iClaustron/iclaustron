// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! What every thread may know about a data node, without taking a lock.
//!
//! A node is assigned to one receive thread when it connects and stays
//! there. That same thread is the one that sees the socket close, and
//! it is the one that executes `API_REGCONF`. So exactly one thread
//! ever writes a node's state, and a mutex would protect against
//! nobody. This type is that state: the owning thread publishes, every
//! other thread reads.
//!
//! The C does it the other way and pays for it. Its
//! `node_failure_handling` takes the heartbeat mutex, a receive state
//! mutex and the node mutex at once, while `check_node_started`, which
//! user threads call before sending, reads the same fields with no
//! mutex at all. Single writer plus atomics removes the first and makes
//! the second legal.
//!
//! **Note for C readers.** An atomic here is what
//! `__atomic_load_n`/`__atomic_store_n` give you in C11, and the
//! orderings mean the same thing. The pattern below is publication:
//! the writer fills the plain fields first and stores the status word
//! last with release ordering; a reader loads the status word first
//! with acquire ordering and only then reads the rest. That pairing is
//! what guarantees a reader seeing "connected" also sees the node group
//! that was written just before it. Anything read without seeing
//! "connected" first is meaningless and the readers below treat it so.

use std::sync::atomic::AtomicU32;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;

use ic_ndb_signals::qmgr::NodeState;
use ic_ndb_signals::qmgr::StartLevel;
use ic_port::time::IcTimer;

/// Set in the status word while the socket is up and the node has
/// answered our registration.
const IC_STATUS_CONNECTED: u32 = 1;
/// Set while the cluster is in single user mode, when only one API node
/// may use it.
const IC_STATUS_SINGLE_USER: u32 = 2;
/// Where the start level sits in the status word.
const IC_STATUS_LEVEL_SHIFT: u32 = 8;
/// How wide the start level is in the status word.
const IC_STATUS_LEVEL_MASK: u32 = 0xF;

// The start level has to fit in the bits reserved for it, or a node
// that is stopping would read back as a node that is running.
const _: () = assert!((StartLevel::Stopping4 as u32) <= IC_STATUS_LEVEL_MASK);

/// One data node's state, as every thread but its owner sees it.
///
/// Readers may call anything here. Only the thread that owns the node
/// may call the `publish_*` functions, and there is exactly one such
/// thread for the life of a connection.
#[derive(Default)]
pub struct PublishedNodeState {
  /// Connected flag, single user flag and start level in one word, so
  /// that one load answers "may I send work here".
  status: AtomicU32,
  /// The node group the node reported.
  node_group: AtomicU32,
  /// The one API node allowed in single user mode, or zero.
  single_user_api: AtomicU32,
  /// Counts up on every connect, so a reader can tell one connection
  /// from the next and discard an answer meant for an older one.
  generation: AtomicU32,
  /// When the last `API_REGCONF` arrived, in the timer's nanoseconds.
  /// Zero until the first one does.
  last_regconf: AtomicU64,
}

impl std::fmt::Debug for PublishedNodeState {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    if !self.is_connected() {
      return write!(f, "PublishedNodeState(not connected)");
    }
    write!(
      f,
      "PublishedNodeState({:?}, node group {}, generation {})",
      self.start_level(),
      self.node_group(),
      self.generation()
    )
  }
}

impl PublishedNodeState {
  /// A node that has never been connected.
  pub fn new() -> PublishedNodeState {
    PublishedNodeState::default()
  }

  // ---- Readers. Any thread may call these. ----

  /// True when the socket is up and the node has registered.
  ///
  /// This says nothing about whether the node will serve transactions.
  /// A node that is still starting is connected but not started.
  pub fn is_connected(&self) -> bool {
    (self.status.load(Ordering::Acquire) & IC_STATUS_CONNECTED) != 0
  }

  /// How far through starting the node is, or `Nothing` when it is not
  /// connected.
  pub fn start_level(&self) -> StartLevel {
    let status = self.status.load(Ordering::Acquire);
    if (status & IC_STATUS_CONNECTED) == 0 {
      return StartLevel::Nothing;
    }
    level_of(status)
  }

  /// True when the node is connected and says it can serve us.
  ///
  /// One load answers it, so a caller never sees a node that is
  /// connected but whose level belongs to the previous connection.
  pub fn is_started(&self) -> bool {
    let status = self.status.load(Ordering::Acquire);
    if (status & IC_STATUS_CONNECTED) == 0 {
      return false;
    }
    level_of(status).is_started()
  }

  /// True while the cluster is in single user mode.
  pub fn is_single_user(&self) -> bool {
    (self.status.load(Ordering::Acquire) & IC_STATUS_SINGLE_USER) != 0
  }

  /// The one API node allowed in single user mode, or zero when the
  /// cluster is not in it.
  pub fn single_user_api(&self) -> u32 {
    if !self.is_single_user() {
      return 0;
    }
    self.single_user_api.load(Ordering::Relaxed)
  }

  /// Which node group the node belongs to, or zero when it has not
  /// said.
  pub fn node_group(&self) -> u32 {
    if !self.is_connected() {
      return 0;
    }
    self.node_group.load(Ordering::Relaxed)
  }

  /// How many times this node has been connected. An answer that
  /// arrives carrying an older generation belongs to a connection that
  /// is gone and should be discarded.
  pub fn generation(&self) -> u32 {
    self.generation.load(Ordering::Relaxed)
  }

  /// When the last `API_REGCONF` arrived, or zero if none has.
  pub fn last_regconf(&self) -> IcTimer {
    self.last_regconf.load(Ordering::Relaxed)
  }

  /// How long since the node last answered a heartbeat, in
  /// milliseconds. Zero when it has not answered at all, which a caller
  /// tells apart with [`last_regconf`](Self::last_regconf).
  pub fn silent_for_ms(&self, now: IcTimer) -> u64 {
    let last = self.last_regconf.load(Ordering::Relaxed);
    if last == 0 || now <= last {
      return 0;
    }
    (now - last) / 1_000_000
  }

  // ---- Writers. Only the thread that owns the node. ----

  /// Say the node is connected and registered. Starts a new generation.
  ///
  /// The level comes from the `API_REGCONF` that completed the
  /// registration, so a node is never published as connected without
  /// one.
  pub fn publish_connected(&self, state: &NodeState, now: IcTimer) {
    self.generation.fetch_add(1, Ordering::Relaxed);
    self.write_state(state, now);
  }

  /// Take what an `API_REGCONF` said about the node.
  ///
  /// Does nothing if the node is not published as connected, because a
  /// reply that outlived its connection must not resurrect it.
  pub fn publish_regconf(&self, state: &NodeState, now: IcTimer) {
    if !self.is_connected() {
      return;
    }
    self.write_state(state, now);
  }

  /// Say the node is gone. Every reader falls back to "not connected"
  /// and the satellite fields stop being read at all.
  pub fn publish_down(&self) {
    self.status.store(0, Ordering::Release);
  }

  /// The plain fields first, the status word last. See the note at the
  /// top of this module for why that order is the whole point.
  fn write_state(&self, state: &NodeState, now: IcTimer) {
    self.node_group.store(state.node_group, Ordering::Relaxed);
    self
      .single_user_api
      .store(state.single_user_api, Ordering::Relaxed);
    if now != 0 {
      self.last_regconf.store(now, Ordering::Relaxed);
    }
    let mut status: u32 = IC_STATUS_CONNECTED;
    if state.single_user_mode {
      status |= IC_STATUS_SINGLE_USER;
    }
    status |= ((state.start_level as u32) & IC_STATUS_LEVEL_MASK)
      << IC_STATUS_LEVEL_SHIFT;
    self.status.store(status, Ordering::Release);
  }
}

/// The start level held in a status word.
fn level_of(status: u32) -> StartLevel {
  StartLevel::from_u32((status >> IC_STATUS_LEVEL_SHIFT) & IC_STATUS_LEVEL_MASK)
}

#[cfg(test)]
mod tests {
  use super::*;

  fn started_state() -> NodeState {
    NodeState {
      start_level: StartLevel::Started,
      node_group: 3,
      ..NodeState::default()
    }
  }

  #[test]
  fn a_node_starts_out_disconnected() {
    let node = PublishedNodeState::new();
    assert!(!node.is_connected());
    assert!(!node.is_started());
    assert_eq!(node.start_level(), StartLevel::Nothing);
    assert_eq!(node.node_group(), 0);
    assert_eq!(node.generation(), 0);
    assert_eq!(node.last_regconf(), 0);
  }

  #[test]
  fn connecting_publishes_everything_at_once() {
    let node = PublishedNodeState::new();
    node.publish_connected(&started_state(), 5_000_000_000);
    assert!(node.is_connected());
    assert!(node.is_started());
    assert_eq!(node.start_level(), StartLevel::Started);
    assert_eq!(node.node_group(), 3);
    assert_eq!(node.generation(), 1);
    assert_eq!(node.last_regconf(), 5_000_000_000);
  }

  #[test]
  fn a_node_that_is_still_starting_is_connected_but_not_started() {
    // The difference matters: we may exchange heartbeats with it, but
    // we may not send it work.
    let node = PublishedNodeState::new();
    let state = NodeState {
      start_level: StartLevel::Starting,
      ..NodeState::default()
    };
    node.publish_connected(&state, 1);
    assert!(node.is_connected());
    assert!(!node.is_started());
    assert_eq!(node.start_level(), StartLevel::Starting);
  }

  #[test]
  fn a_node_that_is_stopping_reads_back_as_stopping() {
    // The start level has to survive the trip through the status word.
    // Stopping4 is the largest value, so it is the one that would be
    // lost if the field were too narrow.
    let node = PublishedNodeState::new();
    let state = NodeState {
      start_level: StartLevel::Stopping4,
      ..NodeState::default()
    };
    node.publish_connected(&state, 1);
    assert_eq!(node.start_level(), StartLevel::Stopping4);
    assert!(!node.is_started());
  }

  #[test]
  fn losing_a_node_hides_what_it_had_said() {
    let node = PublishedNodeState::new();
    node.publish_connected(&started_state(), 5_000_000_000);
    node.publish_down();
    assert!(!node.is_connected());
    assert!(!node.is_started());
    assert_eq!(node.start_level(), StartLevel::Nothing);
    // Stale values must not leak out through the satellite fields.
    assert_eq!(node.node_group(), 0);
    // The generation stays, so an answer from the old connection can
    // still be recognised as old.
    assert_eq!(node.generation(), 1);
  }

  #[test]
  fn a_late_reply_cannot_resurrect_a_lost_node() {
    // A heartbeat reply can be in flight when the socket closes. If it
    // were taken, the node would read as connected with nobody on the
    // other end.
    let node = PublishedNodeState::new();
    node.publish_connected(&started_state(), 1);
    node.publish_down();
    node.publish_regconf(&started_state(), 2);
    assert!(!node.is_connected());
  }

  #[test]
  fn reconnecting_starts_a_new_generation() {
    let node = PublishedNodeState::new();
    node.publish_connected(&started_state(), 1);
    node.publish_down();
    node.publish_connected(&started_state(), 2);
    assert_eq!(node.generation(), 2);
    assert!(node.is_started());
  }

  #[test]
  fn silence_is_measured_in_milliseconds() {
    let node = PublishedNodeState::new();
    node.publish_connected(&started_state(), 1_000_000_000);
    assert_eq!(node.silent_for_ms(1_000_000_000), 0);
    assert_eq!(node.silent_for_ms(7_000_000_000), 6000);
    // A clock that appears to go backwards must not read as a huge
    // silence and kill the node.
    assert_eq!(node.silent_for_ms(500_000_000), 0);
  }

  #[test]
  fn a_node_that_never_answered_reports_no_silence() {
    let node = PublishedNodeState::new();
    assert_eq!(node.last_regconf(), 0);
    assert_eq!(node.silent_for_ms(9_000_000_000), 0);
  }

  #[test]
  fn single_user_mode_names_the_one_api_node_allowed() {
    let node = PublishedNodeState::new();
    let state = NodeState {
      start_level: StartLevel::SingleUser,
      single_user_mode: true,
      single_user_api: 192,
      ..NodeState::default()
    };
    node.publish_connected(&state, 1);
    assert!(node.is_single_user());
    assert_eq!(node.single_user_api(), 192);
    // Single user mode is a level that serves, just not everyone.
    assert!(node.is_started());
  }

  #[test]
  fn an_ordinary_node_names_no_single_user_api() {
    let node = PublishedNodeState::new();
    node.publish_connected(&started_state(), 1);
    assert!(!node.is_single_user());
    assert_eq!(node.single_user_api(), 0);
  }
}
