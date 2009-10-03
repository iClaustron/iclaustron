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

typedef struct ic_apid_global IC_APID_GLOBAL;
typedef struct ic_apid_global_ops IC_APID_GLOBAL_OPS;
typedef struct ic_field_def IC_FIELD_DEF;
typedef struct ic_field_bind IC_FIELD_BIND;
typedef struct ic_key_field_def IC_KEY_FIELD_DEF;
typedef struct ic_key_field_bind IC_KEY_FIELD_BIND;
typedef enum ic_field_type IC_FIELD_TYPE;
typedef enum ic_calculation_type IC_CALCULATION_TYPE;
typedef guint32 IC_SAVEPOINT_ID;
typedef struct ic_transaction_hint IC_TRANSACTION_HINT;
typedef struct ic_apid_connection IC_APID_CONNECTION;
typedef struct ic_transaction_obj IC_TRANSACTION;
typedef enum ic_commit_state IC_COMMIT_STATE;
typedef enum ic_read_key_op IC_READ_KEY_OP;
typedef enum ic_write_key_op IC_WRITE_KEY_OP;
typedef enum ic_scan_op IC_SCAN_OP;
typedef enum ic_instruction_type IC_INSTRUCTION_TYPE;
typedef struct ic_instruction IC_INSTRUCTION;
typedef struct ic_table_def IC_TABLE_DEF;
typedef struct ic_metadata_bind_ops IC_METADATA_BIND_OPS;
typedef struct ic_translation_obj IC_TRANSLATION_OBJ;
typedef enum ic_apid_operation_type IC_APID_OPERATION_TYPE;
typedef struct ic_apid_error IC_APID_ERROR;
typedef struct ic_apid_connection_ops IC_APID_CONNECTION_OPS;
typedef struct ic_transaction_ops IC_TRANSACTION_OPS;
typedef struct ic_transaction_hint_ops IC_TRANSACTION_HINT_OPS;
typedef struct ic_apid_error_ops IC_APID_ERROR_OPS;
typedef struct ic_transaction_state IC_TRANSACTION_STATE;
typedef enum ic_error_severity_level IC_ERROR_SEVERITY_LEVEL;
typedef enum ic_error_category IC_ERROR_CATEGORY;
typedef struct ic_range_condition IC_RANGE_CONDITION;
typedef struct ic_range_condition_ops IC_RANGE_CONDITION_OPS;
typedef struct ic_where_condition IC_WHERE_CONDITION;
typedef struct ic_where_condition_ops IC_WHERE_CONDITION_OPS;
typedef struct ic_conditional_assignment IC_CONDITIONAL_ASSIGNMENT;
typedef struct ic_conditional_assignment_ops IC_CONDITIONAL_ASSIGNMENT_OPS;
typedef enum ic_range_type IC_RANGE_TYPE;
typedef enum ic_comparator_type IC_COMPARATOR_TYPE;
typedef enum ic_boolean_type IC_BOOLEAN_TYPE;
typedef struct ic_apid_operation_ops IC_APID_OPERATION_OPS;
typedef struct ic_table_def_ops IC_TABLE_DEF_OPS;
typedef enum ic_field_data_type IC_FIELD_DATA_TYPE;

#define IC_NO_FIELD_ID 0xFFFFFFF0
#define IC_NO_NULL_OFFSET 0xFFFFFFF1

