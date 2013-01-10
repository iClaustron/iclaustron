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
typedef struct ic_field_in_query IC_FIELD_IN_QUERY;

typedef struct ic_define_field IC_DEFINE_FIELD;
typedef struct ic_define_index IC_DEFINE_INDEX;

typedef struct ic_translation_obj IC_TRANSLATION_OBJ;

typedef enum ic_alter_operation_type IC_ALTER_OPERATION_TYPE;
typedef struct ic_int_alter_table IC_INT_ALTER_TABLE;
typedef struct ic_int_alter_tablespace IC_INT_ALTER_TABLESPACE;
typedef enum ic_metadata_transaction_state IC_METADATA_TRANSACTION_STATE;
typedef struct ic_int_metadata_transaction IC_INT_METADATA_TRANSACTION;

typedef struct ic_int_transaction IC_INT_TRANSACTION;
typedef struct ic_int_apid_query IC_INT_APID_QUERY;
typedef struct ic_int_apid_error IC_INT_APID_ERROR;
typedef struct ic_int_apid_connection IC_INT_APID_CONNECTION;
typedef struct ic_int_apid_global IC_INT_APID_GLOBAL;
typedef struct ic_int_table_def IC_INT_TABLE_DEF;
typedef struct ic_int_range_condition IC_INT_RANGE_CONDITION;
typedef struct ic_int_where_condition IC_INT_WHERE_CONDITION;

typedef enum ic_apid_query_list_type IC_APID_QUERY_LIST_TYPE;

typedef struct ic_send_cluster_node IC_SEND_CLUSTER_NODE;

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
  IC_TRANSACTION_OPS *trans_ops;
  guint64 transaction_id;
  /* Hidden part */
  IC_COMMIT_STATE commit_state;
  IC_SAVEPOINT_ID savepoint_stable;
  IC_SAVEPOINT_ID savepoint_requested;
  /* Internal part */
  IC_INT_APID_QUERY *first_trans_query;
  IC_INT_APID_QUERY *last_trans_query;
};

enum ic_apid_query_list_type
{
  NO_LIST = 0,
  IN_DEFINED_LIST = 1,
  IN_EXECUTING_LIST = 2,
  IN_EXECUTED_LIST = 3,
  IN_COMPLETED_LIST = 4
};

/*
  Read key queries have a definition of fields read, a definition of the
  key fields and finally a definition of the type of read query.

  Write key queries have a definition of fields written, a definition of
  the key fields and finally a definition of the write query type.

  Scan queries also have a definition of fields read, it has a range
  condition in the cases when there is a scan of an index (will be NULL
  for scan table query). There is also a generic where condition.
*/

struct ic_field_in_query
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
    read queries.
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
  IC_TABLE_DEF_OPS *table_def_ops;
  /* Hidden part */
  /* Internal part */
  guint32 table_id;
  guint32 index_id;
  guint32 num_fields;
  guint32 num_key_fields;
  gboolean is_index;
  gboolean is_unique_index;
  IC_BITMAP *key_fields;
  guint32 *key_field_id_order;
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

struct ic_int_apid_error
{
  IC_APID_ERROR_OPS *apid_error_ops;
  gchar *error_msg;
  int error_code;
  guint32 error_line;
  guint32 error_node_id;
  guint32 master_node_id;
  IC_ERROR_CATEGORY error_category;
  IC_ERROR_SEVERITY_LEVEL error_severity;
};

struct ic_define_field
{
  IC_DEFINE_FIELD *next_add_field;
  IC_DEFINE_FIELD *next_drop_field;
  gchar *name;
  IC_FIELD_TYPE type;
  guint32 size;
  guint32 array_size;
  guint32 charset_id;
  guint32 scale;
  guint32 precision;
  gboolean is_nullable;
  gboolean is_field_disk_stored;
  gboolean is_part_of_pkey;
  gchar *default_value;
  guint32 default_value_len;
};

struct ic_define_index
{
  IC_DEFINE_INDEX *next_add_index;
  IC_DEFINE_INDEX *next_drop_index;
  gchar *name;
  IC_INDEX_TYPE type;
  IC_DEFINE_FIELD **def_fields;
  guint32 num_fields;
  gboolean is_null_values_in_index_allowed;
};

