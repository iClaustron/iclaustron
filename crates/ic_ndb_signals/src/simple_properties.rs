// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Reading the key-value encoding the dictionary describes objects in.
//!
//! A table description arrives as a run of properties, each a key and
//! a value:
//!
//! ```text
//!   head word      (value type << 16) | key
//!   Uint32         one word
//!   Uint64         two words, low then high
//!   String/Binary  a length word in bytes, then that many bytes padded
//!                  to whole words
//! ```
//!
//! **Two byte orders in one buffer.** The head, the numbers and the
//! length words are stored in network byte order, each in a word of its
//! own; the bytes of a string are copied in as they are. The words then
//! travel in the sender's byte order like every signal word, which we
//! assume is ours. So a number is read with a big-endian conversion of
//! the native word, and a string is read from the native bytes of its
//! words. Mixing the two up works on a big-endian machine and fails on
//! every other.
//!
//! Keys may repeat and come in any order, and a reader must pass over
//! keys it does not know: a newer data node may send more than an older
//! reader has heard of.
//!
//! Verify: `SimpleProperties.hpp`, the value types, and
//! `SimpleProperties.cpp`, `Writer::add` and `Reader::readValue`.

use ic_port::err;
use ic_port::IcError;

/// A value of one word.
pub const IC_SP_UINT32: u32 = 0;
/// A string: its length in bytes, then the bytes.
pub const IC_SP_STRING: u32 = 1;
/// Bytes that are not text: their length, then the bytes.
pub const IC_SP_BINARY: u32 = 2;
/// A value of two words, the low word first.
pub const IC_SP_UINT64: u32 = 4;

/// One property's value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PropertyValue {
  /// A number of one word.
  Uint32(u32),
  /// A number of two words.
  Uint64(u64),
  /// Text, without the terminating NUL the sender counts in its length.
  String(String),
  /// Bytes, exactly as many as the length says.
  Binary(Vec<u8>),
}

/// One key and its value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Property {
  /// Which property it is.
  pub key: u16,
  /// Its value.
  pub value: PropertyValue,
}

impl Property {
  /// The value as a number, or zero if it is not one. Numbers of two
  /// words keep their low word.
  pub fn as_u32(&self) -> u32 {
    match &self.value {
      PropertyValue::Uint32(value) => *value,
      PropertyValue::Uint64(value) => *value as u32,
      _ => 0,
    }
  }
}

/// Reads properties one by one from the words of a section.
#[derive(Debug)]
pub struct PropertyReader<'a> {
  words: &'a [u32],
  pos: usize,
}

impl<'a> PropertyReader<'a> {
  /// A reader at the start of `words`.
  pub fn new(words: &'a [u32]) -> PropertyReader<'a> {
    PropertyReader { words, pos: 0 }
  }

  /// The next property, or `None` at the end.
  ///
  /// A property cut short by the end of the words, or of a type no
  /// sender uses, is an error: nothing after it could be trusted.
  pub fn read_next(&mut self) -> Result<Option<Property>, IcError> {
    if self.pos >= self.words.len() {
      return Ok(None);
    }
    let bad = IcError::new(err::IC_ERROR_BAD_TABLE_DESCRIPTION);
    let head = u32::from_be(self.words[self.pos]);
    let key = (head & 0xFFFF) as u16;
    let value_type = head >> 16;
    if value_type == IC_SP_UINT32 {
      let value = self.number_at(self.pos + 1)?;
      self.pos += 2;
      return Ok(Some(Property {
        key,
        value: PropertyValue::Uint32(value),
      }));
    }
    if value_type == IC_SP_UINT64 {
      let low = self.number_at(self.pos + 1)? as u64;
      let high = self.number_at(self.pos + 2)? as u64;
      self.pos += 3;
      return Ok(Some(Property {
        key,
        value: PropertyValue::Uint64((high << 32) | low),
      }));
    }
    if value_type != IC_SP_STRING && value_type != IC_SP_BINARY {
      return Err(bad);
    }
    let len = self.number_at(self.pos + 1)? as usize;
    let num_words = len.div_ceil(4);
    let start = self.pos + 2;
    if start + num_words > self.words.len() {
      return Err(bad);
    }
    let mut bytes: Vec<u8> = Vec::with_capacity(num_words * 4);
    let mut i: usize = 0;
    while i < num_words {
      let word_bytes = self.words[start + i].to_ne_bytes();
      bytes.extend_from_slice(&word_bytes);
      i += 1;
    }
    bytes.truncate(len);
    self.pos = start + num_words;
    if value_type == IC_SP_BINARY {
      return Ok(Some(Property {
        key,
        value: PropertyValue::Binary(bytes),
      }));
    }
    // The length counts the terminating NUL; the text ends at the first.
    let mut end: usize = 0;
    while end < bytes.len() && bytes[end] != 0 {
      end += 1;
    }
    bytes.truncate(end);
    let text = String::from_utf8_lossy(&bytes).into_owned();
    Ok(Some(Property {
      key,
      value: PropertyValue::String(text),
    }))
  }

  /// A number word, in network byte order within the native word.
  fn number_at(&self, index: usize) -> Result<u32, IcError> {
    if index >= self.words.len() {
      return Err(IcError::new(err::IC_ERROR_BAD_TABLE_DESCRIPTION));
    }
    Ok(u32::from_be(self.words[index]))
  }
}

/// Build property words the way a data node does, for tests and for
/// writing requests that carry properties.
#[derive(Debug, Default)]
pub struct PropertyWriter {
  words: Vec<u32>,
}

impl PropertyWriter {
  /// An empty buffer.
  pub fn new() -> PropertyWriter {
    PropertyWriter { words: Vec::new() }
  }

