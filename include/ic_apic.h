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

#ifndef IC_APIC_H
#define IC_APIC_H
#include <ic_common.h>

#define IC_VERSION_FILE_LEN 8
#define NO_WAIT 0
#define WAIT_LOCK_INFO 1
#define WAIT_CHANGE_INFO 2

enum ic_node_types
{
  IC_NOT_EXIST_NODE_TYPE = 9,
  IC_KERNEL_NODE = 0,
  IC_CLIENT_NODE = 1,
  IC_CLUSTER_SERVER_NODE = 2,
  IC_SQL_SERVER_NODE = 3,
  IC_REP_SERVER_NODE = 4,
  IC_FILE_SERVER_NODE = 6,
  IC_RESTORE_NODE = 7,
  IC_CLUSTER_MANAGER_NODE = 8
};
typedef enum ic_node_types IC_NODE_TYPES;

enum ic_communication_type
{
  IC_SOCKET_COMM = 0
};
typedef enum ic_communication_type IC_COMMUNICATION_TYPE;

enum ic_config_types
{
  IC_NO_CONFIG_TYPE = 9,
  IC_KERNEL_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2,
  IC_SQL_SERVER_TYPE = 3,
  IC_REP_SERVER_TYPE = 4,
  IC_CLUSTER_MGR_TYPE = 6,
  IC_COMM_TYPE = 5
};
typedef enum ic_config_types IC_CONFIG_TYPES;

enum ic_config_entry_change
{
  IC_ONLINE_CHANGE = 0,
  IC_NODE_RESTART_CHANGE = 1,
  IC_ROLLING_UPGRADE_CHANGE = 2,
  IC_ROLLING_UPGRADE_CHANGE_SPECIAL = 3,
  IC_INITIAL_NODE_RESTART = 4,
  IC_CLUSTER_RESTART_CHANGE = 5,
  IC_NOT_CHANGEABLE = 6
};
typedef enum ic_config_entry_change IC_CONFIG_ENTRY_CHANGE;

typedef enum ic_config_data_type
{
  IC_CHAR = 1,
  IC_BOOLEAN = 1,
  IC_UINT16 = 2,
  IC_UINT32 = 3,
  IC_UINT64 = 4,
  IC_CHARPTR = 5
} IC_CONFIG_DATA_TYPE;

struct ic_config_entry
{
  IC_STRING config_entry_name;
  gchar *config_entry_description;
  guint32 max_value;
  guint32 min_value;
  union
  {
    guint64 default_value;
    gchar *default_string;
  };
  IC_CONFIG_DATA_TYPE data_type;
  guint32 offset;
  /*
    When only one version exists then both these values are 0.
    When a new version of default values is created the
    max_version is set on the old defaults and the new entry
    gets the min_version set to the old max_version + 1 and
    its max_version_used is still 0.
  */
  guint32 min_ic_version_used;
  guint32 max_ic_version_used;
  guint32 min_ndb_version_used;
  guint32 max_ndb_version_used;
  enum ic_config_entry_change change_variant;
  guint32 config_types;
  gchar is_max_value_defined;
  gchar is_min_value_defined;
  gchar is_boolean;
  gchar is_deprecated;
  gchar is_string_type;
  gchar is_mandatory;
  gchar mandatory_bit;
  gchar is_not_configurable;
  gchar is_only_iclaustron;
  gchar is_array_value;
  gchar is_not_sent;
};
typedef struct ic_config_entry IC_CONFIG_ENTRY;

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
    also we keep track of number of kernel nodes, number of
    cluster servers, and number of client nodes and finally the
    number of communication links.
    For each node in the cluster and each communication link we also
    store the node id, the node type and the configuration parameters.
  */

  /*
    node_config is an array of pointers that point to structs of the
    types:
    ic_kernel_config             iClaustron kernel nodes
    ic_client_config             iClaustron client nodes
    ic_conf_server_config        iClaustron config server nodes
    ic_sql_server_config         iClaustron SQL server nodes
    ic_rep_server_config         iClaustron Replication server nodes

    The array node_types below contains the actual type of struct
    used for each entry.
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
    We keep track of the number of nodes of various types, maximum node id
    and the number of communication objects. We don't necessarily store all
    communication objects so the absence of a communication object simply
    means that the default values can be used.
  */
  guint32 max_node_id;
  guint32 num_nodes;
  guint32 num_data_servers;
  guint32 num_cluster_servers;
  guint32 num_clients;
  guint32 num_cluster_mgrs;
  guint32 num_sql_servers;
  guint32 num_rep_servers;
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
  IC_NODE_TYPES *node_types;
  IC_HASHTABLE *comm_hash;
};
typedef struct ic_cluster_config IC_CLUSTER_CONFIG;

