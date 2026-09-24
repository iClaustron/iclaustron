// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `ic_model`: a discrete-event model of the client and the data nodes,
//! to see how a behaviour would change latency and throughput before it
//! is built (doc/rust/12-performance-model.md).
//!
//! ```text
//!   ic_model                                  the calibrated defaults
//!   ic_model threads=8 depth=2
//!   ic_model sweep=depth:1,2,4,8
//!   ic_model threads=8 sweep=node_recv_threads:1,2,4
//!   ic_model show                             every parameter and value
//! ```
//!
//! The model follows one batch at a time through the path it takes: a
//! user thread defines it and writes one request per data node; each
//! data node's receive thread reads it, execution threads work through
//! it in chunks, and a send thread writes each chunk's replies back;
//! the client's receive thread gathers what has arrived in rounds and
//! posts it to the user thread, waking it if it sleeps; the user thread
//! completes the operations and defines the next batch. A data node
//! thread that has been idle longer than it spins has gone to sleep,
//! and work that finds it so waits for it to wake: the latency that a
//! lightly loaded cluster pays and a busy one does not. Every thread is
//! a server with a first-come queue, every cost is a parameter in
//! nanoseconds, and a round, a wake-up, a read and a write each cost
//! what they cost once, however much they carry: batching is the model's
//! subject.
//!
//! Threads that share too few cores slow each other down. With `cores`
//! set, the model runs again with every cost stretched by the factor by
//! which the CPU it asks for exceeds the cores, until that settles: a
//! mean-field view, which catches the direction of spinning and
//! oversubscription and understates their spikes. `other_cores` is the
//! CPU that threads the model does not follow take from the same cores:
//! data node threads spinning or doing other work, the operating system.
//!
//! The defaults were fitted on 2026-09-24 to a Mac M5 Pro (6 fast and
//! 12 slower cores, taken as 10) with two release data nodes on the
//! same machine, `t9` reads by primary key, in million reads a second:
//! one user thread at depths 1, 2 and 4 measured 1.17 to 1.35, 1.79 and
//! 2.58, the prototype of this model 1.06, 1.94 and 2.40; 4, 8 and 16
//! threads with batches of 300 measured 5.80, 7.36 to 8.88 and 7.93,
//! the prototype 6.35, 7.19 and 7.37. Calibrate again for another
//! machine; chapter 12 says how.

use std::cmp::Reverse;
use std::collections::BinaryHeap;

/// Every parameter, all times in nanoseconds.
#[derive(Clone, Debug)]
struct Params {
  threads: usize,
  depth: usize,
  batch: usize,
  nodes: usize,
  receive_threads: usize,
  define_ns: f64,
  complete_ns: f64,
  batch_ns: f64,
  write_ns: f64,
  write_byte_ns: f64,
  recv_ns: f64,
  recv_byte_ns: f64,
  round_ns: f64,
  route_ns: f64,
  post_ns: f64,
  kevent_ns: f64,
  kevent_latency_ns: f64,
  wake_latency_ns: f64,
  wake_cpu_ns: f64,
  spin_ns: f64,
  request_bytes: f64,
  reply_bytes: f64,
  reply_signals: f64,
  net_ns: f64,
  node_recv_threads: usize,
  node_packet_ns: f64,
  node_signal_ns: f64,
  node_exec_threads: usize,
  node_exec_ns: f64,
  chunk_ops: usize,
  node_send_ns: f64,
  node_send_op_ns: f64,
  node_wake_ns: f64,
  node_spin_ns: f64,
  cores: f64,
  other_cores: f64,
  seconds: f64,
  warmup: f64,
  jitter: f64,
  seed: u64,
}

