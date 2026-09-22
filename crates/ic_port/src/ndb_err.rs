// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The error codes the data nodes report, with what kind of error each
//! is and a sentence of our own about it. New: the C passed NDB codes
//! through untouched.
//!
//! An NDB error code says what happened; which kind of error it is
//! decides what an application does about it. NDB sorts its codes into
//! classes, and each class into permanent, temporary or unknown
//! outcome; the class of each code here is that fact, taken from the
//! reference, and the two things the C header asks for, a category and
//! a severity, follow from the class. The text is ours.
//!
//! Only the codes this library and its tools have met, or are likely
//! to, are here: those of key operations, transactions, the dictionary
//! and the cluster's state. A code not here is still an NDB code, with
//! an unknown class. Verify: `ndberror.cpp`, the classification table
//! and the status of each class.

use crate::err::ErrorCategory;
use crate::err::ErrorSeverity;

/// NDB's classes of error, as the reference sorts its codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NdbClass {
  /// No error.
  NoError,
  /// The application asked for something wrong.
  Application,
  /// The configuration, or the application's use of it, is wrong.
  Configuration,
  /// There is no such row.
  NoData,
  /// The row would break a constraint.
  ConstraintViolation,
  /// The schema is not what the request assumed.
  Schema,
  /// An error the application's own program raised.
  UserDefined,
  /// The cluster is out of space for good.
  InsufficientSpace,
  /// A resource ran out for the moment.
  TemporaryResource,
  /// A node failed or is recovering.
  NodeRecovery,
  /// The cluster is overloaded.
  Overload,
  /// Something timed out.
  Timeout,
  /// A node is shutting down.
  NodeShutdown,
  /// An internal condition that passes.
  InternalTemporary,
  /// The outcome is not known.
  UnknownResult,
  /// A code this table does not know.
  UnknownCode,
  /// A fault inside the cluster or the library.
  Internal,
  /// Not implemented.
  NotImplemented,
}

impl NdbClass {
  /// The category the C header reports for the class.
  pub fn category(&self) -> ErrorCategory {
    match self {
      NdbClass::NoError => ErrorCategory::NoError,
      NdbClass::Application
      | NdbClass::Configuration
      | NdbClass::NoData
      | NdbClass::ConstraintViolation
      | NdbClass::Schema
      | NdbClass::UserDefined
      | NdbClass::NotImplemented => ErrorCategory::User,
      NdbClass::Internal | NdbClass::UnknownCode => ErrorCategory::Internal,
      _ => ErrorCategory::External,
    }
  }

  /// The severity the C header reports for the class: temporary for
  /// the classes NDB calls temporary, and for an unknown outcome, since
  /// the request may be tried again; permanent for the rest.
  pub fn severity(&self) -> ErrorSeverity {
    match self {
      NdbClass::NoError => ErrorSeverity::NoError,
      NdbClass::TemporaryResource
      | NdbClass::NodeRecovery
      | NdbClass::Overload
      | NdbClass::Timeout
      | NdbClass::NodeShutdown
      | NdbClass::InternalTemporary
      | NdbClass::UnknownResult => ErrorSeverity::Temporary,
      _ => ErrorSeverity::Error,
    }
  }
}

/// One code: its class, and what to say about it.
struct NdbError {
  code: i32,
  class: NdbClass,
  text: &'static str,
}

