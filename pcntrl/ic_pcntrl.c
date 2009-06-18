/* Copyright (C) 2007-2009 iClaustron AB

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
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_protocol_support.h>
#include <ic_port.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_threadpool.h>
#include <ic_hashtable.h>
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


#define REC_PROG_NAME 0
#define REC_PARAM 1
#define REC_FINAL_CR 2

/* Protocol strings */
static const gchar *start_str= "start";
static const gchar *stop_str= "stop";
static const gchar *kill_str= "kill";
static const gchar *list_str= "list";
static const gchar *list_full_str= "list full";
static const gchar *check_status_str= "check status";
static const gchar *auto_restart_true_str= "autorestart: true";
static const gchar *auto_restart_false_str= "autorestart: false";
static const gchar *num_parameters_str= "num parameters: ";

/* Configurable variables */
static gchar *glob_ip= NULL;
static gchar *glob_port= "10002";
static gchar *glob_base_dir= NULL;

/* Global variables */
static const gchar *glob_process_name= "ic_pcntrld";
static guint32 glob_stop_flag= FALSE;
static IC_STRING glob_base_dir_string;
static IC_STRING glob_ic_base_dir_string;
static IC_STRING glob_mysql_base_dir_string;
GMutex *action_loop_lock= NULL;
GMutex *pc_hash_mutex= NULL;
IC_HASHTABLE *glob_pc_hash= NULL;
static IC_THREADPOOL_STATE *glob_tp_state;

