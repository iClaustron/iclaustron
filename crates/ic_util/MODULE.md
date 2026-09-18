# ic_util — module notes

The containers and helpers the rest of the library is built from. See
`doc/rust/03-module-map.md` for the workspace-wide mapping.

## Source files translated

| C file (legacy-c/) | Rust module | Mode | Status |
|---|---|---|---|
| `util/ic_mc.c`, `include/ic_mc.h` | `mc.rs` | Redesign | done |
| `util/ic_bitmap.c`, `include/ic_bitmap.h` | `bitmap.rs` | 1:1 | done |
| `util/ic_dyn_array.c` (simple array) | `dyn_array.rs` | Redesign | done |
| `util/ic_dyn_array.c` (pointer array) | `ptr_array.rs` | Redesign | done |
| `util/ic_parse_connectstring.c` | `connectstring.rs` | 1:1 + New | done |
| `util/ic_string.c` (number and text helpers) | `string.rs` | Redesign | done |
| `util/ic_threadpool.c`, `include/ic_threadpool.h` | `threadpool.rs` | 1:1 | done |
| `util/ic_err.c`, `util/ic_debug.c` | — | Moved | in `ic_port`; the C port layer used them, and a crate cannot depend upwards |
| `util/ic_hashtable.c`, `_itr.c` | — | Drop | `std::collections::HashMap` |
| `util/ic_string.c` (directory layout) | — | Out | cluster server and process controller |
| `util/ic_dyn_array.c` (ordered array, disk spill) | — | Out | configuration writer |
| `util/ic_config_reader.c` | — | Out | no configuration files are read |
| `util/ic_lex_support.c` | — | Out | served the bison parsers |
| `util/ic_hw_info.c` | — | Out | process controller |
| `util/ic_readline.c` | — | Out | cluster client |
| `util/ic_util.c` | — | Moved | held only the debug globals |

## Deviations from the C code

- **The memory container hands out handles, not pointers.** `mc_alloc`
  returned a `gchar*` the caller stored in a struct. Rust cannot hand out
  a pointer into a buffer the container may later free or move, so
  `alloc` returns an `McHandle` (buffer index, offset, length) and
  `bytes`/`bytes_mut` turn it back into a slice. Using a handle after a
  `reset` cannot corrupt memory: it aborts a debug build and yields an
  empty slice otherwise. The `use_mutex` flag is gone; a shared container
  is wrapped in an `IcMutex` by whoever shares it.
- **Allocations are always zeroed**, so there is no separate `mc_calloc`.
  The C distinguished the two; the cost is a memset the container would
  do on reset anyway.
- **The object map carries generations.** The C `IC_DYNAMIC_PTR_ARRAY`
  returned whatever object occupied a slot, so a reply that arrived after
  its operation was freed would be delivered to the operation that had
  taken its place. An id is now a 24-bit index plus an 8-bit generation
  in one 32-bit word, and a stale id fails to match. A valid id is never
  the word 0, so 0 serves as "no object" the way `RNIL` does. The
  generation wraps at 256 reuses of one slot, which needs a reply to be
  outstanding across 256 operations on that slot to alias.
- **The dynamic array is one `Vec<u8>`.** The C chained fixed 1 kByte
  buffers so earlier pointers stayed valid; nothing holds pointers into
  it here. Its ordered variant with an index tree existed to spill to
  disk for the configuration writer and is out of scope.
- **`IC_STRING` is not translated.** It was a pointer, a length and a
  flag saying whether the text was NUL terminated; `&str` and `String`
  are exactly that, with the length always known and the flag always
  true. What survives in `string.rs` is the handful of helpers with no
  standard equivalent.
- **The hash table is dropped.** `ic_hashtable.c` was Christopher Clark's
  chaining table carrying its own BSD notice. `std::collections::HashMap`
  replaces it at the call sites, and the third-party copyright goes with
  it.
- **The thread pool takes a boxed closure**, not a function pointer and a
  `void*`. See the note below.
- **No `ic_get_threadpool` from thread-local storage.** A thread receives
  its `ThreadState` as an argument; nothing needs to reach back to the
  pool.

## Rust notes for C readers

- **`Box<dyn FnOnce(&ThreadState) + Send + 'static>`** is the type of a
  thread body. `Box<...>` is a heap allocation with one owner, `dyn` says
  the exact function is only known at run time, `FnOnce` says it runs
  once, `Send` says it may cross to another thread and `'static` that it
  borrows nothing that could go away. A caller writes
  `Box::new(move |state| { ... })`: `move` copies what the closure needs
  into it, usually an `Arc` of shared state. This is the one place in the
  library that takes a closure, because it is how a Rust thread receives
  anything at all; every other callback is a plain function pointer with
  a user reference, as in the C.
- **`Arc<T>`** is a reference-counted pointer that is safe to share
  between threads: each clone bumps a counter, the value is dropped when
  the last clone goes. The pool keeps one `Arc<ThreadState>` and the
  thread gets another.
- **`AtomicBool` with `Ordering`.** A plain `bool` cannot be written by
  one thread and read by another; an atomic can. `Acquire` on a read and
  `Release` on a write mean that what the writer did before the store is
  visible to a reader that sees it, which is what the C relied on its
  `volatile` and mutexes for.
- **`Option<&T>` returned by a lookup** replaces a pointer that might be
  NULL. `match map.get(id) { Some(x) => ..., None => ... }` is the null
  check the compiler insists on.
- **A handle instead of a pointer.** `McHandle` and `PtrId` are small
  copyable values naming something inside a container. They behave like
  pointers, but the container can check them, and passing a stale one
  gives a clean failure rather than reading freed memory.

## Open items

- `mc::MemoryContainer::reset` keeps one buffer, as the C did, but always
  the first one rather than the first buffer of base size that the C
  tracked through `first_buf_inx`. Behaviour is the same; the bookkeeping
  is simpler.
- The thread pool has no equivalent of `ic_threadpool_get_thread_state`
  for a thread that has already stopped: the slot is cleared when it is
  joined. Nothing needs it yet.
- `sleep_seconds` checks the process stop flag once a second. A thread
  that must react faster uses `ThreadState::wait_timeout`.
