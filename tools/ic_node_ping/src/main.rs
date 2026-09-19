// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_node_ping`: connect to every data node and exchange heartbeats.
//!
//! This is the whole transport path in one command: fetch the
//! configuration, look up each data node's port, connect, complete the
//! transporter handshake, register, and then keep the connections alive
//! for as long as asked. It is what proves the library can talk to a
//! cluster at all.
//!
//! It also shows what happens when a node stops. Restart a data node
//! while it runs. The link should be reported lost at once, then the
//! node reported failed by the cluster and left alone until its failure
//! is reported handled, then reconnected once it is back. Breaking only
//! the connection, with the node left running, should show the link
//! lost and dialled again with no failure report at all. Debug level
//! 16384 shows each step.
//!
//! The command drives nothing itself. It starts the Data API's threads,
//! which connect, read, route and keep heartbeats, and then only
//! watches, and acts as one user thread for the seize: it sends under
//! its own block number and waits on its own inbox. An application
//! looks the same.
//!
//! ```text
//!   ic_node_ping localhost:1186
//!   ic_node_ping localhost:1186 --seconds 600 --debug-level 1024
//! ```
//!
//! Debug level 1024 traces every signal in and out, 16384 the heartbeat
//! handling, 32 the management protocol.

use std::collections::BTreeMap;

use ic_apic::mgm_client;
use ic_apid::apid_global::ApidGlobal;
use ic_apid::apid_global::LinkStatus;
use ic_apid::apid_global::NodeShared;
use ic_apid::thread_conn::ThreadConnection;
use ic_ndb_signals::blocks;
use ic_ndb_signals::gsn;
use ic_ndb_signals::header::SignalHeader;
use ic_ndb_signals::tc_seize::TcReleaseReq;
use ic_ndb_signals::tc_seize::TcSeizeConf;
use ic_ndb_signals::tc_seize::TcSeizeRef;
use ic_ndb_signals::tc_seize::TcSeizeReq;
use ic_ndb_signals::tc_seize::IC_ANY_TC_INSTANCE;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;

const OPTIONS: [OptionEntry; 4] = [
  OptionEntry {
    long_name: "ndb-connectstring",
    short_name: b'c',
    kind: OptionKind::Str,
    help: "Management servers, as host:port,host:port",
  },
  OptionEntry {
    long_name: "node-id",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Node id to ask for; 0 lets the server choose",
  },
  OptionEntry {
    long_name: "seconds",
    short_name: b's',
    kind: OptionKind::Int,
    help: "How long to keep the connections up; 0 just connects",
  },
  OptionEntry {
    long_name: "debug-level",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Debug level bits; 1024 traces every signal",
  },
];

fn main() {
  std::process::exit(run());
}

fn run() -> i32 {
  let mut parser = OptionParser::new(
    "ic_node_ping",
    "Connect to every data node and exchange heartbeats.",
  );
  parser.add_entries(&OPTIONS);
  if let Err(e) = parser.parse_env_args() {
    if e.code == ic_port::err::IC_ERROR_HELP_REQUESTED {
      return 0;
    }
    return 1;
  }
  ic_port::debug::set_level(parser.get_int_or("debug-level", 0) as u32);
  ic_port::debug::set_screen(true);

  let mut connect_text = parser.get_string_or("ndb-connectstring", "");
  if connect_text.is_empty() && !parser.positional().is_empty() {
    connect_text = parser.positional()[0].clone();
  }
  if connect_text.is_empty() {
    connect_text = "localhost:1186".to_string();
  }
  let mut connect_string = match ic_util::connectstring::parse(&connect_text) {
    Ok(cs) => cs,
    Err(e) => {
      report("Could not read the connectstring", &e);
      return 1;
    }
  };
  let wanted = parser.get_int_or("node-id", 0) as u32;
  if wanted != 0 {
    connect_string.node_id = Some(wanted);
  }

  println!("Fetching the configuration from {}", connect_text);
  let fetched = mgm_client::fetch_configuration(
    &connect_string,
    30_000,
    Some("ic_node_ping"),
  );
  let (config, mgm) = match fetched {
    Ok(pair) => pair,
    Err(e) => {
      report("Could not fetch the configuration", &e);
      return 1;
    }
  };
  println!(
    "We are node {}, the cluster has {} data node(s)",
    config.api.node_id,
    config.data_nodes.len()
  );
  println!();

  let mut global = match ApidGlobal::start(
    config,
    mgm,
    connect_string,
    30_000,
    Some("ic_node_ping"),
  ) {
    Ok(global) => global,
    Err(e) => {
      report("Could not start the Data API threads", &e);
      return 1;
    }
  };

  // The threads connect in the background; give them a moment.
  global.wait_for_started(15_000);
  report_nodes(&global);
  if global.num_connected() == 0 {
    println!();
    println!("No data node accepted us");
    global.stop();
    return 1;
  }
  println!();
  println!("Connected to {} data node(s)", global.num_connected());
  println!();
  seize_on_every_node(&global);

  let seconds = parser.get_int_or("seconds", 0) as u64;
  let code = if seconds == 0 {
    0
  } else {
    keep_alive(&global, seconds)
  };
  global.stop();
  code
}

