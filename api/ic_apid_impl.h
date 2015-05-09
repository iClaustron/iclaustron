/* Copyright (C) 2009-2013 iClaustron AB

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

int ic_poll_messages(IC_APID_CONNECTION *apid_conn, glong wait_time);
int ic_send_messages(IC_APID_CONNECTION *apid_conn, gboolean force_send);

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
  /* Cluster id handled by this receiver thread */
  guint32 cluster_id;

  IC_MUTEX *mutex;
  IC_COND *cond;
};

#define IC_MESSAGE_FRAGMENT_FIRST_AND_LAST 0
#define IC_MESSAGE_FRAGMENT_FIRST_NOT_LAST 1
#define IC_MESSAGE_FRAGMENT_INTERMEDIATE 2
#define IC_MESSAGE_FRAGMENT_LAST 3

struct ic_ndb_message_opaque_area
{
  IC_SEND_NODE_CONNECTION *send_node_conn;
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
    TRANSID_AI and other messages NDB sends.
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
  guint32 cluster_id;
};

struct ic_thread_connection
{
  IC_SOCK_BUF_PAGE *first_received_message;
  IC_SOCK_BUF_PAGE *last_received_message;
  IC_INT_APID_CONNECTION *apid_conn;
  gboolean thread_wait_cond;
  IC_MUTEX *mutex;
  IC_COND *cond;
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
  guint32 my_node_id;
  guint32 thread_id;
  guint32 index;
  guint32 listen_port;
  gboolean started;
  gboolean stop_ordered;
  IC_MUTEX *mutex;
  IC_COND *cond;
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
  /* Send node connection for the node */
  IC_SEND_NODE_CONNECTION *send_node_conn;
};

struct ic_send_node_connection
{
  /* A pointer to the global struct */
  IC_INT_APID_GLOBAL *apid_global;
  /* Receive thread object if connected to one at the moment */
  IC_NDB_RECEIVE_STATE *rec_state;
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
  IC_MUTEX *mutex;
  /* Condition used to wake up send thread when it's needed */
  IC_COND *cond;

  /* Linked list of send buffers awaiting sending */
  IC_SOCK_BUF_PAGE *first_sbp;
  IC_SOCK_BUF_PAGE *last_sbp;
  /* List of buffers to release after sending completed */
  IC_SOCK_BUF_PAGE *release_sbp;

  /* How many bytes are in the send buffers awaiting sending */
  guint32 queued_bytes;
  /* When any thread is actively sending already this boolean is set */
  gboolean send_active;
  /**
   * A connection to a data server node goes through the following phases:
   * 1) A connection is established.
   * 2A) The connection has completed performing the login action of the
   *     NDB protocol. No other action is allowed during this phase.
   *     After completing the login the variable connection_up is set.
   * 2B) For a cluster server we convert the connection used to retrieve the
   *     configuration and use this as the connection. At conversion we
   *     set the connection_up variable to true.
   * 3)  After connection_up has been set the node is added to the heartbeat
   *     thread which will immediately start sending API_REGREQ signals to the
   *     data server node.
   * 4)  At reception of the first API_REGREQ message we will set the various
   *     state variables that comes along with the API_REGREQ message. We will
   *     also set the state node_up to true to indicate that we've now
   *     completed the process of setting up a connection to a data server,
   *     We cannot start sending other things than API_REGREQ until we've
   *     received an API_REGCONF such that we know the state of data server
   *     node.
   * 5)  Each time we receive an API_REGCONF we will update the start state
   *     of the data node as well as the start type if any start is ongoing.
   *     When the start state reaches IC_NDB_STARTED the data server node
   *     is ready to start receiving user transactions and meta data
   *     transactions.
   * 6)  Eventually the node can go through a controlled stop. In this case
   *     the API_REGCONF signal will carry information about that the node
   *     is stopping and which activities that can no longer be started.
   * 7)  At any point in time the data server can disconnect. This will be
   *     discovered as a dropped connection. Any time this happens we will
   *     call node_failure_handling that will set connection_up to false,
   *     node_up to false, node start state to IC_NDB_NOT_STARTED.
   *     NOTE: That we see a failed node doesn't absolutely say that the node
   *     actually failed, it could be simply that we for some reason lost the
   *     connection to the node. So when we later manage to reestablish the
   *     connection to the node it might directly report IC_NDB_STARTED on the
   *     very first API_REGCONF it sends.
   * 8)  We can also decide to disconnect, most likely because we are shutting
   *     down our node. It can also happen if for some reason we get a local
   *     problem.
   */
  /* Indicates if node is up, if not it's no use sending */
  gboolean node_up;
  /* Indicates if the connection is up. */
  gboolean connection_up;
  /**
   * Indicates if we successfully inserted it into poll set.
   * This also indicates that the node is currently attached to a receive
   * thread.
   */
  gboolean in_poll_set;
  /**
   * We save the state received in the last API_REGCONF from the data server
   * node, this will provide a lot of valuable information about the state of
   * the cluster and in particular of the data server nodes.
   */
  IC_API_REGCONF last_api_regconf;
  /*
    Indicates if the node is going through shutdown and we need to
    stop starting new activities
  */
  gboolean stopping;

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
     All those variables are protected by both the receive thread
     mutex and the send node connection mutex. So both mutexes are
     required to change the value, holding the IC_NDB_RECEIVE_STATE
     mutex is required to scan through the send node connections.
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

static gboolean check_node_started(IC_SEND_NODE_CONNECTION *send_node_conn);

struct ic_cluster_comm
{
  IC_SEND_NODE_CONNECTION **send_node_conn_array;
};

struct ic_grid_comm
{
  IC_CLUSTER_COMM **cluster_comm_array;
  IC_THREAD_CONNECTION **thread_conn_array;
};

static IC_NDB_RECEIVE_STATE*
get_first_receive_thread(IC_INT_APID_GLOBAL *apid_global,
                         guint32 cluster_id);

static IC_SEND_NODE_CONNECTION*
get_send_node_conn(IC_INT_APID_GLOBAL *apid_global,
                   guint32 cluster_id,
                   guint32 node_id);

static int
execute_meta_data_transaction(IC_INT_METADATA_TRANSACTION *md_trans);

/* Definitions used to handle NDB Protocol handling data structures. */

#define IC_MEM_BUF_SIZE 32768

#endif
