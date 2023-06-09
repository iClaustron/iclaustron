/* Copyright (C) 2007, 2015 iClaustron AB

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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_string.h>
#include <ic_bitmap.h>
#include <ic_port.h>
#include <ic_threadpool.h>
#include <ic_connection.h>
#include "ic_connection_int.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_TRACE
#include <probes.h>
#endif
#include <errno.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_TIMES_H
#include <sys/times.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SSL
static int ssl_create_connection(IC_SSL_CONNECTION *conn);
static int ic_ssl_write(IC_INT_CONNECTION *conn,
                        const void *buf,
                        guint32 buf_size);
static int ic_ssl_read(IC_INT_CONNECTION *conn,
                       void *buf,
                       guint32 buf_size,
                       guint32 *read_size);
#endif
static void destroy_timers(IC_INT_CONNECTION *conn);
static void destroy_mutexes(IC_INT_CONNECTION *conn);

static int close_socket_connection(IC_CONNECTION *conn);
gchar *ic_zero_str= "0";

/* Implements ic_check_for_data */
static gboolean
check_for_data_on_connection(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  IC_POLLFD_STRUCT poll_struct;
  int ret_code= 0;
  int time_out;
  int timeout_in_ms= conn->rec_wait_ms;

  poll_struct.events= IC_POLL_FLAG;
  poll_struct.fd= conn->rw_sockfd;

  if (timeout_in_ms == 0)
    timeout_in_ms= 1;
  while (timeout_in_ms)
  {
    if (ic_tp_get_stop_flag())
      return TRUE;
    time_out= IC_MIN(timeout_in_ms, 1000);
    timeout_in_ms-= time_out;
    ret_code= ic_poll(&poll_struct, 1, time_out);
  }
  if (ret_code > 0)
    return TRUE;
  else
    return FALSE;
}

static void
lock_connect_mutex(IC_INT_CONNECTION *conn)
{
  if (conn->is_mutex_used)
    ic_mutex_lock(conn->connect_mutex);
}

static void
unlock_connect_mutex(IC_INT_CONNECTION *conn)
{
  if (conn->is_mutex_used)
    ic_mutex_unlock(conn->connect_mutex);
}

static void
set_is_connected(IC_INT_CONNECTION *conn)
{
  lock_connect_mutex(conn);
  conn->conn_stat.is_connected= TRUE;
  unlock_connect_mutex(conn);
  return;
}

/* Implements ic_is_conn_connected */
static gboolean
is_conn_connected(IC_CONNECTION *conn)
{
  gboolean is_connected;

  is_connected= conn->conn_stat.is_connected;
  return is_connected;
}

/* Implements ic_is_conn_connected */
static gboolean
is_mutex_conn_connected(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  gboolean is_connected;

  ic_mutex_lock(conn->connect_mutex);
  is_connected= conn->conn_stat.is_connected;
  ic_mutex_unlock(conn->connect_mutex);
  return is_connected;
}

static void
join_connect_thread(IC_INT_CONNECTION *conn, gboolean thread_active)
{
  if (!thread_active && conn->thread)
  {
    g_thread_join(conn->thread);
    conn->thread= NULL;
  }
}

/* Implements ic_is_conn_thread_active */
static gboolean
is_conn_thread_active(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  gboolean thread_active= conn->conn_stat.is_connect_thread_active;

  join_connect_thread(conn, thread_active);
  return thread_active;
}

/* Implements ic_is_conn_thread_active */
static gboolean
is_mutex_conn_thread_active(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  gboolean is_thread_active;

  ic_mutex_lock(conn->connect_mutex);
  is_thread_active= conn->conn_stat.is_connect_thread_active;
  ic_mutex_unlock(conn->connect_mutex);
  join_connect_thread(conn, is_thread_active);
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
set_socket_nonblocking(int sockfd, gboolean flag)
{
#ifdef WINDOWS
  unsigned long nonblocking_flag= (unsigned long)flag;
  if (ioctlsocket(sockfd, FIONBIO, &nonblocking_flag))
  {
    DEBUG_PRINT(COMM_LEVEL, ("fcntl F_GETFL error: %d", WSAGetLastError()));
  }
#else
  int flags, error;
  int nonblocking_flag;

  if ((flags= fcntl(sockfd, F_GETFL) < 0))
  {
    error= errno;
    DEBUG_PRINT(COMM_LEVEL, ("fcntl F_GETFL error: %d", error));
    return; /* Failed to set socket nonblocking */
  }
#ifdef O_NONBLOCK
  nonblocking_flag= O_NONBLOCK;
#else
#ifdef O_NDELAY
  nonblocking_flag= O_NDELAY;
#else
  No proper nonblocking flag to use
#endif
#endif
  if (flag)
    flags|= nonblocking_flag;
  else
    flags&= ~nonblocking_flag;
  if ((flags= fcntl(sockfd, F_SETFL, (long)flags) < 0))
  {
    error= ic_get_last_socket_error();
    DEBUG_PRINT(COMM_LEVEL, ("fcntl F_SETFL error: %d", error));
  }
#endif
}

static void
set_socket_options(IC_INT_CONNECTION *conn, int sockfd)
{
  int no_delay, error;
#ifndef WINDOWS
  int maxseg_size;
#endif
  int rec_size, snd_size, reuse_addr;
  IC_SOCKLEN_TYPE sock_len= sizeof(int);
#ifdef __APPLE__
  int no_sigpipe= 1;
  /*
    Mac OS X don't support MSG_NOSIGNAL but instead have a socket option
    SO_NOSIGPIPE that gives the same functionality.
  */
  setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE,
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
    DEBUG_PRINT(COMM_LEVEL, ("Set TCP_NODELAY error: %d", error));
  }
  reuse_addr= 1;
  if ((error= setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                         (const void*)&reuse_addr, sizeof(int))))
  {
    /* We will continue even with this error */
    error= ic_get_last_socket_error();
    DEBUG_PRINT(COMM_LEVEL, ("Set SO_REUSEADDR error: %d", error));
    reuse_addr= 0;
  }
#ifndef WINDOWS
  getsockopt(sockfd, IPPROTO_TCP, TCP_MAXSEG,
             (void*)&maxseg_size, &sock_len);
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Default TCP_MAXSEG = %d", maxseg_size));
  if (conn->is_wan_connection)
  {
    maxseg_size= set_option_size(sockfd, WAN_TCP_MAXSEG_SIZE, maxseg_size,
                                 IPPROTO_TCP, TCP_MAXSEG);
  }
  else
  {
    maxseg_size= set_option_size(sockfd, conn->tcp_maxseg_size, maxseg_size,
                                 IPPROTO_TCP, TCP_MAXSEG);
  }
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Used TCP_MAXSEG = %d", maxseg_size));
  conn->conn_stat.used_tcp_maxseg_size= maxseg_size;
#endif
  sock_len= sizeof(int);
  getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF,
             (void*)&rec_size, &sock_len);
  sock_len= sizeof(int);
  getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF,
             (void*)&snd_size, &sock_len);
  sock_len= sizeof(int);
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Default SO_RCVBUF = %d", rec_size));
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Default SO_SNDBUF = %d", snd_size));

  if (conn->is_wan_connection)
  {
    rec_size= set_option_size(sockfd, WAN_REC_BUF_SIZE, rec_size,
                              SOL_SOCKET, SO_RCVBUF);
    snd_size= set_option_size(sockfd, WAN_SND_BUF_SIZE, snd_size,
                              SOL_SOCKET, SO_SNDBUF);
  }
  else
  {
    rec_size= set_option_size(sockfd, conn->tcp_receive_buffer_size,
                              rec_size, SOL_SOCKET, SO_RCVBUF);
    snd_size= set_option_size(sockfd, conn->tcp_send_buffer_size,
                              snd_size, SOL_SOCKET, SO_SNDBUF);
  }
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Used SO_RCVBUF = %d", rec_size));
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Used SO_SNDBUF = %d", snd_size));
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Used TCP_NODELAY = %d", no_delay));
  DEBUG_PRINT(COMM_DETAIL_LEVEL, ("Used SO_REUSEADDR = %d", reuse_addr));
  conn->conn_stat.used_tcp_receive_buffer_size= rec_size;
  conn->conn_stat.used_tcp_send_buffer_size= snd_size;
  conn->conn_stat.tcp_no_delay= no_delay;
  return;
}

