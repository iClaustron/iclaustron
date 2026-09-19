// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Asking a data node's dictionary what objects exist: every table, or
//! the objects that depend on one table, such as its indexes.
//!
//! The request is five words and carries no section. The answer is two
//! words, with three words per object in section 0 and, when names were
//! asked for, each object's name in section 1: a length word in bytes,
//! counting the terminating NUL, then the name padded to whole words.
//!
//! **The dictionary splits a long answer itself**, and each piece
//! carries the count of objects in that piece only; the API adds them
//! up. So once the pieces are joined, the count at the front belongs to
//! the first piece alone. The number of objects is read from the length
//! of section 0 instead, at three words each.
//!
//! Every current data node answers in this long form. An older form
//! packed each object into one word; it is not read here.
//!
//! Verify: `ListTables.hpp`; `NdbDictionaryImpl.cpp`,
//! `listDependentObjects`, `listObjects`, `execLIST_TABLES_CONF` and
//! `unpackListTables`; `Dbdict.cpp`, `sendLIST_TABLES_CONF`.

use ic_port::err;
use ic_port::IcError;

/// Words in a `LIST_TABLES_REQ`.
pub const IC_LIST_TABLES_REQ_LEN: usize = 5;
/// Words in a `LIST_TABLES_CONF`, before any fragment words.
pub const IC_LIST_TABLES_CONF_LEN: usize = 2;
/// Words per object in section 0 of the answer.
pub const IC_LIST_TABLES_DATA_WORDS: usize = 3;

/// Request flag: send each object's name.
const IC_LIST_NAMES_BIT: u32 = 1 << 28;
/// Request flag: list only the objects that depend on the table named.
const IC_LIST_DEPENDENT_BIT: u32 = 1 << 30;
/// Where the older form keeps the table id, in the flags word: twelve
/// bits. Filled in as the reference does, for an older data node.
const IC_OLD_TABLE_ID_MASK: u32 = 0xFFF;

/// Ask for a list of objects.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ListTablesReq {
  /// Our own number for the request, which comes back in the answer.
  pub sender_data: u32,
  /// Our block reference, which is where the answer is sent.
  pub sender_ref: u32,
  /// Flags, and the table id and type in the older form.
  pub request_data: u32,
  /// The table whose dependents are wanted, or zero for all objects.
  pub table_id: u32,
  /// Only objects of this type, or zero for all types.
  pub table_type: u32,
}

impl ListTablesReq {
  /// A request for the objects that depend on a table, names included.
  /// That is how the reference finds a table's indexes.
  pub fn dependents_of(
    sender_data: u32,
    sender_ref: u32,
    table_id: u32,
  ) -> ListTablesReq {
    ListTablesReq {
      sender_data,
      sender_ref,
      request_data: IC_LIST_NAMES_BIT
        | IC_LIST_DEPENDENT_BIT
        | (table_id & IC_OLD_TABLE_ID_MASK),
      table_id,
      table_type: 0,
    }
  }

  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_LIST_TABLES_REQ_LEN] {
    [
      self.sender_data,
      self.sender_ref,
      self.request_data,
      self.table_id,
      self.table_type,
    ]
  }
}

/// The front of an answer.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ListTablesConf {
  /// Our own number for the request.
  pub sender_data: u32,
  /// Objects in this piece of the answer; see the module note.
  pub no_of_tables: u32,
}

impl ListTablesConf {
  /// Read the front of an answer.
  pub fn decode(data: &[u32]) -> Result<ListTablesConf, IcError> {
    if data.len() < IC_LIST_TABLES_CONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(ListTablesConf {
      sender_data: data[0],
      no_of_tables: data[1],
    })
  }
}

/// One object in a list.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ListedObject {
  /// Its id.
  pub id: u32,
  /// What kind of object, as the dictionary numbers kinds: the
  /// `IC_TABLE_TYPE_*` values of `dict_tab_info` for tables and indexes.
  pub object_type: u32,
  /// Its state, such as online or being built.
  pub state: u32,
  /// Whether and how it is stored.
  pub store: u32,
  /// Whether it is temporary.
  pub temporary: bool,
  /// Its internal name, when names were asked for. For an index this is
  /// `sys/def/<table id>/<index>`, or `<db>/<schema>/<table id>/<index>`
  /// for one made before that form.
  pub name: String,
}

impl ListedObject {
  /// The name as the user knows it: for an index, the part after the
  /// last separator.
  pub fn short_name(&self) -> &str {
    match self.name.rfind('/') {
      Some(at) => &self.name[at + 1..],
      None => &self.name,
    }
  }
}

