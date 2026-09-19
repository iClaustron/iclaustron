// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The object every Data API thread works from
//! (`legacy-c/api/ic_apid_global.ic`, `IC_INT_APID_GLOBAL`).
//!
//! [`ApidGlobal::start`] starts the threads that keep an API node in a
//! cluster, and from then on nothing has to be driven by the
//! application:
//!
//! - **a connect thread per data node** ([`connect_thread`]) dials its
//!   node, completes the handshake and the registration, and hands the
//!   link to the receive thread. It dials again whenever the link is
//!   lost, unless the cluster has said the node failed and the failure
//!   has not yet been handled.
//! - **a receive thread** ([`rec_thread`]) owns the poll set and every
//!   link's reading half. It routes what arrives to the user thread it
//!   is for, executes what is about the node it came from, and is the
//!   one writer of every node's published state.
//! - **a heartbeat thread** ([`heartbeat`]) sends `API_REGREQ` to every
//!   connected node and asks for a link to be dropped when its node has
//!   stopped answering.
//!
//! User threads take an inbox from [`ApidGlobal::thread_table`], send
//! with [`ApidGlobal::send`], and wait on their inbox. They never read a
//! socket.
//!
//! # What is shared, and how
//!
//! Per data node, [`NodeShared`]:
//!
//! - the published link state, written only by the receive thread that
//!   owns the node and read by everyone through atomics;
//! - the membership word, which says whether the node may be dialled.
//!   It is the one piece of node state with more than one writer, since
//!   evidence of a failure can arrive on any link; every change is a
//!   compare-and-swap, so the first evidence wins and the rest do
//!   nothing;
//! - the sending half of the link, behind the node's mutex, which every
//!   sender takes for the length of one write;
//! - the handover slot a connect thread leaves a new link in.
//!
//! Once, for the whole API node, the identity: our node id, the
//! configuration as that node sees it, and the management client. It
//! changes only when every link has been lost and a new node id taken,
//! and a counter that goes up with every change lets a connect thread
//! tell whether the link it just made was made under the identity we
//! still have.
//!
//! No thread ever holds two of these mutexes at once, so their levels
//! never have to be compared.
//!
//! [`connect_thread`]: crate::connect_thread
//! [`rec_thread`]: crate::rec_thread
//! [`heartbeat`]: crate::heartbeat

use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicI32;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_apic::data::ClusterConfig;
use ic_apic::mgm_client;
use ic_apic::mgm_client::MgmClient;
use ic_comm::connection::Connection;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::qmgr::NodeState;
use ic_port::debug::IC_COMM_LEVEL;
use ic_port::debug::IC_HEARTBEAT_LEVEL;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_GLOBAL;
use ic_port::sync::IC_MUTEX_LEVEL_NODE_CONN;
use ic_port::time::IcTimer;
use ic_port::IcError;
use ic_util::connectstring::ConnectString;
use ic_util::threadpool::ThreadPool;

use crate::apid_conn::ApidConnection;
use crate::connect_thread;
use crate::heartbeat;
use crate::node_connect::resolve_port;
use crate::node_connect::words_as_bytes;
use crate::node_state::PublishedNodeState;
use crate::rec_thread;
use crate::signal_reader::SignalReader;
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

/// Stack for a connect thread. Small, since there is one per data node,
/// but not as small as the C's 64 KB: the thread also resolves host
/// names, and it runs the node id reclaim, which decodes a whole
/// configuration, and a debug build of Rust uses several times the
/// stack of a release build. Worth measuring and lowering.
pub const IC_CONNECT_THREAD_STACK: usize = 128 * 1024;
/// Stack for the receive thread, the C's medium size.
pub const IC_RECEIVE_THREAD_STACK: usize = 256 * 1024;
/// Stack for the heartbeat thread, which does very little.
pub const IC_HEARTBEAT_THREAD_STACK: usize = 128 * 1024;
/// How long the receive thread waits in its poll set before looking at
/// new links and requests to drop one. It is also the longest a new
/// link waits before it is watched.
pub const IC_RECEIVE_POLL_MS: u32 = 20;
/// How often a connect thread that is not dialling looks at its link.
pub const IC_CONNECT_CHECK_MS: u32 = 100;

/// Where a data node stands, as far as dialling it goes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum LinkStatus {
  /// Not connected, and being dialled on a growing delay. Either it has
  /// never been connected, or our link to it broke and no data node has
  /// said the node itself failed, or its failure has been reported as
  /// handled.
  #[default]
  Disconnected = 0,
  /// A link has been made and registered.
  Connected = 1,
  /// A data node has reported that this node failed. It is not dialled
  /// until a surviving data node reports the failure fully handled.
  AwaitingTakeover = 2,
}