typedef int (*IC_RUN_APID_THREAD_FUNC)(IC_APID_CONNECTION*, IC_THREAD_STATE*);
typedef int (*IC_APID_CALLBACK_FUNC) (IC_APID_CONNECTION*, void* user_data);
/*
  Names of operation objects returned from ic_get_next_executed_operation.
  The information from these objects should be retrieved using the inline
  functions defined in ic_apid_inline.h. The structs are defined in the
  header file ic_apid_hidden.h which isn't safe to use, it will be changed
  from version to version.
*/
typedef struct ic_apid_operation IC_APID_OPERATION;
typedef struct ic_create_savepoint_operation IC_CREATE_SAVEPOINT_OPERATION;
typedef struct ic_rollback_savepoint_operation IC_ROLLBACK_SAVEPOINT_OPERATION;

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
    and table scans and index scans are currently supported.

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
     then it is possible to bind together two conditions using
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
   ic_send and ic_flush. ic_send only sends and then the thread is free
   to define new queries or work on any other thing. However the objects
   which have been sent and buffers used by the queries must not be
   touched by the application until the query has been returned. ic_flush
   will start waiting for queries to complete immediately after sending the
   queries. When using ic_send one can use ic_poll to start waiting
   without sending first.

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
   the synchronous method.
*/

struct ic_table_def_ops
{
  int (*ic_get_table_id) (IC_TABLE_DEF *table_def);
  int (*ic_get_field_id) (IC_TABLE_DEF *table_def,
                          const gchar *field_name,
                          guint32* field_id);
  int (*ic_get_field_type) (IC_TABLE_DEF *table_def,
                            guint32 field_id,
                            IC_FIELD_DATA_TYPE *field_data_type);
  int (*ic_get_field_len) (IC_TABLE_DEF *table_def,
                           guint32 field_id,
                           guint32 *field_len);
};

struct ic_metadata_bind_ops
{
  int (*ic_table_bind) (IC_TABLE_DEF **table_def,
                        const gchar *table_name);
  int (*ic_index_bind) (IC_TABLE_DEF **table_def,
                        const gchar *index_name,
                        const gchar *table_name);
};

struct ic_apid_operation_ops
{
  int (*ic_define_field) (IC_APID_OPERATION *apid_op,
                          guint32 index,
                          guint32 field_id,
                          guint32 buffer_offset,
                          guint32 null_offset,
                          gboolean key_field,
                          IC_FIELD_TYPE field_type);
  int (*ic_define_pos) (IC_APID_OPERATION *apid_op,
                        guint32 start_pos,
                        guint32 end_pos);
  int (*ic_define_alloc_size) (IC_APID_OPERATION *apid_op,
                               guint32 size);
  int (*ic_keep_range) (IC_APID_OPERATION *apid_op);
  int (*ic_release_range) (IC_APID_OPERATION *apid_op);
  IC_RANGE_CONDITION* (*ic_create_range_condition)
                             (IC_APID_OPERATION *apid_op);
  IC_WHERE_CONDITION* (*ic_create_where_condition)
                             (IC_APID_OPERATION *apid_op);
  int (*ic_map_where_condition) (IC_APID_OPERATION *apid_op,
                                 IC_APID_GLOBAL *apid_global,
                                 guint32 where_cond_id);
  IC_CONDITIONAL_ASSIGNMENT** (*ic_create_conditional_assignments)
                             (IC_APID_OPERATION *apid_op,
                              guint32 num_cond_assigns);
  int (*ic_map_conditional_assignment) (IC_APID_OPERATION *apid_op,
                                        IC_APID_GLOBAL *apid_global,
                                        guint32 cond_assign_id);
  int (*ic_set_partition_id) (IC_APID_OPERATION *apid_op,
                              guint32 partition_id);
  IC_APID_ERROR* (*ic_get_error_object) (IC_APID_OPERATION *apid_op);
  int (*ic_reset_apid_op) (IC_APID_OPERATION *apid_op);
  void (*ic_free_apid_op) (IC_APID_OPERATION *apid_op);
};

