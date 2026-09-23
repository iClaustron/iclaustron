// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! One TCP connection (`IC_CONNECTION`,
//! `legacy-c/comm/ic_connection.c`).
//!
//! An API node only ever connects out: to a management server for the
//! configuration, and to each data node for signals. The C also had a
//! server half, with listen, accept and a connect thread, which served
//! the cluster server and the case where a data node connects inward.
//! That half is out of scope.
//!
//! A connection is used by two threads at once, a send thread writing
//! and a receive thread reading, which the C arranged with its read and
//! write sessions. Here [`read`](Connection::read) and
//! [`write`](Connection::write) both take `&self`, so an
//! `Arc<Connection>` can be held by both threads, and the statistics are
//! counted in atomics so neither thread waits for the other.

use std::io::IoSlice;
use std::io::Read;
use std::io::Write;
use std::net::SocketAddr;
use std::net::TcpStream;
use std::net::ToSocketAddrs;
use std::os::unix::io::AsRawFd;
use std::os::unix::io::FromRawFd;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::time::Duration;

use ic_port::debug::IC_COMM_LEVEL;
use ic_port::err;
use ic_port::time;
use ic_port::IcError;

/// Number of message size ranges the statistics count: 0 to 31 bytes,
/// 32 to 63, and so on up to 512 kBytes and larger.
pub const IC_NUM_SIZE_RANGES: usize = 16;

/// How a connection should be made and what socket options to use.
///
/// Zero or `None` everywhere means "leave the system default", as the C
/// prepare calls did.
#[derive(Clone, Debug, Default)]
pub struct ConnectConfig {
  /// Host name or address to connect to.
  pub server_name: String,
  /// Port to connect to.
  pub server_port: u16,
  /// Local address to bind before connecting, for a machine with
  /// several interfaces.
  pub client_name: Option<String>,
  /// Local port to bind before connecting; 0 lets the system choose.
  pub client_port: u16,
  /// `TCP_MAXSEG`, the largest segment the connection should use.
  pub tcp_maxseg: u32,
  /// Tune for a wide area link rather than a local one.
  pub is_wan_connection: bool,
  /// `SO_RCVBUF`, kernel receive buffer size.
  pub tcp_receive_buffer_size: u32,
  /// `SO_SNDBUF`, kernel send buffer size.
  pub tcp_send_buffer_size: u32,
  /// How long to wait for the connection to be established; 0 means the
  /// system default.
  pub connect_timeout_ms: u32,
}

/// What a connection has carried, and what the socket options ended up
/// being (`IC_CONNECT_STAT`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConnectStatSnapshot {
  /// Number of writes.
  pub num_sent_buffers: u64,
  /// Bytes written.
  pub num_sent_bytes: u64,
  /// Number of reads that returned data.
  pub num_rec_buffers: u64,
  /// Bytes read.
  pub num_rec_bytes: u64,
  /// Writes that failed.
  pub num_send_errors: u64,
  /// Writes that timed out.
  pub num_send_timeouts: u64,
  /// Reads that failed.
  pub num_rec_errors: u64,
  /// Writes counted by size range.
  pub num_sent_buf_range: [u32; IC_NUM_SIZE_RANGES],
  /// Reads counted by size range.
  pub num_rec_buf_range: [u32; IC_NUM_SIZE_RANGES],
  /// `SO_RCVBUF` as the system actually set it.
  pub used_tcp_receive_buffer_size: i32,
  /// `SO_SNDBUF` as the system actually set it.
  pub used_tcp_send_buffer_size: i32,
  /// `TCP_MAXSEG` as the system actually set it.
  pub used_tcp_maxseg_size: u32,
}

struct ConnectStat {
  num_sent_buffers: AtomicU64,
  num_sent_bytes: AtomicU64,
  num_rec_buffers: AtomicU64,
  num_rec_bytes: AtomicU64,
  num_send_errors: AtomicU64,
  num_send_timeouts: AtomicU64,
  num_rec_errors: AtomicU64,
  num_sent_buf_range: [AtomicU32; IC_NUM_SIZE_RANGES],
  num_rec_buf_range: [AtomicU32; IC_NUM_SIZE_RANGES],
}

