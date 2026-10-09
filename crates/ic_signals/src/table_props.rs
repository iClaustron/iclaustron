// Copyright (c) 2007-2015 iClaustron AB.
// Copyright (c) 2026 Hopsworks and/or its affiliates.
// Licensed under the MIT License. See LICENSE in the repository root.

//! What a table looks like, as the dictionary describes it
//! (`legacy-c/include/ic_apid_dict_signals.h` for the C's version of
//! the keys).
//!
//! A table description is a run of properties
//! ([`prop_stream`](crate::prop_stream)): first the table's
//! own, then each column's, from its name to an end marker. Three rules
//! make it readable:
//!
//! - **The table part ends at the first column name**, not at the table
//!   end marker, which a reader treats as a key it does not know.
//! - **Only values that differ from the default are sent**, so the
//!   defaults are part of the protocol. They are the ones the data node
//!   code initialises its structures with. Several comments beside the
//!   key numbers name other defaults and are out of date; the code is
//!   what counts.
//! - **Keys a reader does not know are passed over.**
//!
//! A column's size is not taken from what is sent but worked out from
//! its type, length, precision and scale, as the reference does.
//!
//! Verify: chapter 05 §17.8.

use ic_port::err;
use ic_port::IcError;

use crate::prop_stream::Property;
use crate::prop_stream::PropertyReader;
use crate::prop_stream::PropertyValue;

/// The dictionary's "no such thing" value.
pub const IC_NULL_PTR: u32 = 0xFFFF_FF00;

/// The bits of a schema version that count offline changes, 0 to 23.
/// Bits 24 to 31 count online changes. Operations are checked against
/// the lower bits only. Verify: chapter 05 §17.8.
pub const IC_TABLE_VERSION_LOWER_MASK: u32 = 0x00FF_FFFF;
/// Where the count of online changes starts.
pub const IC_TABLE_VERSION_UPPER_SHIFT: u32 = 24;

// ---- Hash map keys ----

/// A hash map's name, such as `DEFAULT-HASHMAP-3840-8`.
pub const IC_HMP_NAME: u16 = 1;
/// The size of the bucket array **in bytes**, two per bucket.
pub const IC_HMP_BUCKETS: u16 = 2;
/// The bucket array: one 16-bit fragment number per bucket.
pub const IC_HMP_VALUES: u16 = 3;
// A hash map's id and version use the table keys of the same meaning,
// `IC_TP_HASH_MAP_ID` and `IC_TP_HASH_MAP_VERSION`.

// ---- Table keys ----

/// The table's internal name, `database/schema/table`.
pub const IC_TP_TABLE_NAME: u16 = 1;
/// The table's id.
pub const IC_TP_TABLE_ID: u16 = 2;
/// The table's version, which changes with every alteration.
pub const IC_TP_TABLE_VERSION: u16 = 3;
/// Whether the table is logged to disk.
pub const IC_TP_LOGGED: u16 = 4;
/// How many columns make up the primary key.
pub const IC_TP_KEY_COLUMN_COUNT: u16 = 5;
/// How many columns the table has.
pub const IC_TP_COLUMN_COUNT: u16 = 6;
/// How many columns may be NULL.
pub const IC_TP_NULLABLE_COUNT: u16 = 7;
/// How many columns have a variable size.
pub const IC_TP_VARSIZE_COUNT: u16 = 8;
/// The hash table's K value.
pub const IC_TP_LINEAR_HASH_K: u16 = 9;
/// The hash table's lowest load factor, in percent.
pub const IC_TP_MIN_FILL: u16 = 10;
/// The hash table's highest load factor, in percent.
pub const IC_TP_MAX_FILL: u16 = 11;
/// The primary key's size in words.
pub const IC_TP_KEY_LENGTH: u16 = 12;
/// How the table is split into fragments.
pub const IC_TP_FRAGMENT_KIND: u16 = 13;
/// What kind of object this is: a table, or one kind of index.
pub const IC_TP_TABLE_KIND: u16 = 18;
/// For an index, the name of the table it indexes.
pub const IC_TP_BASE_TABLE_NAME: u16 = 19;
/// For an index, the id of the table it indexes.
pub const IC_TP_BASE_TABLE_ID: u16 = 20;
/// For an index, whether it is online.
pub const IC_TP_INDEX_STATE: u16 = 21;
/// The index state of an index that can be used: built, and not being
/// dropped. Verify: chapter 05 §17.8.
pub const IC_INDEX_STATE_ONLINE: u32 = 4;
/// The MySQL server's own description of the table, in its older form.
pub const IC_TP_LEGACY_FRM: u16 = 27;
/// Whether the table lives only in memory, without being restored.
pub const IC_TP_TEMPORARY: u16 = 28;
/// Whether every row has a variable-size part.
pub const IC_TP_FORCE_VARSIZE_PART: u16 = 29;
/// The MySQL server's own description of the table, in its newer form.
pub const IC_TP_SQL_METADATA: u16 = 30;
/// How many fragments the table has for each node or each thread that
/// runs operations on fragments.
pub const IC_TP_PARTITION_BALANCE: u16 = 127;
/// How many fragments the table has.
pub const IC_TP_FRAGMENT_COUNT: u16 = 128;
/// Bytes of replica data that follow.
pub const IC_TP_REPLICA_MAP_LEN: u16 = 137;
/// Which nodes hold each fragment: 16-bit values, big-endian, the
/// replica count and the fragment count, then per fragment its log
/// part and the node of each replica. Sent only in answer to
/// `TABLE_DESC_REQ`, where the dictionary asks the distribution block
/// for it. The reference passes over the log part.
/// Verify: chapter 05 §17.8.
pub const IC_TP_REPLICA_MAP: u16 = 138;
/// The low word of the most rows the table is sized for.
pub const IC_TP_MAX_ROWS_LOW: u16 = 139;
/// The high word of the most rows the table is sized for.
pub const IC_TP_MAX_ROWS_HIGH: u16 = 140;
/// The low word of the fewest rows the table is sized for.
pub const IC_TP_MIN_ROWS_LOW: u16 = 143;
/// The high word of the fewest rows the table is sized for.
pub const IC_TP_MIN_ROWS_HIGH: u16 = 144;
/// Whether every row records the global checkpoint it was changed in.
pub const IC_TP_ROW_GCI: u16 = 150;
/// Whether every row carries a checksum.
pub const IC_TP_ROW_CHECKSUM: u16 = 151;
/// Whether the table is usable in single user mode.
pub const IC_TP_SINGLE_USER: u16 = 152;
/// The id of the hash map that sends key hashes to fragments.
pub const IC_TP_HASH_MAP_ID: u16 = 153;
/// The version of that hash map.
pub const IC_TP_HASH_MAP_VERSION: u16 = 154;
/// Whether the table is kept in memory or on disk by default.
pub const IC_TP_STORAGE_KIND: u16 = 155;
/// Extra bits of global checkpoint kept per row.
pub const IC_TP_EXTRA_GCI_BITS: u16 = 156;
/// Extra bits of author kept per row.
pub const IC_TP_EXTRA_AUTHOR_BITS: u16 = 157;
/// Whether reads may be served by a backup replica.
pub const IC_TP_READ_BACKUP: u16 = 158;
/// Whether every data node holds the whole table.
pub const IC_TP_FULLY_REPLICATED: u16 = 159;
/// How many partitions the table has.
pub const IC_TP_PARTITION_COUNT: u16 = 160;
/// Which hash function the table's partitioning uses.
pub const IC_TP_HASH_FUNCTION: u16 = 163;
/// RonDB: how long a row lives, in seconds.
pub const IC_TP_TTL_SECONDS: u16 = 164;
/// RonDB: the column the lifetime is counted from.
pub const IC_TP_TTL_COLUMN: u16 = 165;
/// RonDB: key columns hashed to choose a partition.
pub const IC_TP_FANOUT_BASE_KEYS: u16 = 166;
/// RonDB: further key columns hashed within it.
pub const IC_TP_FANOUT_DETAIL_KEYS: u16 = 167;
/// RonDB: how widely rows of one base key are spread.
pub const IC_TP_FANOUT: u16 = 168;
/// RonDB: the size of a ring buffer table.
pub const IC_TP_RING_SIZE: u16 = 169;
/// RonDB: the ring buffer's index column.
pub const IC_TP_RING_INDEX_COLUMN: u16 = 170;
/// RonDB: the ring buffer's metadata column.
pub const IC_TP_RING_META_COLUMN: u16 = 171;

