/* Copyright (C) 2007, 2008 iClaustron AB

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

#include <ic_common.h>
#include <ic_apic.h>
#include <ic_apid.h>

static IC_STRING glob_config_dir= { NULL, 0, TRUE};
static const gchar *glob_process_name= "ic_fsd";

static gchar *glob_cluster_server_ip= "127.0.0.1";
static gchar *glob_cluster_server_port= "10203";
static gchar *glob_data_path= NULL;
static guint32 glob_node_id= 5;
static guint32 glob_num_threads= 1;
static guint32 glob_use_iclaustron_cluster_server= 1;
static IC_THREADPOOL_STATE *glob_tp_state= NULL;

static GOptionEntry entries[] = 
{
  { "num_fs_threads", 0, 0, G_OPTION_ARG_INT,
    &glob_num_threads,
    "Number of threads executing in file server process", NULL},
  { "use_iclaustron_cluster_server", 0, 0, G_OPTION_ARG_INT,
     &glob_use_iclaustron_cluster_server,
    "Use of iClaustron Cluster Server (default or NDB mgm server", NULL},
  { "cluster_server_hostname", 0, 0, G_OPTION_ARG_STRING,
     &glob_cluster_server_ip,
    "Set Server Hostname of Cluster Server", NULL},
  { "cluster_server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_cluster_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "node_id", 0, 0, G_OPTION_ARG_INT,
    &glob_node_id,
    "Node id of file server in all clusters", NULL},
  { "data_dir", 0, 0, G_OPTION_ARG_FILENAME, &glob_data_path,
    "Sets path to data directory, config files in subdirectory config", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static gpointer
run_file_server_thread(gpointer data)
{
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_APID_GLOBAL *apid_global= (IC_APID_GLOBAL*)thread_state->object;
  IC_APID_CONNECTION *apid_conn;
  gboolean stop_flag;
  DEBUG_ENTRY("run_file_server_thread");

  g_mutex_lock(apid_global->mutex);
  stop_flag= apid_global->stop_flag;
  g_mutex_unlock(apid_global->mutex);
  if (stop_flag)
    DEBUG_RETURN(NULL);
  /*
    Now start-up has completed and at this point in time we have connections
    to all clusters already set-up. So all we need to do now is start a local
    IC_APID_CONNECTION and start using it based on input from users
  */
  if (!(apid_conn= ic_create_apid_connection(apid_global,
                                             apid_global->cluster_bitmap)))
    goto error;
  /*
     We now have a local Data API connection and we are ready to issue
     file system transactions to keep our local cache consistent with the
     global NDB file system
  */
  /* Currently empty implementation */
error:
  g_mutex_lock(apid_global->mutex);
  apid_global->num_user_threads_started--;
  g_cond_signal(apid_global->cond);
  g_mutex_unlock(apid_global->mutex);
  DEBUG_RETURN(NULL);
}

static int
start_file_server_thread(IC_APID_GLOBAL *apid_global)
{
  guint32 thread_id;
  return glob_tp_state->tp_ops.ic_threadpool_start_thread(
                            glob_tp_state,
                            &thread_id,
                            run_file_server_thread,
                            (gpointer)apid_global,
                            IC_MEDIUM_STACK_SIZE);
}

static int
run_file_server(IC_APID_GLOBAL *apid_global, gchar **err_str)
{
  int error= 0;
  guint32 i;
  guint32 num_threads_started= 0;
  DEBUG_ENTRY("run_file_server");

  *err_str= NULL;
  printf("Ready to start file server\n");
  g_mutex_lock(apid_global->mutex);
  for (i= 0; i < glob_num_threads; i++)
  {
    if (!(error= start_file_server_thread(apid_global)))
    {
      apid_global->stop_flag= TRUE;
      break;
    }
    num_threads_started++;
  }
  apid_global->num_user_threads_started= num_threads_started;
  while (1)
  {
    g_cond_wait(apid_global->cond, apid_global->mutex);
    if (apid_global->num_user_threads_started == 0)
    {
      g_mutex_unlock(apid_global->mutex);
      break;
    }
  }
  DEBUG_RETURN(error);
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_API_CLUSTER_CONNECTION api_cluster_conn;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar config_path_buf[IC_MAX_FILE_NAME_SIZE];
  gchar error_str[ERROR_MESSAGE_SIZE];
  gchar *err_str= error_str;

  if ((ret_code= ic_start_program(argc, argv, entries, glob_process_name,
            "- iClaustron File Server")))
    goto early_error;
  if (!(glob_tp_state=
          ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto early_error;
  }
  if ((ret_code= ic_set_config_path(&glob_config_dir,
                                    glob_data_path,
                                    config_path_buf)))
    goto early_error;
  ret_code= 1;
  if (!(apic= ic_get_configuration(&api_cluster_conn,
                                   &glob_config_dir,
                                   glob_node_id,
                                   glob_cluster_server_ip,
                                   glob_cluster_server_port,
                                   glob_use_iclaustron_cluster_server,
                                   &ret_code,
                                   &err_str)))
    goto error;
  if (!(apid_global= ic_connect_apid_global(apic, &ret_code, &err_str)))
    goto error;
  if ((ret_code= run_file_server(apid_global, &err_str)))
    goto error;
end:
  if (apic)
    apic->api_op.ic_free_config(apic);
  if (apid_global)
    ic_disconnect_apid_global(apid_global);
  ic_end();
  return ret_code;

early_error:
  err_str= NULL;
error:
  if (err_str)
    printf("%s", err_str);
  else
    ic_print_error(ret_code);
  goto end;
}
