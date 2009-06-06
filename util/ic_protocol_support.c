/* Copyight (C) 2009 iClaustron AB

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
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_protocol_support.h>

int
ic_check_buf(gchar *read_buf, guint32 read_size, const gchar *str, int str_len)
{
  if ((read_size != (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0))
    return TRUE;
  return FALSE;
}

int
ic_check_buf_with_many_int(gchar *read_buf, guint32 read_size, const gchar *str,
                        int str_len, guint32 num_elements,
                        guint64 *number)
{
  gchar *ptr, *end_ptr;
  guint32 i, num_chars;

  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) == 0))
  {
    ptr= read_buf+str_len;
    end_ptr= read_buf + read_size;
    for (i= 0; i < num_elements; i++)
    {
      num_chars= ic_count_characters(ptr, end_ptr - ptr);
      if (ptr + num_chars > end_ptr)
        goto error;
      if (ic_convert_str_to_int_fixed_size(ptr, num_chars, &number[i]))
        goto error;
      ptr+= num_chars;
      if (*ptr == ' ')
        ptr++;
      else if (*ptr != '\n' || i != (num_elements - 1))
        goto error;
    }
  }
  return FALSE;
error:
  return TRUE;
}

int
ic_check_buf_with_int(gchar *read_buf, guint32 read_size, const gchar *str,
                   int str_len, guint64 *number)
{
  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0) ||
      (ic_convert_str_to_int_fixed_size(read_buf+str_len, read_size - str_len,
                                        number)))
    return TRUE;
  return FALSE;
}

int
ic_check_buf_with_string(gchar *read_buf, guint32 read_size, const gchar *str,
                         int str_len, IC_STRING **string)
{
  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0))
    return TRUE;
  (*string)->len= read_size - str_len;
  (*string)->str= read_buf+str_len;
  (*string)->is_null_terminated= FALSE;
  return FALSE;
}

int
ic_rec_simple_str(IC_CONNECTION *conn, const gchar *str)
{
  gchar *read_buf;
  guint32 read_size;
  int error;

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (ic_check_buf(read_buf, read_size, str,
                     strlen(str)))
    {
      DEBUG_PRINT(CONFIG_LEVEL,
        ("Protocol error in waiting for %s", str));
      DEBUG_RETURN(IC_PROTOCOL_ERROR);
    }
    DEBUG_RETURN(0);
  }
  DEBUG_RETURN(error);
}

int
ic_send_key(IC_CONNECTION *conn,
            const gchar *grid_name,
            const gchar *cluster_name,
            const gchar *node_name)
{
  int error;
  gchar grid_line[256], cluster_line[256], node_line[256];

  g_snprintf(grid_line, 256, "grid name: %s", grid_name);
  g_assert(!(!cluster_name && node_name));
  if (cluster_name)
    g_snprintf(cluster_line, 256, "cluster name: %s", cluster_name);
  if (node_name)
    g_snprintf(node_line, 256, "node name: %s", node_name);
  if ((error= ic_send_with_cr(conn, grid_line)) ||
      (cluster_name && (error= ic_send_with_cr(conn, cluster_line))) ||
      (node_name && (error= ic_send_with_cr(conn, node_line))))
    return error;
  return 0;
}

int
ic_send_version(IC_CONNECTION *conn,
                const gchar *version_string)
{
  int error;
  gchar version_line[256];

  g_snprintf(version_line, 256, "version: %s", version_string);
  if ((error= ic_send_with_cr(conn, version_line)))
    return error;
  return 0;
}

int
ic_send_program(IC_CONNECTION *conn,
                const gchar *program_name)
{
  int error;
  gchar program_line[256];

  g_snprintf(program_line, 256, "program: %s", program_name);
  if ((error= ic_send_with_cr(conn, program_line)))
    return error;
  return 0;
}

int
ic_send_error(IC_CONNECTION *conn,
              const gchar *error_message)
{
  int error;
  gchar error_line[256];

  g_snprintf(error_line, 256, "error: %s", error_message);
  if ((error= ic_send_with_cr(conn, error_line)))
    return error;
  return 0;
}

int
ic_send_pid(IC_CONNECTION *conn,
            const GPid pid)
{
  int error;
  guint32 len;
  gchar pid_buf[128], pid_line[140];

  ic_guint64_str((guint64)pid, pid_buf, &len);
  g_snprintf(pid_line, 140, "pid: %s", pid_buf);
  if ((error= ic_send_with_cr(conn, pid_line)))
    return error;
  return 0;
}

int
ic_send_empty_line(IC_CONNECTION *conn)
{
  int error;

  if ((error= ic_send_with_cr(conn, "")))
    return error;
  return 0;
}

int
ic_receive_key(IC_CONNECTION *conn,
               gchar *read_buf,
               IC_STRING *grid_name,
               IC_STRING *cluster_name,
               IC_STRING *node_name)
{
  int error;
  guint32 read_size;

  if (!(error= ic_rec_with_cr(conn, read_buf, &read_size)))
  {
  }
}

