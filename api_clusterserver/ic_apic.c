#include <ic_apic.h>
/*
  Description of protocol to get configuration profile from
  cluster server.
  1) Start with get_nodeid session. The api sends a number of
     lines of text with information about itself and login
     information.
     get nodeid<CR>
     nodeid: 0<CR>
     version: 327948<CR>
     nodetype: 1<CR>
     user: mysqld<CR>
     password: mysqld<CR>
     public key: a public key<CR>
     endian: little<CR>
     log_event: 0<CR>
     <CR>

     Here nodeid is normally 0, meaning that the receiver can accept
     any nodeid connected to the host we are connecting from. If nodeid
     != 0 then this is the only nodeid we accept.
     Version number is the version of the API, the version number is the
     hexadecimal notation of the version number (e.g. 5.1.12 = 0x5010c =
     327948).
     Nodetype is either 1 for API's to the data servers, for data servers
     the nodetype is 0 and for cluster servers it is 2.
     user, password and public key is preparation for future functionality
     so should always be mysqld, mysqld and a public key for API nodes.
     Endian should either be little or big, dependent on if the API is on
     little-endian box or big-endian box.
     Log event is ??

     The cluster server will respond with either an error response or
     a response that provides the nodeid to the API using the following
     messages.
     get nodeid reply<CR>
     nodeid: 4<CR>
     result: Ok<CR>
     <CR>

     Where nodeid is the one chosen by the cluster server.

     An error response is sent as 
     result: Error (MEssafge)<CR>
     <CR>

  2) The next step is to get the configuration. To get the configuration
     the API sends the following messages:
     get config<CR>
     version: 327948<CR>
     <CR>

     In response to this the cluster server will send the configuration
     or an error response in the same manner as above.
     The configuration is sent as follows.
     get config reply<CR>
     result: Ok<CR>
     Content-Length: 3047<CR>
     ndbconfig/octet-stream<CR>
     Content-Transfer-Encoding: base64<CR>
     <CR>
     base64-encoded string with length as provided above
*/
static const char *get_nodeid_str= "get nodeid";
static const char *get_nodeid_reply_str= "get nodeid reply";
static const char *get_config_str= "get config";
static const char *get_config_reply_str= "get config reply";
static const char *nodeid_str= "nodeid: ";
static const char *version_str= "version: ";
static const char *nodetype_str= "nodetype: 1";
static const char *user_str= "user: mysqld";
static const char *password_str= "password: mysqld";
static const char *public_key_str= "public key: a public key";
static const char *endian_str= "endian: ";
static const char *log_event_str= "log_event: 0";
static const char *result_ok_str= "result: Ok";
static const char *content_len_str= "Content-Length: ";
static const char *octet_stream_str= "Content-Type: ndbconfig/octet-stream";
static const char *content_encoding_str= "Content-Transfer-Encoding: base64";
static const guint32 version_no= (guint32)0x5010C; /* 5.1.12 */


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


static int
send_cluster_server(struct ic_connection *conn, const char *send_buf)
{
  guint32 inx;
  int res;
  char buf[256];

  strcpy(buf, send_buf);
  inx= strlen(buf);
  buf[inx++]= CARRIAGE_RETURN;
  buf[inx]= NULL_BYTE;
  DEBUG(printf("Send: %s", buf));
  res= conn->conn_op.write_ic_connection(conn, (const void*)buf, inx, 0, 1);
  return res;
}

static int
translate_config(__attribute__ ((unused)) struct ic_api_cluster_server *apic,
                 __attribute__ ((unused)) guint32 current_cluster_index,
                 char *config_buf,
                 guint32 config_size)
{
  char *bin_buf;
  guint32 bin_config_size;
  int error;

  g_assert((config_size & 3) == 0);
  bin_config_size= (config_size >> 2) * 3;
  if ((bin_buf= g_try_malloc0(bin_config_size)))
    return MEM_ALLOC_ERROR;
  if ((error= base64_decode(bin_buf, &bin_config_size,
                            config_buf, config_size)))
  {
    DEBUG(printf("Protocol error in base64 decode\n"));
    return PROTOCOL_ERROR;
  }
  return 0;
}

