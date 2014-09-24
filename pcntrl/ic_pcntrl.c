/* Copyright (C) 2007, 2014 iClaustron AB

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
#include <ic_port.h>
#include <ic_string.h>
#include <ic_hw_info.h>
#include <ic_connection.h>
#include <ic_threadpool.h>
#include <ic_hashtable.h>
#include <ic_hashtable_itr.h>
#include <ic_dyn_array.h>
#include <ic_protocol_support.h>
#include <ic_proto_str.h>
#include <ic_pcntrl_proto.h>
#include <ic_apic.h>
#include <ic_apid.h>

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

/*
  TODO: Need to extend basic tests for the functions that can be used in this
        program.
        1) Use error inject to test errors in all sorts of places
*/

#define REC_PROG_NAME 0
#define REC_PARAM 1
#define REC_FINAL_CR 2

/* Configurable variables */
static gchar *glob_server_name= "127.0.0.1";
static gchar *glob_server_port= IC_DEF_PCNTRL_PORT_STR;
static guint32 glob_daemonize= 1;
static gboolean volatile event_occurred= FALSE;
#ifdef WITH_UNIT_TEST
static guint32 glob_unit_test= 0;
#endif

/* Global variables */
static IC_MUTEX *pc_hash_mutex= NULL;
static IC_HASHTABLE *glob_pc_hash= NULL;
static guint64 glob_start_id= 1;
static IC_DYNAMIC_PTR_ARRAY *glob_pc_array= NULL;
static gboolean shutdown_processes_flag= FALSE;

#ifdef WITH_UNIT_TEST
static gchar *version_str= IC_VERSION_STR;
static gchar *not_my_grid= "not_my_grid";
static gchar *my_grid= "my_grid";
static gchar *my_cluster= "my_cluster";
static gchar *my_csd_node= "my_csd_node";
#endif

