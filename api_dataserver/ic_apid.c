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

#include <ic_comm.h>
#include <ic_apic.h>
#include <ic_apid.h>

IC_APID_GLOBAL*
ic_init_apid(IC_API_CONFIG_SERVER *apic)
{
  IC_APID_GLOBAL *apid_global;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_CLUSTER_COMM *cluster_comm;
  guint32 i, j;

  if (!(apid_global= (IC_APID_GLOBAL*)ic_calloc(sizeof(IC_APID_GLOBAL))))
    return NULL;
  if (!(apid_global->grid_comm=
       (IC_GRID_COMM*)ic_calloc(sizeof(IC_GRID_COMM))))
    goto error;
  if (!(apid_global->grid_comm->cluster_comm_array= (IC_CLUSTER_COMM**)
        ic_calloc((IC_MAX_CLUSTER_ID + 1) * sizeof(IC_SEND_NODE_CONNECTION*))))
    goto error;
  if (!(apid_global->grid_comm->thread_conn_array= (IC_THREAD_CONNECTION**)
        ic_calloc((IC_MAX_THREAD_CONNECTIONS + 1) *
        sizeof(IC_THREAD_CONNECTION*))))
    goto error;
  if (!(apid_global->cluster_bitmap= ic_create_bitmap(NULL, IC_MAX_CLUSTER_ID)))
    goto error;
  if (!(apid_global->mutex= g_mutex_new()))
    goto error;
  if (!(apid_global->cond= g_cond_new()))
    goto error;
  if (!(apid_global->mem_buf_pool= ic_create_socket_membuf(IC_MEMBUF_SIZE,
                                                           512)))
    goto error;
  if (!(apid_global->ndb_signal_pool= ic_create_socket_membuf(
                                         sizeof(IC_NDB_SIGNAL), 2048)))
    goto error;
  if (!(apid_global->thread_id_mutex= g_mutex_new()))
    goto error;
  for (i= 0; i <= apic->max_cluster_id; i++)
  {
    if ((clu_conf= apic->api_op.ic_get_cluster_config(apic,i)))
    {
      if (!(cluster_comm= (IC_CLUSTER_COMM*)
                     ic_calloc(sizeof(IC_CLUSTER_COMM))))
        goto error;
      apid_global->grid_comm->cluster_comm_array[i]= cluster_comm;
      if (!(cluster_comm->send_node_conn_array= (IC_SEND_NODE_CONNECTION**)
            ic_calloc((IC_MAX_NODE_ID + 1)*sizeof(IC_SEND_NODE_CONNECTION*))))
        goto error;
      for (j= 0; j <= clu_conf->max_node_id; j++)
      {
        if (clu_conf->node_config[j])
        {
          if (!(send_node_conn= (IC_SEND_NODE_CONNECTION*)
              ic_calloc(sizeof(IC_SEND_NODE_CONNECTION))))
            goto error;
          cluster_comm->send_node_conn_array[j]= send_node_conn;
          if (!(send_node_conn->mutex= g_mutex_new()))
            goto error;
          if (!(send_node_conn->cond= g_cond_new()))
            goto error;
        }
      }
    }
  }
  return apid_global;
error:
  ic_end_apid(apid_global);
  return NULL;
}

