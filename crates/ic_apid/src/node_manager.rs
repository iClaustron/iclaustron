// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Keeping connections to every data node
//! (`legacy-c/api/ic_apid_send_thread.ic`, `run_send_thread` and
//! `connect_by_send_thread`, and the node handling of
//! `ic_apid_rec_thread.ic`).
//!
//! A cluster is not a set of connections made once at startup. Nodes
//! stop and start, and an API node is expected to notice quickly and
//! reconnect by itself. This module owns the set of links and drives
//! them:
//!
//! - a link that is down is retried, with a delay that grows while it
//!   keeps failing;
//! - the port is asked for again on every attempt, because a node that
//!   restarts comes back on a different one, and reports zero until it
//!   has started;
//! - a poll set watches every connected socket, so a node going away is
//!   noticed when it closes rather than when we next try to write;
//! - heartbeats go out on schedule, since a data node that stops
//!   hearing from us declares us dead.
//!
//! A failure another data node reports is recorded on the link but does
//! not hold the retry back. The C send thread retries regardless, and a
//! data node not yet ready to talk refuses the connection, which costs
//! nothing. Waiting for the takeover to be reported would only make
//! recovery slower.
//!
//! In this release one thread drives all of it through [`poll`]. The
//! threaded form of the design keeps the same states and transitions,
//! with a send thread per node doing the connecting and a receive
//! thread owning the poll set. Until then, connecting happens on the
//! polling thread, so a host that swallows packets rather than
//! refusing them can hold up a round for as long as the connect
//! timeout. A node that is merely down refuses at once and costs
//! nothing.
//!
//! [`poll`]: NodeManager::poll

use std::collections::BTreeMap;

use ic_apic::data::ClusterConfig;
use ic_apic::mgm_client::MgmClient;
use ic_comm::poll_set::PollSet;
use ic_ndb_signals::gsn;
use ic_ndb_signals::qmgr::NfCompleteRep;
use ic_ndb_signals::qmgr::NodeFailRep;
use ic_ndb_signals::qmgr::NodeState;
use ic_port::debug::IC_COMM_LEVEL;
use ic_port::debug::IC_HEARTBEAT_LEVEL;
use ic_port::time::IcTimer;
use ic_port::IcError;
use ic_util::connectstring::ConnectString;

use crate::node_connect::resolve_port;
use crate::node_connect::NodeConnection;
use crate::node_connect::ReceivedSignal;

/// How long to wait before the first retry of a failed link.
pub const IC_FIRST_RETRY_MS: u32 = 1000;
/// The longest the retry delay grows to.
pub const IC_MAX_RETRY_MS: u32 = 10_000;
/// Fraction of the heartbeat interval we actually send at, so that a
/// missed round is not immediately fatal. The C++ client uses a fifth.
pub const IC_HEARTBEAT_DIVISOR: u32 = 5;

/// Where a link to one data node stands.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum LinkStatus {
  /// Not connected, and waiting before trying again.
  #[default]
  Disconnected,
  /// Connected and registered; the node answers our signals.
  Connected,
  /// Not connected, and another data node has confirmed that this one
  /// failed. It is retried like any other disconnected node: a node
  /// that is not ready to talk refuses, which costs nothing.
  Failing,
}

/// One link to one data node, and what we know about it.
pub struct NodeLink {
  /// The data node.
  pub node_id: u32,
  /// Where the link stands.
  pub status: LinkStatus,
  /// What the node last told us about itself.
  pub node_state: Option<NodeState>,
  /// How many times connecting has failed since it last worked.
  pub failed_attempts: u32,
  /// The last error, for reporting.
  pub last_error: Option<IcError>,
  /// How long the node lets us go silent before declaring us dead. We
  /// send well inside it.
  pub heartbeat_interval_ms: u32,
  connection: Option<NodeConnection>,
  retry_at: IcTimer,
  heartbeat_at: IcTimer,
}

