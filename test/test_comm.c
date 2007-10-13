#include <ic_common.h>
#include <ic_apic.h>

static gboolean glob_is_client= FALSE;
static gchar *glob_server_ip= "127.0.0.1";
static gchar *glob_client_ip= "127.0.0.1";
static gchar *glob_server_port= "10003";
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

static int
connection_test()
{
  IC_CONNECTION *conn;
  char buf[8192];
  int ret_code;

  printf("Connection Test Started\n");
  if (!(conn= ic_create_socket_object(glob_is_client, TRUE, FALSE, TRUE,
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
  ret_code= conn->conn_op.set_up_ic_connection(conn);
  if (ret_code != 0)
  {
    printf("Error in connection set-up: ret_code = %d\n", ret_code);
    return 1;
  }
  if (glob_is_client)
  {
    guint32 read_size;
    printf("Start reading\n");
    while (!conn->conn_op.read_ic_connection(conn,
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
      if (conn->conn_op.write_ic_connection(conn,
                                            (const void*)buf,
                                            sizeof(buf), 0,
                                            2))
        break;
    }
    time_spent= g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);
    printf("Time spent in writing 64 MBytes: %f\n", time_spent);
  }
  conn->conn_op.write_stat_ic_connection(conn);
  conn->conn_op.close_ic_connection(conn);
  printf("Connection Test Success\n");
  return 0;
}

static int
api_clusterserver_test()
{
  IC_API_CONFIG_SERVER *srv_obj; 
  IC_API_CLUSTER_CONNECTION cluster_conn;
  guint32 cluster_id= 0;
  guint32 node_id= 0;

  printf("Starting API Cluster server test\n");
  cluster_conn.cluster_server_ips= &glob_server_ip;
  cluster_conn.cluster_server_ports= &glob_server_port;
  cluster_conn.num_cluster_servers= 1;
  srv_obj= ic_init_api_cluster(&cluster_conn, &cluster_id,
                               &node_id, (guint32)1);
  if (glob_test_type > 1)
  {
    printf("Testing print of config parameters\n");
    if (glob_test_type == 2)
      ic_print_config_parameters(1 << IC_KERNEL_TYPE);
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
  srv_obj->num_clusters_to_connect= 1;
  srv_obj->api_op.get_ic_config(srv_obj, (guint64)0x60301);
  /*srv_obj->api_op.get_ic_config(srv_obj, (guint64)0x000000); */
  srv_obj->api_op.free_ic_config(srv_obj);
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
static int
run_clusterserver_test()
{
  IC_RUN_CLUSTER_SERVER *run_obj;
  IC_CONFIG_STRUCT clu_conf_struct;
  IC_CLUSTER_CONFIG *clu_conf;
  guint32 cluster_id= 0;
  int ret_code= 0;
  gchar *conf_file= "config.ini";

  printf("Starting Run Cluster server test\n");
  if (!(clu_conf= ic_load_config_server_from_files(conf_file,
                                                   &clu_conf_struct)))
  {
    printf("Failed to load config file %s from disk", conf_file);
    return 1;
  }
  run_obj= ic_create_run_cluster(&clu_conf, &cluster_id, (guint32)1,
                                 glob_server_ip, glob_server_port);

  if ((ret_code= run_obj->run_op.run_ic_cluster_server(run_obj)))
  {
    printf("run_cluster_server returned error code %u\n", ret_code);
    ic_print_error(ret_code);
    goto end;
  }
end:
  clu_conf_struct.clu_conf_ops->ic_config_end(&clu_conf_struct);
  run_obj->run_op.free_ic_run_cluster(run_obj);
  return ret_code;
}

static int
/*  Methods to send and receive buffers with Carriage Return

int ic_send_with_cr(IC_CONNECTION *conn, const char *buf);
int ic_rec_with_cr(IC_CONNECTION *conn,
                   gchar *rec_buf,
                   guint32 *read_size,
                   guint32 *size_curr_buf,
                   guint32 buffer_size);*/

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
  ret_code= conn->conn_op.set_up_ic_connection(conn);
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
  conn->conn_op.free_ic_connection(conn);
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code= 1;
  GError *error= NULL;
  GOptionContext *context;

  context= g_option_context_new("- Basic test program communication module");
  if (!context)
    goto error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);
  printf("Server ip = %s, Client ip = %s\n", glob_server_ip, glob_client_ip);
  printf("Server port = %s, Client port = %s\n",
         glob_server_port, glob_client_port);
  if (ic_init())
    return ret_code;
  switch (glob_test_type)
  {
    case 0:
      ret_code= connection_test();
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
      ret_code= run_clusterserver_test();
      break;
    case 8:
      test_pcntrl();
      break;
    default:
      break;
   }
   ic_end();
   return ret_code;
parse_error:
  printf("No such program option\n");
error:
  return ret_code;
}

