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
  It reads a configuration file at bootstrap plus one file per cluster
  and maintans versions of these configuration files in separate
  files and controls updates to these configuration files. It
  maintains a versioned central file that keeps track of the clusters
  and the configuration files for them.

  It is intended that change to the configuration after bootstrap can be
  done either by editing a configuration file checked out from the
  configuration server or by using commands towards the Cluster Server.
*/

#include <ic_common.h>
#include <ic_apic.h>

/* Global variables */
static gchar *err_str= "Error:";
static IC_STRING glob_config_dir;

/* Option variables */
static gchar *glob_config_path= NULL;
static gboolean glob_bootstrap= FALSE;
static gchar *glob_server_name= "127.0.0.1";
static gchar *glob_server_port= "10203";

static GOptionEntry entries[] = 
{
  { "bootstrap", 0, 0, G_OPTION_ARG_NONE, &glob_bootstrap,
    "Is this bootstrap of a cluster", NULL},
  { "base_dir", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_path,
    "Sets path to base directory, config files in subdirectory config", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Cluster Server connection Port", NULL},
  { "server_name", 0, 0, G_OPTION_ARG_STRING, &glob_server_name,
    "Set Cluster Server Hostname", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

/*
  Cluster Server Start Options:

  The very first start of a Cluster Server always uses the --bootstrap flag.
  When this flag is set one reads the cluster configuration file and each
  of the configuration files for each node in the cluster. After reading these
  files the Cluster Server writes version 1 of the configuration.

  If there are several Cluster Servers in the cluster, then only one of them
  should use the --bootstrap flag. The other ones should start without this
  flag.

  When starting without the flag one will read the configuration version file,
  the cluster configuration file for this version and the configuration files
  for each cluster in this version.

  In the case of a first start of a Cluster Server it won't be possible to
  find those files since they haven't been created yet. However in order to
  start at all, at least a cluster configuration file is required, this file
  will contain name, id and password for each cluster and hostname and port
  for each Cluster Server in the cluster. This file is required to start-up
  any node in a iClaustron grid.

  After reading the local configuration files a node will attempt to connect
  to any other Cluster Servers. If this is unsuccessful and no configuration
  files were present then the Cluster Server will fail with an error code
  after waiting an appropriate time.

  If connect is successful and the version read from the connected server is
  equal to our own read version, then we will fetch configuration from the
  server and verify its correctness. If it's unequal then we'll fetch
  configuration from the server connected, verify the received configuration,
  install the new configuration, update the configuration version file,
  remove the old configuration version, update the configuration version
  file again to indicate the old version is removed.

  If connect was unsuccessful and we had local configuration files then we'll
  start-up our server connection. After that we'll in parallel make more
  attempts to connect as clients to the other Cluster Servers while we're
  also allowing other nodes to connect to us.

  If no other Cluster Server is heard from then we'll start replying to
  any requests from other nodes, also other nodes than Cluster Servers.
  If a Cluster Server contacted us through the server interface while we
  were unsuccessful in contacting this node through the client interface,
  then we'll synchronize with this Cluster Server. If we received a
  connection in parallel with managing to connect to the same Cluster
  Server we'll synchronize with this Cluster Server.

  The names of the configuration files is fixed, it is always config.ini for
  the cluster configuration file, and it will be config.version for the file
  that contains the version of the current configuration. If the version is
  3 then the files created will be called config.ini.3 and the configuration
  files of the cluster will always be called the name of the cluster + .ini.
  Thus for a cluster called kalle it will kalle.ini and versioned it will be
  kalle.ini.3
  The only parameter thus needed for the Cluster Server is which directory
  those files are stored in. The remaining information is always the same
  or provided in configuration files.
*/

/*
 The default configuration directory is ICLAUSTRON_BASE_DIR/config
*/
static int
set_config_path(gchar *buf)
{
  int error;
  IC_STRING base_dir;
  if ((error= ic_set_base_dir(&base_dir, glob_config_path)))
    return error;
  ic_set_binary_base_dir(&glob_config_dir, &base_dir, buf,
                         ic_config_string.str);
  DEBUG_PRINT(CONFIG_LEVEL, ("Config dir: %s", glob_config_dir.str));
  return 0;
}

static guint32
load_config_version_number()
{
  guint32 version_number;
  guint32 state;
  int error;

  if ((error= ic_read_config_version_file(&glob_config_dir,
                                          &version_number,
                                          &state)))
  {
    /* More code to write */
    exit(1);
    return 0;
  }
  return version_number;
}

static int
load_config_files(IC_CLUSTER_CONNECT_INFO **clu_infos,
                  IC_CLUSTER_CONFIG **clusters,
                  IC_CONFIG_STRUCT *conf_server_struct,
                  IC_MEMORY_CONTAINER *mc_ptr,
                  guint32 config_version_number)
{
  IC_CLUSTER_CONNECT_INFO *clu_info;
  IC_CLUSTER_CONFIG *cluster;
  IC_STRING file_name_string;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];

  conf_server_struct->perm_mc_ptr= mc_ptr;
  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
    ic_create_config_file_name(&file_name_string, file_name,
                               &glob_config_dir,
                               &clu_info->cluster_name,
                               config_version_number);
    /*
      We have now formed the filename of the configuration of this
      cluster. It's now time to open the configuration file and
      convert it into a IC_CLUSTER_CONFIG struct.
    */
    if (!(cluster= ic_load_config_server_from_files(file_name,
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

static int
verify_grid_config(IC_CLUSTER_CONFIG **clusters)
{
  guint32 i;
  guint32 max_grid_node_id= 0;
  gboolean first_cs_or_cm= FALSE;
  gboolean first;
  IC_CLUSTER_CONFIG **cluster_iter= clusters;
  IC_NODE_TYPES node_type;

  while (*cluster_iter)
  {
    max_grid_node_id= IC_MAX(max_grid_node_id, (*cluster_iter)->max_node_id);
    cluster_iter++;
  }
  for (i= 1; i <= max_grid_node_id; i++)
  {
    first= TRUE;
    cluster_iter= clusters;
    while (*cluster_iter)
    {
      IC_CLUSTER_CONFIG *cluster= *cluster_iter;
      cluster_iter++;
      if (first)
      {
        if (i <= cluster->max_node_id)
          node_type= cluster->node_types[i];
        else
          node_type= IC_NOT_EXIST_NODE_TYPE;
        first_cs_or_cm= node_type == IC_CLUSTER_SERVER_NODE ||
                        node_type == IC_CLUSTER_MGR_NODE;
        first= FALSE;
      }
      else
      {
        if (first_cs_or_cm)
        {
          if (i > cluster->max_node_id ||
              cluster->node_types[i] != node_type)
            goto error;
        }
        else
        {
          if (i <= cluster->max_node_id &&
              (cluster->node_types[i] == IC_CLUSTER_SERVER_NODE ||
               cluster->node_types[i] == IC_CLUSTER_MGR_NODE))
            goto error;
        }
      }
    }
  }
  return 0;
error:
  printf("%s Grids require cluster managers/servers to be on same nodeid in all clusters\n",
         err_str);
  return 1;
}

static int
load_local_config(IC_MEMORY_CONTAINER *mc_ptr,
                  IC_CONFIG_STRUCT *conf_server_ptr,
                  IC_CONFIG_STRUCT *cluster_conf_ptr,
                  IC_CLUSTER_CONFIG **clusters,
                  guint32 *config_version_number)
{
  int error;
  IC_MEMORY_CONTAINER *cluster_mc_ptr;
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  gchar *cluster_config_file;
  IC_STRING file_name_string;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];

  if (!(cluster_mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
  {
    printf("%s Failed to create memory container for cluster configuration\n",
           err_str);
    return 1;
  }
  *config_version_number= load_config_version_number();
  if (glob_bootstrap && *config_version_number)
  {
    printf("Bootstrap has already been performed\n");
    goto error;
  }
  ic_create_config_file_name(&file_name_string, file_name,
                             &glob_config_dir,
                             &ic_config_string,
                             *config_version_number);
  cluster_conf_ptr->perm_mc_ptr= cluster_mc_ptr;
  if (!(clu_infos= ic_load_cluster_config_from_file(file_name,
                                                    cluster_conf_ptr)))
  {
    printf("%s Failed to load the cluster configuration file %s\n",
           err_str, cluster_config_file);
    goto error;
  }
  if (load_config_files(clu_infos, clusters, conf_server_ptr,
                        mc_ptr, *config_version_number))
  {
    printf("%s Failed to load a configuration file\n", err_str);
    cluster_conf_ptr->clu_conf_ops->ic_config_end(cluster_conf_ptr);
    goto error;
  }
  if (verify_grid_config(clusters))
    goto error;
  if (glob_bootstrap && (*config_version_number == 0))
  {
    if ((error= ic_write_full_config_to_disk(&glob_config_dir,
                                             config_version_number,
                                             clu_infos,
                                             clusters)))
    {
      printf("%s Failed to write initial configuration version\n", err_str);
      cluster_conf_ptr->clu_conf_ops->ic_config_end(cluster_conf_ptr);
      goto error;
    }
  }
  cluster_mc_ptr->mc_ops.ic_mc_free(cluster_mc_ptr);
  return 0;
error:
  cluster_mc_ptr->mc_ops.ic_mc_free(cluster_mc_ptr);
  return 1;
}

int
main(int argc, char *argv[])
{
  int error;
  IC_CONFIG_STRUCT conf_server_struct, cluster_conf_struct;
  IC_CLUSTER_CONFIG *clusters[IC_MAX_CLUSTER_ID+1];
  IC_RUN_CLUSTER_SERVER *run_obj;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  gchar buf[IC_MAX_FILE_NAME_SIZE];
  guint32 config_version_number;

  if ((error= ic_start_program(argc, argv, entries,
           "- iClaustron Cluster Server")))
    return error;
  if ((error= set_config_path(buf)))
    return error;
  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
  {
    printf("%s Failed to create memory container for configuration\n",
           err_str);
    goto end;
  }
  if ((error= load_local_config(mc_ptr, &conf_server_struct,
                                &cluster_conf_struct, clusters,
                                &config_version_number)))
    goto end;
  if (!(run_obj= ic_create_run_cluster(clusters, mc_ptr, glob_server_name,
                                       glob_server_port)))
  {
    printf("%s Failed to initialise run_cluster object\n", err_str);
    error= 1;
    goto late_end;
  }
  run_obj->state.bootstrap= glob_bootstrap;
  run_obj->state.config_dir= &glob_config_dir;
  run_obj->state.config_version_number= config_version_number;
  DEBUG_PRINT(PROGRAM_LEVEL,
    ("Starting the iClaustron Cluster Server"));
  if ((error= run_obj->run_op.ic_run_cluster_server(run_obj)))
  {
    DEBUG_PRINT(PROGRAM_LEVEL,
      ("run_ic_cluster_server returned error code %u", error));
    ic_print_error(error);
  }

late_end:
  run_obj->run_op.ic_free_run_cluster(run_obj);
  conf_server_struct.clu_conf_ops->ic_config_end(&conf_server_struct);
  cluster_conf_struct.clu_conf_ops->ic_config_end(&cluster_conf_struct);
end:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  ic_free(glob_config_dir.str);
  ic_end();
  return error;
}

