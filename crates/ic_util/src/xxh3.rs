// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! XXH3, 64 bits, with no seed: the hash a RonDB table made by a
//! version new enough places its keys by. New here, written from the
//! algorithm, which is public; the data nodes carry its author's own
//! implementation.
//!
//! The result has to match theirs bit for bit, or a key would be looked
//! for at the wrong node, so the tests pin every path of it against the
//! algorithm's published values.
//!
//! This is the plain form. The data nodes take a vector path for long
//! keys; a distribution key is short, and the paths that matter here
//! run no loop at all. See doc/rust/06-phases.md on measuring it.

/// The bytes the algorithm mixes its input with.
const IC_XXH3_SECRET: [u8; 192] = [
  0xB8, 0xFE, 0x6C, 0x39, 0x23, 0xA4, 0x4B, 0xBE, 0x7C, 0x01, 0x81, 0x2C, 0xF7,
  0x21, 0xAD, 0x1C, 0xDE, 0xD4, 0x6D, 0xE9, 0x83, 0x90, 0x97, 0xDB, 0x72, 0x40,
  0xA4, 0xA4, 0xB7, 0xB3, 0x67, 0x1F, 0xCB, 0x79, 0xE6, 0x4E, 0xCC, 0xC0, 0xE5,
  0x78, 0x82, 0x5A, 0xD0, 0x7D, 0xCC, 0xFF, 0x72, 0x21, 0xB8, 0x08, 0x46, 0x74,
  0xF7, 0x43, 0x24, 0x8E, 0xE0, 0x35, 0x90, 0xE6, 0x81, 0x3A, 0x26, 0x4C, 0x3C,
  0x28, 0x52, 0xBB, 0x91, 0xC3, 0x00, 0xCB, 0x88, 0xD0, 0x65, 0x8B, 0x1B, 0x53,
  0x2E, 0xA3, 0x71, 0x64, 0x48, 0x97, 0xA2, 0x0D, 0xF9, 0x4E, 0x38, 0x19, 0xEF,
  0x46, 0xA9, 0xDE, 0xAC, 0xD8, 0xA8, 0xFA, 0x76, 0x3F, 0xE3, 0x9C, 0x34, 0x3F,
  0xF9, 0xDC, 0xBB, 0xC7, 0xC7, 0x0B, 0x4F, 0x1D, 0x8A, 0x51, 0xE0, 0x4B, 0xCD,
  0xB4, 0x59, 0x31, 0xC8, 0x9F, 0x7E, 0xC9, 0xD9, 0x78, 0x73, 0x64, 0xEA, 0xC5,
  0xAC, 0x83, 0x34, 0xD3, 0xEB, 0xC3, 0xC5, 0x81, 0xA0, 0xFF, 0xFA, 0x13, 0x63,
  0xEB, 0x17, 0x0D, 0xDD, 0x51, 0xB7, 0xF0, 0xDA, 0x49, 0xD3, 0x16, 0x55, 0x26,
  0x29, 0xD4, 0x68, 0x9E, 0x2B, 0x16, 0xBE, 0x58, 0x7D, 0x47, 0xA1, 0xFC, 0x8F,
  0xF8, 0xB8, 0xD1, 0x7A, 0xD0, 0x31, 0xCE, 0x45, 0xCB, 0x3A, 0x8F, 0x95, 0x16,
  0x04, 0x28, 0xAF, 0xD7, 0xFB, 0xCA, 0xBB, 0x4B, 0x40, 0x7E,
];

const IC_PRIME32_1: u64 = 0x9E37_79B1;
const IC_PRIME32_2: u64 = 0x85EB_CA77;
const IC_PRIME32_3: u64 = 0xC2B2_AE3D;
const IC_PRIME64_1: u64 = 0x9E37_79B1_85EB_CA87;
const IC_PRIME64_2: u64 = 0xC2B2_AE3D_27D4_EB4F;
const IC_PRIME64_3: u64 = 0x1656_67B1_9E37_79F9;
const IC_PRIME64_4: u64 = 0x85EB_CA77_C2B2_AE63;
const IC_PRIME64_5: u64 = 0x27D4_EB2F_1656_67C5;
/// The multipliers the mixing steps use.
const IC_PRIME_MX1: u64 = 0x1656_6791_9E37_79F9;
const IC_PRIME_MX2: u64 = 0x9FB2_1C65_1E98_DF25;