static int
ic_sock_ntop(const struct sockaddr *sa,
             gchar *ip_addr,
             int ip_addr_len,
             gboolean include_port)
{
  struct sockaddr_in *ipv4_addr= (struct sockaddr_in*)sa;
  struct sockaddr_in6 *ipv6_addr= (struct sockaddr_in6*)sa;
  guint32 port;
  IC_SOCKLEN_TYPE addr_len;
  int ret_code;
  gchar port_str[8];

  if (sa->sa_family == AF_INET)
  {
    port= ntohs(ipv4_addr->sin_port);
    addr_len= sizeof(*ipv4_addr);
  }
  else if (sa->sa_family == AF_INET6)
  {
    port= ntohs(ipv6_addr->sin6_port);
    addr_len= sizeof(*ipv6_addr);
  }
  else
    return IC_ERROR_WRONG_IP_FAMILY;

  if ((ret_code= getnameinfo(sa,
                             addr_len,
                             ip_addr,
                             ip_addr_len-8,
                             NULL,
                             0,
                             NI_NUMERICHOST)))
  {
    if (ret_code == EAI_SYSTEM)
      ret_code= ic_get_last_socket_error();
    return ret_code;
  }
  if (include_port)
  {
    snprintf(port_str, sizeof(port_str), ":%d", port);
    strncat(ip_addr, port_str, ip_addr_len);
  }
  return 0;
}

static void
set_ssl_used_for_data(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  conn->save_is_ssl_used_for_data= conn->is_ssl_used_for_data;
  conn->is_ssl_used_for_data= TRUE;
  return;
}

static void
reset_ssl_used_for_data(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  conn->is_ssl_used_for_data= conn->save_is_ssl_used_for_data;
  return;
}

#ifdef DEBUG_BUILD
static void
debug_new_connect(IC_INT_CONNECTION *conn)
{
  DEBUG_PRINT(COMM_LEVEL,
    ("Server: %s, Server Port: %s",
    conn->server_name,
    conn->server_port));
  DEBUG_PRINT(COMM_LEVEL,
    ("Client: %s, Client Port: %s",
    conn->client_name ? conn->client_name : "NULL client_name",
    conn->client_port ? conn->client_port : "NULL client_port"));
}
#else
#define debug_new_connect(conn)
#endif

static int
perform_ssl_connect(IC_INT_CONNECTION *conn)
{
#ifdef HAVE_SSL
  int error;

  IC_SSL_CONNECTION *ssl_conn= (IC_SSL_CONNECTION*)conn;
  if (conn->is_ssl_connection &&
      (error= ssl_create_connection(ssl_conn)))
  {
    ic_close_socket(conn->rw_sockfd);
    return IC_SSL_ERROR;
  }
  return 0;
#else
  (void)conn;
  return 0;
#endif
}

static void
handle_socket_error(IC_INT_CONNECTION *conn, int error_code)
{
  conn->error_code= error_code;
  conn->err_str= ic_get_strerror(conn->error_code,
                                 conn->err_buf,
                                 (guint32)128);
  DEBUG_PRINT(COMM_LEVEL, ("accept error: %d %s",
                            conn->error_code, conn->err_str));
}

/* Implements ic_accept_connection */
static int
accept_socket_connection(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  gboolean not_accepted= FALSE;
  int ret_sockfd, ret_code;
  IC_SOCKLEN_TYPE addr_len;
  IC_IP_ADDRESS client_address_storage;
  struct sockaddr_storage *client_address= (struct sockaddr_storage*)&client_address_storage;
  const struct sockaddr *client_addr_ptr= 
        (const struct sockaddr*)&client_address_storage;
  struct sockaddr_in *ipv4_client_address=
           (struct sockaddr_in*)&client_address_storage;
  struct sockaddr_in6 *ipv6_client_address=
           (struct sockaddr_in6*)&client_address_storage;
  struct sockaddr_in *ipv4_check_address;
  struct sockaddr_in6 *ipv6_check_address;
  fd_set select_set;
  struct timeval time_out;
  int timer= 0;

  /*
    The socket used to listen to a port can be reused many times.
    accept will return a new socket that can be used to read and
    write from.
  */
  addr_len= sizeof(client_address_storage);
  DEBUG_PRINT(COMM_LEVEL, ("Accepting connections on server %s",
                           conn->conn_stat.server_ip_addr));
  set_socket_nonblocking(conn->listen_sockfd, TRUE);
  do
  {
    time_out.tv_usec= 0;
    time_out.tv_sec= 1; /* Timeout is 1 second */

    if (ic_tp_get_stop_flag())
    {
      return IC_ERROR_APPLICATION_STOPPED;
    }
    FD_ZERO(&select_set);
    FD_SET(conn->listen_sockfd, &select_set);
    select(conn->listen_sockfd + 1, &select_set, NULL, NULL, &time_out);
    if (conn->stop_flag)
    {
      return IC_ERROR_CONNECT_THREAD_STOPPED;
    }
    if (!(FD_ISSET(conn->listen_sockfd, &select_set)))
    {
      /*
        Call timeout function, if return is non-zero we'll return with
        a timeout error.
      */
      timer++;
      if (conn->timeout_func &&
          conn->timeout_func(conn->timeout_obj, timer))
      {
        conn->error_code= IC_ERROR_ACCEPT_TIMEOUT;
        return conn->error_code;
      }
    }
    else
    {
      ret_sockfd= accept(conn->listen_sockfd,
                         (struct sockaddr *)&client_address_storage,
                         &addr_len);
      if (ret_sockfd == IC_INVALID_SOCKET)
      {
        ret_code= ic_get_last_socket_error();
#ifndef WINDOWS
        if (ret_code == EAGAIN ||
            ret_code == ECONNABORTED ||
            ret_code == EPROTO ||
            ret_code == EWOULDBLOCK ||
            ret_code == EINTR)
#else
        if (ret_code == WSAEWOULDBLOCK)
#endif
        {
          /*
             EAGAIN/EWOULDBLOCK happens when no connections are yet
             ready (could happen e.g. when first phase of connect
             phase has occurred, but we're still waiting for ACK on
             our SYN ACK message.
             ECONNABORTED/EPROTO happens when a connection is aborted
             before the connection is established or before we got
             to the accept call.
             EINTR happens if a signal is caught in the middle of
             execution of the accept
             In all those cases we decide to simply wait for a while
             longer for a connection.
          */
          continue;
        }
      }
      break;
    }
  } while (1);
  if (!conn->is_listen_socket_retained)
  {
    ic_close_socket(conn->listen_sockfd);
    conn->listen_sockfd= 0;
  }
  if (ret_sockfd == IC_INVALID_SOCKET)
  {
    handle_socket_error(conn, ic_get_last_socket_error());
    return conn->error_code;
  }
  set_socket_options(conn, ret_sockfd);
  set_socket_nonblocking(ret_sockfd, conn->is_nonblocking);
  if (conn->client_name)
  {
    /*
       Check for connection from proper client is highly dependent on
       IP version used.
     */
    if (client_address->ss_family != conn->client_addrinfo->ai_family)
    {
      return IC_ERROR_DIFFERENT_IP_VERSIONS;
    }
    if (addr_len != conn->client_addrinfo->ai_addrlen)
    {
      return IC_ERROR_DIFFERENT_IP_VERSIONS;
    }
    if (client_address->ss_family == AF_INET)
    {
      ipv4_check_address= (struct sockaddr_in*)conn->client_addrinfo->ai_addr;
      if (ipv4_client_address->sin_addr.s_addr !=
          ipv4_check_address->sin_addr.s_addr)
        not_accepted= TRUE;
      if (conn->client_port_num != 0 &&
          ipv4_client_address->sin_port != ipv4_check_address->sin_port)
      {
        not_accepted= TRUE;
      }
    }
    else if (client_address->ss_family == AF_INET6)
    {
      ipv6_check_address= (struct sockaddr_in6*)conn->client_addrinfo->ai_addr;
      if (conn->client_port_num != 0 &&
          ipv6_client_address->sin6_port != ipv6_check_address->sin6_port)
      {
        not_accepted= TRUE;
      }
      if (memcmp(ipv6_client_address->sin6_addr.s6_addr,
                 ipv6_check_address->sin6_addr.s6_addr,
                 sizeof(ipv6_check_address->sin6_addr.s6_addr)))
      {
        not_accepted= TRUE;
      }
    }
    else
    {
      not_accepted= TRUE;
    }
  }
  /* Record a human-readable address for the client part of the connection */
  ret_code= ic_sock_ntop(client_addr_ptr,
                         conn->conn_stat.client_ip_addr,
                         sizeof(conn->conn_stat.client_ip_addr_str),
                         FALSE);
  if (not_accepted || ret_code)
  {
    /*
      The caller only accepted connect's from certain IP address and
      port number. The accepted connection was not from the accepted
      IP address and port.
      We'll disconnect by closing the socket and report an error.
    */
    if (!ret_code)
    {
      DEBUG_PRINT(COMM_LEVEL,
                  ("Client connect from a disallowed address, ip=%s",
                   conn->conn_stat.client_ip_addr));
      conn->error_code= IC_ACCEPT_ERROR;
    }
    else
    {
      handle_socket_error(conn, ret_code);
    }
    ic_close_socket(ret_sockfd);
    return conn->error_code;
  }
  conn->rw_sockfd= ret_sockfd;
  if ((conn->error_code= perform_ssl_connect(conn)))
  {
    return conn->error_code;
  }
  set_is_connected(conn);
  DEBUG_PRINT(COMM_LEVEL, ("Successful server socket connect"));
  debug_new_connect(conn);
  return 0;
}