void
ic_end_apid(IC_APID_GLOBAL *apid_global)
{
  IC_SOCK_BUF *mem_buf_pool= apid_global->mem_buf_pool;
  IC_SOCK_BUF *ndb_signal_pool= apid_global->ndb_signal_pool;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  guint32 i, j;

  if (!apid_global)
    return;
  if (apid_global->cluster_bitmap)
    ic_free_bitmap(apid_global->cluster_bitmap);
  if (mem_buf_pool)
    mem_buf_pool->sock_buf_op.ic_free_sock_buf(mem_buf_pool);
  if (ndb_signal_pool)
    ndb_signal_pool->sock_buf_op.ic_free_sock_buf(ndb_signal_pool);
  if (apid_global->mutex)
    g_mutex_free(apid_global->mutex);
  if (apid_global->cond)
    g_cond_free(apid_global->cond);
  if (grid_comm)
  {
    if (grid_comm->cluster_comm_array)
    {
      for (i= 0; i <= IC_MAX_CLUSTER_ID; i++)
      {
        cluster_comm= grid_comm->cluster_comm_array[i];
        if (cluster_comm == NULL)
          continue;
        for (j= 0; j <= IC_MAX_NODE_ID; i++)
        {
          send_node_conn= cluster_comm->send_node_conn_array[j];
          if (send_node_conn == NULL)
            continue;
          if (send_node_conn->mutex)
            g_mutex_free(send_node_conn->mutex);
          if (send_node_conn->cond)
            g_cond_free(send_node_conn->cond);
          ic_free(send_node_conn);
        }
        ic_free(cluster_comm);
      }
      ic_free(grid_comm->cluster_comm_array);
    }
    if (grid_comm->thread_conn_array)
    {
      for (j= 0; j < IC_MAX_THREAD_CONNECTIONS; j++)
      {
        if (!grid_comm->thread_conn_array[j])
          continue;
        if (grid_comm->thread_conn_array[j]->mutex)
          g_mutex_free(grid_comm->thread_conn_array[j]->mutex);
        if (grid_comm->thread_conn_array[j]->cond)
          g_cond_free(grid_comm->thread_conn_array[j]->cond);
      }
      ic_free(grid_comm->thread_conn_array);
    }
    ic_free(grid_comm);
  }
  if (apid_global->thread_id_mutex)
    g_mutex_free(apid_global->thread_id_mutex);
  ic_free(apid_global);
}

static gpointer
run_send_thread(void *data)
{
  gboolean is_client_part;
  IC_SEND_NODE_CONNECTION *send_node_conn= (IC_SEND_NODE_CONNECTION*)data;
  /*
    First step is to create a connection object, then it's time to
    start setting it up when told to do so by the main thread. We want
    to ensure that all threads and other preparatory activities have
    been done before we start connecting to the cluster. As soon as we
    connect to the cluster we're immediately a real-time application and
    must avoid to heavy activities such as starting 100's of threads.
  */
  is_client_part= (send_node_conn->my_node_id ==
                   send_node_conn->link_config->server_node_id);
  if (!(send_node_conn->conn= ic_create_socket_object(
                       is_client_part,
                       FALSE,  /* Mutex supplied by API code instead */
                       FALSE,  /* This thread is already a connection thread */
                       0,      /* No read buffer needed, we supply our own */
                       NULL,   /* Authentication function */
                       NULL))) /* Authentication object */
    goto error;
error:
  g_mutex_lock(send_node_conn->mutex);
  g_cond_signal(send_node_conn->cond);
  g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
  /*
    The main thread have started up all threads and all communication objects
    have been created, it's now time to connect to the clusters.
  */
  return NULL;
}

int
start_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  GError *error;
  if (send_node_conn->my_node_id == send_node_conn->active_node_id)
  {
    /*
      This is a local connection, we don't need to start a send thread
      since it isn't possible to overload a local connection and thus
      we don't need a send thread and also there is no need for a
      connection and thus no need for a thread to start the connection.
    */
    send_node_conn->node_up= TRUE;
    return 0;
  }
  /*
    We start the send thread, this thread is also used as the thread to
    set-up the connection.
  */
  g_mutex_lock(send_node_conn->mutex);
  if (!g_thread_create_full(run_send_thread,
                            (gpointer)send_node_conn,
                            16*1024, /* Stack size */
                            FALSE,   /* Not joinable */
                            FALSE,   /* Not bound */
                            G_THREAD_PRIORITY_NORMAL,
                            &error))
  {
    g_mutex_unlock(send_node_conn->mutex);
    return IC_ERROR_MEM_ALLOC;
  }
  g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
  /*
    The send thread is done with its start-up phase and we're ready to start
    the next send thread.
  */
  return 0;
}

int
ic_apid_global_disconnect(IC_APID_GLOBAL *apid_global)
{
  return 0;
}