/// Take a transaction record on every started node and give it back.
///
/// This is the first exchange made as a user thread makes it: sent
/// under our own block number, with the answer addressed to that block.
/// The receive thread never looks at the answer. It sorts it into our
/// inbox by block number, and we wait there for it, which is the whole
/// of the threading model seen from outside.
fn seize_on_every_node(global: &ApidGlobal) {
  let table = global.thread_table();
  let inbox = match table.allocate() {
    Ok(inbox) => inbox,
    Err(e) => {
      report("Could not get an inbox", &e);
      return;
    }
  };
  println!(
    "As user thread {}, block {:#06x}:",
    inbox.thread_id(),
    inbox.block_number()
  );
  for node_id in global.started_nodes() {
    seize_and_release(global, &inbox, node_id);
  }
  table.release(&inbox);
  if table.unroutable() != 0 {
    println!("{} signal(s) had no inbox to go to", table.unroutable());
  }
}

fn seize_and_release(
  global: &ApidGlobal,
  inbox: &ThreadConnection,
  node_id: u32,
) {
  let own_ref =
    blocks::number_to_ref(inbox.block_number(), global.own_node_id());
  // Our own name for the record. It comes back in every answer.
  let our_ptr: u32 = 1000 + node_id;
  let seize = TcSeizeReq {
    api_connect_ptr: our_ptr,
    api_block_ref: own_ref,
    instance: IC_ANY_TC_INSTANCE,
  };
  let header = SignalHeader::new(
    gsn::IC_GSN_TCSEIZEREQ,
    inbox.block_number(),
    blocks::IC_BLOCK_DBTC,
  );
  if let Err(e) = global.send(node_id, &header, &seize.encode()) {
    println!("node {:<4} seize not sent: {}", node_id, e.message());
    return;
  }
  let (answer, data) = wait_for_answer(inbox);
  if answer == gsn::IC_GSN_TCSEIZEREF {
    match TcSeizeRef::decode(&data) {
      Ok(refusal) => println!(
        "node {:<4} would not give us a transaction record: NDB error {}",
        node_id, refusal.error_code
      ),
      Err(e) => println!("node {:<4} bad refusal: {}", node_id, e.message()),
    }
    return;
  }
  if answer != gsn::IC_GSN_TCSEIZECONF {
    println!("node {:<4} did not answer the seize in time", node_id);
    return;
  }
  let conf = match TcSeizeConf::decode(&data) {
    Ok(conf) => conf,
    Err(e) => {
      println!("node {:<4} bad answer: {}", node_id, e.message());
      return;
    }
  };
  // The coordinator's block number carries its instance in a form we
  // have no need to take apart: it is only ever sent back.
  let tc_block = blocks::ref_to_block(conf.tc_block_ref);
  println!(
    "node {:<4} gave us transaction record {} at coordinator {:#06x}{}",
    node_id,
    conf.tc_connect_ptr,
    tc_block,
    if conf.api_connect_ptr == our_ptr {
      ""
    } else {
      " (but for a pointer that is not ours)"
    }
  );
  let release = TcReleaseReq {
    tc_connect_ptr: conf.tc_connect_ptr,
    api_block_ref: own_ref,
    api_connect_ptr: our_ptr,
  };
  let header =
    SignalHeader::new(gsn::IC_GSN_TCRELEASEREQ, inbox.block_number(), tc_block);
  if let Err(e) = global.send(node_id, &header, &release.encode()) {
    println!("node {:<4} release not sent: {}", node_id, e.message());
    return;
  }
  let (answer, _data) = wait_for_answer(inbox);
  if answer == gsn::IC_GSN_TCRELEASECONF {
    println!("node {:<4} took it back", node_id);
  } else if answer == gsn::IC_GSN_TCRELEASEREF {
    println!("node {:<4} would not take it back", node_id);
  } else {
    println!("node {:<4} did not answer the release in time", node_id);
  }
}

/// Wait on our inbox for up to five seconds. Gives the signal number
/// and data of the first signal found, or zero and nothing.
///
/// This is all a user thread does to receive: it sleeps on its inbox
/// and a receive thread wakes it when something is put there.
fn wait_for_answer(inbox: &ThreadConnection) -> (u16, Vec<u32>) {
  let start = ic_port::time::gethrtime();
  loop {
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if waited >= 5000 {
      return (0, Vec::new());
    }
    let mut got = inbox.take(5000 - waited as u32);
    if !got.is_empty() {
      let first = got.remove(0);
      return (first.gsn, first.data);
    }
    // Woken for nothing, or the wait ran out; go round.
  }
}

/// Print what every node looks like now.
fn report_nodes(global: &ApidGlobal) {
  for node in global.nodes() {
    print!("node {:<4} ", node.node_id);
    if !node.published.is_connected() {
      match node.last_error() {
        Some(e) => println!("not connected: {} ({})", e.message(), e.code),
        None => println!("not connected"),
      }
      continue;
    }
    println!(
      "connected, {:?}, node group {}",
      node.published.start_level(),
      node.published.node_group()
    );
    println!(
      "           heartbeat check every {} ms, we send every {} ms",
      node.check_interval_ms(),
      node.heartbeat_period_ms()
    );
    if let Some(state) = node.node_state() {
      println!(
        "           it can see node(s) {:?}",
        state.connected_node_ids()
      );
    }
  }
}