struct ic_range_condition_ops
{
  /*
    To define a range we call ic_define_range_part once for each field in the
    index. We need to define them in order from first index field. Thus
    field_id is mostly used to assert that the user knows the API. All parts
    except the last part must be an equality range.

    As an example if we have an index on a,b and c. We can define a range
    based on the condition a = 1 AND b < 1. In this case the start part is
    a = 1 and the end part is a = 1, b = 1, with the range type on the
    a field equal to IC_RANGE_EQ and the range type on the b field equal
    to IC_RANGE_LT. To indicate that the b field is not involved in the
    start part we set start_ptr to NULL, in this case start_len is undefined.
    NULL values can only be involved in a range as equality ranges. Such a
    condition is defined by setting start_ptr and end_ptr to NULL and in
    this case also range_type must be IC_RANGE_EQ, the value of start_len
    and end_len is undefined.

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
    ic_multi_range with number of ranges equal to 0. The first range id
    is 0. The ranges should be defined in the order of increasing range ids.
  */
  int (*ic_multi_range) (IC_RANGE_CONDITION *range,
                         guint32 num_ranges);
  int (*ic_define_range_part) (IC_RANGE_CONDITION *range,
                               guint32 range_id,
                               guint32 field_id,
                               gchar *start_ptr,
                               guint32 start_len,
                               gchar* end_ptr,
                               guint32 end_len,
                               IC_RANGE_TYPE range_type);
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

