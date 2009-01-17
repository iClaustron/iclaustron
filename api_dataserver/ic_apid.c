/* Copyright (C) 2007-2009 iClaustron AB

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

/*
  The iClaustron Data API is an API to be able to access NDB Data nodes through
  a C-based API. It uses a very flexible data structure making it possible to
  describe very complex queries and allow the API to control the execution of
  this query as described by the data structure defined.

  The API can connect to one or many NDB Clusters. There is one socket
  connection for each node in each cluster.

  THREAD DESCRIPTIONS
  -------------------
  The implementation uses a wide range of threads to implement this API.

  1. Receive threads
  ------------------
  At first there is a set of receive threads. There is at least one such
  thread, but could potentially have up to one thread per node connection.
  The important part here is that each connection is always mapped to one
  and only one receiver thread. One receiver thread can however handle one
  or many socket connections. The receive threads share a thread pool to
  make sure we can control the use of CPU cores and memory allocation for
  receive threads.

  2. Send threads
  ---------------
  Each socket connection also has a send part, there is a one-to-one mapping
  between send threads and socket connections. The send thread is responsible
  for the connection set-up and tear-down, for server connections it is
  assisted in this responsibility by the listen server threads. The send
  threads and the connect threads share another thread pool to control its
  use of CPU cores and memory allocation.

  3. Connect threads (listen server threads)
  ------------------------------------------
  There is also a set of connection threads. The reason to have this is to
  handle the socket connections that are a server part. This thread is
  connected to the send thread as part of the connection set-up phase.
  There is a mapping of each socket connection which is a server side of the
  connection to one such connect thread. All server socket connections
  sharing the same hostname and port will use the same connect thread.

  4. User threads
  ---------------
  Finally there is a set of user threads. These are managed by the API user
  and not by the API.

  DATA STRUCTURE DESCRIPTIONS
  ---------------------------
  Each thread has a data structure connected to it. In addition there are
  a number of other important data structures. All data structures
  representing a thread always have one variable mutex and one variable
  cond which represents the mutex protecting this data structure and the
  condition used to communicate between threads using the data structure.
  It has also a thread variable containing the thread data structure used
  to wait for the thread to die.

  1. IC_THREAD_CONNECTION
  -----------------------
  This data structure is used to represent the user thread.

  2. IC_APID_CONNECTION
  ---------------------
  This data structures also represents the user thread, however this data
  structure focus on the API data structures and do not handle the
  protection and the communication with other threads.

  3. IC_SEND_NODE_CONNECTION
  --------------------------
  This data structure contains the data needed by the send thread. It does
  also handle the socket connection phase for both server connections and
  client connections.

  4. IC_NDB_RECEIVE_STATE
  -----------------------
  This data structure represents the receive thread.

  5. IC_LISTEN_SERVER_THREAD
  --------------------------
  This data structure represents the connection thread.

  6. IC_NDB_MESSAGE
  -----------------
  This data structure represents one NDB message and is used in the user
  API thread.

  7. IC_NDB_MESSAGE_OPAQUE_AREA
  -----------------------------
  This data structure is filled in by the receiver thread and is used to
  fill in the IC_NDB_MESSAGE data.

  8. IC_MESSAGE_ERROR_OBJECT
  --------------------------
  This data structure is used to represent an error that occurred.
*/

#include <ic_common.h>
#include <ic_apic.h>
#include <ic_apid.h>

/*
  These static functions implements the local IC_TRANSLATION_OBJ.
  For speed they're only declared as local objects such that they
  can be inlined by the compiler if so decided.
*/

IC_TRANSLATION_OBJ *create_translation_object();
int insert_object_in_translation(IC_TRANSLATION_OBJ *transl_obj,
                                 void *object);
void remove_object_from_translation(void *object);
/* End definition of IC_TRANSLATION_OBJ functions */
static int send_done_handling(IC_SEND_NODE_CONNECTION *send_node_conn);
static void
adaptive_send_algorithm_adjust(IC_SEND_NODE_CONNECTION *send_node_conn);
static void
adaptive_send_algorithm_statistics(IC_SEND_NODE_CONNECTION *send_node_conn,
                                   IC_TIMER current_time);
static void
adaptive_send_algorithm_decision(IC_SEND_NODE_CONNECTION *send_node_conn,
                                 gboolean *will_wait,
                                 IC_TIMER current_time);
static void apid_free(IC_APID_CONNECTION *apid_conn);
static int start_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn);
static void start_connect_phase(IC_APID_GLOBAL *apid_global,
                                gboolean stop_ordered,
                                gboolean signal_flag);
static void prepare_real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                                      guint32 *send_size,
                                      struct iovec *write_vector,
                                      guint32 *iovec_size);
static int real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                              struct iovec *write_vector, guint32 iovec_size,
                              guint32 send_size);
static int node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn);
static int execute_message(IC_SOCK_BUF_PAGE *ndb_message_page);
static int create_ndb_message(IC_SOCK_BUF_PAGE *message_page,
                              IC_NDB_MESSAGE_OPAQUE_AREA *ndb_message_opaque,
                              IC_NDB_MESSAGE *ndb_message,
                              guint32 *message_id);
static IC_SOCK_BUF_PAGE* get_thread_messages(IC_APID_CONNECTION *apid,
                                             glong wait_time);
static void
close_listen_server_thread(IC_LISTEN_SERVER_THREAD *listen_server_thread);
struct link_message_anchors
{
  IC_SOCK_BUF_PAGE *first_ndb_message_page;
  IC_SOCK_BUF_PAGE *last_ndb_message_page;
};
typedef struct link_message_anchors LINK_MESSAGE_ANCHORS;
static int
ndb_receive_node(IC_NDB_RECEIVE_STATE *rec_state,
                 IC_RECEIVE_NODE_CONNECTION *rec_node,
                 int *min_hash_index,
                 int *max_hash_index,
                 LINK_MESSAGE_ANCHORS *message_anchors,
                 IC_THREAD_CONNECTION *thd_conn);
static void ic_end_apid(IC_APID_GLOBAL *apid_global);

