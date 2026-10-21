// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Scans through an executor (chapter 04, "Consuming scans"): the
//! reader, an application thread's end of a scan that an
//! [executor](crate::executor) runs.
//!
//! **A shape** ([`ScanShape`]) is a table and the record its rows are
//! read into, made once and shared, as an `OpShape` is for key
//! operations. The executor keeps scans of each shape on its
//! connection and runs a reader's scan on one of them.
//!
//! **A reader** ([`ScanReader`]) holds the row and the batches given to
//! it. When a receiver's batch is complete at the executor, its rows
//! move to the reader without a copy: the receiver's buffers and an
//! empty pair change places. The reader reads the batch's rows and
//! hands the batch back when it moves on to the next. Only then is
//! that receiver asked for more, whatever the lock (the author,
//! 2026-10-16): a locking scan's rows stay locked while they are read,
//! and a receiver has at most one batch with the reader. A committed
//! read started with `read_ahead` lets a receiver go on as soon as its
//! batch has moved while the reader holds fewer than two of its
//! batches; one whose reader holds two goes on when it hands one back,
//! so that a receiver never has more than two out, however long the
//! reader pauses (review, 2026-10-21).
//!
//! **Between the two threads** is a feed: under one mutex, the batches
//! for the reader, those handed back, the empty buffers, and how the
//! run stands. The reader sleeps on the feed's condition while it has
//! no batch, and the executor signals it only then. A hand-back or a
//! close reaches the executor as a nudge on its inbox, at most one
//! waiting per run ([`thread_conn`](crate::thread_conn), "Nudges").
//!
//! **A failed run gives no more rows**, those the reader already holds
//! included: the batch it is reading and the merge's are let go of with
//! the feed's, and the run's error given instead, so that a locking
//! scan's rows, whose locks went with the run, are never given as read
//! (review, 2026-10-21). The feed notes the failed run's number in an
//! atomic, which the reader looks at before each row it gives, so that
//! a run going well costs it no lock for the check.
//!
//! Buffers go round: from the receivers to the reader, back to the
//! executor, and to the receivers again. Once they have grown to the
//! batch size, a run allocates nothing.
//!
//! **A scan of an ordered index** (phase 8.7) has a shape over the
//! index ([`ScanShape::new_index`]), and its ranges are added to the
//! reader, encoded at once, and left in the feed for the executor to
//! take as it starts the run, under the lock it checks the run by;
//! the emptied set it leaves there takes the reader's next ranges. An
//! ordered run is merged by the reader, not by the executor, whose own
//! scan of it is unordered over every fragment: each batch
//! comes with its receiver's place and whether it is that receiver's
//! last, and the reader takes the row that comes first once every
//! receiver has a batch with a row or has ended, as a connection's
//! merge does. The batches stay where they are moved to; with
//! `read_ahead` a receiver's next batch may come before its last is
//! read, and waits. In a continuous scan (phase 10) a fragment's
//! batches come in its four receivers in turn, and the merge takes them
//! from each in that order, a head for each fragment.

use std::collections::VecDeque;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ic_port::err;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_COMPLETION;
use ic_port::IcError;

use crate::dict_cache::IndexDef;
use crate::dict_cache::TableDef;
use crate::executor::next_shape_id;
use crate::index_order::IndexOrder;
use crate::index_scan::IndexShape;
use crate::index_scan::RangeSet;
use crate::index_scan::ScanRange;
use crate::program;
use crate::program::Program;
use crate::program::IC_PROGRAM_INPUTS;
use crate::record::Record;
use crate::row_codec;
use crate::scan;
use crate::scan::ScanArgs;
use crate::scan::ScanBatchId;
use crate::thread_conn::ThreadConnection;

// ---- Shapes ----

/// The shape of a scan, made once and shared by every reader that uses
/// it: a table, and the record its rows are read into. An executor
/// keeps scans of each shape it has run, and lets them go once nobody
/// holds the shape.
pub struct ScanShape {
  id: u64,
  table: Arc<TableDef>,
  rec: Record,
  /// For scans of an ordered index: its ranges and order.
  index: Option<IndexShape>,
  /// The program that filters its rows (chapter 04, "Conditions and
  /// assignments").
  program: Option<Arc<Program>>,
}

impl std::fmt::Debug for ScanShape {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "ScanShape({})", self.table.name())
  }
}

impl ScanShape {
  /// Scans of `table` reading the columns of `rec`, which must be over
  /// this version of it.
  pub fn new(
    table: &Arc<TableDef>,
    rec: &Record,
  ) -> Result<Arc<ScanShape>, IcError> {
    scan::check_scan_record(table, rec)?;
    Ok(Arc::new(ScanShape {
      id: next_shape_id(),
      table: Arc::clone(table),
      rec: rec.clone(),
      index: None,
      program: None,
    }))
  }

  /// Scans of `index`, an ordered index, their ranges laid out by
  /// `key_rec`, which must hold every column of the index, and their
  /// rows read into `rec`, both over the index's table (chapter 04,
  /// "Index scans"). Refused for an index that is not ordered
  /// (`IC_ERROR_NOT_SUPPORTED`).
  pub fn new_index(
    index: &Arc<IndexDef>,
    key_rec: &Record,
    rec: &Record,
  ) -> Result<Arc<ScanShape>, IcError> {
    let table = index.table();
    scan::check_scan_record(table, rec)?;
    let read_part = row_codec::read_attr_info(rec);
    let shape = IndexShape::new(index, key_rec, rec, &read_part)?;
    Ok(Arc::new(ScanShape {
      id: next_shape_id(),
      table: Arc::clone(table),
      rec: rec.clone(),
      index: Some(shape),
      program: None,
    }))
  }

