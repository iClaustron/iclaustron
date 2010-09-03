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

#ifndef IC_APID_DATATYPES_H
#define IC_APID_DATATYPES_H
enum ic_commit_state
{
  IC_TRANS_STARTED = 0,
  IC_COMMIT_REQUESTED = 1,
  IC_ROLLBACK_REQUESTED = 2,
  IC_COMMITTED = 3,
  IC_ROLLED_BACK = 4,
  IC_CREATE_SAVEPOINT_REQUESTED= 5,
  IC_ROLLBACK_SAVEPOINT_REQUESTED = 6
};

/*
  Read key operation have different locking semantics. There is a variant
  which locks the key before reading it but also immediately releases
*/
enum ic_read_key_op
{
  IC_SIMPLE_KEY_READ= 0,
  IC_KEY_READ= 1,
  IC_EXCLUSIVE_KEY_READ= 2,
  IC_COMMITTED_KEY_READ= 3
};

enum ic_write_key_op
{
  IC_KEY_UPDATE= 0,
  IC_KEY_WRITE= 1,
  IC_KEY_INSERT= 2,
  IC_KEY_DELETE= 3
};

enum ic_scan_op
{
  IC_SCAN_READ_COMMITTED= 0,
  IC_SCAN_READ_EXCLUSIVE= 1,
  IC_SCAN_HOLD_LOCK= 2,
  IC_SCAN_CONSISTENT_READ= 3
};

enum ic_lower_range_type
{
  IC_NO_MINIMUM= 0,
  IC_LOWER_RANGE_GT= 1,
  IC_LOWER_RANGE_GE= 2,
  IC_LOWER_RANGE_EQ= 3
};

enum ic_upper_range_type
{
  IC_NO_MAXIMUM= 0,
  IC_UPPER_RANGE_LT= 1,
  IC_UPPER_RANGE_LE= 2,
  IC_UPPER_RANGE_EQ= 3
};

enum ic_comparator_type
{
  IC_COND_EQ= 0,
  IC_COND_LT= 1,
  IC_COND_LE= 2,
  IC_COND_GT= 3,
  IC_COND_GE= 4,
  IC_COND_NE= 5
};

enum ic_boolean_type
{
  IC_AND= 0,
  IC_OR= 1,
  IC_XOR= 2
};

enum ic_calculation_type
{
  IC_PLUS= 0,
  IC_MINUS= 1,
  IC_MULTIPLICATION= 2,
  IC_DIVISION= 3,
  IC_BITWISE_OR= 4,
  IC_BITWISE_AND= 5,
  IC_BITWISE_XOR= 6
};

enum ic_error_severity_level
{
  IC_SEVERITY_NO_ERROR= 0,
  IC_SEVERITY_WARNING= 1,
  IC_SEVERITY_ERROR= 2,
  IC_SEVERITY_STOP_ERROR= 3
};

enum ic_error_category
{
  IC_CATEGORY_NO_ERROR= 0,
  IC_CATEGORY_USER_ERROR= 1,
  IC_CATEGORY_INTERNAL_ERROR= 2
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
  IC_API_INT32_TYPE= 0,
  IC_API_INT64_TYPE= 1,
  IC_API_INT24_TYPE= 2,
  IC_API_INT16_TYPE= 3,
  IC_API_INT8_TYPE= 4,
  IC_API_BIT_TYPE= 5,
  IC_API_FIXED_SIZE_CHAR= 6,
  IC_API_VARIABLE_SIZE_CHAR= 7,
  IC_API_BLOB_TYPE= 8,
  IC_API_DECIMAL_TYPE= 9
};

enum ic_index_type
{
  IC_PRIMARY_KEY= 0,
  IC_UNIQUE_KEY= 1,
  IC_ORDERED_INDEX= 2
};

enum ic_partition_type
{
  IC_LINEAR_KEY_PARTITION= 0,
  IC_KEY_PARTITION= 1,
  IC_RANGE_PARTITION= 2,
  IC_LIST_PARTITION= 3
};

enum ic_tablespace_access_mode
{
  IC_TABLESPACE_READ_WRITE= 0,
  IC_TABLESPACE_READ_ONLY= 1,
  IC_TABLESPACE_NO_ACCESS= 2
};
#endif
