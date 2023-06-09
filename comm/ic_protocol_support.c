/* Copyright (C) 2007, 2014 iClaustron AB

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

/**
  Print a buffer with specified size as 1 line with CR at end.

  @parameter buf             The string buffer
  @parameter size            The number of characters to print
*/
void
ic_print_buf(char *buf, guint32 size)
{
  char p_buf[2049];

  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  ic_printf("Receive buffer, size %u:\n%s", size, p_buf);
}

/**
  Check if the read buffer is equal to the expected string to receive

  @parameter read_buf                 IN: The buffer read
  @parameter read_size                IN: The buffer size
  @parameter str                      IN: The expected string
  @parameter str_len                  IN: The expected string size

  @retval  returns TRUE if not equal, FALSE if equal
*/
int
ic_check_buf(gchar *read_buf, guint32 read_size, const gchar *str, int str_len)
{
  DEBUG_ENTRY("ic_check_buf");
  if ((read_size != (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0))
  {
    DEBUG_RETURN_INT(TRUE);
  }
  DEBUG_RETURN_INT(FALSE);
}

/**
  Check read buffer for a protocol line that consists of the string provided
  by the str, str_len pair followed by an list of 64-bit unsigned integer
  numbers that are SPACE-separated.

  @parameter read_buf         IN:  Buffer containing one line read
  @parameter read_size        IN:  Size of buffer read
  @parameter str              IN:  Initial string expected in read buffer
  @parameter str_len          IN:  Length of expected string
  @parameter num_elements     IN:  Number of elements expected
  @parameter number           OUT: The array of 64-bit unsigned integers read
*/
int
ic_check_buf_with_many_int(gchar *read_buf,
                           guint32 read_size,
                           const gchar *str,
                           int str_len,
                           guint32 num_elements,
                           guint64 *number)
{
  gchar *ptr, *end_ptr;
  guint32 i, num_chars;

  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) == 0))
  {
    ptr= read_buf+str_len;
    if (*ptr == SPACE_CHAR)
    {
      ptr++;
    }
    end_ptr= read_buf + read_size;
    for (i= 0; i < num_elements; i++)
    {
      num_chars= ic_count_characters(ptr, end_ptr - ptr);
      if (ptr + num_chars >= end_ptr)
        goto error;
      if (ic_convert_str_to_int_fixed_size(ptr, num_chars, &number[i]))
        goto error;
      ptr+= num_chars;
      if (*ptr == SPACE_CHAR)
      {
        ptr++;
      }
      else if (*ptr != '\n' || i != (num_elements - 1))
        goto error;
    }
  }
  return FALSE;
error:
  return TRUE;
}

/**
  Check read buffer for a protocol line that consists of the string provided
  by the str, str_len pair followed by one 64-bit signed integer.

  @parameter read_buf         IN:  Buffer containing one line read
  @parameter read_size        IN:  Size of buffer read
  @parameter str              IN:  Initial string expected in read buffer
  @parameter str_len          IN:  Length of expected string
  @parameter number           OUT: Number read (unsigned)
  @parameter sign_flag        OUT: Set if number is negative
*/
int
ic_check_buf_with_signed_int(gchar *read_buf, guint32 read_size,
                             const gchar *str,
                             int str_len,
                             guint64 *number,
                             int *sign_flag)
{
  *sign_flag= FALSE;
  if ((read_size < (guint32)str_len + 1) ||
      (memcmp(read_buf, str, str_len) != 0))
  {
    return TRUE;
  }
  if (read_buf[str_len] == SPACE_CHAR)
  {
    str_len++;
  }
  if (((str_len= (read_buf[str_len] == '-') ? str_len+1 : str_len), FALSE) ||
       (ic_convert_str_to_int_fixed_size(read_buf+str_len,
                                         read_size - str_len,
                                         number)))
  {
    return TRUE;
  }
  if (read_buf[str_len] == '-')
  {
    *sign_flag= TRUE;
  }
  return FALSE;
}

