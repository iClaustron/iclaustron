-- The table every example uses. Run it through a MySQL server of the
-- cluster, for instance on the development cluster (doc/dev-cluster.md):
--
--   $BIN/mysql -uroot --protocol=tcp --host=127.0.0.1 --port 13000 \
--     < examples/setup.sql

CREATE DATABASE IF NOT EXISTS ic_ex;
DROP TABLE IF EXISTS ic_ex.t1;
CREATE TABLE ic_ex.t1 (
  id   INT UNSIGNED NOT NULL PRIMARY KEY,
  val  INT,
  code INT UNSIGNED NOT NULL,
  name VARCHAR(32) CHARACTER SET latin1,
  UNIQUE KEY t1_code (code)
) ENGINE=NDB;
