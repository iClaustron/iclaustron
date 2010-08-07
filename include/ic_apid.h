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
#include <ic_threadpool.h>
#include <ic_connection.h>

/*
  GENERAL PRINCIPLES ON NAMING AND DATA HIDING IN iClaustron DATA API
  -------------------------------------------------------------------
  iClaustron uses a naming standard which is that all object types are used
  by capital letters. These names in capital letters are always defined by a
  typedef. The typedef maps the name of the real struct/enum or other data
  type. structs and enums always use the name in lower case. So e.g. the
  global object defining the Data API is called IC_APID_GLOBAL and is in
  reality a struct called ic_apid_global.

  In the iClaustron implementation files there is similarly an object
  IC_INT_APID_GLOBAL that contains the actual data which is hidden to the API
  user. The implementation will when creating a IC_APID_GLOBAL object
  actually create an IC_INT_APID_GLOBAL and at every call map it over to the
  internal data type. There is no execution cost attached to this mapping.
  It's only a method to ensure the implementation data isn't visible to API
  users.

  Certain data is required to be accessible fast through inlined methods
  although they are part of the private data of the objects. In this case
  iClaustron defines those data types in ic_apid_hidden.h. Often by using an
  object called e.g. IC_HIDDEN_APID_GLOBAL which is then a subset of
  IC_INT_APID_GLOBAL and a superset of IC_APID_GLOBAL. The user of the API is
  free to use the inline methods, these are part of the iClaustron Data API,
  however the user should not use variables defined in the ic_apid_hidden.h
  file. These are going to change without notice from release to release. Thus
  they are very unsafe to use. It is however not possible to hide those from
  the user since the application needs to have access to the inline methods
  to be able to inline them in their application.

  Each object used in the have an object called *_ops or *_OPS for the
  external name. These contains the function pointers to methods accessible
  for these objects. E.g. IC_APID_GLOBAL_OPS for the IC_APID_GLOBAL methods.
*/
typedef struct ic_apid_global IC_APID_GLOBAL;
typedef struct ic_apid_global_ops IC_APID_GLOBAL_OPS;
typedef struct ic_metadata_bind_ops IC_METADATA_BIND_OPS;

typedef struct ic_apid_connection IC_APID_CONNECTION;
typedef struct ic_apid_connection_ops IC_APID_CONNECTION_OPS;

typedef struct ic_table_def IC_TABLE_DEF;
typedef struct ic_table_def_ops IC_TABLE_DEF_OPS;

typedef struct ic_index_def IC_INDEX_DEF;
typedef struct ic_index_def_ops IC_INDEX_DEF_OPS;

typedef struct ic_metadata_transaction IC_METADATA_TRANSACTION;
typedef struct ic_metadata_transaction_ops IC_METADATA_TRANSACTION_OPS;

typedef struct ic_alter_table IC_ALTER_TABLE;
typedef struct ic_alter_table_ops IC_ALTER_TABLE_OPS;

typedef struct ic_alter_tablespace IC_ALTER_TABLESPACE;
typedef struct ic_alter_tablespace_ops IC_ALTER_TABLESPACE_OPS;

typedef struct ic_transaction IC_TRANSACTION;
typedef struct ic_transaction_ops IC_TRANSACTION_OPS;
typedef guint32 IC_SAVEPOINT_ID;

typedef struct ic_transaction_hint IC_TRANSACTION_HINT;
typedef struct ic_transaction_hint_ops IC_TRANSACTION_HINT_OPS;

typedef struct ic_apid_operation IC_APID_OPERATION;
typedef struct ic_apid_operation_ops IC_APID_OPERATION_OPS;

typedef struct ic_range_condition IC_RANGE_CONDITION;
typedef struct ic_range_condition_ops IC_RANGE_CONDITION_OPS;
typedef enum ic_lower_range_type IC_LOWER_RANGE_TYPE;
typedef enum ic_upper_range_type IC_UPPER_RANGE_TYPE;

typedef struct ic_where_condition IC_WHERE_CONDITION;
typedef struct ic_where_condition_ops IC_WHERE_CONDITION_OPS;
typedef enum ic_boolean_type IC_BOOLEAN_TYPE;
typedef enum ic_comparator_type IC_COMPARATOR_TYPE;
/* This type is also used by IC_CONDITIONAL_ASSIGNMENT */
typedef enum ic_calculation_type IC_CALCULATION_TYPE; 

typedef struct ic_conditional_assignment IC_CONDITIONAL_ASSIGNMENT;
typedef struct ic_conditional_assignment_ops IC_CONDITIONAL_ASSIGNMENT_OPS;

typedef struct ic_apid_error IC_APID_ERROR;
typedef struct ic_apid_error_ops IC_APID_ERROR_OPS;
typedef enum ic_error_severity_level IC_ERROR_SEVERITY_LEVEL;
typedef enum ic_error_category IC_ERROR_CATEGORY;

/* Parameter when starting Key read, Key write and Scan operations */
typedef enum ic_read_key_op IC_READ_KEY_OP;
typedef enum ic_write_key_op IC_WRITE_KEY_OP;
typedef enum ic_scan_op IC_SCAN_OP;

/* State returned when asking transaction for its commit state */
typedef enum ic_commit_state IC_COMMIT_STATE;

typedef enum ic_field_type IC_FIELD_TYPE;
typedef enum ic_index_type IC_INDEX_TYPE;
typedef enum ic_partition_type IC_PARTITION_TYPE;
typedef enum ic_tablespace_access_mode IC_TABLESPACE_ACCESS_MODE;

#define IC_NO_FIELD_ID 0xFFFFFFF0
#define IC_NO_NULL_OFFSET 0xFFFFFFF1

typedef int (*IC_RUN_APID_THREAD_FUNC)(IC_APID_CONNECTION*, IC_THREAD_STATE*);
typedef int (*IC_APID_CALLBACK_FUNC) (IC_APID_CONNECTION*, void* user_data);

/*
  The iClaustron Data API have a number of basic concepts:
  1) Table objects
    This is the meta data definition of a table and its fields. A table
    object can also be an index object. An index can either be a unique
    key, or an ordered index. All tables have a primary key. The unique
    keys are global indexes that are partitioned on the unique key
    rather than the primary key.

  2) Transaction object
    This object is used by all queries, before a transaction object can be
    used it has to be "started", starting a transaction object means to
    allocate from a pool of objects connected to the NDB kernel nodes.
    It is possible to define multi-threaded transactions. This means that
    several connection objects can work in parallel on defining and
    executing a transaction. It is however required to have some
    interaction among the threads to ensure communication towards the
    NDB kernel is still according to the NDB Protocol.

  3) Connection object
    A connection object represents usually a thread, the normal manner to
    use the API is to define one connection object per API. Thus it is
    not allowed to use the same object in many threads simultaneously
    unless protected by mutexes. Queries are defined on the connection
    object using an operation object, a transaction object and a table
    object.

  4) Operation object
    An operation object represents the actual query and contains the fields
    to read/write, the key to use, ranges for index scans and a possible
    condition of the query. Only primary key access, unique key access
    and table scans and index scans are currently supported. It is also
    possible to define conditional assignments on update queries.

  There are two variants of how to define Data API operation objects.
  The first variant is to define a predefined API operation object that
  contains all fields in the table. In this case one uses a bitmap to
  define which fields to actually use in executing a certain query.

  The other variant is to define a subset of the fields of a table. In
  this case one must use all fields all the time, no bitmap will be
  used in this case.

  The first variant is a good variant to use in an environment with
  generic queries being executed, thus an operation object can be
  easily reused by use of the bitmap. The other variant is more
  efficient in applications with predefined queries that are reused
  again and again such as the iClaustron File Server.

  The set-up of an API operation objects is a mapping from fields to
  a buffer and a null buffer. This mapping is used for both reads
  and writes. For reads the result will be read into the buffer using
  the buffer pointers and offset. For writes the updates will be done
  by reading the data from the buffer. The mapping of primary and
  unique key fields are also predefined. Thus field mappings for read,
  write and key operations are predefined at definition of the operation
  object and only needs to be changed at definition of the operation
  object.

  There are many other parts of the operation object that requires
  definition for each query. However none of these are absolutely
  required to define any query in the API.

  The dynamic parts are:
  1) Definition of ranges for scan operations
  2) Definition of WHERE clause for any operation type
     A WHERE clause is defined by a number of individual conditions,
     then it is possible to bind together conditions using
     AND, OR and XOR.

   Before an operation can be used in a query it also has to be mapped
   towards the following objects.
   1) Transaction object
   2) Data API Connection object

   Finally an operation type has to be defined for the operation object
   before the query is ready to be executed.

   The user has full control over when a query is actually sent to the
   NDB kernel. A query needs to be defined first by using an API operation
   object. Such prepare calls are ic_read_key_access, ic_write_key_access
   and ic_scan_access. In these calls the mapping of objects is performed
   and the operation type is provided. The dynamic fields, ranges and
   conditions have to be defined on the API operation object before this
   call is made. However for simple queries no such preparatory calls are
   needed.

   When the application wants to send the queries it can use two methods,
   ic_send and ic_flush. ic_send only sends and wherafter the thread is free
   to define new queries or work on any other thing. However the objects
   which have been sent and buffers used by the queries must not be
   touched by the application until the query has been returned. ic_flush
   will start waiting for queries to complete immediately after sending the
   queries. When using ic_send one can use ic_poll to start waiting
   at any time convenient for the application to wait.

   The default manner of using the API is to have send and receive done
   separately to ensure that maximum possible parallelism and performance
   can be achieved. However there is also some simplifications available
   whereby the API user can make queries synchronous such that immediately
   after returning the data is available.

   The asynchronous method requires that the API user defines a callback
   method which is called when the query is completed or have been
   found in error. The synchronous method uses callback method inside the
   API to give the user a perception of a RPC interface instead. This method
   is much easier to program but also a lot less performant. Improvements of
   up to 5x have been recorded in using the asynchronous method compared to
   the synchronous method using the NDB API.
*/

