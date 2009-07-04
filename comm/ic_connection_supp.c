/* Copyright (C) 2007-2009 iClaustron AB

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
#include "ic_connection_int.h"

void
ic_print_buf(char *buf, guint32 size)
{
  char p_buf[2049];
  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  printf("Receive buffer, size %u:\n%s\n", size, p_buf);
}

int
ic_send_with_cr(IC_CONNECTION *conn, const gchar *send_buf)
{
  guint32 inx;
  int res;
  char buf[256];

  strcpy(buf, send_buf);
  inx= strlen(buf);
  buf[inx++]= CARRIAGE_RETURN;
  buf[inx]= NULL_BYTE;
  DEBUG_PRINT(COMM_LEVEL, ("Send: %s", buf));
  res= conn->conn_op.ic_write_connection(conn, (const void*)buf, inx, 1);
  return res;
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
  DEBUG_ENTRY("ic_rec_with_cr");

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
        DEBUG(COMM_LEVEL, ic_debug_print_rec_buf(read_buf, inx));
        *read_size= inx;
        *rec_buf= read_buf;
        DEBUG_RETURN(0);
      }
      /*
        We had no complete lines to read in the buffer received so
        far.
      */
      DEBUG_PRINT(COMM_LEVEL, ("No complete lines to report yet"));
    }
    size_to_read= buffer_size - size_curr_buf;
    if (!conn->conn_op.ic_check_for_data((IC_CONNECTION*)conn, 10000))
    {
      /* No data arrived in 10 seconds, we report this as an error */
      return IC_ERROR_RECEIVE_TIMEOUT;
    }
    if ((res= conn->conn_op.ic_read_connection((IC_CONNECTION*)conn,
                                               read_buf + size_curr_buf,
                                               size_to_read,
                                               &size_read)))
      DEBUG_RETURN(res);
    size_curr_buf+= size_read;
  } while (1);
  DEBUG_RETURN(0);
}

