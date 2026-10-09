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

Revised by the author, 2026-10-08: protocols are developed mainly by
listening to the network.

1. **Listen.** Capture the traffic of the reference API, a MySQL server
   or our own tools doing the operation against a live cluster, and read
   it with our Wireshark dissector (chapter 06 §6.7), which lives in a
   Wireshark source tree under Wireshark's licence, not in this
   repository. The wire is the first source.
2. **Ask the author** what the wire does not show: why a field is
   there, when a reply comes, the cases a capture did not reach.
3. **Read** the RonDB source only to settle what neither shows, for
   facts, never to copy.
4. **Write a specification note** in [05-ndb-protocol.md](05-ndb-protocol.md)
   in our own words: field names of our own, word offsets, bit widths,
   semantics, naming the capture it came from and, where the source was
   read, a bare *file:line pointer* into RonDB. The pointer is a
   citation, not a copy, and names no RonDB function, class, constant
   or signal.
5. **Implement** from the note, not from a capture window or the RonDB
   source. Code comments cite the chapter 05 section, never RonDB.
6. **Verify** against a live RonDB cluster, the dissector showing our
   traffic beside the reference's, never by diffing against RonDB code.

## Names (decided by the author, 2026-10-08)

Nothing may be copied or taken from the RonDB source tree, and that
includes names. iClaustron is a new API, written from the ideas of
iClaustron, with a new implementation. The protocol stays the same on
the wire; the names are ours. This replaces the earlier rule here that
we used RonDB's names for signals and blocks as interface vocabulary.

- **Ours:** every name of a signal, a block, a signal's field, a
  dictionary property key, a pseudo-column, a type, a function and a
  module. A name says what the thing does. Signals take new stems with
  the suffixes `_REQ` (request), `_CONF` (confirm), `_REF` (refuse) and
  `_REP` (report); a signal that would have no suffix takes `_ORD` for
  an order, to which nothing comes back, or `_REP` for a report.
  Constants are `IC_SIG_<NAME>` and `IC_BLOCK_<NAME>`, Rust types
  CamelCase (`KeyOpReq`). The table by number is in chapter 05; the
  first draft is in chapter 06 §6.1.
- **RonDB names appear nowhere** in code or docs. Chapter 05 cites RonDB
  by bare file:line and refers to signals by number; code comments cite
  chapter 05. `cargo xtask names` checks this against a RonDB tree given
  at check time, so that the list of RonDB names is never stored here.
- **The wire is the boundary** (author, 2026-10-08): whatever travels
  on it stays exactly as it is: numbers, word layouts, error codes,
  configuration parameter ids and section types, the management
  protocol's text, and the stored names we look up (the counter table
  `sys/def/SYSTAB_0`, the hidden key column `$PK`). Whatever does not
  travel is named by us: signal names, block names, and configuration
  parameter names too, since only their ids are on the wire. SQL names
  an application writes (collations, `AUTO_INCREMENT`) are MySQL's
  language and are used as they are. "RonDB" and "NDB" name what we
  interoperate with.
- `legacy-c/` is iClaustron's old code, reference only, and is not
  changed.

## Character sets and collations

Decided by the author, 2026-10-08. A character set's encoding, its
collations' weights and their ordering are retrieved from the data
nodes at run time, through a dictionary request RonDB 26.04 main adds
in a separate task, and kept in iClaustron's global dictionary (chapter
06 §8.6). They are data a running cluster gives every client, as a
table's description is; nothing of them is written into our code from
MySQL's or RonDB's source. A character set whose tables are a public
standard may be compiled in from that standard, with the source named:
the binary set, the CP1252 code page as Unicode publishes it (MySQL's
latin1 encoding), Unicode's own data.

## Error messages

NDB error codes (numbers, classification) are interface data and are kept
for compatibility. The **message texts** in RonDB's error table
(chapter 05 §10) are copyrighted prose. Write our own message strings.
Keep them short and keep the code number first so operators can
cross-reference.

## Config parameter tables

iClaustron's `ic_apic_conf_param.ic` is iClaustron code and can be
relicensed, but it describes NDB 7.2.9. The new, much smaller API-node
parameter table is written against RonDB 26.10's list of configuration
parameters (chapter 05 §5.2) following the clean-room procedure above
(parameter ids and section types are interface data and travel on the
wire; parameter names and description prose do not, and ours are our
own, mapped to the ids in chapter 05).

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