struct ic_table_def_ops
{
  /*
    The table definition object is used to get data about a table, its
    fields and its keys.
  */

  int (*ic_get_table_id)   (IC_TABLE_DEF *table_def,
                            guint32 *table_id);
  int (*ic_get_field_id)   (IC_TABLE_DEF *table_def,
                            const gchar *field_name,
                            guint32* field_id);
  int (*ic_get_field_name) (IC_TABLE_DEF *table_def,
                            guint32 field_id,
                            gchar **field_name);
  int (*ic_get_field_type) (IC_TABLE_DEF *table_def,
                            guint32 field_id,
                            IC_FIELD_TYPE *field_data_type);
  int (*ic_get_field_len)  (IC_TABLE_DEF *table_def,
                            guint32 field_id,
                            guint32 *field_len);
  /*
    The data buffer used by APID operations can either be defined by the
    user, or it's defined by the API. In the case of a buffer defined by
    the API, these two calls are used to get the buffer and the offsets
    of the fields in this buffer.

    The call to ic_get_buf will allocate a data buffer and null buffer
    and will report back the sizes of these. The buffer pointers and sizes
    can be immediately reused in creating an APID operation.

    The call to ic_get_buf_offset is used to get the actual whereabouts of the
    fields in the buffer. Here fields will always be aligned on a boundary
    such that no special alignment is necessary of these field data.
  */
  int (*ic_get_buf)        (IC_TABLE_DEF *table_def,
                            IC_BITMAP *used_fields,
                            gchar **data_buffer,
                            guint8 **null_buffer,
                            guint32 *data_buffer_size,
                            guint32 *null_buffer_size);

  int (*ic_get_buf_offset) (IC_TABLE_DEF *table_def,
                            guint32 field_id,
                            guint32 *offset,
                            guint32 *null_bit_offset);
};

struct ic_index_def_ops
{
  /*
    The index definition object is used to get data about an index. Data
    about its fields and the table it's part of is derived from the
    table definition object
  */
  int (*ic_get_index_id)   (IC_INDEX_DEF *index_def,
                            guint32 *index_id);
  int (*ic_get_table_id)   (IC_INDEX_DEF *index_def,
                            guint32 *table_id);
  int (*ic_get_num_key_fields) (IC_INDEX_DEF *index_def,
                                guint32 *num_key_fields);
  int (*ic_get_key_field_id) (IC_INDEX_DEF *index_def,
                              guint32 key_field_id_order,
                              guint32 *field_id);
};

struct ic_metadata_bind_ops
{
  /*
    Bind to metadata for an IC_APID_CONNECTION
    ------------------------------------------
    iClaustron binds to tables and indexes in the NDB kernel nodes.
    These metadata objects contain definition of tables, their fields
    and their indexes.

    Each IC_APID_CONNECTION needs to bind itself to a table object in order
    to issue queries towards the table or using the index. When a
    connection object doesn't need a table object anymore it issues an
    unbind call, after this the connection won't be able to access this
    table definition anymore.

    The IC_TABLE_DEF object is created by the API and can only be used for
    reading metadata. It is not possible to change any metadata through this
    object. It's possible that the implementation uses the same object towards
    several connection objects.

    Table names, index names, database names and schema names are normal
    NULL-terminated strings.
  */
  IC_APID_ERROR*
      (*ic_table_bind) (IC_APID_GLOBAL *apid_global,
                        IC_APID_CONNECTION *apid_conn,
                        IC_METADATA_TRANSACTION *metadata_trans_obj,
                        IC_TABLE_DEF **table_def,
                        const gchar *schema_name,
                        const gchar *db_name,
                        const gchar *table_name);
  IC_APID_ERROR*
      (*ic_index_bind) (IC_APID_GLOBAL *apid_global,
                        IC_APID_CONNECTION *apid_conn,
                        IC_METADATA_TRANSACTION *metadata_trans_obj,
                        IC_TABLE_DEF **table_def,
                        const gchar *schema_name,
                        const gchar *db_name,
                        const gchar *index_name,
                        const gchar *table_name);
   IC_APID_ERROR*
       (*ic_table_unbind) (IC_APID_GLOBAL *apid_global,
                           IC_APID_CONNECTION *apid_conn,
                           IC_TABLE_DEF *table_def);
};

struct ic_metadata_transaction_ops
{
  /*
    A metadata transaction consists of a number of operations, each operation
    performs creation of a table, adding an index, altering a table, drop of
    a table and so forth.

    There are certain rutable_defles to what things can be combined into metadata
    transactions. For each operation one needs to create an IC_ALTER_TABLE
    object by ic_create_metadata_op which also connects this operation to
    a metadata transaction.
  */
  int (*ic_create_metadata_op) (IC_METADATA_TRANSACTION *md_trans,
                                IC_ALTER_TABLE **alter_table);

  /*
    Create a metadata transaction operation for altering/dropping/creating
    a tablespace.
  */
  int (*ic_create_tablespace_op) (IC_METADATA_TRANSACTION *md_trans,
                                  IC_ALTER_TABLESPACE **alter_ts);

  /*
    Commit the metadata transaction, it's mostly the atomic part of
    transactions we are dealing in here. Thus the metadata transaction
    either will be fully done or nothing will be done. If it is reported
    as done back to the API then it will also be durable.
  */
  int (*ic_md_commit) (IC_METADATA_TRANSACTION *md_trans);

  int (*ic_free_md_trans) (IC_METADATA_TRANSACTION *md_trans);
};

struct ic_alter_table_ops
{
  /*
    Fields are added as part of create table, or as part of an alter table.
    There are certain restrictions on what fields that can be added as
    part of alter table, as alter table is done as an online operation.

    If at least one field is defined to be disk stored, then the table
    must have a tablespace attached to it, or attached to the individual
    partitions.
  */
  int (*ic_add_field) (IC_ALTER_TABLE *alter_table,
                       const gchar *field_name,
                       IC_FIELD_TYPE field_type,
                       guint32 field_size,
                       gboolean is_nullable,
                       gboolean is_disk_stored);

  int (*ic_set_charset) (IC_ALTER_TABLE *alter_table,
                         const gchar *field_name,
                         guint32 cs_id);

  int (*ic_set_decimal_field) (IC_ALTER_TABLE *alter_table,
                               const gchar *field_name,
                               guint32 scale,
                               guint32 precision);

  int (*ic_set_signed) (IC_ALTER_TABLE *alter_table,
                        const gchar *field_name,
                        gboolean is_signed);

  /* One or more fields can be dropped in an alter table operation */
  int (*ic_drop_field) (IC_ALTER_TABLE *alter_table,
                        const gchar *field_name);

  /*
    Indexes are added as part of create table, or as part of an alter table.
  */
  int (*ic_add_index) (IC_ALTER_TABLE *alter_table,
                       const gchar *index_name,
                       const gchar **field_names,
                       guint32 num_fields,
                       IC_INDEX_TYPE index_type,
                       gboolean is_null_values_in_index);

  /* One or more indexes can be dropped in an alter table operation */
  int (*ic_drop_index) (IC_ALTER_TABLE *alter_table,
                        const gchar *index_name);

  /*
    It's possible to define partitioning as part of create table, it's
    an optional call as part of create table.
  */
  int (*ic_define_partitioning) (IC_ALTER_TABLE *alter_table,
                                 IC_PARTITION_TYPE partition_type,
                                 const gchar **field_names,
                                 guint32 num_fields,
                                 const gchar **partition_names,
                                 guint32 *map_partition_to_nodegroup,
                                 const gchar **map_partition_to_tablespace,
                                 guint32 num_partitions);

  /*
    The ordering of calls is that one starts by defining what type of
    operation we are performing (ic_create_table, ic_alter_table,
    ic_rename_table, ic_drop_table). After this one defines the
    fields added/dropped, indexes added/dropped and possible partitioning
    specification.

    So in the case of ic_create_table one defines things in the following
    order:
    ic_create_table (1 call)
    ic_add_field (1 call per field)
    ic_set_charset, ic_set_decimal_field, ic_set_signed for each field
      where it is needed.
    ic_add_index (1 call per index, at least one call for primary key index).

    In the case of ic_alter_table one defines thing in the following order.

    ic_alter_table (1 call)
    ic_add_field (1 call per new field with additional calls to ic_set_charset,
      ic_set_decimal_field, ic_set_signed where needed).
    ic_drop_field (1 call per dropped field)
    ic_add_index (1 call per added index)
    ic_drop_index (1 call per dropped index, not possible to drop primary key
      index)

    For ic_drop_table and ic_rename_table only one call is required.

    The actual creation, dropping, renaming and alteration of a table happens
    when the metadata transaction is committed.

    To create a table one needs to add at least one field, one also needs to
    add one primary key. It is also optional to define partitioning. No other
    things are needed or allowed to create a table.

    The tablespace name is required if the table uses disk data and no
    partitioning have been defined where each partition is mapped to a
    specific tablespace.
  */
  int (*ic_create_table) (IC_ALTER_TABLE *alter_table,
                          const gchar *schema_name,
                          const gchar *db_name,
                          const gchar *table_name,
                          const gchar *tablespace_name);

