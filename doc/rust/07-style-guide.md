# 07 — Style guide: C-like Rust

The test for every construct is: *can a competent C programmer who has read
one page of Rust syntax follow this without looking anything up?* If not,
rewrite it. Performance is not an excuse; nothing below costs performance.

## Allowed and encouraged

| Rust feature | Use | Why |
|---|---|---|
| `struct` with named fields | everywhere | same as C |
| `enum` with unit variants, `#[repr(u32)]` | constants, states, GSNs | same as C `enum`, but exhaustive `match` catches missing cases |
| `enum` with data | only for `Result`/`Option` and a few clearly documented message types | tagged unions are familiar from C `struct {tag; union}` |
| `impl Type { fn method(&mut self) }` | replaces the `_OPS` vtables | `conn.read_key(...)` reads like `conn->ops->ic_read_key(conn, ...)` |
| `Result<T, IcError>` and `?` | all fallible functions | `?` is "return on error"; one token, one meaning |
| `Option<T>` | nullable values | replaces `NULL` checks with something the compiler enforces |
| `match` | dispatch on enums/integers | same as `switch` without fall-through bugs |
| Small `Option`/`Result` methods | `map`, `unwrap_or`, `unwrap_or_default`, `as_deref`, `is_none`, `take`, `ok_or` | these act on one value, not a collection; clippy requires them and they are shorter than the `match` they replace |
| A single question about a collection | `.iter().find(...)`, `.any(...)`, `.all(...)`, `.position(...)`, `.count()` | one call that answers a question, not a pipeline that builds something; clippy requires these too |
| `for i in 0..n`, `while`, `loop` | all iteration | explicit loops, no adaptor chains |
| `&[T]`, `&mut [T]`, `Vec<T>` | buffers | pointer+length pairs with bounds checks |
| `Box<T>` | heap objects with one owner | `malloc`+`free` handled by the compiler |
| `Arc<T>`, `Mutex<T>`, `Condvar` | shared state between threads, at the documented mutex levels only | the C code used GMutex/GCond the same way |
| `std::thread::spawn` | send/receive/user threads | `pthread_create` |
| `pub(crate)` | share items across files in a crate | this is what the `.ic` amalgamation achieved in C |
| `const`, `static` | constants and tables | same as C |
| `#[repr(C)]` | any struct that crosses the FFI or the wire | layout must be C layout |
| `unsafe` blocks | FFI, `libc` calls, byte/word reinterpretation | must be small, commented, and inside the `port`, `comm` or `capi` crates or the signal codec |
| `#[cfg(target_os = ...)]` | port layer only | replaces `#ifdef LINUX`; only `linux` and `macos` |
| Doc comments `///` | every public item | same discipline as the C header comments |

## Forbidden in library code

- **Generics with trait bounds** (`fn f<T: Trait>(x: T)`), `where` clauses,
  `impl Trait` in argument or return position. Write the concrete type. If
  two types need the same function, write it twice or use an enum.
- **Traits** except: (a) the standard derives (`Debug`, `Clone`, `Copy`,
  `Default`, `PartialEq`), (b) one trait per pluggable backend where more
  than one implementation exists today (`Transport`: plain TCP and TLS), (c)
  what `std` forces (`Drop`, `Error`, `Display`).
- **`dyn Trait`** except for the `Transport` backend above and the
  user-callback slot in the Rust convenience layer.
- **Lifetimes in public signatures** other than elided ones. The public API
  works with owned handles and indices, as the C API does. Internal
  functions may take `&`/`&mut` freely; that is C pointers with a checker.
- **Closures** in library internals. Callbacks are `fn` pointers plus a
  `user_ref: *mut c_void` (C API) or `usize` (Rust API), exactly as in
  `IC_APID_CALLBACK_FUNC`.
- **Iterator adaptor chains over collections** (`.iter().map().filter()
  .collect()`). Write one `for` loop.

  The line is between building something and asking something. Two or
  more adaptors that transform a collection into another collection are
  a pipeline: write the loop. One call that answers a question about a
  collection (`find`, `any`, `all`, `position`, `count`) or that acts on
  a single `Option` or `Result` (`map`, `unwrap_or`) is allowed, reads
  as what it is, and is what clippy insists on.
- **Macros** other than: `debug_entry!`/`debug_return!` (port of
  `DEBUG_ENTRY`/`DEBUG_RETURN`), `ic_assert!`, and the `bitflags`-free
  constant tables. No `macro_rules!` that generates types or impls.
