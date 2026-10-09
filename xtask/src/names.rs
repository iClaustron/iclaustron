// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `cargo xtask names`: no RonDB name in our code or docs (chapter 09,
//! "Names", decided by the author 2026-10-08).
//!
//! The names are read from a RonDB tree when the check runs, never
//! stored here: the names of signal and block numbers, the types that
//! lay out signals, the dictionary's property keys, the configuration's
//! constants and parameter names, the reference API's classes, and the
//! programs the tree builds. Each is looked for as a whole word in
//! `crates/`, `tools/`, `xtask/`, `include/`, `doc/` and `README.md`,
//! and behind our `IC_SIG_` and `IC_BLOCK_` prefixes, where an old name
//! would hide. A word shaped like the name of a RonDB signal handler,
//! `exec` followed by capitals, is reported too.
//!
//! Chapter 05 alone may cite RonDB, by a bare path and line; such
//! paths are skipped there and reported in any other file. Words that
//! are ordinary English or Rust, or that travel on the wire, are listed
//! with their reasons in `xtask/names-allow.txt`.
//!
//! ```text
//!   IC_RONDB_TREE=/path/to/rondb cargo xtask names
//!   cargo xtask names /path/to/rondb
//! ```

use std::collections::HashMap;
use std::collections::HashSet;
use std::fs;
use std::path::Path;

/// Where the allow-list lives.
const ALLOW_PATH: &str = "xtask/names-allow.txt";
/// The one file that may cite RonDB.
const CITING_FILE: &str = "doc/rust/05-ndb-protocol.md";
/// What is searched.
const SCOPE: &[&str] =
  &["crates", "tools", "xtask", "include", "doc", "README.md"];
/// File kinds searched.
const EXTENSIONS: &[&str] =
  &[".rs", ".md", ".html", ".h", ".toml", ".json", ".txt"];
/// RonDB's own sources, relative to its tree; files elsewhere in the
/// tree, such as bundled libraries, are not RonDB's.
const SOURCE_DIRS: &[&str] = &["storage/ndb", "strings", "sql", "sql-common"];
/// The one file allowed to name the RonDB files it reads.
const READER_FILE: &str = "xtask/src/names.rs";
/// Endings of RonDB source files in a citation.
const SOURCE_ENDINGS: &[&str] = &[".cpp", ".hpp", ".h", ".cc", ".yy"];

/// The names to look for, each with what kind of name it is.
struct RonDbNames {
  names: HashMap<String, &'static str>,
  /// Base names of RonDB's source files, to tell its citations from
  /// mentions of our own files.
  files: HashSet<String>,
}

pub fn run_names(rest: &[String]) -> i32 {
  let tree = match tree_from(rest) {
    Some(tree) => tree,
    None => {
      eprintln!(
        "cargo xtask names needs a RonDB tree: set IC_RONDB_TREE or give \
         its path"
      );
      return 2;
    }
  };
  let ron = match read_rondb_names(&tree) {
    Some(ron) => ron,
    None => {
      eprintln!("{} does not look like a RonDB tree", tree);
      return 2;
    }
  };
  let allow = read_allow(ALLOW_PATH);
  let mut files: Vec<String> = Vec::new();
  for top in SCOPE {
    collect(Path::new(top), &mut files);
  }
  files.sort();
  let mut found: usize = 0;
  for file in &files {
    if file == ALLOW_PATH {
      continue;
    }
    let text = match fs::read_to_string(file) {
      Ok(text) => text,
      Err(_) => continue,
    };
    found += check_file(file, &text, &ron, &allow);
  }
  if found > 0 {
    println!("names: {} RonDB name(s) found", found);
    return 1;
  }
  println!(
    "names: clean ({} names from the RonDB tree, {} files)",
    ron.names.len(),
    files.len()
  );
  0
}

fn tree_from(rest: &[String]) -> Option<String> {
  if let Some(first) = rest.first() {
    return Some(first.clone());
  }
  std::env::var("IC_RONDB_TREE").ok()
}

// ---- Reading the names from the RonDB tree ----

