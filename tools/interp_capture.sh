#!/bin/sh
# Phase 9.1 (doc/rust/06-phases.md): interpreted programs sent by a
# MySQL server, one statement after another, for a capture read with
# the dissector against chapter 05 §9. Start the capture before the
# cluster, then this, then stop the capture. The dissector knows a
# signal link by its handshake, and the MySQL server's links come up
# when the cluster starts:
#
#   sudo tcpdump -i lo0 -w /tmp/interp.pcap \
#     'tcp port 1186 or tcp port 11861 or tcp port 11862'
#   (start the release cluster, doc/dev-cluster.md A)
#   tools/interp_capture.sh          # from the repository's root
#
# Each statement runs alone, two seconds after the last, and the time
# printed before it says which frames are its. A statement that fails
# is reported and the next one runs.
#
# A capture begun after the cluster has no handshakes; read it with the
# data nodes' ports decoded as NDB, and the dissector takes the bytes
# that are not text as messages:
#
#   tshark -r interp.pcap -d tcp.port==11861,ndb -d tcp.port==11862,ndb
#
# What each step is for (chapter 05 §9.6 lists what was not seen yet):
#   - scans with conditions of kinds scans2.pcap did not have: IS NULL
#     and IS NOT NULL, a column against another column, a CHAR column
#     (is the value sent at the column's whole size?), an unsigned
#     64-bit column, NOT, OR across columns, and LIKE (for later);
#   - key operations with a condition, true and false: a read, an
#     update and a delete by primary key (does MySQL send the condition
#     with the key operation, and what comes back when it fails?), and
#     an update adding to a column;
#   - AUTO_INCREMENT: inserts that take values from the table's counter
#     row (an interpreted update with a final read), and an explicit
#     value above the counter, which moves it up (a conditional update).
set -u

# RONDB names the RonDB tree the cluster runs from (doc/dev-cluster.md);
# RONDB_BIN, its programs' directory, may be given instead.
if [ -z "${RONDB_BIN:-}" ]; then
  RONDB_BIN=${RONDB:?set RONDB to the RonDB tree of the cluster}/prod_build/bin
fi
MYSQL=${MYSQL:-"$RONDB_BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000"}
ROWS=${ROWS:-1000}

step() {
  what=$1
  shift
  echo "== $(date +%H:%M:%S) $what"
  "$@" > /dev/null 2>&1 || echo "   failed: $*"
  sleep 2
}

sql() {
  what=$1
  step "$what" $MYSQL -e "$2"
}

# Columns 0 id, 1 val, 2 name (NULL on every seventh row), 3 code
# CHAR(4), 4 big BIGINT UNSIGNED.
$MYSQL -e "CREATE DATABASE IF NOT EXISTS ic_it;
  DROP TABLE IF EXISTS ic_it.interp_t;
  DROP TABLE IF EXISTS ic_it.interp_ai;
  CREATE TABLE ic_it.interp_t (
    id INT NOT NULL PRIMARY KEY,
    val INT NOT NULL,
    name VARCHAR(32) CHARACTER SET latin1,
    code CHAR(4) CHARACTER SET latin1 NOT NULL,
    big BIGINT UNSIGNED NOT NULL
  ) ENGINE=NDB;
  CREATE TABLE ic_it.interp_ai (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    val INT NOT NULL
  ) ENGINE=NDB;
  INSERT INTO ic_it.interp_t
    WITH RECURSIVE seq (n) AS
      (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < $ROWS)
    SELECT n, n * 10, IF(n % 7 = 0, NULL, CONCAT('r', n % 10)),
           CONCAT('c', n % 5), 18446744073709551000 + n % 600
    FROM seq;" || exit 1
echo "== $(date +%H:%M:%S) tables ic_it.interp_t ($ROWS rows), interp_ai made"
sleep 2

T=ic_it.interp_t
sql "scan: name IS NULL" "SELECT id FROM $T WHERE name IS NULL"
sql "scan: name IS NOT NULL AND val < 500" \
  "SELECT id FROM $T WHERE name IS NOT NULL AND val < 500"
sql "scan: column against column, val > id" \
  "SELECT id FROM $T WHERE val > id"
sql "scan: CHAR column, code = 'c3'" "SELECT id FROM $T WHERE code = 'c3'"
sql "scan: unsigned 64 bits, big >= 18446744073709551500" \
  "SELECT id FROM $T WHERE big >= 18446744073709551500"
sql "scan: NOT (val BETWEEN 100 AND 200)" \
  "SELECT id FROM $T WHERE NOT (val BETWEEN 100 AND 200)"
sql "scan: OR across columns, val = 10 OR name = 'r3'" \
  "SELECT id FROM $T WHERE val = 10 OR name = 'r3'"
sql "scan: LIKE, name LIKE 'r1%'" "SELECT id FROM $T WHERE name LIKE 'r1%'"
sql "key read, condition true: id = 5 AND val > 3" \
  "SELECT id FROM $T WHERE id = 5 AND val > 3"
sql "key read, condition false: id = 5 AND val > 100" \
  "SELECT id FROM $T WHERE id = 5 AND val > 100"
sql "key update, condition false: id = 5 AND val > 100" \
  "UPDATE $T SET val = 7 WHERE id = 5 AND val > 100"
sql "key update, condition true: id = 6 AND name = 'r6'" \
  "UPDATE $T SET val = 7 WHERE id = 6 AND name = 'r6'"
sql "key delete, condition false: id = 8 AND val > 100000" \
  "DELETE FROM $T WHERE id = 8 AND val > 100000"
sql "key delete, condition true: id = 9 AND val = 90" \
  "DELETE FROM $T WHERE id = 9 AND val = 90"
sql "key update adding to a column: val = val + 1 WHERE id = 11" \
  "UPDATE $T SET val = val + 1 WHERE id = 11"
A=ic_it.interp_ai
sql "AUTO_INCREMENT: the first insert takes values from the counter" \
  "INSERT INTO $A (val) VALUES (1)"
sql "AUTO_INCREMENT: three more, from the values taken" \
  "INSERT INTO $A (val) VALUES (2), (3), (4)"
sql "AUTO_INCREMENT: an explicit value above the counter, 100000" \
  "INSERT INTO $A (id, val) VALUES (100000, 5)"
sql "AUTO_INCREMENT: an insert after it" "INSERT INTO $A (val) VALUES (6)"
sql "AUTO_INCREMENT: ALTER TABLE ... AUTO_INCREMENT = 200000" \
  "ALTER TABLE $A AUTO_INCREMENT = 200000"
sql "AUTO_INCREMENT: an insert after the ALTER" \
  "INSERT INTO $A (val) VALUES (7)"
echo "== $(date +%H:%M:%S) done"
