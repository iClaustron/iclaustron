// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The adaptive send algorithm (`legacy-c/api/ic_apid_adaptive_send.ic`):
//! whether a send to a node should be held back for company.
//!
//! One socket write per operation costs a data node a receive, an
//! execution round and a reply packet each, where a hundred operations
//! in one packet cost it one of each. A thread that sends alone can
//! gather no more than its own batch, but when many threads send to
//! the same node, holding a send back for a moment lets the next
//! thread's signals go with it. The question is how long a moment. The
//! answer here is a bound, 95% of the held sends waiting no longer
//! than a configured time, and a count of how many sends may be held
//! for one write, tuned from what the sends have actually been doing.
//!
//! The statistics keep the times of the last sends to the node. With
//! `max_num_waits` sends allowed to wait, the time a held send would
//! have waited is the time from the send `max_num_waits` back to now,
//! and the time it would wait with one more allowed is the time from
//! the send one further back. Their means over an adjustment interval
//! are compared with half the bound, which is the mean that keeps 95%
//! of a normal distribution within the bound (1.96, taken as 2): a mean
//! over it brings the count down by one, a mean with one more allowed
//! under it puts it up by one. A thread sending alone in lock step
//! never sees the count rise, since its own sends are a round trip
//! apart; many threads sending to one node do.
//!
//! The C adjusts from the receive thread on every wake-up, and records
//! the wake-up as if it were a send; here the node adjusts itself on
//! the send path once an interval, and records only sends. A send that
//! decides to wait is not lost if no company comes: the send pool
//! ([`send_pool`](crate::send_pool)) is told when the wait ends and
//! writes what waits then.

use ic_port::time::IcTimer;

/// How many send times are kept.
pub const IC_MAX_SEND_TIMERS: usize = 16;
/// The most sends that may be held back for one write, and the furthest
/// back the statistics look.
pub const IC_MAX_SENDS_TRACKED: u32 = 8;
/// How long a send may be held back for company, in nanoseconds. The C
/// reads it from the link's configuration (`socket_max_wait_in_nanos`,
/// zero for never); until that parameter is read here it is the grace
/// period the NDB API's own adaptive send allows itself
/// (`TransporterFacade::threadMainSend`), 200 µs.
pub const IC_DEFAULT_MAX_SEND_WAIT_NANOS: u64 = 200_000;
/// How often the count of sends allowed to wait is adjusted.
pub const IC_ADAPTIVE_ADJUST_NANOS: u64 = 1_000_000;

/// The adaptive send state of one node connection.
#[derive(Clone, Debug)]
pub struct AdaptiveSend {
  /// The bound, in nanoseconds; zero means never wait.
  max_wait_nanos: u64,
  /// How many sends have been held back for the write now waiting.
  num_waits: u32,
  /// How many may be.
  max_num_waits: u32,
  /// When the first of them was held back; zero when none is.
  first_buffered: IcTimer,
  /// The times of the last sends, a ring.
  send_times: [IcTimer; IC_MAX_SEND_TIMERS],
  /// How many sends have been recorded.
  num_sends: u64,
  /// The sums over the adjustment interval, and how many sends went
  /// into them.
  tot_curr_wait_time: u64,
  tot_wait_time_plus_one: u64,
  num_stats: u64,
  /// When the count was last adjusted.
  last_adjust: IcTimer,
}

impl AdaptiveSend {
  /// The state for a new link, holding nothing back yet.
  pub fn new(max_wait_nanos: u64) -> AdaptiveSend {
    AdaptiveSend {
      max_wait_nanos,
      num_waits: 0,
      max_num_waits: 0,
      first_buffered: 0,
      send_times: [0; IC_MAX_SEND_TIMERS],
      num_sends: 0,
      tot_curr_wait_time: 0,
      tot_wait_time_plus_one: 0,
      num_stats: 0,
      last_adjust: 0,
    }
  }

  /// How many sends may be held back for one write, as tuned so far.
  pub fn max_num_waits(&self) -> u32 {
    self.max_num_waits
  }

  /// Whether a send that need not go at once should wait for company
  /// (`adaptive_send_algorithm_decision`). True to hold it back. Asked
  /// only when no write is under way; a write under way takes whatever
  /// waits when it is done.
  pub fn decide(&mut self, now: IcTimer) -> bool {
    if self.max_wait_nanos == 0 || self.num_waits >= self.max_num_waits {
      return self.no_wait();
    }
    if self.first_buffered != 0
      && now - self.first_buffered > self.max_wait_nanos
    {
      return self.no_wait();
    }
    if self.num_waits == 0 {
      self.first_buffered = now;
    }
    self.num_waits += 1;
    true
  }

  /// When the sends now held back must go: for the pool, right after
  /// a decision to hold.
  pub fn deadline(&self) -> IcTimer {
    self.first_buffered + self.max_wait_nanos
  }

  /// The held sends went, whether with company or because their time
  /// was up.
  pub fn wait_ended(&mut self) {
    self.no_wait();
  }

  fn no_wait(&mut self) -> bool {
    self.first_buffered = 0;
    self.num_waits = 0;
    false
  }

