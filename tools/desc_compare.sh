#!/bin/sh
# The 0.1 criterion "describes any MySQL-created table (all column types,
# unique and ordered indexes) as the reference's own table description
# tool does": a table with every column type a MySQL server puts in an
# NDB table, a unique and an ordered index, described by ndb_desc and by
# ic_desc. ic_desc's labels are its own (chapter 06 §6.1), so they are
# mapped onto the reference's before the two are compared; what differs
# after that is a fact. Prints the differences, nothing if there are
# none, and exits 1 if there are.
#
#   tools/desc_compare.sh          # from the repository's root
set -u

RONDB=${RONDB:-/Users/mikael/mysql_trees/rondb_2604_main}
RONDB_BIN=${RONDB_BIN:-$RONDB/prod_build/bin}
CS=${CS:-localhost:1186}
MYSQL=${MYSQL:-"$RONDB_BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000"}
IC_DESC=${IC_DESC:-target/release/ic_desc}
DIR=target/desc-compare
mkdir -p "$DIR"

$MYSQL -e "CREATE DATABASE IF NOT EXISTS ic_it;
  DROP TABLE IF EXISTS ic_it.alltypes;
  CREATE TABLE ic_it.alltypes (
    id INT NOT NULL PRIMARY KEY,
    t TINYINT, tu TINYINT UNSIGNED, s SMALLINT, su SMALLINT UNSIGNED,
    m MEDIUMINT, mu MEDIUMINT UNSIGNED, i INT, iu INT UNSIGNED,
    b BIGINT, bu BIGINT UNSIGNED, f FLOAT, d DOUBLE,
    dec1 DECIMAL(10,2), dec2 DECIMAL(30,10) UNSIGNED,
    c CHAR(10), cl CHAR(10) CHARACTER SET latin1,
    vc VARCHAR(100), lvc VARCHAR(1000),
    bn BINARY(10), vb VARBINARY(100), lvb VARBINARY(1000),
    dt DATE, tm TIME, tm3 TIME(3), dtm DATETIME, dtm6 DATETIME(6),
    ts TIMESTAMP NULL, ts3 TIMESTAMP(3) NULL, y YEAR,
    bit1 BIT(1), bit64 BIT(64), en ENUM('a','b','c'), st SET('x','y','z'),
    bl BLOB, tx TEXT, j JSON,
    dflt INT NOT NULL DEFAULT 42, code INT NOT NULL,
    UNIQUE KEY alltypes_code (code), KEY alltypes_iv (i, vc)
  ) ENGINE=NDB;" || exit 2

"$RONDB_BIN/ndb_desc" -c "$CS" -d ic_it alltypes 2>&1 \
  | grep -v '^NDBT_ProgramExit' > "$DIR/ndb_desc.txt"
"$IC_DESC" -c "$CS" -d ic_it alltypes 2>&1 \
  | sed -e '/^-- iClaustron --$/,$d' \
        -e 's/^Version: upper 0, lower \([0-9]*\)$/Version: \1/' \
        -e 's/^Fragment type: hash map$/Fragment type: HashMapPartition/' \
        -e 's/^Linear hash k:/K Value:/' \
        -e 's/^Min fill:/Min load factor:/' \
        -e 's/^Max fill:/Max load factor:/' \
        -e 's/^SQL metadata length:/Length of frm data:/' \
        -e 's/^Single user:/SingleUserMode:/' \
        -e 's/^Force varsize part:/ForceVarPart:/' \
        -e 's/^Partitions:/PartitionCount:/' \
        -e 's/^Fragments:/FragmentCount:/' \
        -e 's/^Partition balance:/PartitionBalance:/' \
        -e 's/^Extra GCI bits:/ExtraRowGciBits:/' \
        -e 's/^Extra author bits:/ExtraRowAuthorBits:/' \
        -e 's/^Status: fetched$/TableStatus: Retrieved/' \
        -e 's/^Hash map:/HashMap:/' \
        -e 's/ array=fixed/ AT=FIXED/' \
        -e 's/ array=short varsize/ AT=SHORT_VAR/' \
        -e 's/ array=long varsize/ AT=MEDIUM_VAR/' \
        -e 's/ storage=MEMORY/ ST=MEMORY/' \
        -e 's/ storage=DISK/ ST=DISK/' \
        -e 's/ blob=v\([0-9]\) parts=/ BV=\1 BT=/' \
        -e 's/ - unique hash index$/ - UniqueHashIndex/' \
        -e 's/ - ordered index$/ - OrderedIndex/' \
  > "$DIR/ic_desc.txt"

if diff -u "$DIR/ndb_desc.txt" "$DIR/ic_desc.txt"; then
  echo "ic_desc and ndb_desc agree on every line ($DIR)"
  exit 0
fi
exit 1