  /*
    Drop table is always called as the single method call, no more calls are
    needed to define dropping a table.
  */
  int (*ic_drop_table) (IC_ALTER_TABLE *alter_table,
                        const gchar *schema_name,
                        const gchar *db_name,
                        const gchar *table_name);

  /*
    Alter table adds or drops one component (field or indexes). If the
    transaction already contains a create table operation, then this operation
    can only add indexes. If the table already exists it is possible to add
    fields, drop fields, add indexes and drop indexes.
  */
  int (*ic_alter_table) (IC_ALTER_TABLE *alter_table,
                         const gchar *schema_name,
                         const gchar *db_name,
                         const gchar *table_name);

  /*
    Rename table is also a single call to define.
  */
  int (*ic_rename_table) (IC_ALTER_TABLE *alter_table,
                          const gchar *old_schema_name,
                          const gchar *old_db_name,
                          const gchar *old_table_name,
                          const gchar *new_schema_name,
                          const gchar *new_db_name,
                          const gchar *new_table_name);
};

struct ic_alter_tablespace_ops
{
  /*
    Initial size of the datafile created, this is used for tablespace
    and log file groups. One data file is created for each tablespace
    and log file group and additional can be created by using
    add data file functions.
   */
  int (*ic_set_initial_size) (IC_ALTER_TABLESPACE *alter_ts,
                              guint64 size);

  /* Memory buffer size for REDO or UNDO log file group.  */
  int (*ic_set_buffer_size) (IC_ALTER_TABLESPACE *alter_ts,
                             guint64 buf_size);

  /*
    Auto extend size is how much the tablespace will automatically extend
    itself when it becomes full.
  */
  int (*ic_set_autoextend_size) (IC_ALTER_TABLESPACE *alter_ts,
                                 guint64 auto_extend_size);

  /* The tablespace will never grow beyond its maximum size.  */
  int (*ic_set_max_size) (IC_ALTER_TABLESPACE *alter_ts,
                          guint64 ts_max_size);

  /*
    Extent size can only be set at creation time. Extent size is the
    size that each table allocates at a time from the tablespace.
    It is thus the allocation unit of the tablespace. It is also
    only possible to have one extent size per tablespace, so it's not
    possible to set this per data file.
  */
  int (*ic_set_extent_size) (IC_ALTER_TABLESPACE *alter_ts,
                             guint64 extent_size);

  /*
    Which nodegroup should the tablespace/log file group be created in.
    Default behaviour is that it is created in all nodegroups.
  */
  int (*ic_set_nodegroup) (IC_ALTER_TABLESPACE *alter_ts,
                           guint32 node_group);

  /*
    Create a tablespace with the given name and which will use the
    given Log File Group as REDO and UNDO log file. The data file
    name is the name of the initial file created for the tablespace.
    It is either an indirect filename based off of the NDB data
    directory or a full file name.
  */
  int (*ic_create_tablespace) (IC_ALTER_TABLESPACE *alter_ts,
                               const gchar *tablespace_name,
                               const gchar *data_file_name,
                               const gchar *log_filegroup_name,
                               const gchar *tablespace_comment);

  /*
    Log file groups can be either UNDO log files or REDO log files.
    One file is mapped to the log file group and this file isn't
    autoextendable, thus the initial size is the final size. It is
    however possible to add new log files to a log file group.
  */
  int (*ic_create_undo_log_file_group) (IC_ALTER_TABLESPACE *alter_ts,
                                        const gchar *log_file_group_name,
                                        const gchar *data_file_name,
                                        guint64 file_size,
                                        const gchar *comment);

  int (*ic_create_redo_log_file_group) (IC_ALTER_TABLESPACE *alter_ts,
                                        const gchar *log_file_group_name,
                                        const gchar *data_file_name,
                                        guint64 file_size,
                                        const gchar *comment);

  /* Drop the entire tablespace with all its data files */
  int (*ic_drop_tablespace) (IC_ALTER_TABLESPACE *alter_ts,
                             const gchar *tablespace_name);

  /* Drop the log file group and all its data files */
  int (*ic_drop_log_file_group) (IC_ALTER_TABLESPACE *alter_ts,
                                 const gchar *log_file_group_name);

  /*
    It's possible to add a datafile, before calling this function it is
    possible to set initial size, auto extend size, maximum size.
  */
  int (*ic_add_ts_data_file) (IC_ALTER_TABLESPACE *alter_ts,
                              const gchar *tablespace_name,
                              const gchar *data_file_name);

  int (*ic_add_lg_data_file) (IC_ALTER_TABLESPACE *alter_ts,
                              const gchar *log_file_group_name,
                              const gchar *data_file_name);

  /*
    It is possible to change maximum data file size, auto extend size and
    initial size for a data file.
  */
  int (*ic_alter_ts_data_file) (IC_ALTER_TABLESPACE *alter_ts,
                                const gchar *tablespace_name,
                                const gchar *data_file_name);

  int (*ic_set_tablespace_access_mode) (IC_ALTER_TABLESPACE *alter_ts,
                                      const gchar *tablespace_name,
                                      IC_TABLESPACE_ACCESS_MODE access_mode);
};

struct ic_apid_operation_ops
{
  /*
    The definition of an APID operation object is expected to be performed
    once and be reused many times. It's expected that the user have allocated
    a buffer, if the user don't want to handle this on his own, the table
    object have an operation to define a buffer using a bitmap of the fields
    to be involved in the buffer. There are also operations in this case to
    map the fields to offsets in the buffer.

    All fields which are larger than 256 bytes are allocated by the API and
    they are freed at the ic_reset_apid_op call or when ic_free_apid_op is
    called. There is also a specific call to transfer the "ownership" of a
    buffer to the API user for a specific field. In this case the API user
    will have the responsibility to free this memory later on.

    After the application have allocated a buffer on their own or

    Handling of normal-sized fields
    -------------------------------
    Define a field to be used in this operation. One must define the buffer
    offset (which is a byte offset) and a null offset (which is a bit offset).
    When creating the operation object one supplied a buffer reference and
    a table object. The table object could either be for a table or for
    an index. This means that the API can find out whether the field is part
    of the primary key (or unique key if unique index) and also the type
    and size of the field. When using the operation object for key access
    it's necessary to have defined all key fields. If this hasn't been done
    the key operations will fail on the operation object. Similarly the API
    will also verify that the buffer offsets used by the application isn't
    overlapping for different fields.

    The null offset is ignored for fields that are NOT NULL.

    The buffer offset is the offset of the pointer to the field data in
    the case of fields of size larger than 255 bytes.

    In the case that the API user created the buffer using the API, the
    buffer offset and null offset are ignored in this call. The API already
    knows about the offsets of the field in the buffer and the API user can
    get this data from the IC_TABLE_DEF object. In the special case when
    buffer is allocated by API and we're using all fields in the table we
    don't need this call and it will be rejected.
  */
  int (*ic_define_field) (IC_APID_OPERATION *apid_op,
                          guint32 field_id,
                          guint32 buffer_offset, /* in bytes */
                          guint32 null_offset); /* in bits */

  /*
    Handling of large fields (over 256 bytes)
    -----------------------------------------
    In order to read or write large fields we have to map a buffer for the
    field. This call is required for each query for each field larger than
    256 bytes. It is never necessary for fields smaller since they will
    always be updated fully and also will be entirely stored in the buffer.

    In the case of read the pointer to the buffer can be NULL. In this the
    API will allocate a buffer of appropriate size. The user can assume
    the ownership of this buffer later on if desired.

    In the case of writes the user must supply a buffer which is != NULL.

    In both the read and write case it is possible to define start_pos and
    end_pos to be only a part of the field. In this case it's necessary to
    set use_full_field to false. In this case start_pos and end_pos are
    both required to be set properly and the buffer size is
    (end_pos - start_pos) and only this part of the buffer is overwritten
    for writes and only this part is read for reads.
    
    If use_full_field is set the start_pos is always ignored and end_pos is
    read as buffer size. In this case the end_pos for reads is defined as
    the size of the buffer supplied. In the case of writes it is the new
    size of the field and the entire field in the NDB kernel will be
    overwritten.
  */
  int (*ic_define_pos) (IC_APID_OPERATION *apid_op,
                        guint32 field_id,
                        gchar *field_data,
                        gboolean use_full_field,
                        guint32 start_pos, /* in bytes */
                        guint32 end_pos);  /* in bytes */

  int (*ic_transfer_ownership) (IC_APID_OPERATION *apid_op,
                                guint32 field_id);

  /*
    Handling of Partitions Ids are used in query
    --------------------------------------------
    The tables in NDB kernel nodes are partitioned. This means that parts of
    the table are stored on different nodes. When issuing a query it is
    possible to restrict the query to only use one partition or a set of
    partitions. In the case of multiple partitions the function uses a
    bitmap where the Nth bit set to 0 means that partition N is to be part
    of the query.
  */
  int (*ic_set_partition_ids) (IC_APID_OPERATION *apid_op,
                               IC_BITMAP *used_partitions);
  int (*ic_set_partition_id) (IC_APID_OPERATION *apid_op,
                              guint32 partition_id);