impl Params {
  fn defaults() -> Params {
    Params {
      threads: 1,
      depth: 2,
      batch: 200,
      nodes: 2,
      receive_threads: 1,
      define_ns: 130.0,
      complete_ns: 180.0,
      batch_ns: 2000.0,
      write_ns: 4000.0,
      write_byte_ns: 0.15,
      recv_ns: 2500.0,
      recv_byte_ns: 0.25,
      round_ns: 500.0,
      route_ns: 20.0,
      post_ns: 300.0,
      kevent_ns: 1500.0,
      kevent_latency_ns: 4000.0,
      wake_latency_ns: 8000.0,
      wake_cpu_ns: 3000.0,
      spin_ns: 0.0,
      request_bytes: 64.0,
      reply_bytes: 80.0,
      reply_signals: 2.0,
      net_ns: 3000.0,
      node_recv_threads: 1,
      node_packet_ns: 2000.0,
      node_signal_ns: 150.0,
      node_exec_threads: 4,
      node_exec_ns: 600.0,
      chunk_ops: 40,
      node_send_ns: 3000.0,
      node_send_op_ns: 60.0,
      node_wake_ns: 20000.0,
      node_spin_ns: 0.0,
      cores: 10.0,
      other_cores: 8.0,
      seconds: 0.2,
      warmup: 0.02,
      jitter: 0.2,
      seed: 1,
    }
  }

  /// Set a parameter by name; false if there is none such.
  fn set(&mut self, name: &str, value: f64) -> bool {
    let count = value.max(0.0) as usize;
    match name {
      "threads" => self.threads = count.max(1),
      "depth" => self.depth = count.max(1),
      "batch" => self.batch = count.max(1),
      "nodes" => self.nodes = count.max(1),
      "receive_threads" => self.receive_threads = count.max(1),
      "define_ns" => self.define_ns = value,
      "complete_ns" => self.complete_ns = value,
      "batch_ns" => self.batch_ns = value,
      "write_ns" => self.write_ns = value,
      "write_byte_ns" => self.write_byte_ns = value,
      "recv_ns" => self.recv_ns = value,
      "recv_byte_ns" => self.recv_byte_ns = value,
      "round_ns" => self.round_ns = value,
      "route_ns" => self.route_ns = value,
      "post_ns" => self.post_ns = value,
      "kevent_ns" => self.kevent_ns = value,
      "kevent_latency_ns" => self.kevent_latency_ns = value,
      "wake_latency_ns" => self.wake_latency_ns = value,
      "wake_cpu_ns" => self.wake_cpu_ns = value,
      "spin_ns" => self.spin_ns = value,
      "request_bytes" => self.request_bytes = value,
      "reply_bytes" => self.reply_bytes = value,
      "reply_signals" => self.reply_signals = value,
      "net_ns" => self.net_ns = value,
      "node_recv_threads" => self.node_recv_threads = count.max(1),
      "node_packet_ns" => self.node_packet_ns = value,
      "node_signal_ns" => self.node_signal_ns = value,
      "node_exec_threads" => self.node_exec_threads = count.max(1),
      "node_exec_ns" => self.node_exec_ns = value,
      "chunk_ops" => self.chunk_ops = count.max(1),
      "node_send_ns" => self.node_send_ns = value,
      "node_send_op_ns" => self.node_send_op_ns = value,
      "node_wake_ns" => self.node_wake_ns = value,
      "node_spin_ns" => self.node_spin_ns = value,
      "cores" => self.cores = value,
      "other_cores" => self.other_cores = value,
      "seconds" => self.seconds = value,
      "warmup" => self.warmup = value,
      "jitter" => self.jitter = value,
      "seed" => self.seed = count as u64,
      _ => return false,
    }
    true
  }
}

/// A small generator of numbers in [0, 1), for jitter: xorshift64*.
struct Rng(u64);

