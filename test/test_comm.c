/* Copyright (C) 2007-2012 iClaustron AB

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
#include <ic_protocol_support.h>
#include <ic_apic.h>
#include <ic_apid.h>

static const gchar *glob_process_name= "test_comm";
static gboolean glob_is_client= FALSE;
static gchar *glob_server_ip= "127.0.0.1";
static gchar *glob_client_ip= "127.0.0.1";
static gchar *glob_server_port= "10006";
static gchar *glob_client_port= "12002";
#ifdef HAVE_SSL
static gchar *glob_root_certificate_path= NULL;
static gchar *glob_certificate_path= NULL;
static gchar *glob_passwd_string= NULL;
#endif

static int glob_tcp_maxseg= 0;
static int glob_tcp_rec_size= 0;
static int glob_tcp_snd_size= 0;
static int glob_test_type= 5;
static gboolean glob_is_wan_connection= FALSE;

static GOptionEntry entries[] = 
{
  { "test-type", 0, 0, G_OPTION_ARG_INT, &glob_test_type,
    "Set test type", NULL},
  { "is-client", 0, 0, G_OPTION_ARG_NONE, &glob_is_client,
    "Set process as client", NULL},
  { "is-wan-connection", 0, 0, G_OPTION_ARG_NONE, &glob_is_wan_connection,
    "Set defaults for connection as WAN connection", NULL},
  { "tcp_maxseg", 0, 0, G_OPTION_ARG_INT, &glob_tcp_maxseg,
    "Set TCP_MAXSEG", NULL},
  { "SO_RCVBUF", 0, 0, G_OPTION_ARG_INT, &glob_tcp_rec_size,
    "Set SO_RCVBUF on connection", NULL},
  { "SO_SNDBUF", 0, 0, G_OPTION_ARG_INT, &glob_tcp_snd_size,
    "Set SO_SNDBUF on connection", NULL},
  { "server-name", 0, 0, G_OPTION_ARG_STRING, &glob_server_ip,
    "Set Server Host address", NULL},
  { "client-name", 0, 0, G_OPTION_ARG_STRING, &glob_client_ip,
    "Set Client Host Address", NULL},
  { "server-port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Server Port", NULL},
  { "client-port", 0, 0, G_OPTION_ARG_STRING, &glob_client_port,
    "Set Client port", NULL},
#ifdef HAVE_SSL
  { "root-certificate", 0, 0, G_OPTION_ARG_STRING, &glob_root_certificate_path,
    "Filename of SSL root certificate", NULL},
  { "certificate", 0, 0, G_OPTION_ARG_STRING, &glob_certificate_path,
    "Filename of SSL certificate", NULL},
  { "password", 0, 0, G_OPTION_ARG_STRING, &glob_passwd_string,
    "SSL password", NULL},
#endif
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
connection_test(gboolean use_ssl)
{
  IC_CONNECTION *conn;
  char buf[8192];
  int ret_code;
#ifdef HAVE_SSL
  IC_STRING root_certificate_path;
  IC_STRING certificate_path;
  IC_STRING passwd_string;
#endif

  ic_printf("Connection Test Started");
  if (use_ssl)
  {
#ifdef HAVE_SSL
    if (!glob_root_certificate_path ||
        !glob_certificate_path ||
        !glob_passwd_string)
    {
      ic_printf("Need password, root certificate + certificate");
      return 1;
    }

    IC_INIT_STRING(&passwd_string,
                   glob_passwd_string,
                   strlen(glob_passwd_string),
                   TRUE);
    IC_INIT_STRING(&root_certificate_path,
                   glob_root_certificate_path,
                   strlen(glob_root_certificate_path),
                   TRUE);
    IC_INIT_STRING(&certificate_path,
                   glob_certificate_path,
                   strlen(glob_certificate_path),
                   TRUE);
    if (!(conn= ic_create_ssl_object(glob_is_client,
                                     &root_certificate_path,
                                     &certificate_path,
                                     &passwd_string,
                                     TRUE, FALSE,
                                     CONFIG_READ_BUF_SIZE,
                                     NULL, NULL)))
    {
      ic_printf("Error creating SSL connection object");
      return 1;
    }
#else
    ic_printf("SSL not supported in this build");
    return 0;
#endif
  }
  else
  {
    if (!(conn= ic_create_socket_object(glob_is_client, TRUE, FALSE,
                                        CONFIG_READ_BUF_SIZE,
                                        NULL, NULL)))
    {
      ic_printf("Memory allocation error");
      return IC_ERROR_MEM_ALLOC;
    }
  }
  if (glob_is_client)
  {
   conn->conn_op.ic_prepare_client_connection(
         conn,
         glob_server_ip,
         glob_server_port,
         glob_client_ip,
         glob_client_port);
  }
  else
  {
    conn->conn_op.ic_prepare_server_connection(
         conn,
         glob_server_ip,
         glob_server_port,
         glob_client_ip,
         glob_client_port,
         0, /* Default backlog */
         FALSE /* Listen socket not retained */);
  }
  conn->conn_op.ic_prepare_extra_parameters(
       conn,
       glob_tcp_maxseg,
       glob_is_wan_connection,
       glob_tcp_rec_size,
       glob_tcp_snd_size);
  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);
  if (ret_code != 0)
  {
    ic_printf("Error in connection set-up: ret_code = %d", ret_code);
    conn->conn_op.ic_free_connection(conn);
    return ret_code;
  }
  if (glob_is_client)
  {
    guint32 read_size;
    ic_printf("Start reading");
    while (!(ret_code= conn->conn_op.ic_read_connection(conn,
                                                        (void*)buf,
                                                        sizeof(buf),
                                                        &read_size)))
      ;
    if (ret_code)
      ic_print_error(ret_code);
  }
  else
  {
    GTimer *timer;
    double time_spent;
    unsigned i;
    ic_zero(buf, sizeof(buf));
    timer= g_timer_new(); /* No errror check in test program */
    ic_printf("Start writing");
    g_timer_start(timer);
    for (i= 0; i < 8192; i++)
    {
      if (conn->conn_op.ic_write_connection(conn,
                                            (const void*)buf,
                                            sizeof(buf),
                                            2))
        break;
    }
    time_spent= g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);
    ic_printf("Time spent in writing 64 MBytes: %f", time_spent);
  }
  conn->conn_op.ic_write_stat_connection(conn);
  conn->conn_op.ic_close_connection(conn);
  conn->conn_op.ic_free_connection(conn);
  ic_printf("Connection Test Success");
  return 0;
}