/* Implements ic_check_connection */
static int
check_connection(IC_CONNECTION *ext_conn,
                 gchar *checked_client_name,
                 gboolean *equal)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  gchar checked_ip_str[256];
  gchar *client_ip_str= conn->conn_stat.client_ip_addr_str;
  struct addrinfo hints;
  struct addrinfo *client_addrinfo= NULL;
  struct addrinfo *loop_addrinfo;
  int ret_code;

  *equal= FALSE;
  /*
    Use checked= _client_name to get numeric host strings that corresponds
    to the hostname (can be a list of them) using getaddrinfo.
    Get address information for Server part
  */
  ic_zero(&hints, sizeof(struct addrinfo));
  hints.ai_flags= AI_PASSIVE;
  hints.ai_family= PF_UNSPEC;
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_protocol= IPPROTO_TCP;
  if ((ret_code= getaddrinfo(checked_client_name, NULL,
       &hints, &client_addrinfo)) != 0)
  {
    conn->err_str= gai_strerror(ret_code);
    return IC_ERROR_GETADDRINFO;
  }
  loop_addrinfo= client_addrinfo;
  for (; loop_addrinfo;
       loop_addrinfo= loop_addrinfo->ai_next)
  {
    ret_code= ic_sock_ntop(loop_addrinfo->ai_addr,
                           checked_ip_str,
                           sizeof(checked_ip_str),
                           FALSE);
    if (!ret_code && (strcmp(checked_ip_str, client_ip_str) == 0))
    {
      *equal= TRUE;
      goto end;
    }
  }
end:
  freeaddrinfo(client_addrinfo);
  return 0;
}

static int
translate_hostnames(IC_INT_CONNECTION *conn)
{
  struct addrinfo hints;
  int ret_code= 0;
  guint64 server_port= 0LL;
  guint64 client_port= 0LL;
  DEBUG_ENTRY("translate_hostnames");

  if (!conn->server_name)
  {
    ret_code= IC_ERROR_NO_SERVER_NAME;
    goto end;
  }
  if (!conn->server_port)
  {
    ret_code= IC_ERROR_NO_SERVER_PORT;
    goto end;
  }
  DEBUG_PRINT(COMM_LEVEL, ("Server port = %s", conn->server_port));
  if (ic_conv_str_to_int(conn->server_port, &server_port, NULL) ||
      server_port == 0LL || server_port > 65535)
  {
    ret_code= IC_ERROR_ILLEGAL_SERVER_PORT;
    goto end;
  }
  conn->server_port_num= (guint16)server_port;
  /* Get address information for Server part */
  ic_zero(&hints, sizeof(struct addrinfo));
#ifdef AI_NUMERICSERV
  hints.ai_flags= AI_NUMERICSERV;
#else
  hints.ai_flags= 0;
#endif
  if (!conn->is_client)
    hints.ai_flags|= AI_PASSIVE;
  hints.ai_family= PF_UNSPEC;
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_protocol= IPPROTO_TCP;
  DEBUG_PRINT(COMM_LEVEL, ("Server name = %s, port = %s",
              conn->server_name, conn->server_port));
  if ((ret_code= getaddrinfo(conn->server_name, conn->server_port,
       &hints, &conn->server_addrinfo)) != 0)
  {
    conn->err_str= gai_strerror(ret_code);
    ret_code= IC_ERROR_GETADDRINFO;
    goto end;
  }
  conn->ret_server_addrinfo= conn->server_addrinfo;
  /* Record a human-readable address for the server part of the connection */
  ic_sock_ntop(conn->server_addrinfo->ai_addr,
               conn->conn_stat.server_ip_addr,
               sizeof(conn->conn_stat.server_ip_addr_str),
               TRUE);

  if (!conn->client_name)
  {
    /*
      Client: Connections from all clients and ports possible
      Server: No bind used since no client host provided
    */
    goto end;
  }
  if (!conn->client_port)
    conn->client_port= ic_zero_str; /* Use "0" if user provided no port */
  if (ic_conv_str_to_int(conn->client_port, &client_port, NULL) ||
      client_port > 65535)
  {
    ret_code= IC_ERROR_ILLEGAL_CLIENT_PORT;
    goto end;
  }
  conn->client_port_num= (guint16)client_port;
  /* Get address information for Client part */
  ic_zero(&hints, sizeof(struct addrinfo));
#ifdef AI_NUMERICSERV
  hints.ai_flags= AI_NUMERICSERV;
#else
  hints.ai_flags= 0;
#endif
  hints.ai_family= PF_UNSPEC;
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_protocol= IPPROTO_TCP;
  DEBUG_PRINT(COMM_LEVEL, ("Client name = %s, service = %s",
              conn->client_name, conn->client_port));
  if ((ret_code= getaddrinfo(conn->client_name, conn->client_port,
       &hints, &conn->client_addrinfo)) != 0)
  {
    conn->err_str= gai_strerror(ret_code);
    ret_code= IC_ERROR_GETADDRINFO;
    goto end;
  }
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
  {
    ret_code= IC_ERROR_DIFFERENT_IP_VERSIONS;
    goto end;
  }
  if (conn->is_client)
  {
    /* Record a human-readable address for the client part of the connection */
    ic_sock_ntop(conn->client_addrinfo->ai_addr,
                 conn->conn_stat.client_ip_addr,
                 sizeof(conn->conn_stat.client_ip_addr_str),
                 FALSE);
  }
end:
  DEBUG_RETURN_INT(ret_code);
}

static int
check_user_timeout(IC_INT_CONNECTION *conn, int *timer)
{
  if (ic_tp_get_stop_flag())
    return IC_ERROR_APPLICATION_STOPPED;
  (*timer)+= 3;
  if (conn->timeout_func &&
      conn->timeout_func(conn->timeout_obj, *timer))
  {
    conn->error_code= IC_ERROR_CONNECT_TIMEOUT;
    return conn->error_code;
  }
  return 0;
}

/**
  Method to increment delay logarithmically up to 10 seconds
*/
static int
logarithmic_delay(int *delay)
{
  int ret_delay = *delay;
  *delay= (*delay) * 2;
  *delay= IC_MAX(*delay, 10);
  return ret_delay;
}

