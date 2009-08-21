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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_parse_connectstring.h>
#include <ic_bitmap.h>
#include <ic_dyn_array.h>
#include <ic_connection.h>
#include <ic_sock_buf.h>
#include <ic_poll_set.h>
#include <ic_threadpool.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_apid_int.h"


IC_STRING ic_glob_config_dir= { NULL, 0, TRUE};
IC_STRING ic_glob_data_dir= { NULL, 0, TRUE};
IC_STRING ic_glob_base_dir= { NULL, 0, TRUE};
IC_STRING ic_glob_binary_dir= { NULL, 0, TRUE};
gchar *ic_glob_cs_server_name= "127.0.0.1";
gchar *ic_glob_cs_server_port= IC_DEF_CLUSTER_SERVER_PORT_STR;
gchar *ic_glob_cs_connectstring= NULL;
gchar *ic_glob_data_path= NULL;
gchar *ic_glob_version_path= IC_VERSION_STR;
gchar *ic_glob_base_path= NULL;
guint32 ic_glob_node_id= 1;
guint32 ic_glob_num_threads= 1;
guint32 ic_glob_use_iclaustron_cluster_server= 1;
guint32 ic_glob_daemonize= 1;
guint32 ic_glob_byte_order= 0;

/*
  These static functions implements the local IC_TRANSLATION_OBJ.
  For speed they're only declared as local objects such that they
  can be inlined by the compiler if so decided.
  TODO: Not implemented yet.
*/

IC_TRANSLATION_OBJ *create_translation_object();
int insert_object_in_translation(IC_TRANSLATION_OBJ *transl_obj,
                                 void *object);
void remove_object_from_translation(void *object);
/* End definition of IC_TRANSLATION_OBJ functions */

/* Some declarations needed to reach functions inside Modules */
/* Initialise function pointers in Message Logic Modules */
static void initialize_message_func_array();
/* Start one receive thread */
static int start_receive_thread(IC_INT_APID_GLOBAL *apid_global);
/* Start send threads */
static int start_send_threads(IC_INT_APID_GLOBAL *apid_global);
/* Close listen server threads at close down of Data API */
static void close_listen_server_thread(
          IC_LISTEN_SERVER_THREAD *listen_server_thread,
          IC_INT_APID_GLOBAL *apid_global);

/* Function for Data API functions to send messages */
/* Not yet used
static int
send_messages(IC_INT_APID_GLOBAL *apid_global,
              guint32 cluster_id,
              guint32 node_id,
              IC_SOCK_BUF_PAGE *first_page_to_send,
              gboolean force_send);
*/

/* Function for Data API functions to receive messages */
static int poll_messages(IC_APID_CONNECTION *apid_conn, glong wait_time);
/*
  External function used by Cluster Server to transfer a connection to the
  Data API, part of Global Data API functions.
*/
static int external_connect(IC_APID_GLOBAL *apid_global,
                            guint32 cluster_id,
                            guint32 node_id,
                            IC_CONNECTION *conn);
/* Internal function to start heartbeat thread */
static int start_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                                  IC_THREADPOOL_STATE *tp_state);
/* Internal function to handle node connection failure */
static int node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn);

/* Internal function to send a NDB message */
static int ndb_send(IC_SEND_NODE_CONNECTION *send_node_conn,
                    IC_SOCK_BUF_PAGE *first_page_to_send,
                    gboolean force_send);

/* Internal function to get NDB message header size */
static guint32 get_ndb_message_header_size(
                    IC_SEND_NODE_CONNECTION *send_node_conn);

/* Internal function to prepare NDB message for sending */
static guint32 fill_ndb_message_header(IC_SEND_NODE_CONNECTION *send_node_conn,
                                       guint32 message_number,
                                       guint32 message_priority,
                                       guint32 sender_module_id,
                                       guint32 receiver_mdoule_id,
                                       guint32 *start_message,
                                       guint32 main_message_size,
                                       guint32 *main_message,
                                       guint32 num_segments,
                                       guint32 *segment_ptrs[3],
                                       guint32 segment_lens[3]);

/*
  NDB Protocol support MODULE
  ---------------------------
  This module contains a number of common functions used by most modules
*/
/*
  The NDB Protocol often makes use of module references. This is a 32-bit
  entity that uniquely identifies a module anywhere in the cluster. It has
  3 entities, a node id, a module id and an instance id which essentially
  is a thread id. For iClaustron nodes we effectively use both module id
  and thread id to indicate the thread number and thus we can handle up
  to 65536 threads.
*/
static guint32
ic_get_ic_reference(guint32 node_id, guint32 thread_id)
{
  return (thread_id << 16) + node_id;
}
/*
static guint32
ic_get_ndb_reference(guint32 node_id, guint32 module_id, guint32 thread_id)
{
  return (thread_id << 25) + (module_id << 16) + node_id;
}
*/
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
static void apid_free(IC_APID_CONNECTION *apid_conn);
static void ic_end_apid(IC_INT_APID_GLOBAL *apid_global);
static void start_connect_phase(IC_INT_APID_GLOBAL *apid_global,
                                gboolean stop_ordered,
                                gboolean signal_flag);

static IC_INT_APID_GLOBAL*
ic_init_apid(IC_API_CONFIG_SERVER *apic)
{
  IC_INT_APID_GLOBAL *apid_global;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_CLUSTER_COMM *cluster_comm;
  guint32 i, j;

  initialize_message_func_array();
  if (!(apid_global= (IC_INT_APID_GLOBAL*)
       ic_calloc(sizeof(IC_INT_APID_GLOBAL))))
    return NULL;
  if (!(apid_global->rec_thread_pool= ic_create_threadpool(
                        IC_MAX_RECEIVE_THREADS + 1, FALSE)))
    goto error;
  if (!(apid_global->send_thread_pool= ic_create_threadpool(
                        IC_DEFAULT_MAX_THREADPOOL_SIZE, TRUE)))
    goto error;
  apid_global->apic= apic;
  apid_global->num_receive_threads= 0;
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
  if (!(apid_global->send_buf_pool= ic_create_sock_buf(IC_MEMBUF_SIZE,
                                                       1024)))
    goto error;
  if (!(apid_global->ndb_message_pool= ic_create_sock_buf(0, 16384)))
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
  This method is called when shutting down a global Data API object. It
  requires all the threads to have been stopped and is only releasing the
  memory on the global Data API object.
*/
static void
ic_end_apid(IC_INT_APID_GLOBAL *apid_global)
{
  IC_SOCK_BUF *send_buf_pool= apid_global->send_buf_pool;
  IC_SOCK_BUF *ndb_message_pool= apid_global->ndb_message_pool;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  guint32 i, j;

  if (!apid_global)
    return;
  if (apid_global->rec_thread_pool)
    apid_global->rec_thread_pool->tp_ops.ic_threadpool_stop(
                  apid_global->rec_thread_pool);
  if (apid_global->send_thread_pool)
    apid_global->send_thread_pool->tp_ops.ic_threadpool_stop(
                  apid_global->send_thread_pool);
  if (apid_global->cluster_bitmap)
    ic_free_bitmap(apid_global->cluster_bitmap);
  if (send_buf_pool)
    send_buf_pool->sock_buf_ops.ic_free_sock_buf(send_buf_pool);
  if (ndb_message_pool)
    ndb_message_pool->sock_buf_ops.ic_free_sock_buf(ndb_message_pool);
  if (apid_global->heartbeat_mutex)
    g_mutex_free(apid_global->heartbeat_mutex);
  if (apid_global->heartbeat_cond)
    g_cond_free(apid_global->heartbeat_cond);
  if (apid_global->heartbeat_conn)
    apid_free(apid_global->heartbeat_conn);
  if (apid_global->mutex)
    g_mutex_free(apid_global->mutex);
  if (apid_global->cond)
    g_cond_free(apid_global->cond);
  for (i= 0; i < apid_global->num_listen_server_threads; i++)
    close_listen_server_thread(apid_global->listen_server_thread[i],
                               apid_global);
  if (grid_comm)
  {
    if (grid_comm->cluster_comm_array)
    {
      for (i= 0; i <= IC_MAX_CLUSTER_ID; i++)
      {
        cluster_comm= grid_comm->cluster_comm_array[i];
        if (cluster_comm == NULL)
          continue;
        /* Listen threads are released as part of stopping send threads */
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

/*
  This is the method used to start up all the threads in the Data API,
  including the send threads, the receive threads and the listen server
  threads. It will also start connecting to the various clusters that
  the Data API should be connected to.
*/
static int
ic_apid_global_connect(IC_INT_APID_GLOBAL *apid_global)
{
  int error= 0;
  DEBUG_ENTRY("ic_apid_global_connect");

  if ((error= start_send_threads(apid_global)))
    goto error;
  if ((error= start_receive_thread(apid_global)))
    goto error;
  /*
    We are now ready to signal to all send threads that they can start up the
    connect process to the other nodes in the cluster.
  */
  start_connect_phase(apid_global, FALSE, TRUE);
  DEBUG_RETURN(0);
error:
  apid_global->stop_flag= TRUE;
  start_connect_phase(apid_global, TRUE, TRUE);
  DEBUG_RETURN(error);
}

static void
start_connect_phase(IC_INT_APID_GLOBAL *apid_global,
                    gboolean stop_ordered,
                    gboolean signal_flag)
{
  guint32 node_id, cluster_id, i, thread_id;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_API_CONFIG_SERVER *apic= apid_global->apic;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  IC_THREADPOOL_STATE *send_tp_state= apid_global->send_thread_pool;
  IC_THREADPOOL_STATE *rec_tp_state= apid_global->rec_thread_pool;
  IC_NDB_RECEIVE_STATE *rec_state;
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
          if (!send_node_conn->send_thread_ended)
          {
            if (stop_ordered)
              send_node_conn->stop_ordered= TRUE;
            thread_id= send_node_conn->thread_id;
            g_mutex_unlock(send_node_conn->mutex);
            /* Wake up send thread to connect/shutdown */
            if (stop_ordered)
            {
              /*
                In this we need to wait for send thread to stop before
                we continue.
              */
              send_tp_state->tp_ops.ic_threadpool_stop_thread(send_tp_state,
                                                              thread_id);
            }
            else if (signal_flag)
            {
              send_tp_state->tp_ops.ic_threadpool_run_thread(send_tp_state,
                                                             thread_id);
            }
            continue;
          }
          g_mutex_unlock(send_node_conn->mutex);
        }
      }
    }
  }
  if (signal_flag)
  {
    /* Wake all receive threads in start position */
    g_mutex_lock(apid_global->mutex);
    for (i= 0; i < apid_global->num_receive_threads; i++)
    {
      rec_state= apid_global->receive_threads[i];
      rec_tp_state->tp_ops.ic_threadpool_run_thread(rec_tp_state,
                                                    rec_state->thread_id);
    }
    g_mutex_unlock(apid_global->mutex);
  }
  if (stop_ordered)
  {
    /* Wait for all receive threads to have exited */
    while (1)
    {
      g_mutex_lock(apid_global->mutex);
      if (apid_global->num_receive_threads == 0)
      {
        g_mutex_unlock(apid_global->mutex);
        break;
      }
      thread_id= apid_global->num_receive_threads - 1;
      rec_state= apid_global->receive_threads[thread_id];
      apid_global->num_receive_threads--;
      g_mutex_unlock(apid_global->mutex);
      rec_tp_state->tp_ops.ic_threadpool_stop_thread_wait(rec_tp_state,
                                              rec_state->thread_id);
    }
  }
  DEBUG_RETURN_EMPTY;
}

IC_RUN_APID_THREAD_FUNC
get_thread_func(IC_APID_GLOBAL *ext_apid_global)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  return apid_global->apid_func;
}

void
set_thread_func(IC_APID_GLOBAL *ext_apid_global,
                IC_RUN_APID_THREAD_FUNC apid_func)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  apid_global->apid_func= apid_func;
}

void
apid_global_cond_wait(IC_APID_GLOBAL *ext_apid_global)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  g_mutex_lock(apid_global->mutex);
  if (apid_global->num_user_threads_started > 0)
    g_cond_wait(apid_global->cond, apid_global->mutex);
  g_mutex_unlock(apid_global->mutex);
}

void
apid_global_set_stop(IC_APID_GLOBAL *ext_apid_global)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  g_mutex_lock(apid_global->mutex);
  apid_global->stop_flag= TRUE;
  g_mutex_unlock(apid_global->mutex);
}

gboolean
apid_global_get_stop(IC_APID_GLOBAL *ext_apid_global)
{
  gboolean stop_flag;
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  g_mutex_lock(apid_global->mutex);
  if (ic_get_stop_flag())
    apid_global->stop_flag= TRUE;
  stop_flag= apid_global->stop_flag;
  g_mutex_unlock(apid_global->mutex);
  return stop_flag;
}

guint32
apid_global_get_num_user_threads(IC_APID_GLOBAL *ext_apid_global)
{
  guint32 num_user_threads;
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  g_mutex_lock(apid_global->mutex);
  num_user_threads= apid_global->num_user_threads_started;
  g_mutex_unlock(apid_global->mutex);
  return num_user_threads;
}

