/* auto_increment: values of an AUTO_INCREMENT column asked for and put
 * in the row, from the counter MySQL servers take theirs from
 * (doc/user-guide.md, section 7).
 *
 * - Three rows of ic_ex.t2, each value asked for alone: the first
 *   refills the range the process holds for the table, 4096 values in
 *   one round trip, and the others take from it.
 * - Five rows whose ids are one block, reserved together.
 * - A row with an id of the application's own, past the counter, which
 *   it then says it used, so that the counter moves past it for every
 *   client.
 * - ic_ex.t3, a table without a primary key: its hidden key, $PK, is
 *   its AUTO_INCREMENT column and takes its values the same way. Its
 *   rows are read back by a scan, which returns the key, and deleted by
 *   it.
 *
 *   ./auto_increment [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

/* A row of ic_ex.t2 or ic_ex.t3 as counted_record lays it out: the
 * AUTO_INCREMENT column, BIGINT UNSIGNED (t2's id, t3's hidden key), and
 * val INT, nullable: null bit 0. */
struct counted_row {
  uint64_t key;
  int32_t  val;
  uint8_t  null_bits;
};

/* A record over a struct counted_row: the table's AUTO_INCREMENT
 * column, found by its IC_FIELD_INFO, and val. Says which column it is,
 * and whether it is the hidden key of a table without a primary key. */
static IC_RECORD *counted_record(const IC_TABLE_DEF *table)
{
  IC_FIELD_INFO info;
  IC_FIELD_SPEC spec[2];
  IC_RECORD *rec;
  uint32_t fields = ic_table_def_num_fields(table);
  uint32_t key = fields;
  uint32_t id;

  for (id = 0; id < fields; id++) {
    EX_CHECK(ic_table_def_field(table, id, &info, sizeof info));
    if (!info.auto_increment)
      continue;
    key = id;
    printf("%s: AUTO_INCREMENT column %s%s\n", ic_table_def_name(table),
           info.name,
           info.hidden_key ? ", the hidden key of a table without a "
                             "primary key"
                           : "");
  }
  if (key == fields)
    ex_fail("finding the AUTO_INCREMENT column", IC_ERROR_NO_AUTO_INCREMENT);
  spec[0].field_id = key;
  spec[0].offset = offsetof(struct counted_row, key);
  spec[0].null_byte_offset = IC_NO_NULL_BIT;
  spec[0].null_bit = 0;
  spec[1].field_id = ex_field(table, "val");
  spec[1].offset = offsetof(struct counted_row, val);
  spec[1].null_byte_offset = offsetof(struct counted_row, null_bits);
  spec[1].null_bit = 0;
  EX_CHECK(ic_record_create(table, spec, 2, sizeof(struct counted_row),
                            &rec));
  return rec;
}

/* Write a row of key and val, as kind says. */
static void write_counted(IC_APID_CONNECTION *conn, const IC_RECORD *rec,
                          uint32_t kind, uint64_t key, int32_t val)
{
  struct counted_row row;

  memset(&row, 0, sizeof row);
  row.key = key;
  row.val = val;
  EX_CHECK(ic_apid_conn_write(conn, kind, rec, (const uint8_t *)&row,
                              sizeof row, rec, (const uint8_t *)&row,
                              sizeof row));
}

