// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The heartbeat thread (`legacy-c/api/ic_apid_heartbeat.ic`).
//!
//! It sends `API_REGREQ` to every connected data node on schedule, and
//! notices a node that keeps its socket open but has stopped answering,
//! which is how a hung process or a network dropping packets shows. The
//! answers themselves are executed by the receive thread that owns the
//! node, which records when each arrived; this thread only reads that.
//!
//! It keeps no list of nodes and takes no lock of its own. The C keeps a
//! linked list of nodes to heartbeat behind a mutex; here the thread
//! walks the fixed table of nodes and skips those without a link. A node
//! it has not seen before, or a new link to one, shows as a change in
//! the link's generation.
//!
//! When a node has gone [`IC_MAX_MISSED_HEARTBEATS`] check intervals in
//! a row without answering, the thread asks the node's receive thread to
//! drop the link. It never touches the link itself.
//!
//! Verify the counting against `ClusterMgr.cpp`, the missed heartbeat
//! test in `threadMain`: a counter goes up at the end of every check
//! interval and back to zero on every answer, and the node is lost when
//! it reaches four.

use std::sync::Arc;

use ic_ndb_signals::blocks;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::qmgr::ApiRegReq;
use ic_port::consts::IC_MYSQL_VERSION;
use ic_port::consts::IC_NDB_VERSION;
use ic_port::debug::IC_HEARTBEAT_LEVEL;
use ic_port::err;
use ic_port::time::IcTimer;
use ic_port::IcError;
use ic_util::threadpool::ThreadState;

use crate::apid_global::period_nanos;
use crate::apid_global::ApidShared;
use crate::apid_global::NodeShared;
use crate::apid_global::IC_MAX_MISSED_HEARTBEATS;
use crate::apid_global::IC_MIN_HEARTBEAT_INTERVAL_MS;

/// What the heartbeat thread remembers about one node. Its own, so
/// nothing in it is locked.
#[derive(Default)]
struct Beat {
  /// True while the node has a link this thread is keeping time for.
  tracking: bool,
  /// The generation of that link.
  generation: u32,
  heartbeat_at: IcTimer,
  check_at: IcTimer,
  /// Check intervals since the last answer.
  missed: u32,
  /// When the last answer we counted arrived.
  last_regconf: IcTimer,
}

/// The body of the heartbeat thread.
pub(crate) fn run_heartbeat_thread(
  shared: Arc<ApidShared>,
  state: &ThreadState,
) {
  let mut beats: Vec<Beat> = Vec::new();
  let mut index: usize = 0;
  while index < shared.nodes.len() {
    beats.push(Beat::default());
    index += 1;
  }
  while !state.stop_flag() {
    let now = ic_port::time::gethrtime();
    // Never sleep longer than the shortest interval there is, so that a
    // new link is taken up promptly.
    let mut next_due = now + period_nanos(IC_MIN_HEARTBEAT_INTERVAL_MS);
    index = 0;
    while index < shared.nodes.len() {
      let node = &shared.nodes[index];
      let beat = &mut beats[index];
      keep_time(&shared, node, beat, now);
      if beat.tracking {
        if beat.heartbeat_at < next_due {
          next_due = beat.heartbeat_at;
        }
        if beat.check_at < next_due {
          next_due = beat.check_at;
        }
      }
      index += 1;
    }
    let after = ic_port::time::gethrtime();
    if next_due > after {
      let _ = state.wait_timeout((next_due - after) / 1000);
    }
  }
}

/// Send what is due to one node and count what it has missed.
fn keep_time(
  shared: &ApidShared,
  node: &NodeShared,
  beat: &mut Beat,
  now: IcTimer,
) {
  if !node.published.is_connected() {
    beat.tracking = false;
    return;
  }
  let generation = node.published.generation();
  if !beat.tracking || beat.generation != generation {
    // A link we have not kept time for before.
    beat.tracking = true;
    beat.generation = generation;
    beat.heartbeat_at = now + period_nanos(node.heartbeat_period_ms());
    beat.check_at = now + period_nanos(node.check_interval_ms());
    beat.missed = 0;
    beat.last_regconf = node.published.last_regconf();
  }
  if now >= beat.heartbeat_at {
    send_heartbeat(shared, node);
    beat.heartbeat_at = now + period_nanos(node.heartbeat_period_ms());
  }
  if now < beat.check_at {
    return;
  }
  beat.check_at = now + period_nanos(node.check_interval_ms());
  let last = node.published.last_regconf();
  if last != beat.last_regconf {
    // An answer arrived during the interval just ended. It set the
    // count back to zero, and the end of the interval adds one.
    beat.last_regconf = last;
    beat.missed = 1;
  } else {
    beat.missed += 1;
  }
  if beat.missed >= 2 {
    ic_port::debug_print!(
      IC_HEARTBEAT_LEVEL,
      "Node {} has missed {} heartbeat(s)",
      node.node_id,
      beat.missed - 1
    );
  }
  if beat.missed >= IC_MAX_MISSED_HEARTBEATS {
    // Asked once; the link will be gone by the next round, and a new
    // one starts a new count.
    node.request_drop(IcError::new(err::IC_ERROR_HEARTBEAT_MISSED));
    beat.tracking = false;
  }
}

fn send_heartbeat(shared: &ApidShared, node: &NodeShared) {
  let request = ApiRegReq {
    block_ref: blocks::number_to_ref(
      blocks::IC_BLOCK_API_CLUSTERMGR,
      shared.own_node_id(),
    ),
    version: IC_NDB_VERSION,
    mysql_version: IC_MYSQL_VERSION,
  };
  let header = SignalHeader::new(
    gsn::IC_GSN_API_REGREQ,
    blocks::IC_BLOCK_API_CLUSTERMGR,
    blocks::IC_BLOCK_QMGR,
  );
  // A failed write has already asked for the link to be dropped.
  if let Err(e) = node.send(&header, &request.encode(), &[]) {
    ic_port::debug_print!(
      IC_HEARTBEAT_LEVEL,
      "Heartbeat to node {} not sent: {}",
      node.node_id,
      e.message()
    );
  }
}
