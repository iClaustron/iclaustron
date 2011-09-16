/* Copyright (C) 2007-2011 iClaustron AB

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
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_readline.h>
#include <ic_lex_support.h>
#include <ic_protocol_support.h>
#include <ic_proto_str.h>
#include <ic_pcntrl_proto.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_boot_int.h"

static const gchar *glob_process_name= "ic_bootstrap";
static gchar *glob_command_file= NULL;
static gchar *glob_generate_command_file= NULL;
static guint32 glob_interactive= 0;
static guint32 glob_history_size= 100;
static guint32 glob_connect_timer= 10;
static gchar *ic_prompt= "iClaustron bootstrap> ";
static IC_CLUSTER_CONNECT_INFO **glob_clu_infos= NULL;
static IC_CLUSTER_CONFIG *glob_grid_cluster= NULL;
static IC_CLUSTER_CONFIG *glob_clusters[IC_MAX_CLUSTER_ID + 1];
static guint32 glob_num_clusters= 0;

static GOptionEntry entries[]=
{
  { "interactive", 0, 0, G_OPTION_ARG_INT,
    &glob_interactive,
    "Run the program interactively", NULL},
  { "command-file", 0, 0, G_OPTION_ARG_STRING,
    &glob_command_file,
    "Use a command file and provide its name", NULL},
  { "generate-command-file", 0, 0, G_OPTION_ARG_STRING,
    &glob_generate_command_file,
    "Generate a command file from config files, provide its name", NULL},
  { "history_size", 0, 0, G_OPTION_ARG_INT, &glob_history_size,
    "Set Size of Command Line History", NULL},
  { "connect-timer", 0, 0, G_OPTION_ARG_INT, &glob_connect_timer,
    "Set timeout in seconds to wait for connect", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static int
generate_command_file(gchar *parse_buf, const gchar *command_file)
{
  int ret_code;
  int len;
  guint32 node_id;
  IC_FILE_HANDLE cmd_file_handle;
  IC_CLUSTER_SERVER_CONFIG *cs_conf;
  IC_CLUSTER_MANAGER_CONFIG *mgr_conf;
  DEBUG_ENTRY("generate_command_file");

  /* Create file to store generated file */
  if ((ret_code= ic_create_file(&cmd_file_handle,
                                command_file)))
    goto end;

  /* grid_cluster can now be used to generate the command file */
  for (node_id = 1; node_id <= glob_grid_cluster->max_node_id; node_id++)
  {
    if (glob_grid_cluster->node_types[node_id] == IC_CLUSTER_SERVER_NODE)
    {
      /* Found a Cluster Server, now generate a command line */
      cs_conf= (IC_CLUSTER_SERVER_CONFIG*)
        glob_grid_cluster->node_config[node_id];
      /*
        Generate a command line like this:
        PREPARE CLUSTER SERVER HOST=hostname
                               PCNTRL_HOST=hostname
                               PCNTRL_PORT=port_number
                               CS_INTERNAL_PORT=port_number
                               NODE_ID=node_id;
      */
      len= g_snprintf(parse_buf,
                      COMMAND_READ_BUF_SIZE,
                      "PREPARE CLUSTER SERVER HOST=%s "
                      "PCNTRL_HOST=%s "
                      "PCNTRL_PORT=%u "
                      "NODE_ID=%u;\n",
                      cs_conf->hostname,
                      cs_conf->pcntrl_hostname,
                      cs_conf->pcntrl_port,
                      node_id);
      if ((ret_code= ic_write_file(cmd_file_handle,
                                   parse_buf,
                                   len)))
        goto late_end;
    }
    if (glob_grid_cluster->node_types[node_id] == IC_CLUSTER_MANAGER_NODE)
    {
      /* Found a Cluster Manager, now generate a command line */
      mgr_conf= (IC_CLUSTER_MANAGER_CONFIG*)
        glob_grid_cluster->node_config[node_id];
      /*
        Generate a command line like this:
        PREPARE CLUSTER MANAGER HOST=hostname
                                PCNTRL_HOST=hostname
                                PCNTRL_PORT=port_number
                                CS_INTERNAL_PORT=port_number
                                NODE_ID=node_id;
      */
      len= g_snprintf(parse_buf,
                      COMMAND_READ_BUF_SIZE,
                      "PREPARE CLUSTER MANAGER HOST=%s "
                      "PCNTRL_HOST=%s "
                      "PCNTRL_PORT=%u "
                      "NODE_ID=%u;\n",
                      mgr_conf->client_conf.hostname,
                      mgr_conf->client_conf.pcntrl_hostname,
                      mgr_conf->client_conf.pcntrl_port,
                      node_id);
      if ((ret_code= ic_write_file(cmd_file_handle,
                                   parse_buf,
                                   len)))
        goto late_end;
    }
  }
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "SEND FILES;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "START CLUSTER SERVERS;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "VERIFY CLUSTER SERVERS;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "START CLUSTER MANAGERS;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;