static int
int_set_up_socket_connection(IC_INT_CONNECTION *conn)
{
  int ret_code, sockfd;
  IC_SOCKLEN_TYPE len;
  struct addrinfo *loc_addrinfo;
  fd_set read_set, write_set;
  struct timeval time_out;
  int timer= 0;
  int delay= 1;
  gboolean first= TRUE;

  if ((ret_code= translate_hostnames(conn)))
  {
    DEBUG_PRINT(COMM_LEVEL,
      ("Translating hostnames failed error = %d", ret_code));
    conn->error_code= ret_code;
    return conn->error_code;
  }

  loc_addrinfo= conn->is_client ?
      conn->client_addrinfo : conn->server_addrinfo;
  /*
    Create a socket for the connection
  */
renew_connect:
  if (!first)
  {

    /*
      We had a connection failure, we will renew the connect message
      if the user is still willing to wait for a successful connect.
    */
    ic_close_socket(sockfd);
    if ((ret_code= check_user_timeout(conn, &timer)))
      return ret_code;
    ic_sleep(logarithmic_delay(&delay));
  }
  first= FALSE;
  if ((sockfd= socket(conn->server_addrinfo->ai_family,
                      conn->server_addrinfo->ai_socktype,
                      conn->server_addrinfo->ai_protocol)) < 0)
    goto error;
  set_socket_options(conn, sockfd);
  if (((conn->is_client == FALSE) ||
       (conn->client_name != NULL)) &&
       (bind(sockfd, (struct sockaddr *)loc_addrinfo->ai_addr,
             loc_addrinfo->ai_addrlen) < 0))
  {
    /*
      Bind the socket to an IP address and port on this box.  If the caller
      set port to "0" for a client connection an ephemeral port will be used.
    */
    DEBUG_PRINT(COMM_LEVEL, ("bind error"));
    goto error;
  }
  if (conn->is_client)
  {
    set_socket_nonblocking(sockfd, TRUE);
    do
    {
      /*
        Connect the client to the server, will block until connected or
        timeout occurs.
      */
      if ((connect(sockfd,
                  (struct sockaddr*)conn->server_addrinfo->ai_addr,
                  conn->server_addrinfo->ai_addrlen)) ==
                  IC_SOCKET_ERROR)
      {
        ret_code= ic_get_last_socket_error();
#ifndef WINDOWS
        if (ret_code == EINPROGRESS || ret_code == EINTR)
#else
        if (ret_code == WSAEWOULDBLOCK)
#endif
        {
          do
          {
            /* We successfully sent a CONNECT request, still waiting for reply */
            time_out.tv_sec= 3; /* Timeout is 3 seconds */
            time_out.tv_usec= 0;
            FD_ZERO(&read_set);
            FD_SET(sockfd, &read_set);
            write_set= read_set;
            ret_code= select(sockfd + 1,
                             &read_set,
                             &write_set,
                             NULL,
                             &time_out);
            if (ret_code == 0)
            {
              /*
                 We timed out, we will callback to the user every 3 seconds.
                 We will wait until the user decides to wait no more.
              */
              if ((ret_code= check_user_timeout(conn, &timer)))
              {
                ic_close_socket(sockfd);
                return ret_code;
              }
            }
            else
            {
              if (ret_code < 0)
              {
                DEBUG_PRINT(COMM_LEVEL,
                            ("select after connect, error = %d",
                            ic_get_last_socket_error()));
                goto renew_connect;
              }
              break;
            }
          } while (1);
          if ((FD_ISSET(sockfd, &read_set)) || 
              (FD_ISSET(sockfd, &write_set)))
          {
            len= sizeof(ret_code);
            if (getsockopt(sockfd,
                           SOL_SOCKET,
                           SO_ERROR,
                           (IC_VOID_PTR_TYPE)&ret_code,
                           &len) < 0 ||
                ret_code != 0)
            {
              if (ret_code != 0)
              {
                DEBUG_PRINT(COMM_DETAIL_LEVEL,
                            ("getsockopt after select/connect, error = %d",
                            ret_code));
              }
              else
              {
                DEBUG_PRINT(COMM_DETAIL_LEVEL, ("getsockopt failed, error = %d",
                                         ic_get_last_socket_error()));
              }
              goto renew_connect;
            }
            break;
          }
          else
          {
            ic_printf("sockfd not set at connect");
            abort();
          }
        }
        DEBUG_PRINT(COMM_LEVEL, ("connect error"));
        goto error;
      }
      break;
    } while (1);
    conn->rw_sockfd= sockfd;
    if ((conn->error_code= perform_ssl_connect(conn)))
    {
      return conn->error_code;
    }
    set_is_connected(conn);
    set_socket_nonblocking(sockfd, conn->is_nonblocking);
    DEBUG_PRINT(COMM_LEVEL, ("Successful client socket connect"));
    debug_new_connect(conn);
  }
  else
  {
    /*
      Server: Listen for incoming connect messages
    */
    if ((listen(sockfd, conn->backlog) < 0))
    {
      DEBUG_PRINT(COMM_LEVEL, ("listen error"));
      goto error;
    }
    conn->listen_sockfd= sockfd;
    if (!conn->is_listen_socket_retained)
      return accept_socket_connection((IC_CONNECTION*)conn);
    else
      conn->listen_sockfd= sockfd;
  }
  conn->error_code= 0;
  return 0;

error:
  ret_code= ic_get_last_socket_error();
  conn->err_str= ic_get_strerror(ret_code,
                                 conn->err_buf,
                                 (guint32)128);
  DEBUG_PRINT(COMM_LEVEL, ("Error code: %d, message: %s",
                            ret_code, conn->err_str));
  conn->error_code= ret_code;
  ic_close_socket(sockfd);
  return ret_code;
}

static gpointer
run_set_up_socket_connection(gpointer data)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)data;

  int_set_up_socket_connection(conn);
  lock_connect_mutex(conn);
  conn->conn_stat.is_connect_thread_active= FALSE;
  unlock_connect_mutex(conn);
  return NULL;
}

/* Implements ic_set_up_connection */
static int
set_up_socket_connection(IC_CONNECTION *ext_conn,
                         accept_timeout_func timeout_func,
                         void *timeout_obj)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  GError *error= NULL;
  GThread *thread;

  conn->timeout_func= timeout_func;
  conn->timeout_obj= timeout_obj;
  if (!conn->is_connect_thread_used)
    return int_set_up_socket_connection(conn);

  if (!(thread= g_thread_try_new(NULL,
                                run_set_up_socket_connection,
                                (gpointer)conn,
                                &error)))
  {
    conn->error_code= 1;
    return 1; /* Should write proper error handling code here */
  }
  conn->thread= thread;
  return 0;
}

struct ic_send_state
{
  GTimer *time_measure;
  gdouble secs_expired;
  gdouble next_sec_check;
  guint32 secs_count;
  guint32 secs_to_try;
  guint32 write_size;
  guint32 loop_count;
  guint32 buf_size;
};
typedef struct ic_send_state IC_SEND_STATE;

static int
handle_return_write(IC_INT_CONNECTION *conn, gssize ret_code, 
                    IC_SEND_STATE *send_state)
{
  guint32 buf_size= send_state->buf_size;

  if ((int)send_state->write_size == (int)buf_size)
  {
    unsigned i;
    guint32 num_sent_buf_range;
    guint64 num_sent_bufs= conn->conn_stat.num_sent_buffers;
    guint64 num_sent_bytes= conn->conn_stat.num_sent_bytes;
    long double num_sent_square= conn->conn_stat.num_sent_bytes_square_sum;

    i= ic_count_highest_bit((guint32)(buf_size | 32));
    i-= 6;
    num_sent_buf_range= conn->conn_stat.num_sent_buf_range[i];
    conn->error_code= 0;
    conn->conn_stat.num_sent_buffers= num_sent_bufs + 1;
    conn->conn_stat.num_sent_bytes= num_sent_bytes + buf_size;
    conn->conn_stat.num_sent_bytes_square_sum=
                    num_sent_square + (long double)(buf_size*buf_size);
    conn->conn_stat.num_sent_buf_range[i]= num_sent_buf_range + 1;
    if (send_state->loop_count && send_state->time_measure)
      g_timer_destroy(send_state->time_measure);
    return 0;
  }
  if (!send_state->loop_count)
  {
    send_state->time_measure= g_timer_new();
    send_state->secs_expired= (gdouble)0;
    send_state->next_sec_check= (gdouble)1;
    send_state->secs_count= 0;
  }
  if (ret_code == IC_SOCKET_ERROR)
  {
    int error;
    if (!conn->is_ssl_used_for_data)
      error= ic_get_last_socket_error();
    else
      error= (-1) * ret_code;
    if (error == EINTR)
    {
      conn->error_code= 0;
      return error;
    }
    conn->conn_stat.num_send_errors++;
    conn->bytes_written_before_interrupt= send_state->write_size;
    conn->error_code= error;
    conn->err_str= ic_get_strerror(conn->error_code,
                                   conn->err_buf,
                                   (guint32)128);
    DEBUG_PRINT(COMM_LEVEL, ("write error: %d %s",
                error, conn->err_str));
    conn->error_code= error;
    return error;
  }
  if (send_state->loop_count)
  {
    if ((send_state->time_measure &&
        ((g_timer_elapsed(send_state->time_measure, NULL) +
                          send_state->secs_expired) >
         send_state->next_sec_check)) ||
         send_state->loop_count == 1000)
    {
      send_state->next_sec_check+= (gdouble)1;
      send_state->secs_count++;
      if (send_state->secs_count >= send_state->secs_to_try)
      {
        conn->conn_stat.num_send_timeouts++;
        conn->bytes_written_before_interrupt= send_state->write_size;
        conn->error_code= EINTR;
        DEBUG_PRINT(COMM_LEVEL, ("timeout error on write"));
        return EINTR;
      }
    }
    send_state->loop_count= 1;
  }
  if (send_state->loop_count)
    g_usleep(1000);
  conn->error_code= 0;
  return EINTR;
}

/* Implements ic_write_connection */
static int
write_socket_connection(IC_CONNECTION *ext_conn,
                        const void *buf,
                        guint32 size,
                        guint32 secs_to_try)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  IC_SEND_STATE send_state;
  gssize ret_code;
  int error;
  gint32 buf_size;

  send_state.time_measure= NULL;
  send_state.write_size= 0;
  send_state.loop_count= 0;
  send_state.secs_to_try= secs_to_try;
  send_state.buf_size= size;

  do
  {
    buf_size= size - send_state.write_size;

#ifdef HAVE_SSL
    if (conn->is_ssl_used_for_data)
      ret_code= ic_ssl_write(conn,
                             buf + send_state.write_size,
                             buf_size);
    else
#endif
      ret_code= send(conn->rw_sockfd,
                     ((const IC_VOID_PTR_TYPE)buf + send_state.write_size),
                     buf_size,
                     IC_MSG_NOSIGNAL);
    if (ret_code != IC_SOCKET_ERROR)
      send_state.write_size+= ret_code;
    if (!(error= handle_return_write(conn, ret_code, &send_state)))
      return 0;
    if (error != EINTR || conn->error_code != 0)
      return error;
    send_state.loop_count++;
  } while (1);
  return 0;
}

static int
flush_connection(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  int ret_code;

  ic_assert(conn->write_buf_pos);
  /* We need to flush buffer */
  ret_code= ext_conn->conn_op.ic_write_connection(ext_conn,
                                           (const void*)conn->write_buf,
                                           conn->write_buf_pos,
                                           1);
  conn->write_buf_pos= 0;
  return ret_code;
}