impl LinkStatus {
  fn from_u32(value: u32) -> LinkStatus {
    match value {
      1 => LinkStatus::Connected,
      2 => LinkStatus::AwaitingTakeover,
      _ => LinkStatus::Disconnected,
    }
  }
}

/// A registered link on its way from a connect thread to the receive
/// thread that will own it (the C's add list, `first_add_node`).
pub(crate) struct PendingLink {
  pub(crate) conn: Connection,
  /// Holds any bytes that arrived past the registration.
  pub(crate) reader: SignalReader,
  pub(crate) state: NodeState,
  pub(crate) heartbeat_interval_ms: u32,
  pub(crate) use_checksum: bool,
}

/// The writing half of a link.
struct Sender {
  conn: Option<Arc<Connection>>,
  use_checksum: bool,
  /// Kept between sends so that its allocation is reused.
  send_buf: Vec<u32>,
}

/// One data node, as every thread sees it (`IC_SEND_NODE_CONNECTION`).
pub struct NodeShared {
  /// The data node.
  pub node_id: u32,
  /// The link's state, written only by the receive thread that owns the
  /// node. This is what to read before sending.
  pub published: PublishedNodeState,
  /// A [`LinkStatus`]; see the module note.
  membership: AtomicU32,
  /// Set by the first report that the node failed, cleared when a new
  /// link to it is installed. Every surviving node reports the same
  /// failure, and this is what makes all but the first do nothing.
  failure_reported: AtomicBool,
  /// The last error, as a code, or zero.
  last_error: AtomicI32,
  /// The node's heartbeat check interval.
  heartbeat_interval_ms: AtomicU32,
  /// An error code asking the owning receive thread to drop the link,
  /// or zero. The first reason given is the one kept.
  drop_request: AtomicI32,
  /// True while a link waits in `pending`, so that the receive thread
  /// looks at the mutex only when there is something there.
  pending_ready: AtomicBool,
  pending: IcMutex<Option<PendingLink>>,
  sender: IcMutex<Sender>,
  /// The whole of the last node state, for reporting only. Decisions go
  /// by `published`, which a sixteen-word structure could not be.
  state_copy: IcMutex<Option<NodeState>>,
}

impl std::fmt::Debug for NodeShared {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "NodeShared(node {}, {:?}, {:?})",
      self.node_id,
      self.membership(),
      self.published
    )
  }
}

impl NodeShared {
  fn new(node_id: u32, heartbeat_interval_ms: u32) -> NodeShared {
    NodeShared {
      node_id,
      published: PublishedNodeState::new(),
      membership: AtomicU32::new(LinkStatus::Disconnected as u32),
      failure_reported: AtomicBool::new(false),
      last_error: AtomicI32::new(0),
      heartbeat_interval_ms: AtomicU32::new(heartbeat_interval_ms),
      drop_request: AtomicI32::new(0),
      pending_ready: AtomicBool::new(false),
      pending: IcMutex::new(IC_MUTEX_LEVEL_NODE_CONN, None),
      sender: IcMutex::new(
        IC_MUTEX_LEVEL_NODE_CONN,
        Sender {
          conn: None,
          use_checksum: false,
          send_buf: Vec::new(),
        },
      ),
      state_copy: IcMutex::new(IC_MUTEX_LEVEL_NODE_CONN, None),
    }
  }

  // ---- Membership ----

  /// Where the node stands, as far as dialling it goes.
  pub fn membership(&self) -> LinkStatus {
    LinkStatus::from_u32(self.membership.load(Ordering::Acquire))
  }

  pub(crate) fn set_membership(&self, to: LinkStatus) {
    self.membership.store(to as u32, Ordering::Release);
  }

  /// Change the membership only if it is still `from`. True if it was.
  pub(crate) fn cas_membership(
    &self,
    from: LinkStatus,
    to: LinkStatus,
  ) -> bool {
    let result = self.membership.compare_exchange(
      from as u32,
      to as u32,
      Ordering::AcqRel,
      Ordering::Acquire,
    );
    result.is_ok()
  }

  /// Note that a data node reported this node failed. True if that had
  /// already been noted for the current link.
  pub(crate) fn mark_failure_reported(&self) -> bool {
    self.failure_reported.swap(true, Ordering::AcqRel)
  }

  pub(crate) fn clear_failure_reported(&self) {
    self.failure_reported.store(false, Ordering::Release);
  }

  // ---- What is known about the node ----

  /// The last error seen on this node's link, if any.
  pub fn last_error(&self) -> Option<IcError> {
    let code = self.last_error.load(Ordering::Relaxed);
    if code == 0 {
      return None;
    }
    Some(IcError::new(code))
  }

