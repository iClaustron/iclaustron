/* Copyright (C) 2007, 2008 iClaustron AB

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

#ifndef IC_CONNECTION_INT_H
#define IC_CONNECTION_INT_H

struct ic_int_connection;
typedef struct ic_int_connection IC_INT_CONNECTION;
struct ic_int_connection
{
  /* Public part */
  IC_CONNECTION_OPERATIONS conn_op;
  IC_CONNECT_STAT conn_stat;

  /* Private part */
  /* Used to pass objects to threads */
  void *param;
  /* This variable is set to the error code if an error occurs. */
  int error_code;
  const gchar *err_str;
  guint32 error_line;
  gchar err_buf[128];
  IC_INT_CONNECTION *orig_conn;
  guint64 cpu_bindings;
  GThread *thread;
  /* stop_flag is used to flag to connect thread to quit */
  gboolean stop_flag;
  /* Should connection be nonblocking */
  gboolean is_nonblocking;

  GMutex *read_mutex;
  GMutex *write_mutex;
  GMutex *connect_mutex;
  gchar *read_buf;
  guint32 read_buf_size;
  guint32 size_curr_read_buf;
  guint32 read_buf_pos;
  struct connection_buffer *first_con_buf;
  struct connection_buffer *last_con_buf;
  int rw_sockfd;
  int listen_sockfd;
  /*
    Milliseconds to wait until returning from connect attempt,
    0 means wait forever
  */
  int ms_wait;
  guint32 node_id;
  /*
    We keep track of time that the connection has been up and a timer for how
    long time has passed since last time the statistics was read. This timer
    is reset every time a call to read statistics is made.
    This is part of the statistics, but we don't want to export timers so we
    put it into the private part of the ic_connection class.
  */
  GTimer *connection_start;
  GTimer *last_read_stat;
  /*
    These are interface variables that are public. By setting
    those to proper values before calling set_up_ic_connection
    the behaviour of the ic_connection is changed.
  */

  /*
    This variable is set after returning an error in the write socket call.
    It provides the caller the number of bytes sent before the call returned
    in error. It only has a valid value if the write call returned in error.
  */
  guint32 bytes_written_before_interrupt;
  /*
    backlog is the parameter provided to the listen call, thus
    this is only applicable for server side connections.
  */
  int backlog;
  /*
    Server Connection:
    ------------------
    This is signalled by setting is_client to FALSE
    For a server connection the server name should be set to a proper ip
    address or to a hostname to bind the interface we are listening
    to. The server name can be a IPv4 or an IPv6 address or hostname.
    The server name and server port will be used by getaddrinfo to set the
    struct's used by the socket functions.

    For server connections the server name isn't allowed to be NULL. We desire
    more control over which interface is actually used.
    server_port must be set != "0" for server connections. This is the
    port we are listening to.
    If we allow any client to connect to the server we should set
    client_name to NULL and client_port to "0". If client_name is not NULL
    we will only accept connections from this IP address and if port != "0"
    we will only accept connections where the client is using this port.

    Client Connection:
    ------------------
    This is signalled by setting is_client to TRUE
    For a client connection the server_name and server_port must
    be set to proper values. Otherwise the client won't find
    the server side part of the connection.
    client_name can be set to NULL and in this case the operating system will
    select the interface to use for the connection. If an IP address/hostname
    is provided this will be used to select a proper interface for the
    connection.

    client_port can be set to "0" and in this case the ephemeral
    port will be used, meaning that the operating system will
    select a port from 1500 and upwards. If a port is provided
    then this is the port which will be used in the connect
    message to the server part of the connection.
  */
  gchar *server_name;
  gchar *client_name;
  gchar *server_port;
  gchar *client_port;
  struct addrinfo *server_addrinfo;
  struct addrinfo *client_addrinfo;
  struct addrinfo *ret_server_addrinfo;
  struct addrinfo *ret_client_addrinfo;
  guint16 server_port_num;
  guint16 client_port_num;
  guint32 forked_connections;
  gboolean is_client;

  /*
    In some cases the application logic requires some authentication
    processing to occur after completing all parts of the connection
    set-up. This is signalled through the use of a user-supplied
    authentication function.
  */
  authenticate_func auth_func;
  void *auth_obj;

  /*
    When calling ic_set_up_connection on the server side we want to ensure
    that we can stop the server connection by calling a callback function
    defined here. It requires an object along with it, it the function will
    also receive a counter of how many seconds we have been waiting. This
    function will be called once a second when waiting for a client to
    connect.
  */
  accept_timeout_func timeout_func;
  void *timeout_obj;

  /*
    The normal way to use this connection API is to listen to a
    socket and then close the listening socket after accept is
    called, this flag set to true means that we will call accept
    separately and that we don't close the listening thread after
    a successful accept. If used the user must close the listening
    connection in a separate call.
  */
  gboolean is_listen_socket_retained;
  /*
    If the connection is used in a multi-threaded environment
    we use a mutex before performing operations on the
    connection. There is a separate mutex for read and write
    operations. Thus we can handle read and write concurrently
    on the connection object.
  */
  gboolean is_mutex_used;
  /*
    To enable asynchronous operation we can perform the connect
    part in a separate thread.
  */
  gboolean is_connect_thread_used;
  /*
    TCP Maximum Segment Size
    This variable can be used to set a non-default MSS of the TCP
    connection. It is not possible to decrease the MSS, only
    increase it.
    This value is set to the used value after connection is established.
  */
  guint16 tcp_maxseg_size;
  /*
    TCP Send Buffer in kernel
    TCP Receive Buffer in kernel
    For LAN connections it is not likely that changing these variables
    will be of much value. However when used in a WAN context it is
    important that these sizes is large enough to handle a large window
    of outstanding messages not yet acknowledged. Thus on a WAN these
    socket options is important to get maximum throughput on the
    connection.
    
    NOTE:
    It is quite likely that you need to change the defaults on the OS
    to get optimal performance on WAN links. The socket options cannot
    set the value higher than the configuration variable rmem_max and
    wmem_max.
    E.g. in Fedora Core 5 these are changed by writing the following
    lines in the file /etc/sysctl.conf:
    net.core.rmem_max=4194304
    net.core.wmem_max=4194304
    This sets the maximum value to 4 MBytes.
    It is also required to set tcp_mem, tcp_rmem and tcp_wmem to proper
    values in those cases.  E.g again in /etc/sysctl.conf on Fedora Core 5
    net.ipv4.tcp_mem= 16384 174670 4194304
    net.ipv4.tcp_rmem= 16384 65536 4194304
    net.ipv4.tcp_wmem= 16384 65536 4194304
    The first parameter says the minimum size of the socket buffer in
    situations with high memory pressure, the second is the default
    size of the buffer and the last parameter is the maximum size it
    can be set to. This cannot be set higher than the values above so
    these must also be set.

    Check
    http://www-didc.lbl.gov/TCP-tuning/TCP-tuning.html
    for more information on how to tune those things on WAN connections.
    Also
    http://ipsysctl-tutorial.frozentux.net/chunkyhtml/tcpvariables.html
    gives good insights on other interesting tcp variables configurable
    by the OS. Especially tcp_adv_win_scale is of value to consider on
    WAN connections.

    What is said about WAN connections also refer to Gigabit Ethernet
    connections to some extent and most definitely to connections
    using Gigabit Ethernet with Jumbo Frames and even more so using
    10Gigabit Ethernet. In those cases the outstanding frames can be
    of substantial even on a highly loaded LAN.
  */
  int tcp_receive_buffer_size;
  int tcp_send_buffer_size;
  /*
    Another interface option here is to set a WAN flag and thus change
    the default behaviour. If this is set then this will be used to
    set up a proper value for TCP_MAXSEG, SO_RCVBUF, SO_SNDBUF. These
    values will be applied before the values in those variables. If these
    values have been set higher then they will be used instead of the
    default, otherwise the default value will be retained.
  */
#define WAN_REC_BUF_SIZE 4194304
#define WAN_SND_BUF_SIZE 4194304
#define WAN_TCP_MAXSEG_SIZE 61440
  gboolean is_wan_connection;
  gboolean is_ssl_connection;
  gboolean is_ssl_used_for_data;
};

#define IC_SSL_SUCCESS 1
#define IC_SSL_INDICATION 0

struct ic_ssl_connection
{
  IC_INT_CONNECTION socket_conn;
#ifdef HAVE_SSL
  /*
    This is a set of variables that define an SSL connection. 
    ssl_connection represents the SSL connection.
  */
  SSL_CTX *ssl_ctx;
  SSL *ssl_conn;
  DH *ssl_dh;

  IC_STRING root_certificate_path;
  IC_STRING loc_certificate_path;
  IC_STRING passwd_string;
#endif
};
typedef struct ic_ssl_connection IC_SSL_CONNECTION;
#endif
