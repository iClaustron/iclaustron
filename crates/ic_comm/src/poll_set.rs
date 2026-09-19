// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Waiting on many sockets at once (`IC_POLL_SET`,
//! `legacy-c/comm/ic_poll_set.c`), over `epoll` on Linux and `kqueue` on
//! macOS and the BSDs.
//!
//! A receive thread owns one poll set holding the sockets of the data
//! nodes assigned to it. It waits, then walks the connections that have
//! data. As in the C, a poll set belongs to one thread and has no lock
//! of its own.
//!
//! Usage:
//!
//! ```text
//!   set.add_connection(fd, user_obj)
//!   loop {
//!     set.check(10)                       wait up to 10 ms
//!     while let Some(c) = set.next_connection() {
//!       ... read from c.fd, c.user_obj says which node it is ...
//!     }
//!   }
//! ```
//!
//! Rust notes for C readers: `epoll` and `kqueue` are selected with
//! `#[cfg(target_os = ...)]`, which is the `#ifdef LINUX` of Rust,
//! except that the condition is checked by the compiler rather than the
//! preprocessor. Only one of the two backends is compiled.

use std::collections::HashMap;

use ic_port::err;
use ic_port::IcError;

/// Largest number of sockets in one poll set, as in the C.
pub const IC_MAX_POLL_SET_SIZE: usize = 1024;

/// One socket in the poll set, and what the owner wants to remember
/// about it (`IC_POLL_CONNECTION`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PollConnection {
  /// The socket.
  pub fd: i32,
  /// Whatever the owner passed to
  /// [`add_connection`](PollSet::add_connection); the Data API puts the
  /// node id here.
  pub user_obj: usize,
  /// 0, or an error the wait reported for this socket.
  pub ret_code: i32,
}

/// A set of sockets to wait on.
pub struct PollSet {
  backend: Backend,
  /// Every socket in the set, by file descriptor.
  connections: HashMap<i32, PollConnection>,
  /// Sockets reported ready by the last [`check`](PollSet::check).
  ready: Vec<PollConnection>,
  /// Position in `ready` for [`next_connection`](PollSet::next_connection).
  next_ready: usize,
  max_size: usize,
}

impl PollSet {
  /// A poll set holding up to [`IC_MAX_POLL_SET_SIZE`] sockets
  /// (`ic_create_poll_set`).
  pub fn new() -> Result<PollSet, IcError> {
    PollSet::with_capacity(IC_MAX_POLL_SET_SIZE)
  }

  /// A poll set holding up to `max_size` sockets.
  pub fn with_capacity(max_size: usize) -> Result<PollSet, IcError> {
    Ok(PollSet {
      backend: Backend::new()?,
      connections: HashMap::new(),
      ready: Vec::with_capacity(max_size),
      next_ready: 0,
      max_size,
    })
  }

  /// Add a socket, with something to remember it by
  /// (`ic_poll_set_add_connection`).
  pub fn add_connection(
    &mut self,
    fd: i32,
    user_obj: usize,
  ) -> Result<(), IcError> {
    if self.connections.len() >= self.max_size {
      return Err(IcError::new(err::IC_ERROR_POLL_SET_FULL));
    }
    if self.connections.contains_key(&fd) {
      return Err(IcError::new(err::IC_ERROR_NODE_ALREADY_DEFINED));
    }
    self.backend.add(fd)?;
    self.connections.insert(
      fd,
      PollConnection {
        fd,
        user_obj,
        ret_code: 0,
      },
    );
    Ok(())
  }

  /// Remove a socket (`ic_poll_set_remove_connection`).
  pub fn remove_connection(&mut self, fd: i32) -> Result<(), IcError> {
    if self.connections.remove(&fd).is_none() {
      return Err(IcError::new(err::IC_ERROR_NOT_FOUND_IN_POLL_SET));
    }
    self.backend.remove(fd)?;
    Ok(())
  }

  /// How many sockets are in the set.
  pub fn len(&self) -> usize {
    self.connections.len()
  }

  /// True if no socket is in the set.
  pub fn is_empty(&self) -> bool {
    self.connections.is_empty()
  }