impl ConnectStat {
  fn new() -> ConnectStat {
    ConnectStat {
      num_sent_buffers: AtomicU64::new(0),
      num_sent_bytes: AtomicU64::new(0),
      num_rec_buffers: AtomicU64::new(0),
      num_rec_bytes: AtomicU64::new(0),
      num_send_errors: AtomicU64::new(0),
      num_send_timeouts: AtomicU64::new(0),
      num_rec_errors: AtomicU64::new(0),
      num_sent_buf_range: std::array::from_fn(|_| AtomicU32::new(0)),
      num_rec_buf_range: std::array::from_fn(|_| AtomicU32::new(0)),
    }
  }
}

/// Which size range a message of `size` bytes belongs to: 0 for 0 to 31
/// bytes, 1 for 32 to 63, and so on, the last range holding everything
/// from 512 kBytes up.
pub fn size_range(size: u64) -> usize {
  let mut range: usize = 0;
  let mut limit: u64 = 32;
  while range < IC_NUM_SIZE_RANGES - 1 {
    if size < limit {
      return range;
    }
    limit *= 2;
    range += 1;
  }
  IC_NUM_SIZE_RANGES - 1
}

/// A connected TCP socket.
pub struct Connection {
  stream: TcpStream,
  stat: ConnectStat,
  connected: AtomicBool,
  server_name: String,
  server_port: u16,
  local_port: u16,
  used_tcp_receive_buffer_size: i32,
  used_tcp_send_buffer_size: i32,
  used_tcp_maxseg_size: u32,
  created_at: time::IcTimer,
}

fn resolve(host: &str, port: u16) -> Result<Vec<SocketAddr>, IcError> {
  let target = format!("{}:{}", host, port);
  match target.to_socket_addrs() {
    Ok(list) => {
      let mut addrs: Vec<SocketAddr> = Vec::new();
      for addr in list {
        addrs.push(addr);
      }
      if addrs.is_empty() {
        return Err(IcError::new(err::IC_ERROR_GETADDRINFO));
      }
      Ok(addrs)
    }
    Err(_) => Err(IcError::new(err::IC_ERROR_GETADDRINFO)),
  }
}

fn set_int_option(
  fd: i32,
  level: i32,
  name: i32,
  value: i32,
) -> Result<(), IcError> {
  // SAFETY: setsockopt with a pointer to a live c_int of the given size.
  let ret = unsafe {
    libc::setsockopt(
      fd,
      level,
      name,
      &value as *const i32 as *const libc::c_void,
      std::mem::size_of::<i32>() as libc::socklen_t,
    )
  };
  if ret < 0 {
    return Err(IcError::last_os_error());
  }
  Ok(())
}

fn get_int_option(fd: i32, level: i32, name: i32) -> i32 {
  let mut value: i32 = 0;
  let mut len = std::mem::size_of::<i32>() as libc::socklen_t;
  // SAFETY: getsockopt writes one c_int into value and the length into
  // len, both of which are live and correctly sized.
  let ret = unsafe {
    libc::getsockopt(
      fd,
      level,
      name,
      &mut value as *mut i32 as *mut libc::c_void,
      &mut len,
    )
  };
  if ret < 0 {
    return 0;
  }
  value
}

impl Connection {
  /// Connect to the configured server, applying the socket options and
  /// binding a local address first if one was given
  /// (`ic_set_up_connection` for the client side).
  pub fn connect(config: &ConnectConfig) -> Result<Connection, IcError> {
    let _dbg = ic_port::debug_entry!("Connection::connect");
    let addrs = resolve(&config.server_name, config.server_port)?;
    let mut last_error = IcError::new(err::IC_ERROR_GETADDRINFO);
    for addr in &addrs {
      match Connection::connect_one(config, addr) {
        Ok(conn) => return Ok(conn),
        Err(e) => last_error = e,
      }
    }
    Err(last_error)
  }

