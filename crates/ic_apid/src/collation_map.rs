// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! A collation made usable for placing keys: what a data node said of
//! it (chapter 05 §8.2) turned into the transform the data nodes put a
//! character key column through before they hash a key (chapter 05
//! §16.2). New: the C placed no key with a character column.
//!
//! **A value's characters** come from its bytes by the character set's
//! encoding: UTF-8 for utf8mb4 and utf8mb3, UTF-16 big- or
//! little-endian for utf16 and utf16le, UCS-2 and UTF-32 big-endian, and
//! for a set of one byte a character the table the weight runs give.
//! Other sets are not decoded, and their keys are placed in turn.
//!
//! **A string's weights** are its characters' weight strings in order,
//! a contraction's in place of the characters it joins: the longest
//! contraction starting at a character, and failing that a pair whose
//! second character is weighed by the first, such as a Japanese kana
//! and the prolonged sound mark after it. Either kind's weight string
//! is the whole sequence's, the first character's own weights included
//! (seen live, 2026-10-12).
//!
//! **What a column puts into the key** depends on the collation:
//!
//! - pad space: the weight strings run together as a sort key, and
//!   padded with the sort key of a single space, repeated, to the
//!   collation's sort-key growth times the column's largest length in
//!   bytes, or cut to that;
//! - no pad, utf8mb4_0900_bin aside: eight bytes, the little-endian form
//!   of 64-bit FNV-1a over the weights, a 16-bit weight at a time, level
//!   by level with a zero weight between levels, zero weights within a
//!   level skipped, a CHAR's trailing spaces stripped first. The first
//!   three levels the collation compares are always hashed, separators
//!   and all; a fourth, utf8mb4_ja_0900_as_cs_ks's kana level, only
//!   when the string has a weight there, as its sort key has it (seen
//!   live, 2026-10-12);
//! - utf8mb4_0900_bin: eight bytes, a fold of the value's bytes, its
//!   trailing spaces stripped first.
//!
//! The collations that weigh a character by its neighbours (latin2's
//! and cp1250's Czech ones, tis620's Thai one) cannot be rebuilt from
//! per-character weights, and are refused.
//!
//! Verify: chapter 05 §16.2, §8.2.

use std::collections::BTreeMap;

use ic_port::err;
use ic_port::IcError;
use ic_signals::collation_desc;
use ic_signals::collation_desc::Contraction;
use ic_signals::collation_desc::WeightRun;

use crate::collation_client::Collation;

/// The collations whose weights depend on a character's neighbours.
const IC_NEIGHBOUR_COLLATIONS: [&str; 3] =
  ["latin2_czech_cs", "cp1250_czech_cs", "tis620_thai_ci"];

/// The levels hashed, separators and all, whether or not they hold a
/// weight; a level past these only when it does.
const IC_LEVELS_ALWAYS_HASHED: usize = 3;

/// 64-bit FNV-1a's starting value.
const IC_FNV_OFFSET_BASIS: u64 = 14_695_981_039_346_656_037;
/// 64-bit FNV-1a's multiplier.
const IC_FNV_PRIME: u64 = 1_099_511_628_211;

/// How a key column's value is put into the key that is hashed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyTransform {
  /// The sort key, padded with the space's: a pad-space collation.
  SortKey,
  /// 64-bit FNV-1a over the weights: a no-pad collation.
  WeightHash,
  /// The bytes folded: utf8mb4_0900_bin.
  ByteFold,
}

/// How a value's bytes are taken as characters.
#[derive(Debug)]
enum Decoder {
  Utf8,
  Utf16Big,
  Utf16Little,
  Ucs2,
  Utf32,
  /// The code point of each byte, or `u32::MAX` for one the set does
  /// not decode.
  SingleByte(Vec<u32>),
}

/// A collation as placing a key uses it.
#[derive(Debug)]
pub struct CollationMap {
  number: u32,
  name: String,
  transform: KeyTransform,
  decoder: Decoder,
  levels: u32,
  sort_key_growth: u32,
  /// The weight runs, by first code point.
  runs: Vec<WeightRun>,
  contractions: Vec<Contraction>,
  /// By first code point, the contractions that start there, longest
  /// first.
  forward: BTreeMap<u32, Vec<usize>>,
  /// By second code point, the pairs that weigh it by the one before.
  previous: BTreeMap<u32, Vec<usize>>,
  /// The encoding of a space, which a CHAR's trailing ones are.
  space: Vec<u8>,
  /// The sort key of a string of one space.
  space_key: Vec<u8>,
}

