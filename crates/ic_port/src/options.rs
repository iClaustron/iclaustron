// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Table driven command line parsing, replacing glib's `GOptionEntry` /
//! `GOptionContext` used by `ic_start_program`.
//!
//! A program declares its options as an array of [`OptionEntry`], like
//! the `GOptionEntry ic_apid_entries[]` tables in the C code, adds them
//! to an [`OptionParser`] and calls `parse`. Values are then read back by
//! long name. Accepted forms: `--name=value`, `--name value`, `-n value`,
//! `-n=value`, `--flag`, `--flag=0|1`, and `--` to end option parsing.
//! `--help`, `-h` and `-?` print the help text and return
//! `IC_ERROR_HELP_REQUESTED`.

use crate::err;
use crate::IcError;

/// What kind of value an option takes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OptionKind {
  /// No value; present means true. `--flag=0` turns it off.
  Flag,
  /// A signed integer; suffixes `k`, `m`, `g` multiply by 1024^n.
  Int,
  /// Any text.
  Str,
}

/// One command line option, declared statically by the program.
#[derive(Clone, Copy, Debug)]
pub struct OptionEntry {
  /// Name after `--`, without the dashes.
  pub long_name: &'static str,
  /// Single-letter alias after `-`, or 0 for none.
  pub short_name: u8,
  /// Kind of value.
  pub kind: OptionKind,
  /// One line of help text.
  pub help: &'static str,
}

/// The parsed value of one option.
#[derive(Clone, Debug, Default)]
pub struct OptionValue {
  /// True if the option appeared on the command line.
  pub is_set: bool,
  /// Value for `Int` options.
  pub int_value: i64,
  /// Value for `Str` options.
  pub str_value: String,
  /// Value for `Flag` options.
  pub flag_value: bool,
}

/// The parser: the declared entries, their values, and the arguments
/// that were not options.
pub struct OptionParser {
  program_name: String,
  description: String,
  entries: Vec<OptionEntry>,
  values: Vec<OptionValue>,
  positional: Vec<String>,
}

/// Parse an integer with an optional sign and an optional `k`, `m` or
/// `g` suffix (powers of 1024).
pub fn parse_int(text: &str) -> Option<i64> {
  let trimmed = text.trim();
  if trimmed.is_empty() {
    return None;
  }
  let mut multiplier: i64 = 1;
  let mut digits = trimmed;
  let last = trimmed.as_bytes()[trimmed.len() - 1];
  if last == b'k' || last == b'K' {
    multiplier = 1024;
    digits = &trimmed[..trimmed.len() - 1];
  } else if last == b'm' || last == b'M' {
    multiplier = 1024 * 1024;
    digits = &trimmed[..trimmed.len() - 1];
  } else if last == b'g' || last == b'G' {
    multiplier = 1024 * 1024 * 1024;
    digits = &trimmed[..trimmed.len() - 1];
  }
  match digits.parse::<i64>() {
    Ok(v) => v.checked_mul(multiplier),
    Err(_) => None,
  }
}

fn parse_flag(text: &str) -> Option<bool> {
  let lower = text.trim().to_ascii_lowercase();
  if lower == "1" || lower == "true" || lower == "yes" || lower == "on" {
    return Some(true);
  }
  if lower == "0" || lower == "false" || lower == "no" || lower == "off" {
    return Some(false);
  }
  None
}

impl OptionParser {
  /// A parser for the named program with a description for the help
  /// text.
  pub fn new(program_name: &str, description: &str) -> OptionParser {
    OptionParser {
      program_name: program_name.to_string(),
      description: description.to_string(),
      entries: Vec::new(),
      values: Vec::new(),
      positional: Vec::new(),
    }
  }

  /// Declare one option.
  pub fn add_entry(&mut self, entry: OptionEntry) {
    self.entries.push(entry);
    self.values.push(OptionValue::default());
  }

  /// Declare a table of options.
  pub fn add_entries(&mut self, entries: &[OptionEntry]) {
    for entry in entries {
      self.add_entry(*entry);
    }
  }

  fn find_long(&self, name: &str) -> Option<usize> {
    let mut i: usize = 0;
    while i < self.entries.len() {
      if self.entries[i].long_name == name {
        return Some(i);
      }
      i += 1;
    }
    None
  }

  fn find_short(&self, c: u8) -> Option<usize> {
    let mut i: usize = 0;
    while i < self.entries.len() {
      if self.entries[i].short_name == c && c != 0 {
        return Some(i);
      }
      i += 1;
    }
    None
  }