struct ic_api_cluster_connection;
struct ic_api_config_server;
struct ic_run_cluster_server;

struct ic_api_cluster_operations
{
  int (*ic_get_config) (struct ic_api_config_server *apic,
                        IC_CLUSTER_CONNECT_INFO **clu_info,
                        guint32 *node_ids);

  int (*ic_get_cluster_ids) (struct ic_api_config_server *apic,
                            IC_CLUSTER_CONNECT_INFO **clu_infos);

  int (*ic_get_info_config_channels) (struct ic_api_config_server *apic);

  IC_CLUSTER_CONFIG* (*ic_get_cluster_config)
       (struct ic_api_config_server *apic, guint32 cluster_id);

  gchar* (*ic_get_node_object)
       (struct ic_api_config_server *apic, guint32 cluster_id, guint32 node_id);

  gchar* (*ic_get_typed_node_object)
       (struct ic_api_config_server *apic, guint32 cluster_id,
        guint32 node_id, IC_NODE_TYPES node_type);

  void (*ic_free_config) (struct ic_api_config_server *apic);
};
typedef struct ic_api_cluster_operations IC_API_CLUSTER_OPERATIONS;

struct ic_run_cluster_server_operations
{
  int (*ic_run_cluster_server) (struct ic_run_cluster_server *run_obj);
  void (*ic_free_run_cluster) (struct ic_run_cluster_server *run_obj);
};

#define REC_BUF_SIZE 256
struct ic_api_cluster_connection
{
  gchar **cluster_server_ips;
  gchar **cluster_server_ports;
  IC_CONNECTION *cluster_srv_conns;
  IC_CONNECTION *current_conn;
  guint32 num_cluster_servers;
  guint32 tail_index;
  guint32 head_index;
  gchar rec_buf[REC_BUF_SIZE];
};
typedef struct ic_api_cluster_connection IC_API_CLUSTER_CONNECTION;


/*
  The struct ic_api_config_server represents the configuration of
  all clusters that this node participates in and the node id it
  has in these clusters.
*/
struct ic_api_config_server
{
  IC_API_CLUSTER_OPERATIONS api_op;
  IC_CLUSTER_CONFIG **conf_objects;
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_API_CLUSTER_CONNECTION cluster_conn;
  GMutex *config_mutex;

  guint32 max_cluster_id;
  guint32 *node_ids;
  /*
    We have a number of variables to keep track of allocated memory
    and use of allocated memory for strings.
  */
  guint32 string_memory_size;
  gchar *config_memory_to_return;
  gchar *string_memory_to_return;
  gchar *end_string_memory;
  gchar *next_string_memory;
};
typedef struct ic_api_config_server IC_API_CONFIG_SERVER;

struct ic_run_cluster_state
{
  gboolean cs_master;
  gboolean cs_started;
  gboolean bootstrap;

  guint8 num_cluster_servers;
  guint8 num_cluster_servers_connected;
  gboolean cs_connect_state[IC_MAX_CLUSTER_SERVERS];
  guint32 config_version_number;
  IC_STRING *config_dir;
  GMutex *protect_state;
};
typedef struct ic_run_cluster_state IC_RUN_CLUSTER_STATE;
/*
  The struct ic_run_cluster_server represents the configuration of
  all clusters that the Cluster Server maintains.
*/
struct ic_run_cluster_server
{
  struct ic_run_cluster_server_operations run_op;
  struct ic_cluster_config **conf_objects;
  IC_RUN_CLUSTER_STATE state;
  IC_CONNECTION *run_conn;
  guint32 max_cluster_id;
  guint32 num_clusters;
  guint32 cs_nodeid;
};
typedef struct ic_run_cluster_server IC_RUN_CLUSTER_SERVER;

struct ic_kernel_config
{
  /* Common for all nodes */
  gchar *hostname;
  gchar *node_data_path;
  guint64 mandatory_bits;
  guint32 node_id;
  guint32 port_number;
  /* End common part */

  gchar *filesystem_path;
  gchar *kernel_checkpoint_path;

