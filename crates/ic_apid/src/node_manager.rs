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
//! - heartbeats go out on schedule, and a node that keeps its socket
//!   open but stops answering them is declared lost.
//!
//! # Losing a link is not losing a node
//!
//! This part follows the data nodes' protocol, not the C. The C handles
//! neither `NODE_FAILREP` nor `NF_COMPLETEREP` and treats every closed
//! socket alike.
//!
//! There are two kinds of evidence and they mean different things.
//!
//! **Our own evidence**: the socket closed, a send failed, the
//! heartbeats stopped. That says our link to the node is gone. It says
//! nothing about the node, which may be up and serving every other
//! client, and a data node does nothing special when it loses an API
//! link. Nobody will ever report a takeover for a node that did not
//! fail, so the link is simply dialled again on the usual delay. The
//! error for it is `IC_ERROR_LINK_LOST`, not a node failure and
//! certainly not a cluster failure. The C++ API reports this state to
//! applications as error 4009, "cluster failure", which it is not.
//!
//! **The cluster's evidence**: a data node sends `NODE_FAILREP` naming
//! the node. Now the node is known to have failed, and it is **not
//! dialled again until a surviving data node reports, with
//! `NF_COMPLETEREP`, that the failure has been fully handled**. Every
//! surviving data node sends that report to every API node once all its
//! blocks are done. Two things depend on the wait. The report is what
//! tells an API node that no more will be heard about transactions that
//! were running on the failed node, so they can be aborted. And
//! dialling early would let us reach the restarted node before noticing
//! that the rest of the cluster had gone too, so that a whole cluster
//! restart would look like one node bouncing.
//!
//! The two usually arrive together when a node dies: the socket closes
//! and a moment later the report comes. Dialling in between is
//! harmless, because a dead node refuses the connection, and the report
//! then stops the dialling.
//!
//! If no data node is connected at all there is nobody to send either
//! report, so every wait ends by itself.
//!
//! # Losing every link can lose the node id
//!
//! Once we are connected to data nodes, our claim on our node id is
//! those connections and nothing else: the management server's own
//! reservation is dropped as soon as the id is held by a connected
//! transporter. Lose every link and, once the data nodes have finished
//! handling our disappearance, the id is free. An id written in the
//! connectstring is asked for by nobody else. An id the management
//! server chose for us can be given to the next API node that asks for
//! any id, and dialling a data node with it then would present two
//! nodes under one id.
//!
//! So after losing every link, a node that can accept any id goes back
//! to the management server before dialling anyone. It asks for the
//! same id first, which keeps its identity when the id is free, and
//! takes another at once when the server says the id is held, together
//! with the configuration as that new node sees it. "Held" usually
//! means held by our own old connection, which the data nodes have not
//! yet timed out; waiting for that can take minutes.
//!
//! A node started with a stated node id never does any of this. That
//! id is the only one it may run under, so it redials under it and is
//! turned away until the data nodes have let the old connection go.
//!
//! Verify: `MgmtSrvr.cpp`, the release of the local reservation on
//! `CONNECT_REP`, `NODE_FAILREP` and `NF_COMPLETEREP`, and
//! `alloc_node_id_req`, which asks the data nodes. The C++ API claims
//! an id once, in `ndb_cluster_connection.cpp`, and not again.
//!
//! Verify: `ClusterMgr.cpp`, the connect gate at the top of the node
//! loop in `threadMain`, `execNODE_FAILREP` and `execNF_COMPLETEREP`;
//! `QmgrMain.cpp`, `execNDB_FAILCONF`, for the data node's side. One
//! deliberate difference: the C++ API treats its own disconnect as a
//! failure report and then waits for a takeover report, which for a
//! link-only loss never comes. Confirmed with the author of the
//! protocol that the data node has no handling that would resolve it.
//!
//! # Threads
//!
//! In this release one thread drives all of it through [`poll`]. The
//! threaded form of the design keeps the same states and transitions,
//! with a connect thread per node and receive threads owning the poll
//! sets. Until then, connecting happens on the polling thread, so a
//! host that swallows packets rather than refusing them can hold up a
//! round for as long as the connect timeout. A node that is merely down
//! refuses at once and costs nothing.
//!
//! [`poll`]: NodeManager::poll