impl CollationMap {
  /// Build the map from a collation described with its weights, or
  /// refuse one whose keys cannot be placed from them.
  pub fn new(collation: &Collation) -> Result<CollationMap, IcError> {
    let desc = &collation.desc;
    let refused = IcError::new(err::IC_ERROR_NOT_SUPPORTED);
    if IC_NEIGHBOUR_COLLATIONS.contains(&desc.collation_name.as_str()) {
      return Err(refused);
    }
    let no_pad = desc.is_no_pad();
    let binary = (desc.flags & collation_desc::IC_COLLATION_BINARY) != 0;
    let transform = match (no_pad, binary) {
      (true, true) => KeyTransform::ByteFold,
      (true, false) => KeyTransform::WeightHash,
      _ => KeyTransform::SortKey,
    };
    if transform == KeyTransform::SortKey && desc.sort_key_growth == 0 {
      return Err(refused);
    }
    let mut runs = collation.runs.clone();
    runs.sort_by_key(|run| run.first);
    let decoder = decoder_for(desc.charset_name.as_str(), desc, &runs)?;
    let mut map = CollationMap {
      number: desc.number,
      name: desc.collation_name.clone(),
      transform,
      decoder,
      levels: desc.weight_levels.max(1),
      sort_key_growth: desc.sort_key_growth,
      runs,
      contractions: collation.contractions.clone(),
      forward: BTreeMap::new(),
      previous: BTreeMap::new(),
      space: Vec::new(),
      space_key: Vec::new(),
    };
    for (i, contraction) in map.contractions.iter().enumerate() {
      let points = &contraction.code_points;
      if points.len() < 2 {
        continue;
      }
      if contraction.previous_context {
        map.previous.entry(points[1]).or_default().push(i);
      } else {
        map.forward.entry(points[0]).or_default().push(i);
      }
    }
    let contractions = &map.contractions;
    for list in map.forward.values_mut() {
      list
        .sort_by_key(|i| std::cmp::Reverse(contractions[*i].code_points.len()));
    }
    map.space = map.encode_space()?;
    if map.transform == KeyTransform::SortKey {
      let mut space_key: Vec<u8> = Vec::new();
      map.weights_of(&[0x20], &mut |w| space_key.extend_from_slice(w))?;
      if space_key.is_empty() {
        return Err(refused);
      }
      map.space_key = space_key;
    }
    Ok(map)
  }

  /// The collation's number, as a column's description names it.
  pub fn number(&self) -> u32 {
    self.number
  }

  /// The collation's name.
  pub fn name(&self) -> &str {
    &self.name
  }

  /// How a key column in this collation is put into the hashed key.
  pub fn transform(&self) -> KeyTransform {
    self.transform
  }

  /// Append to `out` what a key column's value contributes to the key
  /// that is hashed, before its padding to a word: `value` is a CHAR's
  /// whole field or a VARCHAR's bytes after its length, `max_len` the
  /// column's largest length in bytes, `fixed` true for a CHAR.
  pub fn key_part(
    &self,
    value: &[u8],
    max_len: usize,
    fixed: bool,
    out: &mut Vec<u8>,
  ) -> Result<(), IcError> {
    match self.transform {
      KeyTransform::SortKey => {
        let start = out.len();
        let want = self.sort_key_growth as usize * max_len;
        let points = self.decode(value)?;
        self.weights_of(&points, &mut |w| out.extend_from_slice(w))?;
        let made = out.len() - start;
        if made >= want {
          out.truncate(start + want);
          return Ok(());
        }
        let mut i: usize = 0;
        while out.len() - start < want {
          out.push(self.space_key[i % self.space_key.len()]);
          i += 1;
        }
        Ok(())
      }
      KeyTransform::WeightHash => {
        let mut value = value;
        if fixed {
          value = self.strip_spaces(value);
        }
        let points = self.decode(value)?;
        let hash = self.weight_hash(&points)?;
        out.extend_from_slice(&hash.to_le_bytes());
        Ok(())
      }
      KeyTransform::ByteFold => {
        let value = self.strip_spaces(value);
        out.extend_from_slice(&byte_fold(value).to_le_bytes());
        Ok(())
      }
    }
  }

