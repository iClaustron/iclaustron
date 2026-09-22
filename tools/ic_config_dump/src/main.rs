// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_config_dump`: ask a RonDB management server for the cluster
//! configuration and print it.
//!
//! This is the first thing an API node does, and the first thing to run
//! against a new cluster to see that the library and the cluster agree.
//! It answers three questions: does the management server talk to us,
//! what node id do we get, and what does it say about the cluster.
//!
//! ```text
//!   ic_config_dump localhost:1186
//!   ic_config_dump --ndb-connectstring nodeid=68,host1:1186 --raw
//! ```

use ic_apic::conf_blob::ConfigValue;
use ic_apic::conf_blob::Section;
use ic_apic::conf_param;
use ic_apic::data::ClusterConfig;
use ic_apic::mgm_client;
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
    long_name: "timeout",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Milliseconds to wait for the server",
  },
  OptionEntry {
    long_name: "raw",
    short_name: b'r',
    kind: OptionKind::Flag,
    help: "Also print every section and parameter as it arrived",
  },
  OptionEntry {
    long_name: "debug-level",
    short_name: 0,
    kind: OptionKind::Int,
    help: "Debug level bits; 32 traces the management protocol",
  },
];

fn main() {
  let code = run();
  std::process::exit(code);
}

fn run() -> i32 {
  let mut parser = OptionParser::new(
    "ic_config_dump",
    "Print the cluster configuration a management server serves.",
  );
  parser.add_entries(&OPTIONS);
  if let Err(e) = parser.parse_env_args() {
    if e.code == ic_port::err::IC_ERROR_HELP_REQUESTED {
      return 0;
    }
    return 1;
  }
  let debug_level = parser.get_int_or("debug-level", 0) as u32;
  ic_port::debug::set_level(debug_level);
  ic_port::debug::set_screen(true);
  // Seconds since the start on every line, so that a pause can be seen.
  ic_port::debug::set_timestamp(debug_level != 0);

  /* The connectstring may be given with its option or on its own. */
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
  let timeout = parser.get_int_or("timeout", 30_000) as u32;

  println!("Asking {} for the configuration", connect_text);
  let fetched = mgm_client::fetch_configuration(
    &connect_string,
    timeout,
    Some("ic_config_dump"),
  );
  let (config, client) = match fetched {
    Ok(pair) => pair,
    Err(e) => {
      report("Could not fetch the configuration", &e);
      return 1;
    }
  };
  let server = client.server();
  let version = client.version();
  println!(
    "Answered by {}:{}, RonDB {}.{}.{} ({})",
    server.host,
    server.port,
    version.major,
    version.minor,
    version.build,
    version.text
  );
  println!();
  print_config(&config);
  if parser.get_flag("raw") {
    println!();
    print_raw(&config);
  }
  0
}

fn report(what: &str, error: &IcError) {
  println!("{}: {} ({})", what, error.message(), error.code);
  // The library keeps a refusal's words and leaves the printing to us.
  if mgm_client::is_refusal(error.code) {
    println!("The management server said: {}", mgm_client::last_refusal());
  }
}

fn print_config(config: &ClusterConfig) {
  println!("Our node");
  println!("  NodeId                {}", config.api.node_id);
  match config.api.host.as_deref() {
    Some(host) => println!("  HostName              {}", host),
    None => println!("  HostName              (any)"),
  }
  println!("  BatchSize             {}", config.api.batch_size);
  println!("  BatchByteSize         {}", config.api.batch_byte_size);
  println!("  MaxScanBatchSize      {}", config.api.max_scan_batch_size);
  println!(
    "  TotalSendBufferMemory {}",
    config.api.total_send_buffer_memory
  );
  println!("  ArbitrationRank       {}", config.api.arbitration_rank);
  println!(
    "  DefaultHashMapSize    {}",
    config.api.default_hashmap_size
  );
  println!();

  println!("Cluster");
  println!("  NoOfReplicas          {}", config.no_of_replicas);
  println!(
    "  Heartbeat period      {} ms (the shortest any data node asks)",
    config.heartbeat_interval_ms()
  );
  println!();

  println!("Data nodes ({})", config.data_nodes.len());
  for node in &config.data_nodes {
    println!(
      "  node {:<3} group {:<3} heartbeat {:>6} ms  {}",
      node.node_id, node.node_group, node.api_heartbeat_interval_ms, node.host
    );
  }
  println!();

  println!("Management servers ({})", config.mgm_nodes.len());
  for node in &config.mgm_nodes {
    println!("  node {:<3} {}", node.node_id, node.host);
  }
  println!();

  println!("Our links ({})", config.links.len());
  for link in &config.links {
    let other = link.other_node(config.api.node_id);
    let port = if link.port_is_dynamic() {
      "dynamic".to_string()
    } else {
      link.server_port.to_string()
    };
    let direction = if link.we_dial(config.api.node_id) {
      "we dial"
    } else {
      "they dial"
    };
    println!(
      "  node {:<3} {:<9} {}:{}  send {} recv {}{}{}",
      other,
      direction,
      link.server_host,
      port,
      link.send_buffer_size,
      link.receive_buffer_size,
      if link.checksum { " checksum" } else { "" },
      if link.require_tls { " tls" } else { "" }
    );
  }
  let missing = config.data_nodes.len() - config.connectable_data_nodes().len();
  if missing > 0 {
    println!();
    println!(
      "  note: {} data node(s) have no link to us and cannot be reached",
      missing
    );
  }
}

fn print_raw(config: &ClusterConfig) {
  println!("Configuration as it arrived");
  let blob = &config.blob;
  println!(
    "  {} data nodes, {} API nodes, {} management nodes, {} links",
    blob.num_data_nodes,
    blob.num_api_nodes,
    blob.num_mgm_nodes,
    blob.links.len()
  );
  println!();
  for (section_type, section) in &blob.defaults {
    print_section(&format!("default {:?}", section_type), section);
  }
  print_section("system", &blob.system);
  for section in &blob.nodes {
    let node_id = section.node_id().unwrap_or(0);
    let name = format!("{:?} node {}", section.section_type, node_id);
    print_section(&name, section);
  }
  for section in &blob.links {
    let one = blob
      .value_u32(section, conf_param::IC_CFG_CONNECTION_NODE_1)
      .unwrap_or(0);
    let two = blob
      .value_u32(section, conf_param::IC_CFG_CONNECTION_NODE_2)
      .unwrap_or(0);
    let name = format!("{:?} link {} to {}", section.section_type, one, two);
    print_section(&name, section);
  }
}

fn print_section(name: &str, section: &Section) {
  println!("  [{}] {} parameters", name, section.entries.len());
  for (key, value) in &section.entries {
    let label = match conf_param::parameter_name(section.section_type, *key) {
      Some(text) => text.to_string(),
      None => format!("#{}", key),
    };
    match value {
      ConfigValue::Int(v) => println!("    {:<34} {}", label, v),
      ConfigValue::Int64(v) => println!("    {:<34} {}", label, v),
      ConfigValue::Str(v) => println!("    {:<34} {}", label, v),
      ConfigValue::Section(v) => {
        println!("    {:<34} section {}", label, v)
      }
    }
  }
}
