// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The per-thread connection object (`legacy-c/api/ic_apid_conn.ic`,
//! `IC_INT_APID_CONNECTION`, and `ic_apid_exec_message.ic`,
//! `ic_poll_messages`).
//!
//! One per user thread, made by [`ApidGlobal::create_connection`]. It
//! holds the thread's inbox and block number, and turns what arrives in
//! the inbox into answers to the requests the thread has made.
//!
//! What [`poll`](ApidConnection::poll) does with each signal it takes:
//!
//! 1. **Fragments are joined first**, so that nothing past this point,
//!    in this library or above it, ever sees one (doc/rust/02,
//!    "Fragmented signals never leave the library").
//! 2. **The whole signal is matched to a request that expects it**
//!    (doc/rust/02, "Expected replies"): its signal number is one the
//!    request named, it comes from the node the request went to, and its
//!    first data word is the request's own number, which every reply
//!    used so far echoes there.
//! 3. **A signal nobody expects is counted and dropped**, with a trace:
//!    a late reply to a request given up on, or a routing mistake.
//!
//! Then every request still waiting on a node whose link has gone, or
//! been replaced by a new one, since it was sent is completed with an
//! error. Its reply can no longer come, and waiting for it would only
//! run into the timeout.
//!
//! This first form knows requests with a single reply: the first reply
//! that matches completes the request. Key operations, answered by
//! several signals, will extend it.
//!
//! The C executes each signal through a table of handlers indexed by
//! signal number. Matching replies to requests is new; it is what lets
//! several requests wait on one inbox.
//!
//! [`ApidGlobal::create_connection`]:
//!   crate::apid_global::ApidGlobal::create_connection

use std::collections::BTreeMap;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::sync::Arc;

use ic_ndb_signals::blocks;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header;
use ic_ndb_signals::header::FragmentInfo;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::tc_seize::TcReleaseReq;
use ic_ndb_signals::tc_seize::TcSeizeConf;
use ic_ndb_signals::tc_seize::TcSeizeRef;
use ic_ndb_signals::tc_seize::TcSeizeReq;
use ic_ndb_signals::tc_seize::IC_ANY_TC_INSTANCE;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_util::ptr_array::PtrArray;

use crate::apid_global::ApidShared;
use crate::dict_cache;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::dict_client;
use crate::fragments::FragmentAssembler;
use crate::node_connect::ReceivedSignal;
use crate::node_connect::SignalView;
use crate::query::ApidQuery;
use crate::query::QueryId;
use crate::query::TransId;
use crate::signal_page;
use crate::thread_conn::ThreadConnection;
use crate::transaction::Transaction;

/// How long seizing a transaction record waits for the coordinator.
pub const IC_TC_SEIZE_WAIT_MS: u32 = 5_000;

/// How long [`ApidConnection::wait_for`] sleeps on its inbox at a time,
/// so that a link lost meanwhile is noticed without waiting for the
/// timeout.
pub const IC_CALL_SLICE_MS: u32 = 100;

/// The replies a request is waiting for.
struct Expectation {
  /// The number the replies echo in their first data word.
  request_id: u32,
  /// The node the request went to.
  node_id: u32,
  /// The link to that node the request went over. Its loss ends the
  /// wait, whichever node the replies come from.
  generation: u32,
  /// The signal numbers that answer the request.
  gsns: Vec<u16>,
  /// Replies are gathered until the request is forgotten, rather than
  /// the first one completing it.
  several: bool,
  /// Replies may come from any node, as a row does from the node that
  /// read it.
  any_node: bool,
  /// Replies that have come and not been taken.
  replies: Vec<ReceivedSignal>,
  /// Why no more will come, if so.
  failed: Option<IcError>,
}

impl Expectation {
  /// Still taking replies.
  fn open(&self) -> bool {
    self.failed.is_none() && (self.several || self.replies.is_empty())
  }
}

/// The replies a connection is waiting for. Kept apart from the
/// connection so that the matching can be tested without a cluster.
#[derive(Default)]
struct Expectations {
  list: Vec<Expectation>,
}

impl Expectations {
  fn add(
    &mut self,
    request_id: u32,
    node_id: u32,
    generation: u32,
    gsns: &[u16],
    several: bool,
    any_node: bool,
  ) {
    self.list.push(Expectation {
      request_id,
      node_id,
      generation,
      gsns: gsns.to_vec(),
      several,
      any_node,
      replies: Vec::new(),
      failed: None,
    });
  }

  /// Give a whole signal to the request it answers. Hands the signal
  /// back if no request is waiting for it.
  /// True if a request waits for this signal, so that it has to be
  /// kept: what [`offer`](Self::offer) would take.
  fn claims(&self, signal: &SignalView<'_>) -> bool {
    let first = match signal.data.first() {
      Some(first) => *first,
      None => return false,
    };
    for exp in &self.list {
      let from_node = exp.any_node || exp.node_id == signal.sender_node_id;
      if exp.open()
        && exp.request_id == first
        && from_node
        && exp.gsns.contains(&signal.gsn)
      {
        return true;
      }
    }
    false
  }

