// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! Interpreted programs: the instructions a data node runs on the row
//! it holds, as a scan's filter or as a key operation's condition and
//! the updates it computes from the row, and the column section that
//! carries them. The words are chapter 05 §9's, from the author's
//! account of the interpreter and the programs MySQL sends, with names
//! of our own for the instructions.
//!
//! **An instruction is a word**, some with words after it. Its number
//! is in bits 0–5, plus 64 when bit 15 is set; a first register is in
//! bits 6–8, a second in 9–11, a third in 12–14, a fourth in 16–18; and
//! bits 16–31, the field, otherwise hold what the instruction needs: a
//! column id, a 16-bit constant, a jump's distance, a refusal's code.
//! There are eight registers.
//!
//! **A jump's distance is in words, from the jump's first word**, and
//! is known only once the place it goes to is: [`ProgramWriter`] hands
//! out labels, and puts the distances in when the program is finished.
//! Every jump goes forward, as every jump seen on the wire does.
//!
//! **A column test compares the value in the program, or another
//! column, with the column, the value first**: `Lt` jumps when the
//! value is less than the column. Its NULL rule says what a NULL column
//! or value does. MySQL compiles SQL's logic with two of the rules: a
//! term of an AND, negated, jumps to the refusal with rule `Jump`, a
//! term of an OR jumps to the acceptance with rule `GoOn`, and a NULL
//! makes the term fail either way.
//!
//! **A program ends** with `ACCEPT`, the row passes, or `REFUSE` with a
//! code: a scan skips the row on 626, and a key operation fails with
//! the code. Ours refuse with 626 where a missed condition is not to
//! abort and with 6000 where it is ([`IC_REFUSE_NO_ROW`],
//! [`IC_REFUSE_CONDITION`]).
//!
//! **The column section of an interpreted operation** begins with five
//! sizes: the reads before the program, the program, the updates after
//! it, the reads after those, and the subroutines; then the five parts
//! ([`column_parts_into`]). A key operation that carries one sets the
//! interpreted flag.
//!
//! **Inputs** let one program serve many operations: up to 16 integers
//! of 64 bits, set for each operation in front of the reads before the
//! program ([`inputs_into`]) and read into registers by
//! `READ_INPUT`. An input the operation does not send is not cleared,
//! so a program reads only the inputs its operation sends.
//!
//! Verify: chapter 05 §9.

use ic_port::err;
use ic_port::IcError;

// ---- Instructions (chapter 05 §9.2) ----

/// R1 = the row's value of the column in the field.
pub const IC_INSN_READ_COLUMN: u32 = 1;
/// The column in the field = R1.
pub const IC_INSN_WRITE_COLUMN: u32 = 2;
/// R1 = NULL.
pub const IC_INSN_SET_NULL: u32 = 3;
/// R1 = the field, unsigned.
pub const IC_INSN_SET_16: u32 = 4;
/// R1 = the next word.
pub const IC_INSN_SET_32: u32 = 5;
/// R1 = the next two words, the low one first.
pub const IC_INSN_SET_64: u32 = 6;
/// R4 = R1 + R2; NULL if either is.
pub const IC_INSN_ADD: u32 = 7;
/// R4 = R1 − R2; NULL if either is.
pub const IC_INSN_SUBTRACT: u32 = 8;
/// Jump.
pub const IC_INSN_JUMP: u32 = 9;
/// Jump if R1 is NULL.
pub const IC_INSN_JUMP_IF_NULL: u32 = 10;
/// Jump if R1 is not NULL.
pub const IC_INSN_JUMP_IF_VALUE: u32 = 11;
/// Jump if R1 = R2; 13 to 17 are ≠, <, ≤, >, ≥ (see [`RegCmp`]). A NULL
/// in either register fails the program with 878.
pub const IC_INSN_JUMP_IF_EQ: u32 = 12;
/// End; the row passes.
pub const IC_INSN_ACCEPT: u32 = 18;
/// End; the row fails, with the code in the field.
pub const IC_INSN_REFUSE: u32 = 19;
/// End; the row passes and is its fragment's last.
pub const IC_INSN_ACCEPT_LAST: u32 = 22;
/// Jump on the column in word 2 compared with the value after it.
pub const IC_INSN_TEST_COLUMN_VALUE: u32 = 23;
/// Jump if the column in word 2's field is NULL.
pub const IC_INSN_JUMP_IF_COLUMN_NULL: u32 = 24;
/// Jump if the column in word 2's field is not NULL.
pub const IC_INSN_JUMP_IF_COLUMN_VALUE: u32 = 25;
/// Jump on the column in word 2 compared with another column.
pub const IC_INSN_TEST_COLUMN_COLUMN: u32 = 27;
/// R3 = R1 + the field, unsigned; NULL if R1 is.
pub const IC_INSN_ADD_IMM: u32 = 71;
/// R3 = R1 − the field, unsigned; NULL if R1 is.
pub const IC_INSN_SUBTRACT_IMM: u32 = 72;
/// R1 = the input in the field, a signed integer, never NULL.
pub const IC_INSN_READ_INPUT: u32 = 121;

