#include <ic_comm.h>

struct ic_cluster_config_object;
struct ic_api_cluster_connection;

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 num_clusters);

struct ic_api_cluster_operations
{
  int (*get_ic_config) (struct ic_api_cluster_server *apic);
};

struct ic_api_cluster_connection
{
  guint32 *cluster_server_ips;
  guint16 *cluster_server_ports;
  struct ic_connection *cluster_srv_conns;
  guint32 num_cluster_servers;
  guint32 node_id;
};

struct ic_api_cluster_server
{
  struct ic_api_cluster_operations api_op;
  struct ic_cluster_config_object **conf_objects;
  struct ic_api_cluster_connection *cluster_conn;
  guint32 num_clusters;
};

struct ic_cluster_config_object
{
};

