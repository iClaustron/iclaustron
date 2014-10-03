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
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_lex_support.h>
#include <ic_protocol_support.h>
#include <ic_proto_str.h>
#include <ic_pcntrl_proto.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_boot_int.h"

static gchar *glob_bootstrap_file= "bootstrap.cmd";
static guint32 glob_connect_timer= 10;
static IC_CLUSTER_CONNECT_INFO **glob_clu_infos= NULL;
static IC_CLUSTER_CONFIG *glob_grid_cluster= NULL;
static IC_CLUSTER_CONFIG *glob_clusters[IC_MAX_CLUSTER_ID + 1];
static guint32 glob_num_clusters= 0;

static GOptionEntry entries[]=
{
  { "bootstrap-file", 0, 0, G_OPTION_ARG_STRING,
    &glob_bootstrap_file,
    "Generate a command file from config files, provide its name", NULL},
  { "connect-timer", 0, 0, G_OPTION_ARG_INT, &glob_connect_timer,
    "Set timeout in seconds to wait for connect", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static int
generate_bootstrap_file(gchar *parse_buf, const gchar *command_file)
{
  int ret_code;
  int len;
  guint32 node_id;
  IC_FILE_HANDLE cmd_file_handle;
  IC_CLUSTER_SERVER_CONFIG *cs_conf;
  IC_CLUSTER_MANAGER_CONFIG *mgr_conf;
  DEBUG_ENTRY("generate_bootstrap_file");

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

static int
check_cs_node_id(IC_PARSE_DATA *parse_data, guint32 node_id, int include_last)
{
  int i;
  DEBUG_ENTRY("check_cs_node_id");

  for (i= 0;
       i < (int)(((int)parse_data->next_cs_index - 1) + include_last);
       i++)
  {
    if (node_id == parse_data->cs_data[i].node_id)
      goto error;
  }
  DEBUG_RETURN_INT(0);

error:
  ic_printf("Multiple nodes with same node id");
  parse_data->exit_flag= TRUE;
  DEBUG_RETURN_INT(1);
}

static int
check_mgr_node_id(IC_PARSE_DATA *parse_data, guint32 node_id, int include_last)
{
  int i;
  DEBUG_ENTRY("check_mgr_node_id");

  for (i= 0;
       i < (int)(((int)parse_data->next_mgr_index - 1) + include_last);
       i++)
  {
    if (node_id == parse_data->mgr_data[i].node_id)
      goto error;
  }
  DEBUG_RETURN_INT(0);

error:
  ic_printf("Multiple nodes with same node id");
  parse_data->exit_flag= TRUE;
  DEBUG_RETURN_INT(1);
}

static void
ic_prepare_cluster_server_cmd(IC_PARSE_DATA *parse_data)
{
  guint32 node_id;
  DEBUG_ENTRY("ic_prepare_cluster_server_cmd");

  if (parse_data->next_cs_index > IC_MAX_CLUSTER_SERVERS)
  {
    ic_printf("Defined too many Cluster Servers");
    parse_data->exit_flag= TRUE;
    DEBUG_RETURN_EMPTY;
  }
  node_id= parse_data->cs_data[parse_data->next_cs_index - 1].node_id;
  if (check_cs_node_id(parse_data, node_id, 0) ||
      check_mgr_node_id(parse_data, node_id, 1))
  {
    DEBUG_RETURN_EMPTY;
  }
  ic_printf("Prepared cluster server with node id %u", node_id);
  DEBUG_RETURN_EMPTY;
}

static void
ic_prepare_cluster_manager_cmd(IC_PARSE_DATA *parse_data)
{
  guint32 node_id;
  DEBUG_ENTRY("ic_prepare_cluster_manager_cmd");

  if (parse_data->next_mgr_index > IC_MAX_CLUSTER_SERVERS)
  {
    ic_printf("Defined too many Cluster Managers");
    parse_data->exit_flag= TRUE;
    DEBUG_RETURN_EMPTY;
  }
  node_id= parse_data->mgr_data[parse_data->next_mgr_index - 1].node_id;
  if (check_cs_node_id(parse_data, node_id, 1) ||
      check_mgr_node_id(parse_data, node_id, 0))
  {
    DEBUG_RETURN_EMPTY;
  }
  ic_printf("Prepared cluster manager with node id %u", node_id);
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
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_cluster_server_node_id_str,
                                             (guint64)node_id)) ||
       (ret_code= ic_send_with_cr_with_number(conn,
                                              ic_number_of_clusters_str,
                                              (guint64)num_clusters)))
    goto error;

  /*
    We'll send the config.ini an extra time here simply because the protocol
    for copy cluster server files means sending all files, including the
    config.ini file.
  */
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
  A simple n^2-algorithm to check whether we already copied the file
  to this node already. We assume that if there are several nodes using
  the same process controller (same hostname + port), then we don't
  need to copy config.ini several times to this process controller.

  The time to send a file to a node is much higher than the time to scan
  through a set of data structures given that it involves a connect
  and disconnect followed by some interaction. So it pays off to check
  for duplicated effort here.

  @parameter cluster_id          IN: The current cluster_id sending to,
                                 -1 means common grid nodes (CS, CM)
  @parameter node_id             IN: The node_id to send to now
  @parameter ds_conf             IN: The configuration of the node to send
                                 to now

  @retval TRUE if found that process controller already received file,
  FALSE otherwise.
*/
static gboolean
check_for_same_pcntrl_hostname(int cluster_id,
                               guint32 node_id,
                               IC_DATA_SERVER_CONFIG *ds_conf)
{
  int i;
  guint32 j;
  guint32 max_node_id;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_DATA_SERVER_CONFIG *check_ds_conf;
  DEBUG_ENTRY("check_for_same_pcntrl_hostname");

  for (i= (int)-1; i <= cluster_id; i++)
  {
    clu_conf= i < 0 ? glob_grid_cluster : glob_clusters[i];
    if (!clu_conf)
      continue;
    max_node_id= (i != cluster_id) ? clu_conf->max_node_id : (node_id - 1);
    for (j= 1; j <= max_node_id; j++)
    {
      check_ds_conf= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[j];
      if (!check_ds_conf)
        continue;
      if ((strcmp(check_ds_conf->pcntrl_hostname,
                  ds_conf->pcntrl_hostname) == 0) &&
          check_ds_conf->pcntrl_port == ds_conf->pcntrl_port)
      {
        DEBUG_PRINT(CONFIG_LEVEL, ("found TRUE, i = %d, j = %u", i,j));
        DEBUG_RETURN_INT(TRUE);
      }
    }
  }
  DEBUG_RETURN_INT(FALSE);
}

/**
  Copy config.ini to all nodes in a cluster where it is needed

  @parameter cluster_id        IN: Cluster id, -1 if common grid nodes (CS, CM)
*/
static int
cluster_copy_config_ini(int cluster_id)
{
  guint32 i;
  int ret_code;
  IC_DATA_SERVER_CONFIG *ds_conf;
  IC_CLUSTER_CONFIG *clu_conf= cluster_id < 0 ? glob_grid_cluster :
                                                glob_clusters[cluster_id];
  DEBUG_ENTRY("cluster_copy_config_ini");

  if (!clu_conf)
    DEBUG_RETURN_INT(0);
  for (i= 0; i <= clu_conf->max_node_id; i++)
  {
    if (!clu_conf->node_config[i])
      continue;
    ds_conf= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[i];
    if (check_for_same_pcntrl_hostname(cluster_id, i, ds_conf))
      continue;
    if ((ret_code= node_copy_config_ini(ds_conf)))
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
  int i;
  int ret_code;
  DEBUG_ENTRY("copy_config_ini");

  if ((ret_code= cluster_copy_config_ini((int)-1)))
    goto error;
  for (i= 0; i < (int)glob_num_clusters; i++)
  {
    if ((ret_code= cluster_copy_config_ini(i)))
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
        (ret_code= ic_send_with_cr_with_number(conn,
                                               ic_parameter_str,
                                               (guint64)ic_get_debug())))
      return ret_code;
    if (ic_get_debug_timestamp())
    {
      /* Also debug with timestamps if bootstrap is timestamped */
      if ((ret_code= ic_send_with_cr_two_strings(conn,
                                                 ic_parameter_str,
                                                 ic_debug_timestamp_str)) ||
          (ret_code= ic_send_with_cr_with_number(conn,
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

static guint64
get_debug_params()
{
#ifdef DEBUG_BUILD
  if (ic_get_debug())
    if (ic_get_debug_timestamp())
      return (guint64)4;
    else
      return (guint64)2;
  else
    return (guint64)0;
#else
  return (guint64)0;
#endif
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
  gchar *connect_string= NULL;
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

  num_params= get_debug_params() + (guint64)8;
  ret_code= IC_ERROR_MEM_ALLOC;
  if (!(connect_string= ic_get_connectstring(glob_grid_cluster)))
    goto end;
  if ((ret_code= ic_send_with_cr_with_number(conn,
                                             ic_num_parameters_str,
                                             num_params)) ||
      (ret_code= ic_send_debug_level(conn)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_node_id_str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
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
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_parameter_str,
                                   mgr_conf->cluster_manager_port_number)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_cs_connectstring_str)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             connect_string)) ||
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
  if (connect_string)
    ic_free(connect_string);
  DEBUG_RETURN_INT(ret_code);
}

static int
start_cluster_server(IC_CONNECTION *conn,
                     IC_CLUSTER_SERVER_DATA *cs_data,
                     IC_CLUSTER_SERVER_CONFIG *cs_conf)
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

  num_params= get_debug_params() + (guint64)2;
  if ((ret_code= ic_send_with_cr_with_number(conn,
                                             ic_num_parameters_str,
                                             num_params)) ||
      (ret_code= ic_send_debug_level(conn)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_parameter_str,
                                             ic_node_id_str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
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
    ic_printf("Successfully started Cluster Server id %u with pid %u"
              ", on host = %s, listening to port number = %u",
              cs_data->node_id,
              pid,
              cs_conf->hostname,
              cs_conf->cluster_server_port_number);
end:
  DEBUG_RETURN_INT(ret_code);
}

static void
ic_start_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  IC_CLUSTER_SERVER_DATA *cs_data;
  IC_CLUSTER_SERVER_CONFIG *cs_conf;
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
    cs_conf= (IC_CLUSTER_SERVER_CONFIG*)
      glob_grid_cluster->node_config[cs_data->node_id];
    if ((ret_code= start_cluster_server(conn, cs_data, cs_conf)))
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
  gchar *connect_string= NULL;
  IC_CLUSTER_MANAGER_CONFIG *mgr_conf;
  DEBUG_ENTRY("ic_start_cluster_managers_cmd");

  if (parse_data->next_mgr_index == 0)
  {
    ic_printf("No Cluster Managers prepared");
    goto error;
  }
  if (!(connect_string= ic_get_connectstring(glob_grid_cluster)))
  {
    ic_print_error(IC_ERROR_MEM_ALLOC);
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
      ic_printf("Failed to open connection to start Cluster Manager id %u",
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
  if (connect_string)
    ic_free(connect_string);
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
    case IC_EXIT_CMD:
      parse_data->exit_flag= TRUE;
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
  parse_data->break_flag= FALSE;
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
  if (!parse_data->break_flag)
  {
    /**
     * Parsing went ok, we can now execute the command as setup.
     */
    boot_execute(parse_data);
    if (parse_data->exit_flag)
      goto end;
  }
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
      ic_assert(*file_ptr);
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
This program is run from a directory where iClaustron\n\
configuration files are provided. The main file will always\n\
be called config.ini and the file grid_common.ini will\n\
will always contain the definition of the common Cluster Servers\n\
and Cluster Managers in the Grid. The remaining files will be one\n\
for each cluster with the name as given in config.ini.\n\
Based on this configuration the program will generate a bootstrap\n\
command file and also execute it.\n\
\n\
The default name of the bootstrap command file is bootstrap.cmd. A\n\
different name can be used through setting the bootstrap-file\n\
parameter. This file will remain after running the bootstrap program\n\
but cannot be reused, it's merely kept as documentation of what the\n\
bootstrap program actually executed.\n\
\n\
Based on the information in the configuration files the ic_bootstrap\n\
program will copy all the configuration files to the Cluster Servers\n\
defined in the configuration. It will also copy the config.ini to all\n\
nodes of all Clusters.\n\
\n\
As a final step it will also start all Cluster Servers and then all\n\
Cluster Managers.\n\
\n\
Both the copy phase and the start phase requires that all iClaustron\n\
Process Controllers for the grid have been started. It is also required\n\
that all those process controllers are accessible from the machine where\n\
the bootstrap program is executed.\n\
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
    glob_num_clusters= i; /* For error handling purposes */
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
  parse_data->break_flag= FALSE;
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  IC_MEMORY_CONTAINER *glob_mc_ptr= NULL;
  IC_PARSE_DATA parse_data;
  IC_CLUSTER_CONFIG *clu_conf;
  gchar *parse_buf= NULL;
  gchar *read_buf;
  gchar *file_content= NULL;
  gchar *file_ptr= NULL;
  gchar *file_end= NULL;
  guint64 file_size;
  guint32 curr_pos= 0;
  guint32 curr_length= 0;
  guint32 line_size;
  int i;
  gboolean end_of_input;

  if ((ret_code= ic_start_program(argc,
                                  argv,
                                  entries,
                                  NULL,
                                  "ic_bootstrap",
                                  start_text,
                                  TRUE,
                                  FALSE)))
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

  /*
    Start by loading the config files into memory, based on this
    data structures generate a bootstrap file and read this file
    into memory.
  */
  if ((ret_code= init_global_data(&glob_mc_ptr)) ||
      (ret_code= generate_bootstrap_file(parse_buf,
                                         glob_bootstrap_file)) ||
      (ret_code= ic_get_file_contents(glob_bootstrap_file,
                                      &file_content,
                                      &file_size)))
    goto end;

  file_ptr= file_content;
  file_end= file_content + file_size;
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
  ic_printf("Bootstrap successfully completed, %s contains commands executed",
            glob_bootstrap_file);

end:
  for (i= (int)-1; i < (int)glob_num_clusters; i++)
  {
    clu_conf= (i < 0) ? glob_grid_cluster : glob_clusters[i];
    if (clu_conf == NULL)
      continue;
    clu_conf->ic_free_cluster_config(clu_conf);
  }
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