  /// True if no more sockets fit (`ic_is_poll_set_full`).
  pub fn is_full(&self) -> bool {
    self.connections.len() >= self.max_size
  }

  /// Wait up to `ms_time` milliseconds for any socket to have data, and
  /// return how many are ready (`ic_check_poll_set`). A negative
  /// `ms_time` waits forever.
  ///
  /// A socket may be reported ready and still carry an error; the error
  /// is in its `ret_code`, so both this return value and each
  /// connection have to be checked, exactly as the C warned.
  pub fn check(&mut self, ms_time: i32) -> Result<usize, IcError> {
    self.ready.clear();
    self.next_ready = 0;
    let mut events: Vec<ReadyEvent> = Vec::new();
    self.backend.wait(ms_time, self.max_size, &mut events)?;
    for event in &events {
      if let Some(conn) = self.connections.get(&event.fd) {
        let mut ready = *conn;
        ready.ret_code = event.ret_code;
        self.ready.push(ready);
      }
    }
    Ok(self.ready.len())
  }

  /// The next socket with data since the last [`check`](Self::check),
  /// or `None` when they have all been returned
  /// (`ic_get_next_connection`).
  pub fn next_connection(&mut self) -> Option<PollConnection> {
    if self.next_ready >= self.ready.len() {
      return None;
    }
    let conn = self.ready[self.next_ready];
    self.next_ready += 1;
    Some(conn)
  }
}

/// One socket reported ready by the backend.
struct ReadyEvent {
  fd: i32,
  ret_code: i32,
}

#[cfg(target_os = "linux")]
mod backend_impl {
  use super::ReadyEvent;
  use ic_port::IcError;

  /// The `epoll` file descriptor and nothing else: `epoll` remembers the
  /// set for us.
  pub struct Backend {
    epoll_fd: i32,
  }

  impl Backend {
    pub fn new() -> Result<Backend, IcError> {
      // SAFETY: epoll_create1 takes a flag and returns a descriptor.
      let fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
      if fd < 0 {
        return Err(IcError::last_os_error());
      }
      Ok(Backend { epoll_fd: fd })
    }

    pub fn add(&mut self, fd: i32) -> Result<(), IcError> {
      let mut event = libc::epoll_event {
        events: libc::EPOLLIN as u32,
        u64: fd as u64,
      };
      // SAFETY: epoll_ctl with a live event struct we own.
      let ret = unsafe {
        libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_ADD, fd, &mut event)
      };
      if ret < 0 {
        return Err(IcError::last_os_error());
      }
      Ok(())
    }

    pub fn remove(&mut self, fd: i32) -> Result<(), IcError> {
      // SAFETY: epoll_ctl DEL ignores the event argument on modern
      // kernels but older ones dereference it, so a real struct is
      // passed.
      let mut event = libc::epoll_event { events: 0, u64: 0 };
      let ret = unsafe {
        libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, &mut event)
      };
      if ret < 0 {
        return Err(IcError::last_os_error());
      }
      Ok(())
    }

    pub fn wait(
      &mut self,
      ms_time: i32,
      max_events: usize,
      out: &mut Vec<ReadyEvent>,
    ) -> Result<(), IcError> {
      let mut events: Vec<libc::epoll_event> = Vec::with_capacity(max_events);
      // SAFETY: epoll_wait fills up to max_events entries of the buffer
      // just reserved; the length is set from its return value.
      let count = unsafe {
        let ret = libc::epoll_wait(
          self.epoll_fd,
          events.as_mut_ptr(),
          max_events as i32,
          ms_time,
        );
        if ret < 0 {
          let err = IcError::last_os_error();
          if err.code == libc::EINTR {
            return Ok(());
          }
          return Err(err);
        }
        events.set_len(ret as usize);
        ret as usize
      };
      let mut i: usize = 0;
      while i < count {
        let event = events[i];
        let mut ret_code: i32 = 0;
        let flags = event.events;
        if (flags & (libc::EPOLLERR as u32)) != 0 {
          ret_code = libc::ECONNRESET;
        } else if (flags & (libc::EPOLLHUP as u32)) != 0 {
          ret_code = libc::ECONNRESET;
        }
        out.push(ReadyEvent {
          fd: event.u64 as i32,
          ret_code,
        });
        i += 1;
      }
      Ok(())
    }
  }

  impl Drop for Backend {
    fn drop(&mut self) {
      ic_port::socket::close_socket(self.epoll_fd);
    }
  }
}