  fn offer(&mut self, signal: ReceivedSignal) -> Option<ReceivedSignal> {
    let first = match signal.data.first() {
      Some(first) => *first,
      None => return Some(signal),
    };
    let mut i: usize = 0;
    while i < self.list.len() {
      let exp = &self.list[i];
      let from_node = exp.any_node || exp.node_id == signal.sender_node_id;
      let answers = exp.open()
        && exp.request_id == first
        && from_node
        && exp.gsns.contains(&signal.gsn);
      if answers {
        self.list[i].replies.push(signal);
        return None;
      }
      i += 1;
    }
    Some(signal)
  }

  /// End with `error` every wait on `node_id` for a request that went
  /// over a link other than the one up now, if any is.
  fn link_changed(
    &mut self,
    node_id: u32,
    connected: bool,
    generation: u32,
    error: IcError,
  ) {
    for exp in &mut self.list {
      if !exp.open() || exp.node_id != node_id {
        continue;
      }
      if !connected || exp.generation != generation {
        exp.failed = Some(error);
      }
    }
  }

  /// The nodes some request is still waiting on.
  fn waiting_nodes(&self) -> Vec<u32> {
    let mut nodes: Vec<u32> = Vec::new();
    for exp in &self.list {
      if exp.open() && !nodes.contains(&exp.node_id) {
        nodes.push(exp.node_id);
      }
    }
    nodes
  }

  /// The reply to a request answered by one, or why there will be none.
  /// Taking it forgets the request.
  fn take(
    &mut self,
    request_id: u32,
  ) -> Option<Result<ReceivedSignal, IcError>> {
    let mut i: usize = 0;
    while i < self.list.len() {
      let exp = &self.list[i];
      if exp.request_id == request_id && !exp.several && !exp.open() {
        let mut exp = self.list.remove(i);
        if let Some(e) = exp.failed {
          return Some(Err(e));
        }
        return exp.replies.pop().map(Ok);
      }
      i += 1;
    }
    None
  }

  /// The replies gathered for a request answered by several, each
  /// handed out once, or why no more will come.
  fn take_several(
    &mut self,
    request_id: u32,
  ) -> Result<Vec<ReceivedSignal>, IcError> {
    for exp in &mut self.list {
      if exp.request_id == request_id && exp.several {
        if let Some(e) = exp.failed {
          return Err(e);
        }
        return Ok(std::mem::take(&mut exp.replies));
      }
    }
    Ok(Vec::new())
  }

  /// Stop waiting for a request, answered or not.
  fn forget(&mut self, request_id: u32) {
    let mut i: usize = 0;
    while i < self.list.len() {
      if self.list[i].request_id == request_id {
        self.list.remove(i);
      } else {
        i += 1;
      }
    }
  }

  /// How many requests are still waiting.
  fn waiting(&self) -> usize {
    let mut count: usize = 0;
    for exp in &self.list {
      if exp.open() {
        count += 1;
      }
    }
    count
  }
}

/// A transaction record at one node's coordinator, seized for this
/// thread. The data node frees an API node's records when it loses its
/// link, so a record lives no longer than the link it was seized over.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TcRecord {
  /// The node whose coordinator holds it.
  pub node_id: u32,
  /// The link it was seized over.
  pub(crate) generation: u32,
  /// Our pointer for it, which `TCKEYCONF` names.
  pub api_ptr: u32,
  /// The coordinator's pointer for it.
  pub tc_ptr: u32,
  /// The coordinator block to send to, its instance included.
  pub tc_block: u16,
  /// Where the connection keeps it, which never changes.
  pub(crate) index: u32,
  /// A transaction is using it.
  busy: bool,
  /// Its state is unknown, or the coordinator has freed it: never
  /// handed out again.
  lost: bool,
}

#[cfg(test)]
impl TcRecord {
  /// A record as a test needs one, seized nowhere.
  pub(crate) fn for_test(node_id: u32) -> TcRecord {
    TcRecord {
      node_id,
      generation: 1,
      api_ptr: 1,
      tc_ptr: 1,
      tc_block: 0xF5,
      index: 0,
      busy: true,
      lost: false,
    }
  }
}

