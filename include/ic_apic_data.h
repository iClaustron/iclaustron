/* Copyright (C) 2007-2012 iClaustron AB

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

#ifndef IC_APIC_DATA_H
#define IC_APIC_DATA_H
/*
  A node type can always be mapped directly to a config type.
  The opposite isn't true since one config type is the
  IC_COMM_TYPE which represents communication configurations.
  We need to be careful when communicating with NDB nodes
  to map SQL Server, Replication Server, File Server, Cluster
  Manager and Restore nodes to Client nodes since that's the
  only node type except for data nodes and cluster servers
  that the NDB nodes knows about.
*/
enum ic_config_types
{
  IC_NO_CONFIG_TYPE = 0,
  IC_DATA_SERVER_TYPE = 1,
  IC_CLIENT_TYPE = 2,
  IC_CLUSTER_SERVER_TYPE = 3,
  IC_SQL_SERVER_TYPE = 4,
  IC_REP_SERVER_TYPE = 6,
  IC_FILE_SERVER_TYPE = 7,
  IC_RESTORE_TYPE = 8,
  IC_CLUSTER_MANAGER_TYPE = 9,
  IC_COMM_TYPE = 5,
  IC_SYSTEM_TYPE = 10,
  IC_NUMBER_OF_CONFIG_TYPES = 11
};
typedef enum ic_config_types IC_CONFIG_TYPES;

enum ic_node_types
{
  IC_NOT_EXIST_NODE_TYPE = 0,
  IC_DATA_SERVER_NODE = 1,
  IC_CLIENT_NODE = 2,
  IC_CLUSTER_SERVER_NODE = 3,
  IC_SQL_SERVER_NODE = 4,
  IC_REP_SERVER_NODE = 6,
  IC_FILE_SERVER_NODE = 7,
  IC_RESTORE_NODE = 8,
  IC_CLUSTER_MANAGER_NODE = 9
};
typedef enum ic_node_types IC_NODE_TYPES;

struct ic_system_config
{
  guint32 system_primary_cs_node;
  guint32 system_configuration_number;
  gchar *system_name;
};
typedef struct ic_system_config IC_SYSTEM_CONFIG;

struct ic_cluster_connect_info
{
  IC_STRING cluster_name;
  IC_STRING password;
  guint32 cluster_id;
};
typedef struct ic_cluster_connect_info IC_CLUSTER_CONNECT_INFO;

/*
  The struct ic_cluster_config contains the configuration of one
  cluster. This struct is used both by users of the Cluster
  Server and by the Cluster Server itself.

  The Cluster Server itself fills this structure from a configuration
  file whereas the API users fills it up by receiving the cluster
  configuration using the protocol towards the Cluster Server.

  The Cluster Server uses this struct when responding to a configuration
  request from another node in the cluster.
*/

struct ic_cluster_config
{
  /*
    DESCRIPTION:
    ------------
    We keep track of all nodes in each cluster we participate in,
    also we keep track of number of data server nodes, number of
    cluster servers, and number of client nodes and finally the
    number of communication links.
    For each node in the cluster and each communication link we also
    store the node id, the node type and the configuration parameters.
  */

  /*
    node_config is an array of pointers that point to structs of the
    types:
    ic_data_server_config        iClaustron data server nodes
    ic_client_config             iClaustron client nodes
    ic_conf_server_config        iClaustron config server nodes
    ic_sql_server_config         iClaustron SQL server nodes
    ic_rep_server_config         iClaustron Replication server nodes

    The array node_types below contains the actual type of struct
    used for each entry.

    In the build phase the nodes are organised in order found, the node_ids
    array gives the nodeid for each entry, in the final storage after
    spanning all configuration entries it is organised by using nodeid as
    index into the node_config array. Thus there can be NULL pointers when
    no node of a certain node id exists.

    Also the node_types array is addressed by nodeid in the final version of
    this data structure.
  */
  gchar **node_config;
  /*
    comm_config is an array of pointers that point to structs of the
    types:
    ic_socket_link_config         iClaustron Socket link
    ic_shm_comm_link_config       iClaustron Shared Memory link
    ic_sci_comm_link_config       iClaustron SCI link

    The array comm_types below contains the actual type of the struct
    for each entry. Currently only socket links are possible. Thus this
    array is currently not implemented.
  */
  gchar **comm_config;

