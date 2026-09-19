// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Decoding the binary cluster configuration
//! (`legacy-c/api/ic_apic_conf_read_transl.ic`).
//!
//! The management server answers `get config_v2` with a base64 blob
//! whose decoded form is described here. Every word after the magic is
//! in network byte order, unlike the signal protocol, which is in the
//! sender's own order.
//!
//! ```text
//!   "NDBCONF2"                     8 bytes, not a number
//!   total length in words
//!   version, which must be 2
//!   number of default sections, which must be 5
//!   number of data nodes
//!   number of API nodes
//!   number of management nodes
//!   number of communication sections
//!   5 default sections: data node, API node, management node,
//!                       TCP, shared memory
//!   the system section
//!   one section per node, data nodes first
//!   one section per communication link
//!   checksum: every preceding word exclusive-ored together
//! ```
//!
//! A section is three words, its length, its number of entries and its
//! type, followed by the entries. An entry is a word holding the value
//! type in its top four bits and the parameter id in the rest, then the
//! value: one word for an integer, two for a 64-bit integer with the
//! high half first, or a length word and the padded bytes for a string.
//!
//! A concrete section only carries the values that differ from its
//! default section, which is what keeps the blob small, so reading a
//! parameter means looking in the section and then in the default for
//! its type.
//!
//! Verify: RonDB 26.10 `src/common/mgmcommon/ConfigObject.cpp:1032`
//! and `src/common/mgmcommon/ConfigSection.cpp:290,928`.

use std::collections::BTreeMap;

use ic_port::err;
use ic_port::IcError;

use crate::conf_param::IC_CFG_NODE_ID;
use crate::conf_param::IC_CFG_TYPE_OF_SECTION;

/// The eight bytes every version 2 configuration starts with.
pub const IC_MAGIC_V2: &[u8; 8] = b"NDBCONF2";
/// The eight bytes a version 1 configuration starts with, which a
/// RonDB 26.10 management server no longer sends.
pub const IC_MAGIC_V1: &[u8; 8] = b"NDBCONFV";

const IC_V2_TYPE_SHIFT: u32 = 28;
const IC_V2_TYPE_MASK: u32 = 15;
const IC_V2_KEY_MASK: u32 = 0x0FFF_FFFF;

/// What kind of section this is.
/// Verify: `include/util/ConfigSection.hpp:52`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u32)]
pub enum SectionType {
  /// Not a valid section.
  #[default]
  Invalid = 0,
  /// A data node.
  DataNode = 1,
  /// An API node, which is what we are.
  ApiNode = 2,
  /// A management server.
  MgmNode = 3,
  /// A TCP link between two nodes.
  Tcp = 4,
  /// A shared memory link between two nodes.
  Shm = 5,
  /// The cluster-wide section.
  System = 6,
  /// An RDMA link between two nodes, which RonDB adds.
  Rdma = 7,
}

impl SectionType {
  /// The section type a word names, or `Invalid`.
  pub fn from_u32(value: u32) -> SectionType {
    match value {
      1 => SectionType::DataNode,
      2 => SectionType::ApiNode,
      3 => SectionType::MgmNode,
      4 => SectionType::Tcp,
      5 => SectionType::Shm,
      6 => SectionType::System,
      7 => SectionType::Rdma,
      _ => SectionType::Invalid,
    }
  }

  /// True for a section describing a node rather than a link.
  pub fn is_node(&self) -> bool {
    matches!(
      self,
      SectionType::DataNode | SectionType::ApiNode | SectionType::MgmNode
    )
  }

  /// True for a section describing a link between two nodes.
  pub fn is_link(&self) -> bool {
    matches!(
      self,
      SectionType::Tcp | SectionType::Shm | SectionType::Rdma
    )
  }
}

/// One configuration value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ConfigValue {
  /// A 32-bit number.
  Int(u32),
  /// A 64-bit number.
  Int64(u64),
  /// Text.
  Str(String),
  /// A reference to another section, used by the version 1 format.
  Section(u32),
}

