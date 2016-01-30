/* Copyright (C) 2007, 2016 iClaustron AB

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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_parse_connectstring.h>
#include <ic_threadpool.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_apid.h>

static const gchar *std_ptr= "std";
static const gchar *file_server_ptr= "file_server";
static const gchar *file_table_ptr= "file_table";
static const gchar *file_key_ptr= "file_key";
static const gchar *file_data_ptr= "file_data";

static gboolean ic_glob_bootstrap_flag= FALSE;

/**
 * Mutexes that protect the startup phase of the file server threads.
 * The first thread to complete connecting to the cluster will perform
 * the start phase. If we are bootstrapping the file server this means
 * creating the file system tables. In all cases it means retrieving the
 * file server metadata from the data nodes.
 *
 * The mutex protects the variables
 * g_first_thread_started
 *   Flag indicating if someone started as first thread yet.
 * g_first_thread_completed
 *   Flag indicating if first thread has completed its startup yet.
 * g_startup_success
 *   Flag indicating if first thread succeeded in starting.
 */
static IC_MUTEX *g_start_mutex;
static IC_COND  *g_start_cond;

static gboolean g_first_thread_started= FALSE;
static gboolean g_first_thread_completed= FALSE;
static gboolean g_startup_success= FALSE;

/*
   We now have a local Data API connection and we are ready to create
   the tables required by the iClaustron File Server.
*/
static int
run_bootstrap(IC_APID_CONNECTION *apid_conn)
{
  int ret_code;
  IC_METADATA_TRANSACTION *md_trans= NULL;
  IC_ALTER_TABLE *md_alter_table= NULL;
  const gchar *pk_names[1];
  const gchar *file_key_str= "file_key";
  DEBUG_ENTRY("run_bootstrap_thread");

  pk_names[0]= file_key_str;

  ret_code= IC_ERROR_MEM_ALLOC;
  /*
    Creating the file_table has the following steps:
    1) Create metadata transaction
    2) Create metadata operation
    3) Create table
    4) Add two fields (file_key and file_data)
    5) Add primary key index on file_key
    6) Commit metadata transaction (this is where the data node is
       contacted.
  */
  if ((!(md_trans= ic_create_metadata_transaction(apid_conn,
                                                  0,
                                                  &ret_code))) ||

      ((ret_code= md_trans->md_trans_ops->ic_create_metadata_op(
         md_trans,
         &md_alter_table))) ||

      ((ret_code= md_alter_table->alter_table_ops->ic_create_table(
         md_alter_table,
         "std",
         "file_server",
         "file_table",
         NULL))) || /* Tablespace name not provided yet */

      ((ret_code= md_alter_table->alter_table_ops->ic_add_field(
         md_alter_table,
         file_key_str,
         IC_API_BIG_UNSIGNED,
         1 /* A single field, not an array */,
         FALSE /* Not nullable */,
         FALSE /* Not stored on disk */))) ||

      ((ret_code= md_alter_table->alter_table_ops->ic_add_field(
         md_alter_table,
         "file_data",
         IC_API_BINARY,
         8192 /* Maximum size of each part of the file */,
         TRUE /* Nullable */,
         FALSE /* Not stored on disk for now */))) ||

      ((ret_code= md_alter_table->alter_table_ops->ic_add_index(
         md_alter_table,
         "file_table_pkey",
         pk_names,
         1,
         IC_PRIMARY_KEY,
         FALSE /* No null values allowed in index */))) ||

      ((ret_code= md_trans->md_trans_ops->ic_md_commit(md_trans)))) 
    goto error;
  md_trans->md_trans_ops->ic_free_md_trans(md_trans);
  ic_printf("Successfully created table: file_table");
  DEBUG_RETURN_INT(0);

error:
  if (md_trans)
    md_trans->md_trans_ops->ic_free_md_trans(md_trans);
  ic_printf("Failed to create table: file_table, ret_code = %u",
            ret_code);
  DEBUG_RETURN_INT(1);
}

static int
get_file_table_meta_data(IC_APID_GLOBAL *apid_global,
                         IC_APID_CONNECTION *apid_conn,
                         IC_METADATA_TRANSACTION **md_trans,
                         IC_TABLE_DEF **table_def,
                         guint32 *file_key_id,
                         guint32 *file_data_id,
                         guint32 cluster_id)
{
  int ret_code;
  DEBUG_ENTRY("get_file_table_meta_data");
  /*
     We need access to a metadata transaction object in order to
     acquire a table definition.
  */
  if ((*md_trans) == NULL)
  {
    if (!((*md_trans)= ic_create_metadata_transaction(apid_conn,
                                                      cluster_id,
                                                      &ret_code)))
    {
      goto error;
    }
  }

  /**
    Get access to the table definition of vital tables in the file
    server.
  */
  if ((ret_code= apid_global->apid_metadata_ops->ic_table_bind(
         apid_conn,
         *md_trans,
         table_def,
         std_ptr,
         file_server_ptr,
         file_table_ptr)))
    goto error;

  /* Get id of the file_key and the file_data field. */
  if ((ret_code= (*table_def)->table_def_ops->ic_get_field_id(
         *table_def,
         file_key_ptr,
         file_key_id)) ||
      (ret_code= (*table_def)->table_def_ops->ic_get_field_id(
         *table_def,
         file_data_ptr,
         file_data_id)))
    goto error;
  DEBUG_RETURN_INT(0);
error:
  DEBUG_RETURN_INT(ret_code);
}

