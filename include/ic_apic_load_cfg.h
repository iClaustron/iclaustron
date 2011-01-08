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

#ifndef IC_APIC_LOAD_CFG_H
#define IC_APIC_LOAD_CFG_H
/*
  Support to read config files
  ----------------------------
  ic_load_cluster_config_from_file
    Loads the list of clusters in config.ini file
  ic_load_grid_common_config_server_from_file
    Loads the Cluster Server and Cluster Manager configuration from the
    file grid_common.ini
  ic_load_config_server_from_files
    Loads configuration for all other node types for one cluster with a
    given name.

  The first IC_CLUSTER_CONNECT_INFO object returned from
  ic_load_cluster_config_from_file is passed to both the other methods.
  The IC_CLUSTER_CONFIG object returned from
  ic_load_grid_common_server_from_file is passed to
  ic_load_config_server_from_files calls.
  The cluster names to load up can be found in the IC_CLUSTER_CONNECT_INFO
  passed in ic_load_config_server_from_files.
  This means that it is natural to start with calling
  ic_load_cluster_config_from_file, next step is to call
  ic_load_grid_common_config_server_from_file
  and finally to loop over all IC_CLUSTER_CONNECT_INFO objects returned from
  ic_load_cluster_config_from_file.
*/
IC_CLUSTER_CONNECT_INFO**
ic_load_cluster_config_from_file(IC_STRING *config_dir,
                                 IC_CONF_VERSION_TYPE cfg_version_number,
                                 IC_MEMORY_CONTAINER *mc_ptr,
                                 IC_CONFIG_ERROR *err_obj);
IC_CLUSTER_CONFIG*
ic_load_grid_common_config_server_from_file(IC_STRING *config_dir,
                                     IC_CONF_VERSION_TYPE cfg_version_number,
                                     IC_MEMORY_CONTAINER *mc_ptr,
                                     IC_CLUSTER_CONNECT_INFO *clu_connect_info,
                                     IC_CONFIG_ERROR *err_obj);
IC_CLUSTER_CONFIG*
ic_load_config_server_from_files(IC_STRING *config_dir,
                                 IC_CONF_VERSION_TYPE cfg_version_number,
                                 IC_MEMORY_CONTAINER *mc_ptr,
                                 IC_CLUSTER_CONFIG *grid_cluster_conf,
                                 IC_CLUSTER_CONNECT_INFO *clu_connect_info,
                                 IC_CONFIG_ERROR *err_obj);
#endif