/// Bytes in a stripe, the unit the long path works in.
const IC_XXH3_STRIPE: usize = 64;
/// Above this many bytes the long path is taken.
const IC_XXH3_LONG: usize = 240;
/// How far before the end of the secret the last stripe reads.
const IC_XXH3_LAST_ACC_START: usize = 7;
/// Where the accumulators are merged from.
const IC_XXH3_MERGE_START: usize = 11;
/// Where the rounds past the eighth read the secret from.
const IC_XXH3_MID_START: usize = 3;
/// Where the last round of a middle-sized key reads the secret.
const IC_XXH3_MID_LAST: usize = 136 - 17;

/// The hash of `bytes`.
pub fn hash(bytes: &[u8]) -> u64 {
  let n = bytes.len();
  if n == 0 {
    return avalanche64(secret64(56) ^ secret64(64));
  }
  if n <= 3 {
    return short_hash(bytes);
  }
  if n <= 8 {
    let first = read32(bytes, 0) as u64;
    let last = read32(bytes, n - 4) as u64;
    let bitflip = secret64(8) ^ secret64(16);
    return rrmxmx(((first << 32) + last) ^ bitflip, n as u64);
  }
  if n <= 16 {
    let low = read64(bytes, 0) ^ (secret64(24) ^ secret64(32));
    let high = read64(bytes, n - 8) ^ (secret64(40) ^ secret64(48));
    let acc = (n as u64)
      .wrapping_add(low.swap_bytes())
      .wrapping_add(high)
      .wrapping_add(fold(low, high));
    return avalanche(acc);
  }
  if n <= 128 {
    return middle_hash(bytes);
  }
  if n <= IC_XXH3_LONG {
    return larger_hash(bytes);
  }
  long_hash(bytes)
}

/// One, two or three bytes, gathered into one word.
fn short_hash(bytes: &[u8]) -> u64 {
  let n = bytes.len();
  let combined = ((bytes[0] as u32) << 16)
    | ((bytes[n >> 1] as u32) << 24)
    | (bytes[n - 1] as u32)
    | ((n as u32) << 8);
  let bitflip = (secret32(0) ^ secret32(4)) as u64;
  avalanche64(combined as u64 ^ bitflip)
}

/// Seventeen to 128 bytes: sixteen from each end at a time, inwards.
fn middle_hash(bytes: &[u8]) -> u64 {
  let n = bytes.len();
  let mut acc = (n as u64).wrapping_mul(IC_PRIME64_1);
  let mut i = (n - 1) / 32;
  while i > 0 {
    acc = acc.wrapping_add(mix16(bytes, 16 * i, 32 * i));
    acc = acc.wrapping_add(mix16(bytes, n - 16 * (i + 1), 32 * i + 16));
    i -= 1;
  }
  acc = acc.wrapping_add(mix16(bytes, 0, 0));
  acc = acc.wrapping_add(mix16(bytes, n - 16, 16));
  avalanche(acc)
}

/// 129 to 240 bytes: sixteen at a time, with a stir after eight rounds.
fn larger_hash(bytes: &[u8]) -> u64 {
  let n = bytes.len();
  let mut acc = (n as u64).wrapping_mul(IC_PRIME64_1);
  let mut i: usize = 0;
  while i < 8 {
    acc = acc.wrapping_add(mix16(bytes, 16 * i, 16 * i));
    i += 1;
  }
  acc = avalanche(acc);
  let rounds = n / 16;
  while i < rounds {
    let at_secret = 16 * (i - 8) + IC_XXH3_MID_START;
    acc = acc.wrapping_add(mix16(bytes, 16 * i, at_secret));
    i += 1;
  }
  acc = acc.wrapping_add(mix16(bytes, n - 16, IC_XXH3_MID_LAST));
  avalanche(acc)
}

