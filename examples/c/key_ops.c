/* key_ops: insert, read, update and delete by primary key, one
 * synchronous call each: each is a transaction of its own, sent and
 * waited for before the call returns.
 *
 *   ./key_ops [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

/* Read row id and print it, or say it is not there. */
static void read_and_print(IC_APID_CONNECTION *conn, IC_RECORD *rec,
                           uint32_t id, const char *what)
{
  struct t1_row key;
  struct t1_row row;
  bool found;

  ex_fill(&key, id, 0, 0, NULL);
  EX_CHECK(ic_apid_conn_read_committed(conn, rec, (const uint8_t *)&key,
                                       sizeof key, rec, (uint8_t *)&row,
                                       sizeof row, &found));
  if (found)
    ex_print(what, &row);
  else
    printf("%s: no row %u\n", what, (unsigned)id);
}

/* Write row, as kind says. */
static int32_t write_row(IC_APID_CONNECTION *conn, IC_RECORD *rec,
                         uint32_t kind, const struct t1_row *row)
{
  return ic_apid_conn_write(conn, kind, rec, (const uint8_t *)row,
                            sizeof *row, rec, (const uint8_t *)row,
                            sizeof *row);
}

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_key_ops", &conn);
  IC_TABLE_DEF *table;
  IC_RECORD *rec;
  struct t1_row row;
  int32_t ret;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  rec = ex_t1_record(table, 0);

  /* Left from an earlier run, the row goes first; its absence is no
   * failure. */
  ex_fill(&row, 1, 0, 100, NULL);
  ret = write_row(conn, rec, IC_WRITE_KIND_DELETE, &row);
  if (ret != 0 && ret != IC_NDB_ERROR_NO_SUCH_ROW)
    ex_fail("delete", ret);

  ex_fill(&row, 1, 10, 100, "one");
  EX_CHECK(write_row(conn, rec, IC_WRITE_KIND_INSERT, &row));
  read_and_print(conn, rec, 1, "inserted");

  /* A second insert of the same key is refused, and the error says so. */
  ret = write_row(conn, rec, IC_WRITE_KIND_INSERT, &row);
  if (ret == IC_NDB_ERROR_ROW_EXISTS)
    printf("a second insert: error %d, as it should\n", (int)ret);
  else
    ex_fail("the second insert", ret);

  ex_fill(&row, 1, 0, 100, "uno");
  row.null_bits |= EX_VAL_NULL;
  EX_CHECK(write_row(conn, rec, IC_WRITE_KIND_UPDATE, &row));
  read_and_print(conn, rec, 1, "updated, val NULL");

  EX_CHECK(write_row(conn, rec, IC_WRITE_KIND_DELETE, &row));
  read_and_print(conn, rec, 1, "deleted");

  ic_record_free(rec);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
