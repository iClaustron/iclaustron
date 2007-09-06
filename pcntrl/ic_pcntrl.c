#include <glib.h>
#include <stdio.h>


/*
  This program is also used to gather information from local log files as 
  part of any process to gather information about mishaps in the cluster(s).

  This process listens to connections on a server port, this is either a
  default port or one assigned when starting program. One can also set the
  IP address to use when listening to connections. By default the process
  will discover its IP address by itself.

  The program by default uses a configuration placed in /etc/ic_cntrl.conf
  but this can be changed by a parameter to the program.

  This program can be started and stopped by services in the Linux
  environment.
  service ic_cntrl start
  service ic_cntrl stop
  service ic_cntrl status
  service ic_cntrl restart
  The normal usage of ic_cntrl is to place it such that it is automatically
  started when the OS starts. Then use the service commands to control its
  operation. Start will start if the service isn't active already. Stop will
  stop its operation. It will not stop any of the processes that the program
  has spawned off. Status is used to check its current status. Finally
  restart will stop and start the program. The most common use of these
  operations are restart which can be used to force the program to reread
  the configuration file.
*/

#include <ic_comm.h>
#include <ic_common.h>

#define REC_PROG_NAME 0
#define REC_PARAM 1
#define REC_FINAL_CR 2

static gchar *glob_ip= NULL;
static gchar *glob_port= "10002";
static gchar *glob_config_file= "/etc/ic_cntrl.conf";

static GOptionEntry entries[] = 
{
  { "ip", 0, 0, G_OPTION_ARG_STRING, &glob_ip,
    "Set IP address, default is IP address of computer", NULL},
  { "port", 0, 0, G_OPTION_ARG_STRING, &glob_port, "Set Port, default = 10002", NULL},
  { "config-file", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_file,
    "Sets path to configuration file, default /etc/ic_cntrl.conf", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int start_services(struct ic_connection *conn,
                          __attribute__ ((unused)) GKeyFile *conf_file)
{
  int ret_code= 1;
  while (!conn->conn_op.accept_ic_connection(conn))
  {
    ret_code= 0;
    break;
  }
  return ret_code;
}

static int verify_conf_file(__attribute__ ((unused)) GKeyFile *conf_file)
{
  printf("Verifying configuration file\n");
  return 0;
}


int main(int argc, char *argv[])
{
  IC_CONNECTION conn;
  GError *error= NULL;
  gchar *arg_vector[4];
  gchar read_buf[256];
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  int state= REC_PROG_NAME;
  GPid barn_pid;
  GOptionContext *context;
  int i, ret_code;
 
  /* Read command options */
  context= g_option_context_new("iClaustron Control Server");
  if (!context)
    goto mem_error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);

  while (!(ret_code= ic_rec_with_cr(&conn, read_buf, &read_size,
                                    &size_curr_buf,
                                    sizeof(read_buf))))
  {
    DEBUG(ic_print_buf(read_buf, read_size));
    switch (state)
    {
      case REC_PROG_NAME: /* Receive program name */
	state= REC_PARAM;
        break;
      case REC_PARAM: /* Receive parameters */
	state= REC_FINAL_CR;
        break;
      case REC_FINAL_CR: /* Receive final CR */
	return 0;
        break;
      default:
        abort();
    }
  }
  /* for (i=0; i<=argc; i++)
     arg_vector[i]=argv[i];*/

  /*  arg_vector[0]="vi";*/
  /*arg_vector[1]="ic_pcntrl.c";*/
  arg_vector[i]=NULL;
  printf("anrop av g-spawn-async\n");
  g_spawn_async(NULL,&arg_vector[0], NULL, G_SPAWN_SEARCH_PATH,
                NULL,NULL,&barn_pid,&error);
  printf("Anrop gjort barn_pid = %d\n", barn_pid);
  ic_guint64_str((guint64)barn_pid, read_buf);
  /* printf("Kill process %s\n", buf);
  arg_vector[0]="kill";
  arg_vector[1]="-9";
  arg_vector[2]=buf;
  arg_vector[3]=NULL;
  g_spawn_async(NULL,&arg_vector[0], NULL, G_SPAWN_SEARCH_PATH,
  NULL,NULL,&barn_pid,&fel);*/
mem_error:
parse_error:
  return 0;
}
