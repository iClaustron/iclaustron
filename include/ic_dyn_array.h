/* Copyright (C) 2007-2009 iClaustron AB

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

#ifndef IC_DYNAMIC_ARRAY_H
#define IC_DYNAMIC_ARRAY_H

struct ic_dynamic_array_ops
{
  int (*ic_insert_dynamic_array) (IC_DYNAMIC_ARRAY *dyn_array,
                                  const gchar *buf,
                                  guint64 size);
  int (*ic_write_dynamic_array) (IC_DYNAMIC_ARRAY *dyn_array,
                                 guint64 position,
                                 guint64 size,
                                 const gchar *buf);
  guint64 (*ic_get_current_size) (IC_DYNAMIC_ARRAY *dyn_array);
  int (*ic_write_dynamic_array_to_disk) (IC_DYNAMIC_ARRAY *dyn_array,
                                         int file_ptr);
  int (*ic_read_dynamic_array) (IC_DYNAMIC_ARRAY *dyn_array,
                                guint64 position, guint64 size,
                                gchar *ret_buf);
  void (*ic_free_dynamic_array) (IC_DYNAMIC_ARRAY *dyn_array);
};
typedef struct ic_dynamic_array_ops IC_DYNAMIC_ARRAY_OPS;

struct ic_dynamic_array
{
  IC_DYNAMIC_ARRAY_OPS da_ops;
};

IC_DYNAMIC_ARRAY* ic_create_simple_dynamic_array();
IC_DYNAMIC_ARRAY* ic_create_ordered_dynamic_array();

struct ic_dynamic_translation;
typedef struct ic_dynamic_translation IC_DYNAMIC_TRANSLATION;
struct ic_dynamic_translation_ops
{
  int (*ic_get_translation_object) (IC_DYNAMIC_TRANSLATION *dyn_trans,
                                    guint64 index,
                                    void **object);
  int (*ic_insert_translation_object) (IC_DYNAMIC_TRANSLATION *dyn_trans,
                                       guint64 *index,
                                       void *object);
  int (*ic_remove_translation_object) (IC_DYNAMIC_TRANSLATION *dyn_trans,
                                       guint64 index,
                                        void *object);
  guint64 (*ic_get_max_index) (IC_DYNAMIC_TRANSLATION *dyn_trans);
  void (*ic_free_translation_object) (IC_DYNAMIC_TRANSLATION *dyn_trans);
};
typedef struct ic_dynamic_translation_ops IC_DYNAMIC_TRANSLATION_OPS;

struct ic_dynamic_translation
{
  IC_DYNAMIC_TRANSLATION_OPS dt_ops;
};

IC_DYNAMIC_TRANSLATION* ic_create_dynamic_translation_object();
#endif