fn read_rondb_names(tree: &str) -> Option<RonDbNames> {
  let ndb = format!("{}/storage/ndb", tree);
  let signals = format!("{}/include/kernel/GlobalSignalNumbers.h", ndb);
  if !Path::new(&signals).is_file() {
    return None;
  }
  let mut ron = RonDbNames {
    names: HashMap::new(),
    files: HashSet::new(),
  };
  read_signal_names(&signals, &mut ron);
  let blocks = format!("{}/include/kernel/BlockNumbers.h", ndb);
  read_block_names(&blocks, &mut ron);
  let layouts = format!("{}/include/kernel/signaldata", ndb);
  read_type_names(&layouts, "type", &mut ron);
  read_type_names(&format!("{}/include/ndbapi", ndb), "class", &mut ron);
  read_dict_keys(
    &format!("{}/include/kernel/signaldata/DictTabInfo.hpp", ndb),
    &mut ron,
  );
  read_config_constants(
    &format!("{}/include/mgmapi/mgmapi_config_parameters.h", ndb),
    &mut ron,
  );
  read_config_names(
    &format!("{}/src/common/mgmcommon/ConfigInfo.cpp", ndb),
    &mut ron,
  );
  read_programs(Path::new(&ndb), &mut ron);
  for dir in SOURCE_DIRS {
    let path = format!("{}/{}", tree, dir);
    read_source_files(Path::new(&path), &mut ron, 0);
  }
  Some(ron)
}

fn add(ron: &mut RonDbNames, name: &str, kind: &'static str) {
  let distinct_program = kind == "program" && is_program_name(name);
  if distinct_program || is_specific(name) {
    ron.names.insert(name.to_string(), kind);
  }
}

/// RonDB builds test programs with ordinary names (`client`, `parse`);
/// a program name counts when it is RonDB's own (`ndb...`) or of mixed
/// case.
fn is_program_name(name: &str) -> bool {
  if name.starts_with("ndb") {
    return true;
  }
  let mut upper = false;
  let mut lower = false;
  for c in name.chars() {
    if c.is_ascii_uppercase() {
      upper = true;
    } else if c.is_ascii_lowercase() {
      lower = true;
    }
  }
  upper && lower
}

/// A name specific enough that finding it means it was taken: one with
/// an underscore, a word of five or more capitals, or a word of mixed
/// case with two or more capitals. Single words such as `Name` are
/// ordinary English and are not looked for.
fn is_specific(name: &str) -> bool {
  if name.len() < 4 {
    return false;
  }
  let mut upper: usize = 0;
  let mut lower: usize = 0;
  let mut underscore = false;
  for c in name.chars() {
    if c.is_ascii_uppercase() {
      upper += 1;
    } else if c.is_ascii_lowercase() {
      lower += 1;
    } else if c == '_' {
      underscore = true;
    }
  }
  if underscore {
    return upper > 0 || lower > 0;
  }
  if lower == 0 {
    return upper >= 5;
  }
  upper >= 2
}

/// `#define GSN_<NAME> <number>`
fn read_signal_names(path: &str, ron: &mut RonDbNames) {
  let text = fs::read_to_string(path).unwrap_or_default();
  for line in text.lines() {
    let mut words = line.split_whitespace();
    if words.next() != Some("#define") {
      continue;
    }
    if let Some(name) = words.next() {
      if let Some(signal) = name.strip_prefix("GSN_") {
        add(ron, signal, "signal");
        add(ron, name, "signal");
      }
    }
  }
}

/// `#define <NAME> <number or expression>`
fn read_block_names(path: &str, ron: &mut RonDbNames) {
  let text = fs::read_to_string(path).unwrap_or_default();
  for line in text.lines() {
    let words: Vec<&str> = line.split_whitespace().collect();
    if words.len() < 3 || words[0] != "#define" {
      continue;
    }
    let value = words[2];
    let numeric = value.starts_with("0x")
      || value.starts_with('(')
      || first_is_digit(value);
    if numeric && is_upper_name(words[1]) {
      add(ron, words[1], "block");
    }
  }
}

