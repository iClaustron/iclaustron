#include <ic_common.h>
#include <ic_apic.h>

static gboolean glob_is_client= FALSE;
//static guint32 glob_server_ip= 0x7f000001;
static guint32 glob_server_ip= 0x7f000001;
static guint32 glob_client_ip= 0x7f000001;
static guint16 glob_server_port= 10005;
static guint16 glob_client_port= 12001;
static int glob_tcp_maxseg= 0;
static int glob_tcp_rec_size= 0;
static int glob_tcp_snd_size= 0;
static int glob_test_type= 0;
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
  { "server_ip", 0, 0, G_OPTION_ARG_INT, &glob_server_ip,
    "Set Server IP address", NULL},
  { "client_ip", 0, 0, G_OPTION_ARG_INT, &glob_client_ip,
    "Set Client IP Address", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_INT, &glob_server_port,
    "Set Server Port", NULL},
  { "client_port", 0, 0, G_OPTION_ARG_INT, &glob_client_port,
    "Set Client port", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

void connection_test()
{
  struct ic_connection conn;
  char buf[8192];
  int ret_code;

  printf("Connection Test Started\n");
  ic_init_socket_object(&conn, glob_is_client, TRUE, FALSE, TRUE);
  conn.server_ip= glob_server_ip;
  conn.server_port= glob_server_port;
  conn.client_ip= glob_client_ip;
  conn.client_port= glob_client_port;
  conn.is_connect_thread_used= FALSE;
  conn.is_wan_connection= glob_is_wan_connection;
  conn.tcp_maxseg_size= glob_tcp_maxseg;
  conn.tcp_receive_buffer_size= glob_tcp_rec_size;
  conn.tcp_send_buffer_size= glob_tcp_snd_size;
  ret_code= conn.conn_op.set_up_ic_connection(&conn);
  if (ret_code != 0)
  {
    printf("Error in connection set-up: ret_code = %d\n", ret_code);
    return;
  }
  if (glob_is_client)
  {
    guint32 read_size;
    printf("Start reading\n");
    while (!conn.conn_op.read_ic_connection(&conn,
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
      if (conn.conn_op.write_ic_connection(&conn,
                                           (const void*)buf,
                                           sizeof(buf), 0,
                                           2))
        break;
    }
    time_spent= g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);
    printf("Time spent in writing 64 MBytes: %f\n", time_spent);
  }
  conn.conn_op.write_stat_ic_connection(&conn);
  conn.conn_op.close_ic_connection(&conn);
  printf("Connection Test Success\n");
}

static int
api_clusterserver_test()
{
  struct ic_api_cluster_server *srv_obj; 
  struct ic_api_cluster_connection cluster_conn;
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
  srv_obj->api_op.get_ic_config(srv_obj);
  srv_obj->api_op.free_ic_config(srv_obj);
  printf("Completing cluster server test\n");
  return 0;
}

/*
  This test case is used for initial development of Cluster Server.
  It will start by receiving the Cluster Configuration from an
  existing Cluster Server and set-up the data structures based
  on the input.
  The next step is to wait for Cluster node to try to read the
  configuration from the Cluster Server. After one such attempt
  the test will conclude.
*/
static int
run_clusterserver_test()
{
  struct ic_api_cluster_server *srv_obj; 
  struct ic_api_cluster_connection cluster_conn;
  guint32 cluster_id= 0;
  guint32 node_id= 0;

  printf("Starting Run Cluster server test\n");
  cluster_conn.cluster_server_ips= &glob_server_ip;
  cluster_conn.cluster_server_ports= &glob_server_port;
  cluster_conn.num_cluster_servers= 1;
  srv_obj= ic_init_api_cluster(&cluster_conn, &cluster_id,
                               &node_id, (guint32)1);

  srv_obj->num_clusters_to_connect= 1;
  srv_obj->api_op.get_ic_config(srv_obj);
  return 0;
}

int main(int argc, char *argv[])
{
  GError *error= NULL;
  GOptionContext *context;

  context= g_option_context_new("- Basic test program communication module");
  if (!context)
    goto error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);
  switch (glob_test_type)
  {
    case 0:
      connection_test();
      return 0;
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      api_clusterserver_test();
      return 0;
    case 7:
      run_clusterserver_test();
      return 0;
    default:
      return 0;
   }

parse_error:
  printf("No such program option\n");
error:
  return 1;
}