  /*
    Handling of RANGE CONDITION objects
    -----------------------------------
    Operations can optionally define a range. This is only true for scan
    queries. Primary key and unique key operations cannot use a range since
    they will always fetch one row and only one row. There is also a function
    to ask the operation to retain the range also after completing the
    execution of a query to ensure that the range can be reused for the
    next query this operation object is used for. This function needs to be
    called every time a range is used as part of a query. So no specific call
    is needed to release a range, simply avoid calling ic_keep_range and the
    range will be released on the next completion of a query for this object.
  */
  IC_RANGE_CONDITION* (*ic_create_range_condition)
                             (IC_APID_OPERATION *apid_op);
  void (*ic_keep_range) (IC_APID_OPERATION *apid_op);

  /*
    Handling of WHERE CONDITION objects
    -----------------------------------
    All operations can optionally define a WHERE condition which is executed
    before the actual read or write is performed. One can create a WHERE
    condition from this object and it is also possible to reuse the WHERE
    condition object from the APID_GLOBAL object. There can only be one WHERE
    condition per query.

    It is possible to keep a WHERE condition object even after reset by
    calling ic_keep_where, this needs to be done for each query it's reused
    if needed for many queries in a row.
  */
  IC_WHERE_CONDITION* (*ic_create_where_condition)
                             (IC_APID_OPERATION *apid_op);
  int (*ic_map_where_condition) (IC_APID_OPERATION *apid_op,
                                 IC_APID_GLOBAL *apid_global,
                                 guint32 where_cond_id);
  void (*ic_keep_where) (IC_APID_OPERATION *apid_op);

  /*
    Handling of CONDITIONAL ASSIGNMENT objects
    ------------------------------------------
    Update operations can also have conditional assignments. There can be
    multiple such conditional assignment objects. Each conditional
    assignments sets the value of one field and thus one conditional
    assignment is needed for each field needing a conditional assignment.

    Conditional assignments are allocated in two steps. The first steps
    allocation makes it easy to access the conditional assignments, the
    second step does the actual creation of the individual conditional
    assignments.

    So if we define 3 conditional assignments we call
    ic_create_conditional_assignments first with num_cond_assigns set to 3.
    This means that we plan on defining 3 conditional assignments which
    will be numbered 0,1 and 2. Then we create conditional assignment
    objects by calling ic_create_conditional_assignment or by instead
    mapping a global conditional assignment using
    ic_map_conditional_assignment. Both of these functions will return the
    identity of the conditional assignment. If we used
    ic_create_conditional_assignment we also need to actually define the
    IC_CONDITIONAL_ASSIGNMENT object using the methods available for that.

    When a query is to be executed it will always check that all data
    structures related to the query is defined properly such that no
    half-baked data structures are used in the iClaustron Data API.

    It is possible to keep a conditional assignment object even after
    reset by calling ic_keep_conditional_assignments, this needs to
    be done for each query it's reused if needed for many queries in
    a row.
  */
  IC_CONDITIONAL_ASSIGNMENT** (*ic_create_conditional_assignments)
                               (IC_APID_OPERATION *apid_op,
                                guint32 num_cond_assigns);
  IC_CONDITIONAL_ASSIGNMENT* (*ic_create_conditional_assignment)
                               (IC_APID_OPERATION *apid_op,
                                guint32 cond_assign_id);
  int (*ic_map_conditional_assignment) (IC_APID_OPERATION *apid_op,
                                        IC_APID_GLOBAL *apid_global,
                                        guint32 loc_cond_assign_id,
                                        guint32 glob_cond_assign_id);
  void (*ic_keep_conditional_assignment) (IC_APID_OPERATION *apid_op);

  /*
    Handling of Errors
    ------------------
    In the case of an error on a query, an error object is available to
    the application. However to avoid having to fetch this object after
    completing each query it is possible to quickly check the variable
    named any_error on the IC_APID_OPERATION object. If this is set to
    FALSE then the query went through without any warnings or errors.

    If the variable is set one can get an error object and query this object
    for more information on the nature of the error.
  */
  IC_APID_ERROR* (*ic_get_error_object) (IC_APID_OPERATION *apid_op);

  /*
    Handling of Query Completion
    ----------------------------
    There are three ways to handle Query Completion.
    1) Don't do anything, operation object will be reset at close of the
       transaction.
    2) Call is_reset_apid_op after completing the query and before transaction
       is committed or aborted. Operation object can be immediately reused for
       the next query in the same or other transaction.
    3) Free operation object after completing query. This will release all
       memory allocated by the operation object and all objects owned created
       through this object including RANGE conditions, WHERE condition and
       CONDITIONAL ASSIGNMENT objects.

    A reset operation will also release memory in some cases where WHERE
    conditions and CONDITIONAL ASSIGNMENT objects have been allocated to
    the operation object and not been specified to be kept.

    To call reset before close of the transaction has the obvious advantage
    of releasing an operation object early on that can be immediately
    reused for other queries.
  */
  int (*ic_reset_apid_op) (IC_APID_OPERATION *apid_op);
  void (*ic_free_apid_op) (IC_APID_OPERATION *apid_op);
};

struct ic_range_condition_ops
{
  /*
    To define a range we call ic_define_range_part once for each field in the
    index. We need to define them in order from first index field. Thus
    field_id is mostly used to assert that the user knows the API. The
    definition of a range contains an upper range and a lower range. For
    bot the upper range and the lower range all parts except the last part
    must be an equality range.

    For the upper part of the range we can define the range type as:
    1) IC_RANGE_EQ
    2) IC_RANGE_NO_UPPER_LIMIT
    3) IC_RANGE_LT
    4) IC_RANGE_LE

    For the lower part of the range we can define the range type as follows:
    1) IC_RANGE_EQ
    2) IC_RANGE_NO_LOWER_LIMIT
    3) IC_RANGE_GT
    4) IC_RANGE_GE

    Some examples:
    a <= 1 would be translated to:
    IC_RANGE_GT with boundary value set to NULL on lower range part.
    IC_RANGE_LE with boundary value set to 1 on upper range part.
    The reason is that by definition in most databases NULL is not part
    of any range other than the equality range with NULL. The range interface
    will however not take care of such rules, it's the responsibility of the
    API user to enforce such rules.

    a == 1 would be translated to:
    IC_RANGE_EQ with boundary value set to 1 on both lower and upper range
    part. In this case the start_ptr needs to be equal to end_ptr and
    similarly start_len must be equal to end_len.

    a = 1 AND b <= 2 AND c = 2 translates to:
    IC_RANGE_EQ with boundary value set to 1 on both lower and upper range
    for the field a. IC_RANGE_GT with lower bound set to NULL on lower part of
    b and IC_RANGE_LE with upper bound set to 2. Since lower part of the
    field b is using IC_RANGE_GT the lower range on the field c will be
    ignored. However the upper bound using field c is still of interest.
    This will be set to IC_RANGE_EQ with boundary value set to 2. The check
    if a value is within this range is performed by first checking that the
    instantiation of (a,b,c) has a = 1, b > NULL, b <= 1 and c = 2. Thus if
    we scan an ordered index we will place the start of the scan at a = 1,
    b = NULL and the end of the scan at a = 1, b = 2 and c = 2. In this range
    we will skip the starting value and will also throw away all rows which
    don't have c = 2. If the c = 2 would have been replaced by c <= 2 the
    start and end point in the index would have been the same but records
    would be filtered out using c <= 2 instead of c = 2.

    a >= 1 is translated to:
    IC_RANGE_GE as lower bound with value 1, the upper bound is
    IC_RANGE_NO_UPPER_LIMIT, in this the value of end_ptr and end_len is
    ignored.

    As another example if we have an index on a,b and c. We can define a range
    based on the condition a = 1 AND b < 1. In this case the start part is
    a = 1 and the end part is a = 1, b = 1, with the range type on the
    a field equal to IC_RANGE_EQ for both lower and upper part and the upper
    range type on the b field equal to IC_RANGE_LT. To indicate that the
    b field is not involved in the start part we set start_ptr to NULL, in
    this case start_len is undefined.

    NULL values can be involved in a range as equality ranges. Such a
    condition is defined by setting start_ptr and end_ptr to NULL and in
    this case also range_type must be IC_RANGE_EQ, the value of start_len
    and end_len is undefined.

    A definition of a range can contain any number of IC_RANGE_EQ on the
    first set of fields. A definition of IC_RANGE_LT means that only the
    end part needs to be defined and IC_RANGE_LT is always the last field
    in the range definition since both the upper and lower parts have been
    fully defined. In the same manner IC_RANGE_GT requires only the start
    part to be defined and the field is always the last part of the range
    definition.
    
    Ranges are defined as part of the definition of the APID operation object
    and the range will be removed from the APID operation object immediately
    after completing the query. Thus ranges have to be defined for each
    individual query it's used in. It is possible to avoid this pattern by
    specifically calling ic_keep_ranges. The default is to use only one range
    and in this case no other calls are needed. It is also possible to define
    a scan over multiple ranges, in this case one needs to call ic_multi_range
    before defining the individual ranges. This call defines the number of
    ranges to be used. If this call isn't used the first call to
    ic_define_range_part will be interpreted also as a call to
    ic_multi_range with number of ranges equal to 1. The first range id
    is 0. The ranges should be defined in the order of increasing range ids.
  */

