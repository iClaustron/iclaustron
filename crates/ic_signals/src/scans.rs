// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Scans through the transaction coordinator: the request, its
//! confirmations and refusal, the request for more, and the key a
//! locking scan sends with each row. The words are RonDB 26.10's, as
//! chapter 05 §7 has them from the author and from captures.
//!
//! **One request starts a scan.** `SCAN_REQ` goes to the coordinator,
//! naming the scan's own coordinator record, the table (or the ordered
//! index of a range scan) and, for a scan whose rows may be taken over,
//! the owning transaction's record. Its sections are the receivers, an
//! id each, one per fragment scanned in parallel; the columns to read,
//! led by five size words; and for a range scan the bounds.
//!
//! **Rows come one signal each**, as `ROW_DATA_REP` naming a receiver,
//! straight from the node that scans the fragment. A locking scan that
//! asks for keys also gets `SCAN_ROW_KEY_REP` per row.
//!
//! **`SCAN_CONF` reports batches**, an entry per receiver: its id, the
//! coordinator's pointer for it (the null record when that receiver is
//! done), and how many rows and words its batch holds. Every request
//! of ours asks for the extended form, five words an entry; the short
//! form, which the reference uses, is read too.
//!
//! **`SCAN_MORE_REQ` lets receivers go on**, naming them by the
//! coordinator's pointers, or closes the scan when its stop word is
//! set. A close is confirmed by a `SCAN_CONF` with end of data and no
//! entries.
//!
//! Verify: chapter 05 §7.2, §7.4.

use ic_port::err;
use ic_port::IcError;

// ---- SCAN_REQ ----

/// Fixed words in a `SCAN_REQ`.
pub const IC_SCAN_REQ_LEN: usize = 11;
/// Words in a `SCAN_REQ` with the distribution key.
pub const IC_SCAN_REQ_MAX_LEN: usize = 12;
/// Section of a `SCAN_REQ` holding the receiver ids.
pub const IC_SCAN_REQ_RECEIVER_SECTION: usize = 0;
/// Section of a `SCAN_REQ` holding what to read.
pub const IC_SCAN_REQ_COLUMN_SECTION: usize = 1;
/// Section of a `SCAN_REQ` holding a range scan's bounds.
pub const IC_SCAN_REQ_BOUNDS_SECTION: usize = 2;

/// Word 5 of a scan neither pruned nor a join: its low half all ones,
/// as the reference sends it, and no flag in its high half.
/// Verify: chapter 05 §7.2.
pub const IC_SCAN_WORD5_PLAIN: u32 = 0x0000_FFFF;
/// In word 5: the scan covers one partition, the distribution key word
/// being its id. Needs the distribution key flag.
pub const IC_SCAN_WORD5_PARTITION_ID: u32 = 1 << 30;
/// Word 8 of a scan with no owning transaction: a committed read's.
/// Verify: chapter 05 §7.2.
pub const IC_SCAN_NO_OWNER: u32 = 0xFFFF_FFFF;

/// Rows a batch of a locking scan may hold, shared or exclusive: the
/// takeover's operation number has ten bits. The coordinator refuses
/// more with 1230. Verify: chapter 05 §7.2.
pub const IC_SCAN_MAX_LOCKED_ROWS: u32 = 992;
/// Rows a batch of a committed-read scan may hold.
/// Verify: chapter 05 §7.2.
pub const IC_SCAN_MAX_COMMITTED_ROWS: u32 = 4000;
/// Receivers a continuous scan gives each fragment scanned at once,
/// four in a row in section 0; the receivers must be a multiple of it.
/// Verify: chapter 05 §7.2, flag bit 2.
pub const IC_SCAN_CONTINUOUS_RECEIVERS: u32 = 4;

// Request flags, as bit positions in word 2.
const IC_SCAN_CONTINUOUS_SHIFT: u32 = 2;
const IC_SCAN_EXCLUSIVE_SHIFT: u32 = 8;
const IC_SCAN_NO_DISK_SHIFT: u32 = 9;
const IC_SCAN_HOLD_LOCK_SHIFT: u32 = 10;
const IC_SCAN_COMMITTED_SHIFT: u32 = 11;
const IC_SCAN_KEYS_SHIFT: u32 = 12;
const IC_SCAN_STORAGE_ORDER_SHIFT: u32 = 13;
const IC_SCAN_DESCENDING_SHIFT: u32 = 14;
const IC_SCAN_RANGE_SHIFT: u32 = 15;
const IC_SCAN_BATCH_ROWS_SHIFT: u32 = 16;
const IC_SCAN_BATCH_ROWS_MASK: u32 = 0x3FF;
const IC_SCAN_DIST_KEY_SHIFT: u32 = 26;
const IC_SCAN_EXTENDED_CONF_SHIFT: u32 = 29;

