// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Asking a data node's dictionary what a collation is, and for its
//! weights.
//!
//! The request names a collation by its number, the number a column's
//! description carries, or by its name, or names a character set and
//! means that set's default collation. The answer gives the collation's
//! number and names and the properties a client needs to transform and
//! compare its strings: how many bytes a character takes, how much a
//! string grows when it is turned into a sort key, the pad byte, and
//! whether trailing spaces count.
//!
//! Asked with [`IC_COLLATION_WITH_WEIGHTS`], the answer also carries, in
//! section 1, the weight string of every character of the set and of
//! every contraction. One answer is one signal of at most a megabyte,
//! sent in fragments, so a large collation takes several requests: the
//! first from position 0, each next one from the position the last
//! answer gave, until that position is [`IC_COLLATION_ALL_SENT`]. A data
//! node serves one such request at a time and refuses another meanwhile
//! as busy.
//!
//! Only data nodes of version 26.10.0 or later know the request, and an
//! older one takes it as a fatal error, so it is sent only to a node
//! whose version has been seen.
//! Verify: chapter 05 §8.2.

use ic_port::err;
use ic_port::IcError;

/// Words in a `COLLATION_DESC_REQ`.
pub const IC_COLLATION_DESC_REQ_LEN: usize = 7;
/// Words in a `COLLATION_DESC_CONF`.
pub const IC_COLLATION_DESC_CONF_LEN: usize = 19;
/// Words in a `COLLATION_DESC_REF`.
pub const IC_COLLATION_DESC_REF_LEN: usize = 4;

/// The first data node version that knows the request, 26.10.0.
pub const IC_COLLATION_DESC_MIN_VERSION: u32 = 0x1A_0A00;

/// Look up by collation number.
pub const IC_COLLATION_BY_NUMBER: u32 = 0;
/// Look up by collation name.
pub const IC_COLLATION_BY_NAME: u32 = 1;
/// Look up a character set by name, meaning its default collation.
pub const IC_COLLATION_BY_CHARSET: u32 = 2;

/// Request flag: send the weights too.
pub const IC_COLLATION_WITH_WEIGHTS: u32 = 0x1;
/// Positions from here on are contractions, the first one here.
pub const IC_COLLATION_CONTRACTIONS_POSITION: u32 = 0x11_0000;
/// The position an answer gives when every weight has been sent.
pub const IC_COLLATION_ALL_SENT: u32 = 0xFFFF_FFFF;
/// The most bytes of sections one answer with weights carries.
pub const IC_COLLATION_MAX_ANSWER_BYTES: usize = 1024 * 1024;

/// The longest name the request may carry, in bytes.
pub const IC_COLLATION_MAX_NAME: usize = 64;
/// Collation numbers run from 1 up to, not including, this.
pub const IC_COLLATION_NUMBER_LIMIT: u32 = 2048;

/// Flag: the default collation of its character set.
pub const IC_COLLATION_DEFAULT: u32 = 0x1;
/// Flag: a binary order.
pub const IC_COLLATION_BINARY: u32 = 0x2;
/// Flag: the character set is Unicode, in a standard encoding.
pub const IC_COLLATION_UNICODE: u32 = 0x4;
/// Flag: upper and lower case sort apart.
pub const IC_COLLATION_CASE_SENSITIVE: u32 = 0x8;
/// Flag: the character set is pure ASCII.
pub const IC_COLLATION_PURE_ASCII: u32 = 0x10;
/// Flag: trailing spaces count when comparing.
pub const IC_COLLATION_NO_PAD: u32 = 0x20;