void
apid_global_add_user_thread(IC_APID_GLOBAL *ext_apid_global)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  g_mutex_lock(apid_global->mutex);
  apid_global->num_user_threads_started++;
  g_mutex_unlock(apid_global->mutex);
}

void
apid_global_remove_user_thread(IC_APID_GLOBAL *ext_apid_global)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  g_mutex_lock(apid_global->mutex);
  apid_global->num_user_threads_started--;
  g_cond_signal(apid_global->cond);
  g_mutex_unlock(apid_global->mutex);
}

/*
  External interface to GLOBAL DATA API INITIALISATION MODULE
  -----------------------------------------------------------
*/
static void free_apid_global(IC_APID_GLOBAL *apid_global);
IC_APID_GLOBAL*
ic_create_apid_global(IC_API_CONFIG_SERVER *apic,
                      gboolean use_external_connect,
                      int *ret_code,
                      gchar **err_str)
{
  IC_INT_APID_GLOBAL *apid_global;
  gchar *err_string;
  int error;

  if (!(apid_global= ic_init_apid(apic)))
  {
    *ret_code= IC_ERROR_MEM_ALLOC;
    goto error;
  }
  apid_global->use_external_connect= use_external_connect;
  if ((error= ic_apid_global_connect(apid_global)))
  {
    ic_end_apid(apid_global);
    *ret_code= error;
    goto error;
  }
  apid_global->apid_global_ops.ic_cond_wait= apid_global_cond_wait;
  apid_global->apid_global_ops.ic_set_stop_flag= apid_global_set_stop;
  apid_global->apid_global_ops.ic_get_stop_flag= apid_global_get_stop;
  apid_global->apid_global_ops.ic_get_num_user_threads=
    apid_global_get_num_user_threads;
  apid_global->apid_global_ops.ic_add_user_thread=
    apid_global_add_user_thread;
  apid_global->apid_global_ops.ic_remove_user_thread=
    apid_global_remove_user_thread;
  apid_global->apid_global_ops.ic_set_thread_func= set_thread_func;
  apid_global->apid_global_ops.ic_get_thread_func= get_thread_func;
  apid_global->apid_global_ops.ic_external_connect= external_connect;
  apid_global->apid_global_ops.ic_free_apid_global= free_apid_global;
  if ((error= start_heartbeat_thread(apid_global,
                                     apid_global->rec_thread_pool)))
  {
    ic_end_apid(apid_global);
    *ret_code= error;
    goto error;
  }
  return (IC_APID_GLOBAL*)apid_global;
error:
  err_string= ic_common_fill_error_buffer(NULL, 0, *ret_code, *err_str);
  g_assert(err_string == *err_str);
  return NULL;
}

/*
  This method is used to tear down the global Data API part. It will
  disconnect all the connections to all cluster nodes and stop all
  Data API threads.
*/
static void
free_apid_global(IC_APID_GLOBAL *ext_apid_global)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;

  apid_global->stop_flag= TRUE;
  start_connect_phase(apid_global, TRUE, FALSE);
  ic_end_apid(apid_global);
  return;
}

/*
  DATA API Connection Module
  --------------------------
  This module contains all the methods that are part of the Data API
  Connection interface. This interface contains methods to start
  transactions and control execution of transactions. Each thread
  should have its own Data API Connection module.
*/
IC_APID_GLOBAL*
get_apid_global(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  return (IC_APID_GLOBAL*)apid_conn->apid_global;
}

IC_API_CONFIG_SERVER*
get_api_config_server(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  return apid_conn->apic;
}

/*
  This method is only called after a successful call to flush or poll
*/
IC_APID_OPERATION*
get_next_executed_operation(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  IC_APID_OPERATION *ret_op= apid_conn->first_executed_operation;
  IC_APID_OPERATION *first_completed_op= apid_conn->first_completed_operation;

  if (ret_op == NULL)
    return NULL;
  g_assert(ret_op->list_type == IN_EXECUTED_LIST);
  apid_conn->first_executed_operation= ret_op->next_conn_op;
  apid_conn->first_completed_operation= ret_op;
  ret_op->next_conn_op= first_completed_op;
  ret_op->list_type= IN_COMPLETED_LIST;
  return ret_op;
}

static void
apid_free(IC_APID_CONNECTION *ext_apid_conn)
{
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  IC_THREAD_CONNECTION *thread_conn= apid_conn->thread_conn;
  IC_GRID_COMM *grid_comm;
  IC_INT_APID_GLOBAL *apid_global;
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
ic_create_apid_connection(IC_APID_GLOBAL *ext_apid_global,
                          IC_BITMAP *cluster_id_bitmap)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  guint32 thread_id= IC_MAX_THREAD_CONNECTIONS;
  IC_GRID_COMM *grid_comm;
  IC_THREAD_CONNECTION *thread_conn;
  IC_INT_APID_CONNECTION *apid_conn;
  guint32 i, num_bits;
  IC_API_CONFIG_SERVER *apic= apid_global->apic;
  DEBUG_ENTRY("ic_create_apid_connection");

  /* Initialise the APID connection object */
  if (!(apid_conn= (IC_INT_APID_CONNECTION*)
                    ic_calloc(sizeof(IC_INT_APID_CONNECTION))))
    goto end;
  apid_conn->apid_global= apid_global;
  apid_conn->thread_id= thread_id;
  apid_conn->apid_global= apid_global;
  apid_conn->apic= apic;
  num_bits= apic->api_op.ic_get_max_cluster_id(apic) + 1;
  if (!(apid_conn->cluster_id_bitmap= ic_create_bitmap(NULL, num_bits)))
    goto error;
  if (cluster_id_bitmap)
  {
    ic_bitmap_copy(apid_conn->cluster_id_bitmap, cluster_id_bitmap);
  }
  else
  {
    for (i= 0; i < num_bits; i++)
    {
    if (apic->api_op.ic_get_cluster_config(apic, i))
      ic_bitmap_set_bit(apid_conn->cluster_id_bitmap, i);
    }
  }

  /* Initialise Thread Connection object */
  if (!(thread_conn= apid_conn->thread_conn= (IC_THREAD_CONNECTION*)
                      ic_calloc(sizeof(IC_THREAD_CONNECTION))))
    goto error;
  thread_conn->apid_conn= apid_conn;
  if (!(thread_conn->mutex= g_mutex_new()))
    goto error;
  if (!(thread_conn->cond= g_cond_new()))
    goto error;


  grid_comm= apid_global->grid_comm;
  g_mutex_lock(apid_global->thread_id_mutex);
  for (i= 0; i < IC_MAX_THREAD_CONNECTIONS; i++)
  {
    if (!grid_comm->thread_conn_array[i])
    {
      thread_id= i;
      break;
    }
  }
  if (thread_id == IC_MAX_THREAD_CONNECTIONS)
  {
    g_mutex_unlock(apid_global->thread_id_mutex);
    ic_free(apid_conn->thread_conn);
    apid_conn->thread_conn= NULL;
    goto error;
  }
  grid_comm->thread_conn_array[thread_id]= thread_conn;
  g_mutex_unlock(apid_global->thread_id_mutex);
  apid_conn->thread_id= thread_id;
  /* Now initialise the method pointers for the Data API interface */
  apid_conn->apid_conn_ops.ic_free_apid_connection= apid_free;
  apid_conn->apid_conn_ops.ic_get_apid_global= get_apid_global;
  apid_conn->apid_conn_ops.ic_get_api_config_server= get_api_config_server;
  apid_conn->apid_conn_ops.ic_get_next_executed_operation= 
    get_next_executed_operation;
  apid_conn->apid_conn_ops.ic_poll= poll_messages;
  /* TODO many more methods */
  return (IC_APID_CONNECTION*)apid_conn;

error:
  apid_free((IC_APID_CONNECTION*)apid_conn);
end:
  return NULL;
}

/*
  Heartbeat MODULE
  ----------------

  All users of the Data API requires sending of heartbeats every now and
  then. To simplify this the Data API has methods to start a heartbeat
  thread and there is also a method to add nodes to be handled by this
  thread (start_heartbeat_thread and add_node_to_heartbeat_thread).

  Then we also have the run heartbeat thread method that implements the
  heartbeat thread. This thread has one support function that packs the
  API_HEARTBEATREQ message and one support function that does the
  actual send of the heartbeat message (prepare_send_heartbeat and
  real_send_heartbeat).

  Finally there is a callback method that is called when the
  API_HEARTBEATCONF message is received. This method belongs to the
  heartbeat module but is alsoa vital part of the Message Logic Modules.
*/
static int start_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                                  IC_THREADPOOL_STATE *tp_state);
static void add_node_to_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                              IC_SEND_NODE_CONNECTION *send_node_conn);
static void prepare_send_heartbeat(IC_SEND_NODE_CONNECTION *send_node_conn,
                                   IC_SOCK_BUF_PAGE *heartbeat_page,
                                   guint32 thread_id);
static int real_send_heartbeat(IC_SEND_NODE_CONNECTION *send_node_conn,
                               IC_SOCK_BUF_PAGE *heartbeat_page);
static gpointer run_heartbeat_thread(gpointer data);

static int
start_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                       IC_THREADPOOL_STATE *tp_state)
{
  IC_APID_CONNECTION *heartbeat_conn;
  int error;

  ic_require(!apid_global->heartbeat_conn);
  if (!(heartbeat_conn= ic_create_apid_connection(
                (IC_APID_GLOBAL*)apid_global, NULL)))
    goto mem_error;
  if ((!(apid_global->heartbeat_mutex= g_mutex_new())) ||
      (!(apid_global->heartbeat_cond= g_cond_new())))
    goto mem_error;
  /* Start heartbeat thread */
  if ((error= tp_state->tp_ops.ic_threadpool_get_thread_id_wait(
                        tp_state,
                        &apid_global->heartbeat_thread_id,
                        IC_MAX_THREAD_WAIT_TIME)) ||
      ((error= tp_state->tp_ops.ic_threadpool_start_thread_with_thread_id(
                        tp_state,
                        apid_global->heartbeat_thread_id,
                        run_heartbeat_thread,
                        (void*)apid_global,
                        IC_SMALL_STACK_SIZE,
                        FALSE))))
    goto error;
  apid_global->heartbeat_conn= heartbeat_conn;
  return 0;
mem_error:
  error= IC_ERROR_MEM_ALLOC;
error:
  heartbeat_conn->apid_conn_ops.ic_free_apid_connection(heartbeat_conn);
  return error;
}

static void
add_node_to_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                             IC_SEND_NODE_CONNECTION *send_node_conn)
{
  ic_require(send_node_conn);
  g_mutex_lock(apid_global->heartbeat_mutex);
  /*
    If this is the first node added the heartbeat thread is waiting for
    the first node to be connected before starting its activity.
  */
  if (!apid_global->heartbeat_thread_waiting)
    g_cond_signal(apid_global->heartbeat_cond);
  send_node_conn->next_heartbeat_node= apid_global->first_heartbeat_node;
  apid_global->first_heartbeat_node= send_node_conn;
  g_mutex_unlock(apid_global->heartbeat_mutex);
}

static void
rem_node_from_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                               IC_SEND_NODE_CONNECTION *send_node_conn)
{
  IC_SEND_NODE_CONNECTION *prev_node= NULL, *next_node;

  g_mutex_lock(apid_global->heartbeat_mutex);
  next_node= apid_global->first_heartbeat_node;
  while (next_node != send_node_conn && next_node)
  {
    prev_node= next_node;
    next_node= next_node->next_heartbeat_node;
  }
  ic_require(next_node);
  if (prev_node)
    prev_node->next_heartbeat_node= next_node->next_heartbeat_node;
  else
    apid_global->first_heartbeat_node= next_node->next_heartbeat_node;
  if (send_node_conn == apid_global->curr_heartbeat_node)
    apid_global->curr_heartbeat_node= send_node_conn->next_heartbeat_node;
  g_mutex_unlock(apid_global->heartbeat_mutex);
}

static void
prepare_send_heartbeat(IC_SEND_NODE_CONNECTION *send_node_conn,
                       IC_SOCK_BUF_PAGE *hb_page,
                       guint32 thread_id)
{
  guint32 *message_start= (guint32*)hb_page->sock_buf;
  guint32 *main_message_start;
  guint32 header_size;

  header_size= get_ndb_message_header_size(send_node_conn);
  main_message_start= message_start + header_size;

  /* Fill in message data */
  main_message_start[0]= ic_get_ic_reference(send_node_conn->my_node_id,
                                             thread_id);
  main_message_start[1]= NDB_VERSION;
  main_message_start[2]= MYSQL_VERSION;
  hb_page->size= fill_ndb_message_header(send_node_conn,
                                 3, /* API_REGREQ */
                                 IC_NDB_NORMAL_PRIO,
                                 send_node_conn->thread_id,
                                 IC_NDB_QMGR_MODULE,
                                 message_start,
                                 3, /* Size of API_REGREQ */
                                 main_message_start,
                                 0, /* No segments */
                                 NULL,
                                 NULL);
}