/// The inputs an operation may give its program.
pub const IC_INTERP_INPUTS: u32 = 16;
/// The column id the column header of input 0 names, before the reads
/// of an interpreted operation; input n names this plus n.
pub const IC_INPUT_COLUMN_FIRST: u32 = 0xFFA0;
/// The upper half of the word that comes before the inputs; its lower
/// half is their length in words, this word with them.
pub const IC_INPUTS_MARK: u32 = 0xFFFF;

/// The registers there are.
pub const IC_INTERP_REGISTERS: u32 = 8;

/// The code a refusal gives where a missed condition is not to abort:
/// a scan skips the row, and a key operation that ignores errors fails
/// alone, as a row not found. Verify: chapter 05 §9.4.
pub const IC_REFUSE_NO_ROW: u16 = 626;
/// The code a refusal gives where a missed condition is to abort: a key
/// operation that aborts on error, whose transaction's abort then says
/// that the condition failed. Ours, the first of the range NDB keeps
/// for programs' own codes. Verify: chapter 05 §9.4, §9.8.
pub const IC_REFUSE_CONDITION: u16 = 6000;

/// The longest jump, in words: bits 16–30 hold it. Bit 31 would make it
/// a jump backwards, which no program of ours makes.
const IC_MAX_JUMP: u32 = 0x7FFF;
/// The largest column id, value length or constant the field holds.
const IC_MAX_FIELD: u32 = 0xFFFF;
/// A label not yet placed.
const IC_LABEL_UNPLACED: u32 = u32::MAX;

/// How a column test compares, in bits 12–15 of its first word. The
/// value, or the other column, stands first.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ColumnCmp {
  /// The value equals the column.
  Eq = 0,
  /// The value does not equal the column.
  Ne = 1,
  /// The value is less than the column.
  Lt = 2,
  /// The value is less than or equal to the column.
  Le = 3,
  /// The value is greater than the column.
  Gt = 4,
  /// The value is greater than or equal to the column.
  Ge = 5,
  /// The column matches the pattern (later).
  Like = 6,
  /// The column does not match the pattern (later).
  NotLike = 7,
}

/// What a column test does when the column or the value is NULL, in
/// bits 6–7 of its first word. A value of length 0 is NULL.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum NullRule {
  /// Compare anyway: NULL equals NULL and is less than any value.
  Compare = 0,
  /// Jump.
  Jump = 2,
  /// Go on to the next instruction.
  GoOn = 3,
}

/// How a register jump compares R1 with R2: its instruction is
/// [`IC_INSN_JUMP_IF_EQ`] plus this.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum RegCmp {
  /// R1 = R2.
  Eq = 0,
  /// R1 ≠ R2.
  Ne = 1,
  /// R1 < R2.
  Lt = 2,
  /// R1 ≤ R2.
  Le = 3,
  /// R1 > R2.
  Gt = 4,
  /// R1 ≥ R2.
  Ge = 5,
}

/// A place in a program that jumps go to, handed out before it is
/// placed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Label(u32);

/// A jump waiting for its label's place.
#[derive(Clone, Copy, Debug)]
struct Jump {
  /// The jump's first word.
  at: u32,
  label: Label,
}

/// The instruction word of instruction `insn`, 0 to 127.
fn insn_word(insn: u32) -> u32 {
  (insn & 0x3F) | ((insn & 0x40) << 9)
}

/// A register in bits 6–8.
fn r1(reg: u32) -> u32 {
  (reg & 7) << 6
}

/// A register in bits 9–11.
fn r2(reg: u32) -> u32 {
  (reg & 7) << 9
}

/// A register in bits 12–14.
fn r3(reg: u32) -> u32 {
  (reg & 7) << 12
}

/// A register in bits 16–18.
fn r4(reg: u32) -> u32 {
  (reg & 7) << 16
}

/// A value in the field, bits 16–31.
fn field(value: u32) -> u32 {
  (value & IC_MAX_FIELD) << 16
}

