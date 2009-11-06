/* Copyright (C) 2009 iClaustron AB

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
  This data structure contains the important information about the
  receive thread and its state. It contains free pools of receive
  pages containing signals and pools of containers to store NDB
  messages in. Both of these are transported to the user thread and
  will be released in the user thread to the global pool.
*/

#ifndef IC_APID_INT_H
#define IC_APID_INT_H

typedef struct ic_field_def IC_FIELD_DEF;
typedef struct ic_field_in_op IC_FIELD_IN_OP;

typedef struct ic_translation_obj IC_TRANSLATION_OBJ;

typedef struct ic_int_transaction IC_INT_TRANSACTION;
typedef struct ic_int_apid_operation IC_INT_APID_OPERATION;
typedef struct ic_int_apid_error IC_INT_APID_ERROR;
typedef struct ic_int_apid_connection IC_INT_APID_CONNECTION;
typedef struct ic_int_apid_global IC_INT_APID_GLOBAL;
typedef struct ic_int_table_def IC_INT_TABLE_DEF;
typedef struct ic_int_range_condition IC_INT_RANGE_CONDITION;
typedef struct ic_int_where_condition IC_INT_WHERE_CONDITION;

typedef enum ic_apid_operation_list_type IC_APID_OPERATION_LIST_TYPE;

typedef struct ic_thread_connection IC_THREAD_CONNECTION;
typedef struct ic_grid_comm IC_GRID_COMM;
typedef struct ic_send_node_connection IC_SEND_NODE_CONNECTION;
typedef struct ic_ndb_receive_state IC_NDB_RECEIVE_STATE;
typedef struct ic_listen_server_thread IC_LISTEN_SERVER_THREAD;
/*
  Objects used by iClaustron Data API implementation, not related at all
  to the API objects.
*/

struct ic_int_transaction
{
  /* Public part */
  IC_TRANSACTION_OPS trans_ops;
  guint64 transaction_id;
  /* Hidden part */
  IC_COMMIT_STATE commit_state;
  IC_SAVEPOINT_ID savepoint_stable;
  IC_SAVEPOINT_ID savepoint_requested;
  /* Internal part */
};

enum ic_apid_operation_list_type
{
  NO_LIST = 0,
  IN_DEFINED_LIST = 1,
  IN_EXECUTING_LIST = 2,
  IN_EXECUTED_LIST = 3,
  IN_COMPLETED_LIST = 4
};

/*
  Read key operations have a definition of fields read, a definition of the
  key fields and finally a definition of the type of read operation.

  Write key operations have a definition of fields written, a definition of
  the key fields and finally a definition of the write operation type.

  Scan operations also have a definition of fields read, it has a range
  condition in the cases when there is a scan of an index (will be NULL
  for scan table operation). There is also a generic where condition.
*/

struct ic_field_in_op
{
  /*
    The fields in this struct defines a field and is used both to read and
    write data in the Data API. The data in this struct is initialised at
    create time, the only fields that can be changed dynamically is start_pos
    and end_pos when we read or write a partial field.
  */

  /* The field can be specified by field id. */
  guint32 field_id;
  /* Reference inside the buffer of the data we're writing */
  guint32 data_offset;
  /*
    Which of the null bits are we using, we divide by 8 to get the null
    byte to use and we get the 3 least significant bits to get which bit
    in this byte to use. The null byte to use is offset from null pointer
    reference stored in IC_FIELD_BIND object. Set to IC_NO_NULL_OFFSET
    means no NULL offset is used.
  */
  guint32 null_offset;
  /*
    This is the field type of the data sent to the API, if there is a
    mismatch between this type and the field type in the database
    we will apply a conversion method to convert it to the proper
    data type.
  */
  IC_FIELD_TYPE field_type;
  /*
    != 0 if we're writing zero's from start_pos to end_pos,
    Thus no need to send data since we're setting everything
    to zero's. This can be set dynamically and is ignored for
    read operations.
  */
  guint32 allocate_field_size;
  /*
    For fixed size fields the start_pos and end_pos is ignored.

    For variable length it is possible to specify a starting byte position
    and an end position. The actual data read will always start at
    start_pos, the actual length read is returned in data_length. If zero
    then there was no data at start_pos or beyond.

    The start_pos and end_pos is set by caller and not changed by API.

    To read all data in a field set start_pos to 0 and end_pos at
    least as big as the maximum size of the field. However it isn't
    possible to read more than 8 kBytes per read. It is however
    possible to send many parallel reads reading different portions
    of the field for very long fields. This distinction is only relevant
    for BLOB fields.
  */
  guint32 start_pos;
  guint32 end_pos;
};

