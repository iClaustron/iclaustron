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
static int
define_key_field_bind(IC_TABLE_DEF *table_def,
                      guint32 num_fields,
                      gchar *buffer_ptr,
                      guint64 buffer_size,
                      gchar *null_ptr,
                      guint32 null_area_size)
{
}

static int
define_read_field(IC_TABLE_DEF *table_def,
                  guint32 field_id,
                  gchar *buffer_ptr,
                  guint32 offset,
                  gchar *null_ptr,
                  guint32 null_offset)
{
}

static int
define_read_part_field(IC_TABLE_DEF *table_def,
                       guint32 field_id,
                       gchar *buffer_ptr,
                       guint32 offset,
                       gchar *null_ptr,
                       guint32 null_offset,
                       guint32 start_byte,
                       guint32 end_byte)
{
}

static int
define_key_field(IC_TABLE_DEF *table_def,
                 guint32 field_id,
                 gchar *buffer_ptr,
                 guint32 offset,
                 gchar *null_ptr,
                 guint32 null_offset)
{
}

static int
define_write_field(IC_TABLE_DEF *table_def,
                   guint32 field_id,
                   gchar *buffer_ptr,
                   guint32 offset,
                   gchar *null_offset,
                   guint32 null_offset)
{
}

static int
define_write_part_field(IC_TABLE_DEF *table_def,
                        guint32 field_id,
                        gchar *buffer_ptr,
                        guint32 offset,
                        gchar *null_ptr,
                        guint32 null_offset,
                        guint32 start_byte,
                        guint32 end_byte)
{
}

static int
ic_create_apid_operation(IC_TABLE_DEF *table_def,
                         guint32 num_key_fields,
                         guint32 num_read_fields,
                         guint32 num_write_fields,
                         gchar *buffer_ptr,
                         guint64 buffer_size,
                         gchar *null_ptr,
                         guint32 null_area_size)
{
  IC_A
  (void)table_def;
  
}
*/

static void
free_apid_op(IC_APID_OPERATION *apid_op)
{
  if (apid_op)
    ic_free(apid_op);
}

static IC_APID_OPERATION_OPS glob_apid_ops
{
  .ic_define_field            = define_field,
  .ic_multi_range             = multi_range,
  .ic_define_range_part       = define_range_part,
  .ic_keep_ranges             = keep_ranges,
  .ic_define_condition        = define_condition,
  .ic_define_boolean          = define_boolean
  .ic_get_error_object        = get_error_object,
  .ic_free_apid_op            = free_apid_op
};

IC_APID_OPERATION*
ic_create_apid_operation(IC_TABLE_DEF *table_def,
                         guint32 num_fields,
                         guint32 num_key_fields,
                         gchar *buffer_ptr,
                         guint32 buffer_size,
                         gchar *null_ptr,
                         guint32 num_null_bits,
                         gboolean full_table_op)
{
  IC_INT_APID_OPERATION *apid_op;
  IC_FIELD_DEF *field_def;
  gchar *loc_alloc;
  guint32 tot_size;

  tot_size=
    sizeof(IC_INT_APID_OPERATION) +
    sizeof(IC_FIELD_BIND) + 
    num_fields * (
      sizeof(IC_FIELD_DEF*) + sizeof(IC_FIELD_DEF));
  if ((loc_alloc= ic_calloc(tot_size)))
    return NULL;
  apid_op= (IC_INT_APID_OPERATION*)loc_alloc;
  apid_op->fields= (IC_FIELD_DEF*)(loc_alloc + sizeof(IC_INT_APID_OPERATION));
  apid_op->field_def= (IC_FIELD_DEF**)(loc_alloc + sizeof(IC_FIELD_BIND));
  loc_alloc+= (num_fields * sizeof(IC_FIELD_DEF*));
  for (i= 0; i < num_fields; i++)
  {
    apid_op->field_def[i]= (IC_FIELD_DEF*)loc_alloc;
    loc_alloc+= sizeof(IC_FIELD_DEF);
  }
  apid_op->table_def= table_def;
  apid_op->fields->num_fields= num_fields;
  apid_op->fields->num_key_fields= num_key_fields;
  apid_op->fields->buffer_ptr= buffer_ptr;
  apid_op->fields->null_ptr= null_ptr;
  apid_op->fields->num_null_bits= num_null_bits;
  apid_op->fields->buffer_size= buffer_size;
  return apid_op;
}