  /*
    For a range consisting of several subranges it's necessary to define
    the number of ranges before defining the ranges. For ranges consisting
    of only one range this call will be implicit at the first
    ic_define_range_part call.
  */
  int (*ic_multi_range) (IC_RANGE_CONDITION *range,
                         guint32 num_ranges);

  /* Define one subrange */
  int (*ic_define_range_part) (IC_RANGE_CONDITION *range,
                               guint32 range_id,
                               guint32 field_id,
                               gchar *start_ptr,
                               guint32 start_len,
                               gchar* end_ptr,
                               guint32 end_len,
                               IC_BITMAP *partition_bitmap,
                               IC_LOWER_RANGE_TYPE lower_range_type,
                               IC_UPPER_RANGE_TYPE upper_range_type);

  /* Free the range object */
  void (*ic_free_range_cond) (IC_RANGE_CONDITION *range);
};

struct ic_where_condition_ops
{
  /*
   In the iClaustron it is possible to define WHERE conditions for each
   query. This WHERE condition can be a combination of AND, OR, XOR and
   NOT of other conditions. It is possible to define conditions using the
   ==, <, <=, >, >=, != operators.

   The execution model of the WHERE condition uses memory addresses, these
   memory addresses are generated by the API, thus the API will take care
   of efficient use of memory. At start of execution of the WHERE condition
   the interpreter that executes the WHERE condition will ensure that
   sufficient memory is available to execute the WHERE condition. The WHERE
   condition can be executed both in the API and in the NDB kernel.

   The definition of WHERE condition starts by using a special handling of
   boolean conditions like AND/OR/XOR. The reason is to enable optimisations
   where it is only necessary to evaluate the left branch of the boolean
   condition to calculate the result of the boolean condition. Thus in an
   AND condition it is sufficient to evaluate the left branch if the left
   branch evaluates to FALSE, we only need to evaluate the right branch if
   the left branch is TRUE. For OR conditions the opposite holds, if the
   left branch is TRUE it is clear that the OR condition is TRUE whereas
   a FALSE in the left branch requires another step of evaluation.

   To be able to define this easily in walking the WHERE tree we provide the
   boolean condition as two special subroutines that return a condition id.
   Thus when we encounter a boolean condition in the tree we inject the code
   for it immediately, we then pass the subroutine id to the instructions
   on both sides.

   The definition of WHERE condition is supposed to be done by going through
   the condition tree from left-deep to right. Thus to evaluate the condition
   WHERE a = 1 AND b < (c + 3) the tree will look like:

                     ---------
                     | AND   |
                     ---------
                     |       |
                   |           |
                 |               |
              -------         -------
              | EQ  |         | LT  |
              -------         -------
              |     |         |     |
            |         |     |         |
          |             | |             |
        -----         --- ------      ------
        | a |         |1| | b  |      | +  |
        -----         --- ------      ------
                                      |    |
                                    |         |
                                  |              |
                                -----          -----
                                | c |          | 3 |
                                -----          -----

  To evaluate this we start at the AND condition and generate the AND
  statement that uses two subroutines, one for the left branch and one
  for the right branch. The initial subroutine id is always 0, this
  subroutine id indicates the top-level routine. Next we go left-deep
  from AND to EQ to a, so we start by reading the field into memory address
  0, next we load the constant 1 into the next memory address (dependent on
  size of a). Next step is to perform the boolean EQ operation on the memory
  loaded from a and the memory where 1 was loaded.  The result of this
  comparison will end up in condition id 0.  Next we follow the right branch
  in the AND left-deep and load b into the next memory address (here the API
  could decide to start from memory address 0 again since we already used the
  variables on the left side of the AND to calculate condition id 0. Whether
  the API does this or not is dependent on how advanced the memory management
  algorithm the API uses. Next it it follows the right side of the LT
  left-deep and loads c into the next memory address and next loads 3 into the
  memory address. The operation + is performed and the result placed in the
  next memory address or reusing a memory address used to load c and/or 3.
  Then the condition id 1 is derived as the result of the comparison of
  b and (c + 3). Finally the result is TRUE of both condition id 0 and
  condition id 1 is TRUE and the result is stored in condition id 2 
  (or in condition id 0 if reusing condition id's). The result of the WHERE
  condition is always the result of the initial operation. Thus if we have no
  top boolean condition we use a special condition statement that uses one
  special subroutine to evaluate its condition.

  The observation that comparator's like NE, EQ, LE, LT, GE, GT can only feed
  into boolean condition gives us the further simplification that subroutines
  will always end with a ic_define_condition, thus we can make the end of
  subroutine here implicit, it is always the case.

  Another implicit derivation is that the first statement is always the
  statement which evaluates the WHERE condition. Thus the record will
  automatically if the initial boolean condition returns FALSE.

  A call to ic_define_boolean is also an implicit return from a subroutine.
  If it is the first call it is the return of the WHERE condition, in other
  cases it always feeds into another boolean condition and thus is the first
  and also only instruction in the subroutine.

  The first call can be ic_define_boolean in the case where the top condition
  is a boolean condition, ic_define_not if the condition is a NOT condition
  and otherwise if the top condition is a condition the first call is always
  ic_define_first.

  ic_define_regexp and ic_define_like have the same logic as
  ic_define_condition in the sense that they are always the return from the
  subroutine.

  Thus the call chain will be (in pseudocode):
    current_subroutine_id= 0;
    ic_define_boolean(current_subroutine_id,
                      &left_subroutine_id,
                      &right_subroutine_id,
                      "AND");
    ic_read_field_into_memory("a", left_subroutine_id, &a_address);
    ic_read_const_into_memory(1, left_subroutine_id, &1_address);
    ic_define_condition("EQ", left_subroutine_id, a_address, 1_address);

    ic_read_field_into_memory("b", right_subroutine_id, &b_address);
    ic_read_field_into_memory("c", right_subroutine_id, &c_address);
    ic_read_const_into_memory(3, right_subroutine_id, &3_address);
    ic_define_calculation("+", right_subroutine_id, &res1_address,
                          c_address, 3_address);
    ic_define_condition("LT", right_subroutine_id, b_address, res1_address);

  An execution model based on memory is used to make it easy to map condition
  trees to an interpreted program that can be sent to the NDB data nodes or
  executed in the API.
  
  The Data API also gives the option of compiling the interpreted program into
  a more efficient representation (potentially even assembler code used in the
  machine where it is executed).

  If a WHERE condition is heavily used in many queries the API provides the
  capability to store the definition of the WHERE condition. The WHERE
  condition can be stored in the API, but could also potentially be stored in
  the NDB data nodes.

  If the user wants to store a condition globally accessible to all
  connections in the API it should be created using the
  ic_create_where_condition method on the IC_APID_GLOBAL object. In this
  case it is sufficient to map the where condition to a IC_APID_OPERATION
  object, otherwise one needs to call the ic_create_where_condition method
  on the IC_APID_OPERATION object.

  Sometime in the future it might also be possible to store where conditions
  in the Data nodes in the clusters. To support this we have a method on the
  object to store conditions in a cluster.
  */

  /* Create a BOOLEAN expression (AND, OR, XOR) between two subroutines */
  int (*ic_define_boolean) (IC_WHERE_CONDITION *cond,
                            guint32 current_subroutine_id,
                            guint32 *left_subroutine_id,
                            guint32 *right_subroutine_id,
                            IC_BOOLEAN_TYPE boolean_type);

  /*
    A condition (=, !=, <, <=, >, >=) between two memory address, the result
    is the result of a subroutine. The memory address is typed, so it
    remembers what the type of the constant or field it was created with.
  */
  int (*ic_define_condition) (IC_WHERE_CONDITION *cond,
                              guint32 current_subroutine_id,
                              guint32 left_memory_address,
                              guint32 right_memory_address,
                              IC_COMPARATOR_TYPE comp_type);

  /* Read a field into a memory address */
  int (*ic_read_field_into_memory) (IC_WHERE_CONDITION *cond,
                                    guint32 current_subroutine_id,
                                    guint32 *memory_address,
                                    guint32 field_id);

  /* Read a constant into memory, specify constant data type */
  int (*ic_read_const_into_memory) (IC_WHERE_CONDITION *cond,
                                    guint32 current_subroutine_id,
                                    guint32 *memory_address,
                                    gchar *const_ptr,
                                    guint32 const_len,
                                    IC_FIELD_TYPE const_type);

  /*
    Calculate the result of a mathematical operation on two memory
    addresses and store result in a third memory adresses which is
    generated by function. The mathematical operation can be any of
    +, -, *, /. It doesn't return from the subroutine.
  */
  int (*ic_define_calculation) (IC_WHERE_CONDITION *cond,
                                guint32 current_subroutine_id,
                                guint32 *returned_memory_address,
                                guint32 left_memory_address,
                                guint32 right_memory_address,
                                IC_CALCULATION_TYPE calc_type);

  /*
    Take the result from one subroutine and invert it, returns from
    subroutine.
  */
  int (*ic_define_not) (IC_WHERE_CONDITION *cond,
                        guint32 current_subroutine_id,
                        guint32 *subroutine_id);