/// A program, written an instruction at a time.
///
/// What cannot be encoded, a register beyond the eighth, a column id or
/// a value longer than the field holds, is kept as the writer's error
/// and returned by [`finish`](Self::finish), so that a condition is
/// written without a check after every instruction, as the C would. The
/// writer is kept and cleared for the next program, so its buffers are
/// allocated once.
#[derive(Default)]
pub struct ProgramWriter {
  words: Vec<u32>,
  /// Where each label is placed, as a word index.
  labels: Vec<u32>,
  jumps: Vec<Jump>,
  error: Option<IcError>,
}

impl std::fmt::Debug for ProgramWriter {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "ProgramWriter({} words, {} labels{})",
      self.words.len(),
      self.labels.len(),
      match self.error {
        Some(e) => format!(", error {}", e.code),
        None => String::new(),
      }
    )
  }
}

impl ProgramWriter {
  /// An empty program.
  pub fn new() -> ProgramWriter {
    ProgramWriter::default()
  }

  /// Empty the writer for the next program, keeping its buffers.
  pub fn clear(&mut self) {
    self.words.clear();
    self.labels.clear();
    self.jumps.clear();
    self.error = None;
  }

  /// The words written so far; the jumps have their distances only
  /// once the program is finished.
  pub fn words(&self) -> &[u32] {
    &self.words
  }

  /// Words written so far.
  pub fn len(&self) -> usize {
    self.words.len()
  }

  /// True before the first instruction.
  pub fn is_empty(&self) -> bool {
    self.words.is_empty()
  }

  /// A new label, to be placed once.
  pub fn new_label(&mut self) -> Label {
    self.labels.push(IC_LABEL_UNPLACED);
    Label(self.labels.len() as u32 - 1)
  }

  /// Place `label` at the next instruction.
  pub fn place(&mut self, label: Label) {
    let index = label.0 as usize;
    if index < self.labels.len() && self.labels[index] == IC_LABEL_UNPLACED {
      self.labels[index] = self.words.len() as u32;
    } else {
      self.fail(err::IC_ERROR_INCONSISTENT_DATA);
    }
  }

  /// Put the jumps' distances in, and give the first error met.
  /// Afterwards [`words`](Self::words) is the program.
  pub fn finish(&mut self) -> Result<(), IcError> {
    if let Some(e) = self.error {
      return Err(e);
    }
    for jump in &self.jumps {
      let label = jump.label.0 as usize;
      let to = self.labels.get(label).copied().unwrap_or(IC_LABEL_UNPLACED);
      if to == IC_LABEL_UNPLACED || to <= jump.at {
        return Err(IcError::new(err::IC_ERROR_INCONSISTENT_DATA));
      }
      let distance = to - jump.at;
      if distance > IC_MAX_JUMP {
        return Err(IcError::new(err::IC_ERROR_RECORD_SIZE_TOO_BIG));
      }
      self.words[jump.at as usize] |= distance << 16;
    }
    self.jumps.clear();
    Ok(())
  }

  /// R`reg` = the row's value of `column`.
  pub fn read_column(&mut self, reg: u32, column: u32) {
    self.check_reg(reg);
    self.check_column(column);
    self.push(insn_word(IC_INSN_READ_COLUMN) | r1(reg) | field(column));
  }

  /// `column` = R`reg`. Only in an update's program.
  pub fn write_column(&mut self, column: u32, reg: u32) {
    self.check_reg(reg);
    self.check_column(column);
    self.push(insn_word(IC_INSN_WRITE_COLUMN) | r1(reg) | field(column));
  }

  /// R`reg` = NULL.
  pub fn set_null(&mut self, reg: u32) {
    self.check_reg(reg);
    self.push(insn_word(IC_INSN_SET_NULL) | r1(reg));
  }

  /// R`reg` = `value`, in the instruction word itself.
  pub fn set_16(&mut self, reg: u32, value: u16) {
    self.check_reg(reg);
    self.push(insn_word(IC_INSN_SET_16) | r1(reg) | field(value as u32));
  }

  /// R`reg` = `value`, in the word after.
  pub fn set_32(&mut self, reg: u32, value: u32) {
    self.check_reg(reg);
    self.push(insn_word(IC_INSN_SET_32) | r1(reg));
    self.push(value);
  }

  /// R`reg` = `value`, in the two words after, the low one first.
  pub fn set_64(&mut self, reg: u32, value: u64) {
    self.check_reg(reg);
    self.push(insn_word(IC_INSN_SET_64) | r1(reg));
    self.push(value as u32);
    self.push((value >> 32) as u32);
  }

