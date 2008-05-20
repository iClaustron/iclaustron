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

/* Configurable variables */
static gchar *glob_ip= NULL;
static gchar *glob_port= "10002";
static gchar *glob_base_dir= NULL;

/* Global variables */
static guint32 glob_stop_flag= FALSE;
static IC_STRING glob_base_dir_string;
static IC_STRING glob_ic_base_dir_string;
static IC_STRING glob_mysql_base_dir_string;
GMutex *action_loop_lock= NULL;

static GOptionEntry entries[] = 
{
  { "ip", 0, 0, G_OPTION_ARG_STRING, &glob_ip,
    "Set IP address, default is IP address of computer", NULL},
  { "port", 0, 0, G_OPTION_ARG_STRING, &glob_port, "Set Port, default = 10002", NULL},
  { "basedir", 0, 0, G_OPTION_ARG_STRING, &glob_base_dir,
    "Sets path to binaries controlled by this program", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

/*
static int 
kill_process(GPid pid)
{
  gchar buf[128];
  gchar *arg_vector[4];
  GError *error; 
  ic_guint64_str((guint64)pid,buf);
  printf("Kill process %s\n", buf);
  arg_vector[0]="kill";
  arg_vector[1]="-9";
  arg_vector[2]=buf;
  arg_vector[3]=NULL;
  g_spawn_async(NULL,&arg_vector[0], NULL,
                G_SPAWN_SEARCH_PATH,
                NULL,NULL,&pid,&error);
  return 0;
}
*/

static int
send_ok_reply(IC_CONNECTION *conn)
{
  int error;
  if ((error= ic_send_with_cr(conn, "Ok")) ||
      (error= ic_send_with_cr(conn, "")))
    return error;
  return 0;
}

static gpointer
run_command_handler(gpointer data)
{
  IC_CONNECTION *conn= (IC_CONNECTION*)data;
  GError *error= NULL;
  gchar *arg_vector[4];
  gchar read_buf[256];
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  guint32 items_received= 0;
  guint32 i;
  GPid child_pid;
  gchar *arg_str;
  int ret_code;

  memset(read_buf, 0, sizeof(read_buf)); /* Zeroes in read_buf*/
  memset(arg_vector, 0, 4*sizeof(gchar*));

  while (!(ret_code= ic_rec_with_cr(conn, read_buf, &read_size,
                                    &size_curr_buf,
                                    sizeof(read_buf))))
  {
    DEBUG(COMM_LEVEL, ic_print_buf(read_buf, read_size));
    if (read_size == 0)
    {
      arg_vector[items_received]= NULL;
      break;
    }
    arg_str= ic_malloc(read_size+1); /*allocate memory, even for extra NULL*/
    if (arg_str == NULL)
      goto mem_error;
    memcpy(arg_str, read_buf, read_size);/*copy buffer to memory arg_str*/
    printf("memcpy\n");
    arg_str[read_size]= 0; /* add NULL */
    printf("Add NULL\n");
    
    arg_vector[items_received]=arg_str;
     
    if (0==strcmp("",arg_str))
    {
      send_ok_reply(conn);
      break;
    }
    items_received++;
   
  }
  for(i=0; i < items_received; i++)
    printf("arg_vector[%u] = %s\n", i, arg_vector[i]);

  printf("anrop av g-spawn-async\n");
  g_spawn_async_with_pipes(NULL,&arg_vector[0],
                           NULL, G_SPAWN_SEARCH_PATH,
                           NULL,NULL,&child_pid,
                           NULL, NULL, NULL,
                           &error);
  printf("Anrop gjort child_pid = %d\n", child_pid);
  ic_send_with_cr(conn, "Ok");
  conn->conn_op.ic_free_connection(conn);
  return NULL;
mem_error:

  for (i= 0; i < items_received; i++)
  {
    if (arg_vector[i])
      ic_free(arg_vector[i]);
  }
  conn->conn_op.ic_free_connection(conn);
  return NULL;
}

int start_connection_loop()
{
  int ret_code;
  IC_CONNECTION *conn;
  IC_CONNECTION *fork_conn;
  GError *error= NULL;

  conn->server_name= glob_ip;
  conn->server_port= glob_port;
  conn->client_name= NULL;
  conn->client_port= 0;
  conn->is_wan_connection= FALSE;
  conn->is_listen_socket_retained= TRUE;
  ret_code= conn->conn_op.ic_set_up_connection(conn);
  if (ret_code)
    return ret_code;
  do
  {
    do
    {
      /* Wait for someone to connect to us. */
      if ((ret_code= conn->conn_op.ic_accept_connection(conn)))
      {
        printf("Error %d received in accept connection\n", ret_code);
        break;
      }
      if (!(fork_conn= 
            conn->conn_op.ic_fork_accept_connection(conn,
                                        FALSE,    /* No mutex */
                                        FALSE)))  /* No front buffer */
      {
        printf("Error %d received in fork accepted connection\n", ret_code);
        break;
      }
      /*
        We have an active connection, we'll handle the connection in a
        separate thread.
      */
      if (!g_thread_create_full(run_command_handler,
                                (gpointer)fork_conn,
                                1024*32,      /* 32 kByte stack */
                                FALSE,        /* Not joinable */
                                FALSE,        /* Not bound */
                                G_THREAD_PRIORITY_NORMAL,
                                &error))
      {
        printf("Failed to create thread after forking accept connection\n");
        fork_conn->conn_op.ic_free_connection(fork_conn);
        break;
      }
    } while (1);
  } while (!glob_stop_flag);
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code= 0;
  gchar tmp_buf[IC_MAX_FILE_NAME_SIZE];
  gchar iclaustron_buf[IC_MAX_FILE_NAME_SIZE];
  gchar mysql_buf[IC_MAX_FILE_NAME_SIZE];
 
  if ((ret_code= ic_start_program(argc, argv, entries,
           "- iClaustron Control Server")))
    return ret_code;
  ret_code= 1;
  /*
    First step is to set-up path to where the binaries reside. All binaries
    we control will reside under this directory. Under this directory the
    binaries will be placed in either
    MYSQL_VERSION/bin
    or
    ICLAUSTRON_VERSION/bin
    and libraries will be in either
    MYSQL_VERSION/lib
    or
    ICLAUSTRON_VERSION/lib

    The resulting base directory is stored in the global variable
    glob_base_dir_string. We set up the default strings for the
    version we've compiled, the protocol to the process controller
    enables the Cluster Manager to specify which version to use.
  */
  if ((ret_code= ic_set_base_dir(&glob_base_dir_string, glob_base_dir)))
    goto error;
  ic_make_iclaustron_version_string(&glob_ic_base_dir_string, tmp_buf);
  ic_set_relative_dir(&glob_ic_base_dir_string, &glob_base_dir_string,
                      iclaustron_buf, glob_ic_base_dir_string.str);
  ic_make_mysql_version_string(&glob_mysql_base_dir_string, tmp_buf);
  ic_set_relative_dir(&glob_mysql_base_dir_string, &glob_base_dir_string,
                      mysql_buf, glob_mysql_base_dir_string.str);
  DEBUG_PRINT(PROGRAM_LEVEL, ("Base directory: %s",
                              glob_base_dir_string.str));
  /*
    Next step is to wait for Cluster Servers to connect to us, after they
    have connected they can request action from us as well. Any server can
    connect us a Cluster Server, but they have to provide the proper
    password in order to get connected.

    We will always be ready to receive new connections.

    The manner to stop this program is by performing a kill -15 operation
    such that the program receives a SIGKILL signal. The program cannot
    be started and stopped from a Cluster Server for security reasons.
  */
  ret_code= start_connection_loop();
error:
  ic_free(glob_base_dir_string.str);
  return ret_code;
}