/*
  GLOBAL DATA API INITIALISATION MODULE
  -------------------------------------
  This module contains the code to initialise and set-up and tear down the
  global part of the data part. This part contains all the threads required
  to handle the Data API except the user threads which are created by the
  user application.
*/
/*
  This method is used to initialise all the data structures and allocate
  memory for the various parts required on a global level for the Data
  API. This method only allocates the memory and does not start any
  threads, it must be called before any start thread handling can be done.
*/
static IC_APID_GLOBAL*
ic_init_apid(IC_API_CONFIG_SERVER *apic)
{
  IC_APID_GLOBAL *apid_global;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_CLUSTER_COMM *cluster_comm;
  guint32 i, j;

  if (!(apid_global= (IC_APID_GLOBAL*)ic_calloc(sizeof(IC_APID_GLOBAL))))
    return NULL;
  if (!(apid_global->rec_thread_pool= ic_create_threadpool(
                        IC_MAX_RECEIVE_THREADS)))
    goto error;
  if (!(apid_global->send_thread_pool= ic_create_threadpool(
                        IC_DEFAULT_MAX_THREADPOOL_SIZE)))
    goto error;
  apid_global->apic= apic;
  apid_global->num_receive_threads= 1;
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
  if (!(apid_global->cluster_bitmap= ic_create_bitmap(NULL,
                                                      IC_MAX_CLUSTER_ID)))
    goto error;
  if (!(apid_global->mutex= g_mutex_new()))
    goto error;
  if (!(apid_global->cond= g_cond_new()))
    goto error;
  if (!(apid_global->mem_buf_pool= ic_create_socket_membuf(IC_MEMBUF_SIZE,
                                                           512)))
    goto error;
  if (!(apid_global->ndb_message_pool= ic_create_socket_membuf(0, 2048)))
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
      for (j= 1; j <= clu_conf->max_node_id; j++)
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

static void
free_send_node_conn(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  if (send_node_conn == NULL)
    return;
  if (send_node_conn->mutex)
    g_mutex_free(send_node_conn->mutex);
  if (send_node_conn->cond)
    g_cond_free(send_node_conn->cond);
  if (send_node_conn->string_memory)
    ic_free(send_node_conn->string_memory);
  ic_free(send_node_conn);
}

/*
  This method is called when shutting down a global Data API object. It requires
  all the threads to have been stopped and is only releasing the memory on the
  global Data API object.
*/
static void
ic_end_apid(IC_APID_GLOBAL *apid_global)
{
  IC_SOCK_BUF *mem_buf_pool= apid_global->mem_buf_pool;
  IC_SOCK_BUF *ndb_message_pool= apid_global->ndb_message_pool;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  guint32 i, j;

  if (!apid_global)
    return;
  if (apid_global->rec_thread_pool)
    apid_global->rec_thread_pool->tp_ops.ic_threadpool_stop(apid_global->rec_thread_pool);
  if (apid_global->send_thread_pool)
    apid_global->send_thread_pool->tp_ops.ic_threadpool_stop(
                  apid_global->send_thread_pool);
  if (apid_global->cluster_bitmap)
    ic_free_bitmap(apid_global->cluster_bitmap);
  if (mem_buf_pool)
    mem_buf_pool->sock_buf_ops.ic_free_sock_buf(mem_buf_pool);
  if (ndb_message_pool)
    ndb_message_pool->sock_buf_ops.ic_free_sock_buf(ndb_message_pool);
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
        for (j= 0; j <= IC_MAX_NODE_ID; j++)
          free_send_node_conn(cluster_comm->send_node_conn_array[j]);
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

static int
set_hostname_and_port(IC_SEND_NODE_CONNECTION *send_node_conn,
                      IC_SOCKET_LINK_CONFIG *link_config,
                      guint32 my_node_id)
{
  gchar *copy_ptr;
  guint32 first_hostname_len, second_hostname_len;
  guint32 tot_string_len, server_port_len, client_port_len;
  gchar *server_port_str= NULL;
  gchar *client_port_str= NULL;
  gchar server_port[64], client_port[64];
  /*
    We store hostname for both side of the connections on the
    send node connection object. Also strings for the port
    part.
  */
  first_hostname_len= strlen(link_config->first_hostname) + 1;
  second_hostname_len= strlen(link_config->second_hostname) + 1;
  tot_string_len= first_hostname_len;
  tot_string_len+= second_hostname_len;
  server_port_str= ic_guint64_str((guint64)link_config->server_port_number,
                                  server_port, &server_port_len);
  server_port_len++; /* Room for NULL byte */
  if (link_config->client_port_number != 0)
  {
    client_port_str= ic_guint64_str((guint64)link_config->client_port_number,
                                    client_port, &client_port_len);
    client_port_len++; /* Room for NULL byte */
  } else
    client_port_len= 0;
  tot_string_len+= server_port_len;
  tot_string_len+= client_port_len;
  if (!(send_node_conn->string_memory= ic_calloc(tot_string_len)))
    return IC_ERROR_MEM_ALLOC;
  copy_ptr= send_node_conn->string_memory;
  if (link_config->first_node_id == my_node_id)
  {
    send_node_conn->my_hostname= copy_ptr;
    copy_ptr+= first_hostname_len;
    memcpy(send_node_conn->my_hostname,
           link_config->first_hostname,
           first_hostname_len);
    send_node_conn->other_hostname= copy_ptr;
    copy_ptr+= (second_hostname_len + 1);
    memcpy(send_node_conn->other_hostname,
          link_config->second_hostname,
          second_hostname_len);
  }
  else
  {
    send_node_conn->my_hostname= copy_ptr;
    copy_ptr+= second_hostname_len;
    memcpy(send_node_conn->my_hostname,
           link_config->second_hostname,
           second_hostname_len);
    send_node_conn->other_hostname= copy_ptr;
    copy_ptr+= first_hostname_len;
    memcpy(send_node_conn->other_hostname,
           link_config->first_hostname,
           first_hostname_len);
  }
  if (link_config->server_node_id == my_node_id)
  {
    send_node_conn->my_port_number= copy_ptr;
    copy_ptr+= server_port_len;
    memcpy(send_node_conn->my_port_number,
           server_port_str,
           server_port_len);
    if (client_port_len == 0)
      send_node_conn->other_port_number= NULL;
    else
    {
      send_node_conn->other_port_number= copy_ptr;
      copy_ptr+= client_port_len;
      memcpy(send_node_conn->other_port_number,
             client_port_str,
             client_port_len);
    }
  }
  else
  {
    send_node_conn->other_port_number= copy_ptr;
    copy_ptr+= server_port_len;
    memcpy(send_node_conn->other_port_number,
           server_port_str,
           server_port_len);
    if (client_port_len == 0)
      send_node_conn->my_port_number= NULL;
    else
    {
      send_node_conn->my_port_number= copy_ptr;
      copy_ptr+= client_port_len;
      memcpy(send_node_conn->my_port_number,
             client_port_str,
             client_port_len);
    }
  }
  g_assert(((guint32)(copy_ptr - send_node_conn->string_memory)) ==
            tot_string_len);
  return 0;
}

/*
  This is the method used to start up all the threads in the Data API,
  including the send threads, the receive threads and the listen server
  threads. It will also start connecting to the various clusters that
  the Data API should be connected to.
*/
static int
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
    my_node_id= clu_conf->my_node_id;
    cluster_comm= grid_comm->cluster_comm_array[cluster_id];
    g_assert(cluster_comm);
    for (node_id= 1; node_id <= clu_conf->max_node_id; node_id++)
    {
      link_config= NULL;
      if (node_id == my_node_id ||
          (link_config= apic->api_op.ic_get_communication_object(apic,
                            cluster_id,
                            node_id,
                            my_node_id)))
      {
        g_assert(clu_conf->node_config[node_id]);
        send_node_conn= cluster_comm->send_node_conn_array[node_id];
        g_assert(send_node_conn);
        /* We have found a node to connect to, start connect thread */
        if (node_id != my_node_id)
        {
          if ((error= set_hostname_and_port(send_node_conn,
                                            link_config,
                                            my_node_id)))
            goto error;
          send_node_conn->max_wait_in_nanos=
                 (IC_TIMER)link_config->socket_max_wait_in_nanos;
        }
        else
          send_node_conn->max_wait_in_nanos= 0;
        send_node_conn->apid_global= apid_global;
        send_node_conn->link_config= link_config;
        send_node_conn->my_node_id= my_node_id;
        send_node_conn->other_node_id= node_id;
        send_node_conn->cluster_id= cluster_id;
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

/*
  This method is used to tear down the global Data API part. It will
  disconnect all the connections to all cluster nodes and stop all
  Data API threads.
*/
static int
ic_apid_global_disconnect(IC_APID_GLOBAL *apid_global)
{
  start_connect_phase(apid_global, TRUE, FALSE);
  return 0;
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

  /*
    There is no protection of this array since there is only one
    occasion when we remove things from it, this is when the
    API is deallocated. We insert things by inserting pointers to
    objects already fully prepared, also the size of the arrays are
    prepared for the worst case and thus no array growth is needed.
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
        if (send_node_conn)
        {
          g_mutex_lock(send_node_conn->mutex);
          if (send_node_conn->thread &&
              !send_node_conn->send_thread_ended)
          {
            if (stop_ordered)
              send_node_conn->stop_ordered= TRUE;
            /* Wake up send thread to connect/shutdown */
            if (signal_flag)
              g_cond_signal(send_node_conn->cond);
            if (stop_ordered)
            {
              /*
                In this we need to wait for send thread to stop before
                we continue.
              */
              g_mutex_unlock(send_node_conn->mutex);
              g_thread_join(send_node_conn->thread);
              g_mutex_lock(send_node_conn->mutex);
            }
          }
          g_mutex_unlock(send_node_conn->mutex);
        }
      }
    }
  }
  DEBUG_RETURN_EMPTY;
}

/*
  External interface to GLOBAL DATA API INITIALISATION MODULE
  -----------------------------------------------------------
*/
IC_APID_GLOBAL*
ic_connect_apid_global(IC_API_CONFIG_SERVER *apic,
                       int *ret_code,
                       gchar **err_str)
{
  IC_APID_GLOBAL *apid_global;
  gchar *err_string;
  int error;

  if (!(apid_global= ic_init_apid(apic)))
  {
    *ret_code= IC_ERROR_MEM_ALLOC;
    goto error;
  }
  if ((error= ic_apid_global_connect(apid_global)))
  {
    ic_end_apid(apid_global);
    *ret_code= error;
    goto error;
  }
  return apid_global;
error:
  err_string= ic_common_fill_error_buffer(NULL, 0, *ret_code, *err_str);
  g_assert(err_string == *err_str);
  return NULL;
}

int
ic_disconnect_apid_global(IC_APID_GLOBAL *apid_global)
{
  int ret_code;

  if (!(ret_code= ic_apid_global_disconnect(apid_global)))
    return ret_code;
  ic_end_apid(apid_global);
  return 0;
}

/*
  DATA API Connection Module
  --------------------------
  This module contains all the methods that are part of the Data API
  Connection interface. This interface contains methods to start
  transactions and control execution of transactions. Each thread
  should have its own Data API Connection module.
*/
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
  grid_comm= apid_global->grid_comm;
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
  apid_conn->apid_conn_ops.ic_free= apid_free;
  return apid_conn;

error:
  apid_free(apid_conn);
end:
  g_mutex_unlock(apid_global->thread_id_mutex);
  return NULL;
}

/*
  SEND THREAD MODULE
  ------------------
  This module contains the code to run the send thread and the supporting
  methods to perform send operations through the Data API.
*/
static void
run_active_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  guint32 send_size;
  guint32 iovec_size;
  int error;
  struct iovec write_vector[MAX_SEND_BUFFERS];

  g_mutex_lock(send_node_conn->mutex);
  send_node_conn->send_thread_is_sending= FALSE;
  send_node_conn->send_thread_active= TRUE;
  do
  {
    if (send_node_conn->stop_ordered)
      break;
    if (!send_node_conn->node_up)
    {
      node_failure_handling(send_node_conn);
      break;
    }
    g_assert(send_node_conn->send_thread_is_sending);
    g_assert(send_node_conn->send_active);
    prepare_real_send_handling(send_node_conn, &send_size,
                               write_vector, &iovec_size);
    g_mutex_unlock(send_node_conn->mutex);
    error= real_send_handling(send_node_conn, write_vector,
                              iovec_size, send_size);
    g_mutex_lock(send_node_conn->mutex);
    if (error)
    {
      node_failure_handling(send_node_conn);
      break;
    }
    /* Handle send done */
    if (send_node_conn->first_sbp)
    {
      /* All buffers have been sent, we can go to sleep again */
      send_node_conn->send_active= FALSE;
      send_node_conn->send_thread_is_sending= FALSE;
      g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
      continue;
    }
    else if (send_node_conn->node_up)
    {
      /* There are more buffers to send, we need to continue sending */
      continue;
    }
    else
    {
      node_failure_handling(send_node_conn);
      break;
    }
  } while (1);
  send_node_conn->send_active= FALSE;
  send_node_conn->send_thread_active= FALSE;
  send_node_conn->send_thread_is_sending= FALSE;
  g_mutex_unlock(send_node_conn->mutex);
  return;
}

