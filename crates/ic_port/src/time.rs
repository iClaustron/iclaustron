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

#[cfg(test)]
mod tests {
  use super::*;

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
