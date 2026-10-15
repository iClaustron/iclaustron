#!/bin/sh
# The performance pass of phase 7 (doc/rust/06-phases.md): every run it
# needs, against one cluster, into one log. Measure a cluster built in
# release (RONDB_BIN below), not the debug development cluster, whose
# data nodes are the limit long before the client is.
#
#   tools/perf_pass.sh                 # from the repository's root
#   CS=host:1186 SECONDS_RUN=20 tools/perf_pass.sh
#
# What it runs:
#   1. ic_hashbench and xxh3_bench: our XXH3 against the xxHash the
#      data nodes are built with; the checks must agree.
#   2. ic_bench, committed reads: 1 thread, then 8 threads (the 0.1
#      criterion), 8 sending rounds of 400 and waiting for each as
#      flexAsynch does (--batch 400 --depth 1), 8 with --force (adaptive
#      send off), 8 with two receive threads; then updates on 1 thread.
#   3. ic_bench built with count-allocs: allocations per operation for
#      reads and for updates, 1 thread.
#   4. flexAsynch, the reference API's benchmark, like for like with the
#      8-thread runs: 8 threads, 400 one-read transactions in flight each,
#      dirty reads as ours are committed reads; its read run under
#      /usr/bin/time for its CPU, which covers the warm-up and cool-down
#      as well: divide by its rate times their 15 seconds.
#   5. Through executors (phase 8.1): application threads making one
#      blocking call at a time, 64 through one executor and 256 through
#      two, against 64 threads with a connection each, one call at a
#      time (--batch 1 --depth 1); then ic_model's prediction of the same.
set -u

RONDB=${RONDB:-/Users/mikael/mysql_trees/rondb_2604_main}
RONDB_BIN=${RONDB_BIN:-$RONDB/prod_build/bin}
CS=${CS:-localhost:1186}
MYSQL=${MYSQL:-"$RONDB_BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000"}
SECONDS_RUN=${SECONDS_RUN:-10}
KEYS=${KEYS:-100000}
OUT=${OUT:-target/perf-$(date +%Y%m%d-%H%M%S).log}

mkdir -p target
run() {
  echo "== $*" | tee -a "$OUT"
  "$@" 2>&1 | tee -a "$OUT"
}

cargo build --release -p ic_bench -p ic_hashbench -p ic_model || exit 1
cargo build --release -p ic_bench --features count-allocs \
  --target-dir target/count-allocs || exit 1
cc -O3 -I"$RONDB/extra/xxhash/xxHash-0.8.3" \
  $( [ "$(uname -m)" = x86_64 ] && echo -mavx2 ) \
  -o target/xxh3_bench tools/ic_hashbench/xxh3_bench.c || exit 1

$MYSQL -e "CREATE DATABASE IF NOT EXISTS ictest;
  DROP TABLE IF EXISTS ictest.t9;
  CREATE TABLE ictest.t9 (id INT NOT NULL PRIMARY KEY, v INT) ENGINE=NDB" \
  || exit 1

echo "== $(git log -1 --format='%h %s'), $(uname -sm), $(date)" | tee -a "$OUT"

# 1. The key hash.
run target/release/ic_hashbench
run target/xxh3_bench

# 2. Throughput and client CPU.
B="target/release/ic_bench -c $CS -d ictest t9 --keys $KEYS --seconds $SECONDS_RUN"
run $B
run $B --no-prepare --threads 8
run $B --no-prepare --threads 8 --batch 400 --depth 1
run $B --no-prepare --threads 8 --force
run $B --no-prepare --threads 8 --receive-threads 2
run $B --no-prepare --mode write

# 3. Allocations per operation.
A="target/count-allocs/release/ic_bench -c $CS -d ictest t9 --keys $KEYS --seconds $SECONDS_RUN --no-prepare"
run $A
run $A --mode write

# 4. The reference API, like for like with the 8-thread run.
F="$RONDB_BIN/flexAsynch -t 8 -p 400 -o 1 -c 1 -a 2 -s 1 -n -new"
export NDB_CONNECTSTRING=$CS
run $F -create_table
run $F -insert
if [ "$(uname -s)" = Darwin ]; then TIME="/usr/bin/time -l"; else TIME="/usr/bin/time -v"; fi
run $TIME $F -read -warmup_time 3 -execution_time "$SECONDS_RUN" -cooldown_time 2
run $F -delete
run $F -drop_table

# 5. Through executors, and what the model says of the same.
run $B --no-prepare --threads 64 --batch 1 --depth 1
run $B --no-prepare --executors 1 --clients 64
run $B --no-prepare --executors 2 --clients 256
run $B --no-prepare --executors 1 --clients 64 --mode write
run target/release/ic_model threads=64 depth=1 batch=1
run target/release/ic_model executors=1 clients=64
run target/release/ic_model executors=2 clients=256

echo "The log is $OUT"
