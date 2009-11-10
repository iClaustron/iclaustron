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
  MODULE: Define Conditional Assignments module
  ---------------------------------------------
  This module contains the methods needed to define conditional assignments
  used in write operation.
*/
static IC_WHERE_CONDITION*
assign_create_where_condition(IC_CONDITIONAL_ASSIGNMENT *cond_assign)
{
  (void)cond_assign;
  return NULL;
}

static int
assign_map_condition(IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                     IC_APID_GLOBAL *apid_global,
                     guint32 where_cond_id)
{
  (void)cond_assign;
  (void)apid_global;
  (void)where_cond_id;
  return 0;
}

static int
assign_read_field_into_memory(IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                              guint32 *memory_address,
                              guint32 field_id)
{
  (void)cond_assign;
  (void)memory_address;
  (void)field_id;
  return 0;
}

static int
assign_read_const_into_memory(IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                              guint32 *memory_address,
                              gchar *const_ptr,
                              guint32 const_len,
                              IC_FIELD_TYPE const_type)
{
  (void)cond_assign;
  (void)memory_address;
  (void)const_ptr;
  (void)const_len;
  (void)const_type;
  return 0;
}

static int
assign_define_calculation(IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                          guint32 *returned_memory_address,
                          guint32 left_memory_address,
                          guint32 right_memory_address,
                          IC_CALCULATION_TYPE calc_type)
{
  (void)cond_assign;
  (void)returned_memory_address;
  (void)left_memory_address;
  (void)right_memory_address;
  (void)calc_type;
  return 0;
}

static int
assign_write_field_into_memory(IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                               guint32 memory_address,
                               guint32 field_id)
{
  (void)cond_assign;
  (void)memory_address;
  (void)field_id;
  return 0;
}

static int
assign_free(IC_CONDITIONAL_ASSIGNMENT** cond_assigns)
{
  (void)cond_assigns;
  return 0;
}

static IC_CONDITIONAL_ASSIGNMENT_OPS glob_cond_assign_ops =
{
  .ic_create_assignment_condition     = assign_create_where_condition,
  .ic_map_assignment_condition        = assign_map_condition,
  .ic_read_field_into_memory          = assign_read_field_into_memory,
  .ic_read_const_into_memory          = assign_read_const_into_memory,
  .ic_define_calculation              = assign_define_calculation,
  .ic_write_field_into_memory         = assign_write_field_into_memory,
  .ic_free_cond_assign                = assign_free
};

/*
  MODULE: Define API operation object module
  ------------------------------------------
  This module contains methods to define an Data API operation object.
*/
static int
compare_field_in_op(const void *a, const void *b)
{
  guint32 left_field_id= ((IC_FIELD_IN_OP*)a)->field_id;
  guint32 right_field_id= ((IC_FIELD_IN_OP*)b)->field_id;
  if (left_field_id > right_field_id)
    return +1;
  else if (left_field_id < right_field_id)
    return -1;
  return 0;
}

static int
organize_field_in_op_array(IC_INT_APID_OPERATION *apid_op,
                           IC_INT_TABLE_DEF *table_def)
{
  guint32 num_fields= apid_op->num_fields;
  guint32 field_id_compare= IC_MAX_UINT32;
  guint32 num_key_fields= table_def->num_key_fields;
  guint32 i, j, field_id, key_field_id, key_fields;

  qsort(apid_op->fields,
        num_fields,
        sizeof(IC_FIELD_IN_OP*),
        compare_field_in_op);
  for (i= 0; i < num_fields; i++)
  {
    field_id= apid_op->fields[i]->field_id;
    if (field_id_compare == field_id)
      return IC_ERROR_DUPLICATE_FIELD_IDS;
    field_id_compare= field_id;
    if (ic_is_bitmap_set(table_def->key_fields,field_id))
    {
      key_field_id= IC_MAX_UINT32;
      for (j= 0; j < num_key_fields; j++)
      {
        if (table_def->key_field_id_order[j] == field_id)
          key_field_id= j;
      }
      ic_require(key_field_id < num_key_fields);
      apid_op->key_fields[key_field_id]= apid_op->fields[i];
      key_fields++;
    }
  }
  if (key_fields == num_key_fields)
  {
    apid_op->is_all_key_fields_defined= TRUE;
    apid_op->num_key_fields= num_key_fields;
  }
  return 0;
}

