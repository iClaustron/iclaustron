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

static void apid_free(IC_APID_CONNECTION *apid_conn);
static int start_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn);
static void start_connect_phase(IC_APID_GLOBAL *apid_global,
                                gboolean stop_ordered,
                                gboolean signal_flag);
static int prepare_real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                                      guint32 *send_size,
                                      struct iovec *write_vector,
                                      guint32 *iovec_size);
static int real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                              struct iovec *write_vector, guint32 iovec_size,
                              guint32 send_size);
static int node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn);

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
  start_connect_phase(apid_global, FALSE, TRUE);
  DEBUG_RETURN(0);
error:
  start_connect_phase(apid_global, TRUE, TRUE);
  DEBUG_RETURN(error);
}

int
ic_apid_global_disconnect(IC_APID_GLOBAL *apid_global)
{
  start_connect_phase(apid_global, TRUE, FALSE);
  return 0;
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
      ic_free_bitmap(apid_conn->cluster_id_bitmap);
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

static void
start_connect_phase(IC_APID_GLOBAL *apid_global,
                    gboolean stop_ordered,
                    gboolean signal_flag)
{
  guint32 node_id, cluster_id;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_API_CONFIG_SERVER *apic= apid_global->apic;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  DEBUG_ENTRY("start_connect_phase");

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
        if (send_node_conn)
        {
          g_mutex_lock(send_node_conn->mutex);
          if (stop_ordered)
            send_node_conn->stop_ordered= TRUE;
          /* Wake up send thread to connect */
          if (signal_flag)
            g_cond_signal(send_node_conn->cond);
          g_mutex_unlock(send_node_conn->mutex);
        }
      }
    }
  }
  DEBUG_RETURN_EMPTY;
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

/* Send thread Handling */
static void
run_active_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  gboolean more_data= FALSE;
  int error;
  guint32 send_size;
  guint32 iovec_size;
  struct iovec write_vector[MAX_SEND_BUFFERS];

  g_mutex_lock(send_node_conn->mutex);
  send_node_conn->starting_send_thread= FALSE;
  do
  {
    send_node_conn->send_thread_active= FALSE;
    if (!more_data)
      g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
    if (send_node_conn->stop_ordered)
      break;
    if (!send_node_conn->node_up)
    {
      more_data= FALSE;
      continue;
    }
    g_assert(send_node_conn->starting_send_thread);
    g_assert(send_node_conn->send_active);
    send_node_conn->starting_send_thread= FALSE;
    send_node_conn->send_thread_active= TRUE;
    prepare_real_send_handling(send_node_conn, &send_size,
                               write_vector, &iovec_size);
    g_mutex_unlock(send_node_conn->mutex);
    error= real_send_handling(send_node_conn, write_vector,
                              iovec_size, send_size);
    g_mutex_lock(send_node_conn->mutex);
    if (error)
    {
      send_node_conn->node_up= FALSE;
      node_failure_handling(send_node_conn);
      break;
    }
    /* Handle send done */
    if (error || !send_node_conn->node_up)
    {
      error= IC_ERROR_NODE_DOWN;
      more_data= FALSE;
    }
    else if (send_node_conn->first_sbp)
    {
      /* There are more buffers to send, we need to continue sending */
      more_data= TRUE;
    }
    else
    {
      /* All buffers have been sent, we can go to sleep again */
      more_data= FALSE;
      send_node_conn->send_active= FALSE;
    }
  } while (1);
  g_mutex_unlock(send_node_conn->mutex);
  return;
}

static int
connect_by_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  return 0;
}

static gpointer
run_send_thread(void *data)
{
  gboolean is_client_part;
  int error;
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
  send_node_conn->conn= ic_create_socket_object(
                       is_client_part,
                       FALSE,  /* Mutex supplied by API code instead */
                       FALSE,  /* This thread is already a connection thread */
                       0,      /* No read buffer needed, we supply our own */
                       NULL,   /* Authentication function */
                       NULL);/* Authentication object */
  g_mutex_lock(send_node_conn->mutex);
  g_cond_signal(send_node_conn->cond);
  if (send_node_conn == NULL)
    send_node_conn->stop_ordered= TRUE;
  g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
  if (send_node_conn == NULL)
    goto end;
  /*
    The main thread have started up all threads and all communication objects
    have been created, it's now time to connect to the clusters.
  */
  while (!send_node_conn->stop_ordered)
  {
    if ((error= connect_by_send_thread(send_node_conn)))
      goto end;
    run_active_send_thread(send_node_conn);
  }
end:
  g_mutex_lock(send_node_conn->mutex);
  send_node_conn->send_thread_ended= TRUE;
  g_mutex_unlock(send_node_conn->mutex);
  return NULL;
}