  fn set_value(
    &mut self,
    index: usize,
    text: Option<&str>,
  ) -> Result<(), IcError> {
    let kind = self.entries[index].kind;
    let name = self.entries[index].long_name;
    let value = &mut self.values[index];
    value.is_set = true;
    match kind {
      OptionKind::Flag => {
        value.flag_value = match text {
          None => true,
          Some(t) => match parse_flag(t) {
            Some(b) => b,
            None => {
              crate::ic_printf!("Bad boolean value '{}' for --{}", t, name);
              let e = err::IC_ERROR_OPTION_VALUE;
              return Err(IcError::new(e));
            }
          },
        };
      }
      OptionKind::Int => {
        let t = match text {
          Some(t) => t,
          None => {
            crate::ic_printf!("Option --{} needs a value", name);
            return Err(IcError::new(err::IC_ERROR_OPTION_VALUE));
          }
        };
        value.int_value = match parse_int(t) {
          Some(v) => v,
          None => {
            crate::ic_printf!("Bad integer value '{}' for --{}", t, name);
            return Err(IcError::new(err::IC_ERROR_OPTION_VALUE));
          }
        };
      }
      OptionKind::Str => {
        value.str_value = match text {
          Some(t) => t.to_string(),
          None => {
            crate::ic_printf!("Option --{} needs a value", name);
            return Err(IcError::new(err::IC_ERROR_OPTION_VALUE));
          }
        };
      }
    }
    Ok(())
  }

  /// Parse the arguments (without the program name).
  pub fn parse(&mut self, args: &[String]) -> Result<(), IcError> {
    let mut i: usize = 0;
    let mut only_positional = false;
    while i < args.len() {
      let arg = args[i].as_str();
      i += 1;
      if only_positional || !arg.starts_with('-') || arg == "-" {
        self.positional.push(arg.to_string());
        continue;
      }
      if arg == "--" {
        only_positional = true;
        continue;
      }
      if arg == "--help" || arg == "-h" || arg == "-?" {
        crate::ic_printf!("{}", self.help_text());
        return Err(IcError::new(err::IC_ERROR_HELP_REQUESTED));
      }
      let split: (&str, Option<&str>) = match arg.find('=') {
        Some(pos) => (&arg[..pos], Some(&arg[pos + 1..])),
        None => (arg, None),
      };
      let name_part = split.0;
      let inline_value = split.1;
      let index = if let Some(long) = name_part.strip_prefix("--") {
        self.find_long(long)
      } else {
        let short = name_part.as_bytes();
        if short.len() == 2 {
          self.find_short(short[1])
        } else {
          None
        }
      };
      let index = match index {
        Some(ix) => ix,
        None => {
          crate::ic_printf!("Unknown option {}", name_part);
          return Err(IcError::new(err::IC_ERROR_UNKNOWN_OPTION));
        }
      };
      let kind = self.entries[index].kind;
      let mut value_text: Option<String> = inline_value.map(|v| v.to_string());
      let needs_value =
        value_text.is_none() && kind != OptionKind::Flag && i < args.len();
      if needs_value {
        value_text = Some(args[i].clone());
        i += 1;
      }
      self.set_value(index, value_text.as_deref())?;
    }
    Ok(())
  }

  /// Parse the process's own command line.
  pub fn parse_env_args(&mut self) -> Result<(), IcError> {
    let mut args: Vec<String> = Vec::new();
    let mut first = true;
    for arg in std::env::args() {
      if first {
        first = false;
        continue;
      }
      args.push(arg);
    }
    self.parse(&args)
  }

  /// True if the option was given.
  pub fn is_set(&self, long_name: &str) -> bool {
    match self.find_long(long_name) {
      Some(ix) => self.values[ix].is_set,
      None => false,
    }
  }

  /// Value of an `Int` option, `None` if not given.
  pub fn get_int(&self, long_name: &str) -> Option<i64> {
    match self.find_long(long_name) {
      Some(ix) => {
        if self.values[ix].is_set {
          Some(self.values[ix].int_value)
        } else {
          None
        }
      }
      None => None,
    }
  }

  /// Value of an `Int` option, or the default if not given.
  pub fn get_int_or(&self, long_name: &str, default: i64) -> i64 {
    match self.get_int(long_name) {
      Some(v) => v,
      None => default,
    }
  }

  /// Value of a `Str` option, `None` if not given.
  pub fn get_str(&self, long_name: &str) -> Option<&str> {
    match self.find_long(long_name) {
      Some(ix) => {
        if self.values[ix].is_set {
          Some(self.values[ix].str_value.as_str())
        } else {
          None
        }
      }
      None => None,
    }
  }

  /// Value of a `Str` option, or the default if not given.
  pub fn get_string_or(&self, long_name: &str, default: &str) -> String {
    match self.get_str(long_name) {
      Some(v) => v.to_string(),
      None => default.to_string(),
    }
  }

  /// Value of a `Flag` option; false if not given.
  pub fn get_flag(&self, long_name: &str) -> bool {
    match self.find_long(long_name) {
      Some(ix) => self.values[ix].is_set && self.values[ix].flag_value,
      None => false,
    }
  }

  /// Arguments that were not options, in order.
  pub fn positional(&self) -> &[String] {
    &self.positional
  }