/**
  Parameters used to start ic_pcntrld program
*/
static GOptionEntry entries[] = 
{
  { "server-name", 0, 0, G_OPTION_ARG_STRING,
    &glob_server_name,
    "Set server address of process controller", NULL},
  { "server-port", 0, 0, G_OPTION_ARG_STRING,
    &glob_server_port,
    "Set server port, default = 11860", NULL},
  { "iclaustron-version", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_version_path,
    "Version string to find iClaustron binaries used by this program, default "
    IC_VERSION_STR, NULL},
  { "daemonize", 0, 0, G_OPTION_ARG_INT,
     &glob_daemonize,
    "Daemonize program", NULL},
#ifdef WITH_UNIT_TEST
  { "unit-test", 0, 0, G_OPTION_ARG_INT,
     &glob_unit_test,
    "Run unit test", NULL},
#endif
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static gchar *start_text= "\
- iClaustron Process Controller program\n\
\n\
This program is the agent that is needed to start and stop other programs,\n\
to get access to low-level information about hardware and operating system.\n\
\n\
It is used by the bootstrap program to copy configuration files to all\n\
nodes in the iClaustron Grid. It is also used by the bootstrap to perform\n\
an initial start of the Cluster Servers and Cluster Managers.\n\
\n\
It is used by the configurator to get access to information about hardware\n\
and operating system for the configurator to be able to assist the user in\n\
making a proper configuration.\n\
\n\
Since it is the program that is used to install configuration files, some\n\
of which contain login information, it is required that this program is\n\
executed with proper privileges. All files it writes will only be readable\n\
and writeable by the user that runs this program.\n\
\n\
Since it is the program that starts other programs, it requires sufficient\n\
privileges such that these programs can execute their actions. One such\n\
important requirement is the privilege to lock programs and threads to\n\
to certain CPU processors.\n\
\n\
iClaustron Installation Process\n\
-------------------------------\n\
1)\n\
A standard installation of iClaustron would start by creating an iclaustron\n\
user on all machines/VMs to be used by iClaustron.\n\
\n\
2)\n\
Next step would be to install the iClaustron software at\n\
$ICLAUSTRON_USER_HOME/iclaustron_install/ICLAUSTRON_VERSION_NAME\n\
in all machines/VMs.\n\
\n\
3)\n\
Third step is to start this program (ic_pcntrld) in all machines/VMs.\n\
It is a good idea to make sure this program is also restarted when the\n\
OS starts up or if this program should fail.\n\
\n\
4)\n\
The fourth step is to run the iClaustron configurator. This program\n\
requires at least a list of hostname/port pairs for all machines/VMs\n\
to be used (default port for the process controller is 11860).\n\
In addition to this information it really only needs some policy\n\
information about how much of the resources available it can use.\n\
The simplest case is of course when all resources on all machines/VMs\n\
are dedicated to iClaustron usage.\n\
\n\
5)\n\
When the configurator has created all configuration files, the fifth step\n\
is to run the bootstrap program to make sure the configuration files are\n\
all installed in their proper place and that the Cluster Servers and\n\
Cluster Managers are started.\n\
\n\
6)\n\
After these 5 steps the iClaustron is ready to start the actual data\n\
servers, file servers, replication and other servers configured in the\n\
grid. This can be done by starting the iClaustron client (ic_cclient)\n\
and execute the start program from there whereafter the iClaustron\n\
programs will cooperate to start the grid.\n\
\n\
7)\n\
After these 6 steps all installation steps are processed and one can start\n\
using the iClaustron Grid for clustered file services or any other use it\n\
is intended for.\n\
\n\
iClaustron Upgrade Process\n\
--------------------------\n\
An upgrade to a new version of the iClaustron software is easy. Simply\n\
install the new iClaustron software at\n\
$ICLAUSTRON_USER_HOME/NEW_ICLAUSTRON_VERSION_NAME\n\
after which normal upgrade commands can be used to restart all iClaustron\n\
programs in the grid.\n\
\n\
iClaustron Add Node Process\n\
---------------------------\n\
More on this later\n\
";

/* Unit test functions */
#ifdef WITH_UNIT_TEST
/**
  Start a new client socket connection

  @parameter conn         OUT: The new connection created
*/
static int start_client_connection(IC_CONNECTION **conn)
{
  int ret_code;
  DEBUG_ENTRY("start_client_connection");

  if (!(*conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                      CONFIG_READ_BUF_SIZE,
                                      NULL, NULL)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  (*conn)->conn_op.ic_prepare_client_connection(*conn,
                                                glob_server_name,
                                                glob_server_port,
                                                NULL, NULL);
  ret_code= (*conn)->conn_op.ic_set_up_connection(*conn, NULL, NULL);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Send a start cluster server message protocol

  @parameter conn           The connection
*/
static int
send_start_cluster_server(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_start_cluster_server");

  /* Start a cluster server */
  if ((ret_code= ic_send_with_cr(conn, ic_start_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_program_str,
                                             ic_cluster_server_program_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_version_str,
                                             version_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_grid_str,
                                             my_grid)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_cluster_str,
                                             my_cluster)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_node_str,
                                             my_csd_node)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_auto_restart_str,
                                             ic_false_str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_num_parameters_str,
                                             (guint64)2)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_node_id_str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_parameter_str,
                                             (guint64)1)) ||
      (ret_code= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

static int
receive_error_message(IC_CONNECTION *conn)
{
  int ret_code;
  gchar print_buf[ERROR_MESSAGE_SIZE];
  DEBUG_ENTRY("receive_error_message");

  if ((ret_code= ic_receive_error_message(conn, print_buf)))
    goto error;
  ic_printf("Error message: %s from start ic_csd", print_buf);
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Test starting an ic_csd process

  @parameter conn        IN: The connection to the process controller
*/
static int test_successful_start(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 pid;
  gboolean found= FALSE;
  DEBUG_ENTRY("test_successful_start");

  ic_printf("Testing successful start of ic_csd");
  if ((ret_code= send_start_cluster_server(conn)))
    goto error;
  if ((ret_code= ic_rec_simple_str_opt(conn, ic_ok_str, &found)))
    goto error;
  if (!found)
  {
    /* Not ok, expect error message instead */
    if ((ret_code= receive_error_message(conn)))
      goto error;
    DEBUG_RETURN_INT(1);
  }
  if ((ret_code= ic_rec_number(conn, ic_pid_str, &pid)) ||
      (ret_code= ic_rec_empty_line(conn)))
    goto error;
  ic_printf("Successfully started ic_csd with pid %u", pid);
  DEBUG_RETURN_INT(0);
error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Test starting an ic_csd process which is already started, this function
  should always return with an appropriate message.

  @parameter conn        IN: The connection to the process controller
*/
static int test_unsuccessful_start(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 pid;
  gboolean found= FALSE;
  DEBUG_ENTRY("test_unsuccessful_start");

  ic_printf("Testing unsuccessful start of ic_csd");
  if ((ret_code= send_start_cluster_server(conn)))
    goto error;
  if ((ret_code= ic_rec_simple_str_opt(conn, ic_ok_str, &found)))
    goto error;
  if (!found)
  {
    /* Not ok, expect error message instead */
    if ((ret_code= receive_error_message(conn)))
      goto error;
    DEBUG_RETURN_INT(1);
  }
  if ((ret_code= ic_rec_number(conn, ic_pid_str, &pid)) ||
      (ret_code= ic_rec_simple_str(conn, ic_process_already_started_str)) ||
      (ret_code= ic_rec_empty_line(conn)))
    goto error;
  ic_printf("Process already started, pid %u", pid);
  DEBUG_PRINT(PROGRAM_LEVEL, ("Process already started, pid = %u", pid));
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Send a message to stop a Cluster Server

  @parameter conn              The connection
*/
static int send_stop_cluster_server(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_stop_cluster_server");

  /* Stop the Cluster Server */
  if ((ret_code= ic_send_stop_node(conn,
                                   my_grid,
                                   my_cluster,
                                   my_csd_node)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Test stopping the previously successfully started ic_csd process
  This function should be successful also when stopping an already
  stopped process. Thus can be used also to test unsuccessful stop.

  @parameter conn        IN: The connection to the process controller
*/
static int test_successful_stop(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("test_successful_stop");

  ic_printf("Testing successful stop of ic_csd");
  if ((ret_code= send_stop_cluster_server(conn)) ||
      (ret_code= ic_rec_simple_str(conn, ic_ok_str)) ||
      (ret_code= ic_rec_empty_line(conn)))
    goto error;
  ic_printf("Successful stop of ic_csd");
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Send a protocol line containing:
  list full<CR><CR>

  @parameter conn              IN:  The connection from the client
*/
static int
send_list_full(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_list_full");

  if ((ret_code= ic_send_list_node(conn,
                                   NULL,
                                   NULL,
                                   NULL,
                                   TRUE)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Send a command to list all nodes within my grid

  @parameter conn              IN:  The connection from the client
*/
static int
send_list_grid(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_list_grid");

  if ((ret_code= ic_send_list_node(conn,
                                   my_grid,
                                   NULL,
                                   NULL,
                                   FALSE)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Send a command to list all nodes within a grid where no nodes exist.

  @parameter conn              IN:  The connection from the client
*/
static int
send_list_unsuccessful_grid(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_list_unsuccessful_grid");

  if ((ret_code= ic_send_list_node(conn,
                                   not_my_grid,
                                   NULL,
                                   NULL,
                                   FALSE)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Send a command to list all nodes within my cluster

  @parameter conn              IN:  The connection from the client
*/
static int
send_list_cluster(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_list_cluster");

  if ((ret_code= ic_send_list_node(conn,
                                   my_grid,
                                   my_cluster,
                                   NULL,
                                   FALSE)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Send a command to list my node within my cluster

  @parameter conn              IN:  The connection from the client
*/
static int
send_list_node(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("send_list_node");

  if ((ret_code= ic_send_list_node(conn,
                                   my_grid,
                                   my_cluster,
                                   my_csd_node,
                                   FALSE)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Receive CPU info reply

  @parameter conn              IN:  The connection from the client
*/
static int
rec_cpu_info_reply(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 num_processors, num_cores, num_numa_nodes, num_sockets;
  gboolean found= FALSE;
  guint32 i;
  guint32 processor_id, core_id, numa_node_id, socket_id;
  DEBUG_ENTRY("rec_cpu_info_reply");

  if ((ret_code= ic_rec_simple_str_opt(conn,
                                       ic_no_cpu_info_available_str,
                                       &found)))
    goto error;
  if (!found)
  {
    if ((ret_code= ic_rec_number(conn,
                                 ic_number_of_processors_str,
                                 &num_processors)) ||
        (ret_code= ic_rec_number(conn,
                                 ic_number_of_cpu_sockets_str,
                                 &num_sockets)) ||
        (ret_code= ic_rec_number(conn,
                                 ic_number_of_cpu_cores_str,
                                 &num_cores)) ||
        (ret_code= ic_rec_number(conn,
                                 ic_number_of_numa_nodes_str,
                                 &num_numa_nodes)))
      goto error;
    ic_printf(
    "Num processors: %u, Num Sockets: %u, Num NUMA nodes: %u, Num Cores: %u",
      num_processors, num_sockets, num_numa_nodes, num_cores);
    for (i= 0; i < num_processors; i++)
    {
      if ((ret_code= ic_rec_number(conn, ic_processor_str, &processor_id)) ||
          (ret_code= ic_rec_number(conn, ic_core_str, &core_id)) ||
          (ret_code= ic_rec_number(conn, ic_cpu_node_str, &numa_node_id)) ||
          (ret_code= ic_rec_number(conn, ic_socket_str, &socket_id)))
        goto error;
      ic_printf(
        "Processor ID: %u, Core ID: %u, Numa node: %u, Socket ID: %u",
        processor_id, core_id, numa_node_id, socket_id);
    }
  }
  else
  {
    ic_printf("No CPU info available");
  }
  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;
  DEBUG_RETURN_INT(0);
error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive memory info reply

  @parameter conn              IN:  The connection from the client
*/
static int
rec_mem_info_reply(IC_CONNECTION *conn)
{
  int ret_code;
  gboolean found= FALSE;
  guint32 total_memory_size= 0;
  guint32 num_numa_nodes= 0;
  guint32 numa_node_id= 0;
  guint32 memory_size= 0;
  guint32 i;
  DEBUG_ENTRY("rec_mem_info_reply");

  if ((ret_code= ic_rec_simple_str_opt(conn,
                                       ic_no_mem_info_available_str,
                                       &found)))
    goto error;

  if (!found)
  {
    if ((ret_code= ic_rec_number(conn,
                                 ic_number_of_mbyte_user_memory_str,
                                 &total_memory_size)) ||
        (ret_code= ic_rec_number(conn,
                                 ic_number_of_numa_nodes_str,
                                 &num_numa_nodes)))
      goto error;
    ic_printf("Total memory size is %u MBytes, Num NUMA nodes: %u",
              total_memory_size, num_numa_nodes);
    for (i= 0; i < num_numa_nodes; i++)
    {
      if ((ret_code= ic_rec_number(conn, ic_mem_node_str, &numa_node_id)) ||
          (ret_code= ic_rec_number(conn, ic_mb_user_memory_str, &memory_size)))
        goto error;
      ic_printf("NUMA node id: %u has %u MBytes of memory",
                numa_node_id, memory_size);
    }
  }
  else
  {
    ic_printf("No memory information available");
  }
  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;
  DEBUG_RETURN_INT(0);
error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive disk info reply

  @parameter conn              IN:  The connection from the client
*/
static int
rec_disk_info_reply(IC_CONNECTION *conn)
{
  int ret_code;
  gboolean found= FALSE;
  guint32 disk_space= 0;
  DEBUG_ENTRY("rec_disk_info_reply");

  if ((ret_code= ic_rec_simple_str_opt(conn,
                                       ic_no_disk_info_available_str,
                                       &found)))
    goto error;
  if (!found)
  {
    if ((ret_code= ic_rec_number(conn, ic_disk_space_str, &disk_space)) ||
        (ret_code= ic_rec_empty_line(conn)))
      goto error;
    ic_printf("Available disk space = %u GBytes", disk_space);
  }
  else
  {
    /* No disk info available */
    if ((ret_code= ic_rec_empty_line(conn)))
      goto error;
    ic_printf("No disk information available");
  }
  DEBUG_RETURN_INT(0);
error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Receive a list node message

  @parameter conn              IN:  The connection from the client
  @parameter rec_empty_line    Flag indicating whether to receive an empty
                               line at the end
*/
static int
rec_list_node(IC_CONNECTION *conn, gboolean rec_empty_line)
{
  int ret_code;
  guint32 num_params= 0;
  gchar time_buf[IC_NUMBER_SIZE], pid_buf[IC_NUMBER_SIZE];
  DEBUG_ENTRY("rec_list_node");

  if ((ret_code= ic_rec_simple_str(conn, ic_list_node_str)) ||
      (ret_code= ic_rec_two_strings(conn,
                                    ic_program_str,
                                    ic_cluster_server_program_str)) ||
      (ret_code= ic_rec_two_strings(conn,
                                   ic_version_str,
                                   version_str)) ||
      (ret_code= ic_rec_two_strings(conn,
                                   ic_grid_str,
                                   my_grid)) ||
      (ret_code= ic_rec_two_strings(conn,
                                    ic_cluster_str,
                                    my_cluster)) ||
      (ret_code= ic_rec_two_strings(conn,
                                    ic_node_str,
                                    my_csd_node)) ||
      (ret_code= ic_rec_string(conn, ic_start_time_str, time_buf)) ||
      (ret_code= ic_rec_string(conn, ic_pid_str, pid_buf)) ||
      (ret_code= ic_rec_number(conn, ic_num_parameters_str, &num_params)))
    DEBUG_RETURN_INT(ret_code);
  if (num_params != 2)
    DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
  ic_printf("Start_time: %s, pid: %s", time_buf, pid_buf);
  if (rec_empty_line && (ret_code= ic_rec_empty_line(conn)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Receive a list full node message

  @parameter conn              IN:  The connection from the client
*/
static int
rec_list_node_full(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 node_id= 0;
  DEBUG_ENTRY("rec_list_node_full");

  if ((ret_code= rec_list_node(conn, FALSE)))
    DEBUG_RETURN_INT(ret_code);

  if ((ret_code= ic_rec_two_strings(conn,
                                    ic_parameter_str,
                                    ic_node_id_str)) ||
      (ret_code= ic_rec_number(conn, ic_parameter_str, &node_id)))
    DEBUG_RETURN_INT(ret_code);
  if (node_id != 1)
    DEBUG_RETURN_INT(IC_PROTOCOL_ERROR);
  if ((ret_code= ic_rec_empty_line(conn)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Test listing the single ic_csd process we previously started with all
  its parameters.

  We first list with full description where we get all nodes, next we
  list only one grid, then only one cluster and finally only one node.
  We try both successful and unsuccessful searches.

  @parameter conn        IN: The connection to the process controller
*/
static int test_list(IC_CONNECTION *conn, gboolean stop_flag)
{
  int ret_code;
  DEBUG_ENTRY("test_list");

  ic_printf("Testing list full of processes in ic_pcntrld");
  if ((ret_code= send_list_full(conn)) ||
      (ret_code= rec_list_node_full(conn)) ||
      (ret_code= ic_handle_list_stop(conn, stop_flag)))
    goto error;
  ic_printf("Testing list of processes from grid in ic_pcntrld");
  if ((ret_code= send_list_grid(conn)) ||
      (ret_code= rec_list_node(conn, TRUE)) ||
      (ret_code= ic_handle_list_stop(conn, stop_flag)))
    goto error;
  ic_printf("Testing list of processes from cluster in ic_pcntrld");
  if ((ret_code= send_list_cluster(conn)) ||
      (ret_code= rec_list_node(conn, TRUE)) ||
      (ret_code= ic_handle_list_stop(conn, stop_flag)))
    goto error;
  ic_printf("Testing list of processes from node in ic_pcntrld");
  if ((ret_code= send_list_node(conn)) ||
      (ret_code= rec_list_node(conn, TRUE)) ||
      (ret_code= ic_handle_list_stop(conn, stop_flag)))
    goto error;
  ic_printf("Testing unsuccessful search from grid in ic_pcntrld");
  if ((ret_code= send_list_unsuccessful_grid(conn)) ||
      (ret_code= ic_rec_list_stop(conn)))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

static int test_copy_files(IC_CONNECTION *conn)
{
  int ret_code;
  IC_STRING current_dir;
  DEBUG_ENTRY("test_copy_files");

  ic_set_current_dir(&current_dir);

  if ((ret_code= ic_send_with_cr(conn, ic_copy_cluster_server_files_str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_cluster_server_node_id_str,
                                             (guint64)1)) ||
       (ret_code= ic_send_with_cr_with_number(conn,
                                              ic_number_of_clusters_str,
                                              (guint64)2)))
    goto error;
  if ((ret_code= ic_proto_send_file(conn,
                                    "config.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)) ||
      (ret_code= ic_proto_send_file(conn,
                                    "grid_common.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)) ||
      (ret_code= ic_proto_send_file(conn,
                                    "kalle.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)) ||
      (ret_code= ic_proto_send_file(conn,
                                    "jocke.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

static int test_cpu_info(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("test_cpu_info");

  ic_printf("Testing CPU info protocol");
  if ((ret_code= ic_send_cpu_info_req(conn)) ||
      (ret_code= rec_cpu_info_reply(conn)))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

static int test_mem_info(IC_CONNECTION *conn)
{
  int ret_code;
  DEBUG_ENTRY("test_mem_info");

  ic_printf("Testing memory info protocol");
  if ((ret_code= ic_send_mem_info_req(conn)) ||
      (ret_code= rec_mem_info_reply(conn)))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

static int test_disk_info(IC_CONNECTION *conn)
{
  int ret_code;
  IC_STRING dir_name;
  DEBUG_ENTRY("test_disk_info");

  ic_printf("Testing disk info protocol");
  if ((ret_code= ic_set_data_dir(&dir_name, TRUE)) ||
      (ret_code= ic_send_disk_info_req(conn, dir_name.str)) ||
      (ret_code= rec_disk_info_reply(conn)))
    goto error;

error:
  if (dir_name.str)
    ic_free(dir_name.str);
  if (ret_code)
    ic_print_error(ret_code);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Start function for unit tests of the process controller. This function
  will be run in a separate thread from the rest of the process controller.

  @parameter data          Ignored parameter
*/
static void*
run_unit_test(gpointer data)
{
  int ret_code;
  IC_CONNECTION *conn;
  IC_THREADPOOL_STATE *tp_state;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  DEBUG_THREAD_ENTRY("run_unit_test");
  tp_state= thread_state->ic_get_threadpool(thread_state);
  tp_state->ts_ops.ic_thread_started(thread_state);

  (void)data; /* Ignore parameter */

  if ((ret_code= start_client_connection(&conn)))
    goto error;
      
  if ((ret_code= test_copy_files(conn)) ||
      (ret_code= test_successful_start(conn)) ||
      (ret_code= test_unsuccessful_start(conn)) ||
      (ret_code= test_list(conn, TRUE)) ||
      (ret_code= test_list(conn, FALSE)) ||
      (ret_code= test_successful_stop(conn)) ||
      (ret_code= test_successful_stop(conn)) ||
      (ret_code= test_cpu_info(conn)) ||
      (ret_code= test_mem_info(conn)) ||
      (ret_code= test_disk_info(conn)))
    goto error;
  /* When running unit test we're done after completing tests */
  ic_printf("Unit test passed successfully");
  DEBUG_PRINT(PROGRAM_LEVEL, ("Unit test passed successfully"));
error:
  if (conn)
    conn->conn_op.ic_free_connection(conn);
  ic_set_stop_flag();
  tp_state->ts_ops.ic_thread_stops(thread_state);
  DEBUG_THREAD_RETURN;
}

/**
  Start unit test thread
  This function will start up a thread starting function
  run_unit_test

  This thread will although running in the same process be able to communicate
  with the process controller and test its functionality.
*/
static int
start_unit_test(IC_THREADPOOL_STATE *tp_state)
{
  guint32 thread_id;
  DEBUG_ENTRY("start_unit_test");

  DEBUG_PRINT(PROGRAM_LEVEL, ("Start unit test thread in run_unit_test"));

  shutdown_processes_flag= TRUE;
  if (tp_state->tp_ops.ic_threadpool_start_thread(tp_state,
                                                  &thread_id,
                                                  run_unit_test,
                                                  NULL,
                                                  IC_MEDIUM_STACK_SIZE,
                                                  FALSE))
  {
    ic_printf("Failed to create unit test thread");
    DEBUG_RETURN_INT(IC_ERROR_START_THREAD_FAILED);
  }
  DEBUG_RETURN_INT(0);
}
#endif

/**
  Send the protocol message describing one process under our control
  describing its key, program, version, start time, pid and the
  number of parameters and the start parameters.

  @parameter conn              IN:  The connection from the client
  @parameter pc_start          IN:  The data structure describing the process
  @parameter list_full_flag    IN:  Flag indicating if we should send parameter
                                    data
*/
static int
send_list_entry(IC_CONNECTION *conn,
                IC_PC_START *pc_start,
                gboolean list_full_flag)
{
  guint32 i;
  int ret_code;
  DEBUG_ENTRY("send_list_entry");

  if ((ret_code= ic_send_with_cr(conn, ic_list_node_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_program_str,
                                             pc_start->program_name.str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_version_str,
                                             pc_start->version_string.str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_grid_str,
                                             pc_start->key.grid_name.str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_cluster_str,
                                             pc_start->key.cluster_name.str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_node_str,
                                             pc_start->key.node_name.str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_start_time_str,
                                             pc_start->start_id)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_pid_str,
                                             (guint64)pc_start->pid)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_num_parameters_str,
                                       (guint64)pc_start->num_parameters)))
    DEBUG_RETURN_INT(ret_code);
  if (list_full_flag)
  {
    for (i= 0; i < pc_start->num_parameters; i++)
    {
      if ((ret_code= ic_send_with_cr_two_strings(conn,
                                                 ic_parameter_str,
                                                 pc_start->parameters[i].str)))
        DEBUG_RETURN_INT(ret_code);
    }
  }
  if ((ret_code= ic_send_empty_line(conn)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  Get the key to the process from the protocol. The key consists of a
  triplet, node name, cluster name and grid name.

  @parameter conn              IN:  The connection from the client
  @parameter mc_ptr            IN:  Memory container to allocate string objects
  @parameter pc_key            OUT: Data structure containing the key elements
*/
static int
get_key(IC_CONNECTION *conn,
        IC_MEMORY_CONTAINER *mc_ptr,
        IC_PC_KEY *pc_key)
{
  int ret_code;
  DEBUG_ENTRY("get_key");

  if ((ret_code= ic_mc_rec_string(conn, mc_ptr,
                                  ic_grid_str, &pc_key->grid_name)) ||
      (ret_code= ic_mc_rec_string(conn, mc_ptr,
                                  ic_cluster_str, &pc_key->cluster_name)) ||
      (ret_code= ic_mc_rec_string(conn, mc_ptr,
                                  ic_node_str, &pc_key->node_name)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}


/**
  Read the start message from the socket represented by conn and put the
  information into the pc_start data structure.

  @parameter conn              IN:  The connection from the client
  @parameter pc_start          OUT: The place to put the reference to the
                               IC_PC_START data structure.

  @note
    pc_start is allocated by this function and it's allocated on a
    memory container and the reference to this memory container is
    stored in the allocated pc_start object.
*/
static int
rec_start_message(IC_CONNECTION *conn,
                  IC_PC_START **pc_start)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_PC_START *loc_pc_start;
  guint32 i;
  int ret_code;
  DEBUG_ENTRY("rec_start_message");

  /*
    When we come here we have already received the start string, we now
    receive the rest of the start package.
    We put the data into an IC_PC_START struct which is self-contained
    and also contains a memory container for all the memory allocated.
  */
  if (!(mc_ptr= ic_create_memory_container((guint32)1024, (guint32) 0, FALSE)))
  {
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  }
  if (!(*pc_start= (IC_PC_START*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_PC_START))))
    goto mem_error;
  loc_pc_start= *pc_start;
  loc_pc_start->mc_ptr= mc_ptr;

  if ((ret_code= ic_mc_rec_string(conn,
                                  mc_ptr,
                                  ic_program_str,
                                  &loc_pc_start->program_name)) ||
      (ret_code= ic_mc_rec_string(conn,
                                  mc_ptr,
                                  ic_version_str,
                                  &loc_pc_start->version_string)) ||
      (ret_code= get_key(conn, mc_ptr, &loc_pc_start->key)) ||
      (ret_code= ic_rec_boolean(conn,
                                ic_auto_restart_str,
                                &loc_pc_start->autorestart)) ||
      (ret_code= ic_rec_number(conn,
                               ic_num_parameters_str,
                               &loc_pc_start->num_parameters)))
    goto error;
  if (!(loc_pc_start->parameters= (IC_STRING*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
           sizeof(IC_STRING)* loc_pc_start->num_parameters)))
    goto mem_error;
  for (i= 0; i < loc_pc_start->num_parameters; i++)
  {
    if ((ret_code= ic_mc_rec_string(conn,
                                    mc_ptr,
                                    ic_parameter_str,
                                    &loc_pc_start->parameters[i])))
      goto error;
  }
  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;
  DEBUG_RETURN_INT(0);
mem_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Support function used by hash table implementation, it calculates a hash
  value based on the void pointer (which here is a IC_PC_KEY object. It does
  so by using the standard hash value of a string and then XOR:ing all three
  hash values of the individual strings.

  @parameter ptr              IN: The key to calculate the hash value based on

  @retval The hash value
*/
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

/**
  Support function used by hash table implementation, it gets two void
  pointers that actually point to IC_PC_KEY objects. The two objects are
  compared for equality.

  @parameter ptr1       IN: The first process key to compare
  @parameter ptr2       IN: The second process key to compare

  @retval 1 means equal, 0 not equal
*/
static int
ic_pc_key_equal(void *ptr1, void *ptr2)
{
  IC_PC_KEY *pc_key1= ptr1;
  IC_PC_KEY *pc_key2= ptr2;
  DEBUG_ENTRY("ic_pc_key_equal");

  if ((ic_cmp_str(&pc_key1->grid_name, &pc_key2->grid_name)) ||
      (ic_cmp_str(&pc_key1->cluster_name, &pc_key2->cluster_name)) ||
      (ic_cmp_str(&pc_key1->node_name, &pc_key2->node_name)))
    DEBUG_RETURN_INT(0);
  DEBUG_RETURN_INT(1); /* Equal */
}

/**
  Create the hash table containing all processes under our control.
  The key is the node name, cluster name and grid name.

  @retval The hash table object
*/
IC_HASHTABLE*
create_pc_hash()
{
  IC_HASHTABLE *ret_ptr;
  DEBUG_ENTRY("create_pc_hash");
  ret_ptr= ic_create_hashtable(4096,
                               ic_pc_hash_key,
                               ic_pc_key_equal,
                               FALSE);
  DEBUG_RETURN_PTR(ret_ptr);
}

/**
  Remove entry from both hash table and dynamic array.

  @parameter pc_start  IN: The process entry reference
*/
static void
remove_pc_entry(IC_PC_START *pc_start)
{
  IC_PC_START *pc_start_check;
  DEBUG_ENTRY("remove_pc_entry");

  event_occurred= TRUE;
  pc_start_check= ic_hashtable_remove(glob_pc_hash,
                                      (void*)pc_start);
  ic_require(pc_start_check == pc_start);
  glob_pc_array->dpa_ops.ic_remove_ptr(glob_pc_array,
                                       pc_start->dyn_trans_index,
                                       (void*)pc_start);
  DEBUG_RETURN_EMPTY;
}

/**
  Insert process entry into hash table and dynamic array. All process
  entries are stored both in a dynamic array and a hash table. Thus
  we can find the IC_PC_START object either by the key or through the
  dynamic index.

  @parameter pc_start  IN: The process entry reference
*/
static int
insert_pc_entry(IC_PC_START *pc_start)
{
  int ret_code= 0;
  guint64 index;
  DEBUG_ENTRY("insert_pc_entry");

  event_occurred= TRUE;
  if ((ret_code= glob_pc_array->dpa_ops.ic_insert_ptr(glob_pc_array,
                                                      &index,
                                                      (void*)pc_start)))
    goto end;
  pc_start->dyn_trans_index= index;
  if ((ret_code= ic_hashtable_insert(glob_pc_hash,
                                     (void*)pc_start, (void*)pc_start)))
  {
    glob_pc_array->dpa_ops.ic_remove_ptr(glob_pc_array,
                                         pc_start->dyn_trans_index,
                                         (void*)pc_start);
  }
end:
  DEBUG_RETURN_INT(ret_code);
}

static void
print_stopped_program(IC_PC_START *pc_start)
{
  gchar *pid_str;
  gchar pid_buf[IC_NUMBER_SIZE];
  guint32 dummy;

  pid_str= ic_guint64_str(pc_start->pid, pid_buf, &dummy);
  ic_printf("Program %s with pid %s has stopped",
            pc_start->program_name.str,
            pid_str);
}

/**
  As part of the starting of a new process we will insert the process into
  our control structures. However the process might already be there. It
  might also be dead although it's in our control structure. So we need to
  also check reality through the ic_is_process_alive call.

  @parameter pc_start       IN: The data structure describing the process
*/
static int
insert_process(IC_PC_START *pc_start, IC_PC_START **pc_start_hash)
{
  IC_PC_START *pc_start_found;
  guint64 prev_start_id= 0;
  int ret_code= 0;
  guint32 retry_count= 0;
  DEBUG_ENTRY("insert_process");

  /*
    Insert the memory describing the started process in the hash table
    of all programs we have started, the key is the grid name, the cluster
    name and the node name. We can also iterate over this hashtable to
    check all entries. Since there can be multiple threads accessing this
    hash table concurrently we have to protect access by a mutex.
    At this point we need to verify that there isn't already a process
    running with this key, if such a process is already running we
    need to verify it is still running. Only if no process is running
    with this key will we continue to process the start of the program.
  */
try_again:
  ic_mutex_lock(pc_hash_mutex);
  if ((pc_start_found= (IC_PC_START*)
       ic_hashtable_search(glob_pc_hash, (void*)pc_start)))
  {
    *pc_start_hash= pc_start_found;
    /*
      We found a process with the same key, it could be a process which
      has already stopped so we first verify that the process is still
      running.
    */
    if (!pc_start_found->pid)
    {
      /* Someone is already trying to start, we won't continue or attempt */
      ret_code= IC_ERROR_PC_START_ALREADY_ONGOING;
    }
    else if (pc_start->kill_ongoing || pc_start->check_ongoing)
    {
      /*
        Someone is attempting to kill this process, we'll wait for a few
        seconds for this to complete in the hope that we'll be successful
        in starting it after it has been killed. Could also be that the
        check process is busy checking it.
      */
      ic_mutex_unlock(pc_hash_mutex);
      retry_count++;
      if (retry_count > 10)
      {
        ret_code= IC_ERROR_PC_PROCESS_ALREADY_RUNNING;
        goto end;
      }
      ic_sleep(1);
      goto try_again;
    }
    else
    {
      if (pc_start_found->start_id == prev_start_id)
      {
        /*
          We come here the second time around and have verified that it is
          still the same process in the hash, thus we have verified that
          the process is dead and that it is the same process which is
          in the hash, thus it is now safe to remove it from the hash.
        */
        remove_pc_entry(pc_start_found);
        ret_code= insert_pc_entry(pc_start);
        ic_mutex_unlock(pc_hash_mutex);
        print_stopped_program(pc_start_found);
        /* Release memory associated with the deleted process */
        pc_start_found->mc_ptr->mc_ops.ic_mc_free(pc_start_found->mc_ptr);
        goto end;
      }
      prev_start_id= pc_start_found->start_id;
      ic_mutex_unlock(pc_hash_mutex);
      /*
        Verify that the process is still alive, we have released the mutex
        to avoid making the mutex a hotspot at the price of slightly more
        complex logic.
      */
      if ((ret_code= ic_is_process_alive(pc_start_found->pid,
                                         pc_start_found->program_name.str)))
      {
        if (ret_code == IC_ERROR_CHECK_PROCESS_SCRIPT)
          goto end;
        ic_assert(ret_code == IC_ERROR_PROCESS_NOT_ALIVE);
        /*
          Process is dead, we need to acquire the mutex again and verify that
          no one else have inserted a new process into the hash again before
          removing the old copy and inserting ourselves.
          We have remembered the start_id of the instance we found to be dead
          and thus if we can verify that it is still in the hash then we are
          safe to remove it. Otherwise someone got there before us and we
          retry a check of the process being alive.
        */
        goto try_again;
      }
      else
      {
        /* Process is still alive, so no use for us to continue */
        ret_code= IC_ERROR_PC_PROCESS_ALREADY_RUNNING;
        goto end;
      }
    }
  }
  else
  {
    /*
      No similar process found, we'll insert it into the hash at this point
      to make sure no one else is attempting to start the process while we
      are trying.
    */
    ret_code= insert_pc_entry(pc_start);
  }
  ic_mutex_unlock(pc_hash_mutex);
end:
  DEBUG_RETURN_INT(ret_code);
}

/**
  Verify that the starting program is a program handled by iClaustron

  @program_name              IN: The program name

  NOTE:
  Current program handled is:
  1) ic_csd == iClaustron Cluster Server
  2) ic_clmgrd == iClaustron Cluster Manager
  3) ic_fsd == iClaustron File Server
  4) ic_repd == iClaustron Replication Server
  5) ndbmtd == NDB Data Node
*/
static int
check_certified_program(gchar *program_name)
{
  DEBUG_ENTRY("check_certified_program");
  DEBUG_PRINT(PROGRAM_LEVEL, ("Program: %s", program_name));

  if ((strcmp(program_name, ic_cluster_server_program_str) == 0) ||
      (strcmp(program_name, ic_cluster_manager_program_str) == 0) ||
      (strcmp(program_name, ic_file_server_program_str) == 0) ||
      (strcmp(program_name, ic_rep_server_program_str) == 0) ||
      (strcmp(program_name, ic_data_server_program_str) == 0))
    DEBUG_RETURN_INT(0);
  DEBUG_RETURN_INT(IC_ERROR_PROGRAM_NOT_SUPPORTED);
}

static guint32
get_node_id_from_str(gchar *str)
{
  int ret_code;
  guint64 number;

  if ((ret_code= ic_conv_str_to_int(str, &number, NULL)))
  {
    return 0;
  }
  if (number > IC_MAX_NODE_ID)
  {
    return 0;
  }
  DEBUG_PRINT(PROGRAM_LEVEL, ("Node id of starting program: %u",
              (guint32)number));
  return (guint32)number;
}

/**
 * To find the pid file we need to retrieve the node id from the
 * arguments to start the daemon programs. For iClaustron programs
 * this is found in --node-id and --ndb-nodeid.
 */
static guint32
get_nodeid_in_arg_vector(gchar *arg_vector[], guint32 num_parameters)
{
  guint32 i;
  guint32 node_id= 0;
  gchar *program_name= arg_vector[0];

  for (i= 1; i < num_parameters; i++)
  {
    if (strcmp(program_name, ic_data_server_program_str) == 0)
    {
      /* NDB data node program */
      if (strcmp(arg_vector[i], "--ndb-nodeid") == 0)
      {
        if ((i + 1) < num_parameters)
        {
          node_id= get_node_id_from_str(arg_vector[i+1]);
          break;
        }
      }
    }
    else
    {
      if (strcmp(arg_vector[i], "--node-id") == 0)
      {
        if ((i + 1) < num_parameters)
        {
          node_id= get_node_id_from_str(arg_vector[i+1]);
          break;
        }
      }
    }
  }
  return node_id;
}

/**
 * To daemonize the programs is the default behaviour.
 * iClaustron programs can run in foreground by using --daemonize set to
 * 0.
 * ndbmtd can be run in foreground by setting --nodaemon or --foreground.
 */
static gboolean
check_for_daemon_in_arg_vector(gchar *arg_vector[], guint32 num_parameters)
{
  gboolean is_daemon_process= TRUE;
  guint32 i;
  gchar *program_name= arg_vector[0];

  for (i= 1; i < num_parameters; i++)
  {
    if (strcmp(program_name, ic_data_server_program_str) == 0)
    {
      /* NDB data node program */
      if (strcmp(arg_vector[i], "--nodaemon") == 0)
      {
        is_daemon_process= FALSE;
      }
      else if (strcmp(arg_vector[i], "--foreground") == 0)
      {
        is_daemon_process= FALSE;
      }
    }
    else
    {
      /* iClaustron programs */
      if (strcmp(arg_vector[i], "--daemonize") == 0)
      {
        if ((i + 1 < num_parameters) && (strcmp(arg_vector[i+1], "0")))
        {
          is_daemon_process= FALSE;
        }
      }
    }
  }
  DEBUG_PRINT(PROGRAM_LEVEL, ("Process %s will %s be a daemon",
              program_name,
              (is_daemon_process ? "": "not")));
  return is_daemon_process;
}

/**
  Handle a start process request to the process controller

  @parameter conn              IN: The connection from the client
*/
static int
handle_start(IC_CONNECTION *conn)
{
  gchar **arg_vector;
  int ret_code;
  guint32 i;
  IC_PID_TYPE pid= (IC_PID_TYPE)0;
  IC_STRING pid_file, binary_dir;
  IC_PC_START *pc_start;
  IC_PC_START *pc_start_hash= NULL;
  gchar *pid_str;
  guint32 dummy;
  guint32 node_id;
  gboolean is_daemon_process;
  gchar pid_buf[IC_NUMBER_SIZE];
  DEBUG_ENTRY("handle_start");

  IC_INIT_STRING(&binary_dir, NULL, 0, TRUE);
  IC_INIT_STRING(&pid_file, NULL, 0, TRUE);

  if ((ret_code= rec_start_message(conn, &pc_start)))
  {
    DEBUG_RETURN_INT(ret_code);
  }
  if (!(arg_vector= (gchar**)pc_start->mc_ptr->mc_ops.ic_mc_calloc(
                     pc_start->mc_ptr,
                     (pc_start->num_parameters+2) * sizeof(gchar*))))
    goto mem_error;
  /*
    Prepare the argument vector, first program name and then all the
    parameters passed to the program.
  */
  if (check_certified_program(pc_start->program_name.str))
  {
    ret_code= IC_ERROR_PROGRAM_NOT_CERTIFIED_FOR_START;
    goto error;
  }
  arg_vector[0]= pc_start->program_name.str;
  for (i= 0; i < pc_start->num_parameters; i++)
  {
    arg_vector[i+1]= pc_start->parameters[i].str;
  }

  is_daemon_process= check_for_daemon_in_arg_vector(arg_vector,
                                           pc_start->num_parameters+1);

  node_id= get_nodeid_in_arg_vector(arg_vector,
                                     pc_start->num_parameters+1);

  if (node_id == 0)
  {
    ret_code= IC_ERROR_NO_PROPER_NODE_ID_FOR_PROGRAM;
    goto error;
  }
  /* Book the process in the hash table */
  if ((ret_code= insert_process(pc_start, &pc_start_hash)))
    goto error;
  /*
    Create the working directory which is the base directory
    (iclaustron_install) with the version number appended and
    the bin directory where the binaries are placed.
  */
  if ((ret_code= ic_set_binary_dir(&binary_dir,
                                   pc_start->version_string.str)))
    goto late_error;

  if (is_daemon_process)
  {
    /**
     * We need the pid file to get hold of the PID of programs that
     * start up as daemons.
     */
    if ((ret_code= ic_set_out_file(&pid_file,
                                   node_id,
                                   pc_start->program_name.str,
                                   TRUE,
                                   FALSE)))
      goto error;
  }

  /*
    Start the program using the program name, its arguments and the binary
    placed in the working bin directory, return the PID of the started
    process.
  */
  if ((ret_code= ic_start_process(&arg_vector[0],
                                  binary_dir.str,
                                  pid_file.str,
                                  is_daemon_process,
                                  &pid)))
    goto late_error;
  /*
    Verify that a process with this PID still exists and that it's using
    the correct program name.
  */
  if ((ret_code= ic_is_process_alive(pid,
                                     pc_start->program_name.str)))
    goto late_error;

  if (binary_dir.str)
  {
    ic_free(binary_dir.str);
  }
  if (pid_file.str)
  {
    ic_free(pid_file.str);
  }
  binary_dir.str= NULL;
  pid_file.str= NULL;

  ic_mutex_lock(pc_hash_mutex);
  /*
    pc_start struct is protected by mutex, as soon as the pid is nonzero
    anyone can remove it from hash table and release the memory. Thus
    need to be careful with updating the value from zero.
    We also update the start_id at this point to ensure that we can
    distinguish entries of the same key from each other.
  */
  pc_start->pid= pid;
  pc_start->start_id= glob_start_id++;
  ic_mutex_unlock(pc_hash_mutex);
  pid_str= ic_guint64_str(pc_start->pid, pid_buf, &dummy);
  ic_printf("Successfully started program %s with pid %s",
            pc_start->program_name.str,
            pid_str);

  DEBUG_PRINT(PROGRAM_LEVEL, ("New process with pid %d started", (int)pid));
  ret_code= ic_send_ok_pid(conn, pc_start->pid);
  DEBUG_RETURN_INT(ret_code);

late_error:
  ic_mutex_lock(pc_hash_mutex);
  remove_pc_entry(pc_start);
  ic_mutex_unlock(pc_hash_mutex);

  pid_str= ic_guint64_str(pid, pid_buf, &dummy);
  ic_printf("Failed to start program %s with pid %s",
            pc_start->program_name.str,
            pid_str);
  goto error;
mem_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  if (binary_dir.str)
  {
    ic_free(binary_dir.str);
  }
  if (pid_file.str)
  {
    ic_free(pid_file.str);
  }
  pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
  if (ret_code == IC_ERROR_PC_PROCESS_ALREADY_RUNNING)
  {
    ret_code= ic_send_ok_pid_started(conn, pc_start_hash->pid);
  }
  else
  {
    ic_send_error_message(conn, 
                          ic_get_error_message(ret_code));
  }
  DEBUG_RETURN_INT(ret_code);
}

/**
  Initialize a IC_PC_FIND object which is used to find a process

  @parameter pc_find          OUT: The place to put the reference to the
                                   pc_find object allocated
*/
static int
init_pc_find(IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  DEBUG_ENTRY("init_pc_find");

  if (!(mc_ptr= ic_create_memory_container((guint32)1024,
                                           (guint32)0,
                                           FALSE)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  if (!((*pc_find)= (IC_PC_FIND*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_PC_FIND))))
  {
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  }
  (*pc_find)->mc_ptr= mc_ptr;
  DEBUG_RETURN_INT(0);
}

/**
  Receive a protocol message containing a key to a process, fill the data
  into the pc_find container.

  @parameter conn               IN:  The connection from the client
  @parameter pc_find            OUT: The process find object to create
*/
static int
rec_key_message(IC_CONNECTION *conn,
                 IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  int ret_code;
  DEBUG_ENTRY("rec_key_message");

  /*
    When we come here we already received the stop/kill string and we only
    need to fill in the key parameters (grid, cluster and node name).
  */
  if ((ret_code= init_pc_find(pc_find)))
    goto error;
  mc_ptr= (*pc_find)->mc_ptr;
  if ((ret_code= get_key(conn, mc_ptr, &(*pc_find)->key)))
    goto error;
  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Perform the actual killing of a process handled by the process controller.

  @parameter pc_start_found    IN: The data structure describing the process
                                   we're controlling
  @parameter pc_find           IN: The data structure used to find the process
  @parameter kill_flag         IN: Flag to indicate a kill or a stop
*/
static int
kill_process(IC_PC_START *pc_start_found,
             IC_PC_FIND *pc_find,
             gboolean kill_flag)
{
  guint32 loop_count= 0;
  guint64 start_id;
  IC_PID_TYPE pid;
  int ret_code;
  DEBUG_ENTRY("kill_process");

  /*
    We release the mutex on the hash, try to stop the process and
    then check if we were successful in stopping it after waiting
    for a few seconds.

    By setting the kill_ongoing flag we ensure that no one else is
    removing the object.
  */
  start_id= pc_start_found->start_id;
  pid= pc_start_found->pid;
  pc_start_found->kill_ongoing= TRUE;
  ic_mutex_unlock(pc_hash_mutex);

  ic_kill_process(pid, kill_flag);

  loop_count= 0;
retry_kill_check:
  ic_sleep(1); /* Sleep 1 seconds to give OS a chance to complete kill */
  ret_code= ic_is_process_alive(pid, pc_start_found->program_name.str);
  if (ret_code == 0)
  {
    loop_count++;
    if (loop_count > 3)
    {
      ret_code= IC_ERROR_FAILED_TO_STOP_PROCESS;
      goto error;
    }
    goto retry_kill_check;
  }
  else if (ret_code == IC_ERROR_CHECK_PROCESS_SCRIPT)
    goto error;
  else
  {
    ic_assert(ret_code == IC_ERROR_PROCESS_NOT_ALIVE);
    /*
      We were successful in stopping the process, now we need to
      remove the entry from the hash.
    */
    ret_code= 0;
    ic_mutex_lock(pc_hash_mutex);
    pc_start_found= (IC_PC_START*)ic_hashtable_search(glob_pc_hash,
                                                      (void*)pc_find);
    ic_require(pc_start_found &&
               pc_start_found->pid == pid &&
               pc_start_found->start_id == start_id);
    /*
      The process we set out to stop is still in the hash so we remove it.
      Since we set the kill_ongoing flag no one else should remove it,
      if it is removed it's a bug.
    */
    remove_pc_entry(pc_start_found);
    ic_mutex_unlock(pc_hash_mutex);
    print_stopped_program(pc_start_found);
    /* Release memory associated with the deleted process */
    pc_start_found->mc_ptr->mc_ops.ic_mc_free(pc_start_found->mc_ptr);
  }
  DEBUG_RETURN_INT(0);

error:
  ic_mutex_lock(pc_hash_mutex);
  pc_start_found->kill_ongoing= FALSE;
  ic_mutex_unlock(pc_hash_mutex);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Delete the process as requested by the protocol action.  The flag indicates
  whether the client ordered a kill or a stop to be performed.
  Before killing the process we will discover whether we have such a process
  under our control.

  @parameter pc_find           IN: Key of process to stop
  @parameter kill_flag         IN: Flag to indicate a kill or a stop
*/
static int
delete_process(IC_PC_FIND *pc_find,
               gboolean kill_flag)
{
  IC_PC_START *pc_start_found;
  int ret_code= 0;
  guint32 loop_count= 0;
  DEBUG_ENTRY("delete_process");

try_again:
  ic_mutex_lock(pc_hash_mutex);
  if ((pc_start_found= (IC_PC_START*)ic_hashtable_search(glob_pc_hash,
                                                         (void*)pc_find)))
  {
    /*
      We found a process, now it's time to try to stop the process.
      We'll check if the process is still in startup, in that case we
      need to wait for the starting process to complete before we
      stop the process.
    */
    if (pc_start_found->pid == 0 || pc_start_found->check_ongoing)
    {
      /*
        The process is still starting up or the check thread is busy
        checking it already, too early to stop it.
      */
      ic_mutex_unlock(pc_hash_mutex);
      if (++loop_count == 10)
      {
        ret_code= IC_ERROR_PROCESS_STUCK_IN_START_PHASE;
        goto error;
      }
      ic_sleep(1);
      DEBUG_PRINT(THREAD_LEVEL, ("Wake up after sleep in delete_process"));
      goto try_again;
    }
    if (pc_start_found->kill_ongoing)
    {
      ret_code= IC_ERROR_PROCESS_ALREADY_BEING_KILLED;
      goto error;
    }
    ret_code= kill_process(pc_start_found, pc_find, kill_flag);
  }
  else
  {
    /*
      We couldn't find any such process in the hash, this means that the
      process doesn't exist and we have succeeded since our aim was to
      ensure the process was stopped.
    */
    ic_mutex_unlock(pc_hash_mutex);
  }
error:
  DEBUG_RETURN_INT(ret_code);
}

/**
  Handle the protocol message to stop a process. The flag indicates whether
  the client ordered a kill or a stop to be performed.
  Before killing the process we will discover whether we have such a process
  under our control.

  @parameter conn              IN: The connection from the client
  @parameter kill_flag         IN: Flag to indicate a kill or a stop

  @note
    Kill means the same as kill -9 pid
    Stop means the same as kill -15 pid
*/
static int
handle_stop(IC_CONNECTION *conn, gboolean kill_flag)
{
  IC_PC_FIND *pc_find= NULL;
  int ret_code;
  DEBUG_ENTRY("handle_stop");

  if ((ret_code= rec_key_message(conn, &pc_find)))
    goto error;
  ret_code= delete_process(pc_find, kill_flag);
  pc_find->mc_ptr->mc_ops.ic_mc_free(pc_find->mc_ptr);
  if (!ret_code)
    DEBUG_RETURN_INT(ic_send_ok(conn));
error:
  ic_send_error_message(conn,
                        ic_get_error_message(ret_code));
  DEBUG_RETURN_INT(ret_code);
}

/**
  This performs a key match. The key here can have parts of the key
  being NULL. We support searching for processes given only grid name,
  only grid and cluster name and the full key.

  @parameter pc_start           IN: The process descriptor currently matched
  @parameter pc_find            IN: The search description

  @retval          TRUE if matching, FALSE if not matching
*/
static gboolean
is_list_match(IC_PC_START *pc_start, IC_PC_FIND *pc_find)
{
  if (pc_find->key.grid_name.str == NULL)
    return TRUE;
  if (pc_find->key.grid_name.len != pc_start->key.grid_name.len ||
      memcmp(pc_find->key.grid_name.str,
             pc_start->key.grid_name.str,
             pc_find->key.grid_name.len) != 0)
    return FALSE;
  /* Grid names were equal */
  if (pc_find->key.cluster_name.str == NULL)
    return TRUE;
  if (pc_find->key.cluster_name.len != pc_start->key.cluster_name.len ||
      memcmp(pc_find->key.cluster_name.str,
             pc_start->key.cluster_name.str,
             pc_find->key.cluster_name.len) != 0)
    return FALSE;
  /* Cluster names were equal */
  if (pc_find->key.node_name.str == NULL)
    return TRUE;
  if (pc_find->key.node_name.len != pc_start->key.node_name.len ||
      memcmp(pc_find->key.node_name.str,
             pc_start->key.node_name.str,
             pc_find->key.node_name.len) != 0)
    return FALSE;
  /* Node names were equal */
  return TRUE;
}

/**
  Copy an IC_PC_START object found in hash table to make it available
  for use without worry of concurrent users of the object.

  @parameter pc_start          The object to copy
  @parameter mc_ptr            The memory container to allocate the new
                               object from
  @parameter list_full_flag    Copy also parameter list if this is TRUE

  @note
    All parameters are IN-parameters
*/
static IC_PC_START*
copy_pc_start(IC_PC_START *pc_start,
              IC_MEMORY_CONTAINER *mc_ptr,
              gboolean list_full_flag)
{
  IC_PC_START *new_pc_start;
  guint32 i;

  if (!(new_pc_start= (IC_PC_START*)
        mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, sizeof(IC_PC_START))))
    goto mem_error;
  memcpy(new_pc_start, pc_start, sizeof(IC_PC_START));
  if (!((new_pc_start->key.grid_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->key.grid_name.len + 1)) &&
      (new_pc_start->key.cluster_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->key.cluster_name.len + 1)) &&
      (new_pc_start->key.node_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->key.node_name.len + 1)) &&
      (new_pc_start->version_string.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->version_string.len + 1)) &&
      (new_pc_start->program_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->program_name.len + 1))))
    goto mem_error;
  memcpy(new_pc_start->key.grid_name.str,
         pc_start->key.grid_name.str,
         pc_start->key.grid_name.len);
  new_pc_start->key.grid_name.is_null_terminated= TRUE;

  memcpy(new_pc_start->key.cluster_name.str,
         pc_start->key.cluster_name.str,
         pc_start->key.cluster_name.len);
  new_pc_start->key.cluster_name.is_null_terminated= TRUE;

  memcpy(new_pc_start->key.node_name.str,
         pc_start->key.node_name.str,
         pc_start->key.node_name.len);
  new_pc_start->key.node_name.is_null_terminated= TRUE;

  memcpy(new_pc_start->version_string.str,
         pc_start->version_string.str,
         pc_start->version_string.len);
  new_pc_start->version_string.is_null_terminated= TRUE;

  memcpy(new_pc_start->program_name.str,
         pc_start->program_name.str,
         pc_start->program_name.len);
  new_pc_start->program_name.is_null_terminated= TRUE;

  if (list_full_flag)
  {
    if (!(new_pc_start->parameters= (IC_STRING*)mc_ptr->mc_ops.ic_mc_alloc(
          mc_ptr, pc_start->num_parameters * sizeof(IC_STRING))))
      goto mem_error;
    for (i= 0; i < pc_start->num_parameters; i++)
    {
      if (!(new_pc_start->parameters[i].str= mc_ptr->mc_ops.ic_mc_calloc(
            mc_ptr, pc_start->parameters[i].len + 1)))
        goto mem_error;
      memcpy(new_pc_start->parameters[i].str,
             pc_start->parameters[i].str,
             pc_start->parameters[i].len);
      new_pc_start->parameters[i].is_null_terminated= TRUE;
    }
  }
  return new_pc_start;

mem_error:
  return NULL;
}

/**
  Receive a range of keys to search for. One can either receive nothing,
  only grid name, only grid name and cluster name or receive the full key.

  @parameter conn              IN:  The connection
  @parameter pc_find           OUT: The search object to create
*/
static int
rec_opt_key_message(IC_CONNECTION *conn,
                    IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  int ret_code;
  DEBUG_ENTRY("rec_opt_key_message");

  /*
    When we come here we already received the list [full] string and we
    only need to fill in key parameters. All key parameters are optional
    here.
  */
  if ((ret_code= init_pc_find(pc_find)))
    goto error;
  mc_ptr= (*pc_find)->mc_ptr;
  if ((ret_code= ic_mc_rec_opt_string(conn,
                                      mc_ptr,
                                      ic_grid_str,
                                      &((*pc_find)->key.grid_name))))
    goto error;
  if ((*pc_find)->key.grid_name.len == 0)
  {
    /* No grid name provided, list all programs */
    goto end;
  }
  if ((ret_code= ic_mc_rec_opt_string(conn,
                                      mc_ptr,
                                      ic_cluster_str,
                                      &((*pc_find)->key.cluster_name))))
    goto error;
  if ((*pc_find)->key.cluster_name.len == 0)
  {
    /* No cluster name provided, list all programs in grid */
    goto end;
  }
  if ((ret_code= ic_mc_rec_opt_string(conn,
                                      mc_ptr,
                                      ic_node_str,
                                      &((*pc_find)->key.node_name))))
    goto error;
  if ((*pc_find)->key.node_name.len == 0)
  {
    /* No node name provided, list all programs in cluster */
    goto end;
  }
end:
  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;
  DEBUG_RETURN_INT(0);
error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Handle a list protocol action, either a full list or a simple list

  @parameter conn                  IN: The connection
  @parameter list_full_flag        IN: List with parameters list
*/
static int
handle_list(IC_CONNECTION *conn, gboolean list_full_flag)
{
  IC_PC_FIND *pc_find= NULL;
  int ret_code;
  IC_PC_START *first_pc_start= NULL;
  IC_PC_START *last_pc_start= NULL;
  IC_PC_START *pc_start, *loop_pc_start, *new_pc_start;
  guint64 current_index, max_index;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  void *void_pc_start;
  guint32 read_size;
  gchar *read_buf;
  gboolean stop_flag;
  DEBUG_ENTRY("handle_list");

  if ((ret_code= rec_opt_key_message(conn, &pc_find)))
    goto error;
  mc_ptr= pc_find->mc_ptr;
  ic_mutex_lock(pc_hash_mutex);
  max_index= glob_pc_array->dpa_ops.ic_get_max_index(glob_pc_array);
  for (current_index= 0; current_index < max_index; current_index++)
  {
    if ((ret_code= glob_pc_array->dpa_ops.ic_get_ptr(glob_pc_array,
                                                     current_index,
                                                     &void_pc_start)))
      goto error;
    pc_start= (IC_PC_START*)void_pc_start;
    if (pc_start && is_list_match(pc_start, pc_find))
    {
      if (!(new_pc_start= copy_pc_start(pc_start, mc_ptr, list_full_flag)))
        goto mem_error;
      if (!first_pc_start)
        first_pc_start= new_pc_start;
      else
        last_pc_start->next_pc_start= new_pc_start;
      last_pc_start= new_pc_start;
    }
  }
  ic_mutex_unlock(pc_hash_mutex);
  loop_pc_start= first_pc_start;
  stop_flag= FALSE;
  while (loop_pc_start && !stop_flag)
  {
    if ((ret_code= send_list_entry(conn, loop_pc_start, list_full_flag)))
      goto error;
    if (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
    {
      if (!ic_check_buf(read_buf,
                        read_size,
                        ic_list_stop_str,
                        strlen(ic_list_stop_str)))
        stop_flag= TRUE;
      else if ((ic_check_buf(read_buf,
                             read_size,
                             ic_list_next_str,
                             strlen(ic_list_next_str))))
        goto protocol_error;
    }
    if (ic_rec_empty_line(conn))
      goto protocol_error;
    loop_pc_start= loop_pc_start->next_pc_start;
  }
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN_INT(ic_send_list_stop(conn));

protocol_error:
  ret_code= IC_PROTOCOL_ERROR;
  goto error;
mem_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  ic_send_error_message(conn,
                        ic_get_error_message(ret_code));
  DEBUG_RETURN_INT(ret_code);
}

/**
  The protocol removes carriage return, before writing to the file we need
  to put it back again to ensure normal lines are created in the file.

  @parameter read_buf           IN/OUT: The buffer to add a CR to
  @parameter read_size          IN/OUT: The buffer size
*/
static int
add_cr(gchar *read_buf,
       guint32 *read_size)
{
  read_buf[*read_size]= CARRIAGE_RETURN;
  (*read_size)++;
  return 0;
}

/*
  Handle the receive file and write the file to the provided file name.
  For security reasons this function can only write files into the
  config directory. We don't want the client to be able to specify the
  directory since that could be used for too many malicious reasons.

  @parameter conn               The connection
  @parameter file_name_array    Dynamic array of file names received
  @parameter file_name          Name of file to be received in this method
  @parameter num_files          OUT: Number of files opened so far
                                Incremented by this function when successful
  @parameter node_id            Node id of the Cluster Server, this gives us
                                name of the directory to write the file into
  @parameter number_of_lines    Number of lines in file
*/
static int
handle_receive_file(IC_CONNECTION *conn,
                    IC_DYNAMIC_PTR_ARRAY *file_name_array,
                    gchar *file_name,
                    guint64 *num_files,
                    guint32 node_id,
                    guint32 number_of_lines)
{
  gchar *read_buf;
  guint32 read_size;
  guint32 i;
  int ret_code;
  IC_FILE_HANDLE file_ptr;
  IC_STRING file_str;
  DEBUG_ENTRY("handle_receive_file");

  if ((ret_code= ic_set_config_dir(&file_str,
                                   node_id ? TRUE : FALSE,
                                   node_id)))
    goto end;
  if ((ret_code= ic_add_dup_string(&file_str, file_name)))
    goto end;

  /* Record the file name to be able to clean up after an error */
  if (file_name_array)
  {
    if ((ret_code= file_name_array->dpa_ops.ic_insert_ptr(file_name_array,
                                                      num_files,
                                                      (void*)file_str.str)))
      goto end;
  }
  if ((ret_code= ic_create_file(&file_ptr,
                                file_str.str)))
  {
    /*
      Clean up since error handling will attempt to delete file. We don't
      want the file deleted since the error could have been that the file
      already existed.
    */
    if (file_name_array)
      (*num_files)--;
    goto error;
  }
  for (i= 0; i < number_of_lines; i++)
  {
    /* Receive line from connection and write line received to file */
    if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
        (ret_code= add_cr(read_buf, &read_size)) ||
        (ret_code= ic_write_file(file_ptr, read_buf, read_size)))
      goto error;
  }
  if ((ret_code= ic_rec_empty_line(conn)) ||
      (ret_code= ic_close_file(file_ptr)))
    goto error;
end:
  if (file_str.str)
    ic_free(file_str.str);
  DEBUG_RETURN_INT(ret_code);
error:
  (void)ic_close_file(file_ptr); /* Ignore error here */
  goto end;
}

static int
create_config_dir()
{
  int ret_code;
  IC_STRING data_dir;
  IC_STRING config_dir;

  /**
   * Ensure iclaustron_data directory is created
   * Next step is to ensure that the
   * iclaustron_data/config directory is also created
   */
  IC_INIT_STRING(&data_dir, NULL, 0, TRUE);
  if ((ret_code= ic_set_data_dir(&data_dir, FALSE)))
    goto error;
  ret_code= ic_mkdir(data_dir.str);
  ic_free(data_dir.str);
  if (ret_code)
    goto error;

  IC_INIT_STRING(&config_dir, NULL, 0, TRUE);
  if ((ret_code= ic_set_config_dir(&config_dir, FALSE, (guint32)0)))
    goto error;
  ret_code= ic_mkdir(config_dir.str);
  ic_free(config_dir.str);
  if (ret_code)
    goto error;
  return 0;
error:
  return ret_code;
}
/**
  This method receives the config.ini file and places it in the
  ICLAUSTRON_DATA_DIR/config directory. It overwrites the
  config.ini file if one already existed there.

  @parameter conn                 The connection
*/
static int
handle_copy_config_ini(IC_CONNECTION *conn)
{
  IC_STRING file_name;
  int ret_code;
  guint32 num_lines;
  gchar file_name_buf[IC_MAX_FILE_NAME_SIZE];
  DEBUG_ENTRY("handle_copy_config_ini");

  if ((ret_code= ic_rec_simple_str(conn, ic_receive_config_ini_str)) ||
      (ret_code= ic_rec_number(conn,
                               ic_number_of_lines_str,
                               &num_lines)))
    goto error;

  /* Make sure the config directory is created */
  create_config_dir();
  /* Create the config.ini file name */
  IC_INIT_STRING(&file_name, NULL, 0, TRUE);
  ic_create_config_file_name(&file_name,
                             file_name_buf,
                             NULL,
                             &ic_config_string,
                             0);
  if ((ret_code= handle_receive_file(conn,
                                     NULL,
                                     file_name.str,
                                     NULL,
                                     0,
                                     num_lines)))
    goto error;
  if ((ret_code= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
      (ret_code= ic_send_empty_line(conn)))
    goto error;
error:
  DEBUG_RETURN_INT(ret_code);
}

/**
  This method receives a number of files with their content from the client
  and installs those as configuration files in the proper directory where
  iClaustron expects to find the configuration files.

  This method is atomic, it will either write and create all files sent and
  received or it will remove all the files it created and wrote if anything
  goes wrong before the entire file transfer is completed.

  To aid in this we use a dynamic pointer array.

  @parameter conn           IN: The connection
*/
static int
handle_copy_cluster_server_files(IC_CONNECTION *conn)
{
  int ret_code;
  void *mem_alloc_object;
  guint32 num_lines;
  guint32 num_clusters;
  guint32 i;
  guint32 node_id;
  guint64 num_files= 0;
  IC_STRING file_name;
  gchar file_name_buf[IC_MAX_FILE_NAME_SIZE];
  IC_DYNAMIC_PTR_ARRAY *file_name_array;
  DEBUG_ENTRY("handle_copy_cluster_server_files");

  if (!(file_name_array= ic_create_dynamic_ptr_array()))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);

  /* Receive config.ini */
  if ((ret_code= ic_rec_number(conn,
                               ic_cluster_server_node_id_str,
                               &node_id)) ||
      (ret_code= ic_rec_number(conn,
                               ic_number_of_clusters_str,
                               &num_clusters)) ||
      (ret_code= ic_rec_simple_str(conn, ic_receive_config_ini_str)) ||
      (ret_code= ic_rec_number(conn,
                               ic_number_of_lines_str,
                               &num_lines)))

    goto error_delete_files;

  /* Need to make sure the node directory is created */
  if ((ret_code= ic_set_config_dir(&file_name, TRUE, node_id)))
    goto error_delete_files;
  ret_code= ic_mkdir(file_name.str);
  ic_free(file_name.str);
  IC_INIT_STRING(&file_name, NULL, 0, FALSE);
  if (ret_code)
    goto error_delete_files;

  /* Create the config.ini file name */
  ic_create_config_file_name(&file_name,
                             file_name_buf,
                             NULL,
                             &ic_config_string,
                             0);
  if ((ret_code= handle_receive_file(conn,
                                     file_name_array,
                                     file_name.str,
                                     &num_files,
                                     node_id,
                                     num_lines)))
    goto error_delete_files;
  if ((ret_code= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
      (ret_code= ic_send_empty_line(conn)))
    goto error_delete_files;

  /* Receive grid_common.ini */
  if ((ret_code= ic_rec_simple_str(conn, ic_receive_grid_common_ini_str)) ||
      (ret_code= ic_rec_number(conn, ic_number_of_lines_str, &num_lines)))
    goto error_delete_files;

  /* Create the grid_common.ini file name */
  ic_create_config_file_name(&file_name,
                             file_name_buf,
                             NULL,
                             &ic_grid_common_config_string,
                             0);
  if ((ret_code= handle_receive_file(conn,
                                     file_name_array,
                                     file_name.str,
                                     &num_files,
                                     node_id,
                                     num_lines)))
    goto error_delete_files;
  if ((ret_code= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
      (ret_code= ic_send_empty_line(conn)))
    goto error_delete_files;

  /* Receive all cluster config files */
  for (i= 0; i < num_clusters; i++)
  {
    if ((ret_code= ic_rec_string(conn,
                                 ic_receive_cluster_name_ini_str,
                                 file_name_buf)) ||
        (ret_code= ic_rec_number(conn,
                                 ic_number_of_lines_str,
                                 &num_lines)))
      goto error_delete_files;
    if ((ret_code= handle_receive_file(conn,
                                       file_name_array,
                                       file_name_buf,
                                       &num_files,
                                       node_id,
                                       num_lines)))
      goto error_delete_files;
    if ((ret_code= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
        (ret_code= ic_send_empty_line(conn)))
      goto error_delete_files;
  }
  /* Protocol complete and worked ok, clean up dynamic pointer array */
  file_name_array->dpa_ops.ic_free_dynamic_ptr_array(file_name_array);
  DEBUG_RETURN_INT(0);

error_delete_files:
  for (i= 0; i < num_files; i++)
  {
    ic_require(file_name_array->dpa_ops.ic_get_ptr(file_name_array,
                                                   (guint64)i,
                                                   &mem_alloc_object));
    (void)ic_delete_file((gchar*)mem_alloc_object); /* Ignore error */
    ic_free((gchar*)mem_alloc_object);
  }
  file_name_array->dpa_ops.ic_free_dynamic_ptr_array(file_name_array);
  ic_send_error_message(conn,
                        ic_get_error_message(ret_code));
  DEBUG_RETURN_INT(ret_code);
}

/**
  Handle the protocol to get CPU information

  @parameter conn          IN: The connection
*/
static int
handle_get_cpu_info(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 num_processors;
  guint32 num_cpu_sockets;
  guint32 num_cpu_cores;
  guint32 num_numa_nodes;
  guint32 i;
  IC_CPU_INFO *cpu_info= NULL;
  DEBUG_ENTRY("handle_get_cpu_info");

  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;

  ic_get_cpu_info(&num_processors,
                  &num_cpu_sockets,
                  &num_cpu_cores,
                  &num_numa_nodes,
                  &cpu_info);
  if (num_processors == 0)
  {
    if ((ret_code= ic_send_with_cr(conn, ic_no_cpu_info_available_str)) ||
        (ret_code= ic_send_empty_line(conn)))
      goto error;
    goto error;
  }

  if ((ret_code= ic_send_with_cr_with_number(conn,
                                             ic_number_of_processors_str,
                                             (guint64)num_processors)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_number_of_cpu_sockets_str,
                                             (guint64)num_cpu_sockets)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_number_of_cpu_cores_str,
                                             (guint64)num_cpu_cores)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_number_of_numa_nodes_str,
                                             (guint64)num_numa_nodes)))
    goto error;
  for (i= 0; i < num_processors; i++)
  {
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_processor_str,
                                        (guint64)cpu_info[i].processor_id)))
      goto error;
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_core_str,
                                        (guint64)cpu_info[i].core_id)))
      goto error;
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_cpu_node_str,
                                        (guint64)cpu_info[i].numa_node_id)))
      goto error;
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_socket_str,
                                        (guint64)cpu_info[i].cpu_id)))
      goto error;
  }
  ret_code= ic_send_empty_line(conn);

error:
  if (cpu_info)
    ic_free((gchar*)cpu_info);
  if (ret_code)
    ic_send_error_message(conn,
                          ic_get_error_message(ret_code));
  DEBUG_RETURN_INT(ret_code);
}

/**
  Handle the protocol to get memory information

  @parameter conn                 IN: The connection
*/
static int
handle_get_mem_info(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 num_numa_nodes;
  guint32 i;
  guint32 total_memory_size;
  IC_MEM_INFO *mem_info= NULL;
  DEBUG_ENTRY("handle_get_mem_info");

  if ((ret_code= ic_rec_empty_line(conn)))
    goto error;

  ic_get_mem_info(&num_numa_nodes,
                  &total_memory_size,
                  &mem_info);

  if (num_numa_nodes == 0)
  {
    if ((ret_code= ic_send_with_cr(conn, ic_no_mem_info_available_str)) ||
        (ret_code= ic_send_empty_line(conn)))
      goto error;
    goto error;
  }
  if ((ret_code= ic_send_with_cr_with_number(conn,
                                          ic_number_of_mbyte_user_memory_str,
                                          (guint64)total_memory_size)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_number_of_numa_nodes_str,
                                             (guint64)num_numa_nodes)))
    goto error;
  for (i= 0; i < num_numa_nodes; i++)
  {
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_mem_node_str,
                                         (guint64)mem_info[i].numa_node_id)))
      goto error;
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_mb_user_memory_str,
                                         (guint64)mem_info[i].memory_size)))
      goto error;
  }
  ret_code= ic_send_empty_line(conn);

error:
  if (mem_info)
    ic_free((gchar*)mem_info);
  if (ret_code)
    ic_send_error_message(conn,
                          ic_get_error_message(ret_code));
  DEBUG_RETURN_INT(ret_code);
}

/**
  Handle the protocol to get disk information

  @parameter conn                 IN: The connection
*/
static int
handle_get_disk_info(IC_CONNECTION *conn)
{
  int ret_code;
  guint32 disk_space;
  gchar dir_name_buf[IC_MAX_FILE_NAME_SIZE];
  DEBUG_ENTRY("handle_get_disk_info");

  /* Get directory which we want to analyze how much disk space can be used */
  if ((ret_code= ic_rec_string(conn,
                               ic_dir_str,
                               dir_name_buf)) ||
      (ret_code= ic_rec_empty_line(conn)))
    goto error;

  /* Check disk space now */
  ic_get_disk_info(dir_name_buf, &disk_space);
  if (disk_space == 0)
  {
    /*
      Report that info isn't accessible, we don't report any info on why for
      security reasons. We don't want a user to be able to check the existence
      of a directory if he doesn't have permission to it.
    */
    if ((ret_code= ic_send_with_cr(conn, ic_no_disk_info_available_str)))
      goto error;
  }
  else
  {
    /* Report disk space found available in this directory */
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_disk_space_str,
                                               disk_space)))
      goto error;
  }
  ret_code= ic_send_empty_line(conn);
error:
  if (ret_code)
    ic_send_error_message(conn,
                          ic_get_error_message(ret_code));
  DEBUG_RETURN_INT(ret_code);
}

/**
  Remove all entries from process hash before we remove the hash itself
  as part of clean up in shutting down process controller.

  When we call this all other threads have been stopped, thus we need not
  worry about that memory will go away under our feets. Also there should
  be no new entries to the array of processes under our control while
  this method is executing.

  In the presence of errors here, we clean up as good as we can. This is
  part of the cleanup process, so no need to report errors here. We do
  however report if we failed to stop a process.
*/
static void
clean_process_hash(gboolean stop_processes)
{
  guint64 max_index;
  guint32 i;
  int ret_code;
  void *void_pc_start;
  IC_PC_START *pc_start;
  IC_PC_FIND pc_find;
  DEBUG_ENTRY("clean_process_hash");

  pc_find.mc_ptr= NULL;

  DEBUG_PRINT(COMM_LEVEL, ("Stop processes: %u", stop_processes));
  ic_mutex_lock(pc_hash_mutex);
  max_index= glob_pc_array->dpa_ops.ic_get_max_index(glob_pc_array);
  DEBUG_PRINT(COMM_LEVEL, ("max_index = %llu", max_index));
  for (i= 0; i < max_index; i++)
  {
    if (!(ret_code= glob_pc_array->dpa_ops.ic_get_ptr(glob_pc_array,
                                                      i,
                                                      &void_pc_start)))
    {
      DEBUG_PRINT(COMM_LEVEL, ("Found pc_start entry on index %u", i));
      pc_start= (IC_PC_START*)void_pc_start;
      if (stop_processes && pc_start)
      {
        pc_find.key= pc_start->key;
        ic_assert(pc_start->kill_ongoing == FALSE);
        ic_assert(pc_start->check_ongoing == FALSE);
        ic_mutex_unlock(pc_hash_mutex);
        ret_code= delete_process(&pc_find, FALSE);
        if (ret_code)
        {
          delete_process(&pc_find, TRUE);
        }
        if (ret_code)
        {
          /* Print error message about failure to stop process */
          ic_printf(
            "Failed to stop node grid= %s, cluster= %s, node= %s, pid= %u",
                    pc_start->key.grid_name.str,
                    pc_start->key.cluster_name.str,
                    pc_start->key.node_name.str,
                    (guint32)pc_start->pid);
          ic_print_error(ret_code);
          pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
        }
        ic_mutex_lock(pc_hash_mutex);
      }
      else
      {
        /**
         * We are shutting down and releasing all memory, but we won't
         * stop the processes we've started or are controlling, we
         * probably want to come back later and take back the responsibility
         * of the processes we were in control of.
         */
        if (pc_start)
        {
          pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
        }
      }
    }
  }
  ic_mutex_unlock(pc_hash_mutex);
  DEBUG_RETURN_EMPTY;
}

/**
  Method of thread checking started process every 30 seconds
*/
static gpointer
run_check_thread(gpointer data)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state;
  IC_HASHTABLE_ITR check_itr;
  IC_PC_START *pc_start;
  guint32 i;
  int ret_code;
  guint64 check_time= 0;
  DEBUG_THREAD_ENTRY("run_check_thread");
  tp_state= thread_state->ic_get_threadpool(thread_state);
  tp_state->ts_ops.ic_thread_started(thread_state);
  /* End of thread initialization */

  while (!ic_tp_get_stop_flag())
  {
    ic_mutex_lock(pc_hash_mutex);
    event_occurred= FALSE;
    ic_hashtable_iterator(glob_pc_hash, &check_itr, TRUE);
    while (ic_hashtable_iterator_advance(&check_itr))
    {
      pc_start= (IC_PC_START*)ic_hashtable_iterator_value(&check_itr);
      if (pc_start->check_time == check_time /* Already checked */ ||
          pc_start->pid == 0) /* Process not started yet */
      {
        continue;
      }
      /* Ensures no one removes it until we're done with check */
      pc_start->check_ongoing= TRUE;
      pc_start->check_time= check_time;
      ic_mutex_unlock(pc_hash_mutex);
      /* Check that it is still alive */
      if ((ret_code= ic_is_process_alive(pc_start->pid,
                                         pc_start->program_name.str)))
      {
        /* Process was dead, remove it from hashtable */
        ic_mutex_lock(pc_hash_mutex);
        remove_pc_entry(pc_start);
        ic_mutex_unlock(pc_hash_mutex);
        print_stopped_program(pc_start);
        /* Release memory associated with the deleted process */
        pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
        /*
          We have removed the entry which was used by iterator, we'll
          continue search from same bucket, this will recheck some
          entries possibly, to avoid this we keep track of check time.
        */
        ic_hashtable_iterator(glob_pc_hash, &check_itr, FALSE);
        pc_start= NULL;
      }
      ic_mutex_lock(pc_hash_mutex);
      if (pc_start)
      {
        pc_start->check_ongoing= FALSE;
      }
    }
    ic_mutex_unlock(pc_hash_mutex);
    for (i= 0; i < 6; i++)
    {
      if (event_occurred || ic_tp_get_stop_flag())
      {
        /* Check a bit more often when start/stop events occurred */
        break;
      }
      ic_sleep(5); /* Will also check stop flag every second */
    }
    check_time+= 30;
  }
  tp_state->ts_ops.ic_thread_stops(thread_state);
  DEBUG_THREAD_RETURN;
}

/**
  This method starts the thread that constantly checks whether started
  processes are still alive, if it discovers that a process has failed
  it will remove it from the list of running processes. If the process
  has the autorestart flag set to true it will also initiate a new
  start of the process.

  @parameter tp_state             Threadpool to use for check thread
*/
static int
start_check_thread(IC_THREADPOOL_STATE *tp_state)
{
  guint32 thread_id;
  int ret_code;
  DEBUG_ENTRY("start_check_thread");

  if ((ret_code= tp_state->tp_ops.ic_threadpool_start_thread(tp_state,
                                                  &thread_id,
                                                  run_check_thread,
                                                  NULL,
                                                  IC_MEDIUM_STACK_SIZE,
                                                  FALSE)))
    DEBUG_RETURN_INT(ret_code);
  DEBUG_RETURN_INT(0);
}

/**
  The command handler is a thread that is executed on behalf of one of the
  Cluster Managers in a Grid.

  The Cluster Manager can ask the iClaustron Process Controller for 5 tasks.
  1) It can ask to get a node started
  2) It can ask to stop a node gracefully (kill)
  3) It can ask to stop a node in a hard manner (kill -9)
  4) It can also ask for a list of the programs currently running in the Grid
  5) As part of the bootstrap it can be asked to install the configuration
     files of the Cluster Servers.

  GENERIC PROTOCOL
  ----------------
  The protocol between the iClaustron Process Controller and the Cluster
  Manager is entirely half-duplex and it is always the Cluster Manager
  asking for services from the Process Controller. The only action the
  Process Controller performs automatically is to restart a program if
  it discovers it has died when the autorestart flag is set to true in
  the start command.

  Cluster Servers use the Cluster Name * to indicate that they are part
  of all clusters inside a grid. The same goes for Cluster Managers.

  START PROTOCOL
  --------------
  The protocol for starting node is:
  Line 1: start
  Line 2: program: Program Name
  Line 3: version: Version string
          (either an iClaustron version or a MySQL version string)
  Line 4: grid: Grid Name
  Line 5: cluster: Cluster Name
  Line 6: node: Node name
  Line 7: autorestart true OR autorestart false
  Line 8: num parameters: Number of parameters
  Line 9 - Line x: Program Parameters on the format:
    parameter: Parameter Key/Value
  Line x+1: Empty line to indicate end of message

  Example: start\nprogram: ic_fsd\nversion: iclaustron-0.0.1\n
           grid: my_grid\ncluster: my_cluster\nnode: my_csd_node\n
           autorestart: false\nnum parameters: 2\n
           parameter: node_id\nparameter: 1\n\n

  Most parameters are sent as two parameters with a key and a value
  parameter.

  The successful response is:
  Line 1: Ok
  Line 2: pid: Process Id
  Line 3: Empty line

  Example: ok\npid: 1234\n\n

  In the special case where the process is already running we report this
  as a success with an extra line added before the empty line that says:
  Line 3: Process already started
  Line 4: Empty Line

  The unsuccessful response is:
  Line 1: Error
  Line 2: error: message
  Line 3: empty line

  Example: error:\nerror: Failed to start process\n\n

  STOP/KILL PROTOCOL
  ------------------
  The protocol for stopping/killing a node is:
  Line 1: stop/kill
  Line 2: grid: Grid Name
  Line 3: cluster: Cluster Name
  Line 4: node: Node name
  Line 5: Empty line

  The successful response is:
  Line 1: ok
  Line 2: Empty Line

  The unsuccessful response is:
  Line 1: error
  Line 2: error: message

  LIST PROTOCOL
  -------------
  The protocol for listing nodes is:
  Line 1: list [full]
  Line 2: grid: Grid Name (optional)
  Line 3: cluster: Cluster Name (optional)
  Line 4: node: Node Name (optional)
  Line 5: Empty Line

  Only Grid Name is required, if Cluster Name is provided, only programs in
  this cluster will be provided, if Node Name is also provided then only a
  program running this node will be provided.

  The response is a set of program descriptions as:
  NOTE: It is very likely this list will be expanded with more data on the
  state of the process, such as use of CPU, use of memory, disk and so forth.
  Line 1: list node
  Line 2: program: Program Name
  Line 3: version: Version string
  Line 4: grid: Grid Name
  Line 5: cluster: Cluster Name
  Line 6: node: Node Name
  Line 7: start time: Start Time
  Line 8: pid: Process Id
  Line 9: Number of parameters
  Line 10 - Line x: Parameter in same format as when starting (only if list
    full)
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

  INSTALL CONFIGURATION FILES PROTOCOL
  ------------------------------------
  Line 1: copy cluster server files
  Line 2: cluster server node id: #node
  Line 3: number of clusters: #clusters
  Line 4: receive config.ini
  Line 5: number of lines: #lines
  Line 6 - Line x: Contains the data from the config.ini file
  Line x+1: Empty Line

  Response:
  Line 1: receive config file ok
  Line 2: Empty Line

  As usual in the iClaustron Protocol an empty lines indicates end of
  the communication from one side. This is assumed in descriptions here
  below.

  Then the next file is copied with the same protocol but replacing
  config.ini by grid_common.ini.

  Then the protocol is repeated once per cluster to install where the
  filename is cluster_name.ini, so e.g. kalle.ini if the name of the
  cluster is kalle.

  After receiving the last file the server waits for other protocol
  messages or that the client closes the connection.

  GET CPU INFO PROTOCOL
  ---------------------
  This protocol asks the iClaustron Process Controller about the number
  of CPUs, how many cores, how many threads and how many NUMA nodes there
  are in the computer. If the process runs on a virtual machine it is the
  resources this virtual machine have access to which is asked for.

  The information provided here is 3 levels of CPU information, it is
  assumed that CPUs consists of NUMA nodes which usually is equal to
  CPU chips and often also a CPU socket. Each NUMA node have a set of
  CPU cores. Finally each CPU core can have multiple CPU threads. We
  call the lowest level cpu here, there are different names used for it
  in different products.

  We report the number of cpus, number of NUMA nodes and number of cpus
  per core. Then the mapping is provided by specifying cpu id together
  with information onto which NUMA node and CPU core this is related.

  The receiver of this information can use this information to discover
  how to bind iClaustron processes to the correct set of CPUs. It can
  also bind memory allocation to a certain set of NUMA nodes for the
  iClaustron programs.

  This information is valuable for any configurator program that tries to
  assist the user in providing a reasonable configuration based on the HW
  the user have provided.

  Line 1: get cpu info

  Response:
  Line 1: number of cpus: #cpus
  Line 2: number of NUMA nodes: #nodes
  Line 3: number of cpus per core: #cpus_per_core
  Line 4 - (3 + #cpus): cpu: 0, node: 0, core: 0

  GET MEMORY INFORMATION PROTOCOL
  -------------------------------
  This protocol provides the ability to ask the iClaustron Process Controller
  about how much user memory is available to the iClaustron programs on the
  computer, it also lists this information per NUMA node.

  Line 1: get memory info

  Response:
  Line 1: number of MByte user memory: #MB_user_memory
  Line 2: number of NUMA nodes: #nodes
  Line 3 - (2 + #nodes): node: 0, MB user memory: #MB_user_memory_in_node

  GET DISK INFORMATION PROTOCOL
  -----------------------------
  This protocol provides the ability to ask the iClaustron Process Controller
  about 
  Line 1: get disk info
  Line 2: dir: @directory

  Response:
  Line 1: disk space: #GBytes

  Or:
  Line 1: error string: @error_string
*/

/**
  Handle a process controller client connection

  @parameter data             Contains reference from which we get connection
*/
static gpointer
run_command_handler(gpointer data)
{
  gchar *read_buf;
  guint32 read_size;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state;
  IC_CONNECTION *conn;
  int ret_code;
  DEBUG_THREAD_ENTRY("run_command_handler");
  tp_state= thread_state->ic_get_threadpool(thread_state);
  conn= (IC_CONNECTION*)
    tp_state->ts_ops.ic_thread_get_object(thread_state);

  tp_state->ts_ops.ic_thread_started(thread_state);

  while (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!ic_check_buf(read_buf, read_size, ic_start_str,
                      strlen(ic_start_str)))
      ret_code= handle_start(conn);
    else if (!ic_check_buf(read_buf, read_size, ic_stop_str,
                           strlen(ic_stop_str)))
      ret_code= handle_stop(conn, FALSE);
    else if (!ic_check_buf(read_buf, read_size, ic_kill_str,
                           strlen(ic_kill_str)))
      ret_code= handle_stop(conn, TRUE);
    else if (!ic_check_buf(read_buf, read_size, ic_list_str,
                           strlen(ic_list_str)))
      ret_code= handle_list(conn, FALSE);
    else if (!ic_check_buf(read_buf, read_size, ic_list_full_str,
                           strlen(ic_list_full_str)))
      ret_code= handle_list(conn, TRUE);
    else if (!ic_check_buf(read_buf, read_size,
                           ic_copy_cluster_server_files_str,
                           strlen(ic_copy_cluster_server_files_str)))
      ret_code= handle_copy_cluster_server_files(conn);
    else if (!ic_check_buf(read_buf, read_size,
                           ic_copy_config_ini_str,
                           strlen(ic_copy_config_ini_str)))
      ret_code= handle_copy_config_ini(conn);
    else if (!ic_check_buf(read_buf, read_size,
                           ic_get_cpu_info_str,
                           strlen(ic_get_cpu_info_str)))
      ret_code= handle_get_cpu_info(conn);
    else if (!ic_check_buf(read_buf, read_size,
                           ic_get_mem_info_str,
                           strlen(ic_get_mem_info_str)))
      ret_code= handle_get_mem_info(conn);
    else if (!ic_check_buf(read_buf, read_size,
                           ic_get_disk_info_str,
                           strlen(ic_get_disk_info_str)))
      ret_code= handle_get_disk_info(conn);
    else if (read_size)
    {
      read_buf[read_size]= 0;
      ic_printf("Received a message not expected: %s", read_buf);
      ret_code= IC_PROTOCOL_ERROR;
      /* Closing connection */
      break;
    }
    if (ret_code == IC_PROTOCOL_ERROR ||
        ret_code == IC_ERROR_MEM_ALLOC ||
        (ret_code < IC_FIRST_ERROR && ret_code != 0))
    {
      /*
        We cannot recover from protocol error and in the case of out of memory
        we simply want to decrease the pressure on the system by
        disconnecting. Also OS errors and other errors we won't continue after.
        We will however continue after a correctly sent message which was
        declined because of an environmental error, such as attempting to start
        a process already alive.
      */
      break;
    }
  }
  if (ret_code)
  {
    if (ret_code != IC_END_OF_FILE)
    {
      /*
        We don't print any errors when other side closed connection.
        Closing the connection in this position is normal behaviour.
      */
      ic_print_error(ret_code);
    }
    DEBUG_PRINT(PROGRAM_LEVEL,
      ("Command handler exit, error %d", ret_code));
  }
  conn->conn_op.ic_free_connection(conn);
  tp_state->ts_ops.ic_thread_stops(thread_state);
  DEBUG_THREAD_RETURN;
}

/**
  Client connection factory. This method sets up a server connection,
  listens to new connections on this connection and spawns off a new
  thread to handle the new connection until the connection is closed
  when also the thread is closed.

  @parameter tp_state         The thread pool object to create threads from
*/
int start_connection_loop(IC_THREADPOOL_STATE *tp_state)
{
  int ret_code;
  guint32 thread_id;
  IC_CONNECTION *conn= NULL;
  IC_CONNECTION *fork_conn;
  DEBUG_ENTRY("start_connection_loop");

  if (!(conn= ic_create_socket_object(FALSE, FALSE, FALSE,
                                      CONFIG_READ_BUF_SIZE,
                                      NULL, NULL)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);

  conn->conn_op.ic_prepare_server_connection(conn,
                                             glob_server_name,
                                             glob_server_port,
                                             NULL,
                                             NULL,
                                             0,
                                             TRUE);

  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);

  if (ret_code)
  {
    conn->conn_op.ic_free_connection(conn);
    DEBUG_RETURN_INT(ret_code);
  }
  do
  {
    do
    {
      tp_state->tp_ops.ic_threadpool_check_threads(tp_state);
      /* Wait for someone to connect to us. */
      if ((ret_code= conn->conn_op.ic_accept_connection(conn)))
      {
        if (ret_code != IC_ERROR_APPLICATION_STOPPED)
        {
          /*
            This error can occur simply because the client decided to
            close the connection before we had a chance to accept the
            connection.
          */
          ic_printf("Error %d received in accept connection", ret_code);
          ic_print_error(ret_code);
          DEBUG_PRINT(COMM_LEVEL, ("Error %d received in accept connection",
                                   ret_code));
          continue;
        }
        else
          break;
      }
      if (!(fork_conn= 
            conn->conn_op.ic_fork_accept_connection(conn,
                                        FALSE)))  /* No mutex */
      {
        ic_printf("Error occurred in fork of an accepted connection");
        break;
      }
      /*
        We have an active connection, we'll handle the connection in a
        separate thread.
      */
      if (tp_state->tp_ops.ic_threadpool_start_thread(tp_state,
                                                      &thread_id,
                                                      run_command_handler,
                                                      fork_conn,
                                                      IC_MEDIUM_STACK_SIZE,
                                                      FALSE))
      {
        ic_printf("Failed to create thread after forking accept connection");
        fork_conn->conn_op.ic_free_connection(fork_conn);
        break;
      }
    } while (0);
  } while (!ic_tp_get_stop_flag());
  ic_printf("iClaustron Process Controller was stopped");
  conn->conn_op.ic_free_connection(conn);
  DEBUG_RETURN_INT(0);
}

/**
  Main function of ic_pcntrld program

  @parameter argc            Number of arguments in start of program
  @parameter argv            Array of strings where arguments are found
*/
int main(int argc, char *argv[])
{
  int ret_code= 0;
  IC_THREADPOOL_STATE *tp_state= NULL;
  guint32 dummy;
  gchar *pid_str;
  gchar *program_name= "ic_pcntrld";
  gchar pid_buf[IC_NUMBER_SIZE];
 
  if ((ret_code= ic_start_program(argc,
                                  argv,
                                  entries,
                                  NULL,
                                  program_name,
                                  start_text,
                                  TRUE,
                                  FALSE)))
  {
    return ret_code;
  }
  if (glob_daemonize)
  {
    ic_free(ic_glob_stdout_file.str);
    ic_free(ic_glob_pid_file.str);
    ic_free(ic_glob_working_dir.str);

    IC_INIT_STRING(&ic_glob_stdout_file, NULL, 0, TRUE);
    IC_INIT_STRING(&ic_glob_pid_file, NULL, 0, TRUE);

    if ((ret_code= ic_set_out_file(&ic_glob_stdout_file,
                                   ic_glob_node_id,
                                   program_name,
                                   FALSE,
                                   TRUE)))
      goto error;

    if ((ret_code= ic_set_out_file(&ic_glob_pid_file,
                                   ic_glob_node_id,
                                   program_name,
                                   FALSE,
                                   FALSE)))
      goto error;
 
    if ((ret_code= ic_set_working_dir(&ic_glob_working_dir,
                                      ic_glob_node_id,
                                      FALSE)))
      goto error;

    ic_set_umask();
    if ((ret_code= ic_setup_workdir(ic_glob_working_dir.str)))
      goto error;
    if ((ret_code= ic_setup_stdout(ic_glob_stdout_file.str)))
      goto error;
    if ((ret_code= ic_write_pid_file(ic_glob_pid_file.str)))
      goto error;
    pid_str= ic_guint64_str(ic_get_own_pid(), pid_buf, &dummy);
    ic_printf("Starting ic_pcntrld program with pid %s",
              pid_str);
  }

  DEBUG_PRINT(PROGRAM_LEVEL, ("Base directory: %s", ic_glob_base_dir.str));
  DEBUG_PRINT(PROGRAM_LEVEL, ("Data directory: %s", ic_glob_data_dir.str));
  DEBUG_PRINT(PROGRAM_LEVEL, ("Binary directory: %s", ic_glob_binary_dir.str));
  DEBUG_PRINT(PROGRAM_LEVEL, ("Config directory: %s", ic_glob_config_dir.str));

  if (!(tp_state= ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE, TRUE)))
  {
    return IC_ERROR_MEM_ALLOC;
  }
  if (!(glob_pc_hash= create_pc_hash()))
    goto error;
  if (!(pc_hash_mutex= ic_mutex_create()))
    goto error;
  if (!(glob_pc_array= ic_create_dynamic_ptr_array()))
    goto error;
#ifdef WITH_UNIT_TEST
  if (glob_unit_test && (ret_code= start_unit_test(tp_state)))
    goto error;
#endif
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
    ic_glob_base_dir. We set up the default strings for the
    version we've compiled, the protocol to the process controller
    enables the Cluster Manager to specify which version to use.

    This program will have it's state in memory but will also checkpoint this
    state to a file. This is to ensure that we are able to connect to processes
    again even after we had to restart this program. This file is stored in
    the iclaustron_data placed beside the iclaustron_install directory. The
    file is called pcntrl_state.

    Next step is to wait for Cluster Managers to connect to us, after they
    have connected they can request action from us as well. Any server can
    connect as a Cluster Manager, but they have to provide the proper
    password in order to get connected.

    We will always be ready to receive new connections.

    The manner to stop this program is by performing a kill -15 operation
    such that the program receives a SIGKILL signal. This program cannot
    be started and stopped from a Cluster Manager for security reasons.
  */
  if ((ret_code= start_check_thread(tp_state)))
    goto error;
  ret_code= start_connection_loop(tp_state);

error:
  DEBUG_PRINT(PROGRAM_LEVEL, ("Exiting: err: %d", ret_code));
  ic_set_stop_flag();
  if (tp_state)
  {
    tp_state->tp_ops.ic_threadpool_stop(tp_state);
  }
  if (glob_pc_hash &&
      pc_hash_mutex &&
      glob_pc_array)
  {
    /**
     * Unless we succeeded in creating all components can there be
     * entries in the hash table.
     */
    clean_process_hash(shutdown_processes_flag);
  }
  if (glob_pc_hash)
  {
    ic_hashtable_destroy(glob_pc_hash, FALSE);
  }
  if (pc_hash_mutex)
  {
    ic_mutex_destroy(&pc_hash_mutex);
  }
  if (glob_pc_array)
  {
    glob_pc_array->dpa_ops.ic_free_dynamic_ptr_array(glob_pc_array);
  }
  ic_end();
  return ret_code;
}