static int
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
  if (send_node_conn->stop_ordered)
    return IC_ERROR_MEM_ALLOC;
  return 0;
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

/*
  NDB Send handling
  -----------------
  To send messages using the NDB Protocol we have a new struct
  IC_SEND_NODE_CONNECTION which is used by the application thread plus
  one send thread per node.

  Before invoking the send method, the application thread packs a number of
  NDB Signals in the NDB Protocol format into a number of IC_SOCK_BUF_PAGE's.
  The interface to the send method contains a linked list of such
  IC_SOCK_BUF_PAGE's plus the IC_SEND_NODE_CONNECTION object and finally a
  boolean that indicates whether it's necessary to send immediately or
  whether an adaptive send algorithm is allowed.

  The focus is on holding the send mutex for a short time and this means that
  in the normal case there will be two mutex lock/unlocks. One when entering
  to see if someone else is currently sending. If no one is sending one will
  pack the proper number of buffers and release the mutex and then start the
  sending. If one send is not enough to handle all the sending, then we will
  simply wake up the send thread which will send until there are no more
  buffers to send. Thus the send thread has a really easy task in most cases,
  simply sleeping and then occasionally waking up and taking care of a set
  of send operations.

  The adaptive send algorithm is also handled outside the mutex, thus we will
  set the send to active while deciding whether it's a good idea to wait with
  sending. This will possibly change.
*/

static int
prepare_real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                           guint32 *send_size,
                           struct iovec *write_vector,
                           guint32 *iovec_size)
{
  IC_SOCK_BUF_PAGE *loc_last_send;
  guint32 loc_send_size= 0, iovec_index= 0;
  DEBUG_ENTRY("prepare_real_send_handling");

  loc_last_send= send_node_conn->first_sbp;
  do
  {
    write_vector[iovec_index].iov_base= loc_last_send->sock_buf_page;
    write_vector[iovec_index].iov_len= loc_last_send->size;
    iovec_index++;
    loc_send_size+= loc_last_send->size;
    loc_last_send= loc_last_send->next_sock_buf_page;
  } while (loc_send_size < MAX_SEND_SIZE &&
           iovec_index < MAX_SEND_BUFFERS &&
           loc_last_send);
  send_node_conn->first_sbp= loc_last_send;
  if (!loc_last_send)
    send_node_conn->last_sbp= NULL;
  *iovec_size= iovec_index;
  *send_size= loc_send_size;
  send_node_conn->queued_bytes-= loc_send_size;
  DEBUG_RETURN(0);
}

static int
real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                   struct iovec *write_vector, guint32 iovec_size,
                   guint32 send_size)
{
  int error;
  IC_CONNECTION *conn= send_node_conn->conn;
  DEBUG_ENTRY("real_send_handling");
  error= conn->conn_op.ic_writev_connection(conn,
                                            write_vector,
                                            iovec_size,
                                            send_size, 2);
  if (error)
  {
    /*
      We failed to send and in this case we need to invoke node failure
      handling since either the connection was broken or it was slow to
      the point of being similar to broken.
    */
    node_failure_handling(send_node_conn);
    DEBUG_RETURN(error);
  }
  DEBUG_RETURN(0);
}

static int
send_done_handling(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  gboolean signal_send_thread= FALSE;
  int error;
  DEBUG_ENTRY("send_done_handling");
  /* Handle send done */
  error= 0;
  g_mutex_lock(send_node_conn->mutex);
  if (!send_node_conn->node_up)
    error= IC_ERROR_NODE_DOWN;
  else if (send_node_conn->first_sbp)
  {
    /*
      There are more buffers to send, we give this mission to the
      send thread and return immediately
    */
    send_node_conn->starting_send_thread= TRUE;
    signal_send_thread= TRUE;
  }
  else
  {
    /* All buffers have been sent, we can return immediately */
    send_node_conn->send_active= FALSE;
  }
  g_mutex_unlock(send_node_conn->mutex);
  if (signal_send_thread)
    g_cond_signal(send_node_conn->cond);
  DEBUG_RETURN(error);
}