/// Over 240 bytes: eight accumulators over stripes of 64 bytes, stirred
/// between blocks.
fn long_hash(bytes: &[u8]) -> u64 {
  let n = bytes.len();
  let mut acc: [u64; 8] = [
    IC_PRIME32_3,
    IC_PRIME64_1,
    IC_PRIME64_2,
    IC_PRIME64_3,
    IC_PRIME64_4,
    IC_PRIME32_2,
    IC_PRIME64_5,
    IC_PRIME32_1,
  ];
  let stripes_per_block = (IC_XXH3_SECRET.len() - IC_XXH3_STRIPE) / 8;
  let block = IC_XXH3_STRIPE * stripes_per_block;
  let blocks = (n - 1) / block;
  let mut b: usize = 0;
  while b < blocks {
    let mut s: usize = 0;
    while s < stripes_per_block {
      accumulate(&mut acc, bytes, b * block + IC_XXH3_STRIPE * s, 8 * s);
      s += 1;
    }
    scramble(&mut acc, IC_XXH3_SECRET.len() - IC_XXH3_STRIPE);
    b += 1;
  }
  let stripes = ((n - 1) - block * blocks) / IC_XXH3_STRIPE;
  let mut s: usize = 0;
  while s < stripes {
    accumulate(&mut acc, bytes, blocks * block + IC_XXH3_STRIPE * s, 8 * s);
    s += 1;
  }
  // The last stripe is taken from the end, wherever the rest ended.
  let last_secret =
    IC_XXH3_SECRET.len() - IC_XXH3_STRIPE - IC_XXH3_LAST_ACC_START;
  accumulate(&mut acc, bytes, n - IC_XXH3_STRIPE, last_secret);
  let mut result = (n as u64).wrapping_mul(IC_PRIME64_1);
  let mut i: usize = 0;
  while i < 4 {
    let at = IC_XXH3_MERGE_START + 16 * i;
    let low = acc[2 * i] ^ secret64(at);
    let high = acc[2 * i + 1] ^ secret64(at + 8);
    result = result.wrapping_add(fold(low, high));
    i += 1;
  }
  avalanche(result)
}

/// One stripe of 64 bytes into the accumulators.
fn accumulate(acc: &mut [u64; 8], bytes: &[u8], at: usize, at_secret: usize) {
  let mut i: usize = 0;
  while i < 8 {
    let value = read64(bytes, at + 8 * i);
    let keyed = value ^ secret64(at_secret + 8 * i);
    acc[i ^ 1] = acc[i ^ 1].wrapping_add(value);
    let low = keyed & 0xFFFF_FFFF;
    acc[i] = acc[i].wrapping_add(low.wrapping_mul(keyed >> 32));
    i += 1;
  }
}

/// The accumulators stirred, between blocks.
fn scramble(acc: &mut [u64; 8], at_secret: usize) {
  let mut i: usize = 0;
  while i < 8 {
    let mut value = acc[i];
    value ^= value >> 47;
    value ^= secret64(at_secret + 8 * i);
    acc[i] = value.wrapping_mul(IC_PRIME32_1);
    i += 1;
  }
}

/// Sixteen bytes of input against sixteen of secret.
fn mix16(bytes: &[u8], at: usize, at_secret: usize) -> u64 {
  let low = read64(bytes, at) ^ secret64(at_secret);
  let high = read64(bytes, at + 8) ^ secret64(at_secret + 8);
  fold(low, high)
}

/// The halves of a 128-bit product, folded together.
fn fold(a: u64, b: u64) -> u64 {
  let product = (a as u128).wrapping_mul(b as u128);
  (product as u64) ^ ((product >> 64) as u64)
}

fn avalanche(mut h: u64) -> u64 {
  h ^= h >> 37;
  h = h.wrapping_mul(IC_PRIME_MX1);
  h ^ (h >> 32)
}

fn avalanche64(mut h: u64) -> u64 {
  h ^= h >> 33;
  h = h.wrapping_mul(IC_PRIME64_2);
  h ^= h >> 29;
  h = h.wrapping_mul(IC_PRIME64_3);
  h ^ (h >> 32)
}

fn rrmxmx(mut h: u64, len: u64) -> u64 {
  h ^= h.rotate_left(49) ^ h.rotate_left(24);
  h = h.wrapping_mul(IC_PRIME_MX2);
  h ^= (h >> 35).wrapping_add(len);
  h = h.wrapping_mul(IC_PRIME_MX2);
  h ^ (h >> 28)
}

