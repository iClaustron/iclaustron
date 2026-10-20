#!/bin/sh
# Phase 9.2's exit by hand (doc/rust/06-phases.md, 9.2): ic_trans and
# MySQL servers inserting into one AUTO_INCREMENT table at the same
# time, and no value taken twice. Against the development cluster
# (doc/dev-cluster.md A), from the repository's root:
#
#   tools/ai_interleave.sh
#   MYSQL2="$BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13002" \
#     tools/ai_interleave.sh               # both MySQL servers
#
# What it does:
#   1. builds ic_trans and makes ic_it.ai_t: id BIGINT UNSIGNED
#      AUTO_INCREMENT, the key; v INT, which ic_trans fills; who CHAR(1),
#      'm' on MySQL's rows and NULL on ic_trans's;
#   2. starts WRITERS MySQL clients in the background, each inserting a
#      batch of STATEMENTS statements of ROWS rows, again and again,
#      until step 3 is done; with MYSQL2 set, every other writer goes to
#      that server;
#   3. runs ic_trans RUNS times with --auto-increment --keep --count
#      COUNT, JOBS of them at once, each run a process of its own that
#      takes its keys as one block from the table's counter;
#   4. checks that every ic_trans run and every MySQL batch succeeded (a
#      value taken twice fails one of them: 630 for ic_trans, ERROR 1062
#      for MySQL), that every row is there, and how often the rows
#      switch between ic_trans's and MySQL's in id order, which shows
#      that the two took from the counter in turn;
#   5. inserts an explicit value far past the counter through MySQL and
#      runs ic_trans once more, whose keys must start past it.
#
# Each process ic_trans runs takes a free API slot for its life; JOBS
# at once need as many. The logs are in target/ai_interleave/. Exits 0
# when every check holds.
set -u

RONDB=${RONDB:-/Users/mikael/mysql_trees/rondb_2604_main}
BIN=${BIN:-$RONDB/debug_build/bin}
CS=${CS:-${IC_TEST_CONNECTSTRING:-localhost:1186}}
MYSQL=${MYSQL:-${IC_TEST_MYSQL:-"$BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000"}}
MYSQL2=${MYSQL2:-}
WRITERS=${WRITERS:-2}
STATEMENTS=${STATEMENTS:-10}
ROWS=${ROWS:-20}
RUNS=${RUNS:-20}
JOBS=${JOBS:-2}
COUNT=${COUNT:-500}
DIR=target/ai_interleave
TRANS=target/debug/ic_trans