static int
apid_op_define_field(IC_APID_OPERATION *ext_apid_op,
                     guint32 field_id,
                     guint32 buffer_offset,
                     guint32 null_offset)
{
  IC_FIELD_DEF *field_def;
  IC_FIELD_IN_OP *field_in_op;
  IC_INT_APID_OPERATION *apid_op= (IC_INT_APID_OPERATION*)ext_apid_op;
  IC_INT_TABLE_DEF *table_def;
  guint32 num_fields_defined= apid_op->num_fields_defined;
  int ret_code;

  table_def= (IC_INT_TABLE_DEF*)apid_op->table_def;
  if (field_id > table_def->num_fields ||
      num_fields_defined >= apid_op->num_fields)
    return IC_ERROR_TOO_MANY_FIELDS;
  field_def= table_def->fields[field_id];
  field_in_op= apid_op->fields[num_fields_defined];
  num_fields_defined++;
  apid_op->num_fields_defined= num_fields_defined;
  if (!apid_op->is_buffer_allocated_by_api)
  {
    field_in_op->data_offset=  buffer_offset;
    field_in_op->null_offset= null_offset;
  }
  field_in_op->field_id= field_id;
  if (num_fields_defined == apid_op->num_fields)
  {
    /* Last field defined, organise fields data */
    if ((ret_code= organize_field_in_op_array(apid_op, table_def)))
      return ret_code;
  }
  return 0;
}

static int
apid_op_define_pos(IC_APID_OPERATION *apid_op,
                   guint32 field_id,
                   gchar *field_data,
                   gboolean use_full_field,
                   guint32 start_pos,
                   guint32 end_pos)
{
  (void)apid_op;
  (void)field_id;
  (void)field_data;
  (void)use_full_field;
  (void)start_pos;
  (void)end_pos;
  return 0;
}

static int
apid_op_transfer_ownership(IC_APID_OPERATION *apid_op,
                           guint32 field_id)
{
  (void)apid_op;
  (void)field_id;
  return 0;
}

static int
apid_op_set_partition_ids(IC_APID_OPERATION *apid_op,
                          IC_BITMAP *used_partitions)
{
  (void)apid_op;
  (void)used_partitions;
  return 0;
}

static int
apid_op_set_partition_id(IC_APID_OPERATION *apid_op,
                         guint32 partition_id)
{
  (void)apid_op;
  (void)partition_id;
  return 0;
}

static IC_RANGE_CONDITION*
apid_op_create_range_condition(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
  return 0;
}

static void
apid_op_keep_range(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
}

static IC_WHERE_CONDITION*
apid_op_create_where_condition(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
  return NULL;
}

static int
apid_op_map_where_condition(IC_APID_OPERATION *apid_op,
                            IC_APID_GLOBAL *apid_global,
                            guint32 where_cond_id)
{
  (void)apid_op;
  (void)apid_global;
  (void)where_cond_id;
  return 0;
}

static void
apid_op_keep_where(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
}

static IC_CONDITIONAL_ASSIGNMENT**
apid_op_create_conditional_assignments(IC_APID_OPERATION *apid_op,
                                       guint32 num_cond_assigns)
{
  (void)apid_op;
  (void)num_cond_assigns;
  return NULL;
}

static IC_CONDITIONAL_ASSIGNMENT*
apid_op_create_conditional_assignment(IC_APID_OPERATION *apid_op,
                                      guint32 cond_assign_id)
{
  (void)apid_op;
  (void)cond_assign_id;
  return NULL;
}

