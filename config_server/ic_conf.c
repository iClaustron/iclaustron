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

const gchar *data_server_str= "data server";
const gchar *client_node_str= "client";
const gchar *conf_server_str= "configuration server";
const gchar *net_part_str= "network partition server";
const gchar *rep_server_str= "replication server";
const gchar *data_server_def_str= "data server default";
const gchar *client_node_def_str= "client default";
const gchar *conf_server_def_str= "configuration server default";
const gchar *net_part_def_str= "network partition server default";
const gchar *rep_server_def_str= "replication server default";

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

/*
  MODULE:
    CONFIG_SERVER
    This is a configuration server implementing the ic_config_operations
    interface. It converts a configuration file to a set of data structures
    describing the configuration of a iClaustron Cluster Server.
*/
static
int conf_serv_init(struct ic_config_struct *ic_conf)
{
  return 0;
}

static
int conf_serv_add_section(struct ic_config_struct *ic_config,
                          guint32 section_number, guint32 line_number,
                          IC_STRING *section_name)
{
  return 0;
}

static
int conf_serv_add_key(struct ic_config_struct *ic_config,
                      guint32 section_number, guint32 line_number,
                      IC_STRING *key_name, IC_STRING *data)
{
  return 0;
}

static
int conf_serv_add_comment(struct ic_config_struct *ic_config,
                          guint32 line_number, IC_STRING *comment)
{
  return 0;
}

static
int conf_serv_end(struct ic_config_struct *ic_config,
                  guint32 line_number)
{
  return 0;
}

static struct ic_config_operations config_server_ops =
{
  .ic_config_init = conf_serv_init,
  .ic_add_section = conf_serv_add_section,
  .ic_add_key     = conf_serv_add_key,
  .ic_add_comment = conf_serv_add_comment,
  .ic_config_end  = conf_serv_end,
};

static int
bootstrap()
{
  gchar *conf_data_str;
  gsize conf_data_len;
  GError *loc_error= NULL;
  IC_STRING conf_data;
  struct ic_config_struct conf_server;

  printf("glob_config_file = %s\n", glob_config_file);
  if (!glob_config_file ||
      !g_file_get_contents(glob_config_file, &conf_data.str,
                           &conf_data_len, &loc_error))
    goto file_open_error;

  conf_data.str= conf_data_str;
  conf_data.len= conf_data_len;
  conf_data.null_terminated= FALSE;
  if (build_config_data(&conf_data, &config_server_ops,
                        &conf_server))
    return 1;
  return 0;

file_open_error:
  if (!glob_config_file)
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "--config-file parameter required when using --bootstrap\n");
  else
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Couldn't open file %s\n", glob_config_file);
  return 1;
}

int
main(int argc, char *argv[])
{
  GError *loc_error= NULL;
  GOptionContext *context;

  ic_init_error_messages();
  /* Read command options */
  context= g_option_context_new("iClaustron Configuration Server");
  if (!context)
    goto mem_error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &loc_error))
    goto parse_error;
  g_option_context_free(context);

  if (glob_bootstrap)
    return bootstrap();
  return 0;

mem_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Memory allocation error when allocation option context\n");
  return 1;
parse_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Option processing error\n");
  return 1;
}