#ifdef WINDOWS
#define send_msg(a,b,c,d,e) 1
#else
static int
send_msg(IC_INT_CONNECTION *conn,
         IC_IOVEC *write_vector,
         guint32 iovec_size,
         guint32 tot_size,
         guint32 secs_to_try)
{
  IC_SEND_STATE send_state;
  struct iovec iovec_array[IC_MAX_SEND_BUFFERS];
  struct msghdr msg_hdr;
  gssize ret_code;
  int error;
  guint32 iovec_index= 0;
  guint32 vec_len;
  guint32 write_len;
  guint32 i;

  send_state.time_measure= NULL;
  send_state.write_size= 0;
  send_state.loop_count= 0;
  send_state.secs_to_try= secs_to_try;
  send_state.buf_size= tot_size;

  ic_zero(&msg_hdr, sizeof(struct msghdr));
  for (i= 0; i < iovec_size; i++)
  {
    iovec_array[i].iov_base= write_vector[i].iov_base;
    iovec_array[i].iov_len= write_vector[i].iov_len;
  }
  do
  {
    
    msg_hdr.msg_iov= &iovec_array[iovec_index];
    msg_hdr.msg_iovlen= iovec_size - iovec_index;
    ret_code= sendmsg(conn->rw_sockfd,
                      &msg_hdr,
                      IC_MSG_NOSIGNAL);
    if (ret_code > 0)
    {
      send_state.write_size+= ret_code;
      if (send_state.write_size < tot_size)
      {
        /* We need to calculate where to start the next send */
        do
        {
          vec_len= (guint32)iovec_array[iovec_index].iov_len;
          write_len= (guint32)ret_code;
          if (write_len < vec_len)
          {
            iovec_array[iovec_index].iov_len-= write_len;
            iovec_array[iovec_index].iov_base+= write_len;
            break;
          }
          else
          {
            write_len-= iovec_array[iovec_index].iov_len;
            iovec_index++;
          }
        } while (1);
      }
    }
    if (!(error= handle_return_write(conn, ret_code, &send_state)))
      return 0;
    if (error != EINTR || conn->error_code != 0)
      return error;
    send_state.loop_count++;
  } while (1);
  return 0;
}
#endif

/* Implements ic_writev_connection */
static int
writev_socket_connection(IC_CONNECTION *ext_conn,
                         IC_IOVEC *write_vector,
                         guint32 iovec_size,
                         guint32 tot_size,
                         guint32 secs_to_try)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  int ret_code= 0;
  gboolean use_loop= FALSE;
  guint32 i;

  if (conn->is_ssl_used_for_data)
    use_loop= TRUE;
#ifdef WINDOWS
  use_loop= TRUE;
#endif
  if (use_loop)
  {
    for (i= 0; i < iovec_size; i++)
    {
      if ((ret_code= write_socket_connection(ext_conn,
                                             write_vector[i].iov_base,
                                             write_vector[i].iov_len,
                                             secs_to_try)))
        break;
    }
  }
  else
  {
    ret_code= send_msg(conn,
                       write_vector,
                       iovec_size,
                       tot_size,
                       secs_to_try);
  }
  return ret_code;
}


/* Implements ic_close_connection */
static int
close_socket_connection(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  lock_connect_mutex(conn);
  if (conn->listen_sockfd)
  {
    ic_close_socket(conn->listen_sockfd);
    conn->listen_sockfd= 0;
  }
  if (conn->rw_sockfd)
  {
    ic_close_socket(conn->rw_sockfd);
    conn->rw_sockfd= 0;
  }
  conn->error_code= 0;
  unlock_connect_mutex(conn);
  return 0;
}

/* Implements ic_close_listen_connection */
static int
close_listen_socket_connection(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  lock_connect_mutex(conn);
  if (conn->listen_sockfd)
  {
    ic_close_socket(conn->listen_sockfd);
    conn->listen_sockfd= 0;
  }
  conn->error_code= 0;
  unlock_connect_mutex(conn);
  return 0;
}

/* Implements ic_read_connection */
static int
read_socket_connection(IC_CONNECTION *ext_conn,
                       void *buf, guint32 buf_size,
                       guint32 *read_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
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
      long double num_rec_square= conn->conn_stat.num_rec_bytes_square_sum;
      
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
                        num_rec_square + (long double)(ret_code*ret_code);
      conn->conn_stat.num_rec_buf_range[i]= num_rec_buf_range + 1;
      return 0;
    }
    if (ret_code == 0)
    {
      DEBUG_PRINT(COMM_LEVEL, ("End of file received"));
      conn->error_code= IC_END_OF_FILE;
      return IC_END_OF_FILE;
    }
    if (!conn->is_ssl_used_for_data)
      error= ic_get_last_socket_error();
    else
      error= (-1) * ret_code;
  } while (error == EINTR);
  conn->conn_stat.num_rec_errors++;
  conn->error_code= ic_get_last_socket_error();
  conn->err_str= ic_get_strerror(conn->error_code,
                                 conn->err_buf,
                                 (guint32)128);
  DEBUG_PRINT(COMM_LEVEL, ("read error: %d %s",
                           error, conn->err_str));
  return error;
}

/* Implements ic_open_write_session */
static int
open_write_socket_session_mutex(IC_CONNECTION *ext_conn,
                                guint32 total_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  (void)total_size;
  ic_mutex_lock(conn->write_mutex);
  return 0;
}

/* Implements ic_close_write_session */
static int
close_write_socket_session_mutex(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  ic_mutex_unlock(conn->write_mutex);
  return 0;
}

/* Implements ic_open_read_session */
static int
open_read_socket_session_mutex(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  ic_mutex_lock(conn->read_mutex);
  return 0; 
}

/* Implements ic_close_read_session */
static int
close_read_socket_session_mutex(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  ic_mutex_unlock(conn->read_mutex);
  return 0;
}

/* Implements various methods not used */
static int
no_op_socket_method(IC_CONNECTION *ext_conn)
{
  (void)ext_conn;
  return 0;
}

/* Implements various methods not used */
static int
no_op_with_size_socket_method(IC_CONNECTION *ext_conn,
                              guint32 total_size)
{
  (void)ext_conn;
  (void)total_size;
  return 0;
}

/* Implements ic_free_connection */
static void
free_socket_connection(IC_CONNECTION *ext_conn)
{
  DEBUG_DECL(char buf[64];)
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  DEBUG_ENTRY("free_socket_connection");
  DEBUG_DECL(ic_guint64_hex_str((guint64)ext_conn, buf);)
  DEBUG_PRINT(COMM_LEVEL, ("free connection 0x%s", buf));

  if (!conn)
    goto end;
  if (conn->thread)
  {
    conn->stop_flag= TRUE;
    g_thread_join(conn->thread);
    conn->thread= NULL;
  }
  close_socket_connection(ext_conn);
  if (conn->ret_client_addrinfo)
    freeaddrinfo(conn->ret_client_addrinfo);
  if (conn->ret_server_addrinfo)
    freeaddrinfo(conn->ret_server_addrinfo);
  destroy_timers(conn);
  destroy_mutexes(conn);
  if (conn->read_buf)
    ic_free_conn(conn->read_buf);
  if (conn->write_buf)
    ic_free_conn(conn->write_buf);
  ic_free_conn(conn);
end:
  DEBUG_RETURN_EMPTY;
}

/* Implements ic_read_stat_connection */
static void
read_stat_socket_connection(IC_CONNECTION *ext_conn,
                            IC_CONNECT_STAT *conn_stat,
                            gboolean clear_stat_timer)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  memcpy((void*)conn_stat, (void*)&conn->conn_stat,
         sizeof(IC_CONNECT_STAT));
  if (clear_stat_timer)
    g_timer_reset(conn->last_read_stat);
}

/* Implements ic_safe_read_stat_connection */
static void
safe_read_stat_socket_conn(IC_CONNECTION *ext_conn,
                           IC_CONNECT_STAT *conn_stat,
                           gboolean clear_stat_timer)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  ic_mutex_lock(conn->connect_mutex);
  ic_mutex_lock(conn->write_mutex);
  ic_mutex_lock(conn->read_mutex);
  read_stat_socket_connection(ext_conn, conn_stat, clear_stat_timer);
  ic_mutex_unlock(conn->read_mutex);
  ic_mutex_unlock(conn->write_mutex);
  ic_mutex_unlock(conn->connect_mutex);
}

/* Implements ic_read_connection_time */
static double
read_socket_connection_time(IC_CONNECTION *ext_conn,
                            gulong *microseconds)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return g_timer_elapsed(conn->connection_start, microseconds);
}

/* Implements ic_read_stat_time */
static double
read_socket_stat_time(IC_CONNECTION *ext_conn,
                      gulong *microseconds)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return g_timer_elapsed(conn->last_read_stat, microseconds);
}

