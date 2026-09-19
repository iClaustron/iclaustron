// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Fetching a table's description from the data nodes
//! (`legacy-c/api/ic_apid_handle_dict_messages.ic` holds the C's
//! handlers for the answers).
//!
//! The API never creates tables; that is done through a MySQL server.
//! What it needs is a table's shape: its id and version, which every
//! operation on it names, and its columns with their types, sizes and
//! keys. It asks the dictionary of a started data node with
//! `GET_TABINFOREQ` and reads the answer.
//!
//! **Retrying, as the reference does.** A dictionary that is busy says
//! so, and the question is asked again; so is one whose node went away
//! while answering, or that did not answer in time. Up to a hundred
//! attempts, 50 to 100 ms apart and spread a little so that many clients
//! do not ask in step. The reference spreads them at random; here the
//! request number stands in for chance. Each attempt goes to the next
//! started node in turn.
//!
//! **Only for a thread with nothing else in flight.** The fetch waits on
//! the thread's inbox, and anything else that arrives meanwhile is not
//! for it and is dropped, with a trace. The per-thread connection object
//! that comes next keeps such signals for whoever is waiting on them,
//! by having every request say which replies it expects
//! (doc/rust/02-architecture.md, "Expected replies").
//!
//! A table placed by hash map also needs its hash map, which says which
//! fragment a key hash belongs to: [`get_hash_map`] fetches it by the id
//! the table names. The reference does both in one call.
//!
//! Verify: `NdbDictionaryImpl.cpp`, `NdbDictInterface::getTable` and
//! `dictSignal`.

use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;

use ic_ndb_signals::blocks;
use ic_ndb_signals::dict_tab_info;
use ic_ndb_signals::dict_tab_info::HashMapInfo;
use ic_ndb_signals::dict_tab_info::TableInfo;
use ic_ndb_signals::get_tab_info;
use ic_ndb_signals::get_tab_info::GetTabInfoConf;
use ic_ndb_signals::get_tab_info::GetTabInfoRef;
use ic_ndb_signals::get_tab_info::GetTabInfoReq;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::list_tables;
use ic_ndb_signals::list_tables::ListTablesConf;
use ic_ndb_signals::list_tables::ListTablesReq;
use ic_ndb_signals::list_tables::ListedObject;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;

use crate::apid_global::ApidGlobal;
use crate::fragments::FragmentAssembler;
use crate::node_connect::ReceivedSignal;
use crate::thread_conn::ThreadConnection;

/// How long one attempt waits for its answer.
pub const IC_DICT_WAIT_MS: u32 = 10_000;
/// How many attempts before giving up, as the reference.
pub const IC_DICT_ATTEMPTS: u32 = 100;
/// The schema every table lives in, as far as the dictionary's names go.
pub const IC_DICT_SCHEMA: &str = "def";

/// Numbers requests, so that a late answer to an earlier attempt is
/// recognised as such. Any number will do as long as it moves on.
static NEXT_REQUEST: AtomicU32 = AtomicU32::new(1);

/// What to ask the dictionary for.
enum Asking<'a> {
  /// A table, by its internal name.
  ByName(&'a str),
  /// Any object, such as a hash map or an index, by its id.
  ById(u32),
  /// The objects that depend on a table, such as its indexes.
  DependentsOf(u32),
}

/// How one attempt ended.
enum Attempt {
  /// The whole answer, fragments joined, not yet read.
  Done(ReceivedSignal),
  /// Worth asking again.
  Again(IcError),
  /// Not worth asking again.
  Failed(IcError),
}

/// The name the dictionary knows a table by: `database/def/table`.
pub fn internal_name(database: &str, table: &str) -> String {
  format!("{}/{}/{}", database, IC_DICT_SCHEMA, table)
}

/// Fetch a table's description.
///
/// `inbox` is the calling thread's own; see the module note on what
/// happens to anything else that arrives in it meanwhile.
pub fn get_table(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  database: &str,
  table: &str,
) -> Result<TableInfo, IcError> {
  let name = internal_name(database, table);
  let answer = fetch(global, inbox, Asking::ByName(&name))?;
  // A description that cannot be read would read the same way again,
  // so it is not asked for again.
  dict_tab_info::parse_table_info(answer.section(0))
}

/// Fetch a table or an index by id. An index is described as a table of
/// its own, whose `primary_table_id` names the table it indexes.
pub fn get_table_by_id(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  table_id: u32,
) -> Result<TableInfo, IcError> {
  let answer = fetch(global, inbox, Asking::ById(table_id))?;
  dict_tab_info::parse_table_info(answer.section(0))
}

/// The objects that depend on a table: its indexes, and others such as
/// triggers and the tables holding its large objects.
pub fn list_dependents(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  table_id: u32,
) -> Result<Vec<ListedObject>, IcError> {
  let answer = fetch(global, inbox, Asking::DependentsOf(table_id))?;
  list_tables::parse_listed_objects(answer.section(0), answer.section(1))
}