/// A user thread's connection to the cluster.
///
/// Not shared: one thread uses it, which is why nothing in it is
/// locked. Dropping it gives its block number back.
pub struct ApidConnection {
  pub(crate) shared: Arc<ApidShared>,
  inbox: Arc<ThreadConnection>,
  assembler: FragmentAssembler,
  expectations: Expectations,
  next_request_id: u32,
  /// Signals that arrived and that no request was waiting for.
  unexpected: u64,
  /// The tables this thread has bound, by internal name.
  tables: HashMap<String, Arc<TableDef>>,
  /// The indexes this thread has bound, by `database/table/index`.
  indexes: HashMap<String, Arc<IndexDef>>,
  /// Transaction records seized at the coordinators, as many per node
  /// as this thread has had transactions in flight there, at positions
  /// that never change.
  tc_records: Vec<TcRecord>,
  /// By node id, the positions of the free ones.
  free_tc: Vec<Vec<u32>>,
  /// The low word of the next transaction id.
  trans_counter: u32,
  /// The started nodes as of the last transaction start, kept for its
  /// allocation.
  pub(crate) started_scratch: Vec<u32>,
  /// The queries made on this connection, by the id a reply names.
  pub(crate) queries: PtrArray<ApidQuery>,
  /// The transactions started on this connection.
  pub(crate) transactions: PtrArray<Transaction>,
  /// Queries completed and not yet taken, oldest first.
  pub(crate) executed: VecDeque<QueryId>,
  /// The transactions not yet done, by the coordinator record pointer
  /// their replies name.
  pub(crate) active: BTreeMap<u32, TransId>,
  /// Signals packed for a node and not yet handed to it, one entry per
  /// node this connection has sent to.
  outgoing: Vec<Outgoing>,
  /// How many query callbacks are running, on this thread, inside the
  /// poll that completed them. A poll from inside one does nothing.
  pub(crate) in_callback: u32,
  /// Where a request's key and attribute sections are built before
  /// they are packed: two buffers for every query, warm in the cache.
  pub(crate) key_scratch: Vec<u32>,
  pub(crate) attr_scratch: Vec<u32>,
  /// Pages of signals taken from the inbox, read, and given back on the
  /// next poll.
  pages: Vec<signal_page::SignalPage>,
}

/// Signals packed for one node, in the order they were queued
/// (the C's `IC_SEND_CLUSTER_NODE` and its pages).
struct Outgoing {
  node_id: u32,
  words: Vec<u32>,
  /// Requests packed when their queries were defined, waiting for the
  /// next send to set their place in the batch: kept apart from
  /// `words`, which a poll sends with what the replies called for.
  staged: Vec<u32>,
  /// Requests in `staged` that will not go after all, as (where, how
  /// many words): a transaction rolled back or lost before its send.
  cancelled: Vec<(u32, u32)>,
}

/// Where a request packed at define time lies, until it is sent.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Staged {
  /// The node it goes to.
  pub node_id: u32,
  /// Where it begins in that node's staged buffer, in words.
  pub at: u32,
  /// Its length in words; zero for nothing staged.
  pub len: u32,
}

impl std::fmt::Debug for ApidConnection {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ApidConnection(block {:#06x}, {} waiting, {} unexpected)",
      self.block_number(),
      self.expectations.waiting(),
      self.unexpected
    )
  }
}

impl ApidConnection {
  pub(crate) fn new(
    shared: Arc<ApidShared>,
  ) -> Result<ApidConnection, IcError> {
    let inbox = shared.thread_table.allocate()?;
    let trans_counter = shared.thread_table.trans_counter(inbox.thread_id());
    Ok(ApidConnection {
      shared,
      inbox,
      assembler: FragmentAssembler::new(),
      expectations: Expectations::default(),
      next_request_id: 1,
      unexpected: 0,
      tables: HashMap::new(),
      indexes: HashMap::new(),
      tc_records: Vec::new(),
      free_tc: Vec::new(),
      trans_counter,
      started_scratch: Vec::new(),
      queries: PtrArray::new(),
      transactions: PtrArray::new(),
      executed: VecDeque::new(),
      active: BTreeMap::new(),
      outgoing: Vec::new(),
      in_callback: 0,
      pages: Vec::new(),
      key_scratch: Vec::new(),
      attr_scratch: Vec::new(),
    })
  }

  /// This thread's number among the user threads.
  pub fn thread_id(&self) -> u32 {
    self.inbox.thread_id()
  }

  /// The block number data nodes address this thread's replies to.
  pub fn block_number(&self) -> u16 {
    self.inbox.block_number()
  }

  /// This thread's block reference: its block number with our node id.
  /// It goes in every request, as the address for the reply. Our node
  /// id can change after every link has been lost, so it is read anew.
  pub fn block_ref(&self) -> u32 {
    blocks::number_to_ref(self.block_number(), self.shared.own_node_id())
  }

  /// A number for a new request, for the reply to echo.
  pub fn next_request_id(&mut self) -> u32 {
    let id = self.next_request_id;
    self.next_request_id = self.next_request_id.wrapping_add(1);
    id
  }

  /// The data nodes that have a link up and say they are started.
  pub fn started_nodes(&self) -> Vec<u32> {
    self.shared.started_nodes()
  }

  /// Signals that arrived and that no request was waiting for.
  pub fn unexpected(&self) -> u64 {
    self.unexpected
  }

  /// How many requests are still waiting for their reply.
  pub fn waiting(&self) -> usize {
    self.expectations.waiting()
  }

