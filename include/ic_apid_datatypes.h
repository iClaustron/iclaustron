/* Copyright (C) 2009 iClaustron AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  The basic operation types we support are scan, read using key
  and the write using a key.
*/
enum ic_apid_operation_type
{
  SCAN_OPERATION= 0,
  KEY_READ_OPERATION= 1,
  KEY_WRITE_OPERATION= 2
};

/*
  We only handle the basic types here. This means that we manage
  all integer types of various sorts, the bit type, fixed size
  character strings, the variable sized character strings. We also
  handle the new NDB type BLOB type which can store up to e.g.
  16MB or more on the actual record. User level blobs are handled
  on a higher level by using many records to accomodate for large
  BLOB's. A user level BLOB is allowed to be spread among many
  nodes and node groups within one cluster, but it cannot span
  many clusters. The new BLOB type is essentially an array of
  unsigned 8 bit values.
*/
enum ic_field_type
{
  IC_API_UINT32_TYPE= 0,
  IC_API_UINT64_TYPE= 1,
  IC_API_UINT24_TYPE= 2,
  IC_API_UINT16_TYPE= 3,
  IC_API_UINT8_TYPE= 4,
  IC_API_INT32_TYPE= 5,
  IC_API_INT64_TYPE= 6,
  IC_API_INT24_TYPE= 7,
  IC_API_INT16_TYPE= 8,
  IC_API_INT8_TYPE= 9,
  IC_API_BIT_TYPE= 10,
  IC_API_FIXED_SIZE_CHAR= 11,
  IC_API_VARIABLE_SIZE_CHAR= 12,
  IC_API_BLOB_TYPE= 13
};

/* For efficiency reasons the structs are part of the public API.  */

/*
  When performing a read operation we always supply a IC_READ_FIELD_BIND
  object. This object specifies the fields we want to read.
*/
struct ic_read_field_bind
{
  /* Number of fields in bit_array and field_defs array */
  guint32 no_fields;
  /* If a buffer_ptr is provided a buffer_size also is required. */
  guint32 buffer_size;
  /* User supplied buffer to use for returned results. */
  gchar *buffer_ptr;
  /*
    Reference to the buffer where results are returned. If the user
    supplied a buffer it will be the same buffer as he supplied,
    otherwise a buffer allocated by the API. This buffer is released
    when operation is released or transaction ended. So reference
    to the buffer is only safe until this call to the API. If the user
    supplied the buffer the reference to the buffer is safe until
    calling the API to get the next record of a scan. For key reads the
    buffer is stable until the user decides otherwise.
  */
  gchar *returned_buffer_ptr;
  /*
    The bit_array field makes it possible to reuse this structure for many
    different reads on the same table. We will only read those fields that
    have their bit set in the bit_array.
  */
  guint8 *bit_array;
  /*
    An array of pointers to IC_READ_FIELD_DEF objects describing
    fields to read.
  */
  IC_READ_FIELD_DEF **field_defs;
};

struct ic_write_field_bind
{
  /* Number of fields in bit_array and field_defs array */
  guint32 no_fields;
  /*
    For writes the user needs to supply a buffer, data is referenced
    offset from the buffer pointer. The user needs to supply both a
    buffer pointer and a size of the buffer. The buffer is maintained
    by the user of the API and won't be touched by the API other than
    for reading its data.
  */
  guint32 buffer_size;
  gchar *buffer_ptr;
  /*
    The bit_array field makes it possible to reuse this structure for many
    different writes on the same table. We will only write those fields that
    have their bit set in the bit_array.
  */
  guint8 *bit_array;
  /*
    An array of pointers to IC_WRITE_FIELD_DEF objects describing
    fields to write.
  */
  IC_WRITE_FIELD_DEF **field_defs;
};

struct ic_key_field_bind
{
  /* Number of fields in bit_array and field_defs array */
  guint32 no_fields;
  /*
    For writes the user needs to supply a buffer, data is referenced
    offset from the buffer pointer. The user needs to supply both a
    buffer pointer and a size of the buffer. The buffer is maintained
    by the user of the API and won't be touched by the API other than
    for reading its data.
  */
  guint32 buffer_size;
  gchar *buffer_ptr;
  /*
    An array of pointers to IC_KEY_FIELD_DEF objects describing
    fields to use in key.
  */
  IC_KEY_FIELD_DEF **field_defs;
};

