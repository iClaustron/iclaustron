// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Asking a data node's dictionary for a table's description.
//!
//! The request goes to the dictionary block of any started data node,
//! with the table's internal name, `database/schema/table`, in section
//! 0. The answer carries the description in its own section 0. A data
//! node sends it whole up to 7400 words and in fragments beyond that,
//! so most descriptions arrive whole and one of a table with very many
//! columns does not.
//!
//! The name is sent with its terminating NUL, and the length word
//! counts that NUL; the section is the name padded with zeroes to whole
//! words.
//!
//! Verify: `GetTabInfo.hpp`; `NdbDictionaryImpl.cpp`,
//! `NdbDictInterface::getTable` for what the API sends and
//! `execGET_TABINFO_CONF` for how it gathers the fragments;
//! `Dbdict.cpp`, `sendGetTabResponse`; `SimulatedBlock.cpp`,
//! `sendFirstFragment`, and `ndb_limits.h`, `MAX_SIZE_SINGLE_SIGNAL`.

use ic_port::err;
use ic_port::IcError;

/// Words in a `GET_TABINFOREQ`.
pub const IC_GET_TABINFOREQ_LEN: usize = 5;
/// Words in a `GET_TABINFO_CONF`.
pub const IC_GET_TABINFO_CONF_LEN: usize = 6;
/// Words in a `GET_TABINFOREF`.
pub const IC_GET_TABINFOREF_LEN: usize = 7;
/// Words in the `GET_TABINFOREF` of an older data node, which put the
/// error code in its last word.
pub const IC_GET_TABINFOREF_OLD_LEN: usize = 5;

/// Ask by table id.
pub const IC_GET_TABINFO_BY_ID: u32 = 0;
/// Ask by table name.
pub const IC_GET_TABINFO_BY_NAME: u32 = 1;
/// Send the answer as a long signal, with the description in a section.
pub const IC_GET_TABINFO_LONG_CONF: u32 = 2;

/// The dictionary is busy with something else; ask again shortly.
pub const IC_GET_TABINFO_ERR_BUSY: u32 = 701;
/// The name is longer than the dictionary accepts.
pub const IC_GET_TABINFO_ERR_NAME_TOO_LONG: u32 = 702;
/// No table has that id.
pub const IC_GET_TABINFO_ERR_NO_SUCH_ID: u32 = 709;
/// This kind of object cannot be fetched by name.
pub const IC_GET_TABINFO_ERR_NO_FETCH_BY_NAME: u32 = 710;
/// No table has that name.
pub const IC_GET_TABINFO_ERR_NOT_DEFINED: u32 = 723;

/// Ask for a table's description.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GetTabInfoReq {
  /// Our own number for the request, which comes back in the answer.
  pub sender_data: u32,
  /// Our block reference, which is where the answer is sent.
  pub sender_ref: u32,
  /// `IC_GET_TABINFO_BY_*`, with `IC_GET_TABINFO_LONG_CONF` added.
  pub request_type: u32,
  /// The table id, or the name's length in bytes counting its NUL.
  pub table_id_or_name_len: u32,
  /// Zero, unless asking from inside a schema transaction of our own.
  pub schema_trans_id: u32,
}

impl GetTabInfoReq {
  /// A request by name, the name to go in section 0 as
  /// [`name_section`] makes it.
  pub fn by_name(
    sender_data: u32,
    sender_ref: u32,
    name: &str,
  ) -> GetTabInfoReq {
    GetTabInfoReq {
      sender_data,
      sender_ref,
      request_type: IC_GET_TABINFO_BY_NAME | IC_GET_TABINFO_LONG_CONF,
      table_id_or_name_len: name.len() as u32 + 1,
      schema_trans_id: 0,
    }
  }

  /// A request by table id, which needs no section.
  pub fn by_id(
    sender_data: u32,
    sender_ref: u32,
    table_id: u32,
  ) -> GetTabInfoReq {
    GetTabInfoReq {
      sender_data,
      sender_ref,
      request_type: IC_GET_TABINFO_BY_ID | IC_GET_TABINFO_LONG_CONF,
      table_id_or_name_len: table_id,
      schema_trans_id: 0,
    }
  }

  /// The words of signal data this carries.
  pub fn encode(&self) -> [u32; IC_GET_TABINFOREQ_LEN] {
    [
      self.sender_data,
      self.sender_ref,
      self.request_type,
      self.table_id_or_name_len,
      self.schema_trans_id,
    ]
  }
}