static int
real_send_heartbeat(IC_SEND_NODE_CONNECTION *send_node_conn,
                    IC_SOCK_BUF_PAGE *hb_page)
{
  return ndb_send(send_node_conn, hb_page, FALSE);
}

static gpointer
run_heartbeat_thread(gpointer data)
{
  int error= 0;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state= thread_state->ic_get_threadpool(thread_state);
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)
    tp_state->ts_ops.ic_thread_get_object(thread_state);
  IC_SEND_NODE_CONNECTION *last_send_node_conn= NULL;
  IC_SOCK_BUF_PAGE *free_pages= NULL;
  IC_SOCK_BUF_PAGE *hb_page= NULL;
  IC_SOCK_BUF *send_buf_pool= apid_global->send_buf_pool;

  while (!tp_state->ts_ops.ic_thread_get_stop_flag(thread_state))
  {
    if (!(hb_page= send_buf_pool->sock_buf_ops.ic_get_sock_buf_page_wait(
                   send_buf_pool,
                   &free_pages,
                   (guint32)8,
                   3000)))
      goto error;
    g_mutex_lock(apid_global->heartbeat_mutex);
    if (!apid_global->first_heartbeat_node)
    {
      last_send_node_conn= NULL;
      apid_global->heartbeat_thread_waiting= TRUE;
      g_cond_wait(apid_global->heartbeat_cond, apid_global->heartbeat_mutex);
      g_mutex_unlock(apid_global->heartbeat_mutex);
      send_buf_pool->sock_buf_ops.ic_return_sock_buf_page(send_buf_pool,
                                                          hb_page);
      continue;
    }
    if (!apid_global->curr_heartbeat_node)
    {
      last_send_node_conn= apid_global->first_heartbeat_node;
      apid_global->curr_heartbeat_node= apid_global->first_heartbeat_node;
    }
    else
    {
      if (last_send_node_conn == apid_global->curr_heartbeat_node)
      {
        /* Normal case, step forward here to next node */
        apid_global->curr_heartbeat_node=
          last_send_node_conn->next_heartbeat_node;
        last_send_node_conn= last_send_node_conn->next_heartbeat_node;
      }
      else
      {
        /*
          Remove node have moved curr_heartbeat_node forward, we need
          to send to this node as well. No action needed here.
        */
        ;
      }
    }
    prepare_send_heartbeat(last_send_node_conn, hb_page,
                           apid_global->heartbeat_thread_id);
    g_mutex_unlock(apid_global->heartbeat_mutex);
    if ((error= real_send_heartbeat(last_send_node_conn, hb_page)))
    {
      g_mutex_lock(last_send_node_conn->mutex);
      node_failure_handling(last_send_node_conn);
      g_mutex_unlock(last_send_node_conn->mutex);
    }
  }
end:
  send_buf_pool->sock_buf_ops.ic_return_sock_buf_page(send_buf_pool,
                                                      free_pages);
  tp_state->ts_ops.ic_thread_stops(thread_state);
  return NULL;
error:
  /* Serious error, heartbeat thread cannot continue, need to stop */
  ic_controlled_terminate();
  goto end;
}

/*
  ADAPTIVE SEND MODULE
  --------------------
  The idea behind this algorithm is that we want to maintain a level
  of buffering such that 95% of the sends do not have to wait more
  than a configured amount of nanoseconds. Assuming a normal distribution
  we can do statistical analysis of both real waits and waits that we
  decided to not take to see where the appropriate level of buffering
  occurs to meet at least 95% of the sends in the predefined timeframe.

  This module is a support module to the Send Message Module.
*/

/*
  The first method adaptive_send_algorithm_decision does the analysis of
  whether or not to wait or not and the second routine 
  adaptive_send_algorithm_statistics gathers the statistics. The third
  method adaptive_send_algorithm_adjust is called by the receiver thread
  very regularly to ensure that no messages are lost in the wait for
  another application thread, it also adjusts the statistics by calling
  the statistics method.
*/
static void
adaptive_send_algorithm_decision(IC_SEND_NODE_CONNECTION *send_node_conn,
                                 gboolean *will_wait,
                                 IC_TIMER current_time);
static void
adaptive_send_algorithm_statistics(IC_SEND_NODE_CONNECTION *send_node_conn,
                                   IC_TIMER current_time);
static void
adaptive_send_algorithm_adjust(IC_SEND_NODE_CONNECTION *send_node_conn);

/* This function is called very regularly from the receive thread */
static IC_SEND_NODE_CONNECTION*
adaptive_send_handling(IC_SEND_NODE_CONNECTION *conn);

/*
  This method is needed to make sure that messages sent using the
  adaptive algorithm gets sent eventually. The adaptive send algorithm
  aims that send should be done when a number of threads have decided to
  send and to group those sends such that we get better buffering on the
  connection. The algorithm keeps statistics on use of a node connection
  for this purpose. However in the case that no thread shows up to send
  the data we need to send it anyways and this method is called regularly
  (at least as often as the receive thread wakes up to receive data).
*/
static IC_SEND_NODE_CONNECTION*
adaptive_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  gboolean wake_send_thread;
  IC_SEND_NODE_CONNECTION *next_send_node_conn;
  wake_send_thread= FALSE;
  g_mutex_lock(send_node_conn->mutex);
  adaptive_send_algorithm_adjust(send_node_conn);
  if (send_node_conn->first_sbp)
  {
    /*
      Someone has left buffers around waiting for a sender, we need to
      wake the send thread to handle this sending in this case.
    */
    wake_send_thread= TRUE;
  }
  next_send_node_conn= send_node_conn->next_send_node;
  g_mutex_unlock(send_node_conn->mutex);
  if (wake_send_thread)
    g_cond_signal(send_node_conn->cond);
  return next_send_node_conn;
}

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

  adaptive_send_algorithm_statistics(send_node_conn, ic_gethrtime());
  /*
    We assume a gaussian distribution which requires us to be 1.96*mean
    value to be within 95% confidence interval and we approximate this
    by 2. Thus to keep 95% within max_wait_in_nanos the mean value
    should be max_wait_in_nanos/1.96.
  */
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
}

/*
  SEND MESSAGE MODULE
  -------------------
  To send messages using the NDB Protocol we have a struct called
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

  The Send Message Module is a support module to the Send Thread Module.
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

/*
  The external interface to this module is send_messages which is where the
  Data API methods calls when they are ready to deliver a set of messages to
  a certain node in a cluster. However since send logic is also shared by the
  send threads and also the receiver threads some methods here are used also
  by other modules.

  The logic to send is as follows:
  1) Normally messages are sent directly by the application thread
  2) Using the adaptive send algorithm the send can be delayed and then
     be sent by another application thread
  3) If a messages have been waiting for too long to be sent the receiver
     thread will pick it up and send it in the absence of a proper
     application thread to deliver it. Actually the receiver thread won't
     send it, it will only wake up a send thread to ensure that the messages
     are sent.
  4) If an application thread or a receiver thread cannot send due to a
     queueing situation in front of the socket, then the send will be done
     by a send thread.
*/
/* Not yet used
static int
send_messages(IC_INT_APID_GLOBAL *apid_global,
              guint32 cluster_id,
              guint32 node_id,
              IC_SOCK_BUF_PAGE *first_page_to_send,
              gboolean force_send);
*/

/*
  ndb_send is used by send_messages, in principle send_messages only converts
  an APID_GLOBAL object, a cluster id, a node id into a send node connection
  object.

  fill_ndb_message_header is a base function used to fill in the message
  and in particular the message header of the NDB Protocol.

  fill_in_message_id_in_ndb_messages handles message id insertion when it is
  used in the NDB Protocol messages, this is always called under protection
  of the IC_SEND_NODE_CONNECTION mutex which protects the message id variable
  used in this connection.

  get_ndb_message_header_size is a method that returns the size of the
  NDB message header in the NDB Protocol.
*/
static int ndb_send(IC_SEND_NODE_CONNECTION *send_node_conn,
                    IC_SOCK_BUF_PAGE *first_page_to_send,
                    gboolean force_send);

static guint32 get_ndb_message_header_size(
                    IC_SEND_NODE_CONNECTION *send_node_conn);

static guint32 fill_ndb_message_header(IC_SEND_NODE_CONNECTION *send_node_conn,
                                       guint32 message_number,
                                       guint32 message_priority,
                                       guint32 sender_module_id,
                                       guint32 receiver_mdoule_id,
                                       guint32 *start_message,
                                       guint32 main_message_size,
                                       guint32 *main_message,
                                       guint32 num_segments,
                                       guint32 *segment_ptrs[3],
                                       guint32 segment_lens[3]);

static void fill_in_message_id_in_ndb_message(
                          IC_SEND_NODE_CONNECTION *send_node_conn,
                          IC_SOCK_BUF_PAGE *first_page,
                          gboolean use_checksum);
/*
  prepare_real_send_handling prepares send buffers that can be used
  immediately by the socket calls, it doesn't actually send the data.
  The actual send is done by the real_send_handling.
  After send has been performed send_done_handling is called and
  a broken node connection is handled by node_failure_handling.
*/
static void prepare_real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                                       guint32 *send_size,
                                       struct iovec *write_vector,
                                       guint32 *iovec_size);
static int real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                              struct iovec *write_vector,
                              guint32 iovec_size,
                              guint32 send_size);
static int send_done_handling(IC_SEND_NODE_CONNECTION *send_node_conn);
static int node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn);

/* Function used to map cluster_id and node_id to send node connection */
static int map_id_to_send_node_connection(IC_INT_APID_GLOBAL *apid_global,
                                 guint32 cluster_id,
                                 guint32 node_id,
                                 IC_SEND_NODE_CONNECTION **send_node_conn);

static guint32 get_message_size(guint32 word1);
/* Send message functions */
static guint32
get_ndb_message_header_size(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  guint32 header_length= IC_NDB_MESSAGE_HEADER_SIZE;
  return header_length + send_node_conn->link_config->use_message_id;
}

static guint32
get_message_size(guint32 word1)
{
  return ((word1 >> 8) & 0xFFFF);
}

static guint32
fill_ndb_message_header(IC_SEND_NODE_CONNECTION *send_node_conn,
                        guint32 message_number,
                        guint32 message_priority,
                        guint32 sender_module_id,
                        guint32 receiver_module_id,
                        guint32 *start_message,
                        guint32 main_message_size,
                        guint32 *main_message,
                        guint32 num_segments,
                        guint32 *segment_ptrs[3],
                        guint32 segment_lens[3])
{
  guint32 header_length= IC_NDB_MESSAGE_HEADER_SIZE;
  register guint32 word;
  register guint32 loc_variable;
  guint32 tot_message_size;
  gboolean use_message_id= send_node_conn->link_config->use_message_id;
  gboolean use_checksum= send_node_conn->link_config->use_checksum;
  guint32 *message_ptr;
  guint32 i;
  /*
    First calculate length of message header:
      It is 3 words (32-bit words) plus an optional word
      that contains a message number which is unique for the sender
      node. There is also an optional checksum word at the end of
      the message.
    Second copy the main message part and the segment parts into the
    right place in the message if not already done.
  */
  header_length+= use_message_id;
  tot_message_size= header_length + main_message_size;
  if (main_message != (start_message + header_length))
  {
    memcpy(start_message + header_length,
           main_message,
           main_message_size * sizeof(guint32));
  }
  if (num_segments > 0)
  {
    message_ptr= start_message + header_length + main_message_size;
    for (i= 0; i < num_segments; i++)
    {
      tot_message_size+= (segment_lens[i] + 1);
      *message_ptr= segment_lens[i];
      message_ptr++;
    }
    for (i= 0; i < num_segments; i++)
    {
      if (segment_ptrs[i] != message_ptr)
      {
        memcpy(message_ptr,
               segment_ptrs[i],
               segment_lens[i] * sizeof(guint32));
      }
      message_ptr+= segment_lens[i];
    }
  }
  tot_message_size+= use_checksum;
  /*
    Calculate first header word
    Bit 0, 7, 24, 31 are set if big endian on sender side
    Bit 1,25   Fragmented messages indicator (Always 0 for now, TODO)
    Bit 2      Message id used flag
    Bit 3      ???
    Bit 4      Checksum used flag
    Bit 5-6    Message priority
    Bit 8-23   Total message size
    Bit 26-30  Main message size (max 25)
  */
  word= 0;

  /* Set byte order indicator */
  if (ic_glob_byte_order)
    word= 0x81000081; /* Set bit 0,7,24,31 independent of byte order */

  /* Set message id used flag */
  loc_variable= use_message_id << 2;
  word|= loc_variable;

  /* Set checksum used flag */
  loc_variable= use_checksum << 4;
  word|= loc_variable;

  /* Set message priority */
  ic_require(message_priority <= IC_NDB_MAX_PRIO_LEVEL);
  loc_variable= message_priority << 5;
  word|= loc_variable;

  /* Set total message size */
  loc_variable= tot_message_size << 8;
  word|= loc_variable;

  /* Set main message size */
  loc_variable= main_message_size + num_segments;
  ic_require(loc_variable <= IC_NDB_MAX_MAIN_MESSAGE_SIZE);
  loc_variable<<= 26;
  word|= loc_variable;

  start_message[0]= word;
  /*
    Calculate second header word
    Bit 0-19   Message number (e.g. API_HBREQ)
    Bit 20-25  Trace number
    Bit 26-27  Number of segements used
    Bit 28-31  Not used (?)
  */

  /* Set message number */
  word= message_number;

  /* Set trace number (0 always for now) */

  /* Set number of segments used */
  loc_variable= num_segments << 26;
  word|= loc_variable;

  start_message[1]= word;
  /*
    Calculate third header word
    Bit 0-15   Sender module id
    Bit 16-31  Receiver module id
  */

  /* Set sender module id */
  ic_require(sender_module_id <= IC_NDB_MAX_MODULE_ID);
  word= sender_module_id;

  /* Set receiver module id */
  ic_require(receiver_module_id <= IC_NDB_MAX_MODULE_ID);
  loc_variable= receiver_module_id << 16;
  word|= loc_variable;

  start_message[2]= word;
  if (use_message_id)
    start_message[3]= 0;
  if (use_checksum)
  {
    word= 0;
    for (i= 0; i < tot_message_size - 1; i++)
      word ^= start_message[i];
    start_message[tot_message_size]= word;
  }
  return tot_message_size;
}

