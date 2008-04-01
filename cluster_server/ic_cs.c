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
static gchar *ic_version_file_str= "config.version";
static gchar *ic_cluster_config_file= "config.ini";
static gchar *config_ending_str= ".ini";
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
  { "config_path", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_path,
    "Sets path to directory of configuration files", NULL},
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
  ic_set_binary_base_dir(&glob_config_dir, &base_dir, buf, "config");
  DEBUG_PRINT(CONFIG_LEVEL, ("Config dir: %s", glob_config_dir.str));
  return 0;
}

/* Create a string like ".3" if number is 3 */
static void
set_number_ending_string(gchar *buf, guint64 number)
{
  gchar *ignore_ptr;
  buf[0]= '.';
  ignore_ptr= ic_guint64_str(number,
                             &buf[1]);
}

static guint32
load_config_version_number(const gchar *file_path)
{
  gchar file_name[IC_MAX_FILE_NAME_SIZE];
  gchar *version_file_str;
  gsize version_file_len;
  IC_STRING file_name_string, file_path_string, loc_file_name_string;
  guint32 version_num_array[2];
  GError *loc_error= NULL;

  IC_INIT_STRING(&file_name_string, file_name, 0, TRUE);
  IC_INIT_STRING(&file_path_string, (gchar*)file_path,
                (file_path ? strlen(file_path) : 0), TRUE);
  IC_INIT_STRING(&loc_file_name_string, ic_version_file_str,
                 strlen(ic_version_file_str), TRUE);
  ic_add_ic_string(&file_name_string, &file_path_string);
  ic_add_ic_string(&file_name_string, &loc_file_name_string);
  /*
    We have now formed the file name of the version identifier file which
    contains the version number this Cluster Server most recently created.
  */
  if (!file_name_string.str ||
      !g_file_get_contents(file_name_string.str, &version_file_str,
                           &version_file_len, &loc_error))
    goto file_error;
  if (version_file_len != IC_VERSION_FILE_LEN)
    goto file_error;
  /* Now time to interpret the actual data file */
  memcpy(version_num_array, version_file_str, 2*sizeof(guint32));
  ic_free(version_file_str);
  version_num_array[0]= g_ntohl(version_num_array[0]);
  version_num_array[1]= g_ntohl(version_num_array[1]);
  return version_num_array[0];

file_error:
  return 0;
}

static int
load_config_files(IC_CLUSTER_CONNECT_INFO **clu_infos,
                  IC_CLUSTER_CONFIG **clusters,
                  IC_CONFIG_STRUCT *conf_server_struct,
                  IC_MEMORY_CONTAINER *mc_ptr,
                  IC_STRING *base_config_dir_string,
                  guint32 config_version_number)
{
  IC_STRING file_name_string;
  IC_STRING version_ending;
  IC_STRING end_pattern;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  IC_CLUSTER_CONFIG *cluster;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];
  gchar config_version_number_string[IC_MAX_INT_STRING];

  set_number_ending_string(config_version_number_string,
                           config_version_number);
  IC_INIT_STRING(&version_ending, config_version_number_string,
                 strlen(config_version_number_string), TRUE);
  IC_INIT_STRING(&end_pattern, config_ending_str, strlen(config_ending_str),
                 TRUE);
  conf_server_struct->perm_mc_ptr= mc_ptr;
  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
    file_name[0]= 0;
    IC_INIT_STRING(&file_name_string, file_name, 0, TRUE);
    ic_add_ic_string(&file_name_string, base_config_dir_string);
    ic_add_ic_string(&file_name_string, &clu_info->cluster_name);
    ic_add_ic_string(&file_name_string, &end_pattern);
    if (config_version_number)
      ic_add_ic_string(&file_name_string, &version_ending);
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
        first_cs_or_cm= node_type == IC_CLUSTER_SERVER_TYPE ||
                        node_type == IC_CLUSTER_MGR_TYPE;
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
              (cluster->node_types[i] == IC_CLUSTER_SERVER_TYPE ||
               cluster->node_types[i] == IC_CLUSTER_MGR_TYPE))
            goto error;
        }
      }
    }
  }
  return 0;
error:
  printf("%s Grids require cluster managers/servers to be on same nodeid\n",
         err_str);
  return 1;
}

/*
  Create a file name like $CONFIG_PATH/config.ini for initial version
  and $CONFIG_PATH/config.ini.3 if version number is 3.
*/
static gchar*
get_cluster_config_file_name(IC_STRING *path_string, gchar *buf,
                             guint32 config_version_number)
{
  gchar number_str[IC_MAX_INT_STRING];
  IC_STRING buf_string;
  IC_STRING version_ending, std_cluster_config_file;

  set_number_ending_string(number_str, (guint64)config_version_number);
  buf[0]= 0;
  IC_INIT_STRING(&buf_string, buf, 0, TRUE);
  IC_INIT_STRING(&std_cluster_config_file, ic_cluster_config_file,
                 strlen(ic_cluster_config_file), TRUE);
  IC_INIT_STRING(&version_ending, number_str, strlen(number_str), TRUE);

  ic_add_ic_string(&buf_string, path_string);
  ic_add_ic_string(&buf_string, &std_cluster_config_file);
  if (config_version_number)
    ic_add_ic_string(&buf_string, &version_ending);
  return buf;
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
  gchar cluster_config_file_buf[IC_MAX_FILE_NAME_SIZE];

  if (!(cluster_mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
  {
    printf("%s Failed to create memory container for cluster configuration\n",
           err_str);
    return 1;
  }
  *config_version_number= load_config_version_number(glob_config_path);
  if (glob_bootstrap && *config_version_number)
  {
    printf("Bootstrap has already been performed\n");
    goto error;
  }
  cluster_config_file= get_cluster_config_file_name(&glob_config_dir,
                                                    cluster_config_file_buf,
                                                    *config_version_number);
  cluster_conf_ptr->perm_mc_ptr= cluster_mc_ptr;
  if (!(clu_infos= ic_load_cluster_config_from_file(cluster_config_file,
                                                    cluster_conf_ptr)))
  {
    printf("%s Failed to load the cluster configuration file %s\n",
           err_str, cluster_config_file);
    goto error;
  }
  if (load_config_files(clu_infos, clusters, conf_server_ptr,
                        mc_ptr, &glob_config_dir, *config_version_number))
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

