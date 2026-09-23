// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Signals packed one after another into a page of words, which is how
//! a receive thread hands a round's signals to a user thread
//! (`IC_SOCK_BUF_PAGE` in the C, where the signals stay in the buffer
//! they were read into).
//!
//! A page goes round: the receive thread fills it for one user thread
//! during a round, the inbox hands it over, the user thread reads the
//! signals in place, and the emptied page goes back to the inbox for
//! the receive thread's next round. Nothing is allocated per signal and
//! nothing is freed on another thread than the one that allocated it; a
//! page's allocation settles at the size of the largest round and
//! stays.
//!
//! Small signals, which is nearly all of them (`TCKEYCONF`, the parts of
//! `API_PACKED`, a small `TRANSID_AI`), are copied into the page: a
//! copy of a few dozen bytes costs less than sharing the receive page
//! would, whose release would then be spread over every thread it went
//! to. A large signal, a `TRANSID_AI` with kilobytes of row, is not
//! copied: its entry in the page names the receive page it lies in and
//! where, and the page holds that receive page, whose reference count
//! says when the receive thread may read into it again. Where small
//! ends is a setting, `ApidGlobal::set_large_signal_words`, found by
//! measuring (decided with the author, 2026-09-23).
//!
//! Each signal is four words of what the header said, the section
//! lengths, and then either the data and the sections, or where they
//! lie:
//!
//! ```text
//!   word 0   gsn (bits 0-15), fragment info (16-17), sections (18-19),
//!            by reference (20)
//!   word 1   receiver block (0-15), sender block (16-31)
//!   word 2   sending node
//!   word 3   data length in words
//!   then     one length word per section
//!   copied:  the data, the sections
//!   by ref:  which held receive page, where the data begins in it,
//!            where each section begins
//! ```

use std::sync::Arc;

use ic_ndb_signals::header::FragmentInfo;
use ic_ndb_signals::header::IC_MAX_SECTIONS;

use crate::node_connect::SignalView;
use crate::signal_reader::ReceivePage;

/// Words a signal takes in a page before its lengths and contents.
const IC_PAGE_SIGNAL_HEAD: usize = 4;
/// In the first word: the signal's words are in a receive page the
/// page holds, not in the page itself.
const IC_PAGE_BY_REFERENCE: u32 = 1 << 20;

/// What a receive thread hands a user thread in one post: small
/// signals copied into `words`, and the receive pages that large ones
/// lie in, which the user thread reads in place. Letting go of `held`
/// is how a user thread says it is done with those pages; see
/// [`SignalReader`](crate::signal_reader::SignalReader).
#[derive(Debug, Default)]
pub struct SignalPage {
  words: Vec<u32>,
  held: Vec<ReceivePage>,
}

impl SignalPage {
  /// True if no signal is in it.
  pub fn is_empty(&self) -> bool {
    self.words.is_empty()
  }

  /// Empty it for the next round, keeping the allocation and letting
  /// go of the receive pages.
  pub fn clear(&mut self) {
    self.words.clear();
    self.held.clear();
  }

  /// How many receive pages it holds.
  pub fn held_pages(&self) -> usize {
    self.held.len()
  }
}

/// The first four words of a signal in a page.
fn push_head(
  page: &mut Vec<u32>,
  signal: &SignalView<'_>,
  num_sections: usize,
  flags: u32,
) {
  let mut word0 = signal.gsn as u32 | flags;
  word0 |= (signal.fragment_info as u32 & 3) << 16;
  word0 |= (num_sections as u32 & 3) << 18;
  page.push(word0);
  page.push(signal.receiver_block as u32 | (signal.sender_block as u32) << 16);
  page.push(signal.sender_node_id);
  page.push(signal.data.len() as u32);
  let mut i: usize = 0;
  while i < num_sections {
    page.push(signal.sections[i].len() as u32);
    i += 1;
  }
}

/// Append a copy of a signal to a page.
pub(crate) fn push(page: &mut SignalPage, signal: &SignalView<'_>) {
  let num_sections = signal.num_sections.min(IC_MAX_SECTIONS);
  push_head(&mut page.words, signal, num_sections, 0);
  page.words.extend_from_slice(signal.data);
  let mut i: usize = 0;
  while i < num_sections {
    page.words.extend_from_slice(signal.sections[i]);
    i += 1;
  }
}

