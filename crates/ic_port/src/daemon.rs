// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Daemonising, pid files and signal handlers (`ic_daemonize`,
//! `ic_setup_workdir`, `ic_set_umask`, `ic_write_pid_file`,
//! `ic_read_pid_file`, `ic_set_die_handler`, `ic_set_sig_error_handler`,
//! `ic_controlled_terminate`).
//!
//! Daemonisation happens very early, before any thread is started and
//! before the debug system is opened, because `fork` in a process with
//! threads is unsafe and because the pid changes.
//!
//! Rust notes for C readers: the signal handler function pointers are
//! stored as integers in atomics (`AtomicUsize`) because a signal handler
//! may run on any thread at any time; C used plain static globals.

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use crate::debug::IC_COMM_LEVEL;
use crate::debug::IC_PROGRAM_LEVEL;
use crate::err;
use crate::IcError;

/// A handler called with the parameter given at registration.
pub type SigHandlerFunc = fn(usize);

static DIE_HANDLER: AtomicUsize = AtomicUsize::new(0);
static DIE_PARAM: AtomicUsize = AtomicUsize::new(0);
static SIG_ERROR_HANDLER: AtomicUsize = AtomicUsize::new(0);
static SIG_ERROR_PARAM: AtomicUsize = AtomicUsize::new(0);

fn call_stored_handler(handler: &AtomicUsize, param: &AtomicUsize) {
  let h = handler.load(Ordering::Acquire);
  if h == 0 {
    return;
  }
  // SAFETY: the value was stored from a `SigHandlerFunc` by
  // set_die_handler/set_sig_error_handler and nothing else writes it.
  let f: SigHandlerFunc =
    unsafe { std::mem::transmute::<usize, SigHandlerFunc>(h) };
  f(param.load(Ordering::Acquire));
}

fn install(signum: libc::c_int, handler: extern "C" fn(libc::c_int)) {
  // SAFETY: installing a handler that only touches atomics and the
  // stop flag, the same discipline as the C code.
  unsafe {
    libc::signal(signum, handler as usize as libc::sighandler_t);
  }
}

fn ignore(signum: libc::c_int) {
  // SAFETY: SIG_IGN is a valid handler value.
  unsafe {
    libc::signal(signum, libc::SIG_IGN);
  }
}

extern "C" fn kill_handler(signum: libc::c_int) {
  crate::debug_print!(IC_COMM_LEVEL, "kill_handler: signum = {}", signum);
  let handled = signum == libc::SIGTERM
    || signum == libc::SIGINT
    || signum == libc::SIGHUP
    || signum == libc::SIGXCPU
    || signum == extra_kill_signal();
  if !handled {
    // SAFETY: _exit is async-signal-safe.
    unsafe { libc::_exit(1) };
  }
  crate::stop::set_stop_flag();
  call_stored_handler(&DIE_HANDLER, &DIE_PARAM);
}

extern "C" fn sig_error_handler(signum: libc::c_int) {
  crate::debug_print!(IC_COMM_LEVEL, "sig_error_handler: signum = {}", signum);
  let handled = signum == libc::SIGSEGV
    || signum == libc::SIGFPE
    || signum == libc::SIGILL
    || signum == libc::SIGBUS
    || signum == libc::SIGSYS
    || signum == libc::SIGQUIT;
  if !handled {
    return;
  }
  crate::stop::set_stop_flag();
  call_stored_handler(&SIG_ERROR_HANDLER, &SIG_ERROR_PARAM);
  crate::debug_print!(IC_PROGRAM_LEVEL, "Abort process");
  std::process::abort();
}

extern "C" fn daemon_parent_handler(signum: libc::c_int) {
  if signum == libc::SIGUSR1 {
    // The child has finished setting up; the parent may leave.
    // SAFETY: _exit is async-signal-safe.
    unsafe { libc::_exit(0) };
  }
  // SIGALRM: fall through and let pause() return.
}