struct ic_field_def
{
  guint32 field_id;
  guint32 field_size;
  gboolean is_nullable;
  gboolean has_default_value;
  gchar *default_value;
  guint32 default_value_len;
  IC_FIELD_TYPE field_type;
};

struct ic_int_table_def
{
  /* Public part */
  IC_TABLE_DEF_OPS table_def_ops;
  /* Hidden part */
  /* Internal part */
  guint32 table_id;
  guint32 index_id;
  guint32 num_fields;
  guint32 num_key_fields;
  gboolean is_index;
  gboolean is_unique_index;
  IC_FIELD_DEF **fields;
};

struct ic_int_range_condition
{
  IC_RANGE_CONDITION_OPS *range_ops;
  guint32 not_used;
};

struct ic_int_where_condition
{
  IC_WHERE_CONDITION_OPS *cond_ops;
  guint32 not_used;
};

struct ic_int_apid_operation
{
  /* Public part */
  IC_APID_OPERATION_OPS *apid_op_ops;
  gboolean any_error;
  /* Hidden part */
  union
  {
    /*
      read_key_op used by read key operations
      write_key_op used by write key operations
      scan_op used by scan operations.
    */
    IC_READ_KEY_OP read_key_op;
    IC_WRITE_KEY_OP write_key_op;
    IC_SCAN_OP scan_op;
  };
  guint32 num_cond_assignment_ids;
  IC_APID_OPERATION_TYPE op_type;

  IC_APID_CONNECTION *apid_conn;
  IC_TRANSACTION *trans_obj;
  IC_TABLE_DEF *table_def;

  IC_WHERE_CONDITION *where_cond;
  IC_RANGE_CONDITION *range_cond;
  IC_CONDITIONAL_ASSIGNMENT **cond_assign;

  IC_APID_ERROR *error;
  void *user_reference;

  /* fields used by scans, read key operations and write key operations */
  IC_FIELD_IN_OP **fields;
  IC_FIELD_IN_OP **key_fields;

  /*
    For writes the user needs to supply a buffer, data is referenced
    offset from the buffer pointer. The user needs to supply both a
    buffer pointer and a size of the buffer. The buffer is maintained
    by the user of the API and won't be touched by the API other than
    for reading its data.

    User supplied buffer to use for data to write and read results.
    The buffer is stable until the operation is released or the
    transaction is released, for scans until we fetch the next
    row.

    The bit_array field makes it possible to reuse this structure for many
    different reads on the same table. We will only read those fields that
    have their bit set in the bit_array.
  */
  gchar *buffer_ptr;
  guint8 *null_ptr;

  /* Internal part */
  guint32 buffer_size;
  guint32 max_null_bits;

  /* Number of fields in this operation object */
  guint32 num_fields;
  /* Number of fields defined */
  guint32 num_fields_defined;
  /* Number of key fields in table/index, 0 if not all keys are defined */
  guint32 num_key_fields;
  gboolean is_buffer_allocated_by_api;
  /*
    When setting up the APID operation object we will check whether all fields
    have been defined in the case that the operation is using a table object
    (in which case the key is the primary key) and in the case the operation
    is using the unique key (in which case the the key is the unique key).
    For non-unique indexes this flag is always FALSE since the operation
    cannot be used for Key Read/Write operations.
  */
  gboolean is_all_key_fields_defined;

  IC_APID_OPERATION_LIST_TYPE list_type;
  IC_INT_APID_OPERATION *next_trans_op;
  IC_INT_APID_OPERATION *prev_trans_op;
  IC_INT_APID_OPERATION *next_conn_op;
  IC_INT_APID_OPERATION *prev_conn_op;

