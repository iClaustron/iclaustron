/* Copyright (C) 2007 iClaustron AB

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
  It reads a configuration file at bootstrap, one file per cluster
  and maintans versions of this configuration file in a separate
  directory and controls updates to these configuration files. It
  maintains a versioned central file that keeps track of the clusters
  and the configuration files for them.

  To change a configuration after bootstrap can be done either by
  editing a configuration file checked out from the configuration
  server or by using commands towards the Cluster Server.
*/

#include <ic_common.h>
#include <ic_apic.h>

static gchar *glob_config_file= "config.ini";
static gchar *glob_config_path= NULL;
static gboolean glob_bootstrap= FALSE;
static gint glob_cluster_id= 0;
static gchar *glob_server_name= "127.0.0.1";
static gchar *glob_server_port= "10203";

static GOptionEntry entries[] = 
{
  { "bootstrap", 0, 0, G_OPTION_ARG_NONE, &glob_bootstrap,
    "Is this bootstrap of a cluster", NULL},
  { "config_file", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_file,
    "Sets path to bootstrap configuration file", NULL},
  { "config_path", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_path,
    "Sets path to directory of configuration files", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Cluster Server connection Port", NULL},
  { "server_name", 0, 0, G_OPTION_ARG_STRING, &glob_server_name,
    "Set Cluster Server Hostname", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int
main(int argc, char *argv[])
{
  GError *loc_error= NULL;
  GOptionContext *context;
  int error= 1;
  guint32 cluster_id= 0;
  IC_CONFIG_STRUCT clu_conf_struct;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_RUN_CLUSTER_SERVER *run_obj;

  if (ic_init())
    return 1;
  /* Read command options */
  context= g_option_context_new("iClaustron Cluster Server");
  if (!context)
    goto mem_error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &loc_error))
    goto parse_error;
  g_option_context_free(context);
  /*
    Read the configuration from the file provided
  */
  if (!(clu_conf= ic_load_config_server_from_files(glob_config_file,
                                                   &clu_conf_struct)))
  {
    printf("Failed to load config file %s from disk", glob_config_file);
    goto end;
  }
  if (!(run_obj= ic_create_run_cluster(&clu_conf, &cluster_id, (guint32)1,
                                    glob_server_name, glob_server_port)))
  {
    printf("Failed to initialise run_cluster object\n");
    goto late_end;
  }
  printf("Starting the iClaustron Cluster Server\n");
  if ((error= run_obj->run_op.ic_run_cluster_server(run_obj)))
  {
    printf("run_ic_cluster_server returned error code %u\n", error);
    ic_print_error(error);
    goto late_end;
  }
  error= 0;
late_end:
  clu_conf_struct.clu_conf_ops->ic_config_end(&clu_conf_struct);
end:
  ic_end();
  return error;

mem_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Memory allocation error when allocating option context\n");
  goto end;
parse_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Option processing error\n");
  goto end;
}