  /// A shape of the same table, index and record whose scans are
  /// filtered by `program`: a row it refuses is passed over at the data
  /// node. It must be for the scanned table (`IC_ERROR_BAD_PROGRAM`) and
  /// assign nothing (`IC_ERROR_NOT_SUPPORTED`).
  pub fn with_program(
    &self,
    program: &Arc<Program>,
  ) -> Result<Arc<ScanShape>, IcError> {
    program::check_program(program, &self.table, false)?;
    let index = match &self.index {
      Some(index) => {
        let read_part = row_codec::read_attr_info(&self.rec);
        Some(IndexShape::new(
          index.index(),
          index.key_record(),
          &self.rec,
          &read_part,
        )?)
      }
      None => None,
    };
    Ok(Arc::new(ScanShape {
      id: next_shape_id(),
      table: Arc::clone(&self.table),
      rec: self.rec.clone(),
      index,
      program: Some(Arc::clone(program)),
    }))
  }

  /// The program that filters its rows.
  pub fn program(&self) -> Option<&Arc<Program>> {
    self.program.as_ref()
  }

  /// The table scanned.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// The ordered index scanned, for a shape over one.
  pub fn index(&self) -> Option<&Arc<IndexDef>> {
    let index = self.index.as_ref()?;
    Some(index.index())
  }

  /// What the ranges and the order are made with, for a shape over an
  /// ordered index.
  pub(crate) fn index_shape(&self) -> Option<&IndexShape> {
    self.index.as_ref()
  }

  /// The record the rows are laid out by.
  pub fn record(&self) -> &Record {
    &self.rec
  }

  pub(crate) fn id(&self) -> u64 {
    self.id
  }
}

// ---- The feed ----

/// A batch's rows on their way between the executor and a reader.
#[derive(Default)]
pub(crate) struct FeedBatch {
  /// The run it is of, by the reader's count.
  pub(crate) run: u32,
  /// The batch at the executor's connection.
  pub(crate) batch: Option<ScanBatchId>,
  /// The rows, packed as they came, one after another.
  pub(crate) rows: Vec<u32>,
  /// Where each row ends in `rows`.
  pub(crate) row_ends: Vec<u32>,
  /// Its receiver's place in the run.
  pub(crate) place: u32,
  /// It is its receiver's last batch: in a continuous scan, its
  /// fragment's, the receiver's three companions giving no more either.
  pub(crate) last: bool,
}

impl FeedBatch {
  /// Emptied for another batch, its allocations kept.
  pub(crate) fn clear(&mut self) {
    self.batch = None;
    self.rows.clear();
    self.row_ends.clear();
    self.place = 0;
    self.last = false;
  }
}

/// What the feed's mutex protects.
#[derive(Default)]
pub(crate) struct FeedState {
  /// The reader's run, counted from one.
  pub(crate) run: u32,
  /// The executor's number for the run once it runs, which its nudges
  /// carry.
  pub(crate) flight: Option<u32>,
  /// Batches for the reader, oldest first.
  pub(crate) full: VecDeque<FeedBatch>,
  /// Batches the reader has handed back, for the executor.
  pub(crate) back: Vec<FeedBatch>,
  /// Empty buffers, for the receivers' next batches.
  pub(crate) spare: Vec<FeedBatch>,
  /// A nudge is waiting at the executor.
  pub(crate) nudged: bool,
  /// The reader has asked for the run to end.
  pub(crate) closing: bool,
  /// The run is over: read to the end, closed or failed.
  pub(crate) done: bool,
  /// Why it failed.
  pub(crate) error: Option<IcError>,
  /// The reader sleeps on the condition.
  pub(crate) reader_waiting: bool,
  /// The receivers of the run, once it runs: what an ordered run merges.
  pub(crate) receivers: u32,
  /// Receivers for each fragment of the run, once it runs: four in a
  /// continuous scan, whose fragments' batches come in theirs in turn.
  pub(crate) per_fragment: u32,
  /// For a scan of an ordered index: the run's ranges, until the
  /// executor takes them as it starts the run; then an emptied set the
  /// executor leaves for the reader's next. Kept here, not in the run's
  /// work, so that the work stays small as it goes through the inbox.
  pub(crate) ranges: RangeSet,
  /// The run's inputs for the shape's program, kept here for the same
  /// reason.
  pub(crate) inputs: [i64; IC_PROGRAM_INPUTS as usize],
}

/// What a reader and the executor running its scan share.
pub(crate) struct Feed {
  pub(crate) state: IcMutex<FeedState>,
  pub(crate) cond: IcCond,
  /// The last run that failed, by the reader's count; 0 for none. Set
  /// under the mutex with the run's end, read without it by the reader
  /// before each row it gives.
  failed: AtomicU32,
}

impl Feed {
  fn new() -> Arc<Feed> {
    Arc::new(Feed {
      state: IcMutex::new(IC_MUTEX_LEVEL_COMPLETION, FeedState::default()),
      cond: IcCond::new(),
      failed: AtomicU32::new(0),
    })
  }

