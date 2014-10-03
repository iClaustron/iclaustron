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
#include <ic_mc.h>
#include <ic_port.h>
#include <ic_string.h>
#include <ic_threadpool.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_protocol_support.h>
#include <ic_parse_connectstring.h>
#include <ic_apid.h>
#include <ic_lex_support.h>
#include "ic_clmgr_int.h"

/* Option variables */
static gchar *glob_cluster_mgr_ip= "127.0.0.1";
static gchar *glob_cluster_mgr_port= IC_DEF_CLUSTER_MANAGER_PORT_STR;
static gboolean glob_only_find_hash= FALSE;

/* Global variables */
static int PARSE_BUF_SIZE = 256 * 1024; /* 256 kByte parse buffer */

static gchar *not_impl_string= "not implemented yet";
static gchar *no_such_cluster_string=
"The specified cluster name doesn't exist in this grid";
static gchar *wrong_node_type= "Node id not of specified type";
static gchar *no_such_node_str="There is no such node in this cluster";

static GOptionEntry entries[]= 
{
  { "server-name", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_mgr_ip,
    "Set Server Hostname of Cluster Manager", NULL},
  { "server-port", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_mgr_port,
    "Set Server Port of Cluster Manager", NULL},
  { "only-find-hash", 0, 0, G_OPTION_ARG_INT, &glob_only_find_hash,
    "Only run to see if we can find a proper hash function", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
connect_pcntrl(IC_CONNECTION **conn, const gchar *node_config)
{
  const IC_DATA_SERVER_CONFIG *ds_conf=
    (const IC_DATA_SERVER_CONFIG*)node_config;
  IC_CONNECTION *local_conn;
  guint32 len;
  int ret_code= 0;
  guint64 pcntrl_port_num= ds_conf->pcntrl_port;
  gchar *pcntrl_port;
  gchar pcntrl_port_buf[32];
  DEBUG_ENTRY("connect_pcntrl");

  pcntrl_port= ic_guint64_str(pcntrl_port_num,
                              pcntrl_port_buf, &len);
  if (!(local_conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                            CONFIG_READ_BUF_SIZE,
                                            NULL, NULL)))
    return IC_ERROR_MEM_ALLOC;
  local_conn->conn_op.ic_prepare_client_connection(
                 local_conn,
                 ds_conf->pcntrl_hostname,
                 pcntrl_port,
                 NULL, NULL);
  if ((ret_code= local_conn->conn_op.ic_set_up_connection(local_conn,
                                                          NULL,
                                                          NULL)))
    goto error;
  *conn= local_conn;
  DEBUG_RETURN_INT(0);
error:
  local_conn->conn_op.ic_free_connection(local_conn);
  DEBUG_RETURN_INT(ret_code);
}

static gchar*
add_param_num(IC_PARSE_DATA *parse_data, const gchar *param_str,
              guint32 number)
{
  IC_STRING dest_str;
  IC_STRING number_str;
  guint32 len;
  gchar *number_ptr;
  gchar number_buf[32];

  number_ptr= ic_guint64_str((guint64)number, number_buf, &len);
  IC_INIT_STRING(&number_str, number_ptr, len, TRUE);
  IC_INIT_STRING(&dest_str, (gchar*)param_str, strlen(param_str), TRUE);
  if (ic_mc_add_ic_string(parse_data->lex_data.mc_ptr,
                          &dest_str,
                          &number_str))
  {
    return NULL;
  }
  return dest_str.str;
}

static gchar*
add_param_string(IC_PARSE_DATA *parse_data, const gchar *param_str,
                 gchar *param_value)
{
  IC_STRING dest_str;
  IC_STRING value_str;

  IC_INIT_STRING(&dest_str, (gchar*)param_str, strlen(param_str), TRUE);
  IC_INIT_STRING(&value_str, param_value, strlen(param_value), TRUE);
  if (ic_mc_add_ic_string(parse_data->lex_data.mc_ptr,
                          &dest_str,
                          &value_str))
  {
    return NULL;
  }
  return dest_str.str;
}

/*
  Create a string like:
  "--ndb-connectstring=myhost1:port1,myhost2:port2"
  myhost1 and myhost2 are in this case the two hosts where we have
  cluster servers in this cluster and their respective port numbers
  port1 and port2. We don't provide the port numbers if they are
  the standard port number 1186 used for MySQL Cluster Managers.
  Can also be used when parameter is called --cs_connectstring.
*/
static gchar*
add_connectstring(IC_PARSE_DATA *parse_data, const gchar *param_str,
                  IC_CLUSTER_CONFIG *clu_conf)
{
  IC_CLUSTER_SERVER_CONFIG *cs_conf;
  IC_STRING host_str, number_str, dest_str;
  guint32 found= 0;
  guint32 len, i;
  gchar *number_ptr, *buf_ptr;
  gchar number_buf[32];
  gchar buf[512];
  DEBUG_ENTRY("add_connectstring");

  IC_INIT_STRING(&dest_str, (gchar*)param_str, strlen(param_str), TRUE);
  for (i= 1; i <= clu_conf->max_node_id; i++)
  {
    if (clu_conf->node_types[i] == IC_CLUSTER_SERVER_NODE)
    {
      buf_ptr= buf;
      cs_conf= (IC_CLUSTER_SERVER_CONFIG*)clu_conf->node_config[i];
      len= strlen(cs_conf->hostname);
      if (found)
      {
        buf[0]= ',';
        buf_ptr++;
        len++;
      }
      memcpy(buf_ptr, cs_conf->hostname, strlen(cs_conf->hostname)+1);
      IC_INIT_STRING(&host_str, buf, len, TRUE);
      if (cs_conf->port_number != IC_DEF_CLUSTER_SERVER_PORT)
      {
        host_str.str[host_str.len]= ':';
        host_str.len++;
        number_ptr= ic_guint64_str((guint64)cs_conf->port_number,
                                   number_buf, &len);
        IC_INIT_STRING(&number_str, number_ptr, len, TRUE);
        ic_add_ic_string(&host_str, &number_str);
      }
      if (ic_mc_add_ic_string(parse_data->lex_data.mc_ptr,
                              &dest_str,
                              &host_str))
      {
        DEBUG_RETURN_PTR(NULL);
      }
      found++;
    }
  }
  ic_assert(found == clu_conf->num_cluster_servers);
  if (!found)
  {
    DEBUG_RETURN_PTR(NULL);
  }
  DEBUG_RETURN_PTR(dest_str.str);
}

static void
report_error(IC_PARSE_DATA *parse_data, gchar *error_str)
{
  DEBUG_ENTRY("report_error");
  if (ic_send_with_cr(parse_data->conn, error_str) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static int
get_node_id(IC_PARSE_DATA *parse_data)
{
  guint32 node_id;
  IC_API_CONFIG_SERVER *apic= parse_data->apic;
  DEBUG_ENTRY("get_node_id");

  if (parse_data->node_id == IC_MAX_UINT32)
  {
    node_id= apic->api_op.ic_get_node_id_from_name(apic,
                                                   (guint32)parse_data->cluster_id,
                                                   &parse_data->node_name);
  }
  else
  {
    node_id= (guint32)parse_data->node_id;
  }
  if (!parse_data->apic->api_op.ic_get_node_object(parse_data->apic,
                                             (guint32)parse_data->cluster_id,
                                                   node_id))
  {
    report_error(parse_data, no_such_node_str);
    DEBUG_RETURN_INT(1);
  }
  DEBUG_RETURN_INT(0);
}

/**
 * Connect to the process controller to have him start a node
 */
static int
start_node(IC_PARSE_DATA *parse_data,
           const gchar *node_config,
           const gchar *grid_name,
           const gchar *cluster_name,
           const gchar *node_name,
           const gchar *program_name,
           const gchar *version_name,
           const guint32 num_params,
           const gchar **parameters,
           const gboolean autorestart_flag)
{
  int ret_code;
  gchar *read_buf;
  guint32 read_size;
  guint32 i;
  guint32 pid;
  IC_CONNECTION *conn= NULL;
  DEBUG_ENTRY("start_node");

  if ((ret_code= connect_pcntrl(&conn, node_config)))
    goto error;

  if ((ret_code= ic_send_with_cr(conn, ic_start_str)) ||
      (ret_code= ic_send_start_info(conn,
                                    program_name, version_name,
                                    grid_name, cluster_name, node_name)) ||
      (ret_code= ic_send_with_cr_two_strings(conn,
                                             ic_auto_restart_str,
                 autorestart_flag ? ic_true_str : ic_false_str)) ||
      (ret_code= ic_send_with_cr_with_number(conn,
                                             ic_num_parameters_str,
                                             num_params)))
    goto error;
  for (i= 0; i < num_params; i++)
  {
    if ((ret_code= ic_send_with_cr(conn, parameters[i])))
      goto error;
  }
  if ((ret_code= ic_send_empty_line(conn)))
    goto error;
  /* Completed send of start message, now wait for response */
  if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
    goto error;
  if (ic_check_buf(read_buf, read_size, ic_ok_str, strlen(ic_ok_str)))
  {
    /* Received an ok message */
    if ((ret_code= ic_rec_number(conn, ic_pid_str, &pid)))
      goto error;
    if ((ret_code= ic_rec_empty_line(conn)))
      goto error;
    /*
      Send response to client with information about our successful
      start of a node.
    */
    if ((ret_code= ic_send_with_cr(parse_data->conn,
                                   "Successfully started node:")) ||
        (ret_code= ic_send_start_info(parse_data->conn,
                                      program_name, version_name,
                                      grid_name, cluster_name,
                                      node_name)) ||
        (ret_code= ic_send_with_cr_with_number(parse_data->conn,
                                               ic_pid_str,
                                               pid)) ||
        (ret_code= ic_send_empty_line(parse_data->conn)))
      goto error;
  }
  else if (ic_check_buf(read_buf, read_size, ic_error_str,
                        strlen(ic_error_str)))
  {
    /* Received an error message */
    read_buf[read_size]= 0; /* Null terminate error message */
    if ((ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
      goto error;
    if ((ret_code= ic_rec_empty_line(conn)))
      goto error;
    if ((ret_code= ic_send_with_cr(parse_data->conn,
                                   "Failed to start node:")) ||
        (ret_code= ic_send_start_info(parse_data->conn,
                                      program_name, version_name,
                                      grid_name, cluster_name, node_name)) ||
        (ret_code= ic_send_with_cr(parse_data->conn, ic_error_str)) ||
        (ret_code= ic_send_with_cr(parse_data->conn, read_buf)) ||
        (ret_code= ic_send_empty_line(parse_data->conn)))
      goto error;
  }
  else
  {
    ret_code= IC_PROTOCOL_ERROR;
    goto error;
  }
error:
  if (conn)
  {
    conn->conn_op.ic_free_connection(conn);
  }
  DEBUG_RETURN_INT(ret_code);
}

/**
 * Start an NDB data node to be used by iClaustron
 */
static int
start_data_server_node(IC_PARSE_DATA *parse_data,
                       gchar *node_config,
                       IC_CLUSTER_CONFIG *clu_conf)
{
  IC_DATA_SERVER_CONFIG *ds_conf= (IC_DATA_SERVER_CONFIG*)node_config;
  guint32 num_params= 0;
  const gchar* param_array[8];
  DEBUG_ENTRY("start_data_server_node");

  /*
    iClaustron supports the following parameters on Data Server nodes:
    1) --ndb-node-id
      The node id of the node will be set here
    2) --cluster-id
      The cluster id of the node will be set here
      This is an iClaustron extension of the Data Server to enable multiple
      clusters to be maintained by one set of Cluster Servers and Cluster
      Managers.
    3) --ndb-connectstring
      String used to inform the Data Server about where to find the iClaustron
      Cluster Servers.
      It is on the form:
      host=myhost1:port1,myhost2:port2,myhost3:port3
    4) --initial
      Used to perform an initial cluster start of all Data Server nodes.
    5) --initial-start
      When performing an initial start of a partitioned cluster (meaning
      not all nodes are to be started) this parameter will be set together
      with --no-wait-nodes
    6) --no-wait-nodes
      Specifies which nodes are not to be started as part of the initial
      start.
  */

  if (!(param_array[num_params++]= add_param_num(parse_data,
                                                 ic_ndb_node_id_str,
                                                 ds_conf->node_id)) ||
      !(param_array[num_params++]= add_connectstring(parse_data,
              ic_ndb_connectstring_str,
              clu_conf)))
  {
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  }
  /*
    This functionality requires a NDB that has been adapted to use
    cluster ids and some other iClaustron functionality.

  if (!(param_array[num_params++]= add_param_num(parse_data,
                                                 ic_cluster_id_str,
                                                 clu_conf->clu_info.cluster_id)))
    return IC_ERROR_MEM_ALLOC;
  */
  if (parse_data->initial_flag)
  {
    param_array[num_params++]= ic_initial_flag_str;
  }

  DEBUG_RETURN_INT(start_node(parse_data,
                              node_config,
                             ic_def_grid_str,
                             clu_conf->clu_info.cluster_name.str,
                             ds_conf->node_name,
                             ic_data_server_program_str,
                             parse_data->ndb_version_name.str,
                             num_params,
                             (const gchar**)&param_array[0],
                             parse_data->restart_flag));
}

/**
 * Start an iClaustron Data API program
 */
static int
start_client_node(IC_PARSE_DATA *parse_data,
                  gchar *node_config,
                  IC_CLUSTER_CONFIG *clu_conf)
{
  DEBUG_ENTRY("start_client_node");
  (void)parse_data;
  (void)node_config;
  (void)clu_conf;
  DEBUG_RETURN_INT(0);
}

/**
 * Start a MySQL Server
 */
static int
start_sql_server_node(IC_PARSE_DATA *parse_data,
                      gchar *node_config,
                      IC_CLUSTER_CONFIG *clu_conf)
{
  DEBUG_ENTRY("start_sql_server_node");
  (void)parse_data;
  (void)node_config;
  (void)clu_conf;
  DEBUG_RETURN_INT(0);
}

/**
 * Start replication server or file server
 */
static int
start_apid_server_node(IC_PARSE_DATA *parse_data,
                       gchar *node_config,
                       const gchar *program_str,
                       IC_CLUSTER_CONFIG *clu_conf)
{
  IC_CLIENT_CONFIG *client_conf= (IC_CLIENT_CONFIG*)node_config;
  guint32 num_params= 0;
  gchar *param_array[8];
  DEBUG_ENTRY("start_apid_server_node");

  /*
    We can set the following parameters when we start the replication/file
    server:
    1) --node_id
      The node id
    2) --data_dir
      The path to the data directory where config file will be in the
      subdirectory config
    3) --cs_connectstring
      A list like myhost1:port1,myhost2:port2 that points out where the
      cluster servers are placed.
    4) --num_threads
      The number of threads to start up in the server node
  */
  if (!(param_array[num_params++]= add_param_num(parse_data,
                                                 ic_node_id_str,
                                                 client_conf->node_id)) ||
      !(param_array[num_params++]= add_param_string(parse_data,
                                              ic_data_dir_str,
                                              client_conf->node_data_path)) ||
      !(param_array[num_params++]= add_connectstring(parse_data,
                                                     ic_cs_connectstring_str,
                                                     clu_conf)) ||
      !(param_array[num_params++]= add_param_num(parse_data,
                                           ic_num_threads_str,
                                           client_conf->apid_num_threads)))
  {
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  }

  DEBUG_RETURN_INT(start_node(parse_data,
                              node_config,
                              ic_def_grid_str,
                              clu_conf->clu_info.cluster_name.str,
                              client_conf->node_name,
                              program_str,
                              parse_data->iclaustron_version_name.str,
                              num_params,
                              (const gchar**)&param_array[0],
                              parse_data->restart_flag));
}

/**
 * Start restore program
 */
static int
start_restore_node(IC_PARSE_DATA *parse_data,
                   gchar *node_config,
                   IC_CLUSTER_CONFIG *clu_conf)
{
  DEBUG_ENTRY("start_restore_node");
  (void)parse_data;
  (void)node_config;
  (void)clu_conf;
  DEBUG_RETURN_INT(0);
}

/**
 * Start cluster manager or cluster server
 */
static int
start_cluster_node(IC_PARSE_DATA *parse_data,
                   gchar *node_config,
                   const gchar *program_str,
                   guint32 port_number,
                   IC_CLUSTER_CONFIG *clu_conf)
{
  IC_CLUSTER_SERVER_CONFIG *cs_conf= (IC_CLUSTER_SERVER_CONFIG*)node_config;
  guint32 num_params= 0;
  gchar *param_array[8];
  DEBUG_ENTRY("start_cluster_node");

  /*
    We can set the following parameters when we start the cluster manager.
    1) --node_id
      The node id of the cluster manager/server
    2) --server_name
      The hostname of the cluster manager/server
    3) --server_port
      The port number of the cluster manager/server
    4) --data_dir
      The path to the data directory where config file will be in the
      subdirectory config
    5) --cs_connectstring
      A list like myhost1:port1,myhost2:port2 that points out where the
      cluster servers are placed.
  */
  if (!(param_array[num_params++]= add_param_num(parse_data,
                                                 ic_node_id_str,
                                                 cs_conf->node_id)) ||
      !(param_array[num_params++]= add_param_string(parse_data,
                                                    ic_server_name_str,
                                                    cs_conf->hostname)) ||
      !(param_array[num_params++]= add_param_num(parse_data,
                                                 ic_server_port_str,
                                                 port_number)) ||
      !(param_array[num_params++]= add_param_string(parse_data,
                                              ic_data_dir_str,
                                              cs_conf->node_data_path)) ||
      !(param_array[num_params++]= add_connectstring(parse_data,
                                                     ic_cs_connectstring_str,
                                                     clu_conf)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);

  DEBUG_RETURN_INT(start_node(parse_data,
                              node_config,
                              ic_def_grid_str,
                              clu_conf->clu_info.cluster_name.str,
                              cs_conf->node_name,
                              program_str,
                              parse_data->iclaustron_version_name.str,
                              num_params,
                              (const gchar**)&param_array[0],
                              parse_data->restart_flag));
}

static int
start_specific_node(IC_CLUSTER_CONFIG *clu_conf,
                    IC_API_CONFIG_SERVER *apic,
                    IC_PARSE_DATA *parse_data,
                    guint32 node_id)
{
  gchar *node_config;
  guint32 cluster_id= clu_conf->clu_info.cluster_id;
  IC_CLUSTER_SERVER_CONFIG *cs_conf;
  IC_CLUSTER_MANAGER_CONFIG *mgr_conf;
  DEBUG_ENTRY("start_specific_node");

  node_config= apic->api_op.ic_get_node_object(apic, cluster_id, node_id);
  if (!node_config)
  {
    DEBUG_RETURN_INT(1);
  }
  if (parse_data->binary_type_flag)
  {
    if (parse_data->binary_type != clu_conf->node_types[node_id])
    {
      if (parse_data->node_all)
      {
        DEBUG_RETURN_INT(0);
      }
      report_error(parse_data, wrong_node_type);
    }
  }
  switch (clu_conf->node_types[node_id])
  {
    case IC_DATA_SERVER_NODE:
    {
      DEBUG_RETURN_INT(start_data_server_node(parse_data,
                                              node_config,
                                              clu_conf));
      break;
    }
    case IC_CLIENT_NODE:
    {
      DEBUG_RETURN_INT(start_client_node(parse_data,
                                         node_config,
                                         clu_conf));
      break;
    }
    case IC_CLUSTER_SERVER_NODE:
    {
      cs_conf= (IC_CLUSTER_SERVER_CONFIG*)node_config;
      DEBUG_RETURN_INT(start_cluster_node(parse_data,
                                      node_config,
                                      ic_cluster_server_program_str,
                                      cs_conf->cluster_server_port_number,
                                      clu_conf));
      break;
    }
    case IC_CLUSTER_MANAGER_NODE:
    {
      mgr_conf= (IC_CLUSTER_MANAGER_CONFIG*)node_config;
      DEBUG_RETURN_INT(start_cluster_node(parse_data,
                                      node_config,
                                      ic_cluster_manager_program_str,
                                      mgr_conf->cluster_manager_port_number,
                                      clu_conf));
      break;
    }
    case IC_SQL_SERVER_NODE:
    {
      DEBUG_RETURN_INT(start_sql_server_node(parse_data,
                                             node_config,
                                             clu_conf));
      break;
    }
    case IC_REP_SERVER_NODE:
    {
      DEBUG_RETURN_INT(start_apid_server_node(parse_data,
                                              node_config,
                                              ic_rep_server_program_str,
                                              clu_conf));
       break;
    }
    case IC_FILE_SERVER_NODE:
    {
      DEBUG_RETURN_INT(start_apid_server_node(parse_data,
                                              node_config,
                                              ic_file_server_program_str,
                                              clu_conf));
       break;
    }
    case IC_RESTORE_NODE:
    {
      DEBUG_RETURN_INT(start_restore_node(parse_data,
                                          node_config,
                                          clu_conf));
      break;
    }
    default:
      break;
  }
  DEBUG_RETURN_INT(0);
}

static void
ic_die_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "DIE") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_kill_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "KILL") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_move_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "MOVE") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_perform_backup_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "PERFORM BACKUP") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_perform_rolling_upgrade_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "PERFORM ROLLING UPGRADE") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_restart_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "RESTART") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_start_cmd(IC_PARSE_DATA *parse_data)
{
  guint32 cluster_id;
  guint32 node_id;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_API_CONFIG_SERVER *apic= parse_data->apic;
  DEBUG_ENTRY("ic_start_cmd");

  if (parse_data->cluster_all)
    goto error;
  if (parse_data->default_cluster)
  {
    cluster_id= (guint32)parse_data->current_cluster_id;
  }
  else
  {
    cluster_id= (guint32)parse_data->cluster_id;
  }
  clu_conf= apic->api_op.ic_get_cluster_config(apic, cluster_id);
  if (!clu_conf)
    goto error;
  if (parse_data->node_all)
  {
    for (node_id= 1; node_id <= clu_conf->max_node_id; node_id++)
    {
      start_specific_node(clu_conf, apic, parse_data, node_id);
    }
  }
  else
  {
    if (!get_node_id(parse_data))
    {
      node_id= (guint32)parse_data->node_id;
      start_specific_node(clu_conf, apic, parse_data, node_id);
    }
  }
  DEBUG_RETURN_EMPTY;
error:
  if (ic_send_with_cr(parse_data->conn, "START") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_stop_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "STOP") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

/* Handle command LIST CLUSTERS */
static void
ic_list_cmd(IC_PARSE_DATA *parse_data)
{
  IC_CLUSTER_CONFIG *clu_conf;
  IC_API_CONFIG_SERVER *apic= parse_data->apic;
  int ret_code;
  gchar *output_ptr;
  gchar *cluster_name_ptr;
  guint64 cluster_id;
  guint32 cluster_id_size, cluster_name_len, space_len;
  gchar output_buf[2048];
  DEBUG_ENTRY("ic_list_cmd");

  if ((ret_code= ic_send_with_cr(parse_data->conn,
  "CLUSTER_NAME                    CLUSTER_ID")))
    goto error;
  for (cluster_id= 0;
       cluster_id <= apic->api_op.ic_get_max_cluster_id(apic);
       cluster_id++)
  {
    clu_conf= apic->api_op.ic_get_cluster_config(apic, (guint32)cluster_id);
    if (!clu_conf)
      continue;
    cluster_name_ptr= clu_conf->clu_info.cluster_name.str;
    cluster_name_len= clu_conf->clu_info.cluster_name.len;
    memcpy(output_buf, cluster_name_ptr, cluster_name_len);
    if (cluster_name_len < 32)
    {
      space_len= 32 - cluster_name_len;
    }
    else
    {
      space_len= 1;
    }
    output_ptr= output_buf + cluster_name_len;
    memset(output_ptr, SPACE_CHAR, space_len);
    output_ptr+= space_len;
    output_ptr= ic_guint64_str(cluster_id, output_ptr, &cluster_id_size);
    output_ptr[cluster_id_size]= 0;
    if ((ret_code= ic_send_with_cr(parse_data->conn, output_buf)))
      goto error;
  }
  if ((ret_code= ic_send_empty_line(parse_data->conn)))
    goto error;
  DEBUG_RETURN_EMPTY;
error:
  ic_print_error(ret_code);
  parse_data->exit_flag= TRUE;
  DEBUG_RETURN_EMPTY;
}

static void
ic_listen_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "LISTEN") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_cluster_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CLUSTER") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_cluster_status_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CLUSTER STATUS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_connections_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CONNECTIONS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_config_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW CONFIG") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_memory_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW MEMORY") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_statvars_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW STATVARS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_show_stats_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SHOW STATS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_set_stat_level_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "SET STAT LEVEL") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_use_cluster_cmd(IC_PARSE_DATA *parse_data)
{
  gchar buf[256];
  guint32 cluster_id;
  IC_API_CONFIG_SERVER *apic= parse_data->apic;
  DEBUG_ENTRY("ic_use_cluster_cmd");

  ic_printf("len= %u, str= %s", (guint32)parse_data->cluster_name.len,
            parse_data->cluster_name.str);
  if (parse_data->cluster_name.str)
  {
    cluster_id= apic->api_op.ic_get_cluster_id_from_name(apic,
                                               &parse_data->cluster_name);
    if (cluster_id == IC_MAX_UINT32)
    {
      if (ic_send_with_cr(parse_data->conn, no_such_cluster_string) ||
          ic_send_empty_line(parse_data->conn))
      {
        parse_data->exit_flag= TRUE;
      }
    }
    else
    {
      parse_data->cluster_id= cluster_id;
    }
  }
  g_snprintf(buf, 128, "Cluster id set to %u",
             (guint32)parse_data->cluster_id);
  if (ic_send_with_cr(parse_data->conn, buf) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_display_stats_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "DISPLAY STATS") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_top_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "TOP") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_use_ndb_version_cmd(IC_PARSE_DATA *parse_data)
{
  if (ic_send_with_cr(parse_data->conn, "NDB version identifier set to:") ||
      ic_send_with_cr(parse_data->conn, parse_data->ndb_version_name.str) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
ic_use_iclaustron_version_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_use_iclaustron_version_cmd");
  if (ic_send_with_cr(parse_data->conn, "USE VERSION ICLAUSTRON") ||
      ic_send_with_cr(parse_data->conn, not_impl_string) ||
      ic_send_empty_line(parse_data->conn))
  {
    parse_data->exit_flag= TRUE;
  }
  DEBUG_RETURN_EMPTY;
}

static void
mgr_execute(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("mgr_execute");
  switch (parse_data->command)
  {
    case IC_DIE_CMD:
      ic_die_cmd(parse_data);
      break;
    case IC_KILL_CMD:
      ic_kill_cmd(parse_data);
      break;
    case IC_MOVE_CMD:
      ic_move_cmd(parse_data);
      break;
    case IC_PERFORM_BACKUP_CMD:
      ic_perform_backup_cmd(parse_data);
      break;
    case IC_PERFORM_ROLLING_UPGRADE_CMD:
      ic_perform_rolling_upgrade_cmd(parse_data);
      break;
    case IC_RESTART_CMD:
      ic_restart_cmd(parse_data);
      break;
    case IC_START_CMD:
      ic_start_cmd(parse_data);
      break;
    case IC_STOP_CMD:
      ic_stop_cmd(parse_data);
      break;
    case IC_LIST_CMD:
      ic_list_cmd(parse_data);
      break;
    case IC_LISTEN_CMD:
      ic_listen_cmd(parse_data);
      break;
    case IC_SHOW_CLUSTER_CMD:
      ic_show_cluster_cmd(parse_data);
      break;
    case IC_SHOW_CLUSTER_STATUS_CMD:
      ic_show_cluster_status_cmd(parse_data);
      break;
    case IC_SHOW_CONNECTIONS_CMD:
      ic_show_connections_cmd(parse_data);
      break;
    case IC_SHOW_CONFIG_CMD:
      ic_show_config_cmd(parse_data);
      break;
    case IC_SHOW_MEMORY_CMD:
      ic_show_memory_cmd(parse_data);
      break;
    case IC_SHOW_STATVARS_CMD:
      ic_show_statvars_cmd(parse_data);
      break;
    case IC_SHOW_STATS_CMD:
      ic_show_stats_cmd(parse_data);
      break;
    case IC_SET_STAT_LEVEL_CMD:
      ic_set_stat_level_cmd(parse_data);
      break;
    case IC_USE_CLUSTER_CMD:
      ic_use_cluster_cmd(parse_data);
      break;
      DEBUG_RETURN_EMPTY;
    case IC_DISPLAY_STATS_CMD:
      ic_display_stats_cmd(parse_data);
      break;
    case IC_TOP_CMD:
      ic_top_cmd(parse_data);
      break;
    case IC_USE_VERSION_NDB_CMD:
      ic_use_ndb_version_cmd(parse_data);
      break;
    case IC_USE_VERSION_ICLAUSTRON_CMD:
      ic_use_iclaustron_version_cmd(parse_data);
      break;
    default:
      report_error(parse_data, not_impl_string);
      break;
  }
  DEBUG_RETURN_EMPTY;
}

static void
init_parse_data(IC_PARSE_DATA *parse_data)
{
  parse_data->command= IC_NO_SUCH_CMD;
  parse_data->binary_type= IC_NOT_EXIST_NODE_TYPE;
  parse_data->default_cluster= TRUE;
  parse_data->cluster_all= FALSE;
  parse_data->node_all= FALSE;
}

static gpointer
run_handle_new_connection(gpointer data)
{
  gchar *read_buf;
  guint32 read_size;
  int ret_code;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state;
  IC_CONNECTION *conn;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  IC_API_CONFIG_SERVER *apic;
  gchar *parse_buf;
  guint32 parse_inx= 0;
  IC_PARSE_DATA parse_data;
  DEBUG_THREAD_ENTRY("run_handle_new_connection");
  tp_state= thread_state->ic_get_threadpool(thread_state);
  conn= (IC_CONNECTION*)
    tp_state->ts_ops.ic_thread_get_object(thread_state);

  tp_state->ts_ops.ic_thread_started(thread_state);

  apic= (IC_API_CONFIG_SERVER*)conn->conn_op.ic_get_param(conn);
  ic_zero(&parse_data, sizeof(IC_PARSE_DATA));
  if (!(parse_buf= ic_malloc(PARSE_BUF_SIZE)))
  {
    ic_print_error(IC_ERROR_MEM_ALLOC);
    goto error;
  }
  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0, FALSE)))
  {
    ic_print_error(IC_ERROR_MEM_ALLOC);
    goto error;
  }
  parse_data.lex_data.mc_ptr= mc_ptr;
  parse_data.apic= apic;
  parse_data.conn= conn;
  parse_data.current_cluster_id= IC_MAX_UINT32;
  DEBUG_PRINT(THREAD_LEVEL, ("conn: %p", conn));
  while (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (read_size == 0)
    {
      init_parse_data(&parse_data);
      parse_buf[parse_inx]= NULL_BYTE;
      parse_buf[parse_inx+1]= NULL_BYTE;
      parse_inx+= 2;
      DEBUG_PRINT(PROGRAM_LEVEL,
        ("Ready to execute command:\n%s", parse_buf));
      ic_mgr_call_parser(parse_buf, parse_inx, &parse_data);
      if (parse_data.exit_flag)
        goto exit;
      mgr_execute(&parse_data);
      if (parse_data.exit_flag)
        goto exit;
      /* Initialise index to parser buffer before beginning of new cmd */
      parse_inx= 0;
      /* Release memory from old query but reuse memory container */
      mc_ptr->mc_ops.ic_mc_reset(mc_ptr);
    }
    else
    {
      memcpy(parse_buf+parse_inx, read_buf, read_size);
      parse_inx+= read_size;
    }
  }
