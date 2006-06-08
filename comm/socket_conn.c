#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <ic_common.h>
#include <config.h>
#include <ic_comm.h>


static gboolean accept_socket_connection(struct ic_connection *conn)
{
  int error, addr_len;
  struct sockaddr_in client_address;

  addr_len= sizeof(client_address);
  if (accept(conn->sockfd, (struct sockaddr *)&client_address,
             &addr_len) < 0)
  {
    printf("accept error: %d %s\n", errno, sys_errlist[errno]);
    return TRUE;
  }
  return FALSE;
}


static int set_up_socket_connection(struct ic_connection *conn)
{
  int error, sockfd, addr_len;
  struct sockaddr_in client_address, server_address;

  memset(&server_address, sizeof(server_address), 0);
  memset(&client_address, sizeof(client_address), 0);

  server_address.sin_family= AF_INET;
  server_address.sin_addr.s_addr= g_htonl(conn->server_ip);
  server_address.sin_port= g_htons(conn->server_port);

  client_address.sin_family= AF_INET;
  client_address.sin_addr.s_addr= g_htonl(conn->client_ip);
  client_address.sin_port= g_htons(conn->client_port);

  /*
    Create a socket for the client connection
  */
  if ((sockfd= socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
  {
    return errno;
  }
  if (conn->is_client)
  { 
    /*
      Bind the socket to an IP address on this box.
    */
    if (bind(sockfd, (struct sockaddr *)&client_address,
             sizeof(client_address)) < 0)
    {
      printf("bind error\n");
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
  } else
  {
    /*
      Bind the socket to an IP address on this box.
    */
    if (bind(sockfd, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
      printf("bind error\n");
      goto error;
    }
    /*
      Listen for incoming connect messages
    */
    if ((listen(sockfd, conn->backlog) < 0))
    {
      printf("listen error\n");
      goto error;
    }
    if (conn->call_accept)
    {
      if (accept_socket_connection(conn))
        goto error;
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
    if (ret_code == buf_size)
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
      if (time_measure &&
          ((g_timer_elapsed(time_measure, NULL) + secs_expired) >
          next_sec_check) ||
          loop_count == 100)
      {
        next_sec_check+= (gdouble)1;
        secs_count++;
        if (secs_count >= secs_to_try)
          return EINTR;
        loop_count= 1;
      }
    }
    if (loop_count)
      g_usleep(10000); 
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
                                  void **buf, guint32 size)
{
  return 0;
}

static int open_write_socket_session(struct ic_connection *conn,
                                     guint32 total_size)
{
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
}

