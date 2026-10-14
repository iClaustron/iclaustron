/* What the C examples share: the row of ic_ex.t1 as a C struct, the
 * record over it, connecting, and stopping with the error's text.
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