exit:
  DEBUG_PRINT(PROGRAM_LEVEL, ("End of client connection, ret_code = %d"
                              " parse_inx = %u",
              ret_code,
              parse_inx));

error:
  ic_free(parse_buf);
  if (mc_ptr)
  {
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  }
  conn->conn_op.ic_free_connection(conn);
  tp_state->ts_ops.ic_thread_stops(thread_state);
  DEBUG_THREAD_RETURN;
}

static int
handle_new_connection(IC_CONNECTION *conn,
                      IC_API_CONFIG_SERVER *apic,
                      IC_THREADPOOL_STATE *tp_state,
                      guint32 thread_id)
{
  int ret_code;
  DEBUG_ENTRY("handle_new_connection");

  conn->conn_op.ic_set_param(conn, (void*)apic);
  DEBUG_PRINT(THREAD_LEVEL, ("Starting thread in run_handle_new_connection"));
  if ((ret_code= tp_state->tp_ops.ic_threadpool_start_thread_with_thread_id(
                            tp_state,
                            thread_id,
                            run_handle_new_connection,
                            (gpointer)conn,
                            IC_MEDIUM_STACK_SIZE,
                            FALSE)))
  {
    DEBUG_RETURN_INT(ret_code);
  }
  DEBUG_RETURN_INT(0);
}
static int
wait_for_connections_and_fork(IC_CONNECTION *conn,
                              IC_API_CONFIG_SERVER *apic,
                              IC_THREADPOOL_STATE *tp_state)
{
  int ret_code;
  guint32 thread_id;
  gboolean allocated_thread= FALSE;
  IC_CONNECTION *fork_conn;
  DEBUG_ENTRY("wait_for_connections_and_fork");

  while (!tp_state->tp_ops.ic_threadpool_get_stop_flag(tp_state))
  {
    if ((ret_code= tp_state->tp_ops.ic_threadpool_get_thread_id_wait(
                                                     tp_state,
                                                     &thread_id,
                                                     IC_MAX_THREAD_WAIT_TIME)))
      goto error;
    allocated_thread= TRUE;
    if ((ret_code= conn->conn_op.ic_accept_connection(conn)))
      goto error;
    DEBUG_PRINT(PROGRAM_LEVEL,
      ("Cluster Manager has accepted a new connection"));
    if (!(fork_conn= conn->conn_op.ic_fork_accept_connection(conn,
                                              FALSE))) /* No mutex */
    {
      DEBUG_PRINT(PROGRAM_LEVEL,
        ("Failed to fork a new connection from an accepted connection"));
      goto error;
    }
    if ((ret_code= handle_new_connection(fork_conn, apic,
                                         tp_state, thread_id)))
    {
      DEBUG_PRINT(PROGRAM_LEVEL,
        ("Failed to start new Cluster Manager thread"));
      goto error;
    }
    /**
     * Now it's up to the connection thread to handle the thread id,
     * we can safely forget it and rely upon the new thread to take
     * care of its release.
     */
    allocated_thread= FALSE;
  }
  DEBUG_RETURN_INT(0);

error:
  if (allocated_thread)
  {
    tp_state->tp_ops.ic_threadpool_free_thread_id(tp_state, thread_id);
  }
  DEBUG_RETURN_INT(ret_code);
}