/// A name as section 0 of a request: its bytes, a NUL, and zeroes to
/// the end of the last word. The bytes go in as they are, in the order
/// of the words' memory, since the receiver reads them as bytes.
pub fn name_section(name: &str) -> Vec<u32> {
  let mut bytes: Vec<u8> = name.as_bytes().to_vec();
  bytes.push(0);
  let mut words: Vec<u32> = Vec::new();
  let mut i: usize = 0;
  while i < bytes.len() {
    let mut chunk: [u8; 4] = [0; 4];
    let mut j: usize = 0;
    while j < 4 && i + j < bytes.len() {
      chunk[j] = bytes[i + j];
      j += 1;
    }
    words.push(u32::from_ne_bytes(chunk));
    i += 4;
  }
  words
}

/// The description, in section 0 of this signal or of its fragments.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GetTabInfoConf {
  /// Our own number for the request.
  pub sender_data: u32,
  /// The table's id.
  pub table_id: u32,
  /// The global checkpoint of the description, or zero.
  pub gci: u32,
  /// The description's whole length in words, across every fragment.
  pub total_len: u32,
  /// What kind of object it is.
  pub table_type: u32,
  /// The dictionary block that answered.
  pub sender_ref: u32,
}

impl GetTabInfoConf {
  /// Read the answer. Every fragment of a fragmented answer carries
  /// these same words at the front.
  pub fn decode(data: &[u32]) -> Result<GetTabInfoConf, IcError> {
    if data.len() < IC_GET_TABINFO_CONF_LEN {
      return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
    }
    Ok(GetTabInfoConf {
      sender_data: data[0],
      table_id: data[1],
      gci: data[2],
      total_len: data[3],
      table_type: data[4],
      sender_ref: data[5],
    })
  }
}

/// The dictionary would not give the description.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GetTabInfoRef {
  /// Our own number for the request.
  pub sender_data: u32,
  /// Why not, as an NDB error code: `IC_GET_TABINFO_ERR_*` and others.
  pub error_code: u32,
  /// Where in the data node's source it was refused, for the trace.
  pub error_line: u32,
}

impl GetTabInfoRef {
  /// Read the refusal, from a current or an older data node.
  pub fn decode(data: &[u32]) -> Result<GetTabInfoRef, IcError> {
    if data.len() >= IC_GET_TABINFOREF_LEN {
      return Ok(GetTabInfoRef {
        sender_data: data[0],
        error_code: data[5],
        error_line: data[6],
      });
    }
    if data.len() >= IC_GET_TABINFOREF_OLD_LEN {
      return Ok(GetTabInfoRef {
        sender_data: data[0],
        error_code: data[4],
        error_line: 0,
      });
    }
    Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA))
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn bytes_of(words: &[u32]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    for word in words {
      out.extend_from_slice(&word.to_ne_bytes());
    }
    out
  }

  #[test]
  fn a_name_is_sent_with_its_nul_and_padded_to_words() {
    let name = "ictest/def/t1";
    let req = GetTabInfoReq::by_name(9, 0x8000_00C0, name);
    // Thirteen bytes and a NUL.
    assert_eq!(req.table_id_or_name_len, 14);
    assert_eq!(req.encode()[2], 3);
    let section = name_section(name);
    assert_eq!(section.len(), 4);
    let bytes = bytes_of(&section);
    assert_eq!(&bytes[..13], name.as_bytes());
    assert_eq!(&bytes[13..], &[0, 0, 0]);
  }

  #[test]
  fn a_name_filling_its_words_still_gets_a_nul() {
    // Four bytes fill a word exactly; the NUL needs a word of its own.
    let section = name_section("abcd");
    assert_eq!(section.len(), 2);
    assert_eq!(bytes_of(&section)[4], 0);
  }

  #[test]
  fn an_answer_says_how_long_the_whole_description_is() {
    let conf =
      GetTabInfoConf::decode(&[9, 17, 0, 250, 2, 0xFA0001]).expect("conf");
    assert_eq!(conf.sender_data, 9);
    assert_eq!(conf.table_id, 17);
    assert_eq!(conf.total_len, 250);
    assert!(GetTabInfoConf::decode(&[9, 17, 0, 250, 2]).is_err());
  }

  #[test]
  fn refusals_are_read_from_either_layout() {
    let now =
      GetTabInfoRef::decode(&[9, 1, 3, 14, 0, 723, 1234]).expect("current");
    assert_eq!(now.error_code, IC_GET_TABINFO_ERR_NOT_DEFINED);
    assert_eq!(now.error_line, 1234);
    let old = GetTabInfoRef::decode(&[9, 1, 3, 14, 701]).expect("old");
    assert_eq!(old.error_code, IC_GET_TABINFO_ERR_BUSY);
    assert!(GetTabInfoRef::decode(&[9, 1]).is_err());
  }
}
