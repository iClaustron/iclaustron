#ifndef IC_COMM_H
#define IC_COMM_H

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ic_common_header.h>

#define MEM_ALLOC_ERROR 32767
#define ACCEPT_ERROR 32766
#define END_OF_FILE 32765
#define PROTOCOL_ERROR 32764
#define AUTHENTICATE_ERROR 32763

#ifdef USE_MSG_NOSIGNAL
#define IC_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define IC_MSG_NOSIGNAL 0
#endif

typedef int (*authenticate_func) (void *);

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
        gboolean use_mutex,
        gboolean use_front_buffer);
  /*
    Set up a connection object, this can be either the server end or
    the client end of the connection. For server end it is possible
    that this call doesn't perform the accept call.
  */
  int (*set_up_ic_connection) (struct ic_connection *conn);
  int (*accept_ic_connection) (struct ic_connection *conn);
  int (*close_ic_connection) (struct ic_connection *conn);
  int (*close_ic_listen_connection) (struct ic_connection *conn);
  /*
    These calls are used to read and write on a socket connection.
    There is support for memory buffering in front of the socket.
    In this case it is necessary to flush the memory buffer in order
    to ensure that buffered operations have actually been written.
  */
  int (*read_ic_connection) (struct ic_connection *conn,
                             void *buf, guint32 buf_size,
                             guint32 *read_size);
  int (*write_ic_connection) (struct ic_connection *conn,
                              const void *buf, guint32 size,
                              guint32 prio_level,
                              guint32 secs_to_try);
  int (*flush_ic_connection) (struct ic_connection *conn);
  /*
    In order to support multiple threads using the same connection
    object at the same time we need to declare open and close of
    read and write sessions. The socket connection can have a read
    session and a write session concurrently.
  */
  int (*open_write_session) (struct ic_connection *conn,
                             guint32 total_size);
  int (*close_write_session) (struct ic_connection *conn);
  int (*open_read_session) (struct ic_connection *conn);
  int (*close_read_session) (struct ic_connection *conn);
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
  void (*read_stat_ic_connection) (struct ic_connection *conn,
                                   struct ic_connection *stat,
                                   gboolean clear_stat_timer);
  void (*safe_read_stat_ic_connection) (struct ic_connection *conn,
                                        struct ic_connection *stat,
                                        gboolean clear_stat_timer);
  /*
    Print statistics of the connection
  */
  void (*write_stat_ic_connection) (struct ic_connection *conn);
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
  gboolean (*is_ic_conn_connected) (struct ic_connection *conn);
  gboolean (*is_ic_conn_thread_active) (struct ic_connection *conn);
  /*
    Free all memory connected to a connection object
  */
  void (*free_ic_connection) (struct ic_connection *conn);
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
  int (*add_ic_connection) (struct ic_connection *conn);
  int (*drop_ic_connection) (struct ic_connection *conn);
  int (*get_ic_connection) (struct ic_connection **conn, guint32 node_id);
  int (*get_free_ic_connection) (struct ic_connection **conn);
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
  struct ic_connection *orig_conn;
  guint64 cpu_bindings;
  GMutex *read_mutex;
  GMutex *write_mutex;
  GMutex *connect_mutex;
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
    In some cases it is desirable to do some authentication
    processing before accepting a connection already set-up.
    In this case there is a callback function plus a callback
    object defined which is used for this purpose.
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
    If this variable is set we will use a front memory buffer such
    that before writing on the socket we will write into a buffer.
    We will only write to the socket if the memory buffer is full.
    We will also write to the socket when a flush call is made.
  */
  gboolean is_using_front_buffer;
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
};
typedef struct ic_connection IC_CONNECTION;

IC_CONNECTION *ic_create_socket_object(gboolean is_client,
                                       gboolean is_mutex_used,
                                       gboolean is_connect_thread_used,
                                       gboolean is_using_front_buffer,
                                       authenticate_func func,
                                       void *auth_obj);

struct ic_connect_manager
{
  struct ic_connection **ic_con_array;
  GMutex *icm_mutex;
};
typedef struct ic_connect_manager IC_CONNECT_MANAGER;

/*
  Definitions for the front buffer pool on the connection objects
*/
#define PRIO_LEVELS 2
#define MAX_ALLOC_SEGMENTS 8
#define HIGH_PRIO_BUF_SIZE 128
struct ic_sock_buf_operations;
struct ic_sock_buf_page;
struct ic_sock_buf;

struct ic_sock_buf_operations
{
  gboolean (*get_ic_conn_buf) (struct ic_sock_buf *buf,
                               struct ic_sock_buf_page **page);
  gboolean (*return_ic_conn_buf) (struct ic_sock_buf *buf,
                                  struct ic_sock_buf_page *page);
  gboolean (*increase_ic_conn_buf) (guint32 no_of_pages);
};
typedef struct ic_sock_buf_operations IC_SOCK_BUF_OPERATIONS;

struct ic_sock_buf_page
{
  char *prio_ref[PRIO_LEVELS];
  guint32 prio_size[PRIO_LEVELS];
  struct ic_sock_buf_page *next_sock_buf_page;
  struct ic_sock_buf_page *prev_sock_buf_page;
};
typedef struct ic_sock_buf_page IC_SOCK_BUF_PAGE;

struct ic_sock_buf
{
  struct ic_sock_buf_operations sock_buf_op;
  struct ic_sock_buf_page *first_page;
  guint32 page_size;
  guint32 max_prio_size[PRIO_LEVELS];
  guint32 alloc_segments;
  char *alloc_segments_ref[MAX_ALLOC_SEGMENTS];
  GMutex *ic_buf_mutex;
};
typedef struct ic_sock_buf IC_SOCK_BUF;

IC_SOCK_BUF *ic_init_socket_membuf(guint32 page_size,
                                   guint32 no_of_pages);

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
int ic_send_with_cr(IC_CONNECTION *conn, const char *buf);
int ic_rec_with_cr(IC_CONNECTION *conn,
                   gchar *rec_buf,
                   guint32 *read_size,
                   guint32 *size_curr_buf,
                   guint32 buffer_size);

/*
  Methods to handle conversion to integers from strings
*/
gboolean convert_str_to_int_fixed_size(char *str, guint32 num_chars,
                                       guint64 *ret_number);

#define CARRIAGE_RETURN (char)10
#define LINE_FEED (char)13
#define NULL_BYTE (char)0

#endif