/// What a scan asks for, before it is made into the flags word. Of
/// bits 0 to 7, RonDB's extensions, only bit 2 is set, for the
/// continuous scan (chapter 05 §7.2).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanFlags {
  /// The continuous scan: four receivers for each fragment scanned at
  /// once, which goes on scanning after it has reported a batch. For
  /// committed reads only.
  pub continuous: bool,
  /// Exclusive locks; set with `hold_lock` and `keys`.
  pub exclusive: bool,
  /// The rows are in memory only, so the disk need not be read.
  pub no_disk: bool,
  /// Locks are held on the rows of a batch until the scan goes on.
  pub hold_lock: bool,
  /// Committed reads, with no lock.
  pub committed: bool,
  /// Each row's key comes with it, in `SCAN_ROW_KEY_REP`, so that the
  /// row may be taken over. An exclusive scan must ask for keys.
  pub keys: bool,
  /// Rows in the order they are stored, not by the hash index.
  pub storage_order: bool,
  /// A range scan returns its rows in descending order.
  pub descending: bool,
  /// A range scan of an ordered index.
  pub range: bool,
  /// Rows a batch holds, if it fits the field; see
  /// [`batch_rows_field`]. Zero sends the count in word 10 alone.
  pub batch_rows: u32,
  /// The distribution key word follows the fixed words.
  pub dist_key: bool,
  /// `SCAN_CONF` comes in its extended form, five words an entry.
  pub extended_conf: bool,
}

impl ScanFlags {
  /// The flags word, word 2.
  pub fn flags_word(&self) -> u32 {
    let mut info: u32 = 0;
    info |= (self.continuous as u32) << IC_SCAN_CONTINUOUS_SHIFT;
    info |= (self.exclusive as u32) << IC_SCAN_EXCLUSIVE_SHIFT;
    info |= (self.no_disk as u32) << IC_SCAN_NO_DISK_SHIFT;
    info |= (self.hold_lock as u32) << IC_SCAN_HOLD_LOCK_SHIFT;
    info |= (self.committed as u32) << IC_SCAN_COMMITTED_SHIFT;
    info |= (self.keys as u32) << IC_SCAN_KEYS_SHIFT;
    info |= (self.storage_order as u32) << IC_SCAN_STORAGE_ORDER_SHIFT;
    info |= (self.descending as u32) << IC_SCAN_DESCENDING_SHIFT;
    info |= (self.range as u32) << IC_SCAN_RANGE_SHIFT;
    info |=
      (self.batch_rows & IC_SCAN_BATCH_ROWS_MASK) << IC_SCAN_BATCH_ROWS_SHIFT;
    info |= (self.dist_key as u32) << IC_SCAN_DIST_KEY_SHIFT;
    info |= (self.extended_conf as u32) << IC_SCAN_EXTENDED_CONF_SHIFT;
    info
  }
}

/// The batch rows for the flags word: the count itself up to 992 rows,
/// above that 0, which has the coordinator read word 10 instead. The
/// reference does the same. Verify: chapter 05 §7.2.
pub fn batch_rows_field(rows: u32) -> u32 {
  if rows <= IC_SCAN_MAX_LOCKED_ROWS {
    return rows;
  }
  0
}

/// A scan, as sent to the coordinator.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanReq {
  /// The coordinator's pointer for the scan's own record, from the
  /// allocate.
  pub coord_ptr: u32,
  /// What to do; see [`ScanFlags`].
  pub flags_word: u32,
  /// The table, or the ordered index of a range scan.
  pub table_id: u32,
  /// Its version.
  pub table_version: u32,
  /// Word 5: [`IC_SCAN_WORD5_PLAIN`], with [`IC_SCAN_WORD5_PARTITION_ID`]
  /// for a scan of one partition by its id.
  pub word5: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The coordinator's pointer for the owning transaction's record, or
  /// [`IC_SCAN_NO_OWNER`].
  pub owner_ptr: u32,
  /// Bytes a batch may hold, a whole word.
  pub batch_bytes: u32,
  /// Rows a batch may hold.
  pub batch_rows: u32,
  /// The distribution key, a hash or a partition id, when the flags say
  /// it is given.
  pub dist_key: Option<u32>,
}

impl ScanReq {
  /// The signal data; the first [`len`](Self::len) words of it go.
  /// Word 1 is a spare, always 0.
  pub fn encode(&self) -> [u32; IC_SCAN_REQ_MAX_LEN] {
    [
      self.coord_ptr,
      0,
      self.flags_word,
      self.table_id,
      self.table_version,
      self.word5,
      self.trans_id_lo,
      self.trans_id_hi,
      self.owner_ptr,
      self.batch_bytes,
      self.batch_rows,
      self.dist_key.unwrap_or(0),
    ]
  }

