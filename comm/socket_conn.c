#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <ic_comm.h>

static void
set_is_connected(struct ic_connection *conn)
{
  g_mutex_lock(conn->connect_mutex);
  conn->conn_stat.is_connected= TRUE;
  g_mutex_unlock(conn->connect_mutex);
  return;
}

static gboolean
is_conn_connected(struct ic_connection *conn)
{
  gboolean is_connected;
  g_mutex_lock(conn->connect_mutex);
  is_connected= conn->conn_stat.is_connected;
  g_mutex_unlock(conn->connect_mutex);
  return is_connected;
}

static gboolean
is_conn_thread_active(struct ic_connection *conn)
{
  gboolean is_thread_active;
  g_mutex_lock(conn->connect_mutex);
  is_thread_active= conn->conn_stat.is_connect_thread_active;
  g_mutex_unlock(conn->connect_mutex);
  return is_thread_active;
}

static int
set_option_size(int sockfd, int size, int old_size,
                int option_type, int option_no)
{
  if (size && size > old_size)
  {
    while (size > old_size &&
           setsockopt(sockfd, option_type, option_no,
           (const void*)&size, sizeof(int)))
      size>>= 1; /* Try with half the size */
    if (size > old_size)
      return size;
  }
  return old_size;
}

static void
ic_set_socket_options(struct ic_connection *conn, int sockfd)
{
  int no_delay, error, maxseg_size, rec_size, snd_size;
  socklen_t sock_len;
  /*
    We start by setting TCP_NODELAY to ensure the OS does no
    extra delays, we want to be in control of when data is
    sent and received.
  */
  no_delay= 1;
  if ((error= setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
                         (const void*)&no_delay, sizeof(int))))
  {
    /*
      We will continue even with this error.
    */
    no_delay= 0;
    DEBUG(printf("Set TCP_NODELAY error: %d\n", error));
  }
  getsockopt(sockfd, IPPROTO_TCP, TCP_MAXSEG,
             (void*)&maxseg_size, &sock_len);
  getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF,
             (void*)&rec_size, &sock_len);
  getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF,
             (void*)&snd_size, &sock_len);
  DEBUG(printf("Default TCP_MAXSEG = %d\n", maxseg_size));
  DEBUG(printf("Default SO_RCVBUF = %d\n", rec_size));
  DEBUG(printf("Default SO_SNDBUF = %d\n", snd_size));

  if (conn->is_wan_connection)
  {
    maxseg_size= set_option_size(sockfd, WAN_TCP_MAXSEG_SIZE, maxseg_size,
                                 IPPROTO_TCP, TCP_MAXSEG);
    rec_size= set_option_size(sockfd, WAN_REC_BUF_SIZE, rec_size,
                              SOL_SOCKET, SO_RCVBUF);
    snd_size= set_option_size(sockfd, WAN_SND_BUF_SIZE, snd_size,
                              SOL_SOCKET, SO_SNDBUF);
  }
  maxseg_size= set_option_size(sockfd, conn->tcp_maxseg_size, maxseg_size,
                               IPPROTO_TCP, TCP_MAXSEG);
  rec_size= set_option_size(sockfd, conn->tcp_receive_buffer_size,
                            rec_size, SOL_SOCKET, SO_RCVBUF);
  snd_size= set_option_size(sockfd, conn->tcp_send_buffer_size,
                            snd_size, SOL_SOCKET, SO_SNDBUF);
  DEBUG(printf("Used TCP_MAXSEG = %d\n", maxseg_size));
  DEBUG(printf("Used SO_RCVBUF = %d\n", rec_size));
  DEBUG(printf("Used SO_SNDBUF = %d\n", snd_size));
  DEBUG(printf("Used TCP_NODELAY = %d\n", no_delay));
  conn->conn_stat.used_tcp_maxseg_size= maxseg_size;
  conn->conn_stat.used_tcp_receive_buffer_size= rec_size;
  conn->conn_stat.used_tcp_send_buffer_size= snd_size;
  conn->conn_stat.tcp_no_delay= no_delay;
}

