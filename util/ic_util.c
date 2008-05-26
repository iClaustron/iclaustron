/* Copyright (C) 2007 iClaustron AB

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

static guint32 glob_debug= 0;
static gchar *glob_debug_file= "debug.log";
static guint32 glob_debug_screen= 0;

IC_STRING ic_version_file_string=
{(gchar*)"_version", (guint32)8, (gboolean)TRUE};
IC_STRING ic_config_string=
{(gchar*)"config", (guint32)6, (gboolean)TRUE};
IC_STRING ic_config_ending_string=
{(gchar*)".ini", (guint32)4, (gboolean)TRUE};

/*
  A couple of functions to set references to directories in the iClaustron
  installation.
*/

void
ic_create_config_version_file_name(IC_STRING *file_name,
                                   gchar *buf,
                                   IC_STRING *config_dir)
{
  buf[0]= 0;
  IC_INIT_STRING(file_name, buf, 0, TRUE);
  ic_add_ic_string(file_name, config_dir);
  ic_add_ic_string(file_name, &ic_config_string);
  ic_add_ic_string(file_name, &ic_version_file_string);
  ic_add_ic_string(file_name, &ic_config_ending_string);
}

/*
  Create a file name like $CONFIG_PATH/config.ini for initial version
  and $CONFIG_PATH/config.ini.3 if version number is 3.
*/
void
ic_create_config_file_name(IC_STRING *file_name,
                           gchar *buf,
                           IC_STRING *config_dir,
                           IC_STRING *name,
                           guint32 config_version_number)
{
  gchar int_buf[IC_MAX_INT_STRING];
  IC_STRING ending_string;

  buf[0]= 0;
  IC_INIT_STRING(file_name, buf, 0, TRUE);
  ic_add_ic_string(file_name, config_dir);
  ic_add_ic_string(file_name, name); 
  ic_add_ic_string(file_name, &ic_config_ending_string);
  if (config_version_number)
  {
    ic_set_number_ending_string(int_buf, (guint64)config_version_number);
    IC_INIT_STRING(&ending_string, int_buf, strlen(int_buf), TRUE);
    ic_add_ic_string(file_name, &ending_string);
  }
}

/* Create a string like ".3" if number is 3 */
void
ic_set_number_ending_string(gchar *buf, guint64 number)
{
  gchar *ignore_ptr;
  buf[0]= '.';
  ignore_ptr= ic_guint64_str(number,
                             &buf[1],
                             NULL);
}

static int
add_dir_slash(IC_STRING *dir)
{
#ifdef WINDOWS
  if (ic_add_dup_string(dir, "\\"))
#else
  if (ic_add_dup_string(dir, "/"))
#endif
    return IC_ERROR_MEM_ALLOC;
  return 0;
}

static int
set_default_dir(const gchar *default_dir, IC_STRING *dir,
                const gchar *input_dir)
{
  int error= IC_ERROR_MEM_ALLOC;
  gchar *c_str;

  IC_INIT_STRING(dir, NULL, 0, TRUE);
  if (input_dir == NULL)
  {
    /*
      The user specified no base/data directory himself, in this case we'll
      use $HOME/iclaustron_install as the base directory unless the user is
      the root user, in this case we'll instead use
      /var/lib/iclaustron_install as the default directory. This is also how
      the iClaustron will install the software by default. (iclaustron_data
      for data dir).
    */
    const gchar *user_name= g_get_user_name();
    if (strcmp(user_name, "root") == 0)
    {
#ifdef WINDOWS
      if (ic_add_dup_string(dir, "\\var\\lib\\"))
#else
      if (ic_add_dup_string(dir, "/var/lib/"))
#endif
        goto end;
    }
    else
    {
      const gchar *home_var= g_getenv("HOME");
      if (ic_add_dup_string(dir, home_var))
        goto end;
      if (add_dir_slash(dir))
        goto end;
    }
    if (ic_add_dup_string(dir, default_dir))
      goto end;
    if (add_dir_slash(dir))
      goto end;
  }
  else
  {
    if (ic_add_dup_string(dir, input_dir))
      goto end;
#ifdef WINDOWS
    c_str= "\\";
#else
    c_str= "/";
#endif
    if (dir->str[dir->len - 1] != c_str[0])
    {
      if (ic_add_dup_string(dir, c_str))
        goto end;
    }
  }
  error= 0;
end:
  return error;
}

int
ic_set_data_dir(IC_STRING *data_dir, const gchar *input_data_dir)
{
  return set_default_dir("iclaustron_data", data_dir,
                         input_data_dir);
}

int
ic_set_base_dir(IC_STRING *base_dir, const gchar *input_base_dir)
{
  return set_default_dir("iclaustron_install", base_dir,
                         input_base_dir);
}

void ic_make_iclaustron_version_string(IC_STRING *res_str, gchar *buf)
{
  buf[0]= 0;
  IC_INIT_STRING(res_str, buf, 0, TRUE);
  ic_add_string(res_str, (const gchar *)PACKAGE);
  ic_add_string(res_str, (const gchar *)"-");
  ic_add_string(res_str, (const gchar *)VERSION);
}

