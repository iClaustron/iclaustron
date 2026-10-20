# Examples

The same eleven programs in Rust (`rust/`) and in C (`c/`), each against
a running cluster, the development cluster of `doc/dev-cluster.md` for
instance:

| Program | What it shows |
|---|---|
| `describe` | a table's fields as the data nodes describe them, and where a record lays each one out in a row |
| `key_ops` | insert, read, update and delete by primary key, one synchronous call each |
| `pipeline` | many rows written asynchronously, each query's callback starting the next write, a number in flight at once |
| `unique` | a row read and updated through a unique index |
| `retry` | a transaction that fails with a temporary error, a lock wait that timed out, and succeeds when tried again |
| `scan` | every row of the table by a scan of every fragment at once, row by row and then a batch at a time |
| `range_scan` | rows by ranges of the ordered index on val, in the index's order, from either end, and several numbered ranges at once |
| `scan_update` | rows locked by an exclusive scan on a transaction and updated through it, as `UPDATE ... WHERE` does, with one commit |
| `scan_executor` | four threads with no connection of their own, each reading a range in order through a reader of an executor's scan |
| `conditional_update` | programs run in the data nodes: an update only while val is what was read, a counter read back after its update, a scan's filter |
| `auto_increment` | AUTO_INCREMENT values asked for and put in the row: one at a time, a block, an id of the program's own; a table without a primary key, its hidden key, a scan and deletes by it |

`conditional_update` and `auto_increment` check every result they know
to expect and end with "all checks passed"; a check that fails says
what was wrong and exits with 1.

Every program takes the connectstring as its first argument
(`localhost:1186` if none) and works on the table `ic_ex.t1`, which
`setup.sql` makes, with a unique index on code and an ordered index on
val; `auto_increment` works on `ic_ex.t2` and `ic_ex.t3`, which it
makes too. The scan programs first write rows of their own, ids 200 to
299 with val the id negated, which no other program writes, so that a
range of val finds them alone, and `conditional_update` ids 300 to
309. Tables made by an earlier `setup.sql`, without the index on val or
without t2 and t3, are made again by running it again:

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
cargo run -p ic_examples --example scan -- localhost:1186
cargo run -p ic_examples --example range_scan -- localhost:1186
cargo run -p ic_examples --example scan_update -- localhost:1186
cargo run -p ic_examples --example scan_executor -- localhost:1186
cargo run -p ic_examples --example conditional_update -- localhost:1186
cargo run -p ic_examples --example auto_increment -- localhost:1186
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