/* Read row key into row: whether it is there. */
static bool read_counted(IC_APID_CONNECTION *conn, const IC_RECORD *rec,
                         uint64_t key, struct counted_row *row)
{
  struct counted_row key_row;
  bool found;

  memset(&key_row, 0, sizeof key_row);
  key_row.key = key;
  EX_CHECK(ic_apid_conn_read_committed(conn, rec, (const uint8_t *)&key_row,
                                       sizeof key_row, rec, (uint8_t *)row,
                                       sizeof *row, &found));
  return found;
}

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_auto_increment", &conn);
  IC_TABLE_DEF *t2, *t3;
  IC_RECORD *rec2, *rec3;
  IC_SCAN_ID scan;
  IC_SCAN_ARGS args;
  struct counted_row row;
  uint64_t before, after, id, first, own, ids[3], keys[2];
  size_t len;
  bool more;
  uint32_t i, ours = 0;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, "t2", &t2));
  rec2 = counted_record(t2);

  /* Each value asked for alone, and put in the row. */
  EX_CHECK(ic_apid_conn_auto_increment_peek(conn, t2, &before));
  for (i = 0; i < 3; i++) {
    EX_CHECK(ic_apid_conn_auto_increment(conn, t2, &ids[i]));
    write_counted(conn, rec2, IC_WRITE_KIND_INSERT, ids[i], (int32_t)i);
    printf("inserted id %llu\n", (unsigned long long)ids[i]);
  }
  EX_CHECK(ic_apid_conn_auto_increment_peek(conn, t2, &after));
  printf("the counter went from %llu to %llu: %llu values reserved by one "
         "refill\n",
         (unsigned long long)before, (unsigned long long)after,
         (unsigned long long)(after - before));
  ex_check(ids[0] >= before && ids[1] == ids[0] + 1 && ids[2] == ids[1] + 1,
           "the three ids follow on from the counter");
  ex_check(after >= before + 4096, "a refill reserves 4096 values");

  /* A block of five, for an insert of many rows. */
  EX_CHECK(ic_apid_conn_auto_increment_block(conn, t2, 5, &first));
  for (i = 0; i < 5; i++)
    write_counted(conn, rec2, IC_WRITE_KIND_INSERT, first + i, 10);
  printf("a block of five: ids %llu to %llu\n", (unsigned long long)first,
         (unsigned long long)(first + 4));
  ex_check(first == ids[2] + 1, "the block follows on from the range");

  /* An id of the application's own, past the counter. Said, it moves the
   * counter past it, and what was left of this process's range is a
   * gap. */
  EX_CHECK(ic_apid_conn_auto_increment_peek(conn, t2, &own));
  own += 1000;
  write_counted(conn, rec2, IC_WRITE_KIND_INSERT, own, 99);
  EX_CHECK(ic_apid_conn_auto_increment_seen(conn, t2, own));
  EX_CHECK(ic_apid_conn_auto_increment(conn, t2, &id));
  write_counted(conn, rec2, IC_WRITE_KIND_INSERT, id, 100);
  printf("an id of our own, %llu; the next value, %llu, is past it\n",
         (unsigned long long)own, (unsigned long long)id);
  ex_check(id > own, "the next value is past the id of our own");
  ex_check(read_counted(conn, rec2, own, &row) && row.val == 99,
           "the row of our own id is there");

  /* A table without a primary key: the hidden key is the AUTO_INCREMENT
   * column, the last. */
  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, "t3", &t3));
  rec3 = counted_record(t3);
  for (i = 0; i < 2; i++) {
    EX_CHECK(ic_apid_conn_auto_increment(conn, t3, &keys[i]));
    write_counted(conn, rec3, IC_WRITE_KIND_INSERT, keys[i],
                  (int32_t)(i + 1));
  }
  printf("two rows of t3, $PK %llu and %llu\n", (unsigned long long)keys[0],
         (unsigned long long)keys[1]);
  ex_check(keys[1] == keys[0] + 1, "the hidden keys follow on");

  /* A scan returns the hidden key with each row, so a row it finds can
   * be read, updated or deleted by it. */
  EX_CHECK(ic_apid_scan_create(conn, t3, rec3, &scan));
  ic_scan_args_init(&args, sizeof args);
  EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
  for (;;) {
    EX_CHECK(ic_apid_conn_next_row(conn, scan, &more));
    if (!more)
      break;
    memcpy(&row, ic_apid_scan_row(conn, scan, &len), sizeof row);
    if (row.null_bits & 0x01) {
      printf("scanned: $PK %llu, val NULL\n", (unsigned long long)row.key);
      continue;
    }
    printf("scanned: $PK %llu, val %d\n", (unsigned long long)row.key,
           (int)row.val);
    if ((row.key == keys[0] && row.val == 1) ||
        (row.key == keys[1] && row.val == 2))
      ours++;
  }
  EX_CHECK(ic_apid_scan_free(conn, scan));
  ex_check(ours == 2, "the scan finds both rows, with their values");
  for (i = 0; i < 2; i++)
    write_counted(conn, rec3, IC_WRITE_KIND_DELETE, keys[i], 0);
  for (i = 0; i < 2; i++)
    ex_check(!read_counted(conn, rec3, keys[i], &row), "the rows are deleted");
  printf("both deleted by their hidden keys\n");

  ic_record_free(rec3);
  ic_record_free(rec2);
  ic_table_def_free(t3);
  ic_table_def_free(t2);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  printf("auto_increment: all checks passed\n");
  return 0;
}
