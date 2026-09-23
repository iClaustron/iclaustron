// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Nanosecond timers and sleeps (`IC_TIMER`, `ic_gethrtime`,
//! `ic_*_elapsed`, `ic_sleep_low`, `ic_microsleep`).

use std::sync::OnceLock;
use std::time::Duration;
use std::time::Instant;

/// A point in time in nanoseconds from an arbitrary monotonic origin.
pub type IcTimer = u64;

/// The value meaning "no time recorded".
pub const IC_UNDEFINED_TIME: IcTimer = 0;

static ORIGIN: OnceLock<Instant> = OnceLock::new();

/// True if the timer holds a recorded time.
pub fn check_defined_time(timer: IcTimer) -> bool {
  timer != IC_UNDEFINED_TIME
}

/// Current monotonic time in nanoseconds. Never returns
/// [`IC_UNDEFINED_TIME`].
pub fn gethrtime() -> IcTimer {
  let origin = ORIGIN.get_or_init(Instant::now);
  let nanos = origin.elapsed().as_nanos() as u64;
  nanos + 1
}

/// Nanoseconds between two timers; 0 if the end is before the start.
pub fn nanos_elapsed(start_time: IcTimer, end_time: IcTimer) -> IcTimer {
  if end_time < start_time {
    return 0;
  }
  end_time - start_time
}

/// Microseconds between two timers.
pub fn micros_elapsed(start_time: IcTimer, end_time: IcTimer) -> IcTimer {
  nanos_elapsed(start_time, end_time) / 1000
}

/// Milliseconds between two timers.
pub fn millis_elapsed(start_time: IcTimer, end_time: IcTimer) -> IcTimer {
  nanos_elapsed(start_time, end_time) / 1_000_000
}

/// Sleep for a number of seconds.
pub fn sleep_low(seconds_to_sleep: u32) {
  std::thread::sleep(Duration::from_secs(seconds_to_sleep as u64));
}

/// Sleep for a number of microseconds.
pub fn microsleep(microseconds_to_sleep: u32) {
  std::thread::sleep(Duration::from_micros(microseconds_to_sleep as u64));
}

/// What the process has used so far (`getrusage`): CPU time in user
/// and system mode, and how often a thread gave up the CPU because it
/// waited (voluntary switches, each a sleep that took a wake-up) or was
/// made to (involuntary). For measuring what a benchmark costs rather
/// than only how fast it went.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProcessUsage {
  /// CPU time in user mode, in microseconds.
  pub user_micros: u64,
  /// CPU time in the kernel, in microseconds.
  pub system_micros: u64,
  /// Times a thread gave up the CPU to wait.
  pub voluntary_switches: u64,
  /// Times a thread was made to give up the CPU.
  pub involuntary_switches: u64,
  /// Page faults served without I/O: memory mapped in on first touch.
  pub minor_faults: u64,
}

/// The process's usage so far; all zero if the system will not say.
pub fn process_usage() -> ProcessUsage {
  let mut usage = std::mem::MaybeUninit::<libc::rusage>::zeroed();
  // SAFETY: getrusage writes the whole structure it is given, which is
  // zeroed to begin with, so it is initialised whether or not it fails.
  let (ret, usage) = unsafe {
    let ret = libc::getrusage(libc::RUSAGE_SELF, usage.as_mut_ptr());
    (ret, usage.assume_init())
  };
  if ret != 0 {
    return ProcessUsage::default();
  }
  let micros = |tv: libc::timeval| -> u64 {
    tv.tv_sec as u64 * 1_000_000 + tv.tv_usec as u64
  };
  ProcessUsage {
    user_micros: micros(usage.ru_utime),
    system_micros: micros(usage.ru_stime),
    voluntary_switches: usage.ru_nvcsw as u64,
    involuntary_switches: usage.ru_nivcsw as u64,
    minor_faults: usage.ru_minflt as u64,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn usage_grows_with_work() {
    let before = process_usage();
    let mut x: u64 = 0;
    let start = gethrtime();
    while millis_elapsed(start, gethrtime()) < 20 {
      x = x.wrapping_mul(31).wrapping_add(1);
    }
    let after = process_usage();
    assert!(x != 1);
    let cpu = |u: ProcessUsage| u.user_micros + u.system_micros;
    assert!(cpu(after) > cpu(before));
  }

  #[test]
  fn elapsed_is_monotonic() {
    let start = gethrtime();
    assert!(check_defined_time(start));
    microsleep(2000);
    let end = gethrtime();
    assert!(nanos_elapsed(start, end) >= 2_000_000);
    assert!(micros_elapsed(start, end) >= 2000);
    assert!(millis_elapsed(start, end) >= 2);
    assert_eq!(nanos_elapsed(end, start), 0);
  }
}
