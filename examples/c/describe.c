/* describe: a table's fields as the data nodes describe them, and where
 * a record the library lays out puts each one in a row.
 *
 *   ./describe [connectstring] [database table]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

static const char *storage_name(uint32_t storage)
{
  switch (storage) {
  case 0:
    return "memory";
  case 1:
    return "disk";
  default:
    return "default";
  }
}

int main(int argc, char **argv)
{
  const char *db = argc > 3 ? argv[2] : EX_DB;
  const char *name = argc > 3 ? argv[3] : EX_TABLE;
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_describe", &conn);
  IC_TABLE_DEF *table;
  IC_RECORD *rec;
  uint32_t fields;
  uint32_t id;

  EX_CHECK(ic_apid_conn_table_bind(conn, db, name, &table));
  /* With no fields named, every field but the blob columns. */
  EX_CHECK(ic_record_create_default(table, NULL, 0, &rec));
  fields = ic_table_def_num_fields(table);
  printf("%s: %u fields, a row of %u bytes\n", ic_table_def_name(table),
         (unsigned)fields, (unsigned)ic_record_row_size(rec));
  printf("%-4s %-16s %-12s %7s %-8s %-7s %6s %4s\n", "id", "name", "type",
         "length", "null", "storage", "offset", "size");
  for (id = 0; id < fields; id++) {
    IC_FIELD_INFO info;
    uint32_t offset;
    uint32_t size;

    EX_CHECK(ic_table_def_field(table, id, &info, sizeof info));
    printf("%-4u %-16s %-12s %7u %-8s %-7s", (unsigned)id, info.name,
           ic_field_type_name(info.type_code), (unsigned)info.length,
           info.primary_key ? "key" : info.nullable ? "nullable" : "",
           storage_name(info.storage));
    if (ic_record_field(rec, id, &offset, &size) == 0)
      printf(" %6u %4u\n", (unsigned)offset, (unsigned)size);
    else
      printf(" %6s %4s\n", "-", "-");
  }
  ic_record_free(rec);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