const IC_NDB_ERRORS: &[NdbError] = &[
  // Rows.
  NdbError {
    code: 626,
    class: NdbClass::NoData,
    text: "There is no row with that key",
  },
  NdbError {
    code: 630,
    class: NdbClass::ConstraintViolation,
    text: "There is already a row with that key",
  },
  NdbError {
    code: 893,
    class: NdbClass::ConstraintViolation,
    text: "The row would break a unique index",
  },
  NdbError {
    code: 899,
    class: NdbClass::TemporaryResource,
    text: "The row's place was taken meanwhile; try again",
  },
  // Schema.
  NdbError {
    code: 241,
    class: NdbClass::Schema,
    text: "The table has changed since it was bound",
  },
  NdbError {
    code: 283,
    class: NdbClass::Schema,
    text: "The table is being dropped",
  },
  NdbError {
    code: 284,
    class: NdbClass::Schema,
    text: "The coordinator does not know the table",
  },
  NdbError {
    code: 709,
    class: NdbClass::Schema,
    text: "There is no such table",
  },
  NdbError {
    code: 723,
    class: NdbClass::Schema,
    text: "There is no such table",
  },
  NdbError {
    code: 710,
    class: NdbClass::Schema,
    text: "The dictionary can only find that object by id",
  },
  NdbError {
    code: 701,
    class: NdbClass::Timeout,
    text: "The dictionary is busy with another schema change",
  },
  NdbError {
    code: 702,
    class: NdbClass::InternalTemporary,
    text: "The dictionary asked is not the master; ask again",
  },
  // Locks and time.
  NdbError {
    code: 266,
    class: NdbClass::Timeout,
    text: "The operation timed out in the cluster, probably in a deadlock",
  },
  NdbError {
    code: 274,
    class: NdbClass::Timeout,
    text: "The scan timed out in the cluster, probably in a deadlock",
  },
  NdbError {
    code: 296,
    class: NdbClass::Timeout,
    text: "The scan timed out in the cluster, probably in a deadlock",
  },
  NdbError {
    code: 297,
    class: NdbClass::Timeout,
    text: "The scan timed out in the cluster, probably in a deadlock",
  },
  NdbError {
    code: 237,
    class: NdbClass::Timeout,
    text: "The transaction timed out while committing",
  },
  NdbError {
    code: 4351,
    class: NdbClass::Timeout,
    text: "An index build timed out",
  },
  // Resources.
  NdbError {
    code: 218,
    class: NdbClass::TemporaryResource,
    text: "The data node is out of long message buffers",
  },
  NdbError {
    code: 233,
    class: NdbClass::TemporaryResource,
    text: "The coordinator is out of operation records",
  },
  NdbError {
    code: 245,
    class: NdbClass::TemporaryResource,
    text: "Too many scans are running at once",
  },
  NdbError {
    code: 288,
    class: NdbClass::TemporaryResource,
    text: "The coordinator is out of index operation records",
  },
  NdbError {
    code: 1217,
    class: NdbClass::TemporaryResource,
    text: "The data manager is out of operation records",
  },
  NdbError {
    code: 1218,
    class: NdbClass::TemporaryResource,
    text: "The data node's send buffers are full",
  },
  NdbError {
    code: 1220,
    class: NdbClass::TemporaryResource,
    text: "The redo log files are full",
  },
  NdbError {
    code: 1222,
    class: NdbClass::TemporaryResource,
    text: "The data manager is out of transaction markers",
  },
  NdbError {
    code: 4021,
    class: NdbClass::TemporaryResource,
    text: "The API is out of send buffer space",
  },
  NdbError {
    code: 4022,
    class: NdbClass::TemporaryResource,
    text: "The API is out of send buffer space",
  },
  NdbError {
    code: 4032,
    class: NdbClass::TemporaryResource,
    text: "The API is out of send buffer space",
  },
  // Overload.
  NdbError {
    code: 410,
    class: NdbClass::Overload,
    text: "The redo log files are overloaded",
  },
  NdbError {
    code: 1221,
    class: NdbClass::Overload,
    text: "The redo buffers are overloaded",
  },
  NdbError {
    code: 243,
    class: NdbClass::Overload,
    text: "The database's write rate limit is reached",
  },
  NdbError {
    code: 2203,
    class: NdbClass::Overload,
    text: "The database's read rate limit is reached",
  },
  NdbError {
    code: 4006,
    class: NdbClass::Overload,
    text: "No transaction record is free at the coordinator",
  },
  // Nodes.
  NdbError {
    code: 1204,
    class: NdbClass::NodeRecovery,
    text: "The cluster's distribution changed; try again",
  },
  NdbError {
    code: 4010,
    class: NdbClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  NdbError {
    code: 4025,
    class: NdbClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  NdbError {
    code: 4028,
    class: NdbClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  NdbError {
    code: 4029,
    class: NdbClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  NdbError {
    code: 4031,
    class: NdbClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  NdbError {
    code: 4115,
    class: NdbClass::NodeRecovery,
    text:
      "The transaction committed at a node failure, but what it read is lost",
  },
  NdbError {
    code: 4119,
    class: NdbClass::NodeRecovery,
    text: "A node failure lost the reply to a committed read",
  },
  NdbError {
    code: 4002,
    class: NdbClass::NodeRecovery,
    text: "The request could not be sent to the data node",
  },
  NdbError {
    code: 4007,
    class: NdbClass::NodeRecovery,
    text: "The request could not be sent to the data node",
  },
  NdbError {
    code: 4033,
    class: NdbClass::NodeRecovery,
    text: "The request could not be sent to the data node",
  },
  NdbError {
    code: 4008,
    class: NdbClass::UnknownResult,
    text: "Nothing came back from the data node",
  },
  NdbError {
    code: 4009,
    class: NdbClass::UnknownResult,
    text: "No data node can be reached",
  },
  NdbError {
    code: 4012,
    class: NdbClass::UnknownResult,
    text: "The data node did not answer in time",
  },
  NdbError {
    code: 4035,
    class: NdbClass::UnknownResult,
    text: "The data nodes are up but have no connection record for us",
  },
  // Requests the data nodes refuse.
  NdbError {
    code: 242,
    class: NdbClass::Application,
    text: "A scan was asked for with no concurrency",
  },
  NdbError {
    code: 4116,
    class: NdbClass::Application,
    text: "The operation is missing its key",
  },
  NdbError {
    code: 4118,
    class: NdbClass::Application,
    text: "A parameter of the call is wrong",
  },
  NdbError {
    code: 4120,
    class: NdbClass::Application,
    text: "The scan is already complete",
  },
  NdbError {
    code: 4200,
    class: NdbClass::Application,
    text: "The operation is not in a state to be defined",
  },
  NdbError {
    code: 4202,
    class: NdbClass::Application,
    text: "A primary key column cannot be changed",
  },
  NdbError {
    code: 4203,
    class: NdbClass::Application,
    text: "A column that cannot be NULL was set to NULL",
  },
  NdbError {
    code: 4209,
    class: NdbClass::Application,
    text: "A value's length does not fit its column",
  },
  NdbError {
    code: 4225,
    class: NdbClass::Application,
    text: "Every key column is already given",
  },
  NdbError {
    code: 4230,
    class: NdbClass::Application,
    text: "Only a read can bring values back",
  },
  NdbError {
    code: 4234,
    class: NdbClass::Application,
    text: "A value cannot be set in this state",
  },
  NdbError {
    code: 4243,
    class: NdbClass::Application,
    text: "There is no such index",
  },
  NdbError {
    code: 4244,
    class: NdbClass::Application,
    text: "An index or table of that name already exists",
  },
  NdbError {
    code: 4249,
    class: NdbClass::Application,
    text: "The table is not valid",
  },
  NdbError {
    code: 4251,
    class: NdbClass::Application,
    text: "The unique index cannot be made: the rows have duplicates",
  },
  NdbError {
    code: 4254,
    class: NdbClass::Application,
    text: "The table is not an index",
  },
  NdbError {
    code: 4259,
    class: NdbClass::Application,
    text: "The scan's bounds do not make a range",
  },
  NdbError {
    code: 4272,
    class: NdbClass::Application,
    text: "The table has a column with no definition",
  },
  NdbError {
    code: 4277,
    class: NdbClass::Application,
    text: "A key part is too short for its column",
  },
  NdbError {
    code: 4279,
    class: NdbClass::Application,
    text: "A string in the key is malformed",
  },
  NdbError {
    code: 4280,
    class: NdbClass::Application,
    text: "A key part's length is not consistent",
  },
  NdbError {
    code: 4316,
    class: NdbClass::Application,
    text: "A key column cannot be NULL",
  },
  NdbError {
    code: 4260,
    class: NdbClass::UserDefined,
    text: "The interpreted condition uses an operator its group has not",
  },
  NdbError {
    code: 4261,
    class: NdbClass::UserDefined,
    text: "The interpreted condition read a NULL column",
  },
  NdbError {
    code: 4262,
    class: NdbClass::UserDefined,
    text: "The interpreted condition is out of bounds",
  },
  // Faults.
  NdbError {
    code: 4001,
    class: NdbClass::Internal,
    text: "A signal was malformed",
  },
  NdbError {
    code: 4350,
    class: NdbClass::Internal,
    text: "The transaction was already aborted",
  },
];

fn find(code: i32) -> Option<&'static NdbError> {
  IC_NDB_ERRORS.iter().find(|error| error.code == code)
}

/// True if the code is one this table knows.
pub fn is_known(code: i32) -> bool {
  find(code).is_some()
}

/// NDB's class of the code, or unknown for one not in the table.
pub fn class_of(code: i32) -> NdbClass {
  if code == 0 {
    return NdbClass::NoError;
  }
  match find(code) {
    Some(error) => error.class,
    None => NdbClass::UnknownCode,
  }
}

/// What to say about the code, if it is known.
pub fn text_of(code: i32) -> Option<&'static str> {
  match find(code) {
    Some(error) => Some(error.text),
    None => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn the_codes_met_so_far_are_known() {
    for code in [626, 630, 4010, 4031, 4115, 709, 701, 243] {
      assert!(is_known(code), "code {}", code);
      assert!(text_of(code).is_some());
    }
    assert!(!is_known(12345));
    assert_eq!(class_of(12345), NdbClass::UnknownCode);
    assert_eq!(class_of(0), NdbClass::NoError);
  }

  #[test]
  fn each_code_is_listed_once() {
    let mut i: usize = 0;
    while i < IC_NDB_ERRORS.len() {
      let mut j = i + 1;
      while j < IC_NDB_ERRORS.len() {
        assert_ne!(
          IC_NDB_ERRORS[i].code, IC_NDB_ERRORS[j].code,
          "code {} twice",
          IC_NDB_ERRORS[i].code
        );
        j += 1;
      }
      i += 1;
    }
  }

  #[test]
  fn classes_map_to_the_headers_category_and_severity() {
    assert_eq!(class_of(626).category(), ErrorCategory::User);
    assert_eq!(class_of(626).severity(), ErrorSeverity::Error);
    assert_eq!(class_of(4010).category(), ErrorCategory::External);
    assert_eq!(class_of(4010).severity(), ErrorSeverity::Temporary);
    assert_eq!(class_of(4009).severity(), ErrorSeverity::Temporary);
    assert_eq!(class_of(4350).category(), ErrorCategory::Internal);
    assert_eq!(class_of(0).severity(), ErrorSeverity::NoError);
  }
}
