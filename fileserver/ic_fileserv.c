/* Copyright (C) 2007-2013 iClaustron AB

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

  if ((ret_code= get_file_table_meta_data(apid_global,
                                          apid_conn,
                                          &md_trans,
                                          &table_def,
                                          &file_key_id,
                                          &file_data_id,
                                          cluster_id)))
    goto error;

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
}

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
                                  NULL,
                                  "ic_fsd",
                                  "- iClaustron File Server",
                                  TRUE,
                                  TRUE)))
    goto end;
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
