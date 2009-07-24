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

static const gchar *glob_process_name= "ic_fsd";

static GOptionEntry entries[] = 
{
  { "cs_connectstring", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_cs_connectstring,
    "Connect string to Cluster Servers", NULL},
  { "cs_hostname", 0, 0, G_OPTION_ARG_STRING,
     &ic_glob_cs_server_name,
    "Set Server Hostname of Cluster Server", NULL},
  { "cs_port", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_cs_server_port,
    "Set Server Port of Cluster Server", NULL},
  { "node_id", 0, 0, G_OPTION_ARG_INT,
    &ic_glob_node_id,
    "Node id of file server in all clusters", NULL},
  { "data_dir", 0, 0, G_OPTION_ARG_FILENAME,
    &ic_glob_data_path,
    "Sets path to data directory, config files in subdirectory config", NULL},
  { "num_threads", 0, 0, G_OPTION_ARG_INT,
    &ic_glob_num_threads,
    "Number of threads executing in file server process", NULL},
  { "use_iclaustron_cluster_server", 0, 0, G_OPTION_ARG_INT,
     &ic_glob_use_iclaustron_cluster_server,
    "Use of iClaustron Cluster Server (default or NDB mgm server", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
run_replication_server(IC_APID_GLOBAL *apid_global,
                       IC_THREADPOOL_STATE *tp_state,
                       gchar **error_str)
{
  (void)apid_global;
  (void)error_str;
  (void)tp_state;
  return 0;
}

int main(int argc,
         char *argv[])
{
  int ret_code;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar config_path_buf[IC_MAX_FILE_NAME_SIZE];
  gchar error_str[ERROR_MESSAGE_SIZE];
  gchar *err_str= NULL;
  IC_THREADPOOL_STATE *tp_state;

  if ((ret_code= ic_start_program(argc, argv, entries, glob_process_name,
            "- iClaustron Replication Server", TRUE)))
    goto end;
  if ((ret_code= ic_start_apid_program(&tp_state,
                                       config_path_buf,
                                       &err_str,
                                       error_str,
                                       &apid_global,
                                       &apic)))
    goto end;
  ret_code= run_replication_server(apid_global, tp_state, &err_str);
end:
  ic_stop_apid_program(ret_code, err_str, apid_global, apic);
  return ret_code;
}