late_end:
  (void)ic_close_file(cmd_file_handle);
  if (ret_code)
    (void)ic_delete_file(command_file);
end:
  DEBUG_RETURN_INT(ret_code);
}

static void
ic_prepare_cluster_server_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_prepare_cluster_server_cmd");
  if (parse_data->next_cs_index >= IC_MAX_CLUSTER_SERVERS)
  {
    ic_printf("Defined too many Cluster Servers");
    parse_data->exit_flag= TRUE;
    DEBUG_RETURN_EMPTY;
  }
  ic_printf("Prepared cluster server with node id %u",
    parse_data->cs_data[parse_data->next_cs_index - 1].node_id);
  DEBUG_RETURN_EMPTY;
}

static void
ic_prepare_cluster_manager_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_prepare_cluster_manager_cmd");
  if (parse_data->next_mgr_index >= IC_MAX_CLUSTER_SERVERS)
  {
    ic_printf("Defined too many Cluster Managers");
    parse_data->exit_flag= TRUE;
    DEBUG_RETURN_EMPTY;
  }
  ic_printf("Prepared cluster manager with node id %u",
    parse_data->mgr_data[parse_data->next_mgr_index - 1].node_id);
  DEBUG_RETURN_EMPTY;
}

/**
  This method is checked every second from connect, we can avoid waiting
  forever by returning 1 from here.
*/
int
timeout_check(void *object, int timer)
{
  DEBUG_ENTRY("timeout_check");
  DEBUG_PRINT(COMM_LEVEL, ("timer = %d", timer));
  (void)object;
  if (timer < (int)glob_connect_timer)
    DEBUG_RETURN_INT(0);
  DEBUG_RETURN_INT(1);
}

