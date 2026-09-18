// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Debug tracing: `DEBUG_ENTRY`/`DEBUG_RETURN`/`DEBUG_PRINT` from
//! `legacy-c/include/ic_debug.h` and `legacy-c/util/ic_debug.c`.
//!
//! Each thread keeps an indent level and a stack of entry point names so
//! the output is pretty printed as a call tree. Output goes to a file
//! `debug_n<node>_p<pid>.log` and optionally to the screen. Levels are
//! bits; a message is printed when its level bit is set.
//!
//! The macros [`debug_entry!`] and [`debug_print!`] expand to code that
//! tests the constant [`DEBUG_BUILD`], which is true only with the
//! `debug_build` cargo feature; in a normal build the compiler removes
//! them completely, as the C preprocessor did.
//!
//! Rust notes for C readers: the per-thread state is a `thread_local!`
//! wrapped in `RefCell`, the standard way to get a mutable thread-local
//! in Rust; the C used `g_private_get`. The return-value printing of
//! `DEBUG_RETURN_INT` is replaced by an [`EntryGuard`] that prints the
//! exit when the function returns by any path.

use std::cell::RefCell;
use std::fmt;
use std::fs::File;
use std::io::Write;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::sync::Mutex;

use crate::consts::IC_MICROSEC_PER_SECOND;
use crate::time;

/// True when the crate is built with the `debug_build` feature.
pub const DEBUG_BUILD: bool = cfg!(feature = "debug_build");

/// Debug level bit: the program's own debugging.
pub const PROGRAM_LEVEL: u32 = 1;
/// Debug level bit: function entry and exit.
pub const ENTRY_LEVEL: u32 = 2;
/// Debug level bit: thread start and stop.
pub const THREAD_LEVEL: u32 = 4;
/// Debug level bit: configuration handling.
pub const CONFIG_LEVEL: u32 = 8;
/// Debug level bit: the port layer.
pub const PORT_LEVEL: u32 = 16;
/// Debug level bit: the management protocol.
pub const CONFIG_PROTO_LEVEL: u32 = 32;
/// Debug level bit: file operations.
pub const FILE_LEVEL: u32 = 64;
/// Debug level bit: communication.
pub const COMM_LEVEL: u32 = 128;
/// Debug level bit: reading of configuration.
pub const CONFIG_READ_LEVEL: u32 = 256;
/// Debug level bit: building the configuration hash.
pub const BUILD_CONFIG_HASH_LEVEL: u32 = 512;
/// Debug level bit: every NDB signal sent and received.
pub const NDB_MESSAGE_LEVEL: u32 = 1024;
/// Debug level bit: the adaptive send algorithm.
pub const ADAPTIVE_SEND_LEVEL: u32 = 2048;
/// Debug level bit: poll set checks.
pub const CHECK_POLL_SET_LEVEL: u32 = 4096;
/// Debug level bit: memory allocation.
pub const MALLOC_LEVEL: u32 = 8192;
/// Debug level bit: heartbeat handling.
pub const HEARTBEAT_LEVEL: u32 = 16384;
/// Debug level bit: communication details.
pub const COMM_DETAIL_LEVEL: u32 = 32768;
/// Debug level bit: node configuration lookups.
pub const FIND_NODE_CONFIG_LEVEL: u32 = 65536;
/// All debug level bits.
pub const ALL_DEBUG_LEVELS: u32 = 0xFFFF_FFFF;

/// Deepest call nesting the tracer keeps names for.
pub const IC_DEBUG_MAX_INDENT_LEVEL: usize = 128;

static LEVEL: AtomicU32 = AtomicU32::new(0);
static SCREEN: AtomicBool = AtomicBool::new(false);
static TIMESTAMP: AtomicBool = AtomicBool::new(false);
static ACTIVE: AtomicBool = AtomicBool::new(false);
static START_TIME: AtomicU64 = AtomicU64::new(0);
static FILE: Mutex<Option<File>> = Mutex::new(None);

struct ThreadIds {
  next_id: u32,
  free_ids: Vec<u32>,
  num_active: u32,
}

static THREAD_IDS: Mutex<ThreadIds> = Mutex::new(ThreadIds {
  next_id: 1,
  free_ids: Vec::new(),
  num_active: 0,
});

