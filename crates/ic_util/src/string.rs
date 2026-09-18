// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The few string helpers that have no direct equivalent in the Rust
//! standard library (`legacy-c/util/ic_string.c`).
//!
//! `IC_STRING` itself is not translated. It was a pointer, a length and
//! a flag saying whether the text was NUL terminated, which is what Rust
//! `&str` and `String` already are, with the length always known. The
//! directory layout helpers (`ic_set_base_dir` and friends) served the
//! cluster server and the process controller and are out of scope.

use ic_port::err;
use ic_port::IcError;

/// Parse a number as a configuration file writes it: decimal digits with
/// an optional `k`, `m` or `g` suffix meaning 1024, 1024² and 1024³
/// (`ic_conv_config_str_to_int`).
pub fn parse_config_number(text: &str) -> Result<u64, IcError> {
  let trimmed = text.trim();
  if trimmed.is_empty() {
    return Err(IcError::new(err::IC_ERROR_WRONG_CONFIG_NUMBER));
  }
  let mut multiplier: u64 = 1;
  let mut digits = trimmed;
  let last = trimmed.as_bytes()[trimmed.len() - 1];
  if last == b'k' || last == b'K' {
    multiplier = 1024;
    digits = &trimmed[..trimmed.len() - 1];
  } else if last == b'm' || last == b'M' {
    multiplier = 1024 * 1024;
    digits = &trimmed[..trimmed.len() - 1];
  } else if last == b'g' || last == b'G' {
    multiplier = 1024 * 1024 * 1024;
    digits = &trimmed[..trimmed.len() - 1];
  }
  let value = match digits.parse::<u64>() {
    Ok(v) => v,
    Err(_) => return Err(IcError::new(err::IC_ERROR_WRONG_CONFIG_NUMBER)),
  };
  match value.checked_mul(multiplier) {
    Some(v) => Ok(v),
    None => Err(IcError::new(err::IC_ERROR_WRONG_CONFIG_NUMBER)),
  }
}

/// Parse exactly `num_chars` decimal digits from the front of `text`
/// (`ic_convert_str_to_int_fixed_size`). Protocol fields with a fixed
/// width are read this way.
pub fn parse_fixed_digits(
  text: &str,
  num_chars: usize,
) -> Result<u64, IcError> {
  if text.len() < num_chars || num_chars == 0 {
    return Err(IcError::new(err::IC_ERROR_WRONG_CONFIG_NUMBER));
  }
  let head = &text[..num_chars];
  match head.parse::<u64>() {
    Ok(v) => Ok(v),
    Err(_) => Err(IcError::new(err::IC_ERROR_WRONG_CONFIG_NUMBER)),
  }
}

/// Number of leading decimal digits in `text`, at most `max_chars`
/// (`ic_count_characters`).
pub fn count_digits(text: &str, max_chars: usize) -> usize {
  let bytes = text.as_bytes();
  let mut count: usize = 0;
  while count < bytes.len() && count < max_chars {
    if !bytes[count].is_ascii_digit() {
      return count;
    }
    count += 1;
  }
  count
}

/// An ASCII uppercase copy (`ic_convert_to_uppercase`).
pub fn to_uppercase(text: &str) -> String {
  text.to_ascii_uppercase()
}

/// Compare ignoring ASCII case, as the protocol keyword comparisons did
/// (`ic_cmp_null_term_str_upper`).
pub fn eq_ignore_case(first: &str, second: &str) -> bool {
  first.eq_ignore_ascii_case(second)
}

/// The value after `prefix` in `line`, if the line starts with it,
/// ignoring ASCII case and trimming spaces. This is how the NDB
/// management protocol's `name: value` lines are read.
pub fn value_after(line: &str, prefix: &str) -> Option<String> {
  if line.len() < prefix.len() {
    return None;
  }
  if !line[..prefix.len()].eq_ignore_ascii_case(prefix) {
    return None;
  }
  Some(line[prefix.len()..].trim().to_string())
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn config_numbers() {
    assert_eq!(parse_config_number("0"), Ok(0));
    assert_eq!(parse_config_number("100"), Ok(100));
    assert_eq!(parse_config_number(" 64k "), Ok(65536));
    assert_eq!(parse_config_number("2M"), Ok(2 * 1024 * 1024));
    assert_eq!(parse_config_number("3g"), Ok(3 * 1024 * 1024 * 1024));
    assert!(parse_config_number("").is_err());
    assert!(parse_config_number("abc").is_err());
    assert!(parse_config_number("-1").is_err());
    assert!(parse_config_number("99999999999999999999g").is_err());
  }

  #[test]
  fn fixed_digits_and_counting() {
    assert_eq!(parse_fixed_digits("00123rest", 5), Ok(123));
    assert!(parse_fixed_digits("12", 5).is_err());
    assert!(parse_fixed_digits("12ab", 4).is_err());
    assert_eq!(count_digits("1234abc", 10), 4);
    assert_eq!(count_digits("1234abc", 2), 2);
    assert_eq!(count_digits("abc", 10), 0);
  }

  #[test]
  fn case_helpers() {
    assert_eq!(to_uppercase("MixedCase99"), "MIXEDCASE99");
    assert!(eq_ignore_case("Ok", "ok"));
    assert!(!eq_ignore_case("ok", "okay"));
    assert_eq!(value_after("nodeid: 68", "nodeid:"), Some("68".to_string()));
    assert_eq!(
      value_after("Content-Length: 4096", "content-length:"),
      Some("4096".to_string())
    );
    assert_eq!(value_after("result: Ok", "nodeid:"), None);
    assert_eq!(value_after("no", "nodeid:"), None);
  }
}
