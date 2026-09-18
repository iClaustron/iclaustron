// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Standard output handling: `ic_printf!`, `ic_putchar`, redirection of
//! all program output to a file (`ic_setup_stdout`) or to nowhere
//! (`ic_set_stdout_null`). From the non-debug part of
//! `legacy-c/util/ic_debug.c`.

use std::fmt;
use std::fs::File;
use std::io::Write;
use std::sync::Mutex;

/// Where output goes: 0 = stdout, 1 = a file, -1 = nowhere.
struct StdoutState {
  defined: i32,
  file: Option<File>,
}

static STDOUT: Mutex<StdoutState> = Mutex::new(StdoutState {
  defined: 0,
  file: None,
});

fn lock_state() -> std::sync::MutexGuard<'static, StdoutState> {
  match STDOUT.lock() {
    Ok(g) => g,
    Err(poisoned) => poisoned.into_inner(),
  }
}

/// Send all further output to nowhere (daemons without a log file).
pub fn set_stdout_null() {
  let mut state = lock_state();
  state.defined = -1;
  state.file = None;
}

/// Send all further output to the named file, replacing any old file.
pub fn setup_stdout(log_file: &str) -> Result<(), crate::IcError> {
  let _ = std::fs::remove_file(log_file);
  let file = match File::create(log_file) {
    Ok(f) => f,
    Err(_) => {
      return Err(crate::IcError::new(crate::err::IC_ERROR_FAILED_OPEN_STDOUT))
    }
  };
  let mut state = lock_state();
  state.defined = 1;
  state.file = Some(file);
  Ok(())
}

/// Send output back to the real standard output.
pub fn reset_stdout() {
  let mut state = lock_state();
  state.defined = 0;
  state.file = None;
}

/// Flush whatever output is buffered.
pub fn flush_stdout() {
  let mut state = lock_state();
  if state.defined == 0 {
    let _ = std::io::stdout().flush();
    return;
  }
  if let Some(f) = state.file.as_mut() {
    let _ = f.flush();
  }
}

/// Print one line (a newline is appended) to the current output and
/// flush. Called through [`ic_printf!`].
pub fn printf(args: fmt::Arguments<'_>) {
  let mut state = lock_state();
  if state.defined == 0 {
    let mut out = std::io::stdout();
    let _ = out.write_fmt(args);
    let _ = out.write_all(b"\n");
    let _ = out.flush();
    return;
  }
  if let Some(f) = state.file.as_mut() {
    let _ = f.write_fmt(args);
    let _ = f.write_all(b"\n");
    let _ = f.flush();
  }
}

/// Print one byte to the current output without a newline.
pub fn putchar(c: u8) {
  let mut state = lock_state();
  if state.defined == 0 {
    let _ = std::io::stdout().write_all(&[c]);
    return;
  }
  if let Some(f) = state.file.as_mut() {
    let _ = f.write_all(&[c]);
  }
}

/// `ic_printf!("format", args...)`: print one line to the program output
/// (stdout, or the file set with `output::setup_stdout`) and flush.
#[macro_export]
macro_rules! ic_printf {
    ($($arg:tt)*) => {
        $crate::output::printf(format_args!($($arg)*))
    };
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn printf_to_file_and_back() {
    let path = std::env::temp_dir().join("ic_port_output_test.log");
    let path_str = path.to_string_lossy().to_string();
    setup_stdout(&path_str).expect("setup_stdout");
    crate::ic_printf!("hello {}", 42);
    putchar(b'x');
    flush_stdout();
    reset_stdout();
    let text = std::fs::read_to_string(&path).expect("read log");
    assert_eq!(text, "hello 42\nx");
    let _ = std::fs::remove_file(&path);
  }
}
