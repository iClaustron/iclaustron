# ic_protocol — module notes

Base64 and the vocabulary of the NDB management protocol. See
`doc/rust/03-module-map.md` for the workspace-wide mapping.

## Source files translated

| C file (legacy-c/) | Rust module | Mode | Status |
|---|---|---|---|
| `protocol/ic_base64.c`, `include/ic_base64.h` | `base64.rs` | Redesign | done |
| `protocol/ic_proto_str.c` (management half) | `proto_str.rs` | Redesign | done |
| `protocol/ic_proto_str.c` (other programs) | — | Out | cluster manager, process controller |
| `protocol/ic_pcntrl_proto.c` | — | Out | process controller |

## Deviations from the C code

- **The decoder ignores blanks.** The C decoder rejected any character
  outside the base64 alphabet, including the newlines its own encoder
  produced every 76 characters, so callers had to strip them first. This
  one skips newlines, carriage returns, spaces and tabs wherever they
  fall, so a blob straight off the socket decodes as it stands.
- **Encoding allocates a `String` and returns it**, rather than writing
  through an out-pointer and a length the caller must free.
- **The strings are verified against RonDB 26.10, not copied from the
  C.** The C's table dated from NDB 7.2.9 and covered the cluster
  manager and process controller protocols as well. What is here is the
  management protocol only, with the file and line in
  `storage/ndb/src/mgmapi/mgmapi.cpp` where each string is checked.

## Verified against RonDB 26.10

| Item | Value | Where |
|---|---|---|
| node id request | `get nodeid` | `mgmapi.cpp:3301` |
| its reply | `get nodeid reply` | `mgmapi.cpp:3295` |
| its arguments | version, nodetype, nodeid, user, password, public key, endian, name, log_event | `mgmapi.cpp:3283-3293` |
| configuration request | `get config_v2`, falling back to `get config` | `mgmapi.cpp:3136` |
| its reply | `get config reply` for both | `mgmapi.cpp:3126` |
| blob header | Content-Length, Content-Type `ndbconfig/octet-stream`, Content-Transfer-Encoding `base64` | `mgmapi.cpp:3148-3165` |
| version request | `get version`, replying `version` | `mgmapi.cpp:3692,3704` |
| status request | `get status`, replying `node status` | `mgmapi.cpp:1212,1237` |
| connection parameter | `get connection parameter`, replying with ` reply` | `mgmapi.cpp:3560,3565` |
| transporter handover | `transporter connect` | `mgmapi.cpp:3600` |
| node types | DB 0, API 1, MGM 2 | `mgmapi_config_parameters.h:455` |

Two replies do not follow the rule that a reply is the command plus
" reply": `get version` answers `version`, and `get status` answers
`node status`. A test asserts both, so a careless tidy-up cannot break
them.

Configuration parameter ids live in `ic_apic::conf_param`, not here:
they are configuration, not protocol vocabulary, and only the
configuration client uses them.

## Rust notes for C readers

- **`&'static str`** is a constant string: a pointer to fixed text plus
  its length, copyable freely and never freed. It is what a `const char
  *` to a literal was.
- **`div_ceil`** is the rounding-up division the C wrote as
  `(a + b - 1) / b`.
- **`Vec<u8>` returned by `decode`** owns its bytes and frees them when
  the caller drops it, so there is no length out-parameter and no
  ownership question.

## Open items

- The encoder exists for symmetry and for the round-trip tests. Nothing
  in an API node encodes base64; only the management server does, when
  it sends a configuration.
- `proto_str` covers the commands Phase 2 needs. Events, backups and the
  other management commands are added when something uses them.
