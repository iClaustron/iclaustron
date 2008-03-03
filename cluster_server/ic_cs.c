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

/*
  This program implements the iClaustron Cluster Server.
  It reads a configuration file at bootstrap, one file per cluster
  and maintans versions of this configuration file in a separate
  directory and controls updates to these configuration files. It
  maintains a versioned central file that keeps track of the clusters
  and the configuration files for them.

  To change a configuration after bootstrap can be done either by
  editing a configuration file checked out from the configuration
  server or by using commands towards the Cluster Server.
*/

#include <ic_common.h>
#include <ic_apic.h>

static gchar *config_ending_str= ".ini";
static gchar *glob_config_file= "config.ini";
static gchar *glob_config_path= NULL;
static gboolean glob_bootstrap= FALSE;
static gint glob_cluster_id= 0;
static gchar *glob_server_name= "127.0.0.1";
static gchar *glob_server_port= "10203";

static GOptionEntry entries[] = 
{
  { "bootstrap", 0, 0, G_OPTION_ARG_NONE, &glob_bootstrap,
    "Is this bootstrap of a cluster", NULL},
  { "config_file", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_file,
    "Sets path to bootstrap configuration file", NULL},
  { "config_path", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_path,
    "Sets path to directory of configuration files", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Cluster Server connection Port", NULL},
  { "server_name", 0, 0, G_OPTION_ARG_STRING, &glob_server_name,
    "Set Cluster Server Hostname", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
load_config_files(IC_CLUSTER_CONNECT_INFO **clu_infos,
                  IC_CLUSTER_CONFIG **clusters,
                  IC_CONFIG_STRUCT *conf_server_struct,
                  IC_MEMORY_CONTAINER *mc_ptr,
                  gchar *config_file)
{
  IC_STRING base_dir_string;
  IC_STRING file_name_string;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  IC_CLUSTER_CONFIG *cluster;
  gchar dir_name[IC_MAX_FILE_NAME_SIZE];
  gchar file_name[IC_MAX_FILE_NAME_SIZE];
  gchar *base_dir;
  IC_STRING end_pattern;

  conf_server_struct->perm_mc_ptr= mc_ptr;
  base_dir= ic_convert_file_to_dir(dir_name, config_file);
  IC_INIT_STRING(&base_dir_string, base_dir, strlen(base_dir), TRUE);
  IC_INIT_STRING(&end_pattern, config_ending_str, strlen(config_ending_str),
                 TRUE);
  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
    file_name[0]= 0;
    IC_INIT_STRING(&file_name_string, file_name, 0, TRUE);
    ic_add_ic_string(&file_name_string, &base_dir_string);
    ic_add_ic_string(&file_name_string, &clu_info->cluster_name);
    ic_add_ic_string(&file_name_string, &end_pattern);
    /*
      We have now formed the filename of the configuration of this
      cluster. It's now time to open the configuration file and
      convert it into a IC_CLUSTER_CONFIG struct.
    */
    if (!(cluster= ic_load_config_server_from_files(file_name_string.str,
                                                    conf_server_struct)))
    {
      printf("Failed to load config file %s from disk\n",
             file_name_string.str);
      return 1;
    }
    /*
      Copy information from cluster configuration file which isn't set in
      the configuration and ensure it's allocated on the proper memory
      container.
    */
    if (ic_mc_strdup(mc_ptr, &cluster->clu_info.cluster_name,
                     &clu_info->cluster_name))
      return 1;
    if (ic_mc_strdup(mc_ptr, &cluster->clu_info.password,
                     &clu_info->password))
      return 1;
    cluster->clu_info.cluster_id= clu_info->cluster_id;

    *clusters= cluster;
    clusters++;
  }
  *clusters= NULL;
  return 0;
}

static IC_CLUSTER_CONNECT_INFO**
load_cluster_config(gchar *cluster_config_file,
                    IC_CONFIG_STRUCT *cluster_conf,
                    IC_MEMORY_CONTAINER *mc_ptr)
{
  cluster_conf->perm_mc_ptr= mc_ptr;
  return ic_load_cluster_config_from_file(cluster_config_file,
                                          cluster_conf);
}

int
main(int argc, char *argv[])
{
  int error= 1;
  IC_CONFIG_STRUCT conf_server_struct, cluster_conf_struct;
  IC_CLUSTER_CONFIG *clusters[IC_MAX_CLUSTER_ID+1];
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  IC_RUN_CLUSTER_SERVER *run_obj;
  IC_MEMORY_CONTAINER *mc_ptr, *cluster_mc_ptr;

  mc_ptr= cluster_mc_ptr= NULL;
  if ((error= ic_start_program(argc, argv, entries,
           "- iClaustron Cluster Server")))
    return error;
  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
  {
    printf("Failed to create memory container for configuration\n");
    goto end;
  }
  if (!(cluster_mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
  {
    printf("Failed to create memory container for cluster configuration\n");
    goto end;
  }
  if (!(clu_infos= load_cluster_config(glob_config_file,
                                           &cluster_conf_struct,
                                           cluster_mc_ptr)))
  {
    printf("Failed to load the cluster configuration file\n");
    goto end;
  }
  if (load_config_files(clu_infos, clusters, &conf_server_struct,
                        mc_ptr, glob_config_file))
  {
    printf("Failed to load a configuration file\n");
    goto end;
  }
  cluster_mc_ptr->mc_ops.ic_mc_free(cluster_mc_ptr);
  cluster_mc_ptr= NULL;
  if (!(run_obj= ic_create_run_cluster(clusters, mc_ptr, glob_server_name,
                                       glob_server_port)))
  {
    printf("Failed to initialise run_cluster object\n");
    goto late_end;
  }
  DEBUG_PRINT(PROGRAM_LEVEL,
    ("Starting the iClaustron Cluster Server\n"));
  if ((error= run_obj->run_op.ic_run_cluster_server(run_obj)))
  {
    DEBUG_PRINT(PROGRAM_LEVEL,
      ("run_ic_cluster_server returned error code %u\n", error));
    ic_print_error(error);
    goto late_end;
  }
  error= 0;
  run_obj->run_op.ic_free_run_cluster(run_obj);
late_end:
  conf_server_struct.clu_conf_ops->ic_config_end(&conf_server_struct);
  cluster_conf_struct.clu_conf_ops->ic_config_end(&cluster_conf_struct);
end:
  if (cluster_mc_ptr)
    cluster_mc_ptr->mc_ops.ic_mc_free(cluster_mc_ptr);
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  ic_end();
  return error;
}

