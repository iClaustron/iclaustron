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

use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_comm::connection::Connection;
use ic_ndb_signals::header;
use ic_port::err;
use ic_port::IcError;

/// Words the buffer starts with, enough for several largest-sized
/// messages before it has to grow.
/// Words in a receive page: room for four of the largest messages, so
/// that a signal always fits in one page and a read can be large. Two
/// was tried: twice the reads for large rows cost more than the smaller
/// pages saved, in both modes (measured 2026-09-23).
pub const IC_RECEIVE_PAGE_WORDS: usize = 4 * header::IC_MAX_MESSAGE_WORDS;
/// Free pages a reader keeps for its next reads, beyond those others
/// are still reading, which it always keeps. Past this, free pages are
/// let go one at a time: letting them all go at once meant allocating
/// new ones at once, zeroed by the kernel, which with rows of 29 KB in
/// flight doubled the system time (measured 2026-09-23).
const IC_MAX_FREE_PAGES: usize = 64;

/// What every reader in the process has done, for measuring: reads and
/// their bytes, page switches, pages allocated, and the bytes of
/// unfinished signals copied across a switch.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReaderStats {
  /// Reads from a socket.
  pub reads: u64,
  /// Bytes they brought.
  pub bytes_read: u64,
  /// Times a reader went on in another page.
  pub page_switches: u64,
  /// Pages allocated, not taken from those retired.
  pub pages_allocated: u64,
  /// Bytes of signals not yet complete copied into the next page.
  pub tail_bytes_copied: u64,
  /// Reads made again right after one, before waiting for the socket.
  pub extra_reads: u64,
  /// Of those, the ones that found nothing: a system call for nothing.
  pub empty_reads: u64,
}

static IC_READS: AtomicU64 = AtomicU64::new(0);
static IC_BYTES_READ: AtomicU64 = AtomicU64::new(0);
static IC_PAGE_SWITCHES: AtomicU64 = AtomicU64::new(0);
static IC_PAGES_ALLOCATED: AtomicU64 = AtomicU64::new(0);
static IC_TAIL_BYTES_COPIED: AtomicU64 = AtomicU64::new(0);
static IC_EXTRA_READS: AtomicU64 = AtomicU64::new(0);
static IC_EMPTY_READS: AtomicU64 = AtomicU64::new(0);

/// What the readers have done so far.
pub fn reader_stats() -> ReaderStats {
  ReaderStats {
    reads: IC_READS.load(Ordering::Relaxed),
    bytes_read: IC_BYTES_READ.load(Ordering::Relaxed),
    page_switches: IC_PAGE_SWITCHES.load(Ordering::Relaxed),
    pages_allocated: IC_PAGES_ALLOCATED.load(Ordering::Relaxed),
    tail_bytes_copied: IC_TAIL_BYTES_COPIED.load(Ordering::Relaxed),
    extra_reads: IC_EXTRA_READS.load(Ordering::Relaxed),
    empty_reads: IC_EMPTY_READS.load(Ordering::Relaxed),
  }
}

/// A receive page: words read off the socket, shared with the user
/// threads a large signal in it was handed to. The count of the `Arc`
/// is the page's reference count, as the atomic on the C's page is.
pub type ReceivePage = Arc<Vec<u32>>;

/// The bytes read from one link, in pages (`IC_SOCK_BUF_PAGE`).
///
/// The reader writes into its page only while it holds the only
/// reference. A large signal handed on where it lies takes a reference
/// ([`page`](Self::page)); from then on the page is sealed, and the
/// next read goes into another page, the few bytes of a signal not yet
/// complete copied across. A sealed page is kept among the retired
/// ones and taken again once nobody else holds it, so that a page is
/// allocated and freed only by the thread that reads, however many
/// threads it went to.
pub struct SignalReader {
  page: ReceivePage,
  /// Where the first signal not yet consumed begins, in bytes; always
  /// a whole number of words. Consuming a signal moves this instead of
  /// moving what follows; what is left is moved once, before the next
  /// read.
  start_bytes: usize,
  /// How many bytes of the page hold data. A signal may be split across
  /// reads at any byte, not only at a word boundary.
  filled_bytes: usize,
  /// Pages sealed while others still read them.
  retired: Vec<ReceivePage>,
  /// True if the last read filled all the room it was given, so that
  /// more is likely waiting in the socket.
  last_read_full: bool,
}