/**
  Start a new client socket connection

  @parameter conn         OUT: The new connection created
  @parameter hostname     IN: The hostname to connect to
  @parameter port         IN: The port to connect to
*/
static int start_client_connection(IC_CONNECTION **conn,
                                   gchar *server_name,
                                   guint32 server_port)
{
  int ret_code;
  gchar *server_port_str;
  gchar buf[IC_NUMBER_SIZE];
  guint32 dummy;
  DEBUG_ENTRY("start_client_connection");

  if (!(*conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                      CONFIG_READ_BUF_SIZE,
                                      NULL, NULL)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  server_port_str= ic_guint64_str((guint64)server_port, buf, &dummy);
  (*conn)->conn_op.ic_prepare_client_connection(*conn,
                                                server_name,
                                                server_port_str,
                                                NULL, NULL);
  ret_code= (*conn)->conn_op.ic_set_up_connection(*conn, timeout_check, NULL);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Send configuration files to a cluster server through the ic_pcntrld
  agent process.

  @parameter conn              IN: The connection to the process controller
  @parameter clu_infos         IN: An array through which we get cluster names
  @parameter node_id           IN: Node id of cluster
*/
static int
send_files_to_node(IC_CONNECTION *conn,
                   IC_CLUSTER_CONNECT_INFO **clu_infos,
                   guint32 node_id)
{
  int ret_code;
  guint32 num_clusters= 0;
  guint32 i;
  IC_STRING current_dir;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  gchar buf[IC_MAX_FILE_NAME_SIZE];
  IC_STRING cluster_file_name;
  DEBUG_ENTRY("send_files_to_node");

  ic_set_current_dir(&current_dir);
  while (clu_infos[num_clusters])
    num_clusters++; /* Count number of clusters looking for end NULL */

  if ((ret_code= ic_send_with_cr(conn, ic_copy_cluster_server_files_str)) ||
      (ret_code= ic_send_with_cr_with_num(conn,
                                          ic_cluster_server_node_id_str,
                                          (guint64)node_id)) ||
       (ret_code= ic_send_with_cr_with_num(conn,
                                           ic_number_of_clusters_str,
                                           (guint64)num_clusters)))
    goto error;

  if ((ret_code= ic_proto_send_file(conn,
                                    "config.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)) ||
      (ret_code= ic_proto_send_file(conn,
                                    "grid_common.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)))
    goto error;

  for (i= 0; i < num_clusters; i++)
  {
    clu_info= clu_infos[i];
    /* Create cluster_name.ini string */
    buf[0]= 0;
    IC_INIT_STRING(&cluster_file_name, buf, 0, TRUE);
    ic_add_ic_string(&cluster_file_name, &clu_info->cluster_name);
    ic_add_ic_string(&cluster_file_name, &ic_config_ending_string);
    if ((ret_code= ic_proto_send_file(conn,
                                      cluster_file_name.str,
                                      current_dir.str)) ||
        (ret_code= ic_receive_config_file_ok(conn, TRUE)))
      goto error;
  }
error:
  DEBUG_RETURN_INT(ret_code);
}

/**
  Copy config.ini to a node where it is needed
*/
static int
node_copy_config_ini(IC_DATA_SERVER_CONFIG *node_conf)
{
  IC_STRING current_dir;
  int ret_code;
  IC_CONNECTION *conn= NULL;
  DEBUG_ENTRY("node_copy_config_ini");

  if ((ret_code= start_client_connection(&conn,
                                         node_conf->pcntrl_hostname,
                                         node_conf->pcntrl_port)))
    goto error;
  ic_set_current_dir(&current_dir);
  if ((ret_code= ic_send_with_cr(conn, ic_copy_config_ini_str)) ||
      (ret_code= ic_proto_send_file(conn,
                                    "config.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)))
    goto error;
error:
  if (conn)
    conn->conn_op.ic_free_connection(conn);
  DEBUG_RETURN_INT(ret_code);
}
/**
  Copy config.ini to all nodes in a cluster where it is needed
*/
static int
cluster_copy_config_ini(IC_CLUSTER_CONFIG *clu_conf)
{
  guint32 i;
  int ret_code;
  DEBUG_ENTRY("cluster_copy_config_ini");

  for (i= 0; i <= clu_conf->max_node_id; i++)
  {
    if (clu_conf->node_config[i] &&
        (ret_code= node_copy_config_ini(
          (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[i])))
      break;
  }
  DEBUG_RETURN_INT(ret_code);
}

/**
  Copy config.ini to all nodes where it is needed
*/
static int
copy_config_ini(void)
{
  guint32 i;
  int ret_code;
  DEBUG_ENTRY("copy_config_ini");

  if ((ret_code= cluster_copy_config_ini(glob_grid_cluster)))
    goto error;
  for (i= 0; i < glob_num_clusters; i++)
  {
    if ((ret_code= cluster_copy_config_ini(glob_clusters[i])))
      goto error;
  }
error:
  DEBUG_RETURN_INT(ret_code);
}

static void
ic_send_files_cmd(IC_PARSE_DATA *parse_data)
{
  IC_CONNECTION *conn= NULL;
  IC_CLUSTER_SERVER_DATA *cs_data;
  int ret_code;
  guint32 i;
  DEBUG_ENTRY("ic_send_files_cmd");

  (void)parse_data;
  if (parse_data->next_cs_index == 0)
  {
    ic_printf("No Cluster Servers prepared");
    goto error;
  }
  if ((ret_code= copy_config_ini()))
    goto error;
  for (i= 0; i < parse_data->next_cs_index; i++)
  {
    cs_data= &parse_data->cs_data[i];
    /* Send files to all cluster servers one by one */
    if ((ret_code= start_client_connection(&conn,
      cs_data->pcntrl_hostname,
      cs_data->pcntrl_port)))
    {
      ic_printf("Failed to open connection to cluster server id %u",
        cs_data->node_id);
      ic_print_error(ret_code);
      goto error;
    }
    if ((ret_code= send_files_to_node(conn,
                                      glob_clu_infos,
                                      cs_data->node_id)))
    {
      ic_printf("Failed to copy files to cluster server id %u",
        cs_data->node_id);
      ic_print_error(ret_code);
      goto end;
    }
    conn->conn_op.ic_free_connection(conn);
    conn= NULL;
  }
  ic_printf("Copied configuration files to all cluster servers");
end:
  if (conn)
    conn->conn_op.ic_free_connection(conn);
  DEBUG_RETURN_EMPTY;
error:
  parse_data->exit_flag= TRUE;
  goto end;
}

static int
ic_send_debug_level(IC_CONNECTION *conn)
{
#ifdef DEBUG_BUILD
  int ret_code;
  /* If we're debugging the bootstrap we'll also debug the started process */
  if (ic_get_debug())
  {
    if ((ret_code= ic_send_with_cr_two_strings(conn,
                                               ic_parameter_str,
                                               ic_debug_level_str)) ||
        (ret_code= ic_send_with_cr_with_num(conn,
                                            ic_parameter_str,
                                            (guint64)ic_get_debug())))
      return ret_code;
    if (ic_get_debug_timestamp())
    {
      /* Also debug with timestamps if bootstrap is timestamped */
      if ((ret_code= ic_send_with_cr_two_strings(conn,
                                                 ic_parameter_str,
                                                 ic_debug_timestamp_str)) ||
          (ret_code= ic_send_with_cr_with_num(conn,
                                        ic_parameter_str,
                                        (guint64)ic_get_debug_timestamp())))
        return ret_code;
      
    }
  }
#else
  (void)conn;
#endif
  return 0;
}

static int
start_cluster_manager(IC_CONNECTION *conn,
                      IC_CLUSTER_MANAGER_DATA *mgr_data,
                      IC_CLUSTER_MANAGER_CONFIG *mgr_conf)
{
  int ret_code;
  gchar buf[IC_NUMBER_SIZE + 4];
  gchar err_buf[ERROR_MESSAGE_SIZE];
  gchar *node_str;
  guint32 dummy;
  guint32 pid;
  gboolean found;
  guint64 num_params;
  DEBUG_ENTRY("start_cluster_manager");

  node_str= ic_guint64_str((guint64)mgr_data->node_id, buf, &dummy);
  /* Start a cluster server */
  if ((ret_code= ic_send_with_cr(conn, ic_start_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_program_str,
                                   ic_cluster_manager_program_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_version_str,
                                             IC_VERSION_STR)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_grid_str,
                                             IC_GRID_STR)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_cluster_str,
                                   glob_clu_infos[0]->cluster_name.str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_node_str,
                                             node_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_auto_restart_str,
                                             ic_false_str)))
    goto end;
#ifdef DEBUG_BUILD
  num_params= ic_get_debug() ? (guint64)8 : (guint64)6;
#else
  num_params= 6;
#endif
  if ((ret_code= ic_send_with_cr_with_num(conn,
                                          ic_num_parameters_str,
                                          num_params)) ||
      (ret_code= ic_send_debug_level(conn)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_node_parameter_str)) ||
      (ret_code= ic_send_with_cr_with_num(conn,
                                          ic_parameter_str,
                                          (guint64)mgr_data->node_id)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_server_name_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                   mgr_conf->client_conf.hostname)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_server_port_str)) ||
      (ret_code= ic_send_with_cr_with_num(conn,
                                          ic_parameter_str,
                                   mgr_conf->cluster_manager_port_number)) ||
      (ret_code= ic_send_empty_line(conn)))
    goto end;
  if ((ret_code= ic_rec_simple_str_opt(conn, ic_ok_str, &found)))
    goto end;
  if (!found)
  {
    /* Not ok, expect error message instead */
    if (!ic_receive_error_message(conn, err_buf))
      ic_printf("Error: %s", err_buf);
    ret_code= IC_ERROR_FAILED_TO_START_PROCESS;
    goto end;
  }
  if ((ret_code= ic_rec_number(conn, ic_pid_str, &pid)) ||
      (ret_code= ic_rec_empty_line(conn)))
    goto end;
  ic_printf("Successfully started Cluster Manager id %u with pid %u"
            ", on host = %s, listening to port number = %u",
            mgr_data->node_id,
            pid,
            mgr_conf->client_conf.hostname,
            mgr_conf->cluster_manager_port_number);
end:
  DEBUG_RETURN_INT(ret_code);
}

static int
start_cluster_server(IC_CONNECTION *conn, IC_CLUSTER_SERVER_DATA *cs_data)
{
  int ret_code;
  gchar buf[IC_NUMBER_SIZE + 4];
  gchar err_buf[ERROR_MESSAGE_SIZE];
  gchar *node_str;
  guint32 dummy;
  guint32 pid;
  guint64 num_params;
  gboolean found;
  gboolean process_already_started= FALSE;
  DEBUG_ENTRY("start_cluster_server");

  node_str= ic_guint64_str((guint64)cs_data->node_id, buf, &dummy);
  /* Start a cluster server */
  if ((ret_code= ic_send_with_cr(conn, ic_start_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_program_str,
                                             ic_cluster_server_program_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_version_str,
                                             IC_VERSION_STR)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_grid_str,
                                             IC_GRID_STR)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_cluster_str,
                                   glob_clu_infos[0]->cluster_name.str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_node_str,
                                             node_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_auto_restart_str,
                                             ic_false_str)))
    goto end;
#ifdef DEBUG_BUILD
  num_params= ic_get_debug() ? (guint64)4 : (guint64)2;
#else
  num_params= (guint64)2;
#endif
  if ((ret_code= ic_send_with_cr_with_num(conn,
                                          ic_num_parameters_str,
                                          num_params)) ||
      (ret_code= ic_send_debug_level(conn)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_node_parameter_str)) ||
      (ret_code= ic_send_with_cr_with_num(conn,
                                          ic_parameter_str,
                                          (guint64)cs_data->node_id)) ||
      (ret_code= ic_send_empty_line(conn)))
    goto end;
  if ((ret_code= ic_rec_simple_str_opt(conn, ic_ok_str, &found)))
    goto end;
  if (!found)
  {
    if ((ret_code= ic_rec_simple_str_opt(conn,
                                         ic_process_already_started_str,
                                         &found)))
      goto end;
    if (!found)
    {
      /* Not ok, expect error message instead */
      if (!ic_receive_error_message(conn, err_buf))
        ic_printf("Error: %s", err_buf);
      ret_code= IC_ERROR_FAILED_TO_START_PROCESS;
      goto end;
    }
    /* Process already started */
    process_already_started= TRUE;
  }
  if ((ret_code= ic_rec_number(conn, ic_pid_str, &pid)) ||
      (ret_code= ic_rec_empty_line(conn)))
    goto end;
  if (process_already_started)
    ic_printf("Cluster Server id %u already started with pid %u",
              cs_data->node_id,
              pid);
  else
    ic_printf("Successfully started Cluster Server id %u with pid %u",
              cs_data->node_id,
              pid);
end:
  DEBUG_RETURN_INT(ret_code);
}

static void
ic_start_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  IC_CLUSTER_SERVER_DATA *cs_data;
  guint32 i;
  int ret_code;
  IC_CONNECTION *conn= NULL;
  DEBUG_ENTRY("ic_start_cluster_servers_cmd");

  if (parse_data->next_cs_index == 0)
  {
    ic_printf("No Cluster Servers prepared");
    goto error;
  }
  for (i= 0; i < parse_data->next_cs_index; i++)
  {
    cs_data= &parse_data->cs_data[i];
    /* Connect to process control and send start and disconnect */
    if ((ret_code= start_client_connection(&conn,
      cs_data->pcntrl_hostname,
      cs_data->pcntrl_port)))
    {
      ic_printf("Failed to open connection to Cluster Server id %u",
        cs_data->node_id);
      ic_printf("Most likely not started ic_pcntrld on host %s at port %u",
                cs_data->pcntrl_hostname,
                cs_data->pcntrl_port);
      ic_print_error(ret_code);
      goto error;
    }
    if ((ret_code= start_cluster_server(conn, cs_data)))
    {
      ic_printf("Failed to start Cluster Server id %u", cs_data->node_id);
      ic_print_error(ret_code);
      goto error;
    }
    conn->conn_op.ic_free_connection(conn);
    conn= NULL;
  }
end:
  if (conn)
    conn->conn_op.ic_free_connection(conn);
  DEBUG_RETURN_EMPTY;
error:
  parse_data->exit_flag= TRUE;
  goto end;
}

static void
ic_start_cluster_managers_cmd(IC_PARSE_DATA *parse_data)
{
  IC_CLUSTER_MANAGER_DATA *mgr_data;
  IC_CONNECTION *conn= NULL;
  guint32 i;
  int ret_code;
  IC_CLUSTER_MANAGER_CONFIG *mgr_conf;
  DEBUG_ENTRY("ic_start_cluster_managers_cmd");

  if (parse_data->next_mgr_index == 0)
  {
    ic_printf("No Cluster Managers prepared");
    goto error;
  }
  for (i= 0; i < parse_data->next_mgr_index; i++)
  {
    mgr_data= &parse_data->mgr_data[i];
    ic_require(glob_grid_cluster->node_types[mgr_data->node_id] ==
               IC_CLUSTER_MANAGER_NODE);
    mgr_conf= (IC_CLUSTER_MANAGER_CONFIG*)
      glob_grid_cluster->node_config[mgr_data->node_id];
    if ((ret_code= start_client_connection(&conn,
      mgr_data->pcntrl_hostname,
      mgr_data->pcntrl_port)))
    {
      ic_printf("Failed to open connection to Cluster Manager id %u",
        mgr_data->node_id);
      ic_printf("Most likely not started ic_pcntrld on host %s at port %u",
                mgr_data->pcntrl_hostname,
                mgr_data->pcntrl_port);
      ic_print_error(ret_code);
      goto error;
    }
    if ((ret_code= start_cluster_manager(conn, mgr_data, mgr_conf)))
    {
      ic_printf("Failed to start Cluster Manager id %u", mgr_data->node_id);
      ic_print_error(ret_code);
      goto error;
    }
    conn->conn_op.ic_free_connection(conn);
    conn= NULL;
  }
end:
  if (conn)
    conn->conn_op.ic_free_connection(conn);
  DEBUG_RETURN_EMPTY;
error:
  parse_data->exit_flag= TRUE;
  goto end;
}

static void
ic_verify_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_verify_cluster_servers_cmd");
  if (parse_data->next_cs_index == 0)
  {
    ic_printf("No Cluster Servers prepared");
    goto error;
  }
  ic_printf("Found verify cluster servers command");
end:
  DEBUG_RETURN_EMPTY;
error:
  parse_data->exit_flag= TRUE;
  goto end;
}

static void
boot_execute(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("boot_execute");
  switch (parse_data->command)
  {
    case IC_PREPARE_CLUSTER_SERVER_CMD:
      ic_prepare_cluster_server_cmd(parse_data);
      break;
    case IC_PREPARE_CLUSTER_MANAGER_CMD:
      ic_prepare_cluster_manager_cmd(parse_data);
      break;
    case IC_SEND_FILES_CMD:
      ic_send_files_cmd(parse_data);
      break;
    case IC_START_CLUSTER_SERVERS_CMD:
      ic_start_cluster_servers_cmd(parse_data);
      break;
    case IC_START_CLUSTER_MANAGERS_CMD:
      ic_start_cluster_managers_cmd(parse_data);
      break;
    case IC_VERIFY_CLUSTER_SERVERS_CMD:
      ic_verify_cluster_servers_cmd(parse_data);
      break;
    default:
      ic_printf("No such command");
      parse_data->exit_flag= TRUE;
      break;
  }
  DEBUG_RETURN_EMPTY;
}

/**
  Initialise temporary parts of parse_data

  @parameter parse_data    IN: Struct to initialise
*/
static void
line_init_parse_data(IC_PARSE_DATA *parse_data)
{
  parse_data->command= IC_NO_SUCH_CMD;
}

static void
execute_command(IC_PARSE_DATA *parse_data,
                gchar *parse_buf,
                const guint32 line_size,
                IC_MEMORY_CONTAINER *mc_ptr)
{
  DEBUG_ENTRY("execute_command");
  if (parse_data->exit_flag)
    goto end;
  line_init_parse_data(parse_data);
  ic_boot_call_parser(parse_buf, line_size, parse_data);
  if (parse_data->exit_flag)
    goto end;
  boot_execute(parse_data);
  if (parse_data->exit_flag)
    goto end;
  /* Release memory allocated by parser, but keep memory container */
  mc_ptr->mc_ops.ic_mc_reset(mc_ptr);
end:
  DEBUG_RETURN_EMPTY;
}

static void
get_line_from_file_content(gchar **file_ptr,
                           gchar *file_end,
                           IC_STRING *ic_str)
{
  gchar parse_char;

  ic_str->str= *file_ptr;
  while (*file_ptr < file_end)
  {
    parse_char= *(*file_ptr);
    (*file_ptr)++; /* Move to next character */
    if (parse_char == CMD_SEPARATOR)
      break;
  }
  ic_str->len= (int)((*file_ptr) - ic_str->str); /* Pointer arithmetics */
  ic_str->is_null_terminated= FALSE;
}

static int
get_line(gchar **file_ptr,
         gchar *file_end,
         gchar *parse_buf,
         guint32 *line_size,
         guint32 *curr_pos,
         guint32 *curr_length,
         gchar **read_buf,
         gboolean *end_of_input)
{
  int ret_code;
  guint32 start_pos= *curr_pos;
  guint32 buf_length= *curr_length;
  IC_STRING ic_str;
  guint32 i;
  gboolean end_of_file= FALSE;

  *end_of_input= FALSE;
  do
  {
    if (ic_unlikely(start_pos == 0))
    {
      if (*file_ptr)
      {
        /* Get next line from buffer read from file */
        get_line_from_file_content(file_ptr, file_end, &ic_str);
        if (ic_str.len + buf_length > COMMAND_READ_BUF_SIZE)
          return IC_ERROR_COMMAND_TOO_LONG;
        /* "Intelligent" memcpy removing CARRIAGE RETURN's */
        for (i= 0; i < ic_str.len; i++)
        {
          if (ic_str.str[i] != CARRIAGE_RETURN)
          {
            parse_buf[buf_length]= ic_str.str[i];
            buf_length++;
          }
        }
        if (*file_ptr == file_end)
          end_of_file= TRUE;
      }
      else
      {
        /* Get next line by reading next command line */
        if ((ret_code= ic_read_one_line(ic_prompt, &ic_str)))
          return ret_code;
        if (ic_str.len + buf_length > COMMAND_READ_BUF_SIZE)
          return IC_ERROR_COMMAND_TOO_LONG;
        memcpy(&parse_buf[buf_length], ic_str.str, ic_str.len);
        buf_length+= ic_str.len;
        if (parse_buf[buf_length - 1] != CMD_SEPARATOR)
        {
          /*
            Always add a space at the end instead of the CARRIAGE RETURN
            when the end of line isn't a ; ending the command. Otherwise
            PREPARE<CR>CLUSTER will be interpreted as PREPARECLUSTER
            instead of as PREPARE CLUSTER which is the correct.
          */
          parse_buf[buf_length]= SPACE_CHAR;
          buf_length++;
        }
        ic_free(ic_str.str);
      }
      *curr_length= buf_length;
    }
    for (i= start_pos; i < buf_length; i++)
    {
      if (ic_unlikely(parse_buf[i] == CMD_SEPARATOR))
      {
        *curr_pos= i + 1;
        goto end;
      }
    }
    /*
      We have read the entire line and haven't found a CMD_SEPARATOR,
      we need to read another line and continue reading until we don't
      fit in the parse buffer or until we find a CMD_SEPARATOR.
    */
    if (buf_length > start_pos)
    {
      /* Move everything to beginning of parse buffer */
      if (start_pos > 0)
      {
        buf_length= *curr_length= buf_length - start_pos;
        memmove(parse_buf, &parse_buf[start_pos], buf_length);
      }
    }
    else
    {
      /* Buffer is empty, start from empty buffer again */
      buf_length= 0;
    }
    start_pos= *curr_pos= 0;
    if (end_of_file)
    {
      /* We've reached the end of file */
      if (buf_length == 0)
      {
        /* No command at end of file is ok, we report end of input */
        *end_of_input= TRUE;
        return 0;
      }
      else
      {
        /* Command without ; at end, this isn't ok */
        return IC_ERROR_NO_FINAL_COMMAND;
      }
    }
  } while (1);

end:
  if (i == start_pos)
  {
    /* An empty command with a single ; signals end of input (also in file) */
    *end_of_input= TRUE;
    return 0;
  }
  *line_size= (*curr_pos - start_pos);
  *read_buf= &parse_buf[start_pos];
  return 0;
}

static gchar *start_text= "\
- iClaustron Bootstrap program\n\
\n\
This program runs in interactive mode when --interactive=1\n\
is set as parameter.\n\
To finish program in this mode, enter a single command ;\n\
Commands are always separated by ;\n\
The program can also run from a command file provided in the\n\
--command-file parameter.\n\
Finally the program can also run from a directory where\n\
iClaustron configuration files are provided. The main file will\n\
always be called config.ini and the file grid_common.ini will\n\
will always contain the definition of the common Cluster Servers\n\
and Cluster Managers in the Grid. The remaining files will be one\n\
for each cluster with the name as given in config.ini.\n\
Based on this configuration the program will generate a bootstrap\n\
command file and also execute it.\n\
The generation of a command file is done when the option\n\
--generate-command-file is given with file name of command file\n\
It is also the default action and in this case the bootstrap file\n\
generated is named bootstrap.cmd\n\
";

static int
init_global_data(IC_MEMORY_CONTAINER **mc_ptr)
{
  IC_STRING config_dir;
  IC_CONFIG_ERROR err_obj;
  int ret_code= IC_ERROR_MEM_ALLOC;
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  IC_CLUSTER_CONFIG *cluster;
  guint32 i;
  DEBUG_ENTRY("init_global_data");

  if (!(*mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
                                           0, FALSE)))
    goto error;

  ret_code= 0;
  ic_set_current_dir(&config_dir);
  /* Read the config.ini file to get information about cluster names */
  if (!(glob_clu_infos= ic_load_cluster_config_from_file(&config_dir,
                                                    (IC_CONF_VERSION_TYPE)0,
                                                    *mc_ptr,
                                                    &err_obj)))
  {
    ret_code= err_obj.err_num;
    ic_printf("Failed to open config.ini");
    ic_print_error(ret_code);
    goto error;
  }
  /* Read the grid_common.ini to get info about Cluster Servers/Managers */
  if (!(glob_grid_cluster= ic_load_grid_common_config_server_from_file(
                         &config_dir,
                         (IC_CONF_VERSION_TYPE)0,
                         *mc_ptr,
                         *glob_clu_infos,
                         &err_obj)))
  {
    ret_code= err_obj.err_num;
    ic_printf("Failed to open grid_common.ini");
    ic_print_error(ret_code);
    goto error;
  }
  clu_infos= glob_clu_infos;
  i= 0;
  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
    if (!(cluster= ic_load_config_server_from_files(&config_dir,
                                                    (IC_CONF_VERSION_TYPE)0,
                                                    *mc_ptr,
                                                    glob_grid_cluster,
                                                    clu_info,
                                                    &err_obj)))
    {
      ret_code= err_obj.err_num;
      ic_printf("Failed to open %s.ini", clu_info->cluster_name.str);
      ic_print_error(ret_code);
      goto error;
    }
    glob_clusters[i++]= cluster;
  }
  glob_num_clusters= i;
