#ifndef IC_COMM_H
#define IC_COMM_H

#include <netinet/in.h>
#include <config.h>
#include <string.h>
#include <ic_common.h>

struct ic_connection;
void set_socket_methods(struct ic_connection *conn);

struct ic_connect_operations
{
  int (*set_up_ic_connection) (struct ic_connection *conn);
  int (*accept_ic_connection) (struct ic_connection *conn);
  int (*close_ic_connection) (struct ic_connection *conn);
  int (*read_ic_connection) (struct ic_connection *conn,
                             void *buf, guint32 buf_size,
                             guint32 *read_size);
  int (*write_ic_connection) (struct ic_connection *conn,
                              const void *buf, guint32 size,
                              guint32 secs_to_try);
  int (*open_write_session) (struct ic_connection *conn,
                             guint32 total_size);
  int (*close_write_session) (struct ic_connection *conn);
  int (*open_read_session) (struct ic_connection *conn);
  int (*close_read_session) (struct ic_connection *conn);
};

struct ic_connect_mgr_operations
{
  int (*add_ic_connection) (struct ic_connection *conn);
  int (*drop_ic_connection) (struct ic_connection *conn);
  int (*get_ic_connection) (struct ic_connection **conn, guint32 node_id);
  int (*get_free_ic_connection) (struct ic_connection **conn);
};

struct ic_connection
{
  /*
    These are a set of private variables used internally in the
    ic_connection class.
  */
  struct ic_connect_operations conn_op;
  guint64 cpu_bindings;
  GMutex *read_mutex;
  GMutex *write_mutex;
  struct connection_buffer *first_con_buf;
  struct connection_buffer *last_con_buf;
  int sockfd;
  guint32 node_id;
  /*
    These are interface variables that are public. By setting
    those to proper values before calling set_up_ic_connection
    the behaviour of the ic_connection is changed.
  */
  /*
    backlog is the parameter provided to the listen call, thus
    this is only applicable for server side connections.
  */
  int backlog;
  /*
    Server Connection:
    ------------------
    This is signalled by setting is_client to FALSE
    For a server connection the server ip address should be set
    to a proper ip address to bind the interface we are listening
    to. If any interface can be used then server_ip should be
    set to INADDR_ANY.
    server_port must be set for server connections. This is the
    port we are listening to.
    If we allow any client to connect to the server we should set
    client_ip to INADDR_ANY and client_port to 0. If client_ip
    is not set to INADDR_ANY then we will only accept connections
    from this IP address and if port != 0 then we will only accept
    connections where the client is using this port.

    Client Connection:
    ------------------
    This is signalled by setting is_client to TRUE
    For a client connection the server_ip and server_port must
    be set to proper values. Otherwise the client won't find
    the server side part of the connection.

    client_ip can be set to INADDR_ANY and in this case the
    operating system will select the interface to use for the
    connection. If an IP address is provided this will be used
    to select a proper interface for the connection.

    client_port can be set to 0 and in this case the ephemeral
    port will be used, meaning that the operating system will
    select a port from 1500 and upwards. If a port is provided
    then this is the port which will be used in the connect
    message to the server part of the connection.
  */
  guint32 server_ip;
  guint32 client_ip;
  guint16 server_port;
  guint16 client_port;
  gboolean is_client;
  /*
    Not really sure why I introduced this variable. Should
    probably be removed.
  */
  gboolean call_accept;
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
};

struct ic_connect_manager
{
  struct ic_connection **ic_con_array;
  struct GMutex *icm_mutex;
};

#endif