/**
  Check read buffer for a protocol line that consists of the string provided
  by the str, str_len pair followed by one 64-bit unsigned integer.

  @parameter read_buf         IN:  Buffer containing one line read
  @parameter read_size        IN:  Size of buffer read
  @parameter str              IN:  Initial string expected in read buffer
  @parameter str_len          IN:  Length of expected string
  @parameter number           OUT: Number read (unsigned)
*/
int
ic_check_buf_with_int(gchar *read_buf,
                      guint32 read_size,
                      const gchar *str,
                      int str_len,
                      guint64 *number)
{
  if ((read_size < (guint32)str_len + 1) ||
      (memcmp(read_buf, str, str_len) != 0))
  {
    return TRUE;
  }
  if (read_buf[str_len] == SPACE_CHAR)
  {
    str_len++;
  }
  if ((ic_convert_str_to_int_fixed_size(read_buf+str_len,
                                        read_size - str_len,
                                        number)))
  {
    return TRUE;
  }
  return FALSE;
}

/**
  Check read buffer for a protocol line that consists of the string provided
  by the str, str_len pair followed by one string returned in string object.

  @parameter read_buf         IN:  Buffer containing one line read
  @parameter read_size        IN:  Size of buffer read
  @parameter str              IN:  Initial string expected in read buffer
  @parameter str_len          IN:  Length of expected string
  @parameter string           OUT: The string read
*/
int
ic_check_buf_with_string(gchar *read_buf,
                         guint32 read_size,
                         const gchar *str,
                         int str_len,
                         IC_STRING *string)
{
  if ((read_size < (guint32)str_len + 1) ||
      (memcmp(read_buf, str, str_len) != 0))
  {
    return TRUE;
  }
  if (read_buf[str_len] == SPACE_CHAR)
  {
    str_len++;
  }
  string->len= read_size - str_len;
  string->str= read_buf+str_len;
  string->is_null_terminated= FALSE;
  return FALSE;
}
/*
  ic_step_back_rec_with_cr
  This puts back the just read line to simplify programming interface
  such that ic_rec_with_cr can be used many times on the same line in
  cases where the protocol contains optional parts such as cluster id.

  @parameter ext_conn           IN: The connection
  @parameter read_size          IN: The size to step back in the receive
                                    buffer This size doesn't include the
                                    <CR> which we will also step back.
*/
void
ic_step_back_rec_with_cr(IC_CONNECTION *ext_conn, guint32 read_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->read_buf_pos-= (read_size + 1);
}

/**
  Receive a line ended with CARRIAGE RETURN from connection
  This function is heavily used by protocol implementations.

  @parameter conn       IN: Connection object
  @parameter rec_buf    IN/OUT: Pointer to receive buffer
  @parameter read_size  IN: Size of previous read data
                        OUT: Size of line read
                        Neither includes CR
*/
int
ic_rec_with_cr(IC_CONNECTION *ext_conn,
               gchar **rec_buf,
               guint32 *read_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  guint32 inx, size_to_read, size_read;
  int ret_code;
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
      {
        ;
      }
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
    if ((ret_code= conn->conn_op.ic_read_connection((IC_CONNECTION*)conn,
                                                    read_buf + size_curr_buf,
                                                    size_to_read,
                                                    &size_read)))
    {
      return ret_code;
    }
    size_curr_buf+= size_read;
  } while (1);
  return 0;
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
  guint32 prefix_len;
  gchar *str_ptr;

  prefix_len= strlen(prefix_str);
  ic_require(prefix_len);
  if (read_size < (prefix_len + 1))
  {
    return IC_PROTOCOL_ERROR;
  }
  if (memcmp(read_buf, prefix_str, prefix_len))
  {
    return IC_PROTOCOL_ERROR;
  }
  if (read_buf[prefix_len] == SPACE_CHAR)
  {
    prefix_len++;
  }
  read_buf+= prefix_len;
  read_size-= prefix_len;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
  {
    return IC_ERROR_MEM_ALLOC;
  }
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
  int ret_code;
  guint32 read_size;
  gchar *read_buf;
  DEBUG_ENTRY("ic_mc_rec_string");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s string", prefix_str));

  if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
    goto end;
  ret_code= mc_rec_string_impl(mc_ptr,
                               prefix_str,
                               read_buf,
                               read_size,
                               str);
