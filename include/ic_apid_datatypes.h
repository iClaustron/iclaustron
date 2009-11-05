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
  STARTED = 0,
  COMMIT_REQUESTED = 1,
  ROLLBACK_REQUESTED = 2,
  COMMITTED = 3,
  ROLLED_BACK = 4,
  CREATE_SAVEPOINT_REQUESTED= 5,
  ROLLBACK_SAVEPOINT_REQUESTED = 6
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
  WARNING = 0,
  ERROR = 1,
  STOP_ERROR = 2
};

enum ic_error_category
{
  USER_ERROR = 0,
  INTERNAL_ERROR = 1
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
#endif