#[cfg(not(target_os = "linux"))]
mod backend_impl {
  use super::ReadyEvent;
  use ic_port::IcError;

  /// The `kqueue` descriptor. Like `epoll`, the kernel holds the set.
  pub struct Backend {
    kqueue_fd: i32,
  }

  fn make_event(fd: i32, flags: u16) -> libc::kevent {
    libc::kevent {
      ident: fd as libc::uintptr_t,
      filter: libc::EVFILT_READ,
      flags,
      fflags: 0,
      data: 0,
      udata: std::ptr::null_mut(),
    }
  }

  impl Backend {
    pub fn new() -> Result<Backend, IcError> {
      // SAFETY: kqueue takes no argument and returns a descriptor.
      let fd = unsafe { libc::kqueue() };
      if fd < 0 {
        return Err(IcError::last_os_error());
      }
      // SAFETY: setting close-on-exec on a descriptor we own.
      unsafe {
        libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC);
      }
      Ok(Backend { kqueue_fd: fd })
    }

    fn change(&mut self, event: libc::kevent) -> Result<(), IcError> {
      let changes = [event];
      // SAFETY: kevent is given one change and asked for no events, so
      // the output pointer is null and its count zero.
      let ret = unsafe {
        libc::kevent(
          self.kqueue_fd,
          changes.as_ptr(),
          1,
          std::ptr::null_mut(),
          0,
          std::ptr::null(),
        )
      };
      if ret < 0 {
        return Err(IcError::last_os_error());
      }
      Ok(())
    }

    pub fn add(&mut self, fd: i32) -> Result<(), IcError> {
      self.change(make_event(fd, libc::EV_ADD | libc::EV_ENABLE))
    }

    pub fn remove(&mut self, fd: i32) -> Result<(), IcError> {
      self.change(make_event(fd, libc::EV_DELETE))
    }

    pub fn wait(
      &mut self,
      ms_time: i32,
      max_events: usize,
      out: &mut Vec<ReadyEvent>,
    ) -> Result<(), IcError> {
      let timeout = libc::timespec {
        tv_sec: (ms_time / 1000) as libc::time_t,
        tv_nsec: ((ms_time % 1000) * 1_000_000) as libc::c_long,
      };
      let timeout_ptr: *const libc::timespec = if ms_time < 0 {
        std::ptr::null()
      } else {
        &timeout
      };
      let mut events: Vec<libc::kevent> = Vec::with_capacity(max_events);
      // SAFETY: kevent fills up to max_events entries of the buffer just
      // reserved; the length is set from its return value.
      let count = unsafe {
        let ret = libc::kevent(
          self.kqueue_fd,
          std::ptr::null(),
          0,
          events.as_mut_ptr(),
          max_events as libc::c_int,
          timeout_ptr,
        );
        if ret < 0 {
          let err = IcError::last_os_error();
          if err.code == libc::EINTR {
            return Ok(());
          }
          return Err(err);
        }
        events.set_len(ret as usize);
        ret as usize
      };
      let mut i: usize = 0;
      while i < count {
        let event = &events[i];
        let mut ret_code: i32 = 0;
        if (event.flags & libc::EV_ERROR) != 0 {
          ret_code = event.data as i32;
        } else if (event.flags & libc::EV_EOF) != 0 {
          ret_code = libc::ECONNRESET;
        }
        out.push(ReadyEvent {
          fd: event.ident as i32,
          ret_code,
        });
        i += 1;
      }
      Ok(())
    }
  }

  impl Drop for Backend {
    fn drop(&mut self) {
      ic_port::socket::close_socket(self.kqueue_fd);
    }
  }
}

use backend_impl::Backend;

#[cfg(test)]
mod tests {
  use super::*;
  use std::io::Read;
  use std::io::Write;
  use std::net::TcpListener;
  use std::net::TcpStream;
  use std::os::unix::io::AsRawFd;

