#include <ic_apic.h>
static int
send_cluster_server(struct ic_connection *conn, char *send_buf)
{
  guint32 inx;
  int res;
  char buf[2048];

  strcpy(buf, send_buf);
  inx= strlen(buf);
  buf[inx++]= (char)10;
  buf[inx]= (char)0;
  printf("Send: %s", buf);
  res= conn->conn_op.write_ic_connection(conn, (const void*)buf, inx, 0, 1);
  return res;
}

static int
rec_cluster_server(struct ic_api_cluster_server *apic,
                   struct ic_connection *conn)
{
  int res;
  unsigned i;
  guint32 read_size;
  char buf[2048];

  res= conn->conn_op.read_ic_connection(conn, (void*)buf, (guint32)2048,
                                        &read_size);
  if (!res)
  {
    for (i= 0; i < read_size; i++)
    {
      if (i > 0 && buf[i-1] == (char)10 && buf[i] == (char)10)
        res= 1;
    }
  }
  printf("%s", buf);
  return res;
}

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
  {
    DEBUG(printf("Connect failed with error %d\n", error));
    return error;
  }
  DEBUG(printf("Successfully set-up connection to cluster server\n"));
  return 0;
}

static int
send_get_nodeid(struct ic_connection *conn,
                struct ic_api_cluster_server *apic,
                guint32 current_cluster_index)
{
  char version_buf[32];
  char nodeid_buf[32];

  g_snprintf(version_buf, 32, "version: %u", (guint32)327948);
  g_snprintf(nodeid_buf, 32, "nodeid: %u", (guint32)4);
  if (send_cluster_server(conn, "get nodeid") ||
      send_cluster_server(conn, version_buf) ||
      send_cluster_server(conn, "nodetype: 1") ||
      send_cluster_server(conn, nodeid_buf) ||
      send_cluster_server(conn, "user: mysqld") ||
      send_cluster_server(conn, "password: mysqld") ||
      send_cluster_server(conn, "public key: a public key") ||
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
      send_cluster_server(conn, "endian: little") ||
#else
      send_cluster_server(conn, "endian: big") ||
#endif
      send_cluster_server(conn, "log_event: 0") ||
      send_cluster_server(conn, ""))
    return conn->error_code;
  return 0;
}
static int
send_get_config(struct ic_connection *conn,
                struct ic_api_cluster_server *apic,
                guint32 current_cluster_index)
{
  char version_buf[32];

  g_snprintf(version_buf, 32, "version: %u", (guint32)327948);
  if (send_cluster_server(conn, "get config") ||
      send_cluster_server(conn, version_buf) ||
      send_cluster_server(conn, ""))
    return conn->error_code;
  return 0;
}

static int
rec_get_nodeid(struct ic_connection *conn,
               struct ic_api_cluster_server *apic,
               guint32 current_cluster_index)
{
  int error;
  while (!(error= rec_cluster_server(apic, conn)))
    ;
  return 0;
}

static int
rec_get_config(struct ic_connection *conn,
               struct ic_api_cluster_server *apic,
               guint32 current_cluster_index)
{
  int error;
  while (!(error= rec_cluster_server(apic, conn)))
    ;
  return 0;
}

static int
get_cs_config(struct ic_api_cluster_server *apic)
{
  guint32 i;
  int error;
  struct ic_connection *conn;

  printf("Get config start\n");
  for (i= 0; i < apic->cluster_conn.num_cluster_servers; i++)
  {
    conn= apic->cluster_conn.cluster_srv_conns + i;
    if (!(error= set_up_cluster_server_connection(conn,
                  apic->cluster_conn.cluster_server_ips[i],
                  apic->cluster_conn.cluster_server_ports[i])))
      break;
  }
  if (error)
    return error;
  for (i= 0; !error && i < apic->num_clusters_to_connect; i++)
  {
    if ((error= send_get_nodeid(conn, apic, i)) ||
        (error= rec_get_nodeid(conn, apic, i)) ||
        (error= send_get_config(conn, apic, i)) ||
        (error= rec_get_config(conn, apic, i)))
      ;
    conn->conn_op.close_ic_connection(conn);
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

