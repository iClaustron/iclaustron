// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! A bitmap of fixed width (`IC_BITMAP`,
//! `legacy-c/util/ic_bitmap.c`). Used for the set of fields taking part
//! in a query, the set of partitions a scan covers, and the connected
//! node bitmap that arrives in `API_REGCONF`.

/// Number of bytes a bitmap of `num_bits` bits occupies, rounded up to a
/// whole number of 32-bit words as the C `IC_BITMAP_SIZE` did.
pub fn bitmap_size(num_bits: u32) -> usize {
  (4 * (num_bits / 32) + 4) as usize
}

/// Position of the highest set bit, counting from 1; 0 when no bit is
/// set (`ic_count_highest_bit`).
pub fn count_highest_bit(value: u32) -> u32 {
  32 - value.leading_zeros()
}

/// True if bit `bit_number` of `value` is set.
pub fn is_bit_set(value: u32, bit_number: u32) -> bool {
  ((value >> bit_number) & 1) != 0
}

/// A bitmap of a fixed number of bits, all zero to begin with.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bitmap {
  area: Vec<u8>,
  num_bits: u32,
}

impl Bitmap {
  /// A bitmap of `num_bits` bits, all clear.
  pub fn new(num_bits: u32) -> Bitmap {
    Bitmap {
      area: vec![0u8; bitmap_size(num_bits)],
      num_bits,
    }
  }

  /// Number of bits the bitmap holds.
  pub fn num_bits(&self) -> u32 {
    self.num_bits
  }

  /// Set one bit. Out of range bits are ignored in a release build and
  /// abort a debug build.
  pub fn set_bit(&mut self, bit_number: u32) {
    if bit_number >= self.num_bits {
      ic_port::ic_assert!(false);
      return;
    }
    self.area[(bit_number / 8) as usize] |= 1 << (bit_number & 7);
  }

  /// Clear one bit.
  pub fn clear_bit(&mut self, bit_number: u32) {
    if bit_number >= self.num_bits {
      ic_port::ic_assert!(false);
      return;
    }
    self.area[(bit_number / 8) as usize] &= !(1 << (bit_number & 7));
  }

  /// True if the bit is set. Out of range bits read as clear.
  pub fn is_set(&self, bit_number: u32) -> bool {
    if bit_number >= self.num_bits {
      ic_port::ic_assert!(false);
      return false;
    }
    (self.area[(bit_number / 8) as usize] & (1 << (bit_number & 7))) != 0
  }

  /// Clear every bit.
  pub fn clear_all(&mut self) {
    self.area.fill(0);
  }

  /// Set every bit up to [`num_bits`](Self::num_bits).
  pub fn set_all(&mut self) {
    let mut bit: u32 = 0;
    while bit < self.num_bits {
      self.set_bit(bit);
      bit += 1;
    }
  }

  /// Number of bits currently set.
  pub fn count_set(&self) -> u32 {
    let mut count: u32 = 0;
    let mut bit: u32 = 0;
    while bit < self.num_bits {
      if self.is_set(bit) {
        count += 1;
      }
      bit += 1;
    }
    count
  }

  /// True if no bit is set.
  pub fn is_empty(&self) -> bool {
    for byte in &self.area {
      if *byte != 0 {
        return false;
      }
    }
    true
  }

  /// Copy the bits of `source`, which must have the same width.
  pub fn copy_from(&mut self, source: &Bitmap) {
    ic_port::ic_assert!(self.num_bits == source.num_bits);
    if self.num_bits != source.num_bits {
      return;
    }
    self.area.copy_from_slice(&source.area);
  }

  /// The raw bytes, least significant bit of byte 0 first. This is the
  /// layout NDB uses for node bitmaps in signals.
  pub fn as_bytes(&self) -> &[u8] {
    &self.area
  }

  /// The raw bytes, for filling a bitmap from a signal.
  pub fn as_bytes_mut(&mut self) -> &mut [u8] {
    &mut self.area
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn set_get_clear() {
    let mut b = Bitmap::new(100);
    assert_eq!(b.num_bits(), 100);
    assert!(b.is_empty());
    assert!(!b.is_set(0));
    b.set_bit(0);
    b.set_bit(7);
    b.set_bit(8);
    b.set_bit(99);
    assert!(b.is_set(0));
    assert!(b.is_set(7));
    assert!(b.is_set(8));
    assert!(b.is_set(99));
    assert!(!b.is_set(1));
    assert!(!b.is_set(98));
    assert_eq!(b.count_set(), 4);
    assert!(!b.is_empty());
    b.clear_bit(7);
    assert!(!b.is_set(7));
    assert_eq!(b.count_set(), 3);
    b.clear_all();
    assert!(b.is_empty());
  }

  /* The C unit test, test type 5: every bit of every width. */
  #[test]
  fn every_bit_of_every_width() {
    let mut num_bits: u32 = 1;
    while num_bits < 300 {
      let mut b = Bitmap::new(num_bits);
      let mut bit: u32 = 0;
      while bit < num_bits {
        assert!(!b.is_set(bit));
        b.set_bit(bit);
        assert!(b.is_set(bit));
        bit += 1;
      }
      assert_eq!(b.count_set(), num_bits);
      bit = 0;
      while bit < num_bits {
        assert!(b.is_set(bit));
        b.clear_bit(bit);
        assert!(!b.is_set(bit));
        bit += 1;
      }
      assert!(b.is_empty());
      num_bits += 1;
    }
  }

  #[test]
  fn set_all_and_copy() {
    let mut a = Bitmap::new(70);
    a.set_all();
    assert_eq!(a.count_set(), 70);
    let mut b = Bitmap::new(70);
    b.copy_from(&a);
    assert_eq!(b, a);
    assert_eq!(b.count_set(), 70);
  }

  #[test]
  fn sizes_and_highest_bit() {
    assert_eq!(bitmap_size(1), 4);
    assert_eq!(bitmap_size(32), 8);
    assert_eq!(bitmap_size(33), 8);
    assert_eq!(bitmap_size(64), 12);
    assert_eq!(count_highest_bit(0), 0);
    assert_eq!(count_highest_bit(1), 1);
    assert_eq!(count_highest_bit(2), 2);
    assert_eq!(count_highest_bit(255), 8);
    assert!(is_bit_set(0b1010, 1));
    assert!(!is_bit_set(0b1010, 0));
  }

  #[test]
  fn bytes_match_bit_order() {
    let mut b = Bitmap::new(16);
    b.set_bit(0);
    b.set_bit(9);
    assert_eq!(b.as_bytes()[0], 0x01);
    assert_eq!(b.as_bytes()[1], 0x02);
  }
}