/// Fetch a hash map, by the id a table names in `hash_map_object_id`.
///
/// A key operation needs its table's hash map, to find the fragment and
/// so the node a key belongs to. The reference fetches it as part of
/// fetching the table; here it is its own request, since printing a
/// table needs only its name.
pub fn get_hash_map(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  object_id: u32,
) -> Result<HashMapInfo, IcError> {
  let answer = fetch(global, inbox, Asking::ById(object_id))?;
  dict_tab_info::parse_hash_map_info(answer.section(0))
}

/// Ask until answered, or until asking again is pointless.
fn fetch(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  asking: Asking<'_>,
) -> Result<ReceivedSignal, IcError> {
  let mut attempt: u32 = 0;
  loop {
    match fetch_once(global, inbox, &asking, attempt) {
      Attempt::Done(answer) => return Ok(answer),
      Attempt::Failed(e) => return Err(e),
      Attempt::Again(e) => {
        attempt += 1;
        if attempt >= IC_DICT_ATTEMPTS {
          return Err(e);
        }
        let pause = retry_pause_ms(attempt);
        ic_port::debug_print!(
          IC_NDB_MESSAGE_LEVEL,
          "Asking the dictionary again in {} ms: {}",
          pause,
          e.message()
        );
        ic_port::time::microsleep(pause * 1000);
      }
    }
  }
}

/// One question and the wait for its answer.
fn fetch_once(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  asking: &Asking<'_>,
  attempt: u32,
) -> Attempt {
  let started = global.started_nodes();
  if started.is_empty() {
    return Attempt::Again(IcError::new(err::IC_ERROR_NO_STARTED_DATA_NODE));
  }
  // The next node in turn, so that a node that keeps failing does not
  // take every attempt.
  let node_id = started[attempt as usize % started.len()];
  let request_id = NEXT_REQUEST.fetch_add(1, Ordering::Relaxed);
  let own_ref =
    blocks::number_to_ref(inbox.block_number(), global.own_node_id());
  let mut gsn_value = gsn::IC_GSN_GET_TABINFOREQ;
  if let Asking::DependentsOf(_) = asking {
    gsn_value = gsn::IC_GSN_LIST_TABLES_REQ;
  }
  let header =
    SignalHeader::new(gsn_value, inbox.block_number(), blocks::IC_BLOCK_DBDICT);
  let sent = match asking {
    Asking::ByName(name) => {
      let request = GetTabInfoReq::by_name(request_id, own_ref, name);
      let section = get_tab_info::name_section(name);
      let sections: [&[u32]; 1] = [&section];
      global.send(node_id, &header, &request.encode(), &sections)
    }
    Asking::ById(object_id) => {
      let request = GetTabInfoReq::by_id(request_id, own_ref, *object_id);
      global.send(node_id, &header, &request.encode(), &[])
    }
    Asking::DependentsOf(table_id) => {
      let request =
        ListTablesReq::dependents_of(request_id, own_ref, *table_id);
      global.send(node_id, &header, &request.encode(), &[])
    }
  };
  if let Err(e) = sent {
    return Attempt::Again(e);
  }
  wait_for_answer(global, inbox, node_id, request_id)
}

fn wait_for_answer(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  node_id: u32,
  request_id: u32,
) -> Attempt {
  let mut assembler = FragmentAssembler::new();
  let start = ic_port::time::gethrtime();
  loop {
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if waited >= IC_DICT_WAIT_MS as u64 {
      return Attempt::Again(IcError::new(err::IC_ERROR_TIMEOUT));
    }
    if !node_is_up(global, node_id) {
      // The node went away while answering; another one may do.
      return Attempt::Again(IcError::new(err::IC_ERROR_LINK_LOST));
    }
    // Wake now and then to notice the node going, even if nothing comes.
    let mut wait_ms = IC_DICT_WAIT_MS - waited as u32;
    if wait_ms > 100 {
      wait_ms = 100;
    }
    for signal in inbox.take(wait_ms) {
      let outcome = take_answer(&mut assembler, signal, request_id);
      if let Some(attempt) = outcome {
        return attempt;
      }
    }
  }
}