static int
rec_cs_read_line(struct ic_api_cluster_server *apic,
                 struct ic_connection *conn,
                 char **read_line,
                 guint32 *read_size)
{
  char *rec_buf= (char*)&apic->cluster_conn.rec_buf;
  char *tail_rec_buf= rec_buf + apic->cluster_conn.tail_index;
  guint32 size_curr_buf= apic->cluster_conn.head_index - 
                         apic->cluster_conn.tail_index;
  guint32 inx, size_to_read, size_read;
  int res;
  char *end_line;

  do
  {
    if (size_curr_buf > 0)
    {
      for (end_line= tail_rec_buf, inx= 0;
           inx < size_curr_buf && end_line[inx] != CARRIAGE_RETURN;
           inx++)
        ;
      if (inx != size_curr_buf)
      {
        *read_line= tail_rec_buf;
        *read_size= inx;
        apic->cluster_conn.tail_index+= (inx + 1);
        return 0;
      }
      /*
        We had no complete lines to read in the buffer received so
        far. We move the current buffer to the beginning to prepare
        for receiving more data.
      */
      memmove(rec_buf, tail_rec_buf, size_curr_buf);
      tail_rec_buf= rec_buf;
      apic->cluster_conn.tail_index= 0;
      apic->cluster_conn.head_index= size_curr_buf;
    }
    else
    {
      apic->cluster_conn.head_index= apic->cluster_conn.tail_index= 0;
    }

    size_to_read= REC_BUF_SIZE - size_curr_buf;
    if ((res= conn->conn_op.read_ic_connection(conn,
                                               rec_buf + size_curr_buf,
                                               size_to_read,
                                               &size_read)))
      return res;

    DEBUG(ic_print_buf(rec_buf + size_curr_buf, size_read));

    size_curr_buf+= size_read;
    apic->cluster_conn.head_index+= size_read;
    tail_rec_buf= rec_buf;
  } while (1);
  return 0;
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
  char endian_buf[32];
  guint32 node_id= apic->node_ids[current_cluster_index];

  g_snprintf(version_buf, 32, "%s%u", version_str, version_no);
  g_snprintf(nodeid_buf, 32, "%s%u", nodeid_str, node_id);
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
  g_snprintf(endian_buf, 32, "%s%s", endian_str, "little");
#else
  g_snprintf(endian_buf, 32, "%s%s", endian_str, "big");
#endif
  if (send_cluster_server(conn, get_nodeid_str) ||
      send_cluster_server(conn, version_buf) ||
      send_cluster_server(conn, nodetype_str) ||
      send_cluster_server(conn, nodeid_buf) ||
      send_cluster_server(conn, user_str) ||
      send_cluster_server(conn, password_str) ||
      send_cluster_server(conn, public_key_str) ||
      send_cluster_server(conn, endian_buf) ||
      send_cluster_server(conn, log_event_str) ||
      send_cluster_server(conn, ""))
    return conn->error_code;
  return 0;
}
static int
send_get_config(struct ic_connection *conn)
{
  char version_buf[32];

  g_snprintf(version_buf, 32, "%s%u", version_str, version_no);
  if (send_cluster_server(conn, get_config_str) ||
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
  char *read_line_buf;
  guint32 read_size;
  int error;
  guint64 node_number;
  guint32 state= GET_NODEID_REPLY_STATE;
  while (!(error= rec_cs_read_line(apic, conn, &read_line_buf, &read_size)))
  {
     DEBUG(ic_print_buf(read_line_buf, read_size));
     switch (state)
     {
       case GET_NODEID_REPLY_STATE:
         if ((read_size != GET_NODEID_REPLY_LEN) ||
             (memcmp(read_line_buf, get_nodeid_reply_str,
                     GET_NODEID_REPLY_LEN) != 0))
         {
           DEBUG(printf("Protocol error in Get nodeid reply state\n"));
           return PROTOCOL_ERROR;
         }
         state= NODEID_STATE;
         break;
       case NODEID_STATE:
         if ((read_size <= NODEID_LEN) ||
             (memcmp(read_line_buf, nodeid_str, NODEID_LEN) != 0) ||
             (convert_str_to_int_fixed_size(read_line_buf + NODEID_LEN,
                                            read_size - NODEID_LEN,
                                            &node_number)) ||
             (node_number > MAX_NODE_ID))
         {
           DEBUG(printf("Protocol error in nodeid state\n"));
           return PROTOCOL_ERROR;
         }
         DEBUG(printf("Nodeid = %u\n", (guint32)node_number));
         apic->node_ids[current_cluster_index]= (guint32)node_number;
         state= RESULT_OK_STATE;
         break;
       case RESULT_OK_STATE:
         if ((read_size != RESULT_OK_LEN) ||
             (memcmp(read_line_buf, result_ok_str, RESULT_OK_LEN) != 0))
         {
           DEBUG(printf("Protocol error in result ok state\n"));
           return PROTOCOL_ERROR;
         }
         state= WAIT_EMPTY_RETURN_STATE;
         break;
       case WAIT_EMPTY_RETURN_STATE:
         if (read_size != 0)
         {
           DEBUG(printf("Protocol error in result ok state\n"));
           return PROTOCOL_ERROR;
         }
         return 0;
       case RESULT_ERROR_STATE:
         break;
       default:
         break;
     }
  }
  return error;
}

static int
rec_get_config(struct ic_connection *conn,
               struct ic_api_cluster_server *apic,
               guint32 current_cluster_index)
{
  char *read_line_buf, *config_buf;
  guint32 read_size, config_size, rec_config_size;
  int error;
  guint64 content_length;
  guint32 state= GET_CONFIG_REPLY_STATE;
  while (!(error= rec_cs_read_line(apic, conn, &read_line_buf, &read_size)))
  {
     DEBUG(ic_print_buf(read_line_buf, read_size));
     switch (state)
     {
       case GET_CONFIG_REPLY_STATE:
         if ((read_size != GET_CONFIG_REPLY_LEN) ||
             (memcmp(read_line_buf, get_config_reply_str,
                     GET_CONFIG_REPLY_LEN) != 0))
         {
           DEBUG(printf("Protocol error in get config reply state\n"));
           return PROTOCOL_ERROR;
         }
         state= RESULT_OK_STATE;
         break;
       case RESULT_OK_STATE:
         if ((read_size != RESULT_OK_LEN) ||
             (memcmp(read_line_buf, result_ok_str, RESULT_OK_LEN) != 0))
         {
           DEBUG(printf("Protocol error in result ok state\n"));
           return PROTOCOL_ERROR;
         }
         state= CONTENT_LENGTH_STATE;
         break;
       case CONTENT_LENGTH_STATE:
         if ((read_size <= CONTENT_LENGTH_LEN) ||
             (memcmp(read_line_buf, content_len_str,
                     CONTENT_LENGTH_LEN) != 0) ||
             convert_str_to_int_fixed_size(read_line_buf+CONTENT_LENGTH_LEN,
                                           read_size - CONTENT_LENGTH_LEN,
                                           &content_length) ||
             (content_length > MAX_CONTENT_LEN))
         {
           DEBUG(printf("Protocol error in content length state\n"));
           return PROTOCOL_ERROR;
         }
         DEBUG(printf("Content Length: %u\n", (guint32)content_length));
         state= OCTET_STREAM_STATE;
         break;
       case OCTET_STREAM_STATE:
         if ((read_size != OCTET_STREAM_LEN) ||
             (memcmp(read_line_buf, octet_stream_str,
                     OCTET_STREAM_LEN) != 0))
         {
           DEBUG(printf("Protocol error in octet stream state\n"));
           return PROTOCOL_ERROR;
         }
         state= CONTENT_ENCODING_STATE;
         break;
       case CONTENT_ENCODING_STATE:
         if ((read_size != CONTENT_ENCODING_LEN) ||
             (memcmp(read_line_buf, content_encoding_str,
                     CONTENT_ENCODING_LEN) != 0))
         {
           DEBUG(printf("Protocol error in content encoding state\n"));
           return PROTOCOL_ERROR;
         }
         state= WAIT_EMPTY_RETURN_STATE;
         break;
       case WAIT_EMPTY_RETURN_STATE:
         /*
           At this point we should now start receiving the configuration in
           base64 encoded format. It will arrive in 76 character chunks
           followed by a carriage return.
         */
         state= RECEIVE_CONFIG_STATE;
         if ((config_buf= g_try_malloc0(content_length)))
           return MEM_ALLOC_ERROR;
         config_size= 0;
         rec_config_size= 0;
         /*
           Here we need to allocate receive buffer for configuration plus the
           place to put the encoded binary data.
         */
         break;
       case RECEIVE_CONFIG_STATE:
         memcpy(config_buf+config_size, read_line_buf, read_size);
         config_size+= read_size;
         rec_config_size+= (read_size + 1);
         if (rec_config_size >= content_length)
         {
           /*
             This is the last line, we have now received the config
             and are ready to translate it.
           */
           return translate_config(apic, current_cluster_index,
                                   config_buf, config_size);
         }
         else if (read_size != CONFIG_LINE_LEN)
         {
           DEBUG(printf("Protocol error in config receive state\n"));
           return PROTOCOL_ERROR;
         }
         break;
       case RESULT_ERROR_STATE:
         break;
       default:
         break;
     }
  }
  return error;
}

static int
get_cs_config(struct ic_api_cluster_server *apic)
{
  guint32 i;
  int error= END_OF_FILE;
  struct ic_connection conn_obj, *conn;

  DEBUG(printf("Get config start\n"));
  conn= &conn_obj;

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
  for (i= 0; i < apic->num_clusters_to_connect; i++)
  {
    if ((error= send_get_nodeid(conn, apic, i)) ||
        (error= rec_get_nodeid(conn, apic, i)) ||
        (error= send_get_config(conn)) ||
        (error= rec_get_config(conn, apic, i)))
      continue;
  }
  conn->conn_op.close_ic_connection(conn);
  conn->conn_op.free_ic_connection(conn);
  return error;
}

static void
free_cs_config(struct ic_api_cluster_server *apic)
{
  if (apic)
  {
    if (apic->cluster_conn.cluster_srv_conns)
      g_free(apic->cluster_conn.cluster_srv_conns);
    if (apic->cluster_conn.cluster_srv_conns)
      g_free(apic->cluster_conn.cluster_server_ips);
    if (apic->cluster_conn.cluster_server_ports)
      g_free(apic->cluster_conn.cluster_server_ports);
    if (apic->cluster_ids)
      g_free(apic->cluster_ids);
    if (apic->node_ids)
      g_free(apic->node_ids);
    g_free(apic);
  }
  return;
}

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 *node_ids,
                    guint32 num_clusters)
{
  struct ic_api_cluster_server *apic= NULL;
  guint32 num_cluster_servers= cluster_conn->num_cluster_servers;
  /*
    The idea with this method is that the user can set-up his desired usage
    of the clusters using stack variables. Then we copy those variables to
    heap storage and maintain this data hereafter.
    Thus we can in many cases relieve the user from the burden of error
    handling of memory allocation failures.

    We will also ensure that the supplied data is validated.
  */

  if (!(apic= g_try_malloc0(sizeof(struct ic_api_cluster_server))) ||
      !(apic->cluster_ids= g_try_malloc0(sizeof(guint32) * num_clusters)) ||
      !(apic->node_ids= g_try_malloc0(sizeof(guint32) * num_clusters)) ||
      !(apic->cluster_conn.cluster_server_ips= 
         g_try_malloc0(num_cluster_servers *
                       sizeof(guint32))) ||
      !(apic->cluster_conn.cluster_server_ports=
         g_try_malloc0(num_cluster_servers *
                       sizeof(guint16))) ||
      !(apic->cluster_conn.cluster_srv_conns=
         g_try_malloc0(num_cluster_servers *
                       sizeof(struct ic_connection))))
  {
    free_cs_config(apic);
    return NULL;
  }

  memcpy((char*)apic->cluster_ids,
         (char*)cluster_ids,
         num_clusters * sizeof(guint32));

  memcpy((char*)apic->node_ids,
         (char*)node_ids,
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
  apic->api_op.free_ic_config= free_cs_config;
  return apic;
}

