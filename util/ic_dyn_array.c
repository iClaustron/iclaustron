/* Copyright (C) 2007, 2008 iClaustron AB

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

#include <ic_common.h>
#include <ic_dyn_array_int.h>

/*
  Implementation of the dynamic array. A very simple variant
  using a simple linked list of buffers. Also a slightly more
  complex variant that uses the simple linked list and builds
  a tree index on top of it. Both these dynamic array can only
  be added to and cannot be shrinked in any other way than by
  freeing the entire buffer.

  The interface is designed to be useful also for more complex
  dynamic array implementation such as FIFO queues as well.

  We can hopefully also adapt it slightly to be able to use it
  also for a concurrent data structure implementing a linked list
  of buffers. This linked list of buffers should get its buffers from a
  threaded malloc implementation. This can also use one of our own data
  structures and so the reference to a threaded malloc pool should be
  provided in the dynamic array.
*/

static void
release_dyn_buf(IC_SIMPLE_DYNAMIC_BUF *loop_dyn_buf)
{
  IC_SIMPLE_DYNAMIC_BUF *free_dyn_buf;

  loop_dyn_buf= loop_dyn_buf->next_dyn_buf;
  while (loop_dyn_buf)
  {
    free_dyn_buf= loop_dyn_buf;
    loop_dyn_buf= loop_dyn_buf->next_dyn_buf;
    ic_free((void*)free_dyn_buf);
  }
}

static int
insert_simple_dynamic_array(IC_DYNAMIC_ARRAY *ext_dyn_array,
                            const gchar *buf, guint64 size)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  IC_SIMPLE_DYNAMIC_ARRAY *sd_array= &dyn_array->sd_array;
  IC_SIMPLE_DYNAMIC_BUF *curr= sd_array->last_dyn_buf;
  IC_SIMPLE_DYNAMIC_BUF *loop_dyn_buf= curr;
  guint64 bytes_used= sd_array->bytes_used;
  gchar *buf_ptr= (gchar*)&curr->buf[0];
  guint64 size_left_to_copy= size;
  guint64 buf_ptr_start_pos;
  guint64 size_in_buffer= SIMPLE_DYNAMIC_ARRAY_BUF_SIZE - bytes_used;
  IC_SIMPLE_DYNAMIC_BUF *new_simple_dyn_buf;

  buf_ptr+= bytes_used;
  buf_ptr_start_pos= bytes_used;
  while (size_left_to_copy > size_in_buffer)
  {
    memcpy(buf_ptr, buf, size_in_buffer);
    size_left_to_copy-= size_in_buffer;
    buf+= size_in_buffer;
    if (!(new_simple_dyn_buf=
      (IC_SIMPLE_DYNAMIC_BUF*) ic_calloc(sizeof(IC_SIMPLE_DYNAMIC_BUF))))
    {
      /* We need to deallocate all buffers already allocated */
      release_dyn_buf(loop_dyn_buf->next_dyn_buf);
      return IC_ERROR_MEM_ALLOC;
    }
    curr->next_dyn_buf= new_simple_dyn_buf;
    dyn_array->sd_array.last_dyn_buf= new_simple_dyn_buf;
    buf_ptr= (gchar*)&new_simple_dyn_buf->buf[0];
    curr= new_simple_dyn_buf;
    size_in_buffer= SIMPLE_DYNAMIC_ARRAY_BUF_SIZE;
    buf_ptr_start_pos= 0;
  }
  memcpy(buf_ptr, buf, size_left_to_copy);
  sd_array->bytes_used= buf_ptr_start_pos + size_left_to_copy;
  return 0;
}

static int
find_pos_simple_dyn_array(IC_DYNAMIC_ARRAY_INT *dyn_array, guint64 pos,
                          IC_SIMPLE_DYNAMIC_BUF **dyn_buf,
                          guint64 *buf_pos)
{
  guint64 scan_pos= 0;
  guint64 end_pos;
  IC_SIMPLE_DYNAMIC_BUF *loc_dyn_buf= dyn_array->sd_array.first_dyn_buf;

  while (loc_dyn_buf)
  {
    end_pos= scan_pos + SIMPLE_DYNAMIC_ARRAY_BUF_SIZE;
    if (pos >= end_pos)
    {
      /* Still searching for first buffer where to start reading */
      loc_dyn_buf= loc_dyn_buf->next_dyn_buf;
      scan_pos= end_pos;
      continue;
    }
    *dyn_buf= loc_dyn_buf;
    *buf_pos= (pos - scan_pos);
    return 0;
  }
  return 1;
}