  /// 64-bit FNV-1a over the weights of the characters, level by level,
  /// a zero weight between levels, zero weights within a level skipped.
  fn weight_hash(&self, points: &[u32]) -> Result<u64, IcError> {
    let levels = self.levels as usize;
    let mut by_level: Vec<Vec<u16>> = vec![Vec::new(); levels];
    self.weights_of(points, &mut |weights| {
      let mut level: usize = 0;
      for pair in weights.chunks(2) {
        if pair.len() < 2 {
          break;
        }
        let weight = u16::from_be_bytes([pair[0], pair[1]]);
        if weight == 0 {
          level += 1;
        } else if level < levels {
          by_level[level].push(weight);
        }
      }
    })?;
    let mut hash = IC_FNV_OFFSET_BASIS;
    for (level, weights) in by_level.iter().enumerate() {
      if level >= IC_LEVELS_ALWAYS_HASHED && weights.is_empty() {
        // A level past the third counts only where it has weights.
        break;
      }
      if level > 0 {
        hash = fnv_step(hash, 0);
      }
      for weight in weights {
        hash = fnv_step(hash, *weight);
      }
    }
    Ok(hash)
  }

  /// Give `take` the weight string of each character of `points` in
  /// turn, a contraction's in place of the characters it joins.
  fn weights_of(
    &self,
    points: &[u32],
    take: &mut dyn FnMut(&[u8]),
  ) -> Result<(), IcError> {
    let mut buf: Vec<u8> = Vec::new();
    let mut i: usize = 0;
    'next: while i < points.len() {
      if let Some(list) = self.forward.get(&points[i]) {
        for at in list {
          let contraction = &self.contractions[*at];
          let n = contraction.code_points.len();
          if i + n <= points.len()
            && points[i..i + n] == contraction.code_points[..]
          {
            take(&contraction.weight);
            i += n;
            continue 'next;
          }
        }
      }
      // A pair whose second is weighed by this one: its weight string
      // is the pair's, this character's own weights included.
      if i + 1 < points.len() {
        if let Some(list) = self.previous.get(&points[i + 1]) {
          for at in list {
            let contraction = &self.contractions[*at];
            if contraction.code_points[0] == points[i] {
              take(&contraction.weight);
              i += 2;
              continue 'next;
            }
          }
        }
      }
      self.weight_into(points[i], &mut buf)?;
      take(&buf);
      i += 1;
    }
    Ok(())
  }

  /// The weight string of one code point, or a refusal for one the set
  /// does not have.
  fn weight_into(&self, point: u32, out: &mut Vec<u8>) -> Result<(), IcError> {
    let at = self.runs.partition_point(|run| run.first <= point);
    if at > 0 {
      let run = &self.runs[at - 1];
      if point - run.first < run.count {
        run.weight_of(point - run.first, out);
        return Ok(());
      }
    }
    Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED))
  }

  /// The code points of a value's bytes.
  fn decode(&self, bytes: &[u8]) -> Result<Vec<u32>, IcError> {
    let bad = IcError::new(err::IC_ERROR_NOT_SUPPORTED);
    let mut points: Vec<u32> = Vec::with_capacity(bytes.len());
    match &self.decoder {
      Decoder::SingleByte(table) => {
        for b in bytes {
          let point = table[*b as usize];
          if point == u32::MAX {
            return Err(bad);
          }
          points.push(point);
        }
      }
      Decoder::Utf8 => match std::str::from_utf8(bytes) {
        Ok(text) => points.extend(text.chars().map(|c| c as u32)),
        Err(_) => return Err(bad),
      },
      Decoder::Utf16Big | Decoder::Utf16Little | Decoder::Ucs2 => {
        if bytes.len() % 2 != 0 {
          return Err(bad);
        }
        let little = matches!(self.decoder, Decoder::Utf16Little);
        let units = bytes.chunks(2).map(|pair| {
          if little {
            u16::from_le_bytes([pair[0], pair[1]])
          } else {
            u16::from_be_bytes([pair[0], pair[1]])
          }
        });
        if matches!(self.decoder, Decoder::Ucs2) {
          points.extend(units.map(|unit| unit as u32));
        } else {
          for unit in char::decode_utf16(units) {
            match unit {
              Ok(c) => points.push(c as u32),
              Err(_) => return Err(bad),
            }
          }
        }
      }
      Decoder::Utf32 => {
        if bytes.len() % 4 != 0 {
          return Err(bad);
        }
        for quad in bytes.chunks(4) {
          points.push(u32::from_be_bytes([quad[0], quad[1], quad[2], quad[3]]));
        }
      }
    }
    Ok(points)
  }

  /// A value without its trailing spaces.
  fn strip_spaces<'a>(&self, value: &'a [u8]) -> &'a [u8] {
    let n = self.space.len();
    let mut end = value.len();
    while n > 0 && end >= n && value[end - n..end] == self.space[..] {
      end -= n;
    }
    &value[..end]
  }

  /// A space in the set's encoding.
  fn encode_space(&self) -> Result<Vec<u8>, IcError> {
    let space: Vec<u8> = match &self.decoder {
      Decoder::Utf8 => vec![0x20],
      Decoder::Utf16Big | Decoder::Ucs2 => vec![0x00, 0x20],
      Decoder::Utf16Little => vec![0x20, 0x00],
      Decoder::Utf32 => vec![0x00, 0x00, 0x00, 0x20],
      Decoder::SingleByte(table) => match table.iter().position(|p| *p == 0x20)
      {
        Some(byte) => vec![byte as u8],
        None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
      },
    };
    Ok(space)
  }
}