void ic_make_mysql_version_string(IC_STRING *res_str, gchar *buf)
{
  buf[0]= 0;
  IC_INIT_STRING(res_str, buf, 0, TRUE);
  ic_add_string(res_str, (const gchar *)MYSQL_VERSION_STRING);
}

void ic_set_relative_dir(IC_STRING *res_str, IC_STRING *dir,
                         gchar *buf, const gchar *dir_name)
{
  IC_INIT_STRING(res_str, buf, 0, TRUE);
  ic_add_ic_string(res_str, dir);
  ic_add_string(res_str, dir_name);
#ifdef WINDOWS
  ic_add_string(res_str, "\\");
#else
  ic_add_string(res_str, "/");
#endif
}

/* The default configuration directory is ICLAUSTRON_DATA_DIR/config */
int
ic_set_config_path(IC_STRING *config_dir,
                   gchar *config_path,
                   gchar *buf)
{
  int error;
  IC_STRING data_dir;
  if ((error= ic_set_data_dir(&data_dir, config_path)))
    return error;
  ic_set_relative_dir(config_dir, &data_dir, buf,
                      ic_config_string.str);
  DEBUG_PRINT(CONFIG_LEVEL, ("Config dir: %s", config_dir->str));
  return 0;
}

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
insert_simple_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array,
                            const gchar *buf, guint64 size)
{
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
find_pos_simple_dyn_array(IC_DYNAMIC_ARRAY *dyn_array, guint64 pos,
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
read_simple_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array, guint64 pos,
                          guint64 size, gchar *ret_buf)
{
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
write_simple_dynamic_array_to_disk(IC_DYNAMIC_ARRAY *dyn_array, int file_ptr)
{
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
free_simple_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array)
{
  release_dyn_buf(dyn_array->sd_array.first_dyn_buf);
  ic_free((void*)dyn_array);
}

IC_DYNAMIC_ARRAY*
ic_create_simple_dynamic_array()
{
  IC_DYNAMIC_ARRAY *dyn_array;
  IC_SIMPLE_DYNAMIC_ARRAY *sd_array;
  IC_DYNAMIC_ARRAY_OPS *da_ops;
  IC_SIMPLE_DYNAMIC_BUF *dyn_buf;

  if (!(dyn_array= (IC_DYNAMIC_ARRAY*)ic_calloc(sizeof(IC_DYNAMIC_ARRAY))))
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
  return dyn_array;
}

static int
insert_buf_in_ordered_dynamic_index(IC_DYNAMIC_ARRAY *dyn_array,
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
      if ((ret_code= insert_buf_in_ordered_dynamic_index(dyn_array,
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
insert_ordered_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array,
                             const gchar *buf, guint64 size)
{
  int ret_code;
  IC_SIMPLE_DYNAMIC_BUF *old_dyn_buf= dyn_array->sd_array.last_dyn_buf;
  IC_SIMPLE_DYNAMIC_BUF *new_last_dyn_buf, *loop_dyn_buf;
  IC_DYNAMIC_ARRAY_INDEX *last_dyn_index;
  guint64 buf_size= 0;
  guint32 ins_buf_count= 0, i;

  if ((ret_code= insert_simple_dynamic_array(dyn_array, buf, size)))
    return ret_code;
  new_last_dyn_buf= dyn_array->sd_array.last_dyn_buf;
  loop_dyn_buf= old_dyn_buf;
  while (new_last_dyn_buf != loop_dyn_buf)
  {
    loop_dyn_buf= loop_dyn_buf->next_dyn_buf;
    buf_size+= SIMPLE_DYNAMIC_ARRAY_BUF_SIZE;
    g_assert(size <= buf_size);
    last_dyn_index= dyn_array->ord_array.last_dyn_index;
    if ((ret_code= insert_buf_in_ordered_dynamic_index(dyn_array,
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
__attribute__ ((unused)) IC_DYNAMIC_ARRAY *dyn_array,
__attribute__ ((unused)) guint64 pos,
__attribute__ ((unused)) IC_SIMPLE_DYNAMIC_BUF **dyn_buf,
__attribute__ ((unused)) guint64 *buf_pos)
{
  return 0;
}

static int
read_ordered_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array, guint64 pos,
                           guint64 size, gchar *ret_buf)
{
  IC_SIMPLE_DYNAMIC_BUF *dyn_buf;
  guint64 buf_pos;
  int ret_code;

  if ((ret_code= find_pos_ordered_dyn_array(dyn_array, pos,
                                            &dyn_buf, &buf_pos)) || 
      (ret_code= read_pos_simple_dynamic_array(dyn_buf, buf_pos,
                                               size, ret_buf)))
    return ret_code;
  return 0;
}

static void
free_ordered_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array)
{
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
  free_simple_dynamic_array(dyn_array);
}

IC_DYNAMIC_ARRAY*
ic_create_ordered_dynamic_array()
{
  IC_DYNAMIC_ARRAY_OPS *da_ops;
  IC_DYNAMIC_ARRAY *dyn_array;
  IC_DYNAMIC_ARRAY_INDEX *dyn_index_array;
  IC_ORDERED_DYNAMIC_ARRAY *ord_array;

  if (!(dyn_index_array= (IC_DYNAMIC_ARRAY_INDEX*)ic_calloc(
           sizeof(IC_DYNAMIC_ARRAY_INDEX))))
    return NULL;
  if (!(dyn_array= ic_create_simple_dynamic_array()))
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
  return dyn_array;
}

/*
  Return highest bit set in a 32-bit integer, bit 0 is reported as 1 and
  no bit set is reported 0, thus we report one more than the bit index
  of the highest bit set
*/

guint32
ic_count_highest_bit(guint32 bit_var)
{
  guint32 i;
  guint32 bit_inx= 0;
  for (i= 0; i < 32; i++)
  {
    if (bit_var | (1 << i))
      bit_inx= i+1;
  }
  return bit_inx;
}

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
  guint32 buf_size;
  if (max_size == 0) /* No max_size given */
    buf_size= 8;
  else
    buf_size= (max_size + base_size - 1)/base_size;
  return buf_size;
}

static gchar*
mc_realloc_buf_array(IC_MEMORY_CONTAINER *mc_ptr, guint32 new_size)
{
  gchar *new_buf_array;
  guint32 old_buf_size, copy_buf_size;

  old_buf_size= mc_ptr->buf_array_size;
  copy_buf_size= IC_MIN(old_buf_size, new_size);

  if (!(new_buf_array= ic_calloc(new_size*sizeof(gchar*))))
    return NULL;
  memcpy(new_buf_array, mc_ptr->buf_array, copy_buf_size * sizeof(gchar*));
  ic_free(mc_ptr->buf_array);
  mc_ptr->buf_array= (gchar**)new_buf_array;
  mc_ptr->buf_array_size= new_size;
  return new_buf_array;
}

static gchar*
mc_alloc(IC_MEMORY_CONTAINER *mc_ptr, guint32 size)
{
  gchar *new_buf_array= NULL, *ptr, *new_buf;
  guint32 alloc_size, buf_inx;
  guint64 new_total_size;

  size= ic_align(size, 8); /* Always allocate on 8 byte boundaries */
  new_total_size= mc_ptr->total_size+size;
  if (mc_ptr->max_size > 0 && mc_ptr->max_size < new_total_size)
    return NULL;
  if (mc_ptr->current_free_len >= size)
  {
    /* Space is available, no need to allocate more */
    ptr= mc_ptr->current_buf;
    mc_ptr->current_buf+= size;
    mc_ptr->current_free_len-= size;
    mc_ptr->total_size= new_total_size;
    return ptr;
  }
  alloc_size= mc_ptr->base_size;
  if (size > alloc_size)
    alloc_size= size;
  buf_inx= mc_ptr->current_buf_inx + 1;
  if (buf_inx >= mc_ptr->buf_array_size)
  {
    if (!(new_buf_array= mc_realloc_buf_array(mc_ptr,
          mc_ptr->buf_array_size+16)))
      return NULL;
  }
  if (!(new_buf= ic_calloc(alloc_size)))
    return NULL;
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
  return new_buf;
}

static gchar*
mc_calloc(IC_MEMORY_CONTAINER *mc_ptr, guint32 size)
{
  gchar *ptr;
  if (!(ptr= mc_alloc(mc_ptr, size)))
    return NULL;
  memset(ptr, 0, size);
  return ptr;
}

static void
mc_reset(IC_MEMORY_CONTAINER *mc_ptr)
{
  guint32 orig_arr_size, i;
  guint32 first_alloc_buf;
  gchar *tmp;

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
    ic_free(mc_ptr->buf_array[i]);
 
  orig_arr_size= mc_get_base_buf_size(mc_ptr->max_size, mc_ptr->base_size);
  if (mc_ptr->buf_array_size > orig_arr_size)
    mc_realloc_buf_array(mc_ptr, orig_arr_size);
  memset(&mc_ptr->buf_array[1], 0,
         (mc_ptr->buf_array_size-1)*sizeof(gchar*));
  mc_ptr->current_buf= mc_ptr->buf_array[0];
  mc_ptr->total_size= 0;
  mc_ptr->current_buf_inx= 0;
  mc_ptr->current_free_len= mc_ptr->base_size;
  return;
}

static void
mc_free(IC_MEMORY_CONTAINER *mc_ptr)
{
  guint32 i;
  for (i= 0; i <= mc_ptr->current_buf_inx; i++)
    ic_free(mc_ptr->buf_array[i]);
  ic_free(mc_ptr->buf_array);
  ic_free(mc_ptr);
}

IC_MEMORY_CONTAINER*
ic_create_memory_container(guint32 base_size, guint32 max_size)
{
  gchar *buf_array_ptr, *first_buf;
  guint32 buf_size;
  IC_MEMORY_CONTAINER *mc_ptr;

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
  if (!(buf_array_ptr= ic_calloc(buf_size * sizeof(gchar*))))
    return NULL;
  if (!(mc_ptr= (IC_MEMORY_CONTAINER*)ic_calloc(sizeof(IC_MEMORY_CONTAINER))))
  {
    ic_free(buf_array_ptr);
    return NULL;
  }
  if (!(first_buf= ic_calloc(base_size)))
  {
    ic_free(buf_array_ptr);
    ic_free(mc_ptr);
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
  return mc_ptr;
}
 
static void
ic_reverse_str(gchar *in_buf, gchar *out_buf, gchar end_char)
{
  guint32 i= 0;
  guint32 j= 0;
  DEBUG_ENTRY("ic_reverse_str");

  while (in_buf[i] != end_char)
    i++;
  out_buf[i]= in_buf[i]; /* Copy end_char byte */
  while (i)
    out_buf[j++]= in_buf[--i];
}

gchar *ic_guint64_str(guint64 val, gchar *ptr, guint32 *len)
{
  guint32 i= 0;
  gchar buf[128];
  if (val == 0)
  {
    buf[0]= '0';
    i++;
  }
  while (val != 0)
  {
    guint tmp= val/10;
    tmp*= 10;
    tmp= val - tmp;
    buf[i]= (((gchar)'0') + (gchar)tmp);
    val/= 10;
    i++;
  }
  buf[i]= 0;
  ic_reverse_str((gchar*)&buf, ptr, 0);
  if (len)
    *len= i;
  return ptr;
}

gchar *ic_guint64_hex_str(guint64 val, gchar *ptr)
{
  guint32 i= 0;
  gchar buf[128];
  if (val == 0)
  {
    buf[0]= '0';
    i++;
  }
  while (val != 0)
  {
    guint tmp= val/16;
    tmp*= 16;
    tmp= val - tmp;
    if (tmp < 10)
      buf[i]= (((gchar)'0') + (gchar)tmp);
    else
      buf[i]= (((gchar)'A') + (gchar)(tmp-10));
    val/= 16;
    i++;
  }
  buf[i]= 0;
  ic_reverse_str((gchar*)&buf, ptr, 0);
  return ptr;
}

#ifdef DEBUG_BUILD
guint32
ic_get_debug()
{
  return glob_debug;
}

void ic_set_debug(guint32 val)
{
  glob_debug= val;
}

static FILE *ic_fptr;
void
ic_debug_entry(const char *entry_point)
{
  if (glob_debug_screen)
    printf("Entry into: %s\n", entry_point);
  fprintf(ic_fptr, "Entry into: %s\n", entry_point);
}

int ic_debug_open()
{
  ic_fptr= fopen(glob_debug_file, "w");
  if (ic_fptr == NULL)
  {
    printf("Failed to open %s\n", glob_debug_file);
    return 1;
  }
  fprintf(ic_fptr, "Entry into: \n");
  return 0;
}

void
ic_debug_print_char_buf(gchar *buf)
{
  if (glob_debug_screen)
    printf("%s\n", buf);
  fprintf(ic_fptr, "%s\n", buf);
}

void
ic_debug_print_rec_buf(char *buf, guint32 size)
{
  char p_buf[2049];
  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  ic_debug_printf("Receive buffer, size %u:\n%s", size, p_buf);
}

void
ic_debug_printf(const char *format,...)
{
  va_list args;
  char buf[2049];

  va_start(args, format);
  vsprintf(buf, format, args);
  if (glob_debug_screen)
  {
    printf("%s\n", buf);
  }
  fprintf(ic_fptr, "%s\n", buf);
  va_end(args);
}

void
ic_debug_close()
{
  fclose(ic_fptr);
}
#endif

/*
  MODULE: GENERIC_CONFIG_READER
  This module is a generic reader of configuration files.
  It has one external routine:
  build_config_data
  This routine will call into the supplied ic_config_operations
  methods. Those methods will build the actual data structures
  generated by a specific configuration file.
*/

const guint32 MAX_LINE_LEN = 120;

const gchar *false_str= "false";
const gchar *true_str= "true";

static gboolean
ic_check_digit(gchar c)
{
  if (c < '0' || c > '9')
    return FALSE;
  return TRUE;
}

gchar*
ic_convert_file_to_dir(gchar *buf, gchar *file_name)
{
  gchar *buf_start= buf;
  gchar *last_separator= NULL;
  gchar separator;

#ifndef WINDOWS
  separator= '\\';
#else
  separator= '/';
#endif
  while (*file_name)
  {
    *buf= *file_name;
    if (*buf == separator)
      last_separator= buf+1;
    buf++;
    file_name++;
  }
  if (last_separator)
  {
    /*
      We found a separator, thus filename contains a directory part.
      We insert a 0 after this separator to remove filename
      part from the full pathname.
    */
    *last_separator= 0;
  }
  else
  {
    /*
       We found no separator, thus the directory part is empty and we return
       an empty string.
    */
    *buf_start= 0;
  }
  return buf_start;
}

int
ic_conv_config_str_to_int(guint64 *value, IC_STRING *ic_str)
{
  guint32 i;
  guint64 number= 0;
  gboolean no_digit_found= FALSE;

  if (!ic_cmp_null_term_str(false_str, ic_str))
    return 0;
  else if (!ic_cmp_null_term_str(true_str, ic_str))
  {
    number= 1;
    return 0;
  }
  for (i= 0; i < ic_str->len; i++)
  {
    if (no_digit_found)
      return 1;
    if (ic_check_digit(ic_str->str[i]))
    {
      number*= 10;
      number+= (ic_str->str[i] - '0');
    }
    else
    {
      no_digit_found= TRUE;
      if ((ic_str->str[i] == 'k') ||
          (ic_str->str[i] == 'K'))
        number*= 1024;
      else if ((ic_str->str[i] == 'm') ||
               (ic_str->str[i] == 'M'))
        number*= (1024*1024);
      else if ((ic_str->str[i] == 'g') ||
               (ic_str->str[i] == 'G'))
        number*= (1024*1024*1024);
      else
        return 1;
    }
  }
  *value= number;
  return 0;
}

int
ic_conv_str_to_int(gchar *str, guint64 *number, guint32 *len)
{
  gchar *rev_str;
  guint32 str_len;
  guint64 base= 1;
  guint64 this_number;
  gchar end_char;
  gchar reverse_str[64];
  DEBUG_ENTRY("ic_conv_str_to_int");
  rev_str= reverse_str;
  *number= 0;

  for (str_len= 0; str_len < 62; str_len++)
  {
    if (!ic_check_digit(str[str_len]))
      break;
  }
  if (str_len > 60 || str_len == 0)
    return 1;
  end_char= str[str_len];
  if (len)
    *len= str_len;
  ic_reverse_str(str, rev_str, end_char);
  while (*rev_str != end_char)
  {
    this_number= (*rev_str - '0');
    *number= *number + (this_number * base);
    base *= 10;
    rev_str++;
  }
  return 0;
}

static
guint32 read_cr_line(gchar *iter_data)
{
  gsize len= 1;
  do
  {
    gchar iter_char= *iter_data;
    if (iter_char == CARRIAGE_RETURN ||
        iter_char == 0)
      return len;
    len++;
    iter_data++;
  } while (len < MAX_LINE_LEN);
  return 0;
}

static
gchar *conv_group_id(gchar *group_id, guint32 len)
{
  guint32 iter_len= 0;
  gchar *save_group_id= group_id;
  while (iter_len < len)
  {
    gchar c= *group_id;
    if (g_ascii_isalpha(c))
    {
      c= g_ascii_tolower(c);
      *group_id= c;
    }
    else if (c != ' ')
      return NULL;
    group_id++;
    iter_len++;
  }
  return save_group_id;    
}

static
gchar *conv_key_id(gchar *key_id, guint32 *len)
{
  gchar *save_key_id= key_id;
  guint32 iter_len= 0;
  while (iter_len < *len)
  {
    gchar c= *key_id;
    if (!g_ascii_isalpha(c) && c != '_')
    {
      *len= iter_len;
      return save_key_id;
    }
    *key_id= g_ascii_tolower(c);
    iter_len++;
    key_id++;
  }
  return NULL;
}

static
gchar *rm_space(gchar *val_str, guint32 *iter_len, guint32 len)
{
  for ( ; 
       (g_ascii_isspace(val_str[0]) ||
        val_str[0] == '\t')
       && *iter_len < len;
       val_str++, (*iter_len)++)
    ;
  return val_str;
}

static
gchar *conv_key_value(gchar *val_str, guint32 *len)
{
  gboolean found= FALSE;
  guint32 iter_len= 0;
  gchar *save_val_str;
  gchar *first_val_str= val_str;

  val_str= rm_space(val_str, &iter_len, *len);
  if (*len == iter_len || (val_str[0] != '=' && val_str[0] != ':'))
    return NULL;
  val_str++;
  iter_len++;
  val_str= rm_space(val_str, &iter_len, *len);
  save_val_str= val_str;
  while (iter_len < *len)
  {
    gchar c= *val_str;
    if (!(g_ascii_isalpha(c) || c == '/' || c == '\\' ||
        g_ascii_isdigit(c) || '.' ||
        c == '_' || g_ascii_isspace(c)))
      return NULL;
    val_str++;
    iter_len++;
    found= TRUE;
  }
  if (!found)
    return NULL;
  *len= (guint32)(((first_val_str + *len) - save_val_str) - 1);
  return save_val_str;
}

static
guint32 read_config_line(IC_CONFIG_OPERATIONS *conf_ops,
                         IC_CONFIG_STRUCT *conf_obj,
                         IC_STRING *line_data,
                         guint32 line_number,
                         guint32 *section_num,
                         guint32 pass)
{
  gchar *iter_data= line_data->str;
  guint32 line_len= line_data->len;
  DEBUG_ENTRY("read_config_line");

  if (line_len == 1)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("Line number %d in section %d is an empty line", line_number,
       *section_num));
    return 0;
  }
  else if (*iter_data == '#')
  {
    /*
      We have found a correct comment line, report it to the configuration
      object.
    */
    IC_STRING comment_str;
    IC_INIT_STRING(&comment_str, iter_data, line_len, FALSE);
    return conf_ops->ic_add_comment(conf_obj, line_number, *section_num,
                                    &comment_str, pass);
  }
  else if (*iter_data == '[')
  {
    if (iter_data[line_len - 2] != ']')
      return IC_ERROR_CONFIG_BRACKET;
    else
    {
      IC_STRING section_name;
      guint32 group_id_len= line_len - 3;
      gchar *group_id= conv_group_id(&iter_data[1], group_id_len);
      if (!group_id)
        return IC_ERROR_CONFIG_INCORRECT_GROUP_ID;
      /*
        We have found a correct group id, now tell the configuration
        object about this and let him decide if this group id is
        correct according to the definition of this configuration
        object.
      */
      IC_INIT_STRING(&section_name, group_id, group_id_len, FALSE);
      (*section_num)++;
      return conf_ops->ic_add_section(conf_obj, *section_num, line_number,
                                      &section_name, pass);
    }
  }
  else
  {
    IC_STRING key_name, data_str;
    guint32 id_len= line_len;
    guint32 val_len= 0;
    guint32 space_len= 0;
    gchar *key_id= NULL;
    gchar *val_str= NULL;

    rm_space(iter_data, &space_len, line_len);
    id_len-= space_len;
    if (!(key_id= conv_key_id(&iter_data[space_len], &id_len)) ||
        ((val_len= line_len - (id_len + space_len)), FALSE) ||
        (!(val_str= conv_key_value(&iter_data[id_len+space_len], &val_len))))
      return IC_ERROR_CONFIG_IMPROPER_KEY_VALUE;
    /*
      We have found a correct key-value pair. Now let the configuration object
      decide whether this key and value makes sense and if so insert it into
      the configuration.
    */
    IC_INIT_STRING(&key_name, key_id, id_len, FALSE);
    IC_INIT_STRING(&data_str, val_str, val_len, FALSE);
    return conf_ops->ic_add_key(conf_obj, *section_num, line_number,
                                &key_name, &data_str, pass);
  }
}
/*
  This method is used to build the configuration data structure from reading
  the configuration file. It's designed to be reusable for any type of
  configuration file. The configuration file is always read in two passes,
  the first usually gathers some information about size of various parts.
  
  PARAMETERS:
  conf_data      This parameter contains the configuration file as a long string
  ic_conf_op     This contains the function pointers for this specific config
                 file type
  ic_config      This contains the pointer to the data structure where the
                 configuration data is placed, this is done through a union
                 of pointers to one struct per configuration file type
  err_obj        Any error information is returned in this object
*/

int ic_build_config_data(IC_STRING *conf_data,
                         IC_CONFIG_STRUCT *ic_config,
                         IC_CONFIG_ERROR *err_obj)
{
  guint32 line_number;
  guint32 line_length, pass;
  int error;
  guint32 section_num;
  IC_STRING line_data;
  IC_CONFIG_OPERATIONS *ic_conf_op= ic_config->clu_conf_ops;
  DEBUG_ENTRY("ic_build_config_data");

  if (conf_data->len == 0)
  {
    err_obj->err_num= 1;
    err_obj->line_number= 0;
    return 1;
  }
  for (pass= 0; pass < 2; pass++)
  {
    gchar *iter_data= conf_data->str;
    gsize iter_data_len= 0;
    line_number= 1;
    section_num= 0;
    if ((error= ic_conf_op->ic_config_init(ic_config, pass)))
      goto config_error;
    while (iter_data_len < conf_data->len)
    {
      if (*iter_data == LINE_FEED)
      {
        /* Special handling of Windows Line Feeds after Carriage Return */
        DEBUG_PRINT(CONFIG_LEVEL, ("Special case"));
        iter_data_len++;
        iter_data++;
        continue;
      }
      error= IC_ERROR_CONFIG_LINE_TOO_LONG;
      if (!(line_length= read_cr_line(iter_data)))
        goto config_error;
      IC_INIT_STRING(&line_data, iter_data, line_length, FALSE);
      if ((error= read_config_line(ic_conf_op, ic_config,
                                   &line_data, line_number, &section_num,
                                   pass)))
        goto config_error;
      iter_data+= line_length;
      iter_data_len+= line_length;
      line_number++;
    }
  }
  if ((error= ic_conf_op->ic_config_verify(ic_config)))
    goto config_error;
  return 0;
config_error:
  ic_conf_op->ic_config_end(ic_config);
  err_obj->err_num= error;
  err_obj->line_number= line_number;
  return 1;
}

/*
  MODULE:
  iClaustron STRING ROUTINES
  --------------------------
  Some routines to handle iClaustron strings
*/

guint32
ic_str_find_first(IC_STRING *ic_str, gchar searched_char)
{
  guint32 i;
  for (i= 0; i < ic_str->len; i++)
  {
    if (ic_str->str[i] == searched_char)
      return i;
  }
  return ic_str->len;
}

void
ic_add_string(IC_STRING *dest_str, const gchar *input_str)
{
  IC_STRING input_ic_str;

  IC_INIT_STRING(&input_ic_str, (gchar*)input_str, strlen(input_str), TRUE);
  ic_add_ic_string(dest_str, &input_ic_str);
}

int
ic_add_dup_string(IC_STRING *dest_str, const gchar *add_str)
{
  guint32 add_len= strlen(add_str);
  guint32 orig_len= dest_str->len;
  guint32 new_len= add_len + orig_len;
  gchar *new_str= ic_malloc(new_len + 1);
  if (new_str == NULL)
    return IC_ERROR_MEM_ALLOC;
  if (orig_len > 0)
    memcpy(new_str, dest_str->str, orig_len);
  memcpy(new_str + orig_len, add_str, add_len);
  if (dest_str->is_null_terminated)
    new_str[new_len]= 0;
  if (dest_str->str)
    ic_free(dest_str->str);
  dest_str->str= new_str;
  dest_str->len= new_len;
  return 0;
}

void
ic_add_ic_string(IC_STRING *dest_str, IC_STRING *input_str)
{
  gchar *start_ptr= dest_str->str+dest_str->len;
  gchar *end_ptr;
  memcpy(start_ptr, input_str->str, input_str->len);
  if (dest_str->is_null_terminated)
  {
    end_ptr= start_ptr + input_str->len;
    *end_ptr= 0;
  }
  dest_str->len+= input_str->len;
}

gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr)
{
  guint32 i;
  buf_ptr[0]= 0;
  if (str && str->str)
  {
    for (i= 0; i < str->len; i++)
      buf_ptr[i]= str->str[i];
    buf_ptr[str->len]= 0;
  }
  return buf_ptr;
}

void ic_print_ic_string(IC_STRING *str)
{
  if (str->is_null_terminated)
    printf("%s\n", str->str);
  else
  {
    guint32 str_len= 0;
    while (str_len < str->len)
    {
      putchar(str->str[str_len]);
      str_len++;
    }
    putchar('\n');
  }
}

int ic_cmp_null_term_str(const gchar *null_term_str, IC_STRING *cmp_str)
{
  guint32 iter_len= 0;
  gchar *cmp_char= cmp_str->str;
  guint32 str_len= cmp_str->len;
  if (cmp_str->is_null_terminated)
    return (strcmp(null_term_str, cmp_str->str) == 0) ? 1 : 0;
  while (iter_len < str_len)
  {
    if (*null_term_str != *cmp_char)
      return 1;
    null_term_str++;
    cmp_char++;
    iter_len++;
  }
  if (*null_term_str == 0)
    return 0;
  return 1;
}

void
ic_set_up_ic_string(IC_STRING *in_out_str)
{
  guint32 i;
  if (!in_out_str->str)
  {
    in_out_str->len= 0;
    in_out_str->is_null_terminated= FALSE;
    return;
  }
  for (i= 0; i < in_out_str->len; i++)
  {
    if (in_out_str->str[i])
      continue;
    else
    {
      in_out_str->len= i;
      in_out_str->is_null_terminated= TRUE;
      return;
    }
  }
  in_out_str->is_null_terminated= FALSE;
}

int ic_mc_strdup(IC_MEMORY_CONTAINER *mc_ptr, IC_STRING *out_str,
                 IC_STRING *in_str)
{
  gchar *str;
  IC_INIT_STRING(out_str, NULL, 0, FALSE);
  if (in_str->len == 0)
    return 0;
  if (mc_ptr)
  {
    if (!(str= mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, in_str->len+1)))
      return 1;
  }
  else
  {
    if (!(str= ic_malloc(in_str->len+1)))
      return 1;
  }
  IC_COPY_STRING(out_str, in_str);
  out_str->str= str;
  memcpy(out_str->str, in_str->str, in_str->len);
  if (out_str->is_null_terminated)
    out_str->str[out_str->len]= NULL_BYTE;
  return 0;
}

int
ic_strdup(IC_STRING *out_str, IC_STRING *in_str)
{
  return ic_mc_strdup(NULL, out_str, in_str);
}


/*
  Module for printing error codes and retrieving error messages
  given the error number.
  To add a new error number do the following:
  1) Change IC_LAST_ERROR
  2) Add a new entry in ic_init_error_messages
  3) Add the new error code in ic_err.h
*/

#define IC_FIRST_ERROR 7000
#define IC_LAST_ERROR 7020
#define IC_MAX_ERRORS 100
static gchar* ic_error_str[IC_MAX_ERRORS];
static gchar *no_such_error_str= "No such error";
void
ic_init_error_messages()
{
  guint32 i;
  DEBUG_ENTRY("ic_init_error_messages");
  for (i= 0; i < IC_MAX_ERRORS; i++)
    ic_error_str[i]= NULL;
  ic_error_str[IC_ERROR_CONFIG_LINE_TOO_LONG -IC_FIRST_ERROR]=
    "Line was longer than 120 characters";
  ic_error_str[IC_ERROR_CONFIG_BRACKET - IC_FIRST_ERROR]=
    "Missing ] after initial [";
  ic_error_str[IC_ERROR_CONFIG_INCORRECT_GROUP_ID - IC_FIRST_ERROR]=
    "Found incorrect group id";
  ic_error_str[IC_ERROR_CONFIG_IMPROPER_KEY_VALUE - IC_FIRST_ERROR]=
    "Improper key-value pair";
  ic_error_str[IC_ERROR_CONFIG_NO_SUCH_SECTION - IC_FIRST_ERROR]=
    "Section name doesn't exist in this configuration";
  ic_error_str[IC_ERROR_MEM_ALLOC - IC_FIRST_ERROR]=
    "Memory allocation failure";
  ic_error_str[IC_ERROR_NO_SECTION_DEFINED_YET - IC_FIRST_ERROR]=
    "Tried to define key value before first section defined";
  ic_error_str[IC_ERROR_NO_SUCH_CONFIG_KEY - IC_FIRST_ERROR]=
    "No such configuration key exists";
  ic_error_str[IC_ERROR_DEFAULT_VALUE_FOR_MANDATORY - IC_FIRST_ERROR]=
    "Trying to assign default value to a mandatory config entry";
  ic_error_str[IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION - IC_FIRST_ERROR]=
    "Assigning correct config entry in wrong section";
  ic_error_str[IC_ERROR_NO_NODES_FOUND - IC_FIRST_ERROR]=
    "No nodes found in the configuration file";
  ic_error_str[IC_ERROR_WRONG_CONFIG_NUMBER - IC_FIRST_ERROR]=
    "Number expected in config file, true, false and endings with k, m, g also allowed";
  ic_error_str[IC_ERROR_NO_BOOLEAN_VALUE - IC_FIRST_ERROR]=
    "Boolean value expected, got number larger than 1";
  ic_error_str[IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS - IC_FIRST_ERROR]=
    "Configuration value is out of bounds, check data type and min, max values";
  ic_error_str[IC_ERROR_NO_SERVER_NAME - IC_FIRST_ERROR]=
    "Server name must be provided in all connections";
  ic_error_str[IC_ERROR_NO_SERVER_PORT - IC_FIRST_ERROR]=
    "Server port must be provided in all connections";
  ic_error_str[IC_ERROR_GETADDRINFO - IC_FIRST_ERROR]=
    "Provided client/server name/port not found by getaddrinfo";
  ic_error_str[IC_ERROR_ILLEGAL_SERVER_PORT - IC_FIRST_ERROR]=
    "Provided server port isn't a legal port number";
  ic_error_str[IC_ERROR_DIFFERENT_IP_VERSIONS - IC_FIRST_ERROR]=
    "Trying to use IPv4 and IPv6 simultaneously on server/client part not supported";
  ic_error_str[IC_ERROR_ILLEGAL_CLIENT_PORT - IC_FIRST_ERROR]=
    "Provided client port isn't a legal port number";
  ic_error_str[IC_ERROR_INCONSISTENT_DATA - IC_FIRST_ERROR]=
    "Internal data structure error";
}

void
ic_print_error(guint32 error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    printf("%s\n", no_such_error_str);
  else
    printf("%s\n", ic_error_str[error_number - IC_FIRST_ERROR]);
}

gchar *ic_get_error_message(guint32 error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    return no_such_error_str;
  else
    return ic_error_str[error_number - IC_FIRST_ERROR];
}

static GOptionEntry debug_entries[] = 
{
  { "debug_level", 0, 0, G_OPTION_ARG_INT, &glob_debug,
    "Set Debug Level", NULL},
  { "debug_file", 0, 0, G_OPTION_ARG_STRING, &glob_debug_file,
    "Set Debug File", NULL},
  { "debug_screen", 0, 0, G_OPTION_ARG_INT, &glob_debug_screen,
    "Flag whether to print debug info to stdout", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static gchar *help_debug= "Group of flags to set-up debugging";
static gchar *debug_description= "\
Group of flags to set-up debugging level and where to pipe debug output \n\
Debug level is actually several levels, one can decide to debug a certain\n\
area at a time. Each area has a bit in debug level. So if one wants to\n\
debug the communication area one sets debug_level to 1 (set bit 0). By\n\
setting several bits it is possible to debug several areas at once.\n\
\n\
Current levels defined: \n\
  Level 0 (= 1): Communication debugging\n\
  Level 1 (= 2): Entry into functions debugging\n\
  Level 2 (= 4): Configuration debugging\n\
  Level 3 (= 8): Debugging specific to the currently executing program\n\
  Level 4 (=16): Debugging of threads\n\
";

int
ic_start_program(int argc, gchar *argv[], GOptionEntry entries[],
                 gchar *start_text)
{
  int ret_code= 1;
  GError *error= NULL;
  GOptionGroup *debug_group;
  GOptionContext *context;

  context= g_option_context_new(start_text);
  if (!context)
    goto mem_error;
  /* Read command options */
  g_option_context_add_main_entries(context, entries, NULL);
  debug_group= g_option_group_new("debug", debug_description,
                                  help_debug, NULL, NULL);
  if (!debug_group)
    goto mem_error;
  g_option_group_add_entries(debug_group, debug_entries);
  g_option_context_add_group(context, debug_group);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);
  if ((ret_code= ic_init()))
    return ret_code;
  return 0;

parse_error:
  printf("No such program option: %s\n", error->message);
  goto error;
mem_error:
  printf("Memory allocation error\n");
  goto error;
error:
  return ret_code;
}

