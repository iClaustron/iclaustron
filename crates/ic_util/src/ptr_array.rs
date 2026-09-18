// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! The object map (`IC_DYNAMIC_PTR_ARRAY`,
//! `legacy-c/util/ic_dyn_array.c`): objects in, 32-bit ids out, and the
//! object back from the id.
//!
//! Every NDB signal the API sends carries an id in its first data word,
//! and the reply carries it back. The id has to fit in 32 bits, must not
//! be a raw pointer (the data node would then be able to make us
//! dereference anything), and a reply that arrives after its object is
//! gone must be recognised and dropped rather than delivered to whatever
//! now occupies the slot.
//!
//! An id is therefore an index plus a generation, packed into one word:
//! the low 24 bits index the slot, the high 8 bits count how many times
//! that slot has been reused. A stale reply names a generation that no
//! longer matches and is rejected.

use ic_port::err;
use ic_port::IcError;

/// Highest slot index, so also the largest number of live objects.
pub const PTR_ARRAY_MAX_ENTRIES: u32 = 0x00FF_FFFF;

const INDEX_MASK: u32 = 0x00FF_FFFF;
const GENERATION_SHIFT: u32 = 24;
const GENERATION_MASK: u32 = 0xFF;

/// The id of an object in a [`PtrArray`]: what travels in a signal.
///
/// A valid id is never 0, because generations start at 1, so 0 can be
/// used as "no object" the way `RNIL` is in the NDB protocol.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PtrId(u32);

impl PtrId {
  /// The id as the 32-bit word that goes into a signal.
  pub fn as_u32(&self) -> u32 {
    self.0
  }

  /// An id from a word received in a signal. Whether it still names a
  /// live object is answered by [`PtrArray::get`].
  pub fn from_u32(word: u32) -> PtrId {
    PtrId(word)
  }

  /// The slot this id names.
  pub fn index(&self) -> u32 {
    self.0 & INDEX_MASK
  }

  /// The generation this id was issued in.
  pub fn generation(&self) -> u32 {
    (self.0 >> GENERATION_SHIFT) & GENERATION_MASK
  }

  /// True for an id that cannot name any object.
  pub fn is_none(&self) -> bool {
    self.0 == 0
  }
}

struct Slot<T> {
  generation: u32,
  value: Option<T>,
}

/// A table of objects addressed by a 32-bit id.
///
/// Rust notes for C readers: `PtrArray<T>` holds objects of one type `T`,
/// the way `Vec<T>` does. The C version stored `void*` and the caller
/// remembered what it had put in; here the compiler remembers.
pub struct PtrArray<T> {
  slots: Vec<Slot<T>>,
  free_list: Vec<u32>,
  num_entries: u32,
}

impl<T> PtrArray<T> {
  /// An empty table.
  pub fn new() -> PtrArray<T> {
    PtrArray {
      slots: Vec::new(),
      free_list: Vec::new(),
      num_entries: 0,
    }
  }

  /// An empty table with room for `capacity` objects reserved.
  pub fn with_capacity(capacity: usize) -> PtrArray<T> {
    PtrArray {
      slots: Vec::with_capacity(capacity),
      free_list: Vec::new(),
      num_entries: 0,
    }
  }

  /// Store an object and return the id to send in a signal
  /// (`ic_insert_ptr`).
  pub fn insert(&mut self, value: T) -> Result<PtrId, IcError> {
    if let Some(index) = self.free_list.pop() {
      let slot = &mut self.slots[index as usize];
      slot.value = Some(value);
      self.num_entries += 1;
      return Ok(PtrId(index | (slot.generation << GENERATION_SHIFT)));
    }
    let index = self.slots.len() as u32;
    if index > PTR_ARRAY_MAX_ENTRIES {
      return Err(IcError::new(err::IC_ERROR_PTR_ARRAY_INDEX_OUT_OF_BOUND));
    }
    /* Generations start at 1 so that a valid id is never the word 0. */
    self.slots.push(Slot {
      generation: 1,
      value: Some(value),
    });
    self.num_entries += 1;
    Ok(PtrId(index | (1 << GENERATION_SHIFT)))
  }

  /// The object an id names, or `None` if the id is stale or was never
  /// issued (`ic_get_ptr`).
  pub fn get(&self, id: PtrId) -> Option<&T> {
    let slot = self.slots.get(id.index() as usize)?;
    if slot.generation != id.generation() {
      return None;
    }
    slot.value.as_ref()
  }

  /// The object an id names, for modification.
  pub fn get_mut(&mut self, id: PtrId) -> Option<&mut T> {
    let slot = self.slots.get_mut(id.index() as usize)?;
    if slot.generation != id.generation() {
      return None;
    }
    slot.value.as_mut()
  }

