// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Programs: conditions and assignments that run in the data node on
//! the row it holds, as a scan's filter or as a key operation's
//! condition and the updates it works out from the row
//! (doc/rust/04-api-design.md, "Conditions and assignments"). New: the
//! C declared `IC_WHERE_CONDITION` and `IC_CONDITIONAL_ASSIGNMENT` and
//! built neither.
//!
//! **Built bottom-up, compiled whole.** A [`ProgramBuilder`] over a
//! table hands out a [`Value`] for each column, constant, integer, input
//! and sum, and a [`Cond`] for each comparison and NULL test and for each
//! AND, OR and NOT joining them. [`ProgramBuilder::finish`] compiles the
//! tree under the top condition, with the assignments, into a
//! [`Program`], made once and run by any number of operations. What
//! cannot be compiled is kept as the builder's error until then, so a
//! condition is built without a check at every step.
//!
//! **How a condition is compiled.** Each NOT is taken down to the terms
//! by De Morgan's laws, a comparison turned into its opposite and IS
//! NULL into IS NOT NULL. SQL's logic allows it: the opposite of a
//! comparison with a NULL is unknown too. What is left is ANDs and ORs of
//! terms, and a row passes only when the whole is true, so each term
//! need only be told true from not true, unknown going with false.
//! Nested ANDs and ORs are flattened, and each term becomes a jump: in an
//! AND to the refusal when it is not true, in an OR to the acceptance
//! when it is, as MySQL's programs have them (chapter 05 §9.6).
//!
//! - A column compared with a constant is a column test, and so is a
//!   column compared with a column, both of one type (chapter 05 §9.3).
//!   A NULL makes the test jump to the refusal, or go on, by its rule.
//! - Inputs, integers and sums are worked out in registers, a column
//!   read into one where it meets them. Only integer columns go into
//!   registers. A register that may be NULL is tested first, since a
//!   comparison with a NULL register fails the program (878).
//! - IS NULL and IS NOT NULL test a column alone, or a register; of a
//!   value that cannot be NULL the answer is known before the row is.
//!
//! **Assignments** come after the condition. A constant from a row is
//! written after the program, as a write sends it, whatever its type. A
//! value worked out in registers, or a constant assigned only when a
//! condition of its own holds, is written from a register, into an
//! integer column of 32 or 64 bits; the data node cuts a value to the
//! column's size.
//!
//! **Registers** are taken for a term or an assignment while it is
//! worked out, and given back after it: more than eight at once is
//! `IC_ERROR_CONDITION_TOO_COMPLEX`.
//!
//! **The refusal's code** is set as the program goes out with an
//! operation: 626 where a missed condition is not to abort, 6000 where
//! it is (chapter 05 §9.4).

use std::sync::Arc;

use ic_port::err;
use ic_port::IcError;
use ic_signals::column_header;
use ic_signals::interp;
use ic_signals::interp::ColumnCmp;
use ic_signals::interp::Label;
use ic_signals::interp::NullRule;
use ic_signals::interp::ProgramWriter;
use ic_signals::interp::RegCmp;
use ic_signals::table_props;
use ic_signals::table_props::ColumnProps;

use crate::dict_cache::TableDef;
use crate::record::Record;
use crate::row_codec;

/// The inputs an operation gives its program, each a signed integer of
/// 64 bits.
pub const IC_PROGRAM_INPUTS: u32 = interp::IC_INTERP_INPUTS;

/// The longest program, in words: the data node takes a column section
/// of fewer than 32 768 words in all.
const IC_MAX_PROGRAM_WORDS: usize = 32_000;
/// The most terms a condition may come to once flattened.
const IC_MAX_TERMS: usize = 16_384;
/// How deep values may nest in values, and ANDs in ORs.
const IC_MAX_DEPTH: u32 = 64;

/// How a comparison compares its first value with its second.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cmp {
  /// Equal.
  Eq,
  /// Not equal.
  Ne,
  /// Less.
  Lt,
  /// Less or equal.
  Le,
  /// Greater.
  Gt,
  /// Greater or equal.
  Ge,
}

impl Cmp {
  /// The comparison that is true exactly when this one is false; with
  /// a NULL both are unknown.
  fn negated(self) -> Cmp {
    match self {
      Cmp::Eq => Cmp::Ne,
      Cmp::Ne => Cmp::Eq,
      Cmp::Lt => Cmp::Ge,
      Cmp::Le => Cmp::Gt,
      Cmp::Gt => Cmp::Le,
      Cmp::Ge => Cmp::Lt,
    }
  }

  /// The comparison with its values the other way round: a < b is
  /// b > a.
  fn swapped(self) -> Cmp {
    match self {
      Cmp::Eq => Cmp::Eq,
      Cmp::Ne => Cmp::Ne,
      Cmp::Lt => Cmp::Gt,
      Cmp::Le => Cmp::Ge,
      Cmp::Gt => Cmp::Lt,
      Cmp::Ge => Cmp::Le,
    }
  }

  fn column_cmp(self) -> ColumnCmp {
    match self {
      Cmp::Eq => ColumnCmp::Eq,
      Cmp::Ne => ColumnCmp::Ne,
      Cmp::Lt => ColumnCmp::Lt,
      Cmp::Le => ColumnCmp::Le,
      Cmp::Gt => ColumnCmp::Gt,
      Cmp::Ge => ColumnCmp::Ge,
    }
  }

  fn reg_cmp(self) -> RegCmp {
    match self {
      Cmp::Eq => RegCmp::Eq,
      Cmp::Ne => RegCmp::Ne,
      Cmp::Lt => RegCmp::Lt,
      Cmp::Le => RegCmp::Le,
      Cmp::Gt => RegCmp::Gt,
      Cmp::Ge => RegCmp::Ge,
    }
  }
}

/// A value of a [`ProgramBuilder`], good only in the builder that made
/// it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Value(u32);

impl Value {
  /// The id as a word, for a language binding.
  pub fn as_u32(&self) -> u32 {
    self.0
  }

  /// A value from its word, as a binding hands it back; one its builder
  /// did not make fails the builder's finish.
  pub fn from_u32(word: u32) -> Value {
    Value(word)
  }
}

/// A condition of a [`ProgramBuilder`], good only in the builder that
/// made it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cond(u32);

impl Cond {
  /// The id as a word, for a language binding.
  pub fn as_u32(&self) -> u32 {
    self.0
  }

  /// A condition from its word, as a binding hands it back; one its
  /// builder did not make fails the builder's finish.
  pub fn from_u32(word: u32) -> Cond {
    Cond(word)
  }
}

#[derive(Debug)]
enum ValueNode {
  /// The row's value of a column.
  Column(u32),
  /// A constant: the column whose type it has, and its bytes as that
  /// column's are sent; none for NULL.
  Constant {
    column: u32,
    bytes: Option<Vec<u8>>,
  },
  Integer(i64),
  Input(u32),
  Add(Value, Value),
  Subtract(Value, Value),
}

#[derive(Debug)]
enum CondNode {
  Compare(Cmp, Value, Value),
  IsNull(Value),
  IsNotNull(Value),
  And(Cond, Cond),
  Or(Cond, Cond),
  Not(Cond),
}

#[derive(Debug)]
struct Assignment {
  column: u32,
  value: Value,
  /// Assigned only when this is true.
  condition: Option<Cond>,
}

/// A condition and assignments being built over a table, compiled by
/// [`finish`](Self::finish).
///
/// ```no_run
/// # use std::sync::Arc;
/// # use ic_apid::dict_cache::TableDef;
/// # use ic_apid::program::Cmp;
/// # use ic_apid::program::ProgramBuilder;
/// # use ic_apid::record::Record;
/// # fn f(table: &Arc<TableDef>, rec: &Record, row: &[u8], val: u32)
/// #   -> Result<(), ic_port::IcError> {
/// // val > (val in row) AND val < input 0
/// let mut b = ProgramBuilder::new(table);
/// let column = b.column(val);
/// let low = b.constant(rec, row, val);
/// let high = b.input(0);
/// let above = b.compare(Cmp::Gt, column, low);
/// let below = b.compare(Cmp::Lt, column, high);
/// let both = b.and(above, below);
/// let _program = b.finish(Some(both))?;
/// # Ok(())
/// # }
/// ```
#[derive(Debug)]
pub struct ProgramBuilder {
  table: Arc<TableDef>,
  values: Vec<ValueNode>,
  conds: Vec<CondNode>,
  assignments: Vec<Assignment>,
  error: Option<IcError>,
}

impl ProgramBuilder {
  /// An empty builder over `table`.
  pub fn new(table: &Arc<TableDef>) -> ProgramBuilder {
    ProgramBuilder {
      table: Arc::clone(table),
      values: Vec::new(),
      conds: Vec::new(),
      assignments: Vec::new(),
      error: None,
    }
  }