use std::collections::BTreeMap;
use std::sync::Arc;

use ic_apic::data::ClusterConfig;
use ic_apic::mgm_client;
use ic_apic::mgm_client::MgmClient;
use ic_comm::poll_set::PollSet;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::qmgr::NfCompleteRep;
use ic_ndb_signals::qmgr::NodeFailRep;
use ic_ndb_signals::qmgr::NodeState;
use ic_port::debug::IC_COMM_LEVEL;
use ic_port::debug::IC_HEARTBEAT_LEVEL;
use ic_port::err;
use ic_port::time::IcTimer;
use ic_port::IcError;
use ic_util::connectstring::ConnectString;

use crate::node_connect::resolve_port;
use crate::node_connect::NodeConnection;
use crate::node_connect::ReceivedSignal;
use crate::node_state::PublishedNodeState;
use crate::thread_conn::Router;
use crate::thread_conn::ThreadTable;

/// How long to wait before the first retry of a failed link.
pub const IC_FIRST_RETRY_MS: u32 = 1000;
/// The longest the retry delay grows to.
pub const IC_MAX_RETRY_MS: u32 = 10_000;
/// Heartbeats go out at least this many times per check interval, so
/// that a late or lost round still leaves the interval with heartbeats
/// in it.
///
/// A deliberate difference: the C++ API sends twice per interval, and
/// the author of the protocol judges that too seldom (2026-09-19).
/// Sending more often than the other side expects is always safe; the
/// data node only ever counts intervals in which it heard nothing.
/// Verify the C++ rule: `ClusterMgr.cpp`, `get_send_heartbeat_interval`.
pub const IC_HEARTBEATS_PER_INTERVAL: u32 = 3;

// Fewer than three is the rule this constant exists to prevent.
const _: () = assert!(IC_HEARTBEATS_PER_INTERVAL >= 3);
/// The shortest check interval, whatever a node reports.
/// Verify: `ClusterMgr.hpp`, the minimum heartbeat interval.
pub const IC_MIN_HEARTBEAT_INTERVAL_MS: u32 = 100;
/// A node is lost at this many check intervals in a row without an
/// answer, which is at least three whole intervals of silence.
/// Verify: `ClusterMgr.cpp`, the missed heartbeat test in `threadMain`.
pub const IC_MAX_MISSED_HEARTBEATS: u32 = 4;

/// Where a link to one data node stands.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum LinkStatus {
  /// Not connected, and being dialled on a growing delay. Either it
  /// has never been connected, or our link to it broke and no data node
  /// has said the node itself failed, or its failure has been reported
  /// as handled.
  #[default]
  Disconnected,
  /// Connected and registered; the node answers our signals.
  Connected,
  /// A data node has reported that this node failed. It is not dialled
  /// until a surviving data node reports the failure fully handled.
  AwaitingTakeover,
}

/// One link to one data node, and what we know about it.
pub struct NodeLink {
  /// The data node.
  pub node_id: u32,
  /// Where the link stands.
  pub status: LinkStatus,
  /// What the node last told us about itself. This is the whole reply
  /// and belongs to the thread driving the link; other threads read
  /// [`published`](Self::published) instead.
  pub node_state: Option<NodeState>,
  /// How many times connecting has failed since it last worked.
  pub failed_attempts: u32,
  /// The last error, for reporting.
  pub last_error: Option<IcError>,
  /// The node's heartbeat check interval.
  pub heartbeat_interval_ms: u32,
  /// Check intervals in a row that ended without an answer.
  pub missed_heartbeats: u32,
  /// The node's state as other threads may read it, without a lock.
  pub published: Arc<PublishedNodeState>,
  connection: Option<NodeConnection>,
  /// True once a data node has reported this node failed, until we are
  /// next connected to it. Every surviving node reports the same
  /// failure, and this is what makes all but the first do nothing.
  failure_reported: bool,
  retry_at: IcTimer,
  heartbeat_at: IcTimer,
  check_at: IcTimer,
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
      // A node that was never connected has no failure to wait out.
      status: LinkStatus::Disconnected,
      node_state: None,
      failed_attempts: 0,
      last_error: None,
      heartbeat_interval_ms,
      missed_heartbeats: 0,
      published: Arc::new(PublishedNodeState::new()),
      connection: None,
      failure_reported: false,
      // Nothing to wait for before the first attempt.
      retry_at: 0,
      heartbeat_at: 0,
      check_at: 0,
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