  /*
    We have a name and an id and a password on all clusters. This is
    specified in the configuration file where we specify the clusters,
    the rest of the information is stored in each specific cluster
    configuration file. There is also a cluster id for each cluster.
  */
  IC_CLUSTER_CONNECT_INFO clu_info;
  /*
    As part of receiving the configuration we also receive information
    about the cluster (system).
  */
  IC_SYSTEM_CONFIG sys_conf;

  /*
    We keep track of the number of nodes of various types, maximum node id
    and the number of communication objects.
  */
  guint32 max_node_id;
  guint32 num_nodes;
  guint32 num_data_servers;
  guint32 num_cluster_servers;
  guint32 num_clients;
  guint32 num_sql_servers;
  guint32 num_rep_servers;
  guint32 num_file_servers;
  guint32 num_restore_nodes;
  guint32 num_cluster_mgrs;
  guint32 num_comms;
  /*
    node_ids is an array of the node ids used temporarily when building the
    data structures, this is not to be used after that, should be a NULL
    pointer. The reason is that in the beginning this structure is in order
    they arrive in the protocol rather than by node id. After that we arrange
    the arrays to be by node id and thus there can be holes in the array if
    there are node ids that are not represented in the cluster.

    Currently we support only TCP/IP communication and probably won't extend
    this given that most of the interesting alternatives can use sockets to
    communicate.

    We store an array of node types to be able to map the node config array
    pointers into the proper structure and we also store a hash table on
    communication objects where we can quickly find the communication object
    given the node ids of the communication link.

    cs_conn and cs_nodeid is used to keep information about the Cluster
    Server connection used to retrieve configuration information and its
    nodeid, we will complete the NDB Management Protocol as part of setting
    up the Data API.
  */
  guint32 *node_ids;
  guint32 my_node_id;
  guint32 cs_nodeid;
  IC_CONNECTION *cs_conn;
  IC_NODE_TYPES *node_types;
  IC_HASHTABLE *comm_hash;
};
typedef struct ic_cluster_config IC_CLUSTER_CONFIG;

/* Mandatory bits is first in all node types and also in comm type */
#define IC_NODE_COMMON_DECLARES \
  guint64 mandatory_bits; \
  gchar *hostname;        \
  gchar *node_data_path;  \
  gchar *node_name;       \
  gchar *pcntrl_hostname; \
  guint32 node_id;        \
  guint32 port_number;    \
  guint32 pcntrl_port;    \
  guint64 network_buffer_size; \
  guint64 more_network_buffer_size;
  
struct ic_data_server_config
{
  /* Common for all nodes */
  IC_NODE_COMMON_DECLARES;
  /* End common part */

  gchar *filesystem_path;
  gchar *data_server_checkpoint_path;
  gchar *data_server_zero_redo_log;
  gchar *data_server_disk_file_path;
  gchar *data_server_disk_data_file_path;
  gchar *data_server_disk_undo_file_path;
  gchar *data_server_initial_log_file_group;
  gchar *data_server_initial_tablespace;
  gchar *data_server_cpu_thread_configuration;

  guint64 size_of_ram_memory;
  guint64 size_of_hash_memory;
  guint64 page_cache_size;
  guint64 data_server_memory_pool;