// ---- Column keys ----

/// A column's name. Also where the table's own properties end.
pub const IC_TP_COLUMN_NAME: u16 = 1000;
/// A column's id.
pub const IC_TP_COLUMN_ID: u16 = 1001;
/// How many bits one element of the column is, as a power of two.
pub const IC_TP_COLUMN_ELEMENT_SIZE: u16 = 1003;
/// How many elements the column has.
pub const IC_TP_COLUMN_ARRAY_LEN: u16 = 1005;
/// Whether the column is part of the primary key.
pub const IC_TP_COLUMN_IN_KEY: u16 = 1006;
/// Whether the column is kept in memory or on disk.
pub const IC_TP_COLUMN_STORAGE: u16 = 1007;
/// Whether the column may be NULL.
pub const IC_TP_COLUMN_NULLABLE: u16 = 1008;
/// Whether the column is stored in the dynamic part of the row.
pub const IC_TP_COLUMN_DYNAMIC: u16 = 1009;
/// Whether the column is part of the distribution key.
pub const IC_TP_COLUMN_IN_DIST_KEY: u16 = 1010;
/// The column's type, one of the `IC_TYPE_*` values.
pub const IC_TP_COLUMN_TYPE: u16 = 1013;
/// Precision in the low half; the character set in the high half.
pub const IC_TP_COLUMN_PRECISION: u16 = 1014;
/// Scale, for decimals.
pub const IC_TP_COLUMN_SCALE: u16 = 1015;
/// Length, as declared.
pub const IC_TP_COLUMN_LENGTH: u16 = 1016;
/// Whether the column is an auto-increment column.
pub const IC_TP_COLUMN_AUTO_INC: u16 = 1017;
/// How the column's length is stored: fixed, or one or two length bytes.
pub const IC_TP_COLUMN_ARRAY_KIND: u16 = 1019;
/// The column's default value.
pub const IC_TP_COLUMN_DEFAULT: u16 = 1021;
/// Where one column's properties end.
pub const IC_TP_COLUMN_END: u16 = 1999;

// ---- Column types, numbered as the data nodes number them ----

/// No type.
pub const IC_TYPE_UNDEFINED: u32 = 0;
/// One byte, signed.
pub const IC_TYPE_TINYINT: u32 = 1;
/// One byte, unsigned.
pub const IC_TYPE_TINYUNSIGNED: u32 = 2;
/// Two bytes, signed.
pub const IC_TYPE_SMALLINT: u32 = 3;
/// Two bytes, unsigned.
pub const IC_TYPE_SMALLUNSIGNED: u32 = 4;
/// Three bytes, signed.
pub const IC_TYPE_MEDIUMINT: u32 = 5;
/// Three bytes, unsigned.
pub const IC_TYPE_MEDIUMUNSIGNED: u32 = 6;
/// Four bytes, signed.
pub const IC_TYPE_INT: u32 = 7;
/// Four bytes, unsigned.
pub const IC_TYPE_UNSIGNED: u32 = 8;
/// Eight bytes, signed.
pub const IC_TYPE_BIGINT: u32 = 9;
/// Eight bytes, unsigned.
pub const IC_TYPE_BIGUNSIGNED: u32 = 10;
/// Four-byte floating point.
pub const IC_TYPE_FLOAT: u32 = 11;
/// Eight-byte floating point.
pub const IC_TYPE_DOUBLE: u32 = 12;
/// The decimal of MySQL before 5.0, signed.
pub const IC_TYPE_OLDDECIMAL: u32 = 13;
/// Fixed-size characters.
pub const IC_TYPE_CHAR: u32 = 14;
/// Characters with one length byte.
pub const IC_TYPE_VARCHAR: u32 = 15;
/// Fixed-size bytes.
pub const IC_TYPE_BINARY: u32 = 16;
/// Bytes with one length byte.
pub const IC_TYPE_VARBINARY: u32 = 17;
/// Date and time in eight bytes.
pub const IC_TYPE_DATETIME: u32 = 18;
/// Date in three bytes.
pub const IC_TYPE_DATE: u32 = 19;
/// Large bytes, kept partly in another table.
pub const IC_TYPE_BLOB: u32 = 20;
/// Large text, kept partly in another table.
pub const IC_TYPE_TEXT: u32 = 21;
/// A bit field.
pub const IC_TYPE_BIT: u32 = 22;
/// Characters with two length bytes.
pub const IC_TYPE_LONGVARCHAR: u32 = 23;
/// Bytes with two length bytes.
pub const IC_TYPE_LONGVARBINARY: u32 = 24;
/// Time in three bytes.
pub const IC_TYPE_TIME: u32 = 25;
/// Year in one byte.
pub const IC_TYPE_YEAR: u32 = 26;
/// Timestamp in four bytes.
pub const IC_TYPE_TIMESTAMP: u32 = 27;
/// The decimal of MySQL before 5.0, unsigned.
pub const IC_TYPE_OLDDECIMALUNSIGNED: u32 = 28;
/// Packed decimal, signed.
pub const IC_TYPE_DECIMAL: u32 = 29;
/// Packed decimal, unsigned.
pub const IC_TYPE_DECIMALUNSIGNED: u32 = 30;
/// Time with fractional seconds.
pub const IC_TYPE_TIME2: u32 = 31;
/// Date and time with fractional seconds.
pub const IC_TYPE_DATETIME2: u32 = 32;
/// Timestamp with fractional seconds.
pub const IC_TYPE_TIMESTAMP2: u32 = 33;

// ---- How a column's length is stored ----

/// Fixed size, no length bytes.
pub const IC_ARRAY_KIND_FIXED: u32 = 0;
/// One length byte before the data.
pub const IC_ARRAY_KIND_SHORT_VARSIZE: u32 = 1;
/// Two length bytes, little-endian, before the data.
pub const IC_ARRAY_KIND_LONG_VARSIZE: u32 = 2;

// ---- Where a column or table is kept ----

/// In memory.
pub const IC_STORAGE_MEMORY: u32 = 0;
/// On disk.
pub const IC_STORAGE_DISK: u32 = 1;
/// Not set; the table's own setting applies.
pub const IC_STORAGE_DEFAULT: u32 = 2;

// ---- What kind of object ----