  /*
    Create top level condition when top-level is a normal condition
    and no inversion of condition is needed. Always used at top level
    node in code generated.
   */
  int (*ic_define_first) (IC_WHERE_CONDITION *cond,
                          guint32 current_subroutine_id,
                          guint32 *subroutine_id);

  /*
    A special type of condition where we do a regular expression on a
    field, the reg_exp_memory contains the regular expression which
    needs to be loaded as constant first. Can be applied on part or
    full size of field.
  */
  int (*ic_define_regexp) (IC_WHERE_CONDITION *cond,
                           guint32 current_suroutine_id,
                           guint32 field_id,
                           guint32 start_pos,
                           guint32 end_pos,
                           guint32 reg_exp_memory);

  /* Same ic_define_regexp but a LIKE condition instead */
  int (*ic_define_like) (IC_WHERE_CONDITION *cond,
                         guint32 current_subroutine_id,
                         guint32 field_id,
                         guint32 start_pos,
                         guint32 end_pos,
                         guint32 like_memory_address);

  /*
    End of WHERE condition, evaluate it, if true the record will
    be returned to application or assignment will proceed (if used in
    conditional assignment).
  */
  int (*ic_evaluate_where) (IC_WHERE_CONDITION *cond);

  /* FUTURE: Store condition in cluster */
  int (*ic_store_where) (IC_WHERE_CONDITION *cond,
                                   guint32 cluster_id);

  /* Free WHERE condition */
  int (*ic_free_where) (IC_WHERE_CONDITION *cond);
};

struct ic_conditional_assignment_ops
{
  /*
    Conditional assignment can be used to perform some calculations on the
    row to be updated and even make the update of a field conditional upon
    a where condition.

    The final method call defining a conditional assignment is always
    ic_write_field_into_memory. Before this call at a minimum we need to
    assign something to the memory address used in this call. This memory
    address can be prepared by loading constants into memory and also by
    performing operations on constants and field values.

    We can also make the entire aic_define_partitioningssignment conditional, to do this, we reuse
    the IC_WHERE_CONDITION class. Thus a generic WHERE condition can be
    executed before we perform the assignment. If no WHERE condition is used
    the assignment is unconditional. For an API operation it is possible to
    define any number of assignments (withing reasonable limits set by how
    large the interpreted program is allowed to be). Each of those assignments
    will see the results of the previous assignments. Thus if we have two
    assignments: a = a + 1, b = a + 1 and a=b=1 before the assignment then
    first the value of a will be set to 1 + 1 = 2, whereafter b is assigned
    the new value of a = 2 plus one, thus 3. So a = 2, b = 3 after these two
    assignments. There can be several conditional assignments to the same
    field, the last assignment will be the one prevailing.

    An example of a conditional assignment is:
    a = a * (b + 1)
    
    When mapping this into a conditional assignment we use the same technique
    as we did for WHERE conditions using left-deep traversal of the
    assignment tree. The tree for the above assignment is:

                   -------
                   |  =  |
                   -------
                   |     |
                 |         |
               |             |
            -------       -------
            |  a  |       |  *  |
            -------       -------
                          |     |
                        |         |
                      |              |
                   ------          -----
                   | a  |          | + |
                   ------          -----
                                   |   |
                                 |       |
                               |           |
                            ------      -------
                            | b  |      |  1  |
                            ------      -------

    The top node of an assignment tree is always an assignment operator. The
    left-side node is always the field which to assign the value to. Thus we
    can immediately start on the right side of the assignment tree.

    We go left-deep on the right side of the tree and where we find a read of
    the field a into memory address 0. Next we move over to the right side
    of the * operator where we read the field b into a memory address. Next we
    move on to move 1 into a memory address. Now we are prepared to sum the
    memory addresses of b and 1 into a memory address whereafter we multiply
    the memory address of a and the memory address of the addition and store
    the result in a memory address. This final memory address is the one
    to be used in the ic_write_field_into_memory.

    Thus the call chain will be (in pseudocode):
      ic_read_field_into_memory("a", &a_address);
      ic_read_field_into_memory("b", &b_address);
      ic_read_const_into_memory(1, &1_address);
      ic_define_calculation("+", &res1_address, b_address, 1_address);
      ic_define_calculation("*", &res2_address, a_address, res1_address);
      ic_write_field_into_memory("a", res2_address);

    Conditional assignments can either be defined dynamically per API
    operation it's needed in. It could also be defined globally on the
    IC_APID_GLOBAL object from where it can be reused by any query using
    the IC_APID_GLOBAL instance.
  */

  /*
    Conditional assignments can have one WHERE condition attached to them.
    If no where condition is attached, the assignment is made unconditional.
  */
  IC_WHERE_CONDITION* (*ic_create_assignment_condition)
                       (IC_CONDITIONAL_ASSIGNMENT *cond_assign);

  /* Assignment can be reused from a global pool */
  int (*ic_map_assignment_condition) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                      IC_APID_GLOBAL *apid_global,
                                      guint32 cond_assignment_id);

  /* Read a field into a memory address */
  int (*ic_read_field_into_memory) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                    guint32 *memory_address,
                                    guint32 field_id);

  /* Read a constant into a memory address */
  int (*ic_read_const_into_memory) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                    guint32 *memory_address,
                                    gchar *const_ptr,
                                    guint32 const_len,
                                    IC_FIELD_TYPE const_type);

  /*
    Perform a mathematical operation on two memory addresses which are
    equipped with type information and store result in a third memory
    address.
  */
  int (*ic_define_calculation) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                guint32 *returned_memory_address,
                                guint32 left_memory_address,
                                guint32 right_memory_address,
                                IC_CALCULATION_TYPE calc_type);

  /*
    The actual assignment which is made dependent on the WHERE condition
    and the data written is the result of calculations on fields in the
    table as well as constants.
  */
  int (*ic_write_field_into_memory) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                     guint32 memory_address,
                                     guint32 field_id);

  /* Free the conditional assignment object */
  int (*ic_free_cond_assign) (IC_CONDITIONAL_ASSIGNMENT **cond_assign);
};

struct ic_apid_connection_ops
{
  /*
    The table operation part of the iClaustron Data API have 4 parts.
    1) Normal key and scan operations
    2) Transaction start, join, commit and rollback
    3) Send and poll connections to NDB
    4) Iterate through received operations from NDB
  */
  /*
    Insert, Update, Delete and Write operation
    This method is used to send a primary key or unique key operation
    towards NDB. It will not actually send the operation, it will only
    define the operation for later sending and execution. Before calling
    this function the IC_APID_OPERATION needs to be properly set-up for
    the query, this object defines what query to execute. The 
    IC_APID_CONNECTION provides a communication channel to this thread
    in the API, the IC_TRANSACTION puts the query in a transaction
    context. The IC_WRITE_KEY_OP specifies the type of operation we are
    performing.

    The normal use of the API is to provide a callback function together
    with a user object that will be called when the query has completed
    (when this callback is called it carries the outcome of the query,
    whether it was successful and any read data will be available in the
    buffer set-up by the IC_APID_OPERATION object.

    It is also possible to avoid the use of a callback function, in this
    case the callback function is sent as NULL and the user data is ignored,
    in this case the API will execute a default callback function. In this
    case one has to use the IC_APID_OPERATION to get the result of the query
    execution after returning from either the ic_poll or ic_flush call.
   */
  int (*ic_write_key)
                /* Our thread representation */
               (IC_APID_CONNECTION *apid_conn,
                /* Our operation object */
                IC_APID_OPERATION *apid_op,
                /* Transaction object, can span multiple threads */
                IC_TRANSACTION *transaction_obj,
                /* Type of write operation */
                IC_WRITE_KEY_OP write_key_op,
                /* Callback function */
                IC_APID_CALLBACK_FUNC callback_func,
                /* Any reference the user wants to pass to completion phase */
                void *user_reference);

  /*
    Read key operation
    This method is used to perform primary key or unique key operations
    toward NDB. Will not send the operation, only define it.

    It works in exactly the same manner as ic_write_key_access except that it
    defines a read operation instead.
  */
  int (*ic_read_key)
                /* Our thread representation */
               (IC_APID_CONNECTION *apid_conn,
                /* Our operation object */
                IC_APID_OPERATION *apid_op,
                /* Transaction object, can span multiple threads */
                IC_TRANSACTION *transaction_obj,
                /* Type of read operation */
                IC_READ_KEY_OP read_key_op,
                /* Callback function */
                IC_APID_CALLBACK_FUNC callback_func,
                /* Any reference the user wants to pass to completion phase */
                void *user_reference);
  /*
    Scan table operation
    This method is used to scan a table, one can either scan a non-unique
    index or perform a full table scan. It is possible to limit the scan
    to a subset of the partitions used in the table.
  */
  int (*ic_scan)
                /* Our thread representation */
               (IC_APID_CONNECTION *apid_conn,
                /* Our operation object */
                IC_APID_OPERATION *apid_op,
                /* Transaction object, can span multiple threads */
                IC_TRANSACTION *transaction_obj,
                /* Type of scan operation */
                IC_SCAN_OP scan_op,
                /* Callback function */
                IC_APID_CALLBACK_FUNC callback_func,
                /* Any reference the user wants to pass to completion phase */
                void *user_reference);