  /// The table the program is for.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// The row's value of column `id`.
  pub fn column(&mut self, id: u32) -> Value {
    if self.table.field(id).is_none() {
      self.fail(err::IC_ERROR_NO_SUCH_FIELD);
    }
    self.push_value(ValueNode::Column(id))
  }

  /// The value of column `id` in `row`, a row laid out by `rec`, a
  /// record over the builder's table. It is copied now, and has that
  /// column's type; a NULL there is a NULL constant.
  pub fn constant(&mut self, rec: &Record, row: &[u8], id: u32) -> Value {
    let bytes = match self.constant_bytes(rec, row, id) {
      Ok(bytes) => bytes,
      Err(e) => {
        self.fail(e.code);
        None
      }
    };
    self.push_value(ValueNode::Constant { column: id, bytes })
  }

  /// An integer, as a sum adds it or a register holds it.
  pub fn integer(&mut self, value: i64) -> Value {
    self.push_value(ValueNode::Integer(value))
  }

  /// Input `input`, 0 to 15, which each operation sets: a signed
  /// integer.
  pub fn input(&mut self, input: u32) -> Value {
    if input >= IC_PROGRAM_INPUTS {
      self.fail(err::IC_ERROR_BAD_PROGRAM);
    }
    self.push_value(ValueNode::Input(input))
  }

  /// `a + b`, NULL if either is.
  pub fn add(&mut self, a: Value, b: Value) -> Value {
    self.check_value(a);
    self.check_value(b);
    self.push_value(ValueNode::Add(a, b))
  }

  /// `a − b`, NULL if either is.
  pub fn subtract(&mut self, a: Value, b: Value) -> Value {
    self.check_value(a);
    self.check_value(b);
    self.push_value(ValueNode::Subtract(a, b))
  }

  /// `a` compared with `b`; unknown if either is NULL.
  pub fn compare(&mut self, cmp: Cmp, a: Value, b: Value) -> Cond {
    self.check_value(a);
    self.check_value(b);
    self.push_cond(CondNode::Compare(cmp, a, b))
  }

  /// True if `a` is NULL.
  pub fn is_null(&mut self, a: Value) -> Cond {
    self.check_value(a);
    self.push_cond(CondNode::IsNull(a))
  }

  /// True if `a` is not NULL.
  pub fn is_not_null(&mut self, a: Value) -> Cond {
    self.check_value(a);
    self.push_cond(CondNode::IsNotNull(a))
  }

  /// `x AND y`.
  pub fn and(&mut self, x: Cond, y: Cond) -> Cond {
    self.check_cond(x);
    self.check_cond(y);
    self.push_cond(CondNode::And(x, y))
  }

  /// `x OR y`.
  pub fn or(&mut self, x: Cond, y: Cond) -> Cond {
    self.check_cond(x);
    self.check_cond(y);
    self.push_cond(CondNode::Or(x, y))
  }

  /// `NOT x`.
  pub fn not(&mut self, x: Cond) -> Cond {
    self.check_cond(x);
    self.push_cond(CondNode::Not(x))
  }

  /// Column `column` = `value`, once the condition has passed. A
  /// column is assigned once.
  pub fn assign(&mut self, column: u32, value: Value) {
    self.push_assignment(column, value, None);
  }

  /// Column `column` = `value` when `condition` is true, once the
  /// program's condition has passed.
  pub fn assign_if(&mut self, column: u32, value: Value, condition: Cond) {
    self.check_cond(condition);
    self.push_assignment(column, value, Some(condition));
  }

  /// Compile the condition under `top`, none for a program that only
  /// assigns, with the assignments. The first error met in building
  /// comes back here.
  pub fn finish(&self, top: Option<Cond>) -> Result<Arc<Program>, IcError> {
    if let Some(e) = self.error {
      return Err(e);
    }
    let mut c = Compiler {
      b: self,
      w: ProgramWriter::new(),
      refusals: Vec::new(),
      updates: Vec::new(),
      inputs: 0,
      terms: 0,
    };
    match top {
      Some(top) => {
        if top.0 as usize >= self.conds.len() {
          return Err(IcError::new(err::IC_ERROR_BAD_PROGRAM));
        }
        let tree = c.normalize(top, false, 0)?;
        if let Norm::Any(_) = tree {
          // An OR jumps to the acceptance; falling through refuses.
          let accept = c.w.new_label();
          c.cond(&tree, accept, true)?;
          c.refuse();
          c.w.place(accept);
          c.assignments()?;
          c.w.accept();
        } else {
          let refuse = c.w.new_label();
          c.cond(&tree, refuse, false)?;
          c.assignments()?;
          c.w.accept();
          c.w.place(refuse);
          c.refuse();
        }
      }
      None => {
        c.assignments()?;
        c.w.accept();
      }
    }
    c.w.finish()?;
    if c.w.len() > IC_MAX_PROGRAM_WORDS {
      return Err(IcError::new(err::IC_ERROR_CONDITION_TOO_COMPLEX));
    }
    Ok(Arc::new(Program {
      table: Arc::clone(&self.table),
      code: c.w.words().to_vec(),
      refusals: c.refusals,
      updates: c.updates,
      inputs: c.inputs,
      writes: !self.assignments.is_empty(),
    }))
  }

  fn constant_bytes(
    &self,
    rec: &Record,
    row: &[u8],
    id: u32,
  ) -> Result<Option<Vec<u8>>, IcError> {
    let table = rec.table();
    if table.table_id() != self.table.table_id()
      || table.table_version() != self.table.table_version()
    {
      return Err(IcError::new(err::IC_ERROR_RECORD_LAYOUT));
    }
    let position = match rec.position_of(id) {
      Some(position) => position,
      None => return Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    };
    match row_codec::field_value(rec, position, row)? {
      Some(bytes) => Ok(Some(bytes.to_vec())),
      None => Ok(None),
    }
  }

  fn push_assignment(
    &mut self,
    column: u32,
    value: Value,
    condition: Option<Cond>,
  ) {
    if self.table.field(column).is_none() {
      self.fail(err::IC_ERROR_NO_SUCH_FIELD);
    }
    self.check_value(value);
    self.assignments.push(Assignment {
      column,
      value,
      condition,
    });
  }

  /// A value or condition refers only to those made before it, so the
  /// tree has no loops.
  fn check_value(&mut self, v: Value) {
    if v.0 as usize >= self.values.len() {
      self.fail(err::IC_ERROR_BAD_PROGRAM);
    }
  }

  fn check_cond(&mut self, c: Cond) {
    if c.0 as usize >= self.conds.len() {
      self.fail(err::IC_ERROR_BAD_PROGRAM);
    }
  }

  fn push_value(&mut self, node: ValueNode) -> Value {
    self.values.push(node);
    Value(self.values.len() as u32 - 1)
  }

  fn push_cond(&mut self, node: CondNode) -> Cond {
    self.conds.push(node);
    Cond(self.conds.len() as u32 - 1)
  }

  /// Keep the first error.
  fn fail(&mut self, code: i32) {
    if self.error.is_none() {
      self.error = Some(IcError::new(code));
    }
  }
}

/// A compiled program, for one table: made once and shared, run by any
/// number of operations, each with its own inputs.
#[derive(Debug)]
pub struct Program {
  table: Arc<TableDef>,
  /// The instructions, refusing with 626.
  code: Vec<u32>,
  /// Where the `REFUSE` words are, for another code.
  refusals: Vec<u32>,
  /// The updates after the program, as a write sends them.
  updates: Vec<u32>,
  /// The inputs it reads, a bit each.
  inputs: u32,
  writes: bool,
}

impl Program {
  /// The table it is for.
  pub fn table(&self) -> &Arc<TableDef> {
    &self.table
  }

  /// Its instructions, refusing with 626.
  pub fn words(&self) -> &[u32] {
    &self.code
  }

  /// The updates after it: column headers and values, as a write sends
  /// them.
  pub fn updates(&self) -> &[u32] {
    &self.updates
  }

  /// True if it assigns columns, which only an update may.
  pub fn writes(&self) -> bool {
    self.writes
  }

  /// The inputs it reads, a bit each: an operation sends these.
  pub fn inputs(&self) -> u32 {
    self.inputs
  }

  /// Its instructions, appended to `out`, refusing with `refusal`.
  pub fn code_into(&self, refusal: u16, out: &mut Vec<u32>) {
    let start = out.len();
    out.extend_from_slice(&self.code);
    if refusal == interp::IC_REFUSE_NO_ROW {
      return;
    }
    for &at in &self.refusals {
      let word = &mut out[start + at as usize];
      *word = (*word & 0xFFFF) | ((refusal as u32) << 16);
    }
  }

