# Development cluster (RonDB 26.10)

The integration tests and the tools need a live RonDB 26.10 cluster with
at least one free `[api]` slot. Two ways to get one. The local build is
the primary setup: the API node needs direct TCP access to every data
node's transporter port, which containers usually do not expose.

## A. Local build from the RonDB tree

Source: `/Users/mikael/mysql_trees/rondb_2604_main`.

```
cd /Users/mikael/mysql_trees/rondb_2604_main
mkdir -p build && cd build
cmake .. -DWITH_NDB=1 -DWITH_RDRS=0 -DWITH_ROUTER=0 -DWITH_UNIT_TESTS=0 \
         -DWITH_SSL=system -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j8
```

Binaries land in `build/bin` (`ndb_mgmd`, `ndbmtd`, `mysqld`, `ndb_mgm`,
`ndb_desc`, `ndb_config`, `mysql`).

### A.1 Minimal cluster: 1 management server, 2 data nodes, 1 MySQL server

Create a data directory, e.g. `~/rondb-dev`, with this `config.ini`:

```
[ndbd default]
NoOfReplicas=2
DataMemory=512M

[ndb_mgmd]
NodeId=65
HostName=localhost
PortNumber=1186
DataDir=/Users/mikael/rondb-dev/mgmd

[ndbd]
NodeId=1
HostName=localhost
DataDir=/Users/mikael/rondb-dev/ndbd1

[ndbd]
NodeId=2
HostName=localhost
DataDir=/Users/mikael/rondb-dev/ndbd2

[mysqld]
NodeId=67
HostName=localhost

# API slots for iClaustron tests and tools (no HostName: any host)
[api]
NodeId=68
[api]
NodeId=69
[api]
NodeId=70
[api]
NodeId=71
```

Start order (paths abbreviated; `BIN=/Users/mikael/mysql_trees/rondb_2604_main/build/bin`):

```
mkdir -p ~/rondb-dev/{mgmd,ndbd1,ndbd2,mysqld}
$BIN/ndb_mgmd -f ~/rondb-dev/config.ini --configdir=$HOME/rondb-dev/mgmd --initial
$BIN/ndbmtd -c localhost:1186 --ndb-nodeid=1 --initial
$BIN/ndbmtd -c localhost:1186 --ndb-nodeid=2 --initial
$BIN/ndb_mgm -e show            # wait until both data nodes say "started"
$BIN/mysqld --initialize-insecure --datadir=$HOME/rondb-dev/mysqld
$BIN/mysqld --datadir=$HOME/rondb-dev/mysqld --ndbcluster \
            --ndb-connectstring=localhost:1186 --port=3306 \
            --socket=/tmp/rondb-dev.sock &
$BIN/mysql -uroot -S /tmp/rondb-dev.sock -e "CREATE DATABASE IF NOT EXISTS test"
```

`--initial` only on the first start. Stop with `$BIN/ndb_mgm -e shutdown`
and `mysqladmin -uroot -S /tmp/rondb-dev.sock shutdown`.

### A.2 Running the integration tests against it

```
export IC_TEST_CONNECTSTRING=localhost:1186
export IC_TEST_MYSQL="$BIN/mysql -uroot -S /tmp/rondb-dev.sock"
export IC_TEST_NDB_MGM="$BIN/ndb_mgm -c localhost:1186"
cargo xtask test-integration
cargo xtask test-integration localhost:1186 pk_    # one group
```

The tests make their tables in the database `ic_it`. Without
`IC_TEST_MYSQL` only the `connect` group runs; without
`IC_TEST_NDB_MGM` the `failure` group returns without stopping
anything. The `failure` group stops the second started data node and
starts it again, which takes as long as the node takes to come back.

The connectstring for iClaustron is then `localhost:1186`:

```
export IC_TEST_CONNECTSTRING=localhost:1186
cargo xtask test-integration
cargo run -p ic_config_dump -- localhost:1186     # from Phase 2
```

The tests create their tables through the MySQL server; they read the
socket path from `IC_TEST_MYSQL_SOCKET` (default `/tmp/rondb-dev.sock`).

### A.2 Alternative: let MTR start a cluster

```
cd /Users/mikael/mysql_trees/rondb_2604_main/mysql-test
./mtr --start ndb.ndb_basic
```

leaves a cluster running; the connectstring and socket are in
`var/my.cnf` (`ndb-connectstring` and `socket` under `[mysqld.1.1]`).
MTR's configuration has a few spare `[api]` slots. Stop with Ctrl-C.

## B. Docker

`tools/rondb-cli/scripts/start-rondb.sh` in the RonDB tree starts the
official `rondb-docker` compose setup and exposes MySQL on 3306. For the
API node to work the management port (1186) **and** each data node's
transporter port must be reachable from the host, and the data nodes'
`HostName`s in the served configuration must resolve from the host. Check
the compose file before relying on this; use A when in doubt.

## Useful commands during development

```
$BIN/ndb_mgm -e show                        # node states
$BIN/ndb_mgm -e "2 RESTART"                 # restart one data node (failure tests)
$BIN/ndb_mgm -e "all report memory"
$BIN/ndb_desc -c localhost:1186 -d test t1  # compare with tools/ic_desc
$BIN/ndb_config -c localhost:1186 --nodes --query=nodeid,type,host,port
```