static int
apid_op_map_conditional_assignment(IC_APID_OPERATION *apid_op,
                                   IC_APID_GLOBAL *apid_global,
                                   guint32 loc_cond_assign_id,
                                   guint32 glob_cond_assign_id)
{
  (void)apid_op;
  (void)apid_global;
  (void)loc_cond_assign_id;
  (void)glob_cond_assign_id;
  return 0;
}

static void
apid_op_keep_conditional_assignment(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
}

static IC_APID_ERROR*
apid_op_get_error_object(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
  return NULL;
}

static int
apid_op_reset(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
  return 0;
}

static void
apid_op_free(IC_APID_OPERATION *apid_op)
{
  if (apid_op)
    ic_free(apid_op);
}

static IC_APID_OPERATION_OPS glob_apid_ops =
{
  .ic_define_field            = apid_op_define_field,
  .ic_define_pos              = apid_op_define_pos,
  .ic_transfer_ownership      = apid_op_transfer_ownership,
  .ic_set_partition_id        = apid_op_set_partition_id,
  .ic_set_partition_ids       = apid_op_set_partition_ids,
  .ic_create_range_condition  = apid_op_create_range_condition,
  .ic_keep_range              = apid_op_keep_range,
  .ic_create_where_condition  = apid_op_create_where_condition,
  .ic_map_where_condition     = apid_op_map_where_condition,
  .ic_keep_where              = apid_op_keep_where,
  .ic_create_conditional_assignments = apid_op_create_conditional_assignments,
  .ic_create_conditional_assignment = apid_op_create_conditional_assignment,
  .ic_map_conditional_assignment = apid_op_map_conditional_assignment,
  .ic_keep_conditional_assignment = apid_op_keep_conditional_assignment,
  .ic_get_error_object        = apid_op_get_error_object,
  .ic_reset_apid_op           = apid_op_reset,
  .ic_free_apid_op            = apid_op_free
};

static void
set_up_fields_in_apid_op(IC_INT_APID_OPERATION *apid_op,
                         IC_INT_TABLE_DEF *table_def)
{
  guint32 i;
  int ret_code;
  for (i= 0; i < table_def->num_fields; i++)
  {
    ret_code= apid_op_define_field((IC_APID_OPERATION*)apid_op, i, 0, 0);
    ic_require(!ret_code);
  }
}