  pub(crate) fn set_last_error(&self, error: IcError) {
    self.last_error.store(error.code, Ordering::Relaxed);
  }

  /// Forget the last error, when a new link is up and the old reason no
  /// longer describes anything.
  pub(crate) fn clear_last_error(&self) {
    self.last_error.store(0, Ordering::Relaxed);
  }

  /// The heartbeat check interval the node reported, in milliseconds.
  pub fn heartbeat_interval_ms(&self) -> u32 {
    self.heartbeat_interval_ms.load(Ordering::Relaxed)
  }

  pub(crate) fn set_heartbeat_interval_ms(&self, ms: u32) {
    self.heartbeat_interval_ms.store(ms, Ordering::Relaxed);
  }

  /// The check interval with the floor applied.
  pub fn check_interval_ms(&self) -> u32 {
    let interval = self.heartbeat_interval_ms();
    if interval < IC_MIN_HEARTBEAT_INTERVAL_MS {
      return IC_MIN_HEARTBEAT_INTERVAL_MS;
    }
    interval
  }

  /// How often we send a heartbeat. The division rounds down, which
  /// errs towards sending slightly more often, never less.
  pub fn heartbeat_period_ms(&self) -> u32 {
    self.check_interval_ms() / IC_HEARTBEATS_PER_INTERVAL
  }

  /// The whole of what the node last said about itself. For reporting;
  /// to decide anything, read [`published`](Self::published).
  pub fn node_state(&self) -> Option<NodeState> {
    *self.state_copy.lock()
  }

  pub(crate) fn set_node_state(&self, state: Option<NodeState>) {
    *self.state_copy.lock() = state;
  }

  // ---- Asking the owning receive thread to drop the link ----

  pub(crate) fn request_drop(&self, error: IcError) {
    let _ = self.drop_request.compare_exchange(
      0,
      error.code,
      Ordering::AcqRel,
      Ordering::Acquire,
    );
  }

  pub(crate) fn take_drop_request(&self) -> Option<IcError> {
    let code = self.drop_request.swap(0, Ordering::AcqRel);
    if code == 0 {
      return None;
    }
    Some(IcError::new(code))
  }

  // ---- Handing a new link to the receive thread ----

  pub(crate) fn hand_over(&self, link: PendingLink) {
    let mut pending = self.pending.lock();
    if let Some(old) = pending.take() {
      // Cannot happen while one connect thread serves one node, but a
      // link left here would otherwise be a socket nobody reads.
      old.conn.close();
    }
    *pending = Some(link);
    drop(pending);
    self.pending_ready.store(true, Ordering::Release);
  }

  pub(crate) fn take_pending(&self) -> Option<PendingLink> {
    if !self.pending_ready.swap(false, Ordering::AcqRel) {
      return None;
    }
    self.pending.lock().take()
  }

  pub(crate) fn discard_pending(&self) {
    self.pending_ready.store(false, Ordering::Release);
    let taken = self.pending.lock().take();
    if let Some(old) = taken {
      old.conn.close();
    }
  }

  // ---- Sending ----

  pub(crate) fn install_sender(&self, conn: Arc<Connection>, checksum: bool) {
    let mut sender = self.sender.lock();
    sender.conn = Some(conn);
    sender.use_checksum = checksum;
    sender.send_buf.clear();
  }

  pub(crate) fn clear_sender(&self) {
    self.sender.lock().conn = None;
  }

  /// Send a signal to this node.
  ///
  /// Any thread may send. The node's mutex is held for the length of the
  /// write, so that two signals never interleave on the socket. A write
  /// that fails asks the receive thread to drop the link.
  pub fn send(
    &self,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
  ) -> Result<(), IcError> {
    let mut sender = self.sender.lock();
    let conn = match sender.conn.as_ref() {
      Some(conn) => Arc::clone(conn),
      None => {
        drop(sender);
        return Err(self.not_connected_error());
      }
    };
    let use_checksum = sender.use_checksum;
    sender.send_buf.clear();
    header::encode(header, data, sections, use_checksum, &mut sender.send_buf)?;
    ic_port::debug_print!(
      IC_NDB_MESSAGE_LEVEL,
      "-> node {} {} ({} words, {} sections)",
      self.node_id,
      gsn::gsn_name(header.gsn()).unwrap_or("unknown"),
      data.len(),
      sections.len()
    );
    let result = conn.write(words_as_bytes(&sender.send_buf));
    drop(sender);
    if let Err(e) = result {
      self.request_drop(e);
      return Err(e);
    }
    Ok(())
  }