  /// The check interval with the floor applied.
  pub fn check_interval_ms(&self) -> u32 {
    if self.heartbeat_interval_ms < IC_MIN_HEARTBEAT_INTERVAL_MS {
      return IC_MIN_HEARTBEAT_INTERVAL_MS;
    }
    self.heartbeat_interval_ms
  }

  /// How often we send a heartbeat. The division rounds down, which
  /// errs towards sending slightly more often, never less.
  pub fn heartbeat_period_ms(&self) -> u32 {
    self.check_interval_ms() / IC_HEARTBEATS_PER_INTERVAL
  }
}

/// Every link to every data node, and what keeps them up.
pub struct NodeManager {
  config: ClusterConfig,
  mgm: Option<MgmClient>,
  connect_string: ConnectString,
  mgm_timeout_ms: u32,
  /// The name we give the management server when claiming a node id.
  node_name: Option<String>,
  /// True when any node id will do, so the management server chose ours.
  /// False when the application was started with a stated node id, in
  /// which case that id is the only one we ever run under: we never ask
  /// for another, however long the stated one is unavailable.
  node_id_is_dynamic: bool,
  /// True from losing every link until the node id has been claimed
  /// again. Nothing is dialled while it is set.
  must_reclaim_node_id: bool,
  /// Failed attempts of any kind, which set the delay before the next.
  reclaim_attempts: u32,
  reclaim_at: IcTimer,
  links: BTreeMap<u32, NodeLink>,
  poll_set: PollSet,
  /// Where user threads get their inboxes.
  thread_table: Arc<ThreadTable>,
  /// Sorts what arrives by the user thread it is for. This thread is
  /// the receive thread for now, so the router is ours.
  router: Router,
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
  /// client meant to run for weeks. It also says whether our node id
  /// was written in it or chosen for us, which decides what has to
  /// happen after losing every link. `node_name` is what the management
  /// server logs against our node id.
  pub fn new(
    config: ClusterConfig,
    mgm: MgmClient,
    connect_string: ConnectString,
    mgm_timeout_ms: u32,
    node_name: Option<&str>,
  ) -> Result<NodeManager, IcError> {
    let mut links: BTreeMap<u32, NodeLink> = BTreeMap::new();
    for node_id in config.connectable_data_nodes() {
      let interval = match config.data_node(node_id) {
        Some(node) => node.api_heartbeat_interval_ms,
        None => 0,
      };
      links.insert(node_id, NodeLink::new(node_id, interval));
    }
    let node_id_is_dynamic = connect_string.node_id.is_none();
    let thread_table = Arc::new(ThreadTable::new());
    let router = Router::new(Arc::clone(&thread_table));
    let mut owned_name: Option<String> = None;
    if let Some(name) = node_name {
      owned_name = Some(name.to_string());
    }
    Ok(NodeManager {
      config,
      mgm: Some(mgm),
      connect_string,
      mgm_timeout_ms,
      node_name: owned_name,
      node_id_is_dynamic,
      must_reclaim_node_id: false,
      reclaim_attempts: 0,
      reclaim_at: 0,
      links,
      poll_set: PollSet::new()?,
      thread_table,
      router,
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

  /// A node's state as any thread may read it. The handle stays good
  /// for the life of the manager, across any number of reconnects.
  pub fn published(&self, node_id: u32) -> Option<Arc<PublishedNodeState>> {
    // `?` on an Option: no such node, so return None now.
    let link = self.links.get(&node_id)?;
    Some(Arc::clone(&link.published))
  }

  /// Our own node id. It can change after every link has been lost, if
  /// any id will do; see the module note.
  pub fn own_node_id(&self) -> u32 {
    self.config.api.node_id
  }

  /// The table user threads take their inboxes from. A signal a data
  /// node addresses to a user thread's block ends up in that thread's
  /// inbox; this manager never looks inside it.
  pub fn thread_table(&self) -> Arc<ThreadTable> {
    Arc::clone(&self.thread_table)
  }

  /// Send a signal to a data node.
  ///
  /// For the thread that drives [`poll`](Self::poll), which for now is
  /// also the only thread that can send. With the send path of the
  /// thread design, a user thread sends for itself.
  pub fn send(
    &mut self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
  ) -> Result<(), IcError> {
    let result = match self.links.get_mut(&node_id) {
      Some(link) => match link.connection.as_mut() {
        Some(connection) => connection.send_signal(header, data, &[]),
        None => {
          // Say what is known and no more: a node the cluster reported
          // failed is down, anything else is only a link we lack.
          if link.status == LinkStatus::AwaitingTakeover {
            return Err(IcError::new(err::IC_ERROR_NODE_DOWN));
          }
          return Err(IcError::new(err::IC_ERROR_LINK_LOST));
        }
      },
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    if let Err(e) = result {
      self.link_lost(node_id, e);
      return Err(e);
    }
    Ok(())
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
  /// something: connect what is due, read what has arrived, send the
  /// heartbeats that are due, and notice the nodes that stopped
  /// answering them.
  ///
  /// Call it in a loop. It never blocks longer than asked, so a caller
  /// can do other work between rounds.
  pub fn poll(&mut self, wait_ms: u32) -> Result<(), IcError> {
    self.connect_due_links();
    self.read_ready_links(wait_ms)?;
    self.send_due_heartbeats();
    self.check_missed_heartbeats();
    Ok(())
  }

  // ---- Connecting ----

  /// Connect every link whose retry time has come.
  fn connect_due_links(&mut self) {
    if self.must_reclaim_node_id && !self.reclaim_node_id() {
      // Dialling under an id that may now be someone else's is worse
      // than not dialling.
      return;
    }
    let now = ic_port::time::gethrtime();
    let mut due: Vec<u32> = Vec::new();
    for link in self.links.values() {
      // A node awaiting its takeover report is not dialled, however
      // long its retry time has been past.
      if link.status != LinkStatus::Disconnected {
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
    // Registration ends with the node describing itself, so a
    // connection without a state is one that did not register.
    let state = match connection.node_state {
      Some(state) => state,
      None => {
        connection.close();
        self.record_failure(node_id, IcError::new(err::IC_ERROR_NODE_DOWN));
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
      let now = ic_port::time::gethrtime();
      link.node_state = Some(state);
      link.heartbeat_interval_ms = connection.heartbeat_interval_ms;
      link.connection = Some(connection);
      link.status = LinkStatus::Connected;
      link.failure_reported = false;
      link.failed_attempts = 0;
      link.missed_heartbeats = 0;
      link.last_error = None;
      link.heartbeat_at = now + period_nanos(link.heartbeat_period_ms());
      link.check_at = now + period_nanos(link.check_interval_ms());
      // Last, so that a reader seeing "connected" finds a link that is.
      link.published.publish_connected(&state, now);
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

  // ---- Receiving ----

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
    // One post per round, after every ready socket has been read, so
    // that a user thread hearing from several nodes is locked and woken
    // once. The C posts after each node and notes that how often to
    // post "needs clever scheduling principles"; once per round is the
    // obvious next step from there.
    self.router.post_all();
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
        // The socket closed or failed, which we learn now rather than
        // at the next send.
        self.link_lost(node_id, e);
        return;
      }
    };
    for signal in signals {
      // What is for a user thread goes to its inbox unread. What comes
      // back is for one of our own fixed blocks and is about the node
      // that sent it, which is ours to execute.
      if let Some(own) = self.router.route(signal) {
        self.handle_signal(node_id, &own);
      }
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
          self.node_failure_reported(*failed);
        }
      }
      return;
    }
    if signal.gsn == gsn::IC_GSN_NF_COMPLETEREP {
      if let Ok(report) = NfCompleteRep::decode(&signal.data) {
        self.takeover_reported(node_id, report.failed_node_id);
      }
      return;
    }
    if let Some(link) = self.links.get_mut(&node_id) {
      if let Some(connection) = link.connection.as_mut() {
        if connection.apply_signal(signal.gsn, &signal.data) {
          // The node answered a heartbeat.
          link.missed_heartbeats = 0;
          link.node_state = connection.node_state;
          link.heartbeat_interval_ms = connection.heartbeat_interval_ms;
          if let Some(state) = connection.node_state {
            link
              .published
              .publish_regconf(&state, ic_port::time::gethrtime());
          }
        }
      }
    }
  }

  // ---- Losing a link, losing a node ----

  /// Close a link's socket and publish it as down. Returns false if
  /// there was nothing to close.
  fn tear_down(&mut self, node_id: u32) -> bool {
    // Stop watching the socket before closing it, or the poll set
    // would be left holding a descriptor number that the next
    // connection may reuse.
    let mut fd: i32 = -1;
    if let Some(link) = self.links.get(&node_id) {
      if let Some(connection) = link.connection.as_ref() {
        fd = connection.fd();
      }
    }
    if fd < 0 {
      return false;
    }
    let _ = self.poll_set.remove_connection(fd);
    if let Some(link) = self.links.get_mut(&node_id) {
      // First, so that no reader sends to a node we are tearing down.
      link.published.publish_down();
      if let Some(connection) = link.connection.as_mut() {
        connection.close();
      }
      link.connection = None;
      link.node_state = None;
      link.missed_heartbeats = 0;
    }
    true
  }

  /// Our own evidence: the socket closed, a send failed, or the
  /// heartbeats stopped. The link is gone. Whether the node is, we do
  /// not know, so it is dialled again on the usual delay.
  fn link_lost(&mut self, node_id: u32, error: IcError) {
    if !self.tear_down(node_id) {
      return;
    }
    if let Some(link) = self.links.get_mut(&node_id) {
      link.status = LinkStatus::Disconnected;
      link.failed_attempts = 0;
    }
    // Sets the error and the time of the first attempt.
    self.record_failure(node_id, error);
    self.end_waits_if_alone();
  }

  /// The cluster's evidence: a data node says this node failed. It is
  /// not dialled again until the failure is reported as handled.
  fn node_failure_reported(&mut self, node_id: u32) {
    match self.links.get_mut(&node_id) {
      Some(link) => {
        if link.failure_reported {
          // Another node reporting the failure we already know of.
          return;
        }
        link.failure_reported = true;
      }
      // A node we have no link to, such as another API node.
      None => return,
    }
    // The report can beat our own socket to it.
    self.tear_down(node_id);
    if let Some(link) = self.links.get_mut(&node_id) {
      link.status = LinkStatus::AwaitingTakeover;
      link.failed_attempts = 0;
      link.last_error = Some(IcError::new(err::IC_ERROR_NODE_DOWN));
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "Node {} has failed; not dialling until that has been handled",
        node_id
      );
    }
    self.end_waits_if_alone();
  }

  /// A surviving data node says a failure has been fully handled, so
  /// the failed node may be dialled again.
  fn takeover_reported(&mut self, reporter: u32, failed_node_id: u32) {
    if let Some(link) = self.links.get_mut(&failed_node_id) {
      // Every surviving node sends one, so all but the first find the
      // wait already over.
      if link.status != LinkStatus::AwaitingTakeover {
        return;
      }
      link.status = LinkStatus::Disconnected;
      link.retry_at = 0;
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "Node {} reports the failure of node {} fully handled; dialling",
        reporter,
        failed_node_id
      );
    }
  }

  /// With no data node connected there is nobody to report a failure
  /// or its handling, so every wait ends here.
  fn end_waits_if_alone(&mut self) {
    if self.num_connected() != 0 {
      return;
    }
    for link in self.links.values_mut() {
      if link.status == LinkStatus::AwaitingTakeover {
        link.status = LinkStatus::Disconnected;
        link.retry_at = 0;
      }
    }
    ic_port::debug_print!(
      IC_HEARTBEAT_LEVEL,
      "No data node is connected; the whole cluster is gone from here"
    );
    if self.node_id_is_dynamic && !self.must_reclaim_node_id {
      // Our connections were our only claim on the id.
      self.must_reclaim_node_id = true;
      self.reclaim_attempts = 0;
      self.reclaim_at = 0;
    }
  }

  // ---- Our own node id ----

  /// Claim a node id again after losing every link. True when we hold
  /// one and may dial.
  fn reclaim_node_id(&mut self) -> bool {
    let now = ic_port::time::gethrtime();
    if self.reclaim_at != 0 && now < self.reclaim_at {
      return false;
    }
    let old_node_id = self.config.api.node_id;
    let name = self.node_name.clone();
    // The same id first. If it is still free we stay who we were.
    let mut wanted = self.connect_string.clone();
    wanted.node_id = Some(old_node_id);
    let mut fetched = mgm_client::fetch_configuration(
      &wanted,
      self.mgm_timeout_ms,
      name.as_deref(),
    );
    // There are three kinds of no. "Not now" is asked again for as long
    // as it takes; seen live, a restarting cluster says it for several
    // seconds. The other two make us take another id at once, which we
    // may do because this whole function only runs for a node that can
    // accept any id.
    //
    // "Held by another node" means only that the cluster counts the id
    // as connected, and after a link loss the other node is usually
    // ourselves: the data nodes have not noticed we went away, and will
    // not for four heartbeat intervals. Waiting that out can take
    // minutes. Taking a new id has us back in seconds, and this is the
    // cheapest moment there is to change identity, since with every
    // link lost nothing is in flight. The old id holds an API slot until
    // it times out; if that ever leaves no id free, the request for any
    // id is refused as "not now" and simply asked again.
    let mut refusal: i32 = 0;
    if let Err(e) = &fetched {
      refusal = e.code;
    }
    let take_another = refusal == err::IC_ERROR_NODEID_IN_USE
      || refusal == err::IC_ERROR_NODEID_NOT_ALLOWED;
    if take_another {
      if refusal == err::IC_ERROR_NODEID_IN_USE {
        ic_port::debug_print!(
          IC_COMM_LEVEL,
          "The cluster still counts node id {} as connected; asking for \
           another",
          old_node_id
        );
      } else {
        ic_port::debug_print!(
          IC_COMM_LEVEL,
          "The configuration no longer allows node id {}; asking for \
           another",
          old_node_id
        );
      }
      wanted.node_id = None;
      fetched = mgm_client::fetch_configuration(
        &wanted,
        self.mgm_timeout_ms,
        name.as_deref(),
      );
    }
    let (config, client) = match fetched {
      Ok(pair) => pair,
      Err(e) => {
        self.reclaim_attempts += 1;
        let delay = retry_delay_ms(self.reclaim_attempts);
        self.reclaim_at = now + period_nanos(delay);
        // The server's words only when it was the server that said no;
        // after an unreachable server they would be stale.
        let mut words = String::new();
        if mgm_client::is_refusal(e.code) {
          words = mgm_client::last_refusal();
        }
        ic_port::debug_print!(
          IC_COMM_LEVEL,
          "Could not claim a node id ({}) {}; next attempt in {} ms",
          e.message(),
          words,
          delay
        );
        return false;
      }
    };
    ic_port::debug_print!(
      IC_COMM_LEVEL,
      "Holding node id {} (was {})",
      config.api.node_id,
      old_node_id
    );
    self.adopt_configuration(config);
    self.mgm = Some(client);
    self.must_reclaim_node_id = false;
    self.reclaim_attempts = 0;
    self.reclaim_at = 0;
    true
  }

  /// Take a configuration fetched again, possibly as a different node.
  /// Only called with no link connected, so there is nothing to close.
  fn adopt_configuration(&mut self, config: ClusterConfig) {
    let mut links: BTreeMap<u32, NodeLink> = BTreeMap::new();
    for node_id in config.connectable_data_nodes() {
      let interval = match config.data_node(node_id) {
        Some(node) => node.api_heartbeat_interval_ms,
        None => 0,
      };
      // Keep the link we had, so that a handle on its published state
      // taken before the outage still follows the node after it.
      let link = match self.links.remove(&node_id) {
        Some(link) => link,
        None => NodeLink::new(node_id, interval),
      };
      links.insert(node_id, link);
    }
    self.links = links;
    self.config = config;
  }

  // ---- Heartbeats ----

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
        Err(e) => self.link_lost(node_id, e),
      }
    }
  }