enum ic_alter_operation_type
{
  IC_CREATE_TABLE_OP= 0,
  IC_ALTER_TABLE_OP= 1,
  IC_DROP_TABLE_OP= 2,
  IC_RENAME_TABLE_OP= 3
};

struct ic_int_alter_table
{
  IC_ALTER_TABLE_OPS *alter_table_ops;

  /* Memory container for metadata transaction */
  IC_MEMORY_CONTAINER *mc_ptr;

  /* Transaction operation is part of */
  IC_INT_METADATA_TRANSACTION *md_trans;

  /* Lists of added/dropped fields and added/dropped indexes */
  IC_DEFINE_FIELD *first_add_field;
  IC_DEFINE_FIELD *last_add_field;

  IC_DEFINE_FIELD *first_drop_field;
  IC_DEFINE_FIELD *last_drop_field;

  IC_DEFINE_INDEX *first_add_index;
  IC_DEFINE_INDEX *last_add_index;

  IC_DEFINE_INDEX *first_drop_index;
  IC_DEFINE_INDEX *last_drop_index;

  /*
    Table name consisting of a concatenation of schema, database and
    table name.

    In case of rename table_name is the new table name and old_table_name
    is the old table name.
  */
  gchar *table_name;
  gchar *old_table_name;

  /* Number of fields defined in table */
  guint32 num_fields;

  /*
    Tablespace id, needed only if at least one field have is_disk_stored
    is true.
  */
  guint32 tablespace_id;
  guint32 tablespace_version;
  gboolean is_table_disk_stored;

  /*
    A non-logged table will survive a crash but will be empty after a
    cluster crash. A temporary table won't exist after a cluster crash.
    A table with checksum has a checksum on each row which is computed
    at writes and checked on reads. 
  */
  gboolean is_table_logged;
  gboolean is_table_temporary;
  gboolean is_table_checksummed;

  /* Hash map reference */
  guint32 hash_map_id;
  guint32 hash_map_version;

  /* Table reference */
  guint32 table_id;
  guint32 table_version;

  /* Type of operation create/alter/drop/rename table */
  IC_ALTER_OPERATION_TYPE alter_op_type;

  /* Next pointer in linked list of table operations in transaction */
  IC_INT_ALTER_TABLE *next_alter_table;
};

struct ic_int_alter_tablespace
{
  IC_ALTER_TABLESPACE_OPS *alter_ts_ops;
  IC_MEMORY_CONTAINER *mc_ptr;
  /* Transaction operation is part of */
  IC_INT_METADATA_TRANSACTION *md_trans;
  /* Next pointer in linked list of table operations in transaction */
  IC_INT_ALTER_TABLESPACE *next_alter_ts;
};

enum ic_metadata_transaction_state
{
  IC_WAIT_SCHEMA_TRANS_BEGIN_CONF= 0,
  IC_WAIT_SCHEMA_TRANS_END_CONF= 1,
  IC_WAIT_CREATE_TABLE_CONF= 2,
  IC_WAIT_NDB_ALTER_TABLE_CONF= 3,
  IC_WAIT_NDB_DROP_TABLE_CONF= 4,
  IC_WAIT_NDB_CREATE_INDEX_CONF= 5,
  IC_WAIT_NDB_DROP_INDEX_CONF= 6,
  IC_WAIT_CREATE_HASH_MAP_CONF= 7
};

struct ic_int_metadata_transaction
{
  IC_METADATA_TRANSACTION_OPS *md_trans_ops;

  /* Memory container for metadata transaction */
  IC_MEMORY_CONTAINER *mc_ptr;

  /* Cluster id to target metadata transaction towards */
  guint32 cluster_id;
  /* Node id of master node, used for all metadata transactions */
  guint32 node_id;
  /* Error object used to report back what went wrong */
  IC_INT_APID_ERROR error_object;

  /* Boolean to discover when NDB schema transaction completed */
  gboolean return_to_api;

  /* NDB Transaction reference */
  guint32 ndb_trans_ref;

  /* My transaction reference */
  guint32 my_trans_ref;

  /* Transaction state */
  IC_METADATA_TRANSACTION_STATE state;

  /* Data API connection object */
  IC_INT_APID_CONNECTION *apid_conn;

  IC_INT_ALTER_TABLE *current_alter_table;
  IC_INT_ALTER_TABLESPACE *current_alter_ts;