static void
fill_in_message_id_in_ndb_message(IC_SEND_NODE_CONNECTION *send_node_conn,
                                  IC_SOCK_BUF_PAGE *first_page,
                                  gboolean use_checksum)
{
  guint32 word1, current_size, message_id, prev_checksum, message_size;
  guint32 *word_ptr, *message_id_ptr, *checksum_ptr;
  IC_SOCK_BUF_PAGE *current_page= first_page;

  while (current_page)
  {
    word_ptr= (guint32*)current_page->sock_buf;
    current_size= 0;
    while (current_size < current_page->size)
    {
      word1= *word_ptr;
      message_size= get_message_size(word1);
      message_id_ptr= word_ptr + IC_NDB_MESSAGE_HEADER_SIZE;

      message_id= send_node_conn->message_id++;
      if (use_checksum)
      {
        checksum_ptr= (word_ptr + (message_size - 1));
        prev_checksum= *checksum_ptr;
        *checksum_ptr= prev_checksum ^ message_id;
      }
      current_size+= (message_size * sizeof(guint32));
      word_ptr+= message_size;
    }
    ic_require(current_size == current_page->size);
    current_page= current_page->next_sock_buf_page;
  }
}

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
  if (send_node_conn->link_config->use_message_id)
  {
    /*
      We are using message id's on the link, we need to fill in proper
      message id's in all the NDB messages and we also need to update
      the potential checksum in each of the messages where a message
      id is set.
    */
    fill_in_message_id_in_ndb_message(send_node_conn,
                                      first_page_to_send,
                                send_node_conn->link_config->use_checksum);
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
  IC_INT_APID_GLOBAL *apid_global= send_node_conn->apid_global;
  IC_SOCK_BUF *send_buf_pool= apid_global->send_buf_pool;
  send_buf_pool->sock_buf_ops.ic_return_sock_buf_page(
    send_buf_pool, send_node_conn->first_sbp);
  rem_node_from_heartbeat_thread(send_node_conn->apid_global,
                                 send_node_conn);
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
  IC_INT_APID_GLOBAL *apid_global= send_node_conn->apid_global;
  IC_SOCK_BUF *send_buf_pool= apid_global->send_buf_pool;
  DEBUG_ENTRY("real_send_handling");
  error= conn->conn_op.ic_writev_connection(conn,
                                            write_vector,
                                            iovec_size,
                                            send_size, 2);

  /* Release memory buffers used in send */
  send_buf_pool->sock_buf_ops.ic_return_sock_buf_page(
    send_buf_pool, send_node_conn->release_sbp);
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

static int
map_id_to_send_node_connection(IC_INT_APID_GLOBAL *apid_global,
                               guint32 cluster_id,
                               guint32 node_id,
                               IC_SEND_NODE_CONNECTION **send_node_conn)
{
  IC_SEND_NODE_CONNECTION *loc_send_node_conn;
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
  loc_send_node_conn= cluster_comm->send_node_conn_array[node_id];
  if (!loc_send_node_conn)
    return error;
  *send_node_conn= loc_send_node_conn;
  return 0;
}

/*
  This is the internal method used to actually send the gathered signals.
  It essentially finds the proper IC_SEND_NODE_CONNECTION object and calls
  ndb_send which handles the details of sending.
*/
/* Not yet used
static int
send_messages(IC_INT_APID_GLOBAL *apid_global,
              guint32 cluster_id,
              guint32 node_id,
              IC_SOCK_BUF_PAGE *first_page_to_send,
              gboolean force_send)
{
  IC_SEND_NODE_CONNECTION *send_node_conn;
  int error;

  if ((error= map_id_to_send_node_connection(apid_global,
                                             cluster_id,
                                             node_id,
                                             &send_node_conn)))
    return error;
  return ndb_send(send_node_conn, first_page_to_send, force_send);
}
*/

/*
  SEND THREAD MODULE
  ------------------
  This module contains the code to run the send thread and the supporting
  methods to perform send operations through the Data API.

  There are two types of threads to manage send threads, the reason is that
  the server part of the connect handling requires only one listener per
  hostname-port pair, thus a number of send threads use one listen thread to
  handle listening to the server part of the socket. The client part can be
  handled by each send thread by sending connect attempts to the server side
  every now and then.

  The send threads have two responsibilities, the first is to connect nodes.
  After they have been connected the connection needs to be delivered to
  a receive thread. The second responsibility is in the connected state
  when the send thread should assist application threads to send data when
  a queue has lined up in front of the socket.

  At the moment there is one send thread per possible node connection. This
  can obviously become a large number of threads, possibly in the range of
  thousands of threads. Thus an alternative design later on could be that
  each send thread manages more than one node connection. Since the send
  thread handling is very much a separate module this should be a fairly
  simple change if needed.

  The Send Thread Module has very few interfaces to the rest of the code.
  The only direct call from outside is to the 'start_send_threads' function.
  This function is responsible for starting up the send threads. The call
  to stop the send threads is done by stopping the send threadpool. This
  pool interacts with send threads in such a manner that in the normal
  case the send threads should all be gone within 2-3 seconds after
  stopping the threadpool. The threadpool is handling both the send threads
  and the listen threads.

  The send thread can indirectly be called by writing in send thread
  protected areas with mutex protection followed often by a signal of an
  event to ensure that a send thread is acting on the request.

  Finally of course the send threads also reacts to external events when
  nodes are connecting.
*/

/* Server part needs to return for stop check every 2-3 seconds */
static int check_timeout_func(void *timeout_obj, int timer);

/* Client part regularly checks if thread needs to stop */
static int check_thread_state(void *timeout_obj, int timer);

/* Function to check which node was connected to */
static gboolean check_connection(IC_SEND_NODE_CONNECTION *send_node_conn,
                                 IC_CONNECTION *conn);

/* This function is used to close down the listen server thread */
static void close_listen_server_thread(
          IC_LISTEN_SERVER_THREAD *listen_server_thread,
          IC_INT_APID_GLOBAL *apid_global);

/*
  The listen thread is used to wait for clients to connect to the node.
  All send threads sharing hostname and port also share listen thread
  if they are server threads.
*/
static gpointer run_listen_thread(void *data);

/* This functions is used to close down the send thread */
static void remove_send_thread_from_listen_thread(
  IC_SEND_NODE_CONNECTION *send_node_conn,
  IC_LISTEN_SERVER_THREAD *listen_server_thread);

/* These functions are used to secure the node connections.  */
static int server_api_connect(void *data);
static int client_api_connect(void *data);
static int authenticate_client_connection(IC_CONNECTION *conn,
                                          guint32 my_nodeid,
                                          guint32 server_nodeid);
static int authenticate_server_connection(IC_CONNECTION *conn,
                                          guint32 my_nodeid,
                                          guint32 *client_nodeid);

/*
  After connection have been set-up the node connection is also moved to
  the receive thread and then the active_send_thread function is where the
  send thread spends its time in the connected phase.
*/
static void move_node_to_receive_thread(
                 IC_SEND_NODE_CONNECTION *send_node_conn);
static void active_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn,
                               IC_THREADPOOL_STATE *send_tp,
                               IC_THREAD_STATE *thread_state);

/*
  The send thread is executed by the function run_send_thread, in its
  startup it connects to the server part, or uses a listen thread to
  wait for clients to connect or if connect happens externally it waits
  external connect code to report a connection set-up.
  The connect code starts up by preparing to set-up node connections
  using the function prepare_set_up_send_connection and then the actual
  connect happens in the connect_by_send_thread.
*/
static gpointer run_send_thread(void *data);
static int prepare_set_up_send_connection(
              IC_SEND_NODE_CONNECTION *send_node_conn,
              IC_INT_APID_GLOBAL *apid_global,
              gboolean is_server_part,
              IC_THREADPOOL_STATE *send_tp,
              IC_THREAD_STATE *thread_state);
static int connect_by_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn,
                                  IC_THREAD_STATE *thread_state,
                                  gboolean is_server_part);

/*
  start_send_threads is the function to start up the send threads.
  It uses start_send_thread to start each thread and it also uses
  a support function set_hostname_and_port to handle some simple string
  logic around hostnames and ports that consume many lines of code.
*/
static int start_send_threads(IC_INT_APID_GLOBAL *apid_global);
static int start_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn);
static int set_hostname_and_port(IC_SEND_NODE_CONNECTION *send_node_conn,
                                 IC_SOCKET_LINK_CONFIG *link_config,
                                 guint32 my_node_id);

/*
  This function is used by Cluster Server to transfer a connection
   to the Data API.
*/
static int external_connect(IC_APID_GLOBAL *apid_global,
                            guint32 cluster_id,
                            guint32 node_id,
                            IC_CONNECTION *conn);

static int
external_connect(IC_APID_GLOBAL *ext_apid_global,
                 guint32 cluster_id,
                 guint32 node_id,
                 IC_CONNECTION *conn)
{
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)ext_apid_global;
  int error;

  if ((error= map_id_to_send_node_connection(apid_global,
                                             cluster_id,
                                             node_id,
                                             &send_node_conn)))
    return error;
  g_mutex_lock(send_node_conn->mutex);
  send_node_conn->conn= conn;
  g_cond_signal(send_node_conn->cond);
  g_mutex_unlock(send_node_conn->mutex);
  return 0;
}

/*
  We also place the methods called from other threads that assist the
  send threads in performing the send functionality
*/

/* Send thread functions */
static int
check_timeout_func(__attribute__ ((unused)) void *timeout_obj, int timer)
{
  if (timer < 2)
    return 0;
  else
    return 1;
}

/* TODO: This function needs to check which node was actually connected to */
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
run_listen_thread(void *data)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state= thread_state->ic_get_threadpool(thread_state);
  IC_LISTEN_SERVER_THREAD *listen_server_thread= (IC_LISTEN_SERVER_THREAD*)
    tp_state->ts_ops.ic_thread_get_object(thread_state);
  IC_CONNECTION *fork_conn, *conn;
  IC_SEND_NODE_CONNECTION *iter_send_node_conn;
  GList *list_iterator;
  gboolean found;
  int ret_code;

  tp_state->ts_ops.ic_thread_started(thread_state);
  conn= listen_server_thread->conn;
  if ((ret_code= conn->conn_op.ic_set_up_connection(conn,
                                           check_timeout_func,
                                           (void*)listen_server_thread)))
  {
    listen_server_thread->stop_ordered= TRUE;
    goto end;
  }
  /* We report start-up complete to the thread starting us */
  if (tp_state->ts_ops.ic_thread_startup_done(thread_state))
    goto end;
  /* We have been asked to start listening and so we start listening */
  do
  {
    if (!(ret_code= conn->conn_op.ic_accept_connection(conn)))
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
      if (tp_state->ts_ops.ic_thread_get_stop_flag(thread_state))
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
end:
  tp_state->ts_ops.ic_thread_stops(thread_state);
  return NULL;
}

static void
remove_send_thread_from_listen_thread(
  IC_SEND_NODE_CONNECTION *send_node_conn,
  IC_LISTEN_SERVER_THREAD *listen_server_thread)
{
  IC_THREADPOOL_STATE *tp_state;

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
    g_mutex_unlock(listen_server_thread->mutex);
    tp_state= send_node_conn->apid_global->send_thread_pool;
    tp_state->tp_ops.ic_threadpool_join(tp_state,
                                        listen_server_thread->thread_id);
    close_listen_server_thread(listen_server_thread,
                               send_node_conn->apid_global);
  }
  else
    g_mutex_unlock(listen_server_thread->mutex);
}