end:
  DEBUG_RETURN_INT(ret_code);
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
  int ret_code;
  guint32 read_size;
  gchar *read_buf;
  DEBUG_ENTRY("ic_mc_rec_opt_string");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s string", prefix_str));

  if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
    goto end;
  if (read_size == 0)
  {
    /* Found only last empty string, step back to be able to read this again */
    ic_step_back_rec_with_cr(conn, read_size);
    IC_INIT_STRING(str, NULL, 0, FALSE);
    goto end;
  }
  ret_code= mc_rec_string_impl(mc_ptr,
                               prefix_str,
                               read_buf,
                               read_size,
                               str);
end:
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive a line with a predefined string followed by any string
  as part of a protocol line. The strings are always separated by a comma.

  @parameter conn          IN:  The connection
  @parameter prefix_str    IN:  The predefined string (prescribed by protocol)
  @parameter read_str      OUT: The string read

  @note
    The read_str must come with a buffer of at least size COMMAND_READ_BUF_SIZE
*/
int
ic_rec_string(IC_CONNECTION *conn, const gchar *prefix_str, gchar *read_str)
{
  gchar *read_buf;
  guint32 read_size, remaining_len;
  int ret_code;
  guint32 prefix_str_len;
  DEBUG_ENTRY("ic_rec_string");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s string", prefix_str));

  prefix_str_len= strlen(prefix_str);
  if (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (read_size < (prefix_str_len + 1))
      goto error;
    if (ic_check_buf(read_buf,
                     prefix_str_len,
                     prefix_str,
                     prefix_str_len))
      goto error;
    if (read_buf[prefix_str_len] == SPACE_CHAR)
    {
      prefix_str_len++;
    }
    remaining_len= read_size - prefix_str_len;
    memcpy(read_str, &read_buf[prefix_str_len], remaining_len);
    read_str[remaining_len]= 0;
    DEBUG_RETURN_INT(0);
  }
  DEBUG_RETURN_INT(ret_code);
error:
  /* Length must be prefix length + possible space char + at least 1 character */
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Protocol error in waiting for %s", prefix_str));
  DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
}

/**
  Receive two comma separated strings

  @parameter conn                IN:    The connection
  @parameter first_str           IN:    The first string
  @parameter second_str          IN:    The second string
*/
int
ic_rec_two_strings(IC_CONNECTION *conn,
                   const gchar *first_str,
                   const gchar *second_str)
{
  int ret_code;
  gchar buf[CONFIG_READ_BUF_SIZE];
  size_t second_str_len= strlen(second_str);
  DEBUG_ENTRY("ic_rec_two_strings");
  DEBUG_PRINT(COMM_LEVEL, ("Search for %s %s", first_str, second_str));

  if ((ret_code= ic_rec_string(conn, first_str, buf)))
  {
    DEBUG_RETURN_INT(ret_code);
  }
  if ((memcmp(second_str, buf, second_str_len) != 0))
  {
    DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
  }
  DEBUG_RETURN_INT(0);
}