impl SignalReader {
  /// A reader with one page.
  pub fn new() -> SignalReader {
    SignalReader {
      page: Arc::new(vec![0u32; IC_RECEIVE_PAGE_WORDS]),
      start_bytes: 0,
      filled_bytes: 0,
      retired: Vec::new(),
      last_read_full: false,
    }
  }

  /// Add bytes that were read elsewhere, such as the tail of a
  /// handshake.
  pub fn push_bytes(&mut self, bytes: &[u8]) -> Result<(), IcError> {
    self.make_room(bytes.len());
    let at = self.filled_bytes;
    let buffer = self.bytes_mut()?;
    buffer[at..at + bytes.len()].copy_from_slice(bytes);
    self.filled_bytes += bytes.len();
    Ok(())
  }

  /// Read whatever the connection has, and return how many bytes that
  /// was. Zero means the peer closed.
  pub fn read_from(&mut self, conn: &Connection) -> Result<usize, IcError> {
    self.make_room(header::IC_MAX_MESSAGE_BYTES);
    let at = self.filled_bytes;
    let (size, room) = {
      let buffer = self.bytes_mut()?;
      let room = buffer.len() - at;
      (conn.read(&mut buffer[at..])?, room)
    };
    self.filled_bytes += size;
    self.last_read_full = size == room;
    IC_READS.fetch_add(1, Ordering::Relaxed);
    IC_BYTES_READ.fetch_add(size as u64, Ordering::Relaxed);
    Ok(size)
  }

  /// Read again whatever has arrived since, without waiting: `None` if
  /// nothing has. For reading a socket until it is empty before waiting
  /// for it again.
  pub fn read_again(
    &mut self,
    conn: &Connection,
  ) -> Result<Option<usize>, IcError> {
    self.make_room(header::IC_MAX_MESSAGE_BYTES);
    let at = self.filled_bytes;
    let (got, room) = {
      let buffer = self.bytes_mut()?;
      let room = buffer.len() - at;
      (conn.read_nowait(&mut buffer[at..])?, room)
    };
    IC_EXTRA_READS.fetch_add(1, Ordering::Relaxed);
    self.last_read_full = false;
    match got {
      Some(size) => {
        self.filled_bytes += size;
        self.last_read_full = size == room;
        IC_READS.fetch_add(1, Ordering::Relaxed);
        IC_BYTES_READ.fetch_add(size as u64, Ordering::Relaxed);
      }
      None => {
        IC_EMPTY_READS.fetch_add(1, Ordering::Relaxed);
      }
    }
    Ok(got)
  }

  /// True if the last read filled all the room it was given: the
  /// socket likely holds more, and reading again is worth a system
  /// call.
  pub fn last_read_full(&self) -> bool {
    self.last_read_full
  }

  /// The whole words that have arrived, which is where a signal is
  /// looked for.
  pub fn complete_words(&self) -> &[u32] {
    &self.page[self.start_bytes / 4..self.filled_bytes / 4]
  }

  /// The page the complete words lie in, to take a reference to.
  pub fn page(&self) -> &ReceivePage {
    &self.page
  }

  /// Where [`complete_words`](Self::complete_words) begins in the page,
  /// in words.
  pub fn start_words(&self) -> usize {
    self.start_bytes / 4
  }