  /// Add a number of one word.
  pub fn add_u32(&mut self, key: u16, value: u32) {
    self.words.push(head(IC_SP_UINT32, key));
    self.words.push(value.to_be());
  }

  /// Add a number of two words.
  pub fn add_u64(&mut self, key: u16, value: u64) {
    self.words.push(head(IC_SP_UINT64, key));
    self.words.push((value as u32).to_be());
    self.words.push(((value >> 32) as u32).to_be());
  }

  /// Add text. The length sent counts a terminating NUL, as the data
  /// nodes do.
  pub fn add_string(&mut self, key: u16, text: &str) {
    let mut bytes: Vec<u8> = text.as_bytes().to_vec();
    bytes.push(0);
    self.add_bytes(IC_SP_STRING, key, &bytes);
  }

  /// Add bytes.
  pub fn add_binary(&mut self, key: u16, bytes: &[u8]) {
    self.add_bytes(IC_SP_BINARY, key, bytes);
  }

  fn add_bytes(&mut self, value_type: u32, key: u16, bytes: &[u8]) {
    self.words.push(head(value_type, key));
    self.words.push((bytes.len() as u32).to_be());
    let mut i: usize = 0;
    while i < bytes.len() {
      let mut chunk: [u8; 4] = [0; 4];
      let mut j: usize = 0;
      while j < 4 && i + j < bytes.len() {
        chunk[j] = bytes[i + j];
        j += 1;
      }
      self.words.push(u32::from_ne_bytes(chunk));
      i += 4;
    }
  }

  /// The words written so far.
  pub fn words(&self) -> &[u32] {
    &self.words
  }
}

fn head(value_type: u32, key: u16) -> u32 {
  ((value_type << 16) | key as u32).to_be()
}

#[cfg(test)]
mod tests {
  use super::*;

  fn read_all(words: &[u32]) -> Vec<Property> {
    let mut reader = PropertyReader::new(words);
    let mut out: Vec<Property> = Vec::new();
    while let Some(property) = reader.read_next().expect("readable") {
      out.push(property);
    }
    out
  }

  #[test]
  fn numbers_text_and_bytes_come_back_as_written() {
    let mut w = PropertyWriter::new();
    w.add_string(1, "ictest/def/t1");
    w.add_u32(2, 17);
    w.add_u64(154, 0x0000_0001_0000_0002);
    w.add_binary(1021, &[1, 2, 3, 4, 5]);
    let got = read_all(w.words());
    assert_eq!(got.len(), 4);
    assert_eq!(got[0].key, 1);
    let name = PropertyValue::String("ictest/def/t1".to_string());
    assert_eq!(got[0].value, name);
    assert_eq!(got[1].as_u32(), 17);
    assert_eq!(got[2].value, PropertyValue::Uint64(0x0000_0001_0000_0002));
    assert_eq!(got[3].value, PropertyValue::Binary(vec![1, 2, 3, 4, 5]));
  }

  #[test]
  fn numbers_are_stored_big_endian_inside_native_words() {
    // Written by hand as a data node lays it out, not by our writer, so
    // that the writer and the reader cannot share a mistake.
    let words = [((IC_SP_UINT32 << 16) | 2).to_be(), 0x0102_0304u32.to_be()];
    let got = read_all(&words);
    assert_eq!(got[0].key, 2);
    assert_eq!(got[0].as_u32(), 0x0102_0304);
  }

  #[test]
  fn text_ends_at_its_nul_whatever_the_padding() {
    // "abcd" plus NUL is five bytes, which takes two words.
    let mut w = PropertyWriter::new();
    w.add_string(1000, "abcd");
    assert_eq!(w.words().len(), 4);
    let got = read_all(w.words());
    assert_eq!(got[0].value, PropertyValue::String("abcd".to_string()));
  }

  #[test]
  fn a_property_cut_short_is_an_error() {
    let mut w = PropertyWriter::new();
    w.add_string(1, "a longer table name");
    let words = w.words();
    let mut reader = PropertyReader::new(&words[..words.len() - 1]);
    let e = reader.read_next().expect_err("cut short");
    assert_eq!(e.code, err::IC_ERROR_BAD_TABLE_DESCRIPTION);
  }

  #[test]
  fn an_unknown_value_type_is_an_error() {
    let words = [((3u32 << 16) | 7).to_be(), 0];
    let mut reader = PropertyReader::new(&words);
    assert!(reader.read_next().is_err());
  }

  #[test]
  fn an_empty_buffer_has_no_properties() {
    let mut reader = PropertyReader::new(&[]);
    assert!(reader.read_next().expect("empty").is_none());
  }
}