/// Where a signal lies in a receive page, in words from the page's
/// start.
#[derive(Clone, Copy, Debug, Default)]
pub struct Placement {
  /// Where the data begins.
  pub data_at: usize,
  /// Where each section begins.
  pub section_at: [usize; IC_MAX_SECTIONS],
}

/// Append a signal to a page by reference: its words stay where they
/// are in `receive_page`, which the page holds until the user thread
/// has read it. Several signals in one receive page share one hold.
pub(crate) fn push_by_reference(
  page: &mut SignalPage,
  signal: &SignalView<'_>,
  receive_page: &ReceivePage,
  placed: &Placement,
) {
  let reuse = match page.held.last() {
    Some(last) => Arc::ptr_eq(last, receive_page),
    None => false,
  };
  if !reuse {
    page.held.push(Arc::clone(receive_page));
  }
  let held_index = page.held.len() - 1;
  let num_sections = signal.num_sections.min(IC_MAX_SECTIONS);
  push_head(&mut page.words, signal, num_sections, IC_PAGE_BY_REFERENCE);
  page.words.push(held_index as u32);
  page.words.push(placed.data_at as u32);
  let mut i: usize = 0;
  while i < num_sections {
    page.words.push(placed.section_at[i] as u32);
    i += 1;
  }
}

/// The signal at `*at` in a page, with `*at` moved past it; `None` at
/// the end of the page, or at a signal that does not fit in what is
/// there, which only a fault here could write.
pub(crate) fn next<'a>(
  page: &'a SignalPage,
  at: &mut usize,
) -> Option<SignalView<'a>> {
  let words = &page.words;
  let start = *at;
  if start + IC_PAGE_SIGNAL_HEAD > words.len() {
    return None;
  }
  let word0 = words[start];
  let word1 = words[start + 1];
  let num_sections = ((word0 >> 18) & 3) as usize;
  let data_len = words[start + 3] as usize;
  let mut pos = start + IC_PAGE_SIGNAL_HEAD;
  if pos + num_sections > words.len() {
    return None;
  }
  let mut lens: [usize; IC_MAX_SECTIONS] = [0; IC_MAX_SECTIONS];
  let mut i: usize = 0;
  while i < num_sections {
    lens[i] = words[pos + i] as usize;
    i += 1;
  }
  pos += num_sections;
  let mut sections: [&'a [u32]; IC_MAX_SECTIONS] = [&[]; IC_MAX_SECTIONS];
  let data: &'a [u32];
  if word0 & IC_PAGE_BY_REFERENCE != 0 {
    if pos + 2 + num_sections > words.len() {
      return None;
    }
    let held = page.held.get(words[pos] as usize)?;
    let data_at = words[pos + 1] as usize;
    data = held.get(data_at..data_at + data_len)?;
    pos += 2;
    i = 0;
    while i < num_sections {
      let section_at = words[pos + i] as usize;
      sections[i] = held.get(section_at..section_at + lens[i])?;
      i += 1;
    }
    pos += num_sections;
  } else {
    data = words.get(pos..pos + data_len)?;
    pos += data_len;
    i = 0;
    while i < num_sections {
      sections[i] = words.get(pos..pos + lens[i])?;
      pos += lens[i];
      i += 1;
    }
  }
  *at = pos;
  Some(SignalView {
    gsn: (word0 & 0xFFFF) as u16,
    receiver_block: (word1 & 0xFFFF) as u16,
    sender_block: (word1 >> 16) as u16,
    sender_node_id: words[start + 2],
    fragment_info: FragmentInfo::from_u8(((word0 >> 16) & 3) as u8),
    data,
    sections,
    num_sections,
  })
}