  /// How many bytes are held but not yet consumed.
  pub fn buffered_bytes(&self) -> usize {
    self.filled_bytes - self.start_bytes
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

  /// Drop the first `words` words.
  pub fn consume(&mut self, words: usize) {
    self.start_bytes += words * 4;
    if self.start_bytes >= self.filled_bytes {
      self.start_bytes = 0;
      self.filled_bytes = 0;
    }
  }

  /// Forget everything buffered, for a connection being dropped.
  pub fn reset(&mut self) {
    self.start_bytes = 0;
    self.filled_bytes = 0;
  }

  /// How many pages are sealed and still read by someone.
  pub fn pages_in_use(&self) -> usize {
    let mut n: usize = 0;
    for page in &self.retired {
      if Arc::strong_count(page) > 1 {
        n += 1;
      }
    }
    n
  }

  /// Make sure `extra_bytes` more fit after what is held, in a page
  /// nobody else reads, with what is not yet consumed at its front.
  fn make_room(&mut self, extra_bytes: usize) {
    let remaining = self.filled_bytes - self.start_bytes;
    let needed = remaining + extra_bytes;
    let shared = Arc::strong_count(&self.page) > 1;
    if !shared && needed <= self.page.len() * 4 {
      self.compact();
      return;
    }
    self.switch_page(needed);
  }

  /// Move what is not yet consumed to the front: usually a few bytes of
  /// a signal whose rest has not arrived, or nothing.
  fn compact(&mut self) {
    if self.start_bytes == 0 {
      return;
    }
    let start = self.start_bytes;
    let remaining = self.filled_bytes - start;
    if let Ok(buffer) = self.bytes_mut() {
      buffer.copy_within(start..start + remaining, 0);
    }
    self.start_bytes = 0;
    self.filled_bytes = remaining;
  }

  /// Go on in another page: a retired one nobody reads any more, or a
  /// new one. What is not yet consumed is copied across.
  ///
  /// The retired pages are in the order they were sealed, and the free
  /// one taken is the one sealed last: the one used most recently, most
  /// likely still in cache. Taking any free one spread the reads over
  /// every page kept, more memory than the caches hold, and `recv` then
  /// wrote to memory that had to be fetched first.
  fn switch_page(&mut self, needed_bytes: usize) {
    let needed_words = needed_bytes.div_ceil(4);
    let mut next: Option<ReceivePage> = None;
    let mut i: usize = self.retired.len();
    while i > 0 {
      i -= 1;
      let free = Arc::strong_count(&self.retired[i]) == 1;
      if free && self.retired[i].len() >= needed_words {
        next = Some(self.retired.remove(i));
        break;
      }
    }
    let mut next = match next {
      Some(page) => page,
      None => {
        let mut words = IC_RECEIVE_PAGE_WORDS;
        if needed_words > words {
          words = needed_words.next_power_of_two();
        }
        IC_PAGES_ALLOCATED.fetch_add(1, Ordering::Relaxed);
        Arc::new(vec![0u32; words])
      }
    };
    IC_PAGE_SWITCHES.fetch_add(1, Ordering::Relaxed);
    let start = self.start_bytes;
    let remaining = self.filled_bytes - start;
    if remaining > 0 {
      IC_TAIL_BYTES_COPIED.fetch_add(remaining as u64, Ordering::Relaxed);
      if let Some(words) = Arc::get_mut(&mut next) {
        let from = bytes_of(&self.page[..]);
        bytes_of_mut(words)[..remaining]
          .copy_from_slice(&from[start..start + remaining]);
      }
    }
    let old = std::mem::replace(&mut self.page, next);
    if Arc::strong_count(&old) > 1 || old.len() == IC_RECEIVE_PAGE_WORDS {
      self.retire(old);
    }
    self.start_bytes = 0;
    self.filled_bytes = remaining;
  }

  /// Keep a sealed page for later, and let go of free pages beyond
  /// [`IC_MAX_FREE_PAGES`].
  fn retire(&mut self, page: ReceivePage) {
    self.retired.push(page);
    let mut free: usize = 0;
    for held in &self.retired {
      if Arc::strong_count(held) == 1 {
        free += 1;
      }
    }
    // The oldest free pages go first; the order of the rest is kept.
    let mut i: usize = 0;
    while free > IC_MAX_FREE_PAGES && i < self.retired.len() {
      if Arc::strong_count(&self.retired[i]) == 1 {
        self.retired.remove(i);
        free -= 1;
      } else {
        i += 1;
      }
    }
  }

  /// The page seen as bytes, to read into. Only while nobody else holds
  /// the page, which [`make_room`](Self::make_room) sees to.
  fn bytes_mut(&mut self) -> Result<&mut [u8], IcError> {
    match Arc::get_mut(&mut self.page) {
      Some(words) => Ok(bytes_of_mut(words)),
      None => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    }
  }
}

/// Words seen as bytes.
fn bytes_of(words: &[u32]) -> &[u8] {
  let len = words.len() * 4;
  let ptr = words.as_ptr() as *const u8;
  // SAFETY: the pointer comes from a live slice of that many words, so
  // the range covers exactly its storage, and u8 has no alignment need
  // and no invalid values.
  unsafe { std::slice::from_raw_parts(ptr, len) }
}

/// Words seen as bytes, to write into.
///
/// A signal may begin at any byte of the stream, so bytes are read
/// straight into the words rather than into a separate byte buffer that
/// would then have to be copied word by word.
fn bytes_of_mut(words: &mut [u32]) -> &mut [u8] {
  let len = words.len() * 4;
  let ptr = words.as_mut_ptr() as *mut u8;
  // SAFETY: the pointer comes from a live, exclusively borrowed slice
  // of that many words, so the range covers exactly its storage. u32 is
  // aligned more strictly than u8 needs, and u8 has no invalid values,
  // so every byte is readable and writable.
  unsafe { std::slice::from_raw_parts_mut(ptr, len) }
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
      "SignalReader({} bytes buffered, page of {} words)",
      self.buffered_bytes(),
      self.page.len()
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
    reader.push_bytes(&as_bytes(&signal)).expect("push");
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
      reader.push_bytes(&bytes[..split]).expect("push");
      assert!(!reader.has_message(), "split at {}", split);
      reader.push_bytes(&bytes[split..]).expect("push");
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
    reader.push_bytes(&as_bytes(&stream)).expect("push");
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
    reader.push_bytes(&bytes).expect("push");
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
    reader.push_bytes(&second_bytes[5..]).expect("push");
    assert!(reader.has_message());
    let message = header::decode(reader.complete_words()).expect("dec");
    assert_eq!(message.header.gsn(), 2);
    assert_eq!(message.data, &[8, 9]);
  }