  /// Say what is known and no more: a node the cluster reported failed
  /// is down, anything else is only a link we lack.
  pub(crate) fn not_connected_error(&self) -> IcError {
    if self.membership() == LinkStatus::AwaitingTakeover {
      return IcError::new(err::IC_ERROR_NODE_DOWN);
    }
    IcError::new(err::IC_ERROR_LINK_LOST)
  }
}

/// Our identity in the cluster, and what goes with it.
struct Identity {
  /// The configuration as our node id sees it.
  config: Arc<ClusterConfig>,
  /// The management server we ask for data nodes' ports.
  mgm: Option<MgmClient>,
  /// Goes up whenever the identity is taken again. A link made under an
  /// older identity is not used.
  epoch: u32,
  reclaim_attempts: u32,
  reclaim_at: IcTimer,
  /// Why the last attempt to claim a node id failed, whichever connect
  /// thread made it. A thread that finds the next attempt not yet due
  /// reports this, which is the true reason, rather than inventing one.
  last_reclaim_error: Option<IcError>,
}

/// What a connect thread needs to dial, taken under the identity lock
/// and then used without it.
pub(crate) struct DialPlan {
  pub(crate) epoch: u32,
  pub(crate) config: Arc<ClusterConfig>,
  pub(crate) port: u16,
}

/// What the threads hold between them.
pub(crate) struct ApidShared {
  identity: IcMutex<Identity>,
  connect_string: ConnectString,
  mgm_timeout_ms: u32,
  node_name: Option<String>,
  /// True when any node id will do. False when the application was
  /// started with a stated node id, which is then the only one we ever
  /// run under.
  node_id_is_dynamic: bool,
  own_node_id: AtomicU32,
  /// Set when every link has been lost and our node id must be claimed
  /// again before anyone dials.
  reclaim_pending: AtomicBool,
  /// True while no data node is connected, so that it is said once.
  alone: AtomicBool,
  /// One per data node, fixed at start.
  pub(crate) nodes: Vec<Arc<NodeShared>>,
  pub(crate) thread_table: Arc<ThreadTable>,
}

impl ApidShared {
  pub(crate) fn own_node_id(&self) -> u32 {
    self.own_node_id.load(Ordering::Acquire)
  }

  pub(crate) fn node(&self, node_id: u32) -> Option<&Arc<NodeShared>> {
    self.nodes.iter().find(|node| node.node_id == node_id)
  }