/*
  Protocol to start up a connection in the NDB Protocol.
  Client:
    Send "ndbd"<CR>
    Send "ndbd passwd"<CR>
  Server:
    Send "ok"<CR>
  Client:
    Send "2 3"<CR> (2 is client node id and 3 is server node id)
  Server:
    Send "1 1"<CR> (1 is transporter type for client/server)
  Connection is ready to send and receive NDB Protocol messages
*/
static int
authenticate_client_connection(IC_CONNECTION *conn,
                               guint32 my_nodeid,
                               guint32 server_nodeid)
{
  gchar *read_buf;
  guint32 read_size;
  gchar send_buf[64];
  int error;

  g_snprintf(send_buf, (int)64, "%u %u", my_nodeid, server_nodeid);
  if ((error= ic_send_with_cr(conn, "ndbd")) ||
      (error= ic_send_with_cr(conn, "ndbd passwd")) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "ok")) ||
      (error= ic_send_with_cr(conn, send_buf)) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "1 1")))
    return error;
  return 0;
}

static int authenticate_server_connection(IC_CONNECTION *conn,
                                          guint32 my_nodeid,
                                          guint32 *client_nodeid)
{
  guint32 read_size;
  gchar *read_buf;
  int error;
  guint32 len;
  guint64 my_id, client_id;

  /* Retrieve client node id and verify that server is my node id */
  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "ndbd")) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "ndbd passwd")) ||
      (error= ic_send_with_cr(conn, "ok")) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= AUTHENTICATE_ERROR, FALSE) ||
      (ic_conv_str_to_int(read_buf, &client_id, &len)) ||
      (read_buf[len] != ' ') ||
      (len= len + 1, FALSE) ||
      (ic_conv_str_to_int(&read_buf[len], &my_id, &len)) ||
      ((guint64)my_nodeid != my_id) ||
      (client_id >= IC_MAX_NODE_ID) ||
      (error= ic_send_with_cr(conn, "1 1")) ||
      (error= AUTHENTICATE_ERROR, FALSE) ||
      (ic_conv_str_to_int(read_buf, &client_id, &len)) ||
      (read_buf[len] != ' ') ||
      (len++, FALSE) ||
      (ic_conv_str_to_int(&read_buf[len], &my_id, &len)) ||
      ((guint64)my_nodeid != my_id) ||
      (client_id >= IC_MAX_NODE_ID) ||
      (error= ic_send_with_cr(conn, "ok")))
    return error;
  *client_nodeid= client_id;
  return 0;
}

/*
  This callback is called by the object connecting the server to the
  client using the NDB Protocol. This method calls a method on the
  IC_API_CONFIG_SERVER object implementing the server side of the set-up
  of this protocol.
*/
static int
server_api_connect(void *data)
{
  IC_SEND_NODE_CONNECTION *send_node_conn= (IC_SEND_NODE_CONNECTION*)data;
  return authenticate_server_connection(send_node_conn->conn,
                                        send_node_conn->my_node_id,
                                        &send_node_conn->other_node_id);
}

/*
  This callback is called by the object connecting the client to the
  server using the NDB Protocol. This method calls a method on the
  IC_API_CONFIG_SERVER object implementing the client side of the set-up
  of this protocol.
*/
static int
client_api_connect(void *data)
{
  IC_SEND_NODE_CONNECTION *send_node_conn= (IC_SEND_NODE_CONNECTION*)data;
  return authenticate_client_connection(send_node_conn->conn,
                                        send_node_conn->my_node_id,
                                        send_node_conn->other_node_id);
}

/*
  The send thread has two cases, client connecting and server connecting.
  In the client case the thread itself will connect.
  In the server case there are many connections that uses the same server
  port and to resolve this we use a special server connect thread. This
  method simply waits for this thread to report any connections that
  occurred.
*/
static int
connect_by_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn,
                       gboolean is_client_part)
{
  IC_CONNECTION *conn;
  IC_SOCKET_LINK_CONFIG *link_config;
  IC_LISTEN_SERVER_THREAD *listen_server_thread;
  gboolean stop_ordered= FALSE;
  IC_THREADPOOL_STATE *tp_state;
  int ret_code= 0;

  if (is_client_part)
  {
    conn= send_node_conn->conn;
    link_config= send_node_conn->link_config;
    if (!send_node_conn->other_port_number ||
        !send_node_conn->my_port_number)
      return IC_ERROR_INCONSISTENT_DATA;
    conn->conn_op.ic_prepare_client_connection(conn,
                                         send_node_conn->other_hostname,
                                         send_node_conn->other_port_number,
                                         send_node_conn->my_hostname,
                                         send_node_conn->my_port_number);
    conn->conn_op.ic_prepare_extra_parameters(
             conn,
             link_config->socket_maxseg_size,
             link_config->is_wan_connection,
             link_config->socket_read_buffer_size,
             link_config->socket_write_buffer_size);
    if ((ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL)))
      return ret_code;
  }
  else /* Server connection */
  {
    /*
      The actual connection set-up is done by the listen server
      thread. So all we need to do is to communicate with this
      thread. We take mutexes in a careful manner to ensure we
      don't create a mutex deadlock situation.

      It is important here to check for connection being set-up,
      checking if we need to start the listen server thread connect
      phase and finally also checking if it's desired that we simply
      stop the send thread due to stopping the API instance.
    */
    g_mutex_lock(send_node_conn->mutex);
    while (!send_node_conn->connection_up || send_node_conn->stop_ordered)
    {
      if (send_node_conn->stop_ordered)
        stop_ordered= TRUE;
      listen_server_thread= send_node_conn->listen_server_thread;
      g_mutex_unlock(send_node_conn->mutex);
      if (!stop_ordered)
      {
        /*
          This is just to ensure that someone kicks the listen server
          thread into action.
        */
        g_mutex_lock(listen_server_thread->mutex);
        if (!listen_server_thread->started)
        {
          /* Start the listening on the server socket */
          listen_server_thread->started= TRUE;
          g_cond_signal(listen_server_thread->cond);
        }
        g_mutex_unlock(listen_server_thread->mutex);
      }
      else /* stop_ordered == TRUE */
      {
        /*
          Remove our send node connection from the list on this listen
          server thread.
        */
        g_mutex_lock(listen_server_thread->mutex);
        listen_server_thread->first_send_node_conn=
          g_list_remove(listen_server_thread->first_send_node_conn,
                        (void*)send_node_conn);
        if (listen_server_thread->first_send_node_conn == NULL)
        {
          /*
            We are the last send thread to use this listen server
            thread, this means we are responsible to stop this thread.
            We order the thread to stop, signal it in case it is
            in a cond wait and finally wait for the thread to complete.
            When the thread is completed we clean up everything in the
            listen server thread.
          */
          listen_server_thread->stop_ordered= TRUE;
          g_cond_signal(listen_server_thread->cond);
          tp_state= send_node_conn->apid_global->send_thread_pool;
          tp_state->tp_ops.ic_threadpool_join(tp_state,
                                              listen_server_thread->thread_id);
          close_listen_server_thread(listen_server_thread);
        }
        else
          g_mutex_unlock(listen_server_thread->mutex);
        return 1;
      }
      if (stop_ordered)
      {
        /*
          We have completed clean-up for our send node connection. We are now
          ready to stop the send thread by returning with a non-zero return
          code.
        */
        ret_code= 1;
        break;
      }
      g_mutex_lock(send_node_conn->mutex);
      if (!send_node_conn->stop_ordered)
        g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
    }
    g_mutex_unlock(send_node_conn->mutex);
  }
  return ret_code;
}

static int
check_timeout_func(__attribute__ ((unused)) void *timeout_obj, int timer)
{
  if (timer < 2)
    return 0;
  else
    return 1;
}

static void
close_listen_server_thread(IC_LISTEN_SERVER_THREAD *listen_server_thread)
{
  IC_CONNECTION *conn;
  if (listen_server_thread)
  {
    if ((conn= listen_server_thread->conn))
      conn->conn_op.ic_free_connection(conn);
    if (listen_server_thread->mutex)
      g_mutex_free(listen_server_thread->mutex);
    if (listen_server_thread->cond)
      g_cond_free(listen_server_thread->cond);
    if (listen_server_thread->first_send_node_conn)
      g_list_free(listen_server_thread->first_send_node_conn);
    listen_server_thread->conn= NULL;
    listen_server_thread->mutex= NULL;
    listen_server_thread->cond= NULL;
    listen_server_thread->first_send_node_conn= NULL;
    listen_server_thread->thread_id= IC_MAX_UINT32;
  }
}

static gboolean
check_connection(IC_SEND_NODE_CONNECTION *send_node_conn,
                 __attribute__ ((unused)) IC_CONNECTION *conn)
{
  if (!send_node_conn->connection_up)
    return FALSE;
  else
    return TRUE;
}

