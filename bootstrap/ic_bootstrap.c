/* Copyright (C) 2007-2011 iClaustron AB

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
#include <ic_connection.h>
#include <ic_readline.h>
#include <ic_lex_support.h>
#include <ic_protocol_support.h>
#include <ic_proto_str.h>
#include <ic_pcntrl_proto.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_boot_int.h"

static const gchar *glob_process_name= "ic_bootstrap";
static gchar *glob_command_file= NULL;
static gchar *glob_generate_command_file= NULL;
static guint32 glob_history_size= 100;
static gchar *ic_prompt= "iClaustron bootstrap> ";

static GOptionEntry entries[]=
{
  { "command-file", 0, 0, G_OPTION_ARG_STRING,
    &glob_command_file,
    "Use a command file and provide its name", NULL},
  { "generate-command-file", 0, 0, G_OPTION_ARG_STRING,
    &glob_generate_command_file,
    "Generate a command file from config files, provide its name", NULL},
  { "history_size", 0, 0, G_OPTION_ARG_INT, &glob_history_size,
    "Set Size of Command Line History", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static int
generate_command_file(gchar *parse_buf)
{
  int ret_code;
  int len;
  guint32 node_id;
  IC_CONFIG_ERROR err_obj;
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  IC_STRING config_dir;
  IC_CLUSTER_CONFIG *grid_cluster;
  IC_FILE_HANDLE cmd_file_handle;
  IC_CLUSTER_SERVER_CONFIG *cs_conf;
  IC_CLUSTER_MANAGER_CONFIG *mgr_conf;
  DEBUG_ENTRY("generate_command_file");

  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
                                           0, FALSE)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);

  ic_set_current_dir(&config_dir);

  /* Read the config.ini file to get cluster information */
  if (!(clu_infos= ic_load_cluster_config_from_file(&config_dir,
                                                    (IC_CONF_VERSION_TYPE)0,
                                                    mc_ptr,
                                                    &err_obj)))
  {
    ret_code= err_obj.err_num;
    goto end;
  }

  /* Read the grid_common.ini to get info about Cluster Servers/Managers */
  if (!(grid_cluster= ic_load_grid_common_config_server_from_file(
                         &config_dir,
                         (IC_CONF_VERSION_TYPE)0,
                         mc_ptr,
                         *clu_infos,
                         &err_obj)))
  {
    ret_code= err_obj.err_num;
    goto end;
  }

  /* Create file to store generated file */
  if ((ret_code= ic_create_file(&cmd_file_handle,
                                (const gchar*)glob_generate_command_file)))
    goto end;

  /* grid_cluster can now be used to generate the command file */
  for (node_id = 1; node_id <= grid_cluster->max_node_id; node_id++)
  {
    if (grid_cluster->node_types[node_id] == IC_CLUSTER_SERVER_NODE)
    {
      /* Found a Cluster Server, now generate a command line */
      cs_conf= (IC_CLUSTER_SERVER_CONFIG*)grid_cluster->node_config[node_id];
      /*
        Generate a command line like this:
        PREPARE CLUSTER SERVER HOST=hostname
                               PCNTRL_HOST=hostname
                               PCNTRL_PORT=port_number
                               CS_INTERNAL_PORT=port_number
                               NODE_ID=node_id;
      */
      len= g_snprintf(parse_buf,
                      COMMAND_READ_BUF_SIZE,
                      "PREPARE CLUSTER SERVER HOST=%s "
                      "PCNTRL_HOST=%s "
                      "PCNTRL_PORT=%u "
                      "NODE_ID=%u;\n",
                      cs_conf->hostname,
                      cs_conf->pcntrl_hostname,
                      cs_conf->pcntrl_port,
                      node_id);
      if ((ret_code= ic_write_file(cmd_file_handle,
                                   parse_buf,
                                   len)))
        goto late_end;
    }
    if (grid_cluster->node_types[node_id] == IC_CLUSTER_MANAGER_NODE)
    {
      /* Found a Cluster Server, now generate a command line */
      mgr_conf= (IC_CLUSTER_MANAGER_CONFIG*)grid_cluster->node_config[node_id];
      /*
        Generate a command line like this:
        PREPARE CLUSTER MANAGER HOST=hostname
                                PCNTRL_HOST=hostname
                                PCNTRL_PORT=port_number
                                CS_INTERNAL_PORT=port_number
                                NODE_ID=node_id;
      */
      len= g_snprintf(parse_buf,
                      COMMAND_READ_BUF_SIZE,
                      "PREPARE CLUSTER MANAGER HOST=%s "
                      "PCNTRL_HOST=%s "
                      "PCNTRL_PORT=%u "
                      "NODE_ID=%u;\n",
                      mgr_conf->client_conf.hostname,
                      mgr_conf->client_conf.pcntrl_hostname,
                      mgr_conf->client_conf.pcntrl_port,
                      node_id);
      if ((ret_code= ic_write_file(cmd_file_handle,
                                   parse_buf,
                                   len)))
        goto late_end;
    }
  }
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "SEND FILES;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "START CLUSTER SERVERS;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "VERIFY CLUSTER SERVERS;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;
  len= g_snprintf(parse_buf,
                  COMMAND_READ_BUF_SIZE,
                  "START CLUSTER MANAGERS;\n");
  if ((ret_code= ic_write_file(cmd_file_handle,
                               parse_buf,
                               len)))
    goto late_end;