/*
  This function is used to assist the application threads with sending
  data, this is where the send thread is supposed to spend most of its
  time, waiting for application threads to wake it up when they have
  not been able to send the data immediately.
*/
static void
active_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn,
                   IC_THREADPOOL_STATE *send_tp,
                   IC_THREAD_STATE *thread_state)
{
  guint32 send_size;
  guint32 iovec_size;
  int error;
  struct iovec write_vector[MAX_SEND_BUFFERS];

  send_tp->ts_ops.ic_thread_lock(thread_state);
  send_node_conn->send_thread_is_sending= FALSE;
  send_node_conn->send_thread_active= TRUE;
  do
  {
    if (send_node_conn->stop_ordered || !send_node_conn->node_up)
      break;
    if (!send_node_conn->first_sbp)
    {
      /* All buffers have been sent, we can go to sleep again */
      send_node_conn->send_active= FALSE;
      send_node_conn->send_thread_is_sending= FALSE;
      send_tp->ts_ops.ic_thread_wait(thread_state);
    }
    if (send_node_conn->stop_ordered || !send_node_conn->node_up)
      break;
    g_assert(send_node_conn->send_thread_is_sending);
    g_assert(send_node_conn->send_active);
    prepare_real_send_handling(send_node_conn, &send_size,
                               write_vector, &iovec_size);
    send_tp->ts_ops.ic_thread_unlock(thread_state);
    error= real_send_handling(send_node_conn, write_vector,
                              iovec_size, send_size);
    send_tp->ts_ops.ic_thread_lock(thread_state);
    if (error)
      send_node_conn->node_up= FALSE;
  } while (1);
  if (!send_node_conn->node_up)
    node_failure_handling(send_node_conn);
  send_node_conn->send_active= FALSE;
  send_node_conn->send_thread_active= FALSE;
  send_node_conn->send_thread_is_sending= FALSE;
  send_tp->ts_ops.ic_thread_unlock(thread_state);
  return;
}

/*
  Function to move a node connection into a receive thread when the
  node has connected to ensure that both the send and receive parts
  are handled properly.
*/
static void
move_node_to_receive_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  IC_INT_APID_GLOBAL *apid_global= send_node_conn->apid_global;
  IC_NDB_RECEIVE_STATE *rec_state= apid_global->receive_threads[0];

  /*
    At this point in time the send node connection object is only handled
    by the send thread. As soon as we have delivered the object to the
    receive thread we need to protect this object since it's handled
    by multiple threads.
  */
  g_mutex_lock(rec_state->mutex);
  send_node_conn->next_add_node= rec_state->first_add_node;
  rec_state->first_add_node= send_node_conn;
  g_mutex_unlock(rec_state->mutex);
}

static int
check_thread_state(void *timeout_obj,
                   __attribute__ ((unused))int timer)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)timeout_obj;
  IC_THREADPOOL_STATE *tp_state= thread_state->ic_get_threadpool(thread_state);

  if (tp_state->ts_ops.ic_thread_get_stop_flag(thread_state))
    return 1;
  return 0;
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
                       IC_THREAD_STATE *thread_state,
                       gboolean is_server_part)
{
  IC_CONNECTION *conn;
  IC_SOCKET_LINK_CONFIG *link_config;
  IC_LISTEN_SERVER_THREAD *listen_server_thread;
  gboolean stop_ordered= FALSE;
  IC_THREADPOOL_STATE *tp_state;
  int ret_code= 0;

  if (is_server_part)
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
          thread into action to start the listening on the server socket
        */
        tp_state= send_node_conn->apid_global->send_thread_pool;
        tp_state->tp_ops.ic_threadpool_run_thread(tp_state,
                                        listen_server_thread->thread_id);
      }
      else /* stop_ordered == TRUE */
      {
        return IC_ERROR_STOP_ORDERED;
      }
      g_mutex_lock(send_node_conn->mutex);
      if (!send_node_conn->stop_ordered)
        g_cond_wait(send_node_conn->cond, send_node_conn->mutex);
    }
    g_mutex_unlock(send_node_conn->mutex);
  }
  else /* Client connection */
  {
    conn= send_node_conn->conn;
    link_config= send_node_conn->link_config;
    if (!send_node_conn->other_port_number)
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
    if ((ret_code= conn->conn_op.ic_set_up_connection(conn,
                                                      check_thread_state,
                                                      (void*)thread_state)))
      return ret_code;
  }
  return ret_code;
}

static void
close_listen_server_thread(IC_LISTEN_SERVER_THREAD *listen_server_thread,
                           IC_INT_APID_GLOBAL *apid_global)
{
  IC_CONNECTION *conn;
  if (listen_server_thread)
  {
    apid_global->listen_server_thread[listen_server_thread->index]= NULL;
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
      (error= IC_AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "ok")) ||
      (error= ic_send_with_cr(conn, send_buf)) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= IC_AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "1 1")))
    return error;
  return 0;
}

static int
authenticate_server_connection(IC_CONNECTION *conn,
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
      (error= IC_AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "ndbd")) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= IC_AUTHENTICATE_ERROR, FALSE) ||
      (!strcmp(read_buf, "ndbd passwd")) ||
      (error= ic_send_with_cr(conn, "ok")) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (error= IC_AUTHENTICATE_ERROR, FALSE) ||
      (ic_conv_str_to_int(read_buf, &client_id, &len)) ||
      (read_buf[len] != ' ') ||
      (len= len + 1, FALSE) ||
      (ic_conv_str_to_int(&read_buf[len], &my_id, &len)) ||
      ((guint64)my_nodeid != my_id) ||
      (client_id >= IC_MAX_NODE_ID) ||
      (error= ic_send_with_cr(conn, "1 1")) ||
      (error= IC_AUTHENTICATE_ERROR, FALSE) ||
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

static int
prepare_set_up_send_connection(IC_SEND_NODE_CONNECTION *send_node_conn,
                               IC_INT_APID_GLOBAL *apid_global,
                               gboolean is_server_part,
                               IC_THREADPOOL_STATE *send_tp,
                               IC_THREAD_STATE *thread_state)
{
  int ret_code;
  guint32 i, listen_inx;
  gboolean found= FALSE;
  IC_CONNECTION *send_conn;
  IC_LISTEN_SERVER_THREAD *listen_server_thread= NULL;

  if (is_server_part)
  {
    /*
      Here we need to ensure there is a thread handling this server
      connection. We ensure there is one thread per port and this
      thread will start connections for the various send threads
      and report to them when a connection has been performed.
      When we come here to start things up we're still in single-threaded
      mode, so we don't need to protect things through mutexes.
    */
    send_conn= send_node_conn->conn;
    for (i= 0; i < apid_global->num_listen_server_threads; i++)
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
        listen_inx= apid_global->num_listen_server_threads;
        if (!(apid_global->listen_server_thread[listen_inx]=
    	  (IC_LISTEN_SERVER_THREAD*)
              ic_calloc(sizeof(IC_LISTEN_SERVER_THREAD))))
          break;
        /* Successful allocation of memory */
        listen_server_thread= apid_global->listen_server_thread[listen_inx];
        listen_server_thread->index= listen_inx;
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
        if ((ret_code= send_tp->tp_ops.ic_threadpool_get_thread_id(
                            send_tp,
                            &listen_server_thread->thread_id)))
           break;
        thread_state= send_tp->tp_ops.ic_threadpool_get_thread_state(
                            send_tp,
                            listen_server_thread->thread_id);
        send_tp->ts_ops.ic_thread_set_mutex(thread_state,
                    listen_server_thread->mutex);
        send_tp->ts_ops.ic_thread_set_cond(thread_state,
                    listen_server_thread->cond);
        ret_code= send_tp->tp_ops.ic_threadpool_start_thread_with_thread_id(
                                  send_tp,
                                  listen_server_thread->thread_id,
                                  run_listen_thread,
                                  (gpointer)listen_server_thread,
                                  IC_SMALL_STACK_SIZE,
                                  TRUE);                                  
      } while (0);
      if (ret_code)
      {
        /* We didn't succeed in starting listen thread, quit */
        close_listen_server_thread(
           apid_global->listen_server_thread[listen_inx],
           apid_global);
        send_node_conn->send_thread_ended= TRUE;
        send_tp->ts_ops.ic_thread_stops(thread_state);
        return 1;
      }
      apid_global->num_receive_threads++;
    }
    else
    {
     /* Found an existing a new thread to use, need not start a new thread */
      g_mutex_lock(listen_server_thread->mutex);
      if (!(listen_server_thread->first_send_node_conn=
            g_list_prepend(listen_server_thread->first_send_node_conn,
                           (void*)send_node_conn)))
      {
        g_mutex_unlock(listen_server_thread->mutex);
        g_mutex_lock(send_node_conn->mutex);
        send_node_conn->stop_ordered= TRUE;
        send_node_conn->send_thread_ended= TRUE;
        g_mutex_unlock(send_node_conn->mutex);
        send_tp->ts_ops.ic_thread_stops(thread_state);
      }
      g_mutex_unlock(listen_server_thread->mutex);
    }
  }
  else
  {
    send_node_conn->conn= ic_create_socket_object(
                TRUE,   /* is client */
                FALSE,  /* Mutex supplied by API code instead */
                FALSE,  /* This thread is already a connection thread */
                CONFIG_READ_BUF_SIZE, /* Used by authentication function */
                client_api_connect,   /* Authentication function */
                (void*)send_node_conn);  /* Authentication object */
    if (send_node_conn->conn == NULL)
      send_node_conn->stop_ordered= TRUE;
  }
  return 0;
}

static gpointer
run_send_thread(void *data)
{
  gboolean is_server_part;
  int ret_code;
  IC_INT_APID_GLOBAL *apid_global;
  IC_CONNECTION *conn;
  IC_LISTEN_SERVER_THREAD *listen_server_thread= NULL;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *send_tp= thread_state->ic_get_threadpool(thread_state);
  IC_SEND_NODE_CONNECTION *send_node_conn= (IC_SEND_NODE_CONNECTION*)
    send_tp->ts_ops.ic_thread_get_object(thread_state);
  /*
    First step is to create a connection object, then it's time to
    start setting it up when told to do so by the main thread. We want
    to ensure that all threads and other preparatory activities have
    been done before we start connecting to the cluster. As soon as we
    connect to the cluster we're immediately a real-time application and
    must avoid to heavy activities such as starting 100's of threads.
  */
  apid_global= send_node_conn->apid_global;
  send_tp->ts_ops.ic_thread_started(thread_state);
  is_server_part= (send_node_conn->my_node_id ==
                   send_node_conn->link_config->server_node_id);
  if (!apid_global->use_external_connect)
  {
     if (prepare_set_up_send_connection(send_node_conn,
                                        apid_global,
                                        is_server_part,
                                        send_tp,
                                        thread_state))
       return NULL;
  }
  else /* apid_global->use_external_connect == TRUE */
  {
    /*
      When we come here the Data API should not set-up the connections to the
      other nodes in the cluster, rather it should wait for those to be
      delivered to it through an interface to the IC_APID_GLOBAL object.
      This functionality is mainly needed for the iClaustron Cluster Server
      that listens for connections from nodes and then after the configuration
      have been delivered it converts the connection into a NDB Protocol
      connection. We implement this by delivering the connection to the
      iClaustron Data API object.

      For the moment there is no need to set-up any thing here.
    */
    ;
  }
  /*
    For the server part we have now attached ourself to a listen thread
    For the client part we are ready to start attempting to connect
  */
  if (send_tp->ts_ops.ic_thread_startup_done(thread_state))
    goto end;
  /*
    The main thread have started up all threads and all communication objects
    have been created, it's now time to connect to the clusters.
  */
  ret_code= 0;
  while (!send_tp->ts_ops.ic_thread_get_stop_flag(thread_state))
  {
    if (ret_code)
    {
      /*
        Something failed in the connect process. We report the error and
        continue after sleeping for a few seconds.
      */
      ic_print_error(ret_code);
      ic_sleep(3);
    }
    if (!apid_global->use_external_connect)
    {
      if ((ret_code= connect_by_send_thread(send_node_conn,
                                            thread_state,
                                            is_server_part)))
        continue;
      conn= send_node_conn->conn;
      /*
        The send thread needs to perform the login function before moving onto
        the NDB Protocol phase.
      */
      if ((ret_code= conn->conn_op.ic_login_connection(conn)))
        continue;
    }
    else
    {
      /*
        For external connect users we wait on the send node condition object
        which will be signalled when we receive a connection ready to be
        converted to a NDB Protocol connection.
        We will wait 3 seconds and then check if stop has been ordered to
        ensure that we can handle termination of process in a proper manner
        and also when the API is terminated.
      */
      send_tp->ts_ops.ic_thread_lock_and_wait(thread_state);
      /*
         New connection has arrived from external source, make any
         set-up needed before delivering it to receive thread and starting
         the active send thread handling.
      */
      send_tp->ts_ops.ic_thread_unlock(thread_state);
    }
    /*
      We have successfully connected to another node in this send thread.
      It is now time to move the receive part to the proper receive
      thread. First initialise the message id since we have a new
      connection now.
    */
    send_node_conn->message_id= 1;
    move_node_to_receive_thread(send_node_conn);
    /*
      Move node also to heartbeat thread to ensure keep sending heartbeat
      messages to the node even if no other traffic is ongoing
    */
    add_node_to_heartbeat_thread(apid_global, send_node_conn);
    /*
      Now this thread is only handling the send_thread part which is
      taken care by active_send_thread, it returns when the connection
      for some reason have been dropped or a stop has been ordered.
    */
    active_send_thread(send_node_conn, send_tp, thread_state);
  }
end:
  send_tp->ts_ops.ic_thread_lock(thread_state);
  if (is_server_part && !apid_global->use_external_connect)
  {
    /*
      Remove our send node connection from the list on this listen
      server thread as part of server part cleanup since we have
      attached ourselves to the listen server thread. If we are the
      last send thread to remove ourself from a listen thread we
      will also remove the listen thread entirely.
    */
    remove_send_thread_from_listen_thread(send_node_conn,
                                          listen_server_thread);
  }
  send_node_conn->send_thread_ended= TRUE;
  send_tp->ts_ops.ic_thread_unlock(thread_state);
  /* Thread is ready to stop */
  send_tp->ts_ops.ic_thread_stops(thread_state);
  return NULL;
}