/// A table.
pub const IC_OBJECT_KIND_TABLE: u32 = 2;
/// A unique hash index.
pub const IC_OBJECT_KIND_UNIQUE_INDEX: u32 = 3;
/// An ordered index.
pub const IC_OBJECT_KIND_ORDERED_INDEX: u32 = 6;

/// Rows are placed through a hash map, the default.
pub const IC_FRAGMENT_KIND_HASH_MAP: u32 = 9;

/// The default partition balance: one fragment per replica for each
/// thread that runs operations on fragments.
pub const IC_PARTITION_BALANCE_FOR_RP_BY_LDM: u32 = !1;

// A column element is sized as a power of two bits; these are the
// powers the types use. A packed row places a value by it: bits packed
// together, bytes where they fall, words on a word.
/// Element size of a bit column: single bits.
pub const IC_COLUMN_SIZE_BIT: u32 = 0;
/// Element size of byte types.
pub const IC_COLUMN_SIZE_8: u32 = 3;
/// Element size of 16-bit integers.
pub const IC_COLUMN_SIZE_16: u32 = 4;
/// Element size of 32-bit integers and floats.
pub const IC_COLUMN_SIZE_32: u32 = 5;
/// Element size of 64-bit integers and doubles.
pub const IC_COLUMN_SIZE_64: u32 = 6;

/// The head of a large-object column, in words, before its inline data.
const IC_BLOB_V1_HEAD_WORDS: u32 = 2;
const IC_BLOB_V2_HEAD_WORDS: u32 = 4;

/// The largest decimal precision, and the first scale that means "not
/// specified", as MySQL defines them. Verify: chapter 05 §17.8.
const IC_DECIMAL_MAX_PRECISION: u32 = 65;
const IC_DECIMAL_NOT_SPECIFIED: u32 = 31;

/// One column.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ColumnProps {
  /// Its name.
  pub name: String,
  /// Its id, which is how signals name it.
  pub column_id: u32,
  /// Its type, one of the `IC_TYPE_*` values.
  pub type_code: u32,
  /// Its precision, for types that have one.
  pub precision: u32,
  /// Its scale, for decimals.
  pub scale: u32,
  /// Its length, as declared.
  pub length: u32,
  /// Its character set number, zero for types without one.
  pub charset: u32,
  /// How its length is stored, one of the `IC_ARRAY_TYPE_*` values.
  pub array_kind: u32,
  /// One element's size, as a power of two bits. Worked out from the
  /// type, not taken from what is sent.
  pub size_bits_log2: u32,
  /// How many elements. Worked out from the type.
  pub array_len: u32,
  /// Part of the primary key.
  pub primary_key: bool,
  /// Part of the distribution key.
  pub distribution_key: bool,
  /// May be NULL.
  pub nullable: bool,
  /// An auto-increment column.
  pub auto_increment: bool,
  /// Stored in the dynamic part of the row.
  pub dynamic: bool,
  /// Kept in memory or on disk, one of the `IC_STORAGE_*` values.
  pub storage_kind: u32,
  /// The default value's bytes, as the data node stores the value, which
  /// on the little-endian machines we run on is little-endian. Empty
  /// when the column has no default, NULL included.
  pub default_value: Vec<u8>,
}

impl ColumnProps {
  /// A column with nothing sent about it yet: the defaults.
  fn with_defaults(name: String) -> ColumnProps {
    ColumnProps {
      name,
      column_id: 0xFFFF,
      type_code: IC_TYPE_UNSIGNED,
      precision: 0,
      scale: 0,
      length: 0,
      charset: 0,
      array_kind: IC_ARRAY_KIND_FIXED,
      size_bits_log2: IC_COLUMN_SIZE_32,
      array_len: 1,
      primary_key: false,
      distribution_key: false,
      nullable: false,
      auto_increment: false,
      dynamic: false,
      storage_kind: IC_STORAGE_MEMORY,
      default_value: Vec::new(),
    }
  }

  fn apply(&mut self, property: &Property) {
    let value = property.as_u32();
    match property.key {
      IC_TP_COLUMN_ID => self.column_id = value,
      IC_TP_COLUMN_ELEMENT_SIZE => self.size_bits_log2 = value,
      IC_TP_COLUMN_ARRAY_LEN => self.array_len = value,
      IC_TP_COLUMN_IN_KEY => self.primary_key = value != 0,
      IC_TP_COLUMN_STORAGE => self.storage_kind = value,
      IC_TP_COLUMN_NULLABLE => self.nullable = value != 0,
      IC_TP_COLUMN_DYNAMIC => self.dynamic = value != 0,
      IC_TP_COLUMN_IN_DIST_KEY => self.distribution_key = value != 0,
      IC_TP_COLUMN_TYPE => self.type_code = value,
      IC_TP_COLUMN_PRECISION => {
        // The character set rides in the high half.
        self.precision = value & 0xFFFF;
        self.charset = value >> 16;
      }
      IC_TP_COLUMN_SCALE => self.scale = value,
      IC_TP_COLUMN_LENGTH => self.length = value,
      IC_TP_COLUMN_AUTO_INC => self.auto_increment = value != 0,
      IC_TP_COLUMN_ARRAY_KIND => self.array_kind = value,
      IC_TP_COLUMN_DEFAULT => {
        if let PropertyValue::Binary(bytes) = &property.value {
          self.default_value = default_value_of(bytes);
        }
      }
      // Anything else is not needed, or not known: pass it over.
      _ => {}
    }
  }

  /// Work out the element size and count from the type, as the
  /// reference does. False for a type this cannot size.
  fn size_from_type(&mut self) -> bool {
    let n = self.length;
    let (bits, count) = match self.type_code {
      IC_TYPE_TINYINT | IC_TYPE_TINYUNSIGNED => (IC_COLUMN_SIZE_8, n),
      IC_TYPE_SMALLINT | IC_TYPE_SMALLUNSIGNED => (IC_COLUMN_SIZE_16, n),
      IC_TYPE_MEDIUMINT | IC_TYPE_MEDIUMUNSIGNED => (IC_COLUMN_SIZE_8, 3 * n),
      IC_TYPE_INT | IC_TYPE_UNSIGNED | IC_TYPE_FLOAT => (IC_COLUMN_SIZE_32, n),
      IC_TYPE_BIGINT | IC_TYPE_BIGUNSIGNED | IC_TYPE_DOUBLE => {
        (IC_COLUMN_SIZE_64, n)
      }
      IC_TYPE_OLDDECIMAL => {
        let point = if self.scale > 0 { 1 } else { 0 };
        (IC_COLUMN_SIZE_8, (1 + self.precision + point) * n)
      }
      IC_TYPE_OLDDECIMALUNSIGNED => {
        let point = if self.scale > 0 { 1 } else { 0 };
        (IC_COLUMN_SIZE_8, (self.precision + point) * n)
      }
      IC_TYPE_DECIMAL | IC_TYPE_DECIMALUNSIGNED => {
        if self.precision > IC_DECIMAL_MAX_PRECISION
          || self.scale >= IC_DECIMAL_NOT_SPECIFIED
          || self.scale > self.precision
          || self.precision == 0
        {
          return false;
        }
        let bytes = decimal_bin_size(self.precision, self.scale);
        (IC_COLUMN_SIZE_8, bytes * n)
      }
      IC_TYPE_CHAR | IC_TYPE_BINARY => (IC_COLUMN_SIZE_8, n),
      IC_TYPE_VARCHAR | IC_TYPE_VARBINARY => {
        if n > 0xFF {
          return false;
        }
        (IC_COLUMN_SIZE_8, n + 1)
      }
      IC_TYPE_LONGVARCHAR | IC_TYPE_LONGVARBINARY => {
        if n > 0xFFFF {
          return false;
        }
        (IC_COLUMN_SIZE_8, n + 2)
      }
      IC_TYPE_DATETIME => (IC_COLUMN_SIZE_8, 8 * n),
      IC_TYPE_DATE | IC_TYPE_TIME => (IC_COLUMN_SIZE_8, 3 * n),
      IC_TYPE_YEAR => (IC_COLUMN_SIZE_8, n),
      IC_TYPE_TIMESTAMP => (IC_COLUMN_SIZE_8, 4 * n),
      IC_TYPE_BLOB | IC_TYPE_TEXT => {
        let mut head = IC_BLOB_V2_HEAD_WORDS;
        if self.array_kind == IC_ARRAY_KIND_FIXED {
          head = IC_BLOB_V1_HEAD_WORDS;
        }
        (IC_COLUMN_SIZE_8, head * 4 + self.precision)
      }
      IC_TYPE_BIT => (IC_COLUMN_SIZE_BIT, n),
      // Fractional seconds take one byte per two digits, rounded up;
      // the reference writes it (1 + precision) / 2.
      IC_TYPE_TIME2 => (IC_COLUMN_SIZE_8, (3 + self.precision.div_ceil(2)) * n),
      IC_TYPE_DATETIME2 => {
        (IC_COLUMN_SIZE_8, (5 + self.precision.div_ceil(2)) * n)
      }
      IC_TYPE_TIMESTAMP2 => {
        (IC_COLUMN_SIZE_8, (4 + self.precision.div_ceil(2)) * n)
      }
      _ => return false,
    };
    self.size_bits_log2 = bits;
    self.array_len = count;
    true
  }

