# 11 — C to Rust mappings for iClaustron readers

For readers who know the iClaustron C code and little Rust. Each row
names a construct as it appears in the C tree, what it becomes in the
Rust tree, and why that is the equivalent. Everything here follows the
rules in [07-style-guide.md](07-style-guide.md). Crate `MODULE.md` files
add notes for anything specific to that crate.

## Types and declarations

| C (iClaustron) | Rust | Explanation |
|---|---|---|
| `typedef struct ic_bitmap IC_BITMAP;` + `struct ic_bitmap {...}` | `pub struct Bitmap { ... }` | One declaration; the struct name is the type name. No typedef needed. |
| `guint32`, `guint64`, `gint`, `guint8`, `gboolean`, `gchar` | `u32`, `u64`, `i32`, `u8`, `bool`, `u8`/`char` | Fixed-width integers are built in. `bool` is a real type. `gchar*` text becomes `&str` (borrowed) or `String` (owned); raw bytes become `&[u8]`/`Vec<u8>`. |
| `gchar *str; guint32 len;` (pointer + length) | `&[u8]` or `&mut [u8]` (a *slice*) | A slice is a pointer plus a length in one value, with bounds checks on every index. `s.len()` gives the length. |
| `gchar buf[256];` | `[u8; 256]` | Array with compile-time size. `&buf[..]` turns it into a slice. |
| `gchar *p = ic_malloc(n);` ... `ic_free(p);` | `let mut v: Vec<u8> = vec![0; n];` | `Vec` is a heap array that frees itself when it goes out of scope. There is no `free`. |
| `IC_APID_QUERY *q = ic_create_apid_query(...)` ... `q->ops->ic_free_apid_query(q)` | `let q: Box<ApidQuery> = ApidQuery::create(...)?;` … `drop(q)` or end of scope | `Box<T>` is a single heap object with one owner. Dropping the owner frees it. Explicit `free` functions only exist at the C ABI. |
| `NULL` pointers | `Option<&T>` / `Option<Box<T>>` | `None` is NULL; the compiler forces you to check before use (`if let Some(x) = opt {}` or `match`). |
| `enum ic_commit_state { IC_TRANS_STARTED = 0, ... }` | `#[repr(u32)] pub enum CommitState { TransStarted = 0, ... }` | Same values on the wire and in the C ABI. `match` on it must cover all variants (or have a `_ =>` arm). |
| `union { IC_READ_KEY_QUERY_TYPE r; IC_WRITE_KEY_QUERY_TYPE w; }` with a separate tag | `enum QueryKind { Read(ReadKeyQueryType), Write(WriteKeyQueryType) }` | An enum variant can carry data; it is the tag and the union in one, and the compiler checks the tag. |
| `#define IC_RNIL 0xFFFFFF00` | `pub const IC_RNIL: u32 = 0xFFFF_FF00;` | Typed constant. Underscores in numbers are ignored. |
| `static const guint32 x = ...;` in a header | `pub const` | same |
| `#define IC_MIN(a,b) ((a) < (b) ? (a) : (b))` | `fn` or `a.min(b)` | Function-like macros become ordinary (inlined) functions. |
| `struct ic_apid_connection_ops { int (*ic_read_key)(IC_APID_CONNECTION*, ...); ... }` and `conn->ops->ic_read_key(conn, ...)` | `impl ApidConnection { pub fn read_key(&mut self, ...) -> Result<(), IcError> }` and `conn.read_key(...)` | The vtable existed to hide the implementation and to group functions with their object. `impl` blocks group functions with their type; private fields hide the implementation. The call is direct, not through a pointer. |
| The three-tier `IC_X` / `IC_HIDDEN_X` / `IC_INT_X` structs with casts | one `pub struct X { pub(crate) ...; ... }` | Rust privacy does the hiding: fields without `pub` are invisible outside the module; `pub(crate)` is visible inside the crate only. No casts. |
| `IC_INLINE guint32 ic_get_x(IC_X *x) { return ((IC_HIDDEN_X*)x)->x; }` | `#[inline] pub fn x(&self) -> u32 { self.x }` | Inline accessor as a method. |
| `.ic` files `#include`d into one `.c` so statics are shared | one module file per `.ic` in the same crate; shared items marked `pub(crate)` | A crate is the unit of compilation and optimisation, so the unity-build trick is not needed. `mod send_thread;` in `lib.rs` includes the file. |
| `static void helper(...)` (file-local) | `fn helper(...)` (no `pub`) | Items are private by default. |
| `extern guint32 ic_glob_num_threads;` | field of a `struct GlobalOptions` passed around; or `static ATOMIC: AtomicU32` for a true global | Mutable globals are `unsafe` in Rust; the plan replaces the `ic_glob_*` variables by an options struct. |

## Functions and control flow

