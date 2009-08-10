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
typedef struct ic_read_field_def IC_READ_FIELD_DEF;
typedef struct ic_read_field_bind IC_READ_FIELD_BIND;
typedef struct ic_write_field_def IC_WRITE_FIELD_DEF;
typedef struct ic_write_field_bind IC_WRITE_FIELD_BIND;
typedef struct ic_key_field_def IC_KEY_FIELD_DEF;
typedef struct ic_key_field_bind IC_KEY_FIELD_BIND;
typedef enum ic_field_type IC_FIELD_TYPE;
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
typedef struct ic_where_condition IC_WHERE_CONDITION;

typedef int (*IC_RUN_APID_THREAD_FUNC)(IC_APID_GLOBAL*, IC_THREAD_STATE*);
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
  The hidden header file contains parts of the interface which are
  public but which should not be used by API user. They are public
  for performance reasons and should not be trusted as being stable
  between releases.

  The data types header file contains a number of data types definition
  which are public and will remain stable except possibly for additional
  entries in the enum's and struct's at the end.
*/
#include <ic_apid_datatypes.h>
#include <ic_apid_hidden.h>

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

struct ic_apid_error
{
  IC_APID_ERROR_OPS error_ops;
  gboolean any_error;
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
    define the operation for later sending and execution.
   */
  int (*ic_write_key_access)
                /* Our thread representation */
               (IC_APID_CONNECTION *apid_conn,
                /* Transaction object, can span multiple threads */
                IC_TRANSACTION *transaction_obj,
                /* Description of fields and their new value to be written */
                IC_WRITE_FIELD_BIND *write_fields, /* == NULL for delete */
                /* Key fields and their values == WHERE clause */
                IC_KEY_FIELD_BIND *key_fields,
                /* Table and index ids */
                IC_TABLE_DEF *table_def,
                /* Type of write operation */
                IC_WRITE_KEY_OP write_key_op,
                /* Any reference the user wants to pass to completion phase */
                void *user_reference);

  /*
    Read key operation
    This method is used to perform primary key or unique key operations
    toward NDB. Will not send the operation, only define it.
  */
  int (*ic_read_key_access)
               (IC_APID_CONNECTION *apid_conn,
                IC_TRANSACTION *transaction_obj,
                /* Description of read fields and where to write values */
                IC_READ_FIELD_BIND *read_fields,
                IC_KEY_FIELD_BIND *key_fields,
                IC_TABLE_DEF *table_def,
                /* Type of read operation */
                IC_READ_KEY_OP read_key_op,
                void *user_reference);
  /*
    Scan table operation
    This method is used to scan a table, one can either scan a non-unique
    index or perform a full table scan. It is possible to limit the scan
    to a subset of the partitions used in the table.
  */
  int (*ic_scan_access)
               (IC_APID_CONNECTION *apid_conn,
                IC_TRANSACTION *transaction_obj,
                /* Description of where to place fields read from records */
                IC_READ_FIELD_BIND *read_fields,
                IC_RANGE_CONDITION *range_cond,
                IC_WHERE_CONDITION *where_cond,
                IC_TABLE_DEF *table_def,
                IC_SCAN_OP scan_op,
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
                                void *user_reference);

  /*
    Abort the transaction
    Again only an indication. In this case the outcome is though certain
    since the transaction is only committed if specifically committed and
    the commit was successful.
  */
  int (*ic_rollback_transaction) (IC_APID_CONNECTION *apid_conn,
                                  IC_TRANSACTION *transaction_obj,
                                  void *user_reference);

  /*
    Create a savepoint in the transaction
    This creates a savepoint which we can roll back to. The actual
    creation of the savepoint happens in NDB but unless the
    transaction is aborted it is successful.
  */
  int (*ic_create_savepoint) (IC_APID_CONNECTION *apid_conn,
                              IC_TRANSACTION *transaction_obj,
                              IC_SAVEPOINT_ID *savepoint_id,
                              void *user_reference);

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
  void (*ic_free) (IC_APID_CONNECTION *apid_conn);
};

struct ic_apid_connection
{
  IC_APID_CONNECTION_OPS apid_conn_ops;
  IC_METADATA_BIND_OPS apid_metadata_ops;
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

struct ic_apid_global
{
  IC_APID_GLOBAL_OPS apid_global_ops;
  IC_BITMAP *cluster_bitmap;
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

#include <ic_apid_inline.h>
#endif
