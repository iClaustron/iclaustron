#include <ic_apid.h>

static int
is_ds_conn_established(struct ic_ds_conn *ds_conn,
                       gboolean *is_connected)
{
  struct ic_connection *conn= &ds_conn->conn_obj;

  *is_connected= FALSE;
  if (conn->conn_op.is_ic_conn_thread_active(conn))
    return 0;
  *is_connected= TRUE;
  if (conn->conn_op.is_ic_conn_connected(conn))
    return 0;
  return conn->error_code;
}

static int
authenticate_ds_connection(void *conn_obj)
{
  gchar buf[64];
  gchar expected_buf[64];
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  int error;
  struct ic_ds_conn *ds_conn= conn_obj;
  struct ic_connection *conn= &ds_conn->conn_obj;

  ic_send_with_cr(conn, "ndbd");
  ic_send_with_cr(conn, "ndbd passwd");
  conn->conn_op.flush_ic_connection(conn);
  if ((error= ic_rec_with_cr(conn, (gchar*)&buf, &read_size, &size_curr_buf,
                             sizeof(buf))))
    return error;
  if (!strcmp(buf, "ok"))
    return AUTHENTICATE_ERROR;
  ic_send_with_cr(conn, buf);
  if ((error= ic_rec_with_cr(conn, (gchar*)&buf, &read_size, &size_curr_buf,
                             sizeof(buf))))
    return error;
  if (!strcmp(buf, expected_buf))
    return AUTHENTICATE_ERROR;
  return 0;
}

static int
open_ds_connection(struct ic_ds_conn *ds_conn)
{
  int error;
  struct ic_connection *conn= &ds_conn->conn_obj;
  if (ic_init_socket_object(conn,
                            TRUE, TRUE, TRUE, TRUE,
                            authenticate_ds_connection,
                            (void*)ds_conn))
    return MEM_ALLOC_ERROR;
  if ((error= conn->conn_op.set_up_ic_connection(conn)))
    return error;
  return 0;
}

static int
close_ds_connection(__attribute__ ((unused)) struct ic_ds_conn *ds_conn)
{
  return 0;
}

void
ic_init_ds_connection(struct ic_ds_conn *ds_conn)
{
  ds_conn->operations.set_up_ic_ds_connection= open_ds_connection;
  ds_conn->operations.close_ic_ds_connection= close_ds_connection;
  ds_conn->operations.is_conn_established= is_ds_conn_established;
  ds_conn->operations.authenticate_connection= authenticate_ds_connection;
}