static int
read_pos_simple_dynamic_array(IC_SIMPLE_DYNAMIC_BUF *dyn_buf, guint64 buf_pos,
                              guint64 size, gchar *ret_buf)
{
  guint64 read_size, already_read_size;
  gchar *read_buf;

  read_buf= (gchar*)&dyn_buf->buf[0];
  read_buf+= buf_pos;
  read_size= IC_MIN(size,
                    (SIMPLE_DYNAMIC_ARRAY_BUF_SIZE - buf_pos));
  memcpy(ret_buf, read_buf, read_size);
  while (read_size < size)
  {
    read_buf+= SIMPLE_DYNAMIC_ARRAY_BUF_SIZE;
    dyn_buf= dyn_buf->next_dyn_buf;
    if (dyn_buf == NULL)
      return 1;
    already_read_size= read_size;
    read_size= IC_MIN(size, (read_size + SIMPLE_DYNAMIC_ARRAY_BUF_SIZE));
    read_buf= (gchar*)&dyn_buf->buf[0];
    memcpy(ret_buf, read_buf, (read_size - already_read_size));
  }
  return 0;
}

static int
read_simple_dynamic_array(IC_DYNAMIC_ARRAY *ext_dyn_array, guint64 pos,
                          guint64 size, gchar *ret_buf)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  IC_SIMPLE_DYNAMIC_BUF *dyn_buf;
  guint64 buf_pos;
  int ret_code;

  if ((ret_code= find_pos_simple_dyn_array(dyn_array, pos,
                                           &dyn_buf, &buf_pos)) || 
      (ret_code= read_pos_simple_dynamic_array(dyn_buf, buf_pos,
                                               size, ret_buf)))
    return ret_code;
  return 0;
}

/* This call expects an open file which is empty to start with as input */
static int
write_simple_dynamic_array_to_disk(IC_DYNAMIC_ARRAY *ext_dyn_array,
                                   int file_ptr)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  IC_SIMPLE_DYNAMIC_BUF *dyn_buf= dyn_array->sd_array.first_dyn_buf;
  IC_SIMPLE_DYNAMIC_BUF *last_dyn_buf= dyn_array->sd_array.last_dyn_buf;
  IC_SIMPLE_DYNAMIC_BUF *old_dyn_buf;
  gchar *buf;
  guint32 size_in_buffer;
  int ret_code;

  while (dyn_buf)
  {
    if (dyn_buf != last_dyn_buf)
      size_in_buffer= SIMPLE_DYNAMIC_ARRAY_BUF_SIZE;
    else
      size_in_buffer= dyn_array->sd_array.bytes_used;
    buf= (gchar*)&dyn_buf->buf[0];
    if ((ret_code= ic_write_file(file_ptr, buf, size_in_buffer)))
      return ret_code;
    old_dyn_buf= dyn_buf;
    dyn_buf= dyn_buf->next_dyn_buf;
    g_assert((old_dyn_buf != last_dyn_buf && dyn_buf) ||
             (old_dyn_buf == last_dyn_buf && !dyn_buf));
  }
  return 0;
}

void
free_simple_dynamic_array(IC_DYNAMIC_ARRAY *ext_dyn_array)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  release_dyn_buf(dyn_array->sd_array.first_dyn_buf);
  ic_free((void*)dyn_array);
}

IC_DYNAMIC_ARRAY*
ic_create_simple_dynamic_array()
{
  IC_DYNAMIC_ARRAY_INT *dyn_array;
  IC_SIMPLE_DYNAMIC_ARRAY *sd_array;
  IC_DYNAMIC_ARRAY_OPS *da_ops;
  IC_SIMPLE_DYNAMIC_BUF *dyn_buf;

  if (!(dyn_array= (IC_DYNAMIC_ARRAY_INT*)ic_calloc(
        sizeof(IC_DYNAMIC_ARRAY_INT))))
    return NULL;
  if (!(dyn_buf= (IC_SIMPLE_DYNAMIC_BUF*)ic_calloc(
         sizeof(IC_SIMPLE_DYNAMIC_BUF))))
  {
    ic_free((void*)dyn_array);
    return NULL;
  }
  sd_array= &dyn_array->sd_array;
  sd_array->first_dyn_buf= dyn_buf;
  sd_array->last_dyn_buf= dyn_buf;
  da_ops= &dyn_array->da_ops;
  da_ops->ic_insert_dynamic_array= insert_simple_dynamic_array;
  da_ops->ic_read_dynamic_array= read_simple_dynamic_array;
  da_ops->ic_write_dynamic_array_to_disk= write_simple_dynamic_array_to_disk;
  da_ops->ic_free_dynamic_array= free_simple_dynamic_array;
  return (IC_DYNAMIC_ARRAY*)dyn_array;
}