/*
  Adaptive Send algorithm
  -----------------------
  The idea behind this algorithm is that we want to maintain a level
  of buffering such that 95% of the sends do not have to wait more
  than a configured amount of nanoseconds. Assuming a normal distribution
  we can do statistical analysis of both real waits and waits that we
  decided to not take to see where the appropriate level of buffering
  occurs to meet at least 95% of the sends in the predefined timeframe.

  The first method does the analysis of whether or not to wait or not
  and the second routine gathers the statistics.
*/
static void
adaptive_send_algorithm_decision(IC_SEND_NODE_CONNECTION *send_node_conn,
                                 gboolean *will_wait,
                                 IC_TIMER current_time)
{
  guint32 num_waits= send_node_conn->num_waits;
  IC_TIMER first_buffered_timer= send_node_conn->first_buffered_timer;
  /*
    We start by checking whether it's allowed to wait any further based
    on how many we already buffered. Then we check that we haven't
    already waited for a longer time than allowed.
  */
  if (num_waits >= send_node_conn->max_num_waits)
    goto no_wait;
  if (!ic_check_defined_time(first_buffered_timer) &&
      (ic_nanos_elapsed(first_buffered_timer, current_time) >
       send_node_conn->max_wait_in_nanos))
    goto no_wait;
  /*
    If we come here it means that we haven't waited long enough yet and
    there is room for one more to wait according to the current state of
    the adaptive algorithm. Thus we decide to wait.
  */
  if (num_waits == 0)
    send_node_conn->first_buffered_timer= current_time;
  send_node_conn->num_waits= num_waits + 1;
  *will_wait= TRUE;
  return;

no_wait:
  send_node_conn->first_buffered_timer= UNDEFINED_TIME;
  send_node_conn->num_waits= 0;
  *will_wait= FALSE;
  return;
}

static void
adaptive_send_algorithm_statistics(IC_SEND_NODE_CONNECTION *send_node_conn,
                                   IC_TIMER current_time)
{
  guint32 new_send_timer_index, i;
  guint32 last_send_timer_index= send_node_conn->last_send_timer_index;
  guint32 max_num_waits, max_num_waits_plus_one, timer_index1, timer_index2;
  guint32 num_stats;
  IC_TIMER start_time1, start_time2, elapsed_time1, elapsed_time2;
  IC_TIMER tot_curr_wait_time, tot_wait_time_plus_one;

  max_num_waits= send_node_conn->max_num_waits;
  max_num_waits_plus_one= max_num_waits + 1;
  timer_index1= last_send_timer_index - max_num_waits;
  timer_index2= last_send_timer_index - max_num_waits_plus_one;
  start_time1= send_node_conn->last_send_timers[timer_index1];
  start_time2= send_node_conn->last_send_timers[timer_index2];
  elapsed_time1= current_time - start_time1;
  elapsed_time2= current_time - start_time2;

  tot_curr_wait_time= send_node_conn->tot_curr_wait_time;
  tot_wait_time_plus_one= send_node_conn->tot_wait_time_plus_one;
  send_node_conn->tot_curr_wait_time+= elapsed_time1;
  send_node_conn->tot_wait_time_plus_one+= elapsed_time2;

  num_stats= send_node_conn->num_stats;
  send_node_conn->num_stats= num_stats + 1;
  last_send_timer_index++;
  if (last_send_timer_index == MAX_SEND_TIMERS)
  {
    /* Compress array into first 8 entries to free up new entries */
    for (i= 0; i < MAX_SENDS_TRACKED; i++)
    {
      last_send_timer_index--;
      new_send_timer_index= last_send_timer_index - MAX_SENDS_TRACKED;
      send_node_conn->last_send_timers[new_send_timer_index]=
        send_node_conn->last_send_timers[last_send_timer_index];
    }
  }
  send_node_conn->last_send_timers[last_send_timer_index]= current_time;
  send_node_conn->last_send_timer_index= last_send_timer_index;
  return;
}