  /*
    A metadata transaction object is needed to create and alter metadata in
    the iClaustron API. It's also necessary to bind to metadata objects.
    
    A metadata transaction is only performed on one cluster in the iClaustron
    grid.
  */
  IC_APID_ERROR*
      (*ic_start_metadata_transaction)
                              (IC_APID_CONNECTION *apid_conn,
                               IC_METADATA_TRANSACTION *md_trans_obj,
                               guint32 cluster_id);

  /*
    This method is used to create a new transaction object
    A transaction can be joinable from another thread, however to
    avoid unnecessary overhead for simple transactions we add
    a flag that needs to be set for joinable transactions.

    When starting a transaction we can add a transaction hint object
    that is used to hint which NDB node to place the transaction
    coordinator in. If this object isn't used (== NULL) then a round
    robin scheme will be used.

    Start transaction is performed immediately, the transaction object can
    be used immediately. The actual transaction start happens in the NDB
    kernel, this will however happen as part of the first operation of the
    transaction.

    Transactions can only occur within one cluster. It's also necessary to
    only use metadata objects belonging to this cluster in all operations of
    the transaction.

    It is possible to process many transactions on the same or different
    clusters in parallel, thus one ic_send operation can send transaction
    operations to many clusters.
  */
  IC_APID_ERROR*
      (*ic_start_transaction) (IC_APID_CONNECTION *apid_conn,
                               IC_TRANSACTION **transaction_obj,
                               IC_TRANSACTION_HINT *transaction_hint,
                               guint32 cluster_id,
                               gboolean joinable);

  /*
    To be able to use a transaction with this Data API connection we need
    to specifically join the transaction. The transaction must have been
    started in another thread in this node, the transaction_obj is the
    reference to the transaction object that the start_transaction call
    returned in this thread.
  */
  IC_APID_ERROR*
      (*ic_join_transaction) (IC_APID_CONNECTION *apid_conn,
                              IC_TRANSACTION *transaction_obj,
                              void *user_reference);

  /*
    Commit the transaction
    This merely indicates a wish to commit the transaction. The actual
    commit is performed by NDB and thus the outcome of the commit is
    not known until we have sent and polled in all operations of the
    transaction.
  */
  IC_APID_ERROR*
      (*ic_commit_transaction) (IC_APID_CONNECTION *apid_conn,
                                IC_TRANSACTION *transaction_obj,
                                /* Callback function */
                                IC_APID_CALLBACK_FUNC callback_func,
                                /* User data to callback function */
                                void *user_reference);

  /*
    Abort the transaction
    Again only an indication. In this case the outcome is though certain
    since the transaction is only committed if specifically committed and
    the commit was successful.
  */
  IC_APID_ERROR*
      (*ic_rollback_transaction) (IC_APID_CONNECTION *apid_conn,
                                  IC_TRANSACTION *transaction_obj,
                                  /* Callback function */
                                  IC_APID_CALLBACK_FUNC callback_func,
                                  /* User data to callback function */
                                  void *user_reference);

  /*
    Create a savepoint in the transaction
    This creates a savepoint which we can roll back to. The actual
    creation of the savepoint happens in NDB but unless the
    transaction is aborted it is successful. There is no specific
    error reporting from this call.
  */
  IC_APID_ERROR*
      (*ic_create_savepoint) (IC_APID_CONNECTION *apid_conn,
                              IC_TRANSACTION *transaction_obj,
                              IC_SAVEPOINT_ID *savepoint_id);

  /*
    Rollback to savepoint.
    This is an indication to NDB what to do. Rollback to a savepoint
    will remove all operations already defined and not sent. No
    more operations will be accepted on the transaction until the
    rollback to savepoint is completed in NDB.
  */
  IC_APID_ERROR*
      (*ic_rollback_savepoint) (IC_APID_CONNECTION *apid_conn,
                                IC_TRANSACTION *transaction_obj,
                                IC_SAVEPOINT_ID savepoint_id,
                                /* Callback function */
                                IC_APID_CALLBACK_FUNC callback_func,
                                /* User data to callback function */
                                void *user_reference);

  /*
    Check for new operations received. If wait_time > 0 we will wait for
    the number of milliseconds given for new operations.
    If ic_poll returns 0 it means we have received operations that have
    completed their operations and need action from user.
    There are three potential outcomes of this call.
    1) We call it and nothing has arrived for the time that we have
       specified. The method will return IC_ERROR_TIMEOUT in this
       case.
    2) We call it and something arrived but dependent actions had been
       defined for the received operations, so no action is required.
       In this case we return, and when we call ic_get_next_operation
       there will be no operation to handle.
    3) We call and it something arrived that require our attention.
       We return 0 and there will be operations returned from the
       ic_get_next_operation iterator.
  */
  IC_APID_ERROR* (*ic_poll) (IC_APID_CONNECTION *apid_conn, glong wait_time);

  /*
    Send all operations that we have prepared in this connection.
    If force_send is FALSE, we will use an adaptive algorithm which keeps
    track of the mean value and standard deviation on how long time there is
    between sends to a specific node in the cluster. If the expected time to
    wait is less than a specified time in the cluster configuration, then we
    will wait for another message to arrive before sending. When the next
    message arrives we will estimate whether it's likely that another message
    will arrive before the specified time since the first message arrived
    have expired.
  */
  IC_APID_ERROR* (*ic_send) (IC_APID_CONNECTION *apid_conn,
                             gboolean force_send);

  /*
    Send operations and wait for operations to be received. This is
    equivalent to send + poll. The return code is either an error
    found already at the send phase and if the send is successful
    it will report in the same manner as poll.
  */
  IC_APID_ERROR* (*ic_flush) (IC_APID_CONNECTION *apid_conn,
                              glong wait_time,
                              gboolean force_send);

  /*
    After returning 0 from flush or poll we use this iterator to get
    all operations that have completed. It will return one IC_APID_OPERATION
    object at a time until it returns NULL after which no more operations
    exists to report from the poll/flush call.
  */
  IC_APID_OPERATION* (*ic_get_next_executed_operation)
                              (IC_APID_CONNECTION *apid_conn);
  /* Get global data object for Data API */
  IC_APID_GLOBAL* (*ic_get_apid_global) (IC_APID_CONNECTION *apid_conn);
  /* Get config client object */
  IC_API_CONFIG_SERVER* (*ic_get_api_config_server)
                              (IC_APID_CONNECTION *apid_conn);
  /* Free all objects connected to this connection.  */
  void (*ic_free_apid_connection) (IC_APID_CONNECTION *apid_conn);
};

struct ic_transaction_ops
{
  /*
    This method iterates over all ongoing operations that still haven't
    been executed in the cluster. They have though been sent, so this
    means that they are awaiting a response from the cluster.
    It is both a get first and get next iterator. Get first semantics one
    gets by calling with apid_op = NULL and get next by sending in the
    apid_op from the previous call into apid_op.

    It will only list the operations connected to this operation. There
    might be more than one connection used in a transaction.

    All parameters are IN-parameters
  */
  IC_APID_OPERATION* (*ic_get_ongoing_op) (IC_TRANSACTION *trans_obj,
                                           IC_APID_CONNECTION *apid_conn,
                                           IC_APID_OPERATION *apid_op);
};

struct ic_transaction_hint_ops
{
  int (*ic_define_hint) (IC_TRANSACTION_HINT *trans_hint);
};

struct ic_apid_error_ops
{
  /*
    The error object is expected to be a part of another object and thus
    have no own create methods. It does have a initialisation method which
    is called when the user explicitly wants to reset the error object to
    indicate no error.
  */
  /* Initialise error object */
  void (*ic_init_apid_error) (IC_APID_ERROR *apid_error);

  /* Get error code */
  int (*ic_get_apid_error_code) (IC_APID_ERROR *apid_error);

  /* Get error string */
  const gchar* (*ic_get_apid_error_msg) (IC_APID_ERROR *apid_error);

  /* Get category of error */
  IC_ERROR_CATEGORY (*ic_get_apid_error_category) (IC_APID_ERROR *apid_error);

  /* Get severity level of the error */
  IC_ERROR_SEVERITY_LEVEL
    (*ic_get_apid_error_severity) (IC_APID_ERROR *apid_error);
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
  of the Data API.
*/

struct ic_apid_global_ops
{
  /*
    The most important functions for normal user threads is to use the
    ic_get_stop_flag and ic_set_stop_flag functions. The ic_get_stop_flag
    should be called regularly to check for other threads wanting a global
    stop to the user API. The ic_set_stop_flag is used to indicate that the
    user has decided to stop the iClaustron Data API and is now in close
    down operation.

    Also each thread should call ic_wait_first_node_connect at the beginning
    on each cluster that the thread is expecting to use to ensure that there
    is at least one connection to the cluster to be used. If there is no
    connection to a cluster all operations towards that cluster will fail.

    Most other functions are used by the ic_start_apid_program and
    ic_run_apid_programs to handle additions of user threads, removal of user
    threads, setting and getting thread functions. There is also a function
    to give a connection from the iClaustron Cluster Server over to the
    iClaustron Data API, this is only used by the iClaustron Cluster Server.
  */

  /* This function can be waited to get start/stop events from the API */
  void (*ic_cond_wait) (IC_APID_GLOBAL *apid_global);

  /* Check of the API is to be stopped */
  gboolean (*ic_get_stop_flag) (IC_APID_GLOBAL *apid_global);