impl ConfigValue {
  /// The value as a 32-bit number, widening nothing and narrowing
  /// nothing: a 64-bit value that does not fit reads as `None`.
  pub fn as_u32(&self) -> Option<u32> {
    match self {
      ConfigValue::Int(v) => Some(*v),
      ConfigValue::Int64(v) => {
        if *v <= u32::MAX as u64 {
          Some(*v as u32)
        } else {
          None
        }
      }
      _ => None,
    }
  }

  /// The value as a 64-bit number.
  pub fn as_u64(&self) -> Option<u64> {
    match self {
      ConfigValue::Int(v) => Some(*v as u64),
      ConfigValue::Int64(v) => Some(*v),
      _ => None,
    }
  }

  /// The value as text.
  pub fn as_str(&self) -> Option<&str> {
    match self {
      ConfigValue::Str(s) => Some(s.as_str()),
      _ => None,
    }
  }
}

/// One section of the configuration: a type and the parameters that
/// differ from the default for that type.
#[derive(Clone, Debug, Default)]
pub struct Section {
  /// What this section describes.
  pub section_type: SectionType,
  /// Parameter id to value, holding only what this section states.
  ///
  /// Ordered rather than hashed: a section holds a few dozen entries at
  /// most, so lookup cost is the same either way, and printing or
  /// comparing two configurations gives the same order every time.
  pub entries: BTreeMap<u32, ConfigValue>,
}

impl Section {
  /// A parameter of this section alone, without consulting defaults.
  pub fn get(&self, key: u32) -> Option<&ConfigValue> {
    self.entries.get(&key)
  }

  /// The node id this section describes, for a node section.
  pub fn node_id(&self) -> Option<u32> {
    self.get(IC_CFG_NODE_ID)?.as_u32()
  }
}

/// A decoded configuration.
#[derive(Clone, Debug, Default)]
pub struct ConfigBlob {
  /// The default section for each type that has one.
  pub defaults: BTreeMap<SectionType, Section>,
  /// The cluster-wide section.
  pub system: Section,
  /// One section per node, data nodes first, then API nodes, then
  /// management nodes.
  pub nodes: Vec<Section>,
  /// One section per link between a pair of nodes.
  pub links: Vec<Section>,
  /// How many of [`nodes`](Self::nodes) are data nodes.
  pub num_data_nodes: u32,
  /// How many are API nodes.
  pub num_api_nodes: u32,
  /// How many are management nodes.
  pub num_mgm_nodes: u32,
}

/// Reads words out of the blob, in network byte order, refusing to run
/// off the end.
struct Reader<'a> {
  words: &'a [u32],
  at: usize,
}