- **`async`/`await`**. The threading model is explicit threads plus
  `poll()`; that is the whole point of the design.
- **`Rc`, `RefCell`, `Cell`**. Single-threaded interior mutability hides
  ownership. Use `&mut`.
- **Operator overloading**, `Deref` tricks, `From`/`Into` conversions
  except `From<IcError>` for error-code mapping.
- **Pattern matching deeper than one level** and `if let` chains longer
  than one `else if`.
- **Shadowing** (`let x = ...; let x = ...;`). One name, one meaning per
  scope, as in C.
- **`unwrap()`/`expect()`** outside tests. Errors return `IcError`.
- **Panics** as control flow. `ic_assert!` compiles to nothing in release
  builds exactly like the C `ic_assert`.

## Naming

- Crates: `ic_port`, `ic_util`, `ic_comm`, `ic_protocol`, `ic_apic`,
  `ic_ndb_signals`, `ic_apid`, `ic_capi` (see
  [03-module-map.md](03-module-map.md)).
- Modules/files: the C file name without `ic_` and without extension:
  `util/ic_bitmap.c` → `ic_util/src/bitmap.rs`.
- Types: `IC_APID_CONNECTION` → `ApidConnection`, `IC_TABLE_DEF` → `TableDef`.
  Drop the `IC_` prefix, keep the words, CamelCase them. The C ABI keeps
  the exact C names as opaque typedefs.
- Functions: `ic_read_key` → `read_key` as a method on the owning type. The
  C ABI exports `ic_apid_conn_read_key` (object, then verb) so every export
  is unique without a vtable.
- Constants: unchanged, upper case: `IC_RNIL`, `GSN_TCKEYREQ`.
- Fields: unchanged snake_case. Keep the C field names so that the C header
  comments still describe the Rust struct.

## Memory

- The `MemoryContainer` (port of `ic_mc`) survives as the arena for
  per-transaction and per-query allocation. It returns `&mut [u8]` slices
  or indices, never raw pointers, outside the FFI crate.
- No allocation on the send/receive hot path after warm-up. Buffers come
  from pools (`SockBuf` pages, signal buffers), as in the C design.
- Every `Drop` implementation is short and only releases resources. No
  logic in destructors.

## Errors

- `IcError` is `#[repr(C)] struct IcError { code: i32, ... }` and mirrors
  `IC_APID_ERROR`: code, category, severity, message pointer. Error codes
  stay numerically compatible with `ic_err.h` and with NDB error codes so
  that operators' runbooks stay valid.
- Functions return `Result<T, IcError>`. The C ABI turns this into `int`
  return codes and out-parameters, mechanically, in the `capi` crate.

## For C readers

The construct-by-construct translation table, with explanations and two
worked examples, is [11-c-to-rust-mappings.md](11-c-to-rust-mappings.md).
Every crate `MODULE.md` adds a "Rust notes for C readers" section.

## Comments

- `///` for documentation on a public item, `//!` at the top of a file
  for the module. Every public item has one.
- `//` for everything else, including explanations several lines long.
- Not `/* ... */` across several lines. rustfmt does not understand the
  alignment of a continuation line and pushes it back to the left, which
  leaves the comment ragged and makes `cargo fmt --check` fight every
  edit. A single-line `/* ... */` is fine where it reads better, such as
  a note inside an expression.
- Comments say why, not what. The C header's prose is the specification
  and belongs in `///` on the item it describes.

## Layout conventions

- 80 columns and 2-space indentation, as the C code. `rustfmt` with a
  checked-in `rustfmt.toml` (`max_width = 80`, `tab_spaces = 2`,
  `hard_tabs = false`, `use_small_heuristics = "Default"`).
- Opening braces stay on the same line as the `fn`, `if`, `for`, `while`,
  `match` or `struct`. Stable rustfmt does that by default and only moves
  a brace down when the line would exceed 80 columns, so the rule is:
  never let it. Split a long condition into two `if` statements or bind
  it to a named `bool` first; break long argument lists one per line.
- `clippy` runs with `-D warnings` plus a deny-list enforcing the forbidden
  items above where clippy can (e.g. `clippy::needless_lifetimes`,
  `clippy::iter_over_hash_type`, custom lint via `dylint` later if needed).
- One `MODULE.md` per crate listing: source C files, what was translated
  one-to-one, what was redesigned, and what glib functions were replaced by
  what.