error:
  DEBUG_RETURN_INT(ret_code);
}

/**
  Initialise parse_data before first use

  @parameter parse_data   IN: Struct to initialise
*/
static void
init_parse_data(IC_PARSE_DATA *parse_data)
{
  parse_data->next_cs_index= 0;
  parse_data->next_mgr_index= 0;
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  IC_MEMORY_CONTAINER *glob_mc_ptr= NULL;
  IC_PARSE_DATA parse_data;
  gchar *parse_buf= NULL;
  gchar *read_buf;
  gchar *file_content= NULL;
  gchar *file_ptr= NULL;
  gchar *file_end= NULL;
  guint64 file_size;
  guint32 curr_pos= 0;
  guint32 curr_length= 0;
  guint32 line_size;
  gboolean end_of_input;
  gboolean readline_inited= FALSE;

  if ((ret_code= ic_start_program(argc, argv, entries, NULL,
                                  glob_process_name,
                                  start_text, TRUE)))
    goto end;

  if ((ret_code= ic_boot_find_hash_function()))
    goto end;

  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
                                           0, FALSE)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto end;
  }
  if (!(parse_buf= ic_malloc(COMMAND_READ_BUF_SIZE + 1)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto end;
  }
  ic_zero(&parse_data, sizeof(IC_PARSE_DATA));
  parse_data.lex_data.mc_ptr= mc_ptr;

  if (glob_command_file && glob_generate_command_file)
  {
    ret_code= IC_ERROR_TWO_COMMAND_FILES;
    goto end;
  }
  if (!(glob_interactive && glob_command_file))
  {
    /* Start by loading the config files for default action */
    if ((ret_code= init_global_data(&glob_mc_ptr)))
      goto end;
    if (!glob_generate_command_file)
    {
      /* Use default generated command file */
      glob_generate_command_file= "bootstrap.cmd";
    }
    glob_command_file= glob_generate_command_file;
    if ((ret_code= generate_command_file(parse_buf, glob_command_file)))
      goto end;
  }

  if (glob_command_file)
  {
    if ((ret_code= ic_get_file_contents(glob_command_file,
                                        &file_content,
                                        &file_size)))
      goto end;
    file_ptr= file_content;
    file_end= file_content + file_size;
  }
  else
  {
    ic_init_readline(glob_history_size);
    readline_inited= TRUE;
  }
  init_parse_data(&parse_data);

  while (!(ret_code= get_line(&file_ptr,
                              file_end,
                              parse_buf,
                              &line_size,
                              &curr_pos,
                              &curr_length,
                              &read_buf,
                              &end_of_input)))
  {
    if (end_of_input)
      goto end;
    execute_command(&parse_data, read_buf, line_size, mc_ptr);
    if (parse_data.exit_flag)
      goto end;
  }

end:
  if (readline_inited)
    ic_close_readline();
  if (file_content)
    ic_free(file_content);
  if (glob_mc_ptr)
    glob_mc_ptr->mc_ops.ic_mc_free(glob_mc_ptr);
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  if (parse_buf)
    ic_free(parse_buf);
  if (ret_code)
    ic_print_error(ret_code);
  return ret_code;
}
