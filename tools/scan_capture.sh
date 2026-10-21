#!/bin/sh
# Phase 8.2 (doc/rust/06-phases.md): the reference's scans, one after
# another, for a capture read with the dissector against chapter 05 §7.
# Start the capture before the cluster, then this, then stop the
# capture. The dissector knows a signal link by its handshake, and the
# MySQL server's links come up when the cluster starts:
#
#   sudo tcpdump -i lo0 -w /tmp/scans.pcap \
#     'tcp port 1186 or tcp port 11861 or tcp port 11862'
#   (start the release cluster, doc/dev-cluster.md A)
#   tools/scan_capture.sh            # from the repository's root
#
# Each scan runs alone, two seconds after the last, and the time printed
# before it says which frames are its. The table has ROWS rows (20 000
# by default), so that each of its 8 fragments needs several batches of
# the reference's 384 rows, and an ordered index on val. MTR's cluster
# allows 10 000 operations at once, so the rows go in 1 000 at a time.
#
# What each step is for:
#   - the reference's scan tool in each of its lock modes, with its
#     parallelism lowered, in storage order, and ordered by the index
#     both ways: the flags, the receivers, the batches, SCAN_MORE_REQ,
#     and the ordered merge refilling one receiver at a time;
#   - through a MySQL server: committed reads with a condition pushed
#     to the data nodes (the column section's interpreted program), a
#     range scan of the index (the bounds), two ranges and a descending
#     range (several ranges, ordered), a locked scan whose rows are
#     taken over to be updated and deleted (SCAN_ROW_KEY_REP and the
#     takeover KEY_OP_REQ), a locked read, scans closed early with and
#     without locks (SCAN_MORE_REQ with stop), and a locked scan that
#     waits on a row another transaction holds until the deadlock
#     timeout (3 s under MTR) ends it (SCAN_REF and the close it owes;
#     the MySQL client reports it as a failure).
set -u

# RONDB names the RonDB tree the cluster runs from (doc/dev-cluster.md);
# RONDB_BIN, its programs' directory, may be given instead.
if [ -z "${RONDB_BIN:-}" ]; then
  RONDB_BIN=${RONDB:?set RONDB to the RonDB tree of the cluster}/prod_build/bin
fi
CS=${CS:-localhost:1186}
MYSQL=${MYSQL:-"$RONDB_BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000"}
ROWS=${ROWS:-20000}
SELECT_ALL="$RONDB_BIN/ndb_select_all -c $CS -d ic_it --nodata"

step() {
  what=$1
  shift
  echo "== $(date +%H:%M:%S) $what"
  "$@" > /dev/null 2>&1 || echo "   failed: $*"
  sleep 2
}

$MYSQL -e "CREATE DATABASE IF NOT EXISTS ic_it;
  DROP TABLE IF EXISTS ic_it.scan_t;
  CREATE TABLE ic_it.scan_t (
    id INT NOT NULL PRIMARY KEY,
    val INT NOT NULL,
    name VARCHAR(32),
    KEY scan_val (val)
  ) ENGINE=NDB;" || exit 1
base=0
while [ $base -lt $ROWS ]; do
  $MYSQL -e "INSERT INTO ic_it.scan_t
    WITH RECURSIVE seq (n) AS
      (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 1000)
    SELECT n + $base, (n + $base) * 10, CONCAT('r', (n + $base) % 10)
    FROM seq;" || exit 1
  base=$((base + 1000))
done
echo "== $(date +%H:%M:%S) table ic_it.scan_t made, $ROWS rows"
sleep 2

step "full scan, lock 0 (read)" $SELECT_ALL scan_t --lock=0
step "full scan, lock 1 (read and hold)" $SELECT_ALL scan_t --lock=1
step "full scan, lock 2 (exclusive)" $SELECT_ALL scan_t --lock=2
step "full scan, parallelism 1" $SELECT_ALL scan_t --parallelism=1
step "full scan, storage order" $SELECT_ALL scan_t --tupscan
step "index scan, ordered ascending" $SELECT_ALL scan_t scan_val --order
step "index scan, ordered descending" \
  $SELECT_ALL scan_t scan_val --order --descending
step "MySQL: committed reads, condition pushed" \
  $MYSQL -e "SELECT id FROM ic_it.scan_t IGNORE INDEX (scan_val)
             WHERE val > 190000"
step "MySQL: range scan of the index" \
  $MYSQL -e "SELECT id FROM ic_it.scan_t FORCE INDEX (scan_val)
             WHERE val BETWEEN 100 AND 30000 ORDER BY val"
step "MySQL: two ranges of the index, ordered" \
  $MYSQL -e "SELECT id FROM ic_it.scan_t FORCE INDEX (scan_val)
             WHERE val BETWEEN 100 AND 30000
                OR val BETWEEN 150000 AND 180000 ORDER BY val"
step "MySQL: range scan of the index, descending" \
  $MYSQL -e "SELECT id FROM ic_it.scan_t FORCE INDEX (scan_val)
             WHERE val BETWEEN 100 AND 30000 ORDER BY val DESC"
step "MySQL: locked read of a scan's rows" \
  $MYSQL -e "BEGIN;
             SELECT id FROM ic_it.scan_t WHERE name = 'r3' FOR UPDATE;
             COMMIT;"
step "MySQL: update by scan, rows taken over" \
  $MYSQL -e "UPDATE ic_it.scan_t SET val = val + 1 WHERE name = 'r7'"
step "MySQL: delete by scan, rows taken over" \
  $MYSQL -e "DELETE FROM ic_it.scan_t WHERE name = 'r9'"
step "MySQL: a scan closed early" \
  $MYSQL -e "SELECT id FROM ic_it.scan_t LIMIT 5"
step "MySQL: a locked scan closed early" \
  $MYSQL -e "BEGIN;
             SELECT id FROM ic_it.scan_t LIMIT 5 FOR UPDATE;
             COMMIT;"
echo "== $(date +%H:%M:%S) a transaction locks row 5 for 8 s"
$MYSQL -e "BEGIN;
           SELECT id FROM ic_it.scan_t WHERE id = 5 FOR UPDATE;
           SELECT SLEEP(8);
           COMMIT;" > /dev/null 2>&1 &
holder=$!
sleep 1
step "MySQL: a locked scan waits on row 5 until the deadlock timeout" \
  $MYSQL -e "BEGIN;
             SELECT id FROM ic_it.scan_t WHERE name = 'r5' FOR UPDATE;
             COMMIT;"
wait $holder
echo "== $(date +%H:%M:%S) done"
