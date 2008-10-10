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

#ifndef IC_COMM_H
#define IC_COMM_H

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ic_common_header.h>
#include <ic_ssl.h>
#include <glib.h>

#define ACCEPT_ERROR 32767
#define END_OF_FILE 32766
#define PROTOCOL_ERROR 32765
#define AUTHENTICATE_ERROR 32764
#define SSL_ERROR 32763

#ifdef USE_MSG_NOSIGNAL
#define IC_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define IC_MSG_NOSIGNAL 0
#endif

typedef int (*authenticate_func) (void*);

struct ic_connection;
struct ic_connect_stat;
struct ic_connect_operations
{
  /*
    After accept has received a connection it is possible to fork the
    connection and continue listening to the server port.
  */
  struct ic_connection* (*ic_fork_accept_connection)
                         (struct ic_connection *orig_conn,
                          gboolean use_mutex);
  /*
    Set up a connection object, this can be either the server end or
    the client end of the connection. For server end it is possible
    that this call doesn't perform the accept call.

    The necessary parameters to set-up before this call are:
      server_name : This is the server to connect to for client and it is the
                    server ip/name we will listen to as server
      server_ip   : This is the port we will connect to for clients and it is
                    the port we'll listen to for servers.

    For server connections we can also set:
      client_name: This is the only allowed client name that is allowed to connect
      client_port: This is the only allowed port from which clients can connect
      backlog    : Sets the backlog parameter, how many connections can the OS
                   keep in the backlog before refusing new connections.
      is_listen_socket_retained : Set if we don't want the listening socket to
                   be closed after accepting a connection. Useful when we use
                   one socket for multiple connections.
    For client connections we can also set:
      client_name: This is the hostname we will bind to before connecting.
                   If not set we will use any interface on the machine.
      client_ip  : This is the port we will bind to before connecting, if not
                   set we'll use the ephemeral port where the port is choosen
                   by the OS.
    In addition we can also set a number of performance related parameters
    that are useful both on the server and client side.
      tcp_maxseg :  Will set the parameter TCP_MAXSEG on the socket
      is_wan_connection : Will optimise parameters for a WAN connection
      tcp_receive_buffer_size : Receive buffer size allocated in OS kernel
      tcp_send_buffer_size : Send buffer size allocated in OS kernel

    In addition when we create the socket object we need to specify the
    following boolean variables:
    is_client : Is it a server socket or a client socket
    is_mutex_used : Is it necessary to protect all accesses by mutex
    is_connect_thread_used : Is it necessary to handle connects in a separate
                thread.
    Finally an authenticate function and object can be provided in the
    create socket call.
    We've added a number of initialisation support routines which should
    be used afyer ic_create_socket_object but before ic_set_up_connection.
    For ic_prepare_extra_parameters all parameters are optional. To set the
    default for optional parameters use NULL for pointers and 0 for 
    integer parameters. These prepare functions return an error code if
    the set-up used faulty parameters.
  */

  int (*ic_prepare_server_connection) (gchar *server_name,
                                      gchar *server_port,
                                      gchar *client_name, /* Optional */
                                      gchar *client_port, /* Optional */
                                      int backlog,        /* Optional */
                                      gboolean is_listen_socket_retained);
  int (*ic_prepare_client_connection) (gchar *server_name,
                                       gchar *server_port,
                                       gchar *client_name, /* Optional */
                                       gchar *client_port);/* Optional */
  int (*ic_prepare_extra_parameters) (guint32 tcp_maxseg,
                                      gboolean is_wan_connection,
                                      guint32 tcp_receive_buffer_size,
                                      guint32 tcp_send_buffer_size);