fn is_upper_name(name: &str) -> bool {
  for c in name.chars() {
    if !(c.is_ascii_uppercase() || c.is_ascii_digit() || c == '_') {
      return false;
    }
  }
  !name.is_empty()
}

/// `struct <Name>` and `class <Name>` at the start of a line.
fn read_type_names(dir: &str, kind: &'static str, ron: &mut RonDbNames) {
  let entries = match fs::read_dir(dir) {
    Ok(entries) => entries,
    Err(_) => return,
  };
  for entry in entries.flatten() {
    let path = entry.path();
    let text = fs::read_to_string(&path).unwrap_or_default();
    for line in text.lines() {
      let rest = match line.strip_prefix("struct ") {
        Some(rest) => rest,
        None => match line.strip_prefix("class ") {
          Some(rest) => rest,
          None => continue,
        },
      };
      let name = leading_word(rest);
      if first_is_upper(name) {
        add(ron, name, kind);
      }
    }
  }
}

/// `<Name> = <number>,` inside the dictionary's key list.
fn read_dict_keys(path: &str, ron: &mut RonDbNames) {
  let text = fs::read_to_string(path).unwrap_or_default();
  for line in text.lines() {
    let trimmed = line.trim();
    let (name, value) = match trimmed.split_once('=') {
      Some(parts) => parts,
      None => continue,
    };
    let name = name.trim();
    let value = value.trim();
    if !value.ends_with(',') {
      continue;
    }
    let number = &value[..value.len() - 1];
    if number.trim().parse::<u32>().is_err() {
      continue;
    }
    if leading_word(name) == name && first_is_upper(name) {
      add(ron, name, "dictionary key");
    }
  }
}

/// `#define CFG_<NAME> <number>`
fn read_config_constants(path: &str, ron: &mut RonDbNames) {
  let text = fs::read_to_string(path).unwrap_or_default();
  for line in text.lines() {
    let mut words = line.split_whitespace();
    if words.next() != Some("#define") {
      continue;
    }
    if let Some(name) = words.next() {
      if name.starts_with("CFG_") {
        add(ron, name, "configuration constant");
      }
    }
  }
}

/// The quoted name after a `CFG_` constant in the parameter table.
fn read_config_names(path: &str, ron: &mut RonDbNames) {
  let text = fs::read_to_string(path).unwrap_or_default();
  for line in text.lines() {
    let at = match line.find("CFG_") {
      Some(at) => at,
      None => continue,
    };
    let after = &line[at..];
    let open = match after.find('"') {
      Some(open) => open,
      None => continue,
    };
    let quoted = &after[open + 1..];
    let close = match quoted.find('"') {
      Some(close) => close,
      None => continue,
    };
    let name = &quoted[..close];
    if leading_word(name) == name {
      add(ron, name, "configuration parameter");
    }
  }
}

/// Programs: the first word after `ADD_EXECUTABLE(` in a build file.
fn read_programs(dir: &Path, ron: &mut RonDbNames) {
  let entries = match fs::read_dir(dir) {
    Ok(entries) => entries,
    Err(_) => return,
  };
  for entry in entries.flatten() {
    let path = entry.path();
    if path.is_dir() {
      read_programs(&path, ron);
      continue;
    }
    if base_name(&path) != Some("CMakeLists.txt") {
      continue;
    }
    let text = fs::read_to_string(&path).unwrap_or_default();
    for line in text.lines() {
      let upper = line.to_ascii_uppercase();
      let at = match upper.find("ADD_EXECUTABLE(") {
        Some(at) => at,
        None => continue,
      };
      let rest = line[at + "ADD_EXECUTABLE(".len()..].trim_start();
      let name = leading_word(rest);
      if name.len() >= 4 && !name.contains('$') {
        add(ron, name, "program");
      }
    }
  }
}

/// Base names of the tree's C and C++ sources and headers.
fn read_source_files(dir: &Path, ron: &mut RonDbNames, depth: u32) {
  if depth > 12 {
    return;
  }
  let entries = match fs::read_dir(dir) {
    Ok(entries) => entries,
    Err(_) => return,
  };
  for entry in entries.flatten() {
    let path = entry.path();
    let name = match base_name(&path) {
      Some(name) => name.to_string(),
      None => continue,
    };
    if path.is_dir() {
      if name != ".git" {
        read_source_files(&path, ron, depth + 1);
      }
      continue;
    }
    if has_source_ending(&name) {
      ron.files.insert(name);
    }
  }
}

