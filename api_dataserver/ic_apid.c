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

#include <ic_apid.h>

static int
is_ds_conn_established(IC_DS_CONNECTION *ds_conn,
                       gboolean *is_connected)
{
  IC_CONNECTION *conn= ds_conn->conn_obj;

  *is_connected= FALSE;
  if (conn->conn_op.ic_is_conn_thread_active(conn))
    return 0;
  *is_connected= TRUE;
  if (conn->conn_op.ic_is_conn_connected(conn))
    return 0;
  return conn->error_code;
}

static int
authenticate_ds_connection(void *conn_obj)
{
  gchar *read_buf;
  guint32 read_size;
  gchar expected_buf[64];
  int error;
  IC_DS_CONNECTION *ds_conn= conn_obj;
  IC_CONNECTION *conn= ds_conn->conn_obj;

  ic_send_with_cr(conn, "ndbd");
  ic_send_with_cr(conn, "ndbd passwd");
  conn->conn_op.ic_flush_connection(conn);
  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (!strcmp(read_buf, "ok"))
    return AUTHENTICATE_ERROR;
  ic_send_with_cr(conn, read_buf);
  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (!strcmp(read_buf, expected_buf))
    return AUTHENTICATE_ERROR;
  return 0;
}

static int
open_ds_connection(IC_DS_CONNECTION *ds_conn)
{
  int error;
  IC_CONNECTION *conn= ds_conn->conn_obj;
  if (!(conn= ic_create_socket_object(TRUE, TRUE, TRUE, TRUE,
                                      CONFIG_READ_BUF_SIZE,
                                      authenticate_ds_connection,
                                      (void*)ds_conn)))
    return IC_ERROR_MEM_ALLOC;
  ds_conn->conn_obj= conn;
  if ((error= conn->conn_op.ic_set_up_connection(conn)))
    return error;
  return 0;
}

static int
close_ds_connection(__attribute__ ((unused)) IC_DS_CONNECTION *ds_conn)
{
  return 0;
}

void
ic_init_ds_connection(IC_DS_CONNECTION *ds_conn)
{
  ds_conn->operations.ic_set_up_ds_connection= open_ds_connection;
  ds_conn->operations.ic_close_ds_connection= close_ds_connection;
  ds_conn->operations.ic_is_conn_established= is_ds_conn_established;
  ds_conn->operations.ic_authenticate_connection= authenticate_ds_connection;
}