  /// The data nodes that have a link up and say they are started.
  pub(crate) fn started_nodes(&self) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::new();
    for node in &self.nodes {
      if node.published.is_started() {
        out.push(node.node_id);
      }
    }
    out
  }

  fn any_connected(&self) -> bool {
    self
      .nodes
      .iter()
      .any(|node| node.membership() == LinkStatus::Connected)
  }

  // ---- Evidence about the cluster, from the receive thread ----

  /// A data node says `node_id` failed.
  ///
  /// The link to that node usually belongs to another receive thread, so
  /// this only records the failure and asks the owner to drop the link.
  pub(crate) fn node_failure_reported(&self, reporter: u32, node_id: u32) {
    let node = match self.node(node_id) {
      Some(node) => node,
      // Not a data node we link to, such as another API node.
      None => return,
    };
    if node.mark_failure_reported() {
      // Another node reporting the failure we already know of.
      return;
    }
    node.set_membership(LinkStatus::AwaitingTakeover);
    node.set_last_error(IcError::new(err::IC_ERROR_NODE_DOWN));
    node.request_drop(IcError::new(err::IC_ERROR_NODE_DOWN));
    ic_port::debug_print!(
      IC_HEARTBEAT_LEVEL,
      "Node {} reports node {} failed; not dialling it until that has been \
       handled",
      reporter,
      node_id
    );
  }

  /// A data node says the failure of `node_id` has been fully handled.
  pub(crate) fn takeover_reported(&self, reporter: u32, node_id: u32) {
    let node = match self.node(node_id) {
      Some(node) => node,
      None => return,
    };
    // Every surviving node sends one, so all but the first find the
    // wait already over.
    let handled = node
      .cas_membership(LinkStatus::AwaitingTakeover, LinkStatus::Disconnected);
    if handled {
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "Node {} reports the failure of node {} fully handled; dialling",
        reporter,
        node_id
      );
    }
  }

  /// A link has gone. With no data node left connected there is nobody
  /// to report a failure or its handling, so every wait ends, and our
  /// node id may no longer be ours.
  pub(crate) fn note_link_lost(&self) {
    if self.any_connected() {
      return;
    }
    for node in &self.nodes {
      let _ = node
        .cas_membership(LinkStatus::AwaitingTakeover, LinkStatus::Disconnected);
    }
    if !self.alone.swap(true, Ordering::AcqRel) {
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "No data node is connected; the whole cluster is gone from here"
      );
    }
    if self.node_id_is_dynamic {
      // Our connections were our only claim on the id.
      self.reclaim_pending.store(true, Ordering::Release);
    }
  }

  pub(crate) fn note_link_up(&self) {
    self.alone.store(false, Ordering::Release);
  }

  // ---- Dialling, from a connect thread ----

  /// True while every link has been lost and our node id has not yet
  /// been claimed again. Nobody dials meanwhile.
  pub(crate) fn reclaim_pending(&self) -> bool {
    self.reclaim_pending.load(Ordering::Acquire)
  }

  /// Claim our node id again if an attempt is due. `Ok` once there is
  /// nothing left to claim, whichever thread did it. Otherwise the
  /// reason the id is not yet ours: the error of the last attempt.
  ///
  /// The reclaim keeps its own growing delay. A connect thread waiting
  /// on it calls this every [`IC_CONNECT_CHECK_MS`] rather than backing
  /// off on its own, so that every node is dialled the moment the id is
  /// back, not when each thread's own timer next runs out.
  pub(crate) fn reclaim_if_due(&self) -> Result<(), IcError> {
    let mut identity = self.identity.lock();
    if !self.reclaim_pending.load(Ordering::Acquire) {
      return Ok(());
    }
    self.reclaim(&mut identity)
  }

  /// Everything a connect thread needs before it dials: our identity,
  /// claimed again first if every link has been lost, and the port the
  /// node listens on.
  ///
  /// The management server is asked with the identity lock held, which
  /// makes connect threads take turns at it. It is one conversation on
  /// one socket, so they would take turns anyway.
  pub(crate) fn prepare_dial(&self, node_id: u32) -> Result<DialPlan, IcError> {
    let mut identity = self.identity.lock();
    if self.reclaim_pending.load(Ordering::Acquire) {
      self.reclaim(&mut identity)?;
    }
    let port = self.node_port(&mut identity, node_id)?;
    Ok(DialPlan {
      epoch: identity.epoch,
      config: Arc::clone(&identity.config),
      port,
    })
  }

  /// Take a newly made link for its node, if nothing has changed while
  /// it was being made: the identity is the one it was made under, and
  /// no data node has since reported the node failed.
  pub(crate) fn claim_link(
    &self,
    node: &NodeShared,
    epoch: u32,
  ) -> Result<(), IcError> {
    let identity = self.identity.lock();
    if identity.epoch != epoch {
      // Made under a node id we no longer hold.
      return Err(IcError::new(err::IC_ERROR_LINK_LOST));
    }
    if !node.cas_membership(LinkStatus::Disconnected, LinkStatus::Connected) {
      return Err(node.not_connected_error());
    }
    // Back in under the id we hold, so there is nothing to reclaim. The
    // data node accepting us is the proof that the id is ours.
    self.reclaim_pending.store(false, Ordering::Release);
    drop(identity);
    Ok(())
  }

  /// Ask the management server which port a data node listens on.
  ///
  /// A management server can itself stop or drop an idle connection.
  /// When the question cannot be put, the connection is made again and
  /// the question asked once more, so that a management server
  /// restarting does not leave every data node unreachable for good.
  fn node_port(
    &self,
    identity: &mut Identity,
    node_id: u32,
  ) -> Result<u16, IcError> {
    let link = match identity.config.link_to(node_id) {
      Some(link) => link.clone(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    if !link.port_is_dynamic() {
      return Ok(link.server_port);
    }
    let own_node_id = identity.config.api.node_id;
    if let Some(mgm) = identity.mgm.as_mut() {
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
          identity.mgm = None;
        }
      }
    }
    let client =
      MgmClient::connect_any(&self.connect_string, self.mgm_timeout_ms)?;
    let server = client.server();
    ic_port::debug_print!(
      IC_COMM_LEVEL,
      "Connected to management server {}:{} again",
      server.host,
      server.port
    );
    identity.mgm = Some(client);
    match identity.mgm.as_mut() {
      Some(mgm) => resolve_port(&link, own_node_id, mgm),
      None => Err(IcError::new(err::IC_ERROR_MGM_SERVER_REFUSED)),
    }
  }

  /// Claim a node id again after losing every link.
  ///
  /// Only called for a node that can accept any id. It asks for the same
  /// id first. "Not now" is asked again for as long as it takes; seen
  /// live, a restarting cluster says it for several seconds. The other
  /// two refusals make it take another id at once. "Held by another
  /// node" usually means held by our own old connection, which the data
  /// nodes have not yet timed out, and waiting for that can take
  /// minutes. With every link lost nothing is in flight, so this is the
  /// cheapest moment there is to change identity.
  fn reclaim(&self, identity: &mut Identity) -> Result<(), IcError> {
    if self.any_connected() {
      // A connect thread got back in under the id we have.
      self.reclaim_pending.store(false, Ordering::Release);
      return Ok(());
    }
    let now = ic_port::time::gethrtime();
    if identity.reclaim_at != 0 && now < identity.reclaim_at {
      // Another connect thread tried a moment ago, and its reason
      // stands until the next attempt is due.
      let fallback = IcError::new(err::IC_ERROR_NO_NODEID);
      return Err(identity.last_reclaim_error.unwrap_or(fallback));
    }
    let old_node_id = identity.config.api.node_id;
    let name: Option<&str> = self.node_name.as_deref();
    let mut wanted = self.connect_string.clone();
    wanted.node_id = Some(old_node_id);
    let mut fetched =
      mgm_client::fetch_configuration(&wanted, self.mgm_timeout_ms, name);
    let mut refusal: i32 = 0;
    if let Err(e) = &fetched {
      refusal = e.code;
    }
    if refusal == err::IC_ERROR_NODEID_IN_USE
      || refusal == err::IC_ERROR_NODEID_NOT_ALLOWED
    {
      ic_port::debug_print!(
        IC_COMM_LEVEL,
        "Node id {} cannot be had now ({}); asking for another",
        old_node_id,
        mgm_client::last_refusal()
      );
      wanted.node_id = None;
      fetched =
        mgm_client::fetch_configuration(&wanted, self.mgm_timeout_ms, name);
    }
    let (config, client) = match fetched {
      Ok(pair) => pair,
      Err(e) => {
        identity.reclaim_attempts += 1;
        let delay = retry_delay_ms(identity.reclaim_attempts);
        identity.reclaim_at = now + period_nanos(delay);
        identity.last_reclaim_error = Some(e);
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
        return Err(e);
      }
    };
    ic_port::debug_print!(
      IC_COMM_LEVEL,
      "Holding node id {} (was {})",
      config.api.node_id,
      old_node_id
    );
    self.warn_if_nodes_changed(&config);
    self
      .own_node_id
      .store(config.api.node_id, Ordering::Release);
    identity.config = Arc::new(config);
    identity.mgm = Some(client);
    identity.epoch = identity.epoch.wrapping_add(1);
    identity.reclaim_attempts = 0;
    identity.reclaim_at = 0;
    identity.last_reclaim_error = None;
    self.reclaim_pending.store(false, Ordering::Release);
    Ok(())
  }

  /// The set of data nodes is fixed when the threads start. A new node
  /// id could in principle come with a configuration that links to other
  /// data nodes; say so rather than quietly ignore them.
  fn warn_if_nodes_changed(&self, config: &ClusterConfig) {
    let now_linked = config.connectable_data_nodes();
    let mut same = now_linked.len() == self.nodes.len();
    for node_id in &now_linked {
      if self.node(*node_id).is_none() {
        same = false;
      }
    }
    if !same {
      ic_port::ic_printf!(
        "The configuration for our new node id links to other data nodes \
         ({:?}); only the ones we started with are served",
        now_linked
      );
    }
  }
}