  fn connect_one(
    config: &ConnectConfig,
    addr: &SocketAddr,
  ) -> Result<Connection, IcError> {
    let stream = if config.client_name.is_some() || config.client_port != 0 {
      Connection::connect_bound(config, addr)?
    } else {
      Connection::connect_plain(config, addr)?
    };
    let fd = stream.as_raw_fd();
    ic_port::socket::set_no_sigpipe(fd)?;
    //
    // Nagle's algorithm holds a small write back waiting for more data,
    // which is exactly wrong for a protocol of small signals with a
    // reply expected. A wide area link is the one case where the extra
    // round trips cost more than the delay.
    if !config.is_wan_connection {
      let _ = set_int_option(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, 1);
    }
    if config.tcp_receive_buffer_size > 0 {
      let size = config.tcp_receive_buffer_size as i32;
      let _ = set_int_option(fd, libc::SOL_SOCKET, libc::SO_RCVBUF, size);
    }
    if config.tcp_send_buffer_size > 0 {
      let size = config.tcp_send_buffer_size as i32;
      let _ = set_int_option(fd, libc::SOL_SOCKET, libc::SO_SNDBUF, size);
    }
    if config.tcp_maxseg > 0 {
      let size = config.tcp_maxseg as i32;
      let _ = set_int_option(fd, libc::IPPROTO_TCP, libc::TCP_MAXSEG, size);
    }
    let local_port = match stream.local_addr() {
      Ok(local) => local.port(),
      Err(_) => 0,
    };
    let used_rcv = get_int_option(fd, libc::SOL_SOCKET, libc::SO_RCVBUF);
    let used_snd = get_int_option(fd, libc::SOL_SOCKET, libc::SO_SNDBUF);
    let used_seg = get_int_option(fd, libc::IPPROTO_TCP, libc::TCP_MAXSEG);
    ic_port::debug_print!(
      IC_COMM_LEVEL,
      "Connected to {}:{} from local port {}",
      config.server_name,
      config.server_port,
      local_port
    );
    Ok(Connection {
      stream,
      stat: ConnectStat::new(),
      connected: AtomicBool::new(true),
      server_name: config.server_name.clone(),
      server_port: config.server_port,
      local_port,
      used_tcp_receive_buffer_size: used_rcv,
      used_tcp_send_buffer_size: used_snd,
      used_tcp_maxseg_size: used_seg as u32,
      created_at: time::gethrtime(),
    })
  }

  fn connect_plain(
    config: &ConnectConfig,
    addr: &SocketAddr,
  ) -> Result<TcpStream, IcError> {
    let result = if config.connect_timeout_ms > 0 {
      let timeout = Duration::from_millis(config.connect_timeout_ms as u64);
      TcpStream::connect_timeout(addr, timeout)
    } else {
      TcpStream::connect(addr)
    };
    match result {
      Ok(stream) => Ok(stream),
      Err(e) => {
        if e.kind() == std::io::ErrorKind::TimedOut {
          return Err(IcError::new(err::IC_ERROR_CONNECT_TIMEOUT));
        }
        Err(IcError::from_io(&e))
      }
    }
  }