static int
get_file_table_query_object(IC_APID_GLOBAL *apid_global,
                            IC_TABLE_DEF *table_def,
                            guint32 file_key_id,
                            guint32 file_data_id,
                            guint64 *buffer_values,
                            guint8 *null_bits,
                            IC_APID_QUERY **apid_query)
{
  int ret_code= 0;
  IC_APID_QUERY *loc_apid_query;
  DEBUG_ENTRY("get_file_table_query_object");

  /**
    We need an query object in order to be able to read and
    write the file server tables. In this object we define the
    mapping to where the fields are written and read as part of
    the actual read and write queries.
  */
  if (!(loc_apid_query= ic_create_apid_query(apid_global,
                                             table_def,
                                             2,
                                             buffer_values,
                                             3,
                                             null_bits,
                                             1,
                                             &ret_code)))
  {
    DEBUG_RETURN_INT(ret_code);
  }

  /**
    Define mapping, the file_key field is using the first 64-bit value
    and since it is a 64-bit integer field, the value is stored inside
    64-bit value. The field is not nullable, so the position of the
    NULL bit is ignored.

    The second field, the file_data field is a nullable field, so in
    this case the null bit position is valid. Position 1 is the first
    64-bit value used to describe this field. Since it is a variable
    sized field the first 64-bit value is the length in bytes and the
    second 64-bit value is a pointer to where the data is read from
    (for writes) or written (for reads). If it is a read and the pointer
    is 0, then the API will allocate a buffer and store it there and
    write the pointer into this position. If a buffer is used at read,
    then the length attribute specifies the allocated length of this
    buffer.
   */
  if ((ret_code= loc_apid_query->apid_query_ops->ic_define_field(
         loc_apid_query, file_key_id, 0, 0)) ||
      (ret_code= loc_apid_query->apid_query_ops->ic_define_field(
         loc_apid_query, file_data_id, 1, 0)))
    goto error;
  *apid_query= loc_apid_query;
  DEBUG_RETURN_INT(0);

error:
  loc_apid_query->apid_query_ops->ic_free_apid_query(loc_apid_query);
  *apid_query= NULL;
  DEBUG_RETURN_INT(ret_code);
}

static int
get_transaction_object(IC_APID_CONNECTION *apid_conn,
                       IC_TRANSACTION **trans_obj,
                       guint32 cluster_id)
{
  int ret_code;
  DEBUG_ENTRY("get_transaction_object");

  /* We need a transaction object to access NDB */
  if ((ret_code= apid_conn->apid_conn_ops->ic_start_transaction(
         apid_conn,
         trans_obj,
         NULL,
         cluster_id,
         FALSE)))
  {
    ;
  }
  DEBUG_RETURN_INT(ret_code);
}

static int
insert_file_object(IC_APID_CONNECTION *apid_conn,
                   IC_APID_QUERY *apid_query,
                   IC_TRANSACTION *trans_obj)
{
  int ret_code;
  DEBUG_ENTRY("insert_file_object");

  if ((ret_code= apid_conn->apid_conn_ops->ic_write_key(
         apid_conn,
         apid_query,
         trans_obj,
         IC_KEY_INSERT,
         NULL,
         NULL)))
  {
    ;
  }
  DEBUG_RETURN_INT(ret_code);
}