static GOptionEntry entries[] = 
{
  { "ip", 0, 0, G_OPTION_ARG_STRING, &glob_ip,
    "Set IP address, default is IP address of computer", NULL},
  { "port", 0, 0, G_OPTION_ARG_STRING, &glob_port, "Set Port, default = 10002", NULL},
  { "basedir", 0, 0, G_OPTION_ARG_STRING, &glob_base_dir,
    "Sets path to binaries controlled by this program", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int 
kill_process(GPid pid, gboolean hard_kill)
{
  gchar buf[128];
  gchar *arg_vector[4];
  GError *error;
  int i= 0;
  guint32 len;

  ic_guint64_str((guint64)pid,buf, &len);
  DEBUG_PRINT(CONFIG_LEVEL, ("Kill process %s\n", buf));
  arg_vector[i++]="kill";
  if (hard_kill)
    arg_vector[i++]="-9";
  arg_vector[i++]=buf;
  arg_vector[i]=NULL;
  g_spawn_async(NULL,&arg_vector[0], NULL,
                G_SPAWN_SEARCH_PATH,
                NULL,NULL,&pid,&error);
  return 0;
}

static int
send_error_reply(IC_CONNECTION *conn, const gchar *error_message)
{
  int error;
  if ((error= ic_send_with_cr(conn, "error")) ||
      (error= ic_send_with_cr(conn, error_message)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_ok_reply(IC_CONNECTION *conn)
{
  int error;
  if ((error= ic_send_with_cr(conn, "ok")) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_ok_pid_reply(IC_CONNECTION *conn, GPid pid)
{
  int error;

  if ((error= ic_send_with_cr(conn, "ok")) ||
      (error= ic_send_pid(conn, pid)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_no_reply(IC_CONNECTION *conn)
{
  int error;
  if ((error= ic_send_with_cr(conn, "no")) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_list_stop_reply(IC_CONNECTION *conn)
{
  int error;
  if ((error= ic_send_with_cr(conn, "list stop")) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_node_stopped(IC_CONNECTION *conn,
                  const gchar *grid_name,
                  const gchar *cluster_name,
                  const gchar *node_name,
                  const gchar *program_name,
                  const gchar *version_string)
{
  int error;

  if ((error= ic_send_with_cr(conn, "node stopped")) ||
      (error= ic_send_key(conn, grid_name, cluster_name, node_name)) ||
      (error= ic_send_program(conn, program_name)) ||
      (error= ic_send_version(conn, version_string)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
get_optional_str(IC_CONNECTION *conn,
                 IC_MEMORY_CONTAINER *mc_ptr,
                 IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *str_ptr;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == 0)
    return 0;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(str_ptr, read_buf, read_size);
  IC_INIT_STRING(str, str_ptr, read_size, TRUE);
}

static int
get_str(IC_CONNECTION *conn,
        IC_MEMORY_CONTAINER *mc_ptr,
        IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *str_ptr;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == 0)
    return IC_PROTOCOL_ERROR;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(str_ptr, read_buf, read_size);
  IC_INIT_STRING(str, str_ptr, read_size, TRUE);
}

static int
get_key(IC_CONNECTION *conn,
        IC_MEMORY_CONTAINER *mc_ptr,
        IC_PC_KEY *pc_key)
{
  int error;

  if ((error= get_str(conn, mc_ptr, &pc_key->grid_name)) ||
      (error= get_str(conn, mc_ptr, &pc_key->cluster_name)) ||
      (error= get_str(conn, mc_ptr, &pc_key->node_name)))
    return error;
  return 0;
}

static int
get_autorestart(IC_CONNECTION *conn,
                gboolean *auto_restart)
{
  int error;
  guint32 read_size;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == strlen(auto_restart_true_str) &&
      !memcmp(read_buf, auto_restart_true_str, read_size))
  {
    *auto_restart= 1;
    return 0;
  }
  else if (read_size == strlen(auto_restart_false_str) &&
           !memcmp(read_buf, auto_restart_false_str, read_size))
  {
    *auto_restart= 0;
    return 0;
  }
  else
    return IC_PROTOCOL_ERROR;
}

static int
get_num_parameters(IC_CONNECTION *conn,
                   guint32 *num_parameters)
{
  int error;
  guint32 read_size;
  guint64 loc_num_params;
  guint32 num_param_len= strlen(num_parameters_str);
  guint32 len;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (memcmp(read_buf, num_parameters_str, read_size))
    return IC_PROTOCOL_ERROR;
  if ((read_size <= num_param_len) ||
      (ic_conv_str_to_int(read_buf+num_param_len, &loc_num_params, &len)) ||
      (loc_num_params >= IC_MAX_UINT32))
    return IC_PROTOCOL_ERROR;
  *num_parameters= (guint32)loc_num_params;
  return 0;
}

static int
rec_start_message(IC_CONNECTION *conn,
                  IC_PC_START **pc_start)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_PC_START *loc_pc_start;
  guint32 i;
  int error;

  /*
    When we come here we have already received the start string, we now
    receive the rest of the start package.
    We put the data into an IC_PC_START struct which is self-contained
    and also contains a memory container for all the memory allocated.
  */
  if ((mc_ptr= ic_create_memory_container((guint32)1024, (guint32) 0)))
    return IC_ERROR_MEM_ALLOC;
  if ((*pc_start= (IC_PC_START*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_PC_START))))
    goto mem_error;
  loc_pc_start= *pc_start;
  loc_pc_start->mc_ptr= mc_ptr;

  if ((error= get_str(conn, mc_ptr, &loc_pc_start->version_string)) ||
      (error= get_key(conn, mc_ptr, &loc_pc_start->key)) ||
      (error= get_str(conn, mc_ptr, &loc_pc_start->program_name)) ||
      (error= get_autorestart(conn, &loc_pc_start->autorestart)) ||
      (error= get_num_parameters(conn, &loc_pc_start->num_parameters)))
    goto error;
  if ((loc_pc_start->parameters= (IC_STRING*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_STRING)*
         loc_pc_start->num_parameters)))
    goto mem_error;
  for (i= 0; i < loc_pc_start->num_parameters; i++)
  {
    if ((error= get_str(conn, mc_ptr, &loc_pc_start->parameters[i])))
      goto error;
  }
  if ((error= ic_rec_simple_str(conn, ic_empty_string)))
    goto error;
  return 0;
mem_error:
  error= IC_ERROR_MEM_ALLOC;
error:
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return error;
}

static int
rec_stop_message(IC_CONNECTION *conn,
                 IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  int error;

  /*
    When we come here we already received the stop/kill string and we only
    need to fill in the key parameters (grid, cluster and node name).
  */
  if ((mc_ptr= ic_create_memory_container((guint32)1024, (guint32) 0)))
    return IC_ERROR_MEM_ALLOC;
  if ((*pc_find= (IC_PC_FIND*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_PC_FIND))))
    goto mem_error;
  if ((error= get_key(conn, mc_ptr, &(*pc_find)->key)))
    goto error;
  if ((error= ic_rec_simple_str(conn, ic_empty_string)))
    goto error;
  (*pc_find)->mc_ptr= mc_ptr;
  return 0;
mem_error:
  error= IC_ERROR_MEM_ALLOC;
error:
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return error;
}

static int
init_pc_find(IC_MEMORY_CONTAINER **mc_ptr,
             IC_PC_FIND **pc_find)
{
  if (((*mc_ptr)= ic_create_memory_container((guint32)1024, (guint32) 0)))
    return IC_ERROR_MEM_ALLOC;
  if (((*pc_find)= (IC_PC_FIND*)
       (*mc_ptr)->mc_ops.ic_mc_calloc((*mc_ptr), sizeof(IC_PC_FIND))))
    return IC_ERROR_MEM_ALLOC;
  (*pc_find)->mc_ptr= (*mc_ptr);
}

static int
rec_optional_key_message(IC_CONNECTION *conn,
                         IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  int error;

  /*
    When we come here we already received the list [full] string and we
    only need to fill in key parameters. All key parameters are optional
    here.
  */
  if ((error= init_pc_find(&mc_ptr, pc_find)))
    goto error;
  if ((error= get_optional_str(conn, mc_ptr, &(*pc_find)->key.grid_name)))
    goto error;
  if ((*pc_find)->key.grid_name.len == 0)
    return 0; /* No grid name provided, list all programs */
  if ((error= get_optional_str(conn, mc_ptr, &(*pc_find)->key.cluster_name)))
    goto error;
  if ((*pc_find)->key.cluster_name.len == 0)
    return 0; /* No cluster name provided, list all programs in grid */
  if ((error= get_optional_str(conn, mc_ptr, &(*pc_find)->key.node_name)))
    goto error;
  if ((*pc_find)->key.node_name.len == 0)
    return 0; /* No node name provided, list all programs in cluster */
  if ((error= ic_rec_simple_str(conn, ic_empty_string)))
    goto error;
  return 0;
error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return error;
}

static unsigned int
ic_pc_hash_key(void *ptr)
{
  IC_PC_KEY *pc_key= ptr;
  IC_STRING *str;
  unsigned int hash1, hash2, hash3;

  str= &pc_key->grid_name;
  hash1= ic_hash_str((void*)str);
  str= &pc_key->cluster_name;
  hash2= ic_hash_str((void*)str);
  str= &pc_key->node_name;
  hash3= ic_hash_str((void*)str);
  return (hash1 ^ hash2 ^ hash3);
}

static int
ic_pc_key_equal(void *ptr1, void *ptr2)
{
  IC_PC_KEY *pc_key1= ptr1;
  IC_PC_KEY *pc_key2= ptr2;

  if ((ic_cmp_str(&pc_key1->grid_name, &pc_key2->grid_name)) ||
      (ic_cmp_str(&pc_key1->cluster_name, &pc_key2->cluster_name)) ||
      (ic_cmp_str(&pc_key1->node_name, &pc_key2->node_name)))
    return 1;
  return 0;
}

IC_HASHTABLE*
create_pc_hash()
{
  return ic_create_hashtable(4096, ic_pc_hash_key, ic_pc_key_equal);
}

static int
handle_start(IC_CONNECTION *conn)
{
  gchar **arg_vector;
  int ret_code;
  guint32 i;
  GError *error= NULL;
  IC_STRING working_dir;
  IC_PC_START *pc_start;

  IC_INIT_STRING(&working_dir, NULL, 0, FALSE);
  if ((ret_code= rec_start_message(conn, &pc_start)))
    return ret_code;
  if ((arg_vector= (gchar**)pc_start->mc_ptr->mc_ops.ic_mc_calloc(
                   pc_start->mc_ptr,
                   (pc_start->num_parameters+2) * sizeof(gchar*))))
    goto mem_error;
  arg_vector[0]= pc_start->program_name.str;
  for (i= 0; i < pc_start->num_parameters; i++)
  {
    arg_vector[i+1]= pc_start->parameters[i].str;
  }
  if ((ret_code= ic_set_base_path(&working_dir,
                                  glob_base_dir,
                                  pc_start->version_string.str,
                                  NULL)))
    goto error;
  if ((ret_code= start_process(&arg_vector[0],
                               working_dir.str,
                               &pc_start->pid)))
    goto error;
  if (working_dir.str)
    ic_free(working_dir.str);
  g_mutex_lock(pc_hash_mutex);
  ret_code= ic_hashtable_insert(glob_pc_hash,
                                (void*)pc_start, (void*)pc_start);
  g_mutex_unlock(pc_hash_mutex);
  /* Insert into hash keeping track of programs */
  return 0;
mem_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  if (working_dir.str)
    ic_free(working_dir.str);
  pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
  return ret_code;
}

static int
handle_stop(IC_CONNECTION *conn, gboolean kill_flag)
{
  return 0;
}

static int
handle_list(IC_CONNECTION *conn, gboolean list_full_flag)
{
  return 0;
}

static int
handle_check_status(IC_CONNECTION *conn)
{
  return 0;
}

/*
  The command handler is a thread that is executed on behalf of one of the
  Cluster Managers in a Grid.

  The Cluster Manager can ask the iClaustron Process Controller for 4 tasks.
  1) It can ask to get a node started
  2) It can ask to stop a node gracefully (kill)
  3) It can ask to stop a node in a hard manner (kill -9)
  4) It can also ask for a list of the programs currently running in the Grid

  GENERIC PROTOCOL
  ----------------
  The protocol between the iClaustron Process Controller and the Cluster
  Manager is entirely half-duplex and it is always the Cluster Manager
  asking for services from the Process Controller. The only action the
  Process Controller performs automatically is to restart a program if
  it discovers it has died when the autorestart flag is set to true in
  the start command.

  START PROTOCOL
  --------------
  The protocol for starting node is:
  Line 1: start
  Line 2: Version string (either an iClaustron version or a MySQL version string)
  Line 3: Grid Name
  Line 4: Cluster Name
  Line 5: Node name
  Line 6: Program Name
  Line 7: autorestart true OR autorestart false
  Line 8: Number of parameters
  Line 9 - Line x: Program Parameters on the format:
    Parameter name, 1 space, Type of parameter, 1 space, Parameter Value
  Line x+1: Empty line to indicate end of message

  Example: start\nversion: iclaustron-0.0.1\ngrid: my_grid\n
           cluster: my_cluster\nnode: my_node\nprogram: ic_fsd\n
           autorestart: true\nnum parameters: 2\n
           parameter: data_dir string /home/mikael/iclaustron\n\n

  The successful response is:
  Line 1: ok
  Line 2: Process Id
  Line 3: Empty line

  Example: ok\npid: 1234\n\n

  The unsuccessful response is:
  Line 1: error
  Line 2: message

  Example: error:\nerror: Failed to start process\n\n

  STOP/KILL PROTOCOL
  ------------------
  The protocol for stopping/killing a node is:
  Line 1: stop/kill
  Line 2: Grid Name
  Line 3: Cluster Name
  Line 4: Node name
  Line 5: Empty line

  The successful response is:
  Line 1: ok
  Line 2: Empty Line

  The unsuccessful response is:
  Line 1: error
  Line 2: message

  LIST PROTOCOL
  -------------
  The protocol for listing nodes is:
  Line 1: list [full]
  Line 2: Grid Name (optional)
  Line 3: Cluster Name (optional)
  Line 4: Node Name (optional)
  Line 5: Empty Line

  Only Grid Name is required, if Cluster Name is provided, only programs in this
  cluster will be provided, if Node Name is also provided then only a program
  running this node will be provided.

  The response is a set of program descriptions as:
  NOTE: It is very likely this list will be expanded with more data on the
  state of the process, such as use of CPU, use of memory, disk and so forth.
  Line 1: list node
  Line 2: Grid Name
  Line 3: Cluster Name
  Line 4: Node Name
  Line 5: Program Name
  Line 6: Version string
  Line 7: Start Time
  Line 8: Process Id
  Line 9: Number of parameters
  Line 10 - Line x: Parameter in same format as when starting (only if list full)
  The protocol to check status is:
  Line x+1: Empty Line

  After receiving one such line the Cluster Manager responds with either
  list next or list stop to stop the listing as:
  Line 1: list next
  Line 2: Empty Line
  or
  Line 1: list stop
  Line 2: Empty Line

  After list next a new node will be sent, when there are no more nodes to
  send the Process Controller will respond with:
  Line 1: list stop
  Line 2: Empty Line

  CHECK STATUS PROTOCOL
  ---------------------
  The Cluster Manager can also ask the Process Controller if any events of
  interest has occurred since last time the Cluster Manager contacted the
  Process Controller. At this moment the only interesting event to report
  is a program that has stopped.

  The protocol is:
  Line 1: check status
  Line 2: Grid Name (optional)
  Line 3: Cluster Name (optional)
  Line 4: Node Name (optional)
  Line 5: Empty Line

  The response is:
  Line 1: no
  Line 2: Empty Line
  when no new events have occurred

  Line 1: node stopped
  Line 2: Grid Name
  Line 3: Cluster Name
  Line 4: Node Name
  Line 5: Program Name
  Line 6: Version string
  Line 7: Empty Line
*/

static gpointer
run_command_handler(gpointer data)
{
  gchar *read_buf;
  guint32 read_size;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_CONNECTION *conn= (IC_CONNECTION*)
    thread_state->ts_ops.ic_thread_get_object(thread_state);
  guint32 i;
  int ret_code;

  thread_state->ts_ops.ic_thread_started(thread_state);
  thread_state= conn->conn_op.ic_get_param(conn);

  while (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (ic_check_buf(read_buf, read_size, start_str,
                     strlen(start_str)))
    {
      if ((ret_code= handle_start(conn)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, stop_str,
                          strlen(stop_str)))
    {
      if ((ret_code= handle_stop(conn, FALSE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, kill_str,
                          strlen(kill_str)))
    {
      if ((ret_code= handle_stop(conn, TRUE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, list_str,
                          strlen(list_str)))
    {
      if ((ret_code= handle_list(conn, FALSE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, check_status_str,
                          strlen(check_status_str)))
    {
      if ((ret_code= handle_list(conn, TRUE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, list_full_str,
                          strlen(list_full_str)))
    {
      if ((ret_code= handle_check_status(conn)))
        break;
    }
    else if (read_size)
    {
      ret_code= 0;
      break;
    }
  }
  conn->conn_op.ic_free_connection(conn);
  thread_state->ts_ops.ic_thread_stops(thread_state);
  return NULL;
}

int start_connection_loop()
{
  int ret_code;
  guint32 thread_id;
  IC_CONNECTION *conn= NULL;
  IC_CONNECTION *fork_conn;

  conn->conn_op.ic_prepare_server_connection(conn,
                                             glob_ip,
                                             glob_port,
                                             NULL,
                                             NULL,
                                             0,
                                             TRUE);
  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);
  if (ret_code)
    return ret_code;
  do
  {
    do
    {
      glob_tp_state->tp_ops.ic_threadpool_check_threads(glob_tp_state);
      /* Wait for someone to connect to us. */
      if ((ret_code= conn->conn_op.ic_accept_connection(conn, NULL, NULL)))
      {
        printf("Error %d received in accept connection\n", ret_code);
        break;
      }
      if (!(fork_conn= 
            conn->conn_op.ic_fork_accept_connection(conn,
                                        FALSE)))  /* No mutex */
      {
        printf("Error %d received in fork accepted connection\n", ret_code);
        break;
      }
      /*
        We have an active connection, we'll handle the connection in a
        separate thread.
      */
      if (glob_tp_state->tp_ops.ic_threadpool_start_thread(
                 glob_tp_state,
                 &thread_id,
                 run_command_handler,
                 fork_conn,
                 IC_SMALL_STACK_SIZE,
                 FALSE))
      {
        printf("Failed to create thread after forking accept connection\n");
        fork_conn->conn_op.ic_free_connection(fork_conn);
        break;
      }
    } while (0);
  } while (!glob_stop_flag);
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code= 0;
  gchar tmp_buf[IC_MAX_FILE_NAME_SIZE];
  gchar iclaustron_buf[IC_MAX_FILE_NAME_SIZE];
  gchar mysql_buf[IC_MAX_FILE_NAME_SIZE];
 
  if ((ret_code= ic_start_program(argc, argv, entries, glob_process_name,
           "- iClaustron Control Server")))
    return ret_code;
  if (!(glob_tp_state=
        ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE)))
    return IC_ERROR_MEM_ALLOC;
  if (!(glob_pc_hash= create_pc_hash()))
    goto error;
  if (!(pc_hash_mutex= g_mutex_new()))
    goto error;
  /*
    First step is to set-up path to where the binaries reside. All binaries
    we control will reside under this directory. Under this directory the
    binaries will be placed in either
    MYSQL_VERSION/bin
    or
    ICLAUSTRON_VERSION/bin
    and libraries will be in either
    MYSQL_VERSION/lib/mysql
    or
    ICLAUSTRON_VERSION/lib

    The default directory is $HOME/iclaustron_install if we're not running this
    as root, if we're running as root, then the default directory is
    /var/lib/iclaustron_install.

    The resulting base directory is stored in the global variable
    glob_base_dir_string. We set up the default strings for the
    version we've compiled, the protocol to the process controller
    enables the Cluster Manager to specify which version to use.

    This program will have it's state in memory but will also checkpoint this
    state to a file. This is to ensure that we are able to connect to processes
    again even after we had to restart this program. This file is stored in
    the iclaustron_data placed beside the iclaustron_install directory. The file
    is called pcntrl_state.
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
    Next step is to wait for Cluster Managers to connect to us, after they
    have connected they can request action from us as well. Any server can
    connect as a Cluster Manager, but they have to provide the proper
    password in order to get connected.

    We will always be ready to receive new connections.

    The manner to stop this program is by performing a kill -15 operation
    such that the program receives a SIGKILL signal. This program cannot
    be started and stopped from a Cluster Manager for security reasons.
  */
  ret_code= start_connection_loop();
error:
  glob_tp_state->tp_ops.ic_threadpool_stop(glob_tp_state);
  if (glob_pc_hash)
    ic_hashtable_destroy(glob_pc_hash);
  if (pc_hash_mutex)
    g_mutex_free(pc_hash_mutex);
  if (glob_base_dir_string.str)
    ic_free(glob_base_dir_string.str);
  return ret_code;
}
