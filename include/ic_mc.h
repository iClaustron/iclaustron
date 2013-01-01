/* Copyright (C) 2007-2013 iClaustron AB

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
  A helpful support function to handle many memory allocations within one
  container. This allocates a linked list of containers of base size (except
  when allocations are larger than base size. When freeing it will free all
  of those containers.

  Reset means that we deallocate everything one container of base size.
  Thus we're back to the situation after performing the first allocation of
  the container. Reset will also set all bytes to 0 in container.
*/
#ifndef IC_MC_H
#define IC_MC_H

#define MC_MIN_BASE_SIZE 128
#define MC_DEFAULT_BASE_SIZE 8180
struct ic_memory_container_ops
{
  gchar* (*ic_mc_alloc) (IC_MEMORY_CONTAINER *mc_ptr, guint32 size);
  gchar* (*ic_mc_calloc) (IC_MEMORY_CONTAINER *mc_ptr, guint32 size);
  void (*ic_mc_reset) (IC_MEMORY_CONTAINER *mc_ptr);
  void (*ic_mc_free) (IC_MEMORY_CONTAINER *mc_ptr);
};
typedef struct ic_memory_container_ops IC_MEMORY_CONTAINER_OPS;

struct ic_memory_container
{
  IC_MEMORY_CONTAINER_OPS mc_ops;
};

IC_MEMORY_CONTAINER*
ic_create_memory_container(guint32 base_size,
                           guint32 max_size,
                           gboolean use_mutex);
#endif
