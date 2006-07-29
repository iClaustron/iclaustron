#include <ic_apic.h>
static int
set_up_cluster_server_connection(struct ic_connection *conn,
                                 guint32 server_ip,
                                 guint16 server_port)
{
  int error;

  if (ic_init_socket_object(conn, TRUE, FALSE, FALSE, FALSE))
    return MEM_ALLOC_ERROR;
  conn->server_ip= server_ip;
  conn->server_port= server_port;
  if ((error= conn->conn_op.set_up_ic_connection(conn)))
    return error;
  DEBUG(printf("Successfully set-up connection to cluster server\n"));
  return 0;
}

static int
get_cs_config(struct ic_api_cluster_server *apic)
{
  unsigned int i, j;
  int error;
  struct ic_connection *conn;
  struct ic_api_cluster_connection *base_conn= apic->cluster_conn;

  for (i= 0; i < apic->num_clusters; i++)
  {
    struct ic_api_cluster_connection *curr_conn= base_conn + i;
    for (j= 0; j < curr_conn->num_cluster_servers; j++)
    {
       conn= curr_conn->cluster_srv_conns + j;
       if ((error= set_up_cluster_server_connection(conn,
                                       curr_conn->cluster_server_ips[j],
                                       curr_conn->cluster_server_ports[j])))
         return error;
     }
  }
  return 0;
}

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 num_clusters)
{
  unsigned int i;
  struct ic_api_cluster_server *apic;
  /*
    The idea with this method is that the user can set-up his desired usage
    of the clusters using stack variables. Then we copy those variables to
    heap storage and maintain this data hereafter.
    Thus we can in many cases relieve the user from the burden of error
    handling of memory allocation failures.

    We will also ensure that the supplied data is validated.
  */

  apic= malloc(sizeof(struct ic_api_cluster_server));
  apic->cluster_conn= malloc(num_clusters *
                             sizeof(struct ic_api_cluster_connection));
  for (i= 0; i < num_clusters; i++)
  {
    struct ic_api_cluster_connection *curr_conn= cluster_conn + i;
    struct ic_api_cluster_connection *curr_init_conn= apic->cluster_conn + i;
    guint32 num_cluster_servers= curr_conn->num_cluster_servers;

    curr_init_conn->num_cluster_servers= num_cluster_servers;
    curr_init_conn->node_id= curr_conn->node_id;

    curr_init_conn->cluster_server_ips= malloc(num_cluster_servers *
                                               sizeof(guint32));
    curr_init_conn->cluster_server_ports= malloc(num_cluster_servers *
                                                 sizeof(guint16));

    memcpy((char*)curr_init_conn->cluster_server_ips,
           (char*)curr_conn->cluster_server_ips,
           num_cluster_servers * sizeof(guint32));
    memcpy((char*)curr_init_conn->cluster_server_ports,
           (char*)curr_conn->cluster_server_ports,
           num_cluster_servers * sizeof(guint16));
    curr_init_conn->cluster_srv_conns= malloc(num_cluster_servers *
                                              sizeof(struct ic_connection));
    memset((char*)curr_init_conn->cluster_srv_conns, 0,
           num_cluster_servers * sizeof(struct ic_connection));
  }
  apic->conf_objects= NULL;
  apic->api_op.get_ic_config= get_cs_config;
  return apic;
}

