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

#include <ic_common.h>
#include <ic_clmgr.h>

static int PARSE_BUF_SIZE = 256 * 1024; /* 256 kByte parse buffer */
static gchar *glob_cluster_server_ip= "127.0.0.1";
static gchar *glob_cluster_server_port= "10006";
static gchar *glob_cluster_mgr_ip= "127.0.0.1";
static gchar *glob_cluster_mgr_port= "12003";

static gchar *not_impl_string= "not implemented yet";
static gchar *no_such_cluster_string=
"The specified cluster name doesn't exist in this grid";

static GOptionEntry entries[] = 
{
  { "cluster_server_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_server_ip,
    "Set Server Hostname of Cluster Server", NULL},
  { "cluster_server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "cluster_manager_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_mgr_ip,
    "Set Server Hostname of Cluster Manager", NULL},
  { "cluster_server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_mgr_port,
    "Set Server Port of Cluster Manager", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
translate_cluster_name(IC_STRING *cluster_name, guint64 *cluster_id)
{
  return 1;
}

static int
set_up_server_connection(IC_CONNECTION **conn)
{
  IC_CONNECTION *loc_conn;
  int ret_code;

  if (!(loc_conn= ic_create_socket_object(FALSE, TRUE, FALSE, FALSE,
                                          NULL, NULL)))
  {
    DEBUG_PRINT(COMM_LEVEL, ("Failed to create Connection object\n"));
    return 1;
  }
  DEBUG_PRINT(COMM_LEVEL,
    ("Setting up server connection for Cluster Manager at %s:%s\n",
     glob_cluster_mgr_ip, glob_cluster_mgr_port));
  loc_conn->server_name= glob_cluster_mgr_ip;
  loc_conn->server_port= glob_cluster_mgr_port;
  loc_conn->is_listen_socket_retained= TRUE;
  if ((ret_code= loc_conn->conn_op.ic_set_up_connection(loc_conn)))
  {
    DEBUG_PRINT(COMM_LEVEL,
      ("Failed to set-up connection for Cluster Manager\n"));
    loc_conn->conn_op.ic_free_connection(loc_conn);
    return 1;
  }
  printf("Successfully set-up connection for Cluster Manager at %s:%s\n",
         glob_cluster_mgr_ip, glob_cluster_mgr_port);
  *conn= loc_conn;
  return 0;
}

static int
not_implemented_yet(IC_PARSE_DATA *parse_data)
{
  return 1;
}

static int
start_kernel_node(gchar *node_config, guint32 cluster_id, guint32 node_id,
                  gboolean initial_flag)
{
  return 0;
}

static int
start_client_node(gchar *node_config, guint32 cluster_id, guint32 node_id,
                  gboolean initial_flag)
{
  return 0;
}

static int
start_cs_node(gchar *node_config, guint32 cluster_id, guint32 node_id,
              gboolean initial_flag)
{
  return 0;
}

static int
start_sql_node(gchar *node_config, guint32 cluster_id, guint32 node_id,
               gboolean initial_flag)
{
  return 0;
}

static int
start_rep_node(gchar *node_config, guint32 cluster_id, guint32 node_id,
                  gboolean initial_flag)
{
  return 0;
}

static int
ic_die_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "DIE") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_kill_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "KILL") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_move_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "MOVE") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_perform_backup_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "PERFORM BACKUP") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_perform_rolling_upgrade_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "PERFORM ROLLING UPGRADE") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_restart_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "RESTART") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_start_cmd(IC_PARSE_DATA *parse_data)
{
  guint32 cluster_id;
  guint32 node_id;
  int error;
  gchar *node_config;
  gboolean initial_flag;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_API_CONFIG_SERVER *apic= parse_data->apic;

  if (parse_data->cluster_all)
    goto error;
  if (parse_data->default_cluster)
    cluster_id= (guint32)parse_data->current_cluster_id;
  else
    cluster_id= (guint32)parse_data->cluster_id;
  if (parse_data->default_node)
    goto error;
  node_id= (guint32)parse_data->node_id;
  clu_conf= apic->api_op.ic_get_cluster_config(apic, cluster_id);
  if (!clu_conf)
    goto error;
  node_config= apic->api_op.ic_get_node_object(apic, cluster_id, node_id);
  if (!node_config)
    goto error;
  initial_flag= parse_data->initial_flag;
  switch (clu_conf->node_types[node_id])
  {
    case IC_KERNEL_NODE:
    {
      start_kernel_node(node_config, cluster_id, node_id, initial_flag);
      break;
    }
    case IC_CLIENT_NODE:
    {
      start_client_node(node_config, cluster_id, node_id, initial_flag);
      break;
    }
    case IC_CLUSTER_SERVER_NODE:
    {
      start_cs_node(node_config, cluster_id, node_id, initial_flag);
      break;
    }
    case IC_SQL_SERVER_NODE:
    {
      start_sql_node(node_config, cluster_id, node_id, initial_flag);
      break;
    }
    case IC_REP_SERVER_NODE:
    {
      start_rep_node(node_config, cluster_id, node_id, initial_flag);
       break;
    }
    default:
      break;
  }
  return 0;
error:
  if (ic_send_with_cr(parse_data->conn, "START") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_stop_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "STOP") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_list_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "LIST") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_listen_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "LISTEN") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_cluster_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CLUSTER") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_cluster_status_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CLUSTER STATUS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_connections_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CONNECTIONS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_config_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CONFIG") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_memory_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW MEMORY") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_statvars_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW STATVARS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_show_stats_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW STATS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_set_stat_level_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SET STAT LEVEL") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_use_cluster_cmd(IC_PARSE_DATA *parse_data)
{
  gchar buf[256];
  printf("len= %u, ptr= %u, str= %s\n", parse_data->cluster_name.len,
  (guint32)parse_data->cluster_name.str, parse_data->cluster_name.str);
  if (parse_data->cluster_name.str)
  {
    if (translate_cluster_name(&parse_data->cluster_name,
                               &parse_data->cluster_id))
    {
      if (ic_send_with_cr(parse_data->conn, no_such_cluster_string) ||
          ic_send_with_cr(parse_data->conn, ic_empty_string))
        parse_data->exit_flag= TRUE;
    }
  }
  g_snprintf(buf, 128, "Cluster id set to %u", (guint32)parse_data->cluster_id);
  if (ic_send_with_cr(parse_data->conn, buf) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_display_stats_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "DISPLAY STATS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_top_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "TOP") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_with_cr(parse_data->conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
  return 0;
}

static int
ic_execute(IC_PARSE_DATA *parse_data)
{
  switch (parse_data->command)
  {
    case IC_DIE_CMD:
      return ic_die_cmd(parse_data);
    case IC_KILL_CMD:
      return ic_kill_cmd(parse_data);
    case IC_MOVE_CMD:
      return ic_move_cmd(parse_data);
    case IC_PERFORM_BACKUP_CMD:
      return ic_perform_backup_cmd(parse_data);
    case IC_PERFORM_ROLLING_UPGRADE_CMD:
      return ic_perform_rolling_upgrade_cmd(parse_data);
    case IC_RESTART_CMD:
      return ic_restart_cmd(parse_data);
    case IC_START_CMD:
      return ic_start_cmd(parse_data);
    case IC_STOP_CMD:
      return ic_stop_cmd(parse_data);
    case IC_LIST_CMD:
      return ic_list_cmd(parse_data);
    case IC_LISTEN_CMD:
      return ic_listen_cmd(parse_data);
    case IC_SHOW_CLUSTER_CMD:
      return ic_show_cluster_cmd(parse_data);
    case IC_SHOW_CLUSTER_STATUS_CMD:
      return ic_show_cluster_status_cmd(parse_data);
    case IC_SHOW_CONNECTIONS_CMD:
      return ic_show_connections_cmd(parse_data);
    case IC_SHOW_CONFIG_CMD:
      return ic_show_config_cmd(parse_data);
    case IC_SHOW_MEMORY_CMD:
      return ic_show_memory_cmd(parse_data);
    case IC_SHOW_STATVARS_CMD:
      return ic_show_statvars_cmd(parse_data);
    case IC_SHOW_STATS_CMD:
      return ic_show_stats_cmd(parse_data);
    case IC_SET_STAT_LEVEL_CMD:
      return ic_set_stat_level_cmd(parse_data);
    case IC_USE_CLUSTER_CMD:
      return ic_use_cluster_cmd(parse_data);
    case IC_DISPLAY_STATS_CMD:
      return ic_display_stats_cmd(parse_data);
    case IC_TOP_CMD:
      return ic_top_cmd(parse_data);
    default:
      return not_implemented_yet(parse_data);
  }
  return 0;
}

static void
release_parse_data(IC_PARSE_DATA *parse_data)
{
  if (parse_data->current_cluster_name.str)
    ic_free(parse_data->current_cluster_name.str);
}

static gpointer
run_handle_new_connection(gpointer data)
{
  int ret_code;
  guint32 read_size;
  guint32 size_curr_buf= 0;
  IC_CONNECTION *conn= (IC_CONNECTION*)data;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  IC_API_CONFIG_SERVER *apic= conn->param;
  gchar *parse_buf;
  guint32 parse_inx= 0;
  IC_PARSE_DATA parse_data;
  gchar rec_buf[256];

  memset(&parse_data, sizeof(IC_PARSE_DATA), 0);
  if (!(parse_buf= ic_malloc(PARSE_BUF_SIZE)))
  {
    ic_print_error(IC_ERROR_MEM_ALLOC);
    goto error;
  }
  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
  {
    ic_print_error(IC_ERROR_MEM_ALLOC);
    goto error;
  }
  parse_data.mc_ptr= mc_ptr;
  parse_data.conn= conn;
  parse_data.apic= apic;
  while (!(ret_code= ic_rec_with_cr(conn, rec_buf, &read_size,
                                    &size_curr_buf, sizeof(rec_buf))))
  {
    if (read_size == 0)
    {
      memset(&parse_data, sizeof(IC_PARSE_DATA), 0);
      parse_buf[parse_inx]= 0;
      parse_buf[parse_inx+1]= 0;
      parse_inx+= 2;
      DEBUG_PRINT(PROGRAM_LEVEL,
        ("Ready to execute command:\n%s\n", parse_buf));
      ic_call_parser(parse_buf, parse_inx, (void*)&parse_data);
      parse_inx= 0;
      if (parse_data.exit_flag)
        goto exit;
      ret_code= ic_execute(&parse_data);
    }
    else
    {
      memcpy(parse_buf+parse_inx, rec_buf, read_size);
      parse_inx+= read_size;
    }
  }
exit:
  DEBUG_PRINT(PROGRAM_LEVEL, ("End of client connection\n"));
error:
  ic_free(parse_buf);
  release_parse_data(&parse_data);
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  conn->conn_op.ic_free_connection(conn);
  return NULL;
}

static int
handle_new_connection(IC_CONNECTION *conn,
                      IC_API_CONFIG_SERVER *apic)
{
  GError *error= NULL;
  conn->param= apic;
  if (!g_thread_create_full(run_handle_new_connection,
                            (gpointer)conn,
                            1024*256, /* 256 kByte stack size */
                            FALSE,   /* Not joinable        */
                            FALSE,   /* Not bound           */
                            G_THREAD_PRIORITY_NORMAL,
                            &error))
  {
    conn->error_code= 1;
    return 1;
  }
  return 0;
}
static int
wait_for_connections_and_fork(IC_CONNECTION *conn,
                              IC_API_CONFIG_SERVER *apic)
{
  int ret_code;
  IC_CONNECTION *fork_conn;
  do
  {
    if ((ret_code= conn->conn_op.ic_accept_connection(conn)))
      goto error;
    DEBUG_PRINT(PROGRAM_LEVEL,
      ("Cluster Manager has accepted a new connection\n"));
    if (!(fork_conn= conn->conn_op.ic_fork_accept_connection(conn,
                                              FALSE,   /* No mutex */
                                              FALSE))) /* No front buffer */
    {
      DEBUG_PRINT(PROGRAM_LEVEL,
        ("Failed to fork a new connection from an accepted connection\n"));
      goto error;
    }
    if ((ret_code= handle_new_connection(fork_conn, apic)))
    {
      DEBUG_PRINT(PROGRAM_LEVEL,
        ("Failed to start new Cluster Manager thread\n"));
      goto error;
    }
  } while (1);
  return 0;
error:
  return ret_code;
}

static IC_API_CONFIG_SERVER*
get_configuration(IC_API_CLUSTER_CONNECTION *apic)
{
  IC_API_CONFIG_SERVER *config_server_obj;
  guint32 cluster_id= 1;
  guint32 node_id= 1;

  apic->num_cluster_servers= 1;
  apic->cluster_server_ips= &glob_cluster_server_ip;
  apic->cluster_server_ports= &glob_cluster_server_port;
  if ((config_server_obj= ic_create_api_cluster(apic, &cluster_id,
                                                &node_id, (guint32)1)))
  {
    config_server_obj->num_clusters_to_connect= 1;
    if (config_server_obj->api_op.ic_get_config(config_server_obj,
                                                (guint64)0x000001))
      return config_server_obj;
    config_server_obj->api_op.ic_free_config(config_server_obj);
  }
  return NULL;
}

int main(int argc,
         char *argv[])
{
  int ret_code= 1;
  IC_CONNECTION *conn;
  IC_API_CLUSTER_CONNECTION apic;
  IC_API_CONFIG_SERVER *config_server_obj= NULL;

  if ((ret_code= ic_start_program(argc, argv, entries,
           "- iClaustron Cluster Manager")))
    return ret_code;
  if ((config_server_obj= get_configuration(&apic)))
    goto error;
  if ((ret_code= set_up_server_connection(&conn)))
    goto error;
  ret_code= wait_for_connections_and_fork(conn, config_server_obj);
  conn->conn_op.ic_free_connection(conn);
error:
  if (config_server_obj)
    config_server_obj->api_op.ic_free_config(config_server_obj);
  ic_end();
  return ret_code;
}

