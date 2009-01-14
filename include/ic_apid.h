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
#include <ic_connection.h>
#include <ic_common.h>
#include <ic_apic.h>

/* Definitions used to handle NDB Protocol handling data structures. */

#define IC_MEM_BUF_SIZE 32768

/*
  This data structure contains the important information about the
  receive thread and its state. It contains free pools of receive
  pages containing signals and pools of containers to store NDB
  messages in. Both of these are transported to the user thread and
  will be released in the user thread to the global pool.
*/
struct ic_ndb_receive_state
{
  /* Local free pool of receive pages */
  IC_SOCK_BUF_PAGE *free_rec_pages;
  /* Local free pool of NDB messages */
  IC_SOCK_BUF_PAGE *free_ndb_messages;
  /* Reference to global pool of receive pages */
  IC_SOCK_BUF *rec_buf_pool;
  /* Reference to global pool of NDB messages */
  IC_SOCK_BUF *message_pool;
  /* 
    Statistical info to track usage of this socket to enable proper
    configuration of threads to handle NDB Protocol.
  */
  GMutex *mutex;
  GCond *cond;
  GThread *thread;
};
typedef struct ic_ndb_receive_state IC_NDB_RECEIVE_STATE;

#define IC_MESSAGE_FRAGMENT_NONE 0
#define IC_MESSAGE_FRAGMENT_FIRST_NOT_LAST 1
#define IC_MESSAGE_FRAGMENT_INTERMEDIATE 2
#define IC_MESSAGE_FRAGMENT_LAST 3

struct ic_message_error_object
{
  int error;
  gchar *error_string;
  int error_category;
  int error_severity;
};
typedef struct ic_message_error_object IC_MESSAGE_ERROR_OBJECT;

struct ic_ndb_message_opaque_area
{
  guint32 message_offset;
  guint32 sender_node_id;
  guint32 receiver_node_id;
  guint32 ref_count_releases;
  guint32 receiver_module_id;
  guint32 version_num; /* Always 0 currently */
};
typedef struct ic_ndb_message_opaque_area IC_NDB_MESSAGE_OPAQUE_AREA;

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
  guint32 *segment_ptr[4];
  guint32 segment_size[4];

  /* Trace number and message id are mostly for debug information.  */
  guint32 message_id;
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
    The message number is the actual number indicating which message this is.
    A message number could indicate for example that this is a TCKEYCONF,
    TRANSID_AI and other messages NDB sends. We also keep track of the
    total message size mostly for debug purposes.
  */

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
typedef struct ic_ndb_message IC_NDB_MESSAGE;

struct ic_apid_connection;
struct ic_thread_connection
{
  IC_SOCK_BUF_PAGE *first_received_message;
  IC_SOCK_BUF_PAGE *last_received_message;
  struct ic_apid_connection *apid_conn;
  gboolean thread_wait_cond;
  GMutex *mutex;
  GCond *cond;
};
typedef struct ic_thread_connection IC_THREAD_CONNECTION;

struct ic_listen_server_thread
{
  IC_CONNECTION *conn;
  guint32 cluster_id;
  guint32 thread_id;
  gboolean started;
  gboolean stop_ordered;
  GMutex *mutex;
  GCond *cond;
  IC_THREAD_STATE *thread_state;
  GList *first_send_node_conn;
};
typedef struct ic_listen_server_thread IC_LISTEN_SERVER_THREAD;

#define MAX_SEND_TIMERS 16
#define MAX_SENDS_TRACKED 8
#define MAX_SEND_SIZE 65535
#define MAX_SEND_BUFFERS 16
#define IC_MEMBUF_SIZE 32768

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
typedef struct ic_receive_node_connection IC_RECEIVE_NODE_CONNECTION;

struct ic_send_node_connection
{
  /* A pointer to the global struct */
  struct ic_apid_global *apid_global;
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
     are stored.
  */
  gchar *string_memory;
  /* The connection object */
  IC_CONNECTION *conn;
  /* For server connections this is the link to the listen server thread */
  IC_LISTEN_SERVER_THREAD *listen_server_thread;
  /* The configuration for this connection */
  IC_SOCKET_LINK_CONFIG *link_config;

