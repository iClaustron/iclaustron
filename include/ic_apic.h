#include <ic_common.h>


enum ic_node_type
{
  IC_KERNEL_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2
};

enum ic_config_entry_change
{
  IC_ONLINE_CHANGE = 0,
  IC_NODE_RESTART_CHANGE = 1,
  IC_ROLLING_UPGRADE_CHANGE = 2,
  IC_CLUSTER_RESTART_CHANGE = 3,
  IC_NOT_CHANGEABLE = 4
};

struct config_entry
{
  char *config_entry_name;
  char *config_entry_description;
  guint64 max_value;
  guint64 min_value;
  guint64 default_value;
  enum ic_config_entry_change change_variant;
  gchar is_max_value_defined;
  gchar is_min_value_defined;
  gchar is_defined;
  gchar is_boolean;
  gchar is_deprecated;
  gchar is_string_type;
  gchar is_mandatory_to_specify;
};

struct ic_cluster_config_object;
struct ic_api_cluster_connection;

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 *node_ids,
                    guint32 num_clusters);

struct ic_api_cluster_operations
{
  int (*get_ic_config) (struct ic_api_cluster_server *apic);
  void (*free_ic_config) (struct ic_api_cluster_server *apic);
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

struct ic_api_cluster_server
{
  struct ic_api_cluster_operations api_op;
  struct ic_api_cluster_config *conf_objects;
  struct ic_api_cluster_connection cluster_conn;
  guint32 *cluster_ids;
  guint32 *node_ids;
  guint32 num_clusters_to_connect;
};

struct ic_kernel_node_config
{
};

struct ic_client_node_config
{
};

struct ic_cluster_server_config
{
};

struct ic_tcp_comm_link_config
{
  char *first_host_name;
  char *second_host_name;
  guint32 write_buffer_size;
  guint32 read_buffer_size;
  guint16 first_node_id;
  guint16 second_node_id;
  guint16 client_port;
  guint16 server_port;
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

struct ic_api_cluster_config
{
  /*
    We keep track of all nodes in each cluster we participate in,
    also we keep track of number of kernel nodes, number of
    cluster servers, and number of client nodes and finally the
    number of communication links.
    For each node in the cluster and each communication link we also
    store the node id, the node type and the configuration parameters.
  */
  char *config_memory_to_return;
  char *string_memory_to_return;
  char *end_string_memory;
  char *next_string_memory;
  char **node_config;
  char **comm_config;

  guint32 no_of_nodes;
  guint32 no_of_kernel_nodes;
  guint32 no_of_cluster_servers;
  guint32 no_of_client_nodes;
  guint32 no_of_comms;
  guint32 string_memory_size;
  guint32 *node_ids;
  enum ic_node_type *node_types;
};

