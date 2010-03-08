/* Copyright (C) 2009-2010 iClaustron AB

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

#ifndef IC_APID_IMPL_H
#define IC_APID_IMPL_H

typedef struct ic_cluster_comm IC_CLUSTER_COMM;
typedef struct ic_ndb_message_opaque_area IC_NDB_MESSAGE_OPAQUE_AREA;
typedef struct ic_ndb_message IC_NDB_MESSAGE;
typedef struct ic_temp_thread_connection IC_TEMP_THREAD_CONNECTION;
typedef struct ic_receive_node_connection IC_RECEIVE_NODE_CONNECTION;
typedef struct ic_message_error_object IC_MESSAGE_ERROR_OBJECT;

IC_APID_ERROR* ic_poll_messages(IC_APID_CONNECTION *apid_conn,
                                glong wait_time);
IC_APID_ERROR* ic_send_messages(IC_APID_CONNECTION *apid_conn,
                                gboolean force_send);

struct ic_message_error_object
{
  gchar *error_msg;
  int error_code;
  IC_ERROR_CATEGORY error_category;
  IC_ERROR_SEVERITY_LEVEL error_severity;
};

struct ic_ndb_receive_state
{
  /* Global data for Data Server API */
  IC_INT_APID_GLOBAL *apid_global;
  /* Local free pool of receive pages */
  IC_SOCK_BUF_PAGE *free_rec_pages;
  /* Local free pool of NDB messages */
  IC_SOCK_BUF_PAGE *free_ndb_messages;
  /* Reference to global pool of receive pages */
  IC_SOCK_BUF *rec_buf_pool;
  /* Reference to global pool of NDB messages */
  IC_SOCK_BUF *message_pool;
  /* Poll set used by this receive thread */
  IC_POLL_SET *poll_set;
  /*
    Linked lists of connections to other nodes which should be added or
    removed to/from this receive thread handling this NDB receive state.
    Both those lists are protected the mutex on this record.
  */
  IC_SEND_NODE_CONNECTION *first_add_node;
  IC_SEND_NODE_CONNECTION *first_rem_node;
  IC_SEND_NODE_CONNECTION *first_send_node;
  /* 
    Statistical info to track usage of this socket to enable proper
    configuration of threads to handle NDB Protocol.
    Not created yet.
  */
  /* Thread id in receive thread pool of receiver thread */
  guint32 thread_id;

  GMutex *mutex;
  GCond *cond;
};

#define IC_MESSAGE_FRAGMENT_FIRST_AND_LAST 0
#define IC_MESSAGE_FRAGMENT_FIRST_NOT_LAST 1
#define IC_MESSAGE_FRAGMENT_INTERMEDIATE 2
#define IC_MESSAGE_FRAGMENT_LAST 3

struct ic_ndb_message_opaque_area
{
  guint32 sender_node_id;
  guint32 receiver_node_id;
  guint32 cluster_id;
  guint32 packed_message;
  guint32 version_num; /* Always 0 currently */
};

struct ic_ndb_message
{
  IC_INT_APID_CONNECTION *apid_conn;
  /*
    We keep pointers to the message buffer where the actual message data
    resides instead of copying. We also keep a pointer to the start of
    the message to make it easy to release the IC_NDB_MESSAGE object.
    We also need a pointer to the page from which this message is created.
    This is needed to ensure that we can efficiently release the page when
    all messages have been executed.
  */
  guint32 *segment_ptr[4];
  guint32 segment_size[4];

  /* Trace number and message number are mostly for debug information.  */
  guint32 message_number;
  guint32 trace_num;
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
  guint32 num_segments;
  guint32 message_priority;
  guint32 fragmentation_bits;

  /*
    The message id is the actual number indicating which message this is.
    A message number could indicate for example that this is a TCKEYCONF,
    TRANSID_AI and other messages NDB sends. We also keep track of the
    total message size mostly for debug purposes.
  */
  guint32 message_id;

  /*
    We need to keep track of Module id of both the sender and the receiver.
    In reality the address of the sender and the receiver is identified by
    the module id and the node id.
  */
  guint32 sender_module_id;
  guint32 receiver_module_id;
  guint32 sender_node_id;
  guint32 receiver_node_id;
};

struct ic_thread_connection
{
  IC_SOCK_BUF_PAGE *first_received_message;
  IC_SOCK_BUF_PAGE *last_received_message;
  IC_INT_APID_CONNECTION *apid_conn;
  gboolean thread_wait_cond;
  GMutex *mutex;
  GCond *cond;
};