/* Implements ic_write_stat_connection */
static void
write_stat_socket_connection(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  ic_printf("Number of sent buffers = %u, Number of sent bytes = %u",
         (guint32)conn->conn_stat.num_sent_buffers,
         (guint32)conn->conn_stat.num_sent_bytes);
  ic_printf("Number of rec buffers = %u, Number of rec bytes = %u",
         (guint32)conn->conn_stat.num_rec_buffers,
         (guint32)conn->conn_stat.num_rec_bytes);
  ic_printf("Number of send errors = %u, Number of send timeouts = %u",
        (guint32)conn->conn_stat.num_send_errors,
        (guint32)conn->conn_stat.num_send_timeouts);
  ic_printf("Number of rec errors = %u",
        (guint32)conn->conn_stat.num_rec_errors);
}

static void
destroy_timers(IC_INT_CONNECTION *conn)
{
  if (conn->connection_start)
    g_timer_destroy(conn->connection_start);
  if (conn->last_read_stat)
    g_timer_destroy(conn->last_read_stat);
  conn->connection_start= NULL;
  conn->last_read_stat= NULL;
}

static void
destroy_mutexes(IC_INT_CONNECTION *conn)
{
  if (conn->read_mutex)
    ic_mutex_destroy(&conn->read_mutex);
  if (conn->write_mutex)
    ic_mutex_destroy(&conn->write_mutex);
  if (conn->connect_mutex)
    ic_mutex_destroy(&conn->connect_mutex);
}

static gboolean
create_mutexes(IC_INT_CONNECTION *conn)
{
  if (!((conn->read_mutex= ic_mutex_create()) &&
        (conn->write_mutex= ic_mutex_create()) &&
        (conn->connect_mutex= ic_mutex_create())))
  {
    destroy_mutexes(conn);
    return TRUE;
  }
  return FALSE;
}

static gboolean
create_timers(IC_INT_CONNECTION *conn)
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

/* Implements ic_cmp_connection */
static int
cmp_hostname_and_port(IC_CONNECTION *ext_conn, gchar *hostname, gchar *port)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  if ((strcmp(conn->server_name, hostname) == 0) &&
      (strcmp(conn->server_port, port) == 0))
    return 0;
  return 1;
}

static void
set_up_rw_methods_mutex(IC_INT_CONNECTION *conn, gboolean is_mutex_used)
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
init_connect_stat(IC_INT_CONNECTION *conn)
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

/* Implements ic_fork_accept_connection */
IC_CONNECTION*
fork_accept_connection(IC_CONNECTION *ext_orig_conn,
                       gboolean use_mutex)
{
  IC_INT_CONNECTION *orig_conn= (IC_INT_CONNECTION*)ext_orig_conn;
  IC_INT_CONNECTION *fork_conn;
  int size_object= orig_conn->is_ssl_connection ?
                   sizeof(IC_SSL_CONNECTION) : sizeof(IC_INT_CONNECTION);
  DEBUG_ENTRY("fork_accept_connection");

  if ((fork_conn= (IC_INT_CONNECTION*)ic_malloc_conn(size_object)) == NULL)
    goto end;

  memcpy(fork_conn, orig_conn, size_object);
  if (orig_conn->read_buf_size)
  {
    fork_conn->read_buf_size= orig_conn->read_buf_size;
    fork_conn->read_buf_pos= 0;
    fork_conn->size_curr_read_buf= 0;
    if ((fork_conn->read_buf= ic_calloc_conn(orig_conn->read_buf_size)) ==
         NULL)
    fork_conn->write_buf_size= orig_conn->write_buf_size;
    fork_conn->write_buf_pos= 0;
    if ((fork_conn->write_buf= ic_calloc_conn(orig_conn->write_buf_size)) ==
         NULL)
      goto end;
  }

  init_connect_stat(fork_conn);

  /* Copy human-readable server and client addresses */
  memcpy(&fork_conn->conn_stat.server_ip_addr_str[0],
         &orig_conn->conn_stat.server_ip_addr_str[0],
         sizeof(orig_conn->conn_stat.server_ip_addr_str));
  memcpy(&fork_conn->conn_stat.client_ip_addr_str[0],
         &orig_conn->conn_stat.client_ip_addr_str[0],
         sizeof(orig_conn->conn_stat.client_ip_addr_str));

  fork_conn->orig_conn= orig_conn;
  fork_conn->is_nonblocking= orig_conn->is_nonblocking;
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
  fork_conn->err_str= NULL;
  fork_conn->error_line= 0;
  fork_conn->is_listen_socket_retained= FALSE;
  fork_conn->is_mutex_used= FALSE;
  fork_conn->is_connect_thread_used= FALSE;
  fork_conn->is_mutex_used= use_mutex;
  fork_conn->ret_client_addrinfo= NULL;
  fork_conn->ret_server_addrinfo= NULL;
  fork_conn->thread= NULL;
  fork_conn->param= NULL;
  orig_conn->forked_connections++;

  fork_conn->conn_op.ic_write_connection= write_socket_connection;
  fork_conn->conn_op.ic_flush_connection= flush_connection;
  set_up_rw_methods_mutex(fork_conn, fork_conn->is_mutex_used);

  if (create_mutexes(fork_conn))
    goto error;
  if (create_timers(fork_conn))
    goto error;
  DEBUG_RETURN_PTR((IC_CONNECTION*)fork_conn);

error:
  destroy_mutexes(fork_conn);
  ic_free_conn(fork_conn);
end:
  DEBUG_RETURN_PTR(NULL);
}

/* Implements ic_prepare_client_connection */
static void
prepare_client_connection(IC_CONNECTION *ext_conn,
                          gchar *server_name,
                          gchar *server_port,
                          gchar *client_name,
                          gchar *client_port)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->server_name= server_name;
  conn->server_port= server_port;
  conn->client_name= client_name;
  conn->client_port= client_port;
}

/* Implements ic_prepare_server_connection */
static void
prepare_server_connection(IC_CONNECTION *ext_conn,
                          gchar *server_name,
                          gchar *server_port,
                          gchar *client_name,
                          gchar *client_port,
                          int backlog,
                          gboolean is_listen_socket_retained)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  prepare_client_connection(ext_conn, server_name, server_port,
                            client_name, client_port);
  if (backlog == 0)
    backlog= 10;
  conn->backlog= backlog;
  conn->is_listen_socket_retained= is_listen_socket_retained;
}

/* Implements ic_prepare_extra_parameters */
static void
prepare_extra_parameters(IC_CONNECTION *ext_conn,
                         guint32 tcp_maxseg,
                         gboolean is_wan_connection,
                         guint32 tcp_receive_buffer_size,
                         guint32 tcp_send_buffer_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  if (tcp_maxseg == 0)
  {
  }
  else
    conn->tcp_maxseg_size= tcp_maxseg;
  conn->is_wan_connection= is_wan_connection;
  if (tcp_receive_buffer_size == 0)
  {
  }
  else
    conn->tcp_receive_buffer_size= tcp_receive_buffer_size;
  if (tcp_send_buffer_size == 0)
  {
  }
  else
    conn->tcp_send_buffer_size= tcp_send_buffer_size;
}

/* Implements ic_set_param */
static void
set_param(IC_CONNECTION *ext_conn, void *param)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->param= param;
}

/* Implements ic_get_param */
static void*
get_param(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return conn->param;
}

/* Implements ic_get_error_code */
static int
get_error_code(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return conn->error_code;
}

static const gchar*
get_error_str(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return conn->err_str;
}

static gchar*
fill_error_buffer(IC_CONNECTION *ext_conn,
                  int error_code,
                  gchar *error_buffer)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return ic_common_fill_error_buffer(conn->err_str,
                                     conn->error_line,
                                     error_code,
                                     error_buffer);
}

static void
set_error_line(IC_CONNECTION *ext_conn, guint32 error_line)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->error_line= error_line;
}

static void
set_nonblocking_flag(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->is_nonblocking= TRUE;
}

static int
get_fd(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return conn->rw_sockfd;
}

static guint32
get_port_number(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  IC_IP_ADDRESS sa_storage;
  struct sockaddr *sa_addr= (struct sockaddr*)&sa_storage;
  IC_SOCKLEN_TYPE sock_len;
  guint32 port_number;
  int fd= conn->is_client ? conn->rw_sockfd : conn->listen_sockfd;

  sock_len= sizeof(sa_storage);
  if (getsockname(fd, sa_addr, &sock_len) == (int)-1)
    return 0;
  if (sa_addr->sa_family == AF_INET)
  {
    struct sockaddr_in *ipv4_addr= (struct sockaddr_in*)sa_addr;
    port_number= ntohs(ipv4_addr->sin_port);
  }
  else if (sa_addr->sa_family == AF_INET6)
  {
    struct sockaddr_in6 *ipv6_addr= (struct sockaddr_in6*)sa_addr;
    port_number= ntohs(ipv6_addr->sin6_port);
  }
  else
    return 0;
  return port_number;
}