late_end:
  (void)ic_close_file(cmd_file_handle);
  if (ret_code)
    (void)ic_delete_file(glob_command_file);
end:
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN_INT(ret_code);
}

static void
ic_prepare_cluster_server_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_prepare_cluster_server_cmd");
  if (parse_data->cs_index >= IC_MAX_CLUSTER_SERVERS)
  {
    ic_printf("Defined too many Cluster Servers");
    parse_data->exit_flag= TRUE;
    DEBUG_RETURN_EMPTY;
  }
  ic_printf("Prepared cluster server with node id %u",
    parse_data->cs_data[parse_data->cs_index].node_id);
  DEBUG_RETURN_EMPTY;
}

static void
ic_prepare_cluster_manager_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_prepare_cluster_manager_cmd");
  if (parse_data->mgr_index >= IC_MAX_CLUSTER_SERVERS)
  {
    ic_printf("Defined too many Cluster Managers");
    parse_data->exit_flag= TRUE;
    DEBUG_RETURN_EMPTY;
  }
  ic_printf("Prepared cluster manager with node id %u",
    parse_data->mgr_data[parse_data->mgr_index].node_id);
  DEBUG_RETURN_EMPTY;
}

/**
  Start a new client socket connection

  @parameter conn         OUT: The new connection created
  @parameter hostname     IN: The hostname to connect to
  @parameter port         IN: The port to connect to
*/
static int start_client_connection(IC_CONNECTION **conn,
                                   gchar *server_name,
                                   gchar *server_port)
{
  int ret_code;
  DEBUG_ENTRY("start_client_connection");

  if (!(*conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                      CONFIG_READ_BUF_SIZE,
                                      NULL, NULL)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  (*conn)->conn_op.ic_prepare_client_connection(*conn,
                                                server_name,
                                                server_port,
                                                NULL, NULL);
  ret_code= (*conn)->conn_op.ic_set_up_connection(*conn, NULL, NULL);
  DEBUG_RETURN_INT(ret_code);
}

/**
  Send configuration files to a cluster server through the ic_pcntrld
  agent process.

  @parameter conn              IN: The connection to the process controller
  @parameter clu_infos         IN: An array through which we get cluster names
  @parameter node_id           IN: Node id of cluster
*/
static int
send_files_to_node(IC_CONNECTION *conn,
                   IC_CLUSTER_CONNECT_INFO **clu_infos,
                   guint32 node_id)
{
  int ret_code;
  guint32 num_clusters= 0;
  guint32 i;
  IC_STRING current_dir;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  gchar buf[IC_MAX_FILE_NAME_SIZE];
  IC_STRING cluster_file_name;
  DEBUG_ENTRY("send_files_to_node");

  ic_set_current_dir(&current_dir);
  while (clu_infos[num_clusters])
    num_clusters++; /* Count number of clusters looking for end NULL */

  if ((ret_code= ic_send_with_cr(conn, ic_copy_cluster_server_files_str)) ||
      (ret_code= ic_send_with_cr_with_num(conn,
                                          ic_cluster_server_node_id_str,
                                          (guint64)node_id)) ||
       (ret_code= ic_send_with_cr_with_num(conn,
                                           ic_number_of_clusters_str,
                                           (guint64)num_clusters)))
    goto error;

  if ((ret_code= ic_proto_send_file(conn,
                                    "config.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)) ||
      (ret_code= ic_proto_send_file(conn,
                                    "grid_common.ini",
                                    current_dir.str)) ||
      (ret_code= ic_receive_config_file_ok(conn, TRUE)))
    goto error;

  for (i= 0; i < num_clusters; i++)
  {
    clu_info= clu_infos[i];
    /* Create cluster_name.ini string */
    buf[0]= 0;
    IC_INIT_STRING(&cluster_file_name, buf, 0, TRUE);
    ic_add_ic_string(&cluster_file_name, &clu_info->cluster_name);
    ic_add_ic_string(&cluster_file_name, &ic_config_ending_string);
    if ((ret_code= ic_proto_send_file(conn,
                                      cluster_file_name.str,
                                      current_dir.str)) ||
        (ret_code= ic_receive_config_file_ok(conn, TRUE)))
      goto error;
  }
error:
  DEBUG_RETURN_INT(ret_code);
}

static void
ic_send_files_cmd(IC_PARSE_DATA *parse_data)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_CONFIG_ERROR err_obj;
  IC_STRING config_dir;
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  IC_CONNECTION *conn= NULL;
  IC_CLUSTER_SERVER_DATA *cs_data;
  int ret_code;
  guint32 i;
  DEBUG_ENTRY("ic_send_files_cmd");

  (void)parse_data;
  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
                                           0, FALSE)))
    goto end;

  ic_set_current_dir(&config_dir);
  /* Read the config.ini file to get information about cluster names */
  if (!(clu_infos= ic_load_cluster_config_from_file(&config_dir,
                                                    (IC_CONF_VERSION_TYPE)0,
                                                    mc_ptr,
                                                    &err_obj)))
  {
    ret_code= err_obj.err_num;
    ic_printf("Failed to open config.ini");
    ic_print_error(ret_code);
    parse_data->exit_flag= TRUE;
    goto end;
  }
  for (i= 0; i < parse_data->cs_index; i++)
  {
    cs_data= &parse_data->cs_data[i];
    /* Send files to all cluster servers one by one */
    if ((ret_code= start_client_connection(&conn,
      cs_data->pcntrl_hostname,
      cs_data->pcntrl_port)))
    {
      ic_printf("Failed to open connection to cluster server id %u",
        cs_data->node_id);
      ic_print_error(ret_code);
      goto end;
    }
    if ((ret_code= send_files_to_node(conn,
                                      clu_infos,
                                      cs_data->node_id)))
    {
      ic_printf("Failed to copy files to cluster server id %u",
        cs_data->node_id);
      ic_print_error(ret_code);
      goto end;
    }
    conn->conn_op.ic_free_connection(conn);
    conn= NULL;
  }
  ic_printf("Copied configuration files to all cluster servers");
