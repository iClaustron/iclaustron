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

/*
  This program implements the iClaustron Cluster Server.
  It reads a configuration file at bootstrap plus one file per cluster
  and maintans versions of these configuration files in separate
  files and controls updates to these configuration files. It
  maintains a versioned central file that keeps track of the clusters
  and the configuration files for them.

  It is intended that change to the configuration after bootstrap can be
  done either by editing a configuration file checked out from the
  configuration server or by using commands towards the Cluster Server.

  This program implements the Run Cluster Server interface which is a very
  simple interface with the possibility to create a Run Cluster Server,
  start it, run it, stop it and free it. The actual implementation is
  found in ic_apic.c and is part of the API towards the Cluster Server.
*/

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_apid.h>

/* Global variables */
static const gchar *glob_process_name= "ic_csd";

/* Option variables */
static gboolean glob_bootstrap= FALSE;
static gchar *glob_server_name= "127.0.0.1";
static gchar *glob_server_port= IC_DEF_CLUSTER_SERVER_PORT_STR;

static GOptionEntry entries[] = 
{
  { "bootstrap", 0, 0, G_OPTION_ARG_NONE,
    &glob_bootstrap,
    "Is this bootstrap of a cluster", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_server_port,
    "Set Cluster Server connection Port", NULL},
  { "server_name", 0, 0, G_OPTION_ARG_STRING,
    &glob_server_name,
    "Set Cluster Server Hostname", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int
main(int argc, char *argv[])
{
  int error, stop_error;
  gchar *err_str;
  IC_API_CONFIG_SERVER *apic;
  gchar error_buffer[ERROR_MESSAGE_SIZE];
  IC_RUN_CLUSTER_SERVER *run_obj;

  if ((error= ic_start_program(argc, argv, entries, ic_apid_entries,
                               glob_process_name,
           "- iClaustron Cluster Server", TRUE)))
    return error;
  if (!(run_obj= ic_create_run_cluster(&ic_glob_config_dir,
                                       glob_process_name,
                                       glob_server_name,
                                       glob_server_port,
                                       ic_glob_node_id)))
  {
    error= IC_ERROR_MEM_ALLOC;
    goto error;
  }
  DEBUG_PRINT(PROGRAM_LEVEL,
    ("Starting the iClaustron Cluster Server"));
  if ((error= run_obj->run_op.ic_start_cluster_server(run_obj)))
    goto start_error;
  apic= run_obj->run_op.ic_get_api_config(run_obj);
  DEBUG_PRINT(PROGRAM_LEVEL,
    ("Running the iClaustron Cluster Server"));
  if ((error= run_obj->run_op.ic_run_cluster_server(run_obj)))
    goto error;

end:
  if (run_obj)
  {
    if ((stop_error= run_obj->run_op.ic_stop_cluster_server(run_obj)))
    {
      ic_printf("Failed to stop cluster server with error:");
      ic_print_error(stop_error);
    }
    run_obj->run_op.ic_free_run_cluster(run_obj);
  }
  if (ic_glob_config_dir.str)
    ic_free(ic_glob_config_dir.str);
  ic_end();
  return error;
error:
  ic_print_error(error);
  goto end;
start_error:
  err_str= run_obj->run_op.ic_fill_error_buffer(run_obj, error, error_buffer);
  ic_printf("%s", err_str);
  goto end;
}