/**
  Receive a simple protocol line without variable parts. It is however
  optional. The parameter optional_and_found is used both as input and
  output parameter. If it is set then it is ok that the line isn't the
  prescribed string, if it isn't set we will return an error if the
  string isn't found. The optional_and_found is also used to flag back
  whether we found the prescribed string.

  @parameter conn                IN:    The connection
  @parameter str                 IN:    The predefined string
  @parameter optional_and_found IN/OUT: If set as input the string is optional,
                                        If set in output the string was found.

  @note
    This function is a local support function
*/
static int
ic_rec_simple_str_impl(IC_CONNECTION *conn,
                       const gchar *str,
                       gboolean *optional_and_found)
{
  gchar *read_buf;
  guint32 read_size;
  int ret_code;
  DEBUG_ENTRY("ic_rec_simple_str_impl");
  DEBUG_PRINT(COMM_LEVEL, ("Search for string %s", str));

  if (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
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
    {
      *optional_and_found= TRUE;
    }
    DEBUG_RETURN_INT(0);
  }
  *optional_and_found= FALSE;
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive a simple string as one line in the protocol. Implemented using the
  ic_rec_simple_str_impl function above.

  @parameter conn            IN: The connection
  @parameter str             IN: The expected predefined string
*/
int
ic_rec_simple_str(IC_CONNECTION *conn, const gchar *str)
{
  gboolean optional= FALSE;
  int ret_code;
  DEBUG_ENTRY("ic_rec_simple_str");

  ret_code= ic_rec_simple_str_impl(conn, str, &optional);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive a simple string as one line in the protocol. Implemented using the
  ic_rec_simple_str_impl function above. In this function the protocol line is
  optional.

  @parameter conn            IN:  The connection
  @parameter str             IN:  The expected predefined string
  @parameter found           OUT: Is the protocol line found or not
*/
int
ic_rec_simple_str_opt(IC_CONNECTION *conn,
                      const gchar *str,
                      gboolean *found)
{
  int ret_code;
  DEBUG_ENTRY("ic_rec_simple_str_opt");

  *found= TRUE;
  ret_code= ic_rec_simple_str_impl(conn, str, found);
  DEBUG_RETURN_INT(ret_code);
}

/*
  Receive a protocol string like "node1: 12"
  Here str contains "node1: " and 12 is returned in id.
  If optional is set the line is optional and ok will
  be returned even if there is no such line.

  @parameter conn                IN:  The connection
  @parameter str                 IN:  The predefined string
  @parameter id                  OUT: The read number
  @parameter optional            IN:  Is the protocol line optional or not
*/
static int
ic_rec_number_impl(IC_CONNECTION *conn,
                   const gchar *str,
                   guint32 *id,
                   gboolean optional)
{
  gchar *read_buf;
  guint32 read_size;
  int ret_code;
  guint64 local_id;
  DEBUG_ENTRY("ic_rec_number_impl");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s number", str));

  if (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_int(read_buf, read_size, str,
                               strlen(str),
                               &local_id))
    {
      if (local_id >= IC_MAX_UINT32)
      {
        DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
      }
      *id= (guint32)local_id;
      goto end;
    }
    if (!optional)
    {
      DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
    }
    ic_step_back_rec_with_cr(conn, read_size);
  }
end:
  DEBUG_RETURN_INT(ret_code);
}

/**
  Get the autorestart indicator from the protocol

  @parameter conn              IN:  The connection
  @parameter prefix_str        IN:  The prefix string defined by the protocol
  @parameter bool_value        OUT: The boolean value (true/false) read from
                                    protocol
*/
int
ic_rec_boolean(IC_CONNECTION *conn,
               const gchar *prefix_str,
               gboolean *bool_value)
{
  int ret_code;
  guint32 read_size;
  gchar *read_buf;
  guint32 prefix_len= strlen(prefix_str);
  DEBUG_ENTRY("ic_rec_boolean");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s boolean", prefix_str));

  if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
    goto end;
  if (read_size < (prefix_len + 1))
    goto protocol_error;
  if (memcmp(read_buf, prefix_str, prefix_len))
    goto protocol_error;
  if (read_buf[prefix_len] == SPACE_CHAR)
  {
    prefix_len++;
  }
  read_size-= prefix_len;
  read_buf+= prefix_len;
  if (read_size == strlen(ic_true_str) &&
      !memcmp(read_buf, ic_true_str, read_size))
  {
    *bool_value= 1;
  }
  else if (read_size == strlen(ic_false_str) &&
           !memcmp(read_buf, ic_false_str, read_size))
  {
    *bool_value= 0;
  }
  else
    goto protocol_error;
end:
  DEBUG_RETURN_INT(ret_code);
protocol_error:
  DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
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
  int ret_code;
  guint64 local_id;
  DEBUG_ENTRY("ic_rec_long_number");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s long number", prefix_str));

  if (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_int(read_buf,
                               read_size,
                               prefix_str,
                               strlen(prefix_str),
                               &local_id))
    {
      *number= local_id;
    }
    else
    {
      ret_code= IC_PROTOCOL_ERROR;
    }
  }
  DEBUG_RETURN_INT(ret_code);
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
  int ret_code;
  DEBUG_ENTRY("ic_rec_number");

  ret_code= ic_rec_number_impl(conn, prefix_str, number, FALSE);
  DEBUG_RETURN_INT(ret_code);
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
  int ret_code;
  DEBUG_ENTRY("ic_rec_opt_number");

  ret_code= ic_rec_number_impl(conn, str, number, TRUE);
  DEBUG_RETURN_INT(ret_code);
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
  int ret_code;
  gboolean sign_flag;
  guint64 local_number;
  DEBUG_ENTRY("ic_rec_int_number");
  DEBUG_PRINT(COMM_LEVEL, ("Search for: %s int number", str));

  if (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf_with_signed_int(read_buf, read_size, str,
                                      strlen(str),
                                      &local_number, &sign_flag))
    {
      if (local_number >= IC_MAX_UINT32)
      {
        DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
      }
      *number= (guint32)local_number;
      if (sign_flag)
      {
        *number= -(*number);
      }
    }
    else
    {
      ret_code= IC_PROTOCOL_ERROR;
    }
  }
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive an empty line as a protocol action

  @parameter conn             The connection
