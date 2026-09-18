// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! `cargo xtask <command>`: developer commands, run by the developer.
//!
//! Commands:
//!
//! - `check`: fmt --check, clippy -D warnings, test, style.
//! - `style`: check the crates for lines wider than 80 columns and for
//!   constructs forbidden by doc/rust/07-style-guide.md.
//! - `test-integration`: run the ic_apid integration tests against the
//!   cluster named by IC_TEST_CONNECTSTRING (or the first argument).
//! - `header`: regenerate the C header with cbindgen into target/ and
//!   diff it against include/iclaustron/ic_apid.h.
//! - `tags`: build a vim `tags` file over the Rust and C sources with
//!   Universal Ctags (the successor of legacy-c/git_tags.sh);
//!   `--no-legacy` leaves legacy-c/ out.
//!
//! Written in the same C-like style as the library.

use std::fs;
use std::io::Write;
use std::path::Path;
use std::process::Command;
use std::process::Stdio;

fn main() {
  let args: Vec<String> = std::env::args().collect();
  let cmd: &str = if args.len() > 1 {
    args[1].as_str()
  } else {
    "help"
  };
  let rest: &[String] = if args.len() > 2 { &args[2..] } else { &[] };
  let code: i32 = match cmd {
    "check" => run_check(),
    "style" => run_style(),
    "test-integration" => run_test_integration(rest),
    "header" => run_header(),
    "tags" => run_tags(rest),
    _ => {
      print_help();
      0
    }
  };
  std::process::exit(code);
}

fn print_help() {
  println!("cargo xtask <command>");
  println!("  check             fmt --check, clippy, test, style");
  println!("  style             check for forbidden Rust constructs");
  println!("  test-integration  [connectstring] run tests against a cluster");
  println!("  header            regenerate and diff the C header");
  println!("  tags [--no-legacy] build a vim tags file with Universal Ctags");
}

/// Run a program with arguments in the workspace root, return its exit code.
fn run(program: &str, args: &[&str]) -> i32 {
  println!("+ {} {}", program, args.join(" "));
  let status = Command::new(program).args(args).status();
  match status {
    Ok(s) => s.code().unwrap_or(1),
    Err(e) => {
      eprintln!("failed to start {}: {}", program, e);
      1
    }
  }
}

fn run_check() -> i32 {
  let mut code: i32;
  code = run("cargo", &["fmt", "--all", "--check"]);
  if code != 0 {
    return code;
  }
  code = run(
    "cargo",
    &[
      "clippy",
      "--workspace",
      "--all-targets",
      "--",
      "-D",
      "warnings",
    ],
  );
  if code != 0 {
    return code;
  }
  code = run("cargo", &["test", "--workspace"]);
  if code != 0 {
    return code;
  }
  run_style()
}

/// Widest line the style guide allows. rustfmt keeps code within this,
/// but it never reflows comments or string literals, so those are only
/// caught here.
const MAX_WIDTH: usize = 80;

/// Forbidden constructs: (pattern, explanation). Substring match per line.
const FORBIDDEN: &[(&str, &str)] = &[
  ("async fn", "no async; explicit threads and poll"),
  (".await", "no async; explicit threads and poll"),
  ("Rc<", "no Rc; use Box or Arc at the documented places"),
  ("RefCell<", "no RefCell; use &mut"),
  ("Cell<", "no Cell; use &mut or an atomic"),
  (" where ", "no where clauses; write the concrete type"),
  ("impl Trait", "no impl Trait in signatures"),
  (
    ".iter().map(",
    "no iterator adaptor chains; write a for loop",
  ),
  (
    ".iter().filter(",
    "no iterator adaptor chains; write a for loop",
  ),
  ("macro_rules!", "no user macros beyond the sanctioned few"),
  (".unwrap()", "no unwrap outside tests; return IcError"),
];

/// This file holds the FORBIDDEN table, so every pattern appears in it
/// as a string literal. Its width is checked like any other file, but it
/// is skipped when looking for forbidden constructs.
const SELF_PATH: &str = "xtask/src/main.rs";

/// The part of a line that is code: everything before a `//` comment.
fn code_part(line: &str) -> &str {
  match line.find("//") {
    Some(pos) => &line[..pos],
    None => line,
  }
}

