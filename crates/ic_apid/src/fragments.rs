// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Putting a fragmented signal back together.
//!
//! A data node splits a signal that is too large to send in one piece,
//! which is over 7400 words, into pieces of 3840 words, or of 120 from
//! a debug build of the data node. One exception is `TRANSID_AI`, the
//! row data of a read, which a debug build already splits over 240
//! words. The pieces arrive in order on one link, marked first, middle
//! and last, and every piece is laid out the same way:
//!
//! ```text
//!   data     the whole signal data of the original, in every fragment
//!            section number of each section this fragment carries
//!            fragment id, the same in every fragment of one signal
//!   sections the part of each carried section that is in this fragment
//! ```
//!
//! Joining them is appending each fragment's sections to the section
//! of the original that its number names. Fragments of different
//! signals are told apart by their sender and fragment id.
//!
//! **The joined signal keeps the first fragment's data.** That is the
//! original's when the kernel's fragmenting send split it, which copies
//! the data into every piece. A block may also split an answer by hand
//! and put values of each piece's own in the data: the dictionary's
//! `LIST_TABLES_CONF` carries the count of objects in that piece. A
//! handler for such a signal takes those values from the joined
//! sections instead; `list_tables` counts objects by section length.
//!
//! The C does no reassembly on the receiving side at all: iClaustron
//! only ever sent fragmented signals, for schema changes. This follows
//! the kernel's sender, which is what the layout above is taken from.
//!
//! Reassembly is done where the signal is executed, by the user thread,
//! not by the receive thread, which only routes. Every fragment of a
//! reply goes to the same block, so it all lands in one inbox.
//!
//! Verify: `SimulatedBlock.cpp`, `sendFirstFragment` and
//! `sendNextLinearFragment`; `ndb_limits.h`, `MAX_SIZE_SINGLE_SIGNAL`;
//! `SimulatedBlock.hpp`, `FRAGMENT_WORD_SIZE`, whose comment says
//! splitting starts above the piece size, which the code does not do;
//! `NdbApiSignal.hpp`, `getFragmentId` and
//! `getFragmentSectionNumber`; `NdbDictionaryImpl.cpp`,
//! `execGET_TABINFO_CONF`, which relies on every fragment carrying the
//! original signal data.

use ic_ndb_signals::header::FragmentInfo;
use ic_ndb_signals::header::IC_MAX_SECTIONS;
use ic_port::debug::IC_NDB_MESSAGE_LEVEL;
use ic_port::err;
use ic_port::IcError;

use crate::node_connect::ReceivedSignal;

/// One signal being put back together.
struct Assembly {
  sender_node_id: u32,
  sender_block: u16,
  fragment_id: u32,
  /// The original signal, with its sections filled in as they arrive.
  signal: ReceivedSignal,
}

/// Collects fragments until they make a whole signal. One per user
/// thread, like the inbox it takes signals from.
#[derive(Default)]
pub struct FragmentAssembler {
  in_progress: Vec<Assembly>,
}

impl std::fmt::Debug for FragmentAssembler {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "FragmentAssembler({} signal(s) in progress)",
      self.in_progress.len()
    )
  }
}

impl FragmentAssembler {
  /// An assembler with nothing in progress.
  pub fn new() -> FragmentAssembler {
    FragmentAssembler {
      in_progress: Vec::new(),
    }
  }

  /// Take one signal. A whole signal comes straight back. A fragment is
  /// kept, and the last fragment brings back the original, whole.
  ///
  /// A fragment that does not fit what came before, such as a middle
  /// fragment with no first, is an error, and whatever was being put
  /// together from that sender is dropped.
  pub fn add(
    &mut self,
    signal: ReceivedSignal,
  ) -> Result<Option<ReceivedSignal>, IcError> {
    if signal.fragment_info == FragmentInfo::Whole {
      return Ok(Some(signal));
    }
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    let num_sections = signal.sections.len();
    let len = signal.data.len();
    if len < num_sections + 1 {
      return Err(bad);
    }
    let fragment_id = signal.data[len - 1];
    let data_len = len - 1 - num_sections;
    let mut section_numbers: Vec<usize> = Vec::with_capacity(num_sections);
    let mut i: usize = 0;
    while i < num_sections {
      let number = signal.data[data_len + i] as usize;
      if number >= IC_MAX_SECTIONS {
        return Err(bad);
      }
      section_numbers.push(number);
      i += 1;
    }
    let position =
      self.find(signal.sender_node_id, signal.sender_block, fragment_id);
    if signal.fragment_info == FragmentInfo::First {
      if let Some(index) = position {
        // A first fragment for a signal already begun: the rest of the
        // old one never came.
        self.in_progress.remove(index);
        ic_port::debug_print!(
          IC_NDB_MESSAGE_LEVEL,
          "Dropped an unfinished fragmented signal from node {}",
          signal.sender_node_id
        );
      }
      let mut original = ReceivedSignal {
        gsn: signal.gsn,
        receiver_block: signal.receiver_block,
        sender_block: signal.sender_block,
        sender_node_id: signal.sender_node_id,
        fragment_info: FragmentInfo::Whole,
        data: signal.data[..data_len].to_vec(),
        sections: Vec::new(),
      };
      append_sections(&mut original, &section_numbers, &signal.sections);
      self.in_progress.push(Assembly {
        sender_node_id: signal.sender_node_id,
        sender_block: signal.sender_block,
        fragment_id,
        signal: original,
      });
      return Ok(None);
    }
    let index = match position {
      Some(index) => index,
      // A middle or last fragment of a signal whose first never came.
      None => return Err(bad),
    };
    append_sections(
      &mut self.in_progress[index].signal,
      &section_numbers,
      &signal.sections,
    );
    if signal.fragment_info != FragmentInfo::Last {
      return Ok(None);
    }
    let done = self.in_progress.remove(index);
    Ok(Some(done.signal))
  }