/// Look at one signal. `None` means keep waiting.
fn take_answer(
  assembler: &mut FragmentAssembler,
  signal: ReceivedSignal,
  request_id: u32,
) -> Option<Attempt> {
  if signal.gsn == gsn::IC_GSN_GET_TABINFO_CONF
    || signal.gsn == gsn::IC_GSN_LIST_TABLES_CONF
  {
    let added = match assembler.add(signal) {
      Ok(added) => added,
      // A broken train of fragments; the whole answer is lost.
      Err(e) => return Some(Attempt::Again(e)),
    };
    // `None` while fragments are still to come: keep waiting.
    let whole = added?;
    if whole.gsn == gsn::IC_GSN_LIST_TABLES_CONF {
      return list_of(whole, request_id);
    }
    return description_of(whole, request_id);
  }
  if signal.gsn == gsn::IC_GSN_GET_TABINFOREF {
    let refusal = match GetTabInfoRef::decode(&signal.data) {
      Ok(refusal) => refusal,
      Err(e) => return Some(Attempt::Again(e)),
    };
    if refusal.sender_data != request_id {
      // A late answer to an earlier attempt.
      return None;
    }
    return Some(refused(&refusal));
  }
  ic_port::debug_print!(
    IC_NDB_MESSAGE_LEVEL,
    "Dropped {} from node {} while waiting for the dictionary",
    gsn::gsn_name(signal.gsn).unwrap_or("an unknown signal"),
    signal.sender_node_id
  );
  None
}

/// A whole description: check it is ours and complete.
fn description_of(whole: ReceivedSignal, request_id: u32) -> Option<Attempt> {
  let conf = match GetTabInfoConf::decode(&whole.data) {
    Ok(conf) => conf,
    Err(e) => return Some(Attempt::Again(e)),
  };
  if conf.sender_data != request_id {
    return None;
  }
  if whole.section(0).len() != conf.total_len as usize {
    // Fragments went missing on the way.
    let e = IcError::new(err::IC_ERROR_BAD_TABLE_DESCRIPTION);
    return Some(Attempt::Again(e));
  }
  Some(Attempt::Done(whole))
}

/// A whole list: check it is ours. Its count is not checked, since once
/// joined it is the first piece's alone; see `list_tables`.
fn list_of(whole: ReceivedSignal, request_id: u32) -> Option<Attempt> {
  let conf = match ListTablesConf::decode(&whole.data) {
    Ok(conf) => conf,
    Err(e) => return Some(Attempt::Again(e)),
  };
  if conf.sender_data != request_id {
    return None;
  }
  Some(Attempt::Done(whole))
}

/// What a refusal amounts to.
fn refused(refusal: &GetTabInfoRef) -> Attempt {
  let code = refusal.error_code;
  if code == get_tab_info::IC_GET_TABINFO_ERR_BUSY {
    return Attempt::Again(IcError::new(err::IC_ERROR_DICT_REFUSED));
  }
  if code == get_tab_info::IC_GET_TABINFO_ERR_NOT_DEFINED
    || code == get_tab_info::IC_GET_TABINFO_ERR_NO_SUCH_ID
  {
    return Attempt::Failed(IcError::new(err::IC_ERROR_NO_SUCH_TABLE));
  }
  ic_port::debug_print!(
    IC_NDB_MESSAGE_LEVEL,
    "The dictionary refused with NDB error {} (line {})",
    code,
    refusal.error_line
  );
  Attempt::Failed(IcError::new(err::IC_ERROR_DICT_REFUSED))
}

fn node_is_up(global: &ApidGlobal, node_id: u32) -> bool {
  match global.node(node_id) {
    Some(node) => node.published.is_connected(),
    None => false,
  }
}

/// The pause before an attempt, as the reference: 50 ms and up to 40
/// more, then up to 90 more after half the attempts, then from 100 ms
/// after three quarters of them.
fn retry_pause_ms(attempt: u32) -> u32 {
  let mut base: u32 = 50;
  let mut spread: u32 = 5;
  if attempt >= IC_DICT_ATTEMPTS / 2 {
    spread = 10;
  }
  if attempt >= 3 * IC_DICT_ATTEMPTS / 4 {
    base = 100;
  }
  let chance = NEXT_REQUEST.load(Ordering::Relaxed);
  base + 10 * (chance % spread)
}

#[cfg(test)]
mod tests {
  use super::*;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_END;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_LENGTH;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_EXT_TYPE;
  use ic_ndb_signals::dict_tab_info::IC_DTI_ATTRIBUTE_NAME;
  use ic_ndb_signals::dict_tab_info::IC_DTI_NO_OF_ATTRIBUTES;
  use ic_ndb_signals::dict_tab_info::IC_DTI_TABLE_NAME;
  use ic_ndb_signals::dict_tab_info::IC_NDB_TYPE_INT;
  use ic_ndb_signals::header::FragmentInfo;
  use ic_ndb_signals::simple_properties::PropertyWriter;

