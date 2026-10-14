/* unique: a row read and updated through the unique index on code,
 * the row found by the index rather than its primary key.
 *
 *   ./unique [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_unique", &conn);
  IC_TABLE_DEF *table;
  IC_INDEX_DEF *index;
  IC_RECORD *rec;
  IC_RECORD *code_rec;
  IC_RECORD *values_rec;
  IC_READ_KEY_ARGS read_args;
  IC_WRITE_KEY_ARGS write_args;
  IC_TRANSACTION_HINT hint;
  IC_QUERY_ID reader;
  IC_QUERY_ID updater;
  IC_TRANS_ID trans;
  struct t1_row row;
  struct t1_row key;
  uint8_t *rows;
  size_t len;
  bool found;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  /* A MySQL server names the hash index of UNIQUE KEY t1_code
   * t1_code$unique. */
  EX_CHECK(ic_apid_conn_index_bind(conn, EX_DB, EX_INDEX, EX_TABLE, &index));
  rec = ex_t1_record(table, 0);
  code_rec = ex_code_record(table);
  values_rec = ex_t1_record(table, 1);

  /* The row to find, written by primary key. */
  ex_fill(&row, 7, 70, 700, "seven");
  EX_CHECK(ic_apid_conn_write(conn, IC_WRITE_KIND_WRITE, rec,
                              (const uint8_t *)&row, sizeof row, rec,
                              (const uint8_t *)&row, sizeof row));

  /* A committed read through the index: the key row holds code. The
   * transaction starts where the index row is. */
  EX_CHECK(ic_apid_query_create_unique(conn, index, code_rec, rec, &reader));
  ex_fill(&key, 0, 0, 700, NULL);
  rows = ic_apid_query_key_row(conn, reader, &len);
  memcpy(rows, &key, sizeof key);
  hint = ic_apid_conn_hint_for_unique_key(conn, index, code_rec, rows, len);
  EX_CHECK(ic_apid_conn_start_transaction_on_index(conn, hint, index,
                                                   &trans));
  ic_read_key_args_init(&read_args, sizeof read_args);
  read_args.kind = IC_READ_KIND_COMMITTED;
  EX_CHECK(ic_apid_conn_read_key(conn, reader, trans, &read_args));
  EX_CHECK(ic_apid_conn_commit_transaction(conn, trans));
  ex_finish(conn, trans);
  EX_CHECK(ic_apid_query_error(conn, reader));
  rows = ic_apid_query_attr_row(conn, reader, &len);
  memcpy(&row, rows, sizeof row);
  ex_print("read by code 700", &row);

  /* An update through the index. Its values leave out the indexed
   * column: the row is found by it, and an update through the index
   * cannot change it. */
  EX_CHECK(ic_apid_query_create_unique(conn, index, code_rec, values_rec,
                                       &updater));
  rows = ic_apid_query_key_row(conn, updater, &len);
  memcpy(rows, &key, sizeof key);
  ex_fill(&row, 0, 71, 0, "seventy-one");
  rows = ic_apid_query_attr_row(conn, updater, &len);
  memcpy(rows, &row, sizeof row);
  EX_CHECK(ic_apid_conn_start_transaction_on_index(conn, hint, index,
                                                   &trans));
  ic_write_key_args_init(&write_args, sizeof write_args);
  write_args.kind = IC_WRITE_KIND_UPDATE;
  EX_CHECK(ic_apid_conn_write_key(conn, updater, trans, &write_args));
  EX_CHECK(ic_apid_conn_commit_transaction(conn, trans));
  ex_finish(conn, trans);
  EX_CHECK(ic_apid_query_error(conn, updater));

  /* Read back through the index once more, synchronously. */
  EX_CHECK(ic_apid_conn_read_unique_committed(
      conn, index, code_rec, (const uint8_t *)&key, sizeof key, rec,
      (uint8_t *)&row, sizeof row, &found));
  if (!found)
    ex_fail("the read back", IC_NDB_ERROR_NO_SUCH_ROW);
  ex_print("after the update", &row);

  EX_CHECK(ic_apid_query_free(conn, reader));
  EX_CHECK(ic_apid_query_free(conn, updater));
  ic_record_free(values_rec);
  ic_record_free(code_rec);
  ic_record_free(rec);
  ic_index_def_free(index);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
