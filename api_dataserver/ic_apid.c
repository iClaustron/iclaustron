#include <ic_apid.h>

static int
open_ds_connection(struct ic_ds_conn *ds_conn)
{
  struct ic_connection *conn= &ds_conn->conn_obj;
  if (ic_init_socket_object(conn,
                            TRUE, TRUE, TRUE, TRUE))
    return MEM_ALLOC_ERROR;
  if ((error= conn->conn_op.set_up_ic_connection(conn)))
    return error;
}

static int
is_ds_conn_established(struct ic_ds_conn *ds_conn,
                       gboolean *is_connected)
{
  struct ic_connection *conn= &ds_conn->conn_obj;

  *is_connected= FALSE;
  if (conn->conn_op.is_connect_thread_active(conn))
    return 0;
  *is_connected= TRUE;
  if (conn->conn_op.is_connected(conn))
    return 0;
  return conn->error_code;
}

static int
authenticate_ds_connection(struct ic_ds_conn *ds_conn)
{
  char buf[64];
  char expected_buf[64];
  struct ic_connection *conn= &ds_conn->conn_obj;

  send_with_cr(conn, "ndbd");
  send_with_cr(conn, "ndbd passwd");
  conn->conn_op.flush_ic_connection(conn);
  rec_with_cr(conn, &buf, sizeof(buf));
  if (!strcmp(buf, "ok"))
    return AUTHENTICATE_ERROR;
  send_with_cr(conn, buf);
  rec_with_cr(conn, &buf, sizeof(buf));
  if (!strcmp(buf, expected_buf))
    return AUTHENTICATE_ERROR;
  return 0;
}

void
ic_init_ds_connection(struct ic_ds_conn *ds_conn)
{
  ds_conn->operations.set_up_connection= open_ds_connection;
  ds_conn->operations.close_ic_ds_connection= close_ds_connection;
  ds_conn->operations.is_conn_established= is_ds_conn_established;
  ds_conn->operations.authenticate_connection= authenticate_ds_connection;
}