impl Rng {
  fn unit(&mut self) -> f64 {
    let mut x = self.0;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    self.0 = x;
    let v = x.wrapping_mul(0x2545_F491_4F6C_DD1D);
    (v >> 11) as f64 / (1u64 << 53) as f64
  }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Event {
  /// A user thread polls: completes what came, defines what it can.
  Poll(usize),
  /// A user thread's poll is over.
  UserIdle(usize),
  /// A user thread's spin runs out, if it is still the same spin.
  SpinEnd(usize, u64),
  /// A request arrives at a data node: node, thread, slot, operations.
  Request(usize, usize, usize, usize),
  /// A chunk of a request is executed: node, thread, slot, operations.
  Exec(usize, usize, usize, usize),
  /// Replies arrive at the client: receive thread, user thread, slot,
  /// operations, node.
  Reply(usize, usize, usize, usize, usize),
  /// A client receive thread starts a round.
  Round(usize),
  /// A client receive thread's round is over, its posts delivered.
  RoundEnd(usize),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum UserState {
  Busy,
  Spinning,
  Sleeping,
  Waking,
}

struct Slot {
  busy: bool,
  left: usize,
  sent: f64,
}

struct User {
  state: UserState,
  inbox: Vec<(usize, usize)>,
  slots: Vec<Slot>,
  spin_id: u64,
  spin_start: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RxState {
  Idle,
  Waking,
  Busy,
}

struct Rx {
  state: RxState,
  blocked: bool,
  queue: Vec<(usize, usize, usize, usize)>,
  deliver: Vec<(usize, usize, usize, usize)>,
}

struct Node {
  recv_free: Vec<f64>,
  exec_free: Vec<f64>,
  send_free: f64,
}

/// What a run measured, over the measured window.
#[derive(Default)]
struct Stats {
  ops: u64,
  latencies: Vec<f64>,
  user_ns: f64,
  sys_ns: f64,
  rx_user_ns: f64,
  rx_sys_ns: f64,
  spin_ns: f64,
  wakes: u64,
  reads: u64,
  writes: u64,
  node_recv_ns: f64,
  node_exec_ns: f64,
  node_send_ns: f64,
  spin_hits: u64,
  spin_misses: u64,
  /// CPU asked for over the whole run, unstretched, for the cores.
  demand_ns: f64,
}

/// The index of the smallest value.
fn earliest(free: &[f64]) -> usize {
  let mut best: usize = 0;
  let mut i: usize = 1;
  while i < free.len() {
    if free[i] < free[best] {
      best = i;
    }
    i += 1;
  }
  best
}

struct Sim<'a> {
  p: &'a Params,
  /// Every cost stretched by this, for cores shared too thinly.
  f: f64,
  rng: Rng,
  heap: BinaryHeap<Reverse<(u64, u64, Event)>>,
  seq: u64,
  users: Vec<User>,
  rxs: Vec<Rx>,
  nodes: Vec<Node>,
  st: Stats,
  warm: f64,
  end: f64,
}

impl Sim<'_> {
  fn at(&mut self, t: f64, event: Event) {
    self.seq += 1;
    self
      .heap
      .push(Reverse((t.max(0.0) as u64, self.seq, event)));
  }

  fn jit(&mut self, x: f64) -> f64 {
    let j = self.p.jitter;
    x * (1.0 + j * (2.0 * self.rng.unit() - 1.0))
  }

  fn poll(&mut self, u: usize, now: f64) {
    let p = self.p;
    let f = self.f;
    self.users[u].state = UserState::Busy;
    let mut user = 0.0;
    let mut sys = 0.0;
    let mut t = now;
    let inbox = std::mem::take(&mut self.users[u].inbox);
    for (slot, ops) in inbox {
      let c = ops as f64 * p.complete_ns;
      user += c;
      t += c * f;
      let s = &mut self.users[u].slots[slot];
      s.left -= ops.min(s.left);
      if s.left == 0 && s.busy {
        s.busy = false;
        let sent = s.sent;
        if now >= self.warm {
          self.st.ops += p.batch as u64;
          self.st.latencies.push(t - sent);
        }
      }
    }
    let mut i: usize = 0;
    while i < p.depth {
      if self.users[u].slots[i].busy || now >= self.end {
        i += 1;
        continue;
      }
      let c = p.batch as f64 * p.define_ns + p.batch_ns;
      user += c;
      t += c * f;
      let s = &mut self.users[u].slots[i];
      s.busy = true;
      s.left = p.batch;
      s.sent = t;
      let per = p.batch / p.nodes;
      let mut n: usize = 0;
      while n < p.nodes {
        let mut ops = per;
        if n < p.batch % p.nodes {
          ops += 1;
        }
        let c = p.write_ns + ops as f64 * p.request_bytes * p.write_byte_ns;
        sys += c;
        t += c * f;
        if now >= self.warm {
          self.st.writes += 1;
        }
        self.at(t + p.net_ns, Event::Request(n, u, i, ops));
        n += 1;
      }
      i += 1;
    }
    if now >= self.warm {
      self.st.user_ns += user;
      self.st.sys_ns += sys;
    }
    self.st.demand_ns += user + sys;
    self.at(t, Event::UserIdle(u));
  }

  fn user_idle(&mut self, u: usize, now: f64) {
    if !self.users[u].inbox.is_empty() {
      self.poll(u, now);
      return;
    }
    let mut outstanding = false;
    for s in &self.users[u].slots {
      if s.busy {
        outstanding = true;
      }
    }
    if outstanding && self.p.spin_ns > 0.0 {
      let us = &mut self.users[u];
      us.state = UserState::Spinning;
      us.spin_id += 1;
      us.spin_start = now;
      let id = us.spin_id;
      self.at(now + self.p.spin_ns, Event::SpinEnd(u, id));
    } else {
      self.users[u].state = UserState::Sleeping;
    }
  }

  fn spin_end(&mut self, u: usize, id: u64, now: f64) {
    let us = &mut self.users[u];
    if us.state != UserState::Spinning || us.spin_id != id {
      return;
    }
    us.state = UserState::Sleeping;
    if now >= self.warm {
      self.st.spin_misses += 1;
      self.st.spin_ns += self.p.spin_ns;
    }
    self.st.demand_ns += self.p.spin_ns;
  }

  fn deliver(&mut self, u: usize, now: f64, slot: usize, ops: usize) {
    self.users[u].inbox.push((slot, ops));
    match self.users[u].state {
      UserState::Spinning => {
        self.users[u].state = UserState::Busy;
        let spun = now - self.users[u].spin_start;
        if now >= self.warm {
          self.st.spin_hits += 1;
          self.st.spin_ns += spun;
        }
        self.st.demand_ns += spun;
        self.at(now, Event::Poll(u));
      }
      UserState::Sleeping => {
        self.users[u].state = UserState::Waking;
        if now >= self.warm {
          self.st.wakes += 1;
          self.st.sys_ns += self.p.wake_cpu_ns;
        }
        self.st.demand_ns += self.p.wake_cpu_ns;
        self.at(now + self.p.wake_latency_ns * self.f, Event::Poll(u));
      }
      _ => {}
    }
  }

  /// When a data node thread free since `free` starts work arriving at
  /// `now`: at once if it is busy or still spinning, after a wake-up if
  /// it has gone to sleep.
  fn node_start(&self, now: f64, free: f64) -> f64 {
    if now - free > self.p.node_spin_ns {
      return now + self.p.node_wake_ns * self.f;
    }
    now.max(free)
  }

  fn request(&mut self, n: usize, u: usize, slot: usize, ops: usize, now: f64) {
    let p = self.p;
    let r = earliest(&self.nodes[n].recv_free);
    let start = self.node_start(now, self.nodes[n].recv_free[r]);
    let d = self.jit(p.node_packet_ns + ops as f64 * p.node_signal_ns) * self.f;
    let done = start + d;
    self.nodes[n].recv_free[r] = done;
    if now >= self.warm {
      self.st.node_recv_ns += d;
    }
    self.st.demand_ns += d / self.f;
    let mut left = ops;
    while left > 0 {
      let q = left.min(p.chunk_ops);
      left -= q;
      let e = earliest(&self.nodes[n].exec_free);
      let s1 = self.node_start(done, self.nodes[n].exec_free[e]);
      let d1 = self.jit(q as f64 * p.node_exec_ns) * self.f;
      self.nodes[n].exec_free[e] = s1 + d1;
      if now >= self.warm {
        self.st.node_exec_ns += d1;
      }
      self.st.demand_ns += d1 / self.f;
      self.at(s1 + d1, Event::Exec(n, u, slot, q));
    }
  }

  fn exec_done(
    &mut self,
    n: usize,
    u: usize,
    slot: usize,
    ops: usize,
    now: f64,
  ) {
    let p = self.p;
    let start = self.node_start(now, self.nodes[n].send_free);
    let d = self.jit(p.node_send_ns + ops as f64 * p.node_send_op_ns) * self.f;
    self.nodes[n].send_free = start + d;
    if now >= self.warm {
      self.st.node_send_ns += d;
    }
    self.st.demand_ns += d / self.f;
    let rx = n % p.receive_threads;
    self.at(start + d + p.net_ns, Event::Reply(rx, u, slot, ops, n));
  }

  fn reply(
    &mut self,
    rx: usize,
    u: usize,
    slot: usize,
    ops: usize,
    n: usize,
    now: f64,
  ) {
    self.rxs[rx].queue.push((u, slot, ops, n));
    if self.rxs[rx].state == RxState::Idle {
      self.rxs[rx].state = RxState::Waking;
      self.rxs[rx].blocked = true;
      self.at(now + self.p.kevent_latency_ns * self.f, Event::Round(rx));
    }
  }

  fn round(&mut self, rx: usize, now: f64) {
    let p = self.p;
    self.rxs[rx].state = RxState::Busy;
    let queue = std::mem::take(&mut self.rxs[rx].queue);
    let mut node_seen = vec![false; p.nodes];
    let mut thread_seen = vec![false; p.threads];
    let mut ops: usize = 0;
    let mut reads: u64 = 0;
    let mut posts: u64 = 0;
    for (u, _, o, n) in &queue {
      ops += o;
      if !node_seen[*n] {
        node_seen[*n] = true;
        reads += 1;
      }
      if !thread_seen[*u] {
        thread_seen[*u] = true;
        posts += 1;
      }
    }
    let bytes = ops as f64 * p.reply_bytes;
    let mut sys = reads as f64 * p.recv_ns + bytes * p.recv_byte_ns;
    if self.rxs[rx].blocked {
      sys += p.kevent_ns;
    }
    let user = p.round_ns
      + ops as f64 * p.reply_signals * p.route_ns
      + posts as f64 * p.post_ns;
    self.rxs[rx].blocked = false;
    if now >= self.warm {
      self.st.rx_user_ns += user;
      self.st.rx_sys_ns += sys;
      self.st.reads += reads;
    }
    self.st.demand_ns += user + sys;
    self.rxs[rx].deliver = queue;
    self.at(now + (user + sys) * self.f, Event::RoundEnd(rx));
  }

  fn round_end(&mut self, rx: usize, now: f64) {
    let deliver = std::mem::take(&mut self.rxs[rx].deliver);
    for (u, slot, ops, _) in deliver {
      self.deliver(u, now, slot, ops);
    }
    if self.rxs[rx].queue.is_empty() {
      self.rxs[rx].state = RxState::Idle;
    } else {
      self.at(now, Event::Round(rx));
    }
  }
}

/// What a run came to.
struct Outcome {
  rate: f64,
  p50_us: f64,
  p99_us: f64,
  user_ns: f64,
  sys_ns: f64,
  wakes: f64,
  reads: f64,
  writes: f64,
  node_recv_util: f64,
  node_exec_util: f64,
  rx_util: f64,
  spin_hit_share: f64,
  cores_asked: f64,
  stretch: f64,
}

fn run(p: &Params, f: f64) -> Outcome {
  let mut users: Vec<User> = Vec::with_capacity(p.threads);
  while users.len() < p.threads {
    let mut slots: Vec<Slot> = Vec::with_capacity(p.depth);
    while slots.len() < p.depth {
      slots.push(Slot {
        busy: false,
        left: 0,
        sent: 0.0,
      });
    }
    users.push(User {
      state: UserState::Busy,
      inbox: Vec::new(),
      slots,
      spin_id: 0,
      spin_start: 0.0,
    });
  }
  let mut rxs: Vec<Rx> = Vec::with_capacity(p.receive_threads);
  while rxs.len() < p.receive_threads {
    rxs.push(Rx {
      state: RxState::Idle,
      blocked: false,
      queue: Vec::new(),
      deliver: Vec::new(),
    });
  }
  let mut nodes: Vec<Node> = Vec::with_capacity(p.nodes);
  while nodes.len() < p.nodes {
    nodes.push(Node {
      recv_free: vec![0.0; p.node_recv_threads],
      exec_free: vec![0.0; p.node_exec_threads],
      send_free: 0.0,
    });
  }
  let warm = p.warmup * 1e9;
  let end = warm + p.seconds * 1e9;
  let mut sim = Sim {
    p,
    f,
    rng: Rng(p.seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1),
    heap: BinaryHeap::new(),
    seq: 0,
    users,
    rxs,
    nodes,
    st: Stats::default(),
    warm,
    end,
  };
  let mut u: usize = 0;
  while u < p.threads {
    sim.at(0.0, Event::Poll(u));
    u += 1;
  }
  while let Some(Reverse((time, _, event))) = sim.heap.pop() {
    let now = time as f64;
    if now > end + 5e6 {
      break;
    }
    match event {
      Event::Poll(u) => sim.poll(u, now),
      Event::UserIdle(u) => sim.user_idle(u, now),
      Event::SpinEnd(u, id) => sim.spin_end(u, id, now),
      Event::Request(n, u, slot, ops) => sim.request(n, u, slot, ops, now),
      Event::Exec(n, u, slot, ops) => sim.exec_done(n, u, slot, ops, now),
      Event::Reply(rx, u, slot, ops, n) => sim.reply(rx, u, slot, ops, n, now),
      Event::Round(rx) => sim.round(rx, now),
      Event::RoundEnd(rx) => sim.round_end(rx, now),
    }
  }
  let st = &mut sim.st;
  let window = p.seconds * 1e9;
  let ops = st.ops.max(1) as f64;
  st.latencies.sort_by(|a, b| a.total_cmp(b));
  let pct = |q: f64| -> f64 {
    if st.latencies.is_empty() {
      return 0.0;
    }
    let i =
      ((st.latencies.len() as f64 * q) as usize).min(st.latencies.len() - 1);
    st.latencies[i] / 1000.0
  };
  let shots = (st.spin_hits + st.spin_misses).max(1) as f64;
  Outcome {
    rate: st.ops as f64 / p.seconds,
    p50_us: pct(0.5),
    p99_us: pct(0.99),
    user_ns: (st.user_ns + st.rx_user_ns + st.spin_ns) / ops,
    sys_ns: (st.sys_ns + st.rx_sys_ns) / ops,
    wakes: st.wakes as f64 * 1000.0 / ops,
    reads: st.reads as f64 * 1000.0 / ops,
    writes: st.writes as f64 * 1000.0 / ops,
    node_recv_util: st.node_recv_ns
      / (window * (p.nodes * p.node_recv_threads) as f64),
    node_exec_util: st.node_exec_ns
      / (window * (p.nodes * p.node_exec_threads) as f64),
    rx_util: (st.rx_user_ns + st.rx_sys_ns)
      / (window * p.receive_threads as f64),
    spin_hit_share: st.spin_hits as f64 / shots,
    cores_asked: st.demand_ns / end + p.other_cores,
    stretch: f,
  }
}

/// Run, and with `cores` set, again until the stretch for shared cores
/// settles: each run's CPU asked for, over the cores, is the next
/// stretch, averaged with the last so that it does not swing.
fn solve(p: &Params) -> Outcome {
  let mut f = 1.0;
  let mut out = run(p, f);
  if p.cores <= 0.0 {
    return out;
  }
  let mut i = 0;
  while i < 8 {
    let wanted = (out.cores_asked / p.cores).max(1.0);
    f = 0.5 * f + 0.5 * wanted;
    out = run(p, f);
    i += 1;
  }
  out
}

fn print_row(label: &str, o: &Outcome) {
  println!(
    "{:>24} {:7.3} M/s  p50 {:7.1} us  p99 {:7.1} us  cpu {:5.0} ns \
     ({:4.0}/{:4.0})  wakes {:5.1}  reads {:5.1}  writes {:4.1}  \
     node recv {:.2} exec {:.2}  rx {:.2}  spin hits {:.2}  \
     cores {:4.1} stretch {:.2}",
    label,
    o.rate / 1e6,
    o.p50_us,
    o.p99_us,
    o.user_ns + o.sys_ns,
    o.user_ns,
    o.sys_ns,
    o.wakes,
    o.reads,
    o.writes,
    o.node_recv_util,
    o.node_exec_util,
    o.rx_util,
    o.spin_hit_share,
    o.cores_asked,
    o.stretch
  );
}

fn main() {
  std::process::exit(start());
}

fn start() -> i32 {
  let mut p = Params::defaults();
  let mut sweep: Option<(String, Vec<f64>)> = None;
  let mut show = false;
  let args: Vec<String> = std::env::args().skip(1).collect();
  for arg in &args {
    if arg == "show" {
      show = true;
      continue;
    }
    let (name, value) = match arg.split_once('=') {
      Some(pair) => pair,
      None => {
        println!("Arguments are name=value, sweep=name:v1,v2,... or show");
        return 1;
      }
    };
    if name == "sweep" {
      let (what, list) = match value.split_once(':') {
        Some(pair) => pair,
        None => {
          println!("A sweep is sweep=name:v1,v2,...");
          return 1;
        }
      };
      let mut values: Vec<f64> = Vec::new();
      for v in list.split(',') {
        match v.parse::<f64>() {
          Ok(x) => values.push(x),
          Err(_) => {
            println!("{} is not a number", v);
            return 1;
          }
        }
      }
      let mut probe = p.clone();
      if !probe.set(what, 0.0) {
        println!("There is no parameter {}; show lists them", what);
        return 1;
      }
      sweep = Some((what.to_string(), values));
      continue;
    }
    let number = match value.parse::<f64>() {
      Ok(x) => x,
      Err(_) => {
        println!("{} is not a number", value);
        return 1;
      }
    };
    if !p.set(name, number) {
      println!("There is no parameter {}; show lists them", name);
      return 1;
    }
  }
  if show {
    println!("{:#?}", p);
    return 0;
  }
  match sweep {
    None => print_row("", &solve(&p)),
    Some((what, values)) => {
      for v in values {
        let mut q = p.clone();
        q.set(&what, v);
        print_row(&format!("{}={}", what, v), &solve(&q));
      }
    }
  }
  0
}

#[cfg(test)]
mod tests {
  use super::*;