/// True if `text` contains `pattern` other than as the tail of a longer
/// identifier, so that `Cell<` does not match inside `RefCell<`.
fn contains_token(text: &str, pattern: &str) -> bool {
  let bytes = text.as_bytes();
  let mut from: usize = 0;
  loop {
    let found = match text[from..].find(pattern) {
      Some(pos) => from + pos,
      None => return false,
    };
    if found == 0 {
      return true;
    }
    let prev = bytes[found - 1];
    let part_of_name = prev.is_ascii_alphanumeric() || prev == b'_';
    if !part_of_name {
      return true;
    }
    from = found + 1;
  }
}

fn run_style() -> i32 {
  let allow = read_allow_list("xtask/style-allow.txt");
  let mut wide: u32 = 0;
  let mut forbidden: u32 = 0;
  let mut files: Vec<String> = Vec::new();
  collect_rs_files(Path::new("crates"), &mut files);
  collect_rs_files(Path::new("xtask"), &mut files);
  for file in &files {
    let text = match fs::read_to_string(file) {
      Ok(t) => t,
      Err(_) => continue,
    };
    let is_self = file.ends_with(SELF_PATH);
    let mut in_tests = false;
    let mut line_no: u32 = 0;
    for line in text.lines() {
      line_no += 1;
      // Width applies everywhere, comments and tests included.
      let width = line.chars().count();
      if width > MAX_WIDTH {
        println!(
          "{}:{}: {} columns, limit {}",
          file, line_no, width, MAX_WIDTH
        );
        wide += 1;
      }
      if line.contains("#[cfg(test)]") {
        in_tests = true;
      }
      if in_tests || is_self {
        continue;
      }
      // Only code counts; prose in comments may say anything.
      let code = code_part(line);
      for (pattern, why) in FORBIDDEN {
        if !contains_token(code, pattern) {
          continue;
        }
        if is_allowed(&allow, file, pattern) {
          continue;
        }
        println!("{}:{}: `{}` — {}", file, line_no, pattern, why);
        forbidden += 1;
      }
    }
  }
  if wide + forbidden == 0 {
    println!("style: ok ({} files)", files.len());
    return 0;
  }
  if wide > 0 {
    println!("style: {} line(s) over {} columns", wide, MAX_WIDTH);
    println!("  rustfmt leaves comments and strings alone;");
    println!("  wrap those by hand");
  }
  if forbidden > 0 {
    println!("style: {} forbidden construct(s)", forbidden);
  }
  1
}

fn read_allow_list(path: &str) -> Vec<(String, String)> {
  let mut list: Vec<(String, String)> = Vec::new();
  let text = match fs::read_to_string(path) {
    Ok(t) => t,
    Err(_) => return list,
  };
  for line in text.lines() {
    if line.starts_with('#') || line.trim().is_empty() {
      continue;
    }
    if let Some(pos) = line.find(':') {
      let path_part = line[..pos].to_string();
      let pattern_part = line[pos + 1..].to_string();
      list.push((path_part, pattern_part));
    }
  }
  list
}

fn is_allowed(allow: &[(String, String)], file: &str, pattern: &str) -> bool {
  for (path_part, pattern_part) in allow {
    if file.contains(path_part.as_str()) && pattern_part == pattern {
      return true;
    }
  }
  false
}

fn collect_rs_files(dir: &Path, out: &mut Vec<String>) {
  let entries = match fs::read_dir(dir) {
    Ok(e) => e,
    Err(_) => return,
  };
  for entry in entries {
    let entry = match entry {
      Ok(e) => e,
      Err(_) => continue,
    };
    let path = entry.path();
    if path.is_dir() {
      collect_rs_files(&path, out);
    } else if path.extension().map(|e| e == "rs").unwrap_or(false) {
      out.push(path.to_string_lossy().to_string());
    }
  }
}

fn run_test_integration(rest: &[String]) -> i32 {
  let mut connectstring: String = String::new();
  if !rest.is_empty() {
    connectstring = rest[0].clone();
  } else if let Ok(v) = std::env::var("IC_TEST_CONNECTSTRING") {
    connectstring = v;
  }
  if connectstring.is_empty() {
    eprintln!("no connectstring given");
    eprintln!("pass it as argument or set IC_TEST_CONNECTSTRING");
    eprintln!("see doc/dev-cluster.md");
    return 2;
  }
  std::env::set_var("IC_TEST_CONNECTSTRING", &connectstring);
  let mut args: Vec<&str> = vec![
    "test",
    "-p",
    "ic_apid",
    "--features",
    "integration",
    "--",
    "--test-threads=1",
  ];
  if rest.len() > 1 {
    args.push(rest[1].as_str());
  }
  run("cargo", &args)
}

