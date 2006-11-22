#include <ic_comm.h>
#include <ic_common.h>

void ic_init_ds_connection(struct ic_ds_conn *conn);

struct ic_ds_operations
{
  int (*set_up_ic_ds_connection) (struct ic_ds_conn *ds_conn);
  int (*close_ic_ds_connection) (struct ic_ds_conn *ds_conn);
  int (*is_conn_established) (struct ic_ds_conn *ds_conn,
                              gboolean *is_established);
  int (*authenticate_connection) (struct ic_ds_conn *ds_conn);
};

struct ic_ds_conn
{
  guint32 dest_node_id;
  guint32 source_node_id;
  guint32 socket_group;
  struct ic_connection conn_obj;
  struct ic_ds_operations operations;
};

