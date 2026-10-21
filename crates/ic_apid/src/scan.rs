// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Scans (chapter 02, "Data flow for a scan"; chapter 04, "Scans"): a
//! table read fragment by fragment, in batches, through a coordinator,
//! by the protocol of chapter 05 §7.
//!
//! **A scan is made once and run many times**, as a query is. It owns
//! its row, laid out by its record, and keeps its receivers and their
//! batch buffers from run to run, so that a run after the first
//! allocates nothing.
//!
//! **A run** takes a coordinator record of its own from the pool of the
//! node and instance it runs at, sends `SCAN_REQ` with one receiver per
//! fragment scanned in parallel, and lets each receiver go on only once
//! the application has handed its batch back. A batch is complete when
//! its `SCAN_CONF` entry has come and as many words as the entry says,
//! of rows and keys, which come straight from the nodes that scan the
//! fragments, before or after the entry.
//!
//! **A run ends** when every receiver has given its last batch and it
//! has been handed back, when it is closed, or when it fails. A close
//! stops at once, without waiting for the batches asked for, unless the
//! scan's transaction goes on and takeovers of its rows are still to be
//! received (phase 10; the author, 2026-10-21). The record goes back to
//! its pool once the coordinator confirms. Rows that come after the
//! end, of batches the close did not wait for, find their receiver's id
//! stale and are dropped, counted as late rather than unexpected.
//!
//! A scan started on a transaction, so that its rows may be taken over,
//! runs at the transaction's coordinator instance, under its id, and
//! names its record; any other scan names none and runs under an id of
//! its own (the author, 2026-10-16).
//!
//! **A takeover** (phase 8.5) locks, updates or deletes the row last
//! read from a locking scan, as a query on the scan's transaction whose
//! key is the row's, as the data node gave it with the row. The lock
//! moves from the scan to the transaction. The batch's receiver asks for
//! more only once every takeover of the batch is done, confirmed or
//! failed, since asking releases the batch's locks (chapter 05 §7.2).
//!
//! **A scan of an ordered index** (phase 8.7) reads the ranges added
//! before its run ([`index_scan`](crate::index_scan)), and an ordered
//! run merges its receivers' rows in the index's order: every fragment
//! has a receiver, and the merge needs a row or the end from each
//! before it gives one. Each receiver's next row is unpacked once, with
//! its order key ([`IndexOrder`]) and its range number, and the row
//! that comes first is taken. A receiver whose batch is used up is
//! handed back, and the merge waits for its next batch, as the
//! reference's does (chapter 05 §7.2).

use std::sync::Arc;

use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_signals::blocks;
use ic_signals::column_header;
use ic_signals::coord_alloc::IC_ANY_COORD_INSTANCE;
use ic_signals::header::SignalHeader;
use ic_signals::key_ops;
use ic_signals::key_ops::KeyOpFlags;
use ic_signals::key_ops::RowDataRep;
use ic_signals::numbers;
use ic_signals::scans;
use ic_signals::scans::ScanBatch;
use ic_signals::scans::ScanConf;
use ic_signals::scans::ScanFlags;
use ic_signals::scans::ScanMoreReq;
use ic_signals::scans::ScanRef;
use ic_signals::scans::ScanReq;
use ic_signals::scans::ScanRowKeyRep;
use ic_signals::table_props::IC_STORAGE_DISK;
use ic_util::ptr_array::PtrId;

use crate::apid_conn::coord_index_of;
use crate::apid_conn::ApidConnection;
use crate::apid_conn::CoordRecord;
use crate::apid_conn::IC_CALL_SLICE_MS;
use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::index_order::IndexOrder;
use crate::index_scan::IndexScan;
use crate::index_scan::RangeSet;
use crate::index_scan::ScanRange;
use crate::node_connect::SignalView;
use crate::program;
use crate::program::Program;
use crate::program::IC_PROGRAM_INPUTS;
use crate::query::AbortOption;
use crate::query::QueryCallback;
use crate::query::QueryId;
use crate::query::TransId;
use crate::record::Record;
use crate::row_codec;
use crate::transaction::CommitState;
use crate::transaction::TransactionHint;
use crate::transaction::IC_NDB_ERROR_SCHEMA_CHANGED;
use crate::transaction::IC_NDB_ERROR_UNKNOWN_TABLE;

/// Marks a receiver's id, in the first word of `ROW_DATA_REP`, apart
/// from a query's: bit 23, the top bit of an object map's index, which
/// no query's id has (chapter 02).
pub(crate) const IC_RECEIVER_TAG: u32 = 0x0080_0000;

/// NDB's error for a scan whose coordinator's node failed.
pub const IC_NDB_ERROR_SCAN_NODE_FAILURE: i32 = 4028;
/// NDB's error for a scan the coordinator has no room for: too many
/// scans run at once. Temporary.
pub const IC_NDB_ERROR_TOO_MANY_SCANS: i32 = 245;
/// NDB's error for a scan that waited too long for a lock, probably in
/// a deadlock. Temporary.
pub const IC_NDB_ERROR_SCAN_LOCK_TIMEOUT: i32 = 274;
/// NDB's error for a scan left too long without asking for more, as
/// when a batch is not handed back (chapter 05 §7.2). Temporary.
pub const IC_NDB_ERROR_SCAN_IDLE: i32 = 296;

/// True if an object map id fits below the receiver tag, as a query's
/// and a receiver's must.
pub(crate) fn fits_below_tag(id: PtrId) -> bool {
  id.index() & IC_RECEIVER_TAG == 0
}

/// A scan's id in its connection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScanId(pub(crate) PtrId);

impl ScanId {
  /// The id as a word, for a language binding.
  pub fn as_u32(&self) -> u32 {
    self.0.as_u32()
  }

  /// A scan id from its word, as a language binding hands it back.
  /// Whether it still names a live scan is for the connection to say.
  pub fn from_u32(word: u32) -> ScanId {
    ScanId(PtrId::from_u32(word))
  }
}

/// A batch a receiver has delivered: the scan, and the receiver's id
/// in the run that delivered it. A run gives its receivers ids anew, so
/// a batch of an earlier run is never taken for one of this.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScanBatchId {
  scan: ScanId,
  receiver: u32,
}

impl ScanBatchId {
  /// The scan the batch is of.
  pub fn scan(&self) -> ScanId {
    self.scan
  }

  /// The batch as one word, for a language binding: the scan's id in
  /// the high half and the receiver's in the low.
  pub fn as_u64(&self) -> u64 {
    ((self.scan.as_u32() as u64) << 32) | self.receiver as u64
  }

  /// A batch from its word, as a language binding hands it back.
  /// Whether it is still with the application is for the connection to
  /// say.
  pub fn from_u64(word: u64) -> ScanBatchId {
    ScanBatchId {
      scan: ScanId::from_u32((word >> 32) as u32),
      receiver: word as u32,
    }
  }
}

/// What is called when a batch is complete, in place of the ready list:
/// on the thread that polls, from inside the poll, with the connection,
/// the batch and the scan's `user_ref`. The callback may read the
/// batch's rows and hand it back, or keep it and hand it back later.
pub type ScanCallback = fn(&mut ApidConnection, ScanBatchId, usize);

/// How a scan locks the rows it reads.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ScanLock {
  /// The last committed value of each row, with no lock.
  #[default]
  Committed,
  /// A shared lock on each row of a batch, until the batch is handed
  /// back.
  Shared,
  /// An exclusive lock on each row of a batch, until the batch is
  /// handed back.
  Exclusive,
}

/// What a scan asks for when it is started.
#[derive(Clone, Copy, Debug, Default)]
pub struct ScanArgs {
  /// How to lock.
  pub lock: ScanLock,
  /// For a locking scan whose rows may be taken over: the transaction
  /// they are taken over into, on this connection. The scan then runs
  /// at its coordinator, under its id. None for any other scan.
  pub trans: Option<TransId>,
  /// Fragments scanned at once, one receiver each; 0 for every
  /// fragment.
  pub parallelism: u32,
  /// Rows a receiver's batch may hold; 0 for the configuration's.
  pub batch_rows: u32,
  /// Bytes a receiver's batch may hold; 0 for the configuration's.
  pub batch_bytes: u32,
  /// Scan this partition only, by its id.
  pub partition: Option<u32>,
  /// For a scan of an ordered index: the rows in the index's order,
  /// merged from every fragment, read with `next_row` (chapter 04,
  /// "Index scans").
  pub ordered: bool,
  /// For a scan of an ordered index: from the high end of each range.
  pub descending: bool,
  /// Close the scan with `IC_ERROR_TIMEOUT` if it is still running this
  /// long after it was started, in milliseconds; 0 for no limit.
  pub timeout_ms: u32,
  /// For a committed read through an executor: let a receiver go on as
  /// soon as its batch has moved to the reader, rather than once the
  /// reader hands it back (chapter 04, "Consuming scans"). Refused for
  /// a locking scan, and on a connection, where the application hands
  /// each batch back itself.
  pub read_ahead: bool,
  /// Anything, given back to the callback.
  pub user_ref: usize,
  /// Called with each batch as it completes, in place of the ready
  /// list.
  pub callback: Option<ScanCallback>,
  /// A word for a language binding's own use; see
  /// [`crate::query::ReadKeyArgs::binding`].
  pub binding: usize,
}

/// What a takeover of a scanned row asks for (chapter 04, "Scans").
#[derive(Clone, Copy, Debug, Default)]
pub struct TakeoverArgs {
  /// `Default` and `AbortOnError` roll the transaction back if the
  /// takeover fails, as the reference's takeovers do; `IgnoreError`
  /// fails the query alone.
  pub abort_option: AbortOption,
  /// Anything, given back to the callback.
  pub user_ref: usize,
  /// Called when the takeover completes, in place of the executed list.
  pub callback: Option<QueryCallback>,
  /// A word for a language binding's own use; see
  /// [`crate::query::ReadKeyArgs::binding`].
  pub binding: usize,
}

/// The receiver a takeover holds back: the scan, and its receiver's id
/// in the run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct TakeoverOf {
  pub scan: ScanId,
  pub receiver: u32,
}

/// What a takeover's request is made of: the row's key as the data node
/// gave it, the scan information word, the row's fragment, and the
/// receiver it holds back.
#[derive(Debug)]
pub(crate) struct TakeoverKey {
  pub key: Vec<u32>,
  pub info: u32,
  pub fragment: u32,
  pub of: TakeoverOf,
}

/// Where a scan is in its life.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ScanState {
  /// Ready to be started.
  #[default]
  Idle,
  /// Started, waiting for a record at its coordinator.
  Starting,
  /// The request has gone; batches come.
  Running,
  /// A close was asked for, or an error ends the run: the stop sent,
  /// or waiting for takeovers to go first, then for the coordinator to
  /// confirm the close.
  Closing,
  /// The run is over: read to the end, closed, or failed.
  Done,
}

/// Where a receiver is in a run.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum ReceiverState {
  /// A batch has been asked for and is not yet complete.
  #[default]
  Asked,
  /// The batch is complete and with the application.
  Ready,
  /// Its batch is held at the coordinator, its rows dropped: the run is
  /// closing.
  Held,
  /// It gives no more.
  Done,
}

/// The object map's entry for a receiver: its scan and its place there.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ReceiverSlot {
  pub scan: ScanId,
  pub place: u32,
}