*/
int
ic_rec_empty_line(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("ic_rec_empty_line");

  ret_code= ic_rec_simple_str(conn, ic_empty_string);
  DEBUG_RETURN_INT(ret_code);
}

/**
  This method is used very commonly by loads of functions that implement
  the iClaustron management protocols. It sends the string buffer provided
  plus a final <CR>. It uses buffering, so it doesn't necessarily send
  this message until later when buffer is full or when an empty line is
  sent.

  @parameter ext_conn              The connection
  @parameter send_buf              A null terminated string to send
*/
int
ic_send_with_cr(IC_CONNECTION *ext_conn, const gchar *send_buf)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  guint32 send_size;
  int ret_code;

  send_size= strlen(send_buf);
  ic_assert((send_size + 1) < conn->write_buf_size);

  DEBUG_PRINT(CONFIG_PROTO_LEVEL, ("Send: %s", send_buf));
  if ((conn->write_buf_pos + send_size + 1) > conn->write_buf_size)
  {
    /* Buffer is full, we need to send now */
    ret_code= ext_conn->conn_op.ic_write_connection(ext_conn,
                                           (const void*)conn->write_buf,
                                           conn->write_buf_pos,
                                           1);
    conn->write_buf_pos= 0;
    if (ret_code)
    {
      return ret_code;
    }
  }
  /* Copy into buffer, send at least empty line instead or when full */
  memcpy(conn->write_buf+conn->write_buf_pos, send_buf, send_size);
  conn->write_buf_pos+= send_size;
  conn->write_buf[conn->write_buf_pos]= CARRIAGE_RETURN;
  conn->write_buf_pos++;
  return 0;
}

/**
  Send a protocol line like "string: number" where string is contained in
  in_buf and the number is contained in number.

  @parameter conn               The connection
  @parameter in_buf             The input string
  @parameter number             The input number
*/
int
ic_send_with_cr_with_number(IC_CONNECTION *conn,
                            const gchar *in_buf,
                            guint64 number)
{
  gchar out_buf[128], buf[64];

  g_snprintf(out_buf, 128, "%s %s", in_buf,
             ic_guint64_str(number, buf, NULL));
  return ic_send_with_cr(conn, out_buf);
}

/**
  Send a protocol line like "string: str1 str2" where num_strings specifies
  the number of strings and buf is an array of those strings.

  @parameter conn              The connection
  @parameter buf               An array of input strings
  @parameter num_strings       Number of strings in array
*/
int
ic_send_with_cr_composed(IC_CONNECTION *conn,
                         const gchar **buf,
                         guint32 num_strings)
{
  gchar local_buf[COMMAND_READ_BUF_SIZE];
  guint32 local_pos= 0;
  guint32 i, len;

  for (i= 0; i < num_strings; i++)
  {
    len= strlen(buf[i]);
    memcpy(local_buf + local_pos, buf[i], len);
    local_buf[local_pos + len]= SPACE_CHAR;
    local_pos+= (len + 1);
  }
  local_buf[local_pos - 1]= 0;
  return ic_send_with_cr(conn, local_buf);
}