end:
  if (conn)
    conn->conn_op.ic_free_connection(conn);
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN_EMPTY;
}

static void
ic_start_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_start_cluster_servers_cmd");
  ic_printf("Found start cluster servers command");
  (void)parse_data;
  DEBUG_RETURN_EMPTY;
}

static void
ic_start_cluster_managers_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_start_cluster_managers_cmd");
  ic_printf("Found start cluster managers command");
  (void)parse_data;
  DEBUG_RETURN_EMPTY;
}

static void
ic_verify_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("ic_verify_cluster_servers_cmd");
  ic_printf("Found verify cluster servers command");
  (void)parse_data;
  DEBUG_RETURN_EMPTY;
}

static void
boot_execute(IC_PARSE_DATA *parse_data)
{
  DEBUG_ENTRY("boot_execute");
  switch (parse_data->command)
  {
    case IC_PREPARE_CLUSTER_SERVER_CMD:
      ic_prepare_cluster_server_cmd(parse_data);
      break;
    case IC_PREPARE_CLUSTER_MANAGER_CMD:
      ic_prepare_cluster_manager_cmd(parse_data);
      break;
    case IC_SEND_FILES_CMD:
      ic_send_files_cmd(parse_data);
      break;
    case IC_START_CLUSTER_SERVERS_CMD:
      ic_start_cluster_servers_cmd(parse_data);
      break;
    case IC_START_CLUSTER_MANAGERS_CMD:
      ic_start_cluster_managers_cmd(parse_data);
      break;
    case IC_VERIFY_CLUSTER_SERVERS_CMD:
      ic_verify_cluster_servers_cmd(parse_data);
      break;
    default:
      ic_printf("No such command");
      parse_data->exit_flag= TRUE;
      break;
  }
  DEBUG_RETURN_EMPTY;
}

