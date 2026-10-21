// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Scans of an ordered index (chapter 04, "Index scans"): the ranges a
//! run reads, given as rows of a key record and encoded at once as the
//! request carries them (chapter 05 §7.2). New: the C had no range
//! scans.
//!
//! **A range** is a low and a high bound, each the values of the
//! index's first few columns, taken from a row laid out by the key
//! record, and a number. A bound is compared as the index orders its
//! rows, column by column, so every column of a bound but its last is
//! sent inclusive, as the reference sends them, and only the last says
//! whether the bound's own values are in the range. A range whose two
//! bounds take the same columns with the same values, both inclusive,
//! goes as equalities. A range with no bound at all takes in every
//! row: the first column at least NULL.
//!
//! **Pruning**: when every range of a run fixes the values of the
//! table's distribution key columns, the same in both bounds, and they
//! all place it in the same partition, the run scans that partition
//! alone, as the reference prunes. The partition is worked out from the
//! low bound's row as a key's is ([`hash::partition_of`]); one that
//! cannot be worked out, as for a character column whose collation was
//! not described, leaves the run unpruned. On a fanout table the whole
//! primary key places a row, and a run whose ranges fix only its base
//! columns, all with the same base, scans the fanout's partitions for
//! that base ([`hash::fanout_first_of`]; phase 10). The base columns
//! are found wherever they are in the index, not only first, as the
//! reference has them.
//!
//! **Two parts**: an [`IndexShape`], what the ranges and the order are
//! made with, which never changes and which a connection's scan and a
//! reader's scan shape each hold; and [`RangeSet`]s, ranges encoded,
//! held by whoever adds them: a connection's scan, or a reader, whose
//! ranges go to the executor with its run (chapter 04, "Index scans").

use std::sync::Arc;

use ic_port::err;
use ic_port::IcError;
use ic_signals::column_header;
use ic_signals::scans;

use crate::dict_cache::IndexDef;
use crate::hash;
use crate::index_order::IndexOrder;
use crate::record::Record;
use crate::row_codec;
use crate::scan::check_scan_record;

/// One range of an index scan (chapter 04, "Index scans"), its bounds
/// taken from rows laid out by the scan's key record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanRange {
  /// How many of the index's leading columns the low bound takes from
  /// its row; 0 for no low bound.
  pub low_columns: u32,
  /// The low bound's own values are in the range.
  pub low_inclusive: bool,
  /// How many of the index's leading columns the high bound takes from
  /// its row; 0 for no high bound.
  pub high_columns: u32,
  /// The high bound's own values are in the range.
  pub high_inclusive: bool,
  /// The range's number, 0 to 4095, given back with each of its rows
  /// when a run has several ranges.
  pub range_no: u32,
}

/// A column of the index, as a bound takes it.
#[derive(Clone, Copy, Debug)]
struct BoundColumn {
  /// Its id in the table.
  field_id: u32,
  /// Its place in the key record.
  position: u32,
  /// Its id in the index's own table, which a bound's header names.
  index_column: u32,
}

/// Ranges encoded for a run, one after another, as the request carries
/// them.
#[derive(Debug, Default)]
pub(crate) struct RangeSet {
  pub(crate) bounds: Vec<u32>,
  pub(crate) count: u32,
  /// The first range's number: the number of every row of a run with
  /// one range.
  pub(crate) first_no: u32,
  last_no: u32,
  /// Each range's number is above the one before.
  pub(crate) rising: bool,
  /// Every range is in `partition`.
  pub(crate) pruned: bool,
  pub(crate) partition: u32,
  /// On a fanout table: every range's base columns are fixed, with the
  /// same base, whose first hash value is `first`.
  pub(crate) in_fanout: bool,
  pub(crate) first: u32,
}

impl RangeSet {
  /// The partition every range is in, if they are all in one.
  pub(crate) fn pruned_partition(&self) -> Option<u32> {
    if self.count > 0 && self.pruned {
      return Some(self.partition);
    }
    None
  }

  /// On a fanout table, the first hash value of the base every range
  /// fixes, if they all fix the same one and are not in one partition:
  /// the run then scans the fanout's partitions for it.
  pub(crate) fn pruned_fanout(&self) -> Option<u32> {
    if self.count > 0 && self.in_fanout && !self.pruned {
      return Some(self.first);
    }
    None
  }