static void
adaptive_send_algorithm_adjust(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  guint64 mean_curr_wait_time, mean_wait_time_plus_one, limit;

  g_mutex_lock(send_node_conn->mutex);
  adaptive_send_algorithm_statistics(send_node_conn, ic_gethrtime());
  limit= send_node_conn->max_wait_in_nanos / 2;
  mean_curr_wait_time= send_node_conn->tot_curr_wait_time /
                       send_node_conn->num_stats;
  mean_wait_time_plus_one= send_node_conn->tot_wait_time_plus_one /
                           send_node_conn->num_stats;
  send_node_conn->tot_curr_wait_time= 0;
  send_node_conn->tot_wait_time_plus_one= 0;
  send_node_conn->num_stats= 0;
  if (mean_curr_wait_time > limit)
  {
    /*
      The mean waiting time is currently out of bounds, we need to
      decrease it even further to adapt to the new conditions.
    */
    if (send_node_conn->max_num_waits > 0)
      send_node_conn->max_num_waits--;
  }
  if (mean_wait_time_plus_one < limit)
  {
    /*
      The mean waiting is within limits even if we increase the number
      of waiters we can accept.
    */
    if (send_node_conn->max_num_waits < MAX_SENDS_TRACKED)
      send_node_conn->max_num_waits++;
  }
  g_mutex_unlock(send_node_conn->mutex);
  return;
}

/*
  The application thread has prepared to send a number of items and is now
  ready to send these set of pages. The IC_SEND_NODE_CONNECTION is the
  protected data structure that contains the information about the node
  to which this communication is to be sent to.

  This object keeps track of information which is required for the adaptive
  algorithm to work. So it keeps track of how often we send information to
  this node, how long time pages have been waiting to be sent.

  The workings of the sender is the following, the sender locks the
  IC_SEND_NODE_CONNECTION object, then he puts the pages in the linked list of
  pages to be sent here. Next step is to check if another thread is already
  active sending and hasn't flagged that he's not ready to continue
  sending. If there is a thread already sending we unlock and proceed.
*/

static int
ndb_send(IC_SEND_NODE_CONNECTION *send_node_conn,
         IC_SOCK_BUF_PAGE *first_page_to_send,
         gboolean force_send)
{
  IC_SOCK_BUF_PAGE *last_page_to_send;
  guint32 send_size= 0;
  guint32 iovec_size;
  struct iovec write_vector[MAX_SEND_BUFFERS];
  gboolean return_imm= TRUE;
  int error;
  IC_TIMER current_time;
  DEBUG_ENTRY("ndb_send");

  /*
    We start by calculating the last page to send and the total send size
    before acquiring the mutex.
  */
  last_page_to_send= first_page_to_send;
  send_size+= last_page_to_send->size;
  while (last_page_to_send->next_sock_buf_page)
  {
    send_size+= last_page_to_send->size;
    last_page_to_send= last_page_to_send->next_sock_buf_page;
  }
  /* Start critical section for sending */
  g_mutex_lock(send_node_conn->mutex);
  if (!send_node_conn->node_up)
  {
    g_mutex_unlock(send_node_conn->mutex);
    return IC_ERROR_NODE_DOWN;
  }
  /* Link the buffers into the linked list of pages to send */
  if (send_node_conn->last_sbp == NULL)
  {
    g_assert(send_node_conn->queued_bytes == 0);
    send_node_conn->first_sbp= first_page_to_send;
    send_node_conn->last_sbp= last_page_to_send;
  }
  else
  {
    send_node_conn->last_sbp->next_sock_buf_page= first_page_to_send;
    send_node_conn->last_sbp= last_page_to_send;
  }
  send_node_conn->queued_bytes+= send_size;
  current_time= ic_gethrtime();
  if (!send_node_conn->send_active)
  {
    return_imm= FALSE;
    send_node_conn->send_active= TRUE;
    prepare_real_send_handling(send_node_conn, &send_size,
                               write_vector, &iovec_size);
    if (!force_send)
    {
      adaptive_send_algorithm_decision(send_node_conn,
                                       &return_imm,
                                       current_time);
    }
  }
  adaptive_send_algorithm_statistics(send_node_conn, current_time);
  /* End critical section for sending */
  g_mutex_unlock(send_node_conn->mutex);
  if (return_imm)
    return 0;
  /* We will send now */
  if ((error= real_send_handling(send_node_conn, write_vector, iovec_size,
                                 send_size)))
    return error;
  /* Send done handling includes a new critical section for sending */
  DEBUG_RETURN(send_done_handling(send_node_conn));
}

