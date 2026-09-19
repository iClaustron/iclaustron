// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The attribute header: the word in front of each column's value in
//! the attribute information of a request and in the row data of a
//! reply.
//!
//! ```text
//!   bits 16-31  attribute id
//!   bit  15     partial read or write
//!   bits 0-14   size of the value in bytes; 0 is NULL, or nothing sent
//! ```
//!
//! The value follows in whole words, its length rounded up. A read asks
//! for a column with a header of size 0 and no value.
//!
//! Verify: `AttributeHeader.hpp`, `getAttributeId`, `getByteSize`,
//! `getDataSize`, `getPartialReadWriteFlag`.

/// The largest byte size a header can hold.
pub const IC_ATTR_MAX_BYTE_SIZE: u32 = 0x7FFF;
/// Set for a partial read or write of a column.
pub const IC_ATTR_PARTIAL_BIT: u32 = 1 << 15;

/// A header for `attribute_id` with a value of `byte_size` bytes.
pub fn attr_header(attribute_id: u32, byte_size: u32) -> u32 {
  (attribute_id << 16) | (byte_size & IC_ATTR_MAX_BYTE_SIZE)
}

/// The attribute id a header names.
pub fn attribute_id(header: u32) -> u32 {
  header >> 16
}

/// The value's size in bytes; 0 for NULL.
pub fn byte_size(header: u32) -> u32 {
  header & IC_ATTR_MAX_BYTE_SIZE
}

/// The words the value takes after the header.
pub fn data_words(header: u32) -> u32 {
  byte_size(header).div_ceil(4)
}

/// True for a partial read or write.
pub fn is_partial(header: u32) -> bool {
  header & IC_ATTR_PARTIAL_BIT != 0
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn a_header_holds_id_and_size() {
    let header = attr_header(3, 21);
    assert_eq!(header, (3 << 16) | 21);
    assert_eq!(attribute_id(header), 3);
    assert_eq!(byte_size(header), 21);
    // Twenty-one bytes take six words.
    assert_eq!(data_words(header), 6);
    assert!(!is_partial(header));
  }

  #[test]
  fn a_read_asks_with_size_zero() {
    let header = attr_header(7, 0);
    assert_eq!(data_words(header), 0);
    assert!(is_partial(header | IC_ATTR_PARTIAL_BIT));
  }
}