fn base_name(path: &Path) -> Option<&str> {
  match path.file_name() {
    Some(name) => name.to_str(),
    None => None,
  }
}

fn first_is_digit(text: &str) -> bool {
  match text.chars().next() {
    Some(c) => c.is_ascii_digit(),
    None => false,
  }
}

fn first_is_upper(text: &str) -> bool {
  match text.chars().next() {
    Some(c) => c.is_ascii_uppercase(),
    None => false,
  }
}

fn has_source_ending(name: &str) -> bool {
  for ending in SOURCE_ENDINGS {
    if name.ends_with(ending) {
      return true;
    }
  }
  false
}

/// The leading run of letters, digits and underscores.
fn leading_word(text: &str) -> &str {
  let mut end: usize = 0;
  for (i, c) in text.char_indices() {
    if c.is_ascii_alphanumeric() || c == '_' {
      end = i + c.len_utf8();
    } else {
      break;
    }
  }
  &text[..end]
}

// ---- The allow-list ----

/// The allow-list: `word [file ...]  # reason`, one per line; `#`
/// lines are comments. A word with files is allowed in those files
/// only, a word without anywhere.
struct Allow {
  anywhere: HashSet<String>,
  in_files: HashMap<String, Vec<String>>,
}

fn read_allow(path: &str) -> Allow {
  let mut allow = Allow {
    anywhere: HashSet::new(),
    in_files: HashMap::new(),
  };
  let text = fs::read_to_string(path).unwrap_or_default();
  for line in text.lines() {
    let entry = match line.split_once('#') {
      Some((entry, _reason)) => entry.trim(),
      None => line.trim(),
    };
    let mut words = entry.split_whitespace();
    let word = match words.next() {
      Some(word) => word.to_string(),
      None => continue,
    };
    let mut files: Vec<String> = Vec::new();
    for file in words {
      files.push(file.to_string());
    }
    if files.is_empty() {
      allow.anywhere.insert(word);
    } else {
      allow.in_files.entry(word).or_default().extend(files);
    }
  }
  allow
}

fn is_allowed(allow: &Allow, word: &str, file: &str) -> bool {
  if allow.anywhere.contains(word) {
    return true;
  }
  match allow.in_files.get(word) {
    Some(files) => files.iter().any(|f| f == file),
    None => false,
  }
}

// ---- Searching our tree ----

fn collect(path: &Path, out: &mut Vec<String>) {
  if path.is_file() {
    if let Some(name) = path.to_str() {
      out.push(name.to_string());
    }
    return;
  }
  let entries = match fs::read_dir(path) {
    Ok(entries) => entries,
    Err(_) => return,
  };
  for entry in entries.flatten() {
    let child = entry.path();
    let name = match child.to_str() {
      Some(name) => name.to_string(),
      None => continue,
    };
    if child.is_dir() {
      if !name.ends_with("/target") {
        collect(&child, out);
      }
      continue;
    }
    for ext in EXTENSIONS {
      if name.ends_with(ext) {
        out.push(name.clone());
        break;
      }
    }
  }
}

/// Reports what one file holds; returns how many it reported.
fn check_file(
  file: &str,
  text: &str,
  ron: &RonDbNames,
  allow: &Allow,
) -> usize {
  let may_cite = file == CITING_FILE || file == READER_FILE;
  let markdown = file.ends_with(".md");
  let mut found: usize = 0;
  let mut in_kept_block = false;
  let mut number: usize = 0;
  for line in text.lines() {
    number += 1;
    if markdown && line.trim_start().starts_with("```") {
      // Blocks of operator commands and RonDB's own configuration files
      // speak RonDB's language because RonDB reads them.
      let info = line.trim_start().trim_start_matches('`').trim();
      if in_kept_block {
        in_kept_block = false;
      } else {
        in_kept_block = is_kept_block(info);
      }
      continue;
    }
    if in_kept_block {
      continue;
    }
    let mut words: Vec<&str> = Vec::new();
    split_words(line, &mut words);
    for word in &words {
      if is_citation(word) {
        if !may_cite && cites_rondb(word, ron) {
          println!(
            "{}:{}: citation `{}` outside chapter 05",
            file, number, word
          );
          found += 1;
        }
        continue;
      }
      found += check_word(file, number, word, ron, allow);
    }
  }
  found
}