  /// Run `run` is over at the executor: tell the reader, unless it has
  /// moved on or ended the run itself. The unread batches of a failed
  /// run are dropped, as on a connection, and so are those the reader
  /// holds, at its next row (`ScanReader::drop_if_failed`); those of a
  /// run that read to the end stay for the reader. Batches handed back
  /// become spares.
  fn finish_run(&self, run: u32, error: Option<IcError>) {
    let mut state = self.state.lock();
    if state.run != run || state.done {
      return;
    }
    state.done = true;
    state.error = error;
    state.flight = None;
    let mut spare = std::mem::take(&mut state.spare);
    if error.is_some() {
      self.failed.store(run, Ordering::Release);
      while let Some(mut batch) = state.full.pop_front() {
        batch.clear();
        spare.push(batch);
      }
    }
    spare_all(&mut state.back, &mut spare);
    state.spare = spare;
    let wake = state.reader_waiting;
    state.reader_waiting = false;
    drop(state);
    if wake {
      self.cond.signal();
    }
  }
}

/// Every batch in `from`, emptied, onto the spares.
fn spare_all(from: &mut Vec<FeedBatch>, spare: &mut Vec<FeedBatch>) {
  while let Some(mut batch) = from.pop() {
    batch.clear();
    spare.push(batch);
  }
}

// ---- A run, at the executor ----

/// A reader's run, on its way to the executor and running there.
pub(crate) struct ScanWork {
  pub(crate) feed: Arc<Feed>,
  pub(crate) shape: Arc<ScanShape>,
  pub(crate) args: ScanArgs,
  pub(crate) run: u32,
  /// When `args.timeout_ms` runs out, by the high-resolution clock; 0
  /// for never.
  pub(crate) deadline: u64,
  /// The reader has been told how the run ended, or needs no telling.
  ended: bool,
}

impl std::fmt::Debug for ScanWork {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "ScanWork({}, run {})", self.shape.table.name(), self.run)
  }
}

impl ScanWork {
  /// The run is over at the executor: the reader is told
  /// ([`Feed::finish_run`]), once.
  pub(crate) fn finish(&mut self, error: Option<IcError>) {
    if self.ended {
      return;
    }
    self.ended = true;
    self.feed.finish_run(self.run, error);
  }

  /// The reader needs no telling: it has moved on, or the run never
  /// reached the executor.
  pub(crate) fn forget(&mut self) {
    self.ended = true;
  }
}

impl Drop for ScanWork {
  fn drop(&mut self) {
    // Let go of untold, which happens only when the executor's thread
    // has panicked: the reader is told rather than left waiting.
    self.finish(Some(IcError::new(err::IC_ERROR_INTERNAL_PANIC)));
  }
}

// ---- The reader ----

/// An application thread's end of a scan run by an executor (chapter
/// 04, "Consuming scans"). Made once for a shape and used again and
/// again: [`Submitter::scan`](crate::executor::Submitter::scan) starts
/// a run, and [`next_row`](Self::next_row) reads it. Dropping a reader
/// ends its run without waiting.
pub struct ScanReader {
  shape: Arc<ScanShape>,
  feed: Arc<Feed>,
  /// The inbox of the executor the run is on, for nudges.
  inbox: Option<Arc<ThreadConnection>>,
  row: Vec<u8>,
  /// The batch being read, and its next row.
  current: Option<FeedBatch>,
  next_row: usize,
  rows_read: u64,
  run: u32,
  /// A run has been started; it may since have ended.
  started: bool,
  /// For a shape over an ordered index: the ranges added for the next
  /// run.
  pending: RangeSet,
  /// The run reads each row's range number; else every row is of
  /// range `single_no`.
  reads_range_no: bool,
  single_no: u32,
  /// The range number of the row last read.
  range_no: u32,
  /// The inputs of the shape's program, for the next run.
  inputs: [i64; IC_PROGRAM_INPUTS as usize],
  /// The run is merged in the index's order, from the high end when
  /// `descending`.
  ordered: bool,
  descending: bool,
  /// An ordered run's fragments, as far as the reader knows them, and
  /// what each has with the reader; and the receivers for each, four in
  /// a continuous scan.
  receivers: usize,
  per_fragment: usize,
  heads: Vec<MergeHead>,
  /// Batches that came while their receiver's last was being read.
  early: Vec<FeedBatch>,
}

/// No row of a receiver's batch is unpacked for the merge.
const IC_NO_HEAD: usize = usize::MAX;

/// One fragment's part in a reader's merge: the batch being read, and
/// its next row unpacked, with its order key and range number. A
/// fragment has a receiver, or four in a continuous scan.
struct MergeHead {
  batch: Option<FeedBatch>,
  next_row: usize,
  /// Its last batch has been read: the fragment gives no more.
  ended: bool,
  /// In a continuous run: the place, among the fragment's four
  /// receivers, of the one its next batch comes in.
  turn: usize,
  /// The row of the batch that `row`, `key` and `range` are of;
  /// `IC_NO_HEAD` when none is.
  head_at: usize,
  row: Vec<u8>,
  key: Vec<u8>,
  range: u32,
}

impl MergeHead {
  fn new() -> MergeHead {
    MergeHead {
      batch: None,
      next_row: 0,
      ended: false,
      turn: 0,
      head_at: IC_NO_HEAD,
      row: Vec::new(),
      key: Vec::new(),
      range: 0,
    }
  }

  /// True if its batch has a row not yet read.
  fn has_row(&self) -> bool {
    match &self.batch {
      Some(batch) => self.next_row < batch.row_ends.len(),
      None => false,
    }
  }
}

impl std::fmt::Debug for ScanReader {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ScanReader({}, run {}, {} rows read)",
      self.shape.table.name(),
      self.run,
      self.rows_read
    )
  }
}

