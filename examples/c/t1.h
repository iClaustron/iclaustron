/* What the C examples share: the row of ic_ex.t1 as a C struct, the
 * record over it, connecting, stopping with the error's text, and the
 * rows the scan examples read.
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#ifndef EX_T1_H
#define EX_T1_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iclaustron/ic_apid.h>

#define EX_DB "ic_ex"
#define EX_TABLE "t1"
#define EX_INDEX "t1_code$unique"
/* The ordered index a MySQL server makes for KEY t1_val (val). */
#define EX_VAL_INDEX "t1_val"
/* The scan examples' rows: ids from EX_SCAN_FIRST up, clear of the
 * other examples' rows, code the same as id, and val the id negated,
 * which no other example writes, so that a range of val finds these
 * alone. */
#define EX_SCAN_FIRST 200
#define EX_SCAN_ROWS 100
/* How long to wait for the data nodes to start, in milliseconds. */
#define EX_START_WAIT_MS 30000

/* A row of ic_ex.t1, as the record lays it out: each value as the data
 * nodes keep it, a VARCHAR as its length byte and then its bytes. */
struct t1_row {
  uint32_t id;        /* INT UNSIGNED NOT NULL, the primary key */
  int32_t  val;       /* INT, nullable: null bit 0 */
  uint32_t code;      /* INT UNSIGNED NOT NULL, the unique key */
  uint8_t  name_len;  /* VARCHAR(32) latin1, nullable: null bit 1 */
  char     name[32];
  uint8_t  null_bits;
};

#define EX_VAL_NULL  0x01
#define EX_NAME_NULL 0x02

/* Print what failed and why, and stop. */
static inline void ex_fail(const char *what, int32_t code)
{
  char text[200];

  ic_error_message(code, text, sizeof text);
  fprintf(stderr, "%s failed: error %d, %s\n", what, (int)code, text);
  exit(1);
}

/* A check of a result the example knows to expect: on a mismatch, say
 * what was wrong and stop with 1. A run whose checks all held ends with
 * "all checks passed". */
static inline void ex_check(int holds, const char *what)
{
  if (!holds) {
    fprintf(stderr, "check failed: %s\n", what);
    exit(1);
  }
}

