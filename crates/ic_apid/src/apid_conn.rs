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

use std::collections::HashMap;
use std::sync::Arc;

use ic_ndb_signals::blocks;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;

use crate::apid_global::ApidShared;
use crate::dict_cache;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::dict_client;
use crate::fragments::FragmentAssembler;
use crate::node_connect::ReceivedSignal;
use crate::thread_conn::ThreadConnection;

/// How long [`ApidConnection::wait_for`] sleeps on its inbox at a time,
/// so that a link lost meanwhile is noticed without waiting for the
/// timeout.
pub const IC_CALL_SLICE_MS: u32 = 100;

/// A reply a request is waiting for.
struct Expectation {
  /// The number the reply echoes in its first data word.
  request_id: u32,
  /// The node the request went to.
  node_id: u32,
  /// The link to that node the request went over. A reply cannot come
  /// over any other.
  generation: u32,
  /// The signal numbers that answer the request.
  gsns: Vec<u16>,
  /// The reply, or why there will be none. `None` while waiting.
  outcome: Option<Result<ReceivedSignal, IcError>>,
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
  ) {
    self.list.push(Expectation {
      request_id,
      node_id,
      generation,
      gsns: gsns.to_vec(),
      outcome: None,
    });
  }

  /// Give a whole signal to the request it answers. Hands the signal
  /// back if no request is waiting for it.
  fn offer(&mut self, signal: ReceivedSignal) -> Option<ReceivedSignal> {
    let first = match signal.data.first() {
      Some(first) => *first,
      None => return Some(signal),
    };
    let mut i: usize = 0;
    while i < self.list.len() {
      let exp = &self.list[i];
      let answers = exp.outcome.is_none()
        && exp.request_id == first
        && exp.node_id == signal.sender_node_id
        && exp.gsns.contains(&signal.gsn);
      if answers {
        self.list[i].outcome = Some(Ok(signal));
        return None;
      }
      i += 1;
    }
    Some(signal)
  }

  /// Complete with `error` every request still waiting on `node_id` that
  /// went over a link other than the one up now, if any is.
  fn link_changed(
    &mut self,
    node_id: u32,
    connected: bool,
    generation: u32,
    error: IcError,
  ) {
    for exp in &mut self.list {
      if exp.outcome.is_some() || exp.node_id != node_id {
        continue;
      }
      if !connected || exp.generation != generation {
        exp.outcome = Some(Err(error));
      }
    }
  }

  /// The nodes some request is still waiting on.
  fn waiting_nodes(&self) -> Vec<u32> {
    let mut nodes: Vec<u32> = Vec::new();
    for exp in &self.list {
      if exp.outcome.is_none() && !nodes.contains(&exp.node_id) {
        nodes.push(exp.node_id);
      }
    }
    nodes
  }

  /// The outcome of a request, if it has one, which also forgets it.
  fn take(
    &mut self,
    request_id: u32,
  ) -> Option<Result<ReceivedSignal, IcError>> {
    let index = self
      .list
      .iter()
      .position(|exp| exp.request_id == request_id && exp.outcome.is_some())?;
    self.list.remove(index).outcome
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
      if exp.outcome.is_none() {
        count += 1;
      }
    }
    count
  }
}

/// A user thread's connection to the cluster.
///
/// Not shared: one thread uses it, which is why nothing in it is
/// locked. Dropping it gives its block number back.
pub struct ApidConnection {
  shared: Arc<ApidShared>,
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
    Ok(ApidConnection {
      shared,
      inbox,
      assembler: FragmentAssembler::new(),
      expectations: Expectations::default(),
      next_request_id: 1,
      unexpected: 0,
      tables: HashMap::new(),
      indexes: HashMap::new(),
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

  /// Send a signal that no reply is expected to.
  pub fn send(
    &mut self,
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
    node.send(header, data, sections)?;
    // Nothing is read from the inbox but by this thread, so the reply
    // cannot be taken before this is recorded.
    self.expectations.add(request_id, node_id, generation, gsns);
    Ok(())
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
    let signals = self.inbox.take(wait_ms);
    let taken = signals.len();
    for signal in signals {
      self.receive(signal);
    }
    self.fail_lost_requests();
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
    if let Some(stray) = self.expectations.offer(whole) {
      self.unexpected += 1;
      ic_port::debug_print!(
        IC_NDB_MESSAGE_LEVEL,
        "No request expects {} from node {} to block {:#06x}",
        gsn::gsn_name(stray.gsn).unwrap_or("an unknown signal"),
        stray.sender_node_id,
        stray.receiver_block
      );
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
    exp.add(7, 1, 3, &[CONF, REF]);
    exp.add(8, 1, 3, &[CONF, REF]);
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
    exp.add(7, 1, 3, &[CONF, REF]);
    // Wrong signal number, wrong node, wrong request number.
    assert!(exp.offer(reply(194, 1, 7)).is_some());
    assert!(exp.offer(reply(CONF, 2, 7)).is_some());
    assert!(exp.offer(reply(CONF, 1, 9)).is_some());
    assert_eq!(exp.waiting(), 1);
  }

  #[test]
  fn a_second_reply_to_an_answered_request_is_unexpected() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF, REF]);
    assert!(exp.offer(reply(CONF, 1, 7)).is_none());
    assert!(exp.offer(reply(REF, 1, 7)).is_some());
  }

  #[test]
  fn a_lost_link_fails_the_requests_that_went_over_it() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF]);
    exp.add(8, 2, 5, &[CONF]);
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
    exp.add(7, 1, 3, &[CONF]);
    let lost = IcError::new(err::IC_ERROR_LINK_LOST);
    exp.link_changed(1, true, 3, lost);
    assert!(exp.take(7).is_none());
    exp.link_changed(1, true, 4, lost);
    assert!(exp.take(7).expect("failed").is_err());
  }

  #[test]
  fn a_request_given_up_on_is_forgotten() {
    let mut exp = Expectations::default();
    exp.add(7, 1, 3, &[CONF]);
    exp.forget(7);
    assert_eq!(exp.waiting(), 0);
    // Its reply, arriving late, is unexpected.
    assert!(exp.offer(reply(CONF, 1, 7)).is_some());
  }

  #[test]
  fn a_signal_without_data_is_nobodys() {
    let mut exp = Expectations::default();
    exp.add(0, 1, 3, &[CONF]);
    let empty = ReceivedSignal {
      gsn: CONF,
      sender_node_id: 1,
      ..ReceivedSignal::default()
    };
    assert!(exp.offer(empty).is_some());
  }
}
