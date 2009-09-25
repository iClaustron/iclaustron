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
                         
