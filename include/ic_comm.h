#ifndef IC_COMM_H
#define IC_COMM_H

#include <netinet/in.h>
#include <config.h>
#include <string.h>
#include <ic_common.h>

struct ic_connection;

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
  struct ic_connect_operations conn_op;
  guint64 cpu_bindings;
  GMutex *read_mutex;
  GMutex *write_mutex;
  struct connection_buffer *first_con_buf;
  struct connection_buffer *last_con_buf;
  int backlog;
  int sockfd;
  guint32 node_id;
  guint32 server_ip;
  guint32 client_ip;
  guint16 server_port;
  guint16 client_port;
  gboolean is_client;
  gboolean call_accept;
};

struct ic_connect_manager
{
  struct ic_connection **ic_con_array;
  struct GMutex *icm_mutex;
};

#endif