  /// The most bytes the column's value takes, length bytes included.
  /// A bit field is counted in whole words, as the data nodes store it.
  pub fn max_byte_size(&self) -> u32 {
    if self.size_bits_log2 == IC_COLUMN_SIZE_BIT {
      return 4 * self.array_len.div_ceil(32);
    }
    ((1 << self.size_bits_log2) / 8) * self.array_len
  }

  /// True for the types a character set applies to.
  pub fn is_char_type(&self) -> bool {
    self.type_code == IC_TYPE_CHAR
      || self.type_code == IC_TYPE_VARCHAR
      || self.type_code == IC_TYPE_LONGVARCHAR
      || self.type_code == IC_TYPE_TEXT
  }

  /// True for a blob column: BLOB and TEXT, as the dictionary also
  /// describes a MySQL server's JSON and GEOMETRY columns. The row holds
  /// a head and the first bytes; the rest is in rows of a parts table
  /// that the client, not the data node, keeps in step.
  pub fn is_blob(&self) -> bool {
    self.type_code == IC_TYPE_BLOB || self.type_code == IC_TYPE_TEXT
  }
}

/// One table, or one index, as the dictionary describes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TableProps {
  /// The internal name, `database/schema/table`.
  pub name: String,
  /// The id.
  pub table_id: u32,
  /// The version, which changes with every alteration: two counters in
  /// one word, see [`version_upper`](Self::version_upper) and
  /// [`version_lower`](Self::version_lower).
  pub table_version: u32,
  /// What kind of object, one of the `IC_OBJECT_KIND_*` values.
  pub table_kind: u32,
  /// Logged to disk.
  pub logged: bool,
  /// Kept only in memory, not restored after a restart.
  pub temporary: bool,
  /// Every row has a variable-size part.
  pub force_varsize_part: bool,
  /// Columns in the primary key.
  pub key_column_count: u32,
  /// Columns in all.
  pub column_count: u32,
  /// Columns that may be NULL.
  pub nullable_count: u32,
  /// Columns with a variable size.
  pub varsize_count: u32,
  /// The primary key's size in words.
  pub key_length: u32,
  /// The hash table's K value.
  pub linear_hash_k: u32,
  /// The hash table's lowest load factor, in percent.
  pub min_fill: u32,
  /// The hash table's highest load factor, in percent.
  pub max_fill: u32,
  /// Bytes of the MySQL server's own description of the table, which
  /// this library does not read.
  pub sql_metadata_len: u32,
  /// How the table is split into fragments.
  pub fragment_kind: u32,
  /// For an index, the table it indexes.
  pub base_table_name: String,
  /// For an index, the id of the table it indexes.
  pub base_table_id: u32,
  /// For an index, whether it is online.
  pub index_state: u32,
  /// How the fragment count was chosen.
  pub partition_balance: u32,
  /// Fragments in all.
  pub fragment_count: u32,
  /// Partitions in all.
  pub partition_count: u32,
  /// The hash map that sends key hashes to fragments.
  pub hash_map_id: u32,
  /// That hash map's version.
  pub hash_map_version: u32,
  /// In memory or on disk by default.
  pub storage_kind: u32,
  /// Reads may be served by a backup replica.
  pub read_backup: bool,
  /// Every data node holds the whole table.
  pub fully_replicated: bool,
  /// Every row records its global checkpoint.
  pub row_gci: bool,
  /// Every row carries a checksum.
  pub row_checksum: bool,
  /// Usable in single user mode.
  pub single_user: u32,
  /// The most rows the table is sized for.
  pub max_rows: u64,
  /// The fewest rows the table is sized for.
  pub min_rows: u64,
  /// Extra bits of global checkpoint per row.
  pub extra_gci_bits: u32,
  /// Extra bits of author per row.
  pub extra_author_bits: u32,
  /// Which hash function places rows.
  pub hash_function: u32,
  /// RonDB: a row's lifetime in seconds, or [`IC_NULL_PTR`].
  pub ttl_seconds: u32,
  /// RonDB: the column a lifetime counts from, or [`IC_NULL_PTR`].
  pub ttl_column: u32,
  /// RonDB: key columns hashed to choose a partition.
  pub fanout_base_keys: u32,
  /// RonDB: further key columns hashed within it.
  pub fanout_detail_keys: u32,
  /// RonDB: how widely rows of one base key are spread.
  pub fanout: u32,
  /// RonDB: a ring buffer table's size, or [`IC_NULL_PTR`].
  pub ring_size: u32,
  /// How many replicas each fragment has, or zero when no replica data
  /// was sent.
  pub replica_count: u32,
  /// The node of each replica of each fragment, `replica_count` per
  /// fragment in fragment order, the first of each the one the
  /// distribution block placed as primary. See
  /// [`nodes_of_fragment`](Self::nodes_of_fragment).
  pub fragment_nodes: Vec<u16>,
  /// The columns, in id order as sent.
  pub attributes: Vec<ColumnProps>,
}