impl<'a> Reader<'a> {
  fn new(words: &'a [u32]) -> Reader<'a> {
    Reader { words, at: 0 }
  }

  fn next_word(&mut self) -> Result<u32, IcError> {
    if self.at >= self.words.len() {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let word = u32::from_be(self.words[self.at]);
    self.at += 1;
    Ok(word)
  }

  fn next_words(&mut self, count: usize) -> Result<&'a [u32], IcError> {
    if self.at + count > self.words.len() {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    let slice = &self.words[self.at..self.at + count];
    self.at += count;
    Ok(slice)
  }
}

fn decode_entry(
  reader: &mut Reader<'_>,
) -> Result<(u32, ConfigValue), IcError> {
  let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
  let key_word = reader.next_word()?;
  let value_type = (key_word >> IC_V2_TYPE_SHIFT) & IC_V2_TYPE_MASK;
  let key = key_word & IC_V2_KEY_MASK;
  match value_type {
    1 => Ok((key, ConfigValue::Int(reader.next_word()?))),
    4 => {
      let high = reader.next_word()? as u64;
      let low = reader.next_word()? as u64;
      Ok((key, ConfigValue::Int64((high << 32) + low)))
    }
    2 => {
      // The length counts the terminating NUL; the bytes are padded
      // out to a whole number of words.
      let len_with_nul = reader.next_word()? as usize;
      if len_with_nul == 0 {
        return Err(bad);
      }
      let num_words = len_with_nul.div_ceil(4);
      let words = reader.next_words(num_words)?;
      let mut bytes: Vec<u8> = Vec::with_capacity(num_words * 4);
      for word in words {
        // The bytes of a string are in the order they were written,
        // not a number, so they are taken as they lie.
        bytes.extend_from_slice(&word.to_ne_bytes());
      }
      bytes.truncate(len_with_nul - 1);
      match String::from_utf8(bytes) {
        Ok(text) => Ok((key, ConfigValue::Str(text))),
        Err(_) => Err(bad),
      }
    }
    3 => Ok((key, ConfigValue::Section(reader.next_word()?))),
    _ => Err(bad),
  }
}

fn decode_section(reader: &mut Reader<'_>) -> Result<Section, IcError> {
  let header_len = reader.next_word()?;
  let num_entries = reader.next_word()?;
  let section_type = SectionType::from_u32(reader.next_word()?);
  if num_entries == 0 && header_len != 3 {
    return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
  }
  let mut section = Section {
    section_type,
    entries: BTreeMap::new(),
  };
  let mut i: u32 = 0;
  while i < num_entries {
    let (key, value) = decode_entry(reader)?;
    section.entries.insert(key, value);
    i += 1;
  }
  Ok(section)
}

/// Check the trailing checksum: every word but the last, exclusive-ored
/// together, equals the last.
fn check_checksum(words: &[u32]) -> bool {
  if words.len() < 2 {
    return false;
  }
  let mut sum: u32 = 0;
  let mut i: usize = 0;
  while i + 1 < words.len() {
    sum ^= u32::from_be(words[i]);
    i += 1;
  }
  sum == u32::from_be(words[words.len() - 1])
}

impl ConfigBlob {
  /// Decode a configuration as the management server sends it.
  pub fn decode(bytes: &[u8]) -> Result<ConfigBlob, IcError> {
    let bad = IcError::new(err::IC_ERROR_INCONSISTENT_DATA);
    if bytes.len() < 12 || bytes.len() % 4 != 0 {
      return Err(bad);
    }
    if &bytes[..8] == IC_MAGIC_V1 {
      // Version 1 was retired before RonDB 26.10; a server sending it
      // is too old for this library.
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    if &bytes[..8] != IC_MAGIC_V2 {
      return Err(bad);
    }
    let mut words: Vec<u32> = Vec::with_capacity(bytes.len() / 4);
    let mut i: usize = 0;
    while i < bytes.len() {
      words.push(u32::from_ne_bytes([
        bytes[i],
        bytes[i + 1],
        bytes[i + 2],
        bytes[i + 3],
      ]));
      i += 4;
    }
    if !check_checksum(&words) {
      return Err(IcError::new(err::IC_ERROR_MESSAGE_CHECKSUM));
    }
    /* The magic is not a number, so reading starts after it. */
    let mut reader = Reader::new(&words[2..]);
    let total_len = reader.next_word()?;
    let version = reader.next_word()?;
    let num_defaults = reader.next_word()?;
    let num_data_nodes = reader.next_word()?;
    let num_api_nodes = reader.next_word()?;
    let num_mgm_nodes = reader.next_word()?;
    let num_links = reader.next_word()?;
    if version != 2 {
      return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
    }
    if num_defaults != 5 {
      return Err(bad);
    }
    if total_len as usize != words.len() {
      return Err(bad);
    }
    let num_nodes = num_data_nodes + num_api_nodes + num_mgm_nodes;
    if num_nodes == 0 {
      return Err(IcError::new(err::IC_ERROR_NO_NODES_FOUND));
    }
    let mut blob = ConfigBlob {
      num_data_nodes,
      num_api_nodes,
      num_mgm_nodes,
      ..ConfigBlob::default()
    };
    /* Five default sections in a fixed order. */
    let mut d: u32 = 0;
    while d < num_defaults {
      let section = decode_section(&mut reader)?;
      blob.defaults.insert(section.section_type, section);
      d += 1;
    }
    blob.system = decode_section(&mut reader)?;
    if blob.system.section_type != SectionType::System {
      return Err(bad);
    }
    let mut n: u32 = 0;
    while n < num_nodes {
      let section = decode_section(&mut reader)?;
      if !section.section_type.is_node() {
        return Err(bad);
      }
      blob.nodes.push(section);
      n += 1;
    }
    let mut l: u32 = 0;
    while l < num_links {
      let section = decode_section(&mut reader)?;
      if !section.section_type.is_link() {
        return Err(bad);
      }
      blob.links.push(section);
      l += 1;
    }
    Ok(blob)
  }

  /// A parameter of a section, falling back to the default section for
  /// its type when the section itself does not state it.
  pub fn value<'a>(
    &'a self,
    section: &'a Section,
    key: u32,
  ) -> Option<&'a ConfigValue> {
    if let Some(value) = section.get(key) {
      return Some(value);
    }
    self.defaults.get(&section.section_type)?.get(key)
  }

  /// A parameter as a 32-bit number, with the default applied.
  pub fn value_u32(&self, section: &Section, key: u32) -> Option<u32> {
    self.value(section, key)?.as_u32()
  }

  /// A parameter as a 64-bit number, with the default applied.
  pub fn value_u64(&self, section: &Section, key: u32) -> Option<u64> {
    self.value(section, key)?.as_u64()
  }

  /// A parameter as text, with the default applied.
  pub fn value_str<'a>(
    &'a self,
    section: &'a Section,
    key: u32,
  ) -> Option<&'a str> {
    self.value(section, key)?.as_str()
  }

  /// The section describing a node, by its node id.
  pub fn node(&self, node_id: u32) -> Option<&Section> {
    self
      .nodes
      .iter()
      .find(|section| section.node_id() == Some(node_id))
  }

  /// Every node section of one type.
  pub fn nodes_of_type(&self, section_type: SectionType) -> Vec<&Section> {
    let mut out: Vec<&Section> = Vec::new();
    for section in &self.nodes {
      if section.section_type == section_type {
        out.push(section);
      }
    }
    out
  }

  /// The section describing the link between two nodes, whichever order
  /// they are given in.
  pub fn link(&self, node_a: u32, node_b: u32) -> Option<&Section> {
    use crate::conf_param::IC_CFG_CONNECTION_NODE_1;
    use crate::conf_param::IC_CFG_CONNECTION_NODE_2;
    for section in &self.links {
      let one = match self.value_u32(section, IC_CFG_CONNECTION_NODE_1) {
        Some(id) => id,
        None => continue,
      };
      let two = match self.value_u32(section, IC_CFG_CONNECTION_NODE_2) {
        Some(id) => id,
        None => continue,
      };
      if (one == node_a && two == node_b) || (one == node_b && two == node_a) {
        return Some(section);
      }
    }
    None
  }

  /// How many sections the configuration holds in total, which is what
  /// a diagnostic prints.
  pub fn num_sections(&self) -> usize {
    self.defaults.len() + 1 + self.nodes.len() + self.links.len()
  }
}