static int
start_send_thread(IC_SEND_NODE_CONNECTION *send_node_conn)
{
  int ret_code;
  IC_INT_APID_GLOBAL *apid_global= send_node_conn->apid_global;
  IC_THREADPOOL_STATE *tp_state= apid_global->send_thread_pool;
  IC_THREAD_STATE *thread_state;

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
    set-up the connection. We set the synch_startup flag to indicate that
    we want the thread pool to return when the thread has performed its
    startup phase.
    We reuse the mutex and condition from the send thread as mutex for the
    thread state. This means that we can easier make sure to wake the
    thread when it's necessary to stop the thread pool or thread.
  */
  if ((ret_code= tp_state->tp_ops.ic_threadpool_get_thread_id(
                              tp_state,
                              &send_node_conn->thread_id)))
    return ret_code;
  thread_state= tp_state->tp_ops.ic_threadpool_get_thread_state(
                              tp_state,
                              send_node_conn->thread_id);
  tp_state->ts_ops.ic_thread_set_mutex(thread_state,
                      send_node_conn->mutex);
  tp_state->ts_ops.ic_thread_set_cond(thread_state,
                      send_node_conn->cond);
  ret_code= tp_state->tp_ops.ic_threadpool_start_thread_with_thread_id(
                              tp_state,
                              send_node_conn->thread_id,
                              run_send_thread,
                              (gpointer)send_node_conn,
                              IC_SMALL_STACK_SIZE,
                              TRUE);
  /*
    The send thread is done with its start-up phase and we're ready to start
    the next send thread if the start thread was successful.
  */
  return ret_code;
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
    copy_ptr+= second_hostname_len;
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

static int
start_send_threads(IC_INT_APID_GLOBAL *apid_global)
{
  guint32 node_id, cluster_id, my_node_id;
  int error= 0;
  IC_API_CONFIG_SERVER *apic= apid_global->apic;
  IC_GRID_COMM *grid_comm= apid_global->grid_comm;
  IC_CLUSTER_COMM *cluster_comm;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_SEND_NODE_CONNECTION *send_node_conn;
  IC_SOCKET_LINK_CONFIG *link_config;
  DEBUG_ENTRY("start_send_threads");

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
      send_node_conn= cluster_comm->send_node_conn_array[node_id];
      if (node_id == my_node_id ||
          (link_config= apic->api_op.ic_get_communication_object(apic,
                            cluster_id,
                            node_id,
                            my_node_id)))
      {
        g_assert(clu_conf->node_config[node_id]);
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
        if (!apic->api_op.ic_use_iclaustron_cluster_server(apic))
        {
          /*
            In NDB, API's don't need to connect to other API's and
            to management server
          */
          if (clu_conf->node_config[node_id])
          {
            g_assert(clu_conf->node_types[my_node_id] == IC_CLIENT_TYPE);
            g_assert(clu_conf->node_types[node_id] == IC_CLIENT_TYPE ||
                     clu_conf->node_types[node_id] == IC_CLUSTER_SERVER_TYPE);
          }
        }
        else
        {
          /* In iClaustron all nodes are connected */
          g_assert(!clu_conf->node_config[node_id]);
        }
        /* Indicate we have no send thread here */
        if (send_node_conn)
          send_node_conn->send_thread_ended= TRUE;
      }
    }
  }
  return 0;
error:
  return error;
}

/*
  Message Logic Modules
  ---------------------

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
  API. This code signals to the Execute Message Module if there have been
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

static IC_EXEC_MESSAGE_FUNC ic_exec_message_func_array[2][1024];

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
handle_key_ai(__attribute__ ((unused)) IC_APID_OPERATION *apid_op,
              __attribute__ ((unused)) IC_TRANSACTION *trans,
              __attribute__ ((unused)) guint32 *ai_data,
              __attribute__ ((unused)) guint32 data_size,
              __attribute__ ((unused))IC_MESSAGE_ERROR_OBJECT *error_object)
{
  return 0;
}

static int
handle_scan_ai(__attribute__ ((unused)) IC_APID_OPERATION *apid_op,
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
  IC_APID_OPERATION *apid_op;
  IC_TRANSACTION *trans_op;
  IC_DYNAMIC_TRANSLATION *dyn_trans= 0;
  guint32 data_size;
  guint64 connection_ptr= header_data[0];
  guint32 transid_part1= header_data[1];
  guint32 transid_part2= header_data[2];

  if (dyn_trans->dt_ops.ic_get_translation_object(dyn_trans,
                                                  connection_ptr,
                                                  &connection_obj))
  { 
    return 1;
  }
  apid_op= (IC_APID_OPERATION*)connection_obj;
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
  trans_op= apid_op->trans_obj;
  if (trans_op->transaction_id[0] != transid_part1 ||
      trans_op->transaction_id[1] != transid_part2)
  {
    g_assert(FALSE);
    return 1;
  }
  if (apid_op->op_type == SCAN_OPERATION)
  {
    return handle_scan_ai(apid_op,
                          trans_op,
                          attrinfo_data,
                          data_size,
                          error_object);
  }
  return handle_key_ai(apid_op,
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

/* Initialize function pointer array for messages */
static void
initialize_message_func_array()
{
  guint32 i;
  for (i= 0; i < 1024; i++)
  {
    ic_exec_message_func_array[0][i].ic_exec_message_func= NULL;
    ic_exec_message_func_array[1][i].ic_exec_message_func= NULL;
  }
  ic_exec_message_func_array[0][IC_SCAN_TABLE_CONF].ic_exec_message_func=
    execSCAN_TABLE_CONF_v0;
  ic_exec_message_func_array[0][IC_SCAN_TABLE_REF].ic_exec_message_func=
    execSCAN_TABLE_REF_v0;
  ic_exec_message_func_array[0][IC_TRANSACTION_CONF].ic_exec_message_func=
    execTRANSACTION_CONF_v0;
  ic_exec_message_func_array[0][IC_TRANSACTION_REF].ic_exec_message_func=
    execTRANSACTION_REF_v0;
  ic_exec_message_func_array[0][IC_PRIMARY_KEY_OP_CONF].ic_exec_message_func=
    execPRIMARY_KEY_OP_CONF_v0;
  ic_exec_message_func_array[0][IC_PRIMARY_KEY_OP_REF].ic_exec_message_func=
    execPRIMARY_KEY_OP_REF_v0;
  ic_exec_message_func_array[0][IC_ATTRIBUTE_INFO].ic_exec_message_func=
    execATTRIBUTE_INFO_v0;
}

/*
  EXECUTE MESSAGE MODULE
  ----------------------
  This module contains the code executed in the user thread to execute
  messages posted to this thread by the receiver thread. The actual
  logic to handle the individual messages received is found in the
  various MEssage Logic Modules.
*/

/*
  poll_messages is called from outside of this module as a result of some
  user action, the user have decided to check for arrival of new messages
  that he is expecting to arrive. The remainder of the functions are used
  to support this function.
*/
static int poll_messages(IC_APID_CONNECTION *apid_conn, glong wait_time);

/*
  The get_thread_messages gets a page of messages from the thread connection.
  For each message it finds in this page it calls execute_message to perform
  the execution of the message, create_ndb_message turns the message into
  a message format used by the Message Logic Module, these modules only
  see NDB Messages arriving and have no notion of any other format.
  The get_exec_message_func function gets a function pointer to the message
  execution function in a Message Logic Module based on message id and
  version of the NDB Protocol.
  set_sock_buf_page_empty is a simple support function to ensure we don't
  duplicate code.
*/
static IC_EXEC_MESSAGE_FUNC* get_exec_message_func(guint32 message_id,
                                                   guint32 version_num);
static void set_sock_buf_page_empty(IC_THREAD_CONNECTION *thd_conn);
static IC_SOCK_BUF_PAGE*
get_thread_messages(IC_APID_CONNECTION *ext_apid_conn, glong wait_time);
static int execute_message(IC_SOCK_BUF_PAGE *ndb_message_page);
static int create_ndb_message(IC_SOCK_BUF_PAGE *message_page,
                              IC_NDB_MESSAGE_OPAQUE_AREA *ndb_message_opaque,
                              IC_NDB_MESSAGE *ndb_message,
                              guint32 *message_id);

static IC_EXEC_MESSAGE_FUNC*
get_exec_message_func(guint32 message_id,
                      __attribute__ ((unused)) guint32 version_num)
{
  return &ic_exec_message_func_array[0][message_id];
}

static void
set_sock_buf_page_empty(IC_THREAD_CONNECTION *thd_conn)
{
  thd_conn->first_received_message= NULL;
  thd_conn->last_received_message= NULL;
}