static gpointer
run_server_connect_thread(void *data)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_LISTEN_SERVER_THREAD *listen_server_thread=
      (IC_LISTEN_SERVER_THREAD*)thread_state->object;
  IC_CONNECTION *fork_conn, *conn;
  IC_SEND_NODE_CONNECTION *iter_send_node_conn;
  GList *list_iterator;
  gboolean stop_ordered, found;
  int ret_code;

  g_mutex_lock(listen_server_thread->mutex);
  conn= listen_server_thread->conn;
  if ((ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL)))
    listen_server_thread->stop_ordered= TRUE;
  /* We report start-up complete to the thread starting us */
  g_cond_signal(listen_server_thread->cond);
  if (ret_code)
  {
    g_mutex_unlock(listen_server_thread->mutex);
    thread_state->ts_ops.ic_thread_stops(thread_state);
    return NULL;
  }
  g_cond_wait(listen_server_thread->cond, listen_server_thread->mutex);
  g_mutex_unlock(listen_server_thread->mutex);
  /* We have been asked to start listening and so we start listening */
  do
  {
    if (!(ret_code= conn->conn_op.ic_accept_connection(conn,
                  check_timeout_func, (void*)listen_server_thread)))
    {
      if (!(fork_conn= conn->conn_op.ic_fork_accept_connection(conn,
                                                     FALSE))) /* No mutex */
        break;
      /*
         We have a new connection, deliver it to the send thread and
         receive thread.
      */
      g_mutex_lock(listen_server_thread->mutex);
      if (listen_server_thread->stop_ordered)
      {
        g_mutex_unlock(listen_server_thread->mutex);
        break;
      }
      found= FALSE;
      list_iterator= g_list_first(listen_server_thread->first_send_node_conn);
      while (list_iterator)
      {
        iter_send_node_conn= (IC_SEND_NODE_CONNECTION*)list_iterator->data;
        /*
          Here we are doing something dangerous, we're locking a mutex
          while already having one, this means that we're by no means
          allowed to do the opposite, we must NEVER take a mutex on
          listen_server_thread while holding a send_node_conn mutex.
          This would lead to a deadlock situation.
          To not do this would however instead lead to extremely complex
          code which we want to avoid. It's necessary that the list of
          send node connections are stable while we are searching for the
          right one that uses this connection.
        */
        g_mutex_lock(iter_send_node_conn->mutex);
        if (!check_connection(iter_send_node_conn, fork_conn))
        {
          /*
            We have found the right send node connection, give the
            connection over to the send thread and also start a
            receiver thread for this node.
          */
          found= TRUE;
          iter_send_node_conn->connection_up= TRUE;
          g_cond_signal(iter_send_node_conn->cond);
          g_mutex_unlock(iter_send_node_conn->mutex);
          break;
        }
        g_mutex_unlock(iter_send_node_conn->mutex);
        list_iterator= g_list_next(list_iterator);
      }
      g_mutex_unlock(listen_server_thread->mutex);
      if (!found)
      {
        /*
          Somebody tried to connect and we weren't able to detect a node
          which this would be a connection for, thus we will disconnect
          and continue waiting for new connections. Freeing a socket in
          the iClaustron Communication API also will close the socket
          if it's still open.
        */
        fork_conn->conn_op.ic_free_connection(fork_conn);
      }
    }
    else
    {
      /*
        We got a timeout, check if we're ordered to stop and if so
        we will stop. Otherwise we'll simply continue.
      */
      g_mutex_lock(listen_server_thread->mutex);
      stop_ordered= listen_server_thread->stop_ordered;
      g_mutex_unlock(listen_server_thread->mutex);
      if (stop_ordered)
        break;
    }
  } while (1);
  /*
    We got memory allocation failure most likely, it's not so easy to
    continue in this case, so we tell all dependent send connections
    that stop has been ordered. Those that are already connected will
    remain so, they will be dropped as soon as their connection is
    broken since the listen server thread has set stop_ordered also.
  */
  g_mutex_lock(listen_server_thread->mutex);
  listen_server_thread->stop_ordered= TRUE;
  list_iterator= g_list_first(listen_server_thread->first_send_node_conn);
  while (list_iterator)
  {
    iter_send_node_conn= (IC_SEND_NODE_CONNECTION*)list_iterator->data;
    g_mutex_lock(iter_send_node_conn->mutex);
    if (!iter_send_node_conn->send_thread_active)
    {
      iter_send_node_conn->stop_ordered= TRUE;
      g_cond_signal(iter_send_node_conn->cond);
    }
    g_mutex_unlock(iter_send_node_conn->mutex);
    list_iterator= g_list_next(list_iterator);
  }
  g_mutex_unlock(listen_server_thread->mutex);
  return NULL;
}

static gpointer
run_send_thread(void *data)
{
  gboolean is_client_part;
  gboolean found= FALSE;
  int ret_code;
  guint32 i, listen_inx;
  IC_CONNECTION *send_conn;
  IC_APID_GLOBAL *apid_global;
  IC_THREADPOOL_STATE *send_tp;
  IC_LISTEN_SERVER_THREAD *listen_server_thread= NULL;
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
  if (is_client_part)
  {
    send_node_conn->conn= ic_create_socket_object(
                   is_client_part,
                   FALSE,  /* Mutex supplied by API code instead */
                   FALSE,  /* This thread is already a connection thread */
                   CONFIG_READ_BUF_SIZE, /* Used by authentication function */
                   client_api_connect,   /* Authentication function */
                   (void*)send_node_conn);  /* Authentication object */
    if (send_node_conn->conn == NULL)
      send_node_conn->stop_ordered= TRUE;
  }
  else
  {
    /*
      Here we need to ensure there is a thread handling this server
      connection. We ensure there is one thread per port and this
      thread will start connections for the various send threads
      and report to them when a connection has been performed.
      When we come here to start things up we're still in single-threaded
      mode, so we don't need to protect things through mutexes.
    */
    apid_global= send_node_conn->apid_global;
    send_tp= apid_global->send_thread_pool;
    send_conn= send_node_conn->conn;
    for (i= 0; i < apid_global->max_listen_server_threads; i++)
    {
      listen_server_thread= apid_global->listen_server_thread[i];
      if (listen_server_thread)
      {
        if (!send_conn->conn_op.ic_cmp_connection(listen_server_thread->conn,
                                            send_node_conn->my_hostname,
                                            send_node_conn->my_port_number))
        {
          send_node_conn->listen_server_thread= listen_server_thread;
          found= TRUE;
          break;
        }
      }
    }
    if (!found)
    {
      ret_code= 1;
      do
      {
        /* Found a new hostname+port combination, we need another*/
        listen_inx= apid_global->max_listen_server_threads;
        if (!(apid_global->listen_server_thread[listen_inx]=
  	  (IC_LISTEN_SERVER_THREAD*)
            ic_calloc(sizeof(IC_LISTEN_SERVER_THREAD))))
          break;
        /* Successful allocation of memory */
        listen_server_thread= apid_global->listen_server_thread[listen_inx];
        send_node_conn->listen_server_thread= listen_server_thread;
        if (!(listen_server_thread->conn= ic_create_socket_object(
                   FALSE,  /* This is a server connection */
                   FALSE,  /* Mutex supplied by API code instead */
                   FALSE,  /* This thread is already a connection thread */
                   CONFIG_READ_BUF_SIZE, /* Used by authentication function */
                   server_api_connect,   /* Authentication function */
                   (void*)send_node_conn)))  /* Authentication object */
          break;
        listen_server_thread->conn->conn_op.ic_prepare_server_connection(
          listen_server_thread->conn,
          send_node_conn->my_hostname,
          send_node_conn->my_port_number,
          NULL, NULL, /* Any client can connect, we check after connect */
          0,          /* Default backlog */
          TRUE);      /* Retain listen socket */
        if (!(listen_server_thread->mutex= g_mutex_new()))
          break;
        if (!(listen_server_thread->cond= g_cond_new()))
          break;
        if (!(listen_server_thread->first_send_node_conn=
              g_list_prepend(NULL, (void*)send_node_conn)))
          break;
        g_mutex_lock(listen_server_thread->mutex);
        if ((ret_code= send_tp->tp_ops.ic_threadpool_start_thread(
                                  send_tp,
                                  &listen_server_thread->thread_id,
                                  run_server_connect_thread,
                                  (gpointer)listen_server_thread,
                                  IC_SMALL_STACK_SIZE)))
                                  
        {
          g_mutex_unlock(listen_server_thread->mutex);
          break;
        }
        /* Wait for thread to be started */
        g_cond_wait(listen_server_thread->cond, listen_server_thread->mutex);
        if (!listen_server_thread->stop_ordered)
        {
          ret_code= 0;
        }
        else
        {
          send_tp->tp_ops.ic_threadpool_join(send_tp, listen_server_thread->thread_id);
        }
      } while (0);
      if (ret_code != 0)
      {
        /* Error handling */
        send_node_conn->stop_ordered= TRUE;
        g_mutex_unlock(listen_server_thread->mutex);
      }
    }
    else
    {
       /* Found an existing a new thread to use, need not start a new thread */
      g_mutex_lock(listen_server_thread->mutex);
      if (!(listen_server_thread->first_send_node_conn=
            g_list_prepend(listen_server_thread->first_send_node_conn,
                           (void*)send_node_conn)))
        send_node_conn->stop_ordered= TRUE;
      g_mutex_unlock(listen_server_thread->mutex);
    }
  }
  g_mutex_lock(send_node_conn->mutex);
  g_cond_signal(send_node_conn->cond);
  if (send_node_conn->stop_ordered)
  {
    g_mutex_unlock(send_node_conn->mutex);
    goto end;
  }
  g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
  /*
    The main thread have started up all threads and all communication objects
    have been created, it's now time to connect to the clusters.
  */
  g_mutex_unlock(send_node_conn->mutex);
  while (1)
  {
    if ((ret_code= connect_by_send_thread(send_node_conn,
                                          is_client_part)))
      goto end;
    run_active_send_thread(send_node_conn);
  }
end:
  g_mutex_lock(send_node_conn->mutex);
  close_listen_server_thread(listen_server_thread);
  send_node_conn->send_thread_ended= TRUE;
  g_mutex_unlock(send_node_conn->mutex);
  return NULL;
}