  /// R`reg` = input `input`, as a signed integer.
  pub fn read_input(&mut self, reg: u32, input: u32) {
    self.check_reg(reg);
    if input >= IC_INTERP_INPUTS {
      self.fail(err::IC_ERROR_BAD_PROGRAM);
    }
    let word = insn_word(IC_INSN_READ_INPUT) | r1(reg) | field(input);
    self.push(word);
  }

  /// R`dest` = R`a` + R`b`.
  pub fn add(&mut self, dest: u32, a: u32, b: u32) {
    self.arith(IC_INSN_ADD, dest, a, b);
  }

  /// R`dest` = R`a` − R`b`.
  pub fn subtract(&mut self, dest: u32, a: u32, b: u32) {
    self.arith(IC_INSN_SUBTRACT, dest, a, b);
  }

  /// R`dest` = R`src` + `value`.
  pub fn add_imm(&mut self, dest: u32, src: u32, value: u16) {
    self.arith_imm(IC_INSN_ADD_IMM, dest, src, value);
  }

  /// R`dest` = R`src` − `value`.
  pub fn subtract_imm(&mut self, dest: u32, src: u32, value: u16) {
    self.arith_imm(IC_INSN_SUBTRACT_IMM, dest, src, value);
  }

  /// Jump to `to`.
  pub fn jump(&mut self, to: Label) {
    self.push_jump(insn_word(IC_INSN_JUMP), to);
  }

  /// Jump to `to` if R`reg` is NULL.
  pub fn jump_if_null(&mut self, reg: u32, to: Label) {
    self.check_reg(reg);
    self.push_jump(insn_word(IC_INSN_JUMP_IF_NULL) | r1(reg), to);
  }

  /// Jump to `to` if R`reg` is not NULL.
  pub fn jump_if_value(&mut self, reg: u32, to: Label) {
    self.check_reg(reg);
    self.push_jump(insn_word(IC_INSN_JUMP_IF_VALUE) | r1(reg), to);
  }

  /// Jump to `to` if R`a` compares with R`b` as `cmp` says. Both must
  /// hold values: a NULL fails the program with 878.
  pub fn jump_if(&mut self, cmp: RegCmp, a: u32, b: u32, to: Label) {
    self.check_reg(a);
    self.check_reg(b);
    let insn = IC_INSN_JUMP_IF_EQ + cmp as u32;
    self.push_jump(insn_word(insn) | r1(a) | r2(b), to);
  }

  /// End; the row passes.
  pub fn accept(&mut self) {
    self.push(insn_word(IC_INSN_ACCEPT));
  }

  /// End; the row passes and is its fragment's last.
  pub fn accept_last(&mut self) {
    self.push(insn_word(IC_INSN_ACCEPT_LAST));
  }

  /// End; the row fails with `code`.
  pub fn refuse(&mut self, code: u16) {
    self.push(insn_word(IC_INSN_REFUSE) | field(code as u32));
  }

  /// Jump to `to` if `value` compares with `column` as `cmp` says, the
  /// value first, or as `null_rule` says when either is NULL. The value
  /// is in the column's own format: a fixed-size type at the column's
  /// whole size, a variable one with its length bytes, which its length
  /// counts. An empty value is NULL.
  pub fn test_column_value(
    &mut self,
    cmp: ColumnCmp,
    null_rule: NullRule,
    column: u32,
    value: &[u8],
    to: Label,
  ) {
    self.check_column(column);
    if value.len() > IC_MAX_FIELD as usize {
      self.fail(err::IC_ERROR_VALUE_TOO_LONG);
    }
    self.push_test(IC_INSN_TEST_COLUMN_VALUE, cmp, null_rule, to);
    self.push(field(column) | (value.len() as u32 & IC_MAX_FIELD));
    let mut i: usize = 0;
    while i < value.len() {
      let mut word = [0u8; 4];
      let take = (value.len() - i).min(4);
      word[..take].copy_from_slice(&value[i..i + take]);
      self.push(u32::from_ne_bytes(word));
      i += 4;
    }
  }

  /// Jump to `to` if `other` compares with `column` as `cmp` says,
  /// `other` first, or as `null_rule` says when either is NULL. The
  /// data node reads `other` as though it had `column`'s type, so the
  /// two must have one type, length, precision, scale and collation.
  pub fn test_column_column(
    &mut self,
    cmp: ColumnCmp,
    null_rule: NullRule,
    column: u32,
    other: u32,
    to: Label,
  ) {
    self.check_column(column);
    self.check_column(other);
    self.push_test(IC_INSN_TEST_COLUMN_COLUMN, cmp, null_rule, to);
    self.push(field(column) | (other & IC_MAX_FIELD));
  }