fn run_header() -> i32 {
  let generated = "target/ic_apid.h";
  let checked_in = "include/iclaustron/ic_apid.h";
  let _ = fs::create_dir_all("target");
  let code = run(
    "cbindgen",
    &[
      "--config",
      "crates/ic_capi/cbindgen.toml",
      "--crate",
      "ic_capi",
      "--output",
      generated,
    ],
  );
  if code != 0 {
    eprintln!("cbindgen failed (install with `cargo install cbindgen`)");
    return code;
  }
  run("diff", &["-u", checked_in, generated])
}

/// Source file extensions indexed by `tags`.
const TAG_EXTENSIONS: &[&str] =
  &[".rs", ".c", ".h", ".ic", ".y", ".hpp", ".cpp", ".cc"];

fn has_tag_extension(path: &str) -> bool {
  for ext in TAG_EXTENSIONS {
    if path.ends_with(ext) {
      return true;
    }
  }
  false
}

/// True if the `ctags` on PATH is Universal Ctags (the one with a Rust
/// parser). BSD and Exuberant ctags do not know Rust.
fn is_universal_ctags() -> bool {
  let output = Command::new("ctags").arg("--version").output();
  match output {
    Ok(o) => String::from_utf8_lossy(&o.stdout).contains("Universal Ctags"),
    Err(_) => false,
  }
}

/// Build `./tags` for vim over every source file git knows about (tracked
/// or new and not ignored), like legacy-c/git_tags.sh did for the C code.
fn run_tags(rest: &[String]) -> i32 {
  let mut include_legacy = true;
  for arg in rest {
    if arg == "--no-legacy" {
      include_legacy = false;
    }
  }
  if !is_universal_ctags() {
    eprintln!("tags: needs Universal Ctags on PATH");
    eprintln!("  macOS: brew install universal-ctags");
    eprintln!("         or sudo port install universal-ctags");
    eprintln!("  Linux: apt install universal-ctags");
    return 1;
  }
  let listing = Command::new("git")
    .args(["ls-files", "--cached", "--others", "--exclude-standard"])
    .output();
  let listing = match listing {
    Ok(o) => o,
    Err(e) => {
      eprintln!("tags: failed to run git: {}", e);
      return 1;
    }
  };
  let text = String::from_utf8_lossy(&listing.stdout);
  let mut files = String::new();
  let mut count: u32 = 0;
  for line in text.lines() {
    if !include_legacy && line.starts_with("legacy-c/") {
      continue;
    }
    if !has_tag_extension(line) {
      continue;
    }
    files.push_str(line);
    files.push('\n');
    count += 1;
  }
  // Rust, C and C++ parsers are picked from the file names; .h, .ic
  // and .y are treated as C as legacy-c/git_tags.sh did.
  let args: [&str; 12] = [
    "--map-C++=-.h",
    "--map-C=+.h",
    "--map-C=+.ic",
    "--map-C=+.y",
    "--kinds-C=+p",
    "--extras=+fq",
    "--fields=+n",
    "--sort=yes",
    "-L",
    "-",
    "-f",
    "tags",
  ];
  println!("+ ctags {} < ({} files)", args.join(" "), count);
  let child = Command::new("ctags")
    .args(args)
    .stdin(Stdio::piped())
    .spawn();
  let mut child = match child {
    Ok(c) => c,
    Err(e) => {
      eprintln!("tags: failed to start ctags: {}", e);
      return 1;
    }
  };
  if let Some(mut stdin) = child.stdin.take() {
    if let Err(e) = stdin.write_all(files.as_bytes()) {
      eprintln!("tags: failed to feed ctags: {}", e);
      return 1;
    }
  }
  match child.wait() {
    Ok(status) => {
      let code = status.code().unwrap_or(1);
      if code == 0 {
        println!("tags: {} files indexed into ./tags", count);
      }
      code
    }
    Err(e) => {
      eprintln!("tags: ctags failed: {}", e);
      1
    }
  }
}
