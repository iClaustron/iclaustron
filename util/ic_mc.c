/* Copyright (C) 2007-2011 iClaustron AB

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
#include <ic_port.h>
#include <ic_mc.h>
#include "ic_mc_int.h"
/*
  MODULE: Memory Container
  Description:
    This module contains a generic memory container implementation whereby
    one allocates a memory container where many memory allocations can be
    grouped together to minimise the overhead of allocating small memory
    chunks and later deallocating them.
*/
static guint32
mc_get_base_buf_size(guint64 max_size, guint32 base_size)
{
  guint64 buf_size;

  if (max_size == 0) /* No max_size given */
    buf_size= 8;
  else
    buf_size= (max_size + base_size - 1)/base_size;
  return (guint32)buf_size;
}

static gchar*
mc_realloc_buf_array(IC_INT_MEMORY_CONTAINER *mc_ptr, guint32 new_size)
{
  gchar *new_buf_array;
  guint32 old_buf_size, copy_buf_size;

  old_buf_size= mc_ptr->buf_array_size;
  copy_buf_size= IC_MIN(old_buf_size, new_size);

  if (!(new_buf_array= ic_calloc_mc(new_size*sizeof(gchar*))))
    return NULL;
  memcpy(new_buf_array, mc_ptr->buf_array, copy_buf_size * sizeof(gchar*));
  ic_free_mc(mc_ptr->buf_array);
  mc_ptr->buf_array= (gchar**)new_buf_array;
  mc_ptr->buf_array_size= new_size;
  return new_buf_array;
}

static gchar*
mc_alloc(IC_MEMORY_CONTAINER *ext_mc_ptr, guint32 size)
{
  IC_INT_MEMORY_CONTAINER *mc_ptr= (IC_INT_MEMORY_CONTAINER*)ext_mc_ptr;
  gchar *new_buf_array= NULL;
  gchar *ret_ptr= NULL;
  gchar *new_buf;
  guint32 alloc_size, buf_inx;
  guint64 new_total_size;

  size= ic_align(size, 8); /* Always allocate on 8 byte boundaries */
  if (mc_ptr->mutex)
    ic_mutex_lock(mc_ptr->mutex);
  new_total_size= mc_ptr->total_size+size;
  if (mc_ptr->max_size > 0 && mc_ptr->max_size < new_total_size)
    goto end;
  if (mc_ptr->current_free_len >= size)
  {
    /* Space is available, no need to allocate more */
    ret_ptr= mc_ptr->current_buf;
    mc_ptr->current_buf+= size;
    mc_ptr->current_free_len-= size;
    mc_ptr->total_size= new_total_size;
    goto end;
  }
  alloc_size= mc_ptr->base_size;
  if (size > alloc_size)
    alloc_size= size;
  buf_inx= mc_ptr->current_buf_inx + 1;
  if (buf_inx >= mc_ptr->buf_array_size)
  {
    if (!(new_buf_array= mc_realloc_buf_array(mc_ptr,
          mc_ptr->buf_array_size+16)))
      goto end;
  }
  if (!(new_buf= ic_calloc_mc(alloc_size)))
    goto end;
  mc_ptr->current_buf_inx= buf_inx;
  if (size == alloc_size)
  {
    /*
      New buffer already empty, let's continue to use the old
      buffer still, by putting at the front. In this the current
      buffer and its length remains the same as before.
      Keep track of if first buffer index is moved around.
    */
    mc_ptr->buf_array[buf_inx]= mc_ptr->buf_array[buf_inx-1];
    mc_ptr->buf_array[buf_inx-1]= new_buf;
    if (mc_ptr->first_buf_inx == (buf_inx - 1))
      mc_ptr->first_buf_inx= buf_inx;
  }
  else
  {
    mc_ptr->current_buf= new_buf+size;
    mc_ptr->current_free_len= alloc_size - size;
    mc_ptr->buf_array[buf_inx]= new_buf;
  }
  mc_ptr->total_size= new_total_size;
  ret_ptr= new_buf;
end:
  if (mc_ptr->mutex)
    ic_mutex_unlock(mc_ptr->mutex);
  return ret_ptr;
}

static gchar*
mc_calloc(IC_MEMORY_CONTAINER *ext_mc_ptr, guint32 size)
{
  gchar *ptr;

  if (!(ptr= mc_alloc(ext_mc_ptr, size)))
    return NULL;
  ic_zero(ptr, size);
  return ptr;
}

