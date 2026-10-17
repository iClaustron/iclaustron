/* range_scan: rows found by ranges of the ordered index on val, in the
 * index's order. A range's bounds are rows of a record over the index's
 * columns, here val alone: from -250 up to but not -240, then the same
 * from the high end; then three ranges at once, each row coming with
 * the number of the range it is in.
 *
 *   ./range_scan [connectstring]
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#include "t1.h"

/* Read a run to its end, printing each row's val after what, with its
 * range's number when numbered. */
static void print_run(IC_APID_CONNECTION *conn, IC_SCAN_ID scan,
                      const char *what, int numbered)
{
  struct t1_row row;
  const uint8_t *rows;
  size_t len;
  bool more;

  printf("%s:", what);
  for (;;) {
    EX_CHECK(ic_apid_conn_next_row(conn, scan, &more));
    if (!more)
      break;
    rows = ic_apid_scan_row(conn, scan, &len);
    memcpy(&row, rows, sizeof row);
    if (numbered)
      printf(" %u/%d", (unsigned)ic_apid_scan_range_no(conn, scan),
             (int)row.val);
    else
      printf(" %d", (int)row.val);
  }
  printf("\n");
}

int main(int argc, char **argv)
{
  IC_APID_CONNECTION *conn;
  IC_APID_GLOBAL *global = ex_connect(argc, argv, "ex_range_scan", &conn);
  static const int32_t spans[3][2] = {
    {-299, -299}, {-210, -205}, {-201, -200}
  };
  IC_TABLE_DEF *table;
  IC_INDEX_DEF *index;
  IC_RECORD *rec;
  IC_RECORD *val_rec;
  IC_SCAN_ID scan;
  IC_SCAN_ARGS args;
  IC_SCAN_RANGE range;
  struct t1_row low;
  struct t1_row high;
  uint32_t i;

  EX_CHECK(ic_apid_conn_table_bind(conn, EX_DB, EX_TABLE, &table));
  /* A MySQL server names the ordered index of KEY t1_val t1_val. */
  EX_CHECK(ic_apid_conn_index_bind(conn, EX_DB, EX_VAL_INDEX, EX_TABLE,
                                   &index));
  rec = ex_t1_record(table, 0);
  val_rec = ex_one_field_record(table, "val");
  ex_write_scan_rows(conn, rec);
  EX_CHECK(ic_apid_index_scan_create(conn, index, val_rec, rec, &scan));

  /* val from -250, included, up to -240, not included; each run takes
   * the ranges added before it. */
  ic_scan_args_init(&args, sizeof args);
  args.ordered = true; /* merged from every fragment in the index's order */
  for (i = 0; i < 2; i++) {
    range = ex_val_range(-250, -240, 0, &low, &high);
    range.high_inclusive = false;
    EX_CHECK(ic_apid_conn_add_scan_range(conn, scan, &range,
                                         (const uint8_t *)&low, sizeof low,
                                         (const uint8_t *)&high,
                                         sizeof high));
    args.descending = i == 1;
    EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
    print_run(conn, scan,
              i == 0 ? "from -250 to -240" : "the same from the high end",
              0);
  }

  /* Three ranges, numbered in the order added: val = -299; val from
   * -210 to -205; val from -201 to -200. The rows come range by range. */
  for (i = 0; i < 3; i++) {
    range = ex_val_range(spans[i][0], spans[i][1], i, &low, &high);
    EX_CHECK(ic_apid_conn_add_scan_range(conn, scan, &range,
                                         (const uint8_t *)&low, sizeof low,
                                         (const uint8_t *)&high,
                                         sizeof high));
  }
  args.descending = false;
  EX_CHECK(ic_apid_conn_start_scan(conn, scan, &args));
  print_run(conn, scan, "range/val", 1);

  EX_CHECK(ic_apid_scan_free(conn, scan));
  ic_record_free(val_rec);
  ic_record_free(rec);
  ic_index_def_free(index);
  ic_table_def_free(table);
  ic_apid_conn_free(conn);
  ic_apid_global_free(global);
  return 0;
}