  /// Jump to `to` if `column` is NULL.
  pub fn jump_if_column_null(&mut self, column: u32, to: Label) {
    self.check_column(column);
    self.push_jump(insn_word(IC_INSN_JUMP_IF_COLUMN_NULL), to);
    self.push(field(column));
  }

  /// Jump to `to` if `column` is not NULL.
  pub fn jump_if_column_value(&mut self, column: u32, to: Label) {
    self.check_column(column);
    self.push_jump(insn_word(IC_INSN_JUMP_IF_COLUMN_VALUE), to);
    self.push(field(column));
  }

  fn arith(&mut self, insn: u32, dest: u32, a: u32, b: u32) {
    self.check_reg(dest);
    self.check_reg(a);
    self.check_reg(b);
    self.push(insn_word(insn) | r1(a) | r2(b) | r4(dest));
  }

  fn arith_imm(&mut self, insn: u32, dest: u32, src: u32, value: u16) {
    self.check_reg(dest);
    self.check_reg(src);
    self.push(insn_word(insn) | r1(src) | r3(dest) | field(value as u32));
  }

  /// A column test's first word, its distance left for
  /// [`finish`](Self::finish).
  fn push_test(
    &mut self,
    insn: u32,
    cmp: ColumnCmp,
    null_rule: NullRule,
    to: Label,
  ) {
    let mut word = insn_word(insn) | ((null_rule as u32) << 6);
    word |= (cmp as u32) << 12;
    // A comparison of 8 or more sets bit 15, which makes the
    // instruction number 64 more; the bit masks that need it are for
    // later, and ColumnCmp has none.
    self.push_jump(word, to);
  }

  fn push_jump(&mut self, word: u32, to: Label) {
    let at = self.words.len() as u32;
    self.jumps.push(Jump { at, label: to });
    self.push(word);
  }

  fn push(&mut self, word: u32) {
    self.words.push(word);
  }

  fn check_reg(&mut self, reg: u32) {
    if reg >= IC_INTERP_REGISTERS {
      self.fail(err::IC_ERROR_CONDITION_TOO_COMPLEX);
    }
  }

  fn check_column(&mut self, column: u32) {
    if column > IC_MAX_FIELD {
      self.fail(err::IC_ERROR_NO_SUCH_FIELD);
    }
  }

  /// Keep the first error.
  fn fail(&mut self, code: i32) {
    if self.error.is_none() {
      self.error = Some(IcError::new(code));
    }
  }
}

/// The inputs whose bits `mask` sets, from `values`, in front of the
/// reads before the program, appended to `out`: the word that says
/// their length, then for each its column header, of 8 bytes, and its
/// value, the low word first. Nothing when the mask is empty.
/// Verify: chapter 05 §9.5.
pub fn inputs_into(
  mask: u32,
  values: &[i64; IC_INTERP_INPUTS as usize],
  out: &mut Vec<u32>,
) {
  let count = (mask & 0xFFFF).count_ones();
  if count == 0 {
    return;
  }
  out.push((IC_INPUTS_MARK << 16) | (1 + 3 * count));
  let mut input: u32 = 0;
  while input < IC_INTERP_INPUTS {
    if (mask >> input) & 1 != 0 {
      let value = values[input as usize];
      out.push(((IC_INPUT_COLUMN_FIRST + input) << 16) | 8);
      out.push(value as u32);
      out.push((value as u64 >> 32) as u32);
    }
    input += 1;
  }
}

