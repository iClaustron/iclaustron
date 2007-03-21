#ifndef IC_APIC_H
#define IC_APIC_H
#include <ic_common.h>


enum ic_node_types
{
  IC_NOT_EXIST_NODE_TYPE = 0,
  IC_KERNEL_NODE = 1,
  IC_CLIENT_NODE = 2,
  IC_CLUSTER_SERVER_NODE = 3,
  IC_SQL_SERVER_NODE = 4,
  IC_REP_SERVER_NODE = 5
};
typedef enum ic_node_types IC_NODE_TYPES;

enum ic_communication_type
{
  IC_TCP_COMM = 0
};
typedef enum ic_communication_type IC_COMMUNICATION_TYPE;

enum ic_config_type
{
  IC_KERNEL_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2,
  IC_COMM_TYPE = 3,
  IC_CONFIG_SERVER_TYPE = 4,
  IC_SQL_SERVER_TYPE = 5,
  IC_REP_SERVER_TYPE = 6
};
typedef enum ic_config_type IC_CONFIG_TYPE;

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
  char *config_entry_name;
  char *config_entry_description;
  guint64 max_value;
  guint64 min_value;
  union
  {
    guint64 default_value;
    char *default_string;
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
  guint32 min_version_used;
  guint32 max_version_used;
  enum ic_config_entry_change change_variant;
  enum ic_config_type node_type;
  gchar is_max_value_defined;
  gchar is_min_value_defined;
  gchar is_boolean;
  gchar is_deprecated;
  gchar is_string_type;
  gchar is_mandatory_to_specify;
  gchar is_not_configurable;
  gchar is_only_iclaustron;
  gchar is_array_value;
};
typedef struct ic_config_entry IC_CONFIG_ENTRY;

struct ic_cluster_config;
struct ic_api_cluster_connection;
struct ic_api_config_server;
struct ic_run_cluster_server;
struct ic_run_cluster_conn
{
  guint32 ip_addr;
  guint16 ip_port;
};
typedef struct ic_run_cluster_conn IC_RUN_CLUSTER_CONN;

struct ic_api_cluster_operations
{
  int (*get_ic_config) (struct ic_api_config_server *apic);
  void (*free_ic_config) (struct ic_api_config_server *apic);
};

struct ic_run_cluster_server_operations
{
  int (*run_ic_cluster_server) (struct ic_cluster_config *cs_conf,
                                struct ic_run_cluster_conn *run_conn,
                                guint32 num_connects);
  void (*free_ic_run_cluster) (struct ic_run_cluster_server *run_obj);
};

#define REC_BUF_SIZE 256
struct ic_api_cluster_connection
{
  guint32 *cluster_server_ips;
  guint16 *cluster_server_ports;
  IC_CONNECTION *cluster_srv_conns;
  guint32 num_cluster_servers;
  guint32 tail_index;
  guint32 head_index;
  char rec_buf[REC_BUF_SIZE];
};
typedef struct ic_api_cluster_connection IC_API_CLUSTER_CONNECTION;


/*
  The struct ic_api_config_server represents the configuration of
  all clusters that this node participates in and the node id it
  has in these clusters.
*/
struct ic_api_config_server
{
  struct ic_api_cluster_operations api_op;
  struct ic_cluster_config *conf_objects;
  struct ic_api_cluster_connection cluster_conn;
  guint32 *cluster_ids;
  guint32 *node_ids;
  guint32 num_clusters_to_connect;
  /*
    We have a number of variables to keep track of allocated memory
    and use of allocated memory for strings.
  */
  guint32 string_memory_size;
  char *config_memory_to_return;
  char *string_memory_to_return;
  char *end_string_memory;
  char *next_string_memory;
};
typedef struct ic_api_config_server IC_API_CONFIG_SERVER;

/*
  The struct ic_run_cluster_server represents the configuration of
  all clusters that the Cluster Server maintains.
*/
struct ic_run_config_server
{
  /*struct ic_run_config_server_operations run_op; */
  struct ic_cluster_config *conf_objects;
  struct ic_run_cluster_conn run_conn;
  guint32 *cluster_ids;
  guint32 num_clusters;
};
typedef struct ic_run_config_server IC_RUN_CONFIG_SERVER;

struct ic_kernel_config
{
  char *filesystem_path;
  char *kernel_checkpoint_path;
  char *node_data_path;
  char *hostname;

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

  gchar use_unswappable_memory;
  gchar kernel_automatic_restart;
  gchar kernel_volatile_mode;
  gchar kernel_rt_scheduler_threads;
};
typedef struct ic_kernel_config IC_KERNEL_CONFIG;

struct ic_client_config
{
  char *hostname;
  char *node_data_path;

  guint32 client_max_batch_byte_size;
  guint32 client_batch_byte_size;
  guint32 client_batch_size;
  guint32 client_resolve_rank;
  guint32 client_resolve_timer;
};
typedef struct ic_client_config IC_CLIENT_CONFIG;

struct ic_cluster_server_config
{
  char *hostname;
  char *node_data_path;
  char *cluster_server_event_log;

  guint32 client_resolve_rank;
  guint32 client_resolve_timer;
  guint32 cluster_server_port_number;
};
typedef struct ic_cluster_server_config IC_CLUSTER_SERVER_CONFIG;

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

struct ic_config_server_config
{
  IC_CLIENT_CONFIG client_conf;
  guint32          not_used;
};
typedef struct ic_config_server_config IC_CONFIG_SERVER_CONFIG;

struct ic_tcp_comm_link_config
{
  char *first_host_name;
  char *second_host_name;

  guint32 tcp_write_buffer_size;
  guint32 tcp_read_buffer_size;

