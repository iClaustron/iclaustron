// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! describe: a table's fields as the data nodes describe them, and where
//! a record the library lays out puts each one in a row.
//!
//! ```text
//! cargo run -p ic_examples --example describe -- \
//!   [connectstring] [db table]
//! ```

use ic_apid::record::Record;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::EX_DB;
use ic_examples::EX_TABLE;
use ic_port::IcError;
use ic_signals::table_props;

fn storage_name(storage: u32) -> &'static str {
  match storage {
    table_props::IC_STORAGE_MEMORY => "memory",
    table_props::IC_STORAGE_DISK => "disk",
    _ => "default",
  }
}

fn run(args: &[String]) -> Result<(), IcError> {
  let (db, name) = if args.len() > 3 {
    (args[2].as_str(), args[3].as_str())
  } else {
    (EX_DB, EX_TABLE)
  };
  let (_global, mut conn) = connect(args, "ex_describe")?;
  let table = conn.table_bind(db, name)?;
  // With no fields named, every field but the blob columns.
  let rec = Record::default_for(&table, None)?;
  println!(
    "{}: {} fields, a row of {} bytes",
    table.name(),
    table.num_fields(),
    rec.row_size()
  );
  println!(
    "{:<4} {:<16} {:<12} {:>7} {:<8} {:<7} {:>6} {:>4}",
    "id", "name", "type", "length", "null", "storage", "offset", "size"
  );
  let mut id: u32 = 0;
  while id < table.num_fields() {
    if let Some(attr) = table.field(id) {
      let null = if attr.primary_key {
        "key"
      } else if attr.nullable {
        "nullable"
      } else {
        ""
      };
      let field = match rec.position_of(id) {
        Some(position) => rec.field(position),
        None => None,
      };
      let place = match field {
        Some(field) => format!("{:>6} {:>4}", field.offset(), field.size()),
        None => format!("{:>6} {:>4}", "-", "-"),
      };
      println!(
        "{:<4} {:<16} {:<12} {:>7} {:<8} {:<7} {}",
        id,
        attr.name,
        table_props::type_name(attr.type_code),
        attr.length,
        null,
        storage_name(attr.storage_kind),
        place
      );
    }
    id += 1;
  }
  Ok(())
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
