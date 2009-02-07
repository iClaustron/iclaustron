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

#ifndef IC_APID_H
#define IC_APID_H
#include <ic_connection.h>
#include <ic_common.h>
#include <ic_apic.h>

/* Definitions used to handle NDB Protocol handling data structures. */

#define IC_MEM_BUF_SIZE 32768

typedef struct ic_apid_global IC_APID_GLOBAL;
/*
  This data structure contains the important information about the
  receive thread and its state. It contains free pools of receive
  pages containing signals and pools of containers to store NDB
  messages in. Both of these are transported to the user thread and
  will be released in the user thread to the global pool.
*/
typedef struct ic_send_node_connection IC_SEND_NODE_CONNECTION;
struct ic_ndb_receive_state
{
  /* Global data for Data Server API */
  IC_APID_GLOBAL *apid_global;
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
  IC_APID_GLOBAL *apid_global;
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
     are stored.
  */
  gchar *string_memory;
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
  /*
     Next pointers for lists on IC_NDB_RECEIVE_STATE for add/remove
     this connection to the receive thread
     Both those variables are protected by both the receive thread
     mutex and the send node connection mutex.
  */
  IC_SEND_NODE_CONNECTION *next_add_node;
  IC_SEND_NODE_CONNECTION *next_rem_node;
  IC_SEND_NODE_CONNECTION *next_send_node;
  /* Array of timers for the last 16 sends */
  IC_TIMER last_send_timers[MAX_SEND_TIMERS];
};

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

#define IC_MAX_SERVER_PORTS_LISTEN 256
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
  IC_NDB_RECEIVE_STATE *receive_threads[IC_MAX_RECEIVE_THREADS];
  IC_LISTEN_SERVER_THREAD *listen_server_thread[IC_MAX_SERVER_PORTS_LISTEN];
};

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

/* For efficiency reasons the structs are part of the public API.  */

/*
  When performing a read operation we always supply a IC_READ_FIELD_BIND
  object. This object specifies the fields we want to read.
*/
typedef struct ic_read_field_def IC_READ_FIELD_DEF;
struct ic_read_field_bind
{
  /* Number of fields in bit_array and field_defs array */
  guint32 no_fields;
  /* If a buffer_ptr is provided a buffer_size also is required. */
  guint32 buffer_size;
  /* User supplied buffer to use for returned results. */
  gchar *buffer_ptr;
  /*
    Reference to the buffer where results are returned. If the user
    supplied a buffer it will be the same buffer as he supplied,
    otherwise a buffer allocated by the API. This buffer is released
    when operation is released or transaction ended. So reference
    to the buffer is only safe until this call to the API. If the user
    supplied the buffer the reference to the buffer is safe until
    calling the API to get the next record of a scan. For key reads the
    buffer is stable until the user decides otherwise.
  */
  gchar *returned_buffer_ptr;
  /*
    The bit_array field makes it possible to reuse this structure for many
    different reads on the same table. We will only read those fields that
    have their bit set in the bit_array.
  */
  guint8 *bit_array;
  /*
    An array of pointers to IC_READ_FIELD_DEF objects describing
    fields to read.
  */
  IC_READ_FIELD_DEF **field_defs;
};
typedef struct ic_read_field_bind IC_READ_FIELD_BIND;

typedef struct ic_write_field_def IC_WRITE_FIELD_DEF;
struct ic_write_field_bind
{
  /* Number of fields in bit_array and field_defs array */
  guint32 no_fields;
  /*
    For writes the user needs to supply a buffer, data is referenced
    offset from the buffer pointer. The user needs to supply both a
    buffer pointer and a size of the buffer. The buffer is maintained
    by the user of the API and won't be touched by the API other than
    for reading its data.
  */
  guint32 buffer_size;
  gchar *buffer_ptr;
  /*
    The bit_array field makes it possible to reuse this structure for many
    different writes on the same table. We will only write those fields that
    have their bit set in the bit_array.
  */
  guint8 *bit_array;
  /*
    An array of pointers to IC_WRITE_FIELD_DEF objects describing
    fields to write.
  */
  IC_WRITE_FIELD_DEF **field_defs;
};
typedef struct ic_write_field_bind IC_WRITE_FIELD_BIND;

typedef struct ic_key_field_def IC_KEY_FIELD_DEF;
struct ic_key_field_bind
{
  /* Number of fields in bit_array and field_defs array */
  guint32 no_fields;
  /*
    For writes the user needs to supply a buffer, data is referenced
    offset from the buffer pointer. The user needs to supply both a
    buffer pointer and a size of the buffer. The buffer is maintained
    by the user of the API and won't be touched by the API other than
    for reading its data.
  */
  guint32 buffer_size;
  gchar *buffer_ptr;
  /*
    An array of pointers to IC_KEY_FIELD_DEF objects describing
    fields to use in key.
  */
  IC_KEY_FIELD_DEF **field_defs;
};
typedef struct ic_key_field_bind IC_KEY_FIELD_BIND;

