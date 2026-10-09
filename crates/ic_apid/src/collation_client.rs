// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Asking a data node what a collation is, and for its weights (chapter
//! 06 §8.6).
//!
//! One request names the collation. Asked with weights, the answers come
//! in as many round trips as the collation needs, each from the position
//! the last answer gave, and the weight runs and contractions of every
//! answer are gathered here. All the round trips of one fetch go to the
//! same data node, since a position means something only to the node
//! that gave it; when that node's link goes, the fetch starts again from
//! the beginning at the next started node.
//!
//! A data node serves one request with weights at a time and refuses
//! another meanwhile as busy, and may lack the memory to build an answer
//! in; both are asked again after a pause, as the dictionary's other
//! requests are. A collation or character set the node does not know is
//! final.
//!
//! The answers lead with the caller's data, the request's second word,
//! so the request is matched by that number rather than by its first
//! word, which is the reply address.
//!
//! Only data nodes of 26.10.0 or later know the request, and an older
//! one takes it as a fatal error. This release talks only to 26.10
//! clusters (chapter 01) and does not check a node's version before
//! asking.
//! Verify: chapter 05 §8.2.

use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_signals::blocks;
use ic_signals::collation_desc;
use ic_signals::collation_desc::CollationDescConf;
use ic_signals::collation_desc::CollationDescRef;
use ic_signals::collation_desc::CollationDescReq;
use ic_signals::collation_desc::Contraction;
use ic_signals::collation_desc::WeightRun;
use ic_signals::header::SignalHeader;
use ic_signals::numbers;

use crate::apid_conn::ApidConnection;
use crate::dict_client::IC_DICT_ATTEMPTS;
use crate::dict_client::IC_DICT_WAIT_MS;
use crate::node_connect::ReceivedSignal;

/// What answers a collation request.
const IC_COLLATION_REPLIES: [u16; 2] = [
  numbers::IC_SIG_COLLATION_DESC_CONF,
  numbers::IC_SIG_COLLATION_DESC_REF,
];

/// The pause before asking a busy data node again.
const IC_COLLATION_BUSY_PAUSE_MS: u32 = 50;

/// A collation as a data node described it.
#[derive(Clone, Debug, Default)]
pub struct Collation {
  /// The description, from the last answer.
  pub desc: CollationDescConf,
  /// Every weight run, in code point order, when asked with weights.
  pub runs: Vec<WeightRun>,
  /// Every contraction, when asked with weights.
  pub contractions: Vec<Contraction>,
  /// Round trips the fetch took.
  pub round_trips: u32,
  /// The data node that answered.
  pub node_id: u32,
}

/// How one round trip ended.
enum Trip {
  /// A description, whole and not yet read.
  Answered(ReceivedSignal),
  /// Worth asking again at the same node and position.
  Again(IcError),
  /// The node's link went: start again elsewhere.
  Elsewhere(IcError),
  /// Not worth asking again.
  Failed(IcError),
}

/// Describe a collation by its number, the number a column's
/// description carries, with its weights if `with_weights`.
pub fn describe_by_number(
  conn: &mut ApidConnection,
  number: u32,
  with_weights: bool,
) -> Result<Collation, IcError> {
  let request = CollationDescReq::by_number(0, 0, number)?;
  fetch(conn, request, with_weights)
}

/// Describe a collation by its name, or with
/// [`collation_desc::IC_COLLATION_BY_CHARSET`] as `kind` the default
/// collation of the character set `name` names.
pub fn describe_by_name(
  conn: &mut ApidConnection,
  kind: u32,
  name: &str,
  with_weights: bool,
) -> Result<Collation, IcError> {
  let request = CollationDescReq::by_name(0, 0, kind, name)?;
  fetch(conn, request, with_weights)
}