/// The Data API, started: every thread that keeps an API node in its
/// cluster, and the state they share.
///
/// Stopping it, or dropping it, stops every thread and waits for each.
/// A thread in the middle of a connect or a question to the management
/// server finishes that first, which can take up to their timeouts.
pub struct ApidGlobal {
  shared: Arc<ApidShared>,
  pool: ThreadPool,
}

impl std::fmt::Debug for ApidGlobal {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ApidGlobal(node {}, {} of {} data node(s) connected)",
      self.own_node_id(),
      self.num_connected(),
      self.shared.nodes.len()
    )
  }
}

impl ApidGlobal {
  /// Start the Data API threads for a configuration fetched from
  /// `mgm`.
  ///
  /// `connect_string` says where to find a management server again and
  /// whether our node id was stated or chosen for us, which decides what
  /// happens after losing every link. `node_name` is what the management
  /// server logs against our node id.
  ///
  /// Returns at once. The data nodes are connected in the background;
  /// [`wait_for_started`](Self::wait_for_started) waits for them.
  pub fn start(
    config: ClusterConfig,
    mgm: MgmClient,
    connect_string: ConnectString,
    mgm_timeout_ms: u32,
    node_name: Option<&str>,
  ) -> Result<ApidGlobal, IcError> {
    let mut nodes: Vec<Arc<NodeShared>> = Vec::new();
    for node_id in config.connectable_data_nodes() {
      let interval = match config.data_node(node_id) {
        Some(node) => node.api_heartbeat_interval_ms,
        None => 0,
      };
      nodes.push(Arc::new(NodeShared::new(node_id, interval)));
    }
    let own_node_id = config.api.node_id;
    let node_id_is_dynamic = connect_string.node_id.is_none();
    let mut owned_name: Option<String> = None;
    if let Some(name) = node_name {
      owned_name = Some(name.to_string());
    }
    let shared = Arc::new(ApidShared {
      identity: IcMutex::new(
        IC_MUTEX_LEVEL_GLOBAL,
        Identity {
          config: Arc::new(config),
          mgm: Some(mgm),
          epoch: 0,
          reclaim_attempts: 0,
          reclaim_at: 0,
          last_reclaim_error: None,
        },
      ),
      connect_string,
      mgm_timeout_ms,
      node_name: owned_name,
      node_id_is_dynamic,
      own_node_id: AtomicU32::new(own_node_id),
      reclaim_pending: AtomicBool::new(false),
      alone: AtomicBool::new(false),
      nodes,
      thread_table: Arc::new(ThreadTable::new()),
    });

    let num_nodes = shared.nodes.len();
    let mut pool = ThreadPool::new(num_nodes as u32 + 4, "apid");
    // If a start fails, returning drops the pool, which stops and joins
    // every thread already started.

    // One receive thread for every node, for now. More receive threads
    // each take a share of the nodes, assigned here and kept.
    let mut all_nodes: Vec<usize> = Vec::new();
    let mut index: usize = 0;
    while index < num_nodes {
      all_nodes.push(index);
      index += 1;
    }
    let rec_shared = Arc::clone(&shared);
    pool.start_thread(
      Box::new(move |state| {
        rec_thread::run_receive_thread(rec_shared, all_nodes, state);
      }),
      IC_RECEIVE_THREAD_STACK,
      false,
    )?;

    let hb_shared = Arc::clone(&shared);
    pool.start_thread(
      Box::new(move |state| {
        heartbeat::run_heartbeat_thread(hb_shared, state);
      }),
      IC_HEARTBEAT_THREAD_STACK,
      false,
    )?;

    index = 0;
    while index < num_nodes {
      let conn_shared = Arc::clone(&shared);
      let node_index = index;
      pool.start_thread(
        Box::new(move |state| {
          connect_thread::run_connect_thread(conn_shared, node_index, state);
        }),
        IC_CONNECT_THREAD_STACK,
        false,
      )?;
      index += 1;
    }
    Ok(ApidGlobal { shared, pool })
  }