impl ScanReader {
  /// A reader of scans of `shape`, its row laid out by the shape's
  /// record.
  pub fn new(shape: &Arc<ScanShape>) -> ScanReader {
    ScanReader {
      shape: Arc::clone(shape),
      feed: Feed::new(),
      inbox: None,
      row: vec![0; shape.rec.row_size() as usize],
      current: None,
      next_row: 0,
      rows_read: 0,
      run: 0,
      started: false,
      pending: RangeSet::default(),
      reads_range_no: false,
      single_no: 0,
      range_no: 0,
      inputs: [0; IC_PROGRAM_INPUTS as usize],
      ordered: false,
      descending: false,
      receivers: 0,
      per_fragment: 1,
      heads: Vec::new(),
      early: Vec::new(),
    }
  }

  /// Add a range to the next run, for a shape over an ordered index, its
  /// bounds taken from `low` and `high`, rows laid out by the shape's key
  /// record; as a connection's `add_scan_range`. Refused for a shape
  /// over a table (`IC_ERROR_NOT_SUPPORTED`).
  pub fn add_range(
    &mut self,
    range: &ScanRange,
    low: &[u8],
    high: &[u8],
  ) -> Result<(), IcError> {
    match &self.shape.index {
      Some(index) => index.add_range(&mut self.pending, range, low, high),
      None => Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    }
  }

  /// The number of the range the row last read is in.
  pub fn range_no(&self) -> u32 {
    self.range_no
  }

  /// Set input `input`, 0 to 15, of the shape's program to `value`, for
  /// the runs from the next on; each holds what it was last set to, 0 at
  /// first (`IC_ERROR_BAD_PROGRAM` beyond 15).
  pub fn set_input(&mut self, input: u32, value: i64) -> Result<(), IcError> {
    match self.inputs.get_mut(input as usize) {
      Some(slot) => {
        *slot = value;
        Ok(())
      }
      None => Err(IcError::new(err::IC_ERROR_BAD_PROGRAM)),
    }
  }

  /// The shape it reads.
  pub fn shape(&self) -> &Arc<ScanShape> {
    &self.shape
  }

  /// The row last read, laid out by the shape's record.
  pub fn row(&self) -> &[u8] {
    &self.row
  }

  /// Rows read in the run so far.
  pub fn rows_read(&self) -> u64 {
    self.rows_read
  }

  /// Batches of the run moved to the reader and not yet begun: what it
  /// holds ahead of the batch being read. A read-ahead run keeps it
  /// within two for each receiver, however long the reader pauses.
  pub fn batches_waiting(&self) -> usize {
    let state = self.feed.state.lock();
    if state.run != self.run {
      return 0;
    }
    state.full.len()
  }

  /// True once its run is over, read to the end, closed or failed, and
  /// before any run.
  pub fn is_done(&self) -> bool {
    if !self.started {
      return true;
    }
    let state = self.feed.state.lock();
    state.run != self.run || state.done
  }

  /// Why the run failed, or none if it read to the end, was closed, or
  /// is still going.
  pub fn error(&self) -> Option<IcError> {
    if !self.started {
      return None;
    }
    let state = self.feed.state.lock();
    if state.run != self.run {
      return None;
    }
    state.error
  }

  /// Read the next row into the reader's row ([`row`](Self::row)). The
  /// batch read is handed back once its rows are, and the next is
  /// waited for. False once the run has read to the end or was closed,
  /// or before any run; the run's error once it has failed, its unread
  /// rows dropped, those of the batch being read too.
  pub fn next_row(&mut self) -> Result<bool, IcError> {
    self.next_row_with(None)
  }

  /// As [`next_row`](Self::next_row), into `row`, a buffer of the
  /// caller's laid out by the shape's record, a copy less.
  pub fn next_row_into(&mut self, row: &mut [u8]) -> Result<bool, IcError> {
    if row.len() < self.shape.rec.row_size() as usize {
      return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
    }
    self.next_row_with(Some(row))
  }

  fn next_row_with(
    &mut self,
    mut row: Option<&mut [u8]>,
  ) -> Result<bool, IcError> {
    if !self.started {
      return Ok(false);
    }
    if self.ordered {
      return self.next_merged(row);
    }
    loop {
      // Before every row: the batch being read was taken while the run
      // went on, and the run may have failed since.
      if let Some(e) = self.drop_if_failed() {
        return Err(e);
      }
      if let Some(batch) = &self.current {
        let at = self.next_row;
        if at < batch.row_ends.len() {
          let mut start: usize = 0;
          if at > 0 {
            start = batch.row_ends[at - 1] as usize;
          }
          let end = batch.row_ends[at] as usize;
          self.next_row += 1;
          let (range_no, words) = scan::split_range_no(
            &batch.rows[start..end],
            self.reads_range_no,
            self.single_no,
          )?;
          self.range_no = range_no;
          match row.as_deref_mut() {
            Some(into) => row_codec::unpack_row(&self.shape.rec, words, into)?,
            None => {
              row_codec::unpack_row(&self.shape.rec, words, &mut self.row)?
            }
          };
          self.rows_read += 1;
          return Ok(true);
        }
      }
      if let Some(used) = self.current.take() {
        self.hand_back(used);
      }
      // The next batch, waited for.
      let mut state = self.feed.state.lock();
      loop {
        if let Some(mut next) = state.full.pop_front() {
          if next.run == self.run {
            self.current = Some(next);
            self.next_row = 0;
            break;
          }
          next.clear();
          state.spare.push(next);
          continue;
        }
        if state.run != self.run {
          return Ok(false);
        }
        if state.done {
          return match state.error {
            Some(e) => Err(e),
            None => Ok(false),
          };
        }
        state.reader_waiting = true;
        state = self.feed.cond.wait(state);
      }
    }
  }

