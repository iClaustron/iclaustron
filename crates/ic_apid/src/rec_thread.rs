// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The receive thread (`legacy-c/api/ic_apid_rec_thread.ic`,
//! `run_receive_thread`).
//!
//! It owns a share of the data nodes, assigned when the threads start
//! and kept: for those nodes it holds the poll set, the reading half of
//! every link, and the reader that turns bytes back into signals. No
//! other thread reads those sockets.
//!
//! Each round:
//!
//! 1. Drop the links other threads have asked to have dropped: the
//!    heartbeat thread for a node that stopped answering, a sender whose
//!    write failed, anyone who learned the node failed.
//! 2. Install the links connect threads have handed over.
//! 3. Wait in the poll set for up to [`IC_RECEIVE_POLL_MS`], and read
//!    every socket that has something.
//! 4. Route each signal, after taking a packed one apart into the
//!    signals it holds. One for a user thread goes to its inbox unread.
//!    One for our own fixed blocks is about the node that sent it, and is
//!    executed here: that is what makes this thread the one writer of
//!    the node's published state. The one exception is the dictionary's
//!    notice that a table changed, which goes to the dictionary cache.
//! 5. Post every inbox that got something, once, and wake its thread.
//!
//! A socket that closes is noticed here, when it is read, which is as
//! soon as the operating system says so.
//!
//! The C waits in the poll set with no timeout and relies on being
//! signalled; here the timeout is what makes a new link be watched
//! promptly. A self-pipe in the poll set would remove the delay and the
//! idle wake-ups, and is the obvious next step.

use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_comm::connection::Connection;
use ic_comm::poll_set::PollSet;
use ic_ndb_signals::alter_table_rep::AlterTableRep;
use ic_ndb_signals::blocks;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header;
use ic_ndb_signals::header::FragmentInfo;
use ic_ndb_signals::packed;
use ic_ndb_signals::qmgr::ApiRegConf;
use ic_ndb_signals::qmgr::NfCompleteRep;
use ic_ndb_signals::qmgr::NodeFailRep;
use ic_port::debug::IC_COMM_LEVEL;
use ic_port::debug::IC_HEARTBEAT_LEVEL;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_util::threadpool::ThreadState;

use crate::apid_global::ApidShared;
use crate::apid_global::LinkStatus;
use crate::apid_global::NodeShared;
use crate::apid_global::PendingLink;
use crate::apid_global::IC_RECEIVE_POLL_MS;
use crate::node_connect::ReceivedSignal;
use crate::node_connect::SignalView;
use crate::signal_page::Placement;
use crate::signal_reader::SignalReader;
use crate::thread_conn::Router;

/// One node's link, as this receive thread holds it.
struct RecNode {
  node: Arc<NodeShared>,
  /// The socket, shared with the node's sending half. `None` while no
  /// link is installed.
  conn: Option<Arc<Connection>>,
  reader: SignalReader,
  fd: i32,
}

/// Everything the receive thread works with. Never shared.
struct Receiver {
  shared: Arc<ApidShared>,
  /// The nodes this thread owns; a poll set entry's user object is an
  /// index into this.
  nodes: Vec<RecNode>,
  poll_set: PollSet,
  router: Router,
  /// Kept between rounds so that their allocations are reused.
  ready: Vec<usize>,
  /// The signals of a read that are ours to execute.
  signals: Vec<ReceivedSignal>,
  /// The parts of a packed signal.
  parts: Vec<packed::PackedPart>,
}

/// The body of a receive thread, serving the nodes at `node_indexes`.
pub(crate) fn run_receive_thread(
  shared: Arc<ApidShared>,
  node_indexes: Vec<usize>,
  state: &ThreadState,
) {
  let poll_set = match PollSet::new() {
    Ok(poll_set) => poll_set,
    Err(e) => {
      ic_port::ic_printf!(
        "The receive thread could not create a poll set: {}",
        e.message()
      );
      return;
    }
  };
  let mut nodes: Vec<RecNode> = Vec::new();
  for index in &node_indexes {
    nodes.push(RecNode {
      node: Arc::clone(&shared.nodes[*index]),
      conn: None,
      reader: SignalReader::new(),
      fd: -1,
    });
  }
  let router = Router::new(Arc::clone(&shared.thread_table));
  let mut receiver = Receiver {
    shared,
    nodes,
    poll_set,
    router,
    ready: Vec::new(),
    signals: Vec::new(),
    parts: Vec::new(),
  };
  while !state.stop_flag() {
    receiver.round(state);
  }
  receiver.close_all();
}