  guint64 size_of_ram_memory;
  guint64 size_of_hash_memory;
  guint64 page_cache_size;
  guint64 kernel_memory_pool;

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
  guint32 timer_heartbeat_kernel_nodes;
  guint32 timer_heartbeat_client_nodes;
  guint32 timer_local_checkpoint;
  guint32 timer_global_checkpoint;
  guint32 timer_resolve;
  guint32 timer_kernel_watchdog;
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
  guint32 redo_log_memory;
  guint32 long_message_memory;
  guint32 kernel_max_open_files;
  guint32 size_of_string_memory;
  guint32 kernel_open_files;
  guint32 kernel_file_synch_size;
  guint32 kernel_disk_write_speed;
  guint32 kernel_disk_write_speed_start;
  guint32 kernel_dummy;
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
  guint32 kernel_scheduler_no_send_time;
  guint32 kernel_scheduler_no_sleep_time;
  guint32 kernel_lock_main_thread;
  guint32 kernel_lock_other_threads;
  guint32 size_of_redo_log_files;
  guint32 kernel_initial_watchdog_timer;
  guint32 kernel_max_allocate_size;
  guint32 kernel_report_memory_frequency;
  guint32 kernel_backup_status_frequency;
  guint32 kernel_group_commit_delay;

  gchar use_unswappable_memory;
  gchar kernel_automatic_restart;
  gchar kernel_volatile_mode;
  gchar kernel_rt_scheduler_threads;
  gchar use_o_direct;
};
typedef struct ic_kernel_config IC_KERNEL_CONFIG;

struct ic_client_config
{
  /* Common part */
  gchar *hostname;
  gchar *node_data_path;

  guint64 mandatory_bits;
  guint32 node_id;
  guint32 port_number;
  /* End common part */
  guint32 client_resolve_rank;
  guint32 client_resolve_timer;

  guint32 client_max_batch_byte_size;
  guint32 client_batch_byte_size;
  guint32 client_batch_size;
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
  gchar *hostname;
  gchar *node_data_path;

  guint64 mandatory_bits;
  guint32 node_id;
  guint32 port_number;
  /* End common part */
  guint32 client_resolve_rank;
  guint32 client_resolve_timer;
  /* End of section in common with Client config */
  guint32 cluster_server_port_number;

  char *cluster_server_event_log;
};
typedef struct ic_cluster_server_config IC_CLUSTER_SERVER_CONFIG;

struct ic_cluster_mgr_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32          not_used;
};
typedef struct ic_cluster_mgr_config IC_CLUSTER_MGR_CONFIG;

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

struct ic_socket_link_config
{
  gchar *first_hostname;
  gchar *second_hostname;

  guint64 mandatory_bits;
  guint32 socket_write_buffer_size;
  guint32 socket_read_buffer_size;
  guint32 socket_kernel_read_buffer_size;
  guint32 socket_kernel_write_buffer_size;
  guint32 socket_maxseg_size;

  guint16 first_node_id;
  guint16 second_node_id;
  guint16 client_port_number;
  guint16 server_port_number;
  guint16 server_node_id;
  guint16 socket_group;

  gchar use_message_id;
  gchar use_checksum;
  gchar socket_bind_address;
  /* Ignore Connection Group for now */
};
typedef struct ic_socket_link_config IC_SOCKET_LINK_CONFIG;

struct ic_sci_comm_link_config
{
  gchar *first_hostname;
  gchar *second_hostname;

  guint64 mandatory_bits;
};

struct ic_shm_comm_link_config
{
  gchar *first_hostname;
  gchar *second_hostname;

  guint64 mandatory_bits;
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

IC_CLUSTER_CONNECT_INFO**
ic_load_cluster_config_from_file(gchar *config_file_path,
                                 IC_CONFIG_STRUCT *cluster_conf);

IC_CLUSTER_CONFIG*
ic_load_config_server_from_files(gchar *config_file_path,
                                 IC_CONFIG_STRUCT *conf_server);

struct ic_cs_conf_comment
{
  guint32 num_comments;
  gchar **ptr_comments;
  guint32 node_id_attached;
  guint32 config_id_attached;
};
typedef struct ic_cs_conf_comment IC_CS_CONF_COMMENT;

struct ic_cluster_config_temp
{
  IC_CLUSTER_CONNECT_INFO **clu_info;
  guint32 size_string_memory;
  gchar *string_memory;
  guint32 num_clusters;
  gboolean init_state;
  IC_STRING cluster_name;
  IC_STRING password;
  guint32 cluster_id;
};
typedef struct ic_cluster_config_temp IC_CLUSTER_CONFIG_TEMP;

struct ic_cluster_config_load
{
  IC_CLUSTER_CONFIG *conf;
  IC_MEMORY_CONTAINER *temp_mc_ptr;
  void *current_node_config;
  gchar *string_memory;
  gchar *string_memory_to_return;
  gchar *struct_memory;
  gchar *struct_memory_to_return;
  IC_CONFIG_TYPES current_node_config_type;
  IC_CS_CONF_COMMENT comments;
  