  fn description() -> Vec<u32> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_DTI_TABLE_NAME, "ictest/def/t1");
    w.add_u32(IC_DTI_NO_OF_ATTRIBUTES, 1);
    w.add_string(IC_DTI_ATTRIBUTE_NAME, "id");
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_TYPE, IC_NDB_TYPE_INT);
    w.add_u32(IC_DTI_ATTRIBUTE_EXT_LENGTH, 1);
    w.add_u32(IC_DTI_ATTRIBUTE_END, 0);
    w.words().to_vec()
  }

  fn conf(request_id: u32, total_len: usize) -> Vec<u32> {
    vec![request_id, 17, 0, total_len as u32, 2, 0x00FA_0001]
  }

  #[test]
  fn names_are_database_schema_table() {
    assert_eq!(internal_name("ictest", "t1"), "ictest/def/t1");
  }

  #[test]
  fn a_whole_answer_gives_the_table() {
    let words = description();
    let signal = ReceivedSignal {
      gsn: gsn::IC_GSN_GET_TABINFO_CONF,
      data: conf(7, words.len()),
      sections: vec![words],
      ..ReceivedSignal::default()
    };
    let mut assembler = FragmentAssembler::new();
    match take_answer(&mut assembler, signal, 7) {
      Some(Attempt::Done(answer)) => {
        let words = answer.section(0);
        let info = dict_tab_info::parse_table_info(words).expect("table");
        assert_eq!(info.table_name(), "t1");
      }
      _ => panic!("expected the table"),
    }
  }

  #[test]
  fn an_answer_in_fragments_gives_the_table() {
    // A description over 7400 words arrives in fragments; split this
    // small one in two to take the same path.
    let words = description();
    let half = words.len() / 2;
    let mut first_data = conf(7, words.len());
    first_data.push(0);
    first_data.push(33);
    let mut last_data = conf(7, words.len());
    last_data.push(0);
    last_data.push(33);
    let first = ReceivedSignal {
      gsn: gsn::IC_GSN_GET_TABINFO_CONF,
      fragment_info: FragmentInfo::First,
      data: first_data,
      sections: vec![words[..half].to_vec()],
      ..ReceivedSignal::default()
    };
    let last = ReceivedSignal {
      gsn: gsn::IC_GSN_GET_TABINFO_CONF,
      fragment_info: FragmentInfo::Last,
      data: last_data,
      sections: vec![words[half..].to_vec()],
      ..ReceivedSignal::default()
    };
    let mut assembler = FragmentAssembler::new();
    assert!(take_answer(&mut assembler, first, 7).is_none());
    match take_answer(&mut assembler, last, 7) {
      Some(Attempt::Done(answer)) => {
        let words = answer.section(0);
        let info = dict_tab_info::parse_table_info(words).expect("table");
        assert_eq!(info.attributes.len(), 1);
      }
      _ => panic!("expected the table"),
    }
  }

  #[test]
  fn an_answer_to_an_earlier_attempt_is_passed_over() {
    let words = description();
    let signal = ReceivedSignal {
      gsn: gsn::IC_GSN_GET_TABINFO_CONF,
      data: conf(6, words.len()),
      sections: vec![words],
      ..ReceivedSignal::default()
    };
    let mut assembler = FragmentAssembler::new();
    assert!(take_answer(&mut assembler, signal, 7).is_none());
  }

  #[test]
  fn a_list_is_answered_by_its_own_signal() {
    // Two objects, no names asked for.
    let signal = ReceivedSignal {
      gsn: gsn::IC_GSN_LIST_TABLES_CONF,
      data: vec![7, 2],
      sections: vec![vec![0, 14, 3, 0, 15, 6]],
      ..ReceivedSignal::default()
    };
    let mut assembler = FragmentAssembler::new();
    match take_answer(&mut assembler, signal, 7) {
      Some(Attempt::Done(answer)) => {
        let objects = list_tables::parse_listed_objects(answer.section(0), &[])
          .expect("list");
        assert_eq!(objects.len(), 2);
        assert_eq!(objects[1].object_type, 6);
      }
      _ => panic!("expected the list"),
    }
  }

  #[test]
  fn a_missing_table_is_final_and_a_busy_dictionary_is_not() {
    let missing = GetTabInfoRef {
      sender_data: 7,
      error_code: get_tab_info::IC_GET_TABINFO_ERR_NOT_DEFINED,
      error_line: 0,
    };
    match refused(&missing) {
      Attempt::Failed(e) => assert_eq!(e.code, err::IC_ERROR_NO_SUCH_TABLE),
      _ => panic!("a missing table is final"),
    }
    let busy = GetTabInfoRef {
      sender_data: 7,
      error_code: get_tab_info::IC_GET_TABINFO_ERR_BUSY,
      error_line: 0,
    };
    assert!(matches!(refused(&busy), Attempt::Again(_)));
  }

  #[test]
  fn pauses_stay_in_the_reference_range() {
    let mut attempt: u32 = 1;
    while attempt < IC_DICT_ATTEMPTS {
      let pause = retry_pause_ms(attempt);
      assert!((50..=190).contains(&pause), "pause {}", pause);
      attempt += 1;
    }
  }
}
