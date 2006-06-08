/*
  This program handles start and stop of cluster processes. It reads a configuration
  file and based on its content enables start of a number of processes on the local
  machine. It can start and stop processes belonging to different clusters. These sets
  of clusters are referred to as a cluster group. A cluster group shares a common set
  of Cluster Servers. These Cluster Servers handle the configuration and changes to the
  configuration. They are also an essential point of contact for all other management
  of the cluster and can be used to retrieve information from the cluster about its
  current state.

  This program is also used to gather information from local log files as part of any
  process to gather information about mishaps in the cluster(s).

  This process listens to connections on a server port, this is either a default port or
  one assigned when starting program. One can also set the IP address to use when listening
  to connections. By default the process will discover its IP address by itself.

  The program by default uses a configuration placed in /etc/ic_cntrl.conf but this can be
  changed by a parameter to the program.

  This program can be started and stopped by services in the Linux environment.
  service ic_cntrl start
  service ic_cntrl stop
  service ic_cntrl status
  service ic_cntrl restart
  The normal usage of ic_cntrl is to place it such that it is automatically started when the OS
  starts. Then use the service commands to control its operation. Start will start if the service
  isn't active already. Stop will stop its operation. It will not stop any of the processes that
  the program has spawned off. Status is used to check its current status. Finally restart will
  stop and start the program. The most common use of these operations are restart which can be used
  to force the program to reread the configuration file.
*/

#include <ic_comm.h>
#include <ic_common.h>

static gchar *glob_ip= NULL;
static gint   glob_port= 10002;
static gchar *glob_config_file= "/etc/ic_cntrl.conf";

static GOptionEntry entries[] = 
{
  { "ip", 0, 0, G_OPTION_ARG_STRING, &glob_ip,
    "Set IP address, default is IP address of computer", NULL},
  { "port", 0, 0, G_OPTION_ARG_INT, &glob_port, "Set Port, default = 10002", NULL},
  { "config-file", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_file,
    "Sets path to configuration file, default /etc/ic_cntrl.conf", NULL},
  { NULL }
};

static int start_services(struct ic_connection *conn,
                          GKeyFile *conf_file)
{
  int ret_code= 1;
  while (!conn->conn_op.accept_ic_connection(conn))
  {
    ret_code= 0;
    break;
  }
  return ret_code;
}

int main(int argc, char *argv[])
{
  GError *error= NULL;
  GOptionContext *context;
  GKeyFile *conf_file= NULL;
  struct ic_connection conn;
  int ret_code;

  /* Read command options */
  context= g_option_context_new("iClaustron Control Server");
  if (!context)
    goto mem_error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);

  /* Read configuration file */
  if (!(conf_file= g_key_file_new()))
    goto mem_error;
/*
  if (!g_key_file_load_from_file(conf_file, glob_config_file,
                                 G_KEY_FILE_NONE, &error))
    goto conf_file_error;
*/
  /* Start listening for connections */
  if (glob_ip)
  {
    /*conn.server_ip= glob_ip;*/
  }
  else
    conn.server_ip= INADDR_ANY;
  conn.server_port= glob_port;
  conn.is_client= FALSE;
  conn.client_ip= 0;
  conn.client_port= 0;
  conn.backlog= 5;
  conn.call_accept= FALSE;
  
  set_socket_methods(&conn);
  if ((ret_code= conn.conn_op.set_up_ic_connection(&conn)))
  {
    printf("Failed to set up listening port\n");
    g_key_file_free(conf_file);
    return 1;
  }

  /* Run the services of this program */
  start_services(&conn, conf_file);

  conn.conn_op.close_ic_connection(&conn); 
  g_key_file_free(conf_file);
  return 0;

conf_file_error:
  printf("Configuration File Error: \n%s\n", error->message);
  return 1;

mem_error:
  printf("Memory allocation error\n");
  return 1;

parse_error:
  printf("Error in parsing option parameters, use --help for assistance\n%s\n", error->message);
error:
  return 1;
}