impl Receiver {
  fn round(&mut self, state: &ThreadState) {
    // Drops before installs: a request meant for a link that has gone
    // must not fall on the next one.
    self.handle_drop_requests();
    self.install_pending();
    if self.poll_set.is_empty() {
      // Nothing to watch. Sleep, but wake at once if asked to stop.
      let _ = state.wait_timeout((IC_RECEIVE_POLL_MS as u64) * 1000);
      // A link handed over during the sleep may have signals buffered.
      self.router.post_all();
      return;
    }
    if let Err(e) = self.poll_set.check(IC_RECEIVE_POLL_MS as i32) {
      ic_port::debug_print!(
        IC_COMM_LEVEL,
        "Poll set check failed: {}",
        e.message()
      );
      self.router.post_all();
      return;
    }
    self.ready.clear();
    while let Some(conn) = self.poll_set.next_connection() {
      self.ready.push(conn.user_obj);
    }
    let mut i: usize = 0;
    while i < self.ready.len() {
      let index = self.ready[i];
      self.read_node(index);
      i += 1;
    }
    // Once per round, after every ready socket has been read, so that a
    // user thread hearing from several nodes is locked and woken once.
    self.router.post_all();
  }

  // ---- Links coming and going ----

  fn install_pending(&mut self) {
    let mut index: usize = 0;
    while index < self.nodes.len() {
      if let Some(link) = self.nodes[index].node.take_pending() {
        self.install(index, link);
      }
      index += 1;
    }
  }

  fn install(&mut self, index: usize, link: PendingLink) {
    let node = Arc::clone(&self.nodes[index].node);
    if node.membership() != LinkStatus::Connected {
      // The cluster reported the node failed, or our identity changed,
      // between the link being claimed and now.
      link.conn.close();
      return;
    }
    let conn = Arc::new(link.conn);
    let fd = conn.fd();
    // Watch the socket before the link counts as up. A link nobody
    // watches would never be seen to close.
    if let Err(e) = self.poll_set.add_connection(fd, index) {
      conn.close();
      if node.cas_membership(LinkStatus::Connected, LinkStatus::Disconnected) {
        node.set_last_error(e);
      }
      self.shared.note_link_lost();
      return;
    }
    let rec = &mut self.nodes[index];
    rec.fd = fd;
    rec.conn = Some(Arc::clone(&conn));
    rec.reader = link.reader;
    node.clear_failure_reported();
    node.clear_last_error();
    node.set_heartbeat_interval_ms(link.heartbeat_interval_ms);
    node.set_node_state(Some(link.state));
    node.install_sender(conn, link.use_checksum);
    // Last, so that a reader seeing "connected" finds a link that is.
    node
      .published
      .publish_connected(&link.state, ic_port::time::gethrtime());
    self.shared.note_link_up();
    ic_port::debug_print!(IC_COMM_LEVEL, "Connected to node {}", node.node_id);
    // Bytes that arrived past the registration are already in the
    // reader, where the poll set cannot see them.
    self.drain(index);
  }

  fn handle_drop_requests(&mut self) {
    let mut index: usize = 0;
    while index < self.nodes.len() {
      if let Some(error) = self.nodes[index].node.take_drop_request() {
        if self.nodes[index].conn.is_some() {
          self.link_lost(index, error);
        }
      }
      index += 1;
    }
  }

  /// A link has gone. Whether its node has, we do not know unless the
  /// cluster said so, and a failure it reported stands.
  fn link_lost(&mut self, index: usize, error: IcError) {
    if !self.tear_down(index) {
      return;
    }
    let node = &self.nodes[index].node;
    if node.cas_membership(LinkStatus::Connected, LinkStatus::Disconnected) {
      node.set_last_error(error);
    }
    ic_port::debug_print!(
      IC_HEARTBEAT_LEVEL,
      "Lost our link to node {}: {} ({})",
      node.node_id,
      error.message(),
      error.code
    );
    self.shared.note_link_lost();
  }

  /// Take a link down. Returns false if there was none.
  fn tear_down(&mut self, index: usize) -> bool {
    let rec = &mut self.nodes[index];
    let conn = match rec.conn.take() {
      Some(conn) => conn,
      None => return false,
    };
    let fd = rec.fd;
    rec.fd = -1;
    rec.reader.reset();
    let node = Arc::clone(&rec.node);
    // First, so that no one starts a send into a link going away.
    node.published.publish_down();
    // Before taking the node's mutex: shutting the socket makes a
    // writer blocked on it return, and so let go of the mutex.
    conn.close();
    let _ = self.poll_set.remove_connection(fd);
    node.clear_sender();
    // A request to drop this link is answered.
    let _ = node.take_drop_request();
    true
  }

  fn close_all(&mut self) {
    let mut index: usize = 0;
    while index < self.nodes.len() {
      if self.tear_down(index) {
        let node = &self.nodes[index].node;
        let _ =
          node.cas_membership(LinkStatus::Connected, LinkStatus::Disconnected);
      }
      self.nodes[index].node.discard_pending();
      index += 1;
    }
  }

