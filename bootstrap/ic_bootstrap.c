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
#include <ic_readline.h>
#include <ic_lex_support.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_boot_int.h"

static const gchar *glob_process_name= "ic_bootstrap";
static gchar *glob_command_file= NULL;
static guint32 glob_history_size= 100;
static gchar *ic_prompt= "iClaustron bootstrap> ";

static GOptionEntry entries[]=
{
  { "command-file", 0, 0, G_OPTION_ARG_STRING,
    &glob_command_file,
    "Use a command file and provide its name", NULL},
  { "history_size", 0, 0, G_OPTION_ARG_INT, &glob_history_size,
    "Set Size of Command Line History", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static int
load_ic_config_files(IC_PARSE_DATA *parse_data)
{
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  IC_STRING config_dir;
  IC_CLUSTER_CONFIG *grid_cluster;

  ic_set_current_dir(&config_dir);

  if (!(clu_infos= ic_load_cluster_config_from_file(&config_dir,
                                                    (IC_CONF_VERSION_TYPE)0,
                                                    parse_data->mc_ptr,
                                                    &parse_data->err_obj)))
    return parse_data->err_obj.err_num;
  parse_data->clu_infos= clu_infos;

  if (!(grid_cluster= ic_load_grid_common_config_server_from_file(
                         &config_dir,
                         (IC_CONF_VERSION_TYPE)0,
                         parse_data->mc_ptr,
                         *clu_infos,
                         &parse_data->err_obj)))
    return parse_data->err_obj.err_num;
  parse_data->grid_cluster= grid_cluster;
  return 0;
}

static void
ic_prepare_cluster_server_cmd(IC_PARSE_DATA *parse_data)
{
  (void)parse_data;
}

static void
ic_send_files_cmd(IC_PARSE_DATA *parse_data)
{
  (void)parse_data;
}

static void
ic_start_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  (void)parse_data;
}

static void
ic_start_cluster_managers_cmd(IC_PARSE_DATA *parse_data)
{
  (void)parse_data;
}

static void
ic_verify_cluster_servers_cmd(IC_PARSE_DATA *parse_data)
{
  (void)parse_data;
}

static void
boot_execute(IC_PARSE_DATA *parse_data)
{
  switch (parse_data->command)
  {
    case IC_PREPARE_CLUSTER_SERVER_CMD:
      ic_prepare_cluster_server_cmd(parse_data);
      return;
    case IC_SEND_FILES_CMD:
      ic_send_files_cmd(parse_data);
      return;
    case IC_START_CLUSTER_SERVERS_CMD:
      ic_start_cluster_servers_cmd(parse_data);
      return;
    case IC_START_CLUSTER_MANAGERS_CMD:
      ic_start_cluster_managers_cmd(parse_data);
      return;
    case IC_VERIFY_CLUSTER_SERVERS_CMD:
      ic_verify_cluster_servers_cmd(parse_data);
      return;
    default:
      ic_printf("No such command");
      parse_data->exit_flag= TRUE;
      return;
  }
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
  if (parse_data->exit_flag)
    return;
  init_parse_data(parse_data);
  ic_boot_call_parser(parse_buf, line_size, parse_data);
  if (parse_data->exit_flag)
    return;
  boot_execute(parse_data);
  if (parse_data->exit_flag)
    return;
  /* Release memory allocated by parser, but keep memory container */
  mc_ptr->mc_ops.ic_mc_reset(mc_ptr);
}

static int
get_line(const gchar *file_content,
         guint64 file_size,
         guint64 *curr_pos,
         gchar *parse_buf,
         guint32 *line_size)
{
  guint64 inx;
  guint32 parse_inx= 0;

  for (inx= *curr_pos; inx < file_size; inx++)
  {
    if (ic_unlikely(parse_inx == COMMAND_READ_BUF_SIZE))
      return IC_ERROR_COMMAND_TOO_LONG;
    if (ic_unlikely(file_content[inx] == CARRIAGE_RETURN ||
                    file_content[inx] == CMD_SEPARATOR))
    {
      parse_buf[parse_inx]= CMD_SEPARATOR;
      parse_inx++;
      /* Step past the last byte for next command line */
      *curr_pos= inx + 1;
      goto end;
    }
    parse_buf[parse_inx]= file_content[inx];
    parse_inx++;
  }
  *curr_pos= file_size;
  parse_buf[parse_inx]= CMD_SEPARATOR;
  parse_inx++;

end:
  *line_size= parse_inx;
  return 0;
}

static int
get_line_from_stdin(gchar *parse_buf,
                    guint32 *line_size,
                    guint64 *curr_pos,
                    guint64 *curr_length,
                    gchar **read_buf)
{
  int ret_code;
  guint64 start_pos= *curr_pos;
  guint64 buf_length= *curr_length;
  IC_STRING ic_str;
  guint64 inx;

  do
  {
    if (ic_unlikely(start_pos == 0))
    {
      if (!(ret_code= ic_read_one_line(ic_prompt, &ic_str)))
        return ret_code;
      memcpy(parse_buf, ic_str.str, ic_str.len); 
      ic_free(ic_str.str);
      start_pos= *curr_pos= 0;
      buf_length= *curr_length= ic_str.len;
    }
    for (inx= start_pos; inx < buf_length; inx++)
    {
      if (ic_unlikely(parse_buf[inx] == CMD_SEPARATOR))
      {
        *curr_pos= inx + 1;
        goto end;
      }
    }
    if (inx > start_pos)
    {
      *curr_pos= 0;
      *curr_length= 0;
      goto end;
    }
    /* We already processed all the commands in this line */
    start_pos= 0;
  } while (1);

end:
  parse_buf[inx]= CMD_SEPARATOR;
  *line_size= (inx - start_pos) + 1;
  *read_buf= &parse_buf[start_pos];
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_MEMORY_CONTAINER *lex_mc_ptr= NULL;
  IC_MEMORY_CONTAINER *parse_mc_ptr= NULL;
  IC_PARSE_DATA parse_data;
  gchar *parse_buf= NULL;
  gchar *read_buf;
  gchar *file_content= NULL;
  guint64 file_size;
  guint64 curr_pos= 0;
  guint64 curr_length= 0;
  guint32 line_size;
  gboolean readline_inited= FALSE;

  if ((ret_code= ic_start_program(argc, argv, entries, NULL,
                                  glob_process_name,
            "- iClaustron Bootstrap program", TRUE)))
    goto end;

  if (!(lex_mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
                                               0, FALSE)))
  {
    ret_code= IC_ERROR_MEM_ALLOC;
    goto end;
  }
  if (!(parse_mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE,
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
  parse_data.lex_data.mc_ptr= lex_mc_ptr;
  parse_data.mc_ptr= parse_mc_ptr;

  /* Start by loading the config files */
  if ((ret_code= load_ic_config_files(&parse_data)))
    goto end;

  if (glob_command_file)
  {
    if ((ret_code= ic_get_file_contents(glob_command_file,
                                        &file_content,
                                        &file_size)))
      goto end;
    while (!(ret_code= get_line(file_content,
                                file_size,
                                &curr_pos,
                                parse_buf,
                                &line_size)))
    {
      /* We have a command to execute in read_buf, size = line_size */
      execute_command(&parse_data,
                      parse_buf,
                      line_size,
                      lex_mc_ptr);
      if (parse_data.exit_flag)
        goto end;
    }
  }
  else
  {
    ic_init_readline(glob_history_size);
    readline_inited= TRUE;
    while (!(ret_code= get_line_from_stdin(parse_buf,
                                           &line_size,
                                           &curr_pos,
                                           &curr_length,
                                           &read_buf)))
    {
      execute_command(&parse_data, read_buf, line_size, lex_mc_ptr);
      if (parse_data.exit_flag)
        goto end;
    }
  }

end:
  if (readline_inited)
    ic_close_readline();
  if (file_content)
    ic_free(file_content);
  if (lex_mc_ptr)
    lex_mc_ptr->mc_ops.ic_mc_free(lex_mc_ptr);
  if (parse_mc_ptr)
    parse_mc_ptr->mc_ops.ic_mc_free(parse_mc_ptr);
  if (parse_buf)
    ic_free(parse_buf);
  ic_print_error(ret_code);
  return ret_code;
}
