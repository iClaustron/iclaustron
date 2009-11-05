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

/*
  MODULE: Definition of Range Condition Module
  --------------------------------------------
  This module is used to define the range condition used in queries.
*/
static int
range_multi_range(IC_RANGE_CONDITION *range,
                  guint32 num_ranges)
{
  (void)range;
  (void)num_ranges;
  return 0;
}

static int
range_define_range_part(IC_RANGE_CONDITION *range,
                        guint32 range_id,
                        guint32 field_id,
                        gchar *start_ptr,
                        guint32 start_len,
                        gchar *end_ptr,
                        guint32 end_len,
                        IC_LOWER_RANGE_TYPE lower_range_type,
                        IC_UPPER_RANGE_TYPE upper_range_type)
{
  (void)range;
  (void)range_id;
  (void)field_id;
  (void)start_ptr;
  (void)start_len;
  (void)end_ptr;
  (void)end_len;
  (void)lower_range_type;
  (void)upper_range_type;
  return 0;
}

static void
range_free(IC_RANGE_CONDITION *range)
{
  (void)range;
}

static IC_RANGE_CONDITION_OPS glob_range_ops =
{
  .ic_multi_range             = range_multi_range,
  .ic_define_range_part       = range_define_range_part,
  .ic_free_range_cond         = range_free
};

/*
  MODULE: Define WHERE condition
  ------------------------------
  This module contains all the methods used to define a WHERE condition
  in the iClaustron Data API. The WHERE condition is mapped into a
  Data API operation object before the query is defined as part of a
  transaction.
*/
static int
where_define_boolean(IC_WHERE_CONDITION *cond,
                     guint32 current_subroutine_id,
                     guint32 *left_subroutine_id,
                     guint32 *right_subroutine_id,
                     IC_BOOLEAN_TYPE boolean_type)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)left_subroutine_id;
  (void)right_subroutine_id;
  (void)boolean_type;
  return 0;
}

static int
where_define_condition(IC_WHERE_CONDITION *cond,
                       guint32 current_subroutine_id,
                       guint32 left_memory_address,
                       guint32 right_memory_address,
                       IC_COMPARATOR_TYPE comp_type)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)left_memory_address;
  (void)right_memory_address;
  (void)comp_type;
  return 0;
}

static int
where_read_field_into_memory(IC_WHERE_CONDITION *cond,
                             guint32 current_subroutine_id,
                             guint32 *memory_address,
                             guint32 field_id)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)memory_address;
  (void)field_id;
  return 0;
}

static int
where_read_const_into_memory(IC_WHERE_CONDITION *cond,
                             guint32 current_subroutine_id,
                             guint32 *memory_address,
                             gchar *const_ptr,
                             guint32 const_len,
                             IC_FIELD_TYPE const_type)
{
  (void)cond;
  (void)memory_address;
  (void)current_subroutine_id;
  (void)const_ptr;
  (void)const_len;
  (void)const_type;
  return 0;
}

static int
where_define_calculation(IC_WHERE_CONDITION *cond,
                         guint32 current_subroutine_id,
                         guint32 *returned_memory_address,
                         guint32 left_memory_address,
                         guint32 right_memory_address,
                         IC_CALCULATION_TYPE calc_type)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)returned_memory_address;
  (void)left_memory_address;
  (void)right_memory_address;
  (void)calc_type;
  return 0;
}

static int
where_define_not(IC_WHERE_CONDITION *cond,
                 guint32 current_subroutine_id,
                 guint32 *subroutine_id)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)subroutine_id;
  return 0;
}

static int
where_define_first(IC_WHERE_CONDITION *cond,
                   guint32 current_subroutine_id,
                   guint32 *subroutine_id)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)subroutine_id;
  return 0;
}

static int
where_define_regexp(IC_WHERE_CONDITION *cond,
                    guint32 current_subroutine_id,
                    guint32 field_id,
                    guint32 start_pos,
                    guint32 end_pos,
                    guint32 reg_exp_memory_address)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)field_id;
  (void)start_pos;
  (void)end_pos;
  (void)reg_exp_memory_address;
  return 0;
}

static int
where_define_like(IC_WHERE_CONDITION *cond,
                  guint32 current_subroutine_id,
                  guint32 field_id,
                  guint32 start_pos,
                  guint32 end_pos,
                  guint32 like_memory_address)
{
  (void)cond;
  (void)current_subroutine_id;
  (void)field_id;
  (void)start_pos;
  (void)end_pos;
  (void)like_memory_address;
  return 0;
}

static int
where_evaluate(IC_WHERE_CONDITION *cond)
{
  (void)cond;
  return 0;
}

static int
where_store(IC_WHERE_CONDITION *cond,
            guint32 cluster_id)
{
  (void)cond;
  (void)cluster_id;
  return 0;
}

static int
where_free(IC_WHERE_CONDITION *cond)
{
  (void)cond;
  return 0;
}

static IC_WHERE_CONDITION_OPS glob_cond_ops =
{
  .ic_define_boolean          = where_define_boolean,
  .ic_define_condition        = where_define_condition,
  .ic_read_field_into_memory  = where_read_field_into_memory,
  .ic_read_const_into_memory  = where_read_const_into_memory,
  .ic_define_calculation      = where_define_calculation,
  .ic_define_not              = where_define_not,
  .ic_define_first            = where_define_first,
  .ic_define_regexp           = where_define_regexp,
  .ic_define_like             = where_define_like,
  .ic_evaluate_where          = where_evaluate,
  .ic_store_where             = where_store,
  .ic_free_where              = where_free
};

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
apid_op_define_field(IC_APID_OPERATION *apid_op,
                     guint32 field_id,
                     guint32 buffer_offset,
                     guint32 null_offset)
{
  (void)apid_op;
  (void)field_id;
  (void)buffer_offset;
  (void)null_offset;
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

IC_APID_OPERATION*
ic_create_apid_operation(IC_APID_GLOBAL *apid_global,
                         IC_TABLE_DEF *ext_table_def,
                         guint32 num_fields,
                         gchar *data_buffer,
                         guint32 data_buffer_size,
                         guint8 *null_buffer,
                         guint32 null_buffer_size)
{
  IC_INT_APID_OPERATION *apid_op;
  IC_INT_TABLE_DEF *table_def= (IC_INT_TABLE_DEF*)ext_table_def;
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
  apid_op->table_def= (IC_TABLE_DEF*)table_def;
  apid_op->buffer_ptr= data_buffer;
  apid_op->null_ptr= null_buffer;
  apid_op->buffer_size= data_buffer_size;
  apid_op->max_null_bits= null_buffer_size * 8;
  apid_op->apid_global= (IC_INT_APID_GLOBAL*)apid_global;

  /* Assign fields object */
  if (!num_fields)
    num_fields= table_def->num_fields;
  apid_op->fields->num_fields= num_fields;
  /* Assign key fields object */
  apid_op->key_fields->num_fields= table_def->num_key_fields;
  return (IC_APID_OPERATION*)apid_op;
}