/// One receiver of a scan: a fragment scanned, a batch at a time.
#[derive(Debug, Default)]
pub(crate) struct Receiver {
  /// Its id in this run, tagged, as the request and its rows carry it;
  /// 0 outside a run.
  id: u32,
  /// The coordinator's pointer for it, from its last entry.
  coord_ptr: u32,
  state: ReceiverState,
  /// The entry for the batch asked for has come.
  confirmed: bool,
  /// That entry named the null record: nothing more after this batch.
  last: bool,
  /// Rows and words the entry says the batch holds.
  conf_rows: u32,
  conf_words: u32,
  /// Words of the batch that have come, of rows and keys.
  words: u32,
  /// The batch's rows, packed as they came, one after another.
  rows: Vec<u32>,
  /// Where each row ends in `rows`.
  row_ends: Vec<u32>,
  /// The next row to read.
  next_row: u32,
  /// Handed back, and its request for more not yet sent.
  unsent: bool,
  /// A locking scan's keys, one after another, where each ends, and
  /// each row's word, for a takeover: the n-th key is the n-th row's, as
  /// they come in the same order, and its word's operation number says
  /// so (chapter 05 §7.4).
  keys: Vec<u32>,
  key_ends: Vec<u32>,
  key_rows: Vec<u32>,
  /// Takeovers of rows of the batch not yet done; the receiver asks for
  /// more only once there are none.
  takeovers: u32,
  /// For an ordered run's merge: the row `head_at`, unpacked, with its
  /// order key and range number; `IC_NO_HEAD` when none is.
  head_at: u32,
  head_row: Vec<u8>,
  head_key: Vec<u8>,
  head_range: u32,
}

/// No row of the receiver's batch is unpacked for the merge.
const IC_NO_HEAD: u32 = u32::MAX;

impl Receiver {
  /// Ready for the next batch, its buffers kept.
  fn clear_batch(&mut self) {
    self.unsent = false;
    self.confirmed = false;
    self.conf_rows = 0;
    self.conf_words = 0;
    self.words = 0;
    self.rows.clear();
    self.row_ends.clear();
    self.next_row = 0;
    self.keys.clear();
    self.key_ends.clear();
    self.key_rows.clear();
    self.head_at = IC_NO_HEAD;
  }

  /// Where the row `at` of the batch starts and ends in `rows`.
  fn row_span(&self, at: usize) -> (usize, usize) {
    let mut start: usize = 0;
    if at > 0 {
      start = self.row_ends[at - 1] as usize;
    }
    (start, self.row_ends[at] as usize)
  }

  /// True if the batch is with the application and has a row not yet
  /// read.
  fn has_row(&self) -> bool {
    self.state == ReceiverState::Ready
      && (self.next_row as usize) < self.row_ends.len()
  }
}

/// A scan of a table (chapter 04, "Scans").
pub struct Scan {
  table: Arc<TableDef>,
  rec: Record,
  row: Vec<u8>,
  /// What the request reads, made from the record once.
  read_part: Vec<u32>,
  state: ScanState,
  error: Option<IcError>,
  user_ref: usize,
  callback: Option<ScanCallback>,
  binding: usize,
  lock: ScanLock,
  /// The transaction its rows may be taken over into.
  trans: Option<TransId>,
  /// The coordinator's pointer for that transaction's record, or
  /// `scans::IC_SCAN_NO_OWNER`.
  owner_ptr: u32,
  trans_id: u64,
  /// The node and coordinator instance the run goes to, the instance as
  /// asked for, which names the record's pool.
  node_id: u32,
  instance: u32,
  partition: Option<u32>,
  batch_rows: u32,
  batch_bytes: u32,
  /// The run's record at its coordinator, while it has one.
  record: Option<CoordRecord>,
  /// The receivers; the first `used` take part in the run.
  receivers: Vec<Receiver>,
  used: u32,
  /// On the connection's list for the next send.
  queued: bool,
  /// The stop has gone; the bare end of data ends the run.
  stop_sent: bool,
  /// The coordinator is done with the run: it ended or was refused
  /// there, and the record is free again.
  coord_done: bool,
  /// When the run is closed for taking too long, by the high-resolution
  /// clock; 0 for never.
  deadline: u64,
  /// Rows read from the batches of the run so far.
  rows_read: u64,
  /// The batch [`next_row`](ApidConnection::next_row) reads from.
  reading: Option<ScanBatchId>,
  /// The receiver, by place and id, whose batch the row last read came
  /// from: the row a takeover acts on.
  last_read: Option<(u32, u32)>,
  /// For a scan of an ordered index: its ranges and order.
  index: Option<IndexScan>,
  /// The run merges its receivers' rows in the index's order.
  ordered: bool,
  /// The run reads each range from its high end.
  descending: bool,
  /// The run reads each row's range number.
  reads_range_no: bool,
  /// The range number of the row last read.
  range_no: u32,
  /// The program that filters the rows, if any, and its inputs.
  program: Option<Arc<Program>>,
  inputs: [i64; IC_PROGRAM_INPUTS as usize],
}

impl std::fmt::Debug for Scan {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "Scan({}, {:?}, {} receivers{})",
      self.table.name(),
      self.state,
      self.used,
      match self.error {
        Some(e) => format!(", error {}", e.code),
        None => String::new(),
      }
    )
  }
}

impl Scan {
  /// A scan of `table` reading the columns of `rec`, which must be over
  /// this version of it.
  pub fn new(table: &Arc<TableDef>, rec: &Record) -> Result<Scan, IcError> {
    check_scan_record(table, rec)?;
    let mut read_part: Vec<u32> = Vec::new();
    row_codec::read_attr_info_into(rec, &mut read_part);
    Ok(Scan {
      table: Arc::clone(table),
      rec: rec.clone(),
      row: vec![0; rec.row_size() as usize],
      read_part,
      state: ScanState::Idle,
      error: None,
      user_ref: 0,
      callback: None,
      binding: 0,
      lock: ScanLock::Committed,
      trans: None,
      owner_ptr: scans::IC_SCAN_NO_OWNER,
      trans_id: 0,
      node_id: 0,
      instance: IC_ANY_COORD_INSTANCE,
      partition: None,
      batch_rows: 0,
      batch_bytes: 0,
      record: None,
      receivers: Vec::new(),
      used: 0,
      queued: false,
      stop_sent: false,
      coord_done: false,
      deadline: 0,
      rows_read: 0,
      reading: None,
      last_read: None,
      index: None,
      ordered: false,
      descending: false,
      reads_range_no: false,
      range_no: 0,
      program: None,
      inputs: [0; IC_PROGRAM_INPUTS as usize],
    })
  }

  /// A scan of `index`, an ordered index, its ranges laid out by
  /// `key_rec` and its rows read into `rec`, both over the index's
  /// table (chapter 04, "Index scans").
  pub fn new_index(
    index: &Arc<IndexDef>,
    key_rec: &Record,
    rec: &Record,
  ) -> Result<Scan, IcError> {
    let mut scan = Scan::new(index.table(), rec)?;
    scan.index = Some(IndexScan::new(index, key_rec, rec, &scan.read_part)?);
    Ok(scan)
  }

  /// The ordered index it scans, if it scans one.
  pub fn index(&self) -> Option<&Arc<IndexDef>> {
    let index = self.index.as_ref()?;
    Some(index.index())
  }

  /// The number of the range the row last read is in.
  pub fn range_no(&self) -> u32 {
    self.range_no
  }

  /// The number of every row of a run with one range.
  fn single_range_no(&self) -> u32 {
    match &self.index {
      Some(index) => index.run.first_no,
      None => 0,
    }
  }

  /// Refuse what a run of this scan cannot be started with: an order
  /// on a table scan; for an ordered run, a callback, an index whose
  /// order is not known here, or ranges whose numbers do not rise.
  fn check_run(&self, args: &ScanArgs) -> Result<(), IcError> {
    let index = match &self.index {
      Some(index) => index,
      None => {
        if args.ordered || args.descending {
          return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
        }
        return Ok(());
      }
    };
    if !args.ordered {
      return Ok(());
    }
    if args.callback.is_some() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    index.shape.check_ordered(index.pending())
  }

  /// The partition every range added for the next run is in, if they
  /// are all in one.
  fn pruned_partition(&self) -> Option<u32> {
    let index = self.index.as_ref()?;
    index.pending().pruned_partition()
  }

  /// The table it scans.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// The record its rows are laid out by.
  pub fn record(&self) -> &Record {
    &self.rec
  }

  /// The row last read from a batch.
  pub fn row(&self) -> &[u8] {
    &self.row
  }

  /// Where the scan is in its life.
  pub fn state(&self) -> ScanState {
    self.state
  }

  /// True once its run is over, read to the end, closed or failed.
  pub fn is_done(&self) -> bool {
    self.state == ScanState::Done
  }

  /// Why the run failed, or none if it read to the end or was closed.
  pub fn error(&self) -> Option<IcError> {
    self.error
  }

  /// What the run was started with, given back to the callback.
  pub fn user_ref(&self) -> usize {
    self.user_ref
  }

  /// The binding's word the run was started with.
  pub fn binding(&self) -> usize {
    self.binding
  }

  /// Rows read from the run's batches so far.
  pub fn rows_read(&self) -> u64 {
    self.rows_read
  }

  /// Fragments the last run scanned at once, a receiver each: one for a
  /// run of one partition.
  pub fn parallelism(&self) -> u32 {
    self.used
  }

  /// The transaction its rows may be taken over into.
  pub fn transaction(&self) -> Option<TransId> {
    self.trans
  }

  /// True while the run is in progress.
  fn is_running(&self) -> bool {
    self.state == ScanState::Starting
      || self.state == ScanState::Running
      || self.state == ScanState::Closing
  }

  /// True if a takeover of a row of the run is not yet done.
  fn takeovers_pending(&self) -> bool {
    let mut place: usize = 0;
    while place < self.used as usize {
      if self.receivers[place].takeovers > 0 {
        return true;
      }
      place += 1;
    }
    false
  }

  /// True if every receiver of the run gives no more.
  fn all_done(&self) -> bool {
    let mut place: usize = 0;
    while place < self.used as usize {
      if self.receivers[place].state != ReceiverState::Done {
        return false;
      }
      place += 1;
    }
    true
  }

  /// True if any column the record reads is kept on disk.
  fn reads_disk(&self) -> bool {
    let mut position: u32 = 0;
    while position < self.rec.num_fields() {
      if let Some(field) = self.rec.field(position) {
        if let Some(column) = self.table.field(field.field_id()) {
          if column.storage_kind == IC_STORAGE_DISK {
            return true;
          }
        }
      }
      position += 1;
    }
    false
  }

  /// The flags of the run's request (chapter 05 §7.2): committed reads
  /// set 11; shared locks 10, and 12 for keys when the rows may be taken
  /// over; exclusive locks 8, 10 and 12, as the data node requires keys
  /// with them. Every table scan goes in storage order, as the
  /// reference's do; a scan of an ordered index sets 15 instead, and 14
  /// to read from the high end. Every scan asks for the extended
  /// confirmation.
  fn flags(&self) -> ScanFlags {
    let of_index = self.index.is_some();
    let mut flags = ScanFlags {
      no_disk: !self.reads_disk(),
      storage_order: !of_index,
      range: of_index,
      descending: self.descending,
      batch_rows: scans::batch_rows_field(self.batch_rows),
      dist_key: self.partition.is_some(),
      extended_conf: true,
      ..ScanFlags::default()
    };
    match self.lock {
      ScanLock::Committed => flags.committed = true,
      ScanLock::Shared => {
        flags.hold_lock = true;
        flags.keys = self.trans.is_some();
      }
      ScanLock::Exclusive => {
        flags.exclusive = true;
        flags.hold_lock = true;
        flags.keys = true;
      }
    }
    flags
  }
}

/// Refuse a record that is not over this version of the table.
pub(crate) fn check_scan_record(
  table: &TableDef,
  rec: &Record,
) -> Result<(), IcError> {
  let over = rec.table().table_id() == table.table_id()
    && rec.table().table_version() == table.table_version();
  if !over {
    return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
  }
  Ok(())
}

