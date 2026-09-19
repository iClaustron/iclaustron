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
//! ```text
//!   ic_node_ping localhost:1186
//!   ic_node_ping localhost:1186 --seconds 600 --debug-level 1024
//! ```
//!
//! Debug level 1024 traces every signal in and out, 16384 the heartbeat
//! handling, 32 the management protocol.

use ic_apic::data::ClusterConfig;
use ic_apic::mgm_client;
use ic_apic::mgm_client::MgmClient;
use ic_apid::node_connect::NodeConnection;
use ic_port::options::OptionEntry;
use ic_port::options::OptionKind;
use ic_port::options::OptionParser;
use ic_port::IcError;

const OPTIONS: [OptionEntry; 5] = [
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
    help: "How long to keep the connections alive; 0 just connects",
  },
  OptionEntry {
    long_name: "interval",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Milliseconds between heartbeats; 0 uses the configured value",
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
  let (config, mut mgm) = match fetched {
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

  let connections = connect_all(&config, &mut mgm);
  if connections.is_empty() {
    println!();
    println!("No data node accepted us");
    return 1;
  }
  println!();
  println!("Connected to {} data node(s)", connections.len());

  let seconds = parser.get_int_or("seconds", 0) as u64;
  if seconds == 0 {
    return 0;
  }
  keep_alive(
    connections,
    seconds,
    parser.get_int_or("interval", 0) as u32,
  )
}

fn connect_all(
  config: &ClusterConfig,
  mgm: &mut MgmClient,
) -> Vec<NodeConnection> {
  let mut connections: Vec<NodeConnection> = Vec::new();
  for node in &config.data_nodes {
    print!("node {:<4} ", node.node_id);
    match NodeConnection::connect(config, mgm, node.node_id) {
      Ok(connection) => {
        let state = match connection.node_state {
          Some(state) => state,
          None => continue,
        };
        println!(
          "connected, {:?}, node group {}, heartbeat every {} ms",
          state.start_level, state.node_group, connection.heartbeat_interval_ms
        );
        let seen = state.connected_node_ids();
        println!("           it can see node(s) {:?}", seen);
        connections.push(connection);
      }
      Err(e) => {
        println!("failed: {} ({})", e.message(), e.code);
      }
    }
  }
  connections
}

fn keep_alive(
  mut connections: Vec<NodeConnection>,
  seconds: u64,
  interval_override: u32,
) -> i32 {
  // A data node declares us dead if it hears nothing for its heartbeat
  // interval, so send well inside it. The C++ client uses a fifth.
  let mut interval_ms = interval_override;
  if interval_ms == 0 {
    interval_ms = connections[0].heartbeat_interval_ms / 5;
    if interval_ms == 0 {
      interval_ms = 1000;
    }
  }
  println!();
  println!(
    "Keeping the connections alive for {} s, a heartbeat every {} ms",
    seconds, interval_ms
  );
  let start = ic_port::time::gethrtime();
  let mut rounds: u64 = 0;
  let mut failures: u64 = 0;
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
    ic_port::time::microsleep(interval_ms * 1000);
    rounds += 1;
    let mut i: usize = 0;
    while i < connections.len() {
      let node_id = connections[i].node_id;
      match connections[i].heartbeat_round(5000) {
        Ok(()) => {}
        Err(e) => {
          failures += 1;
          println!(
            "node {} heartbeat failed after {} s: {} ({})",
            node_id,
            elapsed / 1000,
            e.message(),
            e.code
          );
          connections.remove(i);
          continue;
        }
      }
      i += 1;
    }
    if connections.is_empty() {
      println!("Every connection is gone");
      return 1;
    }
  }
  println!(
    "Done: {} heartbeat round(s), {} failure(s), {} connection(s) left",
    rounds,
    failures,
    connections.len()
  );
  for connection in &connections {
    println!("  {:?}", connection);
  }
  0
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
}