  // ---- Reading ----

  fn read_node(&mut self, index: usize) {
    let result = {
      let rec = &mut self.nodes[index];
      match rec.conn.as_ref() {
        Some(conn) => rec.reader.read_from(conn),
        None => return,
      }
    };
    match result {
      Ok(0) => {
        // The peer closed. It is the link that is known to be gone,
        // not the node: only another data node's report says that.
        self.link_lost(index, IcError::new(err::IC_ERROR_LINK_LOST));
        return;
      }
      Ok(_) => {}
      Err(e) => {
        self.link_lost(index, e);
        return;
      }
    }
    self.drain(index);
    // Read again, up to a number of times, while a read fills all the
    // room it was given, since the socket then likely holds more; a
    // read that came short took all there was, and the poll set says
    // when more comes.
    let extra = self.shared.extra_reads.load(Ordering::Acquire);
    let mut done: u32 = 0;
    while done < extra && self.nodes[index].reader.last_read_full() {
      done += 1;
      let again = {
        let rec = &mut self.nodes[index];
        match rec.conn.as_ref() {
          Some(conn) => rec.reader.read_again(conn),
          None => return,
        }
      };
      match again {
        Ok(None) => return,
        Ok(Some(0)) => {
          self.link_lost(index, IcError::new(err::IC_ERROR_LINK_LOST));
          return;
        }
        Ok(Some(_)) => self.drain(index),
        Err(e) => {
          self.link_lost(index, e);
          return;
        }
      }
    }
  }

  /// Route every complete signal the node's reader holds: a signal for
  /// a user thread is copied from the read buffer straight into the page
  /// for that thread, and one for our own blocks is kept to be executed
  /// once the buffer is done with.
  fn drain(&mut self, index: usize) {
    let node_id = self.nodes[index].node.node_id;
    let mut own = std::mem::take(&mut self.signals);
    own.clear();
    let large_words =
      self.shared.large_signal_words.load(Ordering::Acquire) as usize;
    let result = route_signals(
      &mut self.nodes[index].reader,
      node_id,
      large_words,
      &mut self.router,
      &mut self.parts,
      &mut own,
    );
    if let Err(e) = result {
      // A signal that cannot be read means the stream is out of step,
      // and nothing after it can be trusted.
      self.signals = own;
      self.link_lost(index, e);
      return;
    }
    for signal in &own {
      self.execute(index, signal);
    }
    own.clear();
    self.signals = own;
  }

  // ---- The signals this thread executes ----

  fn execute(&mut self, index: usize, signal: &ReceivedSignal) {
    let node_id = self.nodes[index].node.node_id;
    if signal.gsn == gsn::IC_GSN_API_REGCONF {
      self.registration_confirmed(index, signal);
      return;
    }
    if signal.gsn == gsn::IC_GSN_API_REGREF {
      // The node refused our heartbeat. It will drop the link itself,
      // and we will see that.
      ic_port::debug_print!(
        IC_HEARTBEAT_LEVEL,
        "Node {} refused our heartbeat",
        node_id
      );
      return;
    }
    if signal.gsn == gsn::IC_GSN_NODE_FAILREP {
      // The bitmap is in the first section unless the sender put it
      // inline after the three fixed words.
      match NodeFailRep::decode(&signal.data, signal.section(0)) {
        Ok(report) => {
          for failed in &report.failed_nodes {
            self.shared.node_failure_reported(node_id, *failed);
          }
        }
        Err(e) => {
          ic_port::debug_print!(
            IC_HEARTBEAT_LEVEL,
            "Unreadable NODE_FAILREP from node {}: {}",
            node_id,
            e.message()
          );
        }
      }
      return;
    }
    if signal.gsn == gsn::IC_GSN_NF_COMPLETEREP {
      // The block field carries the sender's reference, not zero, and is
      // ignored; the failed node id is what counts.
      if let Ok(report) = NfCompleteRep::decode(&signal.data) {
        // Publish preceding takeover replies before making their absence
        // grounds for aborting a transaction. Poll snapshots that state
        // before taking its inbox, so those replies will be processed first.
        self.router.post_all();
        self
          .shared
          .takeover_reported(node_id, report.failed_node_id);
      }
      return;
    }
    if signal.gsn == gsn::IC_GSN_ALTER_TABLE_REP {
      self.table_changed(node_id, signal);
      return;
    }
    ic_port::debug_print!(
      IC_NDB_MESSAGE_LEVEL,
      "No handler for {} from node {} to block {:#06x}",
      gsn::gsn_name(signal.gsn).unwrap_or("an unknown signal"),
      node_id,
      signal.receiver_block
    );
  }

