/* Copyright (C) 2009-2013 iClaustron AB

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
typedef struct ic_hidden_apid_query IC_HIDDEN_APID_QUERY;

typedef struct ic_field_bind IC_FIELD_BIND;
typedef struct ic_key_field_bind IC_KEY_FIELD_BIND;
typedef enum ic_apid_query_type IC_APID_QUERY_TYPE;

/*
  The basic query types we support are scan, read using key
  and the write using a key.
*/
enum ic_apid_query_type
{
  IC_SCAN_QUERY= 0,
  IC_KEY_READ_QUERY= 1,
  IC_KEY_WRITE_QUERY= 2,
  IC_COMMIT_TRANSACTION= 3,
  IC_ROLLBACK_TRANSACTION= 4,
  IC_CREATE_SAVEPOINT= 5,
  IC_ROLLBACK_SAVEPOINT= 6
};

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
  IC_TRANSACTION_OPS *trans_ops;
  guint64 transaction_id;
  /* Hidden part */
  IC_COMMIT_STATE commit_state;
  IC_SAVEPOINT_ID savepoint_stable;
  IC_SAVEPOINT_ID savepoint_requested;
};

struct ic_hidden_apid_query
{
  /* Public part */
  IC_APID_QUERY_OPS *apid_query_ops;
  gboolean any_error;
  /* Hidden part */
  union
  {
    IC_READ_KEY_QUERY_TYPE read_key_query_type;
    IC_WRITE_KEY_QUERY_TYPE write_key_query_type;
    IC_SCAN_QUERY_TYPE scan_query_type;
  };
  guint32 num_cond_assignment_ids;
  IC_APID_QUERY_TYPE query_type;

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

struct ic_hidden_table_def
{
  /* Public part */
  IC_TABLE_DEF_OPS *table_def_ops;
  /* Hidden part */
};
#endif