static int
insert_buf_in_ordered_dynamic_index(IC_DYNAMIC_ARRAY *ext_dyn_array,
                                    IC_DYNAMIC_ARRAY_INDEX *dyn_index,
                                    IC_DYNAMIC_ARRAY_INDEX **new_parent,
                                    void *child_ptr)
{
  IC_DYNAMIC_ARRAY_INDEX *new_dyn_index;
  guint32 next_pos_to_insert= dyn_index->next_pos_to_insert;
  IC_DYNAMIC_ARRAY_INDEX *new_parent_dyn_index= NULL;
  int ret_code;

  if (next_pos_to_insert == ORDERED_DYNAMIC_INDEX_SIZE)
  {
    /*
       We have filled a dynamic array index buffer and we need to
       add another buffer. We use a recursive call to implement
       this functionality.
    */
    if (!(new_dyn_index= (IC_DYNAMIC_ARRAY_INDEX*)ic_calloc(
           sizeof(IC_DYNAMIC_ARRAY_INDEX))))
      return IC_ERROR_MEM_ALLOC;
    if (new_parent)
      *new_parent= new_dyn_index;
    if (dyn_index->parent_dyn_index)
    {
      /* 
        We aren't the top node, then it's sufficient to create a new
        dynamic array index buffer and insert a reference to it in our
        parent node.
      */
      if ((ret_code= insert_buf_in_ordered_dynamic_index(ext_dyn_array,
                                                 dyn_index->parent_dyn_index,
                                                 &new_parent_dyn_index,
                                                 (void*)new_dyn_index)))
      {
        ic_free((void*)new_dyn_index);
        return ret_code;
      }
      if (!new_parent_dyn_index)
      {
        /* The new node has the same parent as this node */
        new_parent_dyn_index= dyn_index->parent_dyn_index;
      }
      new_dyn_index->parent_dyn_index= new_parent_dyn_index;
    }
    else
    {
      /*
        We need to handle the top node in a special manner. The top node
        needs a link to the full node and the new empty node.
      */
      if (!(new_parent_dyn_index= (IC_DYNAMIC_ARRAY_INDEX*)ic_calloc(
             sizeof(IC_DYNAMIC_ARRAY_INDEX))))
      {
        ic_free((void*)new_dyn_index);
        return IC_ERROR_MEM_ALLOC;
      }
      dyn_index->parent_dyn_index= new_parent_dyn_index;
      new_dyn_index->parent_dyn_index= new_parent_dyn_index;
      new_parent_dyn_index->child_ptrs[0]= (void*)dyn_index;
      new_parent_dyn_index->child_ptrs[1]= (void*)new_dyn_index;
      new_parent_dyn_index->next_pos_to_insert= 2;
    }
    dyn_index= new_dyn_index;
    next_pos_to_insert= 0;
  }
  dyn_index->child_ptrs[next_pos_to_insert]= child_ptr;
  dyn_index->next_pos_to_insert= ++next_pos_to_insert;
  return 0;
}

static int
insert_ordered_dynamic_array(IC_DYNAMIC_ARRAY *ext_dyn_array,
                             const gchar *buf, guint64 size)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  int ret_code;
  IC_SIMPLE_DYNAMIC_BUF *old_dyn_buf= dyn_array->sd_array.last_dyn_buf;
  IC_SIMPLE_DYNAMIC_BUF *new_last_dyn_buf, *loop_dyn_buf;
  IC_DYNAMIC_ARRAY_INDEX *last_dyn_index;
  guint64 buf_size= 0;
  guint32 ins_buf_count= 0, i;

  if ((ret_code= insert_simple_dynamic_array(ext_dyn_array, buf, size)))
    return ret_code;
  new_last_dyn_buf= dyn_array->sd_array.last_dyn_buf;
  loop_dyn_buf= old_dyn_buf;
  while (new_last_dyn_buf != loop_dyn_buf)
  {
    loop_dyn_buf= loop_dyn_buf->next_dyn_buf;
    buf_size+= SIMPLE_DYNAMIC_ARRAY_BUF_SIZE;
    g_assert(size <= buf_size);
    last_dyn_index= dyn_array->ord_array.last_dyn_index;
    if ((ret_code= insert_buf_in_ordered_dynamic_index(ext_dyn_array,
                                                       last_dyn_index,
                                                       NULL,
                                                       (void*)loop_dyn_buf)))
    {
      loop_dyn_buf= old_dyn_buf->next_dyn_buf;
      release_dyn_buf(loop_dyn_buf);
      for (i= 0; i < ins_buf_count; i++)
      {
        loop_dyn_buf= loop_dyn_buf->next_dyn_buf;
        release_dyn_buf(loop_dyn_buf);
      }
      return ret_code;
    }
    ins_buf_count++;
  }
  return 0;
}