  int (*ic_set_up_connection) (struct ic_connection *conn);
  int (*ic_accept_connection) (struct ic_connection *conn);
  int (*ic_close_connection) (struct ic_connection *conn);
  int (*ic_close_listen_connection) (struct ic_connection *conn);
  /*
    These calls are used to read and write on a socket connection.
    There is support for memory buffering in front of the socket.
    In this case it is necessary to flush the memory buffer in order
    to ensure that buffered operations have actually been written.
  */
  int (*ic_read_connection) (struct ic_connection *conn,
                             void *buf, guint32 buf_size,
                             guint32 *read_size);
  int (*ic_write_connection) (struct ic_connection *conn,
                              const void *buf, guint32 size,
                              guint32 secs_to_try);
  int (*ic_writev_connection) (struct ic_connection *conn,
                               struct iovec *write_vector,
                               guint32 iovec_size,
                               guint32 tot_size,
                               guint32 secs_to_try);
  int (*ic_flush_connection) (struct ic_connection *conn);
  /* This call is used to check if there is data to be read on the socket */
  gboolean (*ic_check_for_data) (struct ic_connection *conn);
  /*
    In order to support multiple threads using the same connection
    object at the same time we need to declare open and close of
    read and write sessions. The socket connection can have a read
    session and a write session concurrently.
  */
  int (*ic_open_write_session) (struct ic_connection *conn,
                                guint32 total_size);
  int (*ic_close_write_session) (struct ic_connection *conn);
  int (*ic_open_read_session) (struct ic_connection *conn);
  int (*ic_close_read_session) (struct ic_connection *conn);
  /*
    Read statistics of the connection. This is done by copying the
    statistics information into the provided statistics object to ensure
    that the connection can continue to execute.
    The statistics isn't read with a mutex so in a multithreaded environment
    there is no guarantee that the figures are completely consistent with
    each other.
    If safe read is needed use the safe_read_stat_ic_connection method
    instead. The safe read cannot be used while a read or write session is
    ongoing by the same thread as is calling the safe read statistics. This
    would create a deadlock.
  */
  void (*ic_read_stat_connection) (struct ic_connection *conn,
                                   struct ic_connection *stat,
                                   gboolean clear_stat_timer);
  void (*ic_safe_read_stat_connection) (struct ic_connection *conn,
                                        struct ic_connection *stat,
                                        gboolean clear_stat_timer);
  /*
    Print statistics of the connection
  */
  void (*ic_write_stat_connection) (struct ic_connection *conn);
  /*
    These are two routines to read times in conjunction with this connect
    object.
    The first measures the time since object was created if not connected
    and time since connection was established.
    The second reads the time since last time statistics was read. This
    call should be done before calling read_stat_ic_connection if one is
    interested in time since last time this timer was cleared (can be
    cleared in read_stat_ic_connection call).
  */
  double (*ic_read_connection_time) (struct ic_connection *conn,
                                     gulong *microseconds);
  double (*ic_read_stat_time) (struct ic_connection *conn,
                               gulong *microseconds);
  gboolean (*ic_is_conn_connected) (struct ic_connection *conn);
  gboolean (*ic_is_conn_thread_active) (struct ic_connection *conn);
  /*
    Free all memory connected to a connection object
    It also closes all connection if not already done
  */
  void (*ic_free_connection) (struct ic_connection *conn);
};

struct ic_connect_stat
{
  /*
    These variables represent statistics about this connection. It keeps
    track of number of bytes sent and received. It keeps track of the sum
    of squares of these values as well to enable calculation of standard
    deviation.
    On top of this we also keep track of number of sent messages within
    size ranges. The first range is 0-31 bytes, the second 32-63 bytes,
    64-127 bytes and so forth upto the last range which is 512kBytes and
    larger messages.
    Also a similar array for received messages.
  */
  guint64 num_sent_buffers;
  guint64 num_sent_bytes;
  long double num_sent_bytes_square_sum;
  guint64 num_rec_buffers;
  guint64 num_rec_bytes;
  long double num_rec_bytes_square_sum;
  guint32 num_sent_buf_range[16];
  guint32 num_rec_buf_range[16];
  guint32 num_send_errors;
  guint32 num_send_timeouts;
  guint32 num_rec_errors;
  /*
    These variables represent the Socket options as they were actually set.
  */
  int used_tcp_receive_buffer_size;
  int used_tcp_send_buffer_size;
  guint32 used_tcp_maxseg_size;
  /*
    These variables represent the connection status at the moment, endpoints,
    if it is connected, if it is the server or client endpoint and if we have
    a connect thread active
  */
  gchar server_ip_addr_str[128];
  gchar client_ip_addr_str[128];
  gchar *server_ip_addr;
  gchar *client_ip_addr;
  gboolean is_client_used;
  gboolean is_connected;
  gboolean is_connect_thread_active;
  /*
    This variable is set to 1 except in error cases when TCP_NODELAY was not
    possible to set.
  */
  gboolean tcp_no_delay;
};
typedef struct ic_connect_stat IC_CONNECT_STAT;

