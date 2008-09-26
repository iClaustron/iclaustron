/* Copyright (C) 2007 iClaustron AB

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

#ifndef IC_APID_H
#define IC_APID_H
#include <ic_comm.h>
#include <ic_common.h>
#include <ic_apic.h>

struct ic_ds_connection;

void ic_create_ds_connection(struct ic_ds_connection *conn);

struct ic_ds_operations
{
  int (*ic_set_up_ds_connection) (struct ic_ds_connection *ds_conn);
  int (*ic_close_ds_connection) (struct ic_ds_connection *ds_conn);
  int (*ic_is_conn_established) (struct ic_ds_connection *ds_conn,
                                 gboolean *is_established);
  int (*ic_authenticate_connection) (void *conn_obj);
};

struct ic_ds_connection
{
  guint32 dest_node_id;
  guint32 source_node_id;
  guint32 socket_group;
  struct ic_connection *conn_obj;
  struct ic_ds_operations operations;
};

typedef struct ic_ds_connection IC_DS_CONNECTION;
/*
  Definitions used to handle NDB Protocol handling data structures.
*/

#define IC_MEM_BUF_SIZE 32768

struct ic_ndb_receive_state
{
  IC_CONNECTION *conn;
  IC_SOCK_BUF *rec_buf_pool;
  IC_SOCK_BUF *message_pool;
  IC_SOCK_BUF_PAGE *buf_page;
  guint32 read_size;
  gboolean read_header_flag;
  guint32 cluster_id;
  guint32 node_id;
  /* 
    Statistical info to track usage of this socket to enable proper
    configuration of threads to handle NDB Protocol.
  */
  GMutex *ndb_rec_mutex;
};
typedef struct ic_ndb_receive_state IC_NDB_RECEIVE_STATE;

#define IC_MESSAGE_FRAGMENT_NONE 0
#define IC_MESSAGE_FRAGMENT_FIRST_NOT_LAST 1
#define IC_MESSAGE_FRAGMENT_INTERMEDIATE 2
#define IC_MESSAGE_FRAGMENT_LAST 3

struct ic_ndb_message_opaque_area
{
  guint32 message_offset;
  guint32 sender_node_id;
  guint32 receiver_node_id;
  guint32 num_users_to_release;
};
typedef struct ic_ndb_message_opaque_area IC_NDB_MESSAGE_OPAQUE_AREA

struct ic_ndb_message
{
  /*
    We keep pointers to the message buffer where the actual message data
    resides instead of copying. We also keep a pointer to the start of
    the message to make it easy to release the IC_NDB_MESSAGE object.
    We also need a pointer to the page from which this message is created.
    This is needed to ensure that we can efficiently release the page when
    all messages have been executed.
  */
  guint32 *short_data_ptr;
  guint32 *segment_ptr[3];
  guint32 *segment_size_ptr;

  IC_SOCK_BUF_PAGE *buf_page;

  /* Trace number and message id are mostly for debug information.  */
  guint32 message_id;
  guint8  trace_num;
  /*
     There can be up to 3 segments in a  message in addition to the always
     existing short data part which can have a maximum 25 words.
     There is a variable to keep track of number of segments, number of words
     in the short data part. In order to handle really large messages we
     use fragmentation bits to keep track of the message train that should
     be treated as one message.
     Messages also have priorities, currently there are only 2 really used,
     Priority A and Priority B.

     The offset to the short data is
     message_header[header_size]
     The offset to the segment sizes are
     message_header[header_size + short_data_size]
  */
  guint8 num_segments;
  guint8 short_data_size;
  guint8 fragmentation_bits;
  guint8 message_priority;
  guint8 header_size;

  /*
    The message number is the actual number indicating which message this is.
    A message number could indicate for example that this is a TCKEYCONF,
    TRANSID_AI and other messages NDB sends. We also keep track of the
    total message size mostly for debug purposes.
  */
  guint16 segment_offsets[3];
  guint16 message_number;
  guint16 tot_message_size;

  /*
    We need to keep track of Module id of both the sender and the receiver.
    In reality the address of the sender and the receiver is identified by
    the module id and the node id.
  */
  guint16 sender_module_id;
  guint16 receiver_module_id;
  guint16 sender_node_id;
  guint16 receiver_node_id;
};
typedef struct ic_ndb_message IC_NDB_MESSAGE;

struct ic_apid_connection;
struct ic_thread_connection
{
  IC_SOCK_BUF_PAGE *free_rec_pages;
  IC_SOCK_BUF_PAGE *free_ndb_messages;
  IC_SOCK_BUF_PAGE *first_received_message;
  IC_SOCK_BUF_PAGE *last_received_message;
  struct ic_apid_connection *apid_conn;
  gboolean thread_wait_cond;
  GMutex *mutex;
  GCond *cond;
};
typedef struct ic_thread_connection IC_THREAD_CONNECTION;

