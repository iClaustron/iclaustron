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

#ifndef IC_APIC_H
#define IC_APIC_H

/* Strings used in MGM API protocols */
extern const gchar *ic_ok_str;
extern const gchar *ic_error_str;
extern const gchar *ic_version_str;
extern const gchar *ic_program_str;
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
extern const gchar *ic_pid_str;
extern const gchar *ic_grid_str;
extern const gchar *ic_cluster_str;
extern const gchar *ic_node_str;
extern const gchar *ic_program_str;
extern const gchar *ic_start_time_str;

extern const gchar *ic_def_grid_str;
extern const gchar *ic_data_server_program_str;
extern const gchar *ic_file_server_program_str;
extern const gchar *ic_sql_server_program_str;
extern const gchar *ic_rep_server_program_str;
extern const gchar *ic_cluster_manager_program_str;
extern const gchar *ic_cluster_server_program_str;

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

/* Initialisation of configuration parameters */
int ic_init_config_parameters();
void ic_print_config_parameters(guint32 mask);
void ic_destroy_conf_hash();

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
  IC_NO_CONFIG_TYPE = 3,
  IC_DATA_SERVER_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2,
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
  IC_NOT_EXIST_NODE_TYPE = 3,
  IC_DATA_SERVER_NODE = 0,
  IC_CLIENT_NODE = 1,
  IC_CLUSTER_SERVER_NODE = 2,
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
  */
  guint32 *node_ids;
  guint32 my_node_id;
  IC_CONNECTION *cs_conn;
  IC_NODE_TYPES *node_types;
  IC_HASHTABLE *comm_hash;
};
typedef struct ic_cluster_config IC_CLUSTER_CONFIG;

struct ic_data_server_config
{
  /* Common for all nodes */
  /* Mandatory bits is first in all node types and also in comm type */
  guint64 mandatory_bits;
  gchar *hostname;
  gchar *node_data_path;
  gchar *node_name;
  gchar *pcntrl_hostname;
  guint32 node_id;
  guint32 port_number;
  guint32 network_buffer_size;
  guint32 pcntrl_port;
  /* End common part */

  gchar *filesystem_path;
  gchar *data_server_checkpoint_path;
  gchar *data_server_zero_redo_log;

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
  guint32 data_server_local_db_threads;
  guint32 data_server_local_db_workers;
  guint32 data_server_file_thread_pool;
  /* Reserving send buffer memory for data server traffic ignored for now */
  guint32 reserved_send_buffer;
  guint32 data_server_lcp_poll_time;

  gchar use_unswappable_memory;
  gchar data_server_automatic_restart;
  gchar data_server_volatile_mode;
  gchar data_server_rt_scheduler_threads;
  gchar data_server_backup_compression;
  gchar data_server_local_checkpoint_compression;
  gchar use_o_direct;
};
typedef struct ic_data_server_config IC_DATA_SERVER_CONFIG;

struct ic_client_config
{
  /* Common part */
  guint64 mandatory_bits;
  gchar *hostname;
  gchar *node_data_path;
  gchar *node_name;
  gchar *pcntrl_hostname;
  guint32 node_id;
  guint32 port_number;
  guint32 network_buffer_size;
  guint32 pcntrl_port;
  /* End common part */
  guint32 client_resolve_rank;
  guint32 client_resolve_timer;

  guint32 client_max_batch_byte_size;
  guint32 client_batch_byte_size;
  guint32 client_batch_size;

  guint32 apid_num_threads;
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
  guint64 mandatory_bits;
  gchar *hostname;
  gchar *node_data_path;
  gchar *node_name;
  gchar *pcntrl_hostname;
  guint32 node_id;
  guint32 port_number;
  guint32 network_buffer_size;
  guint32 pcntrl_port;
  /* End common part */
  guint32 client_resolve_rank;
  guint32 client_resolve_timer;
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
  /* Ignore socket_overload for now */

  guint16 first_node_id;
  guint16 second_node_id;
  guint16 client_port_number;
  guint16 server_port_number;
  guint16 server_node_id;
  guint16 socket_group;
  /* Ignore Connection Group for now */

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

typedef struct ic_api_config_server IC_API_CONFIG_SERVER;
struct ic_api_cluster_operations
{
  /* Set error code and line number of error */
  void (*ic_set_error_line) (IC_API_CONFIG_SERVER *apic,
                             guint32 error_line);
  /* Fill an error buffer with information about error */
  gchar* (*ic_fill_error_buffer) (IC_API_CONFIG_SERVER *apic,
                                  int error_code,
                                  gchar *error_buffer);
  /*
     Check if we're running towards iClaustron Cluster Server or
     towards NDB management server (mostly for debugging)
  */
  gboolean (*ic_use_iclaustron_cluster_server) (IC_API_CONFIG_SERVER *apic);
  /* Get error string for special errors, not set for all errors */
  const gchar* (*ic_get_error_str) (IC_API_CONFIG_SERVER *apic);
  /*
    This function gets the configuration for a set of clusters from the
    Cluster Server(s). It allocates the node id in each of those
    clusters. Thus one process can be part of many clusters but it has
    to allocate the same node id in all of them.
  */
  int (*ic_get_config) (IC_API_CONFIG_SERVER *apic,
                        IC_CLUSTER_CONNECT_INFO **clu_info,
                        guint32 node_id,
                        guint32 timeout);
  /*
    The following methods are used to retrieve information from the
    configuration after a successful execution of the ic_get_config
    method above such that the process has the entire configuration
    at hand.

    There is one method to get the struct describing a full cluster
    configuration, there are also methods to retrieve the configuration
    object of one node, either of any node type or by specified node
    type.
  */
  IC_CLUSTER_CONFIG* (*ic_get_cluster_config)
       (IC_API_CONFIG_SERVER *apic, guint32 cluster_id);

