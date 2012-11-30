/* Copyright (C) 2007-2012 iClaustron AB

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

/*
   We now have a local Data API connection and we are ready to create
   the tables required by the iClaustron File Server.
*/
static int
run_bootstrap_thread(IC_APID_CONNECTION *apid_conn,
                     IC_THREAD_STATE *thread_state)
{
  int ret_code;
  IC_APID_GLOBAL *apid_global;
  IC_METADATA_TRANSACTION *md_trans= NULL;
  IC_ALTER_TABLE *md_alter_table= NULL;
  const gchar *pk_names[1];
  const gchar *file_key_str= "file_key";
  DEBUG_ENTRY("run_bootstrap_thread");

  pk_names[0]= file_key_str;

  (void)thread_state;

  apid_global= apid_conn->apid_conn_ops->ic_get_apid_global(apid_conn);
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
  if ((md_trans= ic_create_metadata_transaction(apid_conn,
                                                0)) ||

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
         IC_API_INT64_TYPE,
         1 /* A single field, not an array */,
         FALSE /* Not nullable */,
         FALSE /* Not stored on disk */))) ||

      ((ret_code= md_alter_table->alter_table_ops->ic_add_field(
         md_alter_table,
         "file_data",
         IC_API_VARIABLE_SIZE_CHAR,
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
  ic_printf("Failed to create table: file_table");
  DEBUG_RETURN_INT(0);
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
                                  "ic_fs_bootstrap",
                                  "- iClaustron File Server Bootstrap",
                                  TRUE,
                                  TRUE)))
    goto end;
  if ((ret_code= ic_start_apid_program(&tp_state,
                                       &err_str,
                                       error_str,
                                       &apid_global,
                                       &apic)))
    goto end;
  ic_glob_num_threads= 1; /* One thread is enough */
  ret_code= ic_run_apid_program(apid_global,
                                tp_state,
                                run_bootstrap_thread,
                                &err_str);
end:
  ic_stop_apid_program(ret_code,
                       err_str,
                       apid_global,
                       apic,
                       tp_state);
  return ret_code;
}