fn read32(bytes: &[u8], at: usize) -> u32 {
  let mut four: [u8; 4] = [0; 4];
  four.copy_from_slice(&bytes[at..at + 4]);
  u32::from_le_bytes(four)
}

fn read64(bytes: &[u8], at: usize) -> u64 {
  let mut eight: [u8; 8] = [0; 8];
  eight.copy_from_slice(&bytes[at..at + 8]);
  u64::from_le_bytes(eight)
}

fn secret32(at: usize) -> u32 {
  read32(&IC_XXH3_SECRET, at)
}

fn secret64(at: usize) -> u64 {
  read64(&IC_XXH3_SECRET, at)
}

#[cfg(test)]
mod tests {
  use super::*;

  /// The bytes the algorithm's own checks hash: a run that wraps at a
  /// prime, so that it lines up with no block or stripe.
  fn sample(n: usize) -> Vec<u8> {
    let mut bytes: Vec<u8> = Vec::with_capacity(n);
    let mut i: usize = 0;
    while i < n {
      bytes.push((i % 251) as u8);
      i += 1;
    }
    bytes
  }

  /// A length and the hash of that many of those bytes, taken from the
  /// algorithm's published values. Every path is here, and both sides
  /// of every boundary between them.
  const IC_TEST_VECTORS: [(usize, u64); 37] = [
    (1, 0xC44B_DFF4_074E_ECDB),
    (2, 0xD664_5FC3_051A_9457),
    (3, 0x5F42_99FC_161C_9CBB),
    (4, 0x60DA_B036_A582_11F2),
    (5, 0xB075_753A_84CA_0FBE),
    (6, 0xA658_4D1D_9A6A_E704),
    (7, 0x0CD2_084A_6240_6B69),
    (8, 0x3A1C_2D7C_85AF_88F8),
    (9, 0xE961_2598_145B_B9DC),
    (10, 0xAB69_A08E_F83D_8F77),
    (11, 0x1CF3_96AA_4DE6_198D),
    (12, 0x5ACE_6A51_1C10_894B),
    (13, 0xB7A5_D8A8_309A_2CB9),
    (14, 0x4CF4_5C94_4A9A_2237),
    (15, 0x55EC_EDC2_B87B_B042),
    (16, 0x8355_E3A6_F617_70DB),
    (17, 0x9EF3_41A9_9DE3_7328),
    (18, 0xF691_2490_D4C0_EED5),
    (19, 0x60E7_2614_3CF5_0312),
    (31, 0x4F36_DB8E_4DF3_78FD),
    (32, 0x3523_581F_E96E_4C05),
    (33, 0xE68C_56BA_8899_1E58),
    (126, 0x6C2A_9EB7_459C_DC61),
    (127, 0x120B_9787_F842_5F2F),
    (128, 0x85C6_174C_7FF4_C46B),
    (129, 0xEC76_42B4_31BA_3E5A),
    (130, 0x4D32_24B1_0090_8A87),
    (131, 0xE57F_7EA6_741F_E3A0),
    (238, 0x3044_9A0B_4899_DEE9),
    (239, 0x972B_14E3_C46F_214B),
    (240, 0x375A_384D_957F_E865),
    (241, 0x02E8_CD95_421C_6D02),
    (242, 0xDDCB_33C4_9405_1832),
    (243, 0x8835_F952_9193_E3DC),
    (244, 0xBC17_C91E_C3CF_8D7F),
    (1024, 0xE5D7_8BAF_A45B_2AA5),
    (10240, 0xBCD6_3266_DF6E_2244),
  ];

  #[test]
  fn nothing_hashes_to_the_algorithms_own_value() {
    assert_eq!(hash(&[]), 0x2D06_8005_38D3_94C2);
  }

  #[test]
  fn every_path_matches_the_algorithm() {
    for (n, want) in IC_TEST_VECTORS {
      let got = hash(&sample(n));
      assert_eq!(got, want, "a key of {} bytes hashed to {:#x}", n, got);
    }
  }

  #[test]
  fn a_key_of_a_few_words_hashes_as_itself() {
    // What a distribution key looks like: a word or two.
    let one = hash(&7u32.to_le_bytes());
    assert_eq!(one, hash(&7u32.to_le_bytes()));
    assert_ne!(one, hash(&8u32.to_le_bytes()));
  }
}