  /* Indicate that the user wants to stop the Data API */
  void (*ic_set_stop_flag) (IC_APID_GLOBAL *apid_global);

  /* Get the number of threads currently active in the Data API */
  guint32 (*ic_get_num_user_threads) (IC_APID_GLOBAL *apid_global);

  /* Add a new user thread to the Data API */
  void (*ic_add_user_thread) (IC_APID_GLOBAL *apid_global);

  /* Remove a user thread from the Data API */
  void (*ic_remove_user_thread) (IC_APID_GLOBAL *apid_global);

  /* Set thread function of the Data API */
  void (*ic_set_thread_func) (IC_APID_GLOBAL *apid_global,
                              IC_RUN_APID_THREAD_FUNC apid_func);

  /* Get thread function of the Data API */
  IC_RUN_APID_THREAD_FUNC (*ic_get_thread_func) (IC_APID_GLOBAL *apid_global);

  /*
    Method used by iClaustron Cluster Server to deliver IC_CONNECTION object
    to the Data API to be reused as node connection.
  */
  int (*ic_external_connect) (IC_APID_GLOBAL *apid_global,
                              guint32 cluster_id,
                              guint32 node_id,
                              IC_CONNECTION *conn);

  /*
    Wait until at least we're connected to at least one node in the cluster.
    Before this can return OK all operations towards the cluster will fail.
  */
  int (*ic_wait_first_node_connect) (IC_APID_GLOBAL *apid_global,
                                     guint32 cluster_id,
                                     glong wait_time); /* in milliseconds */

  /* Free the IC_APID_GLOBAL object */
  void (*ic_free_apid_global) (IC_APID_GLOBAL *apid_global);
};

/*
  CREATE METHODS
  --------------

  These are methods to create the global object, the connection object to be
  used in a specific thread and the operation object used to handle a certain
  low-level query. All other data structures are allocated as part of the
  interfaces to these objects.

  ic_create_apid_global
  ---------------------
  Method that creates the global APID object, this uses a configuration
  object already created. In the special case of the Cluster Server the
  connections are transferred from the Configuration part to the Data
  API.

  This object is created by the ic_start_apid_program for a standard
  iClaustron Data API application.

  ic_create_apid_connection
  -------------------------
  Method to create the APID connection object, each object connects either
  to all clusters or to a subset of the clusters. An APID connection is
  normally mapped to usage within one thread. Thus the API user must
  protect it with a mutex if it's used on more than one thread at a time.
  
  This function will be called by ic_run_apid_program once for each thread
  the user has decided to start. In this manner the standard use of the
  iClaustron Data API will always have one such object per thread as is
  intended.

  ic_create_apid_operation
  ------------------------
  Method to create APID operation object, it is connected to the global
  APID object but it's ok to use an operation on any APID connection.
  An APID object needs to be connected to a table object and a buffer
  is also connected to the APID operation object.

  Creation of these objects and deciding whether to use a global pool or
  a local pool per thread is a decision by the user, or even to create
  and destroy these objects per query.
*/
IC_APID_GLOBAL* ic_create_apid_global(IC_API_CONFIG_SERVER *apic,
                                      gboolean use_external_connect,
                                      int *ret_code,
                                      gchar **err_str);

IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *apid_global,
                          IC_BITMAP *cluster_id_bitmap);

IC_APID_OPERATION*
ic_create_apid_operation(IC_APID_GLOBAL *apid_global,
                         IC_TABLE_DEF *table_def,
                         /* == 0 means all fields */
                         guint32 num_fields,
                         /* == TRUE means API allocated buffer */
                         gboolean is_buffer_allocated_by_api,
                         gchar *data_buffer,
                         guint32 buffer_size, /* in bytes */
                         guint8 *null_buffer,
                         guint32 null_buffer_size, /* in bytes */
                         int *error);

IC_METADATA_TRANSACTION*
ic_create_metadata_transaction(IC_APID_GLOBAL *apid_global,
                               IC_APID_CONNECTION *apid_conn,
                               guint32 cluster_id);

/*
  EXTERNALLY VISIBLE DATA STRUCTURES
  ----------------------------------
*/

struct ic_apid_error
{
  IC_APID_ERROR_OPS *apid_error_ops;
};

struct ic_apid_connection
{
  IC_APID_CONNECTION_OPS *apid_conn_ops;
};

struct ic_apid_global
{
  IC_APID_GLOBAL_OPS *apid_global_ops;
  IC_METADATA_BIND_OPS *apid_metadata_ops;
  IC_BITMAP *cluster_bitmap;
};

struct ic_table_def
{
  IC_TABLE_DEF_OPS *table_def_ops;
};

struct ic_index_def
{
  IC_INDEX_DEF_OPS *index_def_ops;
};

struct ic_conditional_assignment
{
  IC_CONDITIONAL_ASSIGNMENT_OPS *cond_assign_ops;
};

struct ic_range_condition
{
  IC_RANGE_CONDITION_OPS *range_ops;
};

struct ic_where_condition
{
  IC_WHERE_CONDITION_OPS *cond_ops;
};

struct ic_apid_operation
{
  IC_APID_OPERATION_OPS *apid_op_ops;
  gboolean any_error;
};

struct ic_metadata_transaction
{
  IC_METADATA_TRANSACTION_OPS *md_trans_ops;
};

struct ic_alter_table
{
  IC_ALTER_TABLE_OPS *alter_table_ops;
};

struct ic_alter_tablespace
{
  IC_ALTER_TABLESPACE_OPS *alter_tablespace_ops;
};

struct ic_transaction
{
  IC_TRANSACTION_OPS *trans_ops;
  guint64 transaction_id;
};

struct ic_transaction_hint
{
  IC_TRANSACTION_HINT_OPS *trans_hint_ops;
  guint32 cluster_id;
  guint32 node_id;
};

/*
  Declarations used for programs using easy start and stop
  of iClaustron Data API users. Also declaration of global
  variables used by these programs.

  Programming a standard application using the iClaustron is very
  straightforward.

  1) Call ic_start_program
     This call initialises all iClaustron data structures and parses the
     command line options.
  2) Call ic_start_apid_program
     This call connects to iClaustron Cluster Server to fetch the
     configuration of the iClaustron Grid. It does also initialise the
     iClaustron Data API and starts connection attempts to all the
     nodes in all clusters in the iClaustron Grid. It also initialises
     the thread pool object used by the iClaustron API program.
  3) Call in_run_apid_program
     This call starts all user threads as specified by the user command line
     option "num_threads". The threads will run until stopped by some event
     decided by the API program. When the threads decide to stop the program
     the above method will return.
  4) Call ic_stop_apid_program
     Clean up everything and release all memory allocated by the iClaustron
     Data API.

  So the API user can focus his efforts on how to program the API user
  threads. These threads each receive a IC_APID_CONNECTION object at start
  of the function. They also receive a thread_state variable that can be
  used for some amount of communication between the threads. When the user
  returns from the API user function the API will automatically ensure that
  the thread is stopped in an organised manner to ensure that if all threads
  stops, that the ic_run_apid_program returns. It is also possible to perform
  expedited stops that ensures that all threads are made aware of that the
  user wants to stop the API.

  This easy manner of using the API will be sufficient for many users. For
  the users that need to implement the above functionality in a different
  manner, the functions to build your own start_apid, run_apid and stop_apid
  functions are available in the Data API functions.
*/

extern IC_STRING ic_glob_config_dir;
extern IC_STRING ic_glob_data_dir;
extern gchar *ic_glob_cs_server_name;
extern gchar *ic_glob_cs_server_port;
extern gchar *ic_glob_cs_connectstring;
extern gchar *ic_glob_version_path;
extern guint32 ic_glob_node_id;
extern guint32 ic_glob_cs_timeout;
extern guint32 ic_glob_num_threads;
extern guint32 ic_glob_use_iclaustron_cluster_server;
extern guint32 ic_glob_daemonize;
extern guint32 ic_glob_byte_order;
extern GOptionEntry ic_apid_entries[];

int ic_start_apid_program(IC_THREADPOOL_STATE **tp_state,
                          gchar **err_str,
                          gchar *error_buf,
                          IC_APID_GLOBAL **apid_global,
                          IC_API_CONFIG_SERVER **apic,
                          gboolean daemonize);

int ic_run_apid_program(IC_APID_GLOBAL *apid_global,
                        IC_THREADPOOL_STATE *tp_state,
                        IC_RUN_APID_THREAD_FUNC apid_func,
                        gchar **err_str);

void ic_stop_apid_program(int ret_code,
                          gchar *error_str,
                          IC_APID_GLOBAL *apid_global,
                          IC_API_CONFIG_SERVER *apic,
                          IC_THREADPOOL_STATE *tp_state);

/*
  The hidden header file contains parts of the interface which are
  public but which should not be used by API user. They are public
  for performance reasons and should not be trusted as being stable
  between releases. They provide the ability to use inlined methods
  to improve performance whereas all the other methods use function
  pointers which have a performance impact if called many times.

  The data types header file contains a number of data types definition
  which are public and will remain stable except possibly for additional
  entries in the enum's and struct's at the end.

  The inline header file contains all functions that are inlined.
*/
#include <ic_apid_datatypes.h>
#include <ic_apid_hidden.h>
#include <ic_apid_inline.h>
#endif