  /// The help text: usage line, description, one line per option.
  pub fn help_text(&self) -> String {
    let mut text = String::new();
    text.push_str("Usage: ");
    text.push_str(&self.program_name);
    text.push_str(" [OPTION...]\n");
    if !self.description.is_empty() {
      text.push_str(&self.description);
      text.push('\n');
    }
    text.push_str("\nOptions:\n");
    text.push_str("  -h, --help                       Show this help\n");
    for entry in &self.entries {
      let mut line = String::from("  ");
      if entry.short_name != 0 {
        line.push('-');
        line.push(entry.short_name as char);
        line.push_str(", ");
      } else {
        line.push_str("    ");
      }
      line.push_str("--");
      line.push_str(entry.long_name);
      match entry.kind {
        OptionKind::Flag => {}
        OptionKind::Int => line.push_str("=N"),
        OptionKind::Str => line.push_str("=STRING"),
      }
      while line.len() < 35 {
        line.push(' ');
      }
      line.push_str(entry.help);
      line.push('\n');
      text.push_str(&line);
    }
    text
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  const ENTRIES: [OptionEntry; 4] = [
    OptionEntry {
      long_name: "ndb-connectstring",
      short_name: b'c',
      kind: OptionKind::Str,
      help: "Connect string to ndb_mgmd",
    },
    OptionEntry {
      long_name: "node-id",
      short_name: 0,
      kind: OptionKind::Int,
      help: "Node id of this API node",
    },
    OptionEntry {
      long_name: "num-threads",
      short_name: b't',
      kind: OptionKind::Int,
      help: "Number of user threads",
    },
    OptionEntry {
      long_name: "daemonize",
      short_name: 0,
      kind: OptionKind::Flag,
      help: "Run as a daemon",
    },
  ];

  fn args(list: &[&str]) -> Vec<String> {
    let mut v: Vec<String> = Vec::new();
    for s in list {
      v.push(s.to_string());
    }
    v
  }

  #[test]
  fn parses_all_forms() {
    let mut p = OptionParser::new("test", "A test program");
    p.add_entries(&ENTRIES);
    let a = args(&[
      "--ndb-connectstring=localhost:1186",
      "--node-id",
      "68",
      "-t",
      "4k",
      "--daemonize",
      "--",
      "--not-an-option",
      "file.txt",
    ]);
    p.parse(&a).expect("parse");
    assert_eq!(p.get_str("ndb-connectstring"), Some("localhost:1186"));
    assert_eq!(p.get_int("node-id"), Some(68));
    assert_eq!(p.get_int("num-threads"), Some(4096));
    assert!(p.get_flag("daemonize"));
    assert_eq!(p.positional(), &["--not-an-option", "file.txt"]);
    assert_eq!(p.get_int_or("missing", 7), 7);
    assert_eq!(p.get_string_or("missing", "x"), "x");
    assert!(!p.is_set("missing"));
  }

  #[test]
  fn short_with_equals_and_flag_off() {
    let mut p = OptionParser::new("test", "");
    p.add_entries(&ENTRIES);
    p.parse(&args(&["-c=h:1", "--daemonize=0"])).expect("parse");
    assert_eq!(p.get_str("ndb-connectstring"), Some("h:1"));
    assert!(p.is_set("daemonize"));
    assert!(!p.get_flag("daemonize"));
  }

  #[test]
  fn errors() {
    let mut p = OptionParser::new("test", "");
    p.add_entries(&ENTRIES);
    let e = p.parse(&args(&["--bogus"]));
    assert_eq!(e, Err(IcError::new(err::IC_ERROR_UNKNOWN_OPTION)));
    let mut p = OptionParser::new("test", "");
    p.add_entries(&ENTRIES);
    let e = p.parse(&args(&["--node-id", "abc"]));
    assert_eq!(e, Err(IcError::new(err::IC_ERROR_OPTION_VALUE)));
    let mut p = OptionParser::new("test", "");
    p.add_entries(&ENTRIES);
    let e = p.parse(&args(&["--node-id"]));
    assert_eq!(e, Err(IcError::new(err::IC_ERROR_OPTION_VALUE)));
    let mut p = OptionParser::new("test", "");
    p.add_entries(&ENTRIES);
    let e = p.parse(&args(&["--help"]));
    assert_eq!(e, Err(IcError::new(err::IC_ERROR_HELP_REQUESTED)));
    assert!(p.help_text().contains("--node-id=N"));
  }

  #[test]
  fn int_suffixes() {
    assert_eq!(parse_int("10"), Some(10));
    assert_eq!(parse_int("-3"), Some(-3));
    assert_eq!(parse_int("2k"), Some(2048));
    assert_eq!(parse_int("1M"), Some(1024 * 1024));
    assert_eq!(parse_int("1g"), Some(1024 * 1024 * 1024));
    assert_eq!(parse_int(""), None);
    assert_eq!(parse_int("x"), None);
  }
}
