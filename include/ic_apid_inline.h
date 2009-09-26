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

#ifndef IC_APID_INLINE_H
#define IC_APID_INLINE_H
/*
  MODULE: Inline functions for IC_TRANSACTION_STATE object
  --------------------------------------------------------
*/
static inline IC_COMMIT_STATE
ic_get_commit_state(IC_TRANSACTION_STATE *trans_state)
{
  return trans_state->commit_state;
}

static inline IC_SAVEPOINT_ID
ic_get_stable_savepoint(IC_TRANSACTION_STATE *trans_state)
{
  return trans_state->savepoint_stable;
}

static inline IC_SAVEPOINT_ID
ic_get_requested_savepoint(IC_TRANSACTION_STATE *trans_state)
{
  return trans_state->savepoint_requested;
}

/*
  MODULE: Inline functions for IC_APID_OPERATION classes
  ------------------------------------------------------
*/
static inline IC_APID_OPERATION_TYPE
ic_get_apid_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  return apid_op->op_type;
}

static inline IC_APID_CONNECTION*
ic_get_apid_connection_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  return apid_op->apid_conn;
}

static inline IC_TRANSACTION*
ic_get_transaction_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  return apid_op->trans_obj;
}

static inline IC_TABLE_DEF*
ic_get_table_definition_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == KEY_READ_OPERATION ||
           apid_op->op_type == SCAN_OPERATION ||
           apid_op->op_type == KEY_WRITE_OPERATION);
  return apid_op->table_def;
}

static inline IC_APID_ERROR*
ic_get_error_object_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  return apid_op->error;
}

static inline gboolean
ic_is_failed_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  return apid_op->error->any_error;
}
static inline void*
ic_get_user_reference_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  return apid_op->user_reference;
}

static inline IC_READ_FIELD_BIND*
ic_get_read_field_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == KEY_READ_OPERATION ||
           apid_op->op_type == SCAN_OPERATION);
  return apid_op->read_fields;
}

static inline IC_WRITE_FIELD_BIND*
ic_get_write_field_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == KEY_WRITE_OPERATION);
  return apid_op->write_fields;
}

static inline IC_KEY_FIELD_BIND*
ic_get_key_fields_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == KEY_WRITE_OPERATION ||
           apid_op->op_type == KEY_READ_OPERATION);
  return apid_op->key_fields;
}

static inline IC_READ_KEY_OP
ic_get_read_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == KEY_READ_OPERATION);
  return apid_op->read_key_op;
}

static inline IC_WRITE_KEY_OP
ic_get_write_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == KEY_WRITE_OPERATION);
  return apid_op->write_key_op;
}

static inline IC_SCAN_OP
ic_get_scan_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == SCAN_OPERATION);
  return apid_op->scan_op;
}

static inline IC_RANGE_CONDITION*
ic_get_range_condition(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == SCAN_OPERATION);
  return apid_op->range_cond;
}

static inline IC_WHERE_CONDITION*
ic_get_where_condition(IC_APID_OPERATION *ext_apid_op)
{
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  g_assert(apid_op->op_type == SCAN_OPERATION ||
           apid_op->op_type == KEY_READ_OPERATION ||
           apid_op->op_type == KEY_WRITE_OPERATION);
  return apid_op->where_cond;
}
#endif
