// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The connect thread, one per data node
//! (`legacy-c/api/ic_apid_send_thread.ic`, `run_send_thread` and
//! `connect_by_send_thread`).
//!
//! In the C the per-node send thread also does the connecting. Here the
//! two are apart, so that sending can be done by a small pool shared by
//! every node, while each node keeps a thread of its own for the one
//! job that blocks for a long time: dialling. A slow or unreachable node
//! then delays nobody else, and a thread asleep on a retry timer costs
//! little.
//!
//! The thread's whole life is one loop:
//!
//! 1. Look at the node's membership. While a link is up, or the cluster
//!    has reported the node failed and the failure is not yet handled,
//!    there is nothing to do.
//! 2. When a link it made has been lost, wait the first retry delay;
//!    when a reported failure has been handled, dial at once.
//! 3. Dial: ask the management server for the port, connect, complete
//!    the handshake and the registration. Every step blocks, and none of
//!    them is done holding a lock.
//! 4. Claim the link, which fails if the world moved on while it was
//!    being made, and hand it to the receive thread.
//! 5. On failure, wait a delay that grows while the node stays away.
//!
//! After every link has been lost, our node id may have to be claimed
//! again before anyone dials. Whichever connect thread finds an attempt
//! due makes it; the rest look again every [`IC_CONNECT_CHECK_MS`], so
//! that all of them dial as soon as the id is ours.
//!
//! The thread notices its link has gone by looking at the membership
//! every [`IC_CONNECT_CHECK_MS`], rather than by being woken. Nothing is
//! lost by it: the first retry after a loss is a second later anyway.

use std::sync::Arc;

use ic_port::debug::IC_COMM_LEVEL;
use ic_port::err;
use ic_port::time::IcTimer;
use ic_port::IcError;
use ic_util::threadpool::ThreadState;

use crate::apid_global::period_nanos;
use crate::apid_global::retry_delay_ms;
use crate::apid_global::ApidShared;
use crate::apid_global::LinkStatus;
use crate::apid_global::NodeShared;
use crate::apid_global::PendingLink;
use crate::apid_global::IC_CONNECT_CHECK_MS;
use crate::node_connect::NodeConnection;

/// The body of a connect thread.
pub(crate) fn run_connect_thread(
  shared: Arc<ApidShared>,
  node_index: usize,
  state: &ThreadState,
) {
  let node = Arc::clone(&shared.nodes[node_index]);
  let mut failed_attempts: u32 = 0;
  // Zero means dial as soon as the node may be dialled.
  let mut retry_at: IcTimer = 0;
  let mut last_seen = node.membership();
  while !state.stop_flag() {
    let now = ic_port::time::gethrtime();
    let status = node.membership();
    if status != last_seen {
      if status == LinkStatus::Disconnected {
        if last_seen == LinkStatus::Connected {
          // Our link broke. The node may well be up; try again soon.
          failed_attempts = 1;
          retry_at = now + period_nanos(retry_delay_ms(failed_attempts));
        } else {
          // A reported failure has been handled, or there is nobody
          // left to report on it. Either way, dial now.
          failed_attempts = 0;
          retry_at = 0;
        }
      }
      last_seen = status;
    }
    if status != LinkStatus::Disconnected {
      let _ = state.wait_timeout(micros(IC_CONNECT_CHECK_MS));
      continue;
    }
    if retry_at != 0 && now < retry_at {
      let mut wait_micros = (retry_at - now) / 1000;
      if wait_micros > micros(IC_CONNECT_CHECK_MS) {
        wait_micros = micros(IC_CONNECT_CHECK_MS);
      }
      let _ = state.wait_timeout(wait_micros);
      continue;
    }
    if shared.reclaim_pending() {
      // Every link was lost and our node id must be claimed again first.
      // That is not a failure of this node's dial, so it does not grow
      // this thread's delay; the reclaim has its own. Looking again soon
      // means dialling the moment the id is back, whoever got it.
      if let Err(e) = shared.reclaim_if_due() {
        if node.membership() == LinkStatus::Disconnected {
          node.set_last_error(e);
        }
        let _ = state.wait_timeout(micros(IC_CONNECT_CHECK_MS));
        continue;
      }
    }
    match dial(&shared, &node) {
      Ok(()) => {
        failed_attempts = 0;
        retry_at = 0;
        last_seen = LinkStatus::Connected;
      }
      Err(e) => {
        failed_attempts += 1;
        let delay = retry_delay_ms(failed_attempts);
        retry_at = ic_port::time::gethrtime() + period_nanos(delay);
        // A failure the cluster reported says more than a dial error.
        if node.membership() == LinkStatus::Disconnected {
          node.set_last_error(e);
        }
        ic_port::debug_print!(
          IC_COMM_LEVEL,
          "Node {} is down ({}); next attempt in {} ms",
          node.node_id,
          e.message(),
          delay
        );
      }
    }
  }
}

/// Make one link to the node and hand it to the receive thread.
fn dial(shared: &ApidShared, node: &NodeShared) -> Result<(), IcError> {
  // The port is asked for again every time. A node that restarts comes
  // back on a different one, and reports zero until it has started, so
  // an old port would connect to nothing or to someone else.
  let plan = shared.prepare_dial(node.node_id)?;
  let mut connection =
    NodeConnection::connect_to_port(&plan.config, node.node_id, plan.port)?;
  // Registration ends with the node describing itself, so a connection
  // without a state is one that did not register.
  let node_state = match connection.node_state {
    Some(node_state) => node_state,
    None => {
      connection.close();
      return Err(IcError::new(err::IC_ERROR_LINK_LOST));
    }
  };
  if let Err(e) = shared.claim_link(node, plan.epoch) {
    connection.close();
    return Err(e);
  }
  let heartbeat_interval_ms = connection.heartbeat_interval_ms;
  let use_checksum = connection.use_checksum;
  let (conn, reader) = connection.into_parts();
  node.hand_over(PendingLink {
    conn,
    reader,
    state: node_state,
    heartbeat_interval_ms,
    use_checksum,
  });
  Ok(())
}

/// Milliseconds as the microseconds a thread waits in.
fn micros(ms: u32) -> u64 {
  (ms as u64) * 1000
}
