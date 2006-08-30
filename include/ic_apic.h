#include <ic_common.h>

enum ic_node_type
{
  IC_KERNEL_TYPE = 0,
  IC_CLIENT_TYPE = 1,
  IC_CLUSTER_SERVER_TYPE = 2
};

#define MAX_NODE_ID 63

#define GET_NODEID_REPLY_STATE 0
#define NODEID_STATE 1
#define RESULT_OK_STATE 2
#define WAIT_EMPTY_RETURN_STATE 3
#define RESULT_ERROR_STATE 4

#define GET_CONFIG_REPLY_STATE 5
#define CONTENT_LENGTH_STATE 6
#define OCTET_STREAM_STATE 7
#define CONTENT_ENCODING_STATE 8
#define RECEIVE_CONFIG_STATE 9

#define GET_NODEID_REPLY_LEN 16
#define NODEID_LEN 8
#define RESULT_OK_LEN 10

#define GET_CONFIG_REPLY_LEN 16
#define CONTENT_LENGTH_LEN 16
#define OCTET_STREAM_LEN 36
#define CONTENT_ENCODING_LEN 33

#define CONFIG_LINE_LEN 76
#define MAX_CONTENT_LEN 16 * 1024 * 1024 /* 16 MByte max */

#define IC_CL_KEY_MASK 0x3FFF
#define IC_CL_KEY_SHIFT 28
#define IC_CL_SECT_SHIFT 14
#define IC_CL_SECT_MASK 0x3FFF
#define IC_CL_INT32_TYPE 1
#define IC_CL_CHAR_TYPE  2
#define IC_CL_SECT_TYPE  3
#define IC_CL_INT64_TYPE 4

#define IC_NODE_TYPE     999
#define IC_NODE_ID       3
#define IC_PARENT_ID     16382

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

#define TCP_FIRST_NODE_ID 400
#define TCP_SECOND_NODE_ID 401
#define TCP_USE_MESSAGE_ID 402
#define TCP_USE_CHECKSUM 403
#define TCP_CLIENT_PORT 2000
#define TCP_SERVER_PORT 406
#define TCP_SERVER_NODE_ID 410
#define TCP_WRITE_BUFFER_SIZE 454
#define TCP_READ_BUFFER_SIZE 455
#define TCP_FIRST_HOSTNAME 407
#define TCP_SECOND_HOSTNAME 408
#define TCP_GROUP 409

#define TCP_MIN_WRITE_BUFFER_SIZE 65536
#define TCP_MAX_WRITE_BUFFER_SIZE 100000000

#define TCP_MIN_READ_BUFFER_SIZE 8192
#define TCP_MAX_READ_BUFFER_SIZE 1000000000
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
