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
#include <unistd.h>
#include <stdio.h>
#include <ic_common_header.h>
#include <errno.h>

gchar *
ic_calloc(size_t size)
{
  return g_try_malloc0(size);
}

gchar *
ic_malloc(size_t size)
{
  return g_try_malloc(size);
}

void
ic_free(void *ret_obj)
{
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
ic_read_file(int file_ptr, gchar *buf, size_t size, guint32 *len)
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