  guint16 first_node_id;
  guint16 second_node_id;
  guint16 client_port_number;
  guint16 server_port_number;
  guint16 server_node_id;
  guint16 tcp_group;

  gchar use_message_id;
  gchar use_checksum;
  /* Ignore Connection Group for now */
};
typedef struct ic_tcp_comm_link_config IC_TCP_COMM_LINK_CONFIG;

struct ic_sci_comm_link_config
{
};

struct ic_shm_comm_link_config
{
};

struct ic_comm_link_config
{
  union
  {
    struct ic_tcp_comm_link_config tcp_conf;
    struct ic_sci_comm_link_config sci_conf;
    struct ic_shm_comm_link_config shm_conf;
  };
};
typedef struct ic_comm_link_config IC_COMM_LINK_CONFIG;

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
    ic_cluster_server_config     iClaustron cluster server nodes
    ic_sql_server_config         iClaustron SQL server nodes
    ic_rep_server_config         iClaustron Replication server nodes

    The array node_types below contains the actual type of struct
    used for each entry.
  */
  char **node_config;
  /*
    comm_config is an array of pointers that point to structs of the
    types:
    ic_tcp_comm_link_config       iClaustron TCP/IP link
    ic_shm_comm_link_config       iClaustron Shared Memory link
    ic_sci_comm_link_config       iClaustron SCI link

    The array comm_types below contains the actual type of the struct
    for each entry. Currently only TCP/IP links are possible.
  */
  char **comm_config;

  guint32 no_of_nodes;
  guint32 no_of_kernel_nodes;
  guint32 no_of_config_servers;
  guint32 no_of_cluster_servers;
  guint32 no_sql_servers;
  guint32 no_rep_servers;
  guint32 no_of_client_nodes;
  guint32 no_of_comms;
  guint32 *node_ids;
  IC_NODE_TYPES *node_types;
  enum ic_communication_type *comm_types;
};
typedef struct ic_cluster_config IC_CLUSTER_CONFIG;

int ic_load_config_server_from_files(gchar *config_file_path);

struct ic_cs_conf_comment
{
  guint32 num_comments;
  char **ptr_comments;
  guint32 node_id_attached;
  guint32 config_id_attached;
};
typedef struct ic_cs_conf_comment IC_CS_CONF_COMMENT;

struct ic_cluster_config_load
{
  IC_CLUSTER_CONFIG conf;
  void *current_node_config;
  IC_CONFIG_TYPE current_node_config_type;
  IC_CS_CONF_COMMENT comments;
  guint32 max_node_id;

  /*
    To avoid so many malloc calls we keep all default structures in this
    struct. These structures are initialised by setting the defaults for
    each parameter as defined by the iClaustron Cluster Server API.
  */
  IC_KERNEL_CONFIG default_kernel_config;
  IC_REP_SERVER_CONFIG default_rep_config;
  IC_SQL_SERVER_CONFIG default_sql_config;
  IC_CONFIG_SERVER_CONFIG default_conf_server_config;
  IC_CLIENT_CONFIG default_client_config;
};
typedef struct ic_cluster_config_load IC_CLUSTER_CONFIG_LOAD;

IC_RUN_CONFIG_SERVER*
ic_init_run_cluster(IC_CLUSTER_CONFIG *conf_objs,
                    guint32 *cluster_ids,
                    guint32 num_clusters);

IC_API_CONFIG_SERVER*
ic_init_api_cluster(IC_API_CLUSTER_CONNECTION *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 *node_ids,
                    guint32 num_clusters);

void ic_print_config_parameters();

#define IC_SET_CONFIG_MAP(name, id) \
  map_config_id[name]= id; \
  conf_entry= &glob_conf_entry[id];

#define IC_SET_KERNEL_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_KERNEL_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_KERNEL_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CONFIG_MIN(conf_entry, min) \
  (conf_entry)->is_min_value_defined= TRUE; \
  (conf_entry)->min_value= (min);

#define IC_SET_CONFIG_MAX(conf_entry, min) \
  (conf_entry)->is_max_value_defined= TRUE; \
  (conf_entry)->max_value= (max);

#define IC_SET_CONFIG_MIN_MAX(conf_entry, min, max) \
  (conf_entry)->is_min_value_defined= TRUE; \
  (conf_entry)->is_max_value_defined= TRUE; \
  (conf_entry)->min_value= (min); \
  (conf_entry)->max_value= (max); \
  if ((min) == (max)) (conf_entry)->is_not_configurable= TRUE;

#define IC_SET_KERNEL_BOOLEAN(conf_entry, name, def, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->data_type= IC_BOOLEAN; \
  (conf_entry)->default_value= (def); \
  (conf_entry)->is_boolean= TRUE; \
  (conf_entry)->offset= offsetof(IC_KERNEL_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_KERNEL_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_KERNEL_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->is_mandatory_to_specify= TRUE; \
  (conf_entry)->offset= offsetof(IC_KERNEL_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_KERNEL_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_TCP_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_TCP_COMM_LINK_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_TCP_BOOLEAN(conf_entry, name, def, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->data_type= IC_BOOLEAN; \
  (conf_entry)->default_value= (def); \
  (conf_entry)->is_boolean= TRUE; \
  (conf_entry)->offset= offsetof(IC_TCP_COMM_LINK_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_TCP_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->is_mandatory_to_specify= TRUE; \
  (conf_entry)->offset= offsetof(IC_TCP_COMM_LINK_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLUSTER_SERVER_STRING(conf_entry, name, val, change) \
  (conf_entry)->config_entry_name= "name"; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->default_string= (char*)(val); \
  (conf_entry)->offset= offsetof(IC_CLUSTER_SERVER_CONFIG, name); \
  (conf_entry)->node_type= (1 << IC_CLUSTER_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#endif
