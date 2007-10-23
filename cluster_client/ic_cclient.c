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

#include <ic_common.h>
#include <stdio.h>
#include <readline/readline.h>

static gchar *glob_server_ip= "127.0.0.1";
static gchar *glob_server_port= "10006";
static guint32 glob_history_size= 100;
static gchar *ic_prompt= "iclaustron client> ";

static GOptionEntry entries[] = 
{
  { "server_name", 0, 0, G_OPTION_ARG_STRING, &glob_server_ip,
    "Set Server Host address of Clustrer Manager", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING, &glob_server_port,
    "Set Server Port of Cluster Manager", NULL},
  { "history_size", 0, 0, G_OPTION_ARG_INT, &glob_history_size,
    "Set Size of Command Line History", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
send_parse_string(IC_STRING *parse_string)
{
  return 0;
}


static int
execute_command(IC_STRING **str_array, guint32 num_lines)
{
  guint32 i;
  for (i= 0; i < num_lines; i++)
    printf("%s\n", str_array[i]->str);
  return 0;
}

static int
read_one_line(IC_STRING *out_str)
{
#ifdef HAVE_LIBREADLINE
  IC_STRING line_str;
  int ret_value;

  line_str.str= readline(ic_prompt);
  if (!line_str.str)
    return 1;
  line_str.len= 2048;
  ic_set_up_ic_string(&line_str);
  if (!line_str.is_null_terminated)
  {
    ic_free(line_str.str);
    return 1;
  }
  if (line_str.len)
    add_history(line_str.str);
  ret_value= ic_strdup(out_str, &line_str);
  ic_free(line_str.str);
  return ret_value;
#else
  IC_STRING line_str;
  int ret_value;
  gchar line[2048];

  printf("%s", ic_prompt);
  line_str.str= fgets(line, sizeof(line), stdin);
  if (!line_str.str)
    return 1;
  line_str.len= 2048;
  ic_set_up_ic_string(&line_str);
  if (!line_str.is_null_terminated)
    return 1;
  if (line_str.str[line_str.len - 1] == CARRIAGE_RETURN)
  {
    line_str.str[line_str.len - 1]= NULL_BYTE;
    line_str.len--;
  }
  ret_value= ic_strdup(out_str, &line_str);
  return ret_value;
#endif
}

static gboolean
check_last_line(IC_STRING *ic_str)
{
  if (ic_str->str[ic_str->len - 1] == ';')
    return TRUE;
  return FALSE;
}

static int
command_interpreter()
{
  guint32 lines, i;
  int error;
  IC_STRING line_str;
  IC_STRING *line_ptr= &line_str;
  IC_STRING *line_ptrs[256];
  IC_STRING line_strs[256];

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
      if ((error= read_one_line(line_ptr)))
      {
        printf("error = %u\n", error);
        return error;
      }
      if (line_ptr->len == 0)
      {
        if (line_ptr->str)
          ic_free(line_ptr->str);
        continue;
      }
      IC_COPY_STRING(line_ptrs[lines], line_ptr);
      lines++;
    } while (!check_last_line(line_ptr));
    if (lines == 1 &&
        ((ic_cmp_null_term_str("quit;", line_ptrs[0])) ||
         (ic_cmp_null_term_str("exit;", line_ptrs[0])) ||
          ic_cmp_null_term_str("q;", line_ptrs[0])))
    {
      ic_free(line_ptrs[0]->str);
      break;
    }
    if ((error= execute_command(&line_ptrs[0], lines)))
    {
      printf("error = %u\n", error);
      return error;
    }
    for (i= 0; i < lines; i++)
      ic_free(line_ptrs[i]->str);
  } while (TRUE);
  return 0;

error:
  for (i= 0; i < lines; i++)
    ic_free(line_ptrs[i]->str);
  return error;
}

static int
connect_cluster_mgr(IC_CONNECTION **conn)
{
  IC_CONNECTION *loc_conn;
  int ret_code;

  if (!(loc_conn= ic_create_socket_object(TRUE, FALSE,
                                          FALSE, FALSE,
                                          NULL, NULL)))
  {
    printf("Failed to create Connection object\n");
    return 1;
  }
  printf("Connecting to Cluster Manager at %s:%s\n",
         glob_server_ip, glob_server_port);
  loc_conn->server_name= glob_server_ip;
  loc_conn->server_port= glob_server_port;
  if ((ret_code= loc_conn->conn_op.ic_set_up_connection(loc_conn)))
  {
    printf("Failed to connect to Cluster Manager\n");
    loc_conn->conn_op.ic_free_connection(loc_conn);
    return 1;
  }
  *conn= loc_conn;
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code;
  GError *error= NULL;
  GOptionContext *context;
  IC_CONNECTION *conn;

  context= g_option_context_new("- iClaustron Command Client");
  if (!context)
    goto error;
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);

  if (ic_init())
    return ret_code;
#ifdef HAVE_LIBREADLINE
  using_history();
  stifle_history(glob_history_size);
#endif
  if ((ret_code= connect_cluster_mgr(&conn)))
    goto error;
  ret_code= command_interpreter(conn);
  conn->conn_op.ic_free_connection(conn);
#ifdef HAVE_LIBREADLINE
  clear_history();
#endif
  ic_end();
  return ret_code;

parse_error:
  printf("No such program option\n");
error:
  return ret_code;
}
