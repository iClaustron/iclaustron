/* scan_executor: scans for threads with no connection of their own. An
 * executor runs them on its connection, and each thread reads its rows
 * through a reader of its own; a batch moves to the reader without a
 * copy, and the receiver goes on once the reader has read it. Four
 * threads each read a quarter of the scan examples' rows at the same
 * time, by a range of the index on val, in the index's order, which the
 * reader merges.
 *
 *   ./scan_executor [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include <pthread.h>

#include "t1.h"

#define THREADS 4

/* What one thread reads, and what it found. */
struct quarter {
  IC_APID_EXECUTOR *executor;
  IC_SCAN_SHAPE *shape;
  int32_t from;
  int32_t to;
  uint64_t rows;
  int in_order;
};

/* Read the vals from q->from to q->to through a reader of the shape,
 * with a submitter of this thread's own. */
static void *read_quarter(void *arg)
{
  struct quarter *q = arg;
  IC_APID_SUBMITTER *submitter;
  IC_SCAN_READER *reader;
  IC_SCAN_ARGS args;
  IC_SCAN_RANGE range;
  struct t1_row low;
  struct t1_row high;
  struct t1_row row;
  const uint8_t *rows;
  size_t len;
  bool more;
  int32_t last = INT32_MIN;

  EX_CHECK(ic_apid_executor_submitter(q->executor, &submitter));
  EX_CHECK(ic_scan_reader_create(q->shape, &reader));
  range = ex_val_range(q->from, q->to, 0, &low, &high);
  EX_CHECK(ic_scan_reader_add_range(reader, &range, (const uint8_t *)&low,
                                    sizeof low, (const uint8_t *)&high,
                                    sizeof high));
  ic_scan_args_init(&args, sizeof args);
  args.ordered = true;
  EX_CHECK(ic_apid_submitter_scan(submitter, reader, &args));
  q->in_order = 1;
  for (;;) {
    EX_CHECK(ic_scan_reader_next_row(reader, &more));
    if (!more)
      break;
    rows = ic_scan_reader_row(reader, &len);
    memcpy(&row, rows, sizeof row);
    if (row.val < last)
      q->in_order = 0;
    last = row.val;
    q->rows++;
  }
  ic_scan_reader_free(reader);
  ic_apid_submitter_free(submitter);
  return NULL;
}

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global =
      ex_connect(argc, argv, "ex_scan_executor", &conn);
  struct quarter quarters[THREADS];
  pthread_t threads[THREADS];
  IC_APID_EXECUTOR *executor;
  IC_APID_SUBMITTER *submitter;
  IC_TABLE_DEF *table;
  IC_INDEX_DEF *index;
  IC_RECORD *rec;
  IC_RECORD *val_rec;
  IC_SCAN_SHAPE *shape;
  uint32_t each = EX_SCAN_ROWS / THREADS;
  uint64_t total = 0;
  uint32_t i;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  rec = ex_t1_record(table, 0);
  val_rec = ex_one_field_record(table, "val");
  ex_write_scan_rows(conn, rec);
  EX_CHECK(ic_apid_executor_start(global, NULL, &executor));
  EX_CHECK(ic_apid_executor_submitter(executor, &submitter));
  EX_CHECK(ic_apid_submitter_index_bind(submitter, EX_DB, EX_VAL_INDEX,
                                        EX_TABLE, &index));
  /* A shape is made once and shared by every reader that uses it. */
  EX_CHECK(ic_scan_shape_create_index(index, val_rec, rec, &shape));

  for (i = 0; i < THREADS; i++) {
    uint32_t first = EX_SCAN_FIRST + i * each;

    quarters[i].executor = executor;
    quarters[i].shape = shape;
    quarters[i].from = -(int32_t)(first + each - 1);
    quarters[i].to = -(int32_t)first;
    quarters[i].rows = 0;
    quarters[i].in_order = 0;
    if (pthread_create(&threads[i], NULL, read_quarter, &quarters[i]) != 0)
      ex_fail("pthread_create", IC_ERROR_INCONSISTENT_DATA);
  }
  for (i = 0; i < THREADS; i++) {
    pthread_join(threads[i], NULL);
    printf("thread %u: %llu rows, in order: %s\n", (unsigned)i,
           (unsigned long long)quarters[i].rows,
           quarters[i].in_order ? "yes" : "no");
    total += quarters[i].rows;
  }
  printf("%llu rows read by %d threads through one executor\n",
         (unsigned long long)total, THREADS);

  ic_scan_shape_free(shape);
  ic_index_def_free(index);
  ic_apid_submitter_free(submitter);
  ic_apid_executor_free(executor);
  ic_record_free(val_rec);
  ic_record_free(rec);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
