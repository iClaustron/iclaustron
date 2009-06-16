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

#ifndef IC_DYN_ARRAY_INT_H
#define IC_DYN_ARRAY_INT_H

#define SIMPLE_DYNAMIC_ARRAY_BUF_SIZE 1024
#define ORDERED_DYNAMIC_INDEX_SIZE 256
#define LOG_SIMPLE_DYNAMIC_ARRAY_BUF_SIZE 10
#define LOG_ORDERED_DYNAMIC_INDEX_SIZE 8

struct ic_simple_dynamic_buf;
struct ic_simple_dynamic_buf
{
  struct ic_simple_dynamic_buf *next_dyn_buf;
  guint64 buf[SIMPLE_DYNAMIC_ARRAY_BUF_SIZE/8];
};
typedef struct ic_simple_dynamic_buf IC_SIMPLE_DYNAMIC_BUF;

struct ic_simple_dynamic_array
{
  IC_SIMPLE_DYNAMIC_BUF *first_dyn_buf;
  IC_SIMPLE_DYNAMIC_BUF *last_dyn_buf;
  /*
    bytes_used is the number of bytes not yet used in the last dynamic
    buffer.
  */
  guint64 bytes_used;
};
typedef struct ic_simple_dynamic_array IC_SIMPLE_DYNAMIC_ARRAY;

struct ic_dynamic_array_index;
struct ic_dynamic_array_index
{
  struct ic_dynamic_array_index *next_dyn_index;
  struct ic_dynamic_array_index *parent_dyn_index;
  guint64 next_pos_to_insert;
  void* child_ptrs[ORDERED_DYNAMIC_INDEX_SIZE];
};
typedef struct ic_dynamic_array_index IC_DYNAMIC_ARRAY_INDEX;

struct ic_ordered_dynamic_array
{
  IC_DYNAMIC_ARRAY_INDEX *top_index;
  IC_DYNAMIC_ARRAY_INDEX *first_dyn_index;
  IC_DYNAMIC_ARRAY_INDEX *last_dyn_index;
  guint32 index_levels;
};
typedef struct ic_ordered_dynamic_array IC_ORDERED_DYNAMIC_ARRAY;

struct ic_dynamic_array_int
{
  IC_DYNAMIC_ARRAY_OPS da_ops;
  IC_SIMPLE_DYNAMIC_ARRAY sd_array;
  IC_ORDERED_DYNAMIC_ARRAY ord_array;
  guint64 total_size_in_bytes;
};
typedef struct ic_dynamic_array_int IC_DYNAMIC_ARRAY_INT;

struct ic_translation_entry
{
  union
  {
    void *object;
    guint64 position;
  };
};
typedef struct ic_translation_entry IC_TRANSLATION_ENTRY;

struct ic_dynamic_translation_int
{
  IC_DYNAMIC_TRANSLATION_OPS dt_ops;
  IC_DYNAMIC_ARRAY *dyn_array;
};
typedef struct ic_dynamic_translation_int IC_DYNAMIC_TRANSLATION_INT;
#endif