/// How the set named encodes its characters.
fn decoder_for(
  charset: &str,
  desc: &collation_desc::CollationDescConf,
  runs: &[WeightRun],
) -> Result<Decoder, IcError> {
  let unicode = (desc.flags & collation_desc::IC_COLLATION_UNICODE) != 0;
  if unicode {
    return match charset {
      "utf8mb4" | "utf8mb3" | "utf8" => Ok(Decoder::Utf8),
      "utf16" => Ok(Decoder::Utf16Big),
      "utf16le" => Ok(Decoder::Utf16Little),
      "ucs2" => Ok(Decoder::Ucs2),
      "utf32" => Ok(Decoder::Utf32),
      _ => Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    };
  }
  if desc.min_char_bytes != 1 || desc.max_char_bytes != 1 {
    // A set of several bytes a character, not Unicode: not decoded.
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  let mut table: Vec<u32> = vec![u32::MAX; 256];
  let mut bytes: Vec<u8> = Vec::new();
  for run in runs {
    let mut k: u32 = 0;
    while k < run.count {
      run.encoded_of(k, &mut bytes);
      if bytes.len() == 1 {
        table[bytes[0] as usize] = run.first + k;
      }
      k += 1;
    }
  }
  Ok(Decoder::SingleByte(table))
}

/// One step of 64-bit FNV-1a: the unit XORed in, then the product.
fn fnv_step(hash: u64, unit: u16) -> u64 {
  (hash ^ unit as u64).wrapping_mul(IC_FNV_PRIME)
}

/// The fold utf8mb4_0900_bin hashes its bytes with: for each byte, the
/// first number XORed with ((its low six bits + the second) times the
/// byte, plus itself shifted left eight), and the second grown by
/// three; both start at 0, the second being RonDB's seed.
fn byte_fold(bytes: &[u8]) -> u64 {
  let mut n1: u64 = 0;
  let mut n2: u64 = 0;
  for b in bytes {
    let factor = (n1 & 63).wrapping_add(n2);
    n1 ^= factor.wrapping_mul(*b as u64).wrapping_add(n1 << 8);
    n2 = n2.wrapping_add(3);
  }
  n1
}

#[cfg(test)]
mod tests {
  use super::*;
  use ic_signals::collation_desc::CollationDescConf;
  use ic_signals::collation_desc::IC_COLLATION_NO_STEP;

  /// A latin1-like collation: every byte its own code point, the
  /// letters weighing as their capitals, pad space, one byte of sort
  /// key a byte.
  fn simple() -> Collation {
    let mut runs: Vec<WeightRun> = Vec::new();
    for b in 0u32..256 {
      let mut weight = b as u8;
      if (0x61..=0x7A).contains(&b) {
        weight -= 0x20;
      }
      runs.push(WeightRun {
        first: b,
        count: 1,
        step_offset: IC_COLLATION_NO_STEP,
        encoded: vec![b as u8],
        weight: vec![weight],
      });
    }
    Collation {
      desc: CollationDescConf {
        number: 8,
        collation_name: "test_ci".to_string(),
        charset_name: "latin1".to_string(),
        min_char_bytes: 1,
        max_char_bytes: 1,
        sort_key_growth: 1,
        weight_levels: 1,
        ..CollationDescConf::default()
      },
      runs,
      ..Collation::default()
    }
  }

  /// A Unicode no-pad collation over ASCII with two levels: a letter's
  /// first level is its capital, its second 1 for a capital and 2 for a
  /// small letter, and "ch" is a contraction.
  fn two_levels(flags: u32) -> Collation {
    let mut runs: Vec<WeightRun> = Vec::new();
    for c in 0x20u32..0x7F {
      let upper = if (0x61..=0x7A).contains(&c) {
        c - 0x20
      } else {
        c
      };
      let case = if (0x61..=0x7A).contains(&c) {
        2u16
      } else {
        1u16
      };
      let mut weight: Vec<u8> = (upper as u16).to_be_bytes().to_vec();
      weight.extend_from_slice(&[0, 0]);
      weight.extend_from_slice(&case.to_be_bytes());
      runs.push(WeightRun {
        first: c,
        count: 1,
        step_offset: IC_COLLATION_NO_STEP,
        encoded: Vec::new(),
        weight,
      });
    }
    let contraction = Contraction {
      code_points: vec![0x63, 0x68],
      previous_context: false,
      weight: vec![0x01, 0x00, 0x00, 0x00, 0x00, 0x02],
    };
    Collation {
      desc: CollationDescConf {
        number: 255,
        collation_name: "test_0900".to_string(),
        charset_name: "utf8mb4".to_string(),
        flags: collation_desc::IC_COLLATION_UNICODE
          | collation_desc::IC_COLLATION_NO_PAD
          | flags,
        min_char_bytes: 1,
        max_char_bytes: 4,
        weight_levels: 2,
        ..CollationDescConf::default()
      },
      runs,
      contractions: vec![contraction],
      ..Collation::default()
    }
  }

  fn part(
    map: &CollationMap,
    value: &[u8],
    max: usize,
    fixed: bool,
  ) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    map
      .key_part(value, max, fixed, &mut out)
      .expect("a key part");
    out
  }

  #[test]
  fn a_pad_space_key_is_the_sort_key_padded_with_the_spaces() {
    let map = CollationMap::new(&simple()).expect("a map");
    assert_eq!(map.transform(), KeyTransform::SortKey);
    assert_eq!(part(&map, b"ab", 5, false), b"AB   ".to_vec());
    // Case and trailing spaces make no difference.
    assert_eq!(part(&map, b"Ab ", 5, false), part(&map, b"aB", 5, false));
    // A key as long as the column is cut to it, not padded.
    assert_eq!(part(&map, b"abcdef", 5, false), b"ABCDE".to_vec());
  }

  #[test]
  fn a_no_pad_key_is_fnv_over_the_levels() {
    let map = CollationMap::new(&two_levels(0)).expect("a map");
    assert_eq!(map.transform(), KeyTransform::WeightHash);
    // "ab": level one A and B, a zero, level two 2 and 2.
    let mut want = IC_FNV_OFFSET_BASIS;
    for unit in [0x41u16, 0x42, 0, 2, 2] {
      want = fnv_step(want, unit);
    }
    assert_eq!(part(&map, b"ab", 20, false), want.to_le_bytes().to_vec());
    // A CHAR's trailing spaces go; a VARCHAR's count.
    assert_eq!(part(&map, b"ab  ", 20, true), part(&map, b"ab", 20, false));
    assert_ne!(part(&map, b"ab ", 20, false), part(&map, b"ab", 20, false));
    // The contraction weighs "ch" as one: 0x0100 then case 2.
    let mut want = IC_FNV_OFFSET_BASIS;
    for unit in [0x0100u16, 0, 2] {
      want = fnv_step(want, unit);
    }
    assert_eq!(part(&map, b"ch", 20, false), want.to_le_bytes().to_vec());
  }

  #[test]
  fn a_pair_weighed_together_replaces_both_characters() {
    let mut c = two_levels(0);
    // "k-" weighed as one: the k's own weight and the mark's in the
    // context of k, then the second level.
    c.contractions.push(Contraction {
      code_points: vec![0x6B, 0x2D],
      previous_context: true,
      weight: vec![0x00, 0x4B, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03],
    });
    let map = CollationMap::new(&c).expect("a map");
    let mut want = IC_FNV_OFFSET_BASIS;
    for unit in [0x4Bu16, 0x0200, 0, 2, 3] {
      want = fnv_step(want, unit);
    }
    assert_eq!(part(&map, b"k-", 20, false), want.to_le_bytes().to_vec());
    // A mark after another letter is weighed alone.
    let mut want = IC_FNV_OFFSET_BASIS;
    for unit in [0x41u16, 0x2D, 0, 2, 1] {
      want = fnv_step(want, unit);
    }
    assert_eq!(part(&map, b"a-", 20, false), want.to_le_bytes().to_vec());
  }

  #[test]
  fn a_fourth_level_counts_only_where_it_has_weights() {
    let mut c = two_levels(0);
    c.desc.weight_levels = 4;
    // k alone has a fourth level, 8, behind three separators; the other
    // characters stop at their second.
    for run in &mut c.runs {
      if run.first == 0x6B {
        run.weight = vec![
          0x00, 0x4B, 0, 0, 0x00, 0x02, 0, 0, 0x00, 0x05, 0, 0, 0x00, 0x08,
        ];
      }
    }
    let map = CollationMap::new(&c).expect("a map");
    // Without a fourth-level weight: three levels, two separators.
    let mut want = IC_FNV_OFFSET_BASIS;
    for unit in [0x41u16, 0, 2, 0] {
      want = fnv_step(want, unit);
    }
    assert_eq!(part(&map, b"a", 20, false), want.to_le_bytes().to_vec());
    let mut empty = IC_FNV_OFFSET_BASIS;
    for unit in [0u16, 0] {
      empty = fnv_step(empty, unit);
    }
    assert_eq!(part(&map, b"", 20, false), empty.to_le_bytes().to_vec());
    // With one: the fourth level and its separator too.
    let mut want = IC_FNV_OFFSET_BASIS;
    for unit in [0x4Bu16, 0, 2, 0, 5, 0, 8] {
      want = fnv_step(want, unit);
    }
    assert_eq!(part(&map, b"k", 20, false), want.to_le_bytes().to_vec());
  }

  #[test]
  fn the_binary_no_pad_key_folds_its_bytes() {
    let map =
      CollationMap::new(&two_levels(collation_desc::IC_COLLATION_BINARY))
        .expect("a map");
    assert_eq!(map.transform(), KeyTransform::ByteFold);
    // Trailing spaces go, in a VARCHAR too.
    assert_eq!(part(&map, b"ab  ", 20, false), part(&map, b"ab", 20, false));
    // One byte: n1 = 0 ^ ((0 + 0) * b + 0) = 0; two: the second byte
    // multiplies (0 + 3).
    assert_eq!(byte_fold(b"a"), 0);
    assert_eq!(byte_fold(b"ab"), 3 * 0x62);
  }

  #[test]
  fn neighbour_collations_are_refused() {
    let mut czech = simple();
    czech.desc.collation_name = "latin2_czech_cs".to_string();
    assert!(CollationMap::new(&czech).is_err());
  }

  #[test]
  fn sixteen_bit_encodings_decode() {
    let mut c = two_levels(0);
    c.desc.charset_name = "utf16".to_string();
    let map = CollationMap::new(&c).expect("a map");
    assert_eq!(
      map
        .decode(&[0x00, 0x61, 0xD8, 0x3D, 0xDE, 0x00])
        .expect("decoded"),
      vec![0x61, 0x1F600]
    );
    c.desc.charset_name = "utf16le".to_string();
    let map = CollationMap::new(&c).expect("a map");
    assert_eq!(map.decode(&[0x61, 0x00]).expect("decoded"), vec![0x61]);
    assert_eq!(map.strip_spaces(&[0x61, 0x00, 0x20, 0x00]), &[0x61, 0x00]);
  }
}