struct ThreadDebug {
  thread_id: u32,
  inited: bool,
  indent_level: usize,
  enabled: bool,
  save_enabled: bool,
  disable_count: u32,
  entry_points: [&'static str; IC_DEBUG_MAX_INDENT_LEVEL],
}

impl ThreadDebug {
  const fn new() -> ThreadDebug {
    ThreadDebug {
      thread_id: 0,
      inited: false,
      indent_level: 0,
      enabled: true,
      save_enabled: true,
      disable_count: 0,
      entry_points: [""; IC_DEBUG_MAX_INDENT_LEVEL],
    }
  }
}

thread_local! {
    static THREAD: RefCell<ThreadDebug> =
        const { RefCell::new(ThreadDebug::new()) };
}

fn lock_file() -> std::sync::MutexGuard<'static, Option<File>> {
  match FILE.lock() {
    Ok(g) => g,
    Err(poisoned) => poisoned.into_inner(),
  }
}

fn lock_thread_ids() -> std::sync::MutexGuard<'static, ThreadIds> {
  match THREAD_IDS.lock() {
    Ok(g) => g,
    Err(poisoned) => poisoned.into_inner(),
  }
}

/// Set the debug level bits.
pub fn set_level(level: u32) {
  LEVEL.store(level, Ordering::Relaxed);
}

/// The debug level bits.
pub fn get_level() -> u32 {
  LEVEL.load(Ordering::Relaxed)
}

/// True if any of the given level bits is set.
pub fn is_level(level: u32) -> bool {
  (LEVEL.load(Ordering::Relaxed) & level) != 0
}

/// Also print debug output on the screen (standard output).
pub fn set_screen(on: bool) {
  SCREEN.store(on, Ordering::Relaxed);
}

/// Prefix debug lines with seconds and microseconds since `open`.
pub fn set_timestamp(on: bool) {
  TIMESTAMP.store(on, Ordering::Relaxed);
}

/// True if the timestamp prefix is on.
pub fn get_timestamp() -> bool {
  TIMESTAMP.load(Ordering::Relaxed)
}

/// True between [`open`] and [`close`].
pub fn is_active() -> bool {
  ACTIVE.load(Ordering::Relaxed)
}

fn alloc_thread_id() -> u32 {
  let mut ids = lock_thread_ids();
  ids.num_active += 1;
  match ids.free_ids.pop() {
    Some(id) => id,
    None => {
      let id = ids.next_id;
      ids.next_id += 1;
      id
    }
  }
}

fn free_thread_id(id: u32) {
  let mut ids = lock_thread_ids();
  if ids.num_active > 0 {
    ids.num_active -= 1;
  }
  ids.free_ids.push(id);
}

/// Number of threads currently registered with the tracer.
pub fn num_threads_debugged() -> u32 {
  lock_thread_ids().num_active
}

/// Open the debug file `debug_n<node_id>_p<pid>.log` in the current
/// directory, record the start time and register the calling thread as
/// "main".
pub fn open(node_id: u32) -> Result<(), crate::IcError> {
  let pid = std::process::id();
  let name = format!("debug_n{}_p{}.log", node_id, pid);
  let file = match File::create(&name) {
    Ok(f) => f,
    Err(e) => {
      crate::ic_printf!("Failed to open {}", name);
      crate::output::flush_stdout();
      return Err(crate::IcError::from_io(&e));
    }
  };
  {
    let mut slot = lock_file();
    *slot = Some(file);
  }
  START_TIME.store(time::gethrtime(), Ordering::Relaxed);
  ACTIVE.store(true, Ordering::Relaxed);
  thread_init(Some("main"));
  Ok(())
}

/// Unregister the calling thread, flush and close the debug file.
pub fn close() {
  thread_return();
  crate::output::flush_stdout();
  {
    let mut slot = lock_file();
    if let Some(f) = slot.as_mut() {
      let _ = f.flush();
    }
    *slot = None;
  }
  ACTIVE.store(false, Ordering::Relaxed);
  let active = num_threads_debugged();
  if active != 0 {
    crate::ic_printf!("debug close: {} thread(s) still registered", active);
  }
}