static int
ic_send_handling(IC_APID_GLOBAL *apid_global,
                 guint32 cluster_id,
                 guint32 node_id,
                 IC_SOCK_BUF_PAGE *first_page_to_send,
                 gboolean force_send)
{
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_CLUSTER_COMM *cluster_comm;
  IC_API_CONFIG_SERVER *apic;
  IC_GRID_COMM *grid_comm;
  int error;

  apic= apid_global->apic;
  grid_comm= apid_global->grid_comm;
  error= IC_ERROR_NO_SUCH_CLUSTER;
  if (cluster_id > apic->max_cluster_id)
    return error;
  cluster_comm= grid_comm->cluster_comm_array[cluster_id];
  if (!cluster_comm)
    return error;
  error= IC_ERROR_NO_SUCH_NODE;
  send_node_conn= cluster_comm->send_node_conn_array[node_id];
  if (!send_node_conn)
    return error;
  return ndb_send(send_node_conn, first_page_to_send, force_send);
}

/*
  We handle reception of NDB Signals in one or more separate threads, each
  thread can handle one or more sockets. If there is only one socket to
  handle it can use socket read immediately, otherwise we can use either
  poll, or epoll on Linux or other similar variant on other operating
  system.

  This thread only handles reception of the lowest layer of the NDB Protocol.
  Thus here we get all sections of data and their sizes, the Signal Number
  received, the receiving module (thread in the API, block + thread in the
  data nodes), we also handle any necessary conversion to the byte order
  of our machine. Any tracing and debug information is handled in this
  thread.

  The actual handling of the NDB Signal is handled by the application thread.
  This thread receives a IC_NDB_SIGNAL struct which gives all information
  about the signal, including a reference to the IC_SOCK_BUF_PAGE such that
  the last executer of a signal in a page can return the page to the free
  area.

  We get IC_NDB_SIGNAL structs from a free list of IC_SOCK_BUF_PAGE which uses
  a size which is appropriate for the NDB Signal objects. In the same manner
  we allocate pages to receive into from a free list of IC_SOCK_BUF_PAGE with
  an appropriate page size for receiving into.

  In this manner we don't need to copy any data from the buffer we receive
  the NDB Signal into with the exception of signals that comes from more
  than one recv system call.

  Each application thread has a IC_THREAD_CONNECTION struct which is
  protected by a mutex such that this thread can easily transfer NDB
  Signals to these threads. This thread will acquire the mutex, put the
  signals into a linked list of signals waiting to be executed, check if
  the application thread is hanging on the mutex and if so wake it up
  after releasing the mutex.

  To avoid having to protect also the array of IC_THREAD_CONNECTION we
  preallocate the maximum number of IC_THREAD_CONNECTION we can have
  and set their initial state to not used.

  One problem to resolve is what to do if a thread doesn't handle it's
  signals for some reason. This could happen in an overload situation,
  it could happen also due to programming errors from the application,
  the application might send requests asynchronously and then by mistake
  never call receive to handle the sent requests. To protect the system
  in these cases we need to ensure that the thread isn't allowed to send
  any more requests if he has data waiting to be received. In this way we
  can limit the amount of memory a certain thread can hold up.
  
  In addition as an extra security precaution we can have a thread that
  walks around the IC_THREAD_CONNECTION and if it finds a slow thread it
  moves the signals from its buffers into packed buffers to avoid that
  it holds up lots of memory to contain a short signal. Eventually we
  could also decide to kill the thread from an NDB point of view. This
  would mean drop all signals and set the iClaustron Data Server API in
  such a state that the only function allowed is to deallocate the objects
  allocated in this thread. This does also require a possibility to inform
  DBTC that this thread has been dropped.
*/