  /// Words of signal data: eleven, or twelve with the distribution key.
  pub fn len(&self) -> usize {
    match self.dist_key {
      Some(_) => IC_SCAN_REQ_MAX_LEN,
      None => IC_SCAN_REQ_LEN,
    }
  }

  /// Never: a request has its fixed words. Here for clippy, beside
  /// [`len`](Self::len).
  pub fn is_empty(&self) -> bool {
    false
  }
}

/// The column section of a scan, into `out`, cleared first: five size
/// words, then what to read before the program, the program and its
/// subroutines. The third and fourth sizes, updates and a final read,
/// are always 0: a scan does not update, and a final read is of no use
/// (the author, chapter 05 §7.2). The read part is what a key read
/// sends for the same record.
pub fn column_section_into(
  read: &[u32],
  program: &[u32],
  subroutines: &[u32],
  out: &mut Vec<u32>,
) {
  crate::interp::column_parts_into(read, program, &[], &[], subroutines, out);
}

// ---- Range scan bounds ----

/// A bound that the column's value is at least: a low bound, inclusive.
/// The types read as "the bound is ... the value".
/// Verify: chapter 05 §7.2.
pub const IC_BOUND_LE: u32 = 0;
/// A bound the value is above: a low bound, exclusive.
pub const IC_BOUND_LT: u32 = 1;
/// A bound the value is at most: a high bound, inclusive.
pub const IC_BOUND_GE: u32 = 2;
/// A bound the value is below: a high bound, exclusive.
pub const IC_BOUND_GT: u32 = 3;
/// A value the column equals: a low and a high bound in one.
pub const IC_BOUND_EQ: u32 = 4;
/// The largest range number, twelve bits of a range's first word.
pub const IC_RANGE_NO_MAX: u32 = 0xFFF;
/// The most words a range may take: its length is the high half of its
/// first word.
pub const IC_RANGE_MAX_WORDS: usize = 0xFFFF;
/// The pseudo column that reads the number of the range a row was
/// found in, one word, first in the row when asked for.
/// Verify: chapter 05 §7.4.
pub const IC_COL_RANGE_NO: u32 = 0xFFFB;

/// Append one bound of a range to `out`: its type, the header of the
/// column, by its place in the index, with the value's size in bytes,
/// and the value, as the row keeps it, padded to whole words. A NULL
/// value has size 0 and no words.
pub fn bound_into(
  out: &mut Vec<u32>,
  bound_type: u32,
  index_column: u32,
  value: Option<&[u8]>,
) {
  out.push(bound_type);
  let bytes = value.unwrap_or(&[]);
  out.push(crate::column_header::column_header(
    index_column,
    bytes.len() as u32,
  ));
  let mut i: usize = 0;
  while i < bytes.len() {
    let mut word = [0u8; 4];
    let take = (bytes.len() - i).min(4);
    word[..take].copy_from_slice(&bytes[i..i + take]);
    out.push(u32::from_ne_bytes(word));
    i += 4;
  }
}

/// Append a range that takes in every row to `out`: the first column at
/// least NULL, which every value is, as the reference sends it.
pub fn open_range_into(out: &mut Vec<u32>) {
  bound_into(out, IC_BOUND_LE, 0, None);
}

/// Finish the range that starts at `start` in `bounds`: its first word
/// takes the range's length in words, in its high half, and its number,
/// in bits 4 to 15. Refused for a number above [`IC_RANGE_NO_MAX`], a
/// range of more than [`IC_RANGE_MAX_WORDS`] words, or one with no
/// bound at all.
pub fn finish_range(
  bounds: &mut [u32],
  start: usize,
  range_no: u32,
) -> Result<(), IcError> {
  let bad = IcError::new(err::IC_ERROR_BAD_RANGE);
  if start >= bounds.len() || range_no > IC_RANGE_NO_MAX {
    return Err(bad);
  }
  let len = bounds.len() - start;
  if len > IC_RANGE_MAX_WORDS {
    return Err(bad);
  }
  bounds[start] |= ((len as u32) << 16) | (range_no << 4);
  Ok(())
}

// ---- SCAN_CONF ----