fn print_line(text: &str, thread_id: u32, indent_level: usize) {
  let mut line = String::new();
  if get_timestamp() {
    let now = time::gethrtime();
    let start = START_TIME.load(Ordering::Relaxed);
    let micros = time::micros_elapsed(start, now);
    let seconds = micros / IC_MICROSEC_PER_SECOND;
    let rest = micros % IC_MICROSEC_PER_SECOND;
    line.push_str(&format!("{:010}:{:06}: ", seconds, rest));
  }
  line.push('T');
  line.push_str(&thread_id.to_string());
  line.push(':');
  let mut i: usize = 0;
  while i < indent_level {
    line.push_str("  ");
    i += 1;
  }
  line.push_str(text);
  if SCREEN.load(Ordering::Relaxed) {
    crate::ic_printf!("{}", line);
  }
  let mut slot = lock_file();
  if let Some(f) = slot.as_mut() {
    let _ = f.write_all(line.as_bytes());
    let _ = f.write_all(b"\n");
    let _ = f.flush();
  }
}

/// Print one debug line for the calling thread, if its output is
/// enabled (`ic_debug_print_char_buf`).
pub fn print_str(text: &str) {
  let mut enabled = false;
  let mut thread_id: u32 = 0;
  let mut indent: usize = 0;
  THREAD.with(|cell| {
    let t = cell.borrow();
    enabled = t.enabled;
    thread_id = t.thread_id;
    indent = t.indent_level;
  });
  if !enabled {
    return;
  }
  print_line(text, thread_id, indent);
}

/// Print formatted debug output; used by [`debug_print!`].
pub fn print_fmt(args: fmt::Arguments<'_>) {
  print_str(&args.to_string());
}

/// Print a received buffer (`ic_debug_print_rec_buf`).
pub fn print_buf(buf: &[u8]) {
  let text = String::from_utf8_lossy(buf);
  print_str(&format!("Receive buffer, size {}: {}", buf.len(), text));
}

fn ensure_thread_inited() {
  let mut needs_id = false;
  THREAD.with(|cell| {
    let t = cell.borrow();
    needs_id = !t.inited;
  });
  if needs_id {
    let id = alloc_thread_id();
    THREAD.with(|cell| {
      let mut t = cell.borrow_mut();
      t.thread_id = id;
      t.inited = true;
    });
  }
}

/// Register the calling thread with the tracer and optionally record its
/// first entry point (`ic_debug_thread_init`).
pub fn thread_init(entry_point: Option<&'static str>) {
  ensure_thread_inited();
  THREAD.with(|cell| {
    let mut t = cell.borrow_mut();
    t.enabled = true;
  });
  if let Some(name) = entry_point {
    entry(name);
  }
}

/// Unregister the calling thread; pairs with [`thread_init`]
/// (`ic_debug_thread_return`).
pub fn thread_return() {
  ret();
  let mut thread_id: u32 = 0;
  let mut indent: usize = 0;
  let mut inited = false;
  THREAD.with(|cell| {
    let t = cell.borrow();
    thread_id = t.thread_id;
    indent = t.indent_level;
    inited = t.inited;
  });
  if !inited {
    return;
  }
  if is_level(THREAD_LEVEL) {
    print_str(&format!("Exit from thread id={}", thread_id));
  }
  if indent != 0 {
    print_str(&format!("Thread exit with indent level {}", indent));
  }
  free_thread_id(thread_id);
  THREAD.with(|cell| {
    let mut t = cell.borrow_mut();
    t.inited = false;
    t.thread_id = 0;
    t.indent_level = 0;
  });
}

/// Record entry into a function (`DEBUG_ENTRY`).
pub fn entry(entry_point: &'static str) {
  ensure_thread_inited();
  THREAD.with(|cell| {
    let mut t = cell.borrow_mut();
    let level = t.indent_level;
    if level < IC_DEBUG_MAX_INDENT_LEVEL {
      t.entry_points[level] = entry_point;
    }
    t.indent_level = level + 1;
  });
  if is_level(ENTRY_LEVEL) {
    print_str(&format!("Entry into {}", entry_point));
  }
}

fn current_entry_point() -> (&'static str, usize) {
  let mut name: &'static str = "?";
  let mut indent: usize = 0;
  THREAD.with(|cell| {
    let t = cell.borrow();
    indent = t.indent_level;
    if indent > 0 && indent <= IC_DEBUG_MAX_INDENT_LEVEL {
      name = t.entry_points[indent - 1];
    }
  });
  (name, indent)
}

fn pop_entry() {
  THREAD.with(|cell| {
    let mut t = cell.borrow_mut();
    if t.indent_level > 0 {
      t.indent_level -= 1;
    }
  });
}

