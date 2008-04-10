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
    if (ret_code == size)
      return 0;
    buf+= ret_code;
    size-= ret_code;
  } while (1);
  return 0;
}
