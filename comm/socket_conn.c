/* Copyright (C) 2007 iClaustron AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <ic_common.h>

#ifdef HAVE_SSL
static int ssl_create_connection(IC_SSL_CONNECTION *conn);
#endif
static void destroy_timers(IC_CONNECTION *conn);
static void destroy_mutexes(IC_CONNECTION *conn);

static int close_socket_connection(IC_CONNECTION *conn);

static void
lock_connect_mutex(IC_CONNECTION *conn)
{
  if (conn->is_mutex_used)
    g_mutex_lock(conn->connect_mutex);
}

static void
unlock_connect_mutex(IC_CONNECTION *conn)
{
  if (conn->is_mutex_used)
    g_mutex_unlock(conn->connect_mutex);
}

static void
set_is_connected(IC_CONNECTION *conn)
{
  lock_connect_mutex(conn);
  conn->conn_stat.is_connected= TRUE;
  unlock_connect_mutex(conn);
  return;
}

static gboolean
is_conn_connected(IC_CONNECTION *conn)
{
  gboolean is_connected;
  is_connected= conn->conn_stat.is_connected;
  return is_connected;
}

static gboolean
is_mutex_conn_connected(IC_CONNECTION *conn)
{
  gboolean is_connected;
  g_mutex_lock(conn->connect_mutex);
  is_connected= conn->conn_stat.is_connected;
  g_mutex_unlock(conn->connect_mutex);
  return is_connected;
}

static gboolean
is_conn_thread_active(IC_CONNECTION *conn)
{
  return conn->conn_stat.is_connect_thread_active;
}

static gboolean
is_mutex_conn_thread_active(IC_CONNECTION *conn)
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
ic_set_socket_options(IC_CONNECTION *conn, int sockfd)
{
  int no_delay, error, maxseg_size, rec_size, snd_size, reuse_addr;
  socklen_t sock_len;
#ifdef __APPLE__
  int no_sigpipe= 1;
  /*
    Mac OS X don't support MSG_NOSIGNAL but instead have a socket option
    SO_NO_SIGPIPE that gives the same functionality.
  */
  setsockopt(sockfd, IPPROTO_TCP, SO_NOSIGPIPE,
             (const void *)&no_sigpipe, sizeof(int));
#endif
  /*
    We start by setting TCP_NODELAY to ensure the OS does no
    extra delays, we want to be in control of when data is
    sent and received.
  */
  no_delay= 1;
  if ((error= setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
                         (const void*)&no_delay, sizeof(int))))
  {
    /* We will continue even with this error.  */
    no_delay= 0;
    DEBUG_PRINT(COMM_LEVEL, ("Set TCP_NODELAY error: %d\n", error));
  }
  reuse_addr= 1;
  if ((error= setsockopt(sockfd, IPPROTO_TCP, SO_REUSEADDR,
                         (const void*)&reuse_addr, sizeof(int))))
  {
    /* We will continue even with this error */
    DEBUG_PRINT(COMM_LEVEL, ("Set SO_REUSEADDR error: %d\n", error));
    reuse_addr= 0;
  }
  getsockopt(sockfd, IPPROTO_TCP, TCP_MAXSEG,
             (void*)&maxseg_size, &sock_len);
  getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF,
             (void*)&rec_size, &sock_len);
  getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF,
             (void*)&snd_size, &sock_len);
  DEBUG_PRINT(COMM_LEVEL, ("Default TCP_MAXSEG = %d\n", maxseg_size));
  DEBUG_PRINT(COMM_LEVEL, ("Default SO_RCVBUF = %d\n", rec_size));
  DEBUG_PRINT(COMM_LEVEL, ("Default SO_SNDBUF = %d\n", snd_size));

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
  DEBUG_PRINT(COMM_LEVEL, ("Used TCP_MAXSEG = %d\n", maxseg_size));
  DEBUG_PRINT(COMM_LEVEL, ("Used SO_RCVBUF = %d\n", rec_size));
  DEBUG_PRINT(COMM_LEVEL, ("Used SO_SNDBUF = %d\n", snd_size));
  DEBUG_PRINT(COMM_LEVEL, ("Used TCP_NODELAY = %d\n", no_delay));
  DEBUG_PRINT(COMM_LEVEL, ("Used SO_REUSEADDR = %d\n", reuse_addr));
  conn->conn_stat.used_tcp_maxseg_size= maxseg_size;
  conn->conn_stat.used_tcp_receive_buffer_size= rec_size;
  conn->conn_stat.used_tcp_send_buffer_size= snd_size;
  conn->conn_stat.tcp_no_delay= no_delay;
  return;
}

static int
ic_sock_ntop(const struct sockaddr *sa, gchar *ip_addr, int ip_addr_len)
{
  struct sockaddr_in *ipv4_addr= (struct sockaddr_in*)sa;
  struct sockaddr_in6 *ipv6_addr= (struct sockaddr_in6*)sa;
  guint32 port;
  gchar port_str[8];
  if (sa->sa_family == AF_INET)
  {
     if (inet_ntop(AF_INET, &ipv4_addr->sin_addr, ip_addr, ip_addr_len-8))
       return 1;
     port= ntohs(ipv4_addr->sin_port);
  }
  else if (sa->sa_family == AF_INET6)
  {
     if (inet_ntop(AF_INET6, &ipv6_addr->sin6_addr, ip_addr, ip_addr_len-8))
       return 1;
     port= ntohs(ipv6_addr->sin6_port);
  }
  else
    return 1;
  snprintf(port_str, sizeof(port_str), ":%d", port);
  strcat(ip_addr, port_str);
  return 0;
}

static int
security_handler_at_connect(IC_CONNECTION *conn)
{
  int error;
  gboolean save_ssl_used_for_data;
#ifdef HAVE_SSL
  IC_SSL_CONNECTION *ssl_conn= (IC_SSL_CONNECTION*)conn;
  if (conn->is_ssl_connection &&
      (error= ssl_create_connection(ssl_conn)))
  {
    conn->error_code= SSL_ERROR;
    goto error;
  }
#endif
  if (conn->auth_func)
  {
    save_ssl_used_for_data= conn->is_ssl_used_for_data;
    conn->is_ssl_used_for_data= TRUE;
    error= conn->auth_func(conn->auth_obj);
    conn->is_ssl_used_for_data= save_ssl_used_for_data;
    if (error)
    {
      /* error handler */
      return error;
    }
  }
  return 0;
error:
  return error;
}

