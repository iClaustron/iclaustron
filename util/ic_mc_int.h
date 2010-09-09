/* Copyright (C) 2007-2010 iClaustron AB

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

#ifndef IC_MC_INT_H
#define IC_MC_INT_H

struct ic_int_memory_container
{
  IC_MEMORY_CONTAINER_OPS mc_ops;
  gchar *current_buf;
  gchar **buf_array;
  guint64 total_size;
  guint64 max_size;
  guint32 base_size;
  guint32 buf_array_size;
  guint32 current_buf_inx;
  guint32 current_free_len;
  guint32 first_buf_inx;
  gboolean use_mutex;
  IC_MUTEX *mutex;
};
typedef struct ic_int_memory_container IC_INT_MEMORY_CONTAINER;
#endif
