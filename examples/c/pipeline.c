/* pipeline: write many rows asynchronously. A number of queries are in
 * flight at once, each a transaction of one write; when one completes,
 * its callback lets its transaction go and starts the next write with
 * the same query, so the pipeline stays full until every row is
 * written. The main loop only sends and polls.
 *
 *   ./pipeline [connectstring] [rows] [in flight]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include <time.h>

#include "t1.h"

/* The rows get ids from here up, clear of the other examples' rows. */
#define FIRST_ID 1000
#define MAX_IN_FLIGHT 256

struct pipeline {
  IC_TABLE_DEF *table;
  IC_RECORD *rec;
  uint32_t next;       /* the next id to write */
  uint32_t end;        /* one past the last */
  uint32_t done;
  uint32_t failed;
  int32_t first_error;
};

/* One query in flight: the callback's user_ref. */
struct slot {
  struct pipeline *p;
  IC_QUERY_ID query;
  IC_TRANS_ID trans;
};

static void on_written(IC_APID_CONNECTION *conn, IC_QUERY_ID query,
                       void *user_ref);

/* Start the write of row id on the slot's query: fill its rows, start a
 * transaction where the row lives, define the write, ask to commit.
 * It goes with the next send. */
static int32_t start_write(IC_APID_CONNECTION *conn, struct slot *s,
                           uint32_t id)
{
  struct pipeline *p = s->p;
  struct t1_row row;
  IC_WRITE_KEY_ARGS args;
  IC_TRANSACTION_HINT hint;
  uint8_t *key;
  uint8_t *attr;
  size_t key_len;
  size_t attr_len;
  int32_t ret;

  ex_fill(&row, id, (int32_t)id * 2, id, "piped");
  /* The query owns its rows; the struct is copied in. */
  key = ic_apid_query_key_row(conn, s->query, &key_len);
  attr = ic_apid_query_attr_row(conn, s->query, &attr_len);
  if (key == NULL || attr == NULL || key_len < sizeof row)
    return IC_ERROR_NULL_POINTER;
  memcpy(key, &row, sizeof row);
  memcpy(attr, &row, sizeof row);
  hint = ic_apid_conn_hint_for_key(conn, p->table, p->rec, key, key_len);
  ret = ic_apid_conn_start_transaction(conn, hint, p->table, &s->trans);
  if (ret != 0)
    return ret;
  ic_write_key_args_init(&args, sizeof args);
  args.kind = IC_WRITE_KIND_WRITE;  /* insert, or overwrite a rerun's */
  args.callback = on_written;
  args.user_ref = s;
  ret = ic_apid_conn_write_key(conn, s->query, s->trans, &args);
  if (ret != 0)
    return ret;
  return ic_apid_conn_commit_transaction(conn, s->trans);
}

/* Called from inside a poll when a write completes. Its transaction is
 * done: the commit went with the write. */
static void on_written(IC_APID_CONNECTION *conn, IC_QUERY_ID query,
                       void *user_ref)
{
  struct slot *s = user_ref;
  struct pipeline *p = s->p;
  int32_t ret = ic_apid_query_error(conn, query);

  p->done++;
  if (ret != 0 && p->failed++ == 0)
    p->first_error = ret;
  ic_apid_conn_close_transaction(conn, s->trans);
  while (p->next < p->end) {
    ret = start_write(conn, s, p->next++);
    if (ret == 0)
      break;
    /* Not started: count it as done, and failed. */
    p->done++;
    if (p->failed++ == 0)
      p->first_error = ret;
  }
}

static double seconds_since(const struct timespec *start)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
  uint32_t rows = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 10000;
  uint32_t width = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 32;
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_pipeline", &conn);
  static struct slot slots[MAX_IN_FLIGHT];
  struct pipeline p;
  struct timespec start;
  double seconds;
  uint32_t i;

  if (width == 0 || width > MAX_IN_FLIGHT)
    width = MAX_IN_FLIGHT;
  if (width > rows)
    width = rows;
  memset(&p, 0, sizeof p);
  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &p.table));
  p.rec = ex_t1_record(p.table, 0);
  p.next = FIRST_ID;
  p.end = FIRST_ID + rows;

  clock_gettime(CLOCK_MONOTONIC, &start);
  for (i = 0; i < width; i++) {
    slots[i].p = &p;
    EX_CHECK(ic_apid_query_create(conn, p.table, p.rec, p.rec,
                                  &slots[i].query));
    EX_CHECK(start_write(conn, &slots[i], p.next++));
  }
  /* Send what is defined and take the replies; the callbacks, run
   * inside, keep the pipeline full. Without force, a send may wait a
   * moment for company, never longer than the adaptive send allows. */
  while (p.done < rows)
    EX_CHECK(ic_apid_conn_flush(conn, 10, 0, NULL));
  seconds = seconds_since(&start);

  printf("%u rows written, %u at a time, in %.3f s: %.0f rows/s\n",
         (unsigned)rows, (unsigned)width, seconds,
         seconds > 0 ? rows / seconds : 0.0);
  if (p.failed > 0)
    ex_fail("some writes", p.first_error);

  for (i = 0; i < width; i++)
    EX_CHECK(ic_apid_query_free(conn, slots[i].query));
  ic_record_free(p.rec);
  ic_table_def_free(p.table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
