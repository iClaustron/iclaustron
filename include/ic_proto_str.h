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

#ifndef IC_PROTO_STR_H
#define IC_PROTO_STR_H

/* Strings used in MGM API protocols */
extern const gchar *ic_ok_str;
extern const gchar *ic_process_already_started_str;
extern const gchar *ic_version_str;
extern const gchar *ic_grid_str;
extern const gchar *ic_cluster_str;
extern const gchar *ic_node_str;
extern const gchar *ic_program_str;
extern const gchar *ic_start_time_str;
extern const gchar *ic_error_str;
extern const gchar *ic_start_str;
extern const gchar *ic_stop_str;
extern const gchar *ic_kill_str;
extern const gchar *ic_list_str;
extern const gchar *ic_list_full_str;
extern const gchar *ic_list_next_str;
extern const gchar *ic_list_node_str;
extern const gchar *ic_list_stop_str;
extern const gchar *ic_true_str;
extern const gchar *ic_false_str;
extern const gchar *ic_auto_restart_str;
extern const gchar *ic_num_parameters_str;
extern const gchar *ic_parameter_str;
extern const gchar *ic_pid_str;
extern const gchar *ic_receive_str;
extern const gchar *ic_status_str;
extern const gchar *ic_master_index_size_str;
extern const gchar *ic_csd_program_str;
extern const gchar *ic_node_parameter_str;

extern const gchar *ic_def_grid_str;
extern const gchar *ic_data_server_program_str;
extern const gchar *ic_file_server_program_str;
extern const gchar *ic_rep_server_program_str;
extern const gchar *ic_sql_server_program_str;
extern const gchar *ic_cluster_manager_program_str;
extern const gchar *ic_cluster_server_program_str;
extern const gchar *ic_restore_program_str;
extern const gchar *ic_process_controller_program_str;

extern const gchar *ic_no_angel_str;
extern const gchar *ic_daemon_str;
extern const gchar *ic_pid_file_str;
extern const gchar *ic_log_file_str;
extern const gchar *ic_ndb_node_id_str;
extern const gchar *ic_ndb_connectstring_str;
extern const gchar *ic_cs_connectstring_str;
extern const gchar *ic_initial_flag_str;
extern const gchar *ic_cluster_id_str;
extern const gchar *ic_node_id_str;
extern const gchar *ic_server_name_str;
extern const gchar *ic_server_port_str;
extern const gchar *ic_data_dir_str;
extern const gchar *ic_num_threads_str;
extern const gchar *ic_core_parameter_str;
#ifdef DEBUG_BUILD
extern const gchar *ic_debug_level_str;
extern const gchar *ic_debug_timestamp_str;
#endif

/* Messages for Copy Cluster Server files protocol */
extern const gchar *ic_copy_cluster_server_files_str;
extern const gchar *ic_copy_config_ini_str;
extern const gchar *ic_cluster_server_node_id_str;
extern const gchar *ic_number_of_clusters_str;
extern const gchar *ic_receive_config_ini_str;
extern const gchar *ic_number_of_lines_str;
extern const gchar *ic_receive_grid_common_ini_str;
extern const gchar *ic_receive_cluster_name_ini_str;
extern const gchar *ic_installed_cluster_server_files_str;
extern const gchar *ic_end_str;
extern const gchar *ic_receive_config_file_ok_str;

/* Messages for the Get CPU Info protocol */
extern const gchar *ic_get_cpu_info_str;
extern const gchar *ic_number_of_processors_str;
extern const gchar *ic_number_of_cpu_sockets_str;
extern const gchar *ic_number_of_cpu_cores_str;
extern const gchar *ic_number_of_numa_nodes_str;
extern const gchar *ic_processor_str;
extern const gchar *ic_core_str;
extern const gchar *ic_cpu_node_str;
extern const gchar *ic_socket_str;
extern const gchar *ic_no_cpu_info_available_str;

/* Messages for the Get Memory Info protocol */
extern const gchar *ic_get_mem_info_str;
extern const gchar *ic_number_of_mbyte_user_memory_str;
extern const gchar *ic_mem_node_str;
extern const gchar *ic_mb_user_memory_str;
extern const gchar *ic_no_mem_info_available_str;

/* Messages for the Get Disk Info protocol */
extern const gchar *ic_get_disk_info_str;
extern const gchar *ic_dir_str;
extern const gchar *ic_disk_space_str;
extern const gchar *ic_no_disk_info_available_str;

/* Node type names, part of e.g. the SHOW CLUSTER command */
extern const gchar *ic_data_server_str;
extern const gchar *ic_client_node_str;
extern const gchar *ic_cluster_server_str;
extern const gchar *ic_sql_server_str;
extern const gchar *ic_rep_server_str;
extern const gchar *ic_file_server_str;
extern const gchar *ic_restore_node_str;
extern const gchar *ic_cluster_manager_str;

/* Messages for cluster manager interaction */
extern const gchar *ic_new_connect_clmgr_str;
extern const gchar *ic_reconnect_clmgr_str;
extern const gchar *ic_connected_clmgr_str;
extern const gchar *ic_connection_closed_str;

extern gchar *ic_empty_string;
extern gchar *ic_err_str;
#endif
