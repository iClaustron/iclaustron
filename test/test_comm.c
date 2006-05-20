#include <ic_comm.h>
#include <ic_common.h>

static gboolean is_client= FALSE;

static GOptionEntry entries[] = 
{
  { "is_client", 0, 0, G_OPTION_ARG_NONE, &is_client, "Set process as client", NULL}
};

int main(int argc, char *argv[])
{
  GError *error= NULL;
  GOptionContext *context;
  struct ic_connection conn;
  int ret_code;

  context= g_option_context_new("- Basic test program communication module");
  g_option_context_add_main_entries(context, entries, NULL);
  g_option_context_parse(context, &argc, &argv, &error);

  printf("1\n");
  conn.server_ip= 0x7f000001;
  conn.server_port= 10000;
  conn.client_ip= 0x7f000001;
  conn.client_port= 10001;
  conn.is_client= is_client;
  conn.backlog= 1;
  printf("3\n");
  set_socket_methods(&conn);
  printf("4\n");
  ret_code= conn.conn_op.set_up_ic_connection(&conn);
  printf("ret_code = %d\n", ret_code);
  return 0;
}