static int
set_up_server_connection(IC_CONNECTION **conn)
{
  IC_CONNECTION *loc_conn;
  int ret_code;
  DEBUG_ENTRY("set_up_server_connection");

  if (!(loc_conn= ic_create_socket_object(FALSE, TRUE, FALSE,
                                          COMMAND_READ_BUF_SIZE,
                                          NULL, NULL)))
  {
    DEBUG_PRINT(COMM_LEVEL, ("Failed to create Connection object"));
    DEBUG_RETURN_INT(1);
  }
  DEBUG_PRINT(COMM_LEVEL,
    ("Setting up server connection for Cluster Manager at %s:%s",
     glob_cluster_mgr_ip, glob_cluster_mgr_port));

  loc_conn->conn_op.ic_prepare_server_connection(loc_conn,
    glob_cluster_mgr_ip,
    glob_cluster_mgr_port,
    NULL, NULL,           /* Client can connect from anywhere */
    0,                    /* Default backlog */
    TRUE);                /* Listen socket retained */

  if ((ret_code= loc_conn->conn_op.ic_set_up_connection(loc_conn, NULL, NULL)))
  {
    DEBUG_PRINT(COMM_LEVEL,
      ("Failed to set-up connection for Cluster Manager"));
    loc_conn->conn_op.ic_free_connection(loc_conn);
    DEBUG_RETURN_INT(1);
  }

  ic_printf("Successfully set-up connection for Cluster Manager at %s:%s",
            glob_cluster_mgr_ip, glob_cluster_mgr_port);
  *conn= loc_conn;
  DEBUG_RETURN_INT(0);
}