/*
   We now have a local Data API connection and we are ready to issue
   file system transactions to keep our local cache consistent with the
   global NDB file system.
*/
#define START_TIMEOUT 30
static int
run_file_server_thread(IC_APID_CONNECTION *apid_conn,
                       IC_THREAD_STATE *thread_state)
{
  IC_APID_GLOBAL *apid_global; /* Global environment */
  IC_APID_QUERY *apid_query;   /* Query object */

  /* Metadata objects */
  IC_METADATA_TRANSACTION *md_trans= NULL;
  IC_TABLE_DEF *table_def;
  guint32 file_key_id, file_data_id;

  IC_TRANSACTION *trans_obj; /* Transaction object */

  guint64 buffer_values[3]; /* iClaustron Record */
  guint8 null_bits;         /* iClaustron NULL bits */

  guint64 file_key= 13;
  int ret_code;
  guint32 cluster_id= 0;
  gboolean bootstrap_failed= FALSE;
  gboolean is_first_thread;
  gboolean startup_success= FALSE;
  guint32 i;
  gchar *data_str= "Some random string with data in it";
  DEBUG_ENTRY("run_file_server_thread");

  (void)thread_state;
  apid_global= apid_conn->apid_conn_ops->ic_get_apid_global(apid_conn);

  if ((ret_code= apid_global->apid_global_ops->ic_wait_first_node_connect(
       apid_global,
       cluster_id,
       (glong)30000)))
  {
    DEBUG_RETURN_INT(ret_code);
  }

  ic_mutex_lock(g_start_mutex);
  if (g_first_thread_started)
  {
    is_first_thread= FALSE;
    /**
     * Wait for other thread to take care of startup logic.
     * This thread will wake us up when done.
     */
    do
    {
      ic_cond_wait(g_start_cond, g_start_mutex);
    } while (!g_first_thread_completed);
    startup_success= g_startup_success;
    ic_mutex_unlock(g_start_mutex);
    if (!startup_success)
    {
      /* Startup failed in first thread, we will stop */
      goto end;
    }
  }
  else
  {
    g_first_thread_started= TRUE;
    ic_mutex_unlock(g_start_mutex);
    is_first_thread= TRUE;
    if (ic_glob_bootstrap_flag)
    {
      if (run_bootstrap(apid_conn))
      {
        /**
         * Bootstrap failed, we assume tables already exist and attempt
         * to continue without bootstrap.
         */
        bootstrap_failed= TRUE;
      }
    }
  }
  for (i= 0; i < START_TIMEOUT; i++)
  {
    if ((ret_code= get_file_table_meta_data(apid_global,
                                            apid_conn,
                                            &md_trans,
                                            &table_def,
                                            &file_key_id,
                                            &file_data_id,
                                            cluster_id)))
    {
      if (bootstrap_failed)
      {
        /* We cannot continue, both bootstrap AND get meta data failed. */
        break;
      }
      ic_sleep(1);
    }
    else
    {
      startup_success= TRUE;
      break;
    }
  }

  /**
   * We have successfully retrieved the metadata objects. We are now ready
   * to start running the file server. If we were first thread we will also
   * wake all other threads such that they similarly can proceed.
   */
  if (is_first_thread)
  {
    ic_mutex_lock(g_start_mutex);
    g_startup_success= startup_success;
    g_first_thread_completed= TRUE;
    ic_cond_broadcast(g_start_cond);
    ic_mutex_unlock(g_start_mutex);
  }
  if (!startup_success)
  {
    /* We failed to startup properly, we will quit */
    goto end;
  }

  /**
   * Run file server logic starts:
   * -----------------------------
   */
  if ((ret_code= get_file_table_query_object(apid_global,
                                             table_def,
                                             file_key_id,
                                             file_data_id,
                                             &buffer_values[0],
                                             &null_bits,
                                             &apid_query)))
    goto error;

  if ((ret_code= get_transaction_object(apid_conn,
                                        &trans_obj,
                                        cluster_id)))
    goto error;

  /**
    Perform the insert query, as part of the insert query we will
    read the values from the buffer. Start by preparing the buffer values
    containing the data to insert.
  */
  buffer_values[0]= file_key;
  buffer_values[1]= strlen(data_str) + 1; /* Include null byte in length */
  buffer_values[2]= (guint64)data_str;
  if ((ret_code= insert_file_object(apid_conn,
                                    apid_query,
                                    trans_obj)))
    goto error;
  DEBUG_RETURN_INT(0);
error:
  /* Handle errors reported through IC_APID_ERROR */
  DEBUG_RETURN_INT(ret_code);
end:
  DEBUG_RETURN_INT(0);
}

static void
init_file_server(void)
{
  g_start_mutex= ic_mutex_create();
  g_start_cond= ic_cond_create();
  ic_require(g_start_mutex && g_start_cond);
}

GOptionEntry ic_fs_extra_entries [] =
{
  { "bootstrap", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_bootstrap_flag,
    "Bootstrap file server by creating file server tables", NULL },
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int main(int argc, char *argv[])
{
  int ret_code;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar error_str[ERROR_MESSAGE_SIZE];
  gchar *err_str= NULL;
  IC_THREADPOOL_STATE *tp_state= NULL;

  if ((ret_code= ic_start_program(argc,
                                  argv,
                                  ic_apid_entries,
                                  ic_fs_extra_entries,
                                  "ic_fsd",
                                  "- iClaustron File Server",
                                  TRUE,
                                  TRUE)))
    goto end;

  init_file_server();

  if ((ret_code= ic_start_apid_program(&tp_state,
                                       &err_str,
                                       error_str,
                                       &apid_global,
                                       &apic)))
    goto end;

  ret_code= ic_run_apid_program(apid_global,
                                tp_state,
                                run_file_server_thread,
                                &err_str);
end:
  ic_stop_apid_program(ret_code,
                       err_str,
                       apid_global,
                       apic,
                       tp_state);
  return ret_code;
}