/// Build a configuration blob, for tests and for the tools that write
/// one out again.
pub struct BlobBuilder {
  words: Vec<u32>,
}

impl BlobBuilder {
  /// Start a blob with the given section counts in its header.
  pub fn new(
    num_data_nodes: u32,
    num_api_nodes: u32,
    num_mgm_nodes: u32,
    num_links: u32,
  ) -> BlobBuilder {
    let mut builder = BlobBuilder { words: Vec::new() };
    /* The magic is bytes, not a number, so it is not byte swapped. */
    builder.words.push(u32::from_ne_bytes(*b"NDBC"));
    builder.words.push(u32::from_ne_bytes(*b"ONF2"));
    /* The total length is filled in by finish(). */
    builder.push(0);
    builder.push(2);
    builder.push(5);
    builder.push(num_data_nodes);
    builder.push(num_api_nodes);
    builder.push(num_mgm_nodes);
    builder.push(num_links);
    builder
  }

  fn push(&mut self, word: u32) {
    self.words.push(word.to_be());
  }

  /// Append a section of the given type with the given entries.
  pub fn section(
    &mut self,
    section_type: SectionType,
    entries: &[(u32, ConfigValue)],
  ) {
    let start = self.words.len();
    self.push(0); /* length, filled in below */
    self.push(entries.len() as u32);
    self.push(section_type as u32);
    for (key, value) in entries {
      match value {
        ConfigValue::Int(v) => {
          self.push((1 << IC_V2_TYPE_SHIFT) | key);
          self.push(*v);
        }
        ConfigValue::Int64(v) => {
          self.push((4 << IC_V2_TYPE_SHIFT) | key);
          self.push((*v >> 32) as u32);
          self.push((*v & 0xFFFF_FFFF) as u32);
        }
        ConfigValue::Str(text) => {
          self.push((2 << IC_V2_TYPE_SHIFT) | key);
          let len_with_nul = text.len() + 1;
          self.push(len_with_nul as u32);
          let num_words = len_with_nul.div_ceil(4);
          let mut bytes = text.as_bytes().to_vec();
          bytes.resize(num_words * 4, 0);
          let mut i: usize = 0;
          while i < bytes.len() {
            self.words.push(u32::from_ne_bytes([
              bytes[i],
              bytes[i + 1],
              bytes[i + 2],
              bytes[i + 3],
            ]));
            i += 4;
          }
        }
        ConfigValue::Section(v) => {
          self.push((3 << IC_V2_TYPE_SHIFT) | key);
          self.push(*v);
        }
      }
    }
    let len = (self.words.len() - start) as u32;
    self.words[start] = len.to_be();
  }

