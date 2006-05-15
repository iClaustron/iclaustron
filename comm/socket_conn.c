#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <common.h>
#include <config.h>
#include <ic_comm.h>

int set_up_socket_connection(struct ic_connection *conn)
{
  int error, sockfd;
  struct sockaddr_in client_address, server_address;

  if (conn->is_client)
  { 
    bzero(&server_address, sizeof(server_address));
    bzero(&client_address, sizeof(client_address));

    server_address.sin_family= AF_INET;
    server_address.sin_addr.s_addr= htonl(conn->server_ip);
    server_address.sin_port= htons(conn->server_port);

    client_address.sin_family= AF_INET;
    client_address.sin_addr.s_addr= htonl(conn->client_ip);
    client_address.sin_port= htons(conn->client_port);

    do
    {
      /*
        Create a socket for the client connection
      */
      if ((sockfd= socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
      {
        return errno;
      }
      /*
        Bind the socket to an IP address on this box.
      */
      if (bind(sockfd, (struct sockaddr *)&client_address,
               sizeof(client_address)) < 0)
      {
        error= errno;
        close(sockfd);
        return error;
      }
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
    bzero(&server_address, sizeof(server_address));
    bzero(&client_address, sizeof(client_address));

    server_address.sin_family= AF_INET;
    server_address.sin_addr.s_addr= htonl(conn->server_ip);
    server_address.sin_port= htons(conn->server_port);

    client_address.sin_family= AF_INET;
    client_address.sin_addr.s_addr= htonl(conn->client_ip);
    client_address.sin_port= htons(conn->client_port);

    /*
      Create a socket for the server connection
    */
    if ((sockfd= socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
    {
      return errno;
    }
    /*
      Bind the socket to an IP address on this box.
    */
    if (bind(sockfd, (struct sockaddr *)&client_address,
             sizeof(client_address)) < 0)
    {
      error= errno;
      close(sockfd);
      return error;
    }
    /*
      Listen for incoming connect messages
    */
    if (listen(sockfd, conn->backlog) < 0)
    {
      error= errno;
      close(sockfd);
      return error;
    }
    if (accept(sockfd, (struct sockaddr *)&server_address,
               sizeof(server_address) < 0))
    {
      error= errno;
      close(sockfd);
      return error;
    }
  }
  conn->sockfd= sockfd;
  return 0;
}

int write_socket_connection(struct ic_connection *conn,
                            const void *buf, guint32 size);
{
  return 0;
}

int read_socket_connection(struct ic_connection *conn,
                           void **buf, guint32 size)
{
  return 0;
}

int open_write_socket_connection(struct ic_connection *conn,
                                 guint32 total_size);
{
  return 0;
}

void set_socket_methods(struct ic_connection *conn)
{
  conn->conn_op->set_up_ic_connection= set_up_socket_connection;
  conn->conn_op->write_ic_connection = write_socket_connection,
  conn->conn_op->read_ic_connection = read_socket_connection,
  conn->conn_op->open_write_session = open_write_socket_session
}