#define NUM_RECEIVE_PAGES_ALLOC 2
#define NUM_NDB_SIGNAL_ALLOC 32
#define MIN_NDB_HEADER_SIZE 12

/*
  We get a linked list of IC_SOCK_BUF_PAGE which each contain a reference
  to a IC_NDB_SIGNALS object. We will ensure that these messages are posted
  to the proper thread. They will be put in a linked list of NDB signals
  waiting to be executed by this application thread.
*/
static void
post_ndb_signal(IC_SOCK_BUF_PAGE *ndb_signal_pages,
                IC_SOCK_BUF_PAGE *buf_page,
                IC_THREAD_CONNECTION **thd_conn)
{
}

/*
  A complete signal has been received and is pointed to by read_ptr, we will
  now fill in the IC_NDB_SIGNALS object such that it is efficient to
  execute the query.
*/
static int
create_ndb_signal(gchar *read_ptr,
                  guint32 message_size,
                  IC_SOCK_BUF_PAGE *ndb_signal_page)
{
  return 0;
}

static int
init_ndb_receiver(IC_NDB_RECEIVE_STATE *rec_state,
                  IC_THREAD_CONNECTION *thd_conn,
                  gchar *start_buf,
                  guint32 start_size)
{
  IC_SOCK_BUF_PAGE *buf_page;
  IC_SOCK_BUF *rec_buf_pool= rec_state->rec_buf_pool;
  if (start_size)
  {
    /*
      Before starting the NDB Protocol we might have received the first
      bytes already in a previous read socket call. Initialise the buf_page
      with this information.
    */
    if (!(buf_page= rec_buf_pool->sock_buf_op.ic_get_sock_buf_page(
            rec_buf_pool,
            &thd_conn->free_rec_pages,
            NUM_RECEIVE_PAGES_ALLOC)))
      return 1;
    memcpy(buf_page->sock_buf_page, start_buf, start_size);
    rec_state->buf_page= buf_page;
  }
  return 0;
}

static int
post_ndb_signals(IC_SOCK_BUF_PAGE *ndb_signals,
                 IC_SOCK_BUF_PAGE *receive_page,
                 IC_THREAD_CONNECTION *thd_conn)
{
  return 0;
}

static int
read_message_size(gchar *read_ptr)
{
  return 0;
}