/// Words in a `SCAN_CONF` before its entries.
pub const IC_SCAN_CONF_LEN: usize = 4;
/// In the second word: the scan has ended. With no entries, it
/// confirms a close.
pub const IC_SCAN_CONF_END_OF_DATA: u32 = 1 << 31;
/// In the second word: how many receivers the confirmation reports.
pub const IC_SCAN_CONF_COUNT_MASK: u32 = 0x7FFF_FFFF;
/// The coordinator's pointer for a receiver that is done: the null
/// record.
pub const IC_SCAN_RECEIVER_DONE: u32 = 0xFFFF_FF00;
/// Entries a `SCAN_CONF` decodes without allocating. The coordinator
/// sends one entry a confirmation as a rule (chapter 05 §7.4); one with
/// more than this still decodes, into a vector.
pub const IC_SCAN_CONF_INLINE_ENTRIES: usize = 8;
/// In the short form: rows all ones means a fourth word holds them.
const IC_SCAN_SHORT_ROWS_MASK: u32 = 0x3FF;

/// One receiver's batch, as a `SCAN_CONF` reports it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanBatch {
  /// Our id for the receiver.
  pub receiver_id: u32,
  /// The coordinator's pointer for it, which `SCAN_MORE_REQ` names, or
  /// [`IC_SCAN_RECEIVER_DONE`].
  pub receiver_ptr: u32,
  /// Rows in the batch.
  pub rows: u32,
  /// Words in the batch: the rows' words and the keys' words.
  pub words: u32,
}

impl ScanBatch {
  /// True if the receiver has no more to give: it is never named again.
  pub fn is_done(&self) -> bool {
    self.receiver_ptr == IC_SCAN_RECEIVER_DONE
  }
}

/// The coordinator reports batches, or the end of the scan.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ScanConf {
  /// Our pointer for the scan's record, as the allocate gave it.
  pub client_ptr: u32,
  /// End of data and the count of entries.
  pub info_word: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The entries, the first `num_batches` of them; see
  /// [`batches`](Self::batches).
  pub inline: [ScanBatch; IC_SCAN_CONF_INLINE_ENTRIES],
  /// How many entries it carries.
  pub num_batches: usize,
  /// All the entries instead, for a confirmation with more than fit
  /// inline; empty, and so no allocation, otherwise.
  pub more: Vec<ScanBatch>,
}

impl ScanConf {
  /// Read a confirmation. The entries are inline, or in `section0` for a
  /// long one. `extended` says which form the request asked for: five
  /// words an entry, or four for an API older than 8.0.20; the short
  /// form otherwise, three words, a fourth for a batch of 1 023 rows or
  /// more. Verify: chapter 05 §7.2.
  pub fn decode(
    data: &[u32],
    section0: Option<&[u32]>,
    extended: bool,
  ) -> Result<ScanConf, IcError> {
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if data.len() < IC_SCAN_CONF_LEN {
      return Err(bad);
    }
    let info_word = data[1];
    let count = (info_word & IC_SCAN_CONF_COUNT_MASK) as usize;
    let entries = match section0 {
      Some(section) => section,
      None => &data[IC_SCAN_CONF_LEN..],
    };
    let mut conf = ScanConf {
      client_ptr: data[0],
      info_word,
      trans_id_lo: data[2],
      trans_id_hi: data[3],
      inline: [ScanBatch::default(); IC_SCAN_CONF_INLINE_ENTRIES],
      num_batches: 0,
      more: Vec::new(),
    };
    if count > IC_SCAN_CONF_INLINE_ENTRIES {
      conf.more.reserve_exact(count);
    }
    let mut stride: usize = 5;
    if extended && entries.len() == 4 * count {
      stride = 4;
    }
    let mut at: usize = 0;
    let mut i: usize = 0;
    while i < count {
      let entry = if extended {
        extended_entry(entries, at, stride)
      } else {
        short_entry(entries, at)
      };
      let (batch, used) = match entry {
        Some(entry) => entry,
        None => return Err(bad),
      };
      conf.push(batch);
      at += used;
      i += 1;
    }
    Ok(conf)
  }

  fn push(&mut self, batch: ScanBatch) {
    if self.more.capacity() > 0 {
      self.more.push(batch);
    } else {
      self.inline[self.num_batches] = batch;
    }
    self.num_batches += 1;
  }

  /// The batches reported.
  pub fn batches(&self) -> &[ScanBatch] {
    if !self.more.is_empty() {
      return &self.more;
    }
    &self.inline[..self.num_batches]
  }

  /// True if the scan has ended, with the batches reported here its
  /// last.
  pub fn end_of_data(&self) -> bool {
    self.info_word & IC_SCAN_CONF_END_OF_DATA != 0
  }

  /// True for the bare end of data that confirms a close: no entries.
  pub fn confirms_close(&self) -> bool {
    self.end_of_data() && self.num_batches == 0
  }
}