  /* Linked list of table operations in metadata transaction */
  IC_INT_ALTER_TABLE *first_alter_table;
  IC_INT_ALTER_TABLE *last_alter_table;

  /* Linked list of tablespace operations in metadata transaction */
  IC_INT_ALTER_TABLESPACE *first_alter_ts;
  IC_INT_ALTER_TABLESPACE *last_alter_ts;
};

typedef enum ic_reference_type IC_REFERENCE_TYPE;
enum ic_reference_type
{
  IC_MD_TRANS= 0
};

typedef struct ic_reference_container IC_REFERENCE_CONTAINER;
struct ic_reference_container
{
  void *reference;
  IC_REFERENCE_TYPE ref_type;
};

struct ic_int_apid_query
{
  /* Public part */
  IC_APID_QUERY_OPS *apid_query_ops;
  gboolean any_error;
  /* Hidden part */
  union
  {
    /*
      read_key_query used by read key queries
      write_key_query used by write key queries
      scan_query used by scan queries.
    */
    IC_READ_KEY_QUERY_TYPE read_key_query_type;
    IC_WRITE_KEY_QUERY_TYPE write_key_query_type;
    IC_SCAN_QUERY_TYPE scan_query_type;
  };
  guint32 num_cond_assignment_ids;
  IC_APID_QUERY_TYPE query_type;

  IC_APID_CONNECTION *apid_conn;
  IC_TRANSACTION *trans_obj;
  IC_TABLE_DEF *table_def;

  IC_WHERE_CONDITION *where_cond;
  IC_RANGE_CONDITION *range_cond;
  IC_CONDITIONAL_ASSIGNMENT **cond_assign;

  IC_APID_ERROR *error;
  void *user_reference;

  /* fields used by scans, read key queries and write key queries */
  IC_FIELD_IN_QUERY **fields;
  IC_FIELD_IN_QUERY **key_fields;

  /*
    For writes the user needs to supply a buffer, data is referenced
    offset from the buffer pointer. The user needs to supply both a
    buffer pointer and a size of the buffer. The buffer is maintained
    by the user of the API and won't be touched by the API other than
    for reading its data.

    User supplied buffer to use for data to write and read results.
    The buffer is stable until the query is released or the
    transaction is released, for scans until we fetch the next
    row.

    The bit_array field makes it possible to reuse this structure for many
    different reads on the same table. We will only read those fields that
    have their bit set in the bit_array.
  */
  guint64 *buffer_values;
  guint8 *null_ptr;

  /* Internal part */
  guint32 num_buffer_values;
  guint32 max_null_bits;

  /* Number of fields in this query object */
  guint32 num_fields;
  /* Number of fields defined */
  guint32 num_fields_defined;
  /* Number of key fields in table/index, 0 if not all keys are defined */
  guint32 num_key_fields;
  /*
    When setting up the APID query object we will check whether all fields
    have been defined in the case that the query is using a table object
    (in which case the key is the primary key) and in the case the query
    is using the unique key (in which case the the key is the unique key).
    For non-unique indexes this flag is always FALSE since the query
    cannot be used for Key Read/Write querys.
  */
  gboolean is_all_key_fields_defined;

  IC_APID_QUERY_LIST_TYPE list_type;

  IC_INT_APID_QUERY *next_trans_query;
  IC_INT_APID_QUERY *prev_trans_query;

  IC_INT_APID_QUERY *next_conn_query;
  IC_INT_APID_QUERY *prev_conn_query;

  IC_INT_APID_QUERY *next_defined_query;
  IC_INT_APID_QUERY *prev_defined_query;

  IC_INT_APID_QUERY *next_executing_list;
  IC_INT_APID_QUERY *prev_executing_list;

  IC_INT_APID_QUERY *next_completed_query;
  IC_INT_APID_QUERY *prev_completed_query;

  IC_INT_APID_QUERY *next_executed_query;
  IC_INT_APID_QUERY *prev_executed_query;

  IC_INT_APID_GLOBAL *apid_global;
};

struct ic_send_cluster_node
{
  guint32 cluster_id;
  guint32 node_id;

  IC_SOCK_BUF_PAGE *first_sock_buf_page;
  IC_SOCK_BUF_PAGE *last_sock_buf_page;

  IC_SEND_CLUSTER_NODE *next_send_cluster_node;
  IC_SEND_CLUSTER_NODE *prev_send_cluster_node;
};