/// The platform's "information" signal: `SIGINFO` on BSD/macOS, `SIGPWR`
/// on Linux, both treated as a request to stop.
#[cfg(target_os = "linux")]
fn extra_kill_signal() -> libc::c_int {
  libc::SIGPWR
}
/// See the Linux definition.
#[cfg(not(target_os = "linux"))]
fn extra_kill_signal() -> libc::c_int {
  libc::SIGINFO
}

/// Install `die_handler(param)` to run, after the stop flag is set, on
/// SIGTERM, SIGINT, SIGHUP, SIGXCPU and SIGINFO/SIGPWR.
pub fn set_die_handler(die_handler: Option<SigHandlerFunc>, param: usize) {
  let value = match die_handler {
    Some(f) => f as usize,
    None => 0,
  };
  DIE_PARAM.store(param, Ordering::Release);
  DIE_HANDLER.store(value, Ordering::Release);
  install(libc::SIGHUP, kill_handler);
  install(libc::SIGTERM, kill_handler);
  install(libc::SIGINT, kill_handler);
  install(libc::SIGXCPU, kill_handler);
  install(extra_kill_signal(), kill_handler);
}

/// Install `error_handler(param)` to run, after the stop flag is set and
/// before the process aborts, on SIGSEGV, SIGFPE, SIGILL, SIGQUIT and
/// SIGSYS. Also ignores SIGPIPE.
pub fn set_sig_error_handler(
  error_handler: Option<SigHandlerFunc>,
  param: usize,
) {
  let value = match error_handler {
    Some(f) => f as usize,
    None => 0,
  };
  SIG_ERROR_PARAM.store(param, Ordering::Release);
  SIG_ERROR_HANDLER.store(value, Ordering::Release);
  install(libc::SIGSEGV, sig_error_handler);
  install(libc::SIGFPE, sig_error_handler);
  install(libc::SIGILL, sig_error_handler);
  install(libc::SIGQUIT, sig_error_handler);
  install(libc::SIGSYS, sig_error_handler);
  ignore(libc::SIGPIPE);
}

/// Turn the process into a daemon: fork, become session leader, fork
/// again, close all descriptors and point stdin/stdout/stderr at
/// /dev/null. The original process exits 0 once the daemon is set up
/// (or after 3 seconds). Must be called before any thread is started.
pub fn daemonize() -> Result<(), IcError> {
  install(libc::SIGUSR1, daemon_parent_handler);
  install(libc::SIGALRM, daemon_parent_handler);
  ignore(libc::SIGCHLD);
  ignore(libc::SIGHUP);
  ignore(libc::SIGINT);
  ignore(libc::SIGCONT);
  ignore(libc::SIGTSTP);
  ignore(libc::SIGTTIN);
  ignore(libc::SIGTTOU);

  // SAFETY: single-threaded at this point by contract; every call
  // below is a plain POSIX process call.
  unsafe {
    let child_pid = libc::fork();
    if child_pid < 0 {
      return Err(IcError::new(err::IC_ERROR_FAILED_TO_DAEMONIZE));
    }
    if child_pid > 0 {
      // Original process: wait for the daemon's SIGUSR1, or give
      // up after 3 seconds, then leave.
      libc::alarm(3);
      libc::pause();
      libc::_exit(0);
    }
    let parent_pid = libc::getppid();
    if libc::setsid() < 0 {
      libc::_exit(1);
    }
    let child_pid = libc::fork();
    if child_pid < 0 {
      libc::kill(parent_pid, libc::SIGUSR1);
      libc::_exit(1);
    }
    if child_pid > 0 {
      libc::_exit(0);
    }
    let mut fd: libc::c_int = 0;
    while fd < 64 {
      libc::close(fd);
      fd += 1;
    }
    let null = libc::open(c"/dev/null".as_ptr(), libc::O_RDWR);
    if null >= 0 {
      libc::dup2(null, 0);
      libc::dup2(null, 1);
      libc::dup2(null, 2);
      if null > 2 {
        libc::close(null);
      }
    }
    libc::kill(parent_pid, libc::SIGUSR1);
  }
  Ok(())
}

