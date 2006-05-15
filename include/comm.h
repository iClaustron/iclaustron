#ifdef COMM_H
#define COMM_H

#include <netinet/in.h>
#include <config.h>
#include <common.h>

struct ic_connect_operations
{
  int (*set_up_ic_connection) (struct ic_connection *conn);
  int (*read_ic_connection) (struct ic_connection *conn,
                             void **buf, guint32 size);
  int (*write_ic_connection) (struct ic_connection *conn,
                              const void *buf, guint32 size);
  int (*open_write_session) (struct ic_connection *conn, guint32 total_size);
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
  guint64 cpu_bindings;
  struct GMutex *read_mutex;
  struct GMutex *write_mutex;
  struct connection_buffer *first_con_buf;
  struct connection_buffer *first_con_buf;
  guint32 node_id;
  guint32 server_ip;
  guint32 client_ip;
  guint16 server_port;
  guint16 client_port;
  bool is_client;
};

struct ic_connect_manager
{
  struct ic_connection **ic_con_array;
  struct GMutex *icm_mutex;
};

#endif