  IC_INT_APID_GLOBAL *apid_global;
};

struct ic_int_apid_connection
{
  IC_APID_CONNECTION_OPS apid_conn_ops;
  IC_METADATA_BIND_OPS apid_metadata_ops;
  IC_INT_APID_GLOBAL *apid_global;
  IC_DYNAMIC_TRANSLATION *trans_bindings;
  IC_DYNAMIC_TRANSLATION *op_bindings;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_id_bitmap;
  IC_THREAD_CONNECTION *thread_conn;
  guint32 thread_id;
  /*
    The operations pass through a set of lists from start to end.

    It starts in the defined list. This list is a normal single linked list.

    When the user sends the operation to the cluster they enter the
    executing list. This list is a doubly linked list since they can leave
    this list in any order.

    When all messages of the operation have been received the operation enter
    the executed list. This is again a single linked list.

    When the user asks for the next executed operation the operation enters
    the last list which is the completed operations. This is also a singly
    linked list.
  */
  IC_INT_APID_OPERATION *first_defined_operation;
  IC_INT_APID_OPERATION *last_defined_operation;
  IC_INT_APID_OPERATION *first_executing_list;
  IC_INT_APID_OPERATION *last_executing_list;
  IC_INT_APID_OPERATION *first_completed_operation;
  IC_INT_APID_OPERATION *last_completed_operation;
  IC_INT_APID_OPERATION *first_executed_operation;
  IC_INT_APID_OPERATION *last_executed_operation;
};

struct ic_int_apid_error
{
  IC_APID_ERROR_OPS error_ops;
  int error_code;
  gchar *error_msg;
};

#define IC_MAX_SERVER_PORTS_LISTEN 256
#define IC_MAX_RECEIVE_THREADS 64

struct ic_int_apid_global
{
  IC_APID_GLOBAL_OPS apid_global_ops;
  IC_BITMAP *cluster_bitmap;
  GMutex *mutex;
  GCond *cond;
  guint32 num_user_threads_started;
  gboolean stop_flag;
  gboolean use_external_connect;
  IC_SOCK_BUF *send_buf_pool;
  IC_SOCK_BUF *ndb_message_pool;
  IC_GRID_COMM *grid_comm;
  IC_API_CONFIG_SERVER *apic;
  /*
    Heartbeat thread related variables, the heartbeat thread handles
    heartbeat sending for all active nodes, the active nodes are
    organised in a linked list of send node connections. This list
    and all other heartbeat related variables are protected by the
    heartbeat mutex. The heartbeat condition variable is used to sleep
    on until the first node connection has arrived.

    When the heartbeat thread goes through the list to send heartbeat
    messages it takes one node at a time and keeps track of the node
    currently sending using the curr_heartbeat_node. Since the mutex
    is released, this node could disappear before we have time to
    grab the mutex again, thus the removal of nodes from the heartbeat
    thread has to also update this variable.

    The heartbeat thread acts in the same manner as an application thread
    and thus it requires an IC_APID_CONNECTION object to handle interaction
    with send and receive threads.
  */
  IC_APID_CONNECTION *heartbeat_conn;
  IC_SEND_NODE_CONNECTION *first_heartbeat_node;
  IC_SEND_NODE_CONNECTION *curr_heartbeat_node;
  GMutex *heartbeat_mutex;
  GCond *heartbeat_cond;
  guint32 heartbeat_thread_id;
  guint32 heartbeat_thread_waiting;
  /* End heartbeat thread variables */

  IC_THREADPOOL_STATE *rec_thread_pool;
  IC_THREADPOOL_STATE *send_thread_pool;
  GMutex *thread_id_mutex;
  guint32 num_receive_threads;
  guint32 num_listen_server_threads;
  IC_RUN_APID_THREAD_FUNC apid_func;
  IC_NDB_RECEIVE_STATE *receive_threads[IC_MAX_RECEIVE_THREADS];
  IC_LISTEN_SERVER_THREAD *listen_server_thread[IC_MAX_SERVER_PORTS_LISTEN];
};

#include "ic_apid_impl.h"
#endif
