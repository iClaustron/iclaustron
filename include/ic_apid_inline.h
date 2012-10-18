/* Copyright (C) 2009-2012 iClaustron AB

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
  MODULE: Inline functions for IC_APID_QUERY classes
  --------------------------------------------------
*/
IC_INLINE IC_APID_QUERY_TYPE
ic_get_apid_query_type(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  return apid_query->query_type;
}

IC_INLINE IC_APID_CONNECTION*
ic_get_apid_connection_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  return apid_query->apid_conn;
}

IC_INLINE IC_TRANSACTION*
ic_get_transaction_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  return apid_query->trans_obj;
}

IC_INLINE IC_TABLE_DEF*
ic_get_table_definition_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_KEY_READ_QUERY ||
            apid_query->query_type == IC_SCAN_QUERY ||
            apid_query->query_type == IC_KEY_WRITE_QUERY);
  return apid_query->table_def;
}

IC_INLINE IC_APID_ERROR*
ic_get_error_object_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  return apid_query->error;
}

IC_INLINE gboolean
ic_is_failed_query(IC_APID_QUERY *apid_query)
{
  return apid_query->any_error;
}
IC_INLINE void*
ic_get_user_reference_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  return apid_query->user_reference;
}

IC_INLINE IC_FIELD_BIND*
ic_get_field_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_KEY_READ_QUERY ||
            apid_query->query_type == IC_KEY_WRITE_QUERY ||
            apid_query->query_type == IC_SCAN_QUERY);
  return apid_query->fields;
}

IC_INLINE IC_KEY_FIELD_BIND*
ic_get_key_fields_from_query(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_KEY_WRITE_QUERY ||
            apid_query->query_type == IC_KEY_READ_QUERY);
  return apid_query->key_fields;
}

IC_INLINE IC_READ_KEY_QUERY_TYPE
ic_get_read_query_type(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_KEY_READ_QUERY);
  return apid_query->read_key_query_type;
}

IC_INLINE IC_WRITE_KEY_QUERY_TYPE
ic_get_write_query_type(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_KEY_WRITE_QUERY);
  return apid_query->write_key_query_type;
}

IC_INLINE IC_SCAN_QUERY_TYPE
ic_get_scan_query_type(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_SCAN_QUERY);
  return apid_query->scan_query_type;
}

IC_INLINE IC_RANGE_CONDITION*
ic_get_range_condition(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_SCAN_QUERY);
  return apid_query->range_cond;
}

IC_INLINE IC_WHERE_CONDITION*
ic_get_where_condition(IC_APID_QUERY *ext_apid_query)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_SCAN_QUERY ||
           apid_query->query_type == IC_KEY_READ_QUERY ||
           apid_query->query_type == IC_KEY_WRITE_QUERY);
  return apid_query->where_cond;
}

IC_INLINE IC_CONDITIONAL_ASSIGNMENT*
ic_get_conditional_assignment(IC_APID_QUERY *ext_apid_query,
                              guint32 cond_assignment_id)
{
  IC_HIDDEN_APID_QUERY *apid_query= (IC_HIDDEN_APID_QUERY*)ext_apid_query;
  ic_assert(apid_query->query_type == IC_KEY_WRITE_QUERY &&
            apid_query->write_key_query_type == IC_KEY_UPDATE);
  ic_assert(apid_query->num_cond_assignment_ids > cond_assignment_id);
  return apid_query->cond_assign[cond_assignment_id];
}
#endif
