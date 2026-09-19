// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Turning a byte stream into signals
//! (`legacy-c/api/ic_apid_rec_thread.ic`, `ndb_receive_node`).
//!
//! A socket delivers bytes whenever it likes: half a signal, three
//! signals, a signal and a fragment of the next. This keeps what has
//! arrived and hands out whole signals as they become complete.
//!
//! Signals are read as words in this machine's own order, never
//! swapped, which is what the protocol expects and what makes reading
//! one free. The buffer is therefore a vector of words that bytes are
//! read into directly, rather than a vector of bytes that would have to
//! be converted.

use ic_comm::connection::Connection;
use ic_ndb_signals::header;
use ic_port::err;
use ic_port::IcError;

/// Words the buffer starts with, enough for several largest-sized
/// messages before it has to grow.
const IC_INITIAL_BUFFER_WORDS: usize = 4 * header::IC_MAX_MESSAGE_WORDS;

/// Holds what has arrived on one connection and hands out the signals
/// in it.
pub struct SignalReader {
  words: Vec<u32>,
  /// How many bytes of `words` hold data. A signal may be split across
  /// reads at any byte, not only at a word boundary.
  filled_bytes: usize,
}

impl SignalReader {
  /// A reader with room for a few large messages.
  pub fn new() -> SignalReader {
    SignalReader {
      words: vec![0u32; IC_INITIAL_BUFFER_WORDS],
      filled_bytes: 0,
    }
  }

  /// Add bytes that were read elsewhere, such as the tail of a
  /// handshake.
  pub fn push_bytes(&mut self, bytes: &[u8]) {
    self.make_room(bytes.len());
    let at = self.filled_bytes;
    self.bytes_mut()[at..at + bytes.len()].copy_from_slice(bytes);
    self.filled_bytes += bytes.len();
  }

  /// Read whatever the connection has, and return how many bytes that
  /// was. Zero means the peer closed.
  pub fn read_from(&mut self, conn: &Connection) -> Result<usize, IcError> {
    self.make_room(header::IC_MAX_MESSAGE_BYTES);
    let at = self.filled_bytes;
    let size = {
      let buffer = self.bytes_mut();
      conn.read(&mut buffer[at..])?
    };
    self.filled_bytes += size;
    Ok(size)
  }

  /// The whole words that have arrived, which is where a signal is
  /// looked for.
  pub fn complete_words(&self) -> &[u32] {
    &self.words[..self.filled_bytes / 4]
  }

  /// How many bytes are held but not yet consumed.
  pub fn buffered_bytes(&self) -> usize {
    self.filled_bytes
  }

  /// The length of the signal at the front, if its first word has
  /// arrived; `None` if not even that much is here.
  pub fn peek_len(&self) -> Option<usize> {
    header::peek_message_len(self.complete_words())
  }

  /// True when a whole signal is available.
  pub fn has_message(&self) -> bool {
    match self.peek_len() {
      Some(len) => len > 0 && self.complete_words().len() >= len,
      None => false,
    }
  }

  /// Drop the first `words` words, moving what is left to the front.
  pub fn consume(&mut self, words: usize) {
    let bytes = words * 4;
    if bytes >= self.filled_bytes {
      self.filled_bytes = 0;
      return;
    }
    let remaining = self.filled_bytes - bytes;
    let buffer = self.bytes_mut();
    buffer.copy_within(bytes..bytes + remaining, 0);
    self.filled_bytes = remaining;
  }

  /// Forget everything buffered, for a connection being dropped.
  pub fn reset(&mut self) {
    self.filled_bytes = 0;
  }

  fn make_room(&mut self, extra_bytes: usize) {
    let needed = self.filled_bytes + extra_bytes;
    if needed <= self.words.len() * 4 {
      return;
    }
    let new_words = needed.div_ceil(4).next_power_of_two();
    self.words.resize(new_words, 0);
  }

  /// The buffer seen as bytes.
  ///
  /// A signal may begin at any byte of the stream, so bytes are read
  /// straight into the word buffer rather than into a separate byte
  /// buffer that would then have to be copied word by word.
  fn bytes_mut(&mut self) -> &mut [u8] {
    let len = self.words.len() * 4;
    let ptr = self.words.as_mut_ptr() as *mut u8;
    // SAFETY: the pointer comes from a live Vec<u32> of that many
    // words, so the range covers exactly its storage. A Vec<u32> is
    // aligned for u32, which is more than u8 needs, and u8 has no
    // invalid values, so every byte of it is readable and writable.
    unsafe { std::slice::from_raw_parts_mut(ptr, len) }
  }
}

impl Default for SignalReader {
  fn default() -> SignalReader {
    SignalReader::new()
  }
}

impl std::fmt::Debug for SignalReader {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "SignalReader({} bytes buffered, capacity {} words)",
      self.filled_bytes,
      self.words.len()
    )
  }
}