impl TableProps {
  /// A table with nothing sent about it yet: the defaults.
  fn with_defaults() -> TableProps {
    TableProps {
      name: String::new(),
      table_id: !0,
      table_version: 0,
      table_kind: 0,
      logged: true,
      temporary: false,
      force_varsize_part: false,
      key_column_count: 0,
      column_count: 0,
      nullable_count: 0,
      varsize_count: 0,
      key_length: 0,
      linear_hash_k: 6,
      min_fill: 78,
      max_fill: 80,
      sql_metadata_len: 0,
      fragment_kind: IC_FRAGMENT_KIND_HASH_MAP,
      base_table_name: String::new(),
      base_table_id: IC_NULL_PTR,
      index_state: !0,
      partition_balance: IC_PARTITION_BALANCE_FOR_RP_BY_LDM,
      fragment_count: 0,
      partition_count: 0,
      hash_map_id: IC_NULL_PTR,
      hash_map_version: IC_NULL_PTR,
      storage_kind: IC_STORAGE_DEFAULT,
      read_backup: false,
      fully_replicated: false,
      // The defaults are "not set", which reads as on.
      row_gci: true,
      row_checksum: true,
      single_user: 0,
      max_rows: 0,
      min_rows: 0,
      extra_gci_bits: 0,
      extra_author_bits: 0,
      hash_function: 0,
      ttl_seconds: IC_NULL_PTR,
      ttl_column: IC_NULL_PTR,
      fanout_base_keys: 0,
      fanout_detail_keys: 0,
      fanout: 1,
      ring_size: IC_NULL_PTR,
      replica_count: 0,
      fragment_nodes: Vec::new(),
      attributes: Vec::new(),
    }
  }

  /// The nodes holding a fragment, or nothing for a fragment the data
  /// does not cover.
  pub fn nodes_of_fragment(&self, fragment: u32) -> &[u16] {
    let per = self.replica_count as usize;
    let start = fragment as usize * per;
    if per == 0 || start + per > self.fragment_nodes.len() {
      return &[];
    }
    &self.fragment_nodes[start..start + per]
  }

  /// How many fragments the replica data covers.
  pub fn fragments_with_nodes(&self) -> u32 {
    if self.replica_count == 0 {
      return 0;
    }
    (self.fragment_nodes.len() / self.replica_count as usize) as u32
  }

  /// Read the replica data. Data that is too short for what its counts
  /// say is left out, as if none had come.
  fn set_replica_data(&mut self, property: &Property) {
    let bytes = match &property.value {
      PropertyValue::Binary(bytes) => bytes,
      _ => return,
    };
    let replicas = match be_word(bytes, 0) {
      Some(count) => count as usize,
      None => return,
    };
    let fragments = match be_word(bytes, 1) {
      Some(count) => count as usize,
      None => return,
    };
    // Per fragment its log part, which is not wanted, and its nodes.
    if replicas == 0 || bytes.len() < 2 * (2 + fragments * (1 + replicas)) {
      return;
    }
    let mut nodes: Vec<u16> = Vec::with_capacity(fragments * replicas);
    let mut i: usize = 2;
    let mut f: usize = 0;
    while f < fragments {
      i += 1;
      let mut r: usize = 0;
      while r < replicas {
        if let Some(node) = be_word(bytes, i) {
          nodes.push(node);
        }
        i += 1;
        r += 1;
      }
      f += 1;
    }
    self.replica_count = replicas as u32;
    self.fragment_nodes = nodes;
  }

  fn apply(&mut self, property: &Property) {
    let value = property.as_u32();
    match property.key {
      IC_TP_TABLE_NAME => self.name = text_of(property),
      IC_TP_TABLE_ID => self.table_id = value,
      IC_TP_TABLE_VERSION => self.table_version = value,
      IC_TP_LOGGED => self.logged = value != 0,
      IC_TP_KEY_COLUMN_COUNT => self.key_column_count = value,
      IC_TP_COLUMN_COUNT => self.column_count = value,
      IC_TP_NULLABLE_COUNT => self.nullable_count = value,
      IC_TP_VARSIZE_COUNT => self.varsize_count = value,
      IC_TP_KEY_LENGTH => self.key_length = value,
      IC_TP_LINEAR_HASH_K => self.linear_hash_k = value,
      IC_TP_MIN_FILL => self.min_fill = value,
      IC_TP_MAX_FILL => self.max_fill = value,
      IC_TP_LEGACY_FRM | IC_TP_SQL_METADATA => {
        if let PropertyValue::Binary(bytes) = &property.value {
          self.sql_metadata_len = bytes.len() as u32;
        }
      }
      IC_TP_FRAGMENT_KIND => self.fragment_kind = value,
      IC_TP_TABLE_KIND => self.table_kind = value,
      IC_TP_BASE_TABLE_NAME => self.base_table_name = text_of(property),
      IC_TP_BASE_TABLE_ID => self.base_table_id = value,
      IC_TP_INDEX_STATE => self.index_state = value,
      IC_TP_TEMPORARY => self.temporary = value != 0,
      IC_TP_FORCE_VARSIZE_PART => self.force_varsize_part = value != 0,
      IC_TP_PARTITION_BALANCE => self.partition_balance = value,
      IC_TP_FRAGMENT_COUNT => self.fragment_count = value,
      IC_TP_MAX_ROWS_LOW => {
        self.max_rows = (self.max_rows & !0xFFFF_FFFF) | value as u64;
      }
      IC_TP_MAX_ROWS_HIGH => {
        self.max_rows = (self.max_rows & 0xFFFF_FFFF) | ((value as u64) << 32);
      }
      IC_TP_MIN_ROWS_LOW => {
        self.min_rows = (self.min_rows & !0xFFFF_FFFF) | value as u64;
      }
      IC_TP_MIN_ROWS_HIGH => {
        self.min_rows = (self.min_rows & 0xFFFF_FFFF) | ((value as u64) << 32);
      }
      IC_TP_ROW_GCI => self.row_gci = value != 0,
      IC_TP_ROW_CHECKSUM => self.row_checksum = value != 0,
      IC_TP_SINGLE_USER => self.single_user = value,
      IC_TP_HASH_MAP_ID => self.hash_map_id = value,
      IC_TP_HASH_MAP_VERSION => self.hash_map_version = value,
      IC_TP_STORAGE_KIND => self.storage_kind = value,
      IC_TP_EXTRA_GCI_BITS => self.extra_gci_bits = value,
      IC_TP_EXTRA_AUTHOR_BITS => self.extra_author_bits = value,
      IC_TP_READ_BACKUP => self.read_backup = value != 0,
      IC_TP_FULLY_REPLICATED => self.fully_replicated = value != 0,
      IC_TP_PARTITION_COUNT => self.partition_count = value,
      IC_TP_HASH_FUNCTION => self.hash_function = value,
      IC_TP_TTL_SECONDS => self.ttl_seconds = value,
      IC_TP_TTL_COLUMN => self.ttl_column = value,
      IC_TP_FANOUT_BASE_KEYS => {
        self.fanout_base_keys = value;
      }
      IC_TP_FANOUT_DETAIL_KEYS => {
        self.fanout_detail_keys = value;
      }
      IC_TP_FANOUT => self.fanout = value,
      IC_TP_RING_SIZE => self.ring_size = value,
      IC_TP_REPLICA_MAP => self.set_replica_data(property),
      // Anything else is not needed, or not known: pass it over.
      _ => {}
    }
  }

  /// The database the table belongs to, from its internal name.
  pub fn database(&self) -> &str {
    name_part(&self.name, 0)
  }

  /// The table's own name, without database and schema.
  pub fn table_name(&self) -> &str {
    name_part(&self.name, 2)
  }

  /// A column by name.
  pub fn attribute(&self, name: &str) -> Option<&ColumnProps> {
    self.attributes.iter().find(|attr| attr.name == name)
  }

  /// The upper version, bits 24 to 31: how many online changes. An
  /// operation prepared before one still works after it.
  pub fn version_upper(&self) -> u32 {
    self.table_version >> IC_TABLE_VERSION_UPPER_SHIFT
  }