  /// The inputs it reads, taken from `values`, appended to `out` as
  /// they go in front of the reads before it.
  pub fn inputs_into(
    &self,
    values: &[i64; IC_PROGRAM_INPUTS as usize],
    out: &mut Vec<u32>,
  ) {
    interp::inputs_into(self.inputs, values, out);
  }
}

/// An interpreted operation's column section, written a part at a time
/// into a buffer: the five sizes first, each filled in as its part
/// ends. The parts are the reads before the program, with the inputs in
/// front; the program; the updates after it; the reads after those; and
/// the subroutines, which ours have none of. Verify: chapter 05 §9.5.
pub(crate) struct Section<'b> {
  out: &'b mut Vec<u32>,
  part: usize,
  start: usize,
}

impl<'b> Section<'b> {
  /// A section in `out`, cleared first.
  pub(crate) fn new(out: &'b mut Vec<u32>) -> Section<'b> {
    out.clear();
    out.extend_from_slice(&[0; 5]);
    Section {
      out,
      part: 0,
      start: 5,
    }
  }

  /// The buffer, for the part being written.
  pub(crate) fn out(&mut self) -> &mut Vec<u32> {
    self.out
  }

  /// End the part being written; the next begins.
  pub(crate) fn end_part(&mut self) {
    if self.part < 5 {
      self.out[self.part] = (self.out.len() - self.start) as u32;
      self.part += 1;
      self.start = self.out.len();
    }
  }
}

/// A scan's column section with `program` as its filter, into `out`,
/// cleared first: the inputs it reads from `inputs` and `read`, what a
/// key read of the same record sends; the program, refusing with 626,
/// which skips the row; and nothing after. Verify: chapter 05 §7.2,
/// §9.5.
pub(crate) fn scan_section_into(
  program: &Program,
  inputs: &[i64; IC_PROGRAM_INPUTS as usize],
  read: &[u32],
  out: &mut Vec<u32>,
) {
  let mut section = Section::new(out);
  program.inputs_into(inputs, section.out());
  section.out().extend_from_slice(read);
  section.end_part();
  program.code_into(interp::IC_REFUSE_NO_ROW, section.out());
  section.end_part();
  section.end_part();
  section.end_part();
  section.end_part();
}

/// Refuse a program over another table than `table`, or one that
/// assigns columns where only a read or a delete runs it.
pub(crate) fn check_program(
  program: &Program,
  table: &TableDef,
  may_write: bool,
) -> Result<(), IcError> {
  let ours = program.table();
  if ours.table_id() != table.table_id()
    || ours.table_version() != table.table_version()
  {
    return Err(IcError::new(err::IC_ERROR_BAD_PROGRAM));
  }
  if program.writes() && !may_write {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  Ok(())
}

/// A condition with its NOTs taken down to the terms: a term, perhaps
/// negated, or an AND or OR of several.
enum Norm {
  Term(Cond, bool),
  All(Vec<Norm>),
  Any(Vec<Norm>),
}

struct Compiler<'a> {
  b: &'a ProgramBuilder,
  w: ProgramWriter,
  refusals: Vec<u32>,
  updates: Vec<u32>,
  inputs: u32,
  terms: usize,
}

impl<'a> Compiler<'a> {
  /// `c`, negated if `negated`, with its NOTs taken down to the terms
  /// and its ANDs and ORs flattened. An AND within an AND joins it, so
  /// does an OR within an OR; only where the two alternate is there a
  /// level more.
  fn normalize(
    &mut self,
    c: Cond,
    negated: bool,
    depth: u32,
  ) -> Result<Norm, IcError> {
    if depth > IC_MAX_DEPTH {
      return Err(IcError::new(err::IC_ERROR_CONDITION_TOO_COMPLEX));
    }
    let b = self.b;
    let (c, negated) = self.strip_not(c, negated);
    let all = match b.conds[c.0 as usize] {
      CondNode::And(_, _) => !negated,
      CondNode::Or(_, _) => negated,
      _ => {
        self.count_term()?;
        return Ok(Norm::Term(c, negated));
      }
    };
    let mut items: Vec<Norm> = Vec::new();
    let mut stack: Vec<(Cond, bool)> = vec![(c, negated)];
    while let Some((c, negated)) = stack.pop() {
      let (c, negated) = self.strip_not(c, negated);
      let (x, y, this_all) = match b.conds[c.0 as usize] {
        CondNode::And(x, y) => (x, y, !negated),
        CondNode::Or(x, y) => (x, y, negated),
        _ => {
          self.count_term()?;
          items.push(Norm::Term(c, negated));
          continue;
        }
      };
      if this_all == all {
        // x comes off first, keeping the order written.
        stack.push((y, negated));
        stack.push((x, negated));
      } else {
        items.push(self.normalize(c, negated, depth + 1)?);
      }
    }
    if all {
      Ok(Norm::All(items))
    } else {
      Ok(Norm::Any(items))
    }
  }

  fn strip_not(&self, mut c: Cond, mut negated: bool) -> (Cond, bool) {
    while let CondNode::Not(x) = self.b.conds[c.0 as usize] {
      c = x;
      negated = !negated;
    }
    (c, negated)
  }

  fn count_term(&mut self) -> Result<(), IcError> {
    self.terms += 1;
    if self.terms > IC_MAX_TERMS {
      return Err(IcError::new(err::IC_ERROR_CONDITION_TOO_COMPLEX));
    }
    Ok(())
  }

  /// Jump to `to` when `tree` is true (`when`), or when it is not;
  /// otherwise go on.
  fn cond(
    &mut self,
    tree: &Norm,
    to: Label,
    when: bool,
  ) -> Result<(), IcError> {
    match tree {
      Norm::Term(c, negated) => self.term(*c, *negated, to, when)?,
      Norm::All(items) => {
        if when {
          self.through(items, to, false)?;
        } else {
          for item in items {
            self.cond(item, to, false)?;
          }
        }
      }
      Norm::Any(items) => {
        if when {
          for item in items {
            self.cond(item, to, true)?;
          }
        } else {
          self.through(items, to, true)?;
        }
      }
    }
    if self.w.len() > IC_MAX_PROGRAM_WORDS {
      return Err(IcError::new(err::IC_ERROR_CONDITION_TOO_COMPLEX));
    }
    Ok(())
  }

  /// Jump to `to` when an AND of `items` is true (`skip_on` false), or
  /// when an OR of them is not (`skip_on` true): each item but the last
  /// skips past when it settles the whole the other way, and the last
  /// decides.
  fn through(
    &mut self,
    items: &[Norm],
    to: Label,
    skip_on: bool,
  ) -> Result<(), IcError> {
    let (last, rest) = match items.split_last() {
      Some(split) => split,
      None => return Ok(()),
    };
    let skip = self.w.new_label();
    for item in rest {
      self.cond(item, skip, skip_on)?;
    }
    self.cond(last, to, !skip_on)?;
    self.w.place(skip);
    Ok(())
  }