/* Call, and stop if the call fails. */
#define EX_CHECK(call)                \
  do {                                \
    int32_t ex_ret = (call);          \
    if (ex_ret != 0)                  \
      ex_fail(#call, ex_ret);         \
  } while (0)

/* Start the Data API with the connectstring in argv[1], wait for the
 * data nodes, and make a connection for this thread. */
static inline IC_APID_GLOBAL *ex_connect(int argc, char **argv,
                                         const char *name,
                                         IC_APID_CONNECTION **conn)
{
  const char *connect_string = argc > 1 ? argv[1] : "localhost:1186";
  IC_GLOBAL_OPTIONS options;
  IC_APID_GLOBAL *global;
  uint32_t started = 0;

  ic_global_options_init(&options, sizeof options);
  EX_CHECK(ic_apid_global_connect(connect_string, name, &options, &global));
  EX_CHECK(ic_apid_global_wait_for_started(global, EX_START_WAIT_MS, 1,
                                           &started));
  if (started == 0) {
    fprintf(stderr, "no data node started\n");
    exit(1);
  }
  EX_CHECK(ic_apid_global_create_connection(global, conn));
  return global;
}

/* The id of the field called name. */
static inline uint32_t ex_field(const IC_TABLE_DEF *table,
                                const char *name)
{
  uint32_t id;

  EX_CHECK(ic_table_def_field_id(table, name, &id));
  return id;
}

/* A record over struct t1_row: every field, or with values_only the
 * ones an update through the unique index may set, val and name. */
static inline IC_RECORD *ex_t1_record(const IC_TABLE_DEF *table,
                                      int values_only)
{
  IC_FIELD_SPEC spec[4];
  IC_RECORD *rec;
  uint32_t n = 0;

  if (!values_only) {
    spec[n].field_id = ex_field(table, "id");
    spec[n].offset = offsetof(struct t1_row, id);
    spec[n].null_byte_offset = IC_NO_NULL_BIT;
    spec[n].null_bit = 0;
    n++;
    spec[n].field_id = ex_field(table, "code");
    spec[n].offset = offsetof(struct t1_row, code);
    spec[n].null_byte_offset = IC_NO_NULL_BIT;
    spec[n].null_bit = 0;
    n++;
  }
  spec[n].field_id = ex_field(table, "val");
  spec[n].offset = offsetof(struct t1_row, val);
  spec[n].null_byte_offset = offsetof(struct t1_row, null_bits);
  spec[n].null_bit = 0;
  n++;
  spec[n].field_id = ex_field(table, "name");
  spec[n].offset = offsetof(struct t1_row, name_len);
  spec[n].null_byte_offset = offsetof(struct t1_row, null_bits);
  spec[n].null_bit = 1;
  n++;
  EX_CHECK(ic_record_create(table, spec, n, sizeof(struct t1_row), &rec));
  return rec;
}

/* A record over the unique key alone, code in a struct t1_row. */
static inline IC_RECORD *ex_code_record(const IC_TABLE_DEF *table)
{
  IC_FIELD_SPEC spec;
  IC_RECORD *rec;

  spec.field_id = ex_field(table, "code");
  spec.offset = offsetof(struct t1_row, code);
  spec.null_byte_offset = IC_NO_NULL_BIT;
  spec.null_bit = 0;
  EX_CHECK(ic_record_create(table, &spec, 1, sizeof(struct t1_row), &rec));
  return rec;
}

/* Fill a row; a NULL name leaves the name NULL. */
static inline void ex_fill(struct t1_row *row, uint32_t id, int32_t val,
                           uint32_t code, const char *name)
{
  size_t len = 0;

  memset(row, 0, sizeof *row);
  row->id = id;
  row->val = val;
  row->code = code;
  if (name == NULL) {
    row->null_bits |= EX_NAME_NULL;
  } else {
    len = strlen(name);
    if (len > sizeof row->name)
      len = sizeof row->name;
    memcpy(row->name, name, len);
    row->name_len = (uint8_t)len;
  }
}

/* Print a row. */
static inline void ex_print(const char *what, const struct t1_row *row)
{
  printf("%s: id %u, code %u, val ", what, (unsigned)row->id,
         (unsigned)row->code);
  if (row->null_bits & EX_VAL_NULL)
    printf("NULL");
  else
    printf("%d", (int)row->val);
  if (row->null_bits & EX_NAME_NULL)
    printf(", name NULL\n");
  else
    printf(", name '%.*s'\n", (int)row->name_len, row->name);
}

/* A record over one nullable field of struct t1_row, val or name: a
 * bound of a range of the index on val, or what an update writes. */
static inline IC_RECORD *ex_one_field_record(const IC_TABLE_DEF *table,
                                             const char *name)
{
  IC_FIELD_SPEC spec;
  IC_RECORD *rec;
  int is_val = strcmp(name, "val") == 0;

  spec.field_id = ex_field(table, name);
  spec.offset = is_val ? offsetof(struct t1_row, val)
                       : offsetof(struct t1_row, name_len);
  spec.null_byte_offset = offsetof(struct t1_row, null_bits);
  spec.null_bit = is_val ? 0 : 1;
  EX_CHECK(ic_record_create(table, &spec, 1, sizeof(struct t1_row), &rec));
  return rec;
}

/* Write the scan examples' rows, over whatever an earlier run left: id,
 * code the id, val the id negated, name "row" and the id's last digit. */
static inline void ex_write_scan_rows(IC_APID_CONNECTION *conn,
                                      const IC_RECORD *rec)
{
  struct t1_row row;
  char name[16];
  uint32_t id;

  for (id = EX_SCAN_FIRST; id < EX_SCAN_FIRST + EX_SCAN_ROWS; id++) {
    snprintf(name, sizeof name, "row %u", (unsigned)(id % 10));
    ex_fill(&row, id, -(int32_t)id, id, name);
    EX_CHECK(ic_apid_conn_write(conn, IC_WRITE_KIND_WRITE, rec,
                                (const uint8_t *)&row, sizeof row, rec,
                                (const uint8_t *)&row, sizeof row));
  }
}

/* A range of val from `from` to `to`, both included, numbered
 * range_no, its bounds put into low and high: rows of a record over val
 * alone (ex_one_field_record). */
static inline IC_SCAN_RANGE ex_val_range(int32_t from, int32_t to,
                                         uint32_t range_no,
                                         struct t1_row *low,
                                         struct t1_row *high)
{
  IC_SCAN_RANGE range;

  ic_scan_range_init(&range, sizeof range);
  range.low_columns = 1;
  range.low_inclusive = true;
  range.high_columns = 1;
  range.high_inclusive = true;
  range.range_no = range_no;
  memset(low, 0, sizeof *low);
  low->val = from;
  memset(high, 0, sizeof *high);
  high->val = to;
  return range;
}

/* Flush until the transaction is done, and stop if it did not commit. */
static inline void ex_finish(IC_APID_CONNECTION *conn, IC_TRANS_ID trans)
{
  while (!ic_apid_transaction_is_done(conn, trans))
    EX_CHECK(ic_apid_conn_flush(conn, 100, 1, NULL));
  if (ic_apid_transaction_state(conn, trans) != IC_COMMIT_STATE_COMMITTED)
    ex_fail("the transaction", ic_apid_transaction_error(conn, trans));
  EX_CHECK(ic_apid_conn_close_transaction(conn, trans));
}

#endif /* EX_T1_H */