/// Check that a signal is addressed to this node's block range before
/// acting on it. A data node should never send us anything else, but
/// these bytes come off a network.
pub fn check_receiver(
  message: &header::Message<'_>,
  expected_block: u16,
) -> Result<(), IcError> {
  if message.header.receiver_block == expected_block {
    return Ok(());
  }
  Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA))
}

#[cfg(test)]
mod tests {
  use super::*;
  use ic_ndb_signals::blocks::*;
  use ic_ndb_signals::header::SignalHeader;

  fn a_signal(gsn: u16, data: &[u32]) -> Vec<u32> {
    let header = SignalHeader::new(gsn, IC_BLOCK_QMGR, IC_BLOCK_API_CLUSTERMGR);
    let mut out: Vec<u32> = Vec::new();
    header::encode(&header, data, &[], false, &mut out).expect("encode");
    out
  }

  fn as_bytes(words: &[u32]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    for word in words {
      out.extend_from_slice(&word.to_ne_bytes());
    }
    out
  }

  #[test]
  fn a_whole_signal_is_found() {
    let signal = a_signal(1, &[10, 20, 30]);
    let mut reader = SignalReader::new();
    assert!(!reader.has_message());
    reader.push_bytes(&as_bytes(&signal));
    assert!(reader.has_message());
    assert_eq!(reader.peek_len(), Some(signal.len()));
    let message = header::decode(reader.complete_words()).expect("decode");
    assert_eq!(message.header.gsn(), 1);
    assert_eq!(message.data, &[10, 20, 30]);
    let len = message.total_words;
    reader.consume(len);
    assert!(!reader.has_message());
    assert_eq!(reader.buffered_bytes(), 0);
  }

  #[test]
  fn a_signal_split_at_every_byte() {
    // TCP may break the stream anywhere, including inside a word.
    let signal = a_signal(3, &[1, 2, 3, 4, 5]);
    let bytes = as_bytes(&signal);
    let mut split: usize = 1;
    while split < bytes.len() {
      let mut reader = SignalReader::new();
      reader.push_bytes(&bytes[..split]);
      assert!(!reader.has_message(), "split at {}", split);
      reader.push_bytes(&bytes[split..]);
      assert!(reader.has_message(), "split at {}", split);
      let message = header::decode(reader.complete_words()).expect("decode");
      assert_eq!(message.data, &[1, 2, 3, 4, 5]);
      split += 1;
    }
  }

  #[test]
  fn several_signals_in_one_read() {
    let mut stream: Vec<u32> = Vec::new();
    let mut i: u32 = 1;
    while i <= 4 {
      stream.extend_from_slice(&a_signal(i as u16, &[i; 3]));
      i += 1;
    }
    let mut reader = SignalReader::new();
    reader.push_bytes(&as_bytes(&stream));
    let mut seen: u32 = 0;
    while reader.has_message() {
      let len = {
        let message = header::decode(reader.complete_words()).expect("dec");
        seen += 1;
        assert_eq!(message.header.gsn(), seen as u16);
        assert_eq!(message.data, &[seen; 3]);
        message.total_words
      };
      reader.consume(len);
    }
    assert_eq!(seen, 4);
    assert_eq!(reader.buffered_bytes(), 0);
  }

  #[test]
  fn a_signal_and_a_fragment_of_the_next() {
    let first = a_signal(1, &[7]);
    let second = a_signal(2, &[8, 9]);
    let mut bytes = as_bytes(&first);
    let second_bytes = as_bytes(&second);
    bytes.extend_from_slice(&second_bytes[..5]);
    let mut reader = SignalReader::new();
    reader.push_bytes(&bytes);
    // The first is complete, the second is not.
    assert!(reader.has_message());
    let len = {
      let message = header::decode(reader.complete_words()).expect("dec");
      assert_eq!(message.header.gsn(), 1);
      message.total_words
    };
    reader.consume(len);
    assert!(!reader.has_message());
    assert_eq!(reader.buffered_bytes(), 5);
    // The rest arrives and completes it.
    reader.push_bytes(&second_bytes[5..]);
    assert!(reader.has_message());
    let message = header::decode(reader.complete_words()).expect("dec");
    assert_eq!(message.header.gsn(), 2);
    assert_eq!(message.data, &[8, 9]);
  }

  #[test]
  fn the_buffer_grows_for_a_large_signal() {
    let mut reader = SignalReader::new();
    let big = vec![0xABu8; 300_000];
    reader.push_bytes(&big);
    assert_eq!(reader.buffered_bytes(), 300_000);
    reader.reset();
    assert_eq!(reader.buffered_bytes(), 0);
  }

  #[test]
  fn a_signal_for_another_block_is_noticed() {
    let signal = a_signal(1, &[1]);
    let mut reader = SignalReader::new();
    reader.push_bytes(&as_bytes(&signal));
    let message = header::decode(reader.complete_words()).expect("decode");
    assert!(check_receiver(&message, IC_BLOCK_API_CLUSTERMGR).is_ok());
    assert!(check_receiver(&message, api_block_of_thread(0)).is_err());
  }
}
