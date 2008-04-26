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
#include <ic_apic.h>

static gboolean glob_is_client= FALSE;
static gchar *glob_server_ip= "127.0.0.1";
static gchar *glob_client_ip= "127.0.0.1";
static gchar *glob_server_port= "10006";
static gchar *glob_client_port= "12002";

static int glob_tcp_maxseg= 0;
static int glob_tcp_rec_size= 0;
static int glob_tcp_snd_size= 0;
static int glob_test_type= 5;
static gboolean glob_is_wan_connection= FALSE;

static GOptionEntry entries[] = 
{
  { "test_type", 0, 0, G_OPTION_ARG_INT, &glob_test_type,
    "Set test type", NULL},
  { "is_client", 0, 0, G_OPTION_ARG_NONE, &glob_is_client,
    "Set process as client", NULL},
  { "is_wan_connection", 0, 0, G_OPTION_ARG_NONE, &glob_is_wan_connection,
    "Set defaults for connection as WAN connection", NULL},
  { "tcp_maxseg", 0, 0, G_OPTION_ARG_INT, &glob_tcp_maxseg,
    "Set TCP_MAXSEG", NULL},
  { "SO_RCVBUF", 0, 0, G_OPTION_ARG_INT, &glob_tcp_rec_size,
    "Set SO_RCVBUF on connection", NULL},
  { "SO_SNDBUF", 0, 0, G_OPTION_ARG_INT, &glob_tcp_snd_size,
    "Set SO_SNDBUF on connection", NULL},
  { "server_name", 0, 0, G_OPTION_ARG_STRING, &glob_server_ip,
    "Set Server Host address", NULL},
  { "client_name", 0, 0, G_OPTION_ARG_STRING, &glob_client_ip,
    "Set Client Host Address", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Server Port", NULL},
  { "client_port", 0, 0, G_OPTION_ARG_STRING, &glob_client_port,
    "Set Client port", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

#ifdef WITH_UNIT_TEST
static void
unit_test_mc()
{
  IC_MEMORY_CONTAINER *mc_ptr;
  guint32 i, j, k, max_alloc_size, no_allocs;
  gboolean large;
  for (i= 1; i < 1000; i++)
  {
    srandom(i);
    mc_ptr= ic_create_memory_container(313*i, 0);
    no_allocs= random() & 255;
    for (j= 0; j < 4; j++)
    {
      for (k= 0; k < no_allocs; k++)
      {
        large= ((random() & 3) == 1); /* 25% of allocations are large */
        if (large)
          max_alloc_size= 32767;
        else
          max_alloc_size= 511;
        mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, random() & max_alloc_size);
      }
      mc_ptr->mc_ops.ic_mc_reset(mc_ptr);
    }
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  }
}
#endif

static int
connection_test(gboolean use_ssl)
{
  IC_CONNECTION *conn;
  char buf[8192];
  int ret_code;

  printf("Connection Test Started\n");
  if (use_ssl)
  {
#ifdef HAVE_SSL
    IC_STRING root_certificate_path;
    IC_STRING server_certificate_path;
    IC_STRING client_certificate_path;
    IC_STRING passwd_string;
    IC_STRING *loc_cert_path;
    IC_INIT_STRING(&passwd_string,
                   glob_passwd_string,
                   strlen(glob_passwd_string),
                   TRUE);
    IC_INIT_STRING(&root_certificate_path,
                   glob_root_certificate_path,
                   strlen(glob_root_certificate_path),
                   TRUE);
    IC_INIT_STRING(&server_certificate_path,
                   glob_server_certificate_path,
                   strlen(glob_server_certificate_path),
                   TRUE);
    IC_INIT_STRING(&client_certificate_path,
                   glob_client_certificate_path,
                   strlen(glob_client_certificate_path),
                   TRUE);
    loc_cert_path= glob_is_client ?
           &client_certificate_path :
           &server_certificate_path;
    if (!(conn= ic_create_ssl_object(glob_is_client,
                                     &root_certificate_path,
                                     loc_cert_path,
                                     &passwd_string,
                                     TRUE, FALSE, FALSE,
                                     NULL, NULL)))
    {
      printf("Error creating SSL connection object\n");
      return 1;
    }
#else
    printf("SSL not supported in this build\n");
    return 0;
#endif
  }
  else
  {
    if (!(conn= ic_create_socket_object(glob_is_client, TRUE, FALSE, TRUE,
                                        NULL, NULL)))
    {
      printf("Memory allocation error\n");
      return MEM_ALLOC_ERROR;
    }
  }
  conn->server_name= glob_server_ip;
  conn->server_port= glob_server_port;
  conn->client_name= glob_client_ip;
  conn->client_port= glob_client_port;
  conn->is_wan_connection= glob_is_wan_connection;
  conn->tcp_maxseg_size= glob_tcp_maxseg;
  conn->tcp_receive_buffer_size= glob_tcp_rec_size;
  conn->tcp_send_buffer_size= glob_tcp_snd_size;
  ret_code= conn->conn_op.ic_set_up_connection(conn);
  if (ret_code != 0)
  {
    printf("Error in connection set-up: ret_code = %d\n", ret_code);
    return 1;
  }
  if (glob_is_client)
  {
    guint32 read_size;
    printf("Start reading\n");
    while (!conn->conn_op.ic_read_connection(conn,
                                             (void*)buf,
                                             sizeof(buf),
                                             &read_size))
      ;
  }
  else
  {
    GTimer *timer;
    double time_spent;
    unsigned i;
    memset(buf, 0, sizeof(buf));
    timer= g_timer_new(); /* No errror check in test program */
    printf("Start writing\n");
    g_timer_start(timer);
    for (i= 0; i < 8192; i++)
    {
      if (conn->conn_op.ic_write_connection(conn,
                                            (const void*)buf,
                                            sizeof(buf), 0,
                                            2))
        break;
    }
    time_spent= g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);
    printf("Time spent in writing 64 MBytes: %f\n", time_spent);
  }
  conn->conn_op.ic_write_stat_connection(conn);
  conn->conn_op.ic_close_connection(conn);
  conn->conn_op.ic_free_connection(conn);
  printf("Connection Test Success\n");
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

  printf("Starting API Cluster server test\n");
  cluster_conn.cluster_server_ips= &glob_server_ip;
  cluster_conn.cluster_server_ports= &glob_server_port;
  cluster_conn.num_cluster_servers= 1;
  if ((srv_obj= ic_create_api_cluster(&cluster_conn)))
    return 1;
  if (glob_test_type > 1)
  {
    printf("Testing print of config parameters\n");
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
  clu_info_ptr[0]= &clu_info;
  clu_info_ptr[1]= NULL;
  IC_INIT_STRING(&clu_info.cluster_name, kalle_str, strlen(kalle_str), TRUE);
  IC_INIT_STRING(&clu_info.password, kalle_pwd_str, strlen(kalle_pwd_str),
                 TRUE);
  clu_info.cluster_id= IC_MAX_UINT32;
  srv_obj->api_op.ic_get_config(srv_obj, &clu_info_ptr[0], &node_id);
  srv_obj->api_op.ic_free_config(srv_obj);
  printf("Completing cluster server test\n");
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
/*  Methods to send and receive buffers with Carriage Return

int ic_send_with_cr(IC_CONNECTION *conn, const char *buf);
int ic_rec_with_cr(IC_CONNECTION *conn,
                   gchar *rec_buf,
                   guint32 *read_size,
                   guint32 *size_curr_buf,
                   guint32 buffer_size);*/

static int
test_pcntrl()
{
  int ret_code, error;
  IC_CONNECTION *conn;
  gchar read_buf[256];
  guint32 read_size=0;
  guint32 size_curr_buf=0;

  if (!(conn= ic_create_socket_object(TRUE, TRUE, FALSE, TRUE,
                                      NULL, NULL)))
  {
    printf("Memory allocation error\n");
    return MEM_ALLOC_ERROR;
  }
  conn->server_name= glob_server_ip;
  conn->server_port= glob_server_port;
  conn->client_name= glob_client_ip;
  conn->client_port= glob_client_port;
  conn->is_connect_thread_used= FALSE;
  conn->is_wan_connection= glob_is_wan_connection;
  conn->tcp_maxseg_size= glob_tcp_maxseg;
  conn->tcp_receive_buffer_size= glob_tcp_rec_size;
  conn->tcp_send_buffer_size= glob_tcp_snd_size;
  ret_code= conn->conn_op.ic_set_up_connection(conn);
  if ((error= ic_send_with_cr(conn, "ls")) ||
      (error= ic_send_with_cr(conn, "-la")) ||
      (error= ic_send_with_cr(conn, "")))
  {
    ic_print_error(error);
    goto end;
  }
  if ((error= ic_rec_with_cr(conn, read_buf, &read_size,
                             &size_curr_buf, sizeof(read_buf))))
  {
    ic_print_error(error);
    goto end;
  }
end:
  conn->conn_op.ic_free_connection(conn);
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code= 1;

  if ((ret_code= ic_start_program(argc, argv, entries,
           "- Basic test program communication module")))
    return ret_code;
  printf("Server ip = %s, Client ip = %s\n", glob_server_ip, glob_client_ip);
  printf("Server port = %s, Client port = %s\n",
         glob_server_port, glob_client_port);
  switch (glob_test_type)
  {
    case 0:
      ret_code= connection_test(FALSE);
      break;
    case 1:
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
#ifdef WITH_UNIT_TEST
    case 9:
      unit_test_mc();
      break;
#endif
    case 10:
      ret_code= connection_test(TRUE);
      break;
    default:
      break;
   }
   ic_end();
   return ret_code;
}