/**
  Send a protocol line with 2 input strings that are space separated.

  @parameter conn               The connection
  @parameter buf1               The first input string
  @parameter buf2               The second input string
*/
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

/**
  Send an empty line as part of the protocol, always the last protocol
  action that gives the other side the chance to send a number of lines.

  @conn                        The connection
*/
int
ic_send_empty_line(IC_CONNECTION *ext_conn)
{
  int ret_code;

  if ((ret_code= ic_send_with_cr(ext_conn, ic_empty_string)))
  {
    return ret_code;
  }
  return ext_conn->conn_op.ic_flush_connection(ext_conn);
}

/**
  Send a protocol line with the message:
  Ok<CR><CR>

  @parameter conn              IN:  The connection from the client

  @note
    This message is sent as a positive response to the stop message
*/
int
ic_send_ok(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("ic_send_ok");

  if ((ret_code= ic_send_with_cr(conn, ic_ok_str)) ||
      (ret_code= ic_send_empty_line(conn)))
  {
    DEBUG_RETURN_INT(ret_code);
  }
  DEBUG_RETURN_INT(0);
}

/**
  Send a protocol line with the message:
  error<CR>
  <error message><CR><CR>

  @parameter conn              IN:  The connection from the client
  @parameter error_message     IN:  The error message to send

  @note
    This message is sent as a negative response to messages to the
    process controller.
*/
int
ic_send_error_message(IC_CONNECTION *conn, const gchar *error_message)
{
  int ret_code;
  DEBUG_ENTRY("ic_send_error_message");

  if ((ret_code= ic_send_with_cr(conn, ic_error_str)) ||
      (ret_code= ic_send_with_cr(conn, error_message)) ||
      (ret_code= ic_send_empty_line(conn)))
  {
    DEBUG_RETURN_INT(ret_code);
  }
  DEBUG_RETURN_INT(0);
}

/**
  Receive an error message
  error<CR>
  <error message><CR><CR>

  @parameter conn              IN:  The connection from the client
  @parameter buf               IN/OUT: The buffer to receive error message

  @note
    The buffer size must be of at least size COMMAND_READ_BUF_SIZE
*/
int
ic_rec_error_message(IC_CONNECTION *conn, gchar *buf)
{
  int ret_code;
  guint32 buf_size;
  DEBUG_ENTRY("ic_rec_error_message");

  if ((ret_code= ic_rec_simple_str(conn, ic_error_str)) ||
      (ret_code= ic_rec_with_cr(conn, &buf, &buf_size)) ||
      (ret_code= ic_rec_empty_line(conn)))
  {
    DEBUG_RETURN_INT(ret_code);
  }
  DEBUG_RETURN_INT(0);
}

/**
  Receive an ok message or an error message
  ok<CR><CR>
  or
  error<CR>
  <error message><CR><CR>

  @parameter conn              IN:  The connection from the client
  @parameter buf               IN/OUT: The buffer to receive error message
  @parameter ok_found          OUT: Indicator if ok was found

  @note
    The buffer size must be of at least size COMMAND_READ_BUF_SIZE
*/
int
ic_rec_ok_or_error(IC_CONNECTION *conn, gchar *buf, gboolean *ok_found)
{
  int ret_code;
  DEBUG_ENTRY("ic_rec_ok_or_error");

  buf[0]= 0;
  *ok_found= FALSE;
  if ((ret_code= ic_rec_simple_str_opt(conn, ic_ok_str, ok_found)))
    goto end;
  if (!ok_found)
  {
    ret_code= ic_rec_error_message(conn, buf);
  }
  else
  {
    ret_code= ic_rec_empty_line(conn);
  }
end:
  DEBUG_RETURN_INT(ret_code);
}