/// Watch every node for as long as asked, reporting each change.
///
/// Nothing here connects or reconnects: the Data API's threads do that,
/// and this loop only says what it sees.
fn keep_alive(global: &ApidGlobal, seconds: u64) -> i32 {
  println!();
  println!("Keeping the connections up for {} s", seconds);
  println!("Restart a data node to see the loss reported and made good");
  println!();
  let start = ic_port::time::gethrtime();
  let mut was: BTreeMap<u32, LinkStatus> = BTreeMap::new();
  let mut was_started: BTreeMap<u32, bool> = BTreeMap::new();
  let mut was_node_id = global.own_node_id();
  for node in global.nodes() {
    was.insert(node.node_id, view_of(node));
    was_started.insert(node.node_id, node.published.is_started());
  }
  let mut losses: u64 = 0;
  let mut recoveries: u64 = 0;
  loop {
    let elapsed =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if elapsed >= seconds * 1000 {
      break;
    }
    if ic_port::stop::get_stop_flag() != 0 {
      println!("Stopping");
      break;
    }
    ic_port::time::microsleep(200_000);
    let seconds_in = elapsed / 1000;
    if global.own_node_id() != was_node_id {
      println!(
        "{:>5} s  we are node {} now, having been node {}",
        seconds_in,
        global.own_node_id(),
        was_node_id
      );
      was_node_id = global.own_node_id();
    }
    for node in global.nodes() {
      // Connected is not the same as able to serve: a restarting node
      // accepts us well before it has started.
      let status = view_of(node);
      let started = node.published.is_started();
      let started_before = was_started.insert(node.node_id, started);
      let before = was.insert(node.node_id, status);
      if before == Some(status) {
        if started && started_before == Some(false) {
          println!("{:>5} s  node {} has started", seconds_in, node.node_id);
        }
        continue;
      }
      match status {
        LinkStatus::Connected => {
          recoveries += 1;
          if started {
            println!(
              "{:>5} s  node {} is connected again",
              seconds_in, node.node_id
            );
          } else {
            println!(
              "{:>5} s  node {} is connected again, still starting",
              seconds_in, node.node_id
            );
          }
        }
        LinkStatus::AwaitingTakeover => {
          if before == Some(LinkStatus::Connected) {
            // The report beat our own socket to it.
            losses += 1;
          }
          println!(
            "{:>5} s  node {} reported failed by the cluster",
            seconds_in, node.node_id
          );
          println!(
            "         not dialling it until the failure is reported handled"
          );
        }
        LinkStatus::Disconnected => {
          if before == Some(LinkStatus::AwaitingTakeover) {
            println!(
              "{:>5} s  node {} may be dialled again",
              seconds_in, node.node_id
            );
          } else {
            losses += 1;
            report_loss(seconds_in, node.node_id, node.last_error());
            // We look five times a second, and a node can be lost,
            // reported failed and reported handled in less. The recorded
            // error says which kind of loss it was.
            if was_reported_failed(node) {
              println!(
                "         reported failed by the cluster and already \
                 handled; dialling"
              );
            } else {
              println!(
                "         dialling; no node has reported that it failed"
              );
            }
          }
        }
      }
    }
  }
  println!();
  println!(
    "Done: {} loss(es), {} recovery(ies), {} of {} node(s) connected",
    losses,
    recoveries,
    global.num_connected(),
    global.nodes().len()
  );
  report_nodes(global);
  0
}

/// How a node looks to someone watching: connected only once its link
/// is installed and published, which is later than a connect thread
/// claiming it, and otherwise whether the cluster has it down as failed.
fn view_of(node: &NodeShared) -> LinkStatus {
  if node.published.is_connected() {
    return LinkStatus::Connected;
  }
  if node.membership() == LinkStatus::AwaitingTakeover {
    return LinkStatus::AwaitingTakeover;
  }
  LinkStatus::Disconnected
}

/// True when the last thing recorded about the node is that the cluster
/// reported it failed, as opposed to our link simply breaking.
fn was_reported_failed(node: &NodeShared) -> bool {
  let last = match node.last_error() {
    Some(e) => e,
    None => return false,
  };
  last.code == ic_port::err::IC_ERROR_NODE_DOWN
}

fn report_loss(seconds_in: u64, node_id: u32, error: Option<IcError>) {
  match error {
    Some(e) => println!(
      "{:>5} s  node {} lost: {} ({})",
      seconds_in,
      node_id,
      e.message(),
      e.code
    ),
    None => println!("{:>5} s  node {} lost", seconds_in, node_id),
  }
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  print_refusal(error);
}

/// When a management server said no, say why in its own words. The
/// library keeps them and leaves the printing to us.
fn print_refusal(error: &IcError) {
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}