int
ndb_receive(IC_NDB_RECEIVE_STATE *rec_state,
            IC_THREAD_CONNECTION *thd_conn)
{
  IC_CONNECTION *conn= rec_state->conn;
  IC_SOCK_BUF *rec_buf_pool= rec_state->rec_buf_pool;
  IC_SOCK_BUF *message_pool= rec_state->message_pool;
  guint32 read_size= rec_state->read_size;
  gboolean read_header_flag= rec_state->read_header_flag;
  gchar *read_ptr;
  guint32 page_size= rec_buf_pool->page_size;
  guint32 start_size, message_size;
  int ret_code;
  gboolean any_signal_received= FALSE;
  IC_SOCK_BUF_PAGE *buf_page, *new_buf_page;
  IC_SOCK_BUF_PAGE *ndb_signal_page;
  IC_SOCK_BUF_PAGE *first_ndb_signal_page= NULL;
  IC_SOCK_BUF_PAGE *last_ndb_signal_page= NULL;

  g_assert(message_pool->page_size == sizeof(IC_NDB_SIGNAL));

  /*
    We get a receive buffer from the global pool of free buffers.
    This buffer will be returned to the free pool by the last
    thread that executes a message in this pool.
  */
  if (read_size == 0)
  {
    if (!(buf_page= rec_buf_pool->sock_buf_op.ic_get_sock_buf_page(
            rec_buf_pool,
            &thd_conn->free_rec_pages,
            NUM_RECEIVE_PAGES_ALLOC)))
      goto mem_pool_error;
  }
  else
    buf_page= rec_state->buf_page;
  read_ptr= buf_page->sock_buf_page;
  start_size= read_size;
  ret_code= conn->conn_op.ic_read_connection(conn,
                                             read_ptr + read_size,
                                             page_size - read_size,
                                             &read_size);
  if (!ret_code)
  {
    /*
      We received data in the NDB Protocol, now chunk it up in its
      respective NDB Signals and send those NDB Signals to the
      thread that expects them. The actual execution of the NDB
      Signals happens in the thread that the NDB Signal is destined
      for.
    */
    read_size+= start_size;
    /*
      Check that we have at least received the header of the next
      NDB Signal first, this is at least 12 bytes in size in the
      NDB Protocol.
    */
    while (read_size >= MIN_NDB_HEADER_SIZE)
    {
      if (!read_header_flag)
      {
        message_size= read_message_size(read_ptr);
        read_header_flag= TRUE;
      }
      if (message_size > read_size)
        break;
      if (!(ndb_signal_page= message_pool->sock_buf_op.ic_get_sock_buf_page(
              message_pool,
              &thd_conn->free_ndb_signals,
              NUM_NDB_SIGNAL_ALLOC)))
        goto mem_pool_error;
      if (!first_ndb_signal_page)
      {
        first_ndb_signal_page= ndb_signal_page;
        last_ndb_signal_page= ndb_signal_page;
      }
      else
      {
        ndb_signal_page->next_sock_buf_page= last_ndb_signal_page;
        last_ndb_signal_page= ndb_signal_page;
      }
      if ((ret_code= create_ndb_signal(read_ptr,
                                       message_size,
                                       ndb_signal_page)))
        goto end; /* Protocol error discovered */

      any_signal_received= TRUE;
      read_header_flag= FALSE;
      read_size-= message_size;
      read_ptr+= message_size;
    }
    if (read_size > 0)
    {
      /* We received an incomplete NDB Signal */
      if (any_signal_received)
      {
        /*
          At least one signal was received and we need to post
          these NDB Signals and thus we need to transfer the
          incomplete message before posting the signals. When
          the signals have been posted another thread can get
          access to the socket buffer page and even release
          it before we have transferred the incomplete message
          if we post before we transfer the incomplete message.
        */
        if (!(new_buf_page= rec_buf_pool->sock_buf_op.ic_get_sock_buf_page(
                rec_buf_pool,
                &thd_conn->free_rec_pages,
                NUM_RECEIVE_PAGES_ALLOC)))
          goto mem_pool_error;

        memcpy(new_buf_page->sock_buf_page,
               buf_page->sock_buf_page,
               read_size);
        post_ndb_signals(first_ndb_signal_page, buf_page, thd_conn);
        rec_state->buf_page= new_buf_page;
        rec_state->read_size= read_size;
        rec_state->read_header_flag= read_header_flag;
      }
    }
    else
    {
      post_ndb_signals(ndb_signal_page, buf_page, thd_conn);
      rec_state->buf_page= NULL;
      rec_state->read_size= 0;
      rec_state->read_header_flag= FALSE;
    }
    return 0;
  }
  if (ret_code == END_OF_FILE)
    ret_code= 0;
end:
  while (thd_conn->free_rec_pages)
  {
    buf_page= thd_conn->free_rec_pages;
    thd_conn->free_rec_pages= thd_conn->free_rec_pages->next_sock_buf_page;
    rec_buf_pool->sock_buf_op.ic_return_sock_buf_page(rec_buf_pool,
                                                      buf_page);
  }
  while (thd_conn->free_ndb_signals)
  {
    ndb_signal_page= thd_conn->free_ndb_signals;
    thd_conn->free_ndb_signals= thd_conn->free_ndb_signals->next_sock_buf_page;
    message_pool->sock_buf_op.ic_return_sock_buf_page(message_pool,
                                                      ndb_signal_page);
  }
  while (first_ndb_signal_page)
  {
    ndb_signal_page= first_ndb_signal_page;
    first_ndb_signal_page= first_ndb_signal_page->next_sock_buf_page;
    message_pool->sock_buf_op.ic_return_sock_buf_page(message_pool,
                                                      ndb_signal_page);
  }
  return ret_code;
mem_pool_error:
  ret_code= 1;
  goto end;
}

static int
ndb_receive_one_socket(IC_NDB_RECEIVE_STATE *rec_state)
{
  return 0;
}

static int
node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  return 0;
}