struct ic_int_apid_connection
{
  IC_APID_CONNECTION_OPS *apid_conn_ops;
  IC_INT_APID_GLOBAL *apid_global;
  IC_DYNAMIC_PTR_ARRAY *trans_bindings;
  IC_DYNAMIC_PTR_ARRAY *op_bindings;
  IC_API_CONFIG_SERVER *apic;
  IC_BITMAP *cluster_id_bitmap;
  IC_THREAD_CONNECTION *thread_conn;
  IC_SOCK_BUF_PAGE *free_pages;
  IC_INT_METADATA_TRANSACTION *md_trans;
  guint32 thread_id;
  /*
    The queries pass through a set of lists from start to end.

    It starts in the defined list. This list is a normal single linked list.

    When the user sends the query to the cluster they enter the
    executing list. This list is a doubly linked list since they can leave
    this list in any order.

    When all messages of the query have been received the query enter
    the executed list. This is again a single linked list.

    When the user asks for the next executed query the query enters
    the last list which is the completed queries. This is also a singly
    linked list.
  */
  IC_INT_APID_QUERY *first_conn_query;
  IC_INT_APID_QUERY *last_conn_query;

  IC_INT_APID_QUERY *first_defined_query;
  IC_INT_APID_QUERY *last_defined_query;

  IC_INT_APID_QUERY *first_executing_list;
  IC_INT_APID_QUERY *last_executing_list;

  IC_INT_APID_QUERY *first_completed_query;
  IC_INT_APID_QUERY *last_completed_query;

  IC_INT_APID_QUERY *first_executed_query;
  IC_INT_APID_QUERY *last_executed_query;

  IC_SEND_CLUSTER_NODE *first_send_cluster_node;
  IC_SEND_CLUSTER_NODE *last_send_cluster_node;

  IC_INT_APID_ERROR apid_error;
};

#define IC_MAX_SERVER_PORTS_LISTEN 256
#define IC_MAX_RECEIVE_THREADS 64

struct ic_int_apid_global
{
  /* Public part */
  IC_APID_GLOBAL_OPS *apid_global_ops;
  IC_METADATA_BIND_OPS *apid_metadata_ops;
  IC_BITMAP *cluster_bitmap;
  /* Hidden part */
  /* Internal part */
  IC_MUTEX *mutex;
  IC_COND *cond;
  guint32 num_user_threads_started;
  gboolean stop_flag;
  gboolean use_external_connect;
  IC_SOCK_BUF *send_buf_pool;
  IC_SOCK_BUF *ndb_message_pool;
  IC_GRID_COMM *grid_comm;
  IC_API_CONFIG_SERVER *apic;
  /**
    dynamic_map_array is used when receiving a message from NDB kernel
    to map to our own internal object. This uses a dynamic pointer
    array that maps to a IC_REFERENCE_CONTAINER object which contains
    the pointer to the actual object plus a type identifier to ensure
    we don't get spurious messages that maps to wrong type of object.
  */
  IC_DYNAMIC_PTR_ARRAY *dynamic_map_array;
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
  IC_MUTEX *heartbeat_mutex;
  IC_COND *heartbeat_cond;
  guint32 heartbeat_thread_id;
  guint32 heartbeat_thread_waiting;
  /* End heartbeat thread variables */

  IC_THREADPOOL_STATE *rec_thread_pool;
  IC_THREADPOOL_STATE *send_thread_pool;
  IC_MUTEX *thread_id_mutex;
  guint32 num_receive_threads;
  guint32 num_listen_server_threads;
  /* The API node id I am using for this global connection */
  guint32 my_node_id;
  /*
    An atomic message id incremented each time a message is sent, only if
    configuration is using message ids.
    Protected by the mutex
  */
  guint32 message_id;
  /*
    Variable that indicates if we are waiting for first node to connect
    from any user thread.
  */
  gboolean wait_first_node_connect;
  IC_RUN_APID_THREAD_FUNC apid_func;
  IC_NDB_RECEIVE_STATE *receive_threads[IC_MAX_RECEIVE_THREADS];
  IC_LISTEN_SERVER_THREAD *listen_server_thread[IC_MAX_SERVER_PORTS_LISTEN];
};

#include "ic_apid_impl.h"
#endif
