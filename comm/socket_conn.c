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
accept_socket_connection(IC_CONNECTION *conn)
{
  gboolean not_accepted= FALSE;
  int ret_sockfd, ok;
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
    The socket used to listen to a part can be reused many times.
    accept will return a new socket that can be used to read and
    write from. We will not reuse a listening socket so we will
    always close it immediately and only use the returned socket.
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
  set_is_connected(conn);
  return 0;
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
  if (conn->auth_func)
  {
    if ((ret_code= conn->auth_func(conn->auth_obj)))
    {
      unlock_connect_mutex(conn);
      close_socket_connection(conn);
      conn->error_code= ret_code;
      return NULL;
    }
  }
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

    ret_code= send(conn->rw_sockfd, buf + write_size, buf_size,
                   IC_MSG_NOSIGNAL);
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
      if (errno == EINTR)
        continue;
      conn->conn_stat.num_send_errors++;
      conn->bytes_written_before_interrupt= write_size;
      DEBUG_PRINT(COMM_LEVEL, ("write error: %d %s\n",
                               errno, sys_errlist[errno]));
      conn->error_code= errno;
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

static int
read_socket_connection(IC_CONNECTION *conn,
                       void *buf, guint32 buf_size,
                       guint32 *read_size)
{
  int ret_code, error;
  do
  {
    ret_code= recv(conn->rw_sockfd, buf, buf_size, 0);
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
    error= errno;
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

  if ((fork_conn= (IC_CONNECTION*)ic_calloc(sizeof(IC_CONNECTION))) == NULL)
    return NULL;
  memcpy(fork_conn, orig_conn, sizeof(IC_CONNECTION));

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
ic_create_socket_object(gboolean is_client,
                        gboolean is_mutex_used,
                        gboolean is_connect_thread_used,
                        gboolean is_using_front_buffer,
                        authenticate_func auth_func,
                        void *auth_obj)
{
  IC_CONNECTION *conn;

  if ((conn= (IC_CONNECTION*)ic_calloc(sizeof(IC_CONNECTION))) == NULL)
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

/*
  Module: SSL
  -----------
    This module implements sockets using the SSL protocol. It will use
    the same interface except for when a connection is created. In the
    back-end methods there are lots of new methods although many of the
    old methods are reused also for SSL connections.
*/
#ifdef HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

int
ic_ssl_init()
{
  if (!SSL_library_init())
    return 1;
  return 0;
}

static void
free_ssl_session(IC_CONNECTION *conn)
{
  if (conn->ssl_ctx)
    SSL_CTX_free(conn->ssl_ctx);
  if (conn->ssl_conn)
    SSL_free(conn->ssl_conn);
  if (conn->root_certificate_path.str)
    ic_free(conn->root_certificate_path.str);
  if (conn->server_certificate_path.str)
    ic_free(conn->server_certificate_path.str);
  if (conn->client_certificate_path.str)
    ic_free(conn->client_certificate_path.str);
  conn->ssl_ctx= NULL;
  conn->ssl_conn= NULL;
  IC_INIT_STRING(&conn->root_certificate_path, NULL, 0, FALSE);
  IC_INIT_STRING(&conn->server_certificate_path, NULL, 0, FALSE);
  IC_INIT_STRING(&conn->client_certificate_path, NULL, 0, FALSE);
}

static int
ssl_create_session(IC_CONNECTION *conn)
{
  if (!(conn->ssl_ctx= SSL_CTX_new(SSLv3_method())))
    goto error;
  /*
    Now we have allocated a context from which this connection will work in.
    Next step is to define certificate handling of this context. And finally
    after this we'll create an SSL session which is then ready for SSL_connect
    or SSL_accept.
  */
  if (!(conn->ssl_conn= SSL_new(conn->ssl_ctx)))
    goto error;
  return 0;
error:
  free_ssl_session(conn);
  return 1;
}

int set_up_ssl_connection(IC_CONNECTION *conn)
{
  int error= 1;
  DEBUG_ENTRY("set_up_ssl_connection");

  lock_connect_mutex(conn);
  if (!(error= set_up_socket_connection(conn)))
  {
    /*
      Here we perform the SSL specific set-up of the connection. When we
      arrive here a connection is already set-up and now we want to
      convert the connection into an SSL connection by performing the
      start-up part of the SSL protocol.
    */
    if (ssl_create_session(conn))
      goto error_handler;
    if (!SSL_set_fd(conn->ssl_conn, conn->rw_sockfd))
    {
      error= SSL_get_error(conn->ssl_conn, 0);
      goto error_handler;
    }
    if (conn->is_client)
      error= SSL_connect(conn->ssl_conn);
    else
      error= SSL_accept(conn->ssl_conn);
    error= SSL_get_error(conn->ssl_conn, error);
    if (error == SSL_ERROR_NONE)
      error= 0;
    if (error == SSL_ERROR_SSL)
    {
      while (error != SSL_ERROR_NONE)
      {
        /* Go through SSL error stack */
        SSL_get_error(conn->ssl_conn, error);
        printf("SSL Error: %d\n", error);
      }
    }
  }
  if (error)
    goto error_handler;
  unlock_connect_mutex(conn);
  DEBUG_RETURN(0);

error_handler:
  free_ssl_session(conn);
  DEBUG_RETURN(error);
}

static int
accept_ssl_connection(IC_CONNECTION *conn)
{
  int error;
  DEBUG_ENTRY("accept_ssl_connection");

  lock_connect_mutex(conn);
  if ((error= accept_socket_connection(conn)))
  {
    /* Here we perform server related set-up code for SSL connections. */
  }
  unlock_connect_mutex(conn);
  DEBUG_RETURN(error);
}

static void
free_ssl_connection(IC_CONNECTION *conn)
{
  DEBUG_ENTRY("free_ssl_connection");
  /* Free SSL related parts of the connection */
  free_ssl_session(conn);
  /* Free normal socket connection parts */
  free_socket_connection(conn);
  DEBUG_RETURN_EMPTY;
}

static IC_CONNECTION*
fork_accept_ssl_connection(IC_CONNECTION *orig_conn,
                           gboolean use_mutex,
                           gboolean use_front_buffer)
{
  IC_CONNECTION *new_conn;
  DEBUG_ENTRY("fork_accept_ssl_connection");

  lock_connect_mutex(orig_conn);
  if ((new_conn= fork_accept_connection(orig_conn, use_mutex,
                                        use_front_buffer)))
  {
    /* Assign SSL specific variables to new connection */
  }
  unlock_connect_mutex(orig_conn);
  DEBUG_RETURN(new_conn);
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
                     IC_STRING *server_certificate_path,
                     IC_STRING *client_certificate_path,
                     gboolean is_connect_thread_used,
                     gboolean is_using_front_buffer)
{
  IC_CONNECTION *conn;
  DEBUG_ENTRY("ic_create_ssl_object");

  if (!(conn= ic_create_socket_object(is_client, FALSE,
                                      is_connect_thread_used,
                                      is_using_front_buffer, NULL, NULL)))
  {
    DEBUG_RETURN(NULL);
  }
  if (ic_strdup(&conn->root_certificate_path, root_certificate_path) ||
      ic_strdup(&conn->server_certificate_path, server_certificate_path) ||
      ic_strdup(&conn->client_certificate_path, client_certificate_path))
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
  /* Mutex handling for SSL requires one mutex for read/write */
  conn->is_ssl_connection= TRUE;
  return conn;
}


#endif
