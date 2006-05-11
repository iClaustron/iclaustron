#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <common.h>
#include <config.h>

int setup_client(in_addr_t server_ip, u_short server_port,
                 in_addr_t my_ip, u_short my_port)
{
  int error, sockfd;
  struct sockaddr_in my_address, server_address;
  gpointer a;
 
  bzero(&server_address, sizeof(server_address));
  bzero(&my_address, sizeof(my_address));

  a= g_slice_alloc((gsize)1);
  server_address.sin_family= AF_INET;
  server_address.sin_addr.s_addr= htonl(server_ip);
  server_address.sin_port= htons(server_port);

  my_address.sin_family= AF_INET;
  my_address.sin_addr.s_addr= htonl(my_ip);
  my_address.sin_port= htons(my_port);

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
    if (bind(sockfd, (struct sockaddr *)&my_address, sizeof(my_address)) < 0)
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
  return 0;
}

int setup_server(in_addr_t server_ip, u_short server_port,
                 in_addr_t client_ip, u_short client_port,
                 int backlog)
{
  int error, sockfd;
  struct sockaddr_in client_address, server_address;

  bzero(&server_address, sizeof(server_address));
  bzero(&client_address, sizeof(client_address));

  server_address.sin_family= AF_INET;
  server_address.sin_addr.s_addr= htonl(server_ip);
  server_address.sin_port= htons(server_port);

  client_address.sin_family= AF_INET;
  client_address.sin_addr.s_addr= htonl(client_ip);
  client_address.sin_port= htons(client_port);

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
  if (listen(sockfd, backlog) < 0)
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
  return 0;
}