static int
accept_socket_connection(IC_CONNECTION *conn)
{
  gboolean not_accepted= FALSE;
  int ret_sockfd, ok, error;
  socklen_t addr_len;
  struct sockaddr_storage client_address;
  const struct sockaddr *client_addr_ptr= 
        (const struct sockaddr*)&client_address;
  struct sockaddr_in *ipv4_client_address=
           (struct sockaddr_in*)&client_address;
  struct sockaddr_in6 *ipv6_client_address=
           (struct sockaddr_in6*)&client_address;
  struct sockaddr_in *ipv4_check_address;
  struct sockaddr_in6 *ipv6_check_address;

  /*
    The socket used to listen to a port can be reused many times.
    accept will return a new socket that can be used to read and
    write from.
  */
  addr_len= sizeof(client_address);
  DEBUG_PRINT(COMM_LEVEL, ("Accepting connections on server %s\n",
                           conn->conn_stat.server_ip_addr));
  ret_sockfd= accept(conn->listen_sockfd,
                     (struct sockaddr *)&client_address,
                     &addr_len);
  if (!conn->is_listen_socket_retained)
  {
    close(conn->listen_sockfd);
    conn->listen_sockfd= 0;
  }
  if (ret_sockfd < 0)
  {
    DEBUG_PRINT(COMM_LEVEL, ("accept error: %d %s\n",
                             errno, sys_errlist[errno]));
    conn->error_code= errno;
    return errno;
  }
  if (conn->client_name)
  {
    /*
       Check for connection from proper client is highly dependent on
       IP version used.
     */
    if (client_address.ss_family != conn->client_addrinfo->ai_family)
      return IC_ERROR_DIFFERENT_IP_VERSIONS;
    if (addr_len != conn->client_addrinfo->ai_addrlen)
      return IC_ERROR_DIFFERENT_IP_VERSIONS;
    if (client_address.ss_family == AF_INET)
    {
      ipv4_check_address= (struct sockaddr_in*)conn->client_addrinfo->ai_addr;
      if (ipv4_client_address->sin_addr.s_addr !=
          ipv4_check_address->sin_addr.s_addr)
        not_accepted= TRUE;
      if (conn->client_port_num != 0 &&
          ipv4_client_address->sin_port != ipv4_check_address->sin_port)
        not_accepted= TRUE;
    }
    else if (client_address.ss_family == AF_INET6)
    {
      ipv6_check_address= (struct sockaddr_in6*)conn->client_addrinfo->ai_addr;
      if (conn->client_port_num != 0 &&
          ipv6_client_address->sin6_port != ipv6_check_address->sin6_port)
        not_accepted= TRUE;
      if (memcmp(ipv6_client_address->sin6_addr.s6_addr,
                 ipv6_check_address->sin6_addr.s6_addr,
                 sizeof(ipv6_check_address->sin6_addr.s6_addr)))
        not_accepted= TRUE;
    }
    else
      not_accepted= TRUE;
  }
  /* Record a human-readable address for the client part of the connection */
  ok= ic_sock_ntop(client_addr_ptr,
                   conn->conn_stat.client_ip_addr,
                   sizeof(conn->conn_stat.client_ip_addr_str));
  if (not_accepted)
  {
    /*
      The caller only accepted connect's from certain IP address and
      port number. The accepted connection was not from the accepted
      IP address and port.
      We'll disconnect by closing the socket and report an error.
    */
    if (ok)
    {
      DEBUG_PRINT(COMM_LEVEL,
                  ("Client connect from a disallowed address, ip=%s\n",
                   conn->conn_stat.client_ip_addr));
    }
    conn->error_code= ACCEPT_ERROR;
    close(ret_sockfd);
    return ACCEPT_ERROR;
  }
  conn->rw_sockfd= ret_sockfd;
  conn->error_code= 0;
  if ((error= security_handler_at_connect(conn)))
    goto error;
  set_is_connected(conn);
  return 0;
error:
  close(ret_sockfd);
  return error;
}

static int
translate_hostnames(IC_CONNECTION *conn)
{
  struct addrinfo hints;
  int n;
  guint64 server_port= 0LL;
  guint64 client_port= 0LL;

  if (!conn->server_name)
    return IC_ERROR_NO_SERVER_NAME;
  if (!conn->server_port)
    return IC_ERROR_NO_SERVER_PORT;
  DEBUG_PRINT(COMM_LEVEL, ("Server port = %s\n", conn->server_port));
  if (ic_conv_str_to_int(conn->server_port, &server_port) ||
      server_port == 0LL || server_port > 65535)
    return IC_ERROR_ILLEGAL_SERVER_PORT;
  conn->server_port_num= (guint16)server_port;
  /* Get address information for Server part */
  bzero(&hints, sizeof(struct addrinfo));
  hints.ai_flags= AI_PASSIVE;
  hints.ai_family= PF_UNSPEC;
  hints.ai_socktype= SOCK_STREAM;
  DEBUG_PRINT(COMM_LEVEL, ("Server name = %s, service = %s\n",
              conn->server_name, conn->server_port));
  if ((n= getaddrinfo(conn->server_name, conn->server_port,
       &hints, &conn->server_addrinfo)) != 0)
    return IC_ERROR_GETADDRINFO;
  conn->ret_server_addrinfo= conn->server_addrinfo;
  /* Record a human-readable address for the server part of the connection */
  ic_sock_ntop(conn->server_addrinfo->ai_addr,
               conn->conn_stat.server_ip_addr,
               sizeof(conn->conn_stat.server_ip_addr_str));

  if (!conn->client_name)
  {
    /*
      Client: Connections from all clients and ports possible
      Server: No bind used since no client host provided
    */
    return 0;
  }
  if (!conn->client_port)
    conn->client_port= "0"; /* Use "0" if user provided no port */
  if (ic_conv_str_to_int(conn->client_port, &client_port) ||
      client_port > 65535)
    return IC_ERROR_ILLEGAL_CLIENT_PORT;
  conn->client_port_num= (guint16)client_port;
  /* Get address information for Client part */
  bzero(&hints, sizeof(struct addrinfo));
  hints.ai_flags= AI_CANONNAME;
  hints.ai_family= PF_UNSPEC;
  hints.ai_socktype= SOCK_STREAM;
  DEBUG_PRINT(COMM_LEVEL, ("Client name = %s, service = %s\n",
              conn->client_name, conn->client_port));
  if ((n= getaddrinfo(conn->client_name, conn->client_port,
       &hints, &conn->client_addrinfo)) != 0)
    return IC_ERROR_GETADDRINFO;
  conn->ret_client_addrinfo= conn->client_addrinfo;
  for (; conn->client_addrinfo;
       conn->client_addrinfo= conn->client_addrinfo->ai_next)
  {
    /*
       We search for first entry that uses the same IP version as the
       server address we supplied will use
    */
    if (conn->server_addrinfo->ai_family == conn->client_addrinfo->ai_family)
      break;
  }
  if (!conn->client_addrinfo)
    return IC_ERROR_DIFFERENT_IP_VERSIONS;
  if (conn->is_client)
  {
    /* Record a human-readable address for the client part of the connection */
    ic_sock_ntop(conn->client_addrinfo->ai_addr,
                 conn->conn_stat.client_ip_addr,
                 sizeof(conn->conn_stat.client_ip_addr_str));
  }
  return 0;
}

