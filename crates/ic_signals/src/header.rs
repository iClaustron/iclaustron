// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The signal header and the layout of a message on the wire.
//!
//! This format has not changed since NDB 7.2, so the C framing in
//! `legacy-c/api/ic_apid_send_message.ic` is a sound reference for it,
//! cross-checked here against RonDB 26.10's framing.
//! Verify: chapter 05 §17.6.
//!
//! A message is a run of 32-bit words:
//!
//! ```text
//!   word 1   byte order, flags, total length, data length
//!   word 2   signal number, trace, number of sections
//!   word 3   sender block, receiver block
//!   [signal id]            present when the link is configured for it
//!   signal data            0 to 25 words
//!   [section lengths]      one word per section
//!   [section data]         the sections, end to end
//!   [checksum]             present when the link is configured for it
//! ```
//!
//! Words are written in the sender's own byte order, not network order.
//! The header says which order that was, and a receiver whose order
//! differs rejects the message rather than swapping it. That is what
//! makes reading a signal free: the bytes off the socket are already
//! the words.
//!
//! A signal whose sections do not fit one message goes as a train of
//! fragments, [`encode_train`]; the receiving side joins them in
//! `ic_apid`'s `fragments`.

use ic_port::err;
use ic_port::IcError;

/// Words of signal data a message may carry. Anything longer travels in
/// a section. Verify: chapter 05 §17.6.
pub const IC_MAX_SIGNAL_DATA_WORDS: usize = 25;
/// Sections a message may carry.
pub const IC_MAX_SECTIONS: usize = 3;
/// Words of header before the optional signal id.
pub const IC_SIGNAL_HEADER_WORDS: usize = 3;
/// Largest message in bytes, in either direction.
/// Verify: chapter 05 §17.6.
pub const IC_MAX_MESSAGE_BYTES: usize = 32768;
/// Largest message in words.
pub const IC_MAX_MESSAGE_WORDS: usize = IC_MAX_MESSAGE_BYTES / 4;
/// Most words of sections a message carries: a signal whose sections
/// come to more goes as a train of fragments, each carrying up to this
/// many. It is what a 32 KB message holds with room for its header.
/// Verify: chapter 05 §16.1.
pub const IC_FRAGMENT_SECTION_WORDS: usize = 8040;
/// The unit a data node keeps a section in. Every piece of a section in
/// a train but its last is a whole number of these, so a piece that
/// begins part-way into a fragment is cut short to one. Any multiple of
/// 60 satisfies RonDB 26.10. Verify: chapter 05 §16.1.
pub const IC_SECTION_SEGMENT_WORDS: usize = 60;

// Word 1.
const WORD1_BYTEORDER_MASK: u32 = 0x8100_0081;
const WORD1_FRAG_HIGH_MASK: u32 = 0x0000_0002;
const WORD1_SIGNALID_MASK: u32 = 0x0000_0004;
const WORD1_COMPRESSED_MASK: u32 = 0x0000_0008;
const WORD1_CHECKSUM_MASK: u32 = 0x0000_0010;
const WORD1_PRIO_MASK: u32 = 0x0000_0060;
const WORD1_PRIO_SHIFT: u32 = 5;
const WORD1_MESSAGELEN_MASK: u32 = 0x00FF_FF00;
const WORD1_MESSAGELEN_SHIFT: u32 = 8;
const WORD1_FRAG_LOW_MASK: u32 = 0x0200_0000;
const WORD1_FRAG_LOW_SHIFT: u32 = 25;
const WORD1_SIGNAL_LEN_MASK: u32 = 0x7C00_0000;
const WORD1_SIGNAL_LEN_SHIFT: u32 = 26;

// Word 2.
const WORD2_VERID_SIG_MASK: u32 = 0x000F_FFFF;
const WORD2_TRACE_MASK: u32 = 0x03F0_0000;
const WORD2_TRACE_SHIFT: u32 = 20;
const WORD2_SEC_COUNT_MASK: u32 = 0x0C00_0000;
const WORD2_SEC_COUNT_SHIFT: u32 = 26;

// Word 3.
const WORD3_SENDER_MASK: u32 = 0x0000_FFFF;
const WORD3_RECEIVER_MASK: u32 = 0xFFFF_0000;
const WORD3_RECEIVER_SHIFT: u32 = 16;

/// Byte order value for a big endian machine.
/// Verify: chapter 05 §17.6.
pub const IC_BYTE_ORDER_BIG: u32 = 1;
/// Byte order value for a little endian machine.
pub const IC_BYTE_ORDER_LITTLE: u32 = 0;

