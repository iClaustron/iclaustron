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
//! waits for the batches asked for, then stops, and the record goes
//! back to its pool once the coordinator confirms. Rows that come after
//! the end find their receiver's id stale and are dropped.
//!
//! A scan started on a transaction, so that its rows may be taken over
//! (phase 8.5), runs at the transaction's coordinator instance, under
//! its id, and names its record; any other scan names none and runs
//! under an id of its own (the author, 2026-10-16).

use std::sync::Arc;

use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;
use ic_signals::blocks;
use ic_signals::coord_alloc::IC_ANY_COORD_INSTANCE;
use ic_signals::header::SignalHeader;
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
use crate::dict_cache::TableDef;
use crate::node_connect::SignalView;
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
  /// Close the scan with `IC_ERROR_TIMEOUT` if it is still running this
  /// long after it was started, in milliseconds; 0 for no limit.
  pub timeout_ms: u32,
  /// Anything, given back to the callback.
  pub user_ref: usize,
  /// Called with each batch as it completes, in place of the ready
  /// list.
  pub callback: Option<ScanCallback>,
  /// A word for a language binding's own use; see
  /// [`crate::query::ReadKeyArgs::binding`].
  pub binding: usize,
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
  /// A close was asked for, or an error ends the run: waiting for what
  /// was asked for, then for the coordinator to confirm the close.
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
  /// each row's word, for a takeover (phase 8.5).
  keys: Vec<u32>,
  key_ends: Vec<u32>,
  key_rows: Vec<u32>,
}

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
    let over = rec.table().table_id() == table.table_id()
      && rec.table().table_version() == table.table_version();
    if !over {
      return Err(IcError::new(err::IC_ERROR_KEY_RECORD));
    }
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
    })
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

  /// True if no receiver of the run waits for a batch it asked for.
  fn nothing_asked(&self) -> bool {
    let mut place: usize = 0;
    while place < self.used as usize {
      if self.receivers[place].state == ReceiverState::Asked {
        return false;
      }
      place += 1;
    }
    true
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
  /// reference's do, and asks for the extended confirmation.
  fn flags(&self) -> ScanFlags {
    let mut flags = ScanFlags {
      no_disk: !self.reads_disk(),
      storage_order: true,
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

/// How many fragments a run scans at once: one for a partition, else
/// the scan's own, at most the table's fragments.
fn parallelism_of(args: &ScanArgs, fragments: u32) -> u32 {
  if args.partition.is_some() {
    return 1;
  }
  let fragments = fragments.max(1);
  if args.parallelism == 0 || args.parallelism > fragments {
    return fragments;
  }
  args.parallelism
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
    let (fragments, table) = match self.scans.get(id.0) {
      Some(scan) => {
        if scan.is_running() {
          return Err(IcError::new(err::IC_ERROR_SCAN_ACTIVE));
        }
        (scan.table.info().fragment_count, Arc::clone(&scan.table))
      }
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
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
        if let Some(partition) = args.partition {
          hint = TransactionHint::Partition(partition);
        }
        let mut started = std::mem::take(&mut self.started_scratch);
        self.shared.started_nodes_into(&mut started);
        let chosen =
          self.choose_start_node(hint, Some(table.as_ref()), &started);
        self.started_scratch = started;
        (chosen?, self.next_transaction_id())
      }
    };
    let parallelism = parallelism_of(args, fragments);
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
      scan.partition = args.partition;
      scan.batch_rows = batch_rows;
      scan.batch_bytes = batch_bytes;
      scan.record = None;
      scan.queued = false;
      scan.stop_sent = false;
      scan.coord_done = false;
      scan.deadline = deadline;
      scan.rows_read = 0;
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
    let queued = match self.scans.get_mut(id.0) {
      Some(scan) => {
        scan.record = Some(rec);
        scan.state = ScanState::Running;
        let mut word5 = scans::IC_SCAN_WORD5_PLAIN;
        if scan.partition.is_some() {
          word5 |= scans::IC_SCAN_WORD5_PARTITION_ID;
        }
        let request = ScanReq {
          coord_ptr: rec.coord_ptr,
          flags_word: scan.flags().flags_word(),
          table_id: scan.table.table_id(),
          table_version: scan.table.table_version(),
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
        scans::column_section_into(&scan.read_part, &[], &[], &mut columns);
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
      let sections: [&[u32]; 2] = [&ids, &columns];
      let sent = self.queue_signal_for_generation(
        rec.node_id,
        rec.generation,
        &header,
        &words,
        &sections,
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
    let place = match self.ready_receiver(batch) {
      Some(place) => place,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let scan = match self.scans.get_mut(batch.scan.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let receiver = &mut scan.receivers[place as usize];
    let at = receiver.next_row as usize;
    if at >= receiver.row_ends.len() {
      return Ok(false);
    }
    let mut start: usize = 0;
    if at > 0 {
      start = receiver.row_ends[at - 1] as usize;
    }
    let end = receiver.row_ends[at] as usize;
    receiver.next_row += 1;
    row_codec::unpack_row(
      &scan.rec,
      &receiver.rows[start..end],
      &mut scan.row,
    )?;
    scan.rows_read += 1;
    Ok(true)
  }

  /// As [`next_scan_row`](Self::next_scan_row), into `row`, a buffer of
  /// the caller's laid out by the scan's record, a copy less.
  pub fn next_scan_row_into(
    &mut self,
    batch: ScanBatchId,
    row: &mut [u8],
  ) -> Result<bool, IcError> {
    let place = match self.ready_receiver(batch) {
      Some(place) => place,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    let scan = match self.scans.get_mut(batch.scan.0) {
      Some(scan) => scan,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    if row.len() < scan.rec.row_size() as usize {
      return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
    }
    let receiver = &mut scan.receivers[place as usize];
    let at = receiver.next_row as usize;
    if at >= receiver.row_ends.len() {
      return Ok(false);
    }
    let mut start: usize = 0;
    if at > 0 {
      start = receiver.row_ends[at - 1] as usize;
    }
    let end = receiver.row_ends[at] as usize;
    receiver.next_row += 1;
    row_codec::unpack_row(&scan.rec, &receiver.rows[start..end], row)?;
    scan.rows_read += 1;
    Ok(true)
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
  /// batches asked for are waited for, then the coordinator is told to
  /// stop, and the run is done when it confirms. Batches with the
  /// application are let go of. A run not yet sent ends at once.
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
            if receiver.unsent {
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
      None => return false,
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
      None => return false,
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
      let over = match self.scans.get(id.0) {
        Some(scan) => scan.all_done(),
        None => false,
      };
      if over {
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
  fn deliver(&mut self, id: ScanId, place: u32) {
    let (receiver, callback, user_ref) = match self.scans.get(id.0) {
      Some(scan) => (
        scan.receivers[place as usize].id,
        scan.callback,
        scan.user_ref,
      ),
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

  /// Send the stop of a closing run once nothing it asked for is still
  /// coming; a run the coordinator is done with ends at once.
  fn close_when_settled(&mut self, id: ScanId) {
    let (settled, stop_sent, coord_done) = match self.scans.get(id.0) {
      Some(scan) => (scan.nothing_asked(), scan.stop_sent, scan.coord_done),
      None => return,
    };
    if !settled || stop_sent {
      return;
    }
    if coord_done {
      self.end_run(id);
      return;
    }
    self.queue_more(id, true);
    self.scan_send_due = true;
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
          receiver.state = ReceiverState::Done;
          place += 1;
        }
        scan.state = ScanState::Done;
        scan.queued = false;
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
    assert_eq!(parallelism_of(&all, 8), 8);
    assert_eq!(parallelism_of(&all, 0), 1);
    let two = ScanArgs {
      parallelism: 2,
      ..ScanArgs::default()
    };
    assert_eq!(parallelism_of(&two, 8), 2);
    let too_many = ScanArgs {
      parallelism: 100,
      ..ScanArgs::default()
    };
    assert_eq!(parallelism_of(&too_many, 8), 8);
    let one = ScanArgs {
      partition: Some(3),
      ..ScanArgs::default()
    };
    assert_eq!(parallelism_of(&one, 8), 1);
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
    receiver.clear_batch();
    assert!(!receiver.confirmed);
    assert_eq!(receiver.words, 0);
    assert!(receiver.rows.is_empty() && receiver.row_ends.is_empty());
    assert_eq!(receiver.next_row, 0);
    assert_eq!(receiver.rows.capacity(), capacity);
  }
}