  /// If the run has failed, let go of the rows the reader still holds
  /// of it, the batch being read and the merge's, onto the spares, and
  /// give the run's error, every time it is asked: no row of a failed
  /// run is given once its failure is known here. One atomic load while
  /// the run is well, the mutex only once it has failed.
  fn drop_if_failed(&mut self) -> Option<IcError> {
    if self.feed.failed.load(Ordering::Acquire) != self.run {
      return None;
    }
    let mut state = self.feed.state.lock();
    if let Some(mut used) = self.current.take() {
      used.clear();
      state.spare.push(used);
    }
    spare_merge(&mut self.heads, &mut self.early, &mut state.spare);
    if state.run != self.run {
      return None;
    }
    state.error
  }

  /// A batch whose rows are read, back to the executor: its receiver
  /// goes on, or for a run that reads ahead, one held because the reader
  /// had two of its batches does.
  fn hand_back(&mut self, mut used: FeedBatch) {
    let mut nudge: Option<u32> = None;
    let mut state = self.feed.state.lock();
    if used.run != state.run || state.done {
      used.clear();
      state.spare.push(used);
    } else {
      state.back.push(used);
      if !state.nudged {
        if let Some(flight) = state.flight {
          state.nudged = true;
          nudge = Some(flight);
        }
      }
    }
    drop(state);
    self.send_nudge(nudge);
  }

  // ---- The ordered merge ----

  /// The next row of an ordered run, merged from its receivers' batches
  /// (chapter 04, "Index scans"), into `row` or the reader's row. The
  /// batches that came go to their receivers, those read are handed
  /// back, and once every receiver has a row or has ended, the row that
  /// comes first is taken; until then the reader waits. A failed run's
  /// heads give no more.
  fn next_merged(
    &mut self,
    mut row: Option<&mut [u8]>,
  ) -> Result<bool, IcError> {
    loop {
      if let Some(e) = self.drop_if_failed() {
        return Err(e);
      }
      if !self.collect_batches() {
        return Ok(false);
      }
      self.hand_back_read();
      if let Some(place) = self.first_head()? {
        self.take_head(place, row.as_deref_mut());
        return Ok(true);
      }
      let mut state = self.feed.state.lock();
      loop {
        if state.run != self.run {
          return Ok(false);
        }
        if !state.full.is_empty() {
          break;
        }
        if state.done {
          return match state.error {
            Some(e) => Err(e),
            None => Ok(false),
          };
        }
        state.reader_waiting = true;
        state = self.feed.cond.wait(state);
      }
    }
  }

  /// Take the batches that came, each to its fragment, and learn how
  /// many fragments the run has. False if the reader has moved on from
  /// the run.
  fn collect_batches(&mut self) -> bool {
    let mut state = self.feed.state.lock();
    if state.run != self.run {
      return false;
    }
    self.per_fragment = (state.per_fragment as usize).max(1);
    self.receivers = state.receivers as usize / self.per_fragment;
    while self.heads.len() < self.receivers {
      self.heads.push(MergeHead::new());
    }
    while let Some(mut batch) = state.full.pop_front() {
      if batch.run != self.run {
        batch.clear();
        state.spare.push(batch);
        continue;
      }
      place_batch(&mut self.heads, &mut self.early, batch, self.per_fragment);
    }
    true
  }

  /// Hand back each batch whose rows are all read, its fragment ended if
  /// it was the last, and give the fragment its next if it came early,
  /// in the receiver whose turn it is.
  fn hand_back_read(&mut self) {
    let per = self.per_fragment;
    let mut at: usize = 0;
    while at < self.heads.len() {
      let used_up = self.heads[at].batch.is_some() && !self.heads[at].has_row();
      if !used_up {
        at += 1;
        continue;
      }
      if let Some(used) = self.heads[at].batch.take() {
        self.heads[at].ended = used.last;
        if !used.last {
          self.heads[at].turn = (self.heads[at].turn + 1) % per;
        }
        self.hand_back(used);
      }
      let next_place = at * per + self.heads[at].turn;
      if let Some(next) = take_early(&mut self.early, next_place as u32) {
        place_batch(&mut self.heads, &mut self.early, next, per);
      }
      // The same fragment again: the batch given it may be empty.
    }
  }

  /// The place of the receiver whose next row comes first, once every
  /// receiver has a row or has ended; none while one waits for its
  /// batch, when every one has ended, or before the run has started.
  fn first_head(&mut self) -> Result<Option<usize>, IcError> {
    if self.receivers == 0 {
      return Ok(None);
    }
    let order = match &self.shape.index {
      Some(index) => match &index.order {
        Ok(order) => order,
        Err(e) => return Err(*e),
      },
      None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
    };
    let mut first: Option<usize> = None;
    let mut place: usize = 0;
    while place < self.receivers {
      let head = &mut self.heads[place];
      if head.ended {
        place += 1;
        continue;
      }
      if !head.has_row() {
        return Ok(None);
      }
      fill_head(
        &self.shape.rec,
        order,
        self.reads_range_no,
        self.single_no,
        head,
      )?;
      let before = match first {
        Some(best) => {
          head_before(&self.heads[place], &self.heads[best], self.descending)
        }
        None => true,
      };
      if before {
        first = Some(place);
      }
      place += 1;
    }
    Ok(first)
  }

  /// Take the merged row of the receiver at `place` into `row`, or the
  /// reader's row.
  fn take_head(&mut self, place: usize, row: Option<&mut [u8]>) {
    let head = &mut self.heads[place];
    match row {
      Some(into) => into[..head.row.len()].copy_from_slice(&head.row),
      None => self.row.copy_from_slice(&head.row),
    }
    head.next_row += 1;
    self.range_no = head.range;
    self.rows_read += 1;
  }

