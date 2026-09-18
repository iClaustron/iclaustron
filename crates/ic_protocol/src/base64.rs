// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Base64 (`legacy-c/protocol/ic_base64.c`).
//!
//! The management server sends the cluster configuration as a base64
//! blob after the header lines of its `get config` reply, so decoding
//! this is on the path to every other thing the library does.
//!
//! The encoder breaks lines every 19 quads, that is every 76 characters,
//! and ends with a newline, which is the shape the management protocol
//! uses. The decoder ignores line breaks and spaces wherever they fall,
//! so a blob that arrives in lines needs no preparation.

use ic_port::err;
use ic_port::IcError;

/// Characters per line the encoder produces, matching the C.
pub const BASE64_LINE_LEN: usize = 76;

const ENCODE_TABLE: &[u8; 64] =
  b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// The six bits a base64 character stands for, or `None` if it is not a
/// base64 character.
fn decode_char(c: u8) -> Option<u8> {
  if c.is_ascii_uppercase() {
    return Some(c - b'A');
  }
  if c.is_ascii_lowercase() {
    return Some(c - b'a' + 26);
  }
  if c.is_ascii_digit() {
    return Some(c - b'0' + 52);
  }
  if c == b'+' {
    return Some(62);
  }
  if c == b'/' {
    return Some(63);
  }
  None
}

fn is_skippable(c: u8) -> bool {
  c == b'\n' || c == b'\r' || c == b' ' || c == b'\t'
}

/// Encode with a line break every 76 characters and a final newline,
/// as the management protocol expects (`ic_base64_encode`).
pub fn encode(src: &[u8]) -> String {
  encode_inner(src, true)
}

/// Encode as one unbroken string.
pub fn encode_no_breaks(src: &[u8]) -> String {
  encode_inner(src, false)
}

fn encode_inner(src: &[u8], break_lines: bool) -> String {
  let num_quads = src.len().div_ceil(3);
  let num_breaks = if break_lines {
    num_quads.div_ceil(19)
  } else {
    0
  };
  let mut out = String::with_capacity(4 * num_quads + num_breaks);
  let mut quads_on_line: usize = 0;
  let mut i: usize = 0;
  while i < src.len() {
    let left = src.len() - i;
    let b0 = src[i] as u32;
    let b1 = if left > 1 { src[i + 1] as u32 } else { 0 };
    let b2 = if left > 2 { src[i + 2] as u32 } else { 0 };
    out.push(ENCODE_TABLE[(b0 >> 2) as usize] as char);
    out.push(ENCODE_TABLE[(((b0 & 0x3) << 4) | (b1 >> 4)) as usize] as char);
    if left > 1 {
      out.push(ENCODE_TABLE[(((b1 & 0xF) << 2) | (b2 >> 6)) as usize] as char);
    } else {
      out.push('=');
    }
    if left > 2 {
      out.push(ENCODE_TABLE[(b2 & 0x3F) as usize] as char);
    } else {
      out.push('=');
    }
    i += 3;
    quads_on_line += 1;
    if break_lines && quads_on_line == 19 {
      out.push('\n');
      quads_on_line = 0;
    }
  }
  if break_lines && quads_on_line != 0 {
    out.push('\n');
  }
  out
}