IC_APID_OPERATION*
ic_create_apid_operation(IC_APID_GLOBAL *apid_global,
                         IC_TABLE_DEF *ext_table_def,
                         guint32 num_fields,
                         gboolean is_buffer_allocated_by_api,
                         gchar *data_buffer,
                         guint32 data_buffer_size,
                         guint8 *null_buffer,
                         guint32 null_buffer_size,
                         int *error)
{
  IC_INT_APID_OPERATION *apid_op;
  IC_INT_TABLE_DEF *table_def= (IC_INT_TABLE_DEF*)ext_table_def;
  gchar *loc_alloc;
  guint32 tot_size;
  guint32 i;
  guint32 num_key_fields= 0;

  if (!error)
    return NULL;
  if (num_fields > table_def->num_fields)
  {
    *error= IC_ERROR_TOO_MANY_FIELDS;
    return NULL;
  }
  if (!(data_buffer && null_buffer))
  {
    *error= IC_ERROR_BUFFER_MISSING_CREATE_APID_OP;
    return NULL;
  }

  if (!table_def->is_index || table_def->is_unique_index)
  {
    /*
      Allocate an area for key fields to make Key Read/Writes more efficient.
      Not necessary when using a non-unique index. Also not needed when
      number of fields is smaller than number of key fields obviously.
    */
    if (num_fields >= table_def->num_key_fields)
      num_key_fields= table_def->num_key_fields;
  }
  if (!num_fields) /* Use all fields if num_fields == 0 */
    num_fields= table_def->num_fields;
  tot_size=
    sizeof(IC_INT_APID_OPERATION) +
    sizeof(IC_FIELD_IN_OP*) * num_key_fields +
    num_fields * (
      sizeof(IC_FIELD_IN_OP*) + sizeof(IC_FIELD_IN_OP));
  if ((loc_alloc= ic_calloc(tot_size)))
  {
    *error= IC_ERROR_MEM_ALLOC;
    return NULL;
  }
  apid_op= (IC_INT_APID_OPERATION*)loc_alloc;
  apid_op->fields= (IC_FIELD_IN_OP**)
    (loc_alloc + sizeof(IC_INT_APID_OPERATION));
  loc_alloc+= (num_fields * sizeof(IC_FIELD_IN_OP*));
  if (num_key_fields)
  {
    apid_op->key_fields= (IC_FIELD_IN_OP**)loc_alloc;
    loc_alloc+= (num_key_fields * sizeof(IC_FIELD_IN_OP*));
  }
  for (i= 0; i < num_fields; i++)
  {
    apid_op->fields[i]= (IC_FIELD_IN_OP*)loc_alloc;
    loc_alloc+= sizeof(IC_FIELD_IN_OP);
  }

  /*
    apid_conn set per query
    trans_obj set per query
    where_cond set per query if used in query
    range_cond set per query if used in query
    op_type set per query
    read_key_op, write_key_op, scan_op union set per query
    user_reference set per query
    next_trans_op, prev_trans_op used to keep doubly linked list
    of operation records in transaction.
    next_conn_op, prev_conn_op used to keep doubly linked list of
    operation records on connection object.
  */
  apid_op->apid_op_ops= &glob_apid_ops;
  apid_op->table_def= (IC_TABLE_DEF*)table_def;
  apid_op->buffer_ptr= data_buffer;
  apid_op->null_ptr= null_buffer;
  apid_op->buffer_size= data_buffer_size;
  apid_op->max_null_bits= null_buffer_size * 8;
  apid_op->apid_global= (IC_INT_APID_GLOBAL*)apid_global;
  apid_op->is_buffer_allocated_by_api= is_buffer_allocated_by_api;
  apid_op->num_fields= num_fields;
  apid_op->num_key_fields= num_key_fields;

  if (is_buffer_allocated_by_api &&
      table_def->num_fields == num_fields)
  {
    set_up_fields_in_apid_op(apid_op, table_def);
  }
  *error= 0;
  return (IC_APID_OPERATION*)apid_op;
}

/*
  DATA API Connection Module
  --------------------------
  This module contains all the methods that are part of the Data API
  Connection interface. This interface contains methods to start
  transactions and control execution of transactions. Each thread
  should have its own Data API Connection module.
*/

static int
apid_conn_write_key(IC_APID_CONNECTION *apid_conn,
                    IC_APID_OPERATION *apid_op,
                    IC_TRANSACTION *trans_obj,
                    IC_WRITE_KEY_OP write_key_op,
                    IC_APID_CALLBACK_FUNC callback_func,
                    void *user_reference)
{
  (void)apid_conn;
  (void)apid_op;
  (void)trans_obj;
  (void)write_key_op;
  (void)callback_func;
  (void)user_reference;
  return 0;
}

static int
apid_conn_read_key(IC_APID_CONNECTION *apid_conn,
                   IC_APID_OPERATION *apid_op,
                   IC_TRANSACTION *trans_obj,
                   IC_READ_KEY_OP read_key_op,
                   IC_APID_CALLBACK_FUNC callback_func,
                   void *user_reference)
{
  (void)apid_conn;
  (void)apid_op;
  (void)trans_obj;
  (void)read_key_op;
  (void)callback_func;
  (void)user_reference;
  return 0;
}

static int
apid_conn_scan(IC_APID_CONNECTION *apid_conn,
               IC_APID_OPERATION *apid_op,
               IC_TRANSACTION *trans_obj,
               IC_SCAN_OP scan_op,
               IC_APID_CALLBACK_FUNC callback_func,
               void *user_reference)
{
  (void)apid_conn;
  (void)apid_op;
  (void)trans_obj;
  (void)scan_op;
  (void)callback_func;
  (void)user_reference;
  return 0;
}

