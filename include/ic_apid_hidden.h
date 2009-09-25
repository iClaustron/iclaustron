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

#ifndef IC_APID_HIDDEN_H
#define IC_APID_HIDDEN_H
typedef struct ic_read_key_operation IC_READ_KEY_OPERATION;
typedef struct ic_write_key_operation IC_WRITE_KEY_OPERATION;
typedef struct ic_scan_operation IC_SCAN_OPERATION;
typedef struct ic_record_operation IC_RECORD_OPERATION;
typedef struct ic_key_operation IC_KEY_OPERATION;
typedef struct ic_apid_operation IC_COMMIT_OPERATION;
typedef struct ic_apid_operation IC_ROLLBACK_OPERATION;
typedef struct ic_message_error_object IC_MESSAGE_ERROR_OBJECT;
typedef enum ic_apid_operation_list_type IC_APID_OPERATION_LIST_TYPE;

/*
  This is part of the external interface for efficiency reasons.
  However this part is defined to not be stable. The stable interface
  is always methods. In many cases those are inline methods and
  for efficiency reason their implementation is visible for the user of
  the API. However no implementation on top of the API should use this
  information.
*/
struct ic_transaction_state
{
  IC_COMMIT_STATE commit_state;
  IC_SAVEPOINT_ID savepoint_stable;
  IC_SAVEPOINT_ID savepoint_requested;
};

struct ic_message_error_object
{
  int error;
  gchar *error_string;
  int error_category;
  int error_severity;
};

struct ic_table_def
{
  guint32 table_id;
  guint32 index_id;
  gboolean use_index;
};

struct ic_instruction
{
  IC_INSTRUCTION_TYPE instr_type;
  guint32 source_register1;
  guint32 source_register2;
  guint32 dest_register;
};

struct ic_range_condition
{
  guint32 not_used;
};

struct ic_where_condition
{
  guint32 not_used;
};

/*
  This part is common for all operations towards the iClaustron Data API.
*/

enum ic_apid_operation_list_type
{
  NO_LIST = 0,
  IN_DEFINED_LIST = 1,
  IN_EXECUTING_LIST = 2,
  IN_EXECUTED_LIST = 3,
  IN_COMPLETED_LIST = 4
};

struct ic_apid_operation
{
  IC_APID_OPERATION_TYPE op_type;
  IC_APID_CONNECTION *apid_conn;
  IC_TRANSACTION *trans_obj;
  IC_TABLE_DEF *table_def;
  IC_WHERE_CONDITION *where_cond;
  union
  {
    /* range_cond used by scans, key_fields used by key operations */
    IC_RANGE_CONDITION *range_cond;
    IC_KEY_FIELD_BIND *key_fields;
  };
  union
  {
    /*
      read_fields used by scans and read key operations and
      write_fields used by write key operations
    */
    IC_READ_FIELD_BIND *read_fields;
    IC_WRITE_FIELD_BIND *write_fields;
  };
  union
  {
    /*
      read_key_op used by read key operations
      write_key_op used by write key operations
      scan_op used by scan operations.
    */
    IC_READ_KEY_OP read_key_op;
    IC_WRITE_KEY_OP write_key_op;
    IC_SCAN_OP scan_op;
  };
  IC_APID_ERROR *error;
  void *user_reference;
  IC_APID_OPERATION *next_trans_op;
  IC_APID_OPERATION *prev_trans_op;
  IC_APID_OPERATION *next_conn_op;
  IC_APID_OPERATION *prev_conn_op;
  IC_APID_OPERATION_LIST_TYPE list_type;
};

struct ic_key_operation
{
  IC_APID_OPERATION rec_op;
};

/*
  Read key operations have a definition of fields read, a definition of the
  key fields and finally a definition of the type of read operation.
*/
struct ic_read_key_operation
{
  IC_KEY_OPERATION apid_op;
  IC_READ_FIELD_BIND *read_fields;
};

/*
  Write key operations have a definition of fields written, a definition of
  the key fields and finally a definition of the write operation type.
*/
struct ic_write_key_operation
{
  IC_KEY_OPERATION apid_op;
};

/*
  Scan operations also have a definition of fields read, it has a range
  condition in the cases when there is a scan of an index (will be NULL
  for scan table operation). There is also a generic where condition.
*/
struct ic_scan_operation
{
  IC_APID_OPERATION apid_op;
  IC_READ_FIELD_BIND *read_fields;
  IC_SCAN_OP scan_op;
};

/*
  Commit and rollback operations don't need any extra information outside of
  IC_APID_OPERATION object. Thus we define IC_COMMIT_OPERATION to be of
  type struct ic_apid_operation and the same for IC_ABORT_OPERATION.
*/

/*
  Create/Rollback savepoint operation requires a note about savepoint id
  in addition to the fields in the IC_APID_OPERATION object.
*/
struct ic_create_savepoint_operation
{
  IC_APID_OPERATION apid_op;
  IC_SAVEPOINT_ID savepoint_id;
};

struct ic_rollback_savepoint_operation
{
  IC_APID_OPERATION apid_op;
  IC_SAVEPOINT_ID savepoint_id;
};
#endif
