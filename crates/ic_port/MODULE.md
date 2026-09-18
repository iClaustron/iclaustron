# ic_port — module notes

The portability layer: everything that touches the operating system, and
everything glib used to provide. No other crate calls `libc` for these
services. See `doc/rust/03-module-map.md` for the workspace-wide mapping.

## Source files translated

| C file (legacy-c/) | Rust module | Mode | Status |
|---|---|---|---|
| `include/ic_base_header.h` (constants) | `consts.rs` | 1:1 | done, values updated for RonDB 26.10 |
| `include/ic_err.h`, `util/ic_err.c` | `err.rs` | 1:1 | done |
| `port/ic_port.c` stop flag | `stop.rs` | 1:1 | done |
| `port/ic_port.c` error routines | `oserr.rs` | 1:1 | done |
| `port/ic_port.c` byte order | `endian.rs` | 1:1 | done |
| `port/ic_port.c` timers and sleeps | `time.rs` | 1:1 | done |
| `util/ic_debug.c` (output half) | `output.rs` | 1:1 | done |
| `include/ic_debug.h`, `util/ic_debug.c` | `debug.rs` | Redesign | done |
| `port/ic_port.c` mutex/cond wrappers | `sync.rs` | Redesign | done |
| `port/ic_port.c` file routines | `file.rs` | 1:1 | done |
| `port/ic_port.c` socket routines | `socket.rs` | 1:1 | done |
| `port/ic_port.c` daemon, pid, signals | `daemon.rs` | 1:1 | done |
| glib `GOptionEntry`/`GOptionContext` | `options.rs` | New | done |
| `port/ic_port.c` allocators | — | Drop | Rust owns memory; see below |
| `port/ic_port.c` process start/stop | — | Out | process controller only |
| `port/ic_port.c` `ic_get_hw_info` | — | Out | process controller only |
| Windows branches | — | Out | not supported |

## Deviations from the C code

- **Allocators dropped.** `ic_malloc`, `ic_calloc`, `ic_malloc_conn`,
  `ic_calloc_mc`, `ic_malloc_hash` and their `ic_free*` partners existed
  to attribute leaks to a module in debug builds. Rust frees owned values
  automatically, so the whole family and its leak-tracking hash tables are
  gone. If per-module byte counters are ever wanted for a `SHOW MEMORY`
  style command, they go here as an `AllocStats` type.
- **Mutex ordering enforced.** `IC_MUTEX` was a bare `GMutex` and the
  ordering levels lived only in `api/ic_api_mutex_doc.txt`, where the
  "protected variables" sections were never filled in. `IcMutex` now
  carries its level, and a debug build aborts when a thread locks a mutex
  of a level at or below one it already holds. The levels are the table in
  `doc/rust/02-architecture.md`.
- **The mutex owns its data.** `IcMutex<T>` holds the protected value, so
  there is no way to read the data without locking. The C style of a
  `GMutex` sitting next to the fields it protects cannot be expressed.
- **`DEBUG_RETURN_*` replaced by a guard.** The C convention was that no
  function writes a bare `return`; every exit goes through
  `DEBUG_RETURN_INT`/`DEBUG_RETURN_PTR`/`DEBUG_RETURN_EMPTY` so the
  indent level stays right. In Rust `let _dbg = debug_entry!("name");`
  prints the entry immediately and the exit when the guard is dropped,
  which happens on every path including `?` early returns. Ordinary
  `return` is therefore allowed again, and `ret_int` is kept only for
  code that wants the return value in the trace.
- **Debug thread ids are recycled.** The C code scanned a
  `IC_MAX_THREADS`-sized array (1M entries in debug builds) for a free
  slot. This keeps a free list instead.
- **No `ic_port_init`/`ic_port_end`.** Nothing needs explicit
  initialisation: the statics are const-initialised and the debug file is
  opened by `debug::open`.
- **`ic_spin_lock` dropped.** It was `#define IC_SPINLOCK GMutex`, i.e.
  the same mutex under another name. Call sites use `IcMutex`.
- **Signal handling is slightly stricter.** The C installed the error
  handler only in non-debug builds; here it is always available and the
  registered handler runs before the abort in every build.
