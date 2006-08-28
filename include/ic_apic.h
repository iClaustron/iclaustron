#include <ic_common.h>

enum ic_node_type
{
  IC_KERNEL_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2
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

struct ic_comm_link_config
{
};

struct ic_tcp_comm_link_config
{
};

struct ic_sci_comm_link_config
{
};

struct ic_shm_comm_link_config
{
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
  guint32 no_of_nodes;
  guint32 no_of_kernel_nodes;
  guint32 no_of_cluster_servers;
  guint32 no_of_client_nodes;
  guint32 no_of_comms;
  guint32 *node_ids;
  enum ic_node_type *node_types;
  char **node_config;
  char **comm_config;
};
