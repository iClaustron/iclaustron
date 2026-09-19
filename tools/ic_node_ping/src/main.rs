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
//! while it runs and the loss should be reported within a heartbeat,
//! then the node reconnected once it is back.
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

  let mut manager = match NodeManager::new(config, mgm, connect_string, 30_000)
  {
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

  let seconds = parser.get_int_or("seconds", 0) as u64;
  if seconds == 0 {
    manager.close();
    return 0;
  }
  keep_alive(&mut manager, seconds)
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
      "           it allows {} ms of silence, we send every {} ms",
      link.heartbeat_interval_ms,
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
  for link in manager.links().values() {
    was.insert(link.node_id, link.status);
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
      let before = was.insert(link.node_id, link.status);
      if before == Some(link.status) {
        continue;
      }
      let seconds_in = elapsed / 1000;
      match link.status {
        LinkStatus::Connected => {
          recoveries += 1;
          println!("{:>5} s  node {} is up again", seconds_in, link.node_id);
        }
        LinkStatus::Failing => {
          println!(
            "{:>5} s  node {} confirmed failed by another node",
            seconds_in, link.node_id
          );
        }
        LinkStatus::Disconnected => {
          losses += 1;
          match link.last_error {
            Some(e) => println!(
              "{:>5} s  node {} lost: {} ({})",
              seconds_in,
              link.node_id,
              e.message(),
              e.code
            ),
            None => {
              println!("{:>5} s  node {} lost", seconds_in, link.node_id)
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
    manager.num_connected(),
    manager.links().len()
  );
  report_links(manager);
  manager.close();
  0
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
}