  /// Our own node id. It can change after every link has been lost, if
  /// any id will do.
  pub fn own_node_id(&self) -> u32 {
    self.shared.own_node_id()
  }

  /// The table user threads take their inboxes from. A signal a data
  /// node addresses to a user thread's block ends up in that thread's
  /// inbox, unread by anyone else.
  pub fn thread_table(&self) -> Arc<ThreadTable> {
    Arc::clone(&self.shared.thread_table)
  }

  /// Every data node we link to.
  pub fn nodes(&self) -> &[Arc<NodeShared>] {
    &self.shared.nodes
  }

  /// One data node.
  pub fn node(&self, node_id: u32) -> Option<&Arc<NodeShared>> {
    self.shared.node(node_id)
  }

  /// Send a signal to a data node, from any thread. `sections` may be
  /// empty; a long signal carries up to three.
  pub fn send(
    &self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
  ) -> Result<(), IcError> {
    match self.shared.node(node_id) {
      Some(node) => node.send(header, data, sections),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    }
  }

  /// How many data nodes have a link up.
  pub fn num_connected(&self) -> u32 {
    let mut count: u32 = 0;
    for node in &self.shared.nodes {
      if node.published.is_connected() {
        count += 1;
      }
    }
    count
  }

  /// The data nodes that have a link up and say they are started.
  pub fn started_nodes(&self) -> Vec<u32> {
    self.shared.started_nodes()
  }

  /// A connection for one user thread: its own block number and inbox,
  /// and the requests it waits for. One per thread; a thread keeps it
  /// for as long as it talks to the cluster.
  pub fn create_connection(&self) -> Result<ApidConnection, IcError> {
    ApidConnection::new(Arc::clone(&self.shared))
  }

  /// Wait until every data node is connected and started, or `wait_ms`
  /// has passed. Returns how many are started.
  pub fn wait_for_started(&self, wait_ms: u32) -> u32 {
    let start = ic_port::time::gethrtime();
    loop {
      let started = self.started_nodes().len() as u32;
      if started as usize == self.shared.nodes.len() {
        return started;
      }
      let waited =
        ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
      if waited >= wait_ms as u64 {
        return started;
      }
      ic_port::time::microsleep(20_000);
    }
  }

