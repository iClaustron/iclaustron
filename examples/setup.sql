-- The tables of the examples: t1 for every example but auto_increment,
-- whose tables are t2 and t3. Run it through a MySQL server of the
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
  UNIQUE KEY t1_code (code),
  KEY t1_val (val)
) ENGINE=NDB;

-- auto_increment's: an AUTO_INCREMENT key, and a table without a
-- primary key, whose hidden key is one.
DROP TABLE IF EXISTS ic_ex.t2;
CREATE TABLE ic_ex.t2 (
  id  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  val INT
) ENGINE=NDB;
DROP TABLE IF EXISTS ic_ex.t3;
CREATE TABLE ic_ex.t3 (
  val INT
) ENGINE=NDB;
