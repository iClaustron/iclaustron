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
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_parse_connectstring.h>
#include <ic_threadpool.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_apid.h>

static IC_STRING glob_config_dir= { NULL, 0, TRUE};
static const gchar *glob_process_name= "ic_fsd";

static gchar *glob_cluster_server_ip= "127.0.0.1";
static gchar *glob_cluster_server_port= IC_DEF_CLUSTER_SERVER_PORT_STR;
static gchar *glob_cs_connectstring= NULL;
static gchar *glob_data_path= NULL;
static guint32 glob_node_id= 5;
static guint32 glob_num_threads= 1;
static guint32 glob_use_iclaustron_cluster_server= 1;
static IC_THREADPOOL_STATE *glob_tp_state= NULL;

static GOptionEntry entries[] = 
{
  { "cs_connectstring", 0, 0, G_OPTION_ARG_STRING,
    &glob_cs_connectstring,
    "Connect string to Cluster Servers", NULL},
  { "cs_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_server_ip,
    "Set Server Hostname of Cluster Server", NULL},
  { "cs_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "node_id", 0, 0, G_OPTION_ARG_INT,
    &glob_node_id,
    "Node id of file server in all clusters", NULL},
  { "data_dir", 0, 0, G_OPTION_ARG_FILENAME, &glob_data_path,
    "Sets path to data directory, config files in subdirectory config", NULL},
  { "num_threads", 0, 0, G_OPTION_ARG_INT,
    &glob_num_threads,
    "Number of threads executing in file server process", NULL},
  { "use_iclaustron_cluster_server", 0, 0, G_OPTION_ARG_INT,
     &glob_use_iclaustron_cluster_server,
    "Use of iClaustron Cluster Server (default or NDB mgm server", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
run_replication_server(IC_APID_GLOBAL *apid_global,
                       gchar **error_str)
{
  (void)apid_global;
  (void)error_str;
  return 0;
}

int main(int argc,
         char *argv[])
{
  int ret_code;
  IC_API_CLUSTER_CONNECTION api_cluster_conn;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar config_path_buf[IC_MAX_FILE_NAME_SIZE];
  gchar error_str[ERROR_MESSAGE_SIZE];
  gchar *err_str= error_str;
  IC_CONNECT_STRING conn_str;

  if ((ret_code= ic_start_program(argc, argv, entries, glob_process_name,
            "- iClaustron Replication Server", TRUE)))
    goto early_error;
  if (!(glob_tp_state=
          ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto early_error;
  }
  if ((ret_code= ic_set_config_path(&glob_config_dir,
                                    glob_data_path,
                                    config_path_buf)))
    goto early_error;
  if ((ret_code= ic_parse_connectstring(glob_cs_connectstring,
                                        &conn_str,
                                        glob_cluster_server_ip,
                                        glob_cluster_server_port)))
    goto early_error;
  ret_code= 1;
  apic= ic_get_configuration(&api_cluster_conn,
                             &glob_config_dir,
                             glob_node_id,
                             conn_str.num_cs_servers,
                             conn_str.cs_hosts,
                             conn_str.cs_ports,
                             glob_use_iclaustron_cluster_server,
                             &ret_code,
                             &err_str);
  conn_str.mc_ptr->mc_ops.ic_mc_free(conn_str.mc_ptr);
  if (!apic)
    goto error;
  if (!(apid_global= ic_connect_apid_global(apic, &ret_code, &err_str)))
    goto error;
  if ((ret_code= run_replication_server(apid_global, &err_str)))
    goto error;
end:
  if (apid_global)
    ic_disconnect_apid_global(apid_global);
  if (apic)
    apic->api_op.ic_free_config(apic);
  ic_end();
  return ret_code;

early_error:
  err_str= NULL;
error:
  if (err_str)
    printf("%s", err_str);
  else
    ic_print_error(ret_code);
  goto end;
}
