/* Copyright (C) 2007-2012 iClaustron AB

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
#include <ic_string.h>
#include <ic_readline.h>
#include <ic_connection.h>
#include <ic_protocol_support.h>

static gchar *glob_server_ip= "127.0.0.1";
static gchar *glob_server_port= IC_DEF_CLUSTER_MANAGER_PORT_STR;
static guint32 glob_history_size= 100;
static gchar *ic_prompt= "iClaustron client> ";

static GOptionEntry entries[] = 
{
  { "server-name", 0, 0, G_OPTION_ARG_STRING, &glob_server_ip,
    "Set Server Host address of Clustrer Manager", NULL},
  { "server-port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Server Port of Cluster Manager", NULL},
  { "history-size", 0, 0, G_OPTION_ARG_INT, &glob_history_size,
    "Set Size of Command Line History", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
execute_command(IC_CONNECTION *conn, IC_STRING **str_array, guint32 num_lines)
{
  gchar *read_buf;
  guint32 read_size, i;
  int ret_code;
  DEBUG_ENTRY("execute_command");

  for (i= 0; i < num_lines; i++)
  {
    if ((ret_code= ic_send_with_cr(conn, str_array[i]->str)))
      goto error;
    if ((ret_code= ic_send_with_cr(conn, ic_empty_string)))
      goto error;
  }
  while (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (read_size == 0)
      break;
    read_buf[read_size]= 0;
    ic_printf("%s\n", read_buf);
  }
error:
  DEBUG_RETURN_INT(ret_code);
}

static gchar *help_str[] =
{
  "help",
  NULL,
};

static int
command_interpreter(IC_CONNECTION *conn)
{
  guint32 lines, i;
  int error;
  IC_STRING line_str;
  IC_STRING *line_ptr= &line_str;
  IC_STRING *line_ptrs[256];
  IC_STRING line_strs[256];
  DEBUG_ENTRY("command_interpreter");

  for (i= 0; i < 256; i++)
    line_ptrs[i]= &line_strs[i];
  do
  {
    lines= 0;
    do
    {
      if (lines >= 2048)
      {
        error= 1;
        lines--;
        goto error;
      }
      if ((error= ic_read_one_line(ic_prompt, line_ptr)))
      {
        ic_print_error(error);
        DEBUG_RETURN_INT(error);
      }
      if (line_ptr->len == 0)
      {
        if (line_ptr->str)
          ic_free(line_ptr->str);
        continue;
      }
      IC_COPY_STRING(line_ptrs[lines], line_ptr);
      lines++;
    } while (!ic_check_last_line(line_ptr));
    if (lines == 1 &&
        ((ic_cmp_null_term_str("quit;", line_ptrs[0])) ||
         (ic_cmp_null_term_str("exit;", line_ptrs[0])) ||
          ic_cmp_null_term_str("q;", line_ptrs[0])))
    {
      ic_free(line_ptrs[0]->str);
      break;
    }
    if (lines == 1 && (ic_cmp_null_term_str("help;", line_ptrs[0])))
      ic_output_help(help_str);
    else if ((error= execute_command(conn, &line_ptrs[0], lines)))
    {
      ic_print_error(error);
      goto error;
    }
    for (i= 0; i < lines; i++)
      ic_free(line_ptrs[i]->str);
  } while (TRUE);
  DEBUG_RETURN_INT(0);

error:
  for (i= 0; i < lines; i++)
    ic_free(line_ptrs[i]->str);
  DEBUG_RETURN_INT(error);
}

static int
connect_cluster_mgr(IC_CONNECTION **conn)
{
  IC_CONNECTION *loc_conn;
  int ret_code;
  DEBUG_ENTRY("connect_cluster_mgr");

  if (!(loc_conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                          COMMAND_READ_BUF_SIZE,
                                          NULL, NULL)))
  {
    DEBUG_PRINT(COMM_LEVEL, ("Failed to create Connection object"));
    DEBUG_RETURN_INT(1);
  }
  DEBUG_PRINT(PROGRAM_LEVEL,
    ("Connecting to Cluster Manager at %s:%s",
     glob_server_ip, glob_server_port));
  loc_conn->conn_op.ic_prepare_client_connection(loc_conn,
                                                 glob_server_ip,
                                                 glob_server_port,
                                                 NULL,
                                                 NULL);
  if ((ret_code= loc_conn->conn_op.ic_set_up_connection(loc_conn, NULL, NULL)))
  {
    DEBUG_PRINT(PROGRAM_LEVEL,
     ("Failed to connect to Cluster Manager"));
    ic_print_error(ret_code);
    loc_conn->conn_op.ic_free_connection(loc_conn);
    DEBUG_RETURN_INT(1);
  }
  *conn= loc_conn;
  DEBUG_RETURN_INT(0);
}

int main(int argc, char *argv[])
{
  int ret_code= 1;
  IC_CONNECTION *conn;

  if ((ret_code= ic_start_program(argc,
                                  argv,
                                  entries,
                                  NULL,
                                  "ic_cclient",
                                  "- iClaustron Command Client",
                                  TRUE,
                                  FALSE)))
    return ret_code;

  ic_init_readline(glob_history_size);
  if ((ret_code= connect_cluster_mgr(&conn)))
    goto error;
  ret_code= command_interpreter(conn);
  conn->conn_op.ic_free_connection(conn);
  ic_close_readline();
  ic_end();
  return ret_code;

error:
  return ret_code;
}