struct ic_connect_mgr_operations
{
  int (*ic_add_connection) (struct ic_connection *conn);
  int (*ic_drop_connection) (struct ic_connection *conn);
  int (*ic_get_connection) (struct ic_connection **conn, guint32 node_id);
  int (*ic_get_free_connection) (struct ic_connection **conn);
};
typedef struct ic_connect_mgr_operations IC_CONNECT_MGR_OPERATIONS;

struct ic_connection
{
  /*
    These are a set of private variables used internally in the
    ic_connection class.
  */
  struct ic_connect_operations conn_op;
  struct ic_connect_stat conn_stat;
  void *param; /* Used to pass objects to threads */
  struct ic_connection *orig_conn;
  guint64 cpu_bindings;
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
    This variable is set to the error code if an error occurs.
  */
  int error_code;
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
typedef struct ic_connection IC_CONNECTION;

#define IC_SSL_SUCCESS 1
#define IC_SSL_ERROR -1
#define IC_SSL_INDICATION 0

struct ic_ssl_connection
{
  IC_CONNECTION socket_conn;
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

/*
  There are three levels in the connection set-up. The connection set-up will
  always start by performing a normal TCP/IP connection. When this connection
  is up one will create an SSL session if we created a SSL socket object.
  The final and third phase is application specific authentication logic
  which is also optional.

  In addition the connect can be performed in a separate thread. In this case
  the application should not use the connection other than checking whether
  the connection is connected until the connected flag is set.

  When the connection has been set-up there is the possibility to use a front
  buffer. If this is used there is a specific flush call to ensure no data is
  left in the front buffer.

  For SSL connection one can use whether the data transport should be encrypted
  or not. The application authentication part will always be encrypted since it
  will often entail sending passwords.

  All socket objects gather statistics about its usage and there is a set of
  calls to gather this statistics.

  It is possible to set-up a listening socket and then create new socket objects
  each time someone connects to this listening socket. Each socket object needs
  to be free'd independent of how they were created.
*/
IC_CONNECTION *ic_create_socket_object(gboolean is_client,
                                       gboolean is_mutex_used,
                                       gboolean is_connect_thread_used,
                                       guint32 read_buf_size,
                                       authenticate_func func,
                                       void *auth_obj);

IC_CONNECTION *ic_create_ssl_object(gboolean is_client,
                                    IC_STRING *root_certificate_path,
                                    IC_STRING *loc_certification_path,
                                    IC_STRING *passwd_string,
                                    gboolean is_ssl_used_for_data,
                                    gboolean is_connect_thread_used,
                                    guint32 read_buf_size,
                                    authenticate_func func,
                                    void *auth_obj);

struct ic_connect_manager
{
  struct ic_connection **ic_con_array;
  GMutex *icm_mutex;
};
typedef struct ic_connect_manager IC_CONNECT_MANAGER;

/*
  Definitions for the socket buffer pool
*/
#define PRIO_LEVELS 2
#define MAX_ALLOC_SEGMENTS 8
#define HIGH_PRIO_BUF_SIZE 128
struct ic_sock_buf_operations;
struct ic_sock_buf_page;
struct ic_sock_buf;

struct ic_sock_buf_operations
{
  /*
    This function retrieves one socket buffer page from the free list.
    It does so by using a local free list provided in the free_pages
    variable, this variable is purely local to the thread and need no
    extra mutex protection. When this free list is empty a page is
    retrieved from the global free list. This allocation actually
    allocates as many pages as is requested in the num_pages variable.
    Thus if num_pages is 10, this routine will use the local free_pages
    free list 90% of the time and every 10th time the function is called
    it will allocate 10 socket buffer pages from the global free list.
  */
  struct ic_sock_buf_page* (*ic_get_sock_buf_page)
      (struct ic_sock_buf *buf,
       struct ic_sock_buf_page **free_pages,
       guint32 num_pages);
  /*
    This routine is used return socket buffer pages to the global free list.
    It will treat the pointer to the first socket buffer page as the first
    page in a linked list of pages. Thus more than one page at a time can
    be returned to the global free list.
  */
  void (*ic_return_sock_buf_page) (struct ic_sock_buf *buf,
                                   struct ic_sock_buf_page *page);
  /*
    If the socket buffer page global free list turns out to be too small,
    it is possible to increase the size by allocating more socket buffer
    pages. This is done in increments rather than reallocating everything.
  */
  int (*ic_inc_sock_buf) (struct ic_sock_buf *buf, guint64 no_of_pages);
  /*
    This routine frees all socket buffer pages allocated to this global pool.
  */
  void (*ic_free_sock_buf) (struct ic_sock_buf *buf);
};
typedef struct ic_sock_buf_operations IC_SOCK_BUF_OPERATIONS;

/*
  We want to ensure that the buffer is 64 bytes to avoid false cache sharing
  on most platforms (there are some exceptions using 256 byte cache line sizes
  that we will ignore).
*/
#define IC_SOCK_BUF_PAGE_SIZE 64
struct ic_sock_buf_page
{
  struct ic_sock_buf_page *next_sock_buf_page;
  struct ic_sock_buf *sock_buf_container;
  gchar *sock_buf;
  guint32 size;
  /* This is an atomic counter used to keep track of reference count */
  gint ref_count;
  guint32 opaque_area[6];
};
typedef struct ic_sock_buf_page IC_SOCK_BUF_PAGE;

struct ic_sock_buf
{
  IC_SOCK_BUF_OPERATIONS sock_buf_op;
  IC_SOCK_BUF_PAGE *first_page;
  guint32 page_size;
  guint32 alloc_segments;
  gchar *alloc_segments_ref[MAX_ALLOC_SEGMENTS];
  GMutex *ic_buf_mutex;
};
typedef struct ic_sock_buf IC_SOCK_BUF;

IC_SOCK_BUF*
ic_create_socket_membuf(guint32 page_size,
                        guint64 no_of_pages);

/*
  Debug print-outs
*/
void ic_print_buf(char *buf, guint32 size);
/*
  Methods to encode and decode base64 data
*/
int ic_base64_encode(guint8 **dest, guint32 *dest_len,
                     const guint8 *src, guint32 src_len);
int ic_base64_decode(guint8 *dest, guint32 *dest_len,
                     const guint8 *src, guint32 src_len);

/*
  Methods to send and receive buffers with Carriage Return
*/
int ic_send_with_cr(IC_CONNECTION *conn, const gchar *buf);
int ic_rec_with_cr(IC_CONNECTION *conn,
                   gchar **rec_buf,
                   guint32 *read_size);

/*
  Methods to handle conversion to integers from strings
*/
guint32 ic_count_characters(gchar *str, guint32 max_chars);
gboolean convert_str_to_int_fixed_size(char *str, guint32 num_chars,
                                       guint64 *ret_number);

#define SPACE_CHAR (gchar)32
#define CARRIAGE_RETURN (gchar)10
#define LINE_FEED (gchar)13
#define NULL_BYTE (gchar)0
#endif