/// This machine's byte order, as the header records it.
///
/// Note that the RonDB comment that describes the header words has
/// these two the wrong way round; the code that writes the bit is what
/// counts. Verify: chapter 05 §17.6.
pub fn own_byte_order() -> u32 {
  if cfg!(target_endian = "big") {
    IC_BYTE_ORDER_BIG
  } else {
    IC_BYTE_ORDER_LITTLE
  }
}

/// Where a message stands in a train of fragments that together make
/// one larger message.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FragmentState {
  /// Not fragmented: the whole message is here.
  #[default]
  Whole = 0,
  /// The first of several fragments.
  First = 1,
  /// Neither the first nor the last.
  Middle = 2,
  /// The last fragment, which completes the message.
  Last = 3,
}

impl FragmentState {
  /// The value a two-bit field holds.
  pub fn from_u8(value: u8) -> FragmentState {
    match value {
      1 => FragmentState::First,
      2 => FragmentState::Middle,
      3 => FragmentState::Last,
      _ => FragmentState::Whole,
    }
  }
}

/// What a signal header says.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SignalHeader {
  /// Which signal this is, and in its top four bits which version of
  /// the signal's own layout. The two share a 20-bit field.
  pub verid_sig: u32,
  /// The block that sent it. Only the number travels; the node comes
  /// from the connection it arrived on.
  pub sender_block: u16,
  /// The block it is addressed to.
  pub receiver_block: u16,
  /// Words of signal data, at most [`IC_MAX_SIGNAL_DATA_WORDS`].
  pub data_len: u8,
  /// How many sections follow, at most [`IC_MAX_SECTIONS`].
  pub num_sections: u8,
  /// Where this message stands in a train of fragments.
  pub fragment_state: FragmentState,
  /// Priority, 0 for ordinary traffic.
  pub prio: u8,
  /// Trace number, for following a signal through the cluster.
  pub trace: u8,
  /// The sender's signal id, when the link carries them.
  pub signal_id: Option<u32>,
}

impl SignalHeader {
  /// A header for a signal from `sender_block` to `receiver_block`.
  pub fn new(
    sig_no: u16,
    sender_block: u16,
    receiver_block: u16,
  ) -> SignalHeader {
    SignalHeader {
      verid_sig: sig_no as u32,
      sender_block,
      receiver_block,
      ..SignalHeader::default()
    }
  }

  /// The signal number, without the version in the top four bits.
  pub fn sig_no(&self) -> u16 {
    (self.verid_sig & 0xFFFF) as u16
  }

  /// The version of this signal's layout.
  pub fn version_id(&self) -> u8 {
    ((self.verid_sig >> 16) & 0xF) as u8
  }
}

/// How many words a message occupies, given what it carries.
pub fn message_len_words(
  data_len: usize,
  section_lens: &[usize],
  use_signal_id: bool,
  use_checksum: bool,
) -> usize {
  let mut total = IC_SIGNAL_HEADER_WORDS + data_len + section_lens.len();
  for len in section_lens {
    total += *len;
  }
  if use_signal_id {
    total += 1;
  }
  if use_checksum {
    total += 1;
  }
  total
}

/// The checksum of a message: every word of it except the checksum
/// word itself, exclusive-ored together.
/// Verify: chapter 05 §17.6.
pub fn compute_checksum(words: &[u32]) -> u32 {
  let mut sum: u32 = 0;
  for word in words {
    sum ^= *word;
  }
  sum
}

fn byte_order_bits(order: u32) -> u32 {
  // The one significant bit is repeated into bits 0, 7, 24 and 31, so
  // that a receiver reading the word the other way round still sees a
  // consistent value. Verify: chapter 05 §17.6.
  let mut bits = order;
  bits |= bits << 7;
  bits |= bits << 24;
  bits & WORD1_BYTEORDER_MASK
}

