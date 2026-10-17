/* scan_update: rows locked by a scan and updated through it, as
 * UPDATE ... WHERE does. An exclusive scan of a range of the index on
 * val runs on a transaction; each row's lock is taken over by an update
 * of its name, a query on that transaction; one commit ends them all.
 * A batch's receiver goes on only once the batch's takeovers are done.
 *
 * The name is updated, not val: changing the column a scan goes by
 * would move the row within the index being scanned.
 *
 *   ./scan_update [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_scan_update", &conn);
  IC_QUERY_ID queries[EX_SCAN_ROWS];
  IC_TABLE_DEF *table;
  IC_INDEX_DEF *index;
  IC_RECORD *rec;
  IC_RECORD *val_rec;
  IC_RECORD *name_rec;
  IC_SCAN_ID scan;
  IC_SCAN_ARGS args;
  IC_SCAN_RANGE range;
  IC_TAKEOVER_ARGS takeover;
  IC_TRANSACTION_HINT hint;
  IC_TRANS_ID trans;
  IC_QUERY_ID done;
  struct t1_row low;
  struct t1_row high;
  struct t1_row row;
  struct t1_row update;
  char name[32];
  const uint8_t *rows;
  uint8_t *attr;
  size_t len;
  bool more;
  bool found;
  uint32_t used = 0;
  uint32_t i;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  EX_CHECK(ic_apid_conn_index_bind(conn, EX_DB, EX_VAL_INDEX, EX_TABLE,
                                   &index));
  rec = ex_t1_record(table, 0);
  val_rec = ex_one_field_record(table, "val");
  name_rec = ex_one_field_record(table, "name");
  ex_write_scan_rows(conn, rec);
  EX_CHECK(ic_apid_index_scan_create(conn, index, val_rec, rec, &scan));

  /* A query for each row: its key comes from the scanned row, and its
   * attribute row holds the new name. */
  for (i = 0; i < EX_SCAN_ROWS; i++)
    EX_CHECK(ic_apid_query_create(conn, table, rec, name_rec, &queries[i]));

  /* Every scan example row: val from the lowest id's to the highest's. */
  range = ex_val_range(-(EX_SCAN_FIRST + EX_SCAN_ROWS - 1), -EX_SCAN_FIRST,
                       0, &low, &high);
  EX_CHECK(ic_apid_conn_add_scan_range(conn, scan, &range,
                                       (const uint8_t *)&low, sizeof low,
                                       (const uint8_t *)&high, sizeof high));
  hint.kind = IC_HINT_ANY;
  hint.value = 0;
  EX_CHECK(ic_apid_conn_start_transaction(conn, hint, table, &trans));
  ic_scan_args_init(&args, sizeof args);
  args.lock = IC_SCAN_LOCK_EXCLUSIVE;
  args.trans = trans; /* the transaction the locks are taken over into */
  args.batch_rows = 32;
  EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
  ic_takeover_args_init(&takeover, sizeof takeover);
  for (;;) {
    EX_CHECK(ic_apid_conn_next_row(conn, scan, &more));
    if (!more)
      break;
    if (used == EX_SCAN_ROWS)
      ex_fail("more rows than queries", IC_ERROR_INCONSISTENT_DATA);
    rows = ic_apid_scan_row(conn, scan, &len);
    memcpy(&row, rows, sizeof row);
    snprintf(name, sizeof name, "updated %u", (unsigned)row.id);
    ex_fill(&update, 0, 0, 0, name);
    attr = ic_apid_query_attr_row(conn, queries[used], &len);
    memcpy(attr, &update, sizeof update);
    /* The update goes with the next send; ic_apid_conn_next_row sends it
     * before it waits for the next batch. */
    EX_CHECK(ic_apid_conn_update_scan_row(conn, queries[used], scan,
                                          &takeover));
    used++;
  }
  EX_CHECK(ic_apid_scan_error(conn, scan));
  EX_CHECK(ic_apid_conn_commit_transaction(conn, trans));
  ex_finish(conn, trans);
  printf("%u rows updated by one scan and one commit\n", (unsigned)used);

  /* One of them, read back. */
  ex_fill(&row, EX_SCAN_FIRST, 0, 0, NULL);
  EX_CHECK(ic_apid_conn_read_committed(conn, rec, (const uint8_t *)&row,
                                       sizeof row, rec, (uint8_t *)&update,
                                       sizeof update, &found));
  if (found)
    ex_print("read back", &update);

  while (ic_apid_conn_get_next_executed_query(conn, &done))
    ;
  for (i = 0; i < EX_SCAN_ROWS; i++)
    EX_CHECK(ic_apid_query_free(conn, queries[i]));
  EX_CHECK(ic_apid_scan_free(conn, scan));
  ic_record_free(name_rec);
  ic_record_free(val_rec);
  ic_record_free(rec);
  ic_index_def_free(index);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