  fn quick() -> Params {
    let mut p = Params::defaults();
    p.seconds = 0.02;
    p.warmup = 0.005;
    p
  }

  #[test]
  fn deeper_pipelines_carry_more() {
    let mut p = quick();
    p.cores = 0.0;
    p.depth = 1;
    let one = solve(&p);
    p.depth = 4;
    let four = solve(&p);
    assert!(four.rate > one.rate * 1.5, "{} {}", four.rate, one.rate);
  }

  #[test]
  fn bigger_batches_cost_less_per_operation() {
    let mut p = quick();
    p.batch = 50;
    let small = solve(&p);
    p.batch = 400;
    let big = solve(&p);
    assert!(big.user_ns + big.sys_ns < small.user_ns + small.sys_ns);
  }

  #[test]
  fn a_saturated_node_receive_thread_is_the_limit() {
    let mut p = quick();
    p.cores = 0.0;
    p.threads = 8;
    p.batch = 300;
    let one = solve(&p);
    assert!(one.node_recv_util > 0.95, "{}", one.node_recv_util);
    p.node_recv_threads = 2;
    let two = solve(&p);
    assert!(two.node_recv_util < 0.8, "{}", two.node_recv_util);
    assert!(two.rate > one.rate, "{} {}", two.rate, one.rate);
  }

  #[test]
  fn cpu_taken_by_others_slows_everything() {
    let mut p = quick();
    p.threads = 8;
    p.batch = 300;
    p.other_cores = 0.0;
    let alone = solve(&p);
    p.other_cores = 8.0;
    let shared = solve(&p);
    assert!(shared.stretch > alone.stretch);
    assert!(
      shared.rate < alone.rate * 0.9,
      "{} {}",
      shared.rate,
      alone.rate
    );
  }

  #[test]
  fn a_sleeping_data_node_adds_latency() {
    let mut p = quick();
    p.node_wake_ns = 0.0;
    let awake = solve(&p);
    p.node_wake_ns = 20_000.0;
    let asleep = solve(&p);
    assert!(asleep.p50_us > awake.p50_us + 20.0);
    p.node_spin_ns = 1e9;
    let spinning = solve(&p);
    assert!(spinning.p50_us < asleep.p50_us);
  }

  #[test]
  fn spinning_costs_cpu() {
    let mut p = quick();
    let none = solve(&p);
    p.spin_ns = 20_000.0;
    let spin = solve(&p);
    assert!(spin.user_ns > none.user_ns);
    assert!(spin.wakes < none.wakes);
  }
}