  /// Connect after binding a local address, which `TcpStream::connect`
  /// cannot do, so the socket is made by hand.
  fn connect_bound(
    config: &ConnectConfig,
    addr: &SocketAddr,
  ) -> Result<TcpStream, IcError> {
    let family = match addr {
      SocketAddr::V4(_) => libc::AF_INET,
      SocketAddr::V6(_) => libc::AF_INET6,
    };
    // SAFETY: socket() takes three integers and returns a descriptor.
    let fd = unsafe { libc::socket(family, libc::SOCK_STREAM, 0) };
    if fd < 0 {
      return Err(IcError::last_os_error());
    }
    let _ = set_int_option(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, 1);
    let local_host = match config.client_name.as_deref() {
      Some(name) => name.to_string(),
      None => match addr {
        SocketAddr::V4(_) => "0.0.0.0".to_string(),
        SocketAddr::V6(_) => "::".to_string(),
      },
    };
    let local_addrs = match resolve(&local_host, config.client_port) {
      Ok(list) => list,
      Err(e) => {
        ic_port::socket::close_socket(fd);
        return Err(e);
      }
    };
    let mut local: Option<SocketAddr> = None;
    for candidate in &local_addrs {
      let same_family = matches!(
        (candidate, addr),
        (SocketAddr::V4(_), SocketAddr::V4(_))
          | (SocketAddr::V6(_), SocketAddr::V6(_))
      );
      if same_family {
        local = Some(*candidate);
      }
    }
    let local = match local {
      Some(l) => l,
      None => {
        ic_port::socket::close_socket(fd);
        return Err(IcError::new(err::IC_ERROR_DIFFERENT_IP_VERSIONS));
      }
    };
    let (storage, len) = sockaddr_of(&local);
    // SAFETY: bind is given a sockaddr_storage filled for this family
    // and its true length.
    let ret = unsafe {
      libc::bind(fd, &storage as *const _ as *const libc::sockaddr, len)
    };
    if ret < 0 {
      let e = IcError::last_os_error();
      ic_port::socket::close_socket(fd);
      return Err(e);
    }
    let (storage, len) = sockaddr_of(addr);
    // SAFETY: as for bind.
    let ret = unsafe {
      libc::connect(fd, &storage as *const _ as *const libc::sockaddr, len)
    };
    if ret < 0 {
      let e = IcError::last_os_error();
      ic_port::socket::close_socket(fd);
      return Err(e);
    }
    // SAFETY: fd is a connected socket this function owns and does not
    // close afterwards; the stream takes ownership of it.
    Ok(unsafe { TcpStream::from_raw_fd(fd) })
  }

  /// The socket, for adding to a poll set (`ic_get_fd`).
  pub fn fd(&self) -> i32 {
    self.stream.as_raw_fd()
  }

  /// The host this connection was made to.
  pub fn server_name(&self) -> &str {
    &self.server_name
  }

  /// The port this connection was made to.
  pub fn server_port(&self) -> u16 {
    self.server_port
  }

  /// The local port in use (`ic_get_port_number`).
  pub fn local_port(&self) -> u16 {
    self.local_port
  }

  /// False once the connection has been closed or has failed
  /// (`ic_is_conn_connected`).
  pub fn is_connected(&self) -> bool {
    self.connected.load(Ordering::Acquire)
  }

  /// How long the connection has been up, in microseconds
  /// (`ic_read_connection_time`).
  pub fn connection_time_micros(&self) -> u64 {
    time::micros_elapsed(self.created_at, time::gethrtime())
  }

  /// Put the socket in non-blocking mode (`ic_set_nonblocking`).
  pub fn set_nonblocking(&self, on: bool) -> Result<(), IcError> {
    ic_port::socket::set_nonblocking(self.fd(), on)
  }

  /// How long a read waits before giving up; `None` means forever
  /// (`ic_set_rec_wait_ms`).
  pub fn set_read_timeout_ms(&self, ms: Option<u32>) -> Result<(), IcError> {
    let timeout = ms.map(|v| Duration::from_millis(v as u64));
    match self.stream.set_read_timeout(timeout) {
      Ok(()) => Ok(()),
      Err(e) => Err(IcError::from_io(&e)),
    }
  }

  /// How long a write waits before giving up; `None` means forever.
  pub fn set_write_timeout_ms(&self, ms: Option<u32>) -> Result<(), IcError> {
    let timeout = ms.map(|v| Duration::from_millis(v as u64));
    match self.stream.set_write_timeout(timeout) {
      Ok(()) => Ok(()),
      Err(e) => Err(IcError::from_io(&e)),
    }
  }

