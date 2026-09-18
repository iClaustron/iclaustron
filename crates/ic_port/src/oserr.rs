// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Last operating system error and its text (`ic_get_last_error`,
//! `ic_get_last_socket_error`, `ic_get_strerror`).

/// The `errno` of the last failed OS call on this thread, 0 if none.
pub fn last_error() -> i32 {
  std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

/// The last socket error; on Unix the same as [`last_error`].
pub fn last_socket_error() -> i32 {
  last_error()
}

/// The OS text for an `errno` value (`strerror`).
pub fn strerror(code: i32) -> String {
  let e = std::io::Error::from_raw_os_error(code);
  let text = e.to_string();
  // std appends " (os error N)"; the C strerror did not.
  match text.find(" (os error") {
    Some(pos) => text[..pos].to_string(),
    None => text,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn strerror_has_text() {
    let text = strerror(libc::ENOENT);
    assert!(!text.is_empty());
    assert!(!text.contains("os error"));
  }
}