/// How many signals a page holds.
pub(crate) fn count(page: &SignalPage) -> usize {
  let mut at: usize = 0;
  let mut n: usize = 0;
  while next(page, &mut at).is_some() {
    n += 1;
  }
  n
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn signals_come_back_as_they_went_in() {
    let first_data = [1u32, 2, 3];
    let first = SignalView {
      gsn: 12,
      receiver_block: 0x8001,
      sender_block: 0xF5,
      sender_node_id: 2,
      fragment_info: FragmentInfo::Whole,
      data: &first_data,
      sections: [&[], &[], &[]],
      num_sections: 0,
    };
    let second_data = [7u32];
    let key = [0x10u32, 0x11];
    let attr = [0x20u32];
    let second = SignalView {
      gsn: 0xABC,
      receiver_block: 0x8002,
      sender_block: 0x1FF,
      sender_node_id: 1,
      fragment_info: FragmentInfo::Last,
      data: &second_data,
      sections: [&key, &attr, &[]],
      num_sections: 2,
    };
    let mut page = SignalPage::default();
    push(&mut page, &first);
    push(&mut page, &second);
    assert_eq!(count(&page), 2);
    let mut at: usize = 0;
    let got = next(&page, &mut at).expect("first");
    assert_eq!(got.gsn, 12);
    assert_eq!(got.receiver_block, 0x8001);
    assert_eq!(got.sender_block, 0xF5);
    assert_eq!(got.sender_node_id, 2);
    assert_eq!(got.fragment_info, FragmentInfo::Whole);
    assert_eq!(got.data, &first_data);
    assert_eq!(got.num_sections, 0);
    let got = next(&page, &mut at).expect("second");
    assert_eq!(got.gsn, 0xABC);
    assert_eq!(got.sender_block, 0x1FF);
    assert_eq!(got.fragment_info, FragmentInfo::Last);
    assert_eq!(got.data, &second_data);
    assert_eq!(got.num_sections, 2);
    assert_eq!(got.section(0), &key);
    assert_eq!(got.section(1), &attr);
    assert_eq!(got.section(2), &[] as &[u32]);
    assert!(next(&page, &mut at).is_none());
    assert_eq!(at, page.words.len());
  }

  #[test]
  fn a_large_signal_is_read_where_it_lies() {
    // A receive page with two signals' words in it at known places.
    let mut words = vec![0u32; 64];
    words[10..13].copy_from_slice(&[1, 2, 3]);
    words[20..24].copy_from_slice(&[9, 8, 7, 6]);
    words[30..32].copy_from_slice(&[5, 5]);
    let receive_page: ReceivePage = Arc::new(words);
    let small_data = [42u32];
    let small = SignalView {
      gsn: 1,
      data: &small_data,
      ..SignalView::default()
    };
    let data = &receive_page[10..13];
    let row = &receive_page[20..24];
    let large = SignalView {
      gsn: 2,
      receiver_block: 0x8003,
      data,
      sections: [row, &[], &[]],
      num_sections: 1,
      ..SignalView::default()
    };
    let other = SignalView {
      gsn: 3,
      data: &receive_page[30..32],
      ..SignalView::default()
    };
    let mut page = SignalPage::default();
    push(&mut page, &small);
    let placed = Placement {
      data_at: 10,
      section_at: [20, 0, 0],
    };
    push_by_reference(&mut page, &large, &receive_page, &placed);
    let placed = Placement {
      data_at: 30,
      section_at: [0; IC_MAX_SECTIONS],
    };
    push_by_reference(&mut page, &other, &receive_page, &placed);
    // One hold for both signals in the same receive page.
    assert_eq!(page.held_pages(), 1);
    assert_eq!(Arc::strong_count(&receive_page), 2);
    let mut at: usize = 0;
    assert_eq!(next(&page, &mut at).expect("small").data, &[42]);
    let got = next(&page, &mut at).expect("large");
    assert_eq!(got.gsn, 2);
    assert_eq!(got.receiver_block, 0x8003);
    assert_eq!(got.data, &[1, 2, 3]);
    assert_eq!(got.section(0), &[9, 8, 7, 6]);
    // The words were not copied.
    assert!(std::ptr::eq(got.data.as_ptr(), receive_page[10..].as_ptr()));
    assert_eq!(next(&page, &mut at).expect("other").data, &[5, 5]);
    assert!(next(&page, &mut at).is_none());
    // Emptying the page lets go of the receive page.
    page.clear();
    assert_eq!(Arc::strong_count(&receive_page), 1);
  }

  #[test]
  fn a_page_cut_short_ends_the_signals() {
    let data = [1u32, 2, 3];
    let signal = SignalView {
      gsn: 1,
      data: &data,
      ..SignalView::default()
    };
    let mut page = SignalPage::default();
    push(&mut page, &signal);
    page.words.pop();
    let mut at: usize = 0;
    assert!(next(&page, &mut at).is_none());
    assert_eq!(at, 0);
  }
}