  fn term(
    &mut self,
    c: Cond,
    negated: bool,
    to: Label,
    when: bool,
  ) -> Result<(), IcError> {
    let b = self.b;
    match b.conds[c.0 as usize] {
      CondNode::Compare(cmp, a, b) => {
        let cmp = if negated { cmp.negated() } else { cmp };
        self.compare(cmp, a, b, to, when)
      }
      CondNode::IsNull(v) => self.null_test(v, !negated, to, when),
      CondNode::IsNotNull(v) => self.null_test(v, negated, to, when),
      _ => Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA)),
    }
  }

  /// A term whose truth is known before the row is: jump if it is
  /// what `when` asks.
  fn fixed(&mut self, is_true: bool, to: Label, when: bool) {
    if is_true == when {
      self.w.jump(to);
    }
  }

  fn compare(
    &mut self,
    cmp: Cmp,
    a: Value,
    b: Value,
    to: Label,
    when: bool,
  ) -> Result<(), IcError> {
    let builder = self.b;
    if self.is_null_constant(a) || self.is_null_constant(b) {
      // Unknown, as SQL has a comparison with NULL, either way round.
      self.fixed(false, to, when);
      return Ok(());
    }
    let left = &builder.values[a.0 as usize];
    let right = &builder.values[b.0 as usize];
    match (left, right) {
      (
        ValueNode::Column(column),
        ValueNode::Constant {
          column: of,
          bytes: Some(bytes),
        },
      ) => {
        self.same_type(*column, *of)?;
        // The value stands first in a column test.
        self.column_test(cmp.swapped(), *column, bytes, to, when);
        Ok(())
      }
      (
        ValueNode::Constant {
          column: of,
          bytes: Some(bytes),
        },
        ValueNode::Column(column),
      ) => {
        self.same_type(*column, *of)?;
        self.column_test(cmp, *column, bytes, to, when);
        Ok(())
      }
      (ValueNode::Column(column), ValueNode::Column(other)) => {
        self.same_type(*column, *other)?;
        // The other column stands first, as the value does.
        self.column_pair(cmp.swapped(), *column, *other, to, when);
        Ok(())
      }
      _ => self.register_compare(cmp, a, b, to, when),
    }
  }

  /// A column test, `cmp` with the value first.
  fn column_test(
    &mut self,
    cmp: Cmp,
    column: u32,
    bytes: &[u8],
    to: Label,
    when: bool,
  ) {
    if when {
      let c = cmp.column_cmp();
      self
        .w
        .test_column_value(c, NullRule::GoOn, column, bytes, to);
    } else {
      let c = cmp.negated().column_cmp();
      self
        .w
        .test_column_value(c, NullRule::Jump, column, bytes, to);
    }
  }

  /// A test of two columns, `cmp` with `other` first.
  fn column_pair(
    &mut self,
    cmp: Cmp,
    column: u32,
    other: u32,
    to: Label,
    when: bool,
  ) {
    if when {
      let c = cmp.column_cmp();
      self
        .w
        .test_column_column(c, NullRule::GoOn, column, other, to);
    } else {
      let c = cmp.negated().column_cmp();
      self
        .w
        .test_column_column(c, NullRule::Jump, column, other, to);
    }
  }

  /// `a` in register 0, `b` in register 1, compared once neither is
  /// NULL.
  fn register_compare(
    &mut self,
    cmp: Cmp,
    a: Value,
    b: Value,
    to: Label,
    when: bool,
  ) -> Result<(), IcError> {
    let a_null = self.eval(a, 0, 0)?;
    let b_null = self.eval(b, 1, 0)?;
    if when {
      let skip = self.w.new_label();
      if a_null {
        self.w.jump_if_null(0, skip);
      }
      if b_null {
        self.w.jump_if_null(1, skip);
      }
      self.w.jump_if(cmp.reg_cmp(), 0, 1, to);
      self.w.place(skip);
    } else {
      if a_null {
        self.w.jump_if_null(0, to);
      }
      if b_null {
        self.w.jump_if_null(1, to);
      }
      self.w.jump_if(cmp.negated().reg_cmp(), 0, 1, to);
    }
    Ok(())
  }

  /// `v IS NULL` if `is_null`, else `v IS NOT NULL`.
  fn null_test(
    &mut self,
    v: Value,
    is_null: bool,
    to: Label,
    when: bool,
  ) -> Result<(), IcError> {
    // Jump on a NULL when the term is then what `when` asks.
    let on_null = is_null == when;
    let b = self.b;
    if let ValueNode::Column(column) = b.values[v.0 as usize] {
      if self.attr(column)?.nullable {
        if on_null {
          self.w.jump_if_column_null(column, to);
        } else {
          self.w.jump_if_column_value(column, to);
        }
        return Ok(());
      }
    }
    if self.is_null_constant(v) {
      self.fixed(is_null, to, when);
      return Ok(());
    }
    if !self.may_be_null(v, 0)? {
      self.fixed(!is_null, to, when);
      return Ok(());
    }
    self.eval(v, 0, 0)?;
    if on_null {
      self.w.jump_if_null(0, to);
    } else {
      self.w.jump_if_value(0, to);
    }
    Ok(())
  }

  fn is_null_constant(&self, v: Value) -> bool {
    matches!(
      self.b.values[v.0 as usize],
      ValueNode::Constant { bytes: None, .. }
    )
  }

  fn may_be_null(&self, v: Value, depth: u32) -> Result<bool, IcError> {
    if depth > IC_MAX_DEPTH {
      return Err(IcError::new(err::IC_ERROR_CONDITION_TOO_COMPLEX));
    }
    match &self.b.values[v.0 as usize] {
      ValueNode::Column(column) => Ok(self.attr(*column)?.nullable),
      ValueNode::Constant { bytes, .. } => Ok(bytes.is_none()),
      ValueNode::Integer(_) | ValueNode::Input(_) => Ok(false),
      ValueNode::Add(x, y) | ValueNode::Subtract(x, y) => Ok(
        self.may_be_null(*x, depth + 1)? || self.may_be_null(*y, depth + 1)?,
      ),
    }
  }

  /// Work `v` out into register `reg`, using those above it as it needs;
  /// true if it may come out NULL.
  fn eval(&mut self, v: Value, reg: u32, depth: u32) -> Result<bool, IcError> {
    if reg >= interp::IC_INTERP_REGISTERS || depth > IC_MAX_DEPTH {
      return Err(IcError::new(err::IC_ERROR_CONDITION_TOO_COMPLEX));
    }
    let b = self.b;
    match &b.values[v.0 as usize] {
      ValueNode::Column(column) => {
        let attr = self.attr(*column)?;
        if !is_integer(attr) {
          return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
        }
        self.w.read_column(reg, *column);
        Ok(attr.nullable)
      }
      ValueNode::Constant { column, bytes } => match bytes {
        Some(bytes) => {
          let value = integer_of(self.attr(*column)?, bytes)?;
          self.set_integer(reg, value);
          Ok(false)
        }
        None => {
          self.w.set_null(reg);
          Ok(true)
        }
      },
      ValueNode::Integer(value) => {
        self.set_integer(reg, *value);
        Ok(false)
      }
      ValueNode::Input(input) => {
        self.w.read_input(reg, *input);
        self.inputs |= 1 << *input;
        Ok(false)
      }
      ValueNode::Add(x, y) => self.arith(true, *x, *y, reg, depth),
      ValueNode::Subtract(x, y) => self.arith(false, *x, *y, reg, depth),
    }
  }

  /// `x + y` or `x − y` into `reg`; an integer that fits the field is
  /// added or subtracted as it stands.
  fn arith(
    &mut self,
    add: bool,
    x: Value,
    y: Value,
    reg: u32,
    depth: u32,
  ) -> Result<bool, IcError> {
    if let Some(k) = self.immediate(y) {
      let null = self.eval(x, reg, depth + 1)?;
      self.add_immediate(add == (k >= 0), k.unsigned_abs() as u16, reg);
      return Ok(null);
    }
    if add {
      if let Some(k) = self.immediate(x) {
        let null = self.eval(y, reg, depth + 1)?;
        self.add_immediate(k >= 0, k.unsigned_abs() as u16, reg);
        return Ok(null);
      }
    }
    let x_null = self.eval(x, reg, depth + 1)?;
    let y_null = self.eval(y, reg + 1, depth + 1)?;
    if add {
      self.w.add(reg, reg, reg + 1);
    } else {
      self.w.subtract(reg, reg, reg + 1);
    }
    Ok(x_null || y_null)
  }

  /// An integer of at most 16 bits, either sign.
  fn immediate(&self, v: Value) -> Option<i64> {
    match self.b.values[v.0 as usize] {
      ValueNode::Integer(k) if k.unsigned_abs() <= 0xFFFF => Some(k),
      _ => None,
    }
  }

  fn add_immediate(&mut self, plus: bool, k: u16, reg: u32) {
    if plus {
      self.w.add_imm(reg, reg, k);
    } else {
      self.w.subtract_imm(reg, reg, k);
    }
  }

  /// `value` into `reg`, in the fewest words.
  fn set_integer(&mut self, reg: u32, value: i64) {
    if (0..=0xFFFF).contains(&value) {
      self.w.set_16(reg, value as u16);
    } else if (0..=u32::MAX as i64).contains(&value) {
      self.w.set_32(reg, value as u32);
    } else {
      self.w.set_64(reg, value as u64);
    }
  }

  fn assignments(&mut self) -> Result<(), IcError> {
    let b = self.b;
    let mut assigned: Vec<u32> = Vec::new();
    for a in &b.assignments {
      if assigned.contains(&a.column) {
        return Err(IcError::new(err::IC_ERROR_FIELD_ALREADY_DEFINED));
      }
      assigned.push(a.column);
      let target = self.attr(a.column)?;
      let value = &b.values[a.value.0 as usize];
      if let (ValueNode::Constant { column, bytes }, None) =
        (value, a.condition)
      {
        // Written after the program, as a write sends it.
        self.same_type(a.column, *column)?;
        match bytes {
          Some(bytes) => {
            let len = bytes.len() as u32;
            self
              .updates
              .push(column_header::column_header(a.column, len));
            row_codec::push_padded(&mut self.updates, bytes);
          }
          None => {
            self.updates.push(column_header::column_header(a.column, 0));
          }
        }
        continue;
      }
      if !is_wide_integer(target) {
        return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
      }
      let skip = self.w.new_label();
      if let Some(condition) = a.condition {
        let tree = self.normalize(condition, false, 0)?;
        self.cond(&tree, skip, false)?;
      }
      self.eval(a.value, 0, 0)?;
      self.w.write_column(a.column, 0);
      self.w.place(skip);
    }
    Ok(())
  }

  /// A `REFUSE`, its place kept for another code.
  fn refuse(&mut self) {
    self.refusals.push(self.w.len() as u32);
    self.w.refuse(interp::IC_REFUSE_NO_ROW);
  }

  fn attr(&self, column: u32) -> Result<&'a ColumnProps, IcError> {
    let b: &'a ProgramBuilder = self.b;
    match b.table.field(column) {
      Some(attr) => Ok(attr),
      None => Err(IcError::new(err::IC_ERROR_NO_SUCH_FIELD)),
    }
  }

  /// Columns compared, or a column and a constant, must be of one
  /// type, length, precision, scale and collation: the data node reads
  /// the one as though it were the other (chapter 05 §9.3).
  fn same_type(&self, x: u32, y: u32) -> Result<(), IcError> {
    let a = self.attr(x)?;
    let b = self.attr(y)?;
    if a.type_code != b.type_code
      || a.length != b.length
      || a.precision != b.precision
      || a.scale != b.scale
      || a.charset != b.charset
      || a.array_kind != b.array_kind
    {
      return Err(IcError::new(err::IC_ERROR_NOT_COMPARABLE));
    }
    Ok(())
  }
}