int main(int argc,
         char *argv[])
{
  int ret_code;
  IC_CONNECTION *conn= NULL;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar error_str[ERROR_MESSAGE_SIZE];
  gchar *err_str= error_str;
  IC_THREADPOOL_STATE *tp_state= NULL;

  if ((ret_code= ic_start_program(argc,
                                  argv,
                                  entries,
                                  ic_apid_entries,
                                  "ic_clmgrd",
                                  "- iClaustron Cluster Manager",
                                  TRUE,
                                  TRUE)))
    goto end;
  if (ic_mgr_find_hash_function())
  {
    ic_printf("Failed to setup a proper hash function");
    return 1;
  }
  if (glob_only_find_hash)
  {
    ic_printf("Succeeded in setting up a proper hash function");
    return 0;
  }
  if ((ret_code= ic_start_apid_program(&tp_state,
                                       &err_str,
                                       error_str,
                                       &apid_global,
                                       &apic)))
    goto end;
  if ((ret_code= set_up_server_connection(&conn)))
    goto end;
  ret_code= wait_for_connections_and_fork(conn, apic, tp_state);
  conn->conn_op.ic_free_connection(conn);
end:
  ic_stop_apid_program(ret_code,
                       err_str,
                       apid_global,
                       apic,
                       tp_state);
  return ret_code;
}
