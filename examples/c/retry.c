/* retry: a transaction tried again after a temporary error.
 *
 * One transaction holds a row with an exclusive read and does not end.
 * Another updates the same row: it waits for the lock, and when the
 * data node's lock wait limit runs out it is rolled back with 266, a
 * temporary error. The program then lets the first transaction go,
 * as a conflict usually goes away by itself, and tries the update
 * again, in a new transaction, which commits.
 *
 *   ./retry [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include <time.h>

#include "t1.h"

#define ATTEMPTS 5

/* Flush until the transaction is done, and give how it ended: 0 if it
 * committed, else the error that ended it, the transaction's or else
 * its query's. */
static int32_t wait_for(IC_APID_CONNECTION *conn, IC_TRANS_ID trans,
                        IC_QUERY_ID query)
{
  int32_t ret;

  while (!ic_apid_transaction_is_done(conn, trans))
    EX_CHECK(ic_apid_conn_flush(conn, 100, 1, NULL));
  ret = ic_apid_transaction_error(conn, trans);
  if (ret == 0)
    ret = ic_apid_query_error(conn, query);
  if (ret == 0 &&
      ic_apid_transaction_state(conn, trans) != IC_COMMIT_STATE_COMMITTED)
    ret = IC_NDB_ERROR_NO_REPLY;
  EX_CHECK(ic_apid_conn_close_transaction(conn, trans));
  return ret;
}

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_retry", &conn);
  IC_TABLE_DEF *table;
  IC_RECORD *rec;
  IC_READ_KEY_ARGS read_args;
  IC_WRITE_KEY_ARGS write_args;
  IC_TRANSACTION_HINT hint;
  IC_QUERY_ID holder;
  IC_QUERY_ID updater;
  IC_TRANS_ID holding;
  IC_TRANS_ID trans;
  struct t1_row row;
  uint8_t *rows;
  size_t len;
  int holding_open = 1;
  int attempt;
  int32_t ret = 0;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  rec = ex_t1_record(table, 0);
  ex_fill(&row, 9, 90, 900, "nine");
  EX_CHECK(ic_apid_conn_write(conn, IC_WRITE_KIND_WRITE, rec,
                              (const uint8_t *)&row, sizeof row, rec,
                              (const uint8_t *)&row, sizeof row));
  hint = ic_apid_conn_hint_for_key(conn, table, rec, (const uint8_t *)&row,
                                   sizeof row);

  /* The holder: an exclusive read, sent and completed, and the
   * transaction left open, so that it keeps the row's lock. */
  EX_CHECK(ic_apid_query_create(conn, table, rec, rec, &holder));
  rows = ic_apid_query_key_row(conn, holder, &len);
  memcpy(rows, &row, sizeof row);
  EX_CHECK(ic_apid_conn_start_transaction(conn, hint, table, &holding));
  ic_read_key_args_init(&read_args, sizeof read_args);
  read_args.kind = IC_READ_KIND_EXCLUSIVE;
  EX_CHECK(ic_apid_conn_read_key(conn, holder, holding, &read_args));
  while (ic_apid_query_state(conn, holder) != IC_QUERY_COMPLETED &&
         ic_apid_query_state(conn, holder) != IC_QUERY_IDLE)
    EX_CHECK(ic_apid_conn_flush(conn, 100, 1, NULL));
  EX_CHECK(ic_apid_query_error(conn, holder));
  printf("row 9 is held by another transaction\n");

  /* The update, tried until it commits or fails for good. */
  EX_CHECK(ic_apid_query_create(conn, table, rec, rec, &updater));
  ex_fill(&row, 9, 91, 900, "nine, again");
  ic_write_key_args_init(&write_args, sizeof write_args);
  write_args.kind = IC_WRITE_KIND_UPDATE;
  for (attempt = 1; attempt <= ATTEMPTS; attempt++) {
    rows = ic_apid_query_key_row(conn, updater, &len);
    memcpy(rows, &row, sizeof row);
    rows = ic_apid_query_attr_row(conn, updater, &len);
    memcpy(rows, &row, sizeof row);
    EX_CHECK(ic_apid_conn_start_transaction(conn, hint, table, &trans));
    EX_CHECK(ic_apid_conn_write_key(conn, updater, trans, &write_args));
    EX_CHECK(ic_apid_conn_commit_transaction(conn, trans));
    ret = wait_for(conn, trans, updater);
    if (ret == 0) {
      printf("attempt %d: committed\n", attempt);
      break;
    }
    if (!ic_error_is_temporary(ret))
      ex_fail("the update", ret);
    {
      char text[200];

      ic_error_message(ret, text, sizeof text);
      printf("attempt %d: error %d, %s; temporary, so again\n", attempt,
             (int)ret, text);
    }
    if (holding_open) {
      /* The conflict goes away: the holder rolls back. */
      EX_CHECK(ic_apid_conn_rollback_transaction(conn, holding));
      wait_for(conn, holding, holder);
      holding_open = 0;
    }
    {
      /* A little longer before each attempt. */
      struct timespec pause = { 0, 100000000L * attempt };

      nanosleep(&pause, NULL);
    }
  }
  if (ret != 0)
    ex_fail("the update, every attempt", ret);

  EX_CHECK(ic_apid_query_free(conn, holder));
  EX_CHECK(ic_apid_query_free(conn, updater));
  ic_record_free(rec);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