static int
find_pos_ordered_dyn_array(
__attribute__ ((unused)) IC_DYNAMIC_ARRAY_INT *dyn_array,
__attribute__ ((unused)) guint64 pos,
__attribute__ ((unused)) IC_SIMPLE_DYNAMIC_BUF **dyn_buf,
__attribute__ ((unused)) guint64 *buf_pos)
{
  return 0;
}

static int
read_ordered_dynamic_array(IC_DYNAMIC_ARRAY *ext_dyn_array, guint64 pos,
                           guint64 size, gchar *ret_buf)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  IC_SIMPLE_DYNAMIC_BUF *dyn_buf= NULL;
  guint64 buf_pos= 0;
  int ret_code;

  if ((ret_code= find_pos_ordered_dyn_array(dyn_array, pos,
                                            &dyn_buf, &buf_pos)) || 
      (ret_code= read_pos_simple_dynamic_array(dyn_buf, buf_pos,
                                               size, ret_buf)))
    return ret_code;
  return 0;
}

static void
free_ordered_dynamic_array(IC_DYNAMIC_ARRAY *ext_dyn_array)
{
  IC_DYNAMIC_ARRAY_INT *dyn_array= (IC_DYNAMIC_ARRAY_INT*)ext_dyn_array;
  /* Free ordered index part first */
  IC_ORDERED_DYNAMIC_ARRAY *ord_array= &dyn_array->ord_array;
  IC_DYNAMIC_ARRAY_INDEX *dyn_array_index, *next_dyn_array_index;

  dyn_array_index= ord_array->first_dyn_index;
  while (dyn_array_index)
  {
    next_dyn_array_index= dyn_array_index->next_dyn_index;
    ic_free((void*)dyn_array_index);
    dyn_array_index= next_dyn_array_index;
  }
  /* Now we can free the simple dynamic array part */
  free_simple_dynamic_array(ext_dyn_array);
}

IC_DYNAMIC_ARRAY*
ic_create_ordered_dynamic_array()
{
  IC_DYNAMIC_ARRAY_OPS *da_ops;
  IC_DYNAMIC_ARRAY_INT *dyn_array;
  IC_DYNAMIC_ARRAY_INDEX *dyn_index_array;
  IC_ORDERED_DYNAMIC_ARRAY *ord_array;

  if (!(dyn_index_array= (IC_DYNAMIC_ARRAY_INDEX*)ic_calloc(
           sizeof(IC_DYNAMIC_ARRAY_INDEX))))
    return NULL;
  if (!(dyn_array= (IC_DYNAMIC_ARRAY_INT*)ic_create_simple_dynamic_array()))
  {
    ic_free((void*)dyn_index_array);
    return NULL;
  }

  ord_array= &dyn_array->ord_array;
  ord_array->top_index= dyn_index_array;
  ord_array->first_dyn_index= dyn_index_array;
  ord_array->last_dyn_index= dyn_index_array;

  da_ops= &dyn_array->da_ops;
  da_ops->ic_insert_dynamic_array= insert_ordered_dynamic_array;
  da_ops->ic_free_dynamic_array= free_ordered_dynamic_array;
  da_ops->ic_read_dynamic_array= read_ordered_dynamic_array;
  return (IC_DYNAMIC_ARRAY*)dyn_array;
}