/// Refused: too short, or sections that do not fit the kind of lookup.
pub const IC_COLLATION_ERR_MALFORMED: u32 = 1;
/// Refused: the reply address is on another node than the sender's.
pub const IC_COLLATION_ERR_REPLY_ADDRESS: u32 = 2;
/// Refused: not a kind of lookup, or unknown request flags.
pub const IC_COLLATION_ERR_KIND: u32 = 3;
/// Refused: a collation number of 0, or 2048 and above.
pub const IC_COLLATION_ERR_NUMBER: u32 = 4;
/// Refused: a name that is empty, too long, or holds a NUL.
pub const IC_COLLATION_ERR_NAME: u32 = 5;
/// Refused: no such collation on the data node.
pub const IC_COLLATION_ERR_NO_COLLATION: u32 = 6;
/// Refused: no such character set on the data node.
pub const IC_COLLATION_ERR_NO_CHARSET: u32 = 7;
/// Refused: weights asked for from [`IC_COLLATION_ALL_SENT`].
pub const IC_COLLATION_ERR_POSITION: u32 = 8;
/// Refused: another weights request is being served; try again.
pub const IC_COLLATION_ERR_BUSY: u32 = 9;
/// Refused: no memory to build the answer in; try again later.
pub const IC_COLLATION_ERR_NO_MEMORY: u32 = 10;

/// In a weight run: the weight string is the same for every code point.
pub const IC_COLLATION_NO_STEP: u8 = 0xFF;
/// In a contraction: the second code point is weighed by the first.
pub const IC_COLLATION_PREVIOUS_CONTEXT: u8 = 0x1;

/// Bytes before a weight run's encoded character and weight string.
const IC_WEIGHT_RUN_HEADER: usize = 10;
/// Bytes before a contraction's code points.
const IC_CONTRACTION_HEADER: usize = 4;

/// A request for a collation's description.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CollationDescReq {
  /// Where the answer goes: a block reference on our own node.
  pub caller_ref: u32,
  /// Given back in the answer.
  pub caller_data: u32,
  /// How the collation is named: [`IC_COLLATION_BY_NUMBER`],
  /// [`IC_COLLATION_BY_NAME`] or [`IC_COLLATION_BY_CHARSET`].
  pub kind: u32,
  /// The collation's number, for a lookup by number.
  pub number: u32,
  /// The name, for a lookup by collation or character set name.
  pub name: String,
  /// [`IC_COLLATION_WITH_WEIGHTS`], or 0.
  pub flags: u32,
  /// With weights: where to start, 0 at first, then the position the
  /// last answer gave.
  pub position: u32,
}

impl CollationDescReq {
  /// Ask by collation number.
  pub fn by_number(
    caller_ref: u32,
    caller_data: u32,
    number: u32,
  ) -> Result<CollationDescReq, IcError> {
    if number == 0 || number >= IC_COLLATION_NUMBER_LIMIT {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CollationDescReq {
      caller_ref,
      caller_data,
      kind: IC_COLLATION_BY_NUMBER,
      number,
      ..CollationDescReq::default()
    })
  }

