/* conditional_update: programs that run in the data nodes, on the row
 * an operation holds there (doc/user-guide.md, section 6).
 *
 * - An update of a row's name that happens only while its val is still
 *   the value the application read, as an optimistic lock: once with
 *   val as it is, which commits, and once with a val that is out of
 *   date, which the data node refuses with 6000.
 * - A counter: val plus an input, with nothing read first, and the new
 *   value read back after the update, all in one round trip.
 * - A scan whose filter passes over, on the data nodes, every row but
 *   this example's with val above an input and a name.
 *
 *   ./conditional_update [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

/* This example's rows: ids from EX_COND_FIRST up, clear of the other
 * examples' rows, code the same as id, val ten times the row's place,
 * and a name on every other row. */
#define EX_COND_FIRST 300
#define EX_COND_ROWS 10
/* The filter's bound on val: rows 5 to 9 are above it, and of those 5,
 * 7 and 9 have a name. */
#define EX_COND_VAL_ABOVE 40

/* A record over id alone in a struct t1_row: the key, and what the
 * counter's update writes, which leaves val to its program. */
static IC_RECORD *id_record(const IC_TABLE_DEF *table)
{
  IC_FIELD_SPEC spec;
  IC_RECORD *rec;

  spec.field_id = ex_field(table, "id");
  spec.offset = offsetof(struct t1_row, id);
  spec.null_byte_offset = IC_NO_NULL_BIT;
  spec.null_bit = 0;
  EX_CHECK(ic_record_create(table, &spec, 1, sizeof(struct t1_row), &rec));
  return rec;
}

/* Put a row into a query's key row and attribute row. */
static void put_rows(IC_APID_CONNECTION *conn, IC_QUERY_ID query,
                     const struct t1_row *row)
{
  size_t len;

  memcpy(ic_apid_query_key_row(conn, query, &len), row, sizeof *row);
  memcpy(ic_apid_query_attr_row(conn, query, &len), row, sizeof *row);
}

/* Define an update on query as a transaction of its own, ask to commit
 * and wait: 0 if it committed, else the error it ended with, the
 * query's or the transaction's. The query is taken back from the
 * executed list, idle for its next use. */
