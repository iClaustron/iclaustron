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
  unsigned int i;
  int error;

  for (i= 0; i < apic->cluster_conn.num_cluster_servers; i++)
  {
    if (!(error= set_up_cluster_server_connection(
                                apic->cluster_conn.cluster_srv_conns + i,
                                apic->cluster_conn.cluster_server_ips[i],
                                apic->cluster_conn.cluster_server_ports[i])))
      return 0;
  }
  return error;
}

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 num_clusters)
{
  struct ic_api_cluster_server *apic;
  guint32 num_cluster_servers= cluster_conn->num_cluster_servers;
  /*
    The idea with this method is that the user can set-up his desired usage
    of the clusters using stack variables. Then we copy those variables to
    heap storage and maintain this data hereafter.
    Thus we can in many cases relieve the user from the burden of error
    handling of memory allocation failures.

    We will also ensure that the supplied data is validated.
  */

  apic= malloc(sizeof(struct ic_api_cluster_server));
  apic->cluster_ids= malloc(sizeof(guint32) * num_clusters);
  apic->cluster_conn.cluster_server_ips= malloc(num_cluster_servers *
                                                sizeof(guint32));
  apic->cluster_conn.cluster_server_ports= malloc(num_cluster_servers *
                                                  sizeof(guint16));
  apic->cluster_conn.cluster_srv_conns= malloc(num_cluster_servers *
                                               sizeof(struct ic_connection));

  memcpy((char*)apic->cluster_ids,
         (char*)cluster_ids,
         num_clusters * sizeof(guint32));

  memcpy((char*)apic->cluster_conn.cluster_server_ips,
         (char*)cluster_conn->cluster_server_ips,
         num_cluster_servers * sizeof(guint32));
  memcpy((char*)apic->cluster_conn.cluster_server_ports,
         (char*)cluster_conn->cluster_server_ports,
         num_cluster_servers * sizeof(guint16));
  memset((char*)apic->cluster_conn.cluster_srv_conns, 0,
         num_cluster_servers * sizeof(struct ic_connection));

  apic->cluster_conn.num_cluster_servers= num_cluster_servers;
  apic->conf_objects= NULL;
  apic->api_op.get_ic_config= get_cs_config;
  return apic;
}