/// The rows and bytes a receiver's batch may hold: the run's own, or
/// the configuration's, the bytes kept within the total for every
/// receiver together, and the rows within what the lock allows
/// (chapter 05 §7.2).
fn batch_size(
  args: &ScanArgs,
  parallelism: u32,
  config_rows: u32,
  config_bytes: u32,
  config_total: u32,
) -> (u32, u32) {
  let mut rows = args.batch_rows;
  if rows == 0 {
    rows = config_rows;
  }
  let most = match args.lock {
    ScanLock::Committed => scans::IC_SCAN_MAX_COMMITTED_ROWS,
    _ => scans::IC_SCAN_MAX_LOCKED_ROWS,
  };
  rows = rows.clamp(1, most);
  let mut bytes = args.batch_bytes;
  if bytes == 0 {
    bytes = config_bytes;
    let share = config_total / parallelism.max(1);
    if share > 0 && share < bytes {
      bytes = share;
    }
  }
  (rows, bytes)
}

/// How many fragments a run scans at once: one for a partition, every
/// fragment for an ordered run, which merges them all, else the scan's
/// own, at most the table's fragments.
fn parallelism_of(args: &ScanArgs, fragments: u32, one_partition: bool) -> u32 {
  if one_partition {
    return 1;
  }
  let all = fragments.max(1);
  if args.ordered || args.parallelism == 0 || args.parallelism > all {
    return all;
  }
  args.parallelism
}

/// A row's words with its range number taken off the front, for a run
/// that reads it; else the words and the number of the run's one range.
pub(crate) fn split_range_no(
  words: &[u32],
  reads: bool,
  single_no: u32,
) -> Result<(u32, &[u32]), IcError> {
  if !reads {
    return Ok((single_no, words));
  }
  let numbered = words.len() >= 2
    && column_header::column_id(words[0]) == scans::IC_COL_RANGE_NO;
  if !numbered {
    return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
  }
  Ok((words[1], &words[2..]))
}

/// Unpack the receiver's next row into its head for the merge, with its
/// order key and range number, unless it is there already.
fn fill_head(
  rec: &Record,
  order: &IndexOrder,
  reads_range_no: bool,
  single_no: u32,
  receiver: &mut Receiver,
) -> Result<(), IcError> {
  let at = receiver.next_row;
  if receiver.head_at == at {
    return Ok(());
  }
  let size = rec.row_size() as usize;
  if receiver.head_row.len() != size {
    receiver.head_row.resize(size, 0);
  }
  let (start, end) = receiver.row_span(at as usize);
  let (range_no, words) =
    split_range_no(&receiver.rows[start..end], reads_range_no, single_no)?;
  row_codec::unpack_row(rec, words, &mut receiver.head_row)?;
  order.key_into(rec, &receiver.head_row, &mut receiver.head_key)?;
  receiver.head_range = range_no;
  receiver.head_at = at;
  Ok(())
}

/// True if receiver `a`'s head row comes before `b`'s: by range number,
/// then by order key, the keys reversed for a run from the high end.
fn comes_before(a: &Receiver, b: &Receiver, descending: bool) -> bool {
  if a.head_range != b.head_range {
    return a.head_range < b.head_range;
  }
  if descending {
    return a.head_key > b.head_key;
  }
  a.head_key < b.head_key
}

impl ApidConnection {
  // ---- Scans ----

  /// A scan of a table, reading the columns of `rec` into its row. It
  /// lives in the connection until freed.
  pub fn create_scan(
    &mut self,
    table: &Arc<TableDef>,
    rec: &Record,
  ) -> Result<ScanId, IcError> {
    let scan = Scan::new(table, rec)?;
    Ok(ScanId(self.scans.insert(scan)?))
  }

  /// A scan of an ordered index (chapter 04, "Index scans"): its ranges
  /// laid out by `key_rec`, which must hold every column of the index,
  /// and its rows read into its row by `rec`, both over the index's
  /// table. Refused for an index that is not ordered
  /// (`IC_ERROR_NOT_SUPPORTED`). It lives in the connection until freed.
  pub fn create_index_scan(
    &mut self,
    index: &Arc<IndexDef>,
    key_rec: &Record,
    rec: &Record,
  ) -> Result<ScanId, IcError> {
    let scan = Scan::new_index(index, key_rec, rec)?;
    Ok(ScanId(self.scans.insert(scan)?))
  }