- **`c_int` handler storage.** Handler function pointers live in
  `AtomicUsize` because a signal can arrive on any thread; the C used
  plain globals.

## glib replacements

| glib | Replacement |
|---|---|
| `g_mutex_*`, `g_cond_*` | `sync::IcMutex`, `sync::IcCond` over `std::sync` |
| `g_private_get/set` | `thread_local!` in `debug.rs`, `sync.rs` |
| `g_timer_*`, `g_get_monotonic_time` | `time.rs` over `std::time::Instant` |
| `g_ntohl`, `g_htonl` | `endian::read_be_u32`, `endian::write_be_u32` |
| `g_snprintf` | `format!`, `write!` |
| `g_try_malloc`, `g_free` | ownership (`Box`, `Vec`, `String`) |
| `g_option_context_*`, `GOptionEntry` | `options::OptionParser` |
| `g_unlink`, `g_mkdir` | `file.rs` over `std::fs` |
| `g_usleep` | `time::microsleep` |
| `g_assert` | `ic_assert!`, `ic_require!` |

## Rust notes for C readers

Beyond `doc/rust/11-c-to-rust-mappings.md`:

- **`IcMutex<T>` and its guard.** `let mut g = m.lock();` gives a guard;
  `*g` is the protected value (`g.field` for a struct). The lock is
  released when `g` goes out of scope, so there is no `unlock` to forget.
  To release early, `drop(g)`.
- **`IcCond::wait` takes and returns the guard.** In C you pass the mutex
  and the condition to `ic_cond_wait`. Here the guard is handed to
  `wait`, which releases the lock while it sleeps and gives the guard
  back when it wakes: `g = cond.wait(g);`.
- **`Option<T>` instead of a null pointer.** `open_file` returns
  `Result<File, IcError>` rather than writing through a handle pointer and
  returning an `int`; `?` propagates the error exactly as `goto error`
  did.
- **`&'static str` is a string constant.** Error texts and option names
  are `&'static str`, which is a pointer to fixed text plus a length; they
  can be copied freely without owning anything.
- **`thread_local!` with `RefCell`.** Rust forbids shared mutable state
  by default. A thread-local is private to one thread, but the compiler
  still asks for the `RefCell` wrapper to allow mutation through a shared
  reference. `THREAD.with(|cell| ...)` borrows it for the length of the
  closure, like taking the value out of `g_private_get` and putting it
  back.
- **`static X: AtomicU32`.** A mutable global in Rust must be an atomic
  (or behind a mutex). `load`/`store` with an `Ordering` replace a plain
  read and write of a `volatile guint32`.
- **The `libc` crate** is the raw C library: `libc::fork`, `libc::signal`,
  `libc::O_SYNC`. Calls into it are `unsafe` and every block here carries
  a `SAFETY:` comment saying why it is sound.

## Feature flags

- `debug_build`: compiles the `debug_entry!` and `debug_print!` bodies in.
  Without it they expand to a constant `false` test that the optimiser
  removes, exactly as a C build without `-DDEBUG_BUILD`.

## Open items

- **`errno` and NDB error codes share the number space.** `IcError` holds
  a bare code, as the C `int` return did, and nothing in this crate can
  tell error 626 the NDB "no such row" from error 626 the C library
  number. `IcError::message` asks the C library for anything outside the
  iClaustron range. The Data API knows which domain a code came from and
  translates NDB codes with its own table before the application sees
  them, so the ambiguity stops at this layer. Revisit if it ever leaks.
- Code 7079 means "No such error" and unknown codes now read "Unknown
  error code"; the C code used the same text for both.
- `endian::swap_endian_word` is unused so far; the NDB signal protocol is
  sender-endian and the receiver rejects a mismatch, so it may only ever
  serve the configuration blob. Keep until `ic_apic` is written.
- `socket::IC_SEND_FLAGS` and `set_no_sigpipe` are the two halves of the
  same problem on Linux and macOS; `ic_comm` must use both.
- No test covers `daemonize`, which forks. It needs an integration test
  that runs a helper binary.