  /// True if a run of these ranges reads each row's range number: a run
  /// of more than one.
  pub(crate) fn reads_range_no(&self) -> bool {
    self.count > 1
  }

  /// The same ranges as `other`, in this set's own buffer.
  pub(crate) fn copy_from(&mut self, other: &RangeSet) {
    self.bounds.clear();
    self.bounds.extend_from_slice(&other.bounds);
    self.count = other.count;
    self.first_no = other.first_no;
    self.last_no = other.last_no;
    self.rising = other.rising;
    self.pruned = other.pruned;
    self.partition = other.partition;
    self.in_fanout = other.in_fanout;
    self.first = other.first;
  }

  /// Empty, its buffer kept.
  pub(crate) fn clear(&mut self) {
    self.bounds.clear();
    self.count = 0;
    self.first_no = 0;
    self.last_no = 0;
    self.rising = true;
    self.pruned = false;
    self.partition = 0;
    self.in_fanout = false;
    self.first = 0;
  }

  /// Count a range that has been encoded, with its number, the
  /// partition it is in if it is in one, and on a fanout table the first
  /// hash value of its base if it fixes one.
  fn note(
    &mut self,
    range_no: u32,
    partition: Option<u32>,
    first: Option<u32>,
  ) {
    if self.count == 0 {
      self.first_no = range_no;
      self.rising = true;
      self.pruned = partition.is_some();
      self.partition = partition.unwrap_or(0);
      self.in_fanout = first.is_some();
      self.first = first.unwrap_or(0);
    } else {
      if range_no <= self.last_no {
        self.rising = false;
      }
      if partition != Some(self.partition) {
        self.pruned = false;
      }
      if first != Some(self.first) {
        self.in_fanout = false;
      }
    }
    self.last_no = range_no;
    self.count += 1;
  }
}

/// What the ranges and the order of scans of an ordered index are made
/// with: the index, its key record, its columns' places, and the order
/// of rows laid out by the scan's record. It never changes.
#[derive(Debug)]
pub(crate) struct IndexShape {
  index: Arc<IndexDef>,
  key_rec: Record,
  /// The index's columns, in its order.
  columns: Vec<BoundColumn>,
  /// The fewest leading columns of the index that hold every
  /// distribution key column of the table, or on a fanout table every
  /// primary key column; 0 if the index lacks one, and then no range is
  /// pruned to one partition.
  dist_prefix: u32,
  /// On a fanout table, the fewest leading columns of the index that
  /// hold every base column; 0 if it lacks one, or for any other table.
  base_prefix: u32,
  /// The order of the index's columns in the scan's rows, or why an
  /// ordered run cannot be had.
  pub(crate) order: Result<IndexOrder, IcError>,
  /// What a run with several ranges reads: each row's range number
  /// first, then the record's columns.
  pub(crate) read_part_ranged: Vec<u32>,
}