static int32_t update_and_wait(IC_APID_CONNECTION *conn,
                               const IC_TABLE_DEF *table,
                               IC_QUERY_ID query, uint32_t abort_option)
{
  IC_TRANSACTION_HINT hint;
  IC_WRITE_KEY_ARGS args;
  IC_TRANS_ID trans;
  IC_QUERY_ID done;
  uint32_t state;
  int32_t error;

  hint.kind = IC_HINT_ANY;
  hint.value = 0;
  EX_CHECK(ic_apid_conn_start_transaction(conn, hint, table, &trans));
  ic_write_key_args_init(&args, sizeof args);
  args.kind = IC_WRITE_KIND_UPDATE;
  args.abort_option = abort_option;
  EX_CHECK(ic_apid_conn_write_key(conn, query, trans, &args));
  EX_CHECK(ic_apid_conn_commit_transaction(conn, trans));
  while (!ic_apid_transaction_is_done(conn, trans))
    EX_CHECK(ic_apid_conn_flush(conn, 100, 1, NULL));
  state = ic_apid_transaction_state(conn, trans);
  error = ic_apid_query_error(conn, query);
  if (error == 0)
    error = ic_apid_transaction_error(conn, trans);
  EX_CHECK(ic_apid_conn_close_transaction(conn, trans));
  while (ic_apid_conn_get_next_executed_query(conn, &done))
    ;
  if (state == IC_COMMIT_STATE_COMMITTED)
    return 0;
  return error != 0 ? error : IC_ERROR_INCONSISTENT_DATA;
}

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global =
      ex_connect(argc, argv, "ex_conditional_update", &conn);
  IC_TABLE_DEF *table;
  IC_RECORD *rec, *id_rec, *name_rec, *val_rec;
  IC_PROGRAM_BUILDER *b;
  IC_PROGRAM *still, *plus, *filter;
  IC_PROGRAM_COND same, ours, wanted;
  IC_QUERY_ID guarded, counter;
  IC_SCAN_ID scan;
  IC_SCAN_ARGS args;
  struct t1_row row;
  struct t1_row key;
  char name[16];
  size_t len;
  bool found, more;
  uint32_t id, val, name_id, place, times, seen = 0, count = 0;
  int32_t error;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  id = ex_field(table, "id");
  val = ex_field(table, "val");
  name_id = ex_field(table, "name");
  rec = ex_t1_record(table, 0);
  id_rec = id_record(table);
  name_rec = ex_one_field_record(table, "name");
  val_rec = ex_one_field_record(table, "val");

  /* The rows, over whatever an earlier run left. */
  for (place = 0; place < EX_COND_ROWS; place++) {
    snprintf(name, sizeof name, "row %u", (unsigned)place);
    ex_fill(&row, EX_COND_FIRST + place, (int32_t)(place * 10),
            EX_COND_FIRST + place, place % 2 ? name : NULL);
    EX_CHECK(ic_apid_conn_write(conn, IC_WRITE_KIND_WRITE, rec,
                                (const uint8_t *)&row, sizeof row, rec,
                                (const uint8_t *)&row, sizeof row));
  }

  /* 1. The optimistic update of row 300's name, whose val is 0: only
   * while val is still input 0. The program and the query are made once;
   * each use sets the input. A missed condition rolls the transaction
   * back with 6000 where the update aborts on error, and fails this
   * update alone with 626 where it ignores errors. */
  EX_CHECK(ic_program_builder_create(table, &b));
  same = ic_program_compare(b, IC_CMP_EQ, ic_program_column(b, val),
                            ic_program_input(b, 0));
  EX_CHECK(ic_program_builder_finish(b, same, &still));
  ic_program_builder_free(b);
  EX_CHECK(ic_apid_query_create(conn, table, rec, name_rec, &guarded));
  EX_CHECK(ic_apid_query_set_program(conn, guarded, still));

  ex_fill(&row, EX_COND_FIRST, 0, 0, "checked");
  put_rows(conn, guarded, &row);
  EX_CHECK(ic_apid_query_set_input(conn, guarded, 0, 0));
  EX_CHECK(update_and_wait(conn, table, guarded, IC_ABORT_ON_ERROR));
  printf("val still 0: the update committed\n");

  ex_fill(&row, EX_COND_FIRST, 0, 0, "too late");
  put_rows(conn, guarded, &row);
  EX_CHECK(ic_apid_query_set_input(conn, guarded, 0, 5));
  error = update_and_wait(conn, table, guarded, IC_ABORT_ON_ERROR);
  if (error != IC_NDB_ERROR_CONDITION)
    ex_fail("the update with val out of date", error);
  printf("val not 5 any more: refused with %d, the row as it was\n",
         (int)error);

  ex_fill(&key, EX_COND_FIRST, 0, 0, NULL);
  EX_CHECK(ic_apid_conn_read_committed(conn, rec, (const uint8_t *)&key,
                                       sizeof key, rec, (uint8_t *)&row,
                                       sizeof row, &found));
  ex_check(found, "row 300 is there");
  ex_print("row 300", &row);
  ex_check(!(row.null_bits & (EX_VAL_NULL | EX_NAME_NULL)) && row.val == 0 &&
               row.name_len == 7 && memcmp(row.name, "checked", 7) == 0,
           "row 300 is val 0, name 'checked': the refused update changed "
           "nothing");

  /* 2. The counter on row 301, whose val is 10: the data node adds, and
   * the update reads the new value back. Its attribute row holds the key
   * alone, which leaves val to the program. */
  EX_CHECK(ic_program_builder_create(table, &b));
  EX_CHECK(ic_program_assign(b, val,
                             ic_program_add(b, ic_program_column(b, val),
                                            ic_program_input(b, 0))));
  EX_CHECK(ic_program_builder_finish_no_condition(b, &plus));
  ic_program_builder_free(b);
  EX_CHECK(ic_apid_query_create(conn, table, id_rec, id_rec, &counter));
  EX_CHECK(ic_apid_query_set_program(conn, counter, plus));
  EX_CHECK(ic_apid_query_set_reads_after(conn, counter, val_rec));
  EX_CHECK(ic_apid_query_set_input(conn, counter, 0, 5));
  ex_fill(&row, EX_COND_FIRST + 1, 0, 0, NULL);
  put_rows(conn, counter, &row);
  for (times = 0; times < 2; times++) {
    EX_CHECK(update_and_wait(conn, table, counter, IC_ABORT_DEFAULT));
    memcpy(&row, ic_apid_query_after_row(conn, counter, &len), sizeof row);
    ex_check(!(row.null_bits & EX_VAL_NULL) &&
                 row.val == 10 + 5 * (int32_t)(times + 1),
             "the counter reads back 15, then 20");
    printf("row 301: val plus 5, read back after: %d\n", (int)row.val);
  }

  /* 3. A scan of the whole table, the filter run on every row in the
   * data nodes: id from input 1 up to below input 2, which are this
   * example's rows, val above input 0, and a name. Only rows 305, 307
   * and 309 come back, in the order the fragments send them. */
  EX_CHECK(ic_program_builder_create(table, &b));
  ours = ic_program_and(
      b,
      ic_program_compare(b, IC_CMP_GE, ic_program_column(b, id),
                         ic_program_input(b, 1)),
      ic_program_compare(b, IC_CMP_LT, ic_program_column(b, id),
                         ic_program_input(b, 2)));
  wanted = ic_program_and(
      b,
      ic_program_compare(b, IC_CMP_GT, ic_program_column(b, val),
                         ic_program_input(b, 0)),
      ic_program_is_not_null(b, ic_program_column(b, name_id)));
  EX_CHECK(ic_program_builder_finish(b, ic_program_and(b, ours, wanted),
                                     &filter));
  ic_program_builder_free(b);
  EX_CHECK(ic_apid_scan_create(conn, table, rec, &scan));
  EX_CHECK(ic_apid_conn_set_scan_program(conn, scan, filter));
  EX_CHECK(ic_apid_conn_set_scan_input(conn, scan, 0, EX_COND_VAL_ABOVE));
  EX_CHECK(ic_apid_conn_set_scan_input(conn, scan, 1, EX_COND_FIRST));
  EX_CHECK(ic_apid_conn_set_scan_input(conn, scan, 2,
                                       EX_COND_FIRST + EX_COND_ROWS));
  ic_scan_args_init(&args, sizeof args);
  EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
  printf("the filter, val above %d and a name, let through rows",
         EX_COND_VAL_ABOVE);
  for (;;) {
    EX_CHECK(ic_apid_conn_next_row(conn, scan, &more));
    if (!more)
      break;
    memcpy(&row, ic_apid_scan_row(conn, scan, &len), sizeof row);
    printf(" %u", (unsigned)row.id);
    /* A bit for each of this example's rows that comes back. */
    if (row.id >= EX_COND_FIRST && row.id < EX_COND_FIRST + EX_COND_ROWS)
      seen |= 1u << (row.id - EX_COND_FIRST);
    count++;
  }
  printf("\n");
  ex_check(count == 3 && seen == ((1u << 5) | (1u << 7) | (1u << 9)),
           "the filter lets through 305, 307, 309");

  EX_CHECK(ic_apid_scan_free(conn, scan));
  EX_CHECK(ic_apid_query_free(conn, guarded));
  EX_CHECK(ic_apid_query_free(conn, counter));
  ic_program_free(still);
  ic_program_free(plus);
  ic_program_free(filter);
  ic_record_free(val_rec);
  ic_record_free(name_rec);
  ic_record_free(id_rec);
  ic_record_free(rec);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  printf("conditional_update: all checks passed\n");
  return 0;
}
