# ic_comm — module notes

Everything between a socket and the protocol layers above it. See
`doc/rust/03-module-map.md` for the workspace-wide mapping.

## Source files translated

| C file (legacy-c/) | Rust module | Mode | Status |
|---|---|---|---|
| `comm/ic_sock_buf.c`, `include/ic_sock_buf.h` | `sock_buf.rs` | 1:1 | done |
| `comm/ic_poll_set.c`, `include/ic_poll_set.h` | `poll_set.rs` | 1:1 | done |
| `comm/ic_connection.c` (client half) | `connection.rs` | Redesign | done |
| `comm/ic_protocol_support.c` | `line_proto.rs` | 1:1 | done |
| `comm/ic_connection.c` (server half) | — | Out | an API node only connects out |
| `include/ic_ssl.h` | — | Later | TLS in release 0.3 |

## Deviations from the C code

- **No server half.** Listen, accept, `fork_accept_connection`, the
  connect thread and the "only this client may connect" check served the
  cluster server and the inward-connecting case. An API node dials out to
  the management server and to each data node, so the whole listening
  side is gone.
- **A connection is created connected.** The C made an object, then
  prepared it, then called `set_up_connection`. Here
  `Connection::connect(&ConnectConfig)` does all three and either hands
  back a live connection or an error, so there is no half-built state to
  reason about.
- **Read and write take `&self`.** The C had read and write sessions
  guarded by a mutex so that a sending thread and a receiving thread
  could share one socket. The operating system already allows that, so
  the sessions are gone; an `Arc<Connection>` is held by both threads and
  the statistics are counted in atomics, so neither waits for the other.
- **Statistics lost their square sums.** The C accumulated the sum of
  squares of message sizes so a standard deviation could be printed.
  Nothing consumed it, and `long double` has no Rust equivalent worth
  introducing. The counts, the byte totals, the error and timeout counts
  and the sixteen size ranges are all kept.
- **Short writes are handled without editing the buffer list.** A
  `write_vectored` that writes only part of its buffers leaves a
  remainder. Rather than trim the slice list in place, which would need
  a reference into the list while writing it, the remainder is recorded
  as a buffer index and an offset and the list is rebuilt from the
  caller's buffers. Short writes are rare, so nothing is lost.
- **An interrupted read retries.** The C returned to its caller on
  `EINTR`; returning zero here would be indistinguishable from the peer
  closing, so the read is simply repeated.
- **The Solaris event ports backend is dropped**, leaving `epoll` and
  `kqueue`. The poll set's 1024 socket ceiling became a parameter that
  still defaults to 1024.
- **The line reader is separate from the connection.** The C kept the
  read buffer inside the connection object. Keeping it outside is what
  allows one thread to read and another to write with no lock.

## Rust notes for C readers

- **`&self` on read and write.** A method taking `&self` may be called
  from several threads at once, which is what lets the send thread and
  the receive thread share one `Connection`. A method taking `&mut self`
  may not. The compiler enforces the distinction, so the mutex the C
  needed for its sessions is not needed here.
- **`IoSlice`** is the Rust name for `struct iovec`: a pointer and a
  length, used to hand several buffers to one `writev` call.
- **`AtomicU64::fetch_add`** is an increment that several threads may do
  at once without a lock, replacing the counters the C protected with a
  mutex.
- **`#[cfg(target_os = "linux")]`** on a module is `#ifdef LINUX` around
  it, except the compiler checks both branches parse and only compiles
  one.
- **`Box<SockBufPage>`** is a page on the heap with exactly one owner at
  a time. Passing the box to another thread passes the page; the
  compiler will not let the sender keep using it.
- **`unsafe`** appears where the system calls do: `epoll`, `kqueue`,
  `socket`, `bind`, `connect`, `setsockopt`, `poll`. Each block carries a
  `SAFETY:` comment saying why the pointers and lengths it passes are
  right.

## Open items

- `ConnectConfig::is_wan_connection` only decides whether to set
  `TCP_NODELAY`. The C also adjusted buffer sizes from it; what RonDB
  26.10 wants here should be checked against the configuration
  parameters in Phase 2.
- Nothing yet calls `Connection::write_vectored`. It exists because the
  transporter will send a signal header and its sections without copying
  them together first.
- `line_proto::MAX_LINE_LEN` is 512 to match the management server's own
  parser. Verify against RonDB 26.10 `mgmapi.cpp` when Phase 2 starts.
