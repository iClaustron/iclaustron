/*
  This program implements the iClaustron Configuration Server.
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

static gchar *glob_config_file= NULL;
static gchar *glob_config_path= NULL;
static gboolean glob_bootstrap= FALSE;
static gint glob_cluster_id= 0;

static GOptionEntry entries[] = 
{
  { "bootstrap", 0, 0, G_OPTION_ARG_NONE, &glob_bootstrap,
    "Is this bootstrap of a cluster", NULL},
  { "cluster_id", 0, 0, G_OPTION_ARG_INT, &glob_cluster_id,
    "Cluster id of a bootstrapped cluster", NULL},
  { "cluster-id", 0, 0, G_OPTION_ARG_INT, &glob_cluster_id,
    NULL, NULL},
  { "config_file", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_file,
    "Sets path to bootstrap configuration file", NULL},
  { "config-file", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_file,
    NULL, NULL},
  { "config_path", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_path,
    "Sets path to directory of configuration files", NULL},
  { "config-path", 0, 0, G_OPTION_ARG_FILENAME, &glob_config_path,
    NULL, NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int
main(int argc, char *argv[])
{
  GError *loc_error= NULL;
  GOptionContext *context;
  IC_CONFIG_STRUCT clu_conf;

  ic_init_error_messages();
  /* Read command options */
  context= g_option_context_new("iClaustron Configuration Server");
  if (!context)
    goto mem_error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &loc_error))
    goto parse_error;
  g_option_context_free(context);

  if (ic_load_config_server_from_files(glob_config_file,
                                       &clu_conf))
    return 1;
  clu_conf.clu_conf_ops->ic_config_end(&clu_conf);
  return 0;

mem_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Memory allocation error when allocating option context\n");
  return 1;
parse_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Option processing error\n");
  return 1;
}