  /// Note a send, held or not (`adaptive_send_algorithm_statistics`),
  /// and adjust the count once an interval
  /// (`adaptive_send_algorithm_adjust`).
  pub fn record_send(&mut self, now: IcTimer) {
    let current = self.send_time_back(self.max_num_waits);
    let plus_one = self.send_time_back(self.max_num_waits + 1);
    if current != 0 && plus_one != 0 {
      self.tot_curr_wait_time += now - current;
      self.tot_wait_time_plus_one += now - plus_one;
      self.num_stats += 1;
    }
    self.send_times[(self.num_sends % IC_MAX_SEND_TIMERS as u64) as usize] =
      now;
    self.num_sends += 1;
    if now - self.last_adjust >= IC_ADAPTIVE_ADJUST_NANOS {
      self.adjust(now);
    }
  }

  /// The time of the send `back` sends ago, the last one being zero
  /// back; zero if there has been no such send.
  fn send_time_back(&self, back: u32) -> IcTimer {
    let back = back as u64;
    if back >= self.num_sends || back >= IC_MAX_SEND_TIMERS as u64 {
      return 0;
    }
    let index = (self.num_sends - 1 - back) % IC_MAX_SEND_TIMERS as u64;
    self.send_times[index as usize]
  }

  fn adjust(&mut self, now: IcTimer) {
    self.last_adjust = now;
    if self.num_stats == 0 {
      return;
    }
    let limit = self.max_wait_nanos / 2;
    let mean_curr_wait_time = self.tot_curr_wait_time / self.num_stats;
    let mean_wait_time_plus_one = self.tot_wait_time_plus_one / self.num_stats;
    self.tot_curr_wait_time = 0;
    self.tot_wait_time_plus_one = 0;
    self.num_stats = 0;
    if mean_curr_wait_time > limit && self.max_num_waits > 0 {
      // The held sends wait longer than the bound allows.
      self.max_num_waits -= 1;
    }
    if mean_wait_time_plus_one < limit
      && self.max_num_waits < IC_MAX_SENDS_TRACKED
    {
      // They would still be within it with one more held.
      self.max_num_waits += 1;
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  const MAX_WAIT: u64 = 200_000;

  /// Record sends `gap` nanoseconds apart for `count` of them, from
  /// `start`, and return the time after the last.
  fn send_every(
    adaptive: &mut AdaptiveSend,
    start: IcTimer,
    gap: u64,
    count: u32,
  ) -> IcTimer {
    let mut now = start;
    let mut i = 0;
    while i < count {
      now += gap;
      adaptive.record_send(now);
      i += 1;
    }
    now
  }

  #[test]
  fn never_waits_at_first() {
    let mut adaptive = AdaptiveSend::new(MAX_WAIT);
    assert!(!adaptive.decide(1_000));
    assert_eq!(adaptive.max_num_waits(), 0);
  }

  #[test]
  fn never_waits_with_no_bound() {
    let mut adaptive = AdaptiveSend::new(0);
    // Sends 10 µs apart would raise the count with a bound.
    send_every(&mut adaptive, 1_000, 10_000, 400);
    assert_eq!(adaptive.max_num_waits(), 0);
    assert!(!adaptive.decide(5_000_000));
  }

  #[test]
  fn frequent_sends_raise_the_count() {
    let mut adaptive = AdaptiveSend::new(MAX_WAIT);
    // 10 µs apart: with k held, the oldest waits 10(k+1) µs, well under
    // the 100 µs limit up to the most tracked.
    let now = send_every(&mut adaptive, 1_000, 10_000, 2_000);
    assert_eq!(adaptive.max_num_waits(), IC_MAX_SENDS_TRACKED);
    // Now the first sends wait, and the one past the count goes.
    let mut held = 0;
    while adaptive.decide(now) {
      held += 1;
    }
    assert_eq!(held, IC_MAX_SENDS_TRACKED);
  }

  #[test]
  fn lock_step_sends_keep_the_count_at_zero() {
    let mut adaptive = AdaptiveSend::new(MAX_WAIT);
    // A round trip apart, 150 µs: two of them span 300 µs, over the
    // limit, so holding even one would break the bound.
    send_every(&mut adaptive, 1_000, 150_000, 200);
    assert_eq!(adaptive.max_num_waits(), 0);
  }

  #[test]
  fn the_count_comes_down_when_waits_grow() {
    let mut adaptive = AdaptiveSend::new(MAX_WAIT);
    let now = send_every(&mut adaptive, 1_000, 10_000, 2_000);
    assert_eq!(adaptive.max_num_waits(), IC_MAX_SENDS_TRACKED);
    send_every(&mut adaptive, now, 1_000_000, 20);
    assert_eq!(adaptive.max_num_waits(), 0);
  }

  #[test]
  fn a_held_send_goes_when_its_time_is_up() {
    let mut adaptive = AdaptiveSend::new(MAX_WAIT);
    let now = send_every(&mut adaptive, 1_000, 10_000, 2_000);
    assert!(adaptive.decide(now));
    assert_eq!(adaptive.deadline(), now + MAX_WAIT);
    // Another thread comes past the bound: it must not wait either.
    assert!(!adaptive.decide(now + MAX_WAIT + 1));
    // The wait state is cleared with it.
    assert!(adaptive.decide(now + MAX_WAIT + 2));
    adaptive.wait_ended();
    assert_eq!(adaptive.deadline(), MAX_WAIT);
  }
}