  guint32 max_number_of_trace_files;
  guint32 number_of_replicas;
  guint32 number_of_table_objects;
  guint32 number_of_column_objects;
  guint32 number_of_key_objects;
  guint32 number_of_internal_trigger_objects;
  guint32 number_of_connection_objects;
  guint32 number_of_operation_objects;
  guint32 number_of_scan_objects;
  guint32 number_of_internal_trigger_operation_objects;
  guint32 number_of_key_operation_objects;
  guint32 size_of_connection_buffer;
  guint32 timer_wait_partial_start;
  guint32 timer_wait_partitioned_start;
  guint32 timer_wait_error_start;
  guint32 timer_heartbeat_data_server_nodes;
  guint32 timer_heartbeat_client_nodes;
  guint32 timer_local_checkpoint;
  guint32 timer_global_checkpoint;
  guint32 timer_resolve;
  guint32 timer_data_server_watchdog;
  guint32 number_of_redo_log_files;
  guint32 number_of_redo_log_parts;
  guint32 timer_check_interval;
  guint32 timer_client_activity;
  guint32 timer_deadlock;
  guint32 number_of_checkpoint_objects;
  guint32 checkpoint_memory;
  guint32 checkpoint_data_memory;
  guint32 checkpoint_log_memory;
  guint32 checkpoint_write_size;
  guint32 checkpoint_max_write_size;
  guint32 number_of_ordered_key_objects;
  guint32 number_of_unique_hash_key_objects;
  guint32 number_of_local_operation_objects;
  guint32 number_of_local_scan_objects;
  guint32 size_of_scan_batch;
  guint32 redo_log_buffer_memory;
  guint32 long_message_memory;
  guint32 data_server_max_open_files;
  guint32 size_of_string_memory;
  guint32 data_server_open_files;
  guint32 data_server_file_synch_size;
  guint32 data_server_disk_write_speed;
  guint32 data_server_disk_write_speed_start;
  guint32 data_server_dummy;
  guint32 log_level_start;
  guint32 log_level_stop;
  guint32 log_level_statistics;
  guint32 log_level_congestion;
  guint32 log_level_checkpoint;
  guint32 log_level_restart;
  guint32 log_level_connection;
  guint32 log_level_reports;
  guint32 log_level_debug;
  guint32 log_level_warning;
  guint32 log_level_error;
  guint32 log_level_backup;
  guint32 log_level_schema;
  guint32 inject_fault;
  guint32 data_server_scheduler_no_send_time;
  guint32 data_server_scheduler_no_sleep_time;
  guint32 data_server_lock_main_thread;
  guint32 data_server_lock_other_threads;
  guint32 size_of_redo_log_files;
  guint32 data_server_initial_watchdog_timer;
  guint32 data_server_max_allocate_size;
  guint32 data_server_report_memory_frequency;
  guint32 data_server_backup_status_frequency;
  guint32 data_server_group_commit_delay;
  guint32 data_server_group_commit_timeout;
  guint32 data_server_max_local_triggers;
  guint32 data_server_max_local_trigger_users;
  guint32 data_server_max_local_trigger_operations;
  guint32 data_server_max_stored_group_commits;
  guint32 data_server_local_trigger_handover_timeout;
  guint32 data_server_report_startup_frequency;
  guint32 data_server_node_group;
  guint32 data_server_threads;
  guint32 data_server_file_thread_pool;
  /* Reserving send buffer memory for data server traffic ignored for now */
  guint32 reserved_send_buffer;
  guint32 data_server_lcp_poll_time;
  guint32 data_server_parallel_build_index;
  guint32 data_server_heartbeat_order;
  guint32 data_server_trace_schema_ops;
  guint32 data_server_max_automatic_start_retries;
  guint32 data_server_redo_overload_limit;
  guint32 data_server_redo_overload_report_limit;
  guint32 data_server_log_event_buffer_size;
  guint32 data_server_numa_interleave_memory;
  guint32 data_server_allocate_memory_time;
  guint32 data_server_max_concurrent_scans_per_partition;
  guint32 data_server_node_connect_check_timer;
  guint32 data_server_start_timer_wait_nodes_without_nodegroup;
  guint32 data_server_index_statistics_create_flag;
  guint32 data_server_index_statistics_monitor_update_flag;
  guint32 data_server_index_statistics_size_per_index;
  guint32 data_server_index_statistics_scale_factor_large_indexes;
  guint32 data_server_index_statistics_change_factor_to_update;
  guint32 data_server_index_statistics_change_factor_large_index;
  guint32 data_server_index_statistics_minimum_update_delay;
  guint32 data_server_max_queries_per_transaction;
  guint32 data_server_always_free_pct;
  guint32 data_server_resolve_method;