  /// Read whatever has arrived, up to the size of `buf`, and return how
  /// many bytes that was. Zero means the peer closed the connection
  /// (`ic_read_connection`).
  pub fn read(&self, buf: &mut [u8]) -> Result<usize, IcError> {
    let mut stream = &self.stream;
    loop {
      match stream.read(buf) {
        Ok(0) => {
          self.connected.store(false, Ordering::Release);
          return Ok(0);
        }
        Ok(size) => {
          self.stat.num_rec_buffers.fetch_add(1, Ordering::Relaxed);
          self
            .stat
            .num_rec_bytes
            .fetch_add(size as u64, Ordering::Relaxed);
          let range = size_range(size as u64);
          self.stat.num_rec_buf_range[range].fetch_add(1, Ordering::Relaxed);
          return Ok(size);
        }
        Err(e) => {
          // A signal interrupted the call; nothing was read, try again.
          // Returning 0 here would look like the peer closing.
          if e.kind() == std::io::ErrorKind::Interrupted {
            continue;
          }
          if e.kind() == std::io::ErrorKind::WouldBlock
            || e.kind() == std::io::ErrorKind::TimedOut
          {
            return Err(IcError::new(err::IC_ERROR_RECEIVE_TIMEOUT));
          }
          self.stat.num_rec_errors.fetch_add(1, Ordering::Relaxed);
          self.connected.store(false, Ordering::Release);
          return Err(IcError::from_io(&e));
        }
      }
    }
  }

  /// Read whatever has arrived without waiting for more: `None` if
  /// nothing has, otherwise as [`read`](Self::read). Only this call is
  /// made not to wait; the socket stays as it is, so that a write to it
  /// still waits for room rather than failing.
  pub fn read_nowait(&self, buf: &mut [u8]) -> Result<Option<usize>, IcError> {
    loop {
      // SAFETY: `buf` is a live buffer borrowed exclusively for this
      // call, and recv writes at most `buf.len()` bytes into it.
      let ret = unsafe {
        libc::recv(
          self.fd(),
          buf.as_mut_ptr() as *mut libc::c_void,
          buf.len(),
          libc::MSG_DONTWAIT,
        )
      };
      if ret > 0 {
        let size = ret as usize;
        self.stat.num_rec_buffers.fetch_add(1, Ordering::Relaxed);
        self
          .stat
          .num_rec_bytes
          .fetch_add(size as u64, Ordering::Relaxed);
        let range = size_range(size as u64);
        self.stat.num_rec_buf_range[range].fetch_add(1, Ordering::Relaxed);
        return Ok(Some(size));
      }
      if ret == 0 {
        self.connected.store(false, Ordering::Release);
        return Ok(Some(0));
      }
      let e = std::io::Error::last_os_error();
      match e.kind() {
        std::io::ErrorKind::Interrupted => continue,
        std::io::ErrorKind::WouldBlock => return Ok(None),
        _ => {
          self.stat.num_rec_errors.fetch_add(1, Ordering::Relaxed);
          self.connected.store(false, Ordering::Release);
          return Err(IcError::from_io(&e));
        }
      }
    }
  }

  /// Write the whole buffer (`ic_write_connection`).
  pub fn write(&self, buf: &[u8]) -> Result<(), IcError> {
    let mut stream = &self.stream;
    match stream.write_all(buf) {
      Ok(()) => {
        self.stat.num_sent_buffers.fetch_add(1, Ordering::Relaxed);
        self
          .stat
          .num_sent_bytes
          .fetch_add(buf.len() as u64, Ordering::Relaxed);
        let range = size_range(buf.len() as u64);
        self.stat.num_sent_buf_range[range].fetch_add(1, Ordering::Relaxed);
        Ok(())
      }
      Err(e) => {
        if e.kind() == std::io::ErrorKind::WouldBlock
          || e.kind() == std::io::ErrorKind::TimedOut
        {
          self.stat.num_send_timeouts.fetch_add(1, Ordering::Relaxed);
          return Err(IcError::new(err::IC_ERROR_RECEIVE_TIMEOUT));
        }
        self.stat.num_send_errors.fetch_add(1, Ordering::Relaxed);
        self.connected.store(false, Ordering::Release);
        Err(IcError::from_io(&e))
      }
    }
  }

