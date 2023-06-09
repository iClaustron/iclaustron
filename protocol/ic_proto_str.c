/* Copyright (C) 2007, 2015 iClaustron AB

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

/* Messages for copy cluster server files protocol */
const gchar *ic_copy_cluster_server_files_str= "copy cluster server files";
const gchar *ic_copy_config_ini_str= "copy config.ini";
const gchar *ic_cluster_server_node_id_str= "cluster server node id:";
const gchar *ic_number_of_clusters_str= "number of clusters:";
const gchar *ic_receive_config_ini_str= "receive config.ini";
const gchar *ic_number_of_lines_str= "number of lines:";
const gchar *ic_receive_grid_common_ini_str= "receive grid_common.ini";
const gchar *ic_receive_cluster_name_ini_str= "receive";
const gchar *ic_installed_cluster_server_files_str=
  "installed cluster server files";
const gchar *ic_end_str= "end";
const gchar *ic_receive_config_file_ok_str= "receive config file ok";

/* Messages for the Get CPU info protocol */
const gchar *ic_get_cpu_info_str= "get cpu info";
const gchar *ic_number_of_processors_str= "number of processors:";
const gchar *ic_number_of_cpu_sockets_str= "number of CPU sockets:";
const gchar *ic_number_of_cpu_cores_str= "number of CPU cores:";
const gchar *ic_number_of_numa_nodes_str= "number of NUMA nodes:";
const gchar *ic_processor_str= "processor: ";
const gchar *ic_core_str= ", core:";
const gchar *ic_cpu_node_str= ", node:";
const gchar *ic_socket_str= ", socket:";
const gchar *ic_no_cpu_info_available_str= "no cpu info available";

/* Messages for the Get Memory Information Protocol */
const gchar *ic_get_mem_info_str= "get memory info";
const gchar *ic_number_of_mbyte_user_memory_str=
  "number of MByte user memory:";
const gchar *ic_mem_node_str= "node:";
const gchar *ic_mb_user_memory_str= ", MB user memory:";
const gchar *ic_no_mem_info_available_str= "no memory info available";

/* Messages for Get Disk Information Protocol */
const gchar *ic_get_disk_info_str= "get disk info";
const gchar *ic_dir_str= "dir: ";
const gchar *ic_disk_space_str= "number of GBytes disk space:";
const gchar *ic_no_disk_info_available_str= "no disk info available";

/* Messages used by Start/Stop/Kill/List process protocols */
const gchar *ic_ok_str= "Ok";
const gchar *ic_process_already_started_str= "Process already started";
const gchar *ic_version_str= "version:";
const gchar *ic_grid_str= "grid:";
const gchar *ic_cluster_str= "cluster:";
const gchar *ic_node_str= "node:";
const gchar *ic_program_str= "program:";
const gchar *ic_start_time_str= "start time:";
const gchar *ic_error_str= "Error";
const gchar *ic_start_str= "start";
const gchar *ic_stop_str= "stop";
const gchar *ic_kill_str= "kill";
const gchar *ic_list_str= "list";
const gchar *ic_list_full_str= "list full";
const gchar *ic_list_next_str= "list next";
const gchar *ic_list_node_str= "list node";
const gchar *ic_list_stop_str= "list stop";
const gchar *ic_true_str= "true";
const gchar *ic_false_str= "false";
const gchar *ic_auto_restart_str= "autorestart:";
const gchar *ic_num_parameters_str= "num parameters:";
const gchar *ic_parameter_str= "parameter:";
const gchar *ic_pid_str= "pid:";
const gchar *ic_receive_str= "receive ";
const gchar *ic_status_str= "status:";
const gchar *ic_master_index_size_str= "master index size:";
const gchar *ic_csd_program_str= "ic_csd";
const gchar *ic_clmgrd_program_str= "ic_clmgrd";
const gchar *ic_ndb_program_str= "ndbmtd";

const gchar *ic_def_grid_str= "iclaustron";
const gchar *ic_data_server_program_str= "ndbmtd";
const gchar *ic_file_server_program_str= "ic_fsd";
const gchar *ic_rep_server_program_str= "ic_repd";
const gchar *ic_sql_server_program_str= "mysqld";
const gchar *ic_cluster_manager_program_str= "ic_clmgrd";
const gchar *ic_cluster_server_program_str= "ic_csd";
const gchar *ic_restore_program_str= "ndb_restore";
const gchar *ic_process_controller_program_str= "ic_pcntrld";

const gchar *ic_no_angel_str= "--noangel";
const gchar *ic_daemon_str= "--daemon";
const gchar *ic_pid_file_str= "--pid-file";
const gchar *ic_log_file_str= "--log-file";
const gchar *ic_ndb_node_id_str= "--ndb-nodeid";
const gchar *ic_ndb_connectstring_str= "--ndb-connectstring";
const gchar *ic_cs_connectstring_str= "--cs-connectstring";
const gchar *ic_initial_flag_str= "--initial";
const gchar *ic_cluster_id_str= "--cluster-id";
const gchar *ic_node_id_str= "--node-id";
const gchar *ic_bootstrap_str= "--bootstrap";
const gchar *ic_server_name_str= "--server-name";
const gchar *ic_server_port_str="--server-port";
const gchar *ic_data_dir_str= "--data-dir";
const gchar *ic_num_threads_str= "--num-threads";
const gchar *ic_core_parameter_str= "--core";
#ifdef DEBUG_BUILD
const gchar *ic_debug_level_str= "--debug-level";
const gchar *ic_debug_timestamp_str= "--debug-timestamp";
#endif

const gchar *ic_data_server_str= "DATA SERVER";
const gchar *ic_client_node_str= "CLIENT NODE";
const gchar *ic_cluster_server_str= "CLUSTER SERVER";
const gchar *ic_sql_server_str= "SQL SERVER";
const gchar *ic_rep_server_str= "REPLICATION SERVER";
const gchar *ic_file_server_str= "FILE SERVER";
const gchar *ic_restore_node_str= "RESTORE NODE";
const gchar *ic_cluster_manager_str= "CLUSTER MANAGER";

/* Client to Cluster Manager protocol strings */
const gchar *ic_new_connect_clmgr_str= "CONNECT CLUSTER MANAGER";
const gchar *ic_reconnect_clmgr_str= "RECONNECT CLUSTER MANAGER";
const gchar *ic_connected_clmgr_str= "CONNECTED CLUSTER MANAGER";
const gchar *ic_connection_closed_str= "CONNECTION CLOSED";
