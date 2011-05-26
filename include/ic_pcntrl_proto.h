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

int ic_receive_error_message(IC_CONNECTION *conn, gchar *err_msg);
int ic_rec_list_stop(IC_CONNECTION *conn);
int ic_handle_list_stop(IC_CONNECTION *conn, gboolean stop_flag);
int ic_send_list_node(IC_CONNECTION *conn,
                      const gchar *grid,
                      const gchar *cluster,
                      const gchar *node,
                      gboolean full_flag);
int ic_send_list_stop(IC_CONNECTION *conn);
int ic_send_list_next(IC_CONNECTION *conn);
int ic_send_ok(IC_CONNECTION *conn);
int ic_send_ok_pid(IC_CONNECTION *conn, IC_PID_TYPE pid);
int ic_send_ok_pid_started(IC_CONNECTION *conn, IC_PID_TYPE pid);
int ic_send_mem_info_req(IC_CONNECTION *conn);
int ic_send_disk_info_req(IC_CONNECTION *conn, gchar *dir_name);
int ic_send_cpu_info_req(IC_CONNECTION *conn);
int ic_send_stop_node(IC_CONNECTION *conn,
                      const gchar *grid_str,
                      const gchar *cluster_str,
                      const gchar *node_str);