/// The column section of an interpreted operation, into `out`, cleared
/// first: the five sizes, then the reads before the program, the
/// program, the updates after it (column headers and values, as a
/// write sends them), the reads after those (column headers), and the
/// subroutines. Verify: chapter 05 §9.5.
pub fn column_parts_into(
  read: &[u32],
  program: &[u32],
  updates: &[u32],
  final_read: &[u32],
  subroutines: &[u32],
  out: &mut Vec<u32>,
) {
  out.clear();
  let parts = [read, program, updates, final_read, subroutines];
  let mut total: usize = 5;
  for part in parts {
    total += part.len();
  }
  out.reserve(total);
  for part in parts {
    out.push(part.len() as u32);
  }
  for part in parts {
    out.extend_from_slice(part);
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  /// The value bytes of a 32-bit integer, as a column holds it.
  fn int(value: i32) -> [u8; 4] {
    value.to_ne_bytes()
  }

  // The programs below are MySQL's, word for word, from the captures
  // chapter 05 §9.6 reads: scans2.pcap of scan_t (0 id INT, 1 val INT,
  // 2 name VARCHAR(32)) and interp.pcap of interp_t (the same, and
  // 3 code CHAR(4), 4 big BIGINT UNSIGNED).

  #[test]
  fn a_greater_than_refuses_when_the_value_is_not_less() {
    // WHERE val > 190000 (scans2.pcap, frame 62075).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.test_column_value(ColumnCmp::Ge, NullRule::Jump, 1, &int(190000), refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0004_5097,
        0x0001_0004,
        0x0002_e630,
        0x0000_0012,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn a_between_is_two_tests_falling_through() {
    // WHERE val BETWEEN 100 AND 30000 (scans2.pcap, frame 62918).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.test_column_value(ColumnCmp::Gt, NullRule::Jump, 1, &int(100), refuse);
    w.test_column_value(ColumnCmp::Lt, NullRule::Jump, 1, &int(30000), refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0007_4097,
        0x0001_0004,
        0x0000_0064,
        0x0004_2097,
        0x0001_0004,
        0x0000_7530,
        0x0000_0012,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn two_ranges_joined_by_or_jump_past_each_other() {
    // Two BETWEENs joined by OR (scans2.pcap, frame 63781): the first
    // range fails to the second, and passes by a jump to the ACCEPT.
    let mut w = ProgramWriter::new();
    let second = w.new_label();
    let refuse = w.new_label();
    let accept = w.new_label();
    w.test_column_value(ColumnCmp::Gt, NullRule::Jump, 1, &int(100), second);
    w.test_column_value(ColumnCmp::Lt, NullRule::Jump, 1, &int(200), second);
    w.jump(accept);
    w.place(second);
    w.test_column_value(ColumnCmp::Gt, NullRule::Jump, 1, &int(300), refuse);
    w.test_column_value(ColumnCmp::Lt, NullRule::Jump, 1, &int(400), refuse);
    w.jump(accept);
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.place(accept);
    w.accept();
    w.finish().expect("finish");
    let words = w.words();
    assert_eq!(words.len(), 16);
    assert_eq!(words[0], 0x0007_4097);
    assert_eq!(words[3], 0x0004_2097);
    assert_eq!(words[6], 0x0009_0009);
    assert_eq!(words[7], 0x0007_4097);
    assert_eq!(words[10], 0x0004_2097);
    assert_eq!(words[13], 0x0002_0009);
    assert_eq!(words[14], 0x0272_0013);
    assert_eq!(words[15], 0x0000_0012);
  }

  #[test]
  fn a_varchar_value_is_sent_with_its_length_byte() {
    // WHERE name = 'r3' (scans2.pcap, frame 65578).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.test_column_value(ColumnCmp::Ne, NullRule::Jump, 2, b"\x02r3", refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0004_1097,
        0x0002_0003,
        0x0033_7202,
        0x0000_0012,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn an_and_of_equalities_compares_nulls() {
    // MySQL's scan of a table of its own, column 0 = 494 and column 1
    // = 11, with rule 0 (interp.pcap, frame 9698).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.test_column_value(ColumnCmp::Ne, NullRule::Compare, 0, &int(494), refuse);
    w.test_column_value(ColumnCmp::Ne, NullRule::Compare, 1, &int(11), refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0007_1017,
        0x0000_0004,
        0x0000_01ee,
        0x0004_1017,
        0x0001_0004,
        0x0000_000b,
        0x0000_0012,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn is_null_and_is_not_null_test_the_column_alone() {
    // WHERE name IS NULL (interp.pcap, frame 13922).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.jump_if_column_value(2, refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(w.words(), [0x0003_0019, 0x0002_0000, 0x12, 0x0272_0013]);
    // WHERE name IS NOT NULL AND val < 500 (frame 14765).
    w.clear();
    let refuse = w.new_label();
    w.jump_if_column_null(2, refuse);
    w.test_column_value(ColumnCmp::Le, NullRule::Jump, 1, &int(500), refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0006_0018,
        0x0002_0000,
        0x0004_3097,
        0x0001_0004,
        0x0000_01f4,
        0x0000_0012,
        0x0272_0013
      ]
    );
  }

  #[test]
  fn a_column_pair_puts_the_other_column_first() {
    // WHERE val > id: refuse if id >= val (interp.pcap, frame 15621).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.test_column_column(ColumnCmp::Ge, NullRule::Jump, 1, 0, refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(w.words(), [0x0003_509b, 0x0001_0000, 0x12, 0x0272_0013]);
  }

  #[test]
  fn fixed_size_values_are_sent_whole() {
    // WHERE code = 'c3', a CHAR(4) padded with spaces (frame 16462).
    let mut w = ProgramWriter::new();
    let refuse = w.new_label();
    w.test_column_value(ColumnCmp::Ne, NullRule::Jump, 3, b"c3  ", refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [0x0004_1097, 0x0003_0004, 0x2020_3363, 0x12, 0x0272_0013]
    );
    // WHERE big >= 18446744073709551500, unsigned 64 bits (frame 17310).
    w.clear();
    let refuse = w.new_label();
    let big = 18_446_744_073_709_551_500u64.to_ne_bytes();
    w.test_column_value(ColumnCmp::Gt, NullRule::Jump, 4, &big, refuse);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
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
  fn an_or_and_a_not_go_on_past_nulls() {
    // WHERE NOT (val BETWEEN 100 AND 200) (interp.pcap, frame 18163).
    let mut w = ProgramWriter::new();
    let accept = w.new_label();
    let refuse = w.new_label();
    w.test_column_value(ColumnCmp::Gt, NullRule::GoOn, 1, &int(100), accept);
    w.test_column_value(ColumnCmp::Lt, NullRule::GoOn, 1, &int(200), accept);
    w.jump(refuse);
    w.place(accept);
    w.accept();
    w.place(refuse);
    w.refuse(IC_REFUSE_NO_ROW);
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0007_40d7,
        0x0001_0004,
        0x0000_0064,
        0x0004_20d7,
        0x0001_0004,
        0x0000_00c8,
        0x0002_0009,
        0x0000_0012,
        0x0272_0013
      ]
    );
    // WHERE val = 10 OR name = 'r3' (frame 19005).
    w.clear();
    let accept = w.new_label();
    w.test_column_value(ColumnCmp::Eq, NullRule::GoOn, 1, &int(10), accept);
    w.test_column_value(ColumnCmp::Eq, NullRule::GoOn, 2, b"\x02r3", accept);
    w.refuse(IC_REFUSE_NO_ROW);
    w.place(accept);
    w.accept();
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0007_00d7,
        0x0001_0004,
        0x0000_000a,
        0x0004_00d7,
        0x0002_0003,
        0x0033_7202,
        0x0272_0013,
        0x0000_0012
      ]
    );
  }

  #[test]
  fn the_counter_adds_and_writes_back() {
    // AUTO_INCREMENT taking 512 values (interp.pcap, frame 26536).
    let mut w = ProgramWriter::new();
    w.read_column(6, 1);
    w.set_64(7, 512);
    w.add(7, 6, 7);
    w.write_column(1, 7);
    w.accept();
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0001_0181,
        0x0000_01c6,
        0x0000_0200,
        0x0000_0000,
        0x0007_0f87,
        0x0001_01c2,
        0x0000_0012
      ]
    );
  }

  #[test]
  fn the_counter_moves_up_only() {
    // An explicit value of 100000 above the counter (frame 28361):
    // skip the write when the new value is not above the counter's.
    let mut w = ProgramWriter::new();
    let done = w.new_label();
    w.set_64(1, 100_001);
    w.read_column(2, 1);
    w.jump_if(RegCmp::Le, 1, 2, done);
    w.write_column(1, 1);
    w.accept();
    w.place(done);
    w.accept();
    w.accept();
    w.finish().expect("finish");
    assert_eq!(
      w.words(),
      [
        0x0000_0046,
        0x0001_86a1,
        0x0000_0000,
        0x0001_0081,
        0x0003_044f,
        0x0001_0042,
        0x0000_0012,
        0x0000_0012,
        0x0000_0012
      ]
    );
  }

  #[test]
  fn an_input_is_read_into_a_register_and_sent_before_the_reads() {
    let mut w = ProgramWriter::new();
    w.read_input(3, 15);
    w.finish().expect("finish");
    // 121 is 57 with bit 15; R1 in bits 6-8, the input in the field.
    assert_eq!(w.words(), [0x000f_8000 | 57 | (3 << 6)]);
    w.clear();
    w.read_input(0, 16);
    let e = w.finish().expect_err("input 16");
    assert_eq!(e.code, err::IC_ERROR_BAD_PROGRAM);
    let mut out = vec![7];
    let mut values = [0i64; 16];
    values[0] = 512;
    values[2] = -1;
    values[3] = 99;
    inputs_into(0b101, &values, &mut out);
    assert_eq!(
      out,
      [
        7,
        0xffff_0007,
        0xffa0_0008,
        512,
        0,
        0xffa2_0008,
        0xffff_ffff,
        0xffff_ffff
      ]
    );
    out.clear();
    inputs_into(0, &values, &mut out);
    assert!(out.is_empty());
  }

  #[test]
  fn instructions_above_63_set_bit_15() {
    let mut w = ProgramWriter::new();
    let end = w.new_label();
    w.add_imm(3, 1, 7);
    w.subtract_imm(5, 4, 0xFFFF);
    w.set_16(2, 0x1234);
    w.set_32(0, 0xDEAD_BEEF);
    w.set_null(7);
    w.subtract(1, 2, 3);
    w.jump_if_null(4, end);
    w.jump_if_value(5, end);
    w.place(end);
    w.accept_last();
    w.finish().expect("finish");
    // 71 is 7 with bit 15; R1 in bits 6-8, R3 in 12-14, the field.
    assert_eq!(w.words()[0], 0x0007_8000 | 7 | (1 << 6) | (3 << 12));
    assert_eq!(w.words()[1], 0xFFFF_8000 | 8 | (4 << 6) | (5 << 12));
    assert_eq!(w.words()[2], 0x1234_0000 | 4 | (2 << 6));
    assert_eq!(w.words()[3], 5);
    assert_eq!(w.words()[4], 0xDEAD_BEEF);
    assert_eq!(w.words()[5], 3 | (7 << 6));
    // R4 = R1 - R2: 1 = 2 - 3.
    assert_eq!(w.words()[6], 8 | (2 << 6) | (3 << 9) | (1 << 16));
    // Both jumps go to the label at word 9.
    assert_eq!(w.words()[7], (2 << 16) | 10 | (4 << 6));
    assert_eq!(w.words()[8], (1 << 16) | 11 | (5 << 6));
    assert_eq!(w.words()[9], 22);
  }

  #[test]
  fn what_cannot_be_encoded_is_refused_at_the_finish() {
    let mut w = ProgramWriter::new();
    w.read_column(8, 1);
    w.accept();
    let e = w.finish().expect_err("ninth register");
    assert_eq!(e.code, err::IC_ERROR_CONDITION_TOO_COMPLEX);
    w.clear();
    w.read_column(0, 0x10000);
    let e = w.finish().expect_err("column id");
    assert_eq!(e.code, err::IC_ERROR_NO_SUCH_FIELD);
    w.clear();
    let to = w.new_label();
    let long = vec![0u8; 0x10000];
    w.test_column_value(ColumnCmp::Eq, NullRule::Jump, 1, &long, to);
    w.place(to);
    let e = w.finish().expect_err("value");
    assert_eq!(e.code, err::IC_ERROR_VALUE_TOO_LONG);
    // A label never placed, or placed twice, is the writer's caller's
    // mistake.
    w.clear();
    let to = w.new_label();
    w.jump(to);
    let e = w.finish().expect_err("unplaced");
    assert_eq!(e.code, err::IC_ERROR_INCONSISTENT_DATA);
    w.clear();
    let to = w.new_label();
    w.place(to);
    w.place(to);
    let e = w.finish().expect_err("placed twice");
    assert_eq!(e.code, err::IC_ERROR_INCONSISTENT_DATA);
    // A jump backwards, which no program of ours makes.
    w.clear();
    let to = w.new_label();
    w.place(to);
    w.jump(to);
    let e = w.finish().expect_err("backwards");
    assert_eq!(e.code, err::IC_ERROR_INCONSISTENT_DATA);
    // A jump further than bits 16-30 reach.
    w.clear();
    let to = w.new_label();
    w.jump(to);
    for _ in 0..0x8000 {
      w.accept();
    }
    w.place(to);
    let e = w.finish().expect_err("far");
    assert_eq!(e.code, err::IC_ERROR_RECORD_SIZE_TOO_BIG);
  }

  #[test]
  fn the_counters_section_has_its_final_read() {
    // Sizes [0, 7, 0, 1, 0] and the final read of column 1 (frame
    // 26536).
    let program = [1, 2, 3, 4, 5, 6, 7];
    let mut out = vec![99];
    column_parts_into(&[], &program, &[], &[0x0001_0000], &[], &mut out);
    assert_eq!(out, [0, 7, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 0x0001_0000]);
    column_parts_into(&[10, 11], &[12], &[13], &[14], &[15, 16], &mut out);
    assert_eq!(out, [2, 1, 1, 1, 2, 10, 11, 12, 13, 14, 15, 16]);
  }
}