  /// Append a section that names its own type, the way every real
  /// section does.
  pub fn typed_section(
    &mut self,
    section_type: SectionType,
    entries: &[(u32, ConfigValue)],
  ) {
    let count = entries.len() + 1;
    let mut all: Vec<(u32, ConfigValue)> = Vec::with_capacity(count);
    all.push((
      IC_CFG_TYPE_OF_SECTION,
      ConfigValue::Int(section_type as u32),
    ));
    for entry in entries {
      all.push(entry.clone());
    }
    self.section(section_type, &all);
  }

  /// Finish the blob: fill in the length and append the checksum.
  pub fn finish(mut self) -> Vec<u8> {
    let total = (self.words.len() + 1) as u32;
    self.words[2] = total.to_be();
    let mut sum: u32 = 0;
    for word in &self.words {
      sum ^= u32::from_be(*word);
    }
    self.words.push(sum.to_be());
    let mut bytes: Vec<u8> = Vec::with_capacity(self.words.len() * 4);
    for word in &self.words {
      bytes.extend_from_slice(&word.to_ne_bytes());
    }
    bytes
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::conf_param::*;

  /// A small but complete configuration: two data nodes, one API node,
  /// one management node and the links between them.
  fn sample_blob() -> Vec<u8> {
    let mut b = BlobBuilder::new(2, 1, 1, 3);
    /* Five default sections in the order the format fixes. */
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_CFG_DB_NO_REPLICAS, ConfigValue::Int(2)),
        (IC_CFG_DB_API_HEARTBEAT_INTERVAL, ConfigValue::Int(1500)),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[
        (IC_CFG_BATCH_SIZE, ConfigValue::Int(256)),
        (IC_CFG_BATCH_BYTE_SIZE, ConfigValue::Int(16384)),
        (IC_CFG_MAX_SCAN_BATCH_SIZE, ConfigValue::Int(262144)),
      ],
    );
    b.typed_section(SectionType::MgmNode, &[]);
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_CFG_TCP_SEND_BUFFER_SIZE, ConfigValue::Int(2097152)),
        (IC_CFG_CONNECTION_CHECKSUM, ConfigValue::Int(0)),
      ],
    );
    b.typed_section(SectionType::Shm, &[]);
    /* The system section. */
    b.typed_section(
      SectionType::System,
      &[(1, ConfigValue::Str("MyCluster".to_string()))],
    );
    /* Node sections: data nodes first. */
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_CFG_NODE_ID, ConfigValue::Int(1)),
        (
          IC_CFG_NODE_HOST,
          ConfigValue::Str("node1.example".to_string()),
        ),
        (IC_CFG_DB_NODEGROUP, ConfigValue::Int(0)),
      ],
    );
    b.typed_section(
      SectionType::DataNode,
      &[
        (IC_CFG_NODE_ID, ConfigValue::Int(2)),
        (
          IC_CFG_NODE_HOST,
          ConfigValue::Str("node2.example".to_string()),
        ),
        (IC_CFG_DB_NODEGROUP, ConfigValue::Int(0)),
        /* This one overrides the default heartbeat interval. */
        (IC_CFG_DB_API_HEARTBEAT_INTERVAL, ConfigValue::Int(3000)),
      ],
    );
    b.typed_section(
      SectionType::ApiNode,
      &[
        (IC_CFG_NODE_ID, ConfigValue::Int(68)),
        (
          IC_CFG_TOTAL_SEND_BUFFER_MEMORY,
          ConfigValue::Int64(8 * 1024 * 1024),
        ),
      ],
    );
    b.typed_section(
      SectionType::MgmNode,
      &[
        (IC_CFG_NODE_ID, ConfigValue::Int(65)),
        (
          IC_CFG_NODE_HOST,
          ConfigValue::Str("mgm.example".to_string()),
        ),
      ],
    );
    /* Links: API node to each data node, and between the data nodes. */
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_CFG_CONNECTION_NODE_1, ConfigValue::Int(1)),
        (IC_CFG_CONNECTION_NODE_2, ConfigValue::Int(68)),
        (IC_CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(0)),
        (IC_CFG_CONNECTION_NODE_ID_SERVER, ConfigValue::Int(1)),
        (
          IC_CFG_CONNECTION_HOSTNAME_1,
          ConfigValue::Str("node1.example".to_string()),
        ),
      ],
    );
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_CFG_CONNECTION_NODE_1, ConfigValue::Int(2)),
        (IC_CFG_CONNECTION_NODE_2, ConfigValue::Int(68)),
        (IC_CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(45000)),
        (IC_CFG_CONNECTION_NODE_ID_SERVER, ConfigValue::Int(2)),
        (IC_CFG_CONNECTION_CHECKSUM, ConfigValue::Int(1)),
      ],
    );
    b.typed_section(
      SectionType::Tcp,
      &[
        (IC_CFG_CONNECTION_NODE_1, ConfigValue::Int(1)),
        (IC_CFG_CONNECTION_NODE_2, ConfigValue::Int(2)),
        (IC_CFG_CONNECTION_SERVER_PORT, ConfigValue::Int(44000)),
      ],
    );
    b.finish()
  }

  #[test]
  fn a_blob_round_trips() {
    let bytes = sample_blob();
    let blob = ConfigBlob::decode(&bytes).expect("decode");
    assert_eq!(blob.num_data_nodes, 2);
    assert_eq!(blob.num_api_nodes, 1);
    assert_eq!(blob.num_mgm_nodes, 1);
    assert_eq!(blob.nodes.len(), 4);
    assert_eq!(blob.links.len(), 3);
    assert_eq!(blob.defaults.len(), 5);
    assert_eq!(blob.num_sections(), 13);
    assert_eq!(blob.system.section_type, SectionType::System);
  }

  #[test]
  fn values_and_defaults() {
    let blob = ConfigBlob::decode(&sample_blob()).expect("decode");
    let node1 = blob.node(1).expect("node 1");
    let node2 = blob.node(2).expect("node 2");
    assert_eq!(node1.section_type, SectionType::DataNode);
    assert_eq!(
      blob.value_str(node1, IC_CFG_NODE_HOST),
      Some("node1.example")
    );
    // Node 1 does not state the heartbeat interval, so the data node
    // default applies; node 2 states its own.
    assert_eq!(
      blob.value_u32(node1, IC_CFG_DB_API_HEARTBEAT_INTERVAL),
      Some(1500)
    );
    assert_eq!(
      blob.value_u32(node2, IC_CFG_DB_API_HEARTBEAT_INTERVAL),
      Some(3000)
    );
    assert_eq!(blob.value_u32(node1, IC_CFG_DB_NO_REPLICAS), Some(2));
    /* An API node reads its own settings the same way. */
    let api = blob.node(68).expect("node 68");
    assert_eq!(api.section_type, SectionType::ApiNode);
    assert_eq!(blob.value_u32(api, IC_CFG_BATCH_SIZE), Some(256));
    assert_eq!(
      blob.value_u64(api, IC_CFG_TOTAL_SEND_BUFFER_MEMORY),
      Some(8 * 1024 * 1024)
    );
    /* A parameter nobody states reads as absent. */
    assert_eq!(blob.value_u32(api, IC_CFG_AUTO_RECONNECT), None);
  }

  #[test]
  fn links_are_found_either_way_round() {
    let blob = ConfigBlob::decode(&sample_blob()).expect("decode");
    let link = blob.link(68, 2).expect("link");
    assert_eq!(
      blob.value_u32(link, IC_CFG_CONNECTION_SERVER_PORT),
      Some(45000)
    );
    let same = blob.link(2, 68).expect("link the other way");
    assert_eq!(
      blob.value_u32(same, IC_CFG_CONNECTION_SERVER_PORT),
      Some(45000)
    );
    /* Checksum is off by default but on for this link. */
    assert_eq!(blob.value_u32(link, IC_CFG_CONNECTION_CHECKSUM), Some(1));
    let other = blob.link(68, 1).expect("other link");
    assert_eq!(blob.value_u32(other, IC_CFG_CONNECTION_CHECKSUM), Some(0));
    /* Port 0 means the data node's port is assigned dynamically. */
    assert_eq!(
      blob.value_u32(other, IC_CFG_CONNECTION_SERVER_PORT),
      Some(0)
    );
    assert!(blob.link(1, 99).is_none());
  }

  #[test]
  fn node_lists() {
    let blob = ConfigBlob::decode(&sample_blob()).expect("decode");
    let data_nodes = blob.nodes_of_type(SectionType::DataNode);
    assert_eq!(data_nodes.len(), 2);
    assert_eq!(data_nodes[0].node_id(), Some(1));
    assert_eq!(data_nodes[1].node_id(), Some(2));
    assert_eq!(blob.nodes_of_type(SectionType::MgmNode).len(), 1);
    assert!(blob.node(999).is_none());
  }

  #[test]
  fn strings_of_every_length() {
    // The padding to whole words is where a string decoder goes wrong,
    // so every length up to three words is checked.
    let mut len: usize = 0;
    while len < 12 {
      let text = "x".repeat(len);
      let mut b = BlobBuilder::new(1, 1, 1, 1);
      let mut i = 0;
      while i < 5 {
        b.typed_section(SectionType::DataNode, &[]);
        i += 1;
      }
      b.typed_section(SectionType::System, &[]);
      b.typed_section(
        SectionType::DataNode,
        &[
          (IC_CFG_NODE_ID, ConfigValue::Int(1)),
          (IC_CFG_NODE_HOST, ConfigValue::Str(text.clone())),
        ],
      );
      b.typed_section(
        SectionType::ApiNode,
        &[(IC_CFG_NODE_ID, ConfigValue::Int(68))],
      );
      b.typed_section(
        SectionType::MgmNode,
        &[(IC_CFG_NODE_ID, ConfigValue::Int(65))],
      );
      b.typed_section(
        SectionType::Tcp,
        &[
          (IC_CFG_CONNECTION_NODE_1, ConfigValue::Int(1)),
          (IC_CFG_CONNECTION_NODE_2, ConfigValue::Int(68)),
        ],
      );
      let blob = ConfigBlob::decode(&b.finish()).expect("decode");
      let node = blob.node(1).expect("node");
      assert_eq!(
        blob.value_str(node, IC_CFG_NODE_HOST),
        Some(text.as_str()),
        "length {}",
        len
      );
      len += 1;
    }
  }

  #[test]
  fn bad_blobs_are_refused() {
    let good = sample_blob();
    /* Not a blob at all. */
    assert!(ConfigBlob::decode(b"").is_err());
    assert!(ConfigBlob::decode(b"nonsense").is_err());
    /* Not a multiple of four bytes. */
    assert!(ConfigBlob::decode(&good[..good.len() - 1]).is_err());
    /* The version 1 format, which we do not accept. */
    let mut v1 = good.clone();
    v1[..8].copy_from_slice(IC_MAGIC_V1);
    let err = ConfigBlob::decode(&v1).expect_err("v1");
    assert_eq!(err.code, err::IC_ERROR_NOT_SUPPORTED);
    /* A flipped bit anywhere fails the checksum. */
    let mut corrupt = good.clone();
    corrupt[40] ^= 0x01;
    let err = ConfigBlob::decode(&corrupt).expect_err("corrupt");
    assert_eq!(err.code, err::IC_ERROR_MESSAGE_CHECKSUM);
    /* Truncated. */
    let mut short = good.clone();
    short.truncate(40);
    assert!(ConfigBlob::decode(&short).is_err());
  }
}