impl std::fmt::Debug for NodeLink {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "NodeLink(node {}, {:?}", self.node_id, self.status)?;
    if let Some(state) = self.node_state {
      write!(f, ", {:?}", state.start_level)?;
    }
    if self.failed_attempts > 0 {
      write!(f, ", {} failed attempt(s)", self.failed_attempts)?;
    }
    write!(f, ")")
  }
}

impl NodeLink {
  fn new(node_id: u32, heartbeat_interval_ms: u32) -> NodeLink {
    NodeLink {
      node_id,
      status: LinkStatus::Disconnected,
      node_state: None,
      failed_attempts: 0,
      last_error: None,
      heartbeat_interval_ms,
      connection: None,
      // Nothing to wait for before the first attempt.
      retry_at: 0,
      heartbeat_at: 0,
    }
  }

  /// True when the node answers our signals.
  pub fn is_connected(&self) -> bool {
    self.status == LinkStatus::Connected
  }

  /// True when the node says it is started and can serve us.
  pub fn is_started(&self) -> bool {
    match self.node_state {
      Some(state) => state.start_level.is_started(),
      None => false,
    }
  }

  /// How often we actually send, which is well inside the interval the
  /// node allows.
  pub fn heartbeat_period_ms(&self) -> u32 {
    let mut period = self.heartbeat_interval_ms / IC_HEARTBEAT_DIVISOR;
    if period == 0 {
      period = 1000;
    }
    period
  }
}

/// Every link to every data node, and what keeps them up.
pub struct NodeManager {
  config: ClusterConfig,
  mgm: Option<MgmClient>,
  connect_string: ConnectString,
  mgm_timeout_ms: u32,
  links: BTreeMap<u32, NodeLink>,
  poll_set: PollSet,
}

impl std::fmt::Debug for NodeManager {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "NodeManager({} of {} data node(s) connected)",
      self.num_connected(),
      self.links.len()
    )
  }
}

impl NodeManager {
  /// A manager for every data node the configuration gives us a link
  /// to. Nothing is connected yet; [`poll`](Self::poll) does that.
  ///
  /// The management server is kept, because the port of a data node is
  /// asked of it on every attempt. `connect_string` lets a management
  /// connection that has gone away be made again, which matters for a
  /// client meant to run for weeks.
  pub fn new(
    config: ClusterConfig,
    mgm: MgmClient,
    connect_string: ConnectString,
    mgm_timeout_ms: u32,
  ) -> Result<NodeManager, IcError> {
    let mut links: BTreeMap<u32, NodeLink> = BTreeMap::new();
    for node_id in config.connectable_data_nodes() {
      let interval = match config.data_node(node_id) {
        Some(node) => node.api_heartbeat_interval_ms,
        None => 0,
      };
      links.insert(node_id, NodeLink::new(node_id, interval));
    }
    Ok(NodeManager {
      config,
      mgm: Some(mgm),
      connect_string,
      mgm_timeout_ms,
      links,
      poll_set: PollSet::new()?,
    })
  }

  /// The configuration this manager was built from.
  pub fn config(&self) -> &ClusterConfig {
    &self.config
  }

  /// Every link, by node id.
  pub fn links(&self) -> &BTreeMap<u32, NodeLink> {
    &self.links
  }

  /// One link.
  pub fn link(&self, node_id: u32) -> Option<&NodeLink> {
    self.links.get(&node_id)
  }

  /// How many data nodes answer our signals.
  pub fn num_connected(&self) -> u32 {
    let mut count: u32 = 0;
    for link in self.links.values() {
      if link.is_connected() {
        count += 1;
      }
    }
    count
  }