static int
int_set_up_socket_connection(IC_CONNECTION *conn)
{
  int error, sockfd;
  struct addrinfo *loc_addrinfo;

  DEBUG_PRINT(COMM_LEVEL, ("Translating hostnames\n"));
  if ((error= translate_hostnames(conn)))
  {
    DEBUG_PRINT(COMM_LEVEL,
      ("Translating hostnames failed error = %d\n", error));
    conn->error_code= error;
    return conn->error_code;
  }
  DEBUG_PRINT(COMM_LEVEL, ("Translating hostnames done\n"));

  loc_addrinfo= conn->is_client ?
      conn->client_addrinfo : conn->server_addrinfo;
  /*
    Create a socket for the connection
  */
  if ((sockfd= socket(conn->server_addrinfo->ai_family,
                      conn->server_addrinfo->ai_socktype,
                      conn->server_addrinfo->ai_protocol)) < 0)
  {
    conn->error_code= errno;
    return errno;
  }
  ic_set_socket_options(conn, sockfd);
  if (((conn->is_client == FALSE) ||
       (conn->client_name != NULL)) &&
       (bind(sockfd, (struct sockaddr *)loc_addrinfo->ai_addr,
             loc_addrinfo->ai_addrlen) < 0))
  {
    /*
      Bind the socket to an IP address and port on this box.  If the caller
      set port to "0" for a client connection an ephemeral port will be used.
    */
    DEBUG_PRINT(COMM_LEVEL, ("bind error: %d %s\n",
                 errno, sys_errlist[errno]));
    goto error;
  }
  if (conn->is_client)
  { 
    do
    {
      /*
        Connect the client to the server, will block until connected or
        timeout occurs.
      */
      if (connect(sockfd, (struct sockaddr*)conn->server_addrinfo->ai_addr,
                  conn->server_addrinfo->ai_addrlen) < 0)
      {
        if (errno == EINTR || errno == ECONNREFUSED)
          continue;
        DEBUG_PRINT(COMM_LEVEL, ("connect error: %d %s\n",
                     errno, sys_errlist[errno]));
        goto error;
      }
      conn->rw_sockfd= sockfd;
      if ((error= security_handler_at_connect(conn)))
        goto error2;
      set_is_connected(conn);
      break;
    } while (1);
  }
  else
  {
    /*
      Listen for incoming connect messages
    */
    if ((listen(sockfd, conn->backlog) < 0))
    {
      DEBUG_PRINT(COMM_LEVEL, ("listen error: %d %s\n",
                  errno, sys_errlist[errno]));
      goto error;
    }
    conn->listen_sockfd= sockfd;
    if (!conn->is_listen_socket_retained)
      return accept_socket_connection(conn);
    else
      conn->listen_sockfd= sockfd;
  }
  conn->error_code= 0;
  return 0;

error:
   error= errno;
error2:
   conn->error_code= error;
   close(sockfd);
   return error;
}

static gpointer
run_set_up_socket_connection(gpointer data)
{
  int ret_code;
  IC_CONNECTION *conn= (IC_CONNECTION*)data;
  ret_code= int_set_up_socket_connection(conn);
  lock_connect_mutex(conn);
  conn->conn_stat.is_connect_thread_active= FALSE;
  unlock_connect_mutex(conn);
  return NULL;
}

static int
set_up_socket_connection(IC_CONNECTION *conn)
{
  GError *error= NULL;
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
    conn->error_code= 1;
    return 1; /* Should write proper error handling code here */
  }
  return 0;
}

static int
flush_socket_front_buffer(__attribute__ ((unused)) IC_CONNECTION *conn)
{
  return 0;
}

#ifdef HAVE_SSL
static int
ic_ssl_write(IC_CONNECTION *conn, const void *buf, guint32 buf_size)
{
  IC_SSL_CONNECTION *ssl_conn= (IC_SSL_CONNECTION*)conn;
  int ret_code= SSL_write(ssl_conn->ssl_conn, buf, buf_size);
  int error;

  switch ((error= SSL_get_error(ssl_conn->ssl_conn, ret_code)))
  {
    case SSL_ERROR_NONE:
      /* Data successfully written */
      return ret_code;
    case SSL_ERROR_ZERO_RETURN:
      /* SSL connection closed */
      return 0;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return -EINTR;
    default:
      DEBUG_PRINT(COMM_LEVEL, ("SSL error %d", error));
      return 0;
  }
  return 0;
}
#endif