/// Write a message into `out`: header, data, section lengths, sections
/// and, if asked for, the signal id and the checksum.
///
/// Returns how many words were written, or an error if the message
/// would be malformed or too large.
pub fn encode(
  header: &SignalHeader,
  data: &[u32],
  sections: &[&[u32]],
  use_checksum: bool,
  out: &mut Vec<u32>,
) -> Result<usize, IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  if data.len() > IC_MAX_SIGNAL_DATA_WORDS {
    return Err(bad);
  }
  if sections.len() > IC_MAX_SECTIONS {
    return Err(bad);
  }
  // A data node drops a signal whose data and sections together come
  // to more than 25. Verify: chapter 05 §16.1.
  if data.len() + sections.len() > IC_MAX_SIGNAL_DATA_WORDS {
    return Err(bad);
  }
  let mut lens: [usize; IC_MAX_SECTIONS] = [0; IC_MAX_SECTIONS];
  let mut i: usize = 0;
  while i < sections.len() {
    lens[i] = sections[i].len();
    i += 1;
  }
  let section_lens = &lens[..sections.len()];
  let use_signal_id = header.signal_id.is_some();
  let total =
    message_len_words(data.len(), section_lens, use_signal_id, use_checksum);
  if total > IC_MAX_MESSAGE_WORDS {
    return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
  }
  let start = out.len();

  let mut word1: u32 = byte_order_bits(own_byte_order());
  let fragment = header.fragment_state as u32;
  word1 |= fragment & WORD1_FRAG_HIGH_MASK;
  word1 |= (fragment << WORD1_FRAG_LOW_SHIFT) & WORD1_FRAG_LOW_MASK;
  if use_signal_id {
    word1 |= WORD1_SIGNALID_MASK;
  }
  if use_checksum {
    word1 |= WORD1_CHECKSUM_MASK;
  }
  word1 |= ((header.prio as u32) << WORD1_PRIO_SHIFT) & WORD1_PRIO_MASK;
  word1 |= ((total as u32) << WORD1_MESSAGELEN_SHIFT) & WORD1_MESSAGELEN_MASK;
  word1 |=
    ((data.len() as u32) << WORD1_SIGNAL_LEN_SHIFT) & WORD1_SIGNAL_LEN_MASK;
  out.push(word1);

  let mut word2: u32 = header.verid_sig & WORD2_VERID_SIG_MASK;
  word2 |= ((header.trace as u32) << WORD2_TRACE_SHIFT) & WORD2_TRACE_MASK;
  word2 |=
    ((sections.len() as u32) << WORD2_SEC_COUNT_SHIFT) & WORD2_SEC_COUNT_MASK;
  out.push(word2);

  let mut word3: u32 = (header.sender_block as u32) & WORD3_SENDER_MASK;
  let receiver = (header.receiver_block as u32) << WORD3_RECEIVER_SHIFT;
  word3 |= receiver & WORD3_RECEIVER_MASK;
  out.push(word3);

  if let Some(id) = header.signal_id {
    out.push(id);
  }
  out.extend_from_slice(data);
  for len in section_lens {
    out.push(*len as u32);
  }
  for section in sections {
    out.extend_from_slice(section);
  }
  if use_checksum {
    let sum = compute_checksum(&out[start..]);
    out.push(sum);
  }
  Ok(out.len() - start)
}

/// Where a signal written by [`encode_train`] lies in `out`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Train {
  /// Words written, every message of the train together.
  pub len: usize,
  /// How many messages: one for a signal that went whole.
  pub messages: usize,
  /// Where the last message begins, in words from the first's start.
  pub last_at: usize,
  /// Where the signal's own data begins, likewise. It is in the last
  /// message, the only one that carries it.
  pub data_at: usize,
}

/// Write a signal into `out`, whole when its sections come to at most
/// [`IC_FRAGMENT_SECTION_WORDS`], else as a train of fragments that
/// the receiver joins (chapter 05 §16.1):
///
/// ```text
///   first, middle   data: the numbers of the sections it carries,
///                         then the fragment id
///   last            data: the signal's data, the numbers of the
///                         sections it carries, then the fragment id
/// ```
///
/// The sections go in order, each fragment carrying up to
/// [`IC_FRAGMENT_SECTION_WORDS`] of them: the rest of one section, any
/// sections that fit whole, and the start of the next. Every piece of a
/// section but its last is a multiple of [`IC_SECTION_SEGMENT_WORDS`].
/// `fragment_id` names the train to the receiver, together with the
/// sender block; it is not used for a signal that goes whole.
///
/// On error nothing is left in `out`.
pub fn encode_train(
  header: &SignalHeader,
  data: &[u32],
  sections: &[&[u32]],
  use_checksum: bool,
  fragment_id: u32,
  out: &mut Vec<u32>,
) -> Result<Train, IcError> {
  let start = out.len();
  let written =
    write_train(header, data, sections, use_checksum, fragment_id, out);
  if written.is_err() {
    out.truncate(start);
  }
  written
}