/*
  We only handle the basic types here. This means that we manage
  all integer types of various sorts, the bit type, fixed size
  character strings, the variable sized character strings. We also
  handle the new NDB type BLOB type which can store up to e.g.
  16MB or more on the actual record. User level blobs are handled
  on a higher level by using many records to accomodate for large
  BLOB's. A user level BLOB is allowed to be spread among many
  nodes and node groups within one cluster, but it cannot span
  many clusters. The new BLOB type is essentially an array of
  unsigned 8 bit values.
*/
enum ic_field_type
{
  IC_API_UINT32_TYPE= 0,
  IC_API_UINT64_TYPE= 1,
  IC_API_UINT24_TYPE= 2,
  IC_API_UINT16_TYPE= 3,
  IC_API_UINT8_TYPE= 4,
  IC_API_INT32_TYPE= 5,
  IC_API_INT64_TYPE= 6,
  IC_API_INT24_TYPE= 7,
  IC_API_INT16_TYPE= 8,
  IC_API_INT8_TYPE= 9,
  IC_API_BIT_TYPE= 10,
  IC_API_FIXED_SIZE_CHAR= 11,
  IC_API_VARIABLE_SIZE_CHAR= 12,
  IC_API_BLOB_TYPE= 13
};
typedef enum ic_field_type IC_FIELD_TYPE;

struct ic_read_field_def
{
  /* Fields set by caller */
  /* The field can be specified by field id.  */
  guint32 field_id;
  /* Reference to the data within the buffer: This is set-up by caller */
  guint32 data_offset;
  /*
    For variable length it is possible to specify a starting byte position
    and an end position. The actual data read will always start at
    start_pos, the actual length read is returned in data_length. If zero
    then there was no data at start_pos or beyond.

    The start_pos and end_pos is set by caller and not changed by API.

    The special case of start_pos= end_pos= 0 means that we're interested
    in the length of the complete field. In this case the data type
    returned is always IC_API_UINT32 and the length is returned at
    data_offset and data_length is 4 (size of UINT32).
    
    To read all data in a field set start_pos to 0 and end_pos at
    least as big as the maximum size of the field. However it isn't
    possible to read more than 8 kBytes per read. It is however
    possible to send many parallel reads reading different portions
    of the field for very long fields. This distinction is only relevant
    for BLOB fields.
  */
  guint32 start_pos;
  guint32 end_pos;

  /* Fields set by API */
  /* The actual length of the field, set by API */
  guint32 data_length;
  /* The field type, set by API */
  IC_FIELD_TYPE field_type;
  /* Null indicator set by API */
  gboolean is_null;
};

struct ic_write_field_def
{
  /*
    All fields in this struct is set by caller and the struct is
    read-only in the API.
  */
  /* The field can be specified by field id. */
  guint32 field_id;
  /* Reference inside the buffer of the data we're writing */
  guint32 data_offset;
  /*
    For fixed size fields the start_pos and end_pos is ignored.
    For variable sized fields it indicates the starting position
    to write from and the end position to write into. For normal
    writes of the full field we set start_pos to 0 and end_pos to
    size of the field.
  */
  guint32 start_pos;
  guint32 end_pos;
  /*
    This is the field type of the data sent to the API, if there is a
    mismatch between this type and the field type in the database
    we will apply a conversion method to convert it to the proper
    data type.
  */
  IC_FIELD_TYPE field_type;
  /* TRUE if we're writing NULL to the field */
  gboolean is_null;
  /*
    TRUE if we're writing zero's from start_pos to end_pos,
    Thus no need to send data since we're setting everything
    to zero's.
  */
  gboolean allocate_field_size;
};

struct ic_key_field_def
{
  /*
    All fields in this struct is set by caller and the struct is
    read-only in the API.
  */
  /* The field can be specified by field id. */
  guint32 field_id;
  /* Reference inside the buffer of the data we're writing */
  guint32 data_offset;
  /* Length of field, no BLOBs allowed here */
  guint32 data_length;
  /*
    This is the field type of the data sent to the API, if there is a
    mismatch between this type and the field type in the database
    we will apply a conversion method to convert it to the proper
    data type.
  */
  IC_FIELD_TYPE field_type;
};

enum ic_read_key_op
{
  SIMPLE_KEY_READ= 0,
  KEY_READ= 1,
  EXCLUSIVE_KEY_READ= 2,
  COMMITTED_KEY_READ= 3
};
typedef enum ic_read_key_op IC_READ_KEY_OP;

