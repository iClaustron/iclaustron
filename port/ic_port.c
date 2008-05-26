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

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <stdio.h>
#include <ic_common_header.h>
#include <errno.h>

static gchar **alloc_array= NULL;
static guint64 num_allocs= 0;
static guint64 alloc_inx= 0;

static int
insert_malloc_stat(gchar *ptr)
{
  guint64 current_inx;
  guint64 i;
  gchar *new_alloc_array;

  if (!alloc_array)
  {
    alloc_array= malloc(8192 * sizeof(gchar*));
    if (!alloc_array)
      return 1;
    num_allocs= 8192;
  }
  /* Allocate array */
  alloc_array[alloc_inx]= ptr;
  alloc_inx++;
  if (alloc_inx < 8192)
    return 0;
  /* Time for a reorganize */
  current_inx= 0;
  for (i= 0; i < num_allocs; i++)
  {
    if (alloc_array[i])
    {
      alloc_array[current_inx]= alloc_array[i];
      current_inx++;
    }
  }
  if (current_inx == (guint64)num_allocs)
  {
    new_alloc_array= malloc(2 * num_allocs * sizeof(gchar*));
    if (!new_alloc_array)
      return 1;
    memcpy(new_alloc_array, alloc_array, num_allocs * sizeof(gchar*));
    free(alloc_array);
    num_allocs *= 2;
  }
  else
  {
    alloc_inx= current_inx;
  }
  return 0;
}

static int
free_check(gchar *ptr)
{
  guint64 i;
  for (i= 0; i < alloc_inx; i++)
  {
    if (alloc_array[i] == ptr)
    {
      alloc_array[i]= NULL;
      return 0;
    }
  }
  return 1;
}

gchar *
ic_calloc(size_t size)
{
  gchar *alloc_ptr= g_try_malloc0(size);
#ifdef DEBUG
  if (insert_malloc_stat(alloc_ptr))
    abort();
#endif
  return alloc_ptr;
}

gchar *
ic_malloc(size_t size)
{
  gchar *alloc_ptr= g_try_malloc(size);
#if 0
  if (insert_malloc_stat(alloc_ptr))
    abort();
#endif
  return alloc_ptr;
}

void
ic_free(void *ret_obj)
{
#if 0
  if (free_check(ret_obj))
    abort();
#endif
  g_free(ret_obj);
}

guint32
ic_get_own_pid()
{
  pid_t pid;
  pid= getpid();
  return (guint32)pid;
}

int run_process(gchar **argv,
                int *exit_status)
{
  GError *error;
  if (g_spawn_sync(NULL,
                   argv,
                   NULL,
                   G_SPAWN_FILE_AND_ARGV_ZERO | G_SPAWN_SEARCH_PATH,
                   NULL,
                   NULL,
                   NULL,
                   NULL,
                   exit_status,
                   &error))
    return 0;
  printf("Exit status %d", *exit_status);
  return *exit_status;
}

int
ic_is_process_alive(guint32 pid,
                    gchar *process_name,
                    gchar **err_msg)
{
  gchar *argv[5];
  gint exit_status;
  guint64 value= (guint64)pid;
  gchar *pid_number_str;
  int error;
  gchar buf[64];

  pid_number_str= ic_guint64_str(value, buf, NULL);
#ifdef LINUX
  argv[0]= "linux_check_process.sh";
#endif
#ifdef MACOSX
  argv[0]= "macosx_check_process.sh";
#endif
#ifdef SOLARIS
  argv[0]= "solaris_check_process.sh";
#endif
  argv[1]= "--process_name";
  argv[2]= process_name;
  argv[3]= "--pid";
  argv[4]= pid_number_str;

  error= run_process(argv, &exit_status);
  if (error == (gint)1)
  {
    *err_msg= "Process is still running";
    return 1;
  }
  else if (error == (gint)2)
  {
    *err_msg= "Error in script finding process state";
    return 2;
  }
  return 0;
}

int
ic_delete_file(const gchar *file_name)
{
  return g_unlink(file_name);
}

static int
get_file_length(int file_ptr, guint64 *read_size)
{
  gint64 size;
  int error;
  size= lseek(file_ptr, (off_t)0, SEEK_END);
  if (size == (gint64)-1)
    goto error;
  *read_size= size;
  size= lseek(file_ptr, (off_t)0, SEEK_SET);
  if (size == (gint64)-1)
    goto error;
  if (size != 0)
    return 1;
  return 0;
error:
  error= errno;
  return error;
}

int
ic_get_file_contents(const gchar *file, gchar **file_content,
                     guint64 *file_size)
{
  int file_ptr;
  int error;
  gchar *loc_ptr;
  guint64 read_size, size_left;
  DEBUG_ENTRY("ic_get_file_contents");

  file_ptr= g_open(file, O_RDONLY);
  if (file_ptr == (int)-1)
    goto error;
  if ((error= get_file_length(file_ptr, file_size)))
    return error;
  if (!(loc_ptr= ic_malloc((*file_size) + 1)))
    return IC_ERROR_MEM_ALLOC;
  loc_ptr[*file_size]= 0;
  *file_content= loc_ptr;
  size_left= *file_size;
  do
  {
    if ((error= ic_read_file(file_ptr, loc_ptr, size_left, &read_size)))
    {
      ic_free(*file_content);
      *file_size= 0;
      return error;
    }
    if (read_size == size_left)
      return 0;
    loc_ptr+= read_size;
    size_left-= read_size;
  } while (1);
error:
  error= errno;
  return error;
}

int
ic_write_file(int file_ptr, const gchar *buf, size_t size)
{
  int ret_code;
  do
  {
    ret_code= write(file_ptr, (const void*)buf, size);
    if (ret_code == (int)-1)
    {
      ret_code= errno;
      return ret_code;
    }
    if (ret_code == (int)size)
      return 0;
    buf+= ret_code;
    size-= ret_code;
  } while (1);
  return 0;
}

int
ic_read_file(int file_ptr, gchar *buf, size_t size, guint64 *len)
{
  int ret_code;
  ret_code= read(file_ptr, (void*)buf, size);
  if (ret_code == (int)-1)
  {
    ret_code= errno;
    return ret_code;
  }
  *len= (guint32)ret_code;
  return 0;
}