  /// The data nodes that answer our signals and say they are started.
  pub fn started_nodes(&self) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::new();
    for link in self.links.values() {
      if link.is_connected() && link.is_started() {
        out.push(link.node_id);
      }
    }
    out
  }

  /// Do one round of work, waiting up to `wait_ms` for a socket to say
  /// something: connect what is due, read what has arrived, and send
  /// the heartbeats that are due.
  ///
  /// Call it in a loop. It never blocks longer than asked, so a caller
  /// can do other work between rounds.
  pub fn poll(&mut self, wait_ms: u32) -> Result<(), IcError> {
    self.connect_due_links();
    self.read_ready_links(wait_ms)?;
    self.send_due_heartbeats();
    Ok(())
  }

  /// Connect every link whose retry time has come.
  fn connect_due_links(&mut self) {
    let now = ic_port::time::gethrtime();
    let mut due: Vec<u32> = Vec::new();
    for link in self.links.values() {
      if link.status == LinkStatus::Connected {
        continue;
      }
      if link.retry_at == 0 || now >= link.retry_at {
        due.push(link.node_id);
      }
    }
    for node_id in due {
      self.connect_link(node_id);
    }
  }

  fn connect_link(&mut self, node_id: u32) {
    // The port is asked for again every time. A node that restarts
    // comes back on a different one, and reports zero until it has
    // started, so an old port would connect to nothing or to someone
    // else.
    let result = match self.node_port(node_id) {
      Ok(port) => NodeConnection::connect_to_port(&self.config, node_id, port),
      Err(e) => Err(e),
    };
    let mut connection = match result {
      Ok(connection) => connection,
      Err(e) => {
        self.record_failure(node_id, e);
        return;
      }
    };
    // Watch the socket before the link counts as up. A connection
    // nobody watches would never be seen to close, which is the one
    // thing this module exists to notice.
    let fd = connection.fd();
    if let Err(e) = self.poll_set.add_connection(fd, node_id as usize) {
      connection.close();
      self.record_failure(node_id, e);
      return;
    }
    if let Some(link) = self.links.get_mut(&node_id) {
      link.node_state = connection.node_state;
      link.heartbeat_interval_ms = connection.heartbeat_interval_ms;
      link.connection = Some(connection);
      link.status = LinkStatus::Connected;
      link.failed_attempts = 0;
      link.last_error = None;
      link.heartbeat_at =
        ic_port::time::gethrtime() + period_nanos(link.heartbeat_period_ms());
      ic_port::debug_print!(IC_COMM_LEVEL, "Connected to node {}", node_id);
    }
  }

  /// Note that a link is down and when it should be tried again.
  fn record_failure(&mut self, node_id: u32, error: IcError) {
    if let Some(link) = self.links.get_mut(&node_id) {
      link.failed_attempts += 1;
      link.last_error = Some(error);
      let delay = retry_delay_ms(link.failed_attempts);
      link.retry_at = ic_port::time::gethrtime() + period_nanos(delay);
      ic_port::debug_print!(
        IC_COMM_LEVEL,
        "Node {} is down ({}); next attempt in {} ms",
        node_id,
        error.message(),
        delay
      );
    }
  }

  /// Wait for any socket to have something and read what is there.
  fn read_ready_links(&mut self, wait_ms: u32) -> Result<(), IcError> {
    if self.poll_set.is_empty() {
      // Nothing to watch; do not spin.
      ic_port::time::microsleep(wait_ms * 1000);
      return Ok(());
    }
    self.poll_set.check(wait_ms as i32)?;
    let mut ready: Vec<u32> = Vec::new();
    while let Some(conn) = self.poll_set.next_connection() {
      ready.push(conn.user_obj as u32);
    }
    for node_id in ready {
      self.read_link(node_id);
    }
    Ok(())
  }

  fn read_link(&mut self, node_id: u32) {
    let result = match self.links.get_mut(&node_id) {
      Some(link) => match link.connection.as_mut() {
        Some(connection) => connection.read_available(),
        None => return,
      },
      None => return,
    };
    let signals = match result {
      Ok(signals) => signals,
      Err(e) => {
        // The socket closed or failed. This is how a node going away is
        // noticed promptly rather than at the next send.
        self.drop_link(node_id, e);
        return;
      }
    };
    for signal in &signals {
      self.handle_signal(node_id, signal);
    }
  }

  fn handle_signal(&mut self, node_id: u32, signal: &ReceivedSignal) {
    if signal.gsn == gsn::IC_GSN_NODE_FAILREP {
      // The bitmap is in the first section unless the sender put it
      // inline after the three fixed words.
      let report = NodeFailRep::decode(&signal.data, signal.section(0));
      if let Ok(report) = report {
        ic_port::debug_print!(
          IC_HEARTBEAT_LEVEL,
          "Node {} reports {} node failure(s): {:?}",
          node_id,
          report.num_nodes,
          report.failed_nodes
        );
        for failed in &report.failed_nodes {
          self.mark_failing(*failed);
        }
      }
      return;
    }
    if signal.gsn == gsn::IC_GSN_NF_COMPLETEREP {
      if let Ok(report) = NfCompleteRep::decode(&signal.data) {
        if report.is_whole_node() {
          // Once there are transactions to recover, this is where the
          // ones that were waiting on the failed node are settled.
          ic_port::debug_print!(
            IC_HEARTBEAT_LEVEL,
            "Node {} reports node {} fully taken over",
            node_id,
            report.failed_node_id
          );
        }
      }
      return;
    }
    if let Some(link) = self.links.get_mut(&node_id) {
      if let Some(connection) = link.connection.as_mut() {
        if connection.apply_signal(signal.gsn, &signal.data) {
          link.node_state = connection.node_state;
          link.heartbeat_interval_ms = connection.heartbeat_interval_ms;
        }
      }
    }
  }

  fn mark_failing(&mut self, node_id: u32) {
    let should_drop = match self.links.get(&node_id) {
      Some(link) => link.status == LinkStatus::Connected,
      None => return,
    };
    if should_drop {
      // Another node saw the failure before our own socket did.
      self.drop_link(node_id, IcError::new(ic_port::err::IC_ERROR_NODE_DOWN));
    }
    if let Some(link) = self.links.get_mut(&node_id) {
      link.status = LinkStatus::Failing;
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "Node {} has failed; its work is being taken over",
        node_id
      );
    }
  }

  /// Give up on a link and arrange for it to be retried.
  fn drop_link(&mut self, node_id: u32, error: IcError) {
    // Stop watching the socket before closing it, or the poll set
    // would be left holding a descriptor number that the next
    // connection may reuse.
    let mut fd: i32 = -1;
    if let Some(link) = self.links.get(&node_id) {
      if let Some(connection) = link.connection.as_ref() {
        fd = connection.fd();
      }
    }
    if fd >= 0 {
      let _ = self.poll_set.remove_connection(fd);
    }
    if let Some(link) = self.links.get_mut(&node_id) {
      if let Some(connection) = link.connection.as_mut() {
        connection.close();
      }
      link.connection = None;
      link.node_state = None;
      link.status = LinkStatus::Disconnected;
    }
    self.record_failure(node_id, error);
  }

  /// Send a heartbeat on every link whose turn has come.
  fn send_due_heartbeats(&mut self) {
    let now = ic_port::time::gethrtime();
    let mut due: Vec<u32> = Vec::new();
    for link in self.links.values() {
      if link.status != LinkStatus::Connected {
        continue;
      }
      if now >= link.heartbeat_at {
        due.push(link.node_id);
      }
    }
    for node_id in due {
      let result = match self.links.get_mut(&node_id) {
        Some(link) => match link.connection.as_mut() {
          Some(connection) => connection.send_heartbeat(),
          None => continue,
        },
        None => continue,
      };
      match result {
        Ok(()) => {
          if let Some(link) = self.links.get_mut(&node_id) {
            link.heartbeat_at = ic_port::time::gethrtime()
              + period_nanos(link.heartbeat_period_ms());
          }
        }
        Err(e) => self.drop_link(node_id, e),
      }
    }
  }

  /// Ask the management server which port a data node listens on.
  ///
  /// A management server can itself stop or drop an idle connection.
  /// When the question cannot be put, the connection is made again and
  /// the question asked once more, so that a management server
  /// restarting does not leave every data node unreachable for good.
  fn node_port(&mut self, node_id: u32) -> Result<u16, IcError> {
    let link = match self.config.link_to(node_id) {
      Some(link) => link.clone(),
      None => return Err(IcError::new(ic_port::err::IC_ERROR_NO_SUCH_NODE)),
    };
    if !link.port_is_dynamic() {
      return Ok(link.server_port);
    }
    let own_node_id = self.config.api.node_id;
    if let Some(mgm) = self.mgm.as_mut() {
      match resolve_port(&link, own_node_id, mgm) {
        Ok(port) => return Ok(port),
        Err(e) => {
          if e.code == ic_port::err::IC_ERROR_NODE_DOWN {
            // The node has not reported a port yet, which says nothing
            // about the management server.
            return Err(e);
          }
          ic_port::debug_print!(
            IC_COMM_LEVEL,
            "Management server did not answer ({}); connecting again",
            e.message()
          );
          self.mgm = None;
        }
      }
    }
    self.renew_mgm()?;
    match self.mgm.as_mut() {
      Some(mgm) => resolve_port(&link, own_node_id, mgm),
      None => Err(IcError::new(ic_port::err::IC_ERROR_MGM_SERVER_REFUSED)),
    }
  }

  /// Connect to a management server again, trying each in the
  /// connectstring.
  fn renew_mgm(&mut self) -> Result<(), IcError> {
    let client =
      MgmClient::connect_any(&self.connect_string, self.mgm_timeout_ms)?;
    let server = client.server();
    ic_port::debug_print!(
      IC_COMM_LEVEL,
      "Connected to management server {}:{} again",
      server.host,
      server.port
    );
    self.mgm = Some(client);
    Ok(())
  }

  /// Close every connection.
  pub fn close(&mut self) {
    for link in self.links.values_mut() {
      if let Some(connection) = link.connection.as_mut() {
        let _ = self.poll_set.remove_connection(connection.fd());
        connection.close();
      }
      link.connection = None;
      link.node_state = None;
      link.status = LinkStatus::Disconnected;
    }
  }
}