fn is_integer(attr: &ColumnProps) -> bool {
  (table_props::IC_TYPE_TINYINT..=table_props::IC_TYPE_BIGUNSIGNED)
    .contains(&attr.type_code)
}

/// The integer columns a register is written to: 32 and 64 bits.
fn is_wide_integer(attr: &ColumnProps) -> bool {
  matches!(
    attr.type_code,
    table_props::IC_TYPE_INT
      | table_props::IC_TYPE_UNSIGNED
      | table_props::IC_TYPE_BIGINT
      | table_props::IC_TYPE_BIGUNSIGNED
  )
}

/// The integer an integer column's bytes hold, little-endian as the
/// data node keeps them. An unsigned value above the largest signed one
/// has no place in a register's signed integer.
fn integer_of(attr: &ColumnProps, bytes: &[u8]) -> Result<i64, IcError> {
  let (size, signed) = match attr.type_code {
    table_props::IC_TYPE_TINYINT => (1, true),
    table_props::IC_TYPE_TINYUNSIGNED => (1, false),
    table_props::IC_TYPE_SMALLINT => (2, true),
    table_props::IC_TYPE_SMALLUNSIGNED => (2, false),
    table_props::IC_TYPE_MEDIUMINT => (3, true),
    table_props::IC_TYPE_MEDIUMUNSIGNED => (3, false),
    table_props::IC_TYPE_INT => (4, true),
    table_props::IC_TYPE_UNSIGNED => (4, false),
    table_props::IC_TYPE_BIGINT => (8, true),
    table_props::IC_TYPE_BIGUNSIGNED => (8, false),
    _ => return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED)),
  };
  if bytes.len() < size {
    return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
  }
  let mut le = [0u8; 8];
  le[..size].copy_from_slice(&bytes[..size]);
  let raw = u64::from_le_bytes(le);
  if signed {
    let shift = 64 - 8 * size as u32;
    return Ok(((raw << shift) as i64) >> shift);
  }
  if raw > i64::MAX as u64 {
    return Err(IcError::new(err::IC_ERROR_NOT_SUPPORTED));
  }
  Ok(raw as i64)
}

/// The table the tests of programs run on, for the tests of the
/// operations that carry them.
#[cfg(test)]
pub(crate) fn test_table() -> Arc<TableDef> {
  tests::table()
}

/// Run a program's code on the tests' model of the interpreter over a
/// row of integer columns by id, for the tests of programs other
/// modules build: whether it accepts, and the columns it writes.
#[cfg(test)]
pub(crate) fn run_model(
  words: &[u32],
  row: &[Option<i64>],
  inputs: &[i64; 16],
) -> Result<(bool, tests::Writes), String> {
  tests::run(words, row, inputs)
}

#[cfg(test)]
mod tests {
  use super::*;
  use ic_signals::prop_stream::PropertyWriter;
  use ic_signals::table_props::parse_table_info;
  use ic_signals::table_props::IC_ARRAY_KIND_FIXED;
  use ic_signals::table_props::IC_ARRAY_KIND_SHORT_VARSIZE;
  use ic_signals::table_props::IC_TP_COLUMN_ARRAY_KIND;
  use ic_signals::table_props::IC_TP_COLUMN_COUNT;
  use ic_signals::table_props::IC_TP_COLUMN_END;
  use ic_signals::table_props::IC_TP_COLUMN_ID;
  use ic_signals::table_props::IC_TP_COLUMN_IN_KEY;
  use ic_signals::table_props::IC_TP_COLUMN_LENGTH;
  use ic_signals::table_props::IC_TP_COLUMN_NAME;
  use ic_signals::table_props::IC_TP_COLUMN_NULLABLE;
  use ic_signals::table_props::IC_TP_COLUMN_PRECISION;
  use ic_signals::table_props::IC_TP_COLUMN_TYPE;
  use ic_signals::table_props::IC_TP_TABLE_NAME;
  use ic_signals::table_props::IC_TYPE_BIGUNSIGNED;
  use ic_signals::table_props::IC_TYPE_CHAR;
  use ic_signals::table_props::IC_TYPE_INT;
  use ic_signals::table_props::IC_TYPE_VARCHAR;

  // Columns as interp.pcap's interp_t has them (chapter 05 §9.6),
  // with two more: 0 id INT key, 1 val INT NULL, 2 name VARCHAR(32)
  // NULL, 3 code CHAR(4) NULL, 4 big BIGINT UNSIGNED NULL, 5 n INT,
  // 6 w INT NULL.
  const ID: u32 = 0;
  const VAL: u32 = 1;
  const NAME: u32 = 2;
  const CODE: u32 = 3;
  const BIG: u32 = 4;
  const N: u32 = 5;
  const W: u32 = 6;

  struct Column {
    name: &'static str,
    type_code: u32,
    length: u32,
    nullable: bool,
    // Sent for every variable-sized column: what makes a value carry
    // its length.
    array_kind: u32,
    charset: u32,
  }

  const fn column(
    name: &'static str,
    type_code: u32,
    length: u32,
    nullable: bool,
  ) -> Column {
    Column {
      name,
      type_code,
      length,
      nullable,
      array_kind: IC_ARRAY_KIND_FIXED,
      charset: 0,
    }
  }

  const COLUMNS: [Column; 7] = [
    column("id", IC_TYPE_INT, 1, false),
    column("val", IC_TYPE_INT, 1, true),
    Column {
      name: "name",
      type_code: IC_TYPE_VARCHAR,
      length: 32,
      nullable: true,
      array_kind: IC_ARRAY_KIND_SHORT_VARSIZE,
      charset: 8,
    },
    Column {
      charset: 8,
      ..column("code", IC_TYPE_CHAR, 4, true)
    },
    column("big", IC_TYPE_BIGUNSIGNED, 1, true),
    column("n", IC_TYPE_INT, 1, false),
    column("w", IC_TYPE_INT, 1, true),
  ];

