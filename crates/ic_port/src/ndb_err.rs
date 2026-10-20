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
//! an unknown class. Verify: chapter 05 §17.1.

use crate::err::ErrorCategory;
use crate::err::ErrorSeverity;

/// NDB's classes of error, as the reference sorts its codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorClass {
  /// No error.
  Success,
  /// The application asked for something wrong.
  Application,
  /// The configuration, or the application's use of it, is wrong.
  Configuration,
  /// There is no such row.
  NoData,
  /// The row would break a constraint.
  Constraint,
  /// The schema is not what the request assumed.
  Schema,
  /// An error the application's own program raised.
  AppDefined,
  /// The cluster is out of space for good.
  OutOfSpace,
  /// A resource ran out for the moment.
  TemporaryResource,
  /// A node failed or is recovering.
  NodeRecovery,
  /// The cluster is overloaded.
  Overload,
  /// Something timed out.
  Timeout,
  /// A node is shutting down.
  NodeStopping,
  /// An internal condition that passes.
  TemporaryInternal,
  /// The outcome is not known.
  UnknownResult,
  /// A code this table does not know.
  UnknownCode,
  /// A fault inside the cluster or the library.
  Internal,
  /// Not implemented.
  NotImplemented,
}

impl ErrorClass {
  /// The category the C header reports for the class.
  pub fn category(&self) -> ErrorCategory {
    match self {
      ErrorClass::Success => ErrorCategory::NoError,
      ErrorClass::Application
      | ErrorClass::Configuration
      | ErrorClass::NoData
      | ErrorClass::Constraint
      | ErrorClass::Schema
      | ErrorClass::AppDefined
      | ErrorClass::NotImplemented => ErrorCategory::User,
      ErrorClass::Internal | ErrorClass::UnknownCode => ErrorCategory::Internal,
      _ => ErrorCategory::External,
    }
  }

  /// The severity the C header reports for the class: temporary for
  /// the classes NDB calls temporary, and for an unknown outcome, since
  /// the request may be tried again; permanent for the rest.
  pub fn severity(&self) -> ErrorSeverity {
    match self {
      ErrorClass::Success => ErrorSeverity::NoError,
      ErrorClass::TemporaryResource
      | ErrorClass::NodeRecovery
      | ErrorClass::Overload
      | ErrorClass::Timeout
      | ErrorClass::NodeStopping
      | ErrorClass::TemporaryInternal
      | ErrorClass::UnknownResult => ErrorSeverity::Temporary,
      _ => ErrorSeverity::Error,
    }
  }
}

/// One code: its class, and what to say about it.
struct ErrorEntry {
  code: i32,
  class: ErrorClass,
  text: &'static str,
}