static void
set_rec_wait_ms(IC_CONNECTION *ext_conn, int rec_wait_ms)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  conn->rec_wait_ms= rec_wait_ms;
}

static int
get_rec_wait_ms(IC_CONNECTION *ext_conn)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;

  return conn->rec_wait_ms;
}

IC_CONNECTION*
int_create_socket_object(gboolean is_client,
                         gboolean is_mutex_used,
                         gboolean is_connect_thread_used,
                         gboolean is_ssl,
                         gboolean is_ssl_used_for_data,
                         guint32 read_buf_size)
{
  int size_object= is_ssl ?
      sizeof(IC_SSL_CONNECTION) : sizeof(IC_INT_CONNECTION);
  IC_INT_CONNECTION *conn;
  DEBUG_ENTRY("int_create_socket_object");
  DEBUG_PRINT(COMM_LEVEL, ("Creating new socket, is_client = %u",
                           is_client));

  if ((conn= (IC_INT_CONNECTION*)ic_calloc_conn(size_object)) == NULL)
    goto end;
  if (read_buf_size)
  {
    conn->read_buf_size= read_buf_size;
    if ((conn->read_buf= ic_calloc_conn(read_buf_size)) == NULL)
    {
      ic_free_conn((gchar*)conn);
      goto end;
    }
    conn->write_buf_size= read_buf_size;
    if ((conn->write_buf= ic_calloc_conn(read_buf_size)) == NULL)
    {
      ic_free_conn(conn->read_buf);
      ic_free_conn((gchar*)conn);
      goto end;
    }
  }
  conn->conn_op.ic_set_up_connection= set_up_socket_connection;
  conn->conn_op.ic_accept_connection= accept_socket_connection;
  conn->conn_op.ic_check_connection= check_connection;
  conn->conn_op.ic_close_connection= close_socket_connection;
  conn->conn_op.ic_close_listen_connection= close_listen_socket_connection;
  conn->conn_op.ic_read_connection = read_socket_connection;
  conn->conn_op.ic_check_for_data= check_for_data_on_connection;
  conn->conn_op.ic_cmp_connection= cmp_hostname_and_port;
  conn->conn_op.ic_free_connection= free_socket_connection;
  conn->conn_op.ic_read_stat_connection= read_stat_socket_connection;
  conn->conn_op.ic_safe_read_stat_connection= safe_read_stat_socket_conn;
  conn->conn_op.ic_read_connection_time= read_socket_connection_time;
  conn->conn_op.ic_read_stat_time= read_socket_stat_time;
  conn->conn_op.ic_write_stat_connection= write_stat_socket_connection;
  conn->conn_op.ic_get_error_str= get_error_str;
  conn->conn_op.ic_fork_accept_connection= fork_accept_connection;
  conn->conn_op.ic_prepare_server_connection= prepare_server_connection;
  conn->conn_op.ic_prepare_client_connection= prepare_client_connection;
  conn->conn_op.ic_prepare_extra_parameters= prepare_extra_parameters;
  conn->conn_op.ic_set_param= set_param;
  conn->conn_op.ic_get_param= get_param;
  conn->conn_op.ic_get_error_code= get_error_code;
  conn->conn_op.ic_get_error_str= get_error_str;
  conn->conn_op.ic_fill_error_buffer= fill_error_buffer;
  conn->conn_op.ic_set_error_line= set_error_line;
  conn->conn_op.ic_set_nonblocking= set_nonblocking_flag;
  conn->conn_op.ic_get_fd= get_fd;
  conn->conn_op.ic_set_rec_wait_ms= set_rec_wait_ms;
  conn->conn_op.ic_get_rec_wait_ms= get_rec_wait_ms;
  conn->conn_op.ic_set_ssl_used_for_data= set_ssl_used_for_data;
  conn->conn_op.ic_reset_ssl_used_for_data= reset_ssl_used_for_data;

  conn->is_ssl_connection= is_ssl;
  conn->is_ssl_used_for_data= is_ssl_used_for_data;
  conn->save_is_ssl_used_for_data= is_ssl_used_for_data;
  conn->is_client= is_client;
  conn->is_connect_thread_used= is_connect_thread_used;
  conn->backlog= 1;
  conn->is_mutex_used= is_mutex_used;
  conn->rec_wait_ms= 10000; /* Default wait 10 sec for protocol reads */

  set_up_rw_methods_mutex(conn, is_mutex_used);
  conn->conn_op.ic_write_connection= write_socket_connection;
  conn->conn_op.ic_writev_connection= writev_socket_connection;
  conn->conn_op.ic_flush_connection= flush_connection;
  conn->conn_op.ic_get_port_number= get_port_number;

  init_connect_stat(conn);

  if (create_mutexes(conn))
    goto error;
  if (create_timers(conn))
    goto error;
  DEBUG_RETURN_PTR((IC_CONNECTION*)conn);
error:
  if (conn)
    destroy_mutexes(conn);
  if (conn->read_buf)
    ic_free_conn(conn->read_buf);
  if (conn->write_buf)
    ic_free_conn(conn->write_buf);
  ic_free_conn(conn);
end:
  DEBUG_RETURN_PTR(NULL);
}

IC_CONNECTION*
ic_create_socket_object(gboolean is_client,
                        gboolean is_mutex_used,
                        gboolean is_connect_thread_used,
                        guint32 read_buf_size)
{
  IC_CONNECTION *conn_ptr;
  DEBUG_ENTRY("ic_create_socket_object");
  conn_ptr= int_create_socket_object(is_client,
                                     is_mutex_used,
                                     is_connect_thread_used,
                                     FALSE,
                                     FALSE,
                                     read_buf_size);
  DEBUG_RETURN_PTR(conn_ptr);
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
  DEBUG_ENTRY("ic_ssl_init");

  if (!SSL_library_init())
  {
    DEBUG_RETURN_INT(1);
  }
  if (!((dh_512= DH_new()) &&
      (dh_1024= DH_new())))
  {
    DEBUG_RETURN_INT(1);
  }
  /*
    Check if BIN_bin2bn allocates memory, if so we need to deallocate it in
    the end routine.
  */
  if (!((dh_512->p= BN_bin2bn(dh_512_prime, sizeof(dh_512_prime), NULL)) &&
        (dh_1024->p= BN_bin2bn(dh_1024_prime, sizeof(dh_1024_prime), NULL)) &&
        (dh_512->g= BN_bin2bn(dh_512_group, 1, NULL)) &&
        (dh_1024->g= BN_bin2bn(dh_1024_group, 1, NULL))))
  {
    DEBUG_RETURN_INT(1);
  }
  /* Provide random entropy */
  if (!RAND_load_file("/dev/urandom", 1024))
  {
    DEBUG_RETURN_INT(1);
  }
  DEBUG_RETURN_INT(0);
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
    ic_free_conn(conn->root_certificate_path.str);
  if (conn->loc_certificate_path.str)
    ic_free_conn(conn->loc_certificate_path.str);
  if (conn->passwd_string.str)
    ic_free_conn(conn->passwd_string.str);
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

  ic_printf("verify callback %d", ok);
  if (ok == 0)
  {
    X509 *cert= X509_STORE_CTX_get_current_cert(ctx_store);
    int depth= X509_STORE_CTX_get_error_depth(ctx_store);
    int error= X509_STORE_CTX_get_error(ctx_store);

    ic_printf("Error with certificate at depth %d", depth);
    X509_NAME_oneline(X509_get_issuer_name(cert), buf, 512);
    ic_printf("Issuer = %s", buf);
    X509_NAME_oneline(X509_get_subject_name(cert), buf, 512);
    ic_printf("Subject = %s", buf);
    ic_printf("Error %d, %s", error, X509_verify_cert_error_string(error));
  }
  return ok;
}