  /// A connected pair of sockets over the loopback interface.
  fn socket_pair() -> (TcpStream, TcpStream) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");
    let client = TcpStream::connect(addr).expect("connect");
    let (server, _) = listener.accept().expect("accept");
    (client, server)
  }

  #[test]
  fn an_empty_set_times_out() {
    let mut set = PollSet::new().expect("create");
    assert!(set.is_empty());
    assert!(!set.is_full());
    let ready = set.check(5).expect("check");
    assert_eq!(ready, 0);
    assert_eq!(set.next_connection(), None);
  }

  #[test]
  fn data_on_one_socket_is_reported() {
    let (mut client, server) = socket_pair();
    let mut set = PollSet::new().expect("create");
    set.add_connection(server.as_raw_fd(), 42).expect("add");
    assert_eq!(set.len(), 1);
    /* Nothing written yet, so the wait times out. */
    assert_eq!(set.check(5).expect("check"), 0);
    client.write_all(b"ping").expect("write");
    client.flush().expect("flush");
    let mut ready = 0;
    let mut spins = 0;
    while ready == 0 && spins < 100 {
      ready = set.check(50).expect("check");
      spins += 1;
    }
    assert_eq!(ready, 1);
    let conn = set.next_connection().expect("connection");
    assert_eq!(conn.fd, server.as_raw_fd());
    assert_eq!(conn.user_obj, 42);
    assert_eq!(conn.ret_code, 0);
    assert_eq!(set.next_connection(), None);
    set.remove_connection(server.as_raw_fd()).expect("remove");
    assert!(set.is_empty());
  }

  #[test]
  fn several_sockets_are_distinguished() {
    let (mut client_a, server_a) = socket_pair();
    let (mut client_b, server_b) = socket_pair();
    let mut set = PollSet::new().expect("create");
    set.add_connection(server_a.as_raw_fd(), 1).expect("add a");
    set.add_connection(server_b.as_raw_fd(), 2).expect("add b");
    client_b.write_all(b"only b").expect("write");
    client_b.flush().expect("flush");
    let mut ready = 0;
    let mut spins = 0;
    while ready == 0 && spins < 100 {
      ready = set.check(50).expect("check");
      spins += 1;
    }
    assert_eq!(ready, 1);
    let conn = set.next_connection().expect("connection");
    assert_eq!(conn.user_obj, 2);
    /* Now write on the other one too. */
    client_a.write_all(b"and a").expect("write");
    client_a.flush().expect("flush");
    let mut seen_a = false;
    let mut seen_b = false;
    let mut spins = 0;
    while (!seen_a || !seen_b) && spins < 100 {
      set.check(50).expect("check");
      while let Some(conn) = set.next_connection() {
        if conn.user_obj == 1 {
          seen_a = true;
        }
        if conn.user_obj == 2 {
          seen_b = true;
        }
      }
      spins += 1;
    }
    assert!(seen_a);
    assert!(seen_b);
  }

  #[test]
  fn a_closed_peer_is_reported() {
    let (client, mut server) = socket_pair();
    let mut set = PollSet::new().expect("create");
    set.add_connection(server.as_raw_fd(), 7).expect("add");
    drop(client);
    let mut ready = 0;
    let mut spins = 0;
    while ready == 0 && spins < 100 {
      ready = set.check(50).expect("check");
      spins += 1;
    }
    assert_eq!(ready, 1);
    let conn = set.next_connection().expect("connection");
    assert_eq!(conn.user_obj, 7);
    // A read now returns end of file whether or not the backend
    // flagged the hang-up.
    let mut buf = [0u8; 4];
    let n = server.read(&mut buf).expect("read");
    assert_eq!(n, 0);
  }

  #[test]
  fn errors_on_add_and_remove() {
    let (_client, server) = socket_pair();
    let mut set = PollSet::with_capacity(1).expect("create");
    set.add_connection(server.as_raw_fd(), 1).expect("add");
    assert!(set.is_full());
    assert!(set.add_connection(server.as_raw_fd(), 1).is_err());
    let (_c2, server2) = socket_pair();
    assert!(set.add_connection(server2.as_raw_fd(), 2).is_err());
    assert!(set.remove_connection(999).is_err());
    set.remove_connection(server.as_raw_fd()).expect("remove");
  }
}