  /// Add a range to the next run of a scan of an ordered index, its
  /// bounds taken from `low` and `high`, rows laid out by the scan's key
  /// record; a row a bound does not use may be empty. The range is
  /// encoded at once and the rows are not kept. Refused with
  /// `IC_ERROR_BAD_RANGE` for a number above 4095, more columns than the
  /// index has, or bounds too long to send, and on a table scan with
  /// `IC_ERROR_NOT_SUPPORTED`.
  pub fn add_scan_range(
    &mut self,
    id: ScanId,
    range: &ScanRange,
    low: &[u8],
    high: &[u8],
  ) -> Result<(), IcError> {
    let scan = match self.scans.get_mut(id.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    match &mut scan.index {
      Some(index) => index.add_range(range, low, high),
      None => Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    }
  }

  /// Filter the scan's runs from the next on by `program`, or by none
  /// (chapter 04, "Conditions and assignments"): a row it refuses is
  /// passed over at the data node. It must be for the scanned table
  /// (`IC_ERROR_BAD_PROGRAM`) and assign nothing
  /// (`IC_ERROR_NOT_SUPPORTED`). Refused while a run is on
  /// (`IC_ERROR_SCAN_ACTIVE`).
  pub fn set_scan_program(
    &mut self,
    id: ScanId,
    program: Option<&Arc<Program>>,
  ) -> Result<(), IcError> {
    let scan = self.idle_scan(id)?;
    match program {
      Some(program) => {
        program::check_program(program, &scan.table, false)?;
        scan.program = Some(Arc::clone(program));
      }
      None => scan.program = None,
    }
    Ok(())
  }

  /// Set input `input`, 0 to 15, of the scan's program to `value`, for
  /// its runs from the next on; each holds what it was last set to, 0 at
  /// first (`IC_ERROR_BAD_PROGRAM` beyond 15). Refused while a run is on
  /// (`IC_ERROR_SCAN_ACTIVE`).
  pub fn set_scan_input(
    &mut self,
    id: ScanId,
    input: u32,
    value: i64,
  ) -> Result<(), IcError> {
    let scan = self.idle_scan(id)?;
    match scan.inputs.get_mut(input as usize) {
      Some(slot) => {
        *slot = value;
        Ok(())
      }
      None => Err(IcError::new(err::IC_ERROR_BAD_PROGRAM)),
    }
  }

  /// Every input of the scan's program at once, as an executor's run
  /// gives them, the scan being idle.
  pub(crate) fn set_scan_inputs(
    &mut self,
    id: ScanId,
    inputs: &[i64; IC_PROGRAM_INPUTS as usize],
  ) {
    if let Some(scan) = self.scans.get_mut(id.0) {
      scan.inputs = *inputs;
    }
  }

  /// A scan with no run on.
  fn idle_scan(&mut self, id: ScanId) -> Result<&mut Scan, IcError> {
    match self.scans.get_mut(id.0) {
      Some(scan) => {
        if scan.is_running() {
          return Err(IcError::new(err::IC_ERROR_SCAN_ACTIVE));
        }
        Ok(scan)
      }
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    }
  }

  /// The number of the range the scan's row last read is in; 0 if the
  /// id names no scan.
  pub fn scan_range_no(&self, id: ScanId) -> u32 {
    match self.scans.get(id.0) {
      Some(scan) => scan.range_no,
      None => 0,
    }
  }

  /// A scan, if the id names one.
  pub fn scan(&self, id: ScanId) -> Option<&Scan> {
    self.scans.get(id.0)
  }

  /// True if the scan's run is over, or the id names no scan.
  pub fn scan_done(&self, id: ScanId) -> bool {
    match self.scans.get(id.0) {
      Some(scan) => scan.is_done(),
      None => true,
    }
  }

  /// The row last read from one of the scan's batches, laid out by its
  /// record; empty if the id names no scan.
  pub fn scan_row(&self, id: ScanId) -> &[u8] {
    match self.scans.get(id.0) {
      Some(scan) => &scan.row,
      None => &[],
    }
  }

  /// Free a scan that is not running.
  pub fn free_scan(&mut self, id: ScanId) -> Result<(), IcError> {
    match self.scans.get(id.0) {
      Some(scan) => {
        if scan.is_running() {
          return Err(IcError::new(err::IC_ERROR_SCAN_ACTIVE));
        }
      }
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    }
    self.scans.remove(id.0);
    Ok(())
  }

  /// Start a run of the scan. The request goes with the next send, or
  /// with the poll that brings its record if none was free; batches
  /// come in later polls.
  pub fn start_scan(
    &mut self,
    id: ScanId,
    args: &ScanArgs,
  ) -> Result<(), IcError> {
    let (fragments, table, pruned) = match self.scans.get(id.0) {
      Some(scan) => {
        if scan.is_running() {
          return Err(IcError::new(err::IC_ERROR_SCAN_ACTIVE));
        }
        scan.check_run(args)?;
        (
          scan.table.info().fragment_count,
          Arc::clone(&scan.table),
          scan.pruned_partition(),
        )
      }
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    // A partition named wins over one the ranges fix.
    let mut partition = args.partition;
    if partition.is_none() {
      partition = pruned;
    }
    if args.read_ahead {
      // The application holds each batch and hands it back itself.
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    // Where the run goes, and under which id: the transaction's
    // coordinator and id when its rows may be taken over, else a node
    // chosen as for a transaction and an id of its own.
    let mut owner_ptr = scans::IC_SCAN_NO_OWNER;
    let mut instance = IC_ANY_COORD_INSTANCE;
    let (node_id, trans_id) = match args.trans {
      Some(tid) => {
        if args.lock == ScanLock::Committed {
          // A committed read's rows cannot be taken over.
          return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
        }
        let trans = match self.transaction(tid) {
          Some(trans) => trans,
          None => return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE)),
        };
        if trans.commit_state() != CommitState::Started {
          return Err(IcError::new(err::IC_ERROR_TRANSACTION_ACTIVE));
        }
        owner_ptr = trans.coord.coord_ptr;
        instance = blocks::block_instance(trans.coord.coord_block);
        (trans.coord.node_id, trans.id())
      }
      None => {
        let mut hint = TransactionHint::Any;
        if let Some(one) = partition {
          hint = TransactionHint::Partition(one);
        }
        let mut started = std::mem::take(&mut self.started_scratch);
        self.shared.started_nodes_into(&mut started);
        let chosen =
          self.choose_start_node(hint, Some(table.as_ref()), &started);
        self.started_scratch = started;
        (chosen?, self.next_transaction_id())
      }
    };
    let parallelism = parallelism_of(args, fragments, partition.is_some());
    let (batch_rows, batch_bytes) = batch_size(
      args,
      parallelism,
      self.shared.scan_batch_rows,
      self.shared.scan_batch_bytes,
      self.shared.scan_batch_bytes_total,
    );
    // Receiver ids for the run, before anything is changed, so that a
    // full object map leaves the scan as it was.
    let mut ids = std::mem::take(&mut self.scan_receiver_ids);
    ids.clear();
    let mut place: u32 = 0;
    while place < parallelism {
      let slot = ReceiverSlot { scan: id, place };
      let ptr = match self.receivers.insert(slot) {
        Ok(ptr) => ptr,
        Err(e) => {
          self.drop_receiver_ids(&ids);
          self.scan_receiver_ids = ids;
          return Err(e);
        }
      };
      if !fits_below_tag(ptr) {
        self.receivers.remove(ptr);
        self.drop_receiver_ids(&ids);
        self.scan_receiver_ids = ids;
        return Err(IcError::new(err::IC_ERROR_PTR_ARRAY_INDEX_OUT_OF_BOUND));
      }
      ids.push(ptr.as_u32() | IC_RECEIVER_TAG);
      place += 1;
    }
    let mut deadline: u64 = 0;
    if args.timeout_ms > 0 {
      deadline =
        ic_port::time::gethrtime() + args.timeout_ms as u64 * 1_000_000;
    }
    if let Some(scan) = self.scans.get_mut(id.0) {
      while (scan.receivers.len() as u32) < parallelism {
        scan.receivers.push(Receiver::default());
      }
      let mut k: usize = 0;
      while k < ids.len() {
        let receiver = &mut scan.receivers[k];
        receiver.clear_batch();
        receiver.id = ids[k];
        receiver.coord_ptr = 0;
        receiver.last = false;
        receiver.unsent = false;
        receiver.takeovers = 0;
        receiver.state = ReceiverState::Asked;
        k += 1;
      }
      scan.used = parallelism;
      scan.state = ScanState::Starting;
      scan.error = None;
      scan.user_ref = args.user_ref;
      scan.callback = args.callback;
      scan.binding = args.binding;
      scan.lock = args.lock;
      scan.trans = args.trans;
      scan.owner_ptr = owner_ptr;
      scan.trans_id = trans_id;
      scan.node_id = node_id;
      scan.instance = instance;
      scan.partition = partition;
      scan.ordered = args.ordered;
      scan.descending = args.descending;
      scan.range_no = 0;
      scan.reads_range_no = false;
      if let Some(index) = &mut scan.index {
        index.take_ranges();
        scan.reads_range_no = index.run.count > 1;
      }
      scan.batch_rows = batch_rows;
      scan.batch_bytes = batch_bytes;
      scan.record = None;
      scan.queued = false;
      scan.stop_sent = false;
      scan.coord_done = false;
      scan.deadline = deadline;
      scan.rows_read = 0;
      scan.reading = None;
      scan.last_read = None;
    }
    self.scan_receiver_ids = ids;
    self.scans_live.push(id);
    match self.take_scan_record(node_id, instance) {
      Some(rec) => {
        self.send_scan_req(id, rec);
        self.top_up_scan_pool(node_id, instance);
      }
      None => {
        // None free: ask for one, and start when it comes.
        self.scans_waiting.push(id);
        if let Err(e) = self.ask_scan_record(node_id, instance) {
          self.fail_run(id, e);
        }
      }
    }
    Ok(())
  }

  /// Give back receiver ids taken for a run that does not start.
  fn drop_receiver_ids(&mut self, ids: &[u32]) {
    for word in ids {
      self
        .receivers
        .remove(PtrId::from_u32(*word & !IC_RECEIVER_TAG));
    }
  }

  /// The run's request, on its record, queued for its node.
  fn send_scan_req(&mut self, id: ScanId, rec: CoordRecord) {
    let mut words = std::mem::take(&mut self.scan_words);
    let mut columns = std::mem::take(&mut self.scan_columns);
    let mut ids = std::mem::take(&mut self.scan_receiver_ids);
    let mut bounds = std::mem::take(&mut self.scan_bounds);
    bounds.clear();
    let queued = match self.scans.get_mut(id.0) {
      Some(scan) => {
        scan.record = Some(rec);
        scan.state = ScanState::Running;
        let mut word5 = scans::IC_SCAN_WORD5_PLAIN;
        if scan.partition.is_some() {
          word5 |= scans::IC_SCAN_WORD5_PARTITION_ID;
        }
        // A scan of an ordered index names the index, and reads the
        // range numbers first when it has several ranges.
        let mut table_id = scan.table.table_id();
        let mut table_version = scan.table.table_version();
        let mut read: &[u32] = &scan.read_part;
        if let Some(index) = &scan.index {
          table_id = index.index().index_id();
          table_version = index.index().index_version();
          if scan.reads_range_no {
            read = &index.shape.read_part_ranged;
          }
          bounds.extend_from_slice(&index.run.bounds);
        }
        let request = ScanReq {
          coord_ptr: rec.coord_ptr,
          flags_word: scan.flags().flags_word(),
          table_id,
          table_version,
          word5,
          trans_id_lo: scan.trans_id as u32,
          trans_id_hi: (scan.trans_id >> 32) as u32,
          owner_ptr: scan.owner_ptr,
          batch_bytes: scan.batch_bytes,
          batch_rows: scan.batch_rows,
          dist_key: scan.partition,
        };
        let encoded = request.encode();
        words.clear();
        words.extend_from_slice(&encoded[..request.len()]);
        match &scan.program {
          Some(program) => program::scan_section_into(
            program,
            &scan.inputs,
            read,
            &mut columns,
          ),
          None => scans::column_section_into(read, &[], &[], &mut columns),
        }
        ids.clear();
        let mut place: usize = 0;
        while place < scan.used as usize {
          ids.push(scan.receivers[place].id);
          place += 1;
        }
        true
      }
      None => false,
    };
    if queued {
      self.note_scan_record(rec.index, Some(id));
      let header = SignalHeader::new(
        numbers::IC_SIG_SCAN_REQ,
        self.block_number(),
        rec.coord_block,
      );
      // Section 2, the bounds, only for a scan of an index.
      let all: [&[u32]; 3] = [&ids, &columns, &bounds];
      let mut sections: &[&[u32]] = &all;
      if bounds.is_empty() {
        sections = &all[..2];
      }
      let sent = self.queue_signal_for_generation(
        rec.node_id,
        rec.generation,
        &header,
        &words,
        sections,
      );
      self.scan_send_due = true;
      if let Err(e) = sent {
        // The coordinator never heard of the run: its record is free.
        let instance = match self.scans.get_mut(id.0) {
          Some(scan) => {
            scan.record = None;
            scan.instance
          }
          None => IC_ANY_COORD_INSTANCE,
        };
        self.note_scan_record(rec.index, None);
        self.free_scan_record(&rec, instance);
        self.fail_run(id, e);
      }
    }
    self.scan_words = words;
    self.scan_columns = columns;
    self.scan_receiver_ids = ids;
    self.scan_bounds = bounds;
  }

  /// Note the scan running on the record at `index`, or none.
  fn note_scan_record(&mut self, index: u32, id: Option<ScanId>) {
    let index = index as usize;
    if index >= self.scan_of_record.len() {
      self.scan_of_record.resize(index + 1, None);
    }
    self.scan_of_record[index] = id;
  }

  // ---- Batches ----

  /// The batch completed longest ago and not yet taken, for a scan with
  /// no callback. A batch of a run that has since ended is passed over.
  pub fn get_next_scan_batch(&mut self) -> Option<ScanBatchId> {
    while let Some(batch) = self.ready_batches.pop_front() {
      if self.ready_receiver(batch).is_some() {
        return Some(batch);
      }
    }
    None
  }

  /// The place of a batch's receiver, if the batch is with the
  /// application.
  fn ready_receiver(&self, batch: ScanBatchId) -> Option<u32> {
    let ptr = PtrId::from_u32(batch.receiver & !IC_RECEIVER_TAG);
    let slot = *self.receivers.get(ptr)?;
    if slot.scan != batch.scan {
      return None;
    }
    let scan = self.scans.get(batch.scan.0)?;
    let receiver = scan.receivers.get(slot.place as usize)?;
    if receiver.id != batch.receiver || receiver.state != ReceiverState::Ready {
      return None;
    }
    Some(slot.place)
  }

  /// Rows in a batch.
  pub fn scan_batch_rows(&self, batch: ScanBatchId) -> u32 {
    let place = match self.ready_receiver(batch) {
      Some(place) => place,
      None => return 0,
    };
    match self.scans.get(batch.scan.0) {
      Some(scan) => scan.receivers[place as usize].row_ends.len() as u32,
      None => 0,
    }
  }

  /// Unpack the batch's next row into the scan's row (`Scan::row`).
  /// False once every row of the batch has been read.
  pub fn next_scan_row(&mut self, batch: ScanBatchId) -> Result<bool, IcError> {
    self.read_batch_row(batch, None)
  }

  /// As [`next_scan_row`](Self::next_scan_row), into `row`, a buffer of
  /// the caller's laid out by the scan's record, a copy less.
  pub fn next_scan_row_into(
    &mut self,
    batch: ScanBatchId,
    row: &mut [u8],
  ) -> Result<bool, IcError> {
    self.read_batch_row(batch, Some(row))
  }

  /// The batch's next row into `into`, or the scan's row, with its range
  /// number.
  fn read_batch_row(
    &mut self,
    batch: ScanBatchId,
    into: Option<&mut [u8]>,
  ) -> Result<bool, IcError> {
    let place = match self.ready_receiver(batch) {
      Some(place) => place,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let scan = match self.scans.get_mut(batch.scan.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if let Some(row) = &into {
      if row.len() < scan.rec.row_size() as usize {
        return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
      }
    }
    let single_no = scan.single_range_no();
    let receiver = &mut scan.receivers[place as usize];
    let at = receiver.next_row as usize;
    if at >= receiver.row_ends.len() {
      return Ok(false);
    }
    let (start, end) = receiver.row_span(at);
    receiver.next_row += 1;
    let receiver_id = receiver.id;
    let (range_no, words) = split_range_no(
      &receiver.rows[start..end],
      scan.reads_range_no,
      single_no,
    )?;
    match into {
      Some(row) => row_codec::unpack_row(&scan.rec, words, row)?,
      None => row_codec::unpack_row(&scan.rec, words, &mut scan.row)?,
    };
    scan.range_no = range_no;
    scan.rows_read += 1;
    scan.last_read = Some((place, receiver_id));
    Ok(true)
  }

  /// Trade a batch's rows and their ends for the buffers given, which
  /// must be empty: an executor moves a batch to its reader so, without
  /// copying a row. The batch stays with the application until it is
  /// handed back. Its receiver's place in the run, and whether it is the
  /// receiver's last batch, which a reader's merge needs; none if the
  /// batch is not with the application.
  pub(crate) fn swap_scan_batch(
    &mut self,
    batch: ScanBatchId,
    rows: &mut Vec<u32>,
    row_ends: &mut Vec<u32>,
  ) -> Option<(u32, bool)> {
    let place = self.ready_receiver(batch)?;
    let scan = self.scans.get_mut(batch.scan.0)?;
    let receiver = &mut scan.receivers[place as usize];
    std::mem::swap(&mut receiver.rows, rows);
    std::mem::swap(&mut receiver.row_ends, row_ends);
    Some((place, receiver.last))
  }

  /// The next run of a scan of an ordered index reads the ranges `set`
  /// holds, encoded already, in place of any added: how an executor
  /// passes a reader's on. Refused on a table scan
  /// (`IC_ERROR_NOT_SUPPORTED`).
  pub(crate) fn set_scan_ranges(
    &mut self,
    id: ScanId,
    set: &RangeSet,
  ) -> Result<(), IcError> {
    let scan = match self.scans.get_mut(id.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    match &mut scan.index {
      Some(index) => {
        index.set_ranges(set);
        Ok(())
      }
      None => Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    }
  }

  /// Hand a batch back: its receiver is asked for more with the next
  /// send, or gives no more if that was its last. A batch of a run that
  /// is closing is let go of, and one of a run that has ended is passed
  /// over.
  pub fn release_scan_batch(
    &mut self,
    batch: ScanBatchId,
  ) -> Result<(), IcError> {
    let ptr = PtrId::from_u32(batch.receiver & !IC_RECEIVER_TAG);
    let slot = match self.receivers.get(ptr) {
      Some(slot) if slot.scan == batch.scan => *slot,
      // Its run has ended, closed or failed: nothing to give back.
      _ => return Ok(()),
    };
    let mut ask = false;
    let over = match self.scans.get_mut(batch.scan.0) {
      Some(scan) => {
        let closing = scan.state == ScanState::Closing;
        let receiver = &mut scan.receivers[slot.place as usize];
        if receiver.id != batch.receiver
          || receiver.state != ReceiverState::Ready
        {
          return Ok(());
        }
        receiver.clear_batch();
        if receiver.last {
          receiver.state = ReceiverState::Done;
        } else if closing {
          receiver.state = ReceiverState::Held;
        } else {
          receiver.state = ReceiverState::Asked;
          receiver.unsent = true;
          if !scan.queued {
            scan.queued = true;
            ask = true;
          }
        }
        scan.coord_done && scan.all_done()
      }
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if ask {
      self.scans_to_send.push(batch.scan);
    }
    if over {
      self.end_run(batch.scan);
    }
    Ok(())
  }

  /// End a run before it reads to the end (chapter 05 §7.2): the
  /// coordinator is told to stop at once, and the run is done when it
  /// confirms. The batches asked for are not waited for, and their rows
  /// are dropped as they come; only a locking scan whose transaction
  /// goes on first waits for the takeovers of its rows still to be
  /// received. Batches with the application are let go of. A run not
  /// yet sent ends at once.
  pub fn close_scan(&mut self, id: ScanId) -> Result<(), IcError> {
    let state = match self.scans.get(id.0) {
      Some(scan) => scan.state,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    match state {
      ScanState::Starting => {
        self.end_run(id);
        return Ok(());
      }
      ScanState::Running => {}
      _ => return Ok(()),
    }
    if let Some(scan) = self.scans.get_mut(id.0) {
      scan.state = ScanState::Closing;
      hold_batches(scan);
    }
    self.close_when_settled(id);
    Ok(())
  }

  // ---- Reading row by row ----

  /// Read the scan's next row into its row (`Scan::row`): the scan as a
  /// stream of rows (chapter 04, "Consuming scans"). When the batch
  /// being read is used up it is handed back, so that its receiver goes
  /// on, and the scan's next complete batch is taken, polling while
  /// none is; every other scan and query of the connection is served
  /// meanwhile. False once the run has read to the end or was closed,
  /// or for a scan not started; the run's error once it has failed.
  /// Refused for a scan with a callback, and inside a callback once it
  /// would have to wait (`IC_ERROR_NOT_SUPPORTED`).
  pub fn next_row(&mut self, id: ScanId) -> Result<bool, IcError> {
    self.next_row_with(id, None)
  }

  /// As [`next_row`](Self::next_row), into `row`, a buffer of the
  /// caller's laid out by the scan's record, a copy less.
  pub fn next_row_into(
    &mut self,
    id: ScanId,
    row: &mut [u8],
  ) -> Result<bool, IcError> {
    self.next_row_with(id, Some(row))
  }

  fn next_row_with(
    &mut self,
    id: ScanId,
    mut row: Option<&mut [u8]>,
  ) -> Result<bool, IcError> {
    let ordered = match self.scans.get(id.0) {
      Some(scan) => scan.ordered,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if ordered {
      return self.next_merged_row(id, row);
    }
    loop {
      let reading = match self.scans.get(id.0) {
        Some(scan) => scan.reading,
        None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
      };
      if let Some(batch) = reading {
        // A batch let go of meanwhile, by a close or a failure, is
        // passed over.
        if self.ready_receiver(batch).is_some() {
          let more = match row.as_deref_mut() {
            Some(into) => self.next_scan_row_into(batch, into)?,
            None => self.next_scan_row(batch)?,
          };
          if more {
            return Ok(true);
          }
          self.release_scan_batch(batch)?;
        }
        if let Some(scan) = self.scans.get_mut(id.0) {
          scan.reading = None;
        }
      }
      match self.wait_scan_batch(id)? {
        Some(batch) => {
          if let Some(scan) = self.scans.get_mut(id.0) {
            scan.reading = Some(batch);
          }
        }
        None => return Ok(false),
      }
    }
  }

  /// The scan's next complete batch, waiting as
  /// [`next_row`](Self::next_row) does; none once the run is over with
  /// no error. The application reads its rows with
  /// [`next_scan_row`](Self::next_scan_row) and hands it back itself.
  /// Refused for an ordered run, whose rows `next_row` merges
  /// (`IC_ERROR_NOT_SUPPORTED`).
  pub fn next_batch(
    &mut self,
    id: ScanId,
  ) -> Result<Option<ScanBatchId>, IcError> {
    if let Some(scan) = self.scans.get(id.0) {
      if scan.ordered {
        return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
      }
    }
    self.wait_scan_batch(id)
  }

  /// Wait for a batch of this scan, polling.
  fn wait_scan_batch(
    &mut self,
    id: ScanId,
  ) -> Result<Option<ScanBatchId>, IcError> {
    loop {
      let (state, error) = match self.scans.get(id.0) {
        Some(scan) => {
          if scan.callback.is_some() {
            return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
          }
          (scan.state, scan.error)
        }
        None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
      };
      if let Some(batch) = self.take_ready_batch_of(id) {
        return Ok(Some(batch));
      }
      match state {
        ScanState::Idle => return Ok(None),
        ScanState::Done => {
          return match error {
            Some(e) => Err(e),
            None => Ok(None),
          };
        }
        _ => {}
      }
      self.wait_for_scan(id)?;
    }
  }

  /// One wait for what a scan needs: its takeovers go first, since a
  /// batch handed back with them waits for their confirmations, then a
  /// poll. Refused inside a callback, where a poll does nothing.
  fn wait_for_scan(&mut self, id: ScanId) -> Result<(), IcError> {
    if self.in_callback > 0 {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let pending = match self.scans.get(id.0) {
      Some(scan) => scan.takeovers_pending(),
      None => false,
    };
    if pending {
      let _ = self.send_queries(false);
    }
    self.poll(IC_CALL_SLICE_MS);
    Ok(())
  }

  // ---- The ordered merge ----

  /// The next row of an ordered run, merged from its receivers' (chapter
  /// 04, "Index scans"), into `row` or the scan's row. A batch whose
  /// rows are all read is handed back first, and then the merge waits
  /// until every receiver has a row or has ended, since the next row
  /// may be any of theirs. False once the run has read to the end or was
  /// closed; the run's error once it has failed.
  fn next_merged_row(
    &mut self,
    id: ScanId,
    mut row: Option<&mut [u8]>,
  ) -> Result<bool, IcError> {
    loop {
      let (state, error) = match self.scans.get(id.0) {
        Some(scan) => (scan.state, scan.error),
        None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
      };
      match state {
        ScanState::Idle => return Ok(false),
        ScanState::Done => {
          return match error {
            Some(e) => Err(e),
            None => Ok(false),
          };
        }
        ScanState::Running => {
          self.hand_back_read_batches(id)?;
          if let Some(place) = self.first_merged(id)? {
            self.take_merged(id, place, row.as_deref_mut())?;
            return Ok(true);
          }
        }
        // Starting, or closing: nothing to merge until it is done.
        _ => {}
      }
      self.wait_for_scan(id)?;
    }
  }

  /// Hand back every batch of an ordered run whose rows are all read.
  fn hand_back_read_batches(&mut self, id: ScanId) -> Result<(), IcError> {
    let mut place: usize = 0;
    loop {
      let batch = match self.scans.get(id.0) {
        Some(scan) => {
          if place >= scan.used as usize {
            return Ok(());
          }
          let receiver = &scan.receivers[place];
          let used_up =
            receiver.state == ReceiverState::Ready && !receiver.has_row();
          if used_up {
            Some(ScanBatchId {
              scan: id,
              receiver: receiver.id,
            })
          } else {
            None
          }
        }
        None => return Ok(()),
      };
      if let Some(read) = batch {
        self.release_scan_batch(read)?;
      }
      place += 1;
    }
  }

  /// The place of the receiver whose next row comes first, once every
  /// receiver of the run has a row or has ended; none while one waits
  /// for its batch, or when every one has ended. Each receiver's next
  /// row is unpacked once, with its order key.
  fn first_merged(&mut self, id: ScanId) -> Result<Option<u32>, IcError> {
    let scan = match self.scans.get_mut(id.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let order = match &scan.index {
      Some(index) => match &index.shape.order {
        Ok(order) => order,
        Err(e) => return Err(*e),
      },
      None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    };
    let single_no = match &scan.index {
      Some(index) => index.run.first_no,
      None => 0,
    };
    let mut first: Option<u32> = None;
    let mut place: usize = 0;
    while place < scan.used as usize {
      let receiver = &mut scan.receivers[place];
      match receiver.state {
        ReceiverState::Ready => {}
        ReceiverState::Done => {
          place += 1;
          continue;
        }
        // Its batch is still coming.
        _ => return Ok(None),
      }
      if !receiver.has_row() {
        return Ok(None);
      }
      fill_head(&scan.rec, order, scan.reads_range_no, single_no, receiver)?;
      let before = match first {
        Some(best) => comes_before(
          &scan.receivers[place],
          &scan.receivers[best as usize],
          scan.descending,
        ),
        None => true,
      };
      if before {
        first = Some(place as u32);
      }
      place += 1;
    }
    Ok(first)
  }

  /// Take the merged row of the receiver at `place` into `row`, or the
  /// scan's row.
  fn take_merged(
    &mut self,
    id: ScanId,
    place: u32,
    row: Option<&mut [u8]>,
  ) -> Result<(), IcError> {
    let scan = match self.scans.get_mut(id.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let size = scan.rec.row_size() as usize;
    let receiver = &mut scan.receivers[place as usize];
    match row {
      Some(into) => {
        if into.len() < size {
          return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
        }
        into[..size].copy_from_slice(&receiver.head_row);
      }
      None => scan.row.copy_from_slice(&receiver.head_row),
    }
    receiver.next_row += 1;
    scan.range_no = receiver.head_range;
    scan.rows_read += 1;
    scan.last_read = Some((place, receiver.id));
    Ok(())
  }

  /// The oldest batch of this scan on the ready list, taken off it;
  /// the other scans' batches stay. A batch of the scan no longer with
  /// the application is dropped from the list on the way.
  fn take_ready_batch_of(&mut self, id: ScanId) -> Option<ScanBatchId> {
    let mut at: usize = 0;
    while at < self.ready_batches.len() {
      let batch = self.ready_batches[at];
      if batch.scan != id {
        at += 1;
        continue;
      }
      let _ = self.ready_batches.remove(at);
      if self.ready_receiver(batch).is_some() {
        return Some(batch);
      }
    }
    None
  }

  // ---- Taking rows over ----

  /// Take the lock on the row last read from the scan over into the
  /// scan's transaction, reading the columns of the query's attribute
  /// record into its attribute row. A shared-lock scan's lock is taken
  /// over shared, an exclusive one's exclusively, as the reference does.
  /// The query, over the scanned table, completes as any query does,
  /// and is sent with the next send. The lock is the transaction's
  /// until it ends.
  ///
  /// Refused with `IC_ERROR_NOT_SUPPORTED` for a scan not started on a
  /// transaction, or a query through an index; with `IC_ERROR_NO_SCAN_ROW`
  /// when no row has been read from a batch still with the application.
  pub fn lock_scan_row(
    &mut self,
    query: QueryId,
    scan: ScanId,
    args: &TakeoverArgs,
  ) -> Result<(), IcError> {
    let exclusive = match self.scans.get(scan.0) {
      Some(found) => found.lock == ScanLock::Exclusive,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let mut operation = key_ops::IC_OP_READ;
    if exclusive {
      operation = key_ops::IC_OP_READ_EXCLUSIVE;
    }
    self.take_over_row(query, scan, operation, true, args)
  }

  /// Update the row last read from the scan, in the scan's transaction,
  /// with the values of the query's attribute row, its key columns left
  /// out. Refused as [`lock_scan_row`](Self::lock_scan_row) is.
  pub fn update_scan_row(
    &mut self,
    query: QueryId,
    scan: ScanId,
    args: &TakeoverArgs,
  ) -> Result<(), IcError> {
    self.take_over_row(query, scan, key_ops::IC_OP_UPDATE, false, args)
  }

  /// Delete the row last read from the scan, in the scan's transaction.
  /// Refused as [`lock_scan_row`](Self::lock_scan_row) is, and on a table
  /// with a blob column with `IC_ERROR_DELETE_WITH_BLOBS`.
  pub fn delete_scan_row(
    &mut self,
    query: QueryId,
    scan: ScanId,
    args: &TakeoverArgs,
  ) -> Result<(), IcError> {
    self.take_over_row(query, scan, key_ops::IC_OP_DELETE, false, args)
  }

  /// A takeover of the row last read, defined on the scan's transaction
  /// and counted against its receiver.
  fn take_over_row(
    &mut self,
    query: QueryId,
    scan: ScanId,
    operation: u32,
    is_read: bool,
    args: &TakeoverArgs,
  ) -> Result<(), IcError> {
    let (trans, mut over) = self.last_row_takeover(scan)?;
    let same_table = match (self.queries.get(query.0), self.scans.get(scan.0)) {
      (Some(found), Some(scanned)) => {
        found.table().table_id() == scanned.table.table_id()
          && found.table().table_version() == scanned.table.table_version()
      }
      (None, _) => {
        self.takeover_key = over.key;
        return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD));
      }
      _ => false,
    };
    if !same_table {
      self.takeover_key = over.key;
      return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
    }
    let mut abort_option = key_ops::IC_ABORT_ON_ERROR;
    if args.abort_option == AbortOption::IgnoreError {
      abort_option = key_ops::IC_IGNORE_ERROR;
    }
    let flags = KeyOpFlags {
      operation,
      abort_option,
      dist_key: true,
      scan_takeover: true,
      ..KeyOpFlags::default()
    };
    let defined = self.define(
      query,
      trans,
      flags,
      is_read,
      args.user_ref,
      args.callback,
      args.binding,
      Some(&over),
    );
    self.takeover_key = std::mem::take(&mut over.key);
    defined?;
    if let Some(scanned) = self.scans.get_mut(scan.0) {
      if let Some(receiver) = scanned
        .receivers
        .iter_mut()
        .find(|receiver| receiver.id == over.of.receiver)
      {
        receiver.takeovers += 1;
      }
    }
    Ok(())
  }

  /// The transaction and the key of the row last read from the scan,
  /// from a batch still with the application.
  fn last_row_takeover(
    &mut self,
    id: ScanId,
  ) -> Result<(TransId, TakeoverKey), IcError> {
    let no_row = IcError::new(err::IC_ERROR_NO_SCAN_ROW);
    let scan = match self.scans.get(id.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    // Only a scan started on a transaction asked for the rows' keys.
    let trans = match scan.trans {
      Some(trans) => trans,
      None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    };
    if scan.state != ScanState::Running {
      return Err(no_row);
    }
    let (place, receiver_id) = match scan.last_read {
      Some(last) => last,
      None => return Err(no_row),
    };
    let receiver = match scan.receivers.get(place as usize) {
      Some(receiver) => receiver,
      None => return Err(no_row),
    };
    let with_us = receiver.id == receiver_id
      && receiver.state == ReceiverState::Ready
      && receiver.next_row > 0;
    if !with_us {
      return Err(no_row);
    }
    // The n-th key is the n-th row's, and its operation number says so.
    let at = (receiver.next_row - 1) as usize;
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if at >= receiver.key_ends.len() {
      return Err(bad);
    }
    let row_word = receiver.key_rows[at];
    if ScanRowKeyRep::op_number_of(row_word) != at as u32 {
      return Err(bad);
    }
    let mut start: usize = 0;
    if at > 0 {
      start = receiver.key_ends[at - 1] as usize;
    }
    let end = receiver.key_ends[at] as usize;
    let mut key = std::mem::take(&mut self.takeover_key);
    key.clear();
    key.extend_from_slice(&receiver.keys[start..end]);
    Ok((
      trans,
      TakeoverKey {
        key,
        info: scans::takeover_info(row_word),
        fragment: ScanRowKeyRep::fragment_of(row_word),
        of: TakeoverOf {
          scan: id,
          receiver: receiver_id,
        },
      },
    ))
  }

  /// A takeover is done, confirmed or failed: once its receiver's are
  /// all done, a receiver handed back asks for more with the next send,
  /// and a closing scan may send its stop.
  pub(crate) fn takeover_settled(&mut self, of: TakeoverOf) {
    let slot = match self
      .receivers
      .get(PtrId::from_u32(of.receiver & !IC_RECEIVER_TAG))
    {
      Some(slot) if slot.scan == of.scan => *slot,
      // Its run has ended since.
      _ => return,
    };
    let (ask, closing) = match self.scans.get_mut(of.scan.0) {
      Some(scan) => {
        let running = scan.state == ScanState::Running;
        let closing = scan.state == ScanState::Closing;
        let receiver = &mut scan.receivers[slot.place as usize];
        if receiver.id != of.receiver || receiver.takeovers == 0 {
          return;
        }
        receiver.takeovers -= 1;
        if receiver.takeovers > 0 {
          return;
        }
        let ask = running && receiver.unsent && !scan.queued;
        if ask {
          scan.queued = true;
        }
        (ask, closing)
      }
      None => return,
    };
    if ask {
      self.scans_to_send.push(of.scan);
    }
    if closing {
      self.close_when_settled(of.scan);
    }
  }

  // ---- Sending ----

  /// Queue the requests for more of the scans whose batches were handed
  /// back since the last send: one `SCAN_MORE_REQ` a scan, naming every
  /// receiver handed back.
  pub(crate) fn queue_scan_more(&mut self) {
    let mut ids = std::mem::take(&mut self.scans_to_send);
    for id in &ids {
      self.queue_more(*id, false);
    }
    ids.clear();
    if self.scans_to_send.is_empty() {
      self.scans_to_send = ids;
    }
  }

  /// One scan's `SCAN_MORE_REQ`: its receivers to go on, or with `stop`
  /// a close, naming the receivers held, or the one pointer a close
  /// after a refusal names if none is.
  fn queue_more(&mut self, id: ScanId, stop: bool) {
    let mut words = std::mem::take(&mut self.scan_words);
    let mut ptrs = std::mem::take(&mut self.scan_receiver_ids);
    let rec = match self.scans.get_mut(id.0) {
      Some(scan) => {
        scan.queued = false;
        ptrs.clear();
        if stop {
          let mut place: usize = 0;
          while place < scan.used as usize {
            let receiver = &scan.receivers[place];
            if receiver.state == ReceiverState::Held && !receiver.last {
              ptrs.push(receiver.coord_ptr);
            }
            place += 1;
          }
          if ptrs.is_empty() {
            ptrs.push(scans::IC_SCAN_CLOSE_PTR);
          }
          scan.stop_sent = true;
        } else if scan.state == ScanState::Running {
          let mut place: usize = 0;
          while place < scan.used as usize {
            let receiver = &mut scan.receivers[place];
            // One whose takeovers are not all done waits for them.
            if receiver.unsent && receiver.takeovers == 0 {
              receiver.unsent = false;
              ptrs.push(receiver.coord_ptr);
            }
            place += 1;
          }
        }
        if ptrs.is_empty() {
          // Nothing to ask for: a run closing or ended since.
          None
        } else {
          let request = ScanMoreReq {
            coord_ptr: match scan.record {
              Some(rec) => rec.coord_ptr,
              None => 0,
            },
            stop,
            trans_id_lo: scan.trans_id as u32,
            trans_id_hi: (scan.trans_id >> 32) as u32,
          };
          let inline = request.encode_into(&ptrs, &mut words);
          if inline {
            ptrs.clear();
          }
          scan.record
        }
      }
      None => None,
    };
    if let Some(rec) = rec {
      let header = SignalHeader::new(
        numbers::IC_SIG_SCAN_MORE_REQ,
        self.block_number(),
        rec.coord_block,
      );
      let section: [&[u32]; 1] = [&ptrs];
      let mut sections: &[&[u32]] = &section;
      if ptrs.is_empty() {
        sections = &section[..0];
      }
      let sent = self.queue_signal_for_generation(
        rec.node_id,
        rec.generation,
        &header,
        &words,
        sections,
      );
      if let Err(e) = sent {
        ic_port::debug_print!(
          IC_NDB_MESSAGE_LEVEL,
          "SCAN_MORE_REQ to node {} not queued: {}",
          rec.node_id,
          e.message()
        );
      }
    }
    self.scan_words = words;
    self.scan_receiver_ids = ptrs;
  }

  /// Send what the scans called for: requests for more and whatever a
  /// poll queued for them, such as a request whose record has come or a
  /// stop. Nothing is sent when they called for nothing.
  pub(crate) fn send_scan_requests(&mut self) {
    if self.scans_to_send.is_empty() && !self.scan_send_due {
      return;
    }
    self.queue_scan_more();
    self.scan_send_due = false;
    // A failed write has asked for its link to be dropped, which the
    // next poll reports by failing the runs that used it.
    let _ = self.send_queued(false);
  }

  // ---- What a poll does for scans ----

  /// After a poll has taken its signals: start the runs whose record
  /// has come, fail those whose coordinator's link has gone (when the
  /// links have changed), close those past their deadline, and send
  /// what they called for.
  pub(crate) fn drive_scans(&mut self, check_links: bool) {
    if self.scans_live.is_empty() {
      return;
    }
    self.collect_scan_records();
    if !self.scans_waiting.is_empty() {
      self.start_waiting_scans();
    }
    if check_links {
      self.fail_lost_scans();
    }
    self.close_late_scans();
    self.send_scan_requests();
  }

  /// Start each waiting run whose pool has a record now; fail one whose
  /// pool was refused with nothing more asked; ask again for the rest.
  fn start_waiting_scans(&mut self) {
    let waiting = std::mem::take(&mut self.scans_waiting);
    let mut still: Vec<ScanId> = Vec::new();
    for id in waiting {
      let (node_id, instance) = match self.scans.get(id.0) {
        Some(scan) if scan.state == ScanState::Starting => {
          (scan.node_id, scan.instance)
        }
        // Closed or freed meanwhile.
        _ => continue,
      };
      if let Some(rec) = self.take_scan_record(node_id, instance) {
        self.send_scan_req(id, rec);
        self.top_up_scan_pool(node_id, instance);
        continue;
      }
      let (asked, refused) = self.scan_pool_state(node_id, instance);
      if asked {
        still.push(id);
        continue;
      }
      if let Some(e) = refused {
        self.fail_run(id, e);
        continue;
      }
      match self.ask_scan_record(node_id, instance) {
        Ok(()) => still.push(id),
        Err(e) => self.fail_run(id, e),
      }
    }
    // A callback run meanwhile may have started more.
    still.append(&mut self.scans_waiting);
    self.scans_waiting = still;
  }

  /// Fail every run whose coordinator's link has gone, or been replaced,
  /// since it was sent: its record went with the link. The error is
  /// NDB's for a failed coordinator when the node's failure has been
  /// reported, else the link's.
  fn fail_lost_scans(&mut self) {
    let mut lost: Vec<(ScanId, IcError)> = Vec::new();
    for id in &self.scans_live {
      let rec = match self.scans.get(id.0) {
        Some(scan) => match scan.record {
          Some(rec) => rec,
          None => continue,
        },
        None => continue,
      };
      if self.link_is(rec.node_id, rec.generation) {
        continue;
      }
      let error = match self.shared.node(rec.node_id) {
        Some(node) => {
          if node.failure_reported() {
            IcError::new(IC_NDB_ERROR_SCAN_NODE_FAILURE)
          } else {
            node.not_connected_error()
          }
        }
        None => IcError::new(err::IC_ERROR_LINK_LOST),
      };
      lost.push((*id, error));
    }
    for (id, error) in lost {
      let rec = match self.scans.get_mut(id.0) {
        Some(scan) => scan.record.take(),
        None => None,
      };
      if let Some(rec) = rec {
        self.note_scan_record(rec.index, None);
        self.lose_coord_record(&rec);
      }
      self.fail_run(id, error);
    }
  }

  /// Close every run past its deadline, with `IC_ERROR_TIMEOUT`.
  fn close_late_scans(&mut self) {
    let now = ic_port::time::gethrtime();
    let mut late: Vec<ScanId> = Vec::new();
    for id in &self.scans_live {
      if let Some(scan) = self.scans.get(id.0) {
        if scan.deadline != 0 && now >= scan.deadline {
          late.push(*id);
        }
      }
    }
    for id in late {
      if let Some(scan) = self.scans.get_mut(id.0) {
        scan.deadline = 0;
        if scan.error.is_none() {
          scan.error = Some(IcError::new(err::IC_ERROR_TIMEOUT));
        }
      }
      let _ = self.close_scan(id);
    }
  }

  // ---- Replies ----

  /// The scan whose run is on the record our pointer names, if the
  /// transaction id is the run's.
  fn scan_named(&self, client_ptr: u32, lo: u32, hi: u32) -> Option<ScanId> {
    let index = coord_index_of(client_ptr)?;
    let id = (*self.scan_of_record.get(index)?)?;
    let scan = self.scans.get(id.0)?;
    if scan.trans_id as u32 == lo && (scan.trans_id >> 32) as u32 == hi {
      return Some(id);
    }
    None
  }

  /// The receiver a tagged id names, if it is of a live run of the
  /// transaction id given.
  fn receiver_named(
    &self,
    word: u32,
    lo: u32,
    hi: u32,
  ) -> Option<ReceiverSlot> {
    if word & IC_RECEIVER_TAG == 0 {
      return None;
    }
    let slot = *self
      .receivers
      .get(PtrId::from_u32(word & !IC_RECEIVER_TAG))?;
    let scan = self.scans.get(slot.scan.0)?;
    let same = scan.trans_id as u32 == lo && (scan.trans_id >> 32) as u32 == hi;
    if !same || !scan.is_running() {
      return None;
    }
    Some(slot)
  }

  /// A row or a key that names a receiver of no live run: a run's that
  /// has ended, of a batch its close did not wait for, dropped and
  /// counted as late (chapter 05 §7.2). False if the word is no
  /// receiver's at all.
  fn late_scan_signal(&mut self, word: u32) -> bool {
    if word & IC_RECEIVER_TAG == 0 {
      return false;
    }
    self.late_scan_signals += 1;
    true
  }

  /// A row of a scan: kept in its receiver's batch buffer.
  pub(crate) fn take_scan_row(&mut self, signal: &SignalView<'_>) -> bool {
    let front = match RowDataRep::decode(signal.data) {
      Ok(front) => front,
      Err(_) => return false,
    };
    let slot = match self.receiver_named(
      front.query_ref,
      front.trans_id_lo,
      front.trans_id_hi,
    ) {
      Some(slot) => slot,
      None => return self.late_scan_signal(front.query_ref),
    };
    let mut section: Option<&[u32]> = None;
    if signal.num_sections > 0 {
      section = Some(signal.section(0));
    }
    let row = RowDataRep::row(signal.data, section);
    match self.scans.get_mut(slot.scan.0) {
      Some(scan) => {
        let keep = scan.state != ScanState::Closing;
        let receiver = &mut scan.receivers[slot.place as usize];
        if receiver.state != ReceiverState::Asked {
          return false;
        }
        receiver.words += row.len() as u32;
        // A closing run waits for the batch only to know it is in;
        // its rows are not read.
        if keep {
          receiver.rows.extend_from_slice(row);
          receiver.row_ends.push(receiver.rows.len() as u32);
        }
      }
      None => return false,
    }
    self.check_batch(slot.scan, slot.place);
    true
  }

  /// A row's key, from a locking scan that asked for keys: kept for its
  /// takeover, and counted toward the batch.
  pub(crate) fn take_scan_key(&mut self, signal: &SignalView<'_>) -> bool {
    let front = match ScanRowKeyRep::decode(signal.data) {
      Ok(front) => front,
      Err(_) => return false,
    };
    let slot = match self.receiver_named(
      front.receiver_id,
      front.trans_id_lo,
      front.trans_id_hi,
    ) {
      Some(slot) => slot,
      None => return self.late_scan_signal(front.receiver_id),
    };
    let mut section: Option<&[u32]> = None;
    if signal.num_sections > 0 {
      section = Some(signal.section(0));
    }
    let key = ScanRowKeyRep::key(signal.data, section);
    match self.scans.get_mut(slot.scan.0) {
      Some(scan) => {
        let keep = scan.state != ScanState::Closing;
        let receiver = &mut scan.receivers[slot.place as usize];
        if receiver.state != ReceiverState::Asked {
          return false;
        }
        receiver.words += key.len() as u32;
        if keep {
          receiver.keys.extend_from_slice(key);
          receiver.key_ends.push(receiver.keys.len() as u32);
          receiver.key_rows.push(front.row_word);
        }
      }
      None => return false,
    }
    self.check_batch(slot.scan, slot.place);
    true
  }

  /// The coordinator reports batches, the end of the run, or that a
  /// close is done.
  pub(crate) fn take_scan_conf(&mut self, signal: &SignalView<'_>) -> bool {
    let mut section: Option<&[u32]> = None;
    if signal.num_sections > 0 {
      section = Some(signal.section(0));
    }
    let conf = match ScanConf::decode(signal.data, section, true) {
      Ok(conf) => conf,
      Err(_) => return false,
    };
    let id = match self.scan_named(
      conf.client_ptr,
      conf.trans_id_lo,
      conf.trans_id_hi,
    ) {
      Some(id) => id,
      // A stop that crossed its run's end is confirmed by the
      // coordinator all the same, once the run has ended here.
      None if conf.confirms_close() => {
        self.late_scan_signals += 1;
        return true;
      }
      None => return false,
    };
    if conf.confirms_close() {
      // The stop is confirmed, or the run ended at the coordinator
      // with nothing more to report: either way it is over there.
      self.coord_finished(id);
      self.end_run(id);
      return true;
    }
    for batch in conf.batches() {
      self.take_entry(id, batch);
    }
    if conf.end_of_data() {
      self.coord_finished(id);
      let (closing, over) = match self.scans.get(id.0) {
        Some(scan) => (scan.state == ScanState::Closing, scan.all_done()),
        None => (false, false),
      };
      if closing {
        // The run ended at the coordinator as it closed here, perhaps
        // crossing its stop: what it holds is let go of.
        self.close_when_settled(id);
      } else if over {
        self.end_run(id);
      }
    }
    true
  }

  /// One receiver's entry in a confirmation.
  fn take_entry(&mut self, id: ScanId, batch: &ScanBatch) {
    let slot = match self
      .receivers
      .get(PtrId::from_u32(batch.receiver_id & !IC_RECEIVER_TAG))
    {
      Some(slot) if slot.scan == id => *slot,
      _ => return,
    };
    match self.scans.get_mut(id.0) {
      Some(scan) => {
        let receiver = &mut scan.receivers[slot.place as usize];
        if receiver.id != batch.receiver_id
          || receiver.state != ReceiverState::Asked
        {
          return;
        }
        receiver.confirmed = true;
        receiver.last = batch.is_done();
        receiver.coord_ptr = batch.receiver_ptr;
        receiver.conf_rows = batch.rows;
        receiver.conf_words = batch.words;
      }
      None => return,
    }
    self.check_batch(id, slot.place);
  }

  /// Complete a receiver's batch if all of it has come: delivered, or
  /// held if the run is closing.
  fn check_batch(&mut self, id: ScanId, place: u32) {
    let closing = match self.scans.get_mut(id.0) {
      Some(scan) => {
        let is_closing = scan.state == ScanState::Closing;
        let receiver = &mut scan.receivers[place as usize];
        let complete = receiver.state == ReceiverState::Asked
          && receiver.confirmed
          && receiver.words >= receiver.conf_words;
        if !complete {
          return;
        }
        if is_closing {
          receiver.clear_batch();
          if receiver.last {
            receiver.state = ReceiverState::Done;
          } else {
            receiver.state = ReceiverState::Held;
          }
        } else {
          // Rows of no words, as of a record that reads nothing, do
          // not come; the entry still counts them.
          while (receiver.row_ends.len() as u32) < receiver.conf_rows {
            receiver.row_ends.push(receiver.rows.len() as u32);
          }
          receiver.state = ReceiverState::Ready;
        }
        is_closing
      }
      None => return,
    };
    if closing {
      self.close_when_settled(id);
      return;
    }
    self.deliver(id, place);
  }

  /// A complete batch onto the ready list, or to the scan's callback.
  /// An ordered run's batch goes to neither: `next_row` merges it.
  fn deliver(&mut self, id: ScanId, place: u32) {
    let (receiver, callback, user_ref) = match self.scans.get(id.0) {
      Some(scan) => {
        if scan.ordered {
          return;
        }
        (
          scan.receivers[place as usize].id,
          scan.callback,
          scan.user_ref,
        )
      }
      None => return,
    };
    let batch = ScanBatchId { scan: id, receiver };
    match callback {
      Some(callback) => {
        self.in_callback += 1;
        callback(self, batch, user_ref);
        self.in_callback -= 1;
      }
      None => self.ready_batches.push_back(batch),
    }
  }

  /// The coordinator refuses the run, or ends it with an error. A run
  /// that still holds the coordinator's record is closed, with five
  /// words naming one pointer, as the reference does; one that does not
  /// is over (chapter 05 §7.2).
  pub(crate) fn take_scan_ref(&mut self, signal: &SignalView<'_>) -> bool {
    let refusal = match ScanRef::decode(signal.data) {
      Ok(refusal) => refusal,
      Err(_) => return false,
    };
    let id = match self.scan_named(
      refusal.client_ptr,
      refusal.trans_id_lo,
      refusal.trans_id_hi,
    ) {
      Some(id) => id,
      None => return false,
    };
    let code = refusal.error_code as i32;
    if code == IC_NDB_ERROR_SCHEMA_CHANGED || code == IC_NDB_ERROR_UNKNOWN_TABLE
    {
      let table = self.scans.get(id.0).map(|scan| Arc::clone(&scan.table));
      let index = self.scans.get(id.0).and_then(|scan| scan.index().cloned());
      if let Some(index) = index {
        self.index_changed(&index);
      }
      if let Some(table) = table {
        self.table_changed(&table);
      }
    }
    let stop_sent = match self.scans.get_mut(id.0) {
      Some(scan) => {
        if scan.error.is_none() {
          scan.error = Some(IcError::new(code));
        }
        scan.state = ScanState::Closing;
        hold_batches(scan);
        scan.stop_sent
      }
      None => return false,
    };
    if !refusal.close_owed {
      self.coord_finished(id);
      self.end_run(id);
      return true;
    }
    if !stop_sent {
      self.queue_more(id, true);
      self.scan_send_due = true;
    }
    true
  }

  // ---- Ending a run ----

  /// Send the stop of a closing run (chapter 05 §7.2): at once, the
  /// batches asked for not waited for, unless its takeovers hold it
  /// back ([`stop_waits`]). A run the coordinator is done with ends
  /// instead, the stop sent or not.
  fn close_when_settled(&mut self, id: ScanId) {
    let (pending, trans, stop_sent, coord_done) = match self.scans.get(id.0) {
      Some(scan) => (
        scan.takeovers_pending(),
        scan.trans,
        scan.stop_sent,
        scan.coord_done,
      ),
      None => return,
    };
    // A transaction gone is waited for no more than one rolling back.
    let rolls_back = trans.map(|tid| match self.transaction(tid) {
      Some(trans) => trans.rolls_back(),
      None => true,
    });
    if stop_waits(pending, rolls_back) {
      return;
    }
    if coord_done {
      self.end_run(id);
      return;
    }
    if stop_sent {
      return;
    }
    self.queue_more(id, true);
    self.scan_send_due = true;
  }

  /// The transaction `tid` rolls back: its scans being closed wait no
  /// more for their takeovers, and send their stops.
  pub(crate) fn stop_closing_scans_of(&mut self, tid: TransId) {
    let mut closing: Vec<ScanId> = Vec::new();
    for id in &self.scans_live {
      if let Some(scan) = self.scans.get(id.0) {
        if scan.state == ScanState::Closing && scan.trans == Some(tid) {
          closing.push(*id);
        }
      }
    }
    for id in closing {
      self.close_when_settled(id);
    }
  }

  /// The coordinator is done with the run, and its record is free
  /// again: back to its pool.
  fn coord_finished(&mut self, id: ScanId) {
    let (rec, instance) = match self.scans.get_mut(id.0) {
      Some(scan) => {
        scan.coord_done = true;
        (scan.record.take(), scan.instance)
      }
      None => return,
    };
    if let Some(rec) = rec {
      self.note_scan_record(rec.index, None);
      self.free_scan_record(&rec, instance);
    }
  }

  /// Fail a run with `error`. One that holds no record, or whose record
  /// went with its link, ends at once; one the coordinator still holds
  /// is closed first.
  fn fail_run(&mut self, id: ScanId, error: IcError) {
    let holds = match self.scans.get_mut(id.0) {
      Some(scan) => {
        if scan.error.is_none() {
          scan.error = Some(error);
        }
        scan.record.is_some() && !scan.coord_done
      }
      None => return,
    };
    if holds {
      if let Some(scan) = self.scans.get_mut(id.0) {
        scan.state = ScanState::Closing;
        hold_batches(scan);
      }
      self.close_when_settled(id);
      return;
    }
    self.end_run(id);
  }

  /// The run is over: its receivers' ids are given back, so that rows
  /// that come late are dropped, and the scan is done.
  fn end_run(&mut self, id: ScanId) {
    let mut ids = std::mem::take(&mut self.scan_receiver_ids);
    ids.clear();
    let rec = match self.scans.get_mut(id.0) {
      Some(scan) => {
        let mut place: usize = 0;
        while place < scan.used as usize {
          let receiver = &mut scan.receivers[place];
          if receiver.id != 0 {
            ids.push(receiver.id);
          }
          receiver.id = 0;
          receiver.clear_batch();
          receiver.takeovers = 0;
          receiver.state = ReceiverState::Done;
          place += 1;
        }
        scan.state = ScanState::Done;
        scan.queued = false;
        scan.last_read = None;
        scan.record.take()
      }
      None => None,
    };
    // A record the coordinator never let go of, as for a run ended
    // before its request went, is lost with what it may still hold.
    if let Some(rec) = rec {
      self.note_scan_record(rec.index, None);
      self.lose_coord_record(&rec);
    }
    self.drop_receiver_ids(&ids);
    self.scan_receiver_ids = ids;
    self.scans_live.retain(|live| *live != id);
    self.scans_waiting.retain(|waiting| *waiting != id);
  }
}

/// Whether a closing run's stop waits, given whether takeovers of its
/// rows are still to be received and, for a scan on a transaction,
/// whether that transaction rolls back (the author, 2026-10-21). Only
/// the takeovers of a transaction that goes on are waited for: the stop
/// releases the locks they take over, so they must reach them first. A
/// committed read, a locking scan with no transaction, and one whose
/// transaction rolls back stop at once.
fn stop_waits(takeovers_pending: bool, rolls_back: Option<bool>) -> bool {
  takeovers_pending && rolls_back == Some(false)
}

/// Let go of a closing run's batches: those with the application and
/// those handed back but not yet asked for again are held at the
/// coordinator until the stop.
fn hold_batches(scan: &mut Scan) {
  let mut place: usize = 0;
  while place < scan.used as usize {
    let receiver = &mut scan.receivers[place];
    let asked_unsent =
      receiver.state == ReceiverState::Asked && receiver.unsent;
    if receiver.state == ReceiverState::Ready || asked_unsent {
      receiver.clear_batch();
      if receiver.last {
        receiver.state = ReceiverState::Done;
      } else {
        receiver.state = ReceiverState::Held;
      }
    }
    place += 1;
  }
  scan.queued = false;
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_query_id_and_a_receiver_id_never_meet() {
    // The object map's ids have their index below bit 24; queries and
    // receivers both stay below bit 23, which marks a receiver.
    assert!(fits_below_tag(PtrId::from_u32((1 << 24) | 5)));
    assert!(fits_below_tag(PtrId::from_u32((7 << 24) | 0x7F_FFFF)));
    assert!(!fits_below_tag(PtrId::from_u32((1 << 24) | 0x80_0000)));
    let receiver = (1 << 24) | 5 | IC_RECEIVER_TAG;
    assert_ne!(receiver, (1 << 24) | 5);
    assert_eq!(receiver & !IC_RECEIVER_TAG, (1 << 24) | 5);
  }

  #[test]
  fn a_batch_id_crosses_as_one_word_and_back() {
    let batch = ScanBatchId {
      scan: ScanId::from_u32((3 << 24) | 17),
      receiver: (5 << 24) | 2 | IC_RECEIVER_TAG,
    };
    let back = ScanBatchId::from_u64(batch.as_u64());
    assert_eq!(back, batch);
    assert_eq!(back.scan(), batch.scan());
  }

  #[test]
  fn batch_sizes_follow_the_lock_and_the_configuration() {
    let committed = ScanArgs::default();
    // The configuration's rows and bytes, the bytes within their share
    // of the total.
    assert_eq!(batch_size(&committed, 8, 256, 32768, 524288), (256, 32768));
    assert_eq!(batch_size(&committed, 32, 256, 32768, 524288), (256, 16384));
    // Committed reads up to 4 000 rows, locking scans up to 992.
    let many = ScanArgs {
      batch_rows: 5000,
      ..ScanArgs::default()
    };
    assert_eq!(batch_size(&many, 8, 256, 32768, 524288).0, 4000);
    let locked = ScanArgs {
      lock: ScanLock::Shared,
      batch_rows: 5000,
      ..ScanArgs::default()
    };
    assert_eq!(batch_size(&locked, 8, 256, 32768, 524288).0, 992);
    // The scan's own bytes are taken as given.
    let own = ScanArgs {
      batch_bytes: 1 << 20,
      ..ScanArgs::default()
    };
    assert_eq!(batch_size(&own, 64, 256, 32768, 524288).1, 1 << 20);
    // At least one row.
    assert_eq!(batch_size(&committed, 8, 0, 32768, 524288).0, 1);
  }

  #[test]
  fn parallelism_is_the_fragments_unless_asked_for_or_pruned() {
    let all = ScanArgs::default();
    assert_eq!(parallelism_of(&all, 8, false), 8);
    assert_eq!(parallelism_of(&all, 0, false), 1);
    let two = ScanArgs {
      parallelism: 2,
      ..ScanArgs::default()
    };
    assert_eq!(parallelism_of(&two, 8, false), 2);
    let too_many = ScanArgs {
      parallelism: 100,
      ..ScanArgs::default()
    };
    assert_eq!(parallelism_of(&too_many, 8, false), 8);
    assert_eq!(parallelism_of(&two, 8, true), 1);
    // An ordered run merges every fragment, whatever it asks for.
    let ordered = ScanArgs {
      parallelism: 2,
      ordered: true,
      ..ScanArgs::default()
    };
    assert_eq!(parallelism_of(&ordered, 8, false), 8);
    assert_eq!(parallelism_of(&ordered, 8, true), 1);
  }

  #[test]
  fn a_row_of_a_run_of_several_ranges_starts_with_its_number() {
    let header = column_header::column_header(scans::IC_COL_RANGE_NO, 4);
    let words = [header, 7, 0xAAAA, 0xBBBB];
    let (range_no, rest) = split_range_no(&words, true, 0).expect("split");
    assert_eq!(range_no, 7);
    assert_eq!(rest, &[0xAAAA, 0xBBBB]);
    // A run of one range gives that range's number, the words whole.
    let (range_no, rest) = split_range_no(&words, false, 3).expect("whole");
    assert_eq!(range_no, 3);
    assert_eq!(rest.len(), 4);
    // A row without its number is refused.
    assert!(split_range_no(&words[2..], true, 0).is_err());
    assert!(split_range_no(&[header], true, 0).is_err());
  }

  fn head(range: u32, key: &[u8]) -> Receiver {
    Receiver {
      head_range: range,
      head_key: key.to_vec(),
      ..Receiver::default()
    }
  }

  #[test]
  fn the_merge_orders_by_range_then_by_key() {
    let a = head(0, &[1, 5]);
    let b = head(0, &[1, 6]);
    let c = head(1, &[0]);
    assert!(comes_before(&a, &b, false));
    assert!(!comes_before(&b, &a, false));
    // From the high end the keys turn round, the ranges do not.
    assert!(comes_before(&b, &a, true));
    assert!(comes_before(&a, &c, false));
    assert!(comes_before(&a, &c, true));
    // A key that is the start of another comes first.
    assert!(comes_before(&head(0, &[1]), &a, false));
    // Equals: neither comes before, so the lower place is kept.
    assert!(!comes_before(&a, &head(0, &[1, 5]), false));
  }

  #[test]
  fn a_receiver_is_ready_for_its_next_batch_with_its_buffers_kept() {
    let mut receiver = Receiver {
      confirmed: true,
      conf_rows: 2,
      conf_words: 6,
      words: 6,
      rows: vec![1, 2, 3, 4, 5, 6],
      row_ends: vec![3, 6],
      next_row: 1,
      ..Receiver::default()
    };
    let capacity = receiver.rows.capacity();
    receiver.head_at = 0;
    receiver.clear_batch();
    assert_eq!(receiver.head_at, IC_NO_HEAD);
    assert!(!receiver.confirmed);
    assert_eq!(receiver.words, 0);
    assert!(receiver.rows.is_empty() && receiver.row_ends.is_empty());
    assert_eq!(receiver.next_row, 0);
    assert_eq!(receiver.rows.capacity(), capacity);
  }

  #[test]
  fn a_close_stops_at_once_but_for_takeovers_of_a_transaction_going_on() {
    // A committed read and a locking scan with no transaction: no
    // transaction, and never a takeover.
    assert!(!stop_waits(false, None));
    // A locking scan on a transaction that goes on, started or asked to
    // commit: its takeovers still to be received go first.
    assert!(stop_waits(true, Some(false)));
    assert!(!stop_waits(false, Some(false)));
    // One whose transaction rolls back, or is gone, waits for nothing.
    assert!(!stop_waits(true, Some(true)));
    assert!(!stop_waits(false, Some(true)));
  }
}