static DH*
ssl_get_dh_callback(SSL *ssl,
                    int is_export,
                    int key_length)
{
  (void)ssl;
  (void)is_export;

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
  IC_INT_CONNECTION *sock_conn= (IC_INT_CONNECTION*)conn;

  /* Create an SSL context for this socket connection. */
  if (!(conn->ssl_ctx= SSL_CTX_new(SSLv3_method())))
    goto error_handler;

  /* We specify where the root certificate is stored. */
  if (((error= SSL_CTX_load_verify_locations(conn->ssl_ctx,
                                             conn->root_certificate_path.str,
                                             NULL)) != IC_SSL_SUCCESS))
    goto error_handler;
  if ((error= SSL_CTX_set_default_verify_paths(conn->ssl_ctx))
               != IC_SSL_SUCCESS)
    goto error_handler;

  /*
    Set-up password to use for this SSL session object. This password is the
    private key which the SSL certificates have been signed with.
  */
  buf_ptr= ic_get_ic_string(&conn->passwd_string, buf);
  SSL_CTX_set_default_passwd_cb_userdata(conn->ssl_ctx, (void*)buf_ptr);

  /*
    We use a local certificate chain file, This file is also the file where
    the private key is stored.
  */
  loc_cert_file= &conn->loc_certificate_path;
  if (((error= SSL_CTX_use_certificate_chain_file(conn->ssl_ctx,
                                                 loc_cert_file->str)) !=
                                                 IC_SSL_SUCCESS))
    goto error_handler;
  if (((error= SSL_CTX_use_PrivateKey_file(conn->ssl_ctx,
                                           loc_cert_file->str,
                                           SSL_FILETYPE_PEM)) !=
                                           IC_SSL_SUCCESS))
    goto error_handler;

  /*
    Specify that we want to verify the other side. Fail if the other
    connecting side don't supply a certificate. Thus both sides need
    to supply a certificate in the connect phase.
  */
  SSL_CTX_set_verify(conn->ssl_ctx,
                     SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     ic_ssl_verify_callback);
  SSL_CTX_set_verify_depth(conn->ssl_ctx, 3);

  /*
    SHA1 ciphering of the highest strength will be used to cipher the
    communication on the channel.
  */
  if ((error= SSL_CTX_set_cipher_list(conn->ssl_ctx, "SHA1:@STRENGTH")) !=
              IC_SSL_SUCCESS)
    goto error_handler;

  /*
    We load the Diffie-Hellman keys used for key management, this is only
    performed by the client part of the connection.
  */
  if (sock_conn->is_client)
    SSL_CTX_set_tmp_dh_callback(conn->ssl_ctx, ssl_get_dh_callback);

  if (!(conn->ssl_conn= SSL_new(conn->ssl_ctx)))
    goto error_handler;
  ic_printf("Successfully created a new SSL session");
  if (!SSL_set_fd(conn->ssl_conn, sock_conn->rw_sockfd))
  {
    error= SSL_get_error(conn->ssl_conn, 0);
    goto error_handler;
  }
  ic_printf("SSL session created");
  if (sock_conn->is_client)
    error= SSL_connect(conn->ssl_conn);
  else
    error= SSL_accept(conn->ssl_conn);
  ic_printf("SSL session connected");
  error= SSL_get_error(conn->ssl_conn, error);
  if (error == SSL_ERROR_NONE)
  {
    ic_printf("SSL Success");
    error= 0;
  }
  else
  {
    /* Go through SSL error stack */
    ic_printf("SSL Error: %d", error);
    goto error_handler;
  }

  return 0;
error_handler:
  ic_printf("Session creation failed");
  free_ssl_session(conn);
  return 1;
}

/* Implements ic_set_up_connection */
static int
set_up_ssl_connection(IC_CONNECTION *ext_in_conn,
                      accept_timeout_func timeout_func,
                      void *timeout_obj)
{
  int error= 1;
  IC_INT_CONNECTION *in_conn= (IC_INT_CONNECTION*)ext_in_conn;
  DEBUG_ENTRY("set_up_ssl_connection");

  lock_connect_mutex(in_conn);
  error= set_up_socket_connection(ext_in_conn, timeout_func, timeout_obj);
  unlock_connect_mutex(in_conn);
  DEBUG_RETURN_INT(error);
}

/* Implements ic_accept_connection */
static int
accept_ssl_connection(IC_CONNECTION *ext_conn)
{
  int error;
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  DEBUG_ENTRY("accept_ssl_connection");

  lock_connect_mutex(conn);
  error= accept_socket_connection(ext_conn);
  unlock_connect_mutex(conn);
  DEBUG_RETURN_INT(error);
}

/* Implements ic_free_connection */
static void
free_ssl_connection(IC_CONNECTION *ext_in_conn)
{
  IC_INT_CONNECTION *in_conn= (IC_INT_CONNECTION*)ext_in_conn;
  IC_SSL_CONNECTION *conn= (IC_SSL_CONNECTION*)in_conn;
  DEBUG_ENTRY("free_ssl_connection");

  /* Free SSL related parts of the connection */
  free_ssl_session(conn);
  /* Free normal socket connection parts */
  free_socket_connection(ext_in_conn);
  DEBUG_RETURN_EMPTY;
}

/* Implements ic_fork_accept_connection */
static IC_CONNECTION*
fork_accept_ssl_connection(IC_CONNECTION *ext_orig_conn,
                           gboolean use_mutex)
{
  IC_INT_CONNECTION *orig_conn= (IC_INT_CONNECTION*)ext_orig_conn;
  IC_CONNECTION *new_conn;
  IC_SSL_CONNECTION *new_ssl_conn;
  IC_SSL_CONNECTION *orig_ssl_conn= (IC_SSL_CONNECTION*)ext_orig_conn;
  DEBUG_ENTRY("fork_accept_ssl_connection");

  lock_connect_mutex(orig_conn);
  if ((new_conn= fork_accept_connection(ext_orig_conn, use_mutex)))
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
  DEBUG_RETURN_PTR(new_conn);

error_handler:
  unlock_connect_mutex(orig_conn);
  free_ssl_connection(new_conn);
  DEBUG_RETURN_PTR(NULL);
}

/* Implements ic_read_connection */
static int
read_ssl_connection(IC_CONNECTION *ext_conn, void *buf, guint32 buf_size,
                    guint32 *read_size)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  int error;
  DEBUG_ENTRY("read_ssl_connection");

  lock_connect_mutex(conn);
  error= read_socket_connection(ext_conn, buf, buf_size, read_size);
  unlock_connect_mutex(conn);
  DEBUG_RETURN_INT(error);
}

/* Implements ic_write_connection */
static int
write_ssl_connection(IC_CONNECTION *ext_conn,
                     const void *buf, guint32 size,
                     guint32 secs_to_try)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  int error;
  DEBUG_ENTRY("write_ssl_connection");

  lock_connect_mutex(conn);
  error= write_socket_connection(ext_conn, buf, size, secs_to_try);
  unlock_connect_mutex(conn);
  DEBUG_RETURN_INT(error);
}

/* Implements ic_writev_connection */
static int
writev_ssl_connection(IC_CONNECTION *ext_conn,
                      IC_IOVEC *write_vector, guint32 iovec_size,
                      guint32 tot_size,
                      guint32 secs_to_try)
{
  IC_INT_CONNECTION *conn= (IC_INT_CONNECTION*)ext_conn;
  int error;
  DEBUG_ENTRY("writev_ssl_connection");

  lock_connect_mutex(conn);
  error= writev_socket_connection(ext_conn, write_vector, iovec_size,
                                  tot_size, secs_to_try);
  unlock_connect_mutex(conn);
  DEBUG_RETURN_INT(error);
}

IC_CONNECTION*
ic_create_ssl_object(gboolean is_client,
                     IC_STRING *root_certificate_path,
                     IC_STRING *loc_certificate_path,
                     IC_STRING *passwd_string,
                     gboolean is_ssl_used_for_data,
                     gboolean is_connect_thread_used,
                     guint32 read_buf_size)
{
  IC_INT_CONNECTION *conn;
  IC_CONNECTION *ext_conn;
  IC_SSL_CONNECTION *ssl_conn;
  DEBUG_ENTRY("ic_create_ssl_object");

  if (!(ext_conn= int_create_socket_object(is_client, FALSE,
                                           is_connect_thread_used,
                                           TRUE, is_ssl_used_for_data,
                                           read_buf_size)))
  {
    DEBUG_RETURN_PTR(NULL);
  }
  ssl_conn= (IC_SSL_CONNECTION*)ext_conn;
  conn= (IC_INT_CONNECTION*)ext_conn;
  if (ic_strdup(&ssl_conn->root_certificate_path, root_certificate_path) ||
      ic_strdup(&ssl_conn->loc_certificate_path, loc_certificate_path) ||
      ic_strdup(&ssl_conn->passwd_string, passwd_string))
  {
    free_ssl_connection(ext_conn);
    DEBUG_RETURN_PTR(NULL);
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
  conn->conn_op.ic_write_connection= write_ssl_connection;
  conn->conn_op.ic_writev_connection= writev_ssl_connection;

  set_up_rw_methods_mutex(conn, FALSE);
  conn->conn_op.ic_fork_accept_connection= fork_accept_ssl_connection;
  DEBUG_RETURN_PTR(ext_conn);
}

static int
ic_ssl_write(IC_INT_CONNECTION *conn, const void *buf, guint32 buf_size)
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

static int
ic_ssl_read(IC_INT_CONNECTION *conn,
            void *buf, guint32 buf_size,
            guint32 *read_size)
{
  IC_SSL_CONNECTION *ssl_conn= (IC_SSL_CONNECTION*)conn;
  int ret_code= SSL_read(ssl_conn->ssl_conn, buf, buf_size);
  int error;

  ic_printf("Wrote %d bytes through SSL", buf_size);

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

#else
int ic_ssl_init()
{
  DEBUG_ENTRY("ic_ssl_init");
  DEBUG_PRINT(COMM_LEVEL, ("SSL not compiled in"));
  DEBUG_RETURN_INT(0);
}

void ic_ssl_end()
{
  return;
}
#endif