static int
write_socket_connection(IC_CONNECTION *conn,
                        const void *buf, guint32 size,
                        __attribute__ ((unused)) guint32 prio_level,
                        guint32 secs_to_try)
{
  GTimer *time_measure= 0;
  gdouble secs_expired= 0;
  gdouble next_sec_check= 0;
  guint32 write_size= 0;
  guint32 loop_count= 0;
  guint32 secs_count= 0;

  do
  {
    gssize ret_code;
    gsize buf_size= size - write_size;

#ifdef HAVE_SSL
    if (conn->is_ssl_used_for_data)
      ret_code= ic_ssl_write(conn, buf + write_size, buf_size);
    else
      ret_code= send(conn->rw_sockfd, buf + write_size, buf_size,
                     IC_MSG_NOSIGNAL);
#else
    ret_code= send(conn->rw_sockfd, buf + write_size, buf_size,
                   IC_MSG_NOSIGNAL);
#endif
    if (ret_code == (int)buf_size)
    {
      unsigned i;
      guint32 num_sent_buf_range;
      guint64 num_sent_bufs= conn->conn_stat.num_sent_buffers;
      guint64 num_sent_bytes= conn->conn_stat.num_sent_bytes;
      guint64 num_sent_square= conn->conn_stat.num_sent_bytes_square_sum;

      i= ic_count_highest_bit(buf_size | 32);
      i-= 6;
      num_sent_buf_range= conn->conn_stat.num_sent_buf_range[i];
      conn->error_code= 0;
      conn->conn_stat.num_sent_buffers= num_sent_bufs + 1;
      conn->conn_stat.num_sent_bytes= num_sent_bytes + size;
      conn->conn_stat.num_sent_bytes_square_sum=
                               num_sent_square + (size*size);
      conn->conn_stat.num_sent_buf_range[i]= num_sent_buf_range + 1;
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
      int error;
      if (!conn->is_ssl_used_for_data)
        error= errno;
      else
        error= (-1) * ret_code;
      if (error == EINTR)
        continue;
      conn->conn_stat.num_send_errors++;
      conn->bytes_written_before_interrupt= write_size;
      DEBUG_PRINT(COMM_LEVEL, ("write error: %d %s\n",
                               errno, sys_errlist[errno]));
      conn->error_code= error;
      return error;
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
        {
          conn->conn_stat.num_send_timeouts++;
          conn->bytes_written_before_interrupt= write_size;
          conn->error_code= EINTR;
          DEBUG_PRINT(COMM_LEVEL, ("timeout error on write\n"));
          return EINTR;
        }
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
write_socket_front_buffer(IC_CONNECTION *conn,
                          const void *buf, guint32 size,
                          guint32 prio_level,
                          guint32 secs_to_try)
{
  return write_socket_connection(conn, buf, size, prio_level,
                                 secs_to_try);
}

static int
close_socket_connection(IC_CONNECTION *conn)
{
  lock_connect_mutex(conn);
  if (conn->listen_sockfd)
  {
    close(conn->listen_sockfd);
    conn->listen_sockfd= 0;
  }
  if (conn->rw_sockfd)
  {
    close(conn->rw_sockfd);
    conn->rw_sockfd= 0;
  }
  conn->error_code= 0;
  unlock_connect_mutex(conn);
  return 0;
}

static int
close_listen_socket_connection(IC_CONNECTION *conn)
{
  lock_connect_mutex(conn);
  if (conn->listen_sockfd)
  {
    close(conn->listen_sockfd);
    conn->listen_sockfd= 0;
  }
  conn->error_code= 0;
  unlock_connect_mutex(conn);
  return 0;
}

#ifdef HAVE_SSL
static int
ic_ssl_read(IC_CONNECTION *conn,
            void *buf, guint32 buf_size,
            guint32 *read_size)
{
  IC_SSL_CONNECTION *ssl_conn= (IC_SSL_CONNECTION*)conn;
  int ret_code= SSL_read(ssl_conn->ssl_conn, buf, buf_size);
  int error;
  printf("Wrote %d bytes through SSL\n", buf_size);

  switch ((error= SSL_get_error(ssl_conn->ssl_conn, ret_code)))
  {
    case SSL_ERROR_NONE:
      /* Data successfully read */
      *read_size= (guint32)ret_code;
      return ret_code;
    case SSL_ERROR_ZERO_RETURN:
      /* SSL connection closed */
      return 0;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return -EINTR;
    default:
      DEBUG_PRINT(COMM_LEVEL, ("SSL error %d", error));
      return 0;
  }
  return 0;
}
#endif

static int
read_socket_connection(IC_CONNECTION *conn,
                       void *buf, guint32 buf_size,
                       guint32 *read_size)
{
  int ret_code, error;
  do
  {
#ifdef HAVE_SSL
    if (conn->is_ssl_used_for_data)
      ret_code= ic_ssl_read(conn, buf, buf_size, read_size);
    else
      ret_code= recv(conn->rw_sockfd, buf, buf_size, 0);
#else
    ret_code= recv(conn->rw_sockfd, buf, buf_size, 0);
#endif
    if (ret_code > 0)
    {
      unsigned i;
      int range_limit;
      guint32 num_rec_buf_range;
      guint64 num_rec_bufs= conn->conn_stat.num_rec_buffers;
      guint64 num_rec_bytes= conn->conn_stat.num_rec_bytes;
      guint64 num_rec_square= conn->conn_stat.num_rec_bytes_square_sum;
      
      *read_size= ret_code;
      range_limit= 32;
      for (i= 0; i < 15; i++)
      {
        if (ret_code < range_limit)
          break;
        range_limit<<= 1;
      }
      num_rec_buf_range= conn->conn_stat.num_rec_buf_range[i];
      conn->error_code= 0;
      conn->conn_stat.num_rec_buffers= num_rec_bufs + 1;
      conn->conn_stat.num_rec_bytes= num_rec_bytes + ret_code;
      conn->conn_stat.num_rec_bytes_square_sum=
                             num_rec_square + (ret_code*ret_code);
      conn->conn_stat.num_rec_buf_range[i]= num_rec_buf_range + 1;
      return 0;
    }
    if (ret_code == 0)
    {
      DEBUG_PRINT(COMM_LEVEL, ("End of file received\n"));
      conn->error_code= END_OF_FILE;
      return END_OF_FILE;
    }
    if (!conn->is_ssl_used_for_data)
      error= errno;
    else
      error= (-1) * ret_code;
  } while (error == EINTR);
  conn->conn_stat.num_rec_errors++;
  DEBUG_PRINT(COMM_LEVEL, ("read error: %d %s\n",
                            errno, sys_errlist[errno]));
  conn->error_code= error;
  return error;
}

static int
open_write_socket_session_mutex(IC_CONNECTION *conn,
                                __attribute__ ((unused)) guint32 total_size)
{
  g_mutex_lock(conn->write_mutex);
  return 0;
}

static int
close_write_socket_session_mutex(IC_CONNECTION *conn)
{
  g_mutex_unlock(conn->write_mutex);
  return 0;
}

static int
open_read_socket_session_mutex(IC_CONNECTION *conn)
{
  g_mutex_lock(conn->read_mutex);
  return 0; 
}

static int
close_read_socket_session_mutex(IC_CONNECTION *conn)
{
  g_mutex_unlock(conn->read_mutex);
  return 0;
}

static int
no_op_socket_method(__attribute__ ((unused)) IC_CONNECTION *conn)
{
  return 0;
}

static int
no_op_with_size_socket_method(__attribute__ ((unused)) IC_CONNECTION *conn,
                              __attribute__ ((unused)) guint32 total_size)
{
  return 0;
}

static void
free_socket_connection(IC_CONNECTION *conn)
{
  if (!conn)
    return;
  close_socket_connection(conn);
  if (conn->client_addrinfo)
    freeaddrinfo(conn->ret_client_addrinfo);
  if (conn->server_addrinfo)
    freeaddrinfo(conn->ret_server_addrinfo);
  destroy_timers(conn);
  destroy_mutexes(conn);
  ic_free(conn);
}

static void
read_stat_socket_connection(IC_CONNECTION *conn,
                            IC_CONNECTION *conn_stat,
                            gboolean clear_stat_timer)
{
  memcpy((void*)conn_stat, (void*)conn,
         sizeof(IC_CONNECTION));
  if (clear_stat_timer)
    g_timer_reset(conn->last_read_stat);
}

static void
safe_read_stat_socket_conn(IC_CONNECTION *conn,
                           IC_CONNECTION *conn_stat,
                           gboolean clear_stat_timer)
{
  g_mutex_lock(conn->connect_mutex);
  g_mutex_lock(conn->write_mutex);
  g_mutex_lock(conn->read_mutex);
  read_stat_socket_connection(conn, conn_stat, clear_stat_timer);
  g_mutex_unlock(conn->read_mutex);
  g_mutex_unlock(conn->write_mutex);
  g_mutex_unlock(conn->connect_mutex);
}

static double
read_socket_connection_time(IC_CONNECTION *conn,
                            gulong *microseconds)
{
  return g_timer_elapsed(conn->connection_start, microseconds);
}

static double
read_socket_stat_time(IC_CONNECTION *conn,
                      gulong *microseconds)
{
  return g_timer_elapsed(conn->last_read_stat, microseconds);
}

static void
write_stat_socket_connection(IC_CONNECTION *conn)
{
  printf("Number of sent buffers = %u, Number of sent bytes = %u\n",
         (guint32)conn->conn_stat.num_sent_buffers,
         (guint32)conn->conn_stat.num_sent_bytes);
  printf("Number of rec buffers = %u, Number of rec bytes = %u\n",
         (guint32)conn->conn_stat.num_rec_buffers,
         (guint32)conn->conn_stat.num_rec_bytes);
  printf("Number of send errors = %u, Number of send timeouts = %u\n",
        (guint32)conn->conn_stat.num_send_errors,
        (guint32)conn->conn_stat.num_send_timeouts);
  printf("Number of rec errors = %u\n",
        (guint32)conn->conn_stat.num_rec_errors);
}

static void
destroy_timers(IC_CONNECTION *conn)
{
  if (conn->connection_start)
    g_timer_destroy(conn->connection_start);
  if (conn->last_read_stat)
    g_timer_destroy(conn->last_read_stat);
  conn->connection_start= NULL;
  conn->last_read_stat= NULL;
}

static void
destroy_mutexes(IC_CONNECTION *conn)
{
  if (conn->read_mutex)
    g_mutex_free(conn->read_mutex);
  if (conn->write_mutex)
    g_mutex_free(conn->write_mutex);
  if (conn->connect_mutex)
    g_mutex_free(conn->connect_mutex);
  conn->read_mutex= NULL;
  conn->write_mutex= NULL;
  conn->connect_mutex= NULL;
}

static gboolean
create_mutexes(IC_CONNECTION *conn)
{
  if (!((conn->read_mutex= g_mutex_new()) &&
        (conn->write_mutex= g_mutex_new()) &&
        (conn->connect_mutex= g_mutex_new())))
  {
    destroy_mutexes(conn);
    return TRUE;
  }
  return FALSE;
}

static gboolean
create_timers(IC_CONNECTION *conn)
{
  if (!((conn->connection_start= g_timer_new()) &&
        (conn->last_read_stat= g_timer_new())))
  {
    destroy_timers(conn);
    return TRUE;
  }
  g_timer_start(conn->last_read_stat);
  g_timer_start(conn->connection_start);
  return FALSE;
}

static void
set_up_methods_front_buffer(IC_CONNECTION *conn)
{
  if (conn->is_using_front_buffer)
  {
    conn->conn_op.ic_write_connection= write_socket_front_buffer;
    conn->conn_op.ic_flush_connection= flush_socket_front_buffer;
  }
  else
  {
    conn->conn_op.ic_write_connection= write_socket_connection;
    conn->conn_op.ic_flush_connection= no_op_socket_method;
  }
}

static void
set_up_rw_methods_mutex(IC_CONNECTION *conn, gboolean is_mutex_used)
{
  if (is_mutex_used)
  {
    conn->conn_op.ic_open_write_session= open_write_socket_session_mutex;
    conn->conn_op.ic_close_write_session= close_write_socket_session_mutex;
    conn->conn_op.ic_open_read_session= open_read_socket_session_mutex;
    conn->conn_op.ic_close_read_session= close_read_socket_session_mutex;
    conn->conn_op.ic_is_conn_thread_active= is_mutex_conn_thread_active;
    conn->conn_op.ic_is_conn_connected= is_mutex_conn_connected;
  }
  else
  {
    conn->conn_op.ic_open_write_session= no_op_with_size_socket_method;
    conn->conn_op.ic_open_read_session= no_op_socket_method;
    conn->conn_op.ic_close_write_session= no_op_socket_method;
    conn->conn_op.ic_close_read_session= no_op_socket_method;
    conn->conn_op.ic_is_conn_thread_active= is_conn_thread_active;
    conn->conn_op.ic_is_conn_connected= is_conn_connected;
  }
}

static void
init_connect_stat(IC_CONNECTION *conn)
{
  guint32 i;

  conn->conn_stat.is_connected= FALSE;
  conn->conn_stat.is_connect_thread_active= FALSE;

  conn->conn_stat.server_ip_addr_str[0]= 0;
  conn->conn_stat.client_ip_addr_str[0]= 0;
  conn->conn_stat.server_ip_addr= conn->conn_stat.server_ip_addr_str;
  conn->conn_stat.client_ip_addr= conn->conn_stat.client_ip_addr_str;

  conn->conn_stat.used_tcp_maxseg_size= 0;
  conn->conn_stat.used_tcp_receive_buffer_size= 0;
  conn->conn_stat.used_tcp_send_buffer_size= 0;
  conn->conn_stat.tcp_no_delay= FALSE;

  conn->conn_stat.num_sent_buffers= (guint64)0;
  conn->conn_stat.num_sent_bytes= (guint64)0;
  conn->conn_stat.num_sent_bytes_square_sum= (long double)0;
  conn->conn_stat.num_send_errors= 0;
  conn->conn_stat.num_send_timeouts= 0;
  conn->conn_stat.num_rec_errors= 0;
  conn->conn_stat.num_rec_buffers= (guint64)0;
  conn->conn_stat.num_rec_bytes= (guint64)0;
  conn->conn_stat.num_rec_bytes_square_sum= (long double)0;

  for (i= 0; i < 16; i++)
  {
    conn->conn_stat.num_sent_buf_range[i]= 
    conn->conn_stat.num_rec_buf_range[i]= 0;
  }
}


/*
  In the case of a server connection we have accepted a connection, we want
  to continue to listen to the original connection. Thus we fork a new
  connection object by copying the original one, resetting statistics,
  setting some specific variables on the forked object and resetting the
  original object to not connected.
*/

IC_CONNECTION*
fork_accept_connection(IC_CONNECTION *orig_conn,
                       gboolean use_mutex,
                       gboolean use_front_buffer)
{
  IC_CONNECTION *fork_conn;
  int size_object= orig_conn->is_ssl_connection ?
                   sizeof(IC_SSL_CONNECTION) : sizeof(IC_CONNECTION);
  if ((fork_conn= (IC_CONNECTION*)ic_calloc(size_object)) == NULL)
    return NULL;
  memcpy(fork_conn, orig_conn, size_object);

  init_connect_stat(fork_conn);
  fork_conn->orig_conn= orig_conn;
  fork_conn->forked_connections= 0;
  fork_conn->read_mutex= NULL;
  fork_conn->write_mutex= NULL;
  fork_conn->connect_mutex= NULL;
  fork_conn->connection_start= NULL;
  fork_conn->last_read_stat= NULL;
  fork_conn->first_con_buf= NULL;
  fork_conn->last_con_buf= NULL;
  fork_conn->listen_sockfd= 0;
  fork_conn->bytes_written_before_interrupt= 0;
  fork_conn->error_code= 0;
  fork_conn->is_listen_socket_retained= FALSE;
  fork_conn->is_mutex_used= FALSE;
  fork_conn->is_connect_thread_used= FALSE;
  fork_conn->is_mutex_used= use_mutex;
  fork_conn->is_using_front_buffer= use_front_buffer;
  orig_conn->forked_connections++;

  set_up_methods_front_buffer(fork_conn);
  set_up_rw_methods_mutex(fork_conn, fork_conn->is_mutex_used);

  if (create_mutexes(fork_conn))
    goto error;
  if (create_timers(fork_conn))
    goto error;
  return fork_conn;

error:
  destroy_mutexes(fork_conn);
  ic_free(fork_conn);
  return NULL;
}


IC_CONNECTION*
int_create_socket_object(gboolean is_client,
                         gboolean is_mutex_used,
                         gboolean is_connect_thread_used,
                         gboolean is_using_front_buffer,
                         gboolean is_ssl,
                         gboolean is_ssl_used_for_data,
                         authenticate_func auth_func,
                         void *auth_obj)
{
  int size_object= is_ssl ? sizeof(IC_SSL_CONNECTION) : sizeof(IC_CONNECTION);
  IC_CONNECTION *conn;

  if ((conn= (IC_CONNECTION*)ic_calloc(size_object)) == NULL)
      return NULL;

  conn->conn_op.ic_set_up_connection= set_up_socket_connection;
  conn->conn_op.ic_accept_connection= accept_socket_connection;
  conn->conn_op.ic_close_connection= close_socket_connection;
  conn->conn_op.ic_close_listen_connection= close_listen_socket_connection;
  conn->conn_op.ic_read_connection = read_socket_connection;
  conn->conn_op.ic_free_connection= free_socket_connection;
  conn->conn_op.ic_read_stat_connection= read_stat_socket_connection;
  conn->conn_op.ic_safe_read_stat_connection= safe_read_stat_socket_conn;
  conn->conn_op.ic_read_connection_time= read_socket_connection_time;
  conn->conn_op.ic_read_stat_time= read_socket_stat_time;
  conn->conn_op.ic_write_stat_connection= write_stat_socket_connection;
  conn->conn_op.ic_fork_accept_connection= fork_accept_connection;

  conn->is_ssl_connection= is_ssl;
  conn->is_ssl_used_for_data= is_ssl_used_for_data;
  conn->is_client= is_client;
  conn->is_connect_thread_used= is_connect_thread_used;
  conn->backlog= 1;
  conn->is_using_front_buffer= is_using_front_buffer;
  conn->is_mutex_used= is_mutex_used;

  set_up_rw_methods_mutex(conn, is_mutex_used);
  set_up_methods_front_buffer(conn);

  init_connect_stat(conn);

  conn->auth_func= auth_func;
  conn->auth_obj= auth_obj;

  if (create_mutexes(conn))
    goto error;
  if (create_timers(conn))
    goto error;
  return conn;
error:
  if (conn)
    destroy_mutexes(conn);
  ic_free(conn);
  return NULL;
}

IC_CONNECTION*
ic_create_socket_object(gboolean is_client,
                        gboolean is_mutex_used,
                        gboolean is_connect_thread_used,
                        gboolean is_using_front_buffer,
                        authenticate_func auth_func,
                        void *auth_obj)
{
  return int_create_socket_object(is_client, is_mutex_used,
                                  is_connect_thread_used,
                                  is_using_front_buffer,
                                  FALSE, FALSE,
                                  auth_func, auth_obj);
}

#ifdef HAVE_SSL
/*
  Module: SSL
  -----------
    This module implements sockets using the SSL protocol. It will use
    the same interface except for when a connection is created. In the
    back-end methods there are lots of new methods although many of the
    old methods are reused also for SSL connections.
*/
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <openssl/dh.h>

static unsigned char dh_512_prime[]=
{
  0xc2, 0x38, 0x44, 0x9a, 0x7f, 0x9c, 0x22, 0xc9,
  0x9d, 0x86, 0xe0, 0x2c, 0x3a, 0xf0, 0x70, 0x6a,
  0xed, 0xb1, 0x74, 0x9a, 0x8e, 0x43, 0x9b, 0x86,
  0xa9, 0x98, 0x1e, 0x37, 0x9d, 0x0d, 0x8a, 0x41,
  0xc6, 0x26, 0x8e, 0x73, 0xe4, 0x67, 0x33, 0x36,
  0xb1, 0x0f, 0xbe, 0x48, 0x83, 0xbd, 0x03, 0xb2,
  0x05, 0x6d, 0xf5, 0x2f, 0x1f, 0x49, 0x71, 0x0f,
  0x77, 0x02, 0x93, 0x41, 0xb5, 0x1a, 0x76, 0xe3,
};

static unsigned char dh_1024_prime[]=
{
  0xdd, 0x0c, 0x20, 0x0b, 0xbb, 0x10, 0xc7, 0x4a,
  0x9b, 0x1c, 0x0b, 0xa1, 0x7f, 0x09, 0xdc, 0x4e,
  0x5e, 0xec, 0x15, 0x12, 0xc9, 0xcf, 0x3e, 0xdf,
  0xde, 0x4d, 0x21, 0xc5, 0x72, 0x27, 0x9e, 0xc2,
  0x80, 0x1d, 0x22, 0x33, 0xb3, 0x6e, 0x2d, 0x50,
  0xb0, 0x00, 0x01, 0x3b, 0x3e, 0x70, 0xe9, 0x6e,
  0x00, 0x59, 0xdd, 0xfe, 0x39, 0x39, 0x61, 0x57,
  0x14, 0xfa, 0xfa, 0xb9, 0x7e, 0x1a, 0xc2, 0x2b,
  0x9c, 0x0f, 0xb7, 0x8f, 0x06, 0x33, 0x1d, 0x08,
  0x13, 0xdf, 0x1f, 0x78, 0xfe, 0x2c, 0x68, 0xc6,
  0xaf, 0x52, 0x1a, 0xac, 0x5b, 0xcc, 0x13, 0x31,
  0x28, 0x55, 0x9c, 0x2d, 0x50, 0x53, 0xe2, 0xee,
  0x41, 0x6d, 0x89, 0x89, 0xb2, 0x88, 0x4f, 0xc2,
  0xe4, 0x6c, 0x8b, 0x79, 0x23, 0xa4, 0x7f, 0x2d,
  0x2f, 0xf1, 0xfa, 0x93, 0x8f, 0xce, 0x07, 0xea,
  0x3f, 0x27, 0x98, 0x68, 0x15, 0x38, 0xe2, 0xcb,
};

static DH *dh_512= NULL;
static DH *dh_1024= NULL;

int
ic_ssl_init()
{
  unsigned char dh_512_group[1]= {0x5};
  unsigned char dh_1024_group[1]= {0x5};

  if (!SSL_library_init())
    return 1;
  if (!((dh_512= DH_new()) &&
      (dh_1024= DH_new())))
    return 1;
  /*
    Check if BIN_bin2bn allocates memory, if so we need to deallocate it in
    the end routine.
  */
  if (!((dh_512->p= BN_bin2bn(dh_512_prime, sizeof(dh_512_prime), NULL)) &&
        (dh_1024->p= BN_bin2bn(dh_1024_prime, sizeof(dh_1024_prime), NULL)) &&
        (dh_512->g= BN_bin2bn(dh_512_group, 1, NULL)) &&
        (dh_1024->g= BN_bin2bn(dh_1024_group, 1, NULL))))
    return 1;
  /* Provide random entropy */
  if (!RAND_load_file("/dev/random", 1024))
    return 1;
  return 0;
}

void ic_ssl_end()
{
  if (dh_512)
    DH_free(dh_512);
  if (dh_1024)
    DH_free(dh_1024);
}

static void
free_ssl_session(IC_SSL_CONNECTION *conn)
{
  if (conn->ssl_conn)
  {
    SSL_shutdown(conn->ssl_conn);
    SSL_free(conn->ssl_conn);
  }
  if (conn->ssl_ctx)
    SSL_CTX_free(conn->ssl_ctx);
  if (conn->ssl_dh)
    DH_free(conn->ssl_dh);
  if (conn->root_certificate_path.str)
    ic_free(conn->root_certificate_path.str);
  if (conn->loc_certificate_path.str)
    ic_free(conn->loc_certificate_path.str);
  if (conn->passwd_string.str)
    ic_free(conn->passwd_string.str);
  conn->ssl_ctx= NULL;
  conn->ssl_conn= NULL;
  conn->ssl_dh= NULL;
  IC_INIT_STRING(&conn->root_certificate_path, NULL, 0, FALSE);
  IC_INIT_STRING(&conn->loc_certificate_path, NULL, 0, FALSE);
  IC_INIT_STRING(&conn->passwd_string, NULL, 0, FALSE);
}

static int
ic_ssl_verify_callback(int ok, X509_STORE_CTX *ctx_store)
{
  gchar buf[512];
  printf("verify callback %d\n", ok);
  if (ok == 0)
  {
    X509 *cert= X509_STORE_CTX_get_current_cert(ctx_store);
    int depth= X509_STORE_CTX_get_error_depth(ctx_store);
    int error= X509_STORE_CTX_get_error(ctx_store);

    printf("Error with certificate at depth %d\n", depth);
    X509_NAME_oneline(X509_get_issuer_name(cert), buf, 512);
    printf("Issuer = %s\n", buf);
    X509_NAME_oneline(X509_get_subject_name(cert), buf, 512);
    printf("Subject = %s\n", buf);
    printf("Error %d, %s\n", error, X509_verify_cert_error_string(error));
  }
  return ok;
}

static DH*
ssl_get_dh_callback(__attribute__ ((unused)) SSL *ssl,
                    __attribute__ ((unused)) int is_export,
                    int key_length)
{
  if (key_length == 512)
    return dh_512;
  else
    return dh_1024;
}

/*
  This method is used after a successful set-up of a TCP/IP connection. It is
  used both on client and server side. It creates an SSL context whereafter it
  creates and SSL connection and uses this to perform the SSL related parts of
  the connection set-up.
*/
static int
ssl_create_connection(IC_SSL_CONNECTION *conn)
{
  IC_STRING *loc_cert_file;
  gchar buf [256];
  gchar *buf_ptr;
  int error;
  IC_CONNECTION *sock_conn= (IC_CONNECTION*)conn;

  if (!(conn->ssl_ctx= SSL_CTX_new(SSLv3_method())))
    goto error_handler;
  /*
    Set-up password to use for this SSL session object. This password is the
    private key which the SSL certificates have been signed with.
  */
  loc_cert_file= &conn->loc_certificate_path;
  buf_ptr= ic_get_ic_string(&conn->passwd_string, buf);
  SSL_CTX_set_default_passwd_cb_userdata(conn->ssl_ctx, (void*)buf_ptr);
  /*
    We use a local certificate chain file, normally either server.pem or
    client.pem.
    This file is also the file where the private key is stored.
    We specify where the root certificate is stored. Then we specify that both
    server and client need to have a certificate and we specify that we are
    going to use SHA1 of the greatest strength as the cipher for the
    encryption of the data channel. Finally we load the Diffie-Hellman keys
    used for key management, this is only performed by the server part of the
    connection.
  */
  if ((error= SSL_CTX_use_certificate_chain_file(conn->ssl_ctx,
                                                 loc_cert_file->str) !=
                                                 IC_SSL_SUCCESS))
    goto error_handler;
  if ((error= SSL_CTX_use_PrivateKey_file(conn->ssl_ctx,
                                          loc_cert_file->str,
                                          SSL_FILETYPE_PEM) != IC_SSL_SUCCESS))
    goto error_handler;
  if ((error= SSL_CTX_load_verify_locations(conn->ssl_ctx,
                                            conn->root_certificate_path.str,
                                            NULL) != IC_SSL_SUCCESS))
    goto error_handler;
  if ((error= SSL_CTX_set_default_verify_paths(conn->ssl_ctx)
               != IC_SSL_SUCCESS))
    goto error_handler;
  SSL_CTX_set_verify(conn->ssl_ctx,
                     SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     ic_ssl_verify_callback);
  SSL_CTX_set_verify_depth(conn->ssl_ctx, 3);
  if ((error= SSL_CTX_set_cipher_list(conn->ssl_ctx, "SHA1:@STRENGTH")) !=
              IC_SSL_SUCCESS)
    goto error_handler;
  if (sock_conn->is_client)
    SSL_CTX_set_tmp_dh_callback(conn->ssl_ctx, ssl_get_dh_callback);
  if (!(conn->ssl_conn= SSL_new(conn->ssl_ctx)))
    goto error_handler;
  printf("Successfully created a new SSL session\n");
  if (!SSL_set_fd(conn->ssl_conn, sock_conn->rw_sockfd))
  {
    error= SSL_get_error(conn->ssl_conn, 0);
    goto error_handler;
  }
  printf("SSL session created\n");
  if (sock_conn->is_client)
    error= SSL_connect(conn->ssl_conn);
  else
    error= SSL_accept(conn->ssl_conn);
  printf("SSL session connected\n");
  error= SSL_get_error(conn->ssl_conn, error);
  if (error == SSL_ERROR_NONE)
  {
    printf("SSL Success\n");
    error= 0;
  }
  else
  {
    /* Go through SSL error stack */
    printf("SSL Error: %d\n", error);
    goto error_handler;
  }

  return 0;
error_handler:
  printf("Session creation failed\n");
  free_ssl_session(conn);
  return 1;
}
static int
set_up_ssl_connection(IC_CONNECTION *in_conn)
{
  int error= 1;
  DEBUG_ENTRY("set_up_ssl_connection");

  lock_connect_mutex(in_conn);
  error= set_up_socket_connection(in_conn);
  unlock_connect_mutex(in_conn);
  DEBUG_RETURN(error);
}

static int
accept_ssl_connection(IC_CONNECTION *conn)
{
  int error;
  DEBUG_ENTRY("accept_ssl_connection");

  lock_connect_mutex(conn);
  error= accept_socket_connection(conn);
  unlock_connect_mutex(conn);
  DEBUG_RETURN(error);
}

static void
free_ssl_connection(IC_CONNECTION *in_conn)
{
  IC_SSL_CONNECTION *conn= (IC_SSL_CONNECTION*)in_conn;
  DEBUG_ENTRY("free_ssl_connection");

  /* Free SSL related parts of the connection */
  free_ssl_session(conn);
  /* Free normal socket connection parts */
  free_socket_connection(in_conn);
  DEBUG_RETURN_EMPTY;
}

static IC_CONNECTION*
fork_accept_ssl_connection(IC_CONNECTION *orig_conn,
                           gboolean use_mutex,
                           gboolean use_front_buffer)
{
  IC_CONNECTION *new_conn;
  IC_SSL_CONNECTION *new_ssl_conn;
  IC_SSL_CONNECTION *orig_ssl_conn= (IC_SSL_CONNECTION*)orig_conn;
  DEBUG_ENTRY("fork_accept_ssl_connection");

  lock_connect_mutex(orig_conn);
  if ((new_conn= fork_accept_connection(orig_conn, use_mutex,
                                        use_front_buffer)))
  {
    new_ssl_conn= (IC_SSL_CONNECTION*)new_conn;
    /* Assign SSL specific variables to new connection */
    if (ic_strdup(&new_ssl_conn->root_certificate_path,
                  &orig_ssl_conn->root_certificate_path) ||
        ic_strdup(&new_ssl_conn->loc_certificate_path,
                  &orig_ssl_conn->loc_certificate_path) ||
        ic_strdup(&new_ssl_conn->passwd_string,
                  &orig_ssl_conn->passwd_string))
      goto error_handler;
    new_ssl_conn->ssl_conn= NULL;
    new_ssl_conn->ssl_ctx= NULL;
    new_ssl_conn->ssl_dh= NULL;
    if (ssl_create_connection(new_ssl_conn))
      goto error_handler;
  }
  unlock_connect_mutex(orig_conn);
  DEBUG_RETURN(new_conn);

error_handler:
  unlock_connect_mutex(orig_conn);
  free_ssl_connection(new_conn);
  DEBUG_RETURN(NULL);
}

static int
read_ssl_connection(IC_CONNECTION *conn, void *buf, guint32 buf_size,
                    guint32 *read_size)
{
  int error;
  DEBUG_ENTRY("read_ssl_connection");

  lock_connect_mutex(conn);
  error= read_socket_connection(conn, buf, buf_size, read_size);
  unlock_connect_mutex(conn);
  DEBUG_RETURN(error);
}

static int
write_ssl_connection(IC_CONNECTION *conn, const void *buf, guint32 size,
                     guint32 prio_level, guint32 secs_to_try)
{
  int error;
  DEBUG_ENTRY("write_ssl_connection");

  lock_connect_mutex(conn);
  error= write_socket_connection(conn, buf, size, prio_level, secs_to_try);
  unlock_connect_mutex(conn);
  DEBUG_RETURN(error);
}

static int
write_ssl_front_buffer(IC_CONNECTION *conn, const void *buf, guint32 size,
                       guint32 prio_level, guint32 secs_to_try)
{
  int error;
  DEBUG_ENTRY("write_ssl_connection");

  lock_connect_mutex(conn);
  error= write_socket_front_buffer(conn, buf, size, prio_level, secs_to_try);
  unlock_connect_mutex(conn);
  DEBUG_RETURN(error);
}

IC_CONNECTION*
ic_create_ssl_object(gboolean is_client,
                     IC_STRING *root_certificate_path,
                     IC_STRING *loc_certificate_path,
                     IC_STRING *passwd_string,
                     gboolean is_ssl_used_for_data,
                     gboolean is_connect_thread_used,
                     gboolean is_using_front_buffer,
                     authenticate_func auth_func,
                     void *auth_obj)
{
  IC_CONNECTION *conn;
  IC_SSL_CONNECTION *ssl_conn;
  DEBUG_ENTRY("ic_create_ssl_object");

  if (!(conn= int_create_socket_object(is_client, FALSE,
                                       is_connect_thread_used,
                                       is_using_front_buffer,
                                       TRUE, is_ssl_used_for_data,
                                       auth_func, auth_obj)))
  {
    DEBUG_RETURN(NULL);
  }
  ssl_conn= (IC_SSL_CONNECTION*)conn;
  if (ic_strdup(&ssl_conn->root_certificate_path, root_certificate_path) ||
      ic_strdup(&ssl_conn->loc_certificate_path, loc_certificate_path) ||
      ic_strdup(&ssl_conn->passwd_string, passwd_string))
  {
    free_ssl_connection(conn);
    DEBUG_RETURN(NULL);
  }
  conn->conn_op.ic_set_up_connection= set_up_ssl_connection;
  conn->conn_op.ic_accept_connection= accept_ssl_connection;
  conn->conn_op.ic_free_connection= free_ssl_connection;
  
  /*
    Read and write routines are the same routines but with a number of
    if-statements to handle SSL specific things, however we still want
    to reuse the statistics code in the normal socket object.

    Since we do need to grab a mutex in all cases before accessing an SSL
    connection we define all methods requiring a mutex lock/unlock.
  */
  conn->conn_op.ic_read_connection= read_ssl_connection;
  if (conn->is_using_front_buffer)
    conn->conn_op.ic_write_connection= write_ssl_front_buffer;
  else
    conn->conn_op.ic_write_connection= write_ssl_connection;

  set_up_rw_methods_mutex(conn, FALSE);
  conn->conn_op.ic_fork_accept_connection= fork_accept_ssl_connection;
  return conn;
}
#else
int ic_ssl_init()
{
  return 0;
}

void ic_ssl_end()
{
  return;
}
#endif