static IC_SOCK_BUF_PAGE*
get_thread_messages(IC_APID_CONNECTION *ext_apid_conn, glong wait_time)
{
  GTimeVal stop_timer;
  IC_INT_APID_CONNECTION *apid_conn= (IC_INT_APID_CONNECTION*)ext_apid_conn;
  IC_THREAD_CONNECTION *thd_conn= apid_conn->thread_conn;
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

  word1= message_ptr[0];
  if ((word1 & 1) != ic_glob_byte_order)
  {
    /*
      Sender and receiver use different byte order, switch byte order of
      all words involved in the message.
    */
    ic_swap_endian_word(word1);
    message_size= get_message_size(word1);
    for (i= 1; i < message_size; i++)
      ic_swap_endian_word(message_ptr[i]);
  }
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
  /* Is this + 1 really correct, TODO */
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
  message_size= get_message_size(word1);
  for (i= 0; i < message_size; i++)
    chksum^= message_ptr[i];
  if (chksum)
    return IC_ERROR_MESSAGE_CHECKSUM;
  return 0;
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

/*
  This is the method where we wait for messages to be executed by NDB and
  our return messages to arrive for processing. It is executed in the 
  user thread upon a call to the iClaustron Data API which requires a
  message to be received from another node.
*/
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

/*
  RECEIVE THREAD MODULE:
  ----------------------
  This module contains the code that handles the receiver threads which takes
  care of reading from the socket and directing the NDB message to the proper
  user thread.
*/

/* This functions starts a receive thread */
static int start_receive_thread(IC_INT_APID_GLOBAL *apid_global);
/* This is the main function of the receive thread */
static gpointer run_receive_thread(void *data);

/*
  Messages are linked using a link message anchor which is a data type
  only used in the receive thread module.
*/
struct link_message_anchors
{
  IC_SOCK_BUF_PAGE *first_ndb_message_page;
  IC_SOCK_BUF_PAGE *last_ndb_message_page;
};
typedef struct link_message_anchors LINK_MESSAGE_ANCHORS;
/*
  The receiver thread is responsible to receive messages from other nodes
  using the NDB Protocol and send it on to the application threads. The
  ndb_receive_node function takes care of receiving messages from one
  node and packaging it in a format that can be received by application
  threads.
  The post_ndb_messages function ensures that the messages are delivered to
  the application threads.
  The read_message_early is a support method to read the first part of the
  NDB Protocol.
  The receiver thread uses various variants of polling (different variants
  dependent on OS, implemented in IC_POLL_SET object). The two methods
  get_first_rec_node and get_next_rec_node ensures that we get the nodes
  which have data waiting.
*/
static int ndb_receive_node(IC_NDB_RECEIVE_STATE *rec_state,
                            IC_RECEIVE_NODE_CONNECTION *rec_node,
                            int *min_hash_index,
                            int *max_hash_index,
                            LINK_MESSAGE_ANCHORS *message_anchors,
                            IC_THREAD_CONNECTION **thd_conn);
static void post_ndb_messages(LINK_MESSAGE_ANCHORS *ndb_message_anchors,
                              IC_THREAD_CONNECTION **thd_conn,
                              int min_hash_index,
                              int max_hash_index);
static void read_message_early(gchar *read_ptr, guint32 *message_size,
                               guint32 *receiver_module_id);
static IC_RECEIVE_NODE_CONNECTION*
get_first_rec_node(IC_NDB_RECEIVE_STATE *rec_state);
static IC_RECEIVE_NODE_CONNECTION*
get_next_rec_node(IC_NDB_RECEIVE_STATE *rec_state);
/*
  The remaining methods are there to handle additions and removals of node
  connections from the receiver thread. Currently we don't have any
  adaptiveness to this change but the idea is to make this part adapt to
  the load on the node. There is also a method used when a node connection
  have been broken.
*/
static int handle_node_error(int error,
                             LINK_MESSAGE_ANCHORS *message_anchors,
                             IC_THREAD_CONNECTION **thd_conn);
static void add_node_receive_thread(IC_NDB_RECEIVE_STATE *rec_state);
static void rem_node_receive_thread(IC_NDB_RECEIVE_STATE *rec_state);
static void check_for_reorg_receive_threads(IC_NDB_RECEIVE_STATE *rec_state);
static void check_send_buffers(IC_NDB_RECEIVE_STATE *rec_state);

static int
handle_node_error(int error,
                  LINK_MESSAGE_ANCHORS *message_anchors,
                  IC_THREAD_CONNECTION **thd_conn)
{
  int ret_code= 0;
  (void) message_anchors;
  (void) thd_conn;
  if (error == IC_ERROR_MEM_ALLOC)
  {
    /*
      Will be very hard to continue in a low-memory situation. In the
      future we might add some code to get rid of memory not badly
      needed but for now we simply stop this node entirely in a
      controlled manner.
    */
    ret_code= 1;
  }
  else
  {
    /*
      Any other fault happening will be treated as a close down of the
      socket connection and allow us to continue operating in a normal
      manner. TODO: Close down logic needed here.
    */
    ret_code= 0;
  }
  return ret_code;
}

static void
add_node_receive_thread(IC_NDB_RECEIVE_STATE *rec_state)
{
  IC_POLL_SET *poll_set= rec_state->poll_set;
  IC_CONNECTION *conn;
  IC_SEND_NODE_CONNECTION *send_node_conn= rec_state->first_add_node;
  IC_SEND_NODE_CONNECTION *next_send_node_conn;
  int conn_fd, error;

  while (send_node_conn)
  {
    g_mutex_lock(send_node_conn->mutex);
    /* move to next node to add */
    next_send_node_conn= send_node_conn->next_add_node;
    send_node_conn->next_add_node= NULL;
    if (send_node_conn->node_up && !send_node_conn->stop_ordered)
    {
      conn= send_node_conn->conn;
      conn_fd= conn->conn_op.ic_get_fd(conn);
      if ((error= poll_set->poll_ops.ic_poll_set_add_connection(poll_set,
                                             conn_fd,
                                             &send_node_conn->rec_node)))
      {
        send_node_conn->node_up= FALSE;
        node_failure_handling(send_node_conn);
      }
      else
      {
        /* Add node to list of send node connections on this receive thread */
        send_node_conn->next_send_node= rec_state->first_send_node;
        rec_state->first_send_node= send_node_conn;
      }
    }
    send_node_conn= next_send_node_conn;
    g_mutex_unlock(send_node_conn->mutex);
  }
}

static void
rem_node_receive_thread(IC_NDB_RECEIVE_STATE *rec_state)
{
  IC_POLL_SET *poll_set= rec_state->poll_set;
  IC_CONNECTION *conn;
  IC_SEND_NODE_CONNECTION *send_node_conn= rec_state->first_rem_node;
  int conn_fd, error;

  while (send_node_conn)
  {
    g_mutex_lock(send_node_conn->mutex);
    send_node_conn= send_node_conn->next_rem_node;
    send_node_conn->next_rem_node= NULL;
    if (send_node_conn->node_up && !send_node_conn->stop_ordered)
    {
      conn= send_node_conn->conn;
      conn_fd= conn->conn_op.ic_get_fd(conn);
      if ((error= poll_set->poll_ops.ic_poll_set_remove_connection(poll_set,
                                                    conn_fd)))
      {
        send_node_conn->node_up= FALSE;
        node_failure_handling(send_node_conn);
      }
    }
    g_mutex_unlock(send_node_conn->mutex);
  }
}

static void
check_for_reorg_receive_threads(IC_NDB_RECEIVE_STATE *rec_state)
{
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
  add_node_receive_thread(rec_state);
  rem_node_receive_thread(rec_state);
  rec_state->first_add_node= NULL;
  rec_state->first_rem_node= NULL;
  g_mutex_unlock(rec_state->mutex);
}

static void
check_send_buffers(IC_NDB_RECEIVE_STATE *rec_state)
{
  IC_SEND_NODE_CONNECTION *send_node_conn= rec_state->first_send_node;

  while (send_node_conn)
    send_node_conn= adaptive_send_handling(send_node_conn);
}

static IC_RECEIVE_NODE_CONNECTION*
get_first_rec_node(IC_NDB_RECEIVE_STATE *rec_state)
{
  IC_POLL_SET *poll_set= rec_state->poll_set;
  const IC_POLL_CONNECTION *poll_conn;
  int ret_code;

  if ((ret_code= poll_set->poll_ops.ic_check_poll_set(poll_set, (int)10)) ||
      (!(poll_conn= poll_set->poll_ops.ic_get_next_connection(poll_set))))
    return NULL;
  return poll_conn->user_obj;
}

static IC_RECEIVE_NODE_CONNECTION*
get_next_rec_node(IC_NDB_RECEIVE_STATE *rec_state)
{
  IC_POLL_SET *poll_set= rec_state->poll_set;
  const IC_POLL_CONNECTION *poll_conn;

  if (!(poll_conn= poll_set->poll_ops.ic_get_next_connection(poll_set)))
    return NULL;
  return poll_conn->user_obj;
}

static void
read_message_early(gchar *read_ptr, guint32 *message_size,
                   guint32 *receiver_module_id)
{
  guint32 *message_ptr= (guint32*)read_ptr;
  guint32 word1, word3;

  word1= message_ptr[0];
  word3= message_ptr[2];
  if ((word1 & 1) != ic_glob_byte_order)
  {
    /*
      The sender and receiver use a different endian, we need to swap the
      endian before reading the values from the message received.
    */
    ic_swap_endian_word(word1);
    ic_swap_endian_word(word3);
  }
  *receiver_module_id= word3 >> 16;
  *message_size= get_message_size(word1);
}

/*
  We get a linked list of IC_SOCK_BUF_PAGE which each contain a reference
  to a list of IC_NDB_MESSAGE object. We will ensure that these messages are
  posted to the proper thread. They will be put in a linked list of NDB
  messages waiting to be executed by this application thread.
*/
static void
post_ndb_messages(LINK_MESSAGE_ANCHORS *ndb_message_anchors,
                  IC_THREAD_CONNECTION **thd_conn,
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

  memset(&num_sent[0], 0,
         sizeof(guint32)*(IC_MAX_THREAD_CONNECTIONS/NUM_THREAD_LISTS));
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
      loc_thd_conn= thd_conn[receiver_module_id];
      temp= num_sent[send_index];
      if (!temp)
        g_mutex_lock(loc_thd_conn->mutex);
      num_sent[send_index]= temp + 1;
      /* Link it into list on this thread connection object */
      first_message= loc_thd_conn->first_received_message;
      ndb_message_page->next_sock_buf_page= NULL;
      if (first_message)
      {
        /* Link message into the queue last */
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
        if (thd_conn[j]->thread_wait_cond)
        {
          thd_conn[j]->thread_wait_cond= FALSE;
          signal_flag= TRUE;
        }
        message_opaque= (IC_NDB_MESSAGE_OPAQUE_AREA*)
          &thd_conn[module_id]->last_received_message->opaque_area[0];
        message_opaque->ref_count_releases= num_sent[j];
        g_mutex_unlock(thd_conn[j]->mutex);
        if (signal_flag)
        {
          /* Thread is waiting for our wake-up signal, wake it up */
          g_cond_signal(thd_conn[j]->cond);
          signal_flag= FALSE;
        }
        num_sent[j]= 0;
      }
    }
  }
  return;
}

#define MAX_NDB_RECEIVE_LOOPS 16
static int
ndb_receive_node(IC_NDB_RECEIVE_STATE *rec_state,
                 IC_RECEIVE_NODE_CONNECTION *rec_node,
                 int *min_hash_index,
                 int *max_hash_index,
                 LINK_MESSAGE_ANCHORS *message_anchors,
                 IC_THREAD_CONNECTION **thd_conn)
{
  IC_CONNECTION *conn= rec_node->conn;
  IC_SOCK_BUF *rec_buf_pool= rec_state->rec_buf_pool;
  IC_SOCK_BUF *message_pool= rec_state->message_pool;
  guint32 read_size= rec_node->read_size;
  guint32 real_read_size;
  gboolean read_header_flag= rec_node->read_header_flag;
  gchar *read_ptr;
  guint32 page_size= rec_buf_pool->page_size;
  guint32 start_size;
  guint32 message_size= 0;
  guint32 receiver_module_id= IC_MAX_THREAD_CONNECTIONS;
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
        ndb_message_opaque->version_num= 0; /* Define value although unused */
        ref_count= buf_page->ref_count;
        hash_index= receiver_module_id & (NUM_THREAD_LISTS - 1);
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
  if (ret_code)
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
  }
end:
  *min_hash_index= loc_min_hash_index;
  *max_hash_index= loc_max_hash_index;
  return ret_code;
mem_pool_error:
  ret_code= IC_ERROR_MEM_ALLOC;
  goto end;
}

static gpointer
run_receive_thread(void *data)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *rec_tp= thread_state->ic_get_threadpool(thread_state);
  IC_NDB_RECEIVE_STATE *rec_state= (IC_NDB_RECEIVE_STATE*)
    rec_tp->ts_ops.ic_thread_get_object(thread_state);
  LINK_MESSAGE_ANCHORS message_anchors[NUM_THREAD_LISTS];
  int min_hash_index= NUM_THREAD_LISTS;
  int max_hash_index= (int)-1;
  int ret_code;
  IC_SOCK_BUF_PAGE *buf_page, *ndb_message;
  IC_INT_APID_GLOBAL *apid_global= rec_state->apid_global;
  IC_THREAD_CONNECTION **thd_conn= apid_global->grid_comm->thread_conn_array;
  IC_RECEIVE_NODE_CONNECTION *rec_node;

  rec_tp->ts_ops.ic_thread_started(thread_state);
  memset(message_anchors, 0,
         sizeof(LINK_MESSAGE_ANCHORS)*NUM_THREAD_LISTS);

  /* Flag start-up done and wait for start order */
  rec_tp->ts_ops.ic_thread_startup_done(thread_state);

  while (!rec_tp->ts_ops.ic_thread_get_stop_flag(thread_state))
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
                              &message_anchors[0],
                              thd_conn))
          goto end;
      }
      else
      {
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
      }
      rec_node= get_next_rec_node(rec_state);
    }
    check_for_reorg_receive_threads(rec_state);
    check_send_buffers(rec_state);
  }

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
  rec_tp->ts_ops.ic_thread_stops(thread_state);
  return NULL;
}

static int
start_receive_thread(IC_INT_APID_GLOBAL *apid_global)
{
  IC_THREADPOOL_STATE *tp_state= apid_global->rec_thread_pool;
  IC_NDB_RECEIVE_STATE *rec_state;
  int ret_code= IC_ERROR_MEM_ALLOC;

  if (!(rec_state=
         (IC_NDB_RECEIVE_STATE*)ic_calloc(sizeof(IC_NDB_RECEIVE_STATE))))
    return ret_code;
  if (!(rec_state->mutex= g_mutex_new()))
    goto error;
  if (!(rec_state->cond= g_cond_new()))
    goto error;
  rec_state->apid_global= apid_global;
  rec_state->rec_buf_pool= apid_global->send_buf_pool;
  rec_state->message_pool= apid_global->ndb_message_pool;
  if ((!(rec_state->poll_set= ic_create_poll_set())) ||
      (ret_code= tp_state->tp_ops.ic_threadpool_start_thread(
                        tp_state,
                        &rec_state->thread_id,
                        run_receive_thread,
                        (gpointer)rec_state,
                        IC_MEDIUM_STACK_SIZE,
                        TRUE)))
    goto error;
  /* Wait for receive thread to complete start-up */
  g_mutex_lock(apid_global->mutex);
  apid_global->receive_threads[apid_global->num_receive_threads]= rec_state;
  apid_global->num_receive_threads++;
  g_mutex_unlock(apid_global->mutex);
  return 0;
error:
  if (rec_state->poll_set)
    rec_state->poll_set->poll_ops.ic_free_poll_set(rec_state->poll_set);
  if (rec_state->mutex)
    g_mutex_free(rec_state->mutex);
  if (rec_state->cond)
    g_cond_free(rec_state->cond);
  ic_free((void*)rec_state);
  return ret_code;
}