| C | Rust | Explanation |
|---|---|---|
| `int f(IC_X *x, guint32 *out)` returning 0 or an error code, writing through `out` | `fn f(x: &mut X) -> Result<u32, IcError>` | `Result` is either `Ok(value)` or `Err(error)`. The out-parameter becomes the return value. |
| `if ((ret = g(...))) goto error;` ... `error: cleanup; return ret;` | `g(...)?;` | `?` means "if this is `Err`, return it from the current function now". Cleanup happens automatically because owned values are dropped on return. |
| `if (!ptr) return IC_ERROR_MEM_ALLOC;` | not needed for `Box`/`Vec` (allocation failure aborts); for pool exhaustion return `Err(IcError::new(IC_ERROR_MEM_ALLOC))` | |
| `for (i = 0; i < n; i++)` | `for i in 0..n` | `0..n` is the half-open range. `i` is immutable inside the loop. |
| `while (cond) { }`, `do {} while` | `while cond {}`, `loop { ...; if !cond { break; } }` | `loop` is an infinite loop with `break`. |
| `switch (gsn) { case GSN_X: ...; break; }` | `match gsn { GSN_X => { ... } _ => { ... } }` | No fall-through; the `_` arm is `default`. |
| `void (*cb)(IC_APID_CONNECTION*, void*)` + `void *user_data` | `Option<fn(&mut ApidConnection, &mut ApidQuery, usize)>` + `user_ref: usize` | Plain function pointer, same as C. `usize` is a pointer-sized integer; C callers pass `void*` and the C ABI casts. |
| `ic_assert(cond)` | `ic_assert!(cond)` | Macros are called with `!`. Compiled out in release builds like the C. |
| `if (p) { q = strdup(p); } else { q = NULL; }` | `q = p.map(\|v\| v.to_string());` | `Option::map` is "if present, transform it, else stay absent". The `\|v\|` part is a closure, i.e. an inline function taking `v`. |
| `x = f(); if (!x) x = 0;` | `x = f().unwrap_or(0);` | "the value, or this default if absent". `unwrap_or_default()` uses the type's zero value. |
| `DEBUG_ENTRY("f"); ... DEBUG_RETURN_INT(ret);` | `let _g = debug_entry!("f");` then plain `return` | The guard prints the entry now and the return when the function exits, whatever path it takes. |
| `memcpy(dst, src, n)` | `dst[..n].copy_from_slice(&src[..n])` | Bounds-checked copy. |
| `(guint32*)buf` and `buf32[i]` | `u32::from_ne_bytes(buf[4*i..4*i+4].try_into().unwrap())` or a `&[u32]` obtained once with `bytemuck`-free `unsafe { slice::from_raw_parts }` in the codec crate | Reinterpreting bytes as words is `unsafe` in Rust; the plan confines it to `ic_ndb_signals::header` and `ic_comm::sock_buf`, done once per page, so the rest of the code works on `&[u32]`. |
| bit fields via shifts and masks | identical: `(w >> 8) & 0xFFFF` | Nothing changes; Rust has no bit-field syntax either. |
| `x = a & b; y = a | b; z = a ^ b; ~a` | same operators, `!a` for bitwise not | `!` on integers is bitwise NOT. |

## Memory and ownership

| C | Rust | Explanation |
|---|---|---|
| Caller allocates, callee fills (`ic_get_buf`) | `fn fill(&self, buf: &mut [u8])` | `&mut` is an exclusive, temporary pointer; the callee cannot keep it. |
| Callee allocates, caller frees | `fn make(&self) -> Vec<u8>` | Returning an owned value moves ownership to the caller; it is freed where it goes out of scope. |
| Object holds a pointer to another object it does not own (`apid_query->apid_conn`) | store an index or id (`conn_id: u32`) and look it up, or a `&` reference with a lifetime in internal code only | Rust needs to know who owns what. The plan uses ids in objects that live long and references only inside short functions. |
| Reference-counted page (`gint ref_count` + atomic dec) | `Arc<Page>` or an explicit `AtomicI32` field as in the C | `Arc` is an atomically refcounted pointer that frees at zero. The sock-buf pool keeps the explicit counter to return pages to the pool rather than free them. |
| Memory container (`ic_mc_alloc` from an arena, `ic_mc_free` all at once) | `MemoryContainer` in `ic_util::mc`, same API returning `&mut [u8]` | Kept because the lifetime pattern (everything freed at once) is useful and clear. |
| Linked list via `next` pointers (`IC_INSERT_SLL`) | `Vec<T>` / `VecDeque<T>` or `next: Option<u32>` index links inside a `Vec` | Pointer-linked lists are awkward under Rust ownership; index links give the same O(1) insert/remove without pointers. |

## Threads and synchronisation