static gchar *kalle_str= "kalle";
static gchar *kalle_pwd_str= "jetset";

static int
api_clusterserver_test()
{
  IC_API_CONFIG_SERVER *srv_obj; 
  IC_API_CLUSTER_CONNECTION cluster_conn;
  IC_CLUSTER_CONNECT_INFO clu_info;
  IC_CLUSTER_CONNECT_INFO *clu_info_ptr[2];
  guint32 node_id= 1;

  ic_printf("Starting API Cluster server test");
  cluster_conn.cluster_server_ips= &glob_server_ip;
  cluster_conn.cluster_server_ports= &glob_server_port;
  cluster_conn.num_cluster_servers= 1;
  if (glob_test_type > 1)
  {
    ic_printf("Testing print of config parameters");
    if (glob_test_type == 2)
      ic_print_config_parameters(1 << IC_DATA_SERVER_TYPE);
    if (glob_test_type == 3)
      ic_print_config_parameters(1 << IC_CLIENT_TYPE);
    if (glob_test_type == 4)
      ic_print_config_parameters(1 << IC_CLUSTER_SERVER_TYPE);
    if (glob_test_type == 5)
      ic_print_config_parameters(1 << IC_COMM_TYPE);
    if (glob_test_type == 6) /* Print all */
      ic_print_config_parameters(0xFFFFFFFF);
    return 0;
  }
  if ((srv_obj= ic_create_api_cluster(&cluster_conn, TRUE)))
    return 1;
  clu_info_ptr[0]= &clu_info;
  clu_info_ptr[1]= NULL;
  IC_INIT_STRING(&clu_info.cluster_name, kalle_str, strlen(kalle_str), TRUE);
  IC_INIT_STRING(&clu_info.password, kalle_pwd_str, strlen(kalle_pwd_str),
                 TRUE);
  clu_info.cluster_id= IC_MAX_UINT32;
  srv_obj->api_op.ic_get_config(srv_obj, &clu_info_ptr[0], node_id, (guint32)3);
  srv_obj->api_op.ic_free_config(srv_obj);
  ic_printf("Completing cluster server test");
  return 0;
}

/*
  This test case is used for initial development of Cluster Server.
  It will start by receiving the Cluster Configuration from a file
  and set-up configuration based on it.

  Next step is to wait for configuration requests from a client and
  send response.
  existing Cluster Server and set-up the data structures based
  on the input.
  The next step is to wait for Cluster node to try to read the
  configuration from the Cluster Server. After one such attempt
  the test will conclude.
*/

static int
test_pcntrl()
{
  gchar *read_buf;
  guint32 read_size;
  int ret_code;
  IC_CONNECTION *conn;

  if (!(conn= ic_create_socket_object(TRUE, TRUE, FALSE,
                                      CONFIG_READ_BUF_SIZE,
                                      NULL, NULL)))
  {
    ic_printf("Memory allocation error");
    return IC_ERROR_MEM_ALLOC;
  }
  conn->conn_op.ic_prepare_client_connection(conn,
                                             glob_server_ip,
                                             glob_server_port,
                                             glob_client_ip,
                                             glob_client_port);
  conn->conn_op.ic_prepare_extra_parameters(conn,
                                            glob_tcp_maxseg,
                                            glob_is_wan_connection,
                                            glob_tcp_rec_size,
                                            glob_tcp_snd_size);
  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);
  if (ret_code ||
      (ret_code= ic_send_with_cr(conn, "ls")) ||
      (ret_code= ic_send_with_cr(conn, "-la")) ||
      (ret_code= ic_send_with_cr(conn, "")))
  {
    ic_print_error(ret_code);
    goto end;
  }
  if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    ic_print_error(ret_code);
    goto end;
  }
end:
  conn->conn_op.ic_free_connection(conn);
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code= 1;
  int i;

  if ((ret_code= ic_start_program(argc,
                                  argv,
                                  entries,
                                  NULL,
                                  glob_process_name,
                                  "- Basic test program communication module",
                                  TRUE,
                                  FALSE)))
    return ret_code;
  if (glob_test_type == 1)
  {
    glob_client_ip= NULL;
    glob_client_port= NULL;
  }
  ic_printf("Server ip = %s, Client ip = %s", glob_server_ip,
            glob_client_ip != NULL ? glob_client_ip : "NULL");
  ic_printf("Server port = %s, Client port = %s",
            glob_server_port,
            glob_client_port != NULL ? glob_client_port : "NULL");
  switch (glob_test_type)
  {
    case 0:
    case 1:
      for (i= 0; i < 4; i++)
        ret_code= connection_test(FALSE);
      break;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      /* api_cluster_server_test() uses all the test_type 1-6 */
      ret_code= api_clusterserver_test();
      break;
    case 7:
      break;
    case 8:
      test_pcntrl();
      break;
    case 10:
      ret_code= connection_test(TRUE);
      break;
    default:
      break;
   }
   ic_end();
   return ret_code;
}
