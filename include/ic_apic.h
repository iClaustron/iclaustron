#include <ic_common.h>

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
  char rec_buf[256];
};

struct ic_api_cluster_server
{
  struct ic_api_cluster_operations api_op;
  struct ic_cluster_config_object **conf_objects;
  struct ic_api_cluster_connection cluster_conn;
  guint32 *cluster_ids;
  guint32 *node_ids;
  guint32 num_clusters_to_connect;
};

struct ic_cluster_config_object
{
};