  /// How many signals are part way through.
  pub fn in_progress(&self) -> usize {
    self.in_progress.len()
  }

  /// Drop everything part way through, for instance when the node the
  /// fragments came from has gone.
  pub fn clear(&mut self) {
    self.in_progress.clear();
  }

  fn find(
    &self,
    sender_node_id: u32,
    sender_block: u16,
    fragment_id: u32,
  ) -> Option<usize> {
    self.in_progress.iter().position(|a| {
      a.sender_node_id == sender_node_id
        && a.sender_block == sender_block
        && a.fragment_id == fragment_id
    })
  }
}

/// Append each carried section to the original section its number
/// names, making room for sections not seen before.
fn append_sections(
  original: &mut ReceivedSignal,
  numbers: &[usize],
  sections: &[Vec<u32>],
) {
  let mut i: usize = 0;
  while i < numbers.len() && i < sections.len() {
    let number = numbers[i];
    while original.sections.len() <= number {
      original.sections.push(Vec::new());
    }
    original.sections[number].extend_from_slice(&sections[i]);
    i += 1;
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  const GSN: u16 = 190;

  /// A fragment as a data node sends it: the original data, then the
  /// section numbers and the fragment id.
  fn fragment(
    position: FragmentInfo,
    data: &[u32],
    carried: &[(u32, Vec<u32>)],
    fragment_id: u32,
  ) -> ReceivedSignal {
    let mut words: Vec<u32> = data.to_vec();
    let mut sections: Vec<Vec<u32>> = Vec::new();
    for (number, words_of) in carried {
      words.push(*number);
      sections.push(words_of.clone());
    }
    words.push(fragment_id);
    ReceivedSignal {
      gsn: GSN,
      sender_node_id: 1,
      sender_block: 0xFA,
      fragment_info: position,
      data: words,
      sections,
      ..ReceivedSignal::default()
    }
  }

  #[test]
  fn a_whole_signal_passes_straight_through() {
    let mut assembler = FragmentAssembler::new();
    let signal = ReceivedSignal {
      gsn: GSN,
      data: vec![1, 2, 3],
      ..ReceivedSignal::default()
    };
    let back = assembler.add(signal).expect("ok").expect("whole");
    assert_eq!(back.data, vec![1, 2, 3]);
  }

  #[test]
  fn three_fragments_make_one_signal() {
    let conf = [9, 17, 0, 7, 2, 0xFA0001];
    let mut assembler = FragmentAssembler::new();
    let first = fragment(FragmentInfo::First, &conf, &[(0, vec![1, 2, 3])], 5);
    let middle = fragment(FragmentInfo::Middle, &conf, &[(0, vec![4, 5])], 5);
    let last = fragment(FragmentInfo::Last, &conf, &[(0, vec![6, 7])], 5);
    assert!(assembler.add(first).expect("first").is_none());
    assert!(assembler.add(middle).expect("middle").is_none());
    let whole = assembler.add(last).expect("last").expect("done");
    // The original data, without the numbers and id at its end.
    assert_eq!(whole.data, conf.to_vec());
    assert_eq!(whole.sections.len(), 1);
    assert_eq!(whole.sections[0], vec![1, 2, 3, 4, 5, 6, 7]);
    assert_eq!(whole.fragment_info, FragmentInfo::Whole);
    assert_eq!(assembler.in_progress(), 0);
  }

  #[test]
  fn fragments_of_two_signals_do_not_mix() {
    let mut assembler = FragmentAssembler::new();
    let a1 = fragment(FragmentInfo::First, &[1], &[(0, vec![10])], 5);
    let b1 = fragment(FragmentInfo::First, &[2], &[(0, vec![20])], 6);
    let a2 = fragment(FragmentInfo::Last, &[1], &[(0, vec![11])], 5);
    let b2 = fragment(FragmentInfo::Last, &[2], &[(0, vec![21])], 6);
    assert!(assembler.add(a1).expect("a1").is_none());
    assert!(assembler.add(b1).expect("b1").is_none());
    let a = assembler.add(a2).expect("a2").expect("a");
    let b = assembler.add(b2).expect("b2").expect("b");
    assert_eq!(a.sections[0], vec![10, 11]);
    assert_eq!(b.sections[0], vec![20, 21]);
  }

  #[test]
  fn sections_go_back_where_their_numbers_say() {
    // The kernel sends the highest-numbered section first.
    let mut assembler = FragmentAssembler::new();
    let first = fragment(
      FragmentInfo::First,
      &[7],
      &[(2, vec![30]), (1, vec![20])],
      9,
    );
    let last = fragment(FragmentInfo::Last, &[7], &[(0, vec![10])], 9);
    assert!(assembler.add(first).expect("first").is_none());
    let whole = assembler.add(last).expect("last").expect("done");
    assert_eq!(whole.sections.len(), 3);
    assert_eq!(whole.sections[0], vec![10]);
    assert_eq!(whole.sections[1], vec![20]);
    assert_eq!(whole.sections[2], vec![30]);
  }

  #[test]
  fn a_fragment_without_its_first_is_an_error() {
    let mut assembler = FragmentAssembler::new();
    let stray = fragment(FragmentInfo::Last, &[1], &[(0, vec![1])], 5);
    assert!(assembler.add(stray).is_err());
  }

  #[test]
  fn a_section_number_out_of_range_is_an_error() {
    let mut assembler = FragmentAssembler::new();
    let bad = fragment(FragmentInfo::First, &[1], &[(3, vec![1])], 5);
    assert!(assembler.add(bad).is_err());
  }
}