  /// Write several buffers in one system call, which is how a signal
  /// header and its sections go out without being copied together first
  /// (`ic_writev_connection`).
  pub fn write_vectored(&self, bufs: &[IoSlice<'_>]) -> Result<(), IcError> {
    let mut total: usize = 0;
    for buf in bufs {
      total += buf.len();
    }
    if total == 0 {
      return Ok(());
    }
    let mut stream = &self.stream;
    //
    // A short write leaves part of one buffer and some whole buffers to
    // go. Rather than editing the slice list in place, which would mean
    // holding a reference into it while writing it, the remainder is
    // described by which buffer we are in and how far into it, and the
    // list is rebuilt from the caller's buffers. Short writes are rare,
    // so the rebuild costs nothing in practice.
    let mut buf_index: usize = 0;
    let mut buf_offset: usize = 0;
    while buf_index < bufs.len() {
      let mut slices: Vec<IoSlice<'_>> = Vec::with_capacity(bufs.len());
      slices.push(IoSlice::new(&bufs[buf_index][buf_offset..]));
      for slice in &bufs[buf_index + 1..] {
        slices.push(*slice);
      }
      let size = match stream.write_vectored(&slices) {
        Ok(0) => {
          self.stat.num_send_errors.fetch_add(1, Ordering::Relaxed);
          self.connected.store(false, Ordering::Release);
          return Err(IcError::new(err::IC_END_OF_FILE));
        }
        Ok(n) => n,
        Err(e) => {
          if e.kind() == std::io::ErrorKind::Interrupted {
            continue;
          }
          if e.kind() == std::io::ErrorKind::WouldBlock
            || e.kind() == std::io::ErrorKind::TimedOut
          {
            self.stat.num_send_timeouts.fetch_add(1, Ordering::Relaxed);
            return Err(IcError::new(err::IC_ERROR_RECEIVE_TIMEOUT));
          }
          self.stat.num_send_errors.fetch_add(1, Ordering::Relaxed);
          self.connected.store(false, Ordering::Release);
          return Err(IcError::from_io(&e));
        }
      };
      /* Step forward over what was written. */
      let mut left = size;
      while left > 0 && buf_index < bufs.len() {
        let in_this_buf = bufs[buf_index].len() - buf_offset;
        if left < in_this_buf {
          buf_offset += left;
          left = 0;
        } else {
          left -= in_this_buf;
          buf_index += 1;
          buf_offset = 0;
        }
      }
    }
    self.stat.num_sent_buffers.fetch_add(1, Ordering::Relaxed);
    self
      .stat
      .num_sent_bytes
      .fetch_add(total as u64, Ordering::Relaxed);
    let range = size_range(total as u64);
    self.stat.num_sent_buf_range[range].fetch_add(1, Ordering::Relaxed);
    Ok(())
  }

  /// True if a read would return data now, waiting up to `wait_ms`
  /// (`ic_check_for_data`).
  pub fn check_for_data(&self, wait_ms: i32) -> bool {
    let mut poll_fd = libc::pollfd {
      fd: self.fd(),
      events: libc::POLLIN,
      revents: 0,
    };
    // SAFETY: poll is given one live pollfd and a count of one.
    let ret = unsafe { libc::poll(&mut poll_fd, 1, wait_ms) };
    ret > 0 && (poll_fd.revents & libc::POLLIN) != 0
  }

  /// Close the connection (`ic_close_connection`).
  pub fn close(&self) {
    if !self.connected.swap(false, Ordering::AcqRel) {
      return;
    }
    let _ = self.stream.shutdown(std::net::Shutdown::Both);
    ic_port::debug_print!(
      IC_COMM_LEVEL,
      "Closed connection to {}:{}",
      self.server_name,
      self.server_port
    );
  }

  /// A copy of the statistics (`ic_read_stat_connection`).
  pub fn read_stat(&self) -> ConnectStatSnapshot {
    let mut snapshot = ConnectStatSnapshot {
      num_sent_buffers: self.stat.num_sent_buffers.load(Ordering::Relaxed),
      num_sent_bytes: self.stat.num_sent_bytes.load(Ordering::Relaxed),
      num_rec_buffers: self.stat.num_rec_buffers.load(Ordering::Relaxed),
      num_rec_bytes: self.stat.num_rec_bytes.load(Ordering::Relaxed),
      num_send_errors: self.stat.num_send_errors.load(Ordering::Relaxed),
      num_send_timeouts: self.stat.num_send_timeouts.load(Ordering::Relaxed),
      num_rec_errors: self.stat.num_rec_errors.load(Ordering::Relaxed),
      num_sent_buf_range: [0; IC_NUM_SIZE_RANGES],
      num_rec_buf_range: [0; IC_NUM_SIZE_RANGES],
      used_tcp_receive_buffer_size: self.used_tcp_receive_buffer_size,
      used_tcp_send_buffer_size: self.used_tcp_send_buffer_size,
      used_tcp_maxseg_size: self.used_tcp_maxseg_size,
    };
    let mut i: usize = 0;
    while i < IC_NUM_SIZE_RANGES {
      snapshot.num_sent_buf_range[i] =
        self.stat.num_sent_buf_range[i].load(Ordering::Relaxed);
      snapshot.num_rec_buf_range[i] =
        self.stat.num_rec_buf_range[i].load(Ordering::Relaxed);
      i += 1;
    }
    snapshot
  }
}

impl std::fmt::Debug for Connection {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "Connection({}:{} from port {}, {})",
      self.server_name,
      self.server_port,
      self.local_port,
      if self.is_connected() {
        "connected"
      } else {
        "closed"
      }
    )
  }
}

