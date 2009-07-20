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
ic_check_buf_with_signed_int(gchar *read_buf, guint32 read_size,
                             const gchar *str,
                             int str_len,
                             guint64 *number,
                             int *sign_flag)
{
  *sign_flag= FALSE;
  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0) ||
      ((str_len= (read_buf[str_len] == '-') ? str_len+1 : str_len), FALSE) ||
      (ic_convert_str_to_int_fixed_size(read_buf+str_len, read_size - str_len,
                                        number)))
    return TRUE;
  if (read_buf[str_len] == '-')
    *sign_flag= TRUE;
  return FALSE;
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

/*
  Receive a protocol string like "node1: 12"
  Here str contains "node1: " and 12 is returned in id.
  If optional is set the line is optional and ok will
  be returned even if there is no such line.
*/
static int
ic_rec_number_impl(IC_CONNECTION *conn,
                   const gchar *str,
                   guint32 *id,
                   gboolean optional)
{
  gchar *read_buf;
  guint32 read_size;
  int error;
  guint64 local_id;

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_int(read_buf, read_size, str,
                               strlen(str),
                               &local_id))
    {
      if (local_id >= IC_MAX_UINT32)
        return IC_PROTOCOL_ERROR;
      *id= (guint32)local_id;
      return 0;
    }
    if (!optional)
      return IC_PROTOCOL_ERROR;
    ic_step_back_rec_with_cr(conn, read_size);
    *id= 0;
    return 0;
  }
  return error;
}

int
ic_rec_number(IC_CONNECTION *conn, const gchar *str, guint32 *number)
{
  return ic_rec_number_impl(conn, str, number, FALSE);
}

int
ic_rec_opt_number(IC_CONNECTION *conn, const gchar *str, guint32 *number)
{
  return ic_rec_number_impl(conn, str, number, TRUE);
}

int
ic_rec_int_number(IC_CONNECTION *conn,
                  const gchar *str,
                  int *id)
{
  gchar *read_buf;
  guint32 read_size;
  int error;
  gboolean sign_flag;
  guint64 local_id;

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_signed_int(read_buf, read_size, str,
                                      strlen(str),
                                      &local_id, &sign_flag))
    {
      if (local_id >= IC_MAX_UINT32)
        return IC_PROTOCOL_ERROR;
      *id= (guint32)local_id;
      if (sign_flag)
        *id= -(*id);
      return 0;
    }
    return IC_PROTOCOL_ERROR;
  }
  return error;
}

int
ic_send_with_cr_with_num(IC_CONNECTION *conn,
                         const gchar *in_buf,
                         guint64 number)
{
  gchar out_buf[128], buf[64];

  g_snprintf(out_buf, 128, "%s%s", in_buf,
             ic_guint64_str(number, buf, NULL));
  return ic_send_with_cr(conn, out_buf);
}

int
ic_rec_empty_line(IC_CONNECTION *conn)
{
  return ic_rec_simple_str(conn, "");
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
ic_send_with_cr_composed(IC_CONNECTION *conn,
                         gchar **buf,
                         guint32 num_strings)
{
  gchar local_buf[256];
  guint32 local_pos= 0;
  guint32 i, len;

  for (i= 0; i < num_strings; i++)
  {
    len= strlen(buf[i]);
    memcpy(local_buf, buf[i], len);
    local_pos+= len;
  }
  local_buf[local_pos]= 0;
  return ic_send_with_cr(conn, local_buf);
}

int
ic_send_with_cr_two_strings(IC_CONNECTION *conn,
                            const gchar *buf1,
                            const gchar *buf2)
{
  const gchar *buf[2];
  const gchar **local_buf= &buf[0];
  buf[0]= buf1;
  buf[1]= buf2;
  return ic_send_with_cr_composed(conn, local_buf, (guint32)2);
}
