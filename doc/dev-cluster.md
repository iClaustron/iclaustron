# Development cluster (RonDB 26.10)

The integration tests and the tools need a live RonDB 26.10 cluster with
at least one free `[api]` slot. The API node needs direct TCP access to
the management server and to every data node's port, so the cluster
runs on this machine.

## A. A cluster started by MTR, on fixed ports

The usual setup: the RonDB tree's own test runner starts a cluster and
keeps it running, with the management server and the data nodes on
fixed ports, so that a capture can be set up for them beforehand.

Source: `/Users/mikael/mysql_trees/rondb_2604_main`, with two builds:

- `debug_build`: a debug build (`CMAKE_BUILD_TYPE=Debug`,
  `WITH_DEBUG=1`), made with `build_scripts/build_script_debug.sh`. The
  cluster runs from this one.
- `prod_build`: a release build with debug information
  (`RelWithDebInfo`), made with `build_scripts/build_script_prod.sh`,
  for measurements.

Each has its programs in `bin/`; the commands below name each by its
file name, with

```sh
BIN=/Users/mikael/mysql_trees/rondb_2604_main/debug_build/bin
```

Start the cluster and keep it running until Ctrl-C:

```sh
cd /Users/mikael/mysql_trees/rondb_2604_main/debug_build/mysql-test
./mtr --suite=ndb ndb_setup_fixed_ports --start
```

The test `ndb_setup_fixed_ports` tests nothing; its `.cnf` fixes the
ports. What runs, as `var/my.cnf` describes it:

| Node | Node id | Port |
|---|---|---|
| management server | 3 | 1186 |
| data node | 1 | 11861 |
| data node | 2 | 11862 |
| MySQL server 1 | 494, with 563 and 1271 in its pool | 13000, socket `var/tmp/mysqld-13000.sock` |
| MySQL server 2 | 1600, with 322 and 483 in its pool | 13002, socket `var/tmp/mysqld-13002.sock` |
| free API slots | 192, 228, 255, and five without a fixed id | — |

`var/` is `debug_build/mysql-test/var`. The data nodes' and the
management server's files and logs are in `var/mysql_cluster.1/`, the
MySQL servers' in `var/log/`. The connectstring for iClaustron is
`localhost:1186`; the management server gives a tool one of the free
API slots, a numbered one or one it numbers itself (such as 229).

### A.1 Running the integration tests against it

```sh
export IC_TEST_CONNECTSTRING=localhost:1186
export IC_TEST_MYSQL="$BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000"
export IC_TEST_NDB_MGM="$BIN/ndb_mgm -c localhost:1186"
cargo xtask test-integration
cargo xtask test-integration localhost:1186 pk_    # one group
```

`IC_TEST_MYSQL` may name the socket instead:
`$BIN/mysql -uroot -S /Users/mikael/mysql_trees/rondb_2604_main/debug_build/mysql-test/var/tmp/mysqld-13000.sock`.

The tests make their tables through the MySQL server, in the database
`ic_it`. The `big_rows` group also needs disk data: when the cluster
has no log file group it makes `ic_it_lg` (an undo file of 32 MB) and
the tablespace `ic_it_ts` (a data file of 64 MB), and keeps them; a
cluster has at most one log file group, and one already there is used.
The files are in each data node's file system directory, so a new MTR
cluster starts without them. Without `IC_TEST_MYSQL` only the `connect` group runs; without
`IC_TEST_NDB_MGM` the `failure` group returns without stopping
anything. The `failure` group stops the second started data node and
starts it again, which takes as long as the node takes to come back.

The tools take the same connectstring:

```sh
cargo run -p ic_config_dump -- localhost:1186
cargo run -p ic_desc -- -c localhost:1186 -d ic_it pk
```

### A.2 Capturing what goes over the wire

Every link of the cluster is on the loopback device, on the three
fixed ports. Capture while the tools or the tests run, then read the
capture with the dissector (chapter 06 §6.7), built in the Wireshark
tree beside this one:

```sh
sudo tcpdump -i lo0 -w /tmp/ndb.pcap 'tcp port 1186 or tcp port 11861 or tcp port 11862'
# run the tools or the tests, then stop tcpdump with Ctrl-C
/Users/mikael/github/wireshark/build/run/tshark -r /tmp/ndb.pcap -O ndb
/Users/mikael/github/wireshark/build/run/tshark -r /tmp/ndb.pcap -q -z expert
```

The links between the cluster's own nodes come up when the cluster
starts; start the capture before that to see their handshakes. A tool's
links are new with every run.

## B. A cluster started by hand

For a cluster of other shapes, or from the release build. Create a
data directory, e.g. `~/rondb-dev`, with this `config.ini`:

```ini
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
ServerPort=11861
DataDir=/Users/mikael/rondb-dev/ndbd1

[ndbd]
NodeId=2
HostName=localhost
ServerPort=11862
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

Start order, with `BIN` the `bin/` of either build:

```sh
mkdir -p ~/rondb-dev/{mgmd,ndbd1,ndbd2,mysqld}
$BIN/ndb_mgmd -f ~/rondb-dev/config.ini --configdir=$HOME/rondb-dev/mgmd --initial
$BIN/ndbmtd -c localhost:1186 --ndb-nodeid=1 --initial
$BIN/ndbmtd -c localhost:1186 --ndb-nodeid=2 --initial
$BIN/ndb_mgm -e show            # wait until both data nodes say "started"
$BIN/mysqld --initialize-insecure --datadir=$HOME/rondb-dev/mysqld
$BIN/mysqld --datadir=$HOME/rondb-dev/mysqld --ndbcluster \
            --ndb-connectstring=localhost:1186 --port=3306 \
            --socket=/tmp/rondb-dev.sock &
```

`--initial` only on the first start. The data nodes listen on 11861
and 11862, as in A, so a capture is set up the same way. For the
integration tests,
`IC_TEST_MYSQL="$BIN/mysql -uroot -S /tmp/rondb-dev.sock"`. To stop the
cluster and the MySQL server:

```sh
$BIN/ndb_mgm -e shutdown
$BIN/mysqladmin -uroot -S /tmp/rondb-dev.sock shutdown
```

## C. Docker

`tools/rondb-cli/scripts/start-rondb.sh` in the RonDB tree starts the
official `rondb-docker` compose setup and exposes MySQL on 3306. For the
API node to work the management port (1186) **and** each data node's
transporter port must be reachable from the host, and the data nodes'
host names in the served configuration (parameter 5, our `host`) must
resolve from the host. Check the compose file before relying on this;
use A when in doubt.

## Useful commands during development

```sh
$BIN/ndb_mgm -c localhost:1186 -e show                 # node states
$BIN/ndb_mgm -c localhost:1186 -e "2 RESTART"          # restart one data node
$BIN/ndb_mgm -c localhost:1186 -e "all report memory"
$BIN/ndb_desc -c localhost:1186 -d ic_it pk            # compare with tools/ic_desc
$BIN/ndb_config -c localhost:1186 --nodes --query=nodeid,type,host,port
$BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000   # SQL on MySQL server 1
```