  /// Ask by a collation's name, or with [`IC_COLLATION_BY_CHARSET`] by a
  /// character set's name.
  pub fn by_name(
    caller_ref: u32,
    caller_data: u32,
    kind: u32,
    name: &str,
  ) -> Result<CollationDescReq, IcError> {
    let named = kind == IC_COLLATION_BY_NAME || kind == IC_COLLATION_BY_CHARSET;
    if !named || name.is_empty() || name.len() > IC_COLLATION_MAX_NAME {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    if name.as_bytes().contains(&0) {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CollationDescReq {
      caller_ref,
      caller_data,
      kind,
      name: name.to_string(),
      ..CollationDescReq::default()
    })
  }

  /// Ask for the weights too, from `position`.
  pub fn with_weights(mut self, position: u32) -> CollationDescReq {
    self.flags |= IC_COLLATION_WITH_WEIGHTS;
    self.position = position;
    self
  }

  /// The request's words.
  pub fn encode(&self) -> [u32; IC_COLLATION_DESC_REQ_LEN] {
    [
      self.caller_ref,
      self.caller_data,
      self.kind,
      self.number,
      self.name.len() as u32,
      self.flags,
      self.position,
    ]
  }

  /// Section 0 for a lookup by name: the name's bytes, padded with
  /// zeroes to whole words, with no NUL needed after it. A lookup by
  /// number sends no section.
  pub fn name_section(&self) -> Vec<u32> {
    let bytes = self.name.as_bytes();
    let mut words: Vec<u32> = Vec::with_capacity(bytes.len().div_ceil(4));
    let mut i: usize = 0;
    while i < bytes.len() {
      let mut chunk: [u8; 4] = [0; 4];
      let mut j: usize = 0;
      while j < 4 && i + j < bytes.len() {
        chunk[j] = bytes[i + j];
        j += 1;
      }
      words.push(u32::from_ne_bytes(chunk));
      i += 4;
    }
    words
  }
}

/// A collation's description, and with weights one part of them.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CollationDescConf {
  /// As the request sent it.
  pub caller_data: u32,
  /// The collation's number.
  pub number: u32,
  /// The number of its character set's default collation.
  pub default_number: u32,
  /// The number of its character set's binary collation, 0 if none.
  pub binary_number: u32,
  /// [`IC_COLLATION_DEFAULT`] and the other flags.
  pub flags: u32,
  /// The fewest bytes a character takes.
  pub min_char_bytes: u32,
  /// The most bytes a character takes.
  pub max_char_bytes: u32,
  /// The most sort-key bytes one byte of a string becomes.
  pub sort_key_growth: u32,
  /// The most bytes upper-casing makes of one byte.
  pub upper_case_growth: u32,
  /// The most bytes lower-casing makes of one byte.
  pub lower_case_growth: u32,
  /// The byte a CHAR value is padded with.
  pub pad_byte: u32,
  /// How many weight levels are compared.
  pub weight_levels: u32,
  /// The lowest-sorting code point.
  pub min_sort_char: u32,
  /// The highest-sorting code point.
  pub max_sort_char: u32,
  /// The character set's name.
  pub charset_name: String,
  /// The collation's name.
  pub collation_name: String,
  /// Weight runs in this answer.
  pub weight_entries: u32,
  /// Bytes of weight runs in section 1, before their padding.
  pub weights_len: u32,
  /// Contractions in this answer.
  pub contraction_entries: u32,
  /// Bytes of contractions in section 1, before their padding.
  pub contractions_len: u32,
  /// Where the next request continues, or [`IC_COLLATION_ALL_SENT`].
  pub next_position: u32,
}

impl CollationDescConf {
  /// Read the description: nineteen words, and section 0 with the
  /// character set's name and the collation's name, each followed by a
  /// NUL. The weights in section 1 are read with
  /// [`CollationDescConf::weights`].
  pub fn decode(
    data: &[u32],
    names: &[u32],
  ) -> Result<CollationDescConf, IcError> {
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if data.len() < IC_COLLATION_DESC_CONF_LEN {
      return Err(bad);
    }
    let charset_len = (data[13] & 0xFFFF) as usize;
    let collation_len = (data[13] >> 16) as usize;
    let bytes = bytes_of(names);
    // Both names and their NULs must lie inside the section.
    if charset_len + 1 + collation_len + 1 > bytes.len() {
      return Err(bad);
    }
    let charset = &bytes[..charset_len];
    let collation_start = charset_len + 1;
    let collation = &bytes[collation_start..collation_start + collation_len];
    let charset_name = match std::str::from_utf8(charset) {
      Ok(name) => name.to_string(),
      Err(_) => return Err(bad),
    };
    let collation_name = match std::str::from_utf8(collation) {
      Ok(name) => name.to_string(),
      Err(_) => return Err(bad),
    };
    Ok(CollationDescConf {
      caller_data: data[0],
      number: data[1],
      default_number: data[2],
      binary_number: data[3],
      flags: data[4],
      min_char_bytes: data[5],
      max_char_bytes: data[6],
      sort_key_growth: data[7],
      upper_case_growth: data[8] & 0xFF,
      lower_case_growth: (data[8] >> 8) & 0xFF,
      pad_byte: data[9],
      weight_levels: data[10],
      min_sort_char: data[11],
      max_sort_char: data[12],
      charset_name,
      collation_name,
      weight_entries: data[14],
      weights_len: data[15],
      contraction_entries: data[16],
      contractions_len: data[17],
      next_position: data[18],
    })
  }

