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
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_connection.h>
#include "ic_connection_int.h"
#include <ic_proto_str.h>

void
ic_print_buf(char *buf, guint32 size)
{
  char p_buf[2049];

  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  ic_printf("Receive buffer, size %u:\n%s", size, p_buf);
}

int
ic_send_with_cr(IC_CONNECTION *ext_conn, const gchar *send_buf)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  guint32 send_size;
  int res;

  send_size= strlen(send_buf);
  ic_assert((send_size + 1) < conn->write_buf_size);

  DEBUG_PRINT(CONFIG_PROTO_LEVEL, ("Send: %s", send_buf));
  if ((conn->write_buf_pos + send_size + 1) > conn->write_buf_size)
  {
    /* Buffer is full, we need to send now */
    res= ext_conn->conn_op.ic_write_connection(ext_conn,
                                           (const void*)conn->write_buf,
                                           conn->write_buf_pos,
                                           1);
    conn->write_buf_pos= 0;
    if (res)
      return res;
  }
  /* Copy into buffer, send at least empty line instead or when full */
  memcpy(conn->write_buf+conn->write_buf_pos, send_buf, send_size);
  conn->write_buf_pos+= send_size;
  conn->write_buf[conn->write_buf_pos]= CARRIAGE_RETURN;
  conn->write_buf_pos++;
  return 0;
}

/*
  ic_step_back_rec_with_cr
  This puts back the just read line to simplify programming interface
  such that ic_rec_with_cr can be used many times on the same line in
  cases where the protocol contains optional parts such as cluster id.
*/
void
ic_step_back_rec_with_cr(IC_CONNECTION *ext_conn, guint32 read_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->read_buf_pos-= (read_size + 1);
}

/*
  ic_rec_with_cr:
  Receive a line ended with CARRIAGE RETURN from connection
  Parameters:
    conn                IN: Connection object
    rec_buf             IN/OUT: Pointer to receive buffer
    read_size           IN: Size of previous read data
                        OUT: Size of line read
                        Neither includes CR
    size_curr_buf       OUT: Size of current buffer read
    buffer_size         IN: Total size of buffer
*/

int
ic_rec_with_cr(IC_CONNECTION *ext_conn,
               gchar **rec_buf,
               guint32 *read_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  guint32 inx, size_to_read, size_read;
  int res;
  gchar *end_line;
  guint32 buffer_size= conn->read_buf_size;
  gchar *read_buf= conn->read_buf;
  guint32 size_curr_buf= conn->size_curr_read_buf;
  guint32 read_buf_pos;

  if (size_curr_buf > 0)
  {
    read_buf_pos= conn->read_buf_pos;
    size_curr_buf-= read_buf_pos;
    memmove(read_buf, read_buf + read_buf_pos,
            size_curr_buf);
    conn->read_buf_pos= 0;
  }
  do
  {
    if (size_curr_buf > 0)
    {
      for (end_line= read_buf, inx= 0;
           inx < size_curr_buf && end_line[inx] != CARRIAGE_RETURN;
           inx++)
        ;
      if (inx != size_curr_buf)
      {
        /* Found a line to report */
        conn->size_curr_read_buf= size_curr_buf;
        conn->read_buf_pos= inx + 1; /* Take CR into account */
        DEBUG(CONFIG_PROTO_LEVEL,
          ic_debug_print_rec_buf(read_buf, inx));
        *read_size= inx;
        *rec_buf= read_buf;
        return 0;
      }
      /*
        We had no complete lines to read in the buffer received so
        far.
      */
      DEBUG_PRINT(COMM_LEVEL,
                  ("No complete lines to report yet"));
    }
    size_to_read= buffer_size - size_curr_buf;
    if (!conn->conn_op.ic_check_for_data((IC_CONNECTION*)conn))
    {
      /* No data arrived in time limit, we report this as an error */
      return IC_ERROR_RECEIVE_TIMEOUT;
    }
    if ((res= conn->conn_op.ic_read_connection((IC_CONNECTION*)conn,
                                               read_buf + size_curr_buf,
                                               size_to_read,
                                               &size_read)))
      return res;
    size_curr_buf+= size_read;
  } while (1);
  return 0;
}

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