mkdir -p "$DIR" || exit 1
rm -f "$DIR"/*
# The writers stop at the next batch once this file is there.
trap 'touch "$DIR/stop"' EXIT
trap 'touch "$DIR/stop"; exit 1' INT TERM

sql() {
  $MYSQL --batch --skip-column-names -e "$1"
}

cargo build -p ic_trans || exit 1
sql "CREATE DATABASE IF NOT EXISTS ic_it;
  DROP TABLE IF EXISTS ic_it.ai_t;
  CREATE TABLE ic_it.ai_t (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    v INT,
    who CHAR(1)
  ) ENGINE=NDB" || exit 1

# One batch of MySQL's: STATEMENTS inserts of ROWS rows, the ids its own.
awk -v n="$STATEMENTS" -v r="$ROWS" 'BEGIN {
  for (i = 0; i < n; i++) {
    line = "INSERT INTO ic_it.ai_t (v, who) VALUES "
    for (j = 0; j < r; j++)
      line = line (j ? ", " : "") "(0, \047m\047)"
    print line ";"
  }
}' > "$DIR/batch.sql" || exit 1

# A MySQL writer: batches until told to stop, how many it did left in
# its status file, or why it stopped.
writer() {
  w=$1
  client=$2
  batches=0
  while [ ! -f "$DIR/stop" ]; do
    if ! $client < "$DIR/batch.sql" > "$DIR/writer$w.log" 2>&1; then
      echo "failed after $batches batches" > "$DIR/writer$w.status"
      return 1
    fi
    batches=$((batches + 1))
  done
  echo "$batches" > "$DIR/writer$w.status"
}

# One ic_trans run into a log, tried again while the management server
# has no API slot free yet: a run that took no keys did nothing.
trans_run() {
  log=$1
  tries=0
  while :; do
    tries=$((tries + 1))
    if $TRANS -c "$CS" -d ic_it ai_t --auto-increment --keep \
        --count "$COUNT" > "$log" 2>&1; then
      return 0
    fi
    if grep -q "^Took keys" "$log" || [ "$tries" -ge 10 ]; then
      return 1
    fi
    sleep 1
  done
}

# An ic_trans job: runs j, j + JOBS, j + 2 JOBS and so on, in turn.
job() {
  r=$1
  while [ "$r" -le "$RUNS" ]; do
    if trans_run "$DIR/trans$r.log"; then
      echo ok > "$DIR/trans$r.status"
    else
      echo failed > "$DIR/trans$r.status"
    fi
    r=$((r + JOBS))
  done
}

echo "== $(date +%H:%M:%S) $WRITERS MySQL writers, $RUNS ic_trans runs" \
  "of $COUNT rows, $JOBS at once"
w=1
while [ "$w" -le "$WRITERS" ]; do
  client=$MYSQL
  if [ -n "$MYSQL2" ] && [ $((w % 2)) -eq 0 ]; then
    client=$MYSQL2
  fi
  writer "$w" "$client" &
  w=$((w + 1))
done
jobs_running=""
j=1
while [ "$j" -le "$JOBS" ]; do
  job "$j" &
  jobs_running="$jobs_running $!"
  j=$((j + 1))
done
wait $jobs_running
touch "$DIR/stop"
wait
echo "== $(date +%H:%M:%S) done; checking"

fail=0
ours=0
r=1
while [ "$r" -le "$RUNS" ]; do
  if [ "$(cat "$DIR/trans$r.status" 2>/dev/null)" = ok ]; then
    ours=$((ours + COUNT))
  else
    echo "ic_trans run $r failed: see $DIR/trans$r.log"
    fail=1
  fi
  r=$((r + 1))
done
theirs=0
w=1
while [ "$w" -le "$WRITERS" ]; do
  status=$(cat "$DIR/writer$w.status" 2>/dev/null)
  case "$status" in
    '' | *[!0-9]*)
      echo "MySQL writer $w ${status:-did not finish}: see $DIR/writer$w.log"
      fail=1
      ;;
    *)
      theirs=$((theirs + status * STATEMENTS * ROWS))
      ;;
  esac
  w=$((w + 1))
done

counts=$(sql "SELECT COUNT(*), COUNT(DISTINCT id), SUM(who IS NULL),
  SUM(who = 'm') FROM ic_it.ai_t")
set -- $counts
echo "rows $1, distinct ids $2; ic_trans's $3 (expected $ours)," \
  "MySQL's $4 (expected $theirs)"
if [ "$1" != "$2" ] || [ "$3" != "$ours" ] || [ "$4" != "$theirs" ]; then
  echo "the counts do not match"
  fail=1
fi

switches=$(sql "SELECT COUNT(*) FROM (
  SELECT IFNULL(who, 'i') AS w,
         LAG(IFNULL(who, 'i')) OVER (ORDER BY id) AS p
  FROM ic_it.ai_t) x WHERE p IS NOT NULL AND w <> p")
echo "in id order the rows switch between ic_trans's and MySQL's" \
  "$switches times"
if [ "${switches:-0}" -lt 2 ]; then
  echo "warning: the two did not take from the counter in turn;" \
    "a longer run (RUNS) or more writers may"
fi

# An explicit value past the counter, from MySQL: ic_trans's next keys
# come after it.
top=$(sql "SELECT MAX(id) FROM ic_it.ai_t")
high=$((top + 1000000))
if ! sql "INSERT INTO ic_it.ai_t (id, v, who) VALUES ($high, 0, 'x')"; then
  echo "MySQL's explicit value $high was refused"
  fail=1
elif trans_run "$DIR/trans_after.log"; then
  first=$(sed -n 's/^Took keys \([0-9]*\) to .*/\1/p' \
    "$DIR/trans_after.log")
  if [ -n "$first" ] && [ "$first" -gt "$high" ]; then
    echo "after MySQL's explicit $high, ic_trans's keys start at $first"
  else
    echo "ic_trans's keys start at ${first:-?}, not past $high"
    fail=1
  fi
else
  echo "the ic_trans run after the explicit value failed:" \
    "see $DIR/trans_after.log"
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "== every check holds"
else
  echo "== FAILED"
fi
exit "$fail"