  gchar use_unswappable_memory;
  gchar data_server_automatic_restart;
  gchar data_server_volatile_mode;
  gchar data_server_rt_scheduler_threads;
  gchar data_server_backup_compression;
  gchar data_server_local_checkpoint_compression;
  gchar use_o_direct;
  gchar data_server_copy_data_algorithm;
  gchar data_server_crash_flag_on_checksum_error;
};
typedef struct ic_data_server_config IC_DATA_SERVER_CONFIG;

#define IC_CLIENT_COMMON_DECLARES \
  guint32 client_resolve_rank; \
  guint32 client_resolve_timer

struct ic_client_config
{
  /* Common part */
  IC_NODE_COMMON_DECLARES;
  /* End common part */
  IC_CLIENT_COMMON_DECLARES;

  guint32 client_max_batch_byte_size;
  guint32 client_batch_byte_size;
  guint32 client_batch_size;

  guint32 apid_num_threads;
  guint32 client_default_redo_operation_type;

  gchar client_automatic_reconnect;
};
typedef struct ic_client_config IC_CLIENT_CONFIG;

struct ic_cluster_server_config
{
  /*
     Start of section in common with Client config,
     this section must be ordered in the same way as in IC_CLIENT_CONFIG
     above to ensure offset of variables with same name are the same.
  */
  /* Common part */
  IC_NODE_COMMON_DECLARES;
  /* End common part */
  IC_CLIENT_COMMON_DECLARES;
  /* End of section in common with Client config */
  guint32 cluster_server_port_number;

  gchar *cluster_server_event_log;
};
typedef struct ic_cluster_server_config IC_CLUSTER_SERVER_CONFIG;

struct ic_cluster_manager_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32 cluster_manager_port_number;
};
typedef struct ic_cluster_manager_config IC_CLUSTER_MANAGER_CONFIG;

struct ic_sql_server_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32          not_used;
};
typedef struct ic_sql_server_config IC_SQL_SERVER_CONFIG;

struct ic_rep_server_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32          not_used;
};
typedef struct ic_rep_server_config IC_REP_SERVER_CONFIG;

struct ic_file_server_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32          not_used;
};
typedef struct ic_file_server_config IC_FILE_SERVER_CONFIG;

struct ic_restore_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32          not_used;
};
typedef struct ic_restore_config IC_RESTORE_CONFIG;

struct ic_socket_link_config
{
  guint64 mandatory_bits;

  gchar *first_hostname;
  gchar *second_hostname;

  guint32 socket_write_buffer_size;
  guint32 socket_read_buffer_size;
  guint32 socket_kernel_read_buffer_size;
  guint32 socket_kernel_write_buffer_size;
  guint32 socket_maxseg_size;
  guint32 socket_max_wait_in_nanos;
  guint32 socket_overload;
  guint32 server_port_number;
  /* Ignore socket_overload for now */

  guint16 first_node_id;
  guint16 second_node_id;
  guint16 client_port_number;
  guint16 server_node_id;
  guint16 socket_group;
  /* Ignore Connection Group for now */
  guint16 dynamic_server_port_number;

  gchar use_message_id;
  gchar use_checksum;
  gchar socket_bind_address;
  gchar is_wan_connection;
};
typedef struct ic_socket_link_config IC_SOCKET_LINK_CONFIG;

struct ic_sci_comm_link_config
{
  guint64 mandatory_bits;
  gchar *first_hostname;
  gchar *second_hostname;
};

struct ic_shm_comm_link_config
{
  guint64 mandatory_bits;
  gchar *first_hostname;
  gchar *second_hostname;
};

struct ic_comm_link_config
{
  union
  {
    struct ic_socket_link_config socket_conf;
    struct ic_sci_comm_link_config sci_conf;
    struct ic_shm_comm_link_config shm_conf;
  };
};
typedef struct ic_comm_link_config IC_COMM_LINK_CONFIG;
#endif