struct ic_read_field_def
{
  /* Fields set by caller */
  /* The field can be specified by field id.  */
  guint32 field_id;
  /* Reference to the data within the buffer: This is set-up by caller */
  guint32 data_offset;
  /*
    For variable length it is possible to specify a starting byte position
    and an end position. The actual data read will always start at
    start_pos, the actual length read is returned in data_length. If zero
    then there was no data at start_pos or beyond.

    The start_pos and end_pos is set by caller and not changed by API.

    The special case of start_pos= end_pos= 0 means that we're interested
    in the length of the complete field. In this case the data type
    returned is always IC_API_UINT32 and the length is returned at
    data_offset and data_length is 4 (size of UINT32).
    
    To read all data in a field set start_pos to 0 and end_pos at
    least as big as the maximum size of the field. However it isn't
    possible to read more than 8 kBytes per read. It is however
    possible to send many parallel reads reading different portions
    of the field for very long fields. This distinction is only relevant
    for BLOB fields.
  */
  guint32 start_pos;
  guint32 end_pos;

  /* Fields set by API */
  /* The actual length of the field, set by API */
  guint32 data_length;
  /* The field type, set by API */
  IC_FIELD_TYPE field_type;
  /* Null indicator set by API */
  gboolean is_null;
};

struct ic_write_field_def
{
  /*
    All fields in this struct is set by caller and the struct is
    read-only in the API.
  */
  /* The field can be specified by field id. */
  guint32 field_id;
  /* Reference inside the buffer of the data we're writing */
  guint32 data_offset;
  /*
    For fixed size fields the start_pos and end_pos is ignored.
    For variable sized fields it indicates the starting position
    to write from and the end position to write into. For normal
    writes of the full field we set start_pos to 0 and end_pos to
    size of the field.
  */
  guint32 start_pos;
  guint32 end_pos;
  /*
    This is the field type of the data sent to the API, if there is a
    mismatch between this type and the field type in the database
    we will apply a conversion method to convert it to the proper
    data type.
  */
  IC_FIELD_TYPE field_type;
  /* TRUE if we're writing NULL to the field */
  gboolean is_null;
  /*
    TRUE if we're writing zero's from start_pos to end_pos,
    Thus no need to send data since we're setting everything
    to zero's.
  */
  gboolean allocate_field_size;
};

struct ic_key_field_def
{
  /*
    All fields in this struct is set by caller and the struct is
    read-only in the API.
  */
  /* The field can be specified by field id. */
  guint32 field_id;
  /* Reference inside the buffer of the data we're writing */
  guint32 data_offset;
  /* Length of field, no BLOBs allowed here */
  guint32 data_length;
  /*
    This is the field type of the data sent to the API, if there is a
    mismatch between this type and the field type in the database
    we will apply a conversion method to convert it to the proper
    data type.
  */
  IC_FIELD_TYPE field_type;
};


enum ic_commit_state
{
  STARTED = 0,
  COMMIT_REQUESTED = 1,
  ABORT_REQUESTED = 2,
  COMMITTED = 3,
  ABORTED = 4,
  ROLLBACK_SAVEPOINT_REQUESTED = 5
};

/*
  Read key operation have different locking semantics. There is a variant
  which locks the key before reading it but also immediately releases
*/
enum ic_read_key_op
{
  SIMPLE_KEY_READ= 0,
  KEY_READ= 1,
  EXCLUSIVE_KEY_READ= 2,
  COMMITTED_KEY_READ= 3
};

enum ic_write_key_op
{
  KEY_UPDATE= 0,
  KEY_WRITE= 1,
  KEY_INSERT= 2,
  KEY_DELETE= 3
};

enum ic_scan_op
{
  SCAN_READ_COMMITTED= 0,
  SCAN_READ_EXCLUSIVE= 1,
  SCAN_HOLD_LOCK= 2,
  SCAN_CONSISTENT_READ= 3
};

enum ic_instruction_type
{
  INST_LOAD_CONST32= 0
};

enum ic_error_severity_level
{
  WARNING = 0,
  ERROR = 1,
  STOP_ERROR = 2
};

enum ic_error_category
{
  USER_ERROR = 0,
  INTERNAL_ERROR = 1
};

