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
//! ```text
//!   ic_node_ping localhost:1186
//!   ic_node_ping localhost:1186 --seconds 600 --debug-level 1024
//! ```
//!
//! Debug level 1024 traces every signal in and out, 16384 the heartbeat
//! handling, 32 the management protocol.

use std::collections::BTreeMap;

use ic_apic::mgm_client;
use ic_apid::node_manager::LinkStatus;
use ic_apid::node_manager::NodeManager;
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

  let mut manager = match NodeManager::new(
    config,
    mgm,
    connect_string,
    30_000,
    Some("ic_node_ping"),
  ) {
    Ok(manager) => manager,
    Err(e) => {
      report("Could not set up the node manager", &e);
      return 1;
    }
  };

  // The first round connects every data node.
  if let Err(e) = manager.poll(0) {
    report("Could not connect to the data nodes", &e);
    return 1;
  }
  report_links(&manager);
  if manager.num_connected() == 0 {
    println!();
    println!("No data node accepted us");
    return 1;
  }
  println!();
  println!("Connected to {} data node(s)", manager.num_connected());
  println!();
  seize_on_every_node(&mut manager);

  let seconds = parser.get_int_or("seconds", 0) as u64;
  if seconds == 0 {
    manager.close();
    return 0;
  }
  keep_alive(&mut manager, seconds)
}

/// Take a transaction record on every started node and give it back.
///
/// This is the first exchange made as a user thread would make it: sent
/// under a user thread's own block number, with the answer addressed to
/// that block. The manager never looks at the answer. It sorts it into
/// the inbox by block number, and we find it there, which is the whole
/// of the threading model seen from outside.
fn seize_on_every_node(manager: &mut NodeManager) {
  let table = manager.thread_table();
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
  for node_id in manager.started_nodes() {
    seize_and_release(manager, &inbox, node_id);
  }
  table.release(&inbox);
  if table.unroutable() != 0 {
    println!("{} signal(s) had no inbox to go to", table.unroutable());
  }
}

fn seize_and_release(
  manager: &mut NodeManager,
  inbox: &ThreadConnection,
  node_id: u32,
) {
  let own_ref =
    blocks::number_to_ref(inbox.block_number(), manager.own_node_id());
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
  if let Err(e) = manager.send(node_id, &header, &seize.encode()) {
    println!("node {:<4} seize not sent: {}", node_id, e.message());
    return;
  }
  let (answer, data) = wait_for_answer(manager, inbox);
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
  if let Err(e) = manager.send(node_id, &header, &release.encode()) {
    println!("node {:<4} release not sent: {}", node_id, e.message());
    return;
  }
  let (answer, _data) = wait_for_answer(manager, inbox);
  if answer == gsn::IC_GSN_TCRELEASECONF {
    println!("node {:<4} took it back", node_id);
  } else if answer == gsn::IC_GSN_TCRELEASEREF {
    println!("node {:<4} would not take it back", node_id);
  } else {
    println!("node {:<4} did not answer the release in time", node_id);
  }
}

/// Drive the manager until something is in the inbox, or five seconds
/// pass. Gives the signal number and data of the first signal found,
/// or zero and nothing.
///
/// In this release the thread that polls is also the thread that reads
/// the inbox. With receive threads, a user thread only does the second
/// half, waiting on its inbox while a receive thread fills it.
fn wait_for_answer(
  manager: &mut NodeManager,
  inbox: &ThreadConnection,
) -> (u16, Vec<u32>) {
  let start = ic_port::time::gethrtime();
  loop {
    if manager.poll(50).is_err() {
      return (0, Vec::new());
    }
    let mut got = inbox.take(0);
    if !got.is_empty() {
      let first = got.remove(0);
      return (first.gsn, first.data);
    }
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    if waited > 5000 {
      return (0, Vec::new());
    }
  }
}

/// Print what every link looks like now.
fn report_links(manager: &NodeManager) {
  for link in manager.links().values() {
    print!("node {:<4} ", link.node_id);
    if !link.is_connected() {
      match link.last_error {
        Some(e) => println!("not connected: {} ({})", e.message(), e.code),
        None => println!("not connected"),
      }
      continue;
    }
    let state = match link.node_state {
      Some(state) => state,
      None => {
        println!("connected");
        continue;
      }
    };
    println!(
      "connected, {:?}, node group {}",
      state.start_level, state.node_group
    );
    println!(
      "           heartbeat check every {} ms, we send every {} ms",
      link.check_interval_ms(),
      link.heartbeat_period_ms()
    );
    println!(
      "           it can see node(s) {:?}",
      state.connected_node_ids()
    );
  }
}

/// Keep every link up for as long as asked, reporting each change.
///
/// Nothing here connects or reconnects: the manager does that, and this
/// loop only says what it sees. That is the point of the command, since
/// an application's loop looks the same.
fn keep_alive(manager: &mut NodeManager, seconds: u64) -> i32 {
  println!();
  println!("Keeping the connections up for {} s", seconds);
  println!("Restart a data node to see the loss reported and made good");
  println!();
  let start = ic_port::time::gethrtime();
  let mut was: BTreeMap<u32, LinkStatus> = BTreeMap::new();
  let mut was_started: BTreeMap<u32, bool> = BTreeMap::new();
  for link in manager.links().values() {
    was.insert(link.node_id, link.status);
    was_started.insert(link.node_id, link.is_started());
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
    // A short wait keeps the loop responsive to a socket closing while
    // leaving the heartbeats to the manager.
    if let Err(e) = manager.poll(200) {
      report("The node manager stopped", &e);
      return 1;
    }
    for link in manager.links().values() {
      let seconds_in = elapsed / 1000;
      // Connected is not the same as able to serve: a restarting node
      // accepts us well before it has started.
      let started = link.is_started();
      let started_before = was_started.insert(link.node_id, started);
      let before = was.insert(link.node_id, link.status);
      if before == Some(link.status) {
        if started && started_before == Some(false) {
          println!("{:>5} s  node {} has started", seconds_in, link.node_id);
        }
        continue;
      }
      match link.status {
        LinkStatus::Connected => {
          recoveries += 1;
          if started {
            println!(
              "{:>5} s  node {} is connected again",
              seconds_in, link.node_id
            );
          } else {
            println!(
              "{:>5} s  node {} is connected again, still starting",
              seconds_in, link.node_id
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
            seconds_in, link.node_id
          );
          println!(
            "         not dialling it until the failure is reported handled"
          );
        }
        LinkStatus::Disconnected => {
          if before == Some(LinkStatus::AwaitingTakeover) {
            println!(
              "{:>5} s  node {} may be dialled again",
              seconds_in, link.node_id
            );
          } else {
            losses += 1;
            report_loss(seconds_in, link.node_id, link.last_error);
            println!("         dialling; no node has reported that it failed");
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
    manager.num_connected(),
    manager.links().len()
  );
  report_links(manager);
  manager.close();
  0
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
