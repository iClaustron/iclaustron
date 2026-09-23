// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Packed signals: several small signals of one kind sent to an API
//! node as one, addressed to the block `API_PACKED` (2047).
//!
//! The transaction coordinator packs `TCKEYCONF` this way, and the node
//! that reads a row packs a short `TRANSID_AI`. The outer signal carries
//! the signal number every part shares. Its data is the parts one after
//! another, each a header word and then the part's own data:
//!
//! ```text
//!   bits 16-31  the block the part is for
//!   bits 0-4    the part's length in words, less three
//! ```
//!
//! A part has no sections, and at most 25 words of data, like any short
//! signal. The receive thread takes the parts apart and routes each by
//! its own block, as if it had come alone.
//!
//! Verify: `TransporterFacade.cpp`, where `API_PACKED` is taken apart;
//! `DbtcMain.cpp`, `sendPackedTCKEYCONF`; `DbtupBuffer.cpp`,
//! `sendAPI_TRANSID_AI`.

use ic_port::err;
use ic_port::IcError;

/// The most words of data a part may have.
pub const IC_PACKED_MAX_PART_WORDS: usize = 25;

/// One part of a packed signal: where its data lies in the outer
/// signal's data, and the block it is for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PackedPart {
  /// The block the part is addressed to.
  pub receiver_block: u16,
  /// Where its data starts in the outer signal's data.
  pub start: usize,
  /// How many words of data it has.
  pub len: usize,
}

/// Take a packed signal's data apart. A part whose length runs past the
/// end is an error: the rest cannot be trusted.
pub fn unpack(data: &[u32]) -> Result<Vec<PackedPart>, IcError> {
  let mut parts: Vec<PackedPart> = Vec::new();
  unpack_into(data, &mut parts)?;
  Ok(parts)
}

/// As [`unpack`], into a list the caller keeps, cleared first.
pub fn unpack_into(
  data: &[u32],
  parts: &mut Vec<PackedPart>,
) -> Result<(), IcError> {
  parts.clear();
  let mut pos: usize = 0;
  while pos < data.len() {
    let header = data[pos];
    pos += 1;
    let len = (header & 0x1F) as usize + 3;
    if len > IC_PACKED_MAX_PART_WORDS || pos + len > data.len() {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    parts.push(PackedPart {
      receiver_block: (header >> 16) as u16,
      start: pos,
      len,
    });
    pos += len;
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  /// A part's header word.
  fn header(block: u32, len: u32) -> u32 {
    (block << 16) | (len - 3)
  }

  #[test]
  fn parts_come_out_in_order_with_their_blocks() {
    // A five-word part for block 0x8001, then a seven-word one for 0x8002.
    let mut data = vec![header(0x8001, 5), 1, 2, 3, 4, 5];
    data.push(header(0x8002, 7));
    data.extend_from_slice(&[10, 11, 12, 13, 14, 15, 16]);
    let parts = unpack(&data).expect("unpacked");
    assert_eq!(parts.len(), 2);
    assert_eq!(parts[0].receiver_block, 0x8001);
    assert_eq!(
      &data[parts[0].start..parts[0].start + parts[0].len],
      &[1, 2, 3, 4, 5]
    );
    assert_eq!(parts[1].receiver_block, 0x8002);
    assert_eq!(parts[1].len, 7);
    assert_eq!(data[parts[1].start], 10);
  }

  #[test]
  fn a_part_that_runs_past_the_end_is_an_error() {
    let data = vec![header(0x8001, 5), 1, 2];
    assert!(unpack(&data).is_err());
  }

  #[test]
  fn nothing_packed_is_no_parts() {
    assert!(unpack(&[]).expect("empty").is_empty());
  }
}