  fn send_nudge(&self, nudge: Option<u32>) {
    if let (Some(word), Some(inbox)) = (nudge, &self.inbox) {
      inbox.nudge(word);
    }
  }

  /// Ask for the run to end. One not yet running at the executor ends
  /// at once; the executor drops it when its turn comes.
  fn ask_close(&mut self) {
    let mut nudge: Option<u32> = None;
    let mut state = self.feed.state.lock();
    if let Some(mut used) = self.current.take() {
      used.clear();
      state.spare.push(used);
    }
    spare_merge(&mut self.heads, &mut self.early, &mut state.spare);
    // The rows not read are dropped. No more come once `closing` is
    // set, which the executor reads under this lock.
    let mut spare = std::mem::take(&mut state.spare);
    while let Some(mut batch) = state.full.pop_front() {
      batch.clear();
      spare.push(batch);
    }
    state.spare = spare;
    if state.run == self.run && !state.done && !state.closing {
      state.closing = true;
      match state.flight {
        Some(flight) => {
          if !state.nudged {
            state.nudged = true;
            nudge = Some(flight);
          }
        }
        None => state.done = true,
      }
    }
    drop(state);
    self.send_nudge(nudge);
  }

  /// End the run before it reads to the end, and wait for the executor
  /// to finish it, so that the reader can run again. The coordinator is
  /// told to stop at once, as on a connection, and the rows not read are
  /// dropped.
  pub fn close(&mut self) {
    if !self.started {
      return;
    }
    self.ask_close();
    let mut state = self.feed.state.lock();
    while state.run == self.run && !state.done {
      state.reader_waiting = true;
      state = self.feed.cond.wait(state);
    }
  }

  /// Make ready for a new run, for the executor at `inbox`. Refused
  /// with `IC_ERROR_SCAN_ACTIVE` while the last run is not over.
  pub(crate) fn begin_run(
    &mut self,
    inbox: &Arc<ThreadConnection>,
    args: &ScanArgs,
  ) -> Result<ScanWork, IcError> {
    if !self.is_done() {
      return Err(IcError::new(err::IC_ERROR_SCAN_ACTIVE));
    }
    if args.ordered || args.descending {
      let index = match &self.shape.index {
        Some(index) => index,
        None => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
      };
      if args.ordered {
        index.check_ordered(&self.pending)?;
      }
    }
    let mut deadline: u64 = 0;
    if args.timeout_ms > 0 {
      deadline =
        ic_port::time::gethrtime() + args.timeout_ms as u64 * 1_000_000;
    }
    self.run = self.run.wrapping_add(1);
    if self.run == 0 {
      self.run = 1;
    }
    let mut state = self.feed.state.lock();
    let mut spare = std::mem::take(&mut state.spare);
    if let Some(mut used) = self.current.take() {
      used.clear();
      spare.push(used);
    }
    spare_merge(&mut self.heads, &mut self.early, &mut spare);
    while let Some(mut batch) = state.full.pop_front() {
      batch.clear();
      spare.push(batch);
    }
    spare_all(&mut state.back, &mut spare);
    // The run takes the ranges added, for the executor; the set left
    // there, emptied by the executor or a run's never taken, takes the
    // next.
    let mut ranges = std::mem::take(&mut state.ranges);
    std::mem::swap(&mut ranges, &mut self.pending);
    self.pending.clear();
    self.reads_range_no = ranges.reads_range_no();
    self.single_no = ranges.first_no;
    *state = FeedState {
      run: self.run,
      spare,
      ranges,
      inputs: self.inputs,
      ..FeedState::default()
    };
    drop(state);
    self.next_row = 0;
    self.rows_read = 0;
    self.started = true;
    self.range_no = 0;
    self.ordered = args.ordered;
    self.descending = args.descending;
    self.receivers = 0;
    self.per_fragment = 1;
    self.inbox = Some(Arc::clone(inbox));
    Ok(ScanWork {
      feed: Arc::clone(&self.feed),
      shape: Arc::clone(&self.shape),
      args: *args,
      run: self.run,
      deadline,
      ended: false,
    })
  }

  /// The run was refused before it reached the executor: it is over,
  /// with `error`.
  pub(crate) fn abandon_run(&mut self, mut work: ScanWork, error: IcError) {
    work.forget();
    let mut state = self.feed.state.lock();
    if state.run == self.run {
      state.done = true;
      state.error = Some(error);
    }
  }
}

/// A batch to its fragment's place in the merge, `per` receivers to a
/// fragment, or to wait with the early ones if the fragment has one
/// already, or in a continuous run if its receiver's turn has not come.
fn place_batch(
  heads: &mut Vec<MergeHead>,
  early: &mut Vec<FeedBatch>,
  batch: FeedBatch,
  per: usize,
) {
  let place = batch.place as usize;
  let fragment = place / per;
  while heads.len() <= fragment {
    heads.push(MergeHead::new());
  }
  let head = &mut heads[fragment];
  let in_turn = place == fragment * per + head.turn;
  if head.batch.is_some() || head.ended || !in_turn {
    early.push(batch);
    return;
  }
  head.batch = Some(batch);
  head.next_row = 0;
  head.head_at = IC_NO_HEAD;
}

/// The first batch waiting for the receiver at `place`, taken from the
/// early ones.
fn take_early(early: &mut Vec<FeedBatch>, place: u32) -> Option<FeedBatch> {
  let mut at: usize = 0;
  while at < early.len() {
    if early[at].place == place {
      return Some(early.remove(at));
    }
    at += 1;
  }
  None
}

