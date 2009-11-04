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
typedef struct ic_hidden_transaction IC_HIDDEN_TRANSACTION;
typedef struct ic_hidden_apid_operation IC_HIDDEN_APID_OPERATION;

/*
  This is part of the external interface for efficiency reasons.
  However this part is defined to not be stable. The stable interface
  is always methods. In many cases those are inline methods and
  for efficiency reason their implementation is visible for the user of
  the API. However no implementation on top of the API should use this
  information.
*/
struct ic_hidden_transaction
{
  /* Public part */
  IC_TRANSACTION_OPS trans_ops;
  guint64 transaction_id;
  /* Hidden part */
  IC_COMMIT_STATE commit_state;
  IC_SAVEPOINT_ID savepoint_stable;
  IC_SAVEPOINT_ID savepoint_requested;
};

struct ic_hidden_apid_operation
{
  /* Public part */
  IC_APID_OPERATION_OPS *apid_op_ops;
  gboolean any_error;
  /* Hidden part */
  union
  {
    IC_READ_KEY_OP read_key_op;
    IC_WRITE_KEY_OP write_key_op;
    IC_SCAN_OP scan_op;
  };
  guint32 num_cond_assignment_ids;
  IC_APID_OPERATION_TYPE op_type;

  IC_APID_CONNECTION *apid_conn;
  IC_TRANSACTION *trans_obj;
  IC_TABLE_DEF *table_def;

  IC_WHERE_CONDITION *where_cond;
  IC_RANGE_CONDITION *range_cond;
  IC_CONDITIONAL_ASSIGNMENT **cond_assign;

  IC_APID_ERROR *error;
  void *user_reference;

  IC_KEY_FIELD_BIND *key_fields;
  IC_FIELD_BIND *fields;
  gchar *buffer_ptr;
  guint8 *null_ptr;
};
#endif