static int
start_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  GError *error;
  int ret_code;

  if (send_node_conn->my_node_id == send_node_conn->other_node_id)
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
  if (!(send_node_conn->thread=
        g_thread_create_full(run_send_thread,
                             (gpointer)send_node_conn,
                             IC_SMALL_STACK_SIZE, /* Stack size */
                             TRUE,                /* Joinable */
                             TRUE,                /* Bound */
                             G_THREAD_PRIORITY_NORMAL,
                             &error)))
  {
    g_mutex_unlock(send_node_conn->mutex);
    return IC_ERROR_MEM_ALLOC;
  }
  g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
  /*
    The send thread is done with its start-up phase and we're ready to start
    the next send thread.
  */
  ret_code= 0;
  if (send_node_conn->stop_ordered)
    ret_code= IC_ERROR_MEM_ALLOC;
  g_mutex_unlock(send_node_conn->mutex);
  return ret_code;
}

/*
  SEND MESSAGE MODULE
  -------------------
  To send messages using the NDB Protocol we have a new struct
  IC_SEND_NODE_CONNECTION which is used by the application thread plus
  one send thread per node.

  Before invoking the send method, the application thread packs a number of
  NDB messages in the NDB Protocol format into a number of IC_SOCK_BUF_PAGE's.
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

  The adaptive send algorithm is also handled inside the mutex, thus we will
  set the send to active only after deciding whether it's a good idea to wait
  with sending.
*/

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
node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  IC_APID_GLOBAL *apid_global= send_node_conn->apid_global;
  IC_SOCK_BUF *mem_buf_pool= apid_global->mem_buf_pool;
  mem_buf_pool->sock_buf_ops.ic_return_sock_buf_page(
    mem_buf_pool, send_node_conn->first_sbp);
  return 0;
}

static void
prepare_real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                           guint32 *send_size,
                           struct iovec *write_vector,
                           guint32 *iovec_size)
{
  IC_SOCK_BUF_PAGE *loc_next_send, *loc_last_send;
  guint32 loc_send_size= 0, iovec_index= 0;
  DEBUG_ENTRY("prepare_real_send_handling");

  loc_next_send= send_node_conn->first_sbp;
  loc_last_send= NULL;
  do
  {
    write_vector[iovec_index].iov_base= loc_next_send->sock_buf;
    write_vector[iovec_index].iov_len= loc_next_send->size;
    iovec_index++;
    loc_send_size+= loc_next_send->size;
    loc_last_send= loc_next_send;
    loc_next_send= loc_next_send->next_sock_buf_page;
  } while (loc_send_size < MAX_SEND_SIZE &&
           iovec_index < MAX_SEND_BUFFERS &&
           loc_next_send);
  send_node_conn->release_sbp= send_node_conn->first_sbp;
  send_node_conn->first_sbp= loc_next_send;
  loc_last_send->next_sock_buf_page= NULL;
  if (!loc_next_send)
    send_node_conn->last_sbp= NULL;
  *iovec_size= iovec_index;
  *send_size= loc_send_size;
  send_node_conn->queued_bytes-= loc_send_size;
  DEBUG_RETURN_EMPTY;
}

