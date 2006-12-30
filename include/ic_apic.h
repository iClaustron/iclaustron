#include <ic_common.h>


enum ic_node_type
{
  IC_KERNEL_NODE = 0,
  IC_CLIENT_NODE = 1,
  IC_CLUSTER_SERVER_NODE = 2
};

enum ic_communication_type
{
  IC_TCP_COMM = 0
};

enum ic_config_type
{
  IC_KERNEL_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2,
  IC_COMM_TYPE = 3
};

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

struct config_entry
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
};

struct ic_cluster_config;
struct ic_api_cluster_connection;
struct ic_api_cluster_server;
struct ic_run_cluster_server;
struct ic_run_cluster_conn
{
  guint32 ip_addr;
  guint16 ip_port;
};

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 *node_ids,
                    guint32 num_clusters);

struct ic_run_cluster_server*
ic_init_run_cluster(struct ic_cluster_config *conf_objs,
                    guint32 *cluster_ids,
                    guint32 num_clusters);

void ic_print_config_parameters();

struct ic_api_cluster_operations
{
  int (*get_ic_config) (struct ic_api_cluster_server *apic);
  void (*free_ic_config) (struct ic_api_cluster_server *apic);
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
  struct ic_connection *cluster_srv_conns;
  guint32 num_cluster_servers;
  guint32 tail_index;
  guint32 head_index;
  char rec_buf[REC_BUF_SIZE];
};


/*
  The struct ic_api_cluster_server represents the configuration of
  all clusters that this node participates in and the node id it
  has in these clusters.
*/
struct ic_api_cluster_server
{
  struct ic_api_cluster_operations api_op;
  struct ic_cluster_config *conf_objects;
  struct ic_api_cluster_connection cluster_conn;
  guint32 *cluster_ids;
  guint32 *node_ids;
  guint32 num_clusters_to_connect;
};

/*
  The struct ic_run_cluster_server represents the configuration of
  all clusters that the Cluster Server maintains.
*/
struct ic_run_cluster_server
{
  struct ic_run_cluster_server_operations run_op;
  struct ic_cluster_config *conf_objects;
  struct ic_run_cluster_conn run_conn;
  guint32 *cluster_ids;
  guint32 num_clusters;
};

struct ic_kernel_node_config
{
  char *filesystem_path;
  char *checkpoint_path;
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
  guint32 number_of_key_operation_objects;
  guint32 use_unswappable_memory;
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
  guint32 number_of_ordered_key_objects;
  guint32 number_of_unique_hash_key_objects;
  guint32 redo_log_memory;
  guint32 kernel_disk_write_speed;
  guint32 kernel_disk_write_speed_start;
  guint32 kernel_file_synch_size;
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

  gchar kernel_volatile_mode;
  gchar kernel_automatic_restart;
  gchar kernel_rt_scheduler_threads;
};

struct ic_client_node_config
{
  char *hostname;
  char *node_data_path;

  guint32 client_max_batch_byte_size;
  guint32 client_batch_byte_size;
  guint32 client_batch_size;
  guint32 client_resolve_rank;
  guint32 client_resolve_timer;
};

struct ic_cluster_server_config
{
  char *hostname;
  char *node_data_path;

  guint32 client_resolve_rank;
  guint32 client_resolve_timer;
  guint32 cluster_server_event_log;
  guint32 cluster_server_port_number;
};

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

  gchar use_message_id;
  gchar use_checksum;
  /* Ignore Connection Group for now */
};

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

    We have a number of variables to keep track of allocated memory
    and use of allocated memory for strings.
  */
  char *config_memory_to_return;
  char *string_memory_to_return;
  char *end_string_memory;
  char *next_string_memory;
  /*
    node_config is an array of pointers that point to structs of the
    types:
    ic_kernel_node_config        iClaustron kernel nodes
    ic_client_node_config        iClaustron client nodes
    ic_clusterserver_config      iClaustron cluster server nodes

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
  guint32 no_of_cluster_servers;
  guint32 no_of_client_nodes;
  guint32 no_of_comms;
  guint32 string_memory_size;
  guint32 *node_ids;
  enum ic_node_type *node_types;
  enum ic_communication_type *comm_types;
};

