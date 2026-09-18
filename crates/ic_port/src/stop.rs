// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The process-wide stop flag (`ic_get_stop_flag`/`ic_set_stop_flag`).
//!
//! Anyone wanting to stop the iClaustron subsystem sets the flag; library
//! threads check it regularly (at least every few seconds).

use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;

static STOP_FLAG: AtomicU32 = AtomicU32::new(0);

/// 1 if a stop has been ordered, 0 otherwise.
pub fn get_stop_flag() -> u32 {
  STOP_FLAG.load(Ordering::Acquire)
}

/// Order the iClaustron subsystem to stop.
pub fn set_stop_flag() {
  STOP_FLAG.store(1, Ordering::Release);
}

/// Clear the stop flag again (tests and controlled restarts).
pub fn clear_stop_flag() {
  STOP_FLAG.store(0, Ordering::Release);
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn set_and_clear() {
    clear_stop_flag();
    assert_eq!(get_stop_flag(), 0);
    set_stop_flag();
    assert_eq!(get_stop_flag(), 1);
    clear_stop_flag();
    assert_eq!(get_stop_flag(), 0);
  }
}