  #[test]
  fn the_buffer_grows_for_a_large_signal() {
    let mut reader = SignalReader::new();
    let big = vec![0xABu8; 300_000];
    reader.push_bytes(&big).expect("push");
    assert_eq!(reader.buffered_bytes(), 300_000);
    reader.reset();
    assert_eq!(reader.buffered_bytes(), 0);
  }

  #[test]
  fn a_page_someone_holds_is_left_alone_and_taken_again_later() {
    let first = a_signal(1, &[1, 2]);
    let second = a_signal(1, &[3, 4]);
    let mut reader = SignalReader::new();
    reader.push_bytes(&as_bytes(&first)).expect("push");
    // A large signal handed on where it lies: its page is shared.
    let held = Arc::clone(reader.page());
    let at = reader.start_words();
    let len = header::decode(reader.complete_words())
      .expect("one")
      .total_words;
    reader.consume(len);
    // Half a signal arrives: it goes into another page, and the held
    // page is not written.
    let bytes = as_bytes(&second);
    reader.push_bytes(&bytes[..6]).expect("push");
    assert!(!Arc::ptr_eq(&held, reader.page()));
    assert_eq!(reader.pages_in_use(), 1);
    let message = header::decode(&held[at..]).expect("still there");
    assert_eq!(message.data, &[1, 2]);
    // The rest arrives in the same new page.
    reader.push_bytes(&bytes[6..]).expect("push");
    let message = header::decode(reader.complete_words()).expect("two");
    assert_eq!(message.data, &[3, 4]);
    let len = message.total_words;
    reader.consume(len);
    // Once let go, the old page is free to be read into again.
    let old = Arc::as_ptr(&held);
    drop(held);
    assert_eq!(reader.pages_in_use(), 0);
    let second_page = Arc::clone(reader.page());
    let again = a_signal(1, &[5]);
    reader.push_bytes(&as_bytes(&again)).expect("push");
    assert_eq!(Arc::as_ptr(reader.page()), old);
    drop(second_page);
    assert_eq!(reader.pages_in_use(), 0);
  }

  #[test]
  fn the_free_page_sealed_last_is_taken_first() {
    let bytes = as_bytes(&a_signal(1, &[1]));
    let mut reader = SignalReader::new();
    reader.push_bytes(&bytes).expect("push");
    // Seal the first page, then the second.
    let first = Arc::clone(reader.page());
    reader.push_bytes(&bytes).expect("push");
    let second = Arc::clone(reader.page());
    reader.push_bytes(&bytes).expect("push");
    assert!(!Arc::ptr_eq(&first, &second));
    let second_at = Arc::as_ptr(&second);
    // Both are let go; the third page is sealed in turn.
    drop(first);
    drop(second);
    let third = Arc::clone(reader.page());
    reader.push_bytes(&bytes).expect("push");
    // The page sealed last among the free ones is the one read into.
    assert_eq!(Arc::as_ptr(reader.page()), second_at);
    drop(third);
  }

  #[test]
  fn a_signal_for_another_block_is_noticed() {
    let signal = a_signal(1, &[1]);
    let mut reader = SignalReader::new();
    reader.push_bytes(&as_bytes(&signal)).expect("push");
    let message = header::decode(reader.complete_words()).expect("decode");
    assert!(check_receiver(&message, IC_BLOCK_API_CLUSTERMGR).is_ok());
    assert!(check_receiver(&message, api_block_of_thread(0)).is_err());
  }
}