/// An entry of the extended form at `at`, and the words it takes:
/// `stride`, five, or four for an older API, whose entry has no mask.
fn extended_entry(
  entries: &[u32],
  at: usize,
  stride: usize,
) -> Option<(ScanBatch, usize)> {
  if at + stride > entries.len() {
    return None;
  }
  let batch = ScanBatch {
    receiver_id: entries[at],
    receiver_ptr: entries[at + 1],
    rows: entries[at + 2],
    words: entries[at + 3],
  };
  Some((batch, stride))
}

/// An entry of the short form at `at`, and the words it takes: three,
/// or four when its rows are all ones and the fourth word holds them.
fn short_entry(entries: &[u32], at: usize) -> Option<(ScanBatch, usize)> {
  if at + 3 > entries.len() {
    return None;
  }
  let third = entries[at + 2];
  let mut batch = ScanBatch {
    receiver_id: entries[at],
    receiver_ptr: entries[at + 1],
    rows: third & IC_SCAN_SHORT_ROWS_MASK,
    words: third >> 10,
  };
  if batch.rows != IC_SCAN_SHORT_ROWS_MASK {
    return Some((batch, 3));
  }
  if at + 4 > entries.len() {
    return None;
  }
  batch.rows = entries[at + 3];
  Some((batch, 4))
}

// ---- SCAN_REF ----

/// Words in a `SCAN_REF`.
pub const IC_SCAN_REF_LEN: usize = 5;

/// The coordinator refuses a scan, or ends it with an error.
/// Verify: chapter 05 §7.2.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanRef {
  /// Our pointer for the scan's record.
  pub client_ptr: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
  /// The NDB error code.
  pub error_code: u32,
  /// True if the scan still holds the coordinator's record: it is
  /// closed with a `SCAN_MORE_REQ` with stop, and the record is free
  /// once the close is confirmed. False if it is over already.
  pub close_owed: bool,
}

impl ScanRef {
  /// Read a refusal. The last word may be missing, owing nothing.
  pub fn decode(data: &[u32]) -> Result<ScanRef, IcError> {
    if data.len() < IC_SCAN_REF_LEN - 1 {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let mut close_owed = false;
    if data.len() >= IC_SCAN_REF_LEN {
      close_owed = data[4] != 0;
    }
    Ok(ScanRef {
      client_ptr: data[0],
      trans_id_lo: data[1],
      trans_id_hi: data[2],
      error_code: data[3],
      close_owed,
    })
  }
}

// ---- SCAN_MORE_REQ ----

/// Fixed words in a `SCAN_MORE_REQ`.
pub const IC_SCAN_MORE_REQ_LEN: usize = 4;
/// Receivers a `SCAN_MORE_REQ` names inline; more go in section 0.
pub const IC_SCAN_MORE_REQ_INLINE_PTRS: usize = 21;
/// The pointer a close after a `SCAN_REF` names: the coordinator reads
/// no pointer of a close, but the request goes whole, five words, as
/// the reference sends it (the author, chapter 05 §7.2).
pub const IC_SCAN_CLOSE_PTR: u32 = 0xFFFF_FFFF;

/// Let receivers go on, or close the scan.
/// Verify: chapter 05 §7.2.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanMoreReq {
  /// The coordinator's pointer for the scan's record.
  pub coord_ptr: u32,
  /// Close the scan: the coordinator ignores the pointers.
  pub stop: bool,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl ScanMoreReq {
  /// The request into `out`, cleared first, with the coordinator's
  /// pointers for the receivers to go on. True if they went inline;
  /// false if there are more than [`IC_SCAN_MORE_REQ_INLINE_PTRS`], for
  /// the caller to send them as section 0.
  pub fn encode_into(&self, ptrs: &[u32], out: &mut Vec<u32>) -> bool {
    out.clear();
    out.push(self.coord_ptr);
    out.push(self.stop as u32);
    out.push(self.trans_id_lo);
    out.push(self.trans_id_hi);
    if ptrs.len() > IC_SCAN_MORE_REQ_INLINE_PTRS {
      return false;
    }
    out.extend_from_slice(ptrs);
    true
  }
}

// ---- SCAN_ROW_KEY_REP ----

/// Words in front of the key in a `SCAN_ROW_KEY_REP`.
pub const IC_SCAN_ROW_KEY_REP_LEN: usize = 5;

/// The key of a row a locking scan sent, so that it may be taken over.
/// Verify: chapter 05 §7.2.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanRowKeyRep {
  /// Our id for the receiver.
  pub receiver_id: u32,
  /// Words of key.
  pub key_len: u32,
  /// The row: its operation number above bit 8, the scan's number
  /// below, the fragment from bit 20.
  pub row_word: u32,
  /// The transaction id, low word.
  pub trans_id_lo: u32,
  /// The transaction id, high word.
  pub trans_id_hi: u32,
}