/// Every round trip of one fetch.
fn fetch(
  conn: &mut ApidConnection,
  base: CollationDescReq,
  with_weights: bool,
) -> Result<Collation, IcError> {
  let mut collation = Collation::default();
  let mut attempt: u32 = 0;
  let mut position: u32 = 0;
  loop {
    if collation.node_id == 0 {
      let started = conn.started_nodes();
      if started.is_empty() {
        return Err(IcError::new(err::IC_ERROR_NO_STARTED_DATA_NODE));
      }
      collation.node_id = started[attempt as usize % started.len()];
    }
    let mut request = base.clone();
    if with_weights {
      request = request.with_weights(position);
    }
    match round_trip(conn, collation.node_id, &mut request) {
      Trip::Answered(whole) => {
        let conf = CollationDescConf::decode(&whole.data, whole.section(0))?;
        collation.round_trips += 1;
        if with_weights {
          conf.weights(
            whole.section(1),
            &mut collation.runs,
            &mut collation.contractions,
          )?;
        }
        let more = with_weights && conf.has_more();
        position = conf.next_position;
        collation.desc = conf;
        if !more {
          return Ok(collation);
        }
      }
      Trip::Failed(e) => return Err(e),
      Trip::Again(e) => {
        attempt += 1;
        if attempt >= IC_DICT_ATTEMPTS {
          return Err(e);
        }
        ic_port::time::microsleep(IC_COLLATION_BUSY_PAUSE_MS * 1000);
      }
      Trip::Elsewhere(e) => {
        attempt += 1;
        if attempt >= IC_DICT_ATTEMPTS {
          return Err(e);
        }
        // A position means something only to the node that gave it.
        collation.node_id = 0;
        collation.runs.clear();
        collation.contractions.clear();
        position = 0;
      }
    }
  }
}

/// One request and the wait for its answer.
fn round_trip(
  conn: &mut ApidConnection,
  node_id: u32,
  request: &mut CollationDescReq,
) -> Trip {
  request.caller_ref = conn.block_ref();
  request.caller_data = conn.next_request_id();
  let request_id = request.caller_data;
  let header = SignalHeader::new(
    numbers::IC_SIG_COLLATION_DESC_REQ,
    conn.block_number(),
    blocks::IC_BLOCK_DICTIONARY,
  );
  let name = request.name_section();
  let with_name: [&[u32]; 1] = [&name];
  let mut sections: &[&[u32]] = &[];
  if !name.is_empty() {
    sections = &with_name;
  }
  let sent = conn.send_expecting(
    node_id,
    &header,
    &request.encode(),
    sections,
    request_id,
    &IC_COLLATION_REPLIES,
  );
  if let Err(e) = sent {
    return Trip::Elsewhere(e);
  }
  let whole = match conn.wait_for(request_id, IC_DICT_WAIT_MS) {
    Ok(whole) => whole,
    Err(e) => return Trip::Elsewhere(e),
  };
  if whole.sig_no == numbers::IC_SIG_COLLATION_DESC_REF {
    return match CollationDescRef::decode(&whole.data) {
      Ok(refusal) => refused(&refusal),
      Err(e) => Trip::Failed(e),
    };
  }
  Trip::Answered(whole)
}

/// What a refusal amounts to.
fn refused(refusal: &CollationDescRef) -> Trip {
  if refusal.is_temporary() {
    return Trip::Again(IcError::new(err::IC_ERROR_DICT_REFUSED));
  }
  let code = refusal.error_code;
  if code == collation_desc::IC_COLLATION_ERR_NO_COLLATION
    || code == collation_desc::IC_COLLATION_ERR_NO_CHARSET
  {
    return Trip::Failed(IcError::new(err::IC_ERROR_NO_SUCH_COLLATION));
  }
  ic_port::debug_print!(
    IC_NDB_MESSAGE_LEVEL,
    "The dictionary refused a collation request with error {}",
    code
  );
  Trip::Failed(IcError::new(err::IC_ERROR_DICT_REFUSED))
}
