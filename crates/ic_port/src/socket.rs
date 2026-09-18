// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Socket helpers below the communication layer (`ic_close_socket`,
//! `ic_start_socket_system`, `ic_stop_socket_system`, non-blocking mode,
//! and the platform difference around `SIGPIPE` on send).

use crate::debug::COMM_LEVEL;
use crate::IcError;

/// The value of a socket descriptor that is not open.
pub const IC_INVALID_SOCKET: i32 = -1;

/// Flags to pass to `send()` so a closed peer does not raise `SIGPIPE`.
/// Linux has `MSG_NOSIGNAL`; macOS uses the `SO_NOSIGPIPE` socket option
/// set by [`set_no_sigpipe`] instead.
#[cfg(target_os = "linux")]
pub const IC_SEND_FLAGS: i32 = libc::MSG_NOSIGNAL;
/// See the Linux definition.
#[cfg(not(target_os = "linux"))]
pub const IC_SEND_FLAGS: i32 = 0;

/// Start the socket subsystem. Nothing to do on Unix.
pub fn start_socket_system() -> Result<(), IcError> {
  Ok(())
}

/// Stop the socket subsystem. Nothing to do on Unix.
pub fn stop_socket_system() -> Result<(), IcError> {
  Ok(())
}

/// Close a socket, retrying on `EINTR`. Errors are only debug printed,
/// as in the C code.
pub fn close_socket(sockfd: i32) {
  loop {
    // SAFETY: close on an integer descriptor has no memory effects.
    let ret = unsafe { libc::close(sockfd) };
    if ret == 0 {
      return;
    }
    let err = crate::oserr::last_socket_error();
    if err != libc::EINTR {
      crate::debug_print!(COMM_LEVEL, "close failed with errno = {}", err);
      return;
    }
  }
}

/// Switch a socket between blocking and non-blocking mode.
pub fn set_nonblocking(sockfd: i32, on: bool) -> Result<(), IcError> {
  // SAFETY: fcntl on an integer descriptor with integer arguments.
  let flags = unsafe { libc::fcntl(sockfd, libc::F_GETFL) };
  if flags < 0 {
    return Err(IcError::last_os_error());
  }
  let new_flags = if on {
    flags | libc::O_NONBLOCK
  } else {
    flags & !libc::O_NONBLOCK
  };
  // SAFETY: as above.
  let ret = unsafe { libc::fcntl(sockfd, libc::F_SETFL, new_flags) };
  if ret < 0 {
    return Err(IcError::last_os_error());
  }
  Ok(())
}

/// On macOS set `SO_NOSIGPIPE` so a send to a closed peer returns an
/// error instead of killing the process. No-op on Linux, which uses
/// [`IC_SEND_FLAGS`] per send instead.
pub fn set_no_sigpipe(sockfd: i32) -> Result<(), IcError> {
  #[cfg(target_os = "macos")]
  {
    let one: libc::c_int = 1;
    // SAFETY: setsockopt with a pointer to a live c_int of the given
    // size.
    let ret = unsafe {
      libc::setsockopt(
        sockfd,
        libc::SOL_SOCKET,
        libc::SO_NOSIGPIPE,
        &one as *const libc::c_int as *const libc::c_void,
        std::mem::size_of::<libc::c_int>() as libc::socklen_t,
      )
    };
    if ret < 0 {
      return Err(IcError::last_os_error());
    }
    Ok(())
  }
  #[cfg(not(target_os = "macos"))]
  {
    let _ = sockfd;
    Ok(())
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::io::IntoRawFd;

  #[test]
  fn nonblocking_toggle_on_a_real_socket() {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("bind");
    let fd = listener.into_raw_fd();
    set_nonblocking(fd, true).expect("nonblocking on");
    set_nonblocking(fd, false).expect("nonblocking off");
    set_no_sigpipe(fd).expect("no sigpipe");
    close_socket(fd);
  }
}
