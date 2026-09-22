// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! MD5, the digest RonDB hashes a key with unless the table was made
//! with the newer hash. New here, written from the algorithm, which is
//! public (RFC 1321); the data nodes carry a copy of the same one.
//!
//! Only the digest is wanted, as four 32-bit words in the order the
//! algorithm produces them, since the data nodes read the second of
//! them as a key's place in the cluster. This is a digest, not a
//! password hash, and nothing here is meant to resist an attacker.

/// Words in a digest.
pub const IC_MD5_WORDS: usize = 4;
/// Bytes in a block.
const IC_MD5_BLOCK: usize = 64;

/// How far each round rotates.
const IC_MD5_SHIFTS: [u32; 64] = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5,
  9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11,
  16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10,
  15, 21,
];

/// The sine table the algorithm adds in, one value per round.
const IC_MD5_SINE: [u32; 64] = [
  0xD76A_A478,
  0xE8C7_B756,
  0x2420_70DB,
  0xC1BD_CEEE,
  0xF57C_0FAF,
  0x4787_C62A,
  0xA830_4613,
  0xFD46_9501,
  0x6980_98D8,
  0x8B44_F7AF,
  0xFFFF_5BB1,
  0x895C_D7BE,
  0x6B90_1122,
  0xFD98_7193,
  0xA679_438E,
  0x49B4_0821,
  0xF61E_2562,
  0xC040_B340,
  0x265E_5A51,
  0xE9B6_C7AA,
  0xD62F_105D,
  0x0244_1453,
  0xD8A1_E681,
  0xE7D3_FBC8,
  0x21E1_CDE6,
  0xC337_07D6,
  0xF4D5_0D87,
  0x455A_14ED,
  0xA9E3_E905,
  0xFCEF_A3F8,
  0x676F_02D9,
  0x8D2A_4C8A,
  0xFFFA_3942,
  0x8771_F681,
  0x6D9D_6122,
  0xFDE5_380C,
  0xA4BE_EA44,
  0x4BDE_CFA9,
  0xF6BB_4B60,
  0xBEBF_BC70,
  0x289B_7EC6,
  0xEAA1_27FA,
  0xD4EF_3085,
  0x0488_1D05,
  0xD9D4_D039,
  0xE6DB_99E5,
  0x1FA2_7CF8,
  0xC4AC_5665,
  0xF429_2244,
  0x432A_FF97,
  0xAB94_23A7,
  0xFC93_A039,
  0x655B_59C3,
  0x8F0C_CC92,
  0xFFEF_F47D,
  0x8584_5DD1,
  0x6FA8_7E4F,
  0xFE2C_E6E0,
  0xA301_4314,
  0x4E08_11A1,
  0xF753_7E82,
  0xBD3A_F235,
  0x2AD7_D2BB,
  0xEB86_D391,
];

/// The digest of `bytes`, as four words.
pub fn digest(bytes: &[u8]) -> [u32; IC_MD5_WORDS] {
  let mut state: [u32; IC_MD5_WORDS] =
    [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476];
  let whole = bytes.len() / IC_MD5_BLOCK;
  let mut i: usize = 0;
  while i < whole {
    let at = i * IC_MD5_BLOCK;
    let mut block: [u8; IC_MD5_BLOCK] = [0; IC_MD5_BLOCK];
    block.copy_from_slice(&bytes[at..at + IC_MD5_BLOCK]);
    round(&mut state, &block);
    i += 1;
  }
  // What is left, the 0x80 byte that ends the message, and the length
  // in bits at the end of the last block.
  let rest = &bytes[whole * IC_MD5_BLOCK..];
  let mut tail: [u8; 2 * IC_MD5_BLOCK] = [0; 2 * IC_MD5_BLOCK];
  tail[..rest.len()].copy_from_slice(rest);
  tail[rest.len()] = 0x80;
  let mut blocks: usize = 1;
  if rest.len() + 1 + 8 > IC_MD5_BLOCK {
    blocks = 2;
  }
  let bits = (bytes.len() as u64).wrapping_mul(8);
  let end = blocks * IC_MD5_BLOCK - 8;
  tail[end..end + 8].copy_from_slice(&bits.to_le_bytes());
  let mut b: usize = 0;
  while b < blocks {
    let mut block: [u8; IC_MD5_BLOCK] = [0; IC_MD5_BLOCK];
    block.copy_from_slice(&tail[b * IC_MD5_BLOCK..(b + 1) * IC_MD5_BLOCK]);
    round(&mut state, &block);
    b += 1;
  }
  state
}

