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
//! read started with `read_ahead` lets its receivers go on as soon as
//! their batches have moved, and handing a batch back then only
//! returns its buffers.
//!
//! **Between the two threads** is a feed: under one mutex, the batches
//! for the reader, those handed back, the empty buffers, and how the
//! run stands. The reader sleeps on the feed's condition while it has
//! no batch, and the executor signals it only then. A hand-back or a
//! close reaches the executor as a nudge on its inbox, at most one
//! waiting per run ([`thread_conn`](crate::thread_conn), "Nudges").
//!
//! Buffers go round: from the receivers to the reader, back to the
//! executor, and to the receivers again. Once they have grown to the
//! batch size, a run allocates nothing.

use std::collections::VecDeque;
use std::sync::Arc;

use ic_port::err;
use ic_port::sync::IcCond;
use ic_port::sync::IcMutex;
use ic_port::sync::IC_MUTEX_LEVEL_COMPLETION;
use ic_port::IcError;

use crate::dict_cache::TableDef;
use crate::executor::next_shape_id;
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
    }))
  }

  /// The table scanned.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
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
}

impl FeedBatch {
  /// Emptied for another batch, its allocations kept.
  pub(crate) fn clear(&mut self) {
    self.batch = None;
    self.rows.clear();
    self.row_ends.clear();
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
}

/// What a reader and the executor running its scan share.
pub(crate) struct Feed {
  pub(crate) state: IcMutex<FeedState>,
  pub(crate) cond: IcCond,
}

impl Feed {
  fn new() -> Arc<Feed> {
    Arc::new(Feed {
      state: IcMutex::new(IC_MUTEX_LEVEL_COMPLETION, FeedState::default()),
      cond: IcCond::new(),
    })
  }

  /// Run `run` is over at the executor: tell the reader, unless it has
  /// moved on or ended the run itself. The unread batches of a failed
  /// run are dropped, as on a connection; those of a run that read to
  /// the end stay for the reader. Batches handed back become spares.
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
  read_ahead: bool,
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
      read_ahead: false,
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
  /// rows dropped.
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
    loop {
      if let Some(batch) = &self.current {
        let at = self.next_row;
        if at < batch.row_ends.len() {
          let mut start: usize = 0;
          if at > 0 {
            start = batch.row_ends[at - 1] as usize;
          }
          let end = batch.row_ends[at] as usize;
          self.next_row += 1;
          let words = &batch.rows[start..end];
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

  /// A batch whose rows are read, back to the executor: its receiver
  /// goes on, or for a run that reads ahead, its buffers are spare.
  fn hand_back(&mut self, mut used: FeedBatch) {
    let mut nudge: Option<u32> = None;
    let mut state = self.feed.state.lock();
    if self.read_ahead || used.run != state.run || state.done {
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
  /// to finish it, so that the reader can run again. The batches asked
  /// for are waited for and the coordinator told to stop, as on a
  /// connection; the rows not read are dropped.
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
    while let Some(mut batch) = state.full.pop_front() {
      batch.clear();
      spare.push(batch);
    }
    spare_all(&mut state.back, &mut spare);
    *state = FeedState {
      run: self.run,
      spare,
      ..FeedState::default()
    };
    drop(state);
    self.next_row = 0;
    self.rows_read = 0;
    self.started = true;
    self.read_ahead = args.read_ahead;
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