fn write_train(
  header: &SignalHeader,
  data: &[u32],
  sections: &[&[u32]],
  use_checksum: bool,
  fragment_id: u32,
  out: &mut Vec<u32>,
) -> Result<Train, IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  if data.len() > IC_MAX_SIGNAL_DATA_WORDS || sections.len() > IC_MAX_SECTIONS {
    return Err(bad);
  }
  let mut data_at = IC_SIGNAL_HEADER_WORDS;
  if header.signal_id.is_some() {
    data_at += 1;
  }
  let mut total: usize = 0;
  for section in sections {
    total += section.len();
  }
  if total <= IC_FRAGMENT_SECTION_WORDS {
    let len = encode(header, data, sections, use_checksum, out)?;
    return Ok(Train {
      len,
      messages: 1,
      last_at: 0,
      data_at,
    });
  }

  let start = out.len();
  let mut fragment = *header;
  // The section the next piece comes from, and how much of it has gone.
  let mut section: usize = 0;
  let mut taken: usize = 0;
  let mut messages: usize = 0;
  let mut last_at: usize = 0;
  while section < sections.len() {
    let mut numbers = [0u32; IC_MAX_SECTIONS];
    let mut pieces: [&[u32]; IC_MAX_SECTIONS] = [&[]; IC_MAX_SECTIONS];
    let mut count: usize = 0;
    let mut room = IC_FRAGMENT_SECTION_WORDS;
    while section < sections.len() {
      let rest = sections[section].len() - taken;
      // A section's last piece may be of any length; any other is cut
      // to whole segments, and waits for the next fragment if not even
      // one fits.
      let mut take = rest;
      if rest > room {
        take = room / IC_SECTION_SEGMENT_WORDS * IC_SECTION_SEGMENT_WORDS;
        if take == 0 {
          break;
        }
      }
      numbers[count] = section as u32;
      pieces[count] = &sections[section][taken..taken + take];
      count += 1;
      room -= take;
      taken += take;
      if taken < sections[section].len() {
        break;
      }
      section += 1;
      taken = 0;
    }
    let is_last = section == sections.len();
    let mut words = [0u32; IC_MAX_SIGNAL_DATA_WORDS + IC_MAX_SECTIONS + 1];
    let mut n: usize = 0;
    if is_last {
      words[..data.len()].copy_from_slice(data);
      n = data.len();
    }
    for number in &numbers[..count] {
      words[n] = *number;
      n += 1;
    }
    words[n] = fragment_id;
    n += 1;
    fragment.fragment_state = if messages == 0 {
      FragmentState::First
    } else if is_last {
      FragmentState::Last
    } else {
      FragmentState::Middle
    };
    let at = out.len() - start;
    encode(&fragment, &words[..n], &pieces[..count], use_checksum, out)?;
    if is_last {
      last_at = at;
    }
    messages += 1;
  }
  Ok(Train {
    len: out.len() - start,
    messages,
    last_at,
    data_at: last_at + data_at,
  })
}

/// A message read from the wire, pointing into the buffer it arrived
/// in rather than copying it.
#[derive(Clone, Copy, Debug)]
pub struct Message<'a> {
  /// What the header said.
  pub header: SignalHeader,
  /// The signal data.
  pub data: &'a [u32],
  /// The sections, of which [`SignalHeader::num_sections`] are used.
  pub sections: [&'a [u32]; IC_MAX_SECTIONS],
  /// How many words the whole message occupied.
  pub total_words: usize,
  /// Where the data begins, in words from the start of the message:
  /// for a reader that hands the signal on where it lies.
  pub data_start: usize,
  /// Where each section begins, likewise.
  pub section_starts: [usize; IC_MAX_SECTIONS],
}

impl Message<'_> {
  /// The sections that are present.
  pub fn section(&self, index: usize) -> Option<&[u32]> {
    if index >= self.header.num_sections as usize {
      return None;
    }
    Some(self.sections[index])
  }
}

/// How long the message starting at `words` claims to be, without
/// decoding it. Returns `None` if there is not even a first word.
pub fn peek_message_len(words: &[u32]) -> Option<usize> {
  if words.is_empty() {
    return None;
  }
  let len = (words[0] & WORD1_MESSAGELEN_MASK) >> WORD1_MESSAGELEN_SHIFT;
  Some(len as usize)
}