  /// A table was altered or dropped: whatever is cached of the version
  /// named is let go of, with the indexes bound for it. An alteration is
  /// reported by every data node; only the first report finds anything.
  fn table_changed(&self, node_id: u32, signal: &ReceivedSignal) {
    let rep = match AlterTableRep::decode(&signal.data, signal.section(0)) {
      Ok(rep) => rep,
      Err(e) => {
        ic_port::debug_print!(
          IC_NDB_MESSAGE_LEVEL,
          "Unreadable ALTER_TABLE_REP from node {}: {}",
          node_id,
          e.message()
        );
        return;
      }
    };
    let dropped = self.shared.dict_cache.table_changed(
      &rep.name,
      rep.table_id,
      rep.table_version,
    );
    ic_port::debug_print!(
      IC_NDB_MESSAGE_LEVEL,
      "Node {} says {} (id {}, version {:#x}) was {}; {} cached object(s) \
       let go of",
      node_id,
      rep.name,
      rep.table_id,
      rep.table_version,
      if rep.is_drop() { "dropped" } else { "altered" },
      dropped
    );
  }

  /// The node answered a heartbeat, and says how it is.
  fn registration_confirmed(&self, index: usize, signal: &ReceivedSignal) {
    let node = &self.nodes[index].node;
    let conf = match ApiRegConf::decode(&signal.data) {
      Ok(conf) => conf,
      Err(e) => {
        ic_port::debug_print!(
          IC_HEARTBEAT_LEVEL,
          "Unreadable API_REGCONF from node {}: {}",
          node.node_id,
          e.message()
        );
        return;
      }
    };
    if conf.api_heartbeat_interval != 0 {
      // The signal carries hundredths of a second.
      node.set_heartbeat_interval_ms(conf.heartbeat_interval_ms());
    }
    node.set_node_state(Some(conf.node_state));
    node
      .published
      .publish_regconf(&conf.node_state, ic_port::time::gethrtime());
  }
}

/// Route every complete signal in `reader` from where it lies: a signal
/// for a user thread goes into that thread's page, copied if it is
/// small and by reference to the receive page if it has `large_words`
/// words or more; a packed one is taken apart and each part routed as
/// if it had come alone, and one for our own blocks is copied out into
/// `own`. The reader is compacted once, before its next read, not once
/// per signal.
fn route_signals(
  reader: &mut SignalReader,
  node_id: u32,
  large_words: usize,
  router: &mut Router,
  parts: &mut Vec<packed::PackedPart>,
  own: &mut Vec<ReceivedSignal>,
) -> Result<(), IcError> {
  while reader.has_message() {
    let len = {
      let words = reader.complete_words();
      let message = header::decode(words)?;
      let head = message.header;
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "<- node {} {} ({} words, {} section(s))",
        node_id,
        gsn::gsn_name(head.gsn()).unwrap_or("unknown"),
        message.data.len(),
        head.num_sections
      );
      if head.receiver_block == blocks::IC_BLOCK_API_PACKED {
        // Every part has the packed signal's number and sender, and the
        // block its own header names. See `ic_ndb_signals::packed`.
        match packed::unpack_into(message.data, parts) {
          Ok(()) => {
            for part in parts.iter() {
              let signal = SignalView {
                gsn: head.gsn(),
                receiver_block: part.receiver_block,
                sender_block: head.sender_block,
                sender_node_id: node_id,
                fragment_info: FragmentInfo::Whole,
                data: &message.data[part.start..part.start + part.len],
                ..SignalView::default()
              };
              if !router.route_view(&signal) {
                own.push(signal.to_owned());
              }
            }
          }
          Err(e) => {
            ic_port::debug_print!(
              IC_NDB_MESSAGE_LEVEL,
              "Unreadable packed signal from node {}: {}",
              node_id,
              e.message()
            );
          }
        }
      } else {
        let signal = SignalView {
          gsn: head.gsn(),
          receiver_block: head.receiver_block,
          sender_block: head.sender_block,
          sender_node_id: node_id,
          fragment_info: head.fragment_info,
          data: message.data,
          sections: message.sections,
          num_sections: head.num_sections as usize,
        };
        let by_reference = large_words > 0
          && message.total_words >= large_words
          && head.fragment_info == FragmentInfo::Whole;
        let routed = if by_reference {
          let base = reader.start_words();
          let mut placed = Placement {
            data_at: base + message.data_start,
            ..Placement::default()
          };
          let mut i: usize = 0;
          while i < signal.num_sections {
            placed.section_at[i] = base + message.section_starts[i];
            i += 1;
          }
          router.route_by_reference(&signal, reader.page(), &placed)
        } else {
          router.route_view(&signal)
        };
        if !routed {
          own.push(signal.to_owned());
        }
      }
      message.total_words
    };
    reader.consume(len);
  }
  Ok(())
}