impl Drop for Connection {
  fn drop(&mut self) {
    self.close();
  }
}

/// Turn a `SocketAddr` into the `sockaddr` the system calls want.
fn sockaddr_of(addr: &SocketAddr) -> (libc::sockaddr_storage, libc::socklen_t) {
  // SAFETY: a zeroed sockaddr_storage is a valid starting point; the
  // fields for the right family are filled in below.
  let mut storage: libc::sockaddr_storage = unsafe { std::mem::zeroed() };
  match addr {
    SocketAddr::V4(v4) => {
      let len = std::mem::size_of::<libc::sockaddr_in>();
      // SAFETY: storage is large enough for a sockaddr_in and we only
      // write the fields of that type into it.
      let sa = unsafe {
        &mut *(&mut storage as *mut libc::sockaddr_storage
          as *mut libc::sockaddr_in)
      };
      sa.sin_family = libc::AF_INET as libc::sa_family_t;
      sa.sin_port = v4.port().to_be();
      sa.sin_addr.s_addr = u32::from_ne_bytes(v4.ip().octets());
      (storage, len as libc::socklen_t)
    }
    SocketAddr::V6(v6) => {
      let len = std::mem::size_of::<libc::sockaddr_in6>();
      // SAFETY: as above for sockaddr_in6.
      let sa = unsafe {
        &mut *(&mut storage as *mut libc::sockaddr_storage
          as *mut libc::sockaddr_in6)
      };
      sa.sin6_family = libc::AF_INET6 as libc::sa_family_t;
      sa.sin6_port = v6.port().to_be();
      sa.sin6_addr.s6_addr = v6.ip().octets();
      sa.sin6_flowinfo = v6.flowinfo();
      sa.sin6_scope_id = v6.scope_id();
      (storage, len as libc::socklen_t)
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::net::TcpListener;

  fn listener() -> (TcpListener, u16) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("addr").port();
    (listener, port)
  }

  fn config_for(port: u16) -> ConnectConfig {
    ConnectConfig {
      server_name: "127.0.0.1".to_string(),
      server_port: port,
      connect_timeout_ms: 2000,
      ..ConnectConfig::default()
    }
  }

  #[test]
  fn connect_write_read_close() {
    let (listener, port) = listener();
    let handle = std::thread::spawn(move || {
      let (mut peer, _) = listener.accept().expect("accept");
      let mut buf = [0u8; 5];
      peer.read_exact(&mut buf).expect("read");
      assert_eq!(&buf, b"hello");
      peer.write_all(b"world!").expect("write");
      peer.flush().expect("flush");
    });
    let conn = Connection::connect(&config_for(port)).expect("connect");
    assert!(conn.is_connected());
    assert_eq!(conn.server_port(), port);
    assert_eq!(conn.server_name(), "127.0.0.1");
    assert!(conn.local_port() > 0);
    assert!(conn.fd() > 0);
    conn.write(b"hello").expect("write");
    let mut buf = [0u8; 6];
    let mut got: usize = 0;
    while got < 6 {
      got += conn.read(&mut buf[got..]).expect("read");
    }
    assert_eq!(&buf, b"world!");
    let stat = conn.read_stat();
    assert_eq!(stat.num_sent_buffers, 1);
    assert_eq!(stat.num_sent_bytes, 5);
    assert_eq!(stat.num_rec_bytes, 6);
    assert_eq!(stat.num_send_errors, 0);
    assert_eq!(stat.num_sent_buf_range[0], 1);
    conn.close();
    assert!(!conn.is_connected());
    handle.join().expect("join");
  }

  #[test]
  fn write_vectored_sends_everything() {
    let (listener, port) = listener();
    let handle = std::thread::spawn(move || {
      let (mut peer, _) = listener.accept().expect("accept");
      let mut buf = Vec::new();
      peer.read_to_end(&mut buf).expect("read");
      assert_eq!(buf, b"headerbodytail".to_vec());
    });
    let conn = Connection::connect(&config_for(port)).expect("connect");
    let bufs = [
      IoSlice::new(b"header"),
      IoSlice::new(b"body"),
      IoSlice::new(b"tail"),
    ];
    conn.write_vectored(&bufs).expect("writev");
    conn.close();
    handle.join().expect("join");
  }

  #[test]
  fn closed_peer_reads_zero() {
    let (listener, port) = listener();
    let handle = std::thread::spawn(move || {
      let (peer, _) = listener.accept().expect("accept");
      drop(peer);
    });
    let conn = Connection::connect(&config_for(port)).expect("connect");
    let mut buf = [0u8; 8];
    let mut spins = 0;
    let mut size = 1;
    while size != 0 && spins < 100 {
      size = conn.read(&mut buf).expect("read");
      spins += 1;
    }
    assert_eq!(size, 0);
    assert!(!conn.is_connected());
    handle.join().expect("join");
  }

  #[test]
  fn connect_to_nothing_fails() {
    /* Port 1 on the loopback interface has nothing listening. */
    let mut config = config_for(1);
    config.connect_timeout_ms = 500;
    assert!(Connection::connect(&config).is_err());
    let mut config = config_for(1186);
    config.server_name = "no.such.host.invalid".to_string();
    let err = Connection::connect(&config).expect_err("should fail");
    assert_eq!(err.code, err::IC_ERROR_GETADDRINFO);
  }

  #[test]
  fn read_timeout_is_reported() {
    let (listener, port) = listener();
    let handle = std::thread::spawn(move || {
      let (peer, _) = listener.accept().expect("accept");
      ic_port::time::microsleep(200_000);
      drop(peer);
    });
    let conn = Connection::connect(&config_for(port)).expect("connect");
    conn.set_read_timeout_ms(Some(20)).expect("timeout");
    assert!(!conn.check_for_data(10));
    let mut buf = [0u8; 4];
    let result = conn.read(&mut buf);
    assert_eq!(result, Err(IcError::new(err::IC_ERROR_RECEIVE_TIMEOUT)));
    handle.join().expect("join");
  }

  #[test]
  fn size_ranges() {
    assert_eq!(size_range(0), 0);
    assert_eq!(size_range(31), 0);
    assert_eq!(size_range(32), 1);
    assert_eq!(size_range(63), 1);
    assert_eq!(size_range(64), 2);
    assert_eq!(size_range(1 << 20), IC_NUM_SIZE_RANGES - 1);
  }
}