  pub(super) fn table() -> Arc<TableDef> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "db/def/interp_t");
    w.add_u32(IC_TP_COLUMN_COUNT, COLUMNS.len() as u32);
    let mut id: u32 = 0;
    while (id as usize) < COLUMNS.len() {
      let c = &COLUMNS[id as usize];
      w.add_string(IC_TP_COLUMN_NAME, c.name);
      w.add_u32(IC_TP_COLUMN_ID, id);
      w.add_u32(IC_TP_COLUMN_TYPE, c.type_code);
      w.add_u32(IC_TP_COLUMN_LENGTH, c.length);
      // Only id is in the key.
      w.add_u32(IC_TP_COLUMN_IN_KEY, (id == ID) as u32);
      w.add_u32(IC_TP_COLUMN_NULLABLE, c.nullable as u32);
      w.add_u32(IC_TP_COLUMN_PRECISION, c.charset << 16);
      w.add_u32(IC_TP_COLUMN_ARRAY_KIND, c.array_kind);
      w.add_u32(IC_TP_COLUMN_END, 0);
      id += 1;
    }
    let info = parse_table_info(w.words()).expect("parsed");
    Arc::new(TableDef::new(info, None))
  }

  /// A row of the default record with `bytes` in column `id`, or NULL
  /// there.
  fn row_with(rec: &Record, id: u32, bytes: Option<&[u8]>) -> Vec<u8> {
    let mut row = vec![0u8; rec.row_size() as usize];
    let position = rec.position_of(id).expect("position");
    match bytes {
      Some(bytes) => {
        let at = rec.field(position).expect("field").offset() as usize;
        row[at..at + bytes.len()].copy_from_slice(bytes);
      }
      None => rec.set_null(&mut row, position, true).expect("null"),
    }
    row
  }

  /// A builder, the record, and a constant of an INT column.
  struct Fixture {
    b: ProgramBuilder,
    rec: Record,
  }

  impl Fixture {
    fn new() -> Fixture {
      let t = table();
      let rec = Record::default_for(&t, None).expect("record");
      Fixture {
        b: ProgramBuilder::new(&t),
        rec,
      }
    }

    fn int(&mut self, column: u32, value: i32) -> Value {
      let row = row_with(&self.rec, column, Some(&value.to_le_bytes()));
      self.b.constant(&self.rec, &row, column)
    }

    fn null(&mut self, column: u32) -> Value {
      let row = row_with(&self.rec, column, None);
      self.b.constant(&self.rec, &row, column)
    }

    fn bytes(&mut self, column: u32, bytes: &[u8]) -> Value {
      let row = row_with(&self.rec, column, Some(bytes));
      self.b.constant(&self.rec, &row, column)
    }

    fn compare_int(&mut self, cmp: Cmp, column: u32, value: i32) -> Cond {
      let c = self.b.column(column);
      let k = self.int(column, value);
      self.b.compare(cmp, c, k)
    }
  }

  // MySQL's programs, as the captures have them (chapter 05 §9.6),
  // where ours come out the same.

  #[test]
  fn a_greater_than_is_mysqls() {
    // WHERE val > 190000 (scans2.pcap, frame 62075).
    let mut f = Fixture::new();
    let top = f.compare_int(Cmp::Gt, VAL, 190000);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [0x0004_5097, 0x0001_0004, 0x0002_e630, 0x12, 0x0272_0013]
    );
    assert!(!p.writes());
    assert_eq!(p.inputs(), 0);
  }

  #[test]
  fn a_between_is_mysqls() {
    // WHERE val BETWEEN 100 AND 30000 (scans2.pcap, frame 62918).
    let mut f = Fixture::new();
    let low = f.compare_int(Cmp::Ge, VAL, 100);
    let high = f.compare_int(Cmp::Le, VAL, 30000);
    let top = f.b.and(low, high);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [
        0x0007_4097,
        0x0001_0004,
        0x0000_0064,
        0x0004_2097,
        0x0001_0004,
        0x0000_7530,
        0x12,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn strings_are_sent_as_their_columns_are() {
    // WHERE name = 'r3' (scans2.pcap, frame 65578): with its length.
    let mut f = Fixture::new();
    let c = f.b.column(NAME);
    let k = f.bytes(NAME, b"\x02r3");
    let top = f.b.compare(Cmp::Eq, c, k);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [0x0004_1097, 0x0002_0003, 0x0033_7202, 0x12, 0x0272_0013]
    );
    // WHERE code = 'c3', CHAR(4) padded with spaces (interp.pcap,
    // frame 16462).
    let mut f = Fixture::new();
    let c = f.b.column(CODE);
    let k = f.bytes(CODE, b"c3  ");
    let top = f.b.compare(Cmp::Eq, c, k);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [0x0004_1097, 0x0003_0004, 0x2020_3363, 0x12, 0x0272_0013]
    );
  }

  #[test]
  fn an_unsigned_64_bit_constant_is_mysqls() {
    // WHERE big >= 18446744073709551500 (interp.pcap, frame 17310).
    let mut f = Fixture::new();
    let c = f.b.column(BIG);
    let k = f.bytes(BIG, &18_446_744_073_709_551_500u64.to_le_bytes());
    let top = f.b.compare(Cmp::Ge, c, k);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [
        0x0005_4097,
        0x0004_0008,
        0xffff_ff8c,
        0xffff_ffff,
        0x12,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn null_tests_are_mysqls() {
    // WHERE name IS NULL (interp.pcap, frame 13922).
    let mut f = Fixture::new();
    let c = f.b.column(NAME);
    let top = f.b.is_null(c);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(p.words(), [0x0003_0019, 0x0002_0000, 0x12, 0x0272_0013]);
    // WHERE name IS NOT NULL AND val < 500 (frame 14765).
    let mut f = Fixture::new();
    let c = f.b.column(NAME);
    let some = f.b.is_not_null(c);
    let below = f.compare_int(Cmp::Lt, VAL, 500);
    let top = f.b.and(some, below);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [
        0x0006_0018,
        0x0002_0000,
        0x0004_3097,
        0x0001_0004,
        0x0000_01f4,
        0x12,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn a_column_pair_is_mysqls() {
    // WHERE val > id (interp.pcap, frame 15621).
    let mut f = Fixture::new();
    let val = f.b.column(VAL);
    let id = f.b.column(ID);
    let top = f.b.compare(Cmp::Gt, val, id);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(p.words(), [0x0003_509b, 0x0001_0000, 0x12, 0x0272_0013]);
  }

  #[test]
  fn an_or_is_mysqls() {
    // WHERE val = 10 OR name = 'r3' (interp.pcap, frame 19005).
    let mut f = Fixture::new();
    let ten = f.compare_int(Cmp::Eq, VAL, 10);
    let c = f.b.column(NAME);
    let k = f.bytes(NAME, b"\x02r3");
    let r3 = f.b.compare(Cmp::Eq, c, k);
    let top = f.b.or(ten, r3);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [
        0x0007_00d7,
        0x0001_0004,
        0x0000_000a,
        0x0004_00d7,
        0x0002_0003,
        0x0033_7202,
        0x0272_0013,
        0x12
      ]
    );
  }

  #[test]
  fn a_not_becomes_an_or_one_jump_shorter_than_mysqls() {
    // WHERE NOT (val BETWEEN 100 AND 200) (interp.pcap, frame 18163):
    // val < 100 OR val > 200, the tests as MySQL's, without its jump.
    let mut f = Fixture::new();
    let low = f.compare_int(Cmp::Ge, VAL, 100);
    let high = f.compare_int(Cmp::Le, VAL, 200);
    let between = f.b.and(low, high);
    let top = f.b.not(between);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(
      p.words(),
      [
        0x0007_40d7,
        0x0001_0004,
        0x0000_0064,
        0x0004_20d7,
        0x0001_0004,
        0x0000_00c8,
        0x0272_0013,
        0x12
      ]
    );
  }

  #[test]
  fn the_counter_adds_an_input_and_writes_back() {
    // big = big + input 0, as the AUTO_INCREMENT counter takes values.
    let mut f = Fixture::new();
    let big = f.b.column(BIG);
    let n = f.b.input(0);
    let sum = f.b.add(big, n);
    f.b.assign(BIG, sum);
    let p = f.b.finish(None).expect("finish");
    assert_eq!(
      p.words(),
      [0x0004_0001, 0x0000_8079, 0x0000_0207, 0x0004_0002, 0x12]
    );
    assert!(p.writes());
    assert_eq!(p.inputs(), 1);
    let mut inputs = [0i64; 16];
    inputs[0] = 512;
    let mut out: Vec<u32> = Vec::new();
    p.inputs_into(&inputs, &mut out);
    assert_eq!(out, [0xffff_0004, 0xffa0_0008, 512, 0]);
    // A small integer is added as it stands.
    let mut f = Fixture::new();
    let big = f.b.column(BIG);
    let k = f.b.integer(512);
    let sum = f.b.add(big, k);
    f.b.assign(BIG, sum);
    let p = f.b.finish(None).expect("finish");
    assert_eq!(p.words(), [0x0004_0001, 0x0200_8007, 0x0004_0002, 0x12]);
    assert_eq!(p.inputs(), 0);
  }

  #[test]
  fn a_constant_assigned_is_written_after_the_program() {
    let mut f = Fixture::new();
    let k = f.bytes(NAME, b"\x02r3");
    f.b.assign(NAME, k);
    let none = f.null(VAL);
    f.b.assign(VAL, none);
    let p = f.b.finish(None).expect("finish");
    assert_eq!(p.words(), [0x12]);
    assert_eq!(p.updates(), [0x0002_0003, 0x0033_7202, 0x0001_0000]);
    assert!(p.writes());
  }

  #[test]
  fn the_refusal_takes_the_operations_code() {
    let mut f = Fixture::new();
    let top = f.compare_int(Cmp::Gt, VAL, 190000);
    let p = f.b.finish(Some(top)).expect("finish");
    let mut out = vec![1];
    p.code_into(interp::IC_REFUSE_CONDITION, &mut out);
    assert_eq!(
      out,
      [1, 0x0004_5097, 0x0001_0004, 0x0002_e630, 0x12, 0x1770_0013]
    );
    out.clear();
    p.code_into(interp::IC_REFUSE_NO_ROW, &mut out);
    assert_eq!(out, p.words());
  }

  #[test]
  fn a_scans_section_has_the_inputs_before_the_reads() {
    let mut f = Fixture::new();
    let c = f.b.column(VAL);
    let k = f.b.input(1);
    let top = f.b.compare(Cmp::Gt, c, k);
    let p = f.b.finish(Some(top)).expect("finish");
    let mut inputs = [0i64; 16];
    inputs[1] = -5;
    let mut out = vec![9];
    scan_section_into(&p, &inputs, &[0xfff0_0007], &mut out);
    let code = p.words();
    let mut want = vec![5, code.len() as u32, 0, 0, 0];
    want.extend_from_slice(&[0xffff_0004, 0xffa1_0008, 0xffff_fffb]);
    want.extend_from_slice(&[0xffff_ffff, 0xfff0_0007]);
    want.extend_from_slice(code);
    assert_eq!(out, want);
    // Another table's program, or one that writes, is refused.
    let t = table();
    assert!(check_program(&p, &t, false).is_ok());
    let mut f = Fixture::new();
    let k = f.b.integer(1);
    f.b.assign(VAL, k);
    let writes = f.b.finish(None).expect("finish");
    let e = check_program(&writes, &t, false).expect_err("writes");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
    assert!(check_program(&writes, &t, true).is_ok());
  }

  #[test]
  fn what_cannot_be_compiled_is_refused_at_the_finish() {
    // A constant of another type.
    let mut f = Fixture::new();
    let c = f.b.column(VAL);
    let k = f.bytes(BIG, &1u64.to_le_bytes());
    let top = f.b.compare(Cmp::Eq, c, k);
    let e = f.b.finish(Some(top)).expect_err("types");
    assert_eq!(e.code, err::IC_ERROR_NOT_COMPARABLE);
    // A string through registers.
    let mut f = Fixture::new();
    let c = f.b.column(NAME);
    let k = f.b.input(0);
    let top = f.b.compare(Cmp::Eq, c, k);
    let e = f.b.finish(Some(top)).expect_err("string");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
    // No such column, no such input, a value from another builder.
    let mut f = Fixture::new();
    f.b.column(99);
    let e = f.b.finish(None).expect_err("column");
    assert_eq!(e.code, err::IC_ERROR_NO_SUCH_FIELD);
    let mut f = Fixture::new();
    f.b.input(16);
    let e = f.b.finish(None).expect_err("input");
    assert_eq!(e.code, err::IC_ERROR_BAD_PROGRAM);
    let mut f = Fixture::new();
    let c = f.b.column(VAL);
    f.b.is_null(Value(c.0 + 1));
    let e = f.b.finish(None).expect_err("value");
    assert_eq!(e.code, err::IC_ERROR_BAD_PROGRAM);
    let e = f.b.finish(Some(Cond(7))).expect_err("cond");
    assert_eq!(e.code, err::IC_ERROR_BAD_PROGRAM);
    // Nine registers at once.
    let mut f = Fixture::new();
    let mut v = f.b.input(0);
    let mut i: u32 = 1;
    while i < 9 {
      let next = f.b.input(i);
      v = f.b.add(next, v);
      i += 1;
    }
    let c = f.b.column(VAL);
    let top = f.b.compare(Cmp::Eq, c, v);
    let e = f.b.finish(Some(top)).expect_err("registers");
    assert_eq!(e.code, err::IC_ERROR_CONDITION_TOO_COMPLEX);
    // A condition that doubles at every step.
    let mut f = Fixture::new();
    let mut top = f.compare_int(Cmp::Eq, VAL, 1);
    let mut i: u32 = 0;
    while i < 40 {
      top = f.b.and(top, top);
      i += 1;
    }
    let e = f.b.finish(Some(top)).expect_err("doubling");
    assert_eq!(e.code, err::IC_ERROR_CONDITION_TOO_COMPLEX);
    // A column assigned twice, a string from registers.
    let mut f = Fixture::new();
    let k = f.b.integer(1);
    f.b.assign(VAL, k);
    f.b.assign(VAL, k);
    let e = f.b.finish(None).expect_err("twice");
    assert_eq!(e.code, err::IC_ERROR_FIELD_ALREADY_DEFINED);
    let mut f = Fixture::new();
    let k = f.b.integer(1);
    f.b.assign(NAME, k);
    let e = f.b.finish(None).expect_err("string");
    assert_eq!(e.code, err::IC_ERROR_NOT_SUPPORTED);
  }

  #[test]
  fn known_terms_are_settled_before_the_row() {
    // n is NOT NULL: n IS NULL never holds, and a comparison with a
    // NULL constant is unknown, so the program refuses at once.
    let mut f = Fixture::new();
    let n = f.b.column(N);
    let never = f.b.is_null(n);
    let c = f.b.column(VAL);
    let k = f.null(VAL);
    let unknown = f.b.compare(Cmp::Eq, c, k);
    let top = f.b.or(never, unknown);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(p.words(), [0x0272_0013, 0x12]);
    // NOT of the unknown stays unknown.
    let mut f = Fixture::new();
    let c = f.b.column(VAL);
    let k = f.null(VAL);
    let unknown = f.b.compare(Cmp::Eq, c, k);
    let top = f.b.not(unknown);
    let p = f.b.finish(Some(top)).expect("finish");
    assert_eq!(p.words(), [0x0002_0009, 0x12, 0x0272_0013]);
  }

  // ---- The programs run, on a model of the interpreter ----

  /// A row of the INT columns the model runs on; the others are never
  /// read.
  type Row = [Option<i64>; 7];

  /// The columns a program wrote, and what.
  pub(super) type Writes = Vec<(u32, Option<i64>)>;

  /// Run `words` on `row`: whether it accepts, and what it writes.
  /// Every instruction the compiler makes is here, as chapter 05 §9
  /// describes it; anything else, and a NULL register compared, is an
  /// error.
  pub(super) fn run(
    words: &[u32],
    row: &[Option<i64>],
    inputs: &[i64; 16],
  ) -> Result<(bool, Writes), String> {
    let mut regs: [Option<i64>; 8] = [None; 8];
    let mut writes: Writes = Vec::new();
    let mut pc: usize = 0;
    let mut steps: u32 = 0;
    loop {
      steps += 1;
      if steps > 100_000 || pc >= words.len() {
        return Err(format!("ran off at {}", pc));
      }
      let w = words[pc];
      let insn = (w & 0x3F) | (((w >> 15) & 1) << 6);
      let r1 = ((w >> 6) & 7) as usize;
      let r2 = ((w >> 9) & 7) as usize;
      let r3 = ((w >> 12) & 7) as usize;
      let r4 = ((w >> 16) & 7) as usize;
      let field = w >> 16;
      let distance = ((w >> 16) & 0x7FFF) as usize;
      match insn {
        1 => {
          regs[r1] = row[field as usize];
          pc += 1;
        }
        2 => {
          writes.push((field, regs[r1]));
          pc += 1;
        }
        3 => {
          regs[r1] = None;
          pc += 1;
        }
        4 => {
          regs[r1] = Some(field as i64);
          pc += 1;
        }
        5 => {
          regs[r1] = Some(words[pc + 1] as i64);
          pc += 2;
        }
        6 => {
          let value = words[pc + 1] as u64 | ((words[pc + 2] as u64) << 32);
          regs[r1] = Some(value as i64);
          pc += 3;
        }
        7 | 8 => {
          regs[r4] = match (regs[r1], regs[r2]) {
            (Some(a), Some(b)) if insn == 7 => Some(a + b),
            (Some(a), Some(b)) => Some(a - b),
            _ => None,
          };
          pc += 1;
        }
        71 | 72 => {
          let k = field as i64;
          regs[r3] = match regs[r1] {
            Some(a) if insn == 71 => Some(a + k),
            Some(a) => Some(a - k),
            None => None,
          };
          pc += 1;
        }
        9 => pc += distance,
        10 | 11 => {
          if regs[r1].is_none() == (insn == 10) {
            pc += distance;
          } else {
            pc += 1;
          }
        }
        12..=17 => {
          let (a, b) = match (regs[r1], regs[r2]) {
            (Some(a), Some(b)) => (a, b),
            _ => return Err("878".to_string()),
          };
          if holds(insn - 12, a, b) {
            pc += distance;
          } else {
            pc += 1;
          }
        }
        18 => return Ok((true, writes)),
        19 => return Ok((false, Vec::new())),
        23 | 27 => {
          let w2 = words[pc + 1];
          let column = row[(w2 >> 16) as usize];
          let (value, len) = if insn == 23 {
            let len = (w2 & 0xFFFF) as usize;
            if len != 4 {
              return Err(format!("value of {} bytes", len));
            }
            (Some(words[pc + 2] as i32 as i64), 2 + len.div_ceil(4))
          } else {
            (row[(w2 & 0xFFFF) as usize], 2)
          };
          let cmp = (w >> 12) & 0xF;
          match (value, column) {
            (Some(v), Some(c)) => {
              if holds(cmp, v, c) {
                pc += distance;
              } else {
                pc += len;
              }
            }
            _ => match (w >> 6) & 3 {
              2 => pc += distance,
              3 => pc += len,
              rule => return Err(format!("NULL rule {}", rule)),
            },
          }
        }
        24 | 25 => {
          let column = row[(words[pc + 1] >> 16) as usize];
          if column.is_none() == (insn == 24) {
            pc += distance;
          } else {
            pc += 2;
          }
        }
        121 => {
          regs[r1] = Some(inputs[field as usize]);
          pc += 1;
        }
        _ => return Err(format!("instruction {}", insn)),
      }
    }
  }

  /// Comparison `cmp`, 0 to 5, of `a` with `b`.
  fn holds(cmp: u32, a: i64, b: i64) -> bool {
    match cmp {
      0 => a == b,
      1 => a != b,
      2 => a < b,
      3 => a <= b,
      4 => a > b,
      _ => a >= b,
    }
  }

  /// What SQL makes of a value, worked out from the tree.
  fn value_of(
    b: &ProgramBuilder,
    v: Value,
    row: &Row,
    inputs: &[i64; 16],
  ) -> Option<i64> {
    match &b.values[v.0 as usize] {
      ValueNode::Column(c) => row[*c as usize],
      ValueNode::Constant { column, bytes } => match bytes {
        Some(bytes) => {
          let attr = b.table.field(*column).expect("column");
          Some(integer_of(attr, bytes).expect("integer"))
        }
        None => None,
      },
      ValueNode::Integer(k) => Some(*k),
      ValueNode::Input(n) => Some(inputs[*n as usize]),
      ValueNode::Add(x, y) => {
        Some(value_of(b, *x, row, inputs)? + value_of(b, *y, row, inputs)?)
      }
      ValueNode::Subtract(x, y) => {
        Some(value_of(b, *x, row, inputs)? - value_of(b, *y, row, inputs)?)
      }
    }
  }

  /// What SQL makes of a condition: true, false or unknown.
  fn truth_of(
    b: &ProgramBuilder,
    c: Cond,
    row: &Row,
    inputs: &[i64; 16],
  ) -> Option<bool> {
    match b.conds[c.0 as usize] {
      CondNode::Compare(cmp, x, y) => {
        let x = value_of(b, x, row, inputs)?;
        let y = value_of(b, y, row, inputs)?;
        let index = match cmp {
          Cmp::Eq => 0,
          Cmp::Ne => 1,
          Cmp::Lt => 2,
          Cmp::Le => 3,
          Cmp::Gt => 4,
          Cmp::Ge => 5,
        };
        Some(holds(index, x, y))
      }
      CondNode::IsNull(v) => Some(value_of(b, v, row, inputs).is_none()),
      CondNode::IsNotNull(v) => Some(value_of(b, v, row, inputs).is_some()),
      CondNode::And(x, y) => {
        match (truth_of(b, x, row, inputs), truth_of(b, y, row, inputs)) {
          (Some(false), _) | (_, Some(false)) => Some(false),
          (Some(true), Some(true)) => Some(true),
          _ => None,
        }
      }
      CondNode::Or(x, y) => {
        match (truth_of(b, x, row, inputs), truth_of(b, y, row, inputs)) {
          (Some(true), _) | (_, Some(true)) => Some(true),
          (Some(false), Some(false)) => Some(false),
          _ => None,
        }
      }
      CondNode::Not(x) => truth_of(b, x, row, inputs).map(|t| !t),
    }
  }

  /// A small generator of trees, the same ones every run.
  struct Gen {
    state: u64,
  }

  impl Gen {
    fn below(&mut self, n: u32) -> u32 {
      self.state ^= self.state << 13;
      self.state ^= self.state >> 7;
      self.state ^= self.state << 17;
      ((self.state >> 32) as u32) % n
    }

    fn value(&mut self, f: &mut Fixture, depth: u32) -> Value {
      let columns = [ID, VAL, N, W];
      match self.below(if depth > 1 { 4 } else { 6 }) {
        0 | 1 => f.b.column(columns[self.below(4) as usize]),
        2 => {
          let k = self.below(5) as i32 - 2;
          if self.below(4) == 0 {
            f.null(VAL)
          } else {
            f.int(columns[self.below(4) as usize], k)
          }
        }
        3 => {
          if self.below(2) == 0 {
            f.b.integer(self.below(5) as i64 - 2)
          } else {
            f.b.input(self.below(3))
          }
        }
        4 => {
          let x = self.value(f, depth + 1);
          let y = self.value(f, depth + 1);
          f.b.add(x, y)
        }
        _ => {
          let x = self.value(f, depth + 1);
          let y = self.value(f, depth + 1);
          f.b.subtract(x, y)
        }
      }
    }

    fn cond(&mut self, f: &mut Fixture, depth: u32) -> Cond {
      let cmps = [Cmp::Eq, Cmp::Ne, Cmp::Lt, Cmp::Le, Cmp::Gt, Cmp::Ge];
      match self.below(if depth > 3 { 3 } else { 6 }) {
        0 => {
          let cmp = cmps[self.below(6) as usize];
          let x = self.value(f, 0);
          let y = self.value(f, 0);
          f.b.compare(cmp, x, y)
        }
        1 => {
          let x = self.value(f, 0);
          if self.below(2) == 0 {
            f.b.is_null(x)
          } else {
            f.b.is_not_null(x)
          }
        }
        2 => {
          let x = self.cond(f, depth + 1);
          f.b.not(x)
        }
        3 | 4 => {
          let x = self.cond(f, depth + 1);
          let y = self.cond(f, depth + 1);
          f.b.and(x, y)
        }
        _ => {
          let x = self.cond(f, depth + 1);
          let y = self.cond(f, depth + 1);
          f.b.or(x, y)
        }
      }
    }
  }

  #[test]
  fn compiled_programs_do_what_sqls_logic_says() {
    let domain: [Option<i64>; 4] = [None, Some(-1), Some(0), Some(2)];
    let mut rows: Vec<Row> = Vec::new();
    for id in [-1, 0, 2] {
      for val in domain {
        for n in [-1, 0, 2] {
          for w in domain {
            let mut row: Row = [None; 7];
            row[ID as usize] = Some(id);
            row[VAL as usize] = val;
            row[N as usize] = Some(n);
            row[W as usize] = w;
            rows.push(row);
          }
        }
      }
    }
    let mut g = Gen {
      state: 0x9E37_79B9_7F4A_7C15,
    };
    let mut compiled: u32 = 0;
    let mut round: u32 = 0;
    while round < 600 {
      round += 1;
      let mut f = Fixture::new();
      let top = g.cond(&mut f, 0);
      // An assignment to w, sometimes with a condition of its own.
      let assign = g.below(3);
      let mut condition: Option<Cond> = None;
      let mut value: Option<Value> = None;
      if assign > 0 {
        let column = f.b.column(VAL);
        let k = f.b.integer(g.below(70_000) as i64 - 35_000);
        let sum = f.b.add(column, k);
        value = Some(sum);
        if assign == 2 {
          let c = g.cond(&mut f, 2);
          condition = Some(c);
          f.b.assign_if(W, sum, c);
        } else {
          f.b.assign(W, sum);
        }
      }
      let program = match f.b.finish(Some(top)) {
        Ok(program) => program,
        Err(e) if e.code == err::IC_ERROR_CONDITION_TOO_COMPLEX => continue,
        Err(e) => panic!("round {}: error {}", round, e.code),
      };
      compiled += 1;
      let mut inputs = [0i64; 16];
      inputs[0] = g.below(5) as i64 - 2;
      inputs[1] = g.below(5) as i64 - 2;
      inputs[2] = g.below(5) as i64 - 2;
      for row in &rows {
        let (accepted, writes) = run(program.words(), row, &inputs)
          .unwrap_or_else(|e| panic!("round {}: {}", round, e));
        let expected = truth_of(&f.b, top, row, &inputs) == Some(true);
        assert_eq!(accepted, expected, "round {}, row {:?}", round, row);
        let mut expected_writes: Writes = Vec::new();
        if let (true, Some(sum)) = (expected, value) {
          let holds = match condition {
            Some(c) => truth_of(&f.b, c, row, &inputs) == Some(true),
            None => true,
          };
          if holds {
            expected_writes.push((W, value_of(&f.b, sum, row, &inputs)));
          }
        }
        assert_eq!(writes, expected_writes, "round {}, row {:?}", round, row);
      }
    }
    // Most trees fit the registers.
    assert!(compiled > 500, "{} compiled", compiled);
  }
}