  /// The lower version, bits 0 to 23: how many offline changes. The
  /// data nodes check operations against this part alone.
  pub fn version_lower(&self) -> u32 {
    self.table_version & IC_TABLE_VERSION_LOWER_MASK
  }
}

/// A hash map: which fragment each bucket of key hashes goes to.
///
/// A table placed by hash map sends a row to fragment
/// `fragments[hash % fragments.len()]`, where the hash is taken over
/// the row's distribution key. Many tables share one map, named after
/// its bucket and fragment counts.
/// Verify: chapter 05 §17.8.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HashMapProps {
  /// The map's name.
  pub name: String,
  /// Its id, which a table names with `IC_TP_HASH_MAP_ID`.
  pub object_id: u32,
  /// Its version.
  pub version: u32,
  /// The fragment each bucket goes to.
  pub fragments: Vec<u16>,
}

impl HashMapProps {
  /// The fragment a key hash goes to.
  pub fn fragment_of(&self, hash: u32) -> u32 {
    if self.fragments.is_empty() {
      return 0;
    }
    self.fragments[hash as usize % self.fragments.len()] as u32
  }

  /// How many distinct fragments the map sends rows to.
  pub fn fragment_count(&self) -> u32 {
    let mut highest: u32 = 0;
    for fragment in &self.fragments {
      if *fragment as u32 + 1 > highest {
        highest = *fragment as u32 + 1;
      }
    }
    highest
  }
}

/// Read a hash map description.
///
/// Every value defaults to zero and there is no end marker: the map is
/// whatever the properties say. The bucket count is sent as a length in
/// bytes. The buckets themselves are 16-bit numbers copied as the data
/// node holds them, so they are read in our own byte order, which the
/// protocol assumes is theirs.
/// Verify: chapter 05 §17.8.
pub fn parse_hash_map_info(words: &[u32]) -> Result<HashMapProps, IcError> {
  let bad = IcError::new(err::IC_ERROR_BAD_TABLE_DESCRIPTION);
  let mut reader = PropertyReader::new(words);
  let mut name = String::new();
  let mut object_id: u32 = 0;
  let mut version: u32 = 0;
  let mut bucket_bytes: u32 = 0;
  let mut values: Vec<u8> = Vec::new();
  while let Some(property) = reader.read_next()? {
    match property.key {
      IC_HMP_NAME => name = text_of(&property),
      IC_HMP_BUCKETS => bucket_bytes = property.as_u32(),
      IC_HMP_VALUES => {
        if let PropertyValue::Binary(bytes) = property.value {
          values = bytes;
        }
      }
      IC_TP_HASH_MAP_ID => object_id = property.as_u32(),
      IC_TP_HASH_MAP_VERSION => version = property.as_u32(),
      _ => {}
    }
  }
  let buckets = (bucket_bytes / 2) as usize;
  if buckets == 0 || values.len() < 2 * buckets {
    return Err(bad);
  }
  let mut fragments: Vec<u16> = Vec::with_capacity(buckets);
  let mut i: usize = 0;
  while i < buckets {
    fragments.push(u16::from_ne_bytes([values[2 * i], values[2 * i + 1]]));
    i += 1;
  }
  Ok(HashMapProps {
    name,
    object_id,
    version,
    fragments,
  })
}

/// Read a table description.
pub fn parse_table_info(words: &[u32]) -> Result<TableProps, IcError> {
  let bad = IcError::new(err::IC_ERROR_BAD_TABLE_DESCRIPTION);
  let mut reader = PropertyReader::new(words);
  let mut table = TableProps::with_defaults();
  let mut current: Option<ColumnProps> = None;
  while let Some(property) = reader.read_next()? {
    if property.key == IC_TP_COLUMN_NAME {
      if current.is_some() {
        // A column that never reached its end marker.
        return Err(bad);
      }
      current = Some(ColumnProps::with_defaults(text_of(&property)));
      continue;
    }
    if property.key == IC_TP_COLUMN_END {
      let mut attr = match current.take() {
        Some(attr) => attr,
        None => return Err(bad),
      };
      if !attr.size_from_type() {
        return Err(bad);
      }
      if attr.is_char_type() != (attr.charset != 0) {
        // A character type needs a character set and nothing else may
        // have one; the reference refuses the table otherwise.
        return Err(bad);
      }
      table.attributes.push(attr);
      continue;
    }
    match current.as_mut() {
      Some(attr) => attr.apply(&property),
      None => table.apply(&property),
    }
  }
  let announced = table.column_count as usize;
  if current.is_some() || table.attributes.len() != announced {
    return Err(bad);
  }
  default_distribution_key(&mut table);
  Ok(table)
}

/// The `i`th big-endian 16-bit value of `bytes`, if there is one.
fn be_word(bytes: &[u8], i: usize) -> Option<u16> {
  if 2 * i + 1 < bytes.len() {
    return Some(u16::from_be_bytes([bytes[2 * i], bytes[2 * i + 1]]));
  }
  None
}

/// When no column is marked as part of the distribution key, every
/// primary key column is: the table was created without saying, and the
/// whole key decides where a row goes, and the reference reads it the
/// same way. Key hashing depends on this, not only printing.
/// Verify: chapter 05 §17.8.
fn default_distribution_key(table: &mut TableProps) {
  let any_marked = table.attributes.iter().any(|attr| attr.distribution_key);
  if any_marked {
    return;
  }
  for attr in table.attributes.iter_mut() {
    if attr.primary_key {
      attr.distribution_key = true;
    }
  }
}

/// A default value as the dictionary sends it: a four-byte header, in
/// network byte order, whose low 15 bits are the value's size in bytes,
/// then the value. A size of zero means no default. The header is
/// dropped here, so that an empty result means no default and anything
/// else is the value itself.
///
/// The reference converts the value's byte order only on a big-endian
/// host; on ours the bytes are used as they come, whatever the comment
/// beside that code says about network byte order.
/// Verify: chapter 05 §17.8.
fn default_value_of(bytes: &[u8]) -> Vec<u8> {
  if bytes.len() < 4 {
    return Vec::new();
  }
  let header = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
  let size = (header & 0x7FFF) as usize;
  let end = 4 + size;
  if size == 0 || end > bytes.len() {
    return Vec::new();
  }
  bytes[4..end].to_vec()
}

/// The size in bytes of a packed decimal of this precision and scale:
/// four bytes for every nine digits, and fewer for the remainder, on
/// each side of the point. The same rule as MySQL's `decimal_bin_size`.
pub fn decimal_bin_size(precision: u32, scale: u32) -> u32 {
  const DIGITS_PER_WORD: u32 = 9;
  const BYTES_FOR_DIGITS: [u32; 10] = [0, 1, 1, 2, 2, 3, 3, 4, 4, 4];
  let whole = precision - scale;
  let whole_words = whole / DIGITS_PER_WORD;
  let whole_rest = whole - whole_words * DIGITS_PER_WORD;
  let frac_words = scale / DIGITS_PER_WORD;
  let frac_rest = scale - frac_words * DIGITS_PER_WORD;
  whole_words * 4
    + BYTES_FOR_DIGITS[whole_rest as usize]
    + frac_words * 4
    + BYTES_FOR_DIGITS[frac_rest as usize]
}

