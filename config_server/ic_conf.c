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

#include <ic_comm.h>
#include <ic_common.h>

const guint32 MAX_LINE_LEN = 120;
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

static guint32 read_cr_line(gchar *iter_data, guint32 line_number)
{
  gsize len= 1;
  do
  {
    gchar iter_char= *iter_data;
    if (iter_char == CARRIAGE_RETURN ||
        iter_char == 0)
      return len;
    len++;
    iter_data++;
  } while (len < MAX_LINE_LEN);
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Line %d was longer than %d characters\n",
        line_number, MAX_LINE_LEN);
  return 0;
}

static int chk_str(const gchar *null_term_str, const gchar *cmp_str,
                   guint32 len)
{
  guint32 iter_len= 0;
  while (iter_len < len)
  {
    if (*null_term_str != *cmp_str)
      return 1;
    null_term_str++;
    cmp_str++;
    iter_len++;
  }
  if (*null_term_str == 0)
    return 0;
  return 1;
}

static gchar *conv_group_id(gchar *group_id, guint32 len)
{
  guint32 iter_len= 0;
  gchar *save_group_id= group_id;
  while (iter_len < len)
  {
    gchar c= *group_id;
    if (g_ascii_isalpha(c))
    {
      c= g_ascii_tolower(c);
      *group_id= c;
    }
    else if (c != ' ')
      return NULL;
    group_id++;
    iter_len++;
  }
  return save_group_id;    
}

static gchar *conv_key_id(gchar *key_id, guint32 *len)
{
  gchar *save_key_id= key_id;
  guint32 iter_len= 0;
  while (iter_len < *len)
  {
    gchar c= *key_id;
    if (!g_ascii_isalpha(c) && c != '_')
    {
      *len= iter_len;
      return save_key_id;
    }
    *key_id= g_ascii_tolower(c);
    iter_len++;
    key_id++;
  }
  return NULL;
}

static gchar *rm_space(gchar *val_str, guint32 *iter_len, guint32 len)
{
  for ( ; 
       (g_ascii_isspace(val_str[0]) ||
        val_str[0] == '\t')
       && *iter_len < len;
       val_str++, (*iter_len)++)
    ;
  return val_str;
}

static gchar *conv_key_value(gchar *val_str, guint32 *len)
{
  gboolean found= FALSE;
  guint32 iter_len= 0;
  gchar *save_val_str;
  gchar *first_val_str= val_str;

  val_str= rm_space(val_str, &iter_len, *len);
  if (*len == 0 || (val_str[0] != '=' && val_str[0] != ':'))
    return NULL;
  val_str++;
  iter_len++;
  val_str= rm_space(val_str, &iter_len, *len);
  save_val_str= val_str;
  while (iter_len < *len)
  {
    gchar c= *val_str;
    if (!(g_ascii_isalpha(c) || c == '/' || c == '\\' || c == '_'
        || g_ascii_isspace(c)))
      return NULL;
    val_str++;
    iter_len++;
    found= TRUE;
  }
  if (!found)
    return NULL;
  *len= (guint32)((first_val_str + *len) - save_val_str);
  return save_val_str;
}

static guint32 read_config_line(gchar *iter_data, gsize line_len,
                                guint32 line_number)
{
  if (line_len == 1)
  {
    printf("Line number %d is an empty line\n", line_number);
    return 0;
  }
  else if (*iter_data == '#')
  {
    printf("Line number %d was comment line\n", line_number);
    return 0;
  }
  else if (*iter_data == '[')
  {
    if (iter_data[line_len - 2] != ']')
    {
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
            "Line number %d, missing ] after initial [\n", line_number);
    }
    else
    {
      guint32 group_id_len= line_len - 3;
      gchar *group_id= conv_group_id(&iter_data[1], group_id_len);
      if (!group_id)
      {
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
              "Found incorrect group id at Line number %d\n", line_number);
        return 1;
      }
      if (!chk_str(data_server_str, group_id, group_id_len))
      {
        printf("Found data server group\n");
        return 0;
      }
      else if (!chk_str(client_node_str, group_id, group_id_len))
      {
        printf("Found client group\n");
        return 0;
      }
      else if (!chk_str(conf_server_str, group_id, group_id_len))
      {
        printf("Found configuration server group\n");
        return 0;
      }
      else if (!chk_str(rep_server_str, group_id, group_id_len))
      {
        printf("Found replication server group\n");
        return 0;
      }
      else if (!chk_str(net_part_str, group_id, group_id_len))
      {
        printf("Found network partition server group\n");
        return 0;
      }
      else if (!chk_str(data_server_def_str, group_id, group_id_len))
      {
        printf("Found data server default group\n");
        return 0;
      }
      else if (!chk_str(client_node_def_str, group_id, group_id_len))
      {
        printf("Found client default group\n");
        return 0;
      }
      else if (!chk_str(conf_server_def_str, group_id, group_id_len))
      {
        printf("Found configuration server default group\n");
        return 0;
      }
      else if (!chk_str(rep_server_def_str, group_id, group_id_len))
      {
        printf("Found replication server default group\n");
        return 0;
      }
      else if (!chk_str(net_part_def_str, group_id, group_id_len))
      {
        printf("Found network partition server default group\n");
        return 0;
      }
      else
      {
        group_id[group_id_len]= 0;
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
              "Correct specification of group %s but not a valid group\n",
              group_id);
        return 1;
      }
      return 0;
    }
  }
  else
  {
    guint32 id_len= line_len;
    guint32 val_len;
    guint32 space_len= 0;
    gchar *key_id, *val_str;
    rm_space(iter_data, &space_len, line_len);
    id_len-= space_len;
    if ((key_id= conv_key_id(&iter_data[space_len], &id_len)) &&
        ((val_len= line_len - (id_len + space_len)), TRUE) &&
        ((val_str= conv_key_value(&iter_data[id_len+space_len], &val_len))))
    {
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
            "Line: %d, Not a proper key-value pair\n", (int)line_number);
      return 1;
    }
    printf("Line: %d, Key-value pair\n", (int)line_number);
    return 0;
  }
  return 1;
}

static int verify_config_data(gchar *iter_data, gsize conf_data_len)
{
  guint32 line_number= 1;
  gsize iter_data_len= 0;
  guint32 line_length;
  while (iter_data_len < conf_data_len)
  {
    if (*iter_data == LINE_FEED)
    {
      /* Special handling of Windows Line Feeds after Carriage Return */
      printf("Special case\n");
      iter_data_len++;
      iter_data++;
      continue;
    }
    if (!(line_length= read_cr_line(iter_data, line_number)))
      return 1;
    if (read_config_line(iter_data, line_length, line_number))
      return 1;
    iter_data+= line_length;
    iter_data_len+= line_length;
    line_number++;
  }
  return 0;
}

static int bootstrap()
{
  gchar *conf_data;
  gsize conf_data_len;
  GError *loc_error= NULL;

  printf("glob_config_file = %s\n", glob_config_file);
  if (!glob_config_file ||
      !g_file_get_contents(glob_config_file, &conf_data,
                           &conf_data_len, &loc_error))
    goto file_open_error;

  if (verify_config_data(conf_data, conf_data_len))
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

int main(int argc, char *argv[])
{
  GError *loc_error= NULL;
  GOptionContext *context;

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