impl ScanRowKeyRep {
  /// Read the front of a key.
  pub fn decode(data: &[u32]) -> Result<ScanRowKeyRep, IcError> {
    if data.len() < IC_SCAN_ROW_KEY_REP_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(ScanRowKeyRep {
      receiver_id: data[0],
      key_len: data[1],
      row_word: data[2],
      trans_id_lo: data[3],
      trans_id_hi: data[4],
    })
  }

  /// The key: section 0 of a long signal, or what follows the front in
  /// a short one.
  pub fn key<'a>(data: &'a [u32], section0: Option<&'a [u32]>) -> &'a [u32] {
    if let Some(section) = section0 {
      return section;
    }
    if data.len() <= IC_SCAN_ROW_KEY_REP_LEN {
      return &[];
    }
    &data[IC_SCAN_ROW_KEY_REP_LEN..]
  }

  /// The fragment the row is in.
  pub fn fragment(&self) -> u32 {
    Self::fragment_of(self.row_word)
  }

  /// The row's place in its batch.
  pub fn op_number(&self) -> u32 {
    Self::op_number_of(self.row_word)
  }

  /// The fragment a row word names.
  pub fn fragment_of(row_word: u32) -> u32 {
    row_word >> 20
  }

  /// The place in its batch a row word names: 0 for the batch's first
  /// row, counting up as the rows come (chapter 05 §7.4).
  pub fn op_number_of(row_word: u32) -> u32 {
    (row_word >> 8) & 0x3FF
  }
}