impl IndexShape {
  /// Scans of `index`, an ordered index, their bounds laid out by
  /// `key_rec`, which must hold every column of the index, and their
  /// rows by `rec`, whose read is `read_part`. Both records are over the
  /// index's table.
  pub(crate) fn new(
    index: &Arc<IndexDef>,
    key_rec: &Record,
    rec: &Record,
    read_part: &[u32],
  ) -> Result<IndexShape, IcError> {
    if !index.is_ordered() {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    let table = index.table();
    check_scan_record(table, key_rec)?;
    let mut columns: Vec<BoundColumn> = Vec::new();
    for column in &index.info().attributes {
      // The index's hidden column is no column of the table.
      let field_id = match table.field_id(&column.name) {
        Ok(field_id) => field_id,
        Err(_) => continue,
      };
      let position = match key_rec.position_of(field_id) {
        Some(position) => position,
        None => return Err(IcError::new(err::IC_ERROR_KEY_RECORD)),
      };
      columns.push(BoundColumn {
        field_id,
        position,
        index_column: column.column_id,
      });
    }
    // A fanout table's rows are placed by their whole primary key, in
    // two parts, the first of them the base.
    let info = table.info();
    let fanout = info.fanout > 1;
    let mut dist_prefix: u32 = 0;
    let mut base_prefix: u32 = 0;
    let mut whole = true;
    let mut base = fanout;
    let mut key_column: u32 = 0;
    for attr in &info.attributes {
      let mut in_key = attr.distribution_key;
      if fanout {
        in_key = attr.primary_key;
      }
      if !in_key {
        continue;
      }
      let in_base = fanout && key_column < info.fanout_base_keys;
      key_column += 1;
      match place_of(&columns, attr.column_id) {
        Some(place) => {
          dist_prefix = dist_prefix.max(place + 1);
          if in_base {
            base_prefix = base_prefix.max(place + 1);
          }
        }
        None => {
          whole = false;
          if in_base {
            base = false;
          }
        }
      }
    }
    if !whole {
      dist_prefix = 0;
    }
    if !base {
      base_prefix = 0;
    }
    let mut read_part_ranged: Vec<u32> =
      Vec::with_capacity(1 + read_part.len());
    read_part_ranged
      .push(column_header::column_header(scans::IC_COL_RANGE_NO, 0));
    read_part_ranged.extend_from_slice(read_part);
    Ok(IndexShape {
      index: Arc::clone(index),
      key_rec: key_rec.clone(),
      columns,
      dist_prefix,
      base_prefix,
      order: IndexOrder::new(index, rec),
      read_part_ranged,
    })
  }

  /// The index scanned.
  pub(crate) fn index(&self) -> &Arc<IndexDef> {
    &self.index
  }

  /// The record bounds are laid out by.
  pub(crate) fn key_record(&self) -> &Record {
    &self.key_rec
  }

  /// Encode a range onto `set`, from `low` and `high`, rows laid out by
  /// the key record; a row a bound does not use may be empty. Refused
  /// with `IC_ERROR_BAD_RANGE` for a number above 4095, more columns
  /// than the index has, or bounds too long to send; nothing is added
  /// then.
  pub(crate) fn add_range(
    &self,
    set: &mut RangeSet,
    range: &ScanRange,
    low: &[u8],
    high: &[u8],
  ) -> Result<(), IcError> {
    let columns = self.columns.len() as u32;
    let bad = IcError::new(err::IC_ERROR_BAD_RANGE);
    if range.range_no > scans::IC_RANGE_NO_MAX
      || range.low_columns > columns
      || range.high_columns > columns
    {
      return Err(bad);
    }
    let size = self.key_rec.row_size() as usize;
    let short = (range.low_columns > 0 && low.len() < size)
      || (range.high_columns > 0 && high.len() < size);
    if short {
      return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
    }
    let start = set.bounds.len();
    let encoded = encode_range(
      &self.key_rec,
      &self.columns,
      range,
      low,
      high,
      &mut set.bounds,
    );
    if let Err(e) = encoded {
      set.bounds.truncate(start);
      return Err(e);
    }
    let finished = scans::finish_range(&mut set.bounds, start, range.range_no);
    if let Err(e) = finished {
      set.bounds.truncate(start);
      return Err(e);
    }
    let partition = self.partition_of_range(range, low, high);
    let first = self.fanout_first_of_range(range, low, high);
    set.note(range.range_no, partition, first);
    Ok(())
  }

  /// On a fanout table, the first hash value of the base every row of
  /// the range has, if its bounds fix the base columns, the same in both,
  /// and it can be worked out.
  fn fanout_first_of_range(
    &self,
    range: &ScanRange,
    low: &[u8],
    high: &[u8],
  ) -> Option<u32> {
    let fixed = self.base_prefix > 0
      && range.low_columns >= self.base_prefix
      && range.high_columns >= self.base_prefix;
    if !fixed {
      return None;
    }
    let same =
      same_values(&self.key_rec, &self.columns, low, high, self.base_prefix);
    match same {
      Ok(true) => {
        hash::fanout_first_of(self.index.table(), &self.key_rec, low).ok()
      }
      _ => None,
    }
  }

  /// The partition every row of the range is in, if its bounds fix the
  /// table's distribution key and the partition can be worked out.
  fn partition_of_range(
    &self,
    range: &ScanRange,
    low: &[u8],
    high: &[u8],
  ) -> Option<u32> {
    let fixed = self.dist_prefix > 0
      && range.low_columns >= self.dist_prefix
      && range.high_columns >= self.dist_prefix;
    if !fixed {
      return None;
    }
    let same =
      same_values(&self.key_rec, &self.columns, low, high, self.dist_prefix);
    match same {
      Ok(true) => {
        hash::partition_of(self.index.table(), &self.key_rec, low).ok()
      }
      _ => None,
    }
  }

  /// Refuse an ordered run of `set`'s ranges: an index whose order is
  /// not known here, or several ranges whose numbers do not rise
  /// (`IC_ERROR_BAD_RANGE`).
  pub(crate) fn check_ordered(&self, set: &RangeSet) -> Result<(), IcError> {
    if let Err(e) = &self.order {
      return Err(*e);
    }
    if set.count > 1 && !set.rising {
      return Err(IcError::new(err::IC_ERROR_BAD_RANGE));
    }
    Ok(())
  }
}

/// What a connection's scan of an ordered index has beyond a table
/// scan: its shape, the ranges added for the next run, and those of the
/// run.
#[derive(Debug)]
pub(crate) struct IndexScan {
  pub(crate) shape: IndexShape,
  /// The ranges added since the last run was started.
  pending: RangeSet,
  /// The ranges of the run.
  pub(crate) run: RangeSet,
}

impl IndexScan {
  /// A scan of `index`, as [`IndexShape::new`] makes its shape.
  pub(crate) fn new(
    index: &Arc<IndexDef>,
    key_rec: &Record,
    rec: &Record,
    read_part: &[u32],
  ) -> Result<IndexScan, IcError> {
    Ok(IndexScan {
      shape: IndexShape::new(index, key_rec, rec, read_part)?,
      pending: RangeSet::default(),
      run: RangeSet::default(),
    })
  }

