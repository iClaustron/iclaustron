#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <ic_common.h>
#include <config.h>
#include <ic_comm.h>


static int accept_socket_connection(struct ic_connection *conn)
{
  int error;
  socklen_t addr_len;
  struct sockaddr_in client_address;

  addr_len= sizeof(client_address);
  if (accept(conn->sockfd, (struct sockaddr *)&client_address,
             &addr_len) < 0)
  {
    DEBUG(printf("accept error: %d %s\n", errno, sys_errlist[errno]));
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
    DEBUG(printf("Client connected from a disallowed address\n"));
    error= 1;
    goto error;
  }
  return 0;

error:
  close(conn->sockfd);
  return error;
}


static int set_up_socket_connection(struct ic_connection *conn)
{
  int error, sockfd;
  struct sockaddr_in client_address, server_address;

  if (!g_thread_supported())
    g_thread_init(NULL);
  conn->read_mutex= g_mutex_new();
  conn->write_mutex= g_mutex_new();
  if (!(conn->read_mutex && conn->write_mutex))
  {
    /* Memory allocation failure */
    return 2;
  }

  if (conn->is_client)
  {
    memset(&client_address, sizeof(client_address), 0);
    client_address.sin_family= AF_INET;
    client_address.sin_addr.s_addr= g_htonl(conn->client_ip);
    client_address.sin_port= g_htons(conn->client_port);
  }
  else
  {
    memset(&server_address, sizeof(server_address), 0);
    server_address.sin_family= AF_INET;
    server_address.sin_addr.s_addr= g_htonl(conn->server_ip);
    server_address.sin_port= g_htons(conn->server_port);
  }

  /*
    Create a socket for the connection
  */
  if ((sockfd= socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
  {
    return errno;
  }
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
    /*
      Listen for incoming connect messages
    */
    if ((listen(sockfd, conn->backlog) < 0))
    {
      DEBUG(printf("listen error\n"));
      goto error;
    }
    if (conn->call_accept)
    {
      if ((error= accept_socket_connection(conn)))
        return error;
    }
  }
  conn->sockfd= sockfd;
  return 0;

error:
   error= errno;
   close(sockfd);
   return error;
}


static int write_socket_connection(struct ic_connection *conn,
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

static int close_socket_connection(struct ic_connection *conn)
{
  close(conn->sockfd);
  return 0;
}

static int read_socket_connection(struct ic_connection *conn,
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

static int open_write_socket_session(struct ic_connection *conn,
                                     guint32 total_size)
{
  g_mutex_lock(conn->write_mutex);
  return 0;
}

static int close_write_socket_session(struct ic_connection *conn)
{
  g_mutex_unlock(conn->write_mutex);
  return 0;
}

static int open_read_socket_session(struct ic_connection *conn)
{
  g_mutex_lock(conn->read_mutex);
  return 0;
}

static int close_read_socket_session(struct ic_connection *conn)
{
  g_mutex_unlock(conn->read_mutex);
  return 0;
}

void set_socket_methods(struct ic_connection *conn)
{
  conn->conn_op.set_up_ic_connection= set_up_socket_connection;
  conn->conn_op.accept_ic_connection= accept_socket_connection;
  conn->conn_op.close_ic_connection= close_socket_connection;
  conn->conn_op.write_ic_connection = write_socket_connection;
  conn->conn_op.read_ic_connection = read_socket_connection;
  conn->conn_op.open_write_session = open_write_socket_session;
  conn->conn_op.close_write_session = close_write_socket_session;
  conn->conn_op.open_read_session = open_read_socket_session;
  conn->conn_op.close_read_session = close_read_socket_session;
}

