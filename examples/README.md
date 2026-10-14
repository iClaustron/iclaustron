# Examples

The same five programs in Rust (`rust/`) and in C (`c/`), each against
a running cluster, the development cluster of `doc/dev-cluster.md` for
instance:

| Program | What it shows |
|---|---|
| `describe` | a table's fields as the data nodes describe them, and where a record lays each one out in a row |
| `key_ops` | insert, read, update and delete by primary key, one synchronous call each |
| `pipeline` | many rows written asynchronously, each query's callback starting the next write, a number in flight at once |
| `unique` | a row read and updated through a unique index |
| `retry` | a transaction that fails with a temporary error, a lock wait that timed out, and succeeds when tried again |

Every program takes the connectstring as its first argument
(`localhost:1186` if none) and works on the table `ic_ex.t1`, which
`setup.sql` makes:

```sh
BIN=/Users/mikael/mysql_trees/rondb_2604_main/debug_build/bin
$BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000 \
  < examples/setup.sql
```

## Rust

The examples are cargo examples of the package `ic_examples`, whose
library holds what they share: connecting, the table's record, and
putting values into a row and taking them out.

```sh
cargo run -p ic_examples --example describe -- localhost:1186
cargo run -p ic_examples --example key_ops -- localhost:1186
cargo run -p ic_examples --example pipeline -- localhost:1186 10000 32
cargo run -p ic_examples --example unique -- localhost:1186
cargo run -p ic_examples --example retry -- localhost:1186
```

## C

Each program is one file, with `t1.h` for what they share: the row as a
C struct, the record over it, and connecting. Against an installed
library (`cargo xtask install`, with pkg-config finding it):

```sh
cc -o describe describe.c $(pkg-config --cflags --libs iclaustron)
```

or every program at once, `make` in `examples/c`. Against the tree's
own build (`cargo build --release -p ic_capi`), without installing:

```sh
make IC_TREE=../..
```

and run them the same way: `./describe localhost:1186`,
`./pipeline localhost:1186 10000 32`.