const IC_NDB_ERRORS: &[ErrorEntry] = &[
  // Rows.
  ErrorEntry {
    code: 626,
    class: ErrorClass::NoData,
    text: "There is no row with that key",
  },
  ErrorEntry {
    code: 630,
    class: ErrorClass::Constraint,
    text: "There is already a row with that key",
  },
  ErrorEntry {
    code: 893,
    class: ErrorClass::Constraint,
    text: "The row would break a unique index",
  },
  ErrorEntry {
    code: 899,
    class: ErrorClass::TemporaryResource,
    text: "The row's place was taken meanwhile; try again",
  },
  // Schema.
  ErrorEntry {
    code: 241,
    class: ErrorClass::Schema,
    text: "The table has changed since it was bound",
  },
  ErrorEntry {
    code: 283,
    class: ErrorClass::Schema,
    text: "The table is being dropped",
  },
  ErrorEntry {
    code: 284,
    class: ErrorClass::Schema,
    text: "The coordinator does not know the table",
  },
  ErrorEntry {
    code: 709,
    class: ErrorClass::Schema,
    text: "There is no such table",
  },
  ErrorEntry {
    code: 723,
    class: ErrorClass::Schema,
    text: "There is no such table",
  },
  ErrorEntry {
    code: 710,
    class: ErrorClass::Schema,
    text: "The dictionary can only find that object by id",
  },
  ErrorEntry {
    code: 701,
    class: ErrorClass::Timeout,
    text: "The dictionary is busy with another schema change",
  },
  ErrorEntry {
    code: 702,
    class: ErrorClass::TemporaryInternal,
    text: "The dictionary asked is not the master; ask again",
  },
  // Locks and time.
  ErrorEntry {
    code: 266,
    class: ErrorClass::Timeout,
    text: "The operation timed out in the cluster, probably in a deadlock",
  },
  ErrorEntry {
    code: 274,
    class: ErrorClass::Timeout,
    text: "The scan timed out in the cluster, probably in a deadlock",
  },
  ErrorEntry {
    code: 296,
    class: ErrorClass::Timeout,
    text: "The scan timed out in the cluster, probably in a deadlock",
  },
  ErrorEntry {
    code: 297,
    class: ErrorClass::Timeout,
    text: "The scan timed out in the cluster, probably in a deadlock",
  },
  ErrorEntry {
    code: 237,
    class: ErrorClass::Timeout,
    text: "The transaction timed out while committing",
  },
  ErrorEntry {
    code: 4351,
    class: ErrorClass::Timeout,
    text: "An index build timed out",
  },
  // Resources.
  ErrorEntry {
    code: 218,
    class: ErrorClass::TemporaryResource,
    text: "The data node is out of long message buffers",
  },
  ErrorEntry {
    code: 233,
    class: ErrorClass::TemporaryResource,
    text: "The coordinator is out of operation records",
  },
  ErrorEntry {
    code: 245,
    class: ErrorClass::TemporaryResource,
    text: "Too many scans are running at once",
  },
  ErrorEntry {
    code: 288,
    class: ErrorClass::TemporaryResource,
    text: "The coordinator is out of index operation records",
  },
  ErrorEntry {
    code: 1217,
    class: ErrorClass::TemporaryResource,
    text: "The data manager is out of operation records",
  },
  ErrorEntry {
    code: 1218,
    class: ErrorClass::TemporaryResource,
    text: "The data node's send buffers are full",
  },
  ErrorEntry {
    code: 1220,
    class: ErrorClass::TemporaryResource,
    text: "The redo log files are full",
  },
  ErrorEntry {
    code: 1222,
    class: ErrorClass::TemporaryResource,
    text: "The data manager is out of transaction markers",
  },
  ErrorEntry {
    code: 4021,
    class: ErrorClass::TemporaryResource,
    text: "The API is out of send buffer space",
  },
  ErrorEntry {
    code: 4022,
    class: ErrorClass::TemporaryResource,
    text: "The API is out of send buffer space",
  },
  ErrorEntry {
    code: 4032,
    class: ErrorClass::TemporaryResource,
    text: "The API is out of send buffer space",
  },
  // Overload.
  ErrorEntry {
    code: 410,
    class: ErrorClass::Overload,
    text: "The redo log files are overloaded",
  },
  ErrorEntry {
    code: 1221,
    class: ErrorClass::Overload,
    text: "The redo buffers are overloaded",
  },
  ErrorEntry {
    code: 243,
    class: ErrorClass::Overload,
    text: "The database's write rate limit is reached",
  },
  ErrorEntry {
    code: 2203,
    class: ErrorClass::Overload,
    text: "The database's read rate limit is reached",
  },
  ErrorEntry {
    code: 4006,
    class: ErrorClass::Overload,
    text: "No transaction record is free at the coordinator",
  },
  // Nodes.
  ErrorEntry {
    code: 270,
    class: ErrorClass::NodeStopping,
    text: "A node shut down and aborted the transaction or scan",
  },
  ErrorEntry {
    code: 1204,
    class: ErrorClass::NodeRecovery,
    text: "The cluster's distribution changed; try again",
  },
  ErrorEntry {
    code: 4010,
    class: ErrorClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  ErrorEntry {
    code: 4025,
    class: ErrorClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  ErrorEntry {
    code: 4028,
    class: ErrorClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  ErrorEntry {
    code: 4029,
    class: ErrorClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  ErrorEntry {
    code: 4031,
    class: ErrorClass::NodeRecovery,
    text: "A node failure aborted the transaction",
  },
  ErrorEntry {
    code: 4115,
    class: ErrorClass::NodeRecovery,
    text:
      "The transaction committed at a node failure, but what it read is lost",
  },
  ErrorEntry {
    code: 4119,
    class: ErrorClass::NodeRecovery,
    text: "A node failure lost the reply to a committed read",
  },
  ErrorEntry {
    code: 4002,
    class: ErrorClass::NodeRecovery,
    text: "The request could not be sent to the data node",
  },
  ErrorEntry {
    code: 4007,
    class: ErrorClass::NodeRecovery,
    text: "The request could not be sent to the data node",
  },
  ErrorEntry {
    code: 4033,
    class: ErrorClass::NodeRecovery,
    text: "The request could not be sent to the data node",
  },
  ErrorEntry {
    code: 4008,
    class: ErrorClass::UnknownResult,
    text: "Nothing came back from the data node",
  },
  ErrorEntry {
    code: 4009,
    class: ErrorClass::UnknownResult,
    text: "No data node can be reached",
  },
  ErrorEntry {
    code: 4012,
    class: ErrorClass::UnknownResult,
    text: "The data node did not answer in time",
  },
  ErrorEntry {
    code: 4035,
    class: ErrorClass::UnknownResult,
    text: "The data nodes are up but have no connection record for us",
  },
  // Requests the data nodes refuse.
  ErrorEntry {
    code: 242,
    class: ErrorClass::Application,
    text: "A scan was asked for with no concurrency",
  },
  ErrorEntry {
    code: 4116,
    class: ErrorClass::Application,
    text: "The operation is missing its key",
  },
  ErrorEntry {
    code: 4118,
    class: ErrorClass::Application,
    text: "A parameter of the call is wrong",
  },
  ErrorEntry {
    code: 4120,
    class: ErrorClass::Application,
    text: "The scan is already complete",
  },
  ErrorEntry {
    code: 4200,
    class: ErrorClass::Application,
    text: "The operation is not in a state to be defined",
  },
  ErrorEntry {
    code: 4202,
    class: ErrorClass::Application,
    text: "A primary key column cannot be changed",
  },
  ErrorEntry {
    code: 4203,
    class: ErrorClass::Application,
    text: "A column that cannot be NULL was set to NULL",
  },
  ErrorEntry {
    code: 4209,
    class: ErrorClass::Application,
    text: "A value's length does not fit its column",
  },
  ErrorEntry {
    code: 4225,
    class: ErrorClass::Application,
    text: "Every key column is already given",
  },
  ErrorEntry {
    code: 4230,
    class: ErrorClass::Application,
    text: "Only a read can bring values back",
  },
  ErrorEntry {
    code: 4234,
    class: ErrorClass::Application,
    text: "A value cannot be set in this state",
  },
  ErrorEntry {
    code: 4243,
    class: ErrorClass::Application,
    text: "There is no such index",
  },
  ErrorEntry {
    code: 4244,
    class: ErrorClass::Application,
    text: "An index or table of that name already exists",
  },
  ErrorEntry {
    code: 4249,
    class: ErrorClass::Application,
    text: "The table is not valid",
  },
  ErrorEntry {
    code: 4251,
    class: ErrorClass::Application,
    text: "The unique index cannot be made: the rows have duplicates",
  },
  ErrorEntry {
    code: 4254,
    class: ErrorClass::Application,
    text: "The table is not an index",
  },
  ErrorEntry {
    code: 4259,
    class: ErrorClass::Application,
    text: "The scan's bounds do not make a range",
  },
  ErrorEntry {
    code: 4272,
    class: ErrorClass::Application,
    text: "The table has a column with no definition",
  },
  ErrorEntry {
    code: 4277,
    class: ErrorClass::Application,
    text: "A key part is too short for its column",
  },
  ErrorEntry {
    code: 4279,
    class: ErrorClass::Application,
    text: "A string in the key is malformed",
  },
  ErrorEntry {
    code: 4280,
    class: ErrorClass::Application,
    text: "A key part's length is not consistent",
  },
  ErrorEntry {
    code: 4316,
    class: ErrorClass::Application,
    text: "A key column cannot be NULL",
  },
  // Interpreted programs. The data node's own refusals of a program
  // are the application's error, as NDB classes them; 6000 is ours,
  // the code our programs refuse with where a missed condition is to
  // abort (chapter 05 §9.4).
  ErrorEntry {
    code: 6000,
    class: ErrorClass::AppDefined,
    text: "The row did not meet the operation's condition",
  },
  ErrorEntry {
    code: 871,
    class: ErrorClass::Application,
    text: "The program has an instruction the data node does not know",
  },
  ErrorEntry {
    code: 872,
    class: ErrorClass::Application,
    text: "The program ran past its end without accepting or refusing",
  },
  ErrorEntry {
    code: 876,
    class: ErrorClass::Application,
    text: "The program's instructions do not fit its size",
  },
  ErrorEntry {
    code: 878,
    class: ErrorClass::Application,
    text: "The program compared or computed with a NULL register",
  },
  ErrorEntry {
    code: 879,
    class: ErrorClass::Application,
    text: "The program read or wrote a column the table does not have",
  },
  ErrorEntry {
    code: 882,
    class: ErrorClass::Application,
    text: "The sizes of the program's parts do not add up",
  },
  ErrorEntry {
    code: 888,
    class: ErrorClass::Application,
    text: "The program wrote a column, and the operation is no update",
  },
  ErrorEntry {
    code: 846,
    class: ErrorClass::Application,
    text: "The program read an input beyond the sixteenth",
  },
  ErrorEntry {
    code: 937,
    class: ErrorClass::Application,
    text: "The program's inputs do not add up, or one is not 8 bytes",
  },
  ErrorEntry {
    code: 938,
    class: ErrorClass::Application,
    text: "The program was given more than 16 inputs",
  },
  ErrorEntry {
    code: 939,
    class: ErrorClass::Application,
    text: "Among the program's inputs is a column that is no input",
  },
  ErrorEntry {
    code: 4260,
    class: ErrorClass::AppDefined,
    text: "The interpreted condition uses an operator its group has not",
  },
  ErrorEntry {
    code: 4261,
    class: ErrorClass::AppDefined,
    text: "The interpreted condition read a NULL column",
  },
  ErrorEntry {
    code: 4262,
    class: ErrorClass::AppDefined,
    text: "The interpreted condition is out of bounds",
  },
  // Faults.
  ErrorEntry {
    code: 4001,
    class: ErrorClass::Internal,
    text: "A signal was malformed",
  },
  ErrorEntry {
    code: 4350,
    class: ErrorClass::Internal,
    text: "The transaction was already aborted",
  },
];

fn find(code: i32) -> Option<&'static ErrorEntry> {
  IC_NDB_ERRORS.iter().find(|error| error.code == code)
}

/// True if the code is one this table knows.
pub fn is_known(code: i32) -> bool {
  find(code).is_some()
}

/// NDB's class of the code, or unknown for one not in the table.
pub fn class_of(code: i32) -> ErrorClass {
  if code == 0 {
    return ErrorClass::Success;
  }
  match find(code) {
    Some(error) => error.class,
    None => ErrorClass::UnknownCode,
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
    for code in [626, 630, 4010, 4031, 4115, 709, 701, 243, 270] {
      assert!(is_known(code), "code {}", code);
      assert!(text_of(code).is_some());
    }
    assert!(!is_known(12345));
    assert_eq!(class_of(12345), ErrorClass::UnknownCode);
    assert_eq!(class_of(0), ErrorClass::Success);
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
    // A scan whose reading node shut down is tried again.
    assert_eq!(class_of(270).severity(), ErrorSeverity::Temporary);
    // A row that missed its condition, where that is to abort.
    assert_eq!(class_of(6000), ErrorClass::AppDefined);
    assert_eq!(class_of(6000).category(), ErrorCategory::User);
    assert_eq!(class_of(6000).severity(), ErrorSeverity::Error);
    assert_eq!(class_of(4350).category(), ErrorCategory::Internal);
    assert_eq!(class_of(0).severity(), ErrorSeverity::NoError);
  }
}
