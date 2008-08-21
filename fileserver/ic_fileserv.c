/* Copyright (C) 2007, 2008 iClaustron AB

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
#include <ic_apic.h>
#include <ic_apid.h>

static IC_STRING glob_config_dir= { NULL, 0, TRUE};
static gchar *glob_process_name= "ic_fsd";

static gchar *glob_cluster_server_ip= "127.0.0.1";
static gchar *glob_cluster_server_port= "10203";
static gchar *glob_config_path= NULL;
static guint32 glob_node_id= 5;

static GOptionEntry entries[] = 
{
  { "cluster_server_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_server_ip,
    "Set Server Hostname of Cluster Server", NULL},
  { "cluster_server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "node_id", 0, 0, G_OPTION_ARG_INT,
    &glob_node_id,
    "Node id of Cluster Manager in all clusters", NULL},
  { "config_dir", 0, 0, G_OPTION_ARG_STRING,
    &glob_config_path,
    "Specification of Clusters to manage for Cluster Manager with access info",
     NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
run_file_server(IC_API_CONFIG_SERVER *apic, IC_APID_GLOBAL *apid_global)
{
  printf("Ready to start file server\n");
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_API_CLUSTER_CONNECTION api_cluster_conn;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar config_path_buf[IC_MAX_FILE_NAME_SIZE];

  if ((ret_code= ic_start_program(argc, argv, entries,
            "- iClaustron Cluster File Server")))
    return ret_code;
  if ((ret_code= ic_set_config_path(&glob_config_dir,
                                    glob_config_path,
                                    config_path_buf)))
    return ret_code;
  ret_code= 1;
  if (!(apic= ic_get_configuration(&api_cluster_conn,
                                   &glob_config_dir,
                                   glob_node_id,
                                   glob_cluster_server_ip,
                                   glob_cluster_server_port,
                                   &ret_code)))
    goto error;
  if (!(apid_global= ic_init_apid(apic)))
    return IC_ERROR_MEM_ALLOC;
  ret_code= run_file_server(apic, apid_global);
error:
  if (apic)
    apic->api_op.ic_free_config(apic);
  ic_end_apid(apid_global);
  ic_end();
  return ret_code;
}

