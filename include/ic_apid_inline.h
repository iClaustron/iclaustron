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
  MODULE: Inline functions for IC_TRANSACTION object
  --------------------------------------------------------
*/
IC_INLINE IC_COMMIT_STATE
ic_get_commit_state(IC_TRANSACTION *transaction)
{
  IC_HIDDEN_TRANSACTION *h_trans= (IC_HIDDEN_TRANSACTION*)transaction;
  return h_trans->commit_state;
}

IC_INLINE IC_SAVEPOINT_ID
ic_get_stable_savepoint(IC_TRANSACTION *transaction)
{
  IC_HIDDEN_TRANSACTION *h_trans= (IC_HIDDEN_TRANSACTION*)transaction;
  return h_trans->savepoint_stable;
}

IC_INLINE IC_SAVEPOINT_ID
ic_get_requested_savepoint(IC_TRANSACTION *transaction)
{
  IC_HIDDEN_TRANSACTION *h_trans= (IC_HIDDEN_TRANSACTION*)transaction;
  return h_trans->savepoint_requested;
}

IC_INLINE void
ic_get_transaction_id(IC_TRANSACTION *transaction,
                      guint32 trans_id[2])
{
  trans_id[0]= (guint32)(transaction->transaction_id >> 32);
  trans_id[1]= (guint32)(transaction->transaction_id & 0xFFFFFFFF);
}

/*
  MODULE: Inline functions for IC_APID_OPERATION classes
  ------------------------------------------------------
*/
IC_INLINE IC_APID_OPERATION_TYPE
ic_get_apid_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  return apid_op->op_type;
}

IC_INLINE IC_APID_CONNECTION*
ic_get_apid_connection_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  return apid_op->apid_conn;
}

IC_INLINE IC_TRANSACTION*
ic_get_transaction_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  return apid_op->trans_obj;
}

IC_INLINE IC_TABLE_DEF*
ic_get_table_definition_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_KEY_READ_OPERATION ||
            apid_op->op_type == IC_SCAN_OPERATION ||
            apid_op->op_type == IC_KEY_WRITE_OPERATION);
  return apid_op->table_def;
}

IC_INLINE IC_APID_ERROR*
ic_get_error_object_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  return apid_op->error;
}

IC_INLINE gboolean
ic_is_failed_operation(IC_APID_OPERATION *apid_op)
{
  return apid_op->any_error;
}
IC_INLINE void*
ic_get_user_reference_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  return apid_op->user_reference;
}

IC_INLINE IC_FIELD_BIND*
ic_get_field_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_KEY_READ_OPERATION ||
            apid_op->op_type == IC_KEY_WRITE_OPERATION ||
            apid_op->op_type == IC_SCAN_OPERATION);
  return apid_op->fields;
}

IC_INLINE IC_KEY_FIELD_BIND*
ic_get_key_fields_from_operation(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_KEY_WRITE_OPERATION ||
            apid_op->op_type == IC_KEY_READ_OPERATION);
  return apid_op->key_fields;
}

IC_INLINE IC_READ_KEY_OP
ic_get_read_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_KEY_READ_OPERATION);
  return apid_op->read_key_op;
}

IC_INLINE IC_WRITE_KEY_OP
ic_get_write_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_KEY_WRITE_OPERATION);
  return apid_op->write_key_op;
}

IC_INLINE IC_SCAN_OP
ic_get_scan_operation_type(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_SCAN_OPERATION);
  return apid_op->scan_op;
}

IC_INLINE IC_RANGE_CONDITION*
ic_get_range_condition(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_SCAN_OPERATION);
  return apid_op->range_cond;
}

IC_INLINE IC_WHERE_CONDITION*
ic_get_where_condition(IC_APID_OPERATION *ext_apid_op)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_SCAN_OPERATION ||
           apid_op->op_type == IC_KEY_READ_OPERATION ||
           apid_op->op_type == IC_KEY_WRITE_OPERATION);
  return apid_op->where_cond;
}

IC_INLINE IC_CONDITIONAL_ASSIGNMENT*
ic_get_conditional_assignment(IC_APID_OPERATION *ext_apid_op,
                              guint32 cond_assignment_id)
{
  IC_HIDDEN_APID_OPERATION *apid_op= (IC_HIDDEN_APID_OPERATION*)ext_apid_op;
  ic_assert(apid_op->op_type == IC_KEY_WRITE_OPERATION &&
            apid_op->write_key_op == IC_KEY_UPDATE);
  ic_assert(apid_op->num_cond_assignment_ids > cond_assignment_id);
  return apid_op->cond_assign[cond_assignment_id];
}
#endif