  /// Count the check intervals that ended without an answer, and lose
  /// the nodes that have gone too many in a row.
  ///
  /// This catches what the poll set cannot: a node whose process has
  /// hung, or a network that drops packets without resetting the
  /// connection. The socket stays open and nothing arrives on it.
  fn check_missed_heartbeats(&mut self) {
    let now = ic_port::time::gethrtime();
    let mut silent: Vec<u32> = Vec::new();
    for link in self.links.values_mut() {
      if link.status != LinkStatus::Connected || now < link.check_at {
        continue;
      }
      // An answer sets the count back to zero, so at rest it moves
      // between zero and one.
      link.missed_heartbeats += 1;
      link.check_at = now + period_nanos(link.check_interval_ms());
      if link.missed_heartbeats >= 2 {
        ic_port::debug_print!(
          IC_HEARTBEAT_LEVEL,
          "Node {} has missed {} heartbeat(s)",
          link.node_id,
          link.missed_heartbeats - 1
        );
      }
      if link.missed_heartbeats >= IC_MAX_MISSED_HEARTBEATS {
        silent.push(link.node_id);
      }
    }
    for node_id in silent {
      self.link_lost(node_id, IcError::new(err::IC_ERROR_HEARTBEAT_MISSED));
    }
  }

  // ---- The management server ----

