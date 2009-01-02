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

#ifndef IC_CONNECTION_H
#define IC_CONNECTION_H

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

typedef int (*accept_timeout_func) (void*, int);
typedef int (*authenticate_func) (void*);

struct ic_connection;
typedef struct ic_connection IC_CONNECTION;
struct ic_connect_stat;
typedef struct ic_connect_stat IC_CONNECT_STAT;
struct ic_connect_operations
{
  /*
    After accept has received a connection it is possible to fork the
    connection and continue listening to the server port.
  */
  IC_CONNECTION* (*ic_fork_accept_connection)
                                       (IC_CONNECTION *orig_conn,
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

  void (*ic_prepare_server_connection) (IC_CONNECTION *conn,
                                        gchar *server_name,
                                        gchar *server_port,
                                        gchar *client_name, /* Optional */
                                        gchar *client_port, /* Optional */
                                        int backlog,        /* Optional */
                                        gboolean is_listen_socket_retained);
  void (*ic_prepare_client_connection) (IC_CONNECTION *conn,
                                        gchar *server_name,
                                        gchar *server_port,
                                        gchar *client_name, /* Optional */
                                        gchar *client_port);/* Optional */
  void (*ic_prepare_extra_parameters)  (IC_CONNECTION *conn,
                                        guint32 tcp_maxseg,
                                        gboolean is_wan_connection,
                                        guint32 tcp_receive_buffer_size,
                                        guint32 tcp_send_buffer_size);
  void (*ic_set_param)                 (IC_CONNECTION *conn,
                                        void *param);
  void* (*ic_get_param)                (IC_CONNECTION *conn);

  int (*ic_set_up_connection)          (IC_CONNECTION *conn,
                                        accept_timeout_func timeout_func,
                                        void *timeout_obj);
  int (*ic_accept_connection)          (IC_CONNECTION *conn,
                                        accept_timeout_func timeout_func,
                                        void *timeout_obj);
  int (*ic_close_connection)           (IC_CONNECTION *conn);
  int (*ic_close_listen_connection)    (IC_CONNECTION *conn);
  /*
    These calls are used to read and write on a socket connection.
    There is support for memory buffering in front of the socket.
    In this case it is necessary to flush the memory buffer in order
    to ensure that buffered operations have actually been written.
  */
  int (*ic_read_connection)            (IC_CONNECTION *conn,
                                        void *buf,
                                        guint32 buf_size,
                                        guint32 *read_size);
  int (*ic_write_connection)           (IC_CONNECTION *conn,
                                        const void *buf,
                                        guint32 size,
                                        guint32 secs_to_try);
  int (*ic_writev_connection)          (IC_CONNECTION *conn,
                                        struct iovec *write_vector,
                                        guint32 iovec_size,
                                        guint32 tot_size,
                                        guint32 secs_to_try);
  int (*ic_flush_connection)           (IC_CONNECTION *conn);
  /* This call is used to check if there is data to be read on the socket */
  gboolean (*ic_check_for_data)        (IC_CONNECTION *conn);
  /* This call is used to check if the hostname and port is the same as
     the connection, 0 means equal, 1 non-equal */
  int (*ic_cmp_connection)             (IC_CONNECTION *conn,
                                        gchar *hostname,
                                        gchar *port);
  /* Error handling routines */
  const gchar* (*ic_get_error_str)     (IC_CONNECTION *conn);
  int (*ic_get_error_code)             (IC_CONNECTION *conn);
  void (*ic_set_error_line)            (IC_CONNECTION *conn,
                                        guint32 error_line);
  gchar* (*ic_fill_error_buffer)       (IC_CONNECTION *conn,
                                        int error_code,
                                        gchar *error_buffer);
  /*
    In order to support multiple threads using the same connection
    object at the same time we need to declare open and close of
    read and write sessions. The socket connection can have a read
    session and a write session concurrently.
  */
  int (*ic_open_write_session)         (IC_CONNECTION *conn,
                                        guint32 total_size);
  int (*ic_close_write_session)        (IC_CONNECTION *conn);
  int (*ic_open_read_session)          (IC_CONNECTION *conn);
  int (*ic_close_read_session)         (IC_CONNECTION *conn);
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
  void (*ic_read_stat_connection)      (IC_CONNECTION *conn,
                                        IC_CONNECT_STAT *stat,
                                        gboolean clear_stat_timer);
  void (*ic_safe_read_stat_connection) (IC_CONNECTION *conn,
                                        IC_CONNECT_STAT *stat,
                                        gboolean clear_stat_timer);
  /* Print statistics of the connection */
  void (*ic_write_stat_connection)     (IC_CONNECTION *conn);
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
  double (*ic_read_connection_time)    (IC_CONNECTION *conn,
                                        gulong *microseconds);
  double (*ic_read_stat_time)          (IC_CONNECTION *conn,
                                        gulong *microseconds);
  gboolean (*ic_is_conn_connected)     (IC_CONNECTION *conn);
  gboolean (*ic_is_conn_thread_active) (IC_CONNECTION *conn);
  /*
    Free all memory connected to a connection object
    It also closes all connection if not already done
  */
  void (*ic_free_connection)           (IC_CONNECTION *conn);
};
typedef struct ic_connect_operations IC_CONNECTION_OPERATIONS;

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

struct ic_connection
{
  IC_CONNECTION_OPERATIONS conn_op;
  IC_CONNECT_STAT conn_stat;
};

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
IC_CONNECTION
  *ic_create_socket_object(gboolean is_client,
                           gboolean is_mutex_used,
                           gboolean is_connect_thread_used,
                           guint32  read_buf_size,
                           authenticate_func func,
                           void *auth_obj);

IC_CONNECTION
  *ic_create_ssl_object(gboolean is_client,
                        IC_STRING *root_certificate_path,
                        IC_STRING *loc_certification_path,
                        IC_STRING *passwd_string,
                        gboolean is_ssl_used_for_data,
                        gboolean is_connect_thread_used,
                        guint32  read_buf_size,
                        authenticate_func func,
                        void *auth_obj);
/* SSL initialisation routines */
int ic_ssl_init();
void ic_ssl_end();

/*
  Debug print-outs
*/
void ic_print_buf(char *buf, guint32 size);
/*
  Methods to send and receive buffers with Carriage Return
*/
int ic_send_with_cr(IC_CONNECTION *conn, const gchar *buf);
int ic_rec_with_cr(IC_CONNECTION *conn,
                   gchar **rec_buf,
                   guint32 *read_size);
#endif