static void
init_parse_data(IC_PARSE_DATA *parse_data)
{
  parse_data->command= IC_NO_SUCH_CMD;
}

static void
execute_command(IC_PARSE_DATA *parse_data,
                gchar *parse_buf,
                const guint32 line_size,
                IC_MEMORY_CONTAINER *mc_ptr)
{
  DEBUG_ENTRY("execute_command");
  if (parse_data->exit_flag)
    goto end;
  init_parse_data(parse_data);
  ic_boot_call_parser(parse_buf, line_size, parse_data);
  if (parse_data->exit_flag)
    goto end;
  boot_execute(parse_data);
  if (parse_data->exit_flag)
    goto end;
  /* Release memory allocated by parser, but keep memory container */
  mc_ptr->mc_ops.ic_mc_reset(mc_ptr);
end:
  DEBUG_RETURN_EMPTY;
}

static void
get_line_from_file_content(gchar **file_ptr,
                           gchar *file_end,
                           IC_STRING *ic_str)
{
  gchar parse_char;

  ic_str->str= *file_ptr;
  while (*file_ptr < file_end)
  {
    parse_char= *(*file_ptr);
    (*file_ptr)++; /* Move to next character */
    if (parse_char == CMD_SEPARATOR)
      break;
  }
  ic_str->len= (int)((*file_ptr) - ic_str->str); /* Pointer arithmetics */
  ic_str->is_null_terminated= FALSE;
}

static int
get_line(gchar **file_ptr,
         gchar *file_end,
         gchar *parse_buf,
         guint32 *line_size,
         guint32 *curr_pos,
         guint32 *curr_length,
         gchar **read_buf,
         gboolean *end_of_input)
{
  int ret_code;
  guint32 start_pos= *curr_pos;
  guint32 buf_length= *curr_length;
  IC_STRING ic_str;
  guint32 i;
  gboolean end_of_file= FALSE;

  *end_of_input= FALSE;
  do
  {
    if (ic_unlikely(start_pos == 0))
    {
      if (*file_ptr)
      {
        /* Get next line from buffer read from file */
        get_line_from_file_content(file_ptr, file_end, &ic_str);
        if (ic_str.len + buf_length > COMMAND_READ_BUF_SIZE)
          return IC_ERROR_COMMAND_TOO_LONG;
        /* "Intelligent" memcpy removing CARRIAGE RETURN's */
        for (i= 0; i < ic_str.len; i++)
        {
          if (ic_str.str[i] != CARRIAGE_RETURN)
          {
            parse_buf[buf_length]= ic_str.str[i];
            buf_length++;
          }
        }
        if (*file_ptr == file_end)
          end_of_file= TRUE;
      }
      else
      {
        /* Get next line by reading next command line */
        if ((ret_code= ic_read_one_line(ic_prompt, &ic_str)))
          return ret_code;
        if (ic_str.len + buf_length > COMMAND_READ_BUF_SIZE)
          return IC_ERROR_COMMAND_TOO_LONG;
        memcpy(&parse_buf[buf_length], ic_str.str, ic_str.len);
        buf_length+= ic_str.len;
        if (parse_buf[buf_length - 1] != CMD_SEPARATOR)
        {
          /*
            Always add a space at the end instead of the CARRIAGE RETURN
            when the end of line isn't a ; ending the command. Otherwise
            PREPARE<CR>CLUSTER will be interpreted as PREPARECLUSTER
            instead of as PREPARE CLUSTER which is the correct.
          */
          parse_buf[buf_length]= SPACE_CHAR;
          buf_length++;
        }
        ic_free(ic_str.str);
      }
      *curr_length= buf_length;
    }
    for (i= start_pos; i < buf_length; i++)
    {
      if (ic_unlikely(parse_buf[i] == CMD_SEPARATOR))
      {
        *curr_pos= i + 1;
        goto end;
      }
    }
    /*
      We have read the entire line and haven't found a CMD_SEPARATOR,
      we need to read another line and continue reading until we don't
      fit in the parse buffer or until we find a CMD_SEPARATOR.
    */
    if (buf_length > start_pos)
    {
      /* Move everything to beginning of parse buffer */
      if (start_pos > 0)
      {
        buf_length= *curr_length= buf_length - start_pos;
        memmove(parse_buf, &parse_buf[start_pos], buf_length);
      }
    }
    else
    {
      /* Buffer is empty, start from empty buffer again */
      buf_length= 0;
    }
    start_pos= *curr_pos= 0;
    if (end_of_file)
    {
      /* We've reached the end of file */
      if (buf_length == 0)
      {
        /* No command at end of file is ok, we report end of input */
        *end_of_input= TRUE;
        return 0;
      }
      else
      {
        /* Command without ; at end, this isn't ok */
        return IC_ERROR_NO_FINAL_COMMAND;
      }
    }
  } while (1);

end:
  if (i == start_pos)
  {
    /* An empty command with a single ; signals end of input (also in file) */
    *end_of_input= TRUE;
    return 0;
  }
  *line_size= (*curr_pos - start_pos);
  *read_buf= &parse_buf[start_pos];
  return 0;
}