  /// True if trailing spaces count when comparing.
  pub fn is_no_pad(&self) -> bool {
    (self.flags & IC_COLLATION_NO_PAD) != 0
  }

  /// True if more weights are to be asked for.
  pub fn has_more(&self) -> bool {
    self.next_position != IC_COLLATION_ALL_SENT
  }

  /// The weight runs and contractions section 1 holds, into lists the
  /// caller keeps, which are added to. An empty section is no weights.
  pub fn weights(
    &self,
    section: &[u32],
    runs: &mut Vec<WeightRun>,
    contractions: &mut Vec<Contraction>,
  ) -> Result<(), IcError> {
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if self.weight_entries == 0 && self.contraction_entries == 0 {
      return Ok(());
    }
    let bytes = bytes_of(section);
    let weights_len = self.weights_len as usize;
    let contractions_start = weights_len.next_multiple_of(4);
    let contractions_len = self.contractions_len as usize;
    if contractions_start + contractions_len > bytes.len() {
      return Err(bad);
    }
    parse_runs(&bytes[..weights_len], self.weight_entries, runs)?;
    let end = contractions_start + contractions_len;
    parse_contractions(
      &bytes[contractions_start..end],
      self.contraction_entries,
      contractions,
    )
  }
}

/// Consecutive code points with their weights: `count` of them from
/// `first`. A step offset other than [`IC_COLLATION_NO_STEP`] says that
/// code point `first + k` has the first one's weight string with the
/// big-endian 16-bit number at that byte increased by `k`; otherwise all
/// share it. The character's bytes in its set, given only for a set that
/// is not Unicode, likewise read as a big-endian number increased by `k`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WeightRun {
  /// The first code point.
  pub first: u32,
  /// How many code points, at least 1.
  pub count: u32,
  /// The byte of the weight string that steps, or
  /// [`IC_COLLATION_NO_STEP`].
  pub step_offset: u8,
  /// The first code point's bytes in the character set; empty for a
  /// Unicode set, whose encoding is the standard one.
  pub encoded: Vec<u8>,
  /// The first code point's weight string: the sort key of a string of
  /// that one character, without padding.
  pub weight: Vec<u8>,
}

impl WeightRun {
  /// The weight string of code point `first + k`, into `out`, cleared
  /// first. `k` must be below `count`.
  pub fn weight_of(&self, k: u32, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&self.weight);
    let at = self.step_offset as usize;
    if self.step_offset == IC_COLLATION_NO_STEP || at + 2 > out.len() {
      return;
    }
    let base = u16::from_be_bytes([out[at], out[at + 1]]);
    let value = base.wrapping_add(k as u16);
    out[at..at + 2].copy_from_slice(&value.to_be_bytes());
  }

  /// The bytes of code point `first + k` in the character set, into
  /// `out`, cleared first; nothing for a Unicode set.
  pub fn encoded_of(&self, k: u32, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&self.encoded);
    // Add k to the big-endian number the bytes make, carrying upward.
    let mut carry = k as u64;
    let mut i = out.len();
    while carry != 0 && i > 0 {
      i -= 1;
      let sum = out[i] as u64 + (carry & 0xFF);
      out[i] = sum as u8;
      carry = (carry >> 8) + (sum >> 8);
    }
  }
}

/// A sequence of code points the collation weighs as one unit, such as
/// "ch" in Czech.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Contraction {
  /// The code points, two to six, in string order.
  pub code_points: Vec<u32>,
  /// True if the second code point is weighed by the first, which keeps
  /// its own weights.
  pub previous_context: bool,
  /// The weight string of the whole sequence.
  pub weight: Vec<u8>,
}