int
insert_translation_object(IC_DYNAMIC_TRANSLATION *ext_dyn_trans,
                          guint64 *position,
                          void *object)
{
  IC_TRANSLATION_ENTRY transl_entry, first_entry;
  guint64 pos_first= (guint64)0;
  guint64 pos_first_free;
  guint64 entry_size= sizeof(IC_TRANSLATION_ENTRY);
  IC_DYNAMIC_TRANSLATION_INT *dyn_trans=
    (IC_DYNAMIC_TRANSLATION_INT*)ext_dyn_trans;
  IC_DYNAMIC_ARRAY *dyn_array= dyn_trans->dyn_array;

  if (dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                              pos_first,
                                              entry_size,
                                              (gchar*)&transl_entry))
    abort();
  pos_first_free= transl_entry.position;
  if (pos_first_free == (guint64)0)
  {
    /*
      All entries were already used, we need to extend the
      array
    */
    pos_first_free= dyn_array->da_ops.ic_get_current_size(dyn_array);
    if (dyn_array->da_ops.ic_insert_dynamic_array(dyn_array,
                                                  (const gchar*)&transl_entry,
                                                  entry_size))
    {
      /* Memory allocation error */
      return IC_ERROR_MEM_ALLOC;
    }
  }
  else
  {
    /*
      Use the free entry but also keep the free list up to date
      by reading the next free from the first free.
    */
    if (dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                                pos_first_free,
                                                entry_size,
                                                (gchar*)&transl_entry))
      abort();
    first_entry.position= transl_entry.position;
    if (dyn_array->da_ops.ic_write_dynamic_array(dyn_array,
                                                 pos_first,
                                                 entry_size,
                                                 (const gchar*)&first_entry))
      abort();
    transl_entry.object= object;
    if (dyn_array->da_ops.ic_write_dynamic_array(dyn_array,
                                                 pos_first_free,
                                                 entry_size,
                                                 (gchar*)&transl_entry))
      abort();
  }
  *position= pos_first_free;
  return 0;
}

void
remove_translation_object(IC_DYNAMIC_TRANSLATION *ext_dyn_trans,
                          guint64 index,
                          void *object)
{
  IC_TRANSLATION_ENTRY transl_entry;
  guint64 pos_first= (guint64)0;
  guint64 position= index * sizeof(IC_TRANSLATION_ENTRY);
  guint64 entry_size= sizeof(IC_TRANSLATION_ENTRY);
  IC_DYNAMIC_TRANSLATION_INT *dyn_trans=
    (IC_DYNAMIC_TRANSLATION_INT*)ext_dyn_trans;
  IC_DYNAMIC_ARRAY *dyn_array= dyn_trans->dyn_array;

  if (dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                               position,
                                               entry_size,
                                               (gchar*)&transl_entry))
  {
    /* Serious error cannot find entry to remove */
    abort();
  }
  if (transl_entry.object != object)
  {
    /* Serious error, wrong object where positioned */
    abort();
  }
  if (dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                               pos_first,
                                               entry_size,
                                               (gchar*)&transl_entry))
  {
    /* Serious error cannot find entry to remove */
    abort();
  }
  if (dyn_array->da_ops.ic_write_dynamic_array(dyn_array,
                                               position,
                                               entry_size,
                                               (gchar*)&transl_entry))
    abort();
  transl_entry.position= (guint64)index;
  if (dyn_array->da_ops.ic_write_dynamic_array(dyn_array,
                                               pos_first,
                                               entry_size,
                                               (gchar*)&transl_entry))
    abort();
  return;
}

static void
free_translation_object(IC_DYNAMIC_TRANSLATION *ext_dyn_trans)
{
  IC_DYNAMIC_TRANSLATION_INT *dyn_trans=
    (IC_DYNAMIC_TRANSLATION_INT*)ext_dyn_trans;
  IC_DYNAMIC_ARRAY *dyn_array= dyn_trans->dyn_array;

  free_dynamic_ordered_array(dyn_array);
  ic_free((void*)dyn_trans);
}

IC_DYNAMIC_TRANSLATION*
create_translation_object()
{
  IC_DYNAMIC_ARRAY *dyn_array;
  IC_TRANSLATION_ENTRY transl_entry;
  IC_DYNAMIC_TRANSLATION_INT *dyn_trans;

  if (!(dyn_trans= (IC_DYNAMIC_TRANSLATION_INT*)ic_malloc(
                      sizeof(IC_DYNAMIC_TRANSLATION_INT))))
    return NULL;
  if (!(dyn_array= ic_create_ordered_dynamic_array()))
  {
    ic_free((void*)dyn_trans);
    return NULL;
  }
  transl_entry.object= (void*)0;
  if ((dyn_array->da_ops.ic_insert_dynamic_array(dyn_array,
                                         (const gchar*)&transl_entry,
                                         sizeof(transl_entry))))
  {
    ic_free((void*)dyn_trans);
    dyn_array->da_ops.ic_free_dynamic_array(dyn_array);
    return NULL;
  }
  dyn_trans->dt_ops.ic_insert_translation_object= insert_translation_object;
  dyn_trans->dt_ops.ic_remove_translation_object= remove_translation_object;
  dyn_trans->dt_ops.ic_free_translation_object= free_translation_object;
  dyn_trans->dyn_array= dyn_array;
  return (IC_DYNAMIC_TRANSLATION*)dyn_trans;
}