  /// Take the object out and free the slot for reuse
  /// (`ic_remove_ptr`). A later reply carrying the old id is rejected.
  pub fn remove(&mut self, id: PtrId) -> Option<T> {
    let index = id.index() as usize;
    let slot = self.slots.get_mut(index)?;
    if slot.generation != id.generation() {
      return None;
    }
    let value = slot.value.take()?;
    /* Bump the generation so ids already in flight stop matching. It
    wraps at 8 bits; a slot reused 256 times with a reply still
    outstanding is not a case worth more bits. */
    slot.generation = (slot.generation + 1) & GENERATION_MASK;
    if slot.generation == 0 {
      slot.generation = 1;
    }
    self.free_list.push(index as u32);
    self.num_entries -= 1;
    Some(value)
  }

  /// Number of objects currently stored.
  pub fn len(&self) -> u32 {
    self.num_entries
  }

  /// True if no object is stored.
  pub fn is_empty(&self) -> bool {
    self.num_entries == 0
  }

  /// Highest slot ever used (`ic_get_max_index`).
  pub fn max_index(&self) -> u32 {
    self.slots.len() as u32
  }

  /// Drop every object.
  pub fn clear(&mut self) {
    self.slots.clear();
    self.free_list.clear();
    self.num_entries = 0;
  }
}

impl<T> Default for PtrArray<T> {
  fn default() -> PtrArray<T> {
    PtrArray::new()
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn insert_get_remove() {
    let mut a: PtrArray<String> = PtrArray::new();
    assert!(a.is_empty());
    let one = a.insert("one".to_string()).expect("insert");
    let two = a.insert("two".to_string()).expect("insert");
    assert_eq!(a.len(), 2);
    assert_eq!(a.get(one).map(|s| s.as_str()), Some("one"));
    assert_eq!(a.get(two).map(|s| s.as_str()), Some("two"));
    if let Some(s) = a.get_mut(one) {
      s.push_str(" changed");
    }
    assert_eq!(a.get(one).map(|s| s.as_str()), Some("one changed"));
    let taken = a.remove(one).expect("remove");
    assert_eq!(taken, "one changed");
    assert_eq!(a.get(one), None);
    assert_eq!(a.len(), 1);
  }

  #[test]
  fn a_stale_id_is_rejected() {
    let mut a: PtrArray<u32> = PtrArray::new();
    let old = a.insert(1).expect("insert");
    a.remove(old).expect("remove");
    /* The slot is reused; the new id differs from the old one. */
    let new = a.insert(2).expect("insert");
    assert_eq!(old.index(), new.index());
    assert_ne!(old.as_u32(), new.as_u32());
    assert_eq!(a.get(old), None);
    assert_eq!(a.get(new), Some(&2));
    assert_eq!(a.remove(old), None);
  }

  #[test]
  fn ids_survive_the_wire() {
    let mut a: PtrArray<u32> = PtrArray::new();
    let id = a.insert(42).expect("insert");
    let word = id.as_u32();
    assert_ne!(word, 0);
    let back = PtrId::from_u32(word);
    assert_eq!(a.get(back), Some(&42));
    assert!(PtrId::from_u32(0).is_none());
    assert_eq!(a.get(PtrId::from_u32(0)), None);
    assert_eq!(a.get(PtrId::from_u32(0xFFFF_FFFF)), None);
  }

  /* The C unit test, test type 4: insert many, remove some, check that
  what is left still reads back and that slots are reused. */
  #[test]
  fn many_inserts_and_removes() {
    let mut a: PtrArray<u32> = PtrArray::new();
    let mut ids: Vec<PtrId> = Vec::new();
    let mut i: u32 = 0;
    while i < 1000 {
      ids.push(a.insert(i).expect("insert"));
      i += 1;
    }
    assert_eq!(a.len(), 1000);
    assert_eq!(a.max_index(), 1000);
    i = 0;
    while i < 1000 {
      assert_eq!(a.get(ids[i as usize]), Some(&i));
      i += 1;
    }
    /* Remove every second object. */
    i = 0;
    while i < 1000 {
      assert_eq!(a.remove(ids[i as usize]), Some(i));
      i += 2;
    }
    assert_eq!(a.len(), 500);
    i = 1;
    while i < 1000 {
      assert_eq!(a.get(ids[i as usize]), Some(&i));
      i += 2;
    }
    /* The freed slots are reused rather than the table growing. */
    i = 0;
    while i < 500 {
      a.insert(9999).expect("insert");
      i += 1;
    }
    assert_eq!(a.max_index(), 1000);
    assert_eq!(a.len(), 1000);
    a.clear();
    assert!(a.is_empty());
  }
}
