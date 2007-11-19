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

#include <ic_common.h>

static gchar *glob_cluster_server_ip= "127.0.0.1";
static gchar *glob_cluster_server_port= "10006";
static gchar *glob_cluster_mgr_ip= "127.0.0.1";
static gchar *glob_cluster_mgr_port= "12002";

static GOptionEntry entries[] = 
{
  { "cluster_server_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_server_ip,
    "Set Server Hostname of Cluster Server", NULL},
  { "cluster_server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "cluster_manager_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_mgr_ip,
    "Set Server Hostname of Cluster Manager", NULL},
  { "cluster_server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_mgr_port,
    "Set Server Port of Cluster Manager", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
set_up_server_connection(IC_CONNECTION **conn)
{
  IC_CONNECTION *loc_conn;
  int ret_code;

  if (!(loc_conn= ic_create_socket_object(FALSE, TRUE, FALSE, FALSE,
                                          NULL, NULL)))
  {
    printf("Failed to create Connection object\n");
    return 1;
  }
  printf("Setting up server connection for Cluster Manager at %s:%s\n",
         glob_cluster_mgr_ip, glob_cluster_mgr_port);
  loc_conn->server_name= glob_cluster_mgr_ip;
  loc_conn->server_port= glob_cluster_mgr_port;
  loc_conn->is_listen_socket_retained= TRUE;
  if ((ret_code= loc_conn->conn_op.ic_set_up_connection(loc_conn)))
  {
    printf("Failed to connect to Cluster Manager\n");
    loc_conn->conn_op.ic_free_connection(loc_conn);
    return 1;
  }
  *conn= loc_conn;
  return 0;
}

static gpointer
run_handle_new_connection(gpointer data)
{
  int ret_code;
  guint32 read_size;
  guint32 size_curr_buf= 0;
  IC_CONNECTION *conn= (IC_CONNECTION*)data;
  gchar rec_buf[256];

  while (!(ret_code= ic_rec_with_cr(conn, rec_buf, &read_size,
                                    &size_curr_buf, sizeof(rec_buf))))
  {
    if (read_size == 0)
      ic_send_with_cr(conn, "");
    else
      printf("%s\n", rec_buf);
  }
  return NULL;
}

static int
handle_new_connection(IC_CONNECTION *conn)
{
  GError *error= NULL;
  if (!g_thread_create_full(run_handle_new_connection,
                            (gpointer)conn,
                            1024*256, /* 256 kByte stack size */
                            FALSE,   /* Not joinable        */
                            FALSE,   /* Not bound           */
                            G_THREAD_PRIORITY_NORMAL,
                            &error))
  {
    conn->error_code= 1;
    return 1;
  }
  return 0;
}
static int
wait_for_connections_and_fork(IC_CONNECTION *conn)
{
  int ret_code;
  IC_CONNECTION *fork_conn;
  do
  {
    if ((ret_code= conn->conn_op.ic_accept_connection(conn)))
      goto error;
    printf("Cluster Manager has accepted a new connection\n");
    if (!(fork_conn= conn->conn_op.ic_fork_accept_connection(conn,
                                              FALSE,   /* No mutex */
                                              FALSE))) /* No front buffer */
    {
      printf("Failed to fork a new connection from an accepted connection\n");
      goto error;
    }
    if ((ret_code= handle_new_connection(fork_conn)))
    {
      printf("Failed to start new Cluster Manager thread\n");
      goto error;
    }
  } while (1);
  return 0;
error:
  return ret_code;
}

int main(int argc,
         char *argv[])
{
  int ret_code= 1;
  IC_CONNECTION *conn;

  if ((ret_code= ic_start_program(argc, argv, entries,
           "- iClaustron Cluster Manager")))
    return ret_code;
  if ((ret_code= set_up_server_connection(&conn)))
    goto error;
  ret_code= wait_for_connections_and_fork(conn);
late_error:
  conn->conn_op.ic_free_connection(conn);
error:
  ic_end();
  return ret_code;
}
