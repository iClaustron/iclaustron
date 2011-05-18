/* Copyright (C) 2011 iClaustron AB

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
#include <ic_port.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_protocol_support.h>
#include <ic_proto_str.h>

int
ic_receive_error_message(IC_CONNECTION *conn, gchar *err_msg)
{
  gchar *read_buf;
  guint32 read_size;
  int ret_code;
  DEBUG_ENTRY("ic_receive_error_message");

  if ((ret_code= ic_rec_simple_str(conn, ic_error_str)) ||
      (ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (memcpy(err_msg, read_buf, read_size), FALSE) ||
      (ret_code= ic_rec_empty_line(conn)))
    DEBUG_RETURN_INT(ret_code);
  err_msg[read_size]= (gchar)0;
  DEBUG_RETURN_INT(0);
}

/**
  Send a protocol line containing:
  list stop<CR><CR>

  @parameter conn              IN:  The connection from the client
*/
int
ic_send_list_stop(IC_CONNECTION *conn)
{
  int error;
  DEBUG_ENTRY("ic_send_list_stop");

  if ((error= ic_send_with_cr(conn, ic_list_stop_str)) ||
      (error= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(error);
  DEBUG_RETURN_INT(0);
}

/**
  Send a protocol line containing:
  list next<CR><CR>

  @parameter conn              IN:  The connection from the client
*/
int
ic_send_list_next(IC_CONNECTION *conn)
{
  int error;
  DEBUG_ENTRY("ic_send_list_next");

  if ((error= ic_send_with_cr(conn, ic_list_next_str)) ||
      (error= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(error);
  DEBUG_RETURN_INT(0);
}

/**
  Receive list stop followed by end (empty line).

  @parameter conn              IN:  The connection from the client
*/
int
ic_rec_list_stop(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("ic_rec_list_stop");

  if ((ret_code= ic_rec_simple_str(conn, ic_list_stop_str)) ||
      (ret_code= ic_rec_empty_line(conn)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Handle stop of list by either sending list next followed by
  receiving list stop or by sending list stop and also here
  receiving list stop.

  @parameter conn          IN: The connection to the process controller
  @parameter stop_flag     Flag indicating whether to use list stop or
                           list next.
*/
int
ic_handle_list_stop(IC_CONNECTION *conn, gboolean stop_flag)
{
  int ret_code;
  DEBUG_ENTRY("ic_handle_list_stop");

  if (stop_flag)
  {
    if ((ret_code= ic_send_list_stop(conn)))
      DEBUG_RETURN_INT(ret_code);
  }
  else
  {
    if ((ret_code= ic_send_list_next(conn)))
      DEBUG_RETURN_INT(ret_code);
  }
  DEBUG_RETURN_INT(ic_rec_list_stop(conn));
}

/**
  Send a command to list my node

  @parameter conn              IN:  The connection from the client
  @parameter grid              IN:  The grid to list, NULL lists all
  @parameter cluster           IN:  The cluster to list, NULL lists all
  @parameter node              IN:  The node to list, NULL lists all
  @parameter full_flag         IN: Flag to indicate list full or not
*/
int
ic_send_list_node(IC_CONNECTION *conn,
                  const gchar *grid,
                  const gchar *cluster,
                  const gchar *node,
                  gboolean full_flag)
{
  int ret_code;
  DEBUG_ENTRY("ic_send_list_node");

  if ((ret_code= ic_send_with_cr(conn,
                                 full_flag ? ic_list_full_str :
                                             ic_list_str)))
    goto error;
  if (!grid)
    goto end;
  if ((ret_code= ic_send_with_cr_two_strings(conn, ic_grid_str, grid)))
    goto error;
  if (!cluster)
    goto end;
  if ((ret_code= ic_send_with_cr_two_strings(conn, ic_cluster_str, cluster)))
    goto error;
  if (!node)
    goto end;
  if ((ret_code= ic_send_with_cr_two_strings(conn, ic_node_str, node)))
    goto error;
end:
  if ((ret_code= ic_send_empty_line(conn)))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  DEBUG_RETURN_INT(ret_code);
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
  int error;
  DEBUG_ENTRY("ic_send_ok");

  if ((error= ic_send_with_cr(conn, ic_ok_str)) ||
      (error= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(error);
  DEBUG_RETURN_INT(0);
}

/**
  Send a protocol line with the message:
  Ok<CR>pid: <pid_number><CR><CR>

  @parameter conn              IN:  The connection from the client
  @parameter pid               IN:  The pid of the process

  @note
    This message is sent as a positive response to the start message
*/
int
ic_send_ok_pid(IC_CONNECTION *conn, IC_PID_TYPE pid)
{
  int error;
  DEBUG_ENTRY("ic_send_ok_pid");

  if ((error= ic_send_with_cr(conn, ic_ok_str)) ||
      (error= ic_send_with_cr_with_num(conn, ic_pid_str, pid)) ||
      (error= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(error);
  DEBUG_RETURN_INT(0);
}

/**
  Send a protocol line with the message:
  Ok<CR>pid: <pid_number><CR>Process already started<CR><CR>

  @parameter conn              IN:  The connection from the client
  @parameter pid               IN:  The pid of the process

  @note
    This message is sent as a positive response to the start message
*/
int
ic_send_ok_pid_started(IC_CONNECTION *conn, IC_PID_TYPE pid)
{
  int error;
  DEBUG_ENTRY("ic_send_ok_pid_started");

  if ((error= ic_send_with_cr(conn, ic_ok_str)) ||
      (error= ic_send_with_cr_with_num(conn, ic_pid_str, pid)) ||
      (error= ic_send_with_cr(conn, ic_process_already_started_str)) ||
      (error= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(error);
  DEBUG_RETURN_INT(0);
}