/// What the data nodes call a type, for printing.
pub fn type_name(type_code: u32) -> &'static str {
  match type_code {
    IC_TYPE_TINYINT => "Tinyint",
    IC_TYPE_TINYUNSIGNED => "Tinyunsigned",
    IC_TYPE_SMALLINT => "Smallint",
    IC_TYPE_SMALLUNSIGNED => "Smallunsigned",
    IC_TYPE_MEDIUMINT => "Mediumint",
    IC_TYPE_MEDIUMUNSIGNED => "Mediumunsigned",
    IC_TYPE_INT => "Int",
    IC_TYPE_UNSIGNED => "Unsigned",
    IC_TYPE_BIGINT => "Bigint",
    IC_TYPE_BIGUNSIGNED => "Bigunsigned",
    IC_TYPE_FLOAT => "Float",
    IC_TYPE_DOUBLE => "Double",
    IC_TYPE_OLDDECIMAL => "Olddecimal",
    IC_TYPE_OLDDECIMALUNSIGNED => "Olddecimalunsigned",
    IC_TYPE_DECIMAL => "Decimal",
    IC_TYPE_DECIMALUNSIGNED => "Decimalunsigned",
    IC_TYPE_CHAR => "Char",
    IC_TYPE_VARCHAR => "Varchar",
    IC_TYPE_BINARY => "Binary",
    IC_TYPE_VARBINARY => "Varbinary",
    IC_TYPE_DATETIME => "Datetime",
    IC_TYPE_DATE => "Date",
    IC_TYPE_BLOB => "Blob",
    IC_TYPE_TEXT => "Text",
    IC_TYPE_BIT => "Bit",
    IC_TYPE_LONGVARCHAR => "Longvarchar",
    IC_TYPE_LONGVARBINARY => "Longvarbinary",
    IC_TYPE_TIME => "Time",
    IC_TYPE_YEAR => "Year",
    IC_TYPE_TIMESTAMP => "Timestamp",
    IC_TYPE_TIME2 => "Time2",
    IC_TYPE_DATETIME2 => "Datetime2",
    IC_TYPE_TIMESTAMP2 => "Timestamp2",
    _ => "Undefined",
  }
}

fn text_of(property: &Property) -> String {
  match &property.value {
    PropertyValue::String(text) => text.clone(),
    _ => String::new(),
  }
}