/// The bytes of a section's words, as they lay on the wire.
fn bytes_of(words: &[u32]) -> Vec<u8> {
  let mut bytes: Vec<u8> = Vec::with_capacity(4 * words.len());
  for word in words {
    bytes.extend_from_slice(&word.to_ne_bytes());
  }
  bytes
}

fn be24(bytes: &[u8]) -> u32 {
  ((bytes[0] as u32) << 16) | ((bytes[1] as u32) << 8) | bytes[2] as u32
}

fn parse_runs(
  bytes: &[u8],
  entries: u32,
  runs: &mut Vec<WeightRun>,
) -> Result<(), IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  let mut at: usize = 0;
  let mut n: u32 = 0;
  while n < entries {
    if at + IC_WEIGHT_RUN_HEADER > bytes.len() {
      return Err(bad);
    }
    let first = be24(&bytes[at..]);
    let count = be24(&bytes[at + 3..]);
    let step_offset = bytes[at + 6];
    let encoded_len = bytes[at + 7] as usize;
    let weight_len =
      u16::from_be_bytes([bytes[at + 8], bytes[at + 9]]) as usize;
    let body = at + IC_WEIGHT_RUN_HEADER;
    if count == 0 || body + encoded_len + weight_len > bytes.len() {
      return Err(bad);
    }
    let weight_start = body + encoded_len;
    runs.push(WeightRun {
      first,
      count,
      step_offset,
      encoded: bytes[body..weight_start].to_vec(),
      weight: bytes[weight_start..weight_start + weight_len].to_vec(),
    });
    at = weight_start + weight_len;
    n += 1;
  }
  if at != bytes.len() {
    return Err(bad);
  }
  Ok(())
}

fn parse_contractions(
  bytes: &[u8],
  entries: u32,
  contractions: &mut Vec<Contraction>,
) -> Result<(), IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  let mut at: usize = 0;
  let mut n: u32 = 0;
  while n < entries {
    if at + IC_CONTRACTION_HEADER > bytes.len() {
      return Err(bad);
    }
    let points = bytes[at] as usize;
    let flags = bytes[at + 1];
    let weight_len =
      u16::from_be_bytes([bytes[at + 2], bytes[at + 3]]) as usize;
    let body = at + IC_CONTRACTION_HEADER;
    if !(2..=6).contains(&points) {
      return Err(bad);
    }
    if body + 3 * points + weight_len > bytes.len() {
      return Err(bad);
    }
    let mut code_points: Vec<u32> = Vec::with_capacity(points);
    let mut i: usize = 0;
    while i < points {
      code_points.push(be24(&bytes[body + 3 * i..]));
      i += 1;
    }
    let weight_start = body + 3 * points;
    contractions.push(Contraction {
      code_points,
      previous_context: (flags & IC_COLLATION_PREVIOUS_CONTEXT) != 0,
      weight: bytes[weight_start..weight_start + weight_len].to_vec(),
    });
    at = weight_start + weight_len;
    n += 1;
  }
  if at != bytes.len() {
    return Err(bad);
  }
  Ok(())
}

/// A refused request.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CollationDescRef {
  /// As the request sent it, 0 if it was too short to carry it.
  pub caller_data: u32,
  /// Why: [`IC_COLLATION_ERR_MALFORMED`] and the others.
  pub error_code: u32,
  /// The kind of lookup, as sent.
  pub kind: u32,
  /// The collation number, as sent.
  pub number: u32,
}

impl CollationDescRef {
  /// Read the refusal.
  pub fn decode(data: &[u32]) -> Result<CollationDescRef, IcError> {
    if data.len() < IC_COLLATION_DESC_REF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(CollationDescRef {
      caller_data: data[0],
      error_code: data[1],
      kind: data[2],
      number: data[3],
    })
  }

