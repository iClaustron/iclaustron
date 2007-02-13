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
  -------
  CONFIG_SERVER
    This is a configuration server implementing the ic_config_operations
    interface. It converts a configuration file to a set of data structures
    describing the configuration of a iClaustron Cluster Server.
*/
static
int conf_serv_init(IC_CONFIG_STRUCT *ic_conf)
{
  IC_CLUSTER_CONFIG *clu_conf;
  if (!(clu_conf= (IC_CLUSTER_CONFIG*)g_malloc(sizeof(IC_CLUSTER_CONFIG))))
    return IC_ERROR_MEM_ALLOC;
  memset((char*)clu_conf, 0, sizeof(IC_CLUSTER_CONFIG));
  return 0;
}

static
int conf_serv_add_section(IC_CONFIG_STRUCT *ic_config,
                          guint32 section_number,
                          guint32 line_number,
                          IC_STRING *section_name,
                          guint32 pass)
{
  if (!ic_cmp_null_term_str(data_server_str, section_name))
  {
    printf("Found data server group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(client_node_str, section_name))
  {
    printf("Found client group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(conf_server_str, section_name))
  {
    printf("Found configuration server group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(rep_server_str, section_name))
  {
    printf("Found replication server group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(net_part_str, section_name))
  {
    printf("Found network partition server group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(data_server_def_str, section_name))
  {
    printf("Found data server default group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(client_node_def_str, section_name))
  {
    printf("Found client default group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(conf_server_def_str, section_name))
  {
    printf("Found configuration server default group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(rep_server_def_str, section_name))
  {
    printf("Found replication server default group\n");
    return 0;
  }
  else if (!ic_cmp_null_term_str(net_part_def_str, section_name))
  {
    printf("Found network partition server default group\n");
    return 0;
  }
  else
    return IC_ERROR_CONFIG_NO_SUCH_SECTION;
}

static
int conf_serv_add_key(IC_CONFIG_STRUCT *ic_config,
                      guint32 section_number,
                      guint32 line_number,
                      IC_STRING *key_name,
                      IC_STRING *data,
                      guint32 pass)
{
  printf("Line: %d Section: %d, Key-value pair\n", (int)line_number,
                                                   (int)section_number);
  return 0;
}

static
int conf_serv_add_comment(IC_CONFIG_STRUCT *ic_config,
                          guint32 line_number,
                          guint32 section_number,
                          IC_STRING *comment,
                          guint32 pass)
{
  IC_CLUSTER_CONFIG *clu_conf= ic_config->config_ptr.clu_conf; 
  printf("Line number %d in section %d was comment line\n", line_number, section_number);
  if (pass == INITIAL_PASS)
    clu_conf->comments.num_comments++;
  else
  {
    ;
  }
  return 0;
}

static
int conf_serv_end(IC_CONFIG_STRUCT *ic_conf)
{
  IC_CLUSTER_CONFIG *clu_conf= ic_conf->config_ptr.clu_conf;
  guint32 i;
  if (clu_conf)
  {
    for (i= 0; i < clu_conf->max_node_id; i++)
    {
      if (clu_conf->node_config_pointers[i])
        g_free((gchar*)clu_conf->node_config_pointers[i]);
    }
    if (clu_conf->node_config_types)
      g_free(clu_conf->node_config_types);
    for (i= 0; i < clu_conf->comments.num_comments; i++)
      g_free(clu_conf->comments.ptr_comments[i]);
    g_free(ic_conf->config_ptr.clu_conf);
  }
  return;
}

static IC_CONFIG_OPS config_server_ops =
{
  .ic_config_init = conf_serv_init,
  .ic_add_section = conf_serv_add_section,
  .ic_add_key     = conf_serv_add_key,
  .ic_add_comment = conf_serv_add_comment,
  .ic_config_end  = conf_serv_end,
};

static int
bootstrap(IC_CONFIG_STRUCT *conf_server)
{
  gchar *conf_data_str;
  gsize conf_data_len;
  GError *loc_error= NULL;
  IC_STRING conf_data;
  int ret_val;
  IC_CONFIG_ERROR err_obj;

  printf("glob_config_file = %s\n", glob_config_file);
  if (!glob_config_file ||
      !g_file_get_contents(glob_config_file, &conf_data.str,
                           &conf_data_len, &loc_error))
    goto file_open_error;

  conf_data.str= conf_data_str;
  conf_data.len= conf_data_len;
  conf_data.null_terminated= TRUE;
  ret_val= ic_build_config_data(&conf_data, &config_server_ops,
                                &conf_server, &err_obj);
  g_free(conf_data.str);
  if (ret_val == 1)
  {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Error at Line number %u:\n%s\n",err_obj.line_number,
          ic_get_error_message(err_obj.err_num));
  }
  return ret_val;

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
  IC_CONFIG_STRUCT conf_server;

  memset((char*)&conf_server, 0, sizeof(IC_CLUSTER_CONFIG));
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
    return bootstrap(&conf_server);

  config_server_ops.ic_config_end(&conf_server); 
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