  /// Ask the management server which port a data node listens on.
  ///
  /// A management server can itself stop or drop an idle connection.
  /// When the question cannot be put, the connection is made again and
  /// the question asked once more, so that a management server
  /// restarting does not leave every data node unreachable for good.
  fn node_port(&mut self, node_id: u32) -> Result<u16, IcError> {
    let link = match self.config.link_to(node_id) {
      Some(link) => link.clone(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    if !link.port_is_dynamic() {
      return Ok(link.server_port);
    }
    let own_node_id = self.config.api.node_id;
    if let Some(mgm) = self.mgm.as_mut() {
      match resolve_port(&link, own_node_id, mgm) {
        Ok(port) => return Ok(port),
        Err(e) => {
          if e.code == err::IC_ERROR_NODE_DOWN {
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
      None => Err(IcError::new(err::IC_ERROR_MGM_SERVER_REFUSED)),
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
      link.published.publish_down();
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
  fn heartbeats_go_out_at_least_three_times_per_check_interval() {
    let link = NodeLink::new(2, 30000);
    assert_eq!(link.check_interval_ms(), 30000);
    assert_eq!(link.heartbeat_period_ms(), 10000);
    let fast = NodeLink::new(2, 1500);
    assert_eq!(fast.heartbeat_period_ms(), 500);
    // An interval that does not divide evenly rounds the period down,
    // so three sends still fit inside it.
    let odd = NodeLink::new(2, 1000);
    assert_eq!(odd.heartbeat_period_ms(), 333);
    assert!(odd.heartbeat_period_ms() * 3 <= odd.check_interval_ms());
  }

  #[test]
  fn a_tiny_or_missing_interval_is_raised_to_the_floor() {
    // Without the floor a node reporting nothing would be sent a
    // heartbeat on every round, and declared lost in no time at all.
    let unset = NodeLink::new(2, 0);
    assert_eq!(unset.check_interval_ms(), IC_MIN_HEARTBEAT_INTERVAL_MS);
    assert_eq!(unset.heartbeat_period_ms(), 33);
    let tiny = NodeLink::new(2, 7);
    assert_eq!(tiny.check_interval_ms(), IC_MIN_HEARTBEAT_INTERVAL_MS);
  }

  #[test]
  fn a_new_link_is_ready_to_connect() {
    // A node that was never connected has no failure to wait out, so it
    // starts out being dialled rather than awaiting a takeover.
    let link = NodeLink::new(2, 30000);
    assert_eq!(link.status, LinkStatus::Disconnected);
    assert!(!link.is_connected());
    assert!(!link.is_started());
    assert!(!link.failure_reported);
    assert_eq!(link.retry_at, 0);
    assert_eq!(link.failed_attempts, 0);
    assert!(!link.published.is_connected());
  }

  #[test]
  fn timers_count_in_nanoseconds() {
    assert_eq!(period_nanos(1), 1_000_000);
    assert_eq!(period_nanos(6000), 6_000_000_000);
  }
}