/// Every batch the merge holds, emptied, onto the spares, and every
/// receiver's part made ready for another run.
fn spare_merge(
  heads: &mut [MergeHead],
  early: &mut Vec<FeedBatch>,
  spare: &mut Vec<FeedBatch>,
) {
  for head in heads {
    if let Some(mut batch) = head.batch.take() {
      batch.clear();
      spare.push(batch);
    }
    head.ended = false;
    head.turn = 0;
    head.next_row = 0;
    head.head_at = IC_NO_HEAD;
  }
  spare_all(early, spare);
}

/// Unpack the receiver's next row into its head for the merge, with its
/// order key and range number, unless it is there already.
fn fill_head(
  rec: &Record,
  order: &IndexOrder,
  reads_range_no: bool,
  single_no: u32,
  head: &mut MergeHead,
) -> Result<(), IcError> {
  let at = head.next_row;
  if head.head_at == at {
    return Ok(());
  }
  let batch = match &head.batch {
    Some(batch) => batch,
    None => return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
  };
  let size = rec.row_size() as usize;
  if head.row.len() != size {
    head.row.resize(size, 0);
  }
  let mut start: usize = 0;
  if at > 0 {
    start = batch.row_ends[at - 1] as usize;
  }
  let end = batch.row_ends[at] as usize;
  let (range_no, words) =
    scan::split_range_no(&batch.rows[start..end], reads_range_no, single_no)?;
  row_codec::unpack_row(rec, words, &mut head.row)?;
  order.key_into(rec, &head.row, &mut head.key)?;
  head.range = range_no;
  head.head_at = at;
  Ok(())
}

/// True if `a`'s head row comes before `b`'s: by range number, then by
/// order key, the keys reversed for a run from the high end.
fn head_before(a: &MergeHead, b: &MergeHead, descending: bool) -> bool {
  if a.range != b.range {
    return a.range < b.range;
  }
  if descending {
    return a.key > b.key;
  }
  a.key < b.key
}