/**
  Support function to ic_mc_rec_string and ic_mc_rec_opt_string

  @parameter mc_ptr            IN:  Memory container to allocate string objects
  @parameter prefix_str        IN:  Key string
  @parameter read_buf          IN:  The protocol buffer read
  @parameter read_size         IN:  The size of the protocol buffer
  @parameter str               OUT: The string object found in protocol
*/
static int mc_rec_string_impl(IC_MEMORY_CONTAINER *mc_ptr,
                              const gchar *prefix_str,
                              gchar *read_buf,
                              guint32 read_size,
                              IC_STRING *str)
{
  guint32 prefix_len= 0;
  gchar *str_ptr;

  if (prefix_str)
    prefix_len= strlen(prefix_str);
  if (read_size <= prefix_len)
    return IC_PROTOCOL_ERROR;
  if (prefix_len && memcmp(read_buf, prefix_str, prefix_len))
    return IC_PROTOCOL_ERROR;
  read_buf+= prefix_len;
  read_size-= prefix_len;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(str_ptr, read_buf, read_size);
  IC_INIT_STRING(str, str_ptr, read_size, TRUE);
  return 0;
}

/**
  Get a string in a protocol, return it in provided IC_STRING object

  @parameter conn              IN:  The connection from the client
  @parameter mc_ptr            IN:  Memory container to allocate string objects
  @parameter prefix_str        IN:  Key string
  @parameter str               OUT: The string object found in protocol
*/
int
ic_mc_rec_string(IC_CONNECTION *conn,
                 IC_MEMORY_CONTAINER *mc_ptr,
                 const gchar *prefix_str,
                 IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  return mc_rec_string_impl(mc_ptr,
                            prefix_str,
                            read_buf,
                            read_size,
                            str);
}

/**
  Get a string in a protocol, return it in provided IC_STRING object
  The string is optional in protocol, we'll initialize IC_STRING
  object to NULL object if the string isn't found.

  @parameter conn              IN:  The connection from the client
  @parameter mc_ptr            IN:  Memory container to allocate string objects
  @parameter head_str          IN:  Key string
  @parameter str               OUT: The string object found in protocol
*/
int
ic_mc_rec_opt_string(IC_CONNECTION *conn,
                     IC_MEMORY_CONTAINER *mc_ptr,
                     const gchar *prefix_str,
                     IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == 0)
  {
    /* Found only last empty string, step back to be able to read this again */
    ic_step_back_rec_with_cr(conn, read_size);
    IC_INIT_STRING(str, NULL, 0, FALSE);
    return 0;
  }
  return mc_rec_string_impl(mc_ptr,
                            prefix_str,
                            read_buf,
                            read_size,
                            str);
}

int
ic_rec_string(IC_CONNECTION *conn, const gchar *prefix_str, gchar *read_str)
{
  gchar *read_buf;
  guint32 read_size, remaining_len;
  int error;
  guint32 prefix_str_len= strlen(prefix_str);

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (read_size >= prefix_str_len &&
        ic_check_buf(read_buf, prefix_str_len, prefix_str,
                     prefix_str_len))
    {
      DEBUG_PRINT(CONFIG_LEVEL,
        ("Protocol error in waiting for %s", prefix_str));
      DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
    }
    remaining_len= read_size - prefix_str_len;
    memcpy(read_str, &read_buf[prefix_str_len], remaining_len);
    read_str[remaining_len]= 0;
    DEBUG_RETURN_INT(0);
  }
  DEBUG_RETURN_INT(error);
}

static int
ic_rec_simple_str_impl(IC_CONNECTION *conn,
                       const gchar *str,
                       gboolean *optional_and_found)
{
  gchar *read_buf;
  guint32 read_size;
  int error;
  DEBUG_ENTRY("ic_rec_simple_str_impl");

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (ic_check_buf(read_buf, read_size, str,
                     strlen(str)))
    {
      ic_step_back_rec_with_cr(conn, read_size);
      if (!(*optional_and_found))
      {
        DEBUG_PRINT(CONFIG_LEVEL,
          ("Protocol error in waiting for %s", str));
        DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
      }
      DEBUG_PRINT(CONFIG_LEVEL, ("Step back, didn't find %s", str));
      *optional_and_found= FALSE;
    }
    else
      *optional_and_found= TRUE;
    DEBUG_RETURN_INT(0);
  }
  *optional_and_found= FALSE;
  DEBUG_RETURN_INT(error);
}

int
ic_rec_simple_str(IC_CONNECTION *conn, const gchar *str)
{
  gboolean optional= FALSE;
  return ic_rec_simple_str_impl(conn, str, &optional);
}

int
ic_rec_simple_str_opt(IC_CONNECTION *conn,
                      const gchar *str,
                      gboolean *found)
{
  *found= TRUE;
  return ic_rec_simple_str_impl(conn, str, found);
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
    return 0;
  }
  return error;
}