  guint32 (*ic_get_max_cluster_id) (IC_API_CONFIG_SERVER *apic);

  guint32 (*ic_get_node_id_from_name) (IC_API_CONFIG_SERVER *apic,
                                       guint32 cluster_id,
                                       const IC_STRING *node_name);

  guint32 (*ic_get_cluster_id_from_name) (IC_API_CONFIG_SERVER *apic,
                                          const IC_STRING *cluster_name);

  gchar* (*ic_get_node_object)
       (IC_API_CONFIG_SERVER *apic, guint32 cluster_id, guint32 node_id);

  gchar* (*ic_get_typed_node_object)
       (IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
        guint32 node_id, IC_NODE_TYPES node_type);

  IC_SOCKET_LINK_CONFIG* (*ic_get_communication_object)
      (IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
       guint32 first_node_id, guint32 second_node_id);

  /* Method used to release all memory allocated for the configuration */
  void (*ic_free_config) (IC_API_CONFIG_SERVER *apic);
};
typedef struct ic_api_cluster_operations IC_API_CLUSTER_OPERATIONS;

struct ic_api_config_server
{
  IC_API_CLUSTER_OPERATIONS api_op;
  guint32 max_cluster_id;
};

#define REC_BUF_SIZE 256
struct ic_api_cluster_connection
{
  gchar **cluster_server_ips;
  gchar **cluster_server_ports;
  IC_CONNECTION **cluster_srv_conns;
  IC_CONNECTION *current_conn;
  guint32 num_cluster_servers;
  guint32 tail_index;
  guint32 head_index;
  gchar rec_buf[REC_BUF_SIZE];
};
typedef struct ic_api_cluster_connection IC_API_CLUSTER_CONNECTION;

IC_API_CONFIG_SERVER*
ic_create_api_cluster(IC_API_CLUSTER_CONNECTION *cluster_conn,
                      gboolean use_iclaustron_cluster_server);

typedef struct ic_run_cluster_server IC_RUN_CLUSTER_SERVER;
struct ic_run_cluster_server_operations
{
  IC_API_CONFIG_SERVER* (*ic_get_api_config) (IC_RUN_CLUSTER_SERVER *run_obj);
  int (*ic_start_cluster_server) (IC_RUN_CLUSTER_SERVER *run_obj);
  gchar* (*ic_fill_error_buffer) (IC_RUN_CLUSTER_SERVER *run_obj,
                                  int error_code,
                                  gchar *error_buffer);
  int (*ic_run_cluster_server) (IC_RUN_CLUSTER_SERVER *run_obj);
  int (*ic_stop_cluster_server) (IC_RUN_CLUSTER_SERVER *run_obj);
  void (*ic_free_run_cluster) (IC_RUN_CLUSTER_SERVER *run_obj);
};
typedef struct ic_run_cluster_server_operations
  IC_RUN_CLUSTER_SERVER_OPERATIONS;

struct ic_run_cluster_server
{
  IC_RUN_CLUSTER_SERVER_OPERATIONS run_op;
};

/*
  The struct ic_run_cluster_server represents the configuration of
  all clusters that the Cluster Server maintains.
*/
IC_RUN_CLUSTER_SERVER*
ic_create_run_cluster(IC_STRING *config_dir,
                      const gchar *process_name,
                      gchar *server_name,
                      gchar *server_port,
                      guint32 my_node_id);

IC_API_CONFIG_SERVER*
ic_get_configuration(IC_API_CLUSTER_CONNECTION *apic,
                     IC_STRING *config_dir,
                     guint32 node_id,
                     guint32 num_cs_servers,
                     gchar **cluster_server_ips,
                     gchar **cluster_server_ports,
                     guint32 cs_timeout,
                     gboolean use_iclaustron_cluster_server,
                     int *error,
                     gchar **err_str);

/* iClaustron Protocol Support */
int ic_send_start_info(IC_CONNECTION *conn,
                       const gchar *program_name,
                       const gchar *version_name,
                       const gchar *grid_name,
                       const gchar *cluster_name,
                       const gchar *node_name);
#endif