static gchar *start_text= "\
- iClaustron Bootstrap program\n\
\n\
This program runs by default in command line editing mode.\n\
To finish program in this mode, enter a single command ;\n\
Commands are always separated by ;\n\
The program can also run from a command file provided in the\n\
--command-file parameter.\n\
Finally the program can also run from a directory where\n\
iClaustron configuration files are provided. The main file will\n\
always be called config.ini and the file grid_common.ini will\n\
will always contain the definition of the common Cluster Servers\n\
and Cluster Managers in the Grid. The remaining files will be one\n\
for each cluster with the name as given in config.ini.\n\
Based on this configuration the program will generate a bootstrap\n\
command file and also execute it.\n\
The generation of a command file is done when the option\n\
--generate-command-file is given with file name of command file\n\
";

int main(int argc, char *argv[])
{
  int ret_code;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  IC_PARSE_DATA parse_data;
  gchar *parse_buf= NULL;
  gchar *read_buf;
  gchar *file_content= NULL;
  gchar *file_ptr= NULL;
  gchar *file_end= NULL;
  guint64 file_size;
  guint32 curr_pos= 0;
  guint32 curr_length= 0;
  guint32 line_size;
  gboolean end_of_input;
  gboolean readline_inited= FALSE;

  if ((ret_code= ic_start_program(argc, argv, entries, NULL,
                                  glob_process_name,
                                  start_text, TRUE)))
    goto end;

  if ((ret_code= ic_boot_find_hash_function()))
    goto end;

  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
                                           0, FALSE)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto end;
  }
  if (!(parse_buf= ic_malloc(COMMAND_READ_BUF_SIZE + 1)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto end;
  }
  ic_zero(&parse_data, sizeof(IC_PARSE_DATA));
  parse_data.lex_data.mc_ptr= mc_ptr;

  /* Start by loading the config files */
  if (glob_generate_command_file)
  {
    if (glob_command_file)
    {
      ret_code= IC_ERROR_TWO_COMMAND_FILES;
      goto end;
    }
    glob_command_file= glob_generate_command_file;
    if ((ret_code= generate_command_file(parse_buf)))
      goto end;
  }

  if (glob_command_file)
  {
    if ((ret_code= ic_get_file_contents(glob_command_file,
                                        &file_content,
                                        &file_size)))
      goto end;
    file_ptr= file_content;
    file_end= file_content + file_size;
  }
  else
  {
    ic_init_readline(glob_history_size);
    readline_inited= TRUE;
  }
  while (!(ret_code= get_line(&file_ptr,
                              file_end,
                              parse_buf,
                              &line_size,
                              &curr_pos,
                              &curr_length,
                              &read_buf,
                              &end_of_input)))
  {
    if (end_of_input)
      goto end;
    execute_command(&parse_data, read_buf, line_size, mc_ptr);
    if (parse_data.exit_flag)
      goto end;
  }

end:
  if (readline_inited)
    ic_close_readline();
  if (file_content)
    ic_free(file_content);
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  if (parse_buf)
    ic_free(parse_buf);
  if (ret_code)
    ic_print_error(ret_code);
  return ret_code;
}
