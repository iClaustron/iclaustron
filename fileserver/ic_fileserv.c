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
run_file_server(IC_API_CONFIG_SERVER *apic)
{
  guint32 i;
  IC_CLUSTER_CONFIG *clu_conf;
  for (i= 0; i <= apic->max_cluster_id; i++)
  {
    clu_conf= apic->api_op.ic_get_cluster_config(apic, i);
    if (clu_conf)
    {
    }
  }
  printf("Ready to start file server\n");
  //ic_set_up_cluster_connections(co
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_API_CLUSTER_CONNECTION apic;
  IC_API_CONFIG_SERVER *config_server_obj= NULL;
  gchar config_path_buf[IC_MAX_FILE_NAME_SIZE];

  if ((ret_code= ic_start_program(argc, argv, entries,
            "- iClaustron Cluster File Server")))
    return ret_code;
  if ((ret_code= ic_set_config_path(&glob_config_dir,
                                    glob_config_path,
                                    config_path_buf)))
    return ret_code;
  ret_code= 1;
  if (!(config_server_obj= ic_get_configuration(&apic,
                                                &glob_config_dir,
                                                glob_node_id,
                                                glob_cluster_server_ip,
                                                glob_cluster_server_port,
                                                &ret_code)))
    goto error;
  ret_code= run_file_server(config_server_obj);
error:
  if (config_server_obj)
    config_server_obj->api_op.ic_free_config(config_server_obj);
  ic_end();
  return 1;
}