/*
  MODULE: iClaustron Initialise and End Functions
  -----------------------------------------------
  This module contains a number of common function used by most Data API
  programs for program options and various common start-up code.

  Every iClaustron program should start by calling ic_start_program
  before using any of the iClaustron functionality and after
  completing using the iClaustron API one should call ic_end().

  ic_start_program will define automatically a set of debug options
  for the program which are common to all iClaustron programs. The
  program can also supply a set of options unique to this particular
  program. In addition a start text should be provided.

  So for most iClaustron programs this means calling these functions
  at the start of the main function and at the end of the main
  function.
*/
GOptionEntry ic_apid_entries[] = 
{
  { "cs_connectstring", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_cs_connectstring,
    "Connect string to Cluster Servers", NULL},
  { "cs_hostname", 0, 0, G_OPTION_ARG_STRING,
     &ic_glob_cs_server_name,
    "Set Server Hostname of Cluster Server", NULL},
  { "cs_port", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_cs_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "node_id", 0, 0, G_OPTION_ARG_INT,
    &ic_glob_node_id,
    "Node id of file server in all clusters", NULL},
  { "data_dir", 0, 0, G_OPTION_ARG_FILENAME,
    &ic_glob_data_path,
    "Sets path to data directory, config files in subdirectory config", NULL},
  { "num_threads", 0, 0, G_OPTION_ARG_INT,
    &ic_glob_num_threads,
    "Number of threads executing in process", NULL},
  { "use_iclaustron_cluster_server", 0, 0, G_OPTION_ARG_INT,
     &ic_glob_use_iclaustron_cluster_server,
    "Use of iClaustron Cluster Server (default) or NDB mgm server", NULL},
  { "daemonize", 0, 0, G_OPTION_ARG_INT,
     &ic_glob_daemonize,
    "Daemonize program", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static void
apid_kill_handler(void *param)
{
  IC_INT_APID_GLOBAL *apid_global= (IC_INT_APID_GLOBAL*)param;
  apid_global->apid_global_ops.ic_set_stop_flag((IC_APID_GLOBAL*)apid_global);
}

int
ic_start_apid_program(IC_THREADPOOL_STATE **tp_state,
                      gchar **err_str,
                      gchar *error_buf,
                      IC_APID_GLOBAL **apid_global,
                      IC_API_CONFIG_SERVER **apic,
                      gboolean daemonize)
{
  IC_CONNECT_STRING conn_str;
  IC_API_CLUSTER_CONNECTION api_cluster_conn;
  int ret_code;

  if (!(*tp_state=
          ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE, FALSE)))
    return IC_ERROR_MEM_ALLOC;
  if (daemonize)
  {
    if ((ret_code= ic_daemonize("/dev/null")))
      return ret_code;
  }
  if ((ret_code= ic_parse_connectstring(ic_glob_cs_connectstring,
                                        &conn_str,
                                        ic_glob_cs_server_name,
                                        ic_glob_cs_server_port)))
    return ret_code;
  *err_str= error_buf;
  *apic= ic_get_configuration(&api_cluster_conn,
                              &ic_glob_config_dir,
                              ic_glob_node_id,
                              conn_str.num_cs_servers,
                              conn_str.cs_hosts,
                              conn_str.cs_ports,
                              ic_glob_use_iclaustron_cluster_server,
                              &ret_code,
                              err_str);
  conn_str.mc_ptr->mc_ops.ic_mc_free(conn_str.mc_ptr);
  if (!apic)
    return ret_code;
  
  ic_set_die_handler(apid_kill_handler, apid_global);
  ic_set_sig_error_handler(NULL, NULL);
  if (!(*apid_global= ic_create_apid_global(*apic,
                                            FALSE,
                                             &ret_code,
                                             err_str)))
    return ret_code;
  *err_str= NULL;
  return 0;
}

void
ic_stop_apid_program(int ret_code,
                     gchar *err_str,
                     IC_APID_GLOBAL *apid_global,
                     IC_API_CONFIG_SERVER *apic)
{
  if (err_str)
    ic_printf("%s", err_str);
  if (ret_code)
    ic_print_error(ret_code);
  ic_set_die_handler(NULL, NULL);
  if (apid_global)
    apid_global->apid_global_ops.ic_free_apid_global(apid_global);
  if (apic)
    apic->api_op.ic_free_config(apic);
  ic_end();
}

static gpointer
run_server_thread(gpointer data)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state= thread_state->ic_get_threadpool(thread_state);
  IC_APID_GLOBAL *apid_global= (IC_APID_GLOBAL*)
    tp_state->ts_ops.ic_thread_get_object(thread_state);
  IC_APID_CONNECTION *apid_conn;
  gboolean stop_flag;
  int ret_code;
  IC_RUN_APID_THREAD_FUNC apid_func;
  DEBUG_ENTRY("run_server_thread");

  apid_func= apid_global->apid_global_ops.ic_get_thread_func(apid_global);
  tp_state->ts_ops.ic_thread_started(thread_state);
  apid_global->apid_global_ops.ic_add_user_thread(apid_global);
  stop_flag= apid_global->apid_global_ops.ic_get_stop_flag(apid_global);
  if (stop_flag)
    goto error;
  /*
    Now start-up has completed and at this point in time we have connections
    to all clusters already set-up. So all we need to do now is start a local
    IC_APID_CONNECTION and start using it based on input from users
  */
  if (!(apid_conn= ic_create_apid_connection(apid_global,
                                             apid_global->cluster_bitmap)))
    goto error;
  if (tp_state->ts_ops.ic_thread_startup_done(thread_state))
    goto error;
  /*
     We now have a local Data API connection and we are ready to issue
     file system transactions to keep our local cache consistent with the
     global NDB file system
  */
  ret_code= apid_func(apid_global, thread_state);
  if (ret_code)
    ic_print_error(ret_code);
error:
  apid_global->apid_global_ops.ic_remove_user_thread(apid_global);
  tp_state->ts_ops.ic_thread_stops(thread_state);
  DEBUG_RETURN(NULL);
}

static int
start_server_threads(IC_APID_GLOBAL *apid_global,
                     IC_THREADPOOL_STATE *tp_state,
                     guint32 *thread_id)
{
  return tp_state->tp_ops.ic_threadpool_start_thread(
                            tp_state,
                            thread_id,
                            run_server_thread,
                            (gpointer)apid_global,
                            IC_MEDIUM_STACK_SIZE,
                            TRUE);
}

int
ic_run_apid_program(IC_APID_GLOBAL *apid_global,
                    IC_THREADPOOL_STATE *tp_state,
                    IC_RUN_APID_THREAD_FUNC apid_func,
                    gchar **err_str)
{
  int error= 0;
  guint32 i, thread_id;
  DEBUG_ENTRY("ic_run_apid_program");

  *err_str= NULL;
  ic_printf("Ready to start server threads");
  apid_global->apid_global_ops.ic_set_thread_func(apid_global, apid_func);
  for (i= 0; i < ic_glob_num_threads; i++)
  {
    if ((error= start_server_threads(apid_global,
                                     tp_state,
                                     &thread_id)))
    {
      apid_global->apid_global_ops.ic_set_stop_flag(apid_global);
      break;
    }
    tp_state->tp_ops.ic_threadpool_run_thread(tp_state, thread_id);
  }
  while (1)
  {
    if (apid_global->apid_global_ops.ic_get_num_user_threads(apid_global) == 0)
    {
      break;
    }
    apid_global->apid_global_ops.ic_cond_wait(apid_global);
    tp_state->tp_ops.ic_threadpool_check_threads(tp_state);
  }
  tp_state->tp_ops.ic_threadpool_check_threads(tp_state);
  DEBUG_RETURN(error);
}

static int ic_init();

/* Base directory group */
static GOptionEntry basedir_entries[] =
{
  { "basedir", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_base_path,
    "Sets path to binaries controlled by this program", NULL},
  { "iclaustron_version", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_version_path,
    "Version string to find iClaustron binaries used by this program, default "
    IC_VERSION_STR, NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static gchar *help_basedir= "Groupl of flags to set base directory";
static gchar *basedir_description= "\
Basedir Flags\n\
-------------\n\
These flags are used to point out the directory where the iClaustron\n\
programs expects to find the binaries. Many iClaustron programs make use\n\
of scripts for some advanced information on processes and system.\n\
\n\
iClaustron expects an organisation where basedir is the directory where\n\
all binaries are found under. Under the basedir there can be many\n\
directories. These represents different versions of iClaustron and\n\
MySQL. So e.g. iclaustron-0.0.1 is the directory where the binaries\n\
for this version is expected to be placed. The binaries are thus in\n\
this case placed in basedir/iclaustron-0.0.1/bin. Only the iClaustron\n\
version can be set since iClaustron only call iClaustron scripts\n\
The only MySQL binaries used are the ndbmtd and mysqld and these\n\
are always called from ic_pcntrld (the iClaustron process controller).\n\
\n\
The iClaustron process controller reuses the basedir as well for all\n\
processes started by the process controller. However the version is\n\
provided by the client (mostly the cluster manager which gets the\n\
version from the commands sent to the cluster manager).\n\
";

/* Debug group */
static GOptionEntry debug_entries[] = 
{
  { "debug_level", 0, 0, G_OPTION_ARG_INT, &glob_debug,
    "Set Debug Level", NULL},
  { "debug_file", 0, 0, G_OPTION_ARG_STRING, &glob_debug_file,
    "Set Debug File", NULL},
  { "debug_screen", 0, 0, G_OPTION_ARG_INT, &glob_debug_screen,
    "Flag whether to print debug info to stdout", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static gchar *help_debug= "Group of flags to set-up debugging";

static gchar *debug_description= "\
Debug Flags\n\
-----------\n\
Group of flags to set-up debugging level and where to pipe debug output \n\
Debug level is actually several levels, one can decide to debug a certain\n\
area at a time. Each area has a bit in debug level. So if one wants to\n\
debug the communication area one sets debug_level to 1 (set bit 0). By\n\
setting several bits it is possible to debug several areas at once.\n\
\n\
Current levels defined: \n\
  Level 0 (= 1): Communication debugging\n\
  Level 1 (= 2): Entry into functions debugging\n\
  Level 2 (= 4): Configuration debugging\n\
  Level 3 (= 8): Debugging specific to the currently executing program\n\
  Level 4 (=16): Debugging of threads\n\
";
static int use_config_vars;

int
ic_start_program(int argc, gchar *argv[], GOptionEntry entries[],
                 GOptionEntry add_entries[],
                 const gchar *program_name,
                 gchar *start_text,
                 gboolean use_config)
{
  int ret_code= 1;
  GError *error= NULL;
  GOptionGroup *debug_group;
  GOptionGroup *basedir_group;
  GOptionContext *context;

  ic_printf("Starting %s program", program_name);
  ic_glob_byte_order= ic_byte_order();
  use_config_vars= use_config; /* TRUE for Unit test programs */
  context= g_option_context_new(start_text);
  if (!context)
    goto mem_error;
  /* Read command options */
  g_option_context_add_main_entries(context, entries, NULL);
  if (add_entries)
    g_option_context_add_main_entries(context, add_entries, NULL);
  basedir_group= g_option_group_new("basedir", basedir_description,
                                     help_basedir, NULL, NULL);
  if (!basedir_group)
    goto mem_error;
  debug_group= g_option_group_new("debug", debug_description,
                                  help_debug, NULL, NULL);
  if (!debug_group)
    goto mem_error;
  g_option_group_add_entries(debug_group, debug_entries);
  g_option_context_add_group(context, debug_group);
  g_option_group_add_entries(basedir_group, basedir_entries);
  g_option_context_add_group(context, basedir_group);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);
  if ((ret_code= ic_init()))
    return ret_code;
  if ((ret_code= ic_set_config_dir(&ic_glob_config_dir, ic_glob_data_path)) ||
      (ret_code= ic_set_base_dir(&ic_glob_base_dir, ic_glob_base_path)) ||
      (ret_code= ic_set_binary_dir(&ic_glob_binary_dir,
                                   ic_glob_base_path,
                                   IC_VERSION_STR)))
    return ret_code;
  ic_set_port_binary_dir(ic_glob_binary_dir.str);
  return 0;

parse_error:
  ic_printf("No such program option: %s", error->message);
  goto error;
mem_error:
  ic_printf("Memory allocation error");
  goto error;
error:
  return ret_code;
}

static int
ic_init()
{
  int ret_value;
  DEBUG_OPEN;
  DEBUG_ENTRY("ic_init");

  if (!g_thread_supported())
    g_thread_init(NULL);
  ic_mem_init();
  ic_init_error_messages();
  if (use_config_vars)
  {
    if ((ret_value= ic_init_config_parameters()))
    {
      ic_end();
      DEBUG_RETURN(ret_value);
    }
  }
  if ((ret_value= ic_ssl_init()))
  {
    ic_end();
    DEBUG_RETURN(ret_value);
  }
  DEBUG_RETURN(0);
}

void ic_end()
{
  DEBUG_ENTRY("ic_end");

  if (use_config_vars)
    ic_destroy_conf_hash();
  ic_ssl_end();
  if (ic_glob_base_dir.str)
  {
    ic_free(ic_glob_base_dir.str);
    ic_glob_base_dir.str= NULL;
  }
  if (ic_glob_config_dir.str)
  {
    ic_free(ic_glob_config_dir.str);
    ic_glob_config_dir.str= NULL;
  }
  if (ic_glob_data_dir.str)
  {
    ic_free(ic_glob_data_dir.str);
    ic_glob_data_dir.str= NULL;
  }
  if (ic_glob_binary_dir.str)
  {
    ic_free(ic_glob_binary_dir.str);
    ic_glob_binary_dir.str= NULL;
  }
  DEBUG_CLOSE;
  ic_mem_end();
}