static int
accept_socket_connection(struct ic_connection *conn, int sockfd)
{
  int error;
  socklen_t addr_len;
  struct sockaddr_in client_address;

  addr_len= sizeof(client_address);
  if (accept(sockfd, (struct sockaddr *)&client_address,
             &addr_len) < 0)
  {
    DEBUG(printf("accept error: %d %s\n",
                 errno, sys_errlist[errno]));
    error= errno;
    goto error;
  }
  if (conn->client_ip != INADDR_ANY &&
      (g_htonl(conn->client_ip) != client_address.sin_addr.s_addr ||
       (conn->client_port &&
        g_htons(conn->client_port) != client_address.sin_port)))
  {
    /*
      The caller only accepted connect's from certain IP address and
      port number. The accepted connection was not from the accepted
      IP address and port. Setting client_ip to INADDR_ANY means
      any connection is allowed. Setting client_port to 0 means
      any port number is allowed to connect from.
      We'll disconnect by closing the socket and report an error.
    */
    DEBUG(printf(
      "Client connect from a disallowed address, ip=%x, port=%u\n",
      conn->client_ip, conn->client_port));
    error= ACCEPT_ERROR;
    goto error;
  }
  set_is_connected(conn);
  return 0;

error:
  close(sockfd);
  return error;
}