/**
  Get the autorestart indicator from the protocol

  @parameter conn              IN:  The connection
  @parameter prefix_str        IN:  The prefix string defined by the protocol
  @parameter bool_value        OUT: The boolean value (true/false) read from protocol
*/
int
ic_rec_boolean(IC_CONNECTION *conn,
               const gchar *prefix_str,
               gboolean *bool_value)
{
  int error;
  guint32 read_size;
  gchar *read_buf;
  guint32 prefix_len= strlen(prefix_str);

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size <= prefix_len)
    goto protocol_error;
  if (memcmp(read_buf, prefix_str, prefix_len))
    goto protocol_error;
  read_size-= prefix_len;
  read_buf+= prefix_len;
  if (read_size == strlen(ic_true_str) &&
      !memcmp(read_buf, ic_true_str, read_size))
    *bool_value= 1;
  else if (read_size == strlen(ic_false_str) &&
           !memcmp(read_buf, ic_false_str, read_size))
    *bool_value= 0;
  else
    goto protocol_error;
  return 0;
protocol_error:
  return IC_PROTOCOL_ERROR;
}

/**
  Get a numeric variable from the protocol with prefix_str as word describing
  the parameter.

  @parameter conn              IN:  The connection
  @parameter prefix_str        IN:  The prefix string defined by the protocol
  @parameter number            OUT: Integer sent in protocol

  @note
    This function is used where the protocol accepts nothing bigger than
    2^64 - 1, thus a 64-bit unsigned variable.
*/
int
ic_rec_long_number(IC_CONNECTION *conn,
                   const gchar *prefix_str,
                   guint64 *number)
{
  gchar *read_buf;
  guint32 read_size;
  int error;
  guint64 local_id;

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_int(read_buf,
                               read_size,
                               prefix_str,
                               strlen(prefix_str),
                               &local_id))
    {
      *number= local_id;
      return 0;
    }
    else
      return IC_PROTOCOL_ERROR;
  }
  return error;
}

/**
  Get a numeric variable from the protocol with prefix_str as word describing
  the parameter.

  @parameter conn              IN:  The connection
  @parameter prefix_str        IN:  The prefix string defined by the protocol
  @parameter number            OUT: Integer sent in protocol

  @note
    This function is used where the protocol accepts nothing bigger than
    2^32 - 1, thus a 32-bit unsigned variable.
*/
int
ic_rec_number(IC_CONNECTION *conn, const gchar *prefix_str, guint32 *number)
{
  return ic_rec_number_impl(conn, prefix_str, number, FALSE);
}

/**
  Get a numeric variable from the protocol with prefix_str as word describing
  the parameter.
  
  The protocol defines this parameter as optional. If this is the case the
  variable will not be changed. This means that the user of this function
  should assign it a default value before calling this function.

  @parameter conn              IN:  The connection
  @parameter prefix_str        IN:  The prefix string defined by the protocol
  @parameter number            OUT: Integer sent in protocol

  @note
    This function is used where the protocol accepts nothing bigger than
    2^32 - 1, thus a 32-bit unsigned variable.
*/
int
ic_rec_opt_number(IC_CONNECTION *conn, const gchar *str, guint32 *number)
{
  return ic_rec_number_impl(conn, str, number, TRUE);
}

/**
  Get a numeric variable from the protocol with prefix_str as word describing
  the parameter.

  @parameter conn              IN:  The connection
  @parameter prefix_str        IN:  The prefix string defined by the protocol
  @parameter number            OUT: Integer sent in protocol

  @note
    This function is used where the protocol accepts nothing bigger than
    2^32 - 1, thus a 32-bit unsigned variable.
*/
int
ic_rec_int_number(IC_CONNECTION *conn,
                  const gchar *str,
                  int *number)
{
  gchar *read_buf;
  guint32 read_size;
  int error;
  gboolean sign_flag;
  guint64 local_number;

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_signed_int(read_buf, read_size, str,
                                      strlen(str),
                                      &local_number, &sign_flag))
    {
      if (local_number >= IC_MAX_UINT32)
        return IC_PROTOCOL_ERROR;
      *number= (guint32)local_number;
      if (sign_flag)
        *number= -(*number);
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
ic_send_empty_line(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  int error;

  if ((error= ic_send_with_cr(ext_conn, "")))
    return error;
  ic_assert(conn->write_buf_pos);
  /* We need to flush buffer */
  error= ext_conn->conn_op.ic_write_connection(ext_conn,
                                           (const void*)conn->write_buf,
                                           conn->write_buf_pos,
                                           1);
  conn->write_buf_pos= 0;
  return error;
}

int
ic_send_with_cr_composed(IC_CONNECTION *conn,
                         const gchar **buf,
                         guint32 num_strings)
{
  gchar local_buf[256];
  guint32 local_pos= 0;
  guint32 i, len;

  for (i= 0; i < num_strings; i++)
  {
    len= strlen(buf[i]);
    memcpy(local_buf + local_pos, buf[i], len);
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
