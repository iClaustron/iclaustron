// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! unique: a row read and updated through the unique index on code, the
//! row found by the index rather than its primary key.
//!
//! ```text
//! cargo run -p ic_examples --example unique -- [connectstring]
//! ```

use ic_apid::key_op;
use ic_apid::query::ReadKeyArgs;
use ic_apid::query::ReadKind;
use ic_apid::query::WriteKeyArgs;
use ic_apid::query::WriteKind;
use ic_examples::committed;
use ic_examples::connect;
use ic_examples::exit_on;
use ic_examples::put_u32;
use ic_examples::Values;
use ic_examples::EX_DB;
use ic_examples::EX_INDEX;
use ic_examples::EX_TABLE;
use ic_examples::T1;
use ic_port::err;
use ic_port::IcError;
use ic_signals::key_ops;

fn run(args: &[String]) -> Result<(), IcError> {
  let (_global, mut conn) = connect(args, "ex_unique")?;
  let t1 = T1::bind(&mut conn)?;
  // A MySQL server names the hash index of UNIQUE KEY t1_code
  // t1_code$unique.
  let index = conn.index_bind(EX_DB, EX_INDEX, EX_TABLE)?;
  let code_rec = t1.record_over(&[t1.code])?;
  let values_rec = t1.record_over(&[t1.val, t1.name])?;
  let no_query = IcError::new(err::IC_ERROR_NO_SUCH_FIELD);

  // The row to find, written by primary key.
  let mut row = t1.row();
  let values = Values {
    id: 7,
    val: Some(70),
    code: 700,
    name: Some("seven"),
  };
  t1.fill(&t1.rec, &mut row, &values)?;
  key_op::write_key(
    &mut conn,
    key_ops::IC_OP_WRITE,
    &t1.rec,
    &row,
    &t1.rec,
    &row,
  )?;

  // A committed read through the index: the key row holds code. The
  // transaction starts where the index row is.
  let reader = conn.create_unique_query(&index, &code_rec, &t1.rec)?;
  match conn.query_mut(reader) {
    Some(query) => put_u32(&code_rec, query.key_row_mut(), t1.code, 700)?,
    None => return Err(no_query),
  }
  let hint = match conn.query(reader) {
    Some(query) => conn.hint_for_unique_key(&index, &code_rec, query.key_row()),
    None => return Err(no_query),
  };
  let trans = conn.start_transaction(hint, Some(index.as_table()))?;
  let read = ReadKeyArgs {
    kind: ReadKind::Committed,
    ..ReadKeyArgs::default()
  };
  conn.read_key(reader, trans, &read)?;
  conn.commit_transaction(trans)?;
  committed(&mut conn, trans, reader)?;
  match conn.query(reader) {
    Some(query) => {
      println!("read by code 700: {}", t1.show(query.attr_row()));
    }
    None => return Err(no_query),
  }

  // An update through the index. Its values leave out the indexed
  // column: the row is found by it, and an update through the index
  // cannot change it.
  let updater = conn.create_unique_query(&index, &code_rec, &values_rec)?;
  let new_values = Values {
    id: 0,
    val: Some(71),
    code: 0,
    name: Some("seventy-one"),
  };
  match conn.query_mut(updater) {
    Some(query) => {
      put_u32(&code_rec, query.key_row_mut(), t1.code, 700)?;
      t1.fill(&values_rec, query.attr_row_mut(), &new_values)?;
    }
    None => return Err(no_query),
  }
  let trans = conn.start_transaction(hint, Some(index.as_table()))?;
  let update = WriteKeyArgs {
    kind: WriteKind::Update,
    ..WriteKeyArgs::default()
  };
  conn.write_key(updater, trans, &update)?;
  conn.commit_transaction(trans)?;
  committed(&mut conn, trans, updater)?;

  // Read back through the index once more, synchronously.
  let mut key = vec![0u8; code_rec.row_size() as usize];
  put_u32(&code_rec, &mut key, t1.code, 700)?;
  let mut out = t1.row();
  if key_op::read_unique_committed(
    &mut conn, &index, &code_rec, &key, &t1.rec, &mut out,
  )? {
    println!("after the update: {}", t1.show(&out));
  } else {
    return Err(IcError::new(key_op::IC_NDB_ERROR_NO_SUCH_ROW as i32));
  }
  conn.free_query(reader)?;
  conn.free_query(updater)
}

fn main() {
  let args: Vec<String> = std::env::args().collect();
  exit_on(run(&args));
}
