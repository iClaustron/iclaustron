# legacy-c — the original iClaustron C code

This directory holds the iClaustron C code base exactly as it was before
the Rust rewrite started (September 2026): the Data API design and its
partial implementation, the configuration client, the cluster server,
cluster manager, cluster client, process controller, bootstrap, file
server and replication server, with their autotools and CMake build files.

It is **reference material only**:

- it is not built by the Rust workspace and not part of any release;
- it keeps its original file headers and copyright (GPLv2, iClaustron AB);
  the Rust code at the repository root is copyright Hopsworks and/or its affiliates, MIT
  licensed;
- the parts that are translated are listed, file by file, in
  `../doc/rust/03-module-map.md`; the rest is out of scope for the Rust
  project;
- when the translation no longer needs it, this directory is deleted in
  one commit.

The original `README` (with the description of every directory), `INSTALL`
and `README-CONFIG` are in this directory unchanged. To build it, follow
`INSTALL` from inside this directory; it needs glib2, bison and CMake.