static int
int_set_up_socket_connection(struct ic_connection *conn)
{
  int error, sockfd;
  struct sockaddr_in client_address, server_address;

  if (conn->is_mutex_used)
  {
    conn->read_mutex= g_mutex_new();
    conn->write_mutex= g_mutex_new();
    if (!(conn->read_mutex && conn->write_mutex))
      return MEM_ALLOC_ERROR;
  }
  memset(&client_address, sizeof(client_address), 0);
  client_address.sin_family= AF_INET;
  client_address.sin_addr.s_addr= g_htonl(conn->client_ip);
  client_address.sin_port= g_htons(conn->client_port);
  memset(&server_address, sizeof(server_address), 0);
  server_address.sin_family= AF_INET;
  server_address.sin_addr.s_addr= g_htonl(conn->server_ip);
  server_address.sin_port= g_htons(conn->server_port);

  /*
    Create a socket for the connection
  */
  if ((sockfd= socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
  {
    return errno;
  }
  ic_set_socket_options(conn, sockfd);
  if (conn->is_client)
  { 
    /*
      Bind the socket to an IP address and port on this box.  If the caller
      set client_port to 0 then an ephemeral port will be used. If the
      caller set client_ip to INADDR_ANY then any interface can be used on
      the server.
    */
    if (bind(sockfd, (struct sockaddr *)&client_address,
             sizeof(client_address)) < 0)
    {
      DEBUG(printf("bind error\n"));
      goto error;
    }
    do
    {
      /*
        Connect the client to the server, will block until connected or
        timeout occurs.
      */
      if (connect(sockfd, (struct sockaddr*)&server_address,
                  sizeof(server_address)) < 0)
      {
        if (errno == EINTR || errno == ECONNREFUSED)
          continue;
        return errno;
      }
      set_is_connected(conn);
      break;
    } while (1);
  }
  else
  {
    /*
      Bind the socket to an IP address on this box.
    */
    if (bind(sockfd, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
      DEBUG(printf("bind error\n"));
      goto error;
    }
    /*last time this timer was cleared (can be cleared in read_stat_ic_connection call).

      Listen for incoming connect messages
    */
    if ((listen(sockfd, conn->backlog) < 0))
    {
      DEBUG(printf("listen error\n"));
      goto error;
    }
    if ((error= accept_socket_connection(conn, sockfd)))
      return error;
  }
  conn->sockfd= sockfd;
  return 0;

error:
   error= errno;
   close(sockfd);
   return error;
}

static gpointer
run_set_up_socket_connection(gpointer data)
{
  int ret_code;
  struct ic_connection *conn= (struct ic_connection *)data;
  ret_code= int_set_up_socket_connection(conn);
  g_mutex_lock(conn->connect_mutex);
  conn->conn_stat.is_connect_thread_active= FALSE;
  g_mutex_unlock(conn->connect_mutex);
  return NULL;
}

static int
set_up_socket_connection(struct ic_connection *conn)
{
  GError *error= NULL;
  if (!g_thread_supported())
    g_thread_init(NULL);
  conn->connect_mutex= g_mutex_new();
  if (!conn->connect_mutex)
    return MEM_ALLOC_ERROR;
  if (!conn->is_connect_thread_used)
    return int_set_up_socket_connection(conn);

  if (!g_thread_create_full(run_set_up_socket_connection,
                            (gpointer)conn,
                            8192, /* Stack size */
                            FALSE, /* Not joinable */
                            FALSE, /* Not bound */
                            G_THREAD_PRIORITY_NORMAL,
                            &error))
  {
    return 1; /* Should write proper error handling code here */
  }
  return 0;
}

static int
write_socket_connection(struct ic_connection *conn,
                        const void *buf, guint32 size,
                        guint32 secs_to_try)
{
  GTimer *time_measure;
  gdouble secs_expired;
  gdouble next_sec_check;
  guint32 write_size= 0;
  guint32 loop_count= 0;
  guint32 secs_count;

  do
  {
    gssize ret_code;
    gsize buf_size= size - write_size;

    ret_code= write(conn->sockfd, buf + write_size, buf_size);
    if (ret_code == (int)buf_size)
    {
      if (loop_count && time_measure)
        g_timer_destroy(time_measure);
      return 0;
    }
    if (!loop_count)
    {
      time_measure= g_timer_new();
      secs_expired= (gdouble)0;
      next_sec_check= (gdouble)1;
      secs_count= 0;
    }
    if (ret_code < 0)
    {
      if (errno == EINTR)
        continue;
      return errno;
    }
    write_size+= ret_code;
    if (loop_count)
    {
      if ((time_measure &&
          ((g_timer_elapsed(time_measure, NULL) + secs_expired) >
          next_sec_check)) ||
          loop_count == 1000)
      {
        next_sec_check+= (gdouble)1;
        secs_count++;
        if (secs_count >= secs_to_try)
          return EINTR;
        loop_count= 1;
      }
    }
    if (loop_count)
      g_usleep(1000); 
    loop_count++;
  } while (1);
  return 0;
}

static int
close_socket_connection(struct ic_connection *conn)
{
  close(conn->sockfd);
  return 0;
}

static int
read_socket_connection(struct ic_connection *conn,
                       void *buf, guint32 buf_size,
                       guint32 *read_size)
{
  int ret_code, error;
  do
  {
    ret_code= read(conn->sockfd, buf, buf_size);
    if (ret_code >= 0)
    {
      *read_size= ret_code;
      return 0;
    }
    error= errno;
  } while (error != EINTR);
  return error;
}

static int
open_write_socket_session_mutex(struct ic_connection *conn,
                                guint32 total_size)
{
  g_mutex_lock(conn->write_mutex);
  return 0;
}

static int
open_write_socket_session(struct ic_connection *conn, guint32 total_size)
{
  return 0;
}

static int
close_write_socket_session_mutex(struct ic_connection *conn)
{
  g_mutex_unlock(conn->write_mutex);
  return 0;
}

static int
close_write_socket_session(struct ic_connection *conn)
{
  return 0;
}

static int
open_read_socket_session_mutex(struct ic_connection *conn)
{
  g_mutex_lock(conn->read_mutex);
  return 0; 
}

static int
open_read_socket_session(struct ic_connection *conn)
{
  return 0;
}

static int
close_read_socket_session_mutex(struct ic_connection *conn)
{
  g_mutex_unlock(conn->read_mutex);
  return 0;
}

static int
close_read_socket_session(struct ic_connection *conn)
{
  return 0;
}

static void
free_socket_connection(struct ic_connection *conn)
{
  g_timer_destroy(conn->connection_start);
  g_timer_destroy(conn->last_read_stat);
}

static void
read_stat_socket_connection(struct ic_connection *conn,
                            struct ic_connect_stat *stat,
                            gboolean clear_stat_timer)
{
  memcpy((void*)stat, (void*)&conn->conn_stat,
         sizeof(struct ic_connect_stat));
  if (clear_stat_timer)
    g_timer_reset(conn->last_read_stat);
}

static void
safe_read_stat_socket_conn(struct ic_connection *conn,
                           struct ic_connect_stat *stat,
                           gboolean clear_stat_timer)
{
  g_mutex_lock(conn->read_mutex);
  g_mutex_lock(conn->write_mutex);
  g_mutex_lock(conn->connect_mutex);
  read_stat_socket_connection(conn, stat, clear_stat_timer);
  g_mutex_unlock(conn->read_mutex);
  g_mutex_unlock(conn->write_mutex);
  g_mutex_unlock(conn->connect_mutex);
}

static double
read_socket_connection_time(struct ic_connection *conn,
                            ulong *microseconds)
{
  return g_timer_elapsed(conn->connection_start, microseconds);
}

static double
read_socket_stat_time(struct ic_connection *conn,
                      ulong *microseconds)
{
  return g_timer_elapsed(conn->last_read_stat, microseconds);
}

gboolean
ic_init_socket_object(struct ic_connection *conn,
                      gboolean is_client,
                      gboolean is_mutex_used,
                      gboolean is_connect_thread_used)
{
  guint32 i;

  conn->conn_op.set_up_ic_connection= set_up_socket_connection;
  conn->conn_op.accept_ic_connection= accept_socket_connection;
  conn->conn_op.close_ic_connection= close_socket_connection;
  conn->conn_op.write_ic_connection = write_socket_connection;
  conn->conn_op.read_ic_connection = read_socket_connection;
  conn->conn_op.free_ic_connection= free_socket_connection;
  conn->conn_op.read_stat_ic_connection= read_stat_socket_connection;
  conn->conn_op.safe_read_stat_ic_connection= safe_read_stat_socket_conn;
  conn->conn_op.ic_read_connection_time= read_socket_connection_time;
  conn->conn_op.ic_read_stat_time= read_socket_stat_time;
  conn->conn_op.is_ic_conn_thread_active= is_conn_thread_active;
  conn->conn_op.is_ic_conn_connected= is_conn_connected;
  if (is_mutex_used)
  {
    conn->conn_op.open_write_session = open_write_socket_session_mutex;
    conn->conn_op.close_write_session = close_write_socket_session_mutex;
    conn->conn_op.open_read_session = open_read_socket_session_mutex;
    conn->conn_op.close_read_session = close_read_socket_session_mutex;
  }
  else
  {
    conn->conn_op.open_write_session = open_write_socket_session;
    conn->conn_op.open_read_session = open_read_socket_session;
    conn->conn_op.close_write_session = close_write_socket_session;
    conn->conn_op.close_read_session = close_read_socket_session;
  }
  conn->is_client= is_client;
  conn->is_mutex_used= is_mutex_used;
  conn->is_connect_thread_used= is_connect_thread_used;
  conn->tcp_maxseg_size= 0;
  conn->tcp_receive_buffer_size= 0;
  conn->tcp_send_buffer_size= 0;
  conn->is_wan_connection= FALSE;
  conn->backlog= 1;
  conn->server_ip= g_htonl(INADDR_ANY);
  conn->server_port= 0;
  conn->client_ip= g_htonl(INADDR_ANY);
  conn->client_port= 0;

  conn->conn_stat.used_server_ip= g_htonl(INADDR_ANY);
  conn->conn_stat.used_server_port= 0;
  conn->conn_stat.used_client_ip= g_htonl(INADDR_ANY);
  conn->conn_stat.used_client_port= 0;
  conn->conn_stat.is_client_used= is_client;
  conn->conn_stat.is_connected= FALSE;
  conn->conn_stat.is_connect_thread_active= FALSE;

  conn->conn_stat.used_tcp_maxseg_size= 0;
  conn->conn_stat.used_tcp_receive_buffer_size= 0;
  conn->conn_stat.used_tcp_send_buffer_size= 0;
  conn->conn_stat.tcp_no_delay= FALSE;

  conn->conn_stat.no_sent_buffers= (guint64)0;
  conn->conn_stat.no_sent_bytes= (guint64)0;
  conn->conn_stat.no_sent_bytes_square_sum= (long double)0;
  conn->conn_stat.no_rec_buffers= (guint64)0;
  conn->conn_stat.no_rec_bytes= (guint64)0;
  conn->conn_stat.no_rec_bytes_square_sum= (long double)0;
  for (i= 0; i < 16; i++)
    conn->conn_stat.no_sent_buf_range[i]= 
    conn->conn_stat.no_rec_buf_range[i]= 0;

  if (!((conn->connection_start= g_timer_new()) &&
      (conn->last_read_stat= g_timer_new())))
    return TRUE;
  g_timer_start(conn->last_read_stat);
  g_timer_start(conn->connection_start);
  return FALSE;
}
