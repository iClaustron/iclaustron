#include <ic_comm.h>
#include <ic_common.h>

static gboolean glob_is_client= FALSE;
static guint32 glob_server_ip= 0x7f000001;
static guint32 glob_client_ip= 0x7f000001;
static guint32 glob_server_port= 10000;
static guint32 glob_client_port= 10001;
static int glob_tcp_maxseg= 0;
static int glob_tcp_rec_size= 0;
static int glob_tcp_snd_size= 0;
static gboolean glob_is_wan_connection= FALSE;

static GOptionEntry entries[] = 
{
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
  int ret_code;

  printf("Connection Test Started\n");
  ic_init_socket_object(&conn, glob_is_client, TRUE, FALSE);
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
    printf("ret_code = %d\n", ret_code);
    return;
  }
  conn.conn_op.close_ic_connection(&conn);
  printf("Connection Test Success\n");
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
  connection_test();
  return 0;

parse_error:

error:
  return 1;
}