/// Decode, ignoring any line breaks and spaces
/// (`ic_base64_decode`).
///
/// Fails with `IC_PROTOCOL_ERROR` on a character that is not base64, on
/// data after the padding, or on a truncated final group.
pub fn decode(src: &[u8]) -> Result<Vec<u8>, IcError> {
  let bad = IcError::new(err::IC_PROTOCOL_ERROR);
  let mut out: Vec<u8> = Vec::with_capacity(src.len() / 4 * 3 + 3);
  let mut group = [0u8; 4];
  let mut in_group: usize = 0;
  let mut padding: usize = 0;
  let mut i: usize = 0;
  while i < src.len() {
    let c = src[i];
    i += 1;
    if is_skippable(c) {
      continue;
    }
    if c == 0 {
      /* The C encoder appended a NUL after the last line. */
      break;
    }
    if c == b'=' {
      padding += 1;
      group[in_group] = 0;
      in_group += 1;
    } else {
      if padding > 0 {
        return Err(bad);
      }
      match decode_char(c) {
        Some(bits) => {
          group[in_group] = bits;
          in_group += 1;
        }
        None => return Err(bad),
      }
    }
    if in_group < 4 {
      continue;
    }
    if padding > 2 {
      return Err(bad);
    }
    out.push((group[0] << 2) | (group[1] >> 4));
    if padding < 2 {
      out.push(((group[1] & 0x0F) << 4) | (group[2] >> 2));
    }
    if padding < 1 {
      out.push(((group[2] & 0x03) << 6) | group[3]);
    }
    in_group = 0;
    if padding > 0 {
      /* Padding ends the data; only blanks may follow. */
      while i < src.len() {
        let tail = src[i];
        i += 1;
        if tail == 0 {
          break;
        }
        if !is_skippable(tail) {
          return Err(bad);
        }
      }
      return Ok(out);
    }
  }
  if in_group != 0 {
    return Err(bad);
  }
  Ok(out)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn the_standard_test_vectors() {
    /* From RFC 4648 section 10. */
    assert_eq!(encode_no_breaks(b""), "");
    assert_eq!(encode_no_breaks(b"f"), "Zg==");
    assert_eq!(encode_no_breaks(b"fo"), "Zm8=");
    assert_eq!(encode_no_breaks(b"foo"), "Zm9v");
    assert_eq!(encode_no_breaks(b"foob"), "Zm9vYg==");
    assert_eq!(encode_no_breaks(b"fooba"), "Zm9vYmE=");
    assert_eq!(encode_no_breaks(b"foobar"), "Zm9vYmFy");
    assert_eq!(decode(b"").expect("d"), b"");
    assert_eq!(decode(b"Zg==").expect("d"), b"f");
    assert_eq!(decode(b"Zm8=").expect("d"), b"fo");
    assert_eq!(decode(b"Zm9v").expect("d"), b"foo");
    assert_eq!(decode(b"Zm9vYg==").expect("d"), b"foob");
    assert_eq!(decode(b"Zm9vYmE=").expect("d"), b"fooba");
    assert_eq!(decode(b"Zm9vYmFy").expect("d"), b"foobar");
  }

  #[test]
  fn every_length_round_trips() {
    let mut len: usize = 0;
    while len < 300 {
      let mut data: Vec<u8> = Vec::with_capacity(len);
      let mut i: usize = 0;
      while i < len {
        data.push(((i * 7 + 13) & 0xFF) as u8);
        i += 1;
      }
      let text = encode(&data);
      let back = decode(text.as_bytes()).expect("decode");
      assert_eq!(back, data, "length {}", len);
      let plain = encode_no_breaks(&data);
      assert_eq!(decode(plain.as_bytes()).expect("decode"), data);
      len += 1;
    }
  }

  #[test]
  fn lines_are_broken_every_76_characters() {
    /* 19 quads is 57 bytes of input per line. */
    let data = vec![0xABu8; 57 * 3];
    let text = encode(&data);
    let mut lines: Vec<&str> = Vec::new();
    for line in text.split('\n') {
      if !line.is_empty() {
        lines.push(line);
      }
    }
    assert_eq!(lines.len(), 3);
    for line in &lines {
      assert_eq!(line.len(), BASE64_LINE_LEN);
    }
    assert!(text.ends_with('\n'));
    assert_eq!(decode(text.as_bytes()).expect("decode"), data);
  }

  #[test]
  fn blanks_anywhere_are_ignored() {
    assert_eq!(decode(b"Zm9v YmFy").expect("d"), b"foobar");
    assert_eq!(decode(b"Zm9v\nYmFy\n").expect("d"), b"foobar");
    assert_eq!(decode(b"Zm9v\r\nYmFy").expect("d"), b"foobar");
    assert_eq!(decode(b"  Zm9vYmFy  \n").expect("d"), b"foobar");
    assert_eq!(decode(b"Zm8=\n").expect("d"), b"fo");
  }

  #[test]
  fn bad_input_is_refused() {
    /* A character outside the alphabet. */
    assert!(decode(b"Zm9v*mFy").is_err());
    /* A group that stops half way. */
    assert!(decode(b"Zm9vY").is_err());
    assert!(decode(b"Zg").is_err());
    /* Data after the padding. */
    assert!(decode(b"Zm8=Zm9v").is_err());
    /* Too much padding. */
    assert!(decode(b"Z===").is_err());
  }

  #[test]
  fn a_trailing_nul_ends_the_blob() {
    /* The C encoder wrote a NUL after the last line and the reply from
    the management server carries one too. */
    let mut text: Vec<u8> = b"Zm9vYmFy\n".to_vec();
    text.push(0);
    assert_eq!(decode(&text).expect("decode"), b"foobar");
  }
}