/// The scan information word of a takeover's `KEY_OP_REQ`, from the
/// row word of the row's `SCAN_ROW_KEY_REP`: the takeover flag, the
/// row's low 18 bits above it, the fragment from bit 20. The fragment
/// also goes as the request's distribution key. Verify: chapter 05 §7.2.
pub fn takeover_info(row_word: u32) -> u32 {
  1 | ((row_word & 0x3FFFF) << 1) | ((row_word >> 20) << 20)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_range_carries_its_length_and_number_in_its_first_word() {
    // a >= 5 and a < 9, b = 'xy', as range 3.
    let mut bounds: Vec<u32> = Vec::new();
    bound_into(&mut bounds, IC_BOUND_LE, 0, Some(&5u32.to_le_bytes()));
    bound_into(&mut bounds, IC_BOUND_GT, 0, Some(&9u32.to_le_bytes()));
    bound_into(&mut bounds, IC_BOUND_EQ, 1, Some(&[2, b'x', b'y']));
    finish_range(&mut bounds, 0, 3).expect("a range");
    assert_eq!(bounds.len(), 9);
    assert_eq!(bounds[0], (9 << 16) | (3 << 4) | IC_BOUND_LE);
    assert_eq!(bounds[1], 4);
    assert_eq!(bounds[2], 5);
    assert_eq!(bounds[3], IC_BOUND_GT);
    assert_eq!(bounds[6], IC_BOUND_EQ);
    assert_eq!(bounds[7], (1 << 16) | 3);
    assert_eq!(bounds[8], u32::from_ne_bytes([2, b'x', b'y', 0]));
    // A second range after it starts where the first ended.
    let start = bounds.len();
    open_range_into(&mut bounds);
    finish_range(&mut bounds, start, 4).expect("an open range");
    assert_eq!(&bounds[start..], [(2 << 16) | (4 << 4), 0]);
  }

  #[test]
  fn a_null_bound_has_a_header_of_size_zero() {
    let mut bounds: Vec<u32> = Vec::new();
    bound_into(&mut bounds, IC_BOUND_GE, 2, None);
    assert_eq!(bounds, [IC_BOUND_GE, 2 << 16]);
  }

  #[test]
  fn a_range_that_cannot_be_sent_is_refused() {
    let mut bounds: Vec<u32> = Vec::new();
    open_range_into(&mut bounds);
    let e = finish_range(&mut bounds, 0, IC_RANGE_NO_MAX + 1)
      .expect_err("number too big");
    assert_eq!(e.code, err::IC_ERROR_BAD_RANGE);
    assert!(finish_range(&mut bounds, 2, 0).is_err(), "no bound");
    let mut long: Vec<u32> = vec![0; IC_RANGE_MAX_WORDS + 1];
    assert!(finish_range(&mut long, 0, 0).is_err(), "too long");
    finish_range(&mut bounds, 0, IC_RANGE_NO_MAX).expect("the largest");
  }

  #[test]
  fn the_flags_take_their_bits() {
    // As the reference's committed table scan sends them: no disk,
    // committed, storage order, 384 rows in bits 16 to 25.
    let committed = ScanFlags {
      no_disk: true,
      committed: true,
      storage_order: true,
      batch_rows: batch_rows_field(384),
      ..ScanFlags::default()
    };
    assert_eq!(committed.flags_word(), 0x0180_2A00);
    // Its exclusive scan: exclusive, hold lock, keys as well.
    let exclusive = ScanFlags {
      exclusive: true,
      no_disk: true,
      hold_lock: true,
      keys: true,
      storage_order: true,
      batch_rows: 384,
      ..ScanFlags::default()
    };
    assert_eq!(exclusive.flags_word(), 0x0180_3700);
    // An ordered range scan, descending.
    let range = ScanFlags {
      no_disk: true,
      committed: true,
      range: true,
      descending: true,
      batch_rows: 384,
      ..ScanFlags::default()
    };
    assert_eq!(range.flags_word(), 0x0180_CA00);
    let ours = ScanFlags {
      extended_conf: true,
      dist_key: true,
      ..ScanFlags::default()
    };
    assert_eq!(ours.flags_word(), (1 << 29) | (1 << 26));
    let continuous = ScanFlags {
      continuous: true,
      committed: true,
      ..ScanFlags::default()
    };
    assert_eq!(continuous.flags_word(), (1 << 11) | (1 << 2));
  }

  #[test]
  fn batch_rows_above_992_go_in_word_10_alone() {
    assert_eq!(batch_rows_field(384), 384);
    assert_eq!(batch_rows_field(992), 992);
    assert_eq!(batch_rows_field(993), 0);
    assert_eq!(batch_rows_field(4000), 0);
  }

  #[test]
  fn a_request_is_eleven_words_or_twelve_with_a_key() {
    let req = ScanReq {
      coord_ptr: 2,
      flags_word: 0x0180_2A00,
      table_id: 13,
      table_version: 1,
      word5: IC_SCAN_WORD5_PLAIN,
      trans_id_lo: 0x10,
      trans_id_hi: 0x20,
      owner_ptr: IC_SCAN_NO_OWNER,
      batch_bytes: 32768,
      batch_rows: 384,
      dist_key: None,
    };
    assert_eq!(req.len(), 11);
    let words = req.encode();
    assert_eq!(
      words[..req.len()],
      [
        2,
        0,
        0x0180_2A00,
        13,
        1,
        0xFFFF,
        0x10,
        0x20,
        0xFFFF_FFFF,
        32768,
        384
      ]
    );
    let pruned = ScanReq {
      dist_key: Some(7),
      ..req
    };
    assert_eq!(pruned.len(), 12);
    assert_eq!(pruned.encode()[11], 7);
  }

  #[test]
  fn the_column_section_leads_with_five_sizes() {
    let mut out = vec![99u32];
    // A read by bitmap, as a key read sends it, and no program.
    column_section_into(&[(0xFFF3 << 16) | 4, 0b110], &[], &[], &mut out);
    assert_eq!(out, [2, 0, 0, 0, 0, (0xFFF3 << 16) | 4, 0b110]);
    column_section_into(&[], &[1, 2, 3], &[4], &mut out);
    assert_eq!(out, [0, 3, 0, 0, 1, 1, 2, 3, 4]);
  }

  #[test]
  fn an_extended_confirmation_has_five_words_an_entry() {
    let data = [0x8000_0002, 1, 0x10, 0x20, 0x38, 7, 384, 1152, 0xDEAD];
    let conf = ScanConf::decode(&data, None, true).expect("read");
    assert_eq!(conf.client_ptr, 0x8000_0002);
    assert!(!conf.end_of_data());
    assert_eq!(conf.batches().len(), 1);
    let batch = conf.batches()[0];
    assert_eq!(batch.receiver_id, 0x38);
    assert_eq!(batch.receiver_ptr, 7);
    assert_eq!(batch.rows, 384);
    assert_eq!(batch.words, 1152);
    assert!(!batch.is_done());
  }

  #[test]
  fn an_older_api_gets_four_words_an_entry() {
    let data = [2, 2, 0x10, 0x20, 0x38, 7, 10, 30, 0x34, 0xFFFF_FF00, 5, 15];
    let conf = ScanConf::decode(&data, None, true).expect("read");
    assert_eq!(conf.batches().len(), 2);
    assert_eq!(conf.batches()[1].rows, 5);
    assert_eq!(conf.batches()[1].words, 15);
    assert!(conf.batches()[1].is_done());
  }

  #[test]
  fn the_short_form_reads_as_the_reference_gets_it() {
    // Seen on the wire: 122 rows in 366 words, done; then a batch of
    // 2 000 rows, whose count takes a fourth word.
    let data = [
      0,
      (1 << 31) | 2,
      0,
      0x0020_C000,
      0x38,
      0xFFFF_FF00,
      (366 << 10) | 122,
      0x34,
      3,
      (6000 << 10) | 0x3FF,
      2000,
    ];
    let conf = ScanConf::decode(&data, None, false).expect("read");
    assert!(conf.end_of_data());
    assert!(!conf.confirms_close());
    let batches = conf.batches();
    assert_eq!(batches.len(), 2);
    assert_eq!((batches[0].rows, batches[0].words), (122, 366));
    assert!(batches[0].is_done());
    assert_eq!((batches[1].rows, batches[1].words), (2000, 6000));
  }

  #[test]
  fn a_bare_end_of_data_confirms_a_close() {
    let conf = ScanConf::decode(&[0, 1 << 31, 0x10, 0x20], None, true);
    let conf = conf.expect("read");
    assert!(conf.confirms_close());
    assert!(conf.batches().is_empty());
  }

  #[test]
  fn entries_may_come_in_section_0_and_many() {
    let count: u32 = IC_SCAN_CONF_INLINE_ENTRIES as u32 + 4;
    let mut section: Vec<u32> = Vec::new();
    let mut i: u32 = 0;
    while i < count {
      section.extend_from_slice(&[0x100 + i, i, 1, 3, 0]);
      i += 1;
    }
    let data = [2, count, 0x10, 0x20];
    let conf = ScanConf::decode(&data, Some(&section), true).expect("read");
    assert_eq!(conf.batches().len(), count as usize);
    assert_eq!(
      conf.batches()[count as usize - 1].receiver_id,
      0x100 + count - 1
    );
  }

  #[test]
  fn a_confirmation_short_of_its_entries_is_an_error() {
    let data = [2, 2, 0x10, 0x20, 0x38, 7, 10, 30, 0];
    assert!(ScanConf::decode(&data, None, true).is_err());
    assert!(ScanConf::decode(&[2, 1], None, true).is_err());
  }

  #[test]
  fn a_refusal_says_whether_a_close_is_owed() {
    let owed = ScanRef::decode(&[2, 0x10, 0x20, 274, 1]).expect("read");
    assert_eq!(owed.error_code, 274);
    assert!(owed.close_owed);
    let over = ScanRef::decode(&[2, 0x10, 0x20, 4003, 0]).expect("read");
    assert!(!over.close_owed);
    let short = ScanRef::decode(&[2, 0x10, 0x20, 4003]).expect("read");
    assert!(!short.close_owed);
  }

  #[test]
  fn more_names_its_receivers_inline_up_to_21() {
    let req = ScanMoreReq {
      coord_ptr: 2,
      stop: false,
      trans_id_lo: 0x10,
      trans_id_hi: 0x20,
    };
    let mut out: Vec<u32> = Vec::new();
    assert!(req.encode_into(&[7, 3, 2], &mut out));
    assert_eq!(out, [2, 0, 0x10, 0x20, 7, 3, 2]);
    let many = [1u32; 22];
    assert!(!req.encode_into(&many, &mut out));
    assert_eq!(out, [2, 0, 0x10, 0x20]);
    // The close after a refusal: five words, as the reference sends it.
    let close = ScanMoreReq { stop: true, ..req };
    assert!(close.encode_into(&[IC_SCAN_CLOSE_PTR], &mut out));
    assert_eq!(out, [2, 1, 0x10, 0x20, 0xFFFF_FFFF]);
  }

  #[test]
  fn a_row_key_names_its_row_and_fragment() {
    // Seen on the wire: fragment 4, row 3 of the batch, scan 0xC0.
    let data = [0x28, 1, 0x0040_03C0, 0x10, 0x20, 77];
    let rep = ScanRowKeyRep::decode(&data).expect("read");
    assert_eq!(rep.receiver_id, 0x28);
    assert_eq!(rep.fragment(), 4);
    assert_eq!(rep.op_number(), 3);
    assert_eq!(ScanRowKeyRep::key(&data, None), &[77]);
    let section = [5u32, 6];
    let long = &data[..IC_SCAN_ROW_KEY_REP_LEN];
    assert_eq!(ScanRowKeyRep::key(long, Some(&section[..])), &[5, 6]);
  }

  #[test]
  fn the_takeover_word_is_built_from_the_row_word() {
    // Seen on the wire: row word 0xC0 gives 0x181, fragment 0.
    assert_eq!(takeover_info(0x0000_00C0), 0x181);
    assert_eq!(takeover_info(0x0000_01C0), 0x381);
    // Fragment 4 stays in the top bits.
    assert_eq!(takeover_info(0x0040_00C0), 0x0040_0181);
  }
}