/// How long to wait before the next attempt: longer each time it
/// fails, up to a ceiling, so a node that is down does not keep us
/// busy.
fn retry_delay_ms(failed_attempts: u32) -> u32 {
  let mut delay = IC_FIRST_RETRY_MS;
  let mut i: u32 = 1;
  while i < failed_attempts && delay < IC_MAX_RETRY_MS {
    delay *= 2;
    i += 1;
  }
  if delay > IC_MAX_RETRY_MS {
    return IC_MAX_RETRY_MS;
  }
  delay
}

/// Milliseconds as the nanoseconds the timers count in.
fn period_nanos(ms: u32) -> IcTimer {
  (ms as IcTimer) * 1_000_000
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn the_retry_delay_grows_and_stops_growing() {
    assert_eq!(retry_delay_ms(0), IC_FIRST_RETRY_MS);
    assert_eq!(retry_delay_ms(1), 1000);
    assert_eq!(retry_delay_ms(2), 2000);
    assert_eq!(retry_delay_ms(3), 4000);
    assert_eq!(retry_delay_ms(4), 8000);
    // It stops at the ceiling however long the node stays away.
    assert_eq!(retry_delay_ms(5), IC_MAX_RETRY_MS);
    assert_eq!(retry_delay_ms(500), IC_MAX_RETRY_MS);
  }

  #[test]
  fn heartbeats_go_out_well_inside_the_interval() {
    let link = NodeLink::new(2, 30000);
    // A data node declares us dead after its interval, so we send at a
    // fraction of it.
    assert_eq!(link.heartbeat_period_ms(), 6000);
    let fast = NodeLink::new(2, 1500);
    assert_eq!(fast.heartbeat_period_ms(), 300);
    // A node that reports nothing still gets heartbeats.
    let unset = NodeLink::new(2, 0);
    assert_eq!(unset.heartbeat_period_ms(), 1000);
  }

  #[test]
  fn a_new_link_is_ready_to_connect() {
    let link = NodeLink::new(2, 30000);
    assert_eq!(link.status, LinkStatus::Disconnected);
    assert!(!link.is_connected());
    assert!(!link.is_started());
    assert_eq!(link.retry_at, 0);
    assert_eq!(link.failed_attempts, 0);
  }

  #[test]
  fn timers_count_in_nanoseconds() {
    assert_eq!(period_nanos(1), 1_000_000);
    assert_eq!(period_nanos(6000), 6_000_000_000);
  }
}