#define MAX_SEND_TIMERS 16
#define MAX_SENDS_TRACKED 8
#define MAX_SEND_SIZE 65535
#define MAX_SEND_BUFFERS 16
#define IC_MEMBUF_SIZE 32768

struct ic_send_node_connection
{
  /* The connection object */
  IC_CONNECTION *conn;
  /* The configuration for this connection */
  IC_SOCKET_LINK_CONFIG *link_config;

  /* Mutex protecting this struct */
  GMutex *mutex;
  /* Condition used to wake up send thread when it's needed */
  GCond *cond;

  /* Linked list of send buffers awaiting sending */
  IC_SOCK_BUF_PAGE *first_sbp;
  IC_SOCK_BUF_PAGE *last_sbp;

  /* How many bytes are in the send buffers awaiting sending */
  guint32 queued_bytes;
  /* When any thread is actively sending already this boolean is set */
  gboolean send_active;
  /* Indicates if node is up, if not it's no use sending */
  gboolean node_up;
  /* Indicates if the connection is up. */
  gboolean connection_up;

  /* Debug variable set when waking up send thread */
  gboolean starting_send_thread;
  /* Variable indicating when send thread has exited */
  gboolean send_thread_ended;
  /* Variable indicating send thread is awake and working */
  gboolean send_thread_active;
  /* Somebody ordered the node to stop */
  gboolean stop_ordered;
  /*
    The node id we have in this cluster. If this node id is equal to
    the receiving end, the link is a local connection.
  */
  guint32 my_node_id;
  /* The node id in the cluster of the receiving end */
  guint32 active_node_id;
  /* The cluster id this connection is used in */
  guint32 cluster_id;

  /* Timer set when the first buffer was linked and not sent */
  IC_TIMER first_buffered_timer;
  /* Timer for how long we want the maximum wait to be */
  IC_TIMER max_wait_in_nanos;
  /*
    num_waits keeps track of how many sends are currently already waiting
    max_num_waits keeps track of how many are currently allowed to wait
    at maximum, this is the current state of the adaptive send algorithm.
  */
  guint32 num_waits;
  guint32 max_num_waits;
  /* Number of statistical entries */
  guint32 num_stats;
  /* Sum of waits with current state */
  IC_TIMER tot_curr_wait_time;
  /* Sum of waits if we added one to the state */
  IC_TIMER tot_wait_time_plus_one;
  /* Index into timer array */
  guint32 last_send_timer_index;
  /* Array of timers for the last 16 sends */
  IC_TIMER last_send_timers[MAX_SEND_TIMERS];
};
typedef struct ic_send_node_connection IC_SEND_NODE_CONNECTION;

struct ic_cluster_comm
{
  IC_SEND_NODE_CONNECTION **send_node_conn_array;
};
typedef struct ic_cluster_comm IC_CLUSTER_COMM;

struct ic_grid_comm
{
  IC_CLUSTER_COMM **cluster_comm_array;
  IC_THREAD_CONNECTION **thread_conn_array;
};
typedef struct ic_grid_comm IC_GRID_COMM;

struct ic_apid_global
{
  IC_SOCK_BUF *mem_buf_pool;
  IC_SOCK_BUF *ndb_message_pool;
  IC_GRID_COMM *grid_comm;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_bitmap;
  GMutex *thread_id_mutex;
  GMutex *mutex;
  GCond *cond;
  gboolean stop_flag;
  guint32 num_threads_started;
};
typedef struct ic_apid_global IC_APID_GLOBAL;

struct ic_apid_connection;
struct ic_apid_connection_ops
{
  int (*ic_poll) (struct ic_apid_connection *apid_conn, glong wait_time);
  void (*ic_free) (struct ic_apid_connection *apid_conn);
};
typedef struct ic_apid_connection_ops IC_APID_CONNECTION_OPS;

struct ic_apid_connection
{
  IC_APID_CONNECTION_OPS apid_ops;
  IC_APID_GLOBAL *apid_global;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_id_bitmap;
  IC_THREAD_CONNECTION *thread_conn;
  guint32 thread_id;
};
typedef struct ic_apid_connection IC_APID_CONNECTION;

IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *apid_global,
                          IC_BITMAP *cluster_id_bitmap);

int ic_apid_global_connect(IC_APID_GLOBAL *apid_global);
int ic_apid_global_disconnect(IC_APID_GLOBAL *apid_global);

IC_APID_GLOBAL* ic_init_apid(IC_API_CONFIG_SERVER *apic);
void ic_end_apid(IC_APID_GLOBAL *apid_global);
#endif