impl Drop for ScanReader {
  fn drop(&mut self) {
    if self.started {
      self.ask_close();
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn feed_with_run(run: u32) -> Arc<Feed> {
    let feed = Feed::new();
    feed.state.lock().run = run;
    feed
  }

  fn batch(run: u32, rows: &[u32]) -> FeedBatch {
    FeedBatch {
      run,
      rows: rows.to_vec(),
      row_ends: vec![rows.len() as u32],
      ..FeedBatch::default()
    }
  }

  #[test]
  fn a_failed_run_drops_its_unread_batches_and_one_read_to_the_end_keeps_them()
  {
    for error in [None, Some(IcError::new(274))] {
      let feed = feed_with_run(3);
      {
        let mut state = feed.state.lock();
        state.full.push_back(batch(3, &[1, 2]));
        state.back.push(batch(3, &[7]));
      }
      feed.finish_run(3, error);
      let state = feed.state.lock();
      assert!(state.done);
      assert_eq!(state.error.map(|e| e.code), error.map(|e| e.code));
      assert!(state.back.is_empty(), "handed back: spare");
      assert!(state.spare.iter().all(|b| b.rows.is_empty()), "emptied");
      match error {
        Some(_) => {
          assert!(state.full.is_empty());
          assert_eq!(state.spare.len(), 2);
        }
        None => {
          assert_eq!(state.full.len(), 1, "left for the reader");
          assert_eq!(state.spare.len(), 1);
        }
      }
    }
  }

  /// A reader in run `run`, of a shape over the test table, its feed at
  /// that run as the executor leaves it.
  fn reader_on_run(run: u32) -> ScanReader {
    let table = crate::program::test_table();
    let rec = Record::default_for(&table, None).expect("a record");
    let shape = ScanShape::new(&table, &rec).expect("a shape");
    let mut reader = ScanReader::new(&shape);
    reader.run = run;
    reader.started = true;
    reader.feed.state.lock().run = run;
    reader
  }

  /// Give the reader a batch being read and a merge head and an early
  /// batch, each with rows not yet read.
  fn hold_rows(reader: &mut ScanReader) {
    reader.current = Some(batch(reader.run, &[1, 2]));
    reader.next_row = 0;
    let head = FeedBatch {
      run: reader.run,
      ..of_place(0, &[5, 6], false)
    };
    place_batch(&mut reader.heads, &mut reader.early, head, 1);
    reader.early.push(FeedBatch {
      run: reader.run,
      ..of_place(0, &[7], false)
    });
  }

  #[test]
  fn a_failed_run_gives_no_row_its_reader_still_holds() {
    // While the run goes on, and once it has read to the end, the rows
    // the reader holds are its own to read.
    let mut reader = reader_on_run(3);
    hold_rows(&mut reader);
    assert!(reader.drop_if_failed().is_none());
    reader.feed.finish_run(3, None);
    assert!(reader.drop_if_failed().is_none());
    assert!(reader.current.is_some() && reader.heads[0].has_row());
    // A failed run's are let go of, and its error given, each time.
    let mut reader = reader_on_run(3);
    hold_rows(&mut reader);
    reader.feed.finish_run(3, Some(IcError::new(274)));
    assert_eq!(reader.drop_if_failed().map(|e| e.code), Some(274));
    assert!(reader.current.is_none());
    assert!(reader.heads.iter().all(|head| head.batch.is_none()));
    assert!(reader.early.is_empty());
    assert_eq!(reader.feed.state.lock().spare.len(), 3, "spares, all");
    assert_eq!(reader.drop_if_failed().map(|e| e.code), Some(274));
    // Through next_row: the error, no row of the batch it held, whether
    // the run is read as it comes or merged.
    for ordered in [false, true] {
      let mut reader = reader_on_run(5);
      reader.ordered = ordered;
      hold_rows(&mut reader);
      reader.feed.finish_run(5, Some(IcError::new(4028)));
      assert_eq!(reader.next_row().map_err(|e| e.code), Err(4028));
      assert_eq!(reader.rows_read(), 0, "ordered {}", ordered);
      assert_eq!(reader.next_row().map_err(|e| e.code), Err(4028));
    }
    // An earlier run's failure is not this one's.
    let mut reader = reader_on_run(4);
    hold_rows(&mut reader);
    reader.feed.failed.store(3, Ordering::Release);
    assert!(reader.drop_if_failed().is_none());
    assert!(reader.current.is_some());
  }

  fn of_place(place: u32, rows: &[u32], last: bool) -> FeedBatch {
    FeedBatch {
      place,
      last,
      ..batch(1, rows)
    }
  }

  #[test]
  fn batches_go_to_their_receivers_and_wait_their_turn() {
    let mut heads: Vec<MergeHead> = Vec::new();
    let mut early: Vec<FeedBatch> = Vec::new();
    place_batch(&mut heads, &mut early, of_place(2, &[7], false), 1);
    assert_eq!(heads.len(), 3, "room up to its place");
    assert!(heads[2].has_row());
    assert!(!heads[0].has_row() && !heads[1].has_row());
    // A second for the same receiver waits, behind any before it.
    place_batch(&mut heads, &mut early, of_place(2, &[8], true), 1);
    place_batch(&mut heads, &mut early, of_place(2, &[9], true), 1);
    assert_eq!(early.len(), 2);
    assert!(take_early(&mut early, 1).is_none());
    let next = take_early(&mut early, 2).expect("the next of place 2");
    assert_eq!(next.rows, vec![8]);
    assert!(next.last);
    // An ended receiver takes none.
    heads[0].ended = true;
    place_batch(&mut heads, &mut early, of_place(0, &[5], false), 1);
    assert!(heads[0].batch.is_none());
    // Made ready for another run, every batch a spare.
    let mut spare: Vec<FeedBatch> = Vec::new();
    spare_merge(&mut heads, &mut early, &mut spare);
    assert_eq!(spare.len(), 3);
    assert!(!heads[0].ended);
    assert!(heads[2].batch.is_none());
    assert!(early.is_empty());
  }

  #[test]
  fn a_continuous_fragments_batches_are_taken_in_their_turn() {
    // Fragment 1's receivers are places 4 to 7. Its second batch, in
    // place 5, comes before its first, in place 4: it waits.
    let mut heads: Vec<MergeHead> = Vec::new();
    let mut early: Vec<FeedBatch> = Vec::new();
    place_batch(&mut heads, &mut early, of_place(5, &[2], false), 4);
    assert_eq!(heads.len(), 2, "a head for each fragment");
    assert!(heads[1].batch.is_none());
    assert_eq!(early.len(), 1);
    place_batch(&mut heads, &mut early, of_place(4, &[1], false), 4);
    assert!(heads[1].has_row());
    // The first read and its turn passed on: the second is in turn.
    heads[1].batch = None;
    heads[1].turn = 1;
    let next = take_early(&mut early, 5).expect("the second batch");
    place_batch(&mut heads, &mut early, next, 4);
    assert!(heads[1].has_row());
    // Round the four: the fifth batch is in place 4 again.
    heads[1].batch = None;
    heads[1].turn = 3;
    place_batch(&mut heads, &mut early, of_place(4, &[5], false), 4);
    assert!(heads[1].batch.is_none(), "place 7's turn, not 4's");
    heads[1].turn = 0;
    let fifth = take_early(&mut early, 4).expect("the fifth batch");
    place_batch(&mut heads, &mut early, fifth, 4);
    assert_eq!(
      heads[1].batch.as_ref().map(|b| b.rows.clone()),
      Some(vec![5])
    );
  }

  #[test]
  fn the_reader_merges_by_range_then_by_key() {
    let mut a = MergeHead::new();
    a.key = vec![1, 5];
    let mut b = MergeHead::new();
    b.key = vec![1, 6];
    let mut c = MergeHead::new();
    c.range = 1;
    assert!(head_before(&a, &b, false));
    assert!(head_before(&b, &a, true));
    assert!(head_before(&a, &c, false) && head_before(&a, &c, true));
  }

  #[test]
  fn a_run_the_reader_has_moved_on_from_is_not_told() {
    let feed = feed_with_run(5);
    feed.finish_run(4, Some(IcError::new(274)));
    let state = feed.state.lock();
    assert!(!state.done);
    assert!(state.error.is_none());
  }

  #[test]
  fn a_waiting_reader_is_woken_by_the_end_of_its_run() {
    let feed = feed_with_run(1);
    let ender = Arc::clone(&feed);
    let handle = std::thread::spawn(move || {
      ic_port::time::microsleep(50_000);
      ender.finish_run(1, None);
    });
    let start = ic_port::time::gethrtime();
    let mut state = feed.state.lock();
    while !state.done {
      state.reader_waiting = true;
      state = feed.cond.wait(state);
    }
    drop(state);
    handle.join().expect("joined");
    let waited =
      ic_port::time::millis_elapsed(start, ic_port::time::gethrtime());
    assert!(waited < 5_000, "woken after {} ms", waited);
  }
}