  /// The index scanned.
  pub(crate) fn index(&self) -> &Arc<IndexDef> {
    self.shape.index()
  }

  /// Ranges added for the next run.
  pub(crate) fn pending(&self) -> &RangeSet {
    &self.pending
  }

  /// Encode a range for the next run; see [`IndexShape::add_range`].
  pub(crate) fn add_range(
    &mut self,
    range: &ScanRange,
    low: &[u8],
    high: &[u8],
  ) -> Result<(), IcError> {
    self.shape.add_range(&mut self.pending, range, low, high)
  }

  /// The next run's ranges are `set`'s, encoded already, in place of
  /// any added: how an executor passes a reader's on.
  pub(crate) fn set_ranges(&mut self, set: &RangeSet) {
    self.pending.copy_from(set);
  }

  /// The ranges added become the run's, and the next run is given its
  /// own. A run given none reads the whole index, as one open range.
  pub(crate) fn take_ranges(&mut self) {
    std::mem::swap(&mut self.pending, &mut self.run);
    self.pending.clear();
    if self.run.count > 0 {
      return;
    }
    scans::open_range_into(&mut self.run.bounds);
    // An open range of number 0 always fits.
    let _ = scans::finish_range(&mut self.run.bounds, 0, 0);
    self.run.note(0, None, None);
  }
}

/// Where a column of the table is among the index's columns.
fn place_of(columns: &[BoundColumn], field_id: u32) -> Option<u32> {
  let mut place: usize = 0;
  while place < columns.len() {
    if columns[place].field_id == field_id {
      return Some(place as u32);
    }
    place += 1;
  }
  None
}

/// True if `low` and `high` hold the same values, NULL or not, in the
/// first `count` columns of the index.
fn same_values(
  key_rec: &Record,
  columns: &[BoundColumn],
  low: &[u8],
  high: &[u8],
  count: u32,
) -> Result<bool, IcError> {
  let mut place: usize = 0;
  while place < count as usize {
    let position = columns[place].position;
    let a = row_codec::field_value(key_rec, position, low)?;
    let b = row_codec::field_value(key_rec, position, high)?;
    if a != b {
      return Ok(false);
    }
    place += 1;
  }
  Ok(true)
}

/// One range's bounds onto `out`, column by column, a column's low
/// bound before its high one, as the reference sends them; the range's
/// first word is finished by the caller.
fn encode_range(
  key_rec: &Record,
  columns: &[BoundColumn],
  range: &ScanRange,
  low: &[u8],
  high: &[u8],
  out: &mut Vec<u32>,
) -> Result<(), IcError> {
  if range.low_columns == 0 && range.high_columns == 0 {
    scans::open_range_into(out);
    return Ok(());
  }
  let equal = range.low_columns == range.high_columns
    && range.low_inclusive
    && range.high_inclusive
    && same_values(key_rec, columns, low, high, range.low_columns)?;
  let most = range.low_columns.max(range.high_columns);
  let mut place: u32 = 0;
  while place < most {
    let column = columns[place as usize];
    if equal {
      let value = row_codec::field_value(key_rec, column.position, low)?;
      scans::bound_into(out, scans::IC_BOUND_EQ, column.index_column, value);
      place += 1;
      continue;
    }
    if place < range.low_columns {
      let mut bound_type = scans::IC_BOUND_LE;
      if !range.low_inclusive && place + 1 == range.low_columns {
        bound_type = scans::IC_BOUND_LT;
      }
      let value = row_codec::field_value(key_rec, column.position, low)?;
      scans::bound_into(out, bound_type, column.index_column, value);
    }
    if place < range.high_columns {
      let mut bound_type = scans::IC_BOUND_GE;
      if !range.high_inclusive && place + 1 == range.high_columns {
        bound_type = scans::IC_BOUND_GT;
      }
      let value = row_codec::field_value(key_rec, column.position, high)?;
      scans::bound_into(out, bound_type, column.index_column, value);
    }
    place += 1;
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  fn set_with(numbers: &[(u32, Option<u32>)]) -> RangeSet {
    let mut set = RangeSet::default();
    set.clear();
    for (range_no, partition) in numbers {
      set.note(*range_no, *partition, None);
    }
    set
  }

  #[test]
  fn ranges_rise_and_prune_only_together() {
    let set = set_with(&[(0, Some(3)), (1, Some(3)), (5, Some(3))]);
    assert!(set.rising);
    assert_eq!(set.pruned_partition(), Some(3));
    assert_eq!(set.first_no, 0);
    assert_eq!(set.count, 3);
    assert!(set.reads_range_no());
    assert!(!set_with(&[(4, None)]).reads_range_no());
    assert_eq!(RangeSet::default().pruned_partition(), None);
    // A number not above the last, a second partition, or a range in
    // no single partition each undo it.
    assert!(!set_with(&[(2, None), (2, None)]).rising);
    assert!(!set_with(&[(0, Some(3)), (1, Some(4))]).pruned);
    assert!(!set_with(&[(0, Some(3)), (1, None)]).pruned);
    assert!(!set_with(&[(0, None), (1, Some(3))]).pruned);
    // Cleared, it is ready for the next run, its buffer kept.
    let mut set = set_with(&[(7, Some(1))]);
    set.bounds.extend_from_slice(&[1, 2, 3]);
    let capacity = set.bounds.capacity();
    // Copied, then cleared: ready for the next run, its buffer kept.
    let mut copy = RangeSet::default();
    copy.copy_from(&set);
    assert_eq!(copy.bounds, set.bounds);
    assert_eq!(copy.pruned_partition(), Some(1));
    assert_eq!(copy.first_no, 7);
    set.clear();
    assert_eq!(set.count, 0);
    assert!(set.bounds.is_empty());
    assert_eq!(set.bounds.capacity(), capacity);
  }

  #[test]
  fn ranges_of_one_base_scan_the_fanouts_partitions() {
    // Every range fixing the same base, none the whole key: the
    // fanout's partitions for it.
    let mut set = RangeSet::default();
    set.clear();
    set.note(0, None, Some(12));
    set.note(1, None, Some(12));
    assert_eq!(set.pruned_fanout(), Some(12));
    assert_eq!(set.pruned_partition(), None);
    // Each in one partition, the same: that partition, not the fanout's.
    set.clear();
    set.note(0, Some(5), Some(12));
    set.note(1, Some(5), Some(12));
    assert_eq!(set.pruned_partition(), Some(5));
    assert_eq!(set.pruned_fanout(), None);
    // Two partitions of one base: its fanout's partitions.
    set.clear();
    set.note(0, Some(5), Some(12));
    set.note(1, Some(6), Some(12));
    assert_eq!(set.pruned_partition(), None);
    assert_eq!(set.pruned_fanout(), Some(12));
    // A second base, or a range with its base open: every partition.
    set.clear();
    set.note(0, None, Some(12));
    set.note(1, None, Some(16));
    assert_eq!(set.pruned_fanout(), None);
    set.clear();
    set.note(0, None, Some(12));
    set.note(1, None, None);
    assert_eq!(set.pruned_fanout(), None);
    // Copied with the rest.
    set.clear();
    set.note(0, None, Some(8));
    let mut copy = RangeSet::default();
    copy.copy_from(&set);
    assert_eq!(copy.pruned_fanout(), Some(8));
  }
}