/// Part `index` of a `database/schema/table` name: the whole name when
/// it has fewer parts, so that a name without separators is its own
/// table name.
fn name_part(name: &str, index: usize) -> &str {
  match name.splitn(3, '/').nth(index) {
    Some(piece) => piece,
    None => name,
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::prop_stream::PropertyWriter;

  /// A description as a data node sends it for
  /// `CREATE TABLE ictest.t1 (id INT PRIMARY KEY, name VARCHAR(32))`:
  /// only values that differ from the defaults.
  fn sample() -> Vec<u32> {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "ictest/def/t1");
    w.add_u32(IC_TP_TABLE_ID, 17);
    w.add_u32(IC_TP_TABLE_VERSION, 3);
    w.add_u32(IC_TP_KEY_COLUMN_COUNT, 1);
    w.add_u32(IC_TP_COLUMN_COUNT, 2);
    w.add_u32(IC_TP_NULLABLE_COUNT, 1);
    w.add_u32(IC_TP_TABLE_KIND, IC_OBJECT_KIND_TABLE);
    w.add_u32(IC_TP_READ_BACKUP, 1);
    // A key this reader has never heard of.
    w.add_u32(9876, 5);
    w.add_string(IC_TP_COLUMN_NAME, "id");
    w.add_u32(IC_TP_COLUMN_ID, 0);
    w.add_u32(IC_TP_COLUMN_IN_KEY, 1);
    w.add_u32(IC_TP_COLUMN_IN_DIST_KEY, 1);
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 1);
    w.add_u32(IC_TP_COLUMN_END, 0);
    w.add_string(IC_TP_COLUMN_NAME, "name");
    w.add_u32(IC_TP_COLUMN_ID, 1);
    w.add_u32(IC_TP_COLUMN_NULLABLE, 1);
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_VARCHAR);
    // Precision 0 with character set 255 in the high half.
    w.add_u32(IC_TP_COLUMN_PRECISION, 255 << 16);
    w.add_u32(IC_TP_COLUMN_LENGTH, 32 * 4);
    w.add_u32(IC_TP_COLUMN_ARRAY_KIND, IC_ARRAY_KIND_SHORT_VARSIZE);
    w.add_u32(IC_TP_COLUMN_END, 0);
    w.add_u32(999, 0);
    w.words().to_vec()
  }

  #[test]
  fn the_replica_data_says_which_nodes_hold_each_fragment() {
    // Two replicas, two fragments: log part 0 with nodes 1 and 2, log
    // part 1 with nodes 2 and 1, all big-endian.
    let values: [u16; 8] = [2, 2, 0, 1, 2, 1, 2, 1];
    let mut bytes: Vec<u8> = Vec::new();
    for value in values {
      bytes.extend_from_slice(&value.to_be_bytes());
    }
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "ictest/def/t1");
    w.add_u32(IC_TP_REPLICA_MAP_LEN, bytes.len() as u32);
    w.add_binary(IC_TP_REPLICA_MAP, &bytes);
    let table = parse_table_info(w.words()).expect("parsed");
    assert_eq!(table.replica_count, 2);
    assert_eq!(table.fragments_with_nodes(), 2);
    assert_eq!(table.nodes_of_fragment(0), &[1, 2]);
    assert_eq!(table.nodes_of_fragment(1), &[2, 1]);
    assert!(table.nodes_of_fragment(2).is_empty());
  }

  #[test]
  fn replica_data_too_short_for_its_counts_is_left_out() {
    let values: [u16; 5] = [2, 2, 0, 1, 2];
    let mut bytes: Vec<u8> = Vec::new();
    for value in values {
      bytes.extend_from_slice(&value.to_be_bytes());
    }
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "ictest/def/t1");
    w.add_binary(IC_TP_REPLICA_MAP, &bytes);
    let table = parse_table_info(w.words()).expect("parsed");
    assert_eq!(table.replica_count, 0);
    assert!(table.nodes_of_fragment(0).is_empty());
  }

  #[test]
  fn a_version_is_two_counters() {
    // Seen live: a table altered once online, after being created.
    let table = TableProps {
      table_version: 16_777_217,
      ..parse_table_info(&sample()).expect("parsed")
    };
    assert_eq!(table.version_upper(), 1);
    assert_eq!(table.version_lower(), 1);
  }

  #[test]
  fn a_table_and_its_columns_are_read() {
    let table = parse_table_info(&sample()).expect("parsed");
    assert_eq!(table.name, "ictest/def/t1");
    assert_eq!(table.database(), "ictest");
    assert_eq!(table.table_name(), "t1");
    assert_eq!(table.table_id, 17);
    assert_eq!(table.table_version, 3);
    assert_eq!(table.version_upper(), 0);
    assert_eq!(table.version_lower(), 3);
    assert!(table.read_backup);
    assert_eq!(table.attributes.len(), 2);
    let id = table.attribute("id").expect("id");
    assert!(id.primary_key && id.distribution_key && !id.nullable);
    assert_eq!(id.max_byte_size(), 4);
    let name = table.attribute("name").expect("name");
    assert!(name.nullable && !name.primary_key);
    assert_eq!(name.charset, 255);
    // One length byte and up to 128 bytes of text.
    assert_eq!(name.max_byte_size(), 129);
  }

  #[test]
  fn what_is_not_sent_takes_the_default() {
    let table = parse_table_info(&sample()).expect("parsed");
    assert!(table.logged);
    assert_eq!(table.fragment_kind, IC_FRAGMENT_KIND_HASH_MAP);
    assert_eq!(table.hash_map_id, IC_NULL_PTR);
    assert_eq!(table.ttl_seconds, IC_NULL_PTR);
    assert_eq!(table.fanout, 1);
    let id = table.attribute("id").expect("id");
    assert_eq!(id.storage_kind, IC_STORAGE_MEMORY);
    assert_eq!(id.array_kind, IC_ARRAY_KIND_FIXED);
  }

  #[test]
  fn with_no_column_marked_the_primary_key_is_the_distribution_key() {
    // As mysqld creates `id INT PRIMARY KEY`: nothing marked, which the
    // reference reads as the whole primary key.
    let mut w = PropertyWriter::new();
    w.add_u32(IC_TP_COLUMN_COUNT, 2);
    w.add_string(IC_TP_COLUMN_NAME, "id");
    w.add_u32(IC_TP_COLUMN_IN_KEY, 1);
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 1);
    w.add_u32(IC_TP_COLUMN_END, 0);
    w.add_string(IC_TP_COLUMN_NAME, "v");
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 1);
    w.add_u32(IC_TP_COLUMN_END, 0);
    let table = parse_table_info(w.words()).expect("parsed");
    assert!(table.attribute("id").expect("id").distribution_key);
    assert!(!table.attribute("v").expect("v").distribution_key);
  }

  #[test]
  fn a_default_is_its_value_without_the_header() {
    // BIGINT UNSIGNED DEFAULT 0: header of size 8, then eight bytes.
    let mut bytes: Vec<u8> = ((2u32 << 16) | 8).to_be_bytes().to_vec();
    bytes.extend_from_slice(&0u64.to_le_bytes());
    assert_eq!(default_value_of(&bytes), vec![0u8; 8]);
    // Every column is sent a header; one of size zero is no default.
    let none = (1u32 << 16).to_be_bytes();
    assert!(default_value_of(&none).is_empty());
    // A size larger than what came is not trusted.
    let short = 9u32.to_be_bytes();
    assert!(default_value_of(&short).is_empty());
  }

  fn hash_map_words(buckets: &[u16]) -> Vec<u32> {
    let mut bytes: Vec<u8> = Vec::new();
    for bucket in buckets {
      bytes.extend_from_slice(&bucket.to_ne_bytes());
    }
    let mut w = PropertyWriter::new();
    w.add_string(IC_HMP_NAME, "DEFAULT-HASHMAP-4-2");
    w.add_u32(IC_HMP_BUCKETS, (2 * buckets.len()) as u32);
    w.add_binary(IC_HMP_VALUES, &bytes);
    w.add_u32(IC_TP_HASH_MAP_ID, 1);
    w.add_u32(IC_TP_HASH_MAP_VERSION, 7);
    w.words().to_vec()
  }

  #[test]
  fn a_hash_map_says_which_fragment_a_hash_goes_to() {
    let map =
      parse_hash_map_info(&hash_map_words(&[0, 1, 0, 1])).expect("parsed");
    assert_eq!(map.name, "DEFAULT-HASHMAP-4-2");
    assert_eq!(map.object_id, 1);
    assert_eq!(map.version, 7);
    assert_eq!(map.fragments, vec![0, 1, 0, 1]);
    assert_eq!(map.fragment_count(), 2);
    // The hash picks a bucket, and the bucket names the fragment.
    assert_eq!(map.fragment_of(5), 1);
    assert_eq!(map.fragment_of(6), 0);
  }

  #[test]
  fn the_bucket_count_is_sent_in_bytes() {
    // Four buckets are announced as eight bytes. Reading the count as
    // buckets would ask for eight and find the values short.
    let words = hash_map_words(&[3, 2, 1, 0]);
    let map = parse_hash_map_info(&words).expect("parsed");
    assert_eq!(map.fragments.len(), 4);
  }

  #[test]
  fn a_hash_map_without_buckets_is_refused() {
    let mut w = PropertyWriter::new();
    w.add_string(IC_HMP_NAME, "EMPTY");
    assert!(parse_hash_map_info(w.words()).is_err());
  }

  #[test]
  fn fewer_columns_than_announced_is_refused() {
    let mut w = PropertyWriter::new();
    w.add_string(IC_TP_TABLE_NAME, "a/def/b");
    w.add_u32(IC_TP_COLUMN_COUNT, 2);
    w.add_string(IC_TP_COLUMN_NAME, "x");
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    w.add_u32(IC_TP_COLUMN_LENGTH, 1);
    w.add_u32(IC_TP_COLUMN_END, 0);
    let e = parse_table_info(w.words()).expect_err("short");
    assert_eq!(e.code, err::IC_ERROR_BAD_TABLE_DESCRIPTION);
  }

  #[test]
  fn a_column_without_its_end_marker_is_refused() {
    let mut w = PropertyWriter::new();
    w.add_u32(IC_TP_COLUMN_COUNT, 1);
    w.add_string(IC_TP_COLUMN_NAME, "x");
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_INT);
    assert!(parse_table_info(w.words()).is_err());
  }

  #[test]
  fn a_character_column_needs_a_character_set() {
    let mut w = PropertyWriter::new();
    w.add_u32(IC_TP_COLUMN_COUNT, 1);
    w.add_string(IC_TP_COLUMN_NAME, "c");
    w.add_u32(IC_TP_COLUMN_TYPE, IC_TYPE_CHAR);
    w.add_u32(IC_TP_COLUMN_LENGTH, 10);
    w.add_u32(IC_TP_COLUMN_END, 0);
    assert!(parse_table_info(w.words()).is_err());
  }

  #[test]
  fn decimal_sizes_match_the_mysql_rule() {
    // Nine digits fit four bytes; the remainder takes a table lookup.
    assert_eq!(decimal_bin_size(10, 2), 5);
    assert_eq!(decimal_bin_size(9, 0), 4);
    assert_eq!(decimal_bin_size(65, 30), 30);
    assert_eq!(decimal_bin_size(1, 0), 1);
    assert_eq!(decimal_bin_size(18, 9), 8);
  }

  #[test]
  fn every_type_has_a_size_rule() {
    // Walk the whole type list, so that a type added to the constants
    // without a size rule is noticed here rather than by a user.
    let mut t = IC_TYPE_TINYINT;
    while t <= IC_TYPE_TIMESTAMP2 {
      let mut attr = ColumnProps::with_defaults("c".to_string());
      attr.type_code = t;
      attr.length = 1;
      attr.precision = 5;
      attr.scale = 2;
      attr.array_kind = IC_ARRAY_KIND_LONG_VARSIZE;
      assert!(attr.size_from_type(), "type {} has no size rule", t);
      assert_ne!(type_name(t), "Undefined");
      t += 1;
    }
  }

  #[test]
  fn a_name_without_separators_is_its_own_table_name() {
    assert_eq!(name_part("t1", 2), "t1");
    assert_eq!(name_part("db/def/t1", 0), "db");
    assert_eq!(name_part("db/def/t1", 2), "t1");
    // A table name may itself contain the separator; it is not split.
    assert_eq!(name_part("db/def/a/b", 2), "a/b");
  }
}