struct ic_temp_thread_connection
{
  IC_SOCK_BUF_PAGE *first_received_message;
  IC_SOCK_BUF_PAGE *last_received_message;
  IC_SOCK_BUF_PAGE *last_long_received_message;
  guint32 num_messages_on_page;
};

struct ic_listen_server_thread
{
  IC_CONNECTION *conn;
  guint32 cluster_id;
  guint32 thread_id;
  guint32 index;
  guint32 listen_port;
  gboolean started;
  gboolean stop_ordered;
  GMutex *mutex;
  GCond *cond;
  GList *first_send_node_conn;
};


struct ic_receive_node_connection
{
  /* Reference to socket representation for this node */
  IC_CONNECTION *conn;
  /* The current page received into on this socket */
  IC_SOCK_BUF_PAGE *buf_page;
  /* How many bytes have been received into the current receive page */
  guint32 read_size;
  /* Have we received a full header of the NDB message in the receive page */
  gboolean read_header_flag;
  /* Cluster id of this connection */
  guint32 cluster_id;
  /* Node id of the node on the other end of the socket connection */
  guint32 other_node_id;
  /* Node id of myself on the socket connection */
  guint32 my_node_id;
};

struct ic_send_node_connection
{
  /* A pointer to the global struct */
  IC_INT_APID_GLOBAL *apid_global;
  /* Receive node object */
  IC_RECEIVE_NODE_CONNECTION rec_node;
  /* My hostname of the connection used by this thread */
  gchar *my_hostname;
  /* My port number of connection used by thread */
  gchar *my_port_number;
  /* Hostname on other side of the connection used by this thread */
  gchar *other_hostname;
  /* Port number of other side of connection used by thread */
  gchar *other_port_number;
  /*
     Allocated string memory where strings for my_hostname,
     other_hostname, my_port_number and other_port_number
     are
     else
       other_port_number= send_node_conn->other_port_number;stored.
     When dynamic ports are used we need to dynamic_port_number_to_release
     to remember which memory to release.
  */
  gchar *string_memory;
  gchar *dynamic_port_number_to_release;

  /* The connection object */
  IC_CONNECTION *conn;
  /* For server connections this is the link to the listen server thread */
  IC_LISTEN_SERVER_THREAD *listen_server_thread;
  /* The configuration for this connection */
  IC_SOCKET_LINK_CONFIG *link_config;

  /*
     Thread data for send thread, presence of thread_state set to
     non-NULL value also indicates thread has started.
  */
  IC_THREAD_STATE *thread_state;
  guint32 thread_id;
  /* Mutex protecting this struct */
  GMutex *mutex;
  /* Condition used to wake up send thread when it's needed */
  GCond *cond;

  /* Linked list of send buffers awaiting sending */
  IC_SOCK_BUF_PAGE *first_sbp;
  IC_SOCK_BUF_PAGE *last_sbp;
  /* List of buffers to release after sending completed */
  IC_SOCK_BUF_PAGE *release_sbp;

  /* How many bytes are in the send buffers awaiting sending */
  guint32 queued_bytes;
  /* When any thread is actively sending already this boolean is set */
  gboolean send_active;
  /* Indicates if node is up, if not it's no use sending */
  gboolean node_up;
  /* Indicates if the connection is up. */
  gboolean connection_up;

  /* Debug variable set when waking up send thread */
  gboolean send_thread_is_sending;
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
  guint32 other_node_id;
  /* The cluster id this connection is used in */
  guint32 cluster_id;
  /* The message id variable used for debugging purposes in the NDB Protocol */
  guint32 message_id;

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
  /*
     Next pointers for lists on IC_NDB_RECEIVE_STATE for add/remove
     this connection to the receive thread
     Both those variables are protected by both the receive thread
     mutex and the send node connection mutex.
  */
  IC_SEND_NODE_CONNECTION *next_add_node;
  IC_SEND_NODE_CONNECTION *next_rem_node;
  IC_SEND_NODE_CONNECTION *next_send_node;
  /*
    A pointer to the next node in the heartbeat thread linked list.
    This list is protected by the mutex on the IC_APID_GLOBAL object.
  */
  IC_SEND_NODE_CONNECTION *next_heartbeat_node;
  /* Array of timers for the last 16 sends */
  IC_TIMER last_send_timers[IC_MAX_SEND_TIMERS];
};

struct ic_cluster_comm
{
  IC_SEND_NODE_CONNECTION **send_node_conn_array;
};

struct ic_grid_comm
{
  IC_CLUSTER_COMM **cluster_comm_array;
  IC_THREAD_CONNECTION **thread_conn_array;
};

/* Definitions used to handle NDB Protocol handling data structures. */

#define IC_MEM_BUF_SIZE 32768

#endif
