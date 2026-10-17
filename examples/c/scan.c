/* scan: every row of the table, read by a scan of every fragment at
 * once. The scan is made once and run twice: row by row, then a batch
 * at a time, each batch handed back once its rows are read, which is
 * what lets that fragment's receiver go on.
 *
 *   ./scan [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_scan", &conn);
  IC_TABLE_DEF *table;
  IC_RECORD *rec;
  IC_SCAN_ID scan;
  IC_SCAN_ARGS args;
  IC_SCAN_BATCH_ID batch;
  struct t1_row row;
  const uint8_t *rows;
  size_t len;
  bool more;
  uint64_t count = 0;
  uint64_t ours = 0;
  uint64_t again = 0;
  uint32_t batches = 0;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  rec = ex_t1_record(table, 0);
  ex_write_scan_rows(conn, rec);
  EX_CHECK(ic_apid_scan_create(conn, table, rec, &scan));

  /* Row by row: committed reads, with no lock, of every fragment at
   * once. ic_apid_conn_next_row waits for batches and hands each back
   * as it goes. */
  ic_scan_args_init(&args, sizeof args);
  EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
  for (;;) {
    EX_CHECK(ic_apid_conn_next_row(conn, scan, &more));
    if (!more)
      break;
    rows = ic_apid_scan_row(conn, scan, &len);
    memcpy(&row, rows, sizeof row);
    count++;
    if (!(row.null_bits & EX_VAL_NULL) && row.val <= -EX_SCAN_FIRST &&
        row.val > -(EX_SCAN_FIRST + EX_SCAN_ROWS))
      ours++;
  }
  printf("%llu rows, %llu of them the scan examples'\n",
         (unsigned long long)count, (unsigned long long)ours);

  /* A batch at a time, at most 16 rows each: the program holds each
   * batch for as long as it likes, and hands it back. */
  args.batch_rows = 16;
  EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
  for (;;) {
    EX_CHECK(ic_apid_conn_next_batch(conn, scan, &batch, &more));
    if (!more)
      break;
    batches++;
    for (;;) {
      EX_CHECK(ic_apid_conn_next_scan_row(conn, batch, &more));
      if (!more)
        break;
      again++;
    }
    EX_CHECK(ic_apid_conn_release_scan_batch(conn, batch));
  }
  printf("again: %llu rows in %u batches\n", (unsigned long long)again,
         (unsigned)batches);

  EX_CHECK(ic_apid_scan_free(conn, scan));
  ic_record_free(rec);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