| C | Rust | Explanation |
|---|---|---|
| `g_thread_try_new(f, arg)` | `std::thread::Builder::new().name(..).stack_size(..).spawn(move \|\| f(arg))` | The closure captures `arg`; the plan wraps this in `ic_port::thread::spawn(name, stack, fn, arg)` so callers pass a function pointer. |
| `IC_MUTEX *m; ic_mutex_lock(m); data...; ic_mutex_unlock(m);` | `let mut guard = m.lock(); guard.field = ...;` unlock at end of scope | A `Mutex<T>` *contains* the data it protects; you cannot touch the data without locking, and unlock is automatic when the guard goes out of scope. |
| `ic_mutex_unlock(m);` and then more work, such as `ic_cond_signal(c);` after the unlock | `drop(guard);` then the work | `drop(guard)` is the explicit unlock, for when the mutex must be released before the end of the scope. `thread_conn.rs` uses it to wake a user thread only after releasing the inbox, as `post_ndb_messages` does in the C. |
| `ic_cond_wait(c, m)` | `guard = c.wait(guard)` | Same semantics; the guard proves the mutex is held. |
| `g_private_get()` thread-local | `thread_local! { static X: ... }` | |
| `volatile gboolean stop_flag` | `AtomicBool` with `load`/`store` | `volatile` is not a synchronisation tool in C either; atomics are. |
| `g_atomic_int_dec_and_test` | `AtomicI32::fetch_sub(1, Ordering::AcqRel) == 1` | |

## Errors

| C | Rust | Explanation |
|---|---|---|
| `#define IC_ERROR_MEM_ALLOC 7003` + message table | `pub const IC_ERROR_MEM_ALLOC: i32 = 7003;` + `fn message(code) -> &'static str` | same numbers |
| `IC_APID_ERROR` object filled on failure | `#[repr(C)] pub struct IcError { code: i32, category: ErrorCategory, severity: ErrorSeverity, message: &'static str }` | Returned in `Err(...)`; the C ABI stores it on the object and returns the code. |
| System errno | `std::io::Error` converted to `IcError` with the errno as code | |

## Things that exist in Rust and have no C equivalent, and how the plan uses them

- **Ownership and borrowing.** The compiler tracks who owns each value and
  forbids two mutable pointers to the same thing at once. This is what
  removes use-after-free and data races. In practice the plan's rules
  (one thread owns one connection, ids instead of back-pointers) make the
  compiler agree with the design without contortions.
- **Modules and crates.** `mod x;` = one file; `use crate::x::Y;` =
  import. A crate = a library with a `Cargo.toml`; the workspace ties them.
- **`cargo test`.** Tests are functions marked `#[test]` next to the code
  or in `tests/`. `cargo test -p ic_util` builds and runs them.
- **Traits.** Interfaces. The plan allows exactly one: `Transport` (TCP
  now, TLS later). Everything else is concrete types.
- **`unsafe`.** A block where raw pointers and FFI are allowed. Confined
  to `ic_port`, `ic_comm::sock_buf`, `ic_ndb_signals::header` and
  `ic_capi`; each block has a comment stating why it is sound.

## Worked example: `ic_bitmap` set/get

C (`util/ic_bitmap.c`, simplified):

```c
struct ic_bitmap { guchar *bitmap_area; guint32 num_bits; };

void ic_bitmap_set_bit(IC_BITMAP *b, guint32 bit)
{
  ic_assert(bit < b->num_bits);
  b->bitmap_area[bit >> 3] |= (1 << (bit & 7));
}

gboolean ic_is_bitmap_set(IC_BITMAP *b, guint32 bit)
{
  ic_assert(bit < b->num_bits);
  return (b->bitmap_area[bit >> 3] & (1 << (bit & 7))) != 0;
}
```

Rust (`ic_util/src/bitmap.rs`):

```rust
pub struct Bitmap {
    area: Vec<u8>,
    num_bits: u32,
}

impl Bitmap {
    pub fn new(num_bits: u32) -> Bitmap {
        let bytes = ((num_bits + 7) / 8) as usize;
        Bitmap { area: vec![0; bytes], num_bits }
    }

    pub fn set_bit(&mut self, bit: u32) {
        ic_assert!(bit < self.num_bits);
        self.area[(bit >> 3) as usize] |= 1 << (bit & 7);
    }

    pub fn is_set(&self, bit: u32) -> bool {
        ic_assert!(bit < self.num_bits);
        (self.area[(bit >> 3) as usize] & (1 << (bit & 7))) != 0
    }
}
```

Reading it as C: `&mut self` is `IC_BITMAP *b` for a function that
modifies the object; `&self` is `const IC_BITMAP *b`. `Vec<u8>` is
`guchar *` plus the length plus automatic free. `as usize` is a cast to
the index type. There is no `ic_free_bitmap`: dropping the `Bitmap` frees
the `Vec`.

## Worked example: header word 1 (`fill_ndb_message_header`)

C builds word 0 with shifts into a `register guint32 word`. Rust:

```rust
pub fn encode_word1(byte_order_big: bool, fragment: u32, signal_id: bool,
                    checksum: bool, prio: u32, total_len: u32,
                    data_len: u32) -> u32 {
    let mut word: u32 = 0;
    if byte_order_big { word |= 0x8100_0081; }
    word |= (fragment & 2) >> 1;            // low fragment bit -> bit 1
    word |= (fragment & 1) << 25;           // high fragment bit -> bit 25
    if signal_id { word |= 1 << 2; }
    if checksum { word |= 1 << 4; }
    word |= (prio & 3) << 5;
    word |= (total_len & 0xFFFF) << 8;
    word |= (data_len & 0x1F) << 26;
    word
}
```

Identical arithmetic; the only differences are `bool` parameters instead
of `gboolean`, and that the function returns the word instead of writing
through a pointer.