  /// Stop every thread and wait for each.
  pub fn stop(&mut self) {
    self.pool.stop();
  }
}

/// How long to wait before the next attempt: longer each time it
/// fails, up to a ceiling, so a node that is down does not keep us
/// busy.
pub(crate) fn retry_delay_ms(failed_attempts: u32) -> u32 {
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
pub(crate) fn period_nanos(ms: u32) -> IcTimer {
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
    let node = NodeShared::new(2, 30000);
    assert_eq!(node.check_interval_ms(), 30000);
    assert_eq!(node.heartbeat_period_ms(), 10000);
    let fast = NodeShared::new(2, 1500);
    assert_eq!(fast.heartbeat_period_ms(), 500);
    // An interval that does not divide evenly rounds the period down,
    // so three sends still fit inside it.
    let odd = NodeShared::new(2, 1000);
    assert_eq!(odd.heartbeat_period_ms(), 333);
    assert!(odd.heartbeat_period_ms() * 3 <= odd.check_interval_ms());
  }

  #[test]
  fn a_tiny_or_missing_interval_is_raised_to_the_floor() {
    // Without the floor a node reporting nothing would be sent a
    // heartbeat on every round, and declared lost in no time at all.
    let unset = NodeShared::new(2, 0);
    assert_eq!(unset.check_interval_ms(), IC_MIN_HEARTBEAT_INTERVAL_MS);
    assert_eq!(unset.heartbeat_period_ms(), 33);
  }

  #[test]
  fn a_new_node_is_ready_to_be_dialled() {
    let node = NodeShared::new(2, 30000);
    assert_eq!(node.membership(), LinkStatus::Disconnected);
    assert!(!node.published.is_connected());
    assert!(node.last_error().is_none());
    assert!(node.node_state().is_none());
  }

  #[test]
  fn membership_changes_only_from_what_it_was() {
    // The first evidence wins and later evidence of the same thing does
    // nothing, which is what compare-and-swap gives.
    let node = NodeShared::new(2, 30000);
    let down = LinkStatus::Disconnected;
    let up = LinkStatus::Connected;
    let failed = LinkStatus::AwaitingTakeover;
    assert!(node.cas_membership(down, up));
    assert!(!node.cas_membership(down, up));
    node.set_membership(failed);
    // A lost link does not undo a reported failure.
    assert!(!node.cas_membership(up, down));
    assert_eq!(node.membership(), failed);
    assert!(node.cas_membership(failed, down));
  }

  #[test]
  fn the_first_reason_to_drop_a_link_is_the_one_kept() {
    let node = NodeShared::new(2, 30000);
    assert!(node.take_drop_request().is_none());
    node.request_drop(IcError::new(err::IC_ERROR_HEARTBEAT_MISSED));
    node.request_drop(IcError::new(err::IC_ERROR_LINK_LOST));
    let reason = node.take_drop_request().expect("requested");
    assert_eq!(reason.code, err::IC_ERROR_HEARTBEAT_MISSED);
    // Taking it clears it.
    assert!(node.take_drop_request().is_none());
  }

  #[test]
  fn a_failure_is_noted_once_per_link() {
    let node = NodeShared::new(2, 30000);
    assert!(!node.mark_failure_reported());
    assert!(node.mark_failure_reported());
    node.clear_failure_reported();
    assert!(!node.mark_failure_reported());
  }

  #[test]
  fn sending_without_a_link_says_what_is_known() {
    let node = NodeShared::new(2, 30000);
    let header = SignalHeader::new(gsn::IC_GSN_API_REGREQ, 0x0FA2, 0xFC);
    let e = node.send(&header, &[1, 2, 3], &[]).expect_err("no link");
    assert_eq!(e.code, err::IC_ERROR_LINK_LOST);
    node.set_membership(LinkStatus::AwaitingTakeover);
    let e = node
      .send(&header, &[1, 2, 3], &[])
      .expect_err("failed node");
    assert_eq!(e.code, err::IC_ERROR_NODE_DOWN);
  }

  #[test]
  fn link_status_survives_the_trip_through_a_word() {
    let all = [
      LinkStatus::Disconnected,
      LinkStatus::Connected,
      LinkStatus::AwaitingTakeover,
    ];
    for status in all {
      assert_eq!(LinkStatus::from_u32(status as u32), status);
    }
  }

  #[test]
  fn timers_count_in_nanoseconds() {
    assert_eq!(period_nanos(1), 1_000_000);
    assert_eq!(period_nanos(6000), 6_000_000_000);
  }
}