  /// Pack a signal for a node, to go with the next
  /// [`send_queued`](Self::send_queued). This is how a batch of
  /// operations becomes one socket write per node.
  pub fn queue_signal(
    &mut self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
  ) -> Result<(), IcError> {
    let use_checksum = match self.shared.node(node_id) {
      Some(node) => node.uses_checksum(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    let index = self.outgoing_index(node_id);
    let out = &mut self.outgoing[index].words;
    header::encode(header, data, sections, use_checksum, out)?;
    ic_port::debug_print!(
      IC_NDB_MESSAGE_LEVEL,
      "-> node {} {} ({} words, {} sections) queued",
      node_id,
      gsn::gsn_name(header.gsn()).unwrap_or("unknown"),
      data.len(),
      sections.len()
    );
    Ok(())
  }

  fn outgoing_index(&mut self, node_id: u32) -> usize {
    let mut index: usize = 0;
    while index < self.outgoing.len() {
      if self.outgoing[index].node_id == node_id {
        return index;
      }
      index += 1;
    }
    self.outgoing.push(Outgoing {
      node_id,
      words: Vec::new(),
      staged: Vec::new(),
      cancelled: Vec::new(),
    });
    index
  }

  /// Pack a request for a node when its query is defined, to be sent
  /// by the next [`send_staged`](Self::send_staged) once its flags are
  /// set with [`patch_staged`](Self::patch_staged).
  pub(crate) fn stage_signal(
    &mut self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
  ) -> Result<Staged, IcError> {
    let use_checksum = match self.shared.node(node_id) {
      Some(node) => node.uses_checksum(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    let index = self.outgoing_index(node_id);
    let out = &mut self.outgoing[index].staged;
    let at = out.len();
    let len = header::encode(header, data, sections, use_checksum, out)?;
    Ok(Staged {
      node_id,
      at: at as u32,
      len: len as u32,
    })
  }

  /// Set one word of a staged request, its data word `data_word`, and
  /// its checksum again if the link carries them.
  pub(crate) fn patch_staged(
    &mut self,
    staged: &Staged,
    data_word: usize,
    value: u32,
  ) {
    let use_checksum = match self.shared.node(staged.node_id) {
      Some(node) => node.uses_checksum(),
      None => false,
    };
    let index = self.outgoing_index(staged.node_id);
    let out = &mut self.outgoing[index].staged;
    let at = staged.at as usize;
    let len = staged.len as usize;
    let word = at + header::IC_SIGNAL_HEADER_WORDS + data_word;
    if len == 0 || at + len > out.len() || word >= at + len {
      return;
    }
    out[word] = value;
    if use_checksum {
      out[at + len - 1] = header::compute_checksum(&out[at..at + len - 1]);
    }
  }

  /// A staged request that is not to go after all.
  pub(crate) fn unstage(&mut self, staged: &Staged) {
    if staged.len == 0 {
      return;
    }
    let index = self.outgoing_index(staged.node_id);
    self.outgoing[index].cancelled.push((staged.at, staged.len));
  }

  /// Move every staged request, less the cancelled ones, to be written
  /// with the next [`send_queued`](Self::send_queued). Every staged
  /// request must have had its flags set by then.
  pub(crate) fn send_staged(&mut self) {
    for out in &mut self.outgoing {
      if !out.cancelled.is_empty() {
        out.cancelled.sort_unstable();
        let mut kept: usize = 0;
        let mut from: usize = 0;
        for (at, len) in out.cancelled.drain(..) {
          let (at, len) = (at as usize, len as usize);
          out.staged.copy_within(from..at, kept);
          kept += at - from;
          from = at + len;
        }
        let end = out.staged.len();
        out.staged.copy_within(from..end, kept);
        kept += end - from;
        out.staged.truncate(kept);
      }
      if out.staged.is_empty() {
        continue;
      }
      if out.words.is_empty() {
        std::mem::swap(&mut out.words, &mut out.staged);
      } else {
        out.words.extend_from_slice(&out.staged);
        out.staged.clear();
      }
    }
  }

  /// Hand every node what has been packed for it, one write each
  /// (`ic_send_messages`). With `force` the writes happen now; without
  /// it a node's write may be held back for a moment to go with other
  /// threads' signals, by the adaptive send algorithm
  /// ([`adaptive_send`](crate::adaptive_send)). Returns the first
  /// error, after every node has been given its signals.
  pub fn send_queued(&mut self, force: bool) -> Result<(), IcError> {
    let mut first_error: Option<IcError> = None;
    let mut index: usize = 0;
    while index < self.outgoing.len() {
      if !self.outgoing[index].words.is_empty() {
        let node_id = self.outgoing[index].node_id;
        let result = match self.shared.node(node_id) {
          Some(node) => node.send_words(&self.outgoing[index].words, force),
          None => Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
        };
        self.outgoing[index].words.clear();
        if let Err(e) = result {
          if first_error.is_none() {
            first_error = Some(e);
          }
        }
      }
      index += 1;
    }
    match first_error {
      Some(e) => Err(e),
      None => Ok(()),
    }
  }

  /// Send a signal now, with whatever else is packed for its node. For
  /// a request that will be waited for, and for what no reply is
  /// expected to.
  pub fn send(
    &mut self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
  ) -> Result<(), IcError> {
    self.queue_signal(node_id, header, data, sections)?;
    self.send_queued(true)
  }

  /// Send a request, and say which signals answer it: `gsns`, echoing
  /// `request_id` in their first data word. The reply is collected by
  /// [`poll`](Self::poll) and handed out by [`take_reply`](Self::take_reply).
  pub fn send_expecting(
    &mut self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
    request_id: u32,
    gsns: &[u16],
  ) -> Result<(), IcError> {
    let node = match self.shared.node(node_id) {
      Some(node) => Arc::clone(node),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    // Read before sending, so that a link replaced while the request is
    // on its way is seen as a different one.
    let generation = node.published.generation();
    self.send(node_id, header, data, sections)?;
    // Nothing is read from the inbox but by this thread, so the reply
    // cannot be taken before this is recorded.
    self
      .expectations
      .add(request_id, node_id, generation, gsns, false, false);
    Ok(())
  }

  /// Say that a request about to be sent to `node_id` is answered by
  /// several signals, `gsns`, echoing `request_id` in their first data
  /// word, gathered until the request is forgotten. With `any_node`
  /// they may come from any node, as a row comes from the node that read
  /// it; the loss of the link to `node_id` still ends the wait. Said
  /// before sending, so that a link replaced meanwhile is seen as
  /// another. The replies are handed out by
  /// [`take_replies`](Self::take_replies).
  pub fn expect_several(
    &mut self,
    request_id: u32,
    node_id: u32,
    gsns: &[u16],
    any_node: bool,
  ) -> Result<(), IcError> {
    let generation = match self.shared.node(node_id) {
      Some(node) => node.published.generation(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    self
      .expectations
      .add(request_id, node_id, generation, gsns, true, any_node);
    Ok(())
  }

  /// The replies gathered for a request that expects several, each
  /// handed out once, or why no more will come.
  pub fn take_replies(
    &mut self,
    request_id: u32,
  ) -> Result<Vec<ReceivedSignal>, IcError> {
    self.expectations.take_several(request_id)
  }

  /// The reply to a request, if it has arrived or will never come.
  /// Taking it forgets the request.
  pub fn take_reply(
    &mut self,
    request_id: u32,
  ) -> Option<Result<ReceivedSignal, IcError>> {
    self.expectations.take(request_id)
  }

  /// Stop waiting for a request. A reply that comes later is counted as
  /// unexpected.
  pub fn forget(&mut self, request_id: u32) {
    self.expectations.forget(request_id);
  }

  /// Take what the inbox holds, waiting up to `wait_ms` if it is empty,
  /// and match it to the requests waiting for it. Returns how many
  /// signals were taken.
  pub fn poll(&mut self, wait_ms: u32) -> usize {
    if self.in_callback > 0 {
      // A query callback is running inside a poll already.
      return 0;
    }
    // The pages read last time go back to the inbox, and the pages
    // posted since come out; the signals are read where they lie.
    let mut pages = std::mem::take(&mut self.pages);
    self.inbox.exchange(wait_ms, &mut pages);
    let mut taken: usize = 0;
    for page in &pages {
      let mut at: usize = 0;
      while let Some(signal) = signal_page::next(page, &mut at) {
        taken += 1;
        self.receive_view(&signal);
      }
    }
    self.pages = pages;
    self.fail_lost_requests();
    self.fail_lost_transactions();
    // What the replies called for, such as commit acknowledgements,
    // goes out together. A failed write has asked for its link to be
    // dropped, which is where its loss is reported.
    let _ = self.send_queued(false);
    taken
  }

  /// Send a request and wait for its reply, for up to `wait_ms`. For a
  /// thread that wants one answer before doing anything else; other
  /// requests may be waiting at the same time and are served as their
  /// replies arrive.
  ///
  /// The request's number is taken from its first data word, where most
  /// requests carry the number their reply echoes. One that carries it
  /// elsewhere, such as `TCRELEASEREQ`, which leads with the
  /// coordinator's record, is sent with [`send_expecting`] and waited
  /// for with [`wait_for`].
  ///
  /// [`send_expecting`]: Self::send_expecting
  /// [`wait_for`]: Self::wait_for
  pub fn call(
    &mut self,
    node_id: u32,
    header: &SignalHeader,
    data: &[u32],
    sections: &[&[u32]],
    gsns: &[u16],
    wait_ms: u32,
  ) -> Result<ReceivedSignal, IcError> {
    let request_id = match data.first() {
      Some(first) => *first,
      None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    };
    self.send_expecting(node_id, header, data, sections, request_id, gsns)?;
    self.wait_for(request_id, wait_ms)
  }

  /// Wait for the reply to a request already sent, for up to `wait_ms`.
  /// A request not answered in time is forgotten.
  pub fn wait_for(
    &mut self,
    request_id: u32,
    wait_ms: u32,
  ) -> Result<ReceivedSignal, IcError> {
    let start = ic_port::time::gethrtime();
    loop {
      if let Some(outcome) = self.take_reply(request_id) {
        return outcome;
      }
      let waited =
        ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
      if waited >= wait_ms as u64 {
        self.forget(request_id);
        return Err(IcError::new(err::IC_ERROR_TIMEOUT));
      }
      let mut slice = wait_ms - waited as u32;
      if slice > IC_CALL_SLICE_MS {
        slice = IC_CALL_SLICE_MS;
      }
      self.poll(slice);
    }
  }

  // ---- Transactions ----

  /// A new transaction id (`Ndb::allocate_transaction_id`): this
  /// thread's block number in bits 52 to 63, our node id in bits 40 to
  /// 51, and a count in the low word. The count goes on from where the
  /// last connection with this block number left it, so an id is not
  /// used twice. Verify: `Ndbif.cpp`, where the first id is made;
  /// `Ndbinit.cpp`, where the count is kept for the block.
  pub fn next_transaction_id(&mut self) -> u64 {
    let low = self.trans_counter;
    self.trans_counter = self.trans_counter.wrapping_add(1);
    let block = self.block_number() as u64 & 0xFFF;
    let node = self.shared.own_node_id() as u64 & 0xFFF;
    (block << 52) | (node << 40) | low as u64
  }

  /// A transaction record at `node_id`'s coordinator for a transaction
  /// to use: a free one this thread holds, or a new one seized. Given
  /// back with [`free_tc_record`](Self::free_tc_record).
  ///
  /// A free record seized over a link that has since been replaced is
  /// lost, as the coordinator freed it when the link went, and is
  /// dropped as it comes up. One seized over the current link while
  /// the link is down is handed out, and the send fails as any send
  /// would. Nothing here looks at more than the record handed out: a
  /// thread with hundreds of transactions in flight starts each in
  /// constant time.
  pub fn tc_record(&mut self, node_id: u32) -> Result<TcRecord, IcError> {
    let generation = match self.shared.node(node_id) {
      Some(node) => node.published.generation(),
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_NODE)),
    };
    let slot = node_id as usize;
    if slot >= self.free_tc.len() {
      self.free_tc.resize_with(slot + 1, Vec::new);
    }
    while let Some(index) = self.free_tc[slot].pop() {
      let rec = &mut self.tc_records[index as usize];
      if rec.generation == generation {
        rec.busy = true;
        return Ok(*rec);
      }
      rec.lost = true;
    }
    let api_ptr = self.next_request_id();
    let seize = TcSeizeReq {
      api_connect_ptr: api_ptr,
      api_block_ref: self.block_ref(),
      instance: IC_ANY_TC_INSTANCE,
    };
    let header = SignalHeader::new(
      gsn::IC_GSN_TCSEIZEREQ,
      self.block_number(),
      blocks::IC_BLOCK_DBTC,
    );
    let replies = [gsn::IC_GSN_TCSEIZECONF, gsn::IC_GSN_TCSEIZEREF];
    let reply = self.call(
      node_id,
      &header,
      &seize.encode(),
      &[],
      &replies,
      IC_TC_SEIZE_WAIT_MS,
    )?;
    if reply.gsn == gsn::IC_GSN_TCSEIZEREF {
      let refusal = TcSeizeRef::decode(&reply.data)?;
      return Err(IcError::new(refusal.error_code as i32));
    }
    let conf = TcSeizeConf::decode(&reply.data)?;
    let rec = TcRecord {
      node_id,
      generation,
      api_ptr,
      tc_ptr: conf.tc_connect_ptr,
      tc_block: blocks::ref_to_block(conf.tc_block_ref),
      index: self.tc_records.len() as u32,
      busy: true,
      lost: false,
    };
    self.tc_records.push(rec);
    Ok(rec)
  }

  /// Give a transaction record back once its transaction is over.
  pub fn free_tc_record(&mut self, rec: &TcRecord) {
    let held = match self.tc_records.get_mut(rec.index as usize) {
      Some(held) => held,
      None => return,
    };
    if held.api_ptr != rec.api_ptr || held.lost || !held.busy {
      return;
    }
    held.busy = false;
    let slot = held.node_id as usize;
    if slot >= self.free_tc.len() {
      self.free_tc.resize_with(slot + 1, Vec::new);
    }
    self.free_tc[slot].push(rec.index);
  }

  /// Stop using a transaction record whose state is not known, such as
  /// one whose transaction timed out. The coordinator keeps it until the
  /// link goes.
  pub fn lose_tc_record(&mut self, rec: &TcRecord) {
    if let Some(held) = self.tc_records.get_mut(rec.index as usize) {
      if held.api_ptr == rec.api_ptr {
        held.lost = true;
      }
    }
  }

  /// True if the link to `node_id` is up and is the one of `generation`.
  pub(crate) fn link_is(&self, node_id: u32, generation: u32) -> bool {
    match self.shared.node(node_id) {
      Some(node) => {
        node.published.is_connected()
          && node.published.generation() == generation
      }
      None => false,
    }
  }

  /// Give back every free record whose link still stands, without
  /// waiting for the answers: the connection is going.
  fn release_tc_records(&mut self) {
    let own_ref = self.block_ref();
    let block = self.block_number();
    for rec in &self.tc_records {
      if rec.busy || !self.link_is(rec.node_id, rec.generation) {
        continue;
      }
      let release = TcReleaseReq {
        tc_connect_ptr: rec.tc_ptr,
        api_block_ref: own_ref,
        api_connect_ptr: rec.api_ptr,
      };
      let header =
        SignalHeader::new(gsn::IC_GSN_TCRELEASEREQ, block, rec.tc_block);
      if let Some(node) = self.shared.node(rec.node_id) {
        let _ = node.send(&header, &release.encode(), &[]);
      }
    }
  }

  // ---- Tables and indexes ----

  /// A table's description (`ic_apid_conn_table_bind`): from what this
  /// thread has bound, from the cache every thread shares, or from the
  /// dictionary, with its hash map. What was bound before is given
  /// again for as long as it is valid, which takes no lock.
  pub fn table_bind(
    &mut self,
    database: &str,
    table: &str,
  ) -> Result<Arc<TableDef>, IcError> {
    let name = dict_client::internal_name(database, table);
    if let Some(def) = self.tables.get(&name) {
      if def.is_valid() {
        return Ok(Arc::clone(def));
      }
    }
    self.tables.remove(&name);
    let shared = Arc::clone(&self.shared);
    let def = dict_cache::bind_table(&shared.dict_cache, self, &name)?;
    self.tables.insert(name, Arc::clone(&def));
    Ok(def)
  }

  /// An index's description (`ic_apid_conn_index_bind`), for the table's
  /// current version. The name is the index's own, as `ndb_desc` lists
  /// it, such as `uk$unique` for the hash part of a unique key.
  pub fn index_bind(
    &mut self,
    database: &str,
    index: &str,
    table: &str,
  ) -> Result<Arc<IndexDef>, IcError> {
    let table_def = self.table_bind(database, table)?;
    let key = format!("{}/{}/{}", database, table, index);
    if let Some(def) = self.indexes.get(&key) {
      let fits = def.is_valid()
        && def.table_id() == table_def.table_id()
        && def.table_version() == table_def.table_version();
      if fits {
        return Ok(Arc::clone(def));
      }
    }
    self.indexes.remove(&key);
    let shared = Arc::clone(&self.shared);
    let def = dict_cache::bind_index(
      &shared.dict_cache,
      self,
      database,
      index,
      &table_def,
    )?;
    self.indexes.insert(key, Arc::clone(&def));
    Ok(def)
  }

  /// Stop keeping a table bound (`ic_apid_conn_table_unbind`). It lives
  /// on for as long as anyone holds it.
  pub fn table_unbind(&mut self, table: &TableDef) {
    let held = match self.tables.get(table.name()) {
      Some(def) => std::ptr::eq(Arc::as_ptr(def), table),
      None => false,
    };
    if held {
      self.tables.remove(table.name());
    }
  }

  /// Say that an operation failed because the table has changed since
  /// it was bound. Every thread's next bind fetches it again.
  pub fn table_changed(&mut self, table: &TableDef) {
    self.shared.dict_cache.forget_table(table);
    self.table_unbind(table);
  }

  /// One signal from the inbox: joined, then matched.
  /// A signal read where it lies. Only what has to be kept is copied:
  /// a fragment, which waits for the rest of its train, and a reply a
  /// request waits for. The rest, which is nearly everything, is
  /// executed in place.
  fn receive_view(&mut self, signal: &SignalView<'_>) {
    let whole = signal.fragment_info == FragmentInfo::Whole;
    if !whole || self.expectations.claims(signal) {
      self.receive(signal.to_owned());
      return;
    }
    if !self.route_reply(signal) {
      self.note_stray(signal);
    }
  }

  fn note_stray(&mut self, signal: &SignalView<'_>) {
    self.unexpected += 1;
    ic_port::debug_print!(
      IC_NDB_MESSAGE_LEVEL,
      "No request expects {} from node {} to block {:#06x}",
      gsn::gsn_name(signal.gsn).unwrap_or("an unknown signal"),
      signal.sender_node_id,
      signal.receiver_block
    );
  }

  fn receive(&mut self, signal: ReceivedSignal) {
    let sender = signal.sender_node_id;
    let added = match self.assembler.add(signal) {
      Ok(added) => added,
      Err(e) => {
        ic_port::debug_print!(
          IC_NDB_MESSAGE_LEVEL,
          "Dropped a broken train of fragments from node {}: {}",
          sender,
          e.message()
        );
        self.unexpected += 1;
        return;
      }
    };
    let whole = match added {
      Some(whole) => whole,
      // More fragments to come.
      None => return,
    };
    let unclaimed = match self.expectations.offer(whole) {
      Some(unclaimed) => unclaimed,
      None => return,
    };
    // Not a request's reply: a transaction's, or a query's, if the
    // first word names one.
    let view = unclaimed.view();
    if !self.route_reply(&view) {
      self.note_stray(&view);
    }
  }

  /// Complete the requests whose link has gone since they were sent.
  fn fail_lost_requests(&mut self) {
    for node_id in self.expectations.waiting_nodes() {
      let node = match self.shared.node(node_id) {
        Some(node) => Arc::clone(node),
        None => continue,
      };
      let connected = node.published.is_connected();
      let generation = node.published.generation();
      let error = node.not_connected_error();
      self
        .expectations
        .link_changed(node_id, connected, generation, error);
      if !connected {
        // Fragments begun over that link can never be finished.
        self.assembler.clear_node(node_id);
      }
    }
  }
}

impl Drop for ApidConnection {
  fn drop(&mut self) {
    self.release_tc_records();
    self
      .shared
      .thread_table
      .keep_trans_counter(self.inbox.thread_id(), self.trans_counter);
    self.shared.thread_table.release(&self.inbox);
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  const CONF: u16 = 190;
  const REF: u16 = 23;

  fn reply(gsn: u16, node: u32, first: u32) -> ReceivedSignal {
    ReceivedSignal {
      gsn,
      sender_node_id: node,
      data: vec![first, 17],
      ..ReceivedSignal::default()
    }
  }

  #[test]
  fn a_reply_goes_to_the_request_it_echoes() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF, REF], false, false);
    exp.add(8, 1, 3, &[CONF, REF], false, false);
    assert!(exp.offer(reply(CONF, 1, 8)).is_none());
    assert_eq!(exp.waiting(), 1);
    let got = exp.take(8).expect("answered").expect("ok");
    assert_eq!(got.data[0], 8);
    // Taking it forgets it.
    assert!(exp.take(8).is_none());
    // The other is still waiting and has nothing to take.
    assert!(exp.take(7).is_none());
  }

  #[test]
  fn a_signal_of_another_kind_or_node_is_not_a_reply() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF, REF], false, false);
    // Wrong signal number, wrong node, wrong request number.
    assert!(exp.offer(reply(194, 1, 7)).is_some());
    assert!(exp.offer(reply(CONF, 2, 7)).is_some());
    assert!(exp.offer(reply(CONF, 1, 9)).is_some());
    assert_eq!(exp.waiting(), 1);
  }