fn is_kept_block(info: &str) -> bool {
  info == "ini"
    || info == "sh"
    || info == "bash"
    || info == "shell"
    || info == "console"
}

/// Splits a line into words of letters, digits, `_`, `.`, `/`, `:`,
/// `-` and `,` so that a path with its lines stays whole.
fn split_words<'a>(line: &'a str, out: &mut Vec<&'a str>) {
  let mut start: Option<usize> = None;
  for (i, c) in line.char_indices() {
    let part = c.is_ascii_alphanumeric()
      || c == '_'
      || c == '.'
      || c == '/'
      || c == ':'
      || c == '-'
      || c == '$';
    match (part, start) {
      (true, None) => start = Some(i),
      (false, Some(s)) => {
        out.push(&line[s..i]);
        start = None;
      }
      _ => {}
    }
  }
  if let Some(s) = start {
    out.push(&line[s..]);
  }
}

/// A word that names a source file, with or without its lines.
fn is_citation(word: &str) -> bool {
  let path = match word.split_once(':') {
    Some((path, _lines)) => path,
    None => word,
  };
  let path = path.trim_end_matches('.');
  has_source_ending(path)
}

/// True if the cited file is RonDB's rather than ours.
fn cites_rondb(word: &str, ron: &RonDbNames) -> bool {
  let path = match word.split_once(':') {
    Some((path, _lines)) => path,
    None => word,
  };
  let path = path.trim_end_matches('.');
  let base = match path.rsplit_once('/') {
    Some((_dir, base)) => base,
    None => path,
  };
  ron.files.contains(base)
}

/// Checks the identifiers inside one word; returns how many it reported.
fn check_word(
  file: &str,
  number: usize,
  word: &str,
  ron: &RonDbNames,
  allow: &Allow,
) -> usize {
  let mut found: usize = 0;
  let mut start: Option<usize> = None;
  let bytes = word.as_bytes();
  let mut i: usize = 0;
  while i <= bytes.len() {
    let part =
      i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_');
    match (part, start) {
      (true, None) => start = Some(i),
      (false, Some(s)) => {
        found += check_identifier(file, number, &word[s..i], ron, allow);
        start = None;
      }
      _ => {}
    }
    i += 1;
  }
  found
}

fn check_identifier(
  file: &str,
  number: usize,
  ident: &str,
  ron: &RonDbNames,
  allow: &Allow,
) -> usize {
  if is_allowed(allow, ident, file) {
    return 0;
  }
  let mut candidates: Vec<&str> = vec![ident];
  for prefix in ["IC_SIG_", "IC_BLOCK_", "IC_GSN_"] {
    if let Some(rest) = ident.strip_prefix(prefix) {
      candidates.push(rest);
    }
  }
  for candidate in &candidates {
    if is_allowed(allow, candidate, file) {
      continue;
    }
    if let Some(kind) = ron.names.get(*candidate) {
      println!("{}:{}: `{}` is a RonDB {} name", file, number, ident, kind);
      return 1;
    }
  }
  if is_handler_name(ident) {
    println!(
      "{}:{}: `{}` is named like a RonDB signal handler",
      file, number, ident
    );
    return 1;
  }
  0
}

/// `exec` followed by at least three capitals or underscores.
fn is_handler_name(ident: &str) -> bool {
  let rest = match ident.strip_prefix("exec") {
    Some(rest) => rest,
    None => return false,
  };
  let mut capitals: usize = 0;
  for c in rest.chars() {
    if c.is_ascii_uppercase() || c == '_' || c.is_ascii_digit() {
      capitals += 1;
    } else {
      return false;
    }
  }
  capitals >= 3
}