  /* Thread data for send thread (id and state) */
  GThread *thread;
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

#define MAX_SERVER_PORTS_LISTEN 256
#define IC_MAX_RECEIVE_THREADS 64
struct ic_apid_global
{
  IC_SOCK_BUF *mem_buf_pool;
  IC_SOCK_BUF *ndb_message_pool;
  IC_GRID_COMM *grid_comm;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_bitmap;
  IC_THREADPOOL_STATE *rec_thread_pool;
  IC_THREADPOOL_STATE *send_thread_pool;
  GMutex *thread_id_mutex;
  GMutex *mutex;
  GCond *cond;
  gboolean stop_flag;
  guint32 num_user_threads_started;
  guint32 num_receive_threads;
  guint32 max_listen_server_threads;
  IC_LISTEN_SERVER_THREAD *listen_server_thread[MAX_SERVER_PORTS_LISTEN];
};
typedef struct ic_apid_global IC_APID_GLOBAL;

struct ic_transaction_hint
{
  guint32 cluster_id;
  guint32 node_id;
};
typedef struct ic_transaction_hint IC_TRANSACTION_HINT;

struct ic_transaction_obj
{
  guint32 transaction_id[2];
};
typedef struct ic_transaction_obj IC_TRANSACTION;

enum ic_field_type
{
  UINT32_TYPE= 0
};
typedef enum ic_field_type IC_FIELD_TYPE;

struct ic_field_def
{
  gchar *field_name;
  guint32 field_id;
  IC_FIELD_TYPE field_type;
};
typedef struct ic_field_def IC_FIELD_DEF;

enum ic_operation_type
{
  KEY_OPERATION= 0,
  SCAN_OPERATION= 1
};
typedef enum ic_operation_type IC_OPERATION_TYPE;

enum ic_apid_key_operation_type
{
  SIMPLE_KEY_READ= 0,
  KEY_READ= 1,
  EXCLUSIVE_KEY_READ= 2,
  KEY_READ_COMMITTED= 3,
  key_update= 4,
  key_delete= 5,
  key_write= 6,
  key_insert= 7,
  take_over_update= 8,
  take_over_delete= 9
};
typedef enum ic_apid_key_operation_type IC_APID_KEY_OPERATION_TYPE;

enum ic_apid_scan_operation_type
{
  SCAN_READ_COMMITTED= 0,
  SCAN_READ_EXCLUSIVE= 1,
  SCAN_HOLD_LOCK= 2,
  SCAN_CONSISTENT_READ= 3
};
typedef enum ic_apid_scan_operation_type IC_APID_SCAN_OPERATION_TYPE;

struct ic_key_operation
{
  guint32 operation_type;
  IC_APID_KEY_OPERATION_TYPE key_op_type;
  IC_TRANSACTION *transaction;
};
typedef struct ic_key_operation IC_KEY_OPERATION;

struct ic_scan_operation
{
  guint32 operation_type;
  IC_APID_SCAN_OPERATION_TYPE scan_op_type;
  IC_TRANSACTION *transaction;
};
typedef struct ic_scan_operation IC_SCAN_OPERATION;

enum ic_instruction_type
{
  INST_LOAD_CONST32= 0
};
typedef enum ic_instruction_type IC_INSTRUCTION_TYPE;

struct ic_instruction
{
  IC_INSTRUCTION_TYPE instr_type;
  guint32 source_register1;
  guint32 source_register2;
  guint32 dest_register;
};
typedef struct ic_instruction IC_INSTRUCTION;

struct ic_table_def
{
  gchar *table_name;
  guint32 table_id;
  guint32 num_fields;
};
typedef struct ic_table_def IC_TABLE_DEF;

struct ic_metadata_bind_ops
{
  int (*ic_table_bind) (IC_TABLE_DEF *table_def);
  int (*ic_field_bind) (IC_TABLE_DEF *table_def,
                        IC_FIELD_DEF *field_def);
};
typedef struct ic_metadata_bind_ops IC_METADATA_BIND_OPS;

#define TRANSLATION_ARRAY_SIZE 256
struct ic_translation_obj;
typedef struct ic_translation_obj IC_TRANSLATION_OBJ;
struct ic_translation_obj
{
  IC_HASHTABLE *hash_table;
};

struct ic_apid_transaction_ops
{
  /*
    ic_key_access is used for read, update, delete, insert and replace
    operations. For all operations the key_def sets up the value of the
    key fields (needs to be all of the fields in either a primary key
    or in a unique key. For reads the field_def defines the placeholders
    of the data to be read back. For updates, inserts and replace operations
    it's the values to set and for deletes this pointer must be NULL.

    table_def is a table definition which has been bound before this call.
    A table could here either be a real table or a unique index table.
    transaction_obj is a reference to the transaction to place this
    operation into.
    Finally apid_op contains information about the operation type.
  */
  int (*ic_key_access) (IC_TRANSACTION *transaction_obj,
                        IC_APID_KEY_OPERATION_TYPE apid_key_op_type,
                        IC_KEY_OPERATION **key_op,
                        IC_TABLE_DEF *table_def,
                        IC_FIELD_DEF **field_def,
                        IC_FIELD_DEF **key_def);
};
typedef struct ic_apid_transaction_ops IC_APID_TRANSACTION_OPS;

struct ic_apid_connection;
struct ic_apid_connection_ops
{
  int (*ic_start_transaction) (struct ic_apid_connection *apid_conn,
                               IC_TRANSACTION **transaction_obj,
                               IC_TRANSACTION_HINT *transaction_hint);
  int (*ic_join_transaction) (struct ic_apid_connection *apid_connection,
                              IC_TRANSACTION transaction_obj);
  int (*ic_poll) (struct ic_apid_connection *apid_conn, glong wait_time);
  void (*ic_free) (struct ic_apid_connection *apid_conn);
};
typedef struct ic_apid_connection_ops IC_APID_CONNECTION_OPS;

struct ic_apid_connection
{
  IC_APID_CONNECTION_OPS apid_conn_ops;
  IC_APID_TRANSACTION_OPS apid_trans_ops;
  IC_METADATA_BIND_OPS apid_metadata_ops;
  IC_APID_GLOBAL *apid_global;
  IC_TRANSLATION_OBJ *trans_bindings;
  IC_TRANSLATION_OBJ *op_bindings;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_id_bitmap;
  IC_THREAD_CONNECTION *thread_conn;
  guint32 thread_id;
};
typedef struct ic_apid_connection IC_APID_CONNECTION;

/*
  These two functions are used to connect/disconnect to/from the clusters
  we have in the configuration. The connect function performs both the
  initialisation of the Data API instance as well as setting up all the
  necessary threads and start up the connections to the other cluster nodes.

  Then before actually using this Data API instance it's a good idea to
  call ic_wait_first_node_connect which is waiting for at least one connection
  to be completed in a specified cluster (so this function only waits for
  one cluster, to wait for all clusters it's necessary to create a loop with
  calls to this function.

  Finally before actually starting to use the Data API it's also necessary to
  create one Data API connection. IMPORTANT: It's assumed that the user of
  this Data API connection ensures that it isn't from several threads at the
  same time. If it's used by several threads, each access must be protected
  by the user through a mutex.

  Once a Data API connection has been used the user can start using all the
  operations as defined by the ic_apid_connection_ops and all other parts
  of the Ds
*/
IC_APID_GLOBAL* ic_connect_apid_global(IC_API_CONFIG_SERVER *apic);
int ic_disconnect_apid_global(IC_APID_GLOBAL *apid_global);
int ic_wait_first_node_connect(IC_APID_GLOBAL *apid_global,
                               guint32 cluster_id);
IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *apid_global,
                          IC_BITMAP *cluster_id_bitmap);
#endif