static IC_APID_ERROR*
apid_conn_start_metadata_transaction(IC_APID_CONNECTION *apid_conn,
                                     IC_METADATA_TRANSACTION *md_trans_obj,
                                     guint32 cluster_id)
{
  (void)apid_conn;
  (void)md_trans_obj;
  (void)cluster_id;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_start_transaction(IC_APID_CONNECTION *apid_conn,
                            IC_TRANSACTION **trans_obj,
                            IC_TRANSACTION_HINT *transaction_hint,
                            guint32 cluster_id,
                            gboolean joinable)
{
  (void)apid_conn;
  (void)trans_obj;
  (void)transaction_hint;
  (void)cluster_id;
  (void)joinable;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_join_transaction(IC_APID_CONNECTION *apid_conn,
                           IC_TRANSACTION *trans_obj,
                           void *user_reference)
{
  (void)apid_conn;
  (void)trans_obj;
  (void)user_reference;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_commit_transaction(IC_APID_CONNECTION *apid_conn,
                             IC_TRANSACTION *trans_obj,
                             IC_APID_CALLBACK_FUNC callback_func,
                             void *user_reference)
{
  (void)apid_conn;
  (void)trans_obj;
  (void)callback_func;
  (void)user_reference;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_rollback_transaction(IC_APID_CONNECTION *apid_conn,
                               IC_TRANSACTION *trans_obj,
                               IC_APID_CALLBACK_FUNC callback_func,
                               void *user_reference)
{
  (void)apid_conn;
  (void)trans_obj;
  (void)callback_func;
  (void)user_reference;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_create_savepoint(IC_APID_CONNECTION *apid_conn,
                           IC_TRANSACTION *trans_obj,
                           IC_SAVEPOINT_ID *savepoint_id)
{
  (void)apid_conn;
  (void)trans_obj;
  (void)savepoint_id;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_rollback_savepoint(IC_APID_CONNECTION *apid_conn,
                             IC_TRANSACTION *trans_obj,
                             IC_SAVEPOINT_ID savepoint_id,
                             IC_APID_CALLBACK_FUNC callback_func,
                             void *user_reference)
{
  (void)apid_conn;
  (void)trans_obj;
  (void)savepoint_id;
  (void)callback_func;
  (void)user_reference;
  return NULL;
}

static IC_APID_ERROR*
apid_conn_flush(IC_APID_CONNECTION *apid_conn,
                glong wait_time,
                gboolean force_send)
{
  IC_APID_ERROR* apid_error;
  if (!(apid_error= ic_send_messages(apid_conn, force_send)))
    return apid_error;
  return ic_poll_messages(apid_conn, wait_time);
}

/*
  This method is only called after a successful call to flush or poll
*/
IC_APID_OPERATION*
apid_conn_get_next_executed_operation(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  IC_INT_APID_OPERATION *ret_op= apid_conn->first_executed_operation;
  IC_INT_APID_OPERATION *first_completed_op= apid_conn->first_completed_operation;

  if (ret_op == NULL)
    return NULL;
  g_assert(ret_op->list_type == IN_EXECUTED_LIST);
  apid_conn->first_executed_operation= ret_op->next_conn_op;
  apid_conn->first_completed_operation= ret_op;
  ret_op->next_conn_op= first_completed_op;
  ret_op->list_type= IN_COMPLETED_LIST;
  return (IC_APID_OPERATION*)ret_op;
}

IC_APID_GLOBAL*
apid_conn_get_apid_global(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  return (IC_APID_GLOBAL*)apid_conn->apid_global;
}

IC_API_CONFIG_SERVER*
apid_conn_get_api_config_server(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  return apid_conn->apic;
}

static void
apid_conn_free_apid_connection(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  IC_THREAD_CONNECTION *thread_conn= apid_conn->thread_conn;
  IC_GRID_COMM *grid_comm;
  IC_INT_APID_GLOBAL *apid_global;
  guint32 thread_id= apid_conn->thread_id;
  DEBUG_ENTRY("apid_free");

  apid_global= apid_conn->apid_global;
  grid_comm= apid_global->grid_comm;
  if (apid_conn)
  {
    if (apid_conn->cluster_id_bitmap)
      ic_free_bitmap(apid_conn->cluster_id_bitmap);
    if (apid_conn->trans_bindings)
    {
      apid_conn->trans_bindings->dt_ops.ic_free_dynamic_translation(
        apid_conn->trans_bindings);
    }
    if (apid_conn->op_bindings)
    {
      apid_conn->op_bindings->dt_ops.ic_free_dynamic_translation(
        apid_conn->op_bindings);
    }
    ic_free(apid_conn);
  }
  if (thread_conn)
  {
    thread_id= apid_conn->thread_id;
    if (thread_conn->mutex)
      g_mutex_free(thread_conn->mutex);
    if (thread_conn->cond)
      g_cond_free(thread_conn->cond);
    ic_free(thread_conn);
  }
  g_mutex_lock(apid_global->thread_id_mutex);
  if (thread_conn &&
      grid_comm->thread_conn_array[thread_id] == thread_conn)
    grid_comm->thread_conn_array[thread_id]= NULL;
  g_mutex_unlock(apid_global->thread_id_mutex);
}

static IC_APID_CONNECTION_OPS glob_apid_conn_ops=
{
  .ic_write_key              = apid_conn_write_key,
  .ic_read_key               = apid_conn_read_key,
  .ic_scan                   = apid_conn_scan,
  .ic_start_metadata_transaction = apid_conn_start_metadata_transaction,
  .ic_start_transaction      = apid_conn_start_transaction,
  .ic_join_transaction       = apid_conn_join_transaction,
  .ic_commit_transaction     = apid_conn_commit_transaction,
  .ic_rollback_transaction   = apid_conn_rollback_transaction,
  .ic_create_savepoint       = apid_conn_create_savepoint,
  .ic_rollback_savepoint     = apid_conn_rollback_savepoint,
  .ic_poll                   = ic_poll_messages,
  .ic_send                   = ic_send_messages,
  .ic_flush                  = apid_conn_flush,
  .ic_get_next_executed_operation = apid_conn_get_next_executed_operation,
  .ic_get_apid_global        = apid_conn_get_apid_global,
  .ic_get_api_config_server  = apid_conn_get_api_config_server,
  .ic_free_apid_connection   = apid_conn_free_apid_connection
};

static void
apid_error_init_apid_error(IC_APID_ERROR *ext_apid_error)
{
  IC_INT_APID_ERROR *apid_error= (IC_INT_APID_ERROR*)ext_apid_error;
  apid_error->error_msg= "No error";
  apid_error->error_code= 0;
  apid_error->error_category= IC_CATEGORY_NO_ERROR;
  apid_error->error_severity= IC_SEVERITY_NO_ERROR;
}

static int
apid_error_get_apid_error_code(IC_APID_ERROR *ext_apid_error)
{
  IC_INT_APID_ERROR *apid_error= (IC_INT_APID_ERROR*)ext_apid_error;
  return apid_error->error_code;
}

static const gchar*
apid_error_get_apid_error_msg(IC_APID_ERROR *ext_apid_error)
{
  IC_INT_APID_ERROR *apid_error= (IC_INT_APID_ERROR*)ext_apid_error;
  return (const gchar*)apid_error->error_msg;
}

static IC_ERROR_CATEGORY
apid_error_get_apid_error_category(IC_APID_ERROR *ext_apid_error)
{
  IC_INT_APID_ERROR *apid_error= (IC_INT_APID_ERROR*)ext_apid_error;
  return apid_error->error_category;
}

static IC_ERROR_SEVERITY_LEVEL
apid_error_get_apid_error_severity(IC_APID_ERROR *ext_apid_error)
{
  IC_INT_APID_ERROR *apid_error= (IC_INT_APID_ERROR*)ext_apid_error;
  return apid_error->error_severity;
}

static IC_APID_ERROR_OPS glob_apid_error_ops=
{
  .ic_init_apid_error              = apid_error_init_apid_error,
  .ic_get_apid_error_code          = apid_error_get_apid_error_code,
  .ic_get_apid_error_msg           = apid_error_get_apid_error_msg,
  .ic_get_apid_error_category      = apid_error_get_apid_error_category,
  .ic_get_apid_error_severity      = apid_error_get_apid_error_severity
};

IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *ext_apid_global,
                          IC_BITMAP *cluster_id_bitmap)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  guint32 thread_id= IC_MAX_THREAD_CONNECTIONS;
  IC_GRID_COMM *grid_comm;
  IC_THREAD_CONNECTION *thread_conn;
  IC_INT_APID_CONNECTION *apid_conn;
  guint32 i, num_bits;
  IC_API_CONFIG_SERVER *apic= apid_global->apic;
  DEBUG_ENTRY("ic_create_apid_connection");

  /* Initialise the APID connection object */
  if (!(apid_conn= (IC_INT_APID_CONNECTION*)
                    ic_calloc(sizeof(IC_INT_APID_CONNECTION))))
    goto end;
  apid_conn->apid_global= apid_global;
  apid_conn->thread_id= thread_id;
  apid_conn->apic= apic;
  num_bits= apic->api_op.ic_get_max_cluster_id(apic) + 1;
  if (!(apid_conn->cluster_id_bitmap= ic_create_bitmap(NULL, num_bits)))
    goto error;
  if (cluster_id_bitmap)
  {
    ic_bitmap_copy(apid_conn->cluster_id_bitmap, cluster_id_bitmap);
  }
  else
  {
    for (i= 0; i < num_bits; i++)
    {
    if (apic->api_op.ic_get_cluster_config(apic, i))
      ic_bitmap_set_bit(apid_conn->cluster_id_bitmap, i);
    }
  }

  if (!(apid_conn->trans_bindings= ic_create_dynamic_translation()))
    goto error;
  if (!(apid_conn->op_bindings= ic_create_dynamic_translation()))
    goto error;

  /* Initialise Thread Connection object */
  if (!(thread_conn= apid_conn->thread_conn= (IC_THREAD_CONNECTION*)
                      ic_calloc(sizeof(IC_THREAD_CONNECTION))))
    goto error;
  thread_conn->apid_conn= apid_conn;
  if (!(thread_conn->mutex= g_mutex_new()))
    goto error;
  if (!(thread_conn->cond= g_cond_new()))
    goto error;


  grid_comm= apid_global->grid_comm;
  g_mutex_lock(apid_global->thread_id_mutex);
  for (i= 0; i < IC_MAX_THREAD_CONNECTIONS; i++)
  {
    if (!grid_comm->thread_conn_array[i])
    {
      thread_id= i;
      break;
    }
  }
  if (thread_id == IC_MAX_THREAD_CONNECTIONS)
  {
    g_mutex_unlock(apid_global->thread_id_mutex);
    ic_free(apid_conn->thread_conn);
    apid_conn->thread_conn= NULL;
    goto error;
  }
  grid_comm->thread_conn_array[thread_id]= thread_conn;
  g_mutex_unlock(apid_global->thread_id_mutex);
  apid_conn->thread_id= thread_id;
  /* Now initialise the method pointers for the Data API interface */
  apid_conn->apid_conn_ops= &glob_apid_conn_ops;
  /* Now initialise error object */
  apid_error_init_apid_error((IC_APID_ERROR*)&apid_conn->apid_error);
  apid_conn->apid_error.apid_error_ops= &glob_apid_error_ops;
  return (IC_APID_CONNECTION*)apid_conn;

error:
  apid_conn_free_apid_connection((IC_APID_CONNECTION*)apid_conn);
end:
  return NULL;
}