/*
  real_send_handling is not executed with mutex protection, it is
  however imperative that prepare_real_send_handling and real_send_handling
  is executed in the same thread since the release_sbp is only used to
  communicate between those two methods and therefore not protected.
*/
static int
real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                   struct iovec *write_vector, guint32 iovec_size,
                   guint32 send_size)
{
  int error;
  IC_CONNECTION *conn= send_node_conn->conn;
  IC_APID_GLOBAL *apid_global= send_node_conn->apid_global;
  IC_SOCK_BUF *mem_buf_pool= apid_global->mem_buf_pool;
  DEBUG_ENTRY("real_send_handling");
  error= conn->conn_op.ic_writev_connection(conn,
                                            write_vector,
                                            iovec_size,
                                            send_size, 2);

  /* Release memory buffers used in send */
  mem_buf_pool->sock_buf_ops.ic_return_sock_buf_page(
    mem_buf_pool, send_node_conn->release_sbp);
  send_node_conn->release_sbp= NULL;

  if (error)
  {
    /*
      We failed to send and in this case we need to invoke node failure
      handling since either the connection was broken or it was slow to
      the point of being similar to broken.
    */
    g_mutex_lock(send_node_conn->mutex);
    node_failure_handling(send_node_conn);
    g_mutex_unlock(send_node_conn->mutex);
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
    send_node_conn->send_thread_is_sending= TRUE;
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
  We handle reception of NDB messages in one or more separate threads, each
  thread can handle one or more sockets. If there is only one socket to
  handle it can use socket read immediately, otherwise we can use either
  poll, or epoll on Linux or other similar variant on other operating
  system.

  This thread only handles reception of the lowest layer of the NDB Protocol.

  We mainly transport a header about the message to enable a quick conversion
  of the message to a IC_NDB_MESSAGE struct in the user thread. In this
  struct we set up data, their sizes, message numbers, receiving module,
  sender module, receiving node id, sender node id and various other data.

  The actual handling of the NDB message is handled by the application thread.
  This thread receives a IC_NDB_MESSAGE struct which gives all information
  about the signal, including a reference to the IC_SOCK_BUF_PAGE such that
  the last executer of a signal in a page can return the page to the free
  area.

  In this manner we don't need to copy any data from the buffer we receive
  the NDB message into with the exception of messages that comes from more
  than one recv system call.

  Each application thread has a IC_THREAD_CONNECTION struct which is
  protected by a mutex such that this thread can easily transfer NDB
  messages to these threads. This thread will acquire the mutex, put the
  messages into a linked list of signals waiting to be executed, check if
  the application thread is hanging on the mutex and if so wake it up
  after releasing the mutex.

  To avoid having to protect also the array of IC_THREAD_CONNECTION we
  preallocate the maximum number of IC_THREAD_CONNECTION we can have
  and set their initial state to not used.

  One problem to resolve is what to do if a thread doesn't handle it's
  messages for some reason. This could happen in an overload situation,
  it could happen also due to programming errors from the application,
  the application might send requests asynchronously and then by mistake
  never call receive to handle the sent requests. To protect the system
  in these cases we need to ensure that the thread isn't allowed to send
  any more requests if he has data waiting to be received. In this way we
  can limit the amount of memory a certain thread can hold up.
  
  In addition as an extra security precaution we can have a thread that
  walks around the IC_THREAD_CONNECTION and if it finds a slow thread it
  moves the messages from its buffers into packed buffers to avoid that
  it holds up lots of memory to contain a short message. Eventually we
  could also decide to kill the thread from an NDB point of view. This
  would mean drop all messages and set the iClaustron Data Server API in
  such a state that the only function allowed is to deallocate the objects
  allocated in this thread. This does also require a possibility to inform
  DBTC that this thread has been dropped.
*/

#define NUM_RECEIVE_PAGES_ALLOC 2
#define NUM_NDB_SIGNAL_ALLOC 32
#define MIN_NDB_HEADER_SIZE 12
#define NUM_THREAD_LISTS 16
#define MAX_LOOP_CHECK 100000
#define IC_MAX_TIME 10000000

/*
  MESSAGE LOGIC MODULE
  --------------------
  This module contains the code of all messages received in the Data Server
  API. This code signals to the EXECUTE MESSAGE MODULE if there have been
  events on the user level which needs to be taken care of. These user
  level events are then usually handled by callbacks after handling all
  messages received. It's of vital importance that the user thread doesn't
  block for an extended period while executing messages.
*/

struct ic_exec_message_func
{
  int (*ic_exec_message_func) (IC_NDB_MESSAGE *message,
                               IC_MESSAGE_ERROR_OBJECT *error_object);
};
typedef struct ic_exec_message_func IC_EXEC_MESSAGE_FUNC;

static IC_EXEC_MESSAGE_FUNC *ic_exec_message_func_array[2][1024];

static int
execSCAN_TABLE_CONF_v0(__attribute__ ((unused)) IC_NDB_MESSAGE *ndb_message,
               __attribute__ ((unused))IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
execPRIMARY_KEY_OP_CONF_v0(__attribute__ ((unused)) IC_NDB_MESSAGE *ndb_message,
               __attribute__ ((unused))IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
execTRANSACTION_CONF_v0(__attribute__ ((unused)) IC_NDB_MESSAGE *ndb_message,
               __attribute__ ((unused))IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
handle_key_ai(__attribute__ ((unused)) IC_KEY_OPERATION *key_op,
              __attribute__ ((unused)) IC_TRANSACTION *trans,
              __attribute__ ((unused)) guint32 *ai_data,
              __attribute__ ((unused)) guint32 data_size,
              __attribute__ ((unused))IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
handle_scan_ai(__attribute__ ((unused)) IC_SCAN_OPERATION *scan_op,
               __attribute__ ((unused)) IC_TRANSACTION *trans,
               __attribute__ ((unused)) guint32 *ai_data,
               __attribute__ ((unused)) guint32 data_size,
               __attribute__ ((unused))IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

/*
  Signal Format:
  Word 0: Connection object
  Word 1: Transaction id part 1
  Word 2: Transaction id part 2
  One segment which contains the actual ATTRINFO data
*/
static int
execATTRIBUTE_INFO_v0(IC_NDB_MESSAGE *ndb_message,
                      IC_MESSAGE_ERROR_OBJECT *error_object)
{
  guint32 *header_data= ndb_message->segment_ptr[0];
  guint32 header_size= ndb_message->segment_size[0];
  guint32 *attrinfo_data;
  void *connection_obj;
  IC_KEY_OPERATION *key_op;
  IC_SCAN_OPERATION *scan_op;
  IC_TRANSACTION *trans_op;
  IC_DYNAMIC_TRANSLATION *dyn_trans= 0;
  guint32 data_size;
  guint64 connection_ptr= header_data[0];
  guint32 transid_part1= header_data[1];
  guint32 transid_part2= header_data[2];

  if (dyn_trans->dt_ops.ic_get_object(dyn_trans,
                                      connection_ptr,
                                      &connection_obj))
  {
    return 1;
  }
  key_op= (IC_KEY_OPERATION*)connection_obj;
  if (ndb_message->num_segments == 1)
  {
    attrinfo_data= ndb_message->segment_ptr[1];
    data_size= ndb_message->segment_size[1];
    g_assert(header_size == 3);
  }
  else
  {
    g_assert(ndb_message->num_segments == 0);
    g_assert(header_size <= 25);
    attrinfo_data= &header_data[3];
    data_size= header_size - 3;
  }
  trans_op= key_op->transaction;
  if (trans_op->transaction_id[0] != transid_part1 ||
      trans_op->transaction_id[1] != transid_part2)
  {
    g_assert(FALSE);
    return 1;
  }
  if (key_op->operation_type == SCAN_OPERATION)
  {
    scan_op= (IC_SCAN_OPERATION*)key_op;
    return handle_scan_ai(scan_op,
                          trans_op,
                          attrinfo_data,
                          data_size,
                          error_object);
  }
  return handle_key_ai(key_op,
                       trans_op,
                       attrinfo_data,
                       data_size,
                       error_object);
}

static int
execTRANSACTION_REF_v0(__attribute__ ((unused)) IC_NDB_MESSAGE *ndb_message,
             __attribute__ ((unused)) IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
execPRIMARY_KEY_OP_REF_v0(__attribute__ ((unused)) IC_NDB_MESSAGE *ndb_message,
             __attribute__ ((unused)) IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
execSCAN_TABLE_REF_v0(__attribute__ ((unused)) IC_NDB_MESSAGE *ndb_message,
             __attribute__ ((unused)) IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

#define IC_SCAN_TABLE_CONF 1
#define IC_SCAN_TABLE_REF 2
#define IC_PRIMARY_KEY_OP_CONF 3
#define IC_PRIMARY_KEY_OP_REF 4
#define IC_TRANSACTION_CONF 5
#define IC_TRANSACTION_REF 6
#define IC_ATTRIBUTE_INFO 7

static void
initialize_message_func_array()
{
  guint32 i;
  for (i= 0; i < 1024; i++)
  {
    ic_exec_message_func_array[0][i]->ic_exec_message_func= NULL;
    ic_exec_message_func_array[1][i]->ic_exec_message_func= NULL;
  }
  ic_exec_message_func_array[0][IC_SCAN_TABLE_CONF]->ic_exec_message_func=
    execSCAN_TABLE_CONF_v0;
  ic_exec_message_func_array[0][IC_SCAN_TABLE_REF]->ic_exec_message_func=
    execSCAN_TABLE_REF_v0;
  ic_exec_message_func_array[0][IC_TRANSACTION_CONF]->ic_exec_message_func=
    execTRANSACTION_CONF_v0;
  ic_exec_message_func_array[0][IC_TRANSACTION_REF]->ic_exec_message_func=
    execTRANSACTION_REF_v0;
  ic_exec_message_func_array[0][IC_PRIMARY_KEY_OP_CONF]->ic_exec_message_func=
    execPRIMARY_KEY_OP_CONF_v0;
  ic_exec_message_func_array[0][IC_PRIMARY_KEY_OP_REF]->ic_exec_message_func=
    execPRIMARY_KEY_OP_REF_v0;
  ic_exec_message_func_array[0][IC_ATTRIBUTE_INFO]->ic_exec_message_func=
    execATTRIBUTE_INFO_v0;
}

/*
  EXECUTE MESSAGE MODULE
  ----------------------
  This module contains the code executed in the user thread to execute
  messages posted to this thread by the receiver thread. The actual
  logic to handle the individual messages received is found in the
  MESSAGE LOGIC MODULE.
*/
static IC_EXEC_MESSAGE_FUNC*
get_exec_message_func(guint32 message_id,
                      __attribute__ ((unused)) guint32 version_num)
{
  return ic_exec_message_func_array[0][message_id];
}

static void
set_sock_buf_page_empty(IC_THREAD_CONNECTION *thd_conn)
{
  thd_conn->first_received_message= NULL;
  thd_conn->last_received_message= NULL;
}

static IC_SOCK_BUF_PAGE*
get_thread_messages(IC_APID_CONNECTION *apid, glong wait_time)
{
  GTimeVal stop_timer;
  IC_THREAD_CONNECTION *thd_conn= apid->thread_conn;
  IC_SOCK_BUF_PAGE *sock_buf_page;
  g_mutex_lock(thd_conn->mutex);
  sock_buf_page= thd_conn->first_received_message;
  if (sock_buf_page)
    set_sock_buf_page_empty(thd_conn);
  else if (wait_time)
  {
    g_get_current_time(&stop_timer);
    g_time_val_add(&stop_timer, wait_time);
    thd_conn->thread_wait_cond= TRUE;
    g_cond_timed_wait(thd_conn->cond, thd_conn->mutex, &stop_timer);
    sock_buf_page= thd_conn->first_received_message;
    if (sock_buf_page)
      set_sock_buf_page_empty(thd_conn);
  }
  g_mutex_unlock(thd_conn->mutex);
  return sock_buf_page;
}

static int
execute_message(IC_SOCK_BUF_PAGE *ndb_message_page)
{
  guint32 message_id;
  IC_NDB_MESSAGE ndb_message;
  IC_NDB_MESSAGE_OPAQUE_AREA *ndb_message_opaque;
  IC_EXEC_MESSAGE_FUNC *exec_message_func;
  IC_MESSAGE_ERROR_OBJECT error_object;
  IC_SOCK_BUF_PAGE *message_page;
  IC_SOCK_BUF *sock_buf_container;
  gint release_count, ref_count;
  volatile gint *ref_count_ptr;

  ndb_message_opaque= (IC_NDB_MESSAGE_OPAQUE_AREA*)
    &ndb_message_page->opaque_area[0];
  if (!create_ndb_message(ndb_message_page,
                          ndb_message_opaque,
                          &ndb_message,
                          &message_id))
  {
    exec_message_func= get_exec_message_func(message_id,
                                             ndb_message_opaque->version_num);
    if (!exec_message_func)
    {
       if (exec_message_func->ic_exec_message_func
             (&ndb_message, &error_object))
       {
         /* Handle error */
         return error_object.error;
       }
    }
    if (ndb_message_opaque->ref_count_releases > 0)
    {
      release_count= ndb_message_opaque->ref_count_releases;
      message_page= (IC_SOCK_BUF_PAGE*)ndb_message_page->sock_buf;
      ref_count_ptr= (volatile gint*)&message_page->ref_count;
      ref_count= g_atomic_int_exchange_and_add(ref_count_ptr, release_count);
      if (ref_count == release_count)
      {
        /*
          This was the last message we executed on this page so we're now
          ready to release the page where these messages were stored.
        */
        sock_buf_container= message_page->sock_buf_container;
        sock_buf_container->sock_buf_ops.ic_return_sock_buf_page(
          sock_buf_container, message_page);
      }
    }
    return 0;
  }
  return 1;
}

static int
poll_messages(IC_APID_CONNECTION *apid_conn, glong wait_time)
{
  IC_SOCK_BUF_PAGE *ndb_message_page;
  int error;

  /*
    We start by getting the messages waiting for us in the send queue, this
    is the first step in putting together the new scheduler for message
    handling in the iClaustron API.
    If no messages are waiting we can wait for a small amount of time
    before returning, if no messages arrive in this timeframe we'll
    return anyways. As soon as messages arrive we'll wake up and
    start executing them.
  */
  ndb_message_page= get_thread_messages(apid_conn, wait_time);
  /*
  */
  while (ndb_message_page)
  {
    /* Execute one message received */
    if ((error= execute_message(ndb_message_page)))
    {
      /* Error handling */
      return error;
    }
    /*
      Here we need to check if we should decrement the values from the
      buffer page.
    */
    ndb_message_page= ndb_message_page->next_sock_buf_page;
  }
  return 0;
}

/* Create a IC_NDB_MESSAGE for execution by the user thread.  */
static int
create_ndb_message(IC_SOCK_BUF_PAGE *message_page,
                   IC_NDB_MESSAGE_OPAQUE_AREA *ndb_message_opaque,
                   IC_NDB_MESSAGE *ndb_message,
                   guint32 *message_id)
{
  guint32 word1, word2, word3, receiver_node_id, sender_node_id;
  guint32 num_segments, fragmentation_bits, i, segment_size;
  guint32 *segment_size_ptr, message_size;
  guint32 *message_ptr= (guint32*)message_page->sock_buf;
  guint32 chksum, message_id_used, chksum_used, start_segment_word;

#ifdef HAVE_ENDIAN_SUPPORT
  word1= message_ptr[0];
  if ((word1 & 1) != glob_byte_order)
  {
    if ((word1 & 1) == 0)
    {
      /* Change byte order for entire message from low endian to big endian */
      word1= g_htonl(word1);
      for (i= 1; i < message_size; i++)
        message_ptr[i]= g_htonl(message_ptr[i]);
    }
    else
    {
      /* Change byte order for entire message from big endian to low endian */
      word1= g_ntohl(word1);
      for (i= 1; i < message_size; i++)
        message_ptr[i]= g_ntohl(message_ptr[i]);
    }
  }
#endif
  word1= message_ptr[0];
  word2= message_ptr[1];
  word3= message_ptr[2];

  /* Get message priority from Bit 5-6 in word 1 */
  ndb_message->message_priority= (word1 >> 5) & 3;
  /* Get fragmentation bits from bit 1 and bit 25 in word 1 */
  fragmentation_bits= (word1 >> 1) & 1;
  fragmentation_bits+= ((word1 >> 25) & 1) << 1;
  ndb_message->fragmentation_bits= fragmentation_bits;
  /* Get short data size from Bit 26-30 (Max 25 words) in word 1 */
  ndb_message->segment_size[0]= (word1 >> 26) & 0x1F;
  g_assert(ndb_message->segment_size[0] <= 25);

  /* Get message number from Bit 0-19 in word 2 */
  *message_id= word2 & 0x7FFFF;
  /* Get trace number from Bit 20-25 in word 2 */
  ndb_message->trace_num= (word2 >> 20) & 0x3F;

  receiver_node_id= ndb_message_opaque->receiver_node_id;
  sender_node_id= ndb_message_opaque->sender_node_id;
  ndb_message->receiver_node_id= receiver_node_id;
  ndb_message->sender_node_id= sender_node_id;
  /* Get senders module id from Bit 0-15 in word 3 */
  ndb_message->sender_module_id= word3 & 0xFFFF;
  /* Get receivers module id from Bit 16-31 in word 3 */
  ndb_message->receiver_module_id= word3 >> 16;

  ndb_message->message_id= 0;
  ndb_message->segment_ptr[1]= NULL;
  ndb_message->segment_ptr[2]= NULL;
  ndb_message->segment_ptr[3]= NULL;
  ndb_message->segment_ptr[0]= &message_ptr[3];

  /* Get message id flag from Bit 2 in word 1 */
  message_id_used= word1 & 4;
  /* Get checksum used flag from Bit 4 in word 1 */
  chksum_used= word1 & 0x10;
  /* Get number of segments from Bit 26-27 in word 2 */
  num_segments= (word2 >> 26) & 3;
  ndb_message->num_segments= num_segments + 1;
  
  start_segment_word= 3;
  if (message_id_used)
  {
    ndb_message->segment_ptr[0]= &message_ptr[4];
    ndb_message->message_id= message_ptr[3];
    start_segment_word= 4;
  }
  start_segment_word+= ndb_message->segment_size[0];
  segment_size_ptr= &message_ptr[start_segment_word];
  start_segment_word+= num_segments;
  for (i= 0; i < num_segments; i++)
  {
    segment_size= segment_size_ptr[i];
    ndb_message->segment_size[i+1]= segment_size;
    ndb_message->segment_ptr[i+1]= &message_ptr[start_segment_word];
    start_segment_word+= segment_size;
  }
  if (!chksum_used)
    return 0;
  /* We need to check the checksum first, before handling the message. */
  chksum= 0;
  message_size= (word2 >> 8) & 0xFFFF;
  for (i= 0; i < message_size; i++)
    chksum^= message_ptr[i];
  if (chksum)
    return IC_ERROR_MESSAGE_CHECKSUM;
  return 0;
}

/*
  RECEIVE THREAD MODULE:
  ----------------------
  This module contains the code that handles the receiver threads which takes
  care of reading from the socket and directing the NDB message to the proper
  user thread.
*/

/*
  We get a linked list of IC_SOCK_BUF_PAGE which each contain a reference
  to a list of IC_NDB_MESSAGE object. We will ensure that these messages are
  posted to the proper thread. They will be put in a linked list of NDB
  messages waiting to be executed by this application thread.
*/
static void
post_ndb_messages(LINK_MESSAGE_ANCHORS *ndb_message_anchors,
                 IC_THREAD_CONNECTION *thd_conn,
                 int min_hash_index,
                 int max_hash_index)
{
  int i;
  guint32 j, send_index, receiver_module_id, loop_count, temp, module_id;
  guint32 num_sent[IC_MAX_THREAD_CONNECTIONS/NUM_THREAD_LISTS];
  gboolean signal_flag= FALSE;
  IC_SOCK_BUF_PAGE *ndb_message_page, *next_ndb_message_page, *first_message;
  LINK_MESSAGE_ANCHORS *ndb_message_anchor;
  IC_NDB_MESSAGE_OPAQUE_AREA *message_opaque;
  IC_THREAD_CONNECTION *loc_thd_conn;

  memset(&num_sent[0],
         sizeof(guint32)*(IC_MAX_THREAD_CONNECTIONS/NUM_THREAD_LISTS), 0);
  for (i= min_hash_index; i < max_hash_index; i++)
  {
    ndb_message_anchor= &ndb_message_anchors[i];
    ndb_message_page= ndb_message_anchor->first_ndb_message_page;
    if (!ndb_message_page)
      continue;
    loop_count= 0;
    do
    {
      next_ndb_message_page= ndb_message_page->next_sock_buf_page;
      message_opaque= (IC_NDB_MESSAGE_OPAQUE_AREA*)
        &ndb_message_page->opaque_area[0];
      receiver_module_id= message_opaque->receiver_module_id;
      send_index= receiver_module_id / NUM_THREAD_LISTS;
      loc_thd_conn= &thd_conn[receiver_module_id];
      temp= num_sent[send_index];
      if (!temp)
        g_mutex_lock(loc_thd_conn->mutex);
      num_sent[send_index]= temp + 1;
      /* Link it into list on this thread connection object */
      first_message= loc_thd_conn->first_received_message;
      ndb_message_page->next_sock_buf_page= NULL;
      if (first_message)
      {
        /* Link signal into the queue last */
        loc_thd_conn->last_received_message->next_sock_buf_page=
          ndb_message_page;
        loc_thd_conn->last_received_message= ndb_message_page;
      }
      else
      {
        loc_thd_conn->first_received_message= ndb_message_page;
        loc_thd_conn->last_received_message= ndb_message_page;
      }
      if (!next_ndb_message_page)
        break;
      g_assert(++loop_count > MAX_LOOP_CHECK);
    } while (TRUE);
    for (j= 0; j < NUM_THREAD_LISTS; j++)
    {
      module_id= (j * NUM_THREAD_LISTS) + i;
      if (num_sent[j])
      {
        if (thd_conn[j].thread_wait_cond)
        {
          thd_conn[j].thread_wait_cond= FALSE;
          signal_flag= TRUE;
        }
        message_opaque= (IC_NDB_MESSAGE_OPAQUE_AREA*)
          &thd_conn[module_id].last_received_message->opaque_area[0];
        message_opaque->ref_count_releases= num_sent[j];
        g_mutex_unlock(thd_conn[j].mutex);
        if (signal_flag)
        {
          /* Thread is waiting for our wake-up signal, wake it up */
          g_cond_signal(thd_conn[j].cond);
          signal_flag= FALSE;
        }
        num_sent[j]= 0;
      }
    }
  }
  return;
}

static int
init_ndb_receiver(IC_RECEIVE_NODE_CONNECTION *rec_node,
                  IC_NDB_RECEIVE_STATE *rec_state,
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
    if (!(buf_page= rec_buf_pool->sock_buf_ops.ic_get_sock_buf_page(
            rec_buf_pool,
            &rec_state->free_rec_pages,
            NUM_RECEIVE_PAGES_ALLOC)))
      return 1;
    memcpy(buf_page->sock_buf, start_buf, start_size);
    rec_node->buf_page= buf_page;
  }
  return 0;
}

static void
read_message_early(gchar *read_ptr, guint32 *message_size,
                   guint32 *receiver_module_id)
{
  guint32 *message_ptr= (guint32*)read_ptr, word1, word3;

  word1= message_ptr[0];
  word3= message_ptr[2];
#ifdef HAVE_ENDIAN_SUPPORT
  if ((word1 & 1) != glob_byte_order)
  {
    if ((word1 & 1) == 0)
    {
      word1= g_htonl(word1);
      word3= g_htonl(word3);
    }
    else
    {
      word1= g_ntohl(word1);
      word3= g_ntohl(word3);
    }
  }
#endif
  *receiver_module_id= word3 >> 16;
  *message_size= (word1 >> 8) & 0xFFFF;
}

static int
handle_node_error(int ret_code,
                  int min_hash_index,
                  int max_hash_index,
                  LINK_MESSAGE_ANCHORS *message_anchors,
                  IC_THREAD_CONNECTION *thd_conn)
{
  if (ret_code == END_OF_FILE)
  {
    /*
      A normal close down of the socket connection. We will stop the
      node using the socket connection but will continue with all other
      socket connections in a normal fashion
    */
    ret_code= 0;
  }
  else if (ret_code == IC_ERROR_MEM_ALLOC)
  {
    /*
      Will be very hard to continue in a low-memory situation. In the
      future we might add some code to get rid of memory not badly
      needed but for now we simply stop this node entirely in a
      controlled manner.
    */
    return 1;
  }
  else
  {
    /*
      Any other fault happening will be treated as a close down of the
      socket connection and allow us to continue operating in a normal
      manner.
    */
    ret_code= 0;
  }
  post_ndb_messages(&message_anchors[0],
                    thd_conn,
                    min_hash_index,
                    max_hash_index);
  return ret_code;
}

static IC_RECEIVE_NODE_CONNECTION*
get_first_rec_node(__attribute__ ((unused)) IC_NDB_RECEIVE_STATE *rec_state)
{
  return NULL;
}

static IC_RECEIVE_NODE_CONNECTION*
get_next_rec_node(__attribute__ ((unused)) IC_NDB_RECEIVE_STATE *rec_state)
{
  return NULL;
}

static gpointer
run_receive_thread(void *data)
{
  IC_NDB_RECEIVE_STATE *rec_state= (IC_NDB_RECEIVE_STATE*)data;
  LINK_MESSAGE_ANCHORS message_anchors[NUM_THREAD_LISTS];
  int min_hash_index= NUM_THREAD_LISTS;
  int max_hash_index= (int)-1;
  int ret_code;
  IC_SOCK_BUF_PAGE *buf_page, *ndb_message;
  IC_THREAD_CONNECTION *thd_conn;
  IC_RECEIVE_NODE_CONNECTION *rec_node;

  memset(message_anchors,
         sizeof(LINK_MESSAGE_ANCHORS)*NUM_THREAD_LISTS, 0);


  do
  {
    /* Check for which nodes to receive from */
    /* Loop over each node to receive from */
    rec_node= get_first_rec_node(rec_state);
    while (rec_node)
    {
      if ((ret_code= ndb_receive_node(rec_state,
                                      rec_node,
                                      &min_hash_index,
                                      &max_hash_index,
                                      &message_anchors[0],
                                      thd_conn)))
      {
        if (handle_node_error(ret_code,
                          min_hash_index,
                          max_hash_index,
                          &message_anchors[0],
                          thd_conn))
          goto end;
        min_hash_index= NUM_THREAD_LISTS;
        max_hash_index= (int)-1;
      }
      /*
        Here is the place where we need to employ some clever scheuling
        principles. The question is simply put how often to post ndb
        messages to the application threads. At first we simply post
        ndb messages for each node we receive from.
      */
      post_ndb_messages(&message_anchors[0],
                        thd_conn,
                        min_hash_index,
                        max_hash_index);
      min_hash_index= NUM_THREAD_LISTS;
      max_hash_index= (int)-1;
      get_next_rec_node(rec_state);
    }
    g_mutex_lock(rec_state->mutex);
    /*
      We need to check for:
      1) Someone ordering this node to stop, as part of this the
      stop_ordered flag on the receive thread needs to be set.
      2) Someone ordering a change of socket connections. This
      means that a socket connection is moved from one receive
      thread to another. This is ordered by setting the change_flag
      on the receive thread data structure.
      3) We also update all the statistics in this slot such that
      the thread deciding on reorganization of socket connections
      has statistics to use here.
    */
    g_mutex_unlock(rec_state->mutex);
  } while (1);

end:
  while (rec_state->free_rec_pages)
  {
    buf_page= rec_state->free_rec_pages;
    rec_state->free_rec_pages= rec_state->free_rec_pages->next_sock_buf_page;
    rec_state->rec_buf_pool->sock_buf_ops.ic_return_sock_buf_page(
      rec_state->rec_buf_pool,
      buf_page);
  }
  while (rec_state->free_ndb_messages)
  {
    ndb_message= rec_state->free_ndb_messages;
    rec_state->free_ndb_messages=
      rec_state->free_ndb_messages->next_sock_buf_page;
    rec_state->message_pool->sock_buf_ops.ic_return_sock_buf_page(
      rec_state->message_pool,
      ndb_message);
  }
  return NULL;
}
#define MAX_NDB_RECEIVE_LOOPS 16
int
ndb_receive_node(IC_NDB_RECEIVE_STATE *rec_state,
                 IC_RECEIVE_NODE_CONNECTION *rec_node,
                 int *min_hash_index,
                 int *max_hash_index,
                 LINK_MESSAGE_ANCHORS *message_anchors,
                 IC_THREAD_CONNECTION *thd_conn)
{
  IC_CONNECTION *conn= rec_node->conn;
  IC_SOCK_BUF *rec_buf_pool= rec_state->rec_buf_pool;
  IC_SOCK_BUF *message_pool= rec_state->message_pool;
  guint32 read_size= rec_node->read_size;
  guint32 real_read_size;
  gboolean read_header_flag= rec_node->read_header_flag;
  gchar *read_ptr;
  guint32 page_size= rec_buf_pool->page_size;
  guint32 start_size, message_size, receiver_module_id;
  int hash_index;
  guint32 loop_count= 0;
  int ret_code;
  gint ref_count;
  int loc_min_hash_index= *min_hash_index;
  int loc_max_hash_index= *max_hash_index;
  gboolean any_message_received= FALSE;
  gboolean read_more;
  IC_SOCK_BUF_PAGE *buf_page, *new_buf_page;
  IC_SOCK_BUF_PAGE *ndb_message_page;
  IC_SOCK_BUF_PAGE *anchor_first_page;
  IC_NDB_MESSAGE *ndb_message;
  IC_NDB_MESSAGE_OPAQUE_AREA *ndb_message_opaque;
  LINK_MESSAGE_ANCHORS *anchor;

  g_assert(message_pool->page_size == 0);

  /*
    We get a receive buffer from the global pool of free buffers.
    This buffer will be returned to the free pool by the last
    thread that executes a message in this pool.
  */
  while (1)
  {
    if (read_size == 0)
    {
      if (!(buf_page= rec_buf_pool->sock_buf_ops.ic_get_sock_buf_page(
              rec_buf_pool,
              &rec_state->free_rec_pages,
              NUM_RECEIVE_PAGES_ALLOC)))
        goto mem_pool_error;
    }
    else
      buf_page= rec_node->buf_page;
    read_ptr= buf_page->sock_buf;
    start_size= read_size;
    ret_code= conn->conn_op.ic_read_connection(conn,
                                               read_ptr + read_size,
                                               page_size - read_size,
                                               &real_read_size);
    if (!ret_code)
    {
      /*
        We received data in the NDB Protocol, now chunk it up in its
        respective NDB messages and send those NDB messages to the
        thread that expects them. The actual execution of the NDB
        messages happens in the thread that the NDB message is destined
        for.

        We check to see if we read an entire page in which case we
        might have more data to read.
      */
      read_size= start_size + real_read_size;
      read_more= (read_size == page_size);
      /*
        Check that we have at least received the header of the next
        NDB message first, this is at least 12 bytes in size in the
        NDB Protocol.
      */
      while (read_size >= MIN_NDB_HEADER_SIZE)
      {
        if (!read_header_flag)
        {
          read_message_early(read_ptr, &message_size,
                             &receiver_module_id);
          read_header_flag= TRUE;
        }
        if (message_size > read_size)
          break;
        if (!(ndb_message_page=
              message_pool->sock_buf_ops.ic_get_sock_buf_page(
                message_pool,
                &rec_state->free_ndb_messages,
                NUM_NDB_SIGNAL_ALLOC)))
          goto mem_pool_error;
        ndb_message_opaque= (IC_NDB_MESSAGE_OPAQUE_AREA*)
          &ndb_message_page->opaque_area[0];
        ndb_message_page->sock_buf= (gchar*)buf_page;
        ndb_message_opaque->message_offset= read_ptr - buf_page->sock_buf;
        ndb_message_opaque->sender_node_id= rec_node->other_node_id;
        ndb_message_opaque->receiver_node_id= rec_node->my_node_id;
        ndb_message_opaque->ref_count_releases= 0;
        ndb_message_opaque->receiver_module_id= receiver_module_id;
        ndb_message_opaque->version_num= 0; /* Defined value although unused */
        ref_count= buf_page->ref_count;
        hash_index= ndb_message->receiver_module_id &
                    (NUM_THREAD_LISTS - 1);
        loc_min_hash_index= IC_MIN(loc_min_hash_index, hash_index);
        loc_max_hash_index= IC_MAX(loc_max_hash_index, hash_index);
        anchor= &message_anchors[hash_index];
        anchor_first_page= anchor->first_ndb_message_page;
        buf_page->ref_count= ref_count + 1;
        if (!anchor_first_page)
        {
          anchor->first_ndb_message_page= ndb_message_page;
          anchor->last_ndb_message_page= ndb_message_page;
        }
        else
        {
          anchor->first_ndb_message_page->next_sock_buf_page= ndb_message_page;
          anchor->last_ndb_message_page= ndb_message_page;
          ndb_message_page->next_sock_buf_page= NULL;
        }
        any_message_received= TRUE;
        read_header_flag= FALSE;
        read_size-= message_size;
        read_ptr+= message_size;
      }
      if (read_size > 0)
      {
        /* We received an incomplete NDB Signal */
        if (any_message_received)
        {
          /*
            At least one message was received and we need to post
            these NDB Signals and thus we need to transfer the
            incomplete message before posting the messages. When
            the messages have been posted another thread can get
            access to the socket buffer page and even release
            it before we have transferred the incomplete message
            if we post before we transfer the incomplete message.
          */
          if (!(new_buf_page= rec_buf_pool->sock_buf_ops.ic_get_sock_buf_page(
                  rec_buf_pool,
                  &rec_state->free_rec_pages,
                  NUM_RECEIVE_PAGES_ALLOC)))
            goto mem_pool_error;

          memcpy(new_buf_page->sock_buf,
                 buf_page->sock_buf,
                 read_size);
          rec_node->buf_page= new_buf_page;
          rec_node->read_size= read_size;
          rec_node->read_header_flag= read_header_flag;
        }
      }
      else
      {
        rec_node->buf_page= NULL;
        rec_node->read_size= 0;
        rec_node->read_header_flag= FALSE;
      }
      if (!read_more || (loop_count++ >= MAX_NDB_RECEIVE_LOOPS))
        break;
    }
  }
  if (ret_code == END_OF_FILE)
  {
    if (any_message_received)
    {
      post_ndb_messages(&message_anchors[0],
                        thd_conn,
                        loc_min_hash_index,
                        loc_max_hash_index);
      loc_min_hash_index= NUM_THREAD_LISTS;
      loc_max_hash_index= (int)-1;
    }
    ret_code= END_OF_FILE;
  }
end:
  *min_hash_index= loc_min_hash_index;
  *max_hash_index= loc_max_hash_index;
  return ret_code;
mem_pool_error:
  ret_code= IC_ERROR_MEM_ALLOC;
  goto end;
}