/// Create the working directory if needed and change into it.
pub fn setup_workdir(new_work_dir: &str) -> Result<(), IcError> {
  if crate::file::mkdir(new_work_dir).is_err() {
    return Err(IcError::new(err::IC_ERROR_FAILED_TO_CHANGE_DIR));
  }
  match std::env::set_current_dir(new_work_dir) {
    Ok(()) => Ok(()),
    Err(e) => {
      let os = IcError::from_io(&e);
      crate::ic_printf!("chdir error: {}: {}", os.code, os.message());
      Err(IcError::new(err::IC_ERROR_FAILED_TO_CHANGE_DIR))
    }
  }
}

/// Placeholder kept from the C code; core file limits are left to the
/// environment.
pub fn generate_core_files() {}

/// Limit files created by this process to owner and group (mask out all
/// "other" permissions).
pub fn set_umask() {
  let mask: libc::mode_t = libc::S_IROTH | libc::S_IWOTH | libc::S_IXOTH;
  // SAFETY: umask has no memory effects.
  unsafe {
    libc::umask(mask);
  }
}

/// Our own process id.
pub fn get_own_pid() -> u64 {
  std::process::id() as u64
}

/// Write our pid into the file, replacing any old content.
pub fn write_pid_file(pid_file: &str) -> Result<(), IcError> {
  let _dbg = crate::debug_entry!("write_pid_file");
  let mut file = match crate::file::create_file(pid_file) {
    Ok(f) => f,
    Err(e) => {
      crate::debug_print!(
        IC_PROGRAM_LEVEL,
        "Pid file {} could not be created",
        pid_file
      );
      return Err(e);
    }
  };
  let text = get_own_pid().to_string();
  if let Err(e) = crate::file::write_file(&mut file, text.as_bytes()) {
    let _ = crate::file::delete_file(pid_file);
    return Err(e);
  }
  if let Err(e) = crate::file::close_file(file) {
    let _ = crate::file::delete_file(pid_file);
    return Err(e);
  }
  crate::debug_print!(
    IC_PROGRAM_LEVEL,
    "Created pid file for process {}",
    text
  );
  Ok(())
}

/// Read a pid written by [`write_pid_file`].
pub fn read_pid_file(pid_file: &str) -> Result<u64, IcError> {
  let _dbg = crate::debug_entry!("read_pid_file");
  let contents = crate::file::get_file_contents(pid_file)?;
  let text = String::from_utf8_lossy(&contents);
  match text.trim().parse::<u64>() {
    Ok(pid) => {
      crate::debug_print!(
        IC_PROGRAM_LEVEL,
        "Read pidfile {}, found pid {}",
        pid_file,
        pid
      );
      Ok(pid)
    }
    Err(_) => {
      crate::debug_print!(
        IC_PROGRAM_LEVEL,
        "Wrong content in pidfile: {}",
        text
      );
      Err(IcError::new(err::IC_ERROR_WRONG_PID_FILE_CONTENT))
    }
  }
}

/// Remove the pid file at shutdown.
pub fn delete_daemon_file(pid_file: &str) {
  let _ = crate::file::delete_file(pid_file);
}

/// Send ourselves SIGTERM to start the normal termination path.
pub fn controlled_terminate() {
  // SAFETY: kill on our own pid with a standard signal.
  unsafe {
    libc::kill(libc::getpid(), libc::SIGTERM);
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn pid_file_round_trip() {
    let mut path = std::env::temp_dir();
    path.push(format!("ic_port_{}.pid", std::process::id()));
    let name = path.to_string_lossy().to_string();
    write_pid_file(&name).expect("write pid");
    let pid = read_pid_file(&name).expect("read pid");
    assert_eq!(pid, get_own_pid());
    std::fs::write(&name, "not a number").expect("overwrite");
    let bad = read_pid_file(&name);
    let wrong = err::IC_ERROR_WRONG_PID_FILE_CONTENT;
    assert_eq!(bad, Err(IcError::new(wrong)));
    delete_daemon_file(&name);
  }

  #[test]
  fn umask_and_pid() {
    set_umask();
    assert!(get_own_pid() > 0);
    generate_core_files();
  }
}