/// One block of 64 bytes into the state.
fn round(state: &mut [u32; IC_MD5_WORDS], block: &[u8; IC_MD5_BLOCK]) {
  let mut words: [u32; 16] = [0; 16];
  let mut i: usize = 0;
  while i < 16 {
    let at = 4 * i;
    words[i] = u32::from_le_bytes([
      block[at],
      block[at + 1],
      block[at + 2],
      block[at + 3],
    ]);
    i += 1;
  }
  let mut a = state[0];
  let mut b = state[1];
  let mut c = state[2];
  let mut d = state[3];
  let mut step: usize = 0;
  while step < 64 {
    let (mixed, which) = match step / 16 {
      0 => ((b & c) | (!b & d), step),
      1 => ((d & b) | (!d & c), (5 * step + 1) % 16),
      2 => (b ^ c ^ d, (3 * step + 5) % 16),
      _ => (c ^ (b | !d), (7 * step) % 16),
    };
    let sum = a
      .wrapping_add(mixed)
      .wrapping_add(IC_MD5_SINE[step])
      .wrapping_add(words[which]);
    a = d;
    d = c;
    c = b;
    b = b.wrapping_add(sum.rotate_left(IC_MD5_SHIFTS[step]));
    step += 1;
  }
  state[0] = state[0].wrapping_add(a);
  state[1] = state[1].wrapping_add(b);
  state[2] = state[2].wrapping_add(c);
  state[3] = state[3].wrapping_add(d);
}

/// The digest as the sixteen bytes it is usually written as.
pub fn digest_bytes(bytes: &[u8]) -> [u8; 16] {
  let words = digest(bytes);
  let mut out: [u8; 16] = [0; 16];
  let mut i: usize = 0;
  while i < IC_MD5_WORDS {
    out[4 * i..4 * i + 4].copy_from_slice(&words[i].to_le_bytes());
    i += 1;
  }
  out
}

#[cfg(test)]
mod tests {
  use super::*;

  fn hex(bytes: &[u8]) -> String {
    let mut text = String::new();
    for byte in digest_bytes(bytes) {
      text.push_str(&format!("{:02x}", byte));
    }
    text
  }

  #[test]
  fn the_digests_of_the_algorithms_own_examples() {
    assert_eq!(hex(b""), "d41d8cd98f00b204e9800998ecf8427e");
    assert_eq!(hex(b"a"), "0cc175b9c0f1b6a831c399e269772661");
    assert_eq!(hex(b"abc"), "900150983cd24fb0d6963f7d28e17f72");
    assert_eq!(hex(b"message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    assert_eq!(
      hex(b"abcdefghijklmnopqrstuvwxyz"),
      "c3fcd3d76192e4007dfb496cca67e13b"
    );
    assert_eq!(
      hex(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
      "d174ab98d277d9f5a5611c2c9f419d9f"
    );
    let digits = "1234567890".repeat(8);
    assert_eq!(hex(digits.as_bytes()), "57edf4a22be3c955ac49da2e2107b67a");
  }

  #[test]
  fn a_message_that_fills_its_last_block_takes_another() {
    // 55 bytes leave room for the end byte and the length; 56 do not,
    // so another block follows.
    assert_eq!(hex(&[b'x'; 55]), "04364420e25c512fd958a70738aa8f72");
    assert_eq!(hex(&[b'x'; 56]), "668a72d5ba17f08e62dabcafad6db14b");
    assert_eq!(hex(&[b'x'; 64]), "c1bb4f81d892b2d57947682aeb252456");
    assert_eq!(hex(&[b'x'; 120]), "fb98667f98096de92620b64f46e1c5b5");
  }

  #[test]
  fn the_words_are_the_bytes_in_native_order() {
    let words = digest(b"abc");
    let bytes = digest_bytes(b"abc");
    assert_eq!(words[0].to_le_bytes(), bytes[0..4]);
    assert_eq!(words[1].to_le_bytes(), bytes[4..8]);
  }
}