/// Read one message from the front of `words`.
///
/// The words must already be in this machine's order, which for a
/// message from a peer of the same endianness means the bytes as they
/// arrived. A message from a peer of the other endianness is refused,
/// as RonDB refuses it, rather than being swapped.
pub fn decode(words: &[u32]) -> Result<Message<'_>, IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  if words.len() < IC_SIGNAL_HEADER_WORDS {
    return Err(bad);
  }
  let word1 = words[0];
  let word2 = words[1];
  let word3 = words[2];

  if (word1 & WORD1_BYTEORDER_MASK) != byte_order_bits(own_byte_order()) {
    // Either the peer is of the other endianness, which RonDB does not
    // support between two nodes, or this is not a message at all.
    return Err(IcError::new(err::IC_ERROR_WRONG_IP_FAMILY));
  }
  if (word1 & WORD1_COMPRESSED_MASK) != 0 {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }

  let total =
    ((word1 & WORD1_MESSAGELEN_MASK) >> WORD1_MESSAGELEN_SHIFT) as usize;
  if total < IC_SIGNAL_HEADER_WORDS || total > words.len() {
    return Err(bad);
  }
  let data_len =
    ((word1 & WORD1_SIGNAL_LEN_MASK) >> WORD1_SIGNAL_LEN_SHIFT) as usize;
  if data_len > IC_MAX_SIGNAL_DATA_WORDS {
    return Err(bad);
  }
  let num_sections =
    ((word2 & WORD2_SEC_COUNT_MASK) >> WORD2_SEC_COUNT_SHIFT) as usize;
  if num_sections > IC_MAX_SECTIONS {
    return Err(bad);
  }
  let use_signal_id = (word1 & WORD1_SIGNALID_MASK) != 0;
  let use_checksum = (word1 & WORD1_CHECKSUM_MASK) != 0;

  let fragment_1 = (word1 & WORD1_FRAG_HIGH_MASK) as u8;
  let fragment_2 =
    ((word1 & WORD1_FRAG_LOW_MASK) >> WORD1_FRAG_LOW_SHIFT) as u8;
  let header = SignalHeader {
    verid_sig: word2 & WORD2_VERID_SIG_MASK,
    sender_block: (word3 & WORD3_SENDER_MASK) as u16,
    receiver_block: ((word3 & WORD3_RECEIVER_MASK) >> WORD3_RECEIVER_SHIFT)
      as u16,
    data_len: data_len as u8,
    num_sections: num_sections as u8,
    fragment_state: FragmentState::from_u8(fragment_1 | fragment_2),
    prio: ((word1 & WORD1_PRIO_MASK) >> WORD1_PRIO_SHIFT) as u8,
    trace: ((word2 & WORD2_TRACE_MASK) >> WORD2_TRACE_SHIFT) as u8,
    signal_id: None,
  };

  let mut at = IC_SIGNAL_HEADER_WORDS;
  let mut message = Message {
    header,
    data: &[],
    sections: [&[], &[], &[]],
    total_words: total,
    data_start: 0,
    section_starts: [0; IC_MAX_SECTIONS],
  };
  if use_signal_id {
    if at >= total {
      return Err(bad);
    }
    message.header.signal_id = Some(words[at]);
    at += 1;
  }
  if at + data_len > total {
    return Err(bad);
  }
  message.data = &words[at..at + data_len];
  message.data_start = at;
  at += data_len;

  // The section lengths come together, then the sections themselves.
  if at + num_sections > total {
    return Err(bad);
  }
  let mut lens = [0usize; IC_MAX_SECTIONS];
  let mut i: usize = 0;
  while i < num_sections {
    lens[i] = words[at] as usize;
    at += 1;
    i += 1;
  }
  i = 0;
  while i < num_sections {
    if at + lens[i] > total {
      return Err(bad);
    }
    message.sections[i] = &words[at..at + lens[i]];
    message.section_starts[i] = at;
    at += lens[i];
    i += 1;
  }
  if use_checksum {
    if at + 1 != total {
      return Err(bad);
    }
    let expected = compute_checksum(&words[..at]);
    if expected != words[at] {
      return Err(IcError::new(err::IC_ERROR_MESSAGE_CHECKSUM));
    }
    at += 1;
  }
  // Everything the header promised has to be accounted for, with
  // nothing left over inside the message.
  if at != total {
    return Err(bad);
  }
  Ok(message)
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::blocks::*;

  fn sample_header() -> SignalHeader {
    SignalHeader::new(12, api_block_of_thread(3), IC_BLOCK_COORDINATOR)
  }

  #[test]
  fn a_plain_signal_round_trips() {
    let header = sample_header();
    let data = [1u32, 2, 3, 4, 5];
    let mut out: Vec<u32> = Vec::new();
    let written = encode(&header, &data, &[], false, &mut out).expect("encode");
    assert_eq!(written, 3 + 5);
    assert_eq!(out.len(), written);
    assert_eq!(peek_message_len(&out), Some(written));

    let message = decode(&out).expect("decode");
    assert_eq!(message.header.sig_no(), 12);
    assert_eq!(message.header.sender_block, 0x8003);
    assert_eq!(message.header.receiver_block, IC_BLOCK_COORDINATOR);
    assert_eq!(message.header.data_len, 5);
    assert_eq!(message.header.num_sections, 0);
    assert_eq!(message.data, &data);
    assert_eq!(message.total_words, written);
    assert_eq!(message.section(0), None);
  }

  #[test]
  fn sections_and_checksum_round_trip() {
    let mut header = sample_header();
    header.signal_id = Some(0xDEAD_BEEF);
    header.trace = 7;
    header.prio = 1;
    let data = [9u32; 8];
    let key = [0x11u32, 0x22, 0x33];
    let attr = [0x44u32; 20];
    let mut out: Vec<u32> = Vec::new();
    let written =
      encode(&header, &data, &[&key, &attr], true, &mut out).expect("encode");
    // header 3, signal id 1, data 8, two lengths, 3 + 20 section words,
    // checksum 1.
    assert_eq!(written, 3 + 1 + 8 + 2 + 3 + 20 + 1);

    let message = decode(&out).expect("decode");
    assert_eq!(message.header.signal_id, Some(0xDEAD_BEEF));
    assert_eq!(message.header.trace, 7);
    assert_eq!(message.header.prio, 1);
    assert_eq!(message.header.num_sections, 2);
    assert_eq!(message.data, &data);
    assert_eq!(message.section(0), Some(&key[..]));
    assert_eq!(message.section(1), Some(&attr[..]));
    assert_eq!(message.section(2), None);
  }

  #[test]
  fn fragments_carry_their_position() {
    let positions = [
      FragmentState::Whole,
      FragmentState::First,
      FragmentState::Middle,
      FragmentState::Last,
    ];
    for position in &positions {
      let mut header = sample_header();
      header.fragment_state = *position;
      let mut out: Vec<u32> = Vec::new();
      encode(&header, &[1, 2], &[], false, &mut out).expect("encode");
      let message = decode(&out).expect("decode");
      assert_eq!(message.header.fragment_state, *position, "{:?}", position);
    }
  }

  #[test]
  fn a_corrupted_word_fails_the_checksum() {
    let header = sample_header();
    let mut out: Vec<u32> = Vec::new();
    encode(&header, &[1, 2, 3], &[&[7, 8]], true, &mut out).expect("encode");
    let good = decode(&out);
    assert!(good.is_ok());
    // Flip a bit in the data and the checksum no longer agrees.
    out[4] ^= 1;
    let err = decode(&out).expect_err("corrupt");
    assert_eq!(err.code, err::IC_ERROR_MESSAGE_CHECKSUM);
  }

  #[test]
  fn malformed_messages_are_refused() {
    let header = sample_header();
    let mut good: Vec<u32> = Vec::new();
    encode(&header, &[1, 2, 3], &[&[7, 8]], false, &mut good).expect("encode");
    // Too short to hold a header.
    assert!(decode(&good[..2]).is_err());
    // The header claims more words than arrived.
    let mut truncated = good.clone();
    truncated.pop();
    assert!(decode(&truncated).is_err());
    // A section longer than the message.
    let mut lying = good.clone();
    lying[6] = 1000;
    assert!(decode(&lying).is_err());
    // The byte order bits of a peer with the other endianness.
    let mut foreign = good.clone();
    foreign[0] ^= WORD1_BYTEORDER_MASK;
    assert!(decode(&foreign).is_err());
    // A compressed message, which no RonDB node sends.
    let mut compressed = good.clone();
    compressed[0] |= WORD1_COMPRESSED_MASK;
    assert!(decode(&compressed).is_err());
  }

  #[test]
  fn oversized_signals_are_refused() {
    let header = sample_header();
    let data = [0u32; IC_MAX_SIGNAL_DATA_WORDS + 1];
    let mut out: Vec<u32> = Vec::new();
    assert!(encode(&header, &data, &[], false, &mut out).is_err());
    let section = vec![0u32; IC_MAX_MESSAGE_WORDS];
    let mut out: Vec<u32> = Vec::new();
    let result = encode(&header, &[], &[&section], false, &mut out);
    assert_eq!(
      result.expect_err("too large").code,
      err::IC_ERROR_RECORD_SIZE_TOO_BIG
    );
    let mut out: Vec<u32> = Vec::new();
    let four: [&[u32]; 4] = [&[1], &[2], &[3], &[4]];
    assert!(encode(&header, &[], &four, false, &mut out).is_err());
  }

  #[test]
  fn too_many_data_words_with_sections_are_refused() {
    let header = sample_header();
    let data = [0u32; IC_MAX_SIGNAL_DATA_WORDS - 1];
    let mut out: Vec<u32> = Vec::new();
    assert!(encode(&header, &data, &[&[1]], false, &mut out).is_ok());
    let mut out: Vec<u32> = Vec::new();
    assert!(encode(&header, &data, &[&[1], &[2]], false, &mut out).is_err());
  }

  /// What a receiver makes of a train: the joined signal, and for each
  /// section the lengths of its pieces in the order they came.
  struct Joined {
    states: Vec<FragmentState>,
    data: Vec<u32>,
    sections: Vec<Vec<u32>>,
    pieces: Vec<Vec<usize>>,
    ids: Vec<u32>,
    signal_ids: Vec<Option<u32>>,
  }

  fn join(words: &[u32]) -> Joined {
    let mut joined = Joined {
      states: Vec::new(),
      data: Vec::new(),
      sections: Vec::new(),
      pieces: Vec::new(),
      ids: Vec::new(),
      signal_ids: Vec::new(),
    };
    let mut at: usize = 0;
    while at < words.len() {
      let message = decode(&words[at..]).expect("decode");
      assert!(message.total_words <= IC_MAX_MESSAGE_WORDS);
      let header = message.header;
      let count = header.num_sections as usize;
      assert!(header.data_len as usize + count <= IC_MAX_SIGNAL_DATA_WORDS);
      joined.states.push(header.fragment_state);
      joined.signal_ids.push(header.signal_id);
      let mut data_len = message.data.len();
      let mut numbers: Vec<usize> = (0..count).collect();
      if header.fragment_state != FragmentState::Whole {
        joined.ids.push(message.data[data_len - 1]);
        data_len -= count + 1;
        numbers = message.data[data_len..data_len + count]
          .iter()
          .map(|n| *n as usize)
          .collect();
      }
      if header.fragment_state == FragmentState::Whole
        || header.fragment_state == FragmentState::Last
      {
        joined.data = message.data[..data_len].to_vec();
      } else {
        assert_eq!(data_len, 0, "a fragment before the last has no data");
      }
      let mut i: usize = 0;
      while i < count {
        let number = numbers[i];
        while joined.sections.len() <= number {
          joined.sections.push(Vec::new());
          joined.pieces.push(Vec::new());
        }
        let piece = message.section(i).expect("section");
        joined.sections[number].extend_from_slice(piece);
        joined.pieces[number].push(piece.len());
        i += 1;
      }
      at += message.total_words;
    }
    assert_eq!(at, words.len());
    joined
  }

  /// Sections of the given lengths, each word naming its section and
  /// place, so that a piece out of place shows.
  fn numbered_sections(lens: &[usize]) -> Vec<Vec<u32>> {
    let mut sections: Vec<Vec<u32>> = Vec::new();
    for (number, len) in lens.iter().enumerate() {
      sections.push(
        (0..*len as u32)
          .map(|i| ((number as u32) << 24) | i)
          .collect(),
      );
    }
    sections
  }

  /// Send a signal with sections of these lengths, check that it comes
  /// back whole and that every piece follows the rules, and return how
  /// many messages it took.
  fn round_trip(lens: &[usize], use_checksum: bool, signal_id: bool) -> usize {
    let mut header = sample_header();
    if signal_id {
      header.signal_id = Some(0x5157);
    }
    let data = [11u32, 12, 13, 14, 15, 16, 17, 18];
    let sections = numbered_sections(lens);
    let slices: Vec<&[u32]> = sections.iter().map(|s| &s[..]).collect();
    let mut out: Vec<u32> = vec![0xAAAA, 0xBBBB];
    let train =
      encode_train(&header, &data, &slices, use_checksum, 77, &mut out)
        .expect("encode");
    assert_eq!(out[..2], [0xAAAA, 0xBBBB], "what was there is kept");
    let words = &out[2..];
    assert_eq!(train.len, words.len());
    let joined = join(words);
    assert_eq!(joined.states.len(), train.messages);
    assert_eq!(joined.data, data.to_vec());
    assert_eq!(joined.sections, sections);
    // The signal's data is where the train says, in the last message.
    assert_eq!(words[train.data_at..train.data_at + data.len()], data);
    assert_eq!(
      peek_message_len(&words[train.last_at..]),
      Some(words.len() - train.last_at)
    );
    let mut sum_of_sections: usize = 0;
    for len in lens {
      sum_of_sections += *len;
    }
    if sum_of_sections <= IC_FRAGMENT_SECTION_WORDS {
      assert_eq!(joined.states, vec![FragmentState::Whole]);
      return 1;
    }
    let n = joined.states.len();
    assert_eq!(joined.states[0], FragmentState::First);
    assert_eq!(joined.states[n - 1], FragmentState::Last);
    for state in &joined.states[1..n - 1] {
      assert_eq!(*state, FragmentState::Middle);
    }
    assert!(joined.ids.iter().all(|id| *id == 77));
    let wanted = if signal_id { Some(0x5157) } else { None };
    assert!(joined.signal_ids.iter().all(|id| *id == wanted));
    // Every piece of a section but its last is whole segments.
    for pieces in &joined.pieces {
      for len in &pieces[..pieces.len() - 1] {
        assert_eq!(len % IC_SECTION_SEGMENT_WORDS, 0, "{:?}", pieces);
        assert!(*len >= IC_SECTION_SEGMENT_WORDS);
      }
    }
    n
  }

  #[test]
  fn a_signal_goes_whole_up_to_the_fragment_size() {
    assert_eq!(
      round_trip(&[1, IC_FRAGMENT_SECTION_WORDS - 1], false, false),
      1
    );
    assert_eq!(round_trip(&[IC_FRAGMENT_SECTION_WORDS], true, false), 1);
    assert_eq!(round_trip(&[1, IC_FRAGMENT_SECTION_WORDS], false, false), 2);
  }

  #[test]
  fn fragments_fill_to_the_boundaries() {
    // A one-word key leaves room for 7 980 words of the next section in
    // the first fragment, and the second takes 8 040.
    assert_eq!(round_trip(&[1, 16020], false, false), 2);
    assert_eq!(round_trip(&[1, 16021], false, false), 3);
    // A 1 023-word key leaves room for 6 960.
    assert_eq!(round_trip(&[1023, 15000], false, false), 2);
    assert_eq!(round_trip(&[1023, 15001], false, false), 3);
    // The largest row there is with the largest key.
    assert_eq!(round_trip(&[1023, 18000], false, false), 3);
  }

  #[test]
  fn a_section_is_cut_on_segments_wherever_it_begins() {
    // A head piece that is not whole segments, so the next section
    // begins part-way into the first fragment.
    let mut out: Vec<u32> = Vec::new();
    let sections = numbered_sections(&[7, 9000]);
    let slices: Vec<&[u32]> = sections.iter().map(|s| &s[..]).collect();
    encode_train(&sample_header(), &[1], &slices, false, 3, &mut out)
      .expect("encode");
    let joined = join(&out);
    assert_eq!(joined.pieces[0], vec![7]);
    assert_eq!(joined.pieces[1], vec![7980, 1020]);
    // Less than a segment of room left: the next section waits.
    assert_eq!(round_trip(&[8000, 100], false, false), 2);
    out.clear();
    let sections = numbered_sections(&[8000, 100]);
    let slices: Vec<&[u32]> = sections.iter().map(|s| &s[..]).collect();
    encode_train(&sample_header(), &[1], &slices, false, 3, &mut out)
      .expect("encode");
    let joined = join(&out);
    assert_eq!(joined.pieces[1], vec![100]);
    // But a whole section that fits goes in what room there is.
    out.clear();
    let sections = numbered_sections(&[8000, 30, 100]);
    let slices: Vec<&[u32]> = sections.iter().map(|s| &s[..]).collect();
    encode_train(&sample_header(), &[1], &slices, false, 3, &mut out)
      .expect("encode");
    let joined = join(&out);
    assert_eq!(joined.states.len(), 2);
    assert_eq!(joined.pieces[1], vec![30]);
    assert_eq!(joined.pieces[2], vec![100]);
  }

  #[test]
  fn trains_of_every_shape_join_again() {
    let shapes: [&[usize]; 9] = [
      &[8041],
      &[30000],
      &[59, 8041],
      &[61, 8040, 61],
      &[8040, 8040, 8040],
      &[1, 1, 20000],
      &[4000, 4100, 0],
      &[0, 16080],
      &[7990, 49, 9000],
    ];
    for shape in &shapes {
      for (checksum, signal_id) in [(false, false), (true, false), (true, true)]
      {
        round_trip(shape, checksum, signal_id);
      }
    }
  }

  #[test]
  fn a_train_too_large_to_carry_leaves_nothing() {
    let header = sample_header();
    // The last fragment carries the data, the section numbers and the
    // fragment id, with the sections counted against 25 as well: 24
    // words of data and one section go whole, but not in a train.
    let data = [0u32; 24];
    let attr = vec![2u32; 9000];
    let mut out: Vec<u32> = vec![5, 6];
    let result = encode_train(&header, &data, &[&attr], false, 1, &mut out);
    assert!(result.is_err());
    assert_eq!(out, vec![5, 6], "the first fragment is taken back");
    let attr = vec![2u32; 100];
    assert!(encode_train(&header, &data, &[&attr], false, 1, &mut out).is_ok());
  }

  #[test]
  fn several_messages_pack_end_to_end() {
    // The receive path reads whatever arrived and walks it message by
    // message, which is what peek_message_len is for.
    let header = sample_header();
    let mut stream: Vec<u32> = Vec::new();
    let mut expected: Vec<usize> = Vec::new();
    let mut i: u32 = 1;
    while i <= 5 {
      let data = vec![i; i as usize];
      let written =
        encode(&header, &data, &[], i % 2 == 0, &mut stream).expect("encode");
      expected.push(written);
      i += 1;
    }
    let mut at: usize = 0;
    let mut seen: usize = 0;
    while at < stream.len() {
      let len = peek_message_len(&stream[at..]).expect("len");
      assert_eq!(len, expected[seen]);
      let message = decode(&stream[at..]).expect("decode");
      assert_eq!(message.data.len(), seen + 1);
      at += len;
      seen += 1;
    }
    assert_eq!(seen, 5);
  }
}