  /// True if the same request may succeed when sent again later.
  pub fn is_temporary(&self) -> bool {
    self.error_code == IC_COLLATION_ERR_BUSY
      || self.error_code == IC_COLLATION_ERR_NO_MEMORY
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn words_of(bytes: &[u8]) -> Vec<u32> {
    let mut words: Vec<u32> = Vec::new();
    let mut i: usize = 0;
    while i < bytes.len() {
      let mut chunk: [u8; 4] = [0; 4];
      let mut j: usize = 0;
      while j < 4 && i + j < bytes.len() {
        chunk[j] = bytes[i + j];
        j += 1;
      }
      words.push(u32::from_ne_bytes(chunk));
      i += 4;
    }
    words
  }

  fn conf_words(names: (usize, usize), weights: [u32; 5]) -> Vec<u32> {
    let mut data: Vec<u32> = vec![9, 8, 8, 47, 0x1, 1, 1, 1, 0x0101, 0x20];
    data.extend_from_slice(&[1, 0, 255]);
    data.push(names.0 as u32 | ((names.1 as u32) << 16));
    data.extend_from_slice(&weights);
    data
  }

  #[test]
  fn a_lookup_by_number_is_seven_words_and_no_section() {
    let req = CollationDescReq::by_number(0x8001_00C0, 7, 8).expect("ok");
    assert_eq!(
      req.encode(),
      [0x8001_00C0, 7, IC_COLLATION_BY_NUMBER, 8, 0, 0, 0]
    );
    assert!(req.name_section().is_empty());
  }

  #[test]
  fn weights_are_asked_for_from_a_position() {
    let req = CollationDescReq::by_number(1, 2, 8)
      .expect("ok")
      .with_weights(0x41);
    assert_eq!(req.encode()[5], IC_COLLATION_WITH_WEIGHTS);
    assert_eq!(req.encode()[6], 0x41);
  }

  #[test]
  fn a_number_outside_the_range_is_refused_here() {
    assert!(CollationDescReq::by_number(1, 1, 0).is_err());
    assert!(CollationDescReq::by_number(1, 1, 2048).is_err());
    assert!(CollationDescReq::by_number(1, 1, 2047).is_ok());
  }

  #[test]
  fn a_name_is_padded_to_words_without_a_nul() {
    let req =
      CollationDescReq::by_name(1, 2, IC_COLLATION_BY_NAME, "latin1_bin")
        .expect("ok");
    assert_eq!(req.encode()[4], 10);
    assert_eq!(req.name_section(), words_of(b"latin1_bin\0\0"));
  }

  #[test]
  fn a_bad_name_or_kind_is_refused_here() {
    let empty = CollationDescReq::by_name(1, 2, IC_COLLATION_BY_NAME, "");
    assert!(empty.is_err());
    let long = "x".repeat(IC_COLLATION_MAX_NAME + 1);
    let too_long = CollationDescReq::by_name(1, 2, IC_COLLATION_BY_NAME, &long);
    assert!(too_long.is_err());
    let nul = CollationDescReq::by_name(1, 2, IC_COLLATION_BY_NAME, "a\0b");
    assert!(nul.is_err());
    let kind = CollationDescReq::by_name(1, 2, IC_COLLATION_BY_NUMBER, "x");
    assert!(kind.is_err());
  }

  #[test]
  fn a_description_gives_its_words_and_both_names() {
    let data = conf_words((6, 17), [0, 0, 0, 0, IC_COLLATION_ALL_SENT]);
    let names = words_of(b"latin1\0latin1_swedish_ci\0\0\0\0");
    let conf = CollationDescConf::decode(&data, &names).expect("ok");
    assert_eq!(conf.caller_data, 9);
    assert_eq!(conf.binary_number, 47);
    assert_eq!(conf.upper_case_growth, 1);
    assert_eq!(conf.lower_case_growth, 1);
    assert_eq!(conf.pad_byte, 0x20);
    assert_eq!(conf.max_sort_char, 255);
    assert!(!conf.is_no_pad());
    assert!(!conf.has_more());
    assert_eq!(conf.charset_name, "latin1");
    assert_eq!(conf.collation_name, "latin1_swedish_ci");
  }

  #[test]
  fn names_longer_than_their_section_are_refused() {
    let data = conf_words((6, 40), [0, 0, 0, 0, IC_COLLATION_ALL_SENT]);
    let names = words_of(b"latin1\0latin1_bin\0\0\0");
    assert!(CollationDescConf::decode(&data, &names).is_err());
  }

  #[test]
  fn weight_runs_and_contractions_are_read_and_stepped() {
    // A run of three code points from 'a', each one weight higher, with
    // its one-byte character, and a run of one with no step.
    let mut weights: Vec<u8> = Vec::new();
    weights.extend_from_slice(&[0, 0, 0x61, 0, 0, 3, 0, 1, 0, 2]);
    weights.extend_from_slice(&[0x61, 0x1C, 0x47]);
    weights.extend_from_slice(&[0, 0, 0x20, 0, 0, 1, 0xFF, 1, 0, 2]);
    weights.extend_from_slice(&[0x20, 0x02, 0x09]);
    let weights_len = weights.len();
    weights.resize(weights_len.next_multiple_of(4), 0);
    // A contraction of "c" and "h".
    let mut contractions: Vec<u8> = vec![2, 0, 0, 2];
    contractions.extend_from_slice(&[0, 0, 0x63, 0, 0, 0x68]);
    contractions.extend_from_slice(&[0x1C, 0x60]);
    let contractions_len = contractions.len();
    let mut section = weights;
    section.extend_from_slice(&contractions);
    let counts = [2, weights_len as u32, 1, contractions_len as u32, 0x11_0001];
    let data = conf_words((6, 10), counts);
    let names = words_of(b"latin1\0latin1_bin\0\0\0");
    let conf = CollationDescConf::decode(&data, &names).expect("ok");
    assert!(conf.has_more());
    let mut runs: Vec<WeightRun> = Vec::new();
    let mut found: Vec<Contraction> = Vec::new();
    let read = conf.weights(&words_of(&section), &mut runs, &mut found);
    assert!(read.is_ok());
    assert_eq!(runs.len(), 2);
    assert_eq!(runs[0].first, 0x61);
    assert_eq!(runs[0].count, 3);
    let mut out: Vec<u8> = Vec::new();
    runs[0].weight_of(2, &mut out);
    assert_eq!(out, vec![0x1C, 0x49]);
    runs[0].encoded_of(2, &mut out);
    assert_eq!(out, vec![0x63]);
    runs[1].weight_of(0, &mut out);
    assert_eq!(out, vec![0x02, 0x09]);
    assert_eq!(found.len(), 1);
    assert_eq!(found[0].code_points, vec![0x63, 0x68]);
    assert!(!found[0].previous_context);
    assert_eq!(found[0].weight, vec![0x1C, 0x60]);
  }

  #[test]
  fn a_run_past_its_section_is_refused() {
    let weights: Vec<u8> = vec![0, 0, 0x61, 0, 0, 3, 0, 1, 0, 9, 0x61, 0];
    let data = conf_words((6, 10), [1, 12, 0, 0, IC_COLLATION_ALL_SENT]);
    let names = words_of(b"latin1\0latin1_bin\0\0\0");
    let conf = CollationDescConf::decode(&data, &names).expect("ok");
    let mut runs: Vec<WeightRun> = Vec::new();
    let mut found: Vec<Contraction> = Vec::new();
    let result = conf.weights(&words_of(&weights), &mut runs, &mut found);
    assert!(result.is_err());
  }

  #[test]
  fn a_refusal_gives_the_error_and_what_was_asked() {
    let words = [9, IC_COLLATION_ERR_BUSY, 0, 8];
    let refusal = CollationDescRef::decode(&words).expect("ok");
    assert_eq!(refusal.error_code, IC_COLLATION_ERR_BUSY);
    assert_eq!(refusal.number, 8);
    assert!(refusal.is_temporary());
  }
}