int
ic_apid_global_connect(IC_APID_GLOBAL *apid_global)
{
  guint32 node_id, cluster_id, my_node_id;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_API_CONFIG_SERVER *apic= apid_global->apic;
  IC_SOCKET_LINK_CONFIG *link_config;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  int error= 0;
  DEBUG_ENTRY("ic_apid_global_connect");

  for (cluster_id= 0; cluster_id <= apic->max_cluster_id; cluster_id++)
  {
    if (!(clu_conf= apic->api_op.ic_get_cluster_config(apic, cluster_id)))
      continue;
    cluster_comm= grid_comm->cluster_comm_array[cluster_id];
    g_assert(cluster_comm);
    for (node_id= 1; node_id <= clu_conf->max_node_id; node_id++)
    {
      if (node_id == clu_conf->my_node_id ||
          (link_config= apic->api_op.ic_get_communication_object(apic,
                            cluster_id,
                            IC_MIN(node_id, my_node_id),
                            IC_MAX(node_id, my_node_id))))
      {
        g_assert(clu_conf->node_config[node_id]);
        send_node_conn= cluster_comm->send_node_conn_array[node_id];
        g_assert(send_node_conn);
        /* We have found a node to connect to, start connect thread */
        send_node_conn->link_config= link_config;
        send_node_conn->active_node_id= node_id;
        send_node_conn->my_node_id= clu_conf->my_node_id;
        send_node_conn->cluster_id= cluster_id;
        send_node_conn->max_wait_in_nanos=
               (IC_TIMER)link_config->socket_max_wait_in_nanos;
        /* Start send thread */
        if ((error= start_send_thread(send_node_conn)))
          goto error;
      }
      else
      {
        g_assert(!clu_conf->node_config[node_id]);
      }
    }
  }
  /*
    We are now ready to signal to all send threads that they can start up the
    connect process to the other nodes in the cluster.
  */
  for (cluster_id= 0; cluster_id <= apic->max_cluster_id; cluster_id++)
  {
    if (!(clu_conf= apic->api_op.ic_get_cluster_config(apic, cluster_id)))
      continue;
    cluster_comm= grid_comm->cluster_comm_array[cluster_id];
    for (node_id= 1; node_id <= clu_conf->max_node_id; node_id++)
    {
      if (clu_conf->node_config[node_id] && node_id != clu_conf->my_node_id)
      {
        send_node_conn= cluster_comm->send_node_conn_array[node_id];
        g_mutex_lock(send_node_conn->mutex);
        /* Wake up send thread to connect */
        g_cond_signal(send_node_conn->cond);
        g_mutex_unlock(send_node_conn->mutex);
      }
    }
  }
  return 0;
error:
  return error;
}

static void
apid_free(IC_APID_CONNECTION *apid_conn)
{
  IC_THREAD_CONNECTION *thread_conn= apid_conn->thread_conn;
  IC_GRID_COMM *grid_comm;
  IC_APID_GLOBAL *apid_global;
  guint32 thread_id= apid_conn->thread_id;
  DEBUG_ENTRY("apid_free");

  apid_global= apid_conn->apid_global;
  grid_comm= apid_global->grid_comm;
  if (apid_conn)
  {
    if (apid_conn->cluster_id_bitmap)
      ic_bitmap_free(apid_conn->cluster_id_bitmap);
    ic_free(apid_conn);
  }
  if (thread_conn)
  {
    thread_id= apid_conn->thread_id;
    if (thread_conn->mutex)
      g_mutex_free(thread_conn->mutex);
    if (thread_conn->cond)
      g_cond_free(thread_conn->cond);
    ic_free(thread_conn);
  }
  g_mutex_lock(apid_global->thread_id_mutex);
  if (thread_conn &&
      grid_comm->thread_conn_array[thread_id] == thread_conn)
    grid_comm->thread_conn_array[thread_id]= NULL;
  g_mutex_unlock(apid_global->thread_id_mutex);
}

IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *apid_global,
                          IC_BITMAP *cluster_id_bitmap)
{
  guint32 thread_id= IC_MAX_THREAD_CONNECTIONS;
  IC_GRID_COMM *grid_comm;
  IC_THREAD_CONNECTION *thread_conn;
  IC_APID_CONNECTION *apid_conn;
  guint32 i;
  DEBUG_ENTRY("ic_create_apid_connection");

  g_mutex_lock(apid_global->thread_id_mutex);
  for (i= 0; i < IC_MAX_THREAD_CONNECTIONS; i++)
  {
    if (!grid_comm->thread_conn_array[i])
    {
      /* Initialise the APID connection object */
      if (!(apid_conn= (IC_APID_CONNECTION*)
                        ic_calloc(sizeof(IC_APID_CONNECTION))))
        goto end;
      apid_conn->apid_global= apid_global;
      apid_conn->thread_id= thread_id;
      apid_conn->apid_global= apid_global;
      apid_conn->apic= apid_global->apic;
      if (!(apid_conn->cluster_id_bitmap= ic_create_bitmap(NULL,
                            ic_bitmap_get_num_bits(cluster_id_bitmap))))
        goto error;
      ic_bitmap_copy(apid_conn->cluster_id_bitmap, cluster_id_bitmap);
      /* Initialise Thread Connection object */
      if (!(thread_conn= apid_conn->thread_conn= (IC_THREAD_CONNECTION*)
                          ic_calloc(sizeof(IC_THREAD_CONNECTION))))
        goto error;
      thread_conn->apid_conn= apid_conn;
      if (!(thread_conn->mutex= g_mutex_new()))
        goto error;
      if (!(thread_conn->cond= g_cond_new()))
        goto error;
      thread_id= i;
      break;
    }
  }
  if (thread_id == IC_MAX_THREAD_CONNECTIONS)
    goto end;
  grid_comm->thread_conn_array[thread_id]= thread_conn;
  g_mutex_unlock(apid_global->thread_id_mutex);
  /* Now initialise the method pointers for the Data API interface */
  apid_conn->apid_ops.ic_free= apid_free;
  return apid_conn;

error:
  apid_free(apid_conn);
end:
  g_mutex_unlock(apid_global->thread_id_mutex);
  return NULL;
}

static int
is_ds_conn_established(IC_DS_CONNECTION *ds_conn,
                       gboolean *is_connected)
{
  IC_CONNECTION *conn= ds_conn->conn_obj;

  *is_connected= FALSE;
  if (conn->conn_op.ic_is_conn_thread_active(conn))
    return 0;
  *is_connected= TRUE;
  if (conn->conn_op.ic_is_conn_connected(conn))
    return 0;
  return conn->error_code;
}

static int
authenticate_ds_connection(void *conn_obj)
{
  gchar *read_buf;
  guint32 read_size;
  gchar expected_buf[64];
  int error;
  IC_DS_CONNECTION *ds_conn= conn_obj;
  IC_CONNECTION *conn= ds_conn->conn_obj;

  ic_send_with_cr(conn, "ndbd");
  ic_send_with_cr(conn, "ndbd passwd");
  conn->conn_op.ic_flush_connection(conn);
  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (!strcmp(read_buf, "ok"))
    return AUTHENTICATE_ERROR;
  ic_send_with_cr(conn, read_buf);
  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (!strcmp(read_buf, expected_buf))
    return AUTHENTICATE_ERROR;
  return 0;
}

static int
open_ds_connection(IC_DS_CONNECTION *ds_conn)
{
  int error;
  IC_CONNECTION *conn= ds_conn->conn_obj;
  if (!(conn= ic_create_socket_object(TRUE, TRUE, TRUE,
                                      CONFIG_READ_BUF_SIZE,
                                      authenticate_ds_connection,
                                      (void*)ds_conn)))
    return IC_ERROR_MEM_ALLOC;
  ds_conn->conn_obj= conn;
  if ((error= conn->conn_op.ic_set_up_connection(conn)))
    return error;
  return 0;
}

static int
close_ds_connection(__attribute__ ((unused)) IC_DS_CONNECTION *ds_conn)
{
  return 0;
}

void
ic_init_ds_connection(IC_DS_CONNECTION *ds_conn)
{
  ds_conn->operations.ic_set_up_ds_connection= open_ds_connection;
  ds_conn->operations.ic_close_ds_connection= close_ds_connection;
  ds_conn->operations.ic_is_conn_established= is_ds_conn_established;
  ds_conn->operations.ic_authenticate_connection= authenticate_ds_connection;
}