  guint32 size_string_memory;
  gboolean default_section;

  /*
    To avoid so many malloc calls we keep all default structures in this
    struct. These structures are initialised by setting the defaults for
    each parameter as defined by the iClaustron Cluster Server API.
  */
  IC_KERNEL_CONFIG default_kernel_config;
  IC_CLIENT_CONFIG default_client_config;
  IC_CLUSTER_MGR_CONFIG default_cluster_mgr_config;
  IC_CLUSTER_SERVER_CONFIG default_cluster_server_config;
  IC_REP_SERVER_CONFIG default_rep_config;
  IC_SQL_SERVER_CONFIG default_sql_config;
  IC_SOCKET_LINK_CONFIG default_socket_config;
};
typedef struct ic_cluster_config_load IC_CLUSTER_CONFIG_LOAD;

IC_API_CONFIG_SERVER*
ic_create_api_cluster(IC_API_CLUSTER_CONNECTION *cluster_conn);

IC_RUN_CLUSTER_SERVER*
ic_create_run_cluster(IC_CLUSTER_CONFIG **conf_objs,
                      IC_MEMORY_CONTAINER *mc_ptr,
                      gchar *server_name,
                      gchar* server_port);

void ic_print_config_parameters();

#define IC_SET_CONFIG_MAP(name, id) \
  if (name >= MAX_MAP_CONFIG_ID) \
    id_out_of_range(name); \
  if (id >= MAX_CONFIG_ID) \
    name_out_of_range(id); \
  map_config_id_to_inx[name]= id; \
  map_inx_to_config_id[id]= name; \
  conf_entry= &glob_conf_entry[id]; \
  if (conf_entry->config_entry_name.str) \
    id_already_used_aborting(id);   \
  if (id > glob_max_config_id) glob_max_config_id= id;

#define IC_SET_KERNEL_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_KERNEL_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_KERNEL_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CONFIG_MIN(conf_entry, min) \
  (conf_entry)->is_min_value_defined= TRUE; \
  (conf_entry)->min_value= (min);

#define IC_SET_CONFIG_MAX(conf_entry, max) \
  (conf_entry)->is_max_value_defined= TRUE; \
  (conf_entry)->max_value= (max);

#define IC_SET_CONFIG_MIN_MAX(conf_entry, min, max) \
  (conf_entry)->is_min_value_defined= TRUE; \
  (conf_entry)->is_max_value_defined= TRUE; \
  (conf_entry)->min_value= (min); \
  (conf_entry)->max_value= (max); \
  if ((min) == (max)) (conf_entry)->is_not_configurable= TRUE;

#define IC_SET_KERNEL_BOOLEAN(conf_entry, name, def, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_BOOLEAN; \
  (conf_entry)->default_value= (def); \
  (conf_entry)->is_boolean= TRUE; \
  (conf_entry)->offset= offsetof(IC_KERNEL_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_KERNEL_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_KERNEL_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->offset= offsetof(IC_KERNEL_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_KERNEL_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SOCKET_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_SOCKET_LINK_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SOCKET_BOOLEAN(conf_entry, name, def, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_BOOLEAN; \
  (conf_entry)->default_value= (def); \
  (conf_entry)->is_boolean= TRUE; \
  (conf_entry)->offset= offsetof(IC_SOCKET_LINK_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SOCKET_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->offset= offsetof(IC_SOCKET_LINK_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLUSTER_SERVER_STRING(conf_entry, name, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->default_string= (char*)(val); \
  (conf_entry)->offset= offsetof(IC_CLUSTER_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLUSTER_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLUSTER_SERVER_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_CLUSTER_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLUSTER_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLIENT_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_CLIENT_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLIENT_TYPE); \
  (conf_entry)->change_variant= (change);

#endif