static void
mc_reset(IC_MEMORY_CONTAINER *ext_mc_ptr)
{
  IC_INT_MEMORY_CONTAINER *mc_ptr= (IC_INT_MEMORY_CONTAINER*)ext_mc_ptr;
  guint32 orig_arr_size, i;
  guint32 first_alloc_buf;
  gchar *tmp;

  if (mc_ptr->use_mutex)
    ic_mutex_lock(mc_ptr->mutex);
  /*
    The first allocated buffer which is of appropriate length has been
    moved around, moved it to the first slot such that it is kept and
    the other buffers are released.
  */
  first_alloc_buf= mc_ptr->first_buf_inx;
  tmp= mc_ptr->buf_array[first_alloc_buf];
  mc_ptr->buf_array[first_alloc_buf]= mc_ptr->buf_array[0];
  mc_ptr->buf_array[0]= tmp;
  mc_ptr->first_buf_inx= 0;

  /* Deallocate all extra buffers except the first one */
  for (i= 1; i <= mc_ptr->current_buf_inx; i++)
    ic_free_mc(mc_ptr->buf_array[i]);
 
  orig_arr_size= mc_get_base_buf_size(mc_ptr->max_size, mc_ptr->base_size);
  if (mc_ptr->buf_array_size > orig_arr_size)
    mc_realloc_buf_array(mc_ptr, orig_arr_size);
  ic_zero(&mc_ptr->buf_array[1],
          (mc_ptr->buf_array_size-1)*sizeof(gchar*));
  mc_ptr->current_buf= mc_ptr->buf_array[0];
  mc_ptr->total_size= 0;
  mc_ptr->current_buf_inx= 0;
  mc_ptr->current_free_len= mc_ptr->base_size;
  if (mc_ptr->use_mutex)
    ic_mutex_unlock(mc_ptr->mutex);
  return;
}

static void
mc_free(IC_MEMORY_CONTAINER *ext_mc_ptr)
{
  IC_INT_MEMORY_CONTAINER *mc_ptr= (IC_INT_MEMORY_CONTAINER*)ext_mc_ptr;
  guint32 i;

  for (i= 0; i <= mc_ptr->current_buf_inx; i++)
    ic_free_mc(mc_ptr->buf_array[i]);
  if (mc_ptr->use_mutex)
    ic_mutex_destroy(mc_ptr->mutex);
  ic_free_mc(mc_ptr->buf_array);
  ic_free_mc(mc_ptr);
}

IC_MEMORY_CONTAINER*
ic_create_memory_container(guint32 base_size, guint32 max_size,
                           gboolean use_mutex)
{
  gchar *buf_array_ptr, *first_buf;
  guint32 buf_size;
  IC_INT_MEMORY_CONTAINER *mc_ptr;

  if (base_size < MC_MIN_BASE_SIZE)
    base_size= MC_MIN_BASE_SIZE;
  base_size= ic_align(base_size, 8);
  max_size= ic_align(max_size, 8);
  if (max_size > 0 && max_size < base_size)
    max_size= base_size;
  buf_size= mc_get_base_buf_size(max_size, base_size);
  /*
    Allocate buffer array indepently to be able to easily grow it without
    the need to handle complex lists of arrays.
  */
  if (!(buf_array_ptr= ic_calloc_mc(buf_size * sizeof(gchar*))))
    return NULL;
  if (!(mc_ptr= (IC_INT_MEMORY_CONTAINER*)ic_calloc_mc(
         sizeof(IC_INT_MEMORY_CONTAINER))))
  {
    ic_free_mc(buf_array_ptr);
    return NULL;
  }
  if (!(first_buf= ic_calloc_mc(base_size)))
  {
    ic_free_mc(buf_array_ptr);
    ic_free_mc(mc_ptr);
    return NULL;
  }
  if (use_mutex && !(mc_ptr->mutex= ic_mutex_create()))
  {
    ic_free_mc(first_buf);
    ic_free_mc(buf_array_ptr);
    ic_free_mc(mc_ptr);
    return NULL;
  }

  /* Initialise methods */
  mc_ptr->mc_ops.ic_mc_alloc= mc_alloc;
  mc_ptr->mc_ops.ic_mc_calloc= mc_calloc;
  mc_ptr->mc_ops.ic_mc_reset= mc_reset;
  mc_ptr->mc_ops.ic_mc_free= mc_free;
  /* Initialise Memory Container variables */ 
  mc_ptr->current_buf= first_buf;
  mc_ptr->buf_array= (gchar**)buf_array_ptr;
  mc_ptr->buf_array[0]= first_buf;
  mc_ptr->total_size= 0;
  mc_ptr->max_size= max_size;
  mc_ptr->base_size= base_size;
  mc_ptr->buf_array_size= buf_size;
  mc_ptr->current_buf_inx= 0;
  mc_ptr->current_free_len= base_size;
  mc_ptr->first_buf_inx= 0;
  mc_ptr->use_mutex= use_mutex;
  return (IC_MEMORY_CONTAINER*)mc_ptr;
}