enum ic_write_key_op
{
  KEY_UPDATE= 0,
  KEY_WRITE= 1,
  KEY_INSERT= 2,
  KEY_DELETE= 3
};
typedef enum ic_write_key_op IC_WRITE_KEY_OP;

enum ic_scan_op
{
  SCAN_READ_COMMITTED= 0,
  SCAN_READ_EXCLUSIVE= 1,
  SCAN_HOLD_LOCK= 2,
  SCAN_CONSISTENT_READ= 3
};
typedef enum ic_scan_op IC_SCAN_OP;

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
  guint32 table_id;
  guint32 index_id;
  gboolean use_index;
};
typedef struct ic_table_def IC_TABLE_DEF;

struct ic_metadata_bind_ops
{
  int (*ic_table_bind) (IC_TABLE_DEF *table_def,
                        const gchar *table_name);
  int (*ic_index_bind) (IC_TABLE_DEF *table_def,
                        const gchar *index_name,
                        const gchar *table_name);
  int (*ic_field_bind) (guint32 table_id,
                        guint32* field_id,
                        const gchar *field_name);
};
typedef struct ic_metadata_bind_ops IC_METADATA_BIND_OPS;

#define TRANSLATION_ARRAY_SIZE 256
struct ic_translation_obj;
typedef struct ic_translation_obj IC_TRANSLATION_OBJ;
struct ic_translation_obj
{
  IC_HASHTABLE *hash_table;
};

enum ic_apid_operation_type
{
  SCAN_OPERATION= 0,
  KEY_READ_OPERATION= 1,
  KEY_WRITE_OPERATION= 2
};
typedef enum ic_apid_operation_type IC_APID_OPERATION_TYPE;

struct ic_apid_error
{
  int error_code;
  gchar *error_msg;
};
typedef struct ic_apid_error IC_APID_ERROR;

struct ic_apid_operation
{
  IC_TRANSACTION *trans_obj;
  IC_READ_FIELD_BIND *read_fields;
  IC_TABLE_DEF *table_def;
  IC_APID_ERROR *error;
  IC_APID_OPERATION_TYPE op_type;
  void *user_reference;
};
typedef struct ic_apid_operation IC_APID_OPERATION;

typedef struct ic_apid_connection IC_APID_CONNECTION;
struct ic_apid_connection_ops
{
  int (*ic_read_key_access)
               (IC_APID_CONNECTION *apid_conn,
                IC_TRANSACTION *transaction_obj,
                IC_READ_FIELD_BIND *read_fields,
                IC_KEY_FIELD_BIND *key_fields,
                IC_TABLE_DEF *table_def,
                IC_READ_KEY_OP read_key_op,
                void *user_reference);

  int (*ic_write_key_access)
               (IC_APID_CONNECTION *apid_conn,
                IC_TRANSACTION *transaction_obj,
                IC_WRITE_FIELD_BIND *write_fields,
                IC_KEY_FIELD_BIND *key_fields,
                IC_TABLE_DEF *table_def,
                IC_WRITE_KEY_OP write_key_op,
                void *user_reference);

  int (*ic_start_transaction) (IC_APID_CONNECTION *apid_conn,
                               IC_TRANSACTION **transaction_obj,
                               IC_TRANSACTION_HINT *transaction_hint);
  int (*ic_join_transaction) (IC_APID_CONNECTION *apid_conn,
                              IC_TRANSACTION transaction_obj);
  int (*ic_poll) (IC_APID_CONNECTION *apid_conn, glong wait_time);
  IC_APID_OPERATION* (*ic_get_next_completed_operation)
                              (IC_APID_CONNECTION *apid_conn);
  void (*ic_free) (IC_APID_CONNECTION *apid_conn);
};
typedef struct ic_apid_connection_ops IC_APID_CONNECTION_OPS;

struct ic_apid_connection
{
  IC_APID_CONNECTION_OPS apid_conn_ops;
  IC_METADATA_BIND_OPS apid_metadata_ops;
  IC_APID_GLOBAL *apid_global;
  IC_TRANSLATION_OBJ *trans_bindings;
  IC_TRANSLATION_OBJ *op_bindings;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_id_bitmap;
  IC_THREAD_CONNECTION *thread_conn;
  guint32 thread_id;
};

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
IC_APID_GLOBAL* ic_connect_apid_global(IC_API_CONFIG_SERVER *apic,
                                       int *ret_code,
                                       gchar **err_str);
int ic_disconnect_apid_global(IC_APID_GLOBAL *apid_global);
int ic_wait_first_node_connect(IC_APID_GLOBAL *apid_global,
                               guint32 cluster_id);
IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *apid_global,
                          IC_BITMAP *cluster_id_bitmap);
#endif