  #[test]
  fn a_second_reply_to_an_answered_request_is_unexpected() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF, REF], false, false);
    assert!(exp.offer(reply(CONF, 1, 7)).is_none());
    assert!(exp.offer(reply(REF, 1, 7)).is_some());
  }

  #[test]
  fn a_lost_link_fails_the_requests_that_went_over_it() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF], false, false);
    exp.add(8, 2, 5, &[CONF], false, false);
    let lost = IcError::new(err::IC_ERROR_LINK_LOST);
    // Node 1's link went down.
    exp.link_changed(1, false, 3, lost);
    let e = exp.take(7).expect("failed").expect_err("an error");
    assert_eq!(e.code, err::IC_ERROR_LINK_LOST);
    // Node 2 is untouched.
    assert!(exp.take(8).is_none());
    assert_eq!(exp.waiting_nodes(), vec![2]);
  }

  #[test]
  fn a_replaced_link_fails_them_too() {
    // Down and up again between two polls: the link is up, but it is a
    // new one, and the reply to the old request is not coming over it.
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF], false, false);
    let lost = IcError::new(err::IC_ERROR_LINK_LOST);
    exp.link_changed(1, true, 3, lost);
    assert!(exp.take(7).is_none());
    exp.link_changed(1, true, 4, lost);
    assert!(exp.take(7).expect("failed").is_err());
  }

  #[test]
  fn a_request_given_up_on_is_forgotten() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF], false, false);
    exp.forget(7);
    assert_eq!(exp.waiting(), 0);
    // Its reply, arriving late, is unexpected.
    assert!(exp.offer(reply(CONF, 1, 7)).is_some());
  }

  #[test]
  fn a_signal_without_data_is_nobodys() {
    let mut exp = Expectations::default();
    exp.add(0, 1, 3, &[CONF], false, false);
    let empty = ReceivedSignal {
      gsn: CONF,
      sender_node_id: 1,
      ..ReceivedSignal::default()
    };
    assert!(exp.offer(empty).is_some());
  }

  const ROW: u16 = 5;
  const KEYREF: u16 = 11;

  #[test]
  fn several_replies_are_gathered_from_any_node() {
    let mut exp = Expectations::default();
    // An operation sent to node 1, whose row may come from any node.
    exp.add(22, 1, 3, &[KEYREF, ROW], true, true);
    assert!(exp.offer(reply(ROW, 2, 22)).is_none());
    assert!(exp.offer(reply(ROW, 3, 22)).is_none());
    let got = exp.take_several(22).expect("replies");
    assert_eq!(got.len(), 2);
    assert_eq!(got[1].sender_node_id, 3);
    // Each is handed out once; the request still waits.
    assert!(exp.take_several(22).expect("none yet").is_empty());
    assert!(exp.offer(reply(KEYREF, 1, 22)).is_none());
    assert_eq!(exp.take_several(22).expect("the refusal").len(), 1);
    assert_eq!(exp.waiting(), 1);
    exp.forget(22);
    assert!(exp.offer(reply(ROW, 2, 22)).is_some());
  }

  #[test]
  fn several_replies_from_one_node_only_take_that_node() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF], true, false);
    assert!(exp.offer(reply(CONF, 2, 7)).is_some());
    assert!(exp.offer(reply(CONF, 1, 7)).is_none());
  }

  #[test]
  fn losing_the_link_ends_several_replies_too() {
    let mut exp = Expectations::default();
    exp.add(22, 1, 3, &[ROW], true, true);
    let lost = IcError::new(err::IC_ERROR_LINK_LOST);
    exp.link_changed(1, false, 3, lost);
    let e = exp.take_several(22).expect_err("ended");
    assert_eq!(e.code, err::IC_ERROR_LINK_LOST);
    // A row arriving after that is nobody's.
    assert!(exp.offer(reply(ROW, 2, 22)).is_some());
  }
}
