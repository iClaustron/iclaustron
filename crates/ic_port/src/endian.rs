// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Byte order helpers (`ic_byte_order`, `ic_swap_endian_word`).
//!
//! The NDB signal protocol carries the sender's byte order in the header
//! and the receiver rejects a mismatch, so no swapping happens on the
//! signal path. Swapping is still needed for the configuration blob,
//! which is in network byte order.

/// 0 on a little endian machine, 1 on a big endian machine, as the NDB
/// protocol byte order flag expects.
pub fn byte_order() -> u32 {
  if cfg!(target_endian = "little") {
    0
  } else {
    1
  }
}

/// Reverse the four bytes of a word.
pub fn swap_endian_word(word: u32) -> u32 {
  word.swap_bytes()
}

/// Read a big endian (network order) word from four bytes.
pub fn read_be_u32(bytes: &[u8]) -> u32 {
  u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

/// Write a word as big endian (network order) into four bytes.
pub fn write_be_u32(word: u32, bytes: &mut [u8]) {
  let b = word.to_be_bytes();
  bytes[0] = b[0];
  bytes[1] = b[1];
  bytes[2] = b[2];
  bytes[3] = b[3];
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn swap_and_network_order() {
    assert_eq!(swap_endian_word(0x1122_3344), 0x4433_2211);
    let mut buf = [0u8; 4];
    write_be_u32(0x0102_0304, &mut buf);
    assert_eq!(buf, [1, 2, 3, 4]);
    assert_eq!(read_be_u32(&buf), 0x0102_0304);
    assert!(byte_order() == 0 || byte_order() == 1);
  }
}