/// Read the objects of a joined answer from its two sections.
pub fn parse_listed_objects(
  data: &[u32],
  names: &[u32],
) -> Result<Vec<ListedObject>, IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  if data.len() % IC_LIST_TABLES_DATA_WORDS != 0 {
    return Err(bad);
  }
  let count = data.len() / IC_LIST_TABLES_DATA_WORDS;
  let mut objects: Vec<ListedObject> = Vec::with_capacity(count);
  let mut name_pos: usize = 0;
  let mut i: usize = 0;
  while i < count {
    let flags = data[i * IC_LIST_TABLES_DATA_WORDS];
    let mut object = ListedObject {
      id: data[i * IC_LIST_TABLES_DATA_WORDS + 1],
      object_type: data[i * IC_LIST_TABLES_DATA_WORDS + 2],
      store: (flags >> 20) & 0x7,
      // Parenthesised for C readers: Rust ranks & above !=, C below.
      temporary: ((flags >> 23) & 0x1) != 0,
      state: (flags >> 24) & 0xF,
      name: String::new(),
    };
    if !names.is_empty() {
      if name_pos >= names.len() {
        return Err(bad);
      }
      let size = names[name_pos] as usize;
      let words = size.div_ceil(4);
      let start = name_pos + 1;
      if start + words > names.len() {
        return Err(bad);
      }
      object.name = text_of_words(&names[start..start + words], size);
      name_pos = start + words;
    }
    objects.push(object);
    i += 1;
  }
  Ok(objects)
}

/// A name from words holding its bytes as they are, up to its NUL.
fn text_of_words(words: &[u32], size: usize) -> String {
  let mut bytes: Vec<u8> = Vec::with_capacity(words.len() * 4);
  for word in words {
    bytes.extend_from_slice(&word.to_ne_bytes());
  }
  bytes.truncate(size);
  let mut end: usize = 0;
  while end < bytes.len() && bytes[end] != 0 {
    end += 1;
  }
  bytes.truncate(end);
  String::from_utf8_lossy(&bytes).into_owned()
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::get_tab_info::name_section;

  fn names_of(list: &[&str]) -> Vec<u32> {
    let mut words: Vec<u32> = Vec::new();
    for name in list {
      words.push(name.len() as u32 + 1);
      words.extend_from_slice(&name_section(name));
    }
    words
  }

  #[test]
  fn dependents_are_asked_for_with_names() {
    let req = ListTablesReq::dependents_of(9, 0x8000_00C0, 13);
    let words = req.encode();
    assert_eq!(words[0], 9);
    assert_eq!(words[1], 0x8000_00C0);
    // Names and dependents asked for, and the id in its old place too.
    assert_eq!(words[2], (1 << 28) | (1 << 30) | 13);
    assert_eq!(words[3], 13);
    assert_eq!(words[4], 0);
  }

  #[test]
  fn objects_and_their_names_are_read_in_step() {
    // State online (4) in bits 24..27.
    let data = [4 << 24, 14, 3, 4 << 24, 15, 6];
    let names = names_of(&["sys/def/13/PRIMARY$unique", "sys/def/13/PRIMARY"]);
    let objects = parse_listed_objects(&data, &names).expect("parsed");
    assert_eq!(objects.len(), 2);
    assert_eq!(objects[0].id, 14);
    assert_eq!(objects[0].object_type, 3);
    assert_eq!(objects[0].state, 4);
    assert_eq!(objects[0].short_name(), "PRIMARY$unique");
    assert_eq!(objects[1].object_type, 6);
    assert_eq!(objects[1].short_name(), "PRIMARY");
  }

  #[test]
  fn the_count_comes_from_the_section_not_the_front() {
    // A joined answer keeps the first piece's count; the section says
    // how many objects there really are.
    let data = [0, 1, 2, 0, 2, 2, 0, 3, 6];
    let objects = parse_listed_objects(&data, &[]).expect("parsed");
    assert_eq!(objects.len(), 3);
    assert!(objects[2].name.is_empty());
  }

  #[test]
  fn names_that_run_out_are_an_error() {
    let data = [0, 1, 2, 0, 2, 2];
    let names = names_of(&["only/one"]);
    assert!(parse_listed_objects(&data, &names).is_err());
    assert!(parse_listed_objects(&[0, 1], &[]).is_err());
  }

  #[test]
  fn an_old_index_name_is_shortened_the_same_way() {
    let object = ListedObject {
      name: "ictest/def/13/idx_amount".to_string(),
      ..ListedObject::default()
    };
    assert_eq!(object.short_name(), "idx_amount");
  }
}
