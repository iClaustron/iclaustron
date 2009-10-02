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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_string.h>
#include <ic_dyn_array.h>
#include <ic_sock_buf.h>
#include <ic_connection.h>
#include <ic_poll_set.h>
#include <ic_threadpool.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_apid_int.h"

static int
define_field(IC_APID_OPERATION *apid_op,
             guint32 index,
             guint32 field_id,
             guint32 buffer_offset,
             guint32 null_offset,
             gboolean key_field,
             IC_FIELD_TYPE field_type)
{
  (void)apid_op;
  (void)index;
  (void)field_id;
  (void)buffer_offset;
  (void)null_offset;
  (void)key_field;
  (void)field_type;
  return 0;
}

static int
define_pos(IC_APID_OPERATION *apid_op,
           guint32 start_pos,
           guint32 end_pos)
{
  (void)apid_op;
  (void)start_pos;
  (void)end_pos;
  return 0;
}

static int
define_alloc_size(IC_APID_OPERATION *apid_op,
                  guint32 alloc_size)
{
  (void)apid_op;
  (void)alloc_size;
  return 0;
}

static int
multi_range(IC_APID_OPERATION *apid_op,
            guint32 num_ranges)
{
  (void)apid_op;
  (void)num_ranges;
  return 0;
}

static int
define_range_part(IC_APID_OPERATION *apid_op,
                  guint32 range_id,
                  guint32 field_id,
                  gchar *start_ptr,
                  guint32 start_len,
                  gchar *end_ptr,
                  guint32 end_len,
                  IC_RANGE_TYPE range_type)
{
  (void)apid_op;
  (void)range_id;
  (void)field_id;
  (void)start_ptr;
  (void)start_len;
  (void)end_ptr;
  (void)end_len;
  (void)range_type;
  return 0;
}

static int
keep_ranges(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
  return 0;
}

static int
define_condition(IC_APID_OPERATION *apid_op,
                 guint32 *condition_id,
                 guint32 left_memory_address,
                 guint32 right_memory_address,
                 IC_COMPARATOR_TYPE comp_type)
{
  (void)apid_op;
  (void)condition_id;
  (void)left_memory_address;
  (void)right_memory_address;
  (void)comp_type;
  return 0;
}

static int
read_field_into_memory(IC_APID_OPERATION *apid_op,
                       guint32 *memory_address,
                       guint32 field_id)
{
  (void)apid_op;
  (void)memory_address;
  (void)field_id;
  return 0;
}

static int
read_const_into_memory(IC_APID_OPERATION *apid_op,
                       guint32 *memory_address,
                       gchar *const_ptr,
                       guint32 const_len,
                       IC_FIELD_TYPE const_type)
{
  (void)apid_op;
  (void)memory_address;
  (void)const_ptr;
  (void)const_len;
  (void)const_type;
  return 0;
}

static int
define_boolean(IC_APID_OPERATION *apid_op,
               guint32 *result_condition_id,
               guint32 left_condition_id,
               guint32 right_condition_id,
               IC_BOOLEAN_TYPE boolean_type)
{
  (void)apid_op;
  (void)result_condition_id;
  (void)left_condition_id;
  (void)right_condition_id;
  (void)boolean_type;
  return 0;
}

static int
set_partition_id(IC_APID_OPERATION *apid_op,
                 guint32 partition_id)
{
  (void)apid_op;
  (void)partition_id;
  return 0;
}

static IC_APID_ERROR*
get_error_object(IC_APID_OPERATION *apid_op)
{
  (void)apid_op;
  return NULL;
}

static void
free_apid_op(IC_APID_OPERATION *apid_op)
{
  if (apid_op)
    ic_free(apid_op);
}

static IC_APID_OPERATION_OPS glob_apid_ops =
{
  .ic_define_field            = define_field,
  .ic_define_pos              = define_pos,
  .ic_define_alloc_size       = define_alloc_size,
  .ic_multi_range             = multi_range,
  .ic_define_range_part       = define_range_part,
  .ic_keep_ranges             = keep_ranges,
  .ic_define_condition        = define_condition,
  .ic_read_field_into_memory  = read_field_into_memory,
  .ic_read_const_into_memory  = read_const_into_memory,
  .ic_define_boolean          = define_boolean,
  .ic_set_partition_id        = set_partition_id,
  .ic_get_error_object        = get_error_object,
  .ic_free_apid_op            = free_apid_op
};

IC_APID_OPERATION*
ic_create_apid_operation(IC_TABLE_DEF *table_def,
                         guint32 num_fields,
                         gchar *buffer_ptr,
                         guint32 buffer_size,
                         guint8 *null_ptr,
                         guint32 num_null_bits,
                         gboolean full_table_op)
{
  IC_INT_APID_OPERATION *apid_op;
  gchar *loc_alloc;
  guint32 tot_size;
  guint32 i;

  tot_size=
    sizeof(IC_INT_APID_OPERATION) +
    sizeof(IC_FIELD_BIND) + 
    num_fields * (
      sizeof(IC_FIELD_DEF*) + sizeof(IC_FIELD_DEF));
  if ((loc_alloc= ic_calloc(tot_size)))
    return NULL;
  apid_op= (IC_INT_APID_OPERATION*)loc_alloc;
  apid_op->fields= (IC_FIELD_BIND*)(loc_alloc + sizeof(IC_INT_APID_OPERATION));
  apid_op->fields->field_defs= (IC_FIELD_DEF**)(loc_alloc + sizeof(IC_FIELD_BIND));
  loc_alloc+= (num_fields * sizeof(IC_FIELD_DEF*));
  for (i= 0; i < num_fields; i++)
  {
    apid_op->fields->field_defs[i]= (IC_FIELD_DEF*)loc_alloc;
    loc_alloc+= sizeof(IC_FIELD_DEF);
  }
  apid_op->apid_op_ops= &glob_apid_ops;
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
  apid_op->table_def= table_def;
  /* Assign fields object */
  apid_op->fields->num_fields= num_fields;
  apid_op->fields->buffer_size= buffer_size;
  apid_op->fields->num_null_bits= num_null_bits;
  apid_op->fields->buffer_ptr= buffer_ptr;
  apid_op->fields->null_ptr= null_ptr;
  /* Assign key fields object */
  apid_op->key_fields->num_fields= num_fields;
  apid_op->key_fields->buffer_size= buffer_size;
  apid_op->key_fields->num_null_bits= num_null_bits;
  apid_op->key_fields->buffer_ptr= buffer_ptr;
  apid_op->key_fields->null_ptr= null_ptr;
  return (IC_APID_OPERATION*)apid_op;
}