/// Record return from the current function (`DEBUG_RETURN_EMPTY`).
pub fn ret() {
  let (name, indent) = current_entry_point();
  if indent == 0 {
    return;
  }
  if is_level(ENTRY_LEVEL) {
    print_str(&format!("Exit from {}, void", name));
  }
  pop_entry();
}

/// Record return of an integer from the current function
/// (`DEBUG_RETURN_INT`).
pub fn ret_int(value: i32) {
  let (name, indent) = current_entry_point();
  if indent == 0 {
    return;
  }
  if is_level(ENTRY_LEVEL) {
    print_str(&format!("Exit from {}, int_val= {}", name, value));
  }
  pop_entry();
}

/// Abort unless the calling thread's nesting depth is `level`
/// (`DEBUG_INDENT_LEVEL_CHECK`).
pub fn indent_level_check(level: usize) {
  let (_, indent) = current_entry_point();
  crate::ic_require!(indent == level);
}

/// Suppress output on this thread unless `level` is enabled; nests
/// (`DEBUG_DISABLE`).
pub fn disable(level: u32) {
  if is_level(level) {
    return;
  }
  THREAD.with(|cell| {
    let mut t = cell.borrow_mut();
    if t.disable_count == 0 {
      t.save_enabled = t.enabled;
      t.enabled = false;
    }
    t.disable_count += 1;
  });
}

/// Undo one [`disable`] (`DEBUG_ENABLE`).
pub fn enable(level: u32) {
  if is_level(level) {
    return;
  }
  THREAD.with(|cell| {
    let mut t = cell.borrow_mut();
    if t.disable_count > 0 {
      t.disable_count -= 1;
      if t.disable_count == 0 {
        t.enabled = t.save_enabled;
      }
    }
  });
}

/// Guard returned by [`entry_guard`]: prints the exit from the function
/// when dropped, whichever way the function returns.
pub struct EntryGuard {
  _private: (),
}

impl Drop for EntryGuard {
  fn drop(&mut self) {
    ret();
  }
}

/// Record entry and return a guard that records the exit. Returns `None`
/// (and does nothing) unless built with `debug_build`.
pub fn entry_guard(entry_point: &'static str) -> Option<EntryGuard> {
  if !DEBUG_BUILD {
    return None;
  }
  entry(entry_point);
  Some(EntryGuard { _private: () })
}

/// `let _dbg = debug_entry!("function_name");` records entry now and
/// exit when `_dbg` goes out of scope. Nothing in a normal build.
#[macro_export]
macro_rules! debug_entry {
  ($name:expr) => {
    $crate::debug::entry_guard($name)
  };
}

/// `debug_print!(LEVEL, "format", args...)`: print a debug line when the
/// level bit is set. Nothing in a normal build.
#[macro_export]
macro_rules! debug_print {
    ($level:expr, $($arg:tt)*) => {
        if $crate::debug::DEBUG_BUILD && $crate::debug::is_level($level) {
            $crate::debug::print_fmt(format_args!($($arg)*));
        }
    };
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn entry_and_return_track_depth() {
    thread_init(None);
    entry("outer");
    entry("inner");
    let (name, depth) = current_entry_point();
    assert_eq!(name, "inner");
    assert_eq!(depth, 2);
    ret_int(0);
    let (name, depth) = current_entry_point();
    assert_eq!(name, "outer");
    assert_eq!(depth, 1);
    ret();
    let (_, depth) = current_entry_point();
    assert_eq!(depth, 0);
    thread_return();
  }

  #[test]
  fn disable_and_enable_nest() {
    set_level(0);
    thread_init(None);
    disable(PROGRAM_LEVEL);
    disable(PROGRAM_LEVEL);
    THREAD.with(|cell| assert!(!cell.borrow().enabled));
    enable(PROGRAM_LEVEL);
    THREAD.with(|cell| assert!(!cell.borrow().enabled));
    enable(PROGRAM_LEVEL);
    THREAD.with(|cell| assert!(cell.borrow().enabled));
    thread_return();
  }

  #[test]
  fn level_bits() {
    set_level(COMM_LEVEL | FILE_LEVEL);
    assert!(is_level(COMM_LEVEL));
    assert!(is_level(FILE_LEVEL | PROGRAM_LEVEL));
    assert!(!is_level(PROGRAM_LEVEL));
    set_level(0);
  }
}
