# 09 — Licensing and clean-room rules

## Facts

- The iClaustron C code (`legacy-c/`) is GPLv2, copyright iClaustron AB,
  authored by Mikael Ronström (AUTHORS). iClaustron AB keeps that
  copyright. The copyright holder can relicense a derivative work; the GPL
  is a grant to others, not a restriction on the owner.
- The new Rust code, the C header and the C++ wrapper are copyright
  Hopsworks and/or its affiliates, MIT licensed.
- RonDB / MySQL NDB Cluster is GPLv2 with the Universal FOSS Exception,
  copyright Oracle and Hopsworks. Nothing from
  `/Users/mikael/mysql_trees/rondb_2604_main` may be copied into the new
  code base: no code, no tables, no comments, no header layouts pasted
  verbatim.
- The wire protocol itself (signal numbers, block numbers, field layouts,
  text protocol strings) is an interface. Reimplementing an interface is
  what the GPL's own FAQ and decades of practice (Samba, Wine, ReactOS)
  treat as permitted, provided the implementation is our own.

## Choice of license

Decided: **MIT** for all Rust crates, the C header and the C++ wrapper.

Third-party crates must be MIT, BSD, Apache-2.0 or ISC. `cargo deny` with a
checked-in `deny.toml` enforces this in CI. `rustls` (Apache-2.0/ISC/MIT)
qualifies; anything GPL/LGPL is rejected by the tool.

## Clean-room procedure for the protocol

1. **Read** the RonDB source to understand a signal or format.
2. **Write a specification note** in [05-ndb-protocol.md](05-ndb-protocol.md)
   in our own words: field names, word offsets, bit widths, semantics,
   with a *file:line pointer* into RonDB for later verification. The
   pointer is a citation, not a copy.
3. **Implement** from the note, not from the RonDB source window.
4. **Verify** against a live RonDB cluster (or captured traffic), never by
   diffing against RonDB code.

Where RonDB constants have official names (e.g. `GSN_TCKEYREQ`, block
`DBTC = 245`), we use the same names: they are the interface vocabulary and
appear in RonDB's logs and documentation. Using the vocabulary is not
copying.

## Error messages

NDB error codes (numbers, classification) are interface data and are kept
for compatibility. The **message texts** in RonDB's `ndberror.cpp` are
copyrighted prose. Write our own message strings. Keep them short and keep
the code number first so operators can cross-reference.

## Config parameter tables

iClaustron's `ic_apic_conf_param.ic` is iClaustron code and can be
relicensed, but it describes NDB 7.2.9. The new, much smaller API-node
parameter table is written against RonDB 26.10's
`mgmapi_config_parameters.h` following the clean-room procedure above
(parameter names and ids are interface data; description prose is not).

## Repository

Decided: the project keeps the name iClaustron and lives in this
repository. All existing C code moves unchanged into `legacy-c/` (with its
original GPL file headers; it is reference material, never built, never
installed, never part of a release artefact). The Rust workspace takes the
repository root and the root `LICENSE` becomes MIT. The copyright holder
can relicense; when a piece of C is translated its prose moves into Rust
doc comments under MIT, and when `legacy-c/` is no longer needed it is
deleted in one commit.

Every new source file starts with:

```
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.
```

A Rust file that is a translation of a C file (mode 1:1 in chapter 03)
is a derivative of iClaustron AB's work and carries both holders:

```
// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.
```

Both are MIT; the two-line form only records who holds what. No per-file
GPL boilerplate, no "based on iClaustron file X" notes in the files (that
belongs in the crate `MODULE.md`, which is documentation).