  To evaluate this we go left-deep from AND to EQ to a, so we start by reading
  the field into memory address 0, next we load the constant 1 into the next
  memory address (dependent on size of a). Next step is to perform the boolean
  EQ operation on the memory loaded from a and the memory where 1 was loaded.
  The result of this comparison will end up in condition id 0.  Next we follow
  the right branch in the AND left-deep and load b into the next memory
  address (here the API could decide to start from memory address 0 again since
  we already used the variables on the left side of the AND to calculate
  condition id 0. Whether the API does this or not is dependent on how
  advanced the memory management algorithm the API uses. Next it it follows the
  right side of the LT left-deep and loads c into the next memory address and
  next loads 3 into the memory address. The operation + is performed and the
  result placed in the next memory address or reusing a memory address used to
  load c and/or 3. Then the condition id 1 is derived as the result of the
  comparison of b and (c + 3). Finally the result is TRUE of both condition id
  0 and condition id 1 is TRUE and the result is stored in condition id 2 
  (or in condition id 0 if reusing condition id's). After completing this one
  calls ic_evaluate_cond to indicate that the last condition

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
  int (*ic_define_condition) (IC_WHERE_CONDITION *cond,
                              guint32 *condition_id,
                              guint32 left_memory_address,
                              guint32 right_memory_address,
                              IC_COMPARATOR_TYPE comp_type);
  int (*ic_read_field_into_memory) (IC_WHERE_CONDITION *cond,
                                    guint32 *memory_address,
                                    guint32 field_id);
  int (*ic_read_const_into_memory) (IC_WHERE_CONDITION *cond,
                                    guint32 *memory_address,
                                    gchar *const_ptr,
                                    guint32 const_len,
                                    IC_FIELD_TYPE const_type);
  int (*ic_define_calculation) (IC_WHERE_CONDITION *cond,
                                guint32 *returned_memory_address,
                                guint32 left_memory_address,
                                guint32 right_memory_address,
                                IC_CALCULATION_TYPE calc_type);
  int (*ic_define_boolean) (IC_WHERE_CONDITION *cond,
                            guint32 *result_condition_id,
                            guint32 left_condition_id,
                            guint32 right_condition_id,
                            IC_BOOLEAN_TYPE boolean_type);
  int (*ic_define_not) (IC_WHERE_CONDITION *cond,
                        guint32 condition_id);
  int (*ic_define_regexp) (IC_WHERE_CONDITION *cond,
                           guint32 *condition_id,
                           guint32 field_id,
                           guint32 start_pos,
                           guint32 end_pos,
                           guint32 reg_exp_memory);
  int (*ic_define_like) (IC_WHERE_CONDITION *cond,
                         guint32 *condition_id,
                         guint32 field_id,
                         guint32 start_pos,
                         guint32 end_pos,
                         guint32 like_memory_address);
  int (*ic_evaluate_condition) (IC_WHERE_CONDITION *cond);
  int (*ic_store_where_condition) (IC_WHERE_CONDITION *cond,
                                   guint32 cluster_id);
  int (*ic_free_cond) (IC_WHERE_CONDITION *cond);
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

    We can also make the entire assignment conditional, to do this, we reuse
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
  IC_WHERE_CONDITION* (*ic_create_assignment_condition)
                       (IC_CONDITIONAL_ASSIGNMENT *cond_assign);
  int (*ic_map_assignment_condition) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                      IC_APID_GLOBAL *apid_global,
                                      guint32 where_cond_id);
  int (*ic_read_field_into_memory) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                    guint32 *memory_address,
                                    guint32 field_id);
  int (*ic_read_const_into_memory) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                    guint32 *memory_address,
                                    gchar *const_ptr,
                                    guint32 const_len,
                                    IC_FIELD_TYPE const_type);
  int (*ic_define_calculation) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                guint32 *returned_memory_address,
                                guint32 left_memory_address,
                                guint32 right_memory_address,
                                IC_CALCULATION_TYPE calc_type);
  int (*ic_write_field_into_memory) (IC_CONDITIONAL_ASSIGNMENT *cond_assign,
                                     guint32 memory_address,
                                     guint32 field_id);
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
  int (*ic_write_key_access)
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
  int (*ic_read_key_access)
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
  int (*ic_scan_access)
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
  */
  int (*ic_start_transaction) (IC_APID_CONNECTION *apid_conn,
                               IC_TRANSACTION **transaction_obj,
                               IC_TRANSACTION_HINT *transaction_hint,
                               gboolean joinable);

  /*
    To be able to use a transaction with this Data API connection we need
    to specifically join the transaction.
  */
  int (*ic_join_transaction) (IC_APID_CONNECTION *apid_conn,
                              IC_TRANSACTION *transaction_obj,
                              void *user_reference);

  /*
    Commit the transaction
    This merely indicates a wish to commit the transaction. The actual
    commit is performed by NDB and thus the outcome of the commit is
    not known until we have sent and polled in all operations of the
    transaction.
  */
  int (*ic_commit_transaction) (IC_APID_CONNECTION *apid_conn,
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
  int (*ic_rollback_transaction) (IC_APID_CONNECTION *apid_conn,
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
  int (*ic_create_savepoint) (IC_APID_CONNECTION *apid_conn,
                              IC_TRANSACTION *transaction_obj,
                              IC_SAVEPOINT_ID *savepoint_id);

  /*
    Rollback to savepoint.
    This is an indication to NDB what to do. Rollback to a savepoint
    will remove all operations already defined and not sent. No
    more operations will be accepted on the transaction until the
    rollback to savepoint is completed in NDB.
  */
  int (*ic_rollback_savepoint) (IC_APID_CONNECTION *apid_conn,
                                IC_TRANSACTION *transaction_obj,
                                IC_SAVEPOINT_ID savepoint_id,
                                /* Callback function */
                                IC_APID_CALLBACK_FUNC callback_func,
                                /* User data to callback function */
                                void *user_reference);

  int (*ic_check_transaction_state) (IC_APID_CONNECTION *apid_conn,
                                     IC_TRANSACTION *transaction_obj,
                                     IC_TRANSACTION_STATE *transaction_state);
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
  int (*ic_poll) (IC_APID_CONNECTION *apid_conn, glong wait_time);

  /* Send all operations that we have prepared in this connection.  */
  int (*ic_send) (IC_APID_CONNECTION *apid_conn);

  /*
    Send operations and wait for operations to be received. This is
    equivalent to send + poll. The return code is either an error
    found already at the send phase and if the send is successful
    it will report in the same manner as poll.
  */
  int (*ic_flush) (IC_APID_CONNECTION *apid_conn, glong wait_time);

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
    Get state of transaction
    trans_obj                 IN
    trans_state               OUT
  */
  int (*ic_check_state) (IC_TRANSACTION *trans_obj,
                         IC_TRANSACTION_STATE *trans_state);

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
};

struct ic_apid_error_ops
{
  /* Get error code */
  int (*ic_get_apid_error_code) (IC_APID_ERROR *apid_error);

  /* Get error string */
  const gchar* (*ic_get_apid_error_msg) (IC_APID_ERROR *apid_error);

  /* Get severity level of the error */
  IC_ERROR_SEVERITY_LEVEL
    (*ic_get_apid_error_severity) (IC_APID_ERROR *apid_error);

  /* Get category of error */
  IC_ERROR_CATEGORY (*ic_get_apid_error_category) (IC_APID_ERROR *apid_error);
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
  void (*ic_cond_wait) (IC_APID_GLOBAL *apid_global);
  gboolean (*ic_get_stop_flag) (IC_APID_GLOBAL *apid_global);
  void (*ic_set_stop_flag) (IC_APID_GLOBAL *apid_global);
  guint32 (*ic_get_num_user_threads) (IC_APID_GLOBAL *apid_global);
  void (*ic_add_user_thread) (IC_APID_GLOBAL *apid_global);
  void (*ic_remove_user_thread) (IC_APID_GLOBAL *apid_global);
  void (*ic_set_thread_func) (IC_APID_GLOBAL *apid_global,
                              IC_RUN_APID_THREAD_FUNC apid_func);
  IC_RUN_APID_THREAD_FUNC (*ic_get_thread_func) (IC_APID_GLOBAL *apid_global);
  int (*ic_external_connect) (IC_APID_GLOBAL *apid_global,
                              guint32 cluster_id,
                              guint32 node_id,
                              IC_CONNECTION *conn);
  void (*ic_free_apid_global) (IC_APID_GLOBAL *apid_global);
};

IC_APID_GLOBAL* ic_create_apid_global(IC_API_CONFIG_SERVER *apic,
                                      gboolean use_external_connect,
                                      int *ret_code,
                                      gchar **err_str);
void ic_disconnect_apid_global(IC_APID_GLOBAL *apid_global);
int ic_wait_first_node_connect(IC_APID_GLOBAL *apid_global,
                               guint32 cluster_id);
IC_APID_CONNECTION*
ic_create_apid_connection(IC_APID_GLOBAL *apid_global,
                          IC_BITMAP *cluster_id_bitmap);

struct ic_apid_error
{
  IC_APID_ERROR_OPS error_ops;
  gboolean any_error;
};

struct ic_apid_connection
{
  IC_APID_CONNECTION_OPS apid_conn_ops;
  IC_METADATA_BIND_OPS apid_metadata_ops;
};

struct ic_apid_global
{
  IC_APID_GLOBAL_OPS apid_global_ops;
  IC_BITMAP *cluster_bitmap;
};

struct ic_table_def
{
  IC_TABLE_DEF_OPS table_def_ops;
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
};

struct ic_transaction_hint
{
  IC_TRANSACTION_HINT_OPS trans_hint_ops;
  guint32 cluster_id;
  guint32 node_id;
};

struct ic_transaction_obj
{
  IC_TRANSACTION_OPS trans_ops;
  guint32 transaction_id[2];
};

/*
  Declarations used for programs using easy start and stop
  of iClaustron Data API users. Also declaration of global
  variables used by these programs.
*/
extern IC_STRING ic_glob_config_dir;
extern IC_STRING ic_glob_data_dir;
extern gchar *ic_glob_cs_server_name;
extern gchar *ic_glob_cs_server_port;
extern gchar *ic_glob_cs_connectstring;
extern gchar *ic_glob_data_path;
extern gchar *ic_glob_version_path;
extern gchar *ic_glob_base_path;
extern guint32 ic_glob_node_id;
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
                          IC_API_CONFIG_SERVER *apic);

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
