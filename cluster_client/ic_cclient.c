/* Copyright (C) 2007, 2014 iClaustron AB

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
  }
  if ((ret_code= ic_send_empty_line(conn)))
    goto error;
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

static gchar *help_str[]=
{
  "All commands entered are executed when an end character ';' is found",
  "at the end of the command line. Commands can span many lines, they are",
  "not executed until a line is completed with a ;",
  "",
  "The following commands are currently supported:",
  "DIE, KILL, MOVE, RESTART, PERFORM BACKUP, START, LIST, LISTEN,",
  "SHOW CLUSTER, SHOW CLUSTER STATUS, SHOW CONNECTIONS, SHOW CONFIG,",
  "SHOW MEMORY, SHOW STATVARS, SHOW STATS, SET STAT_LEVEL, USE CLUSTER,",
  "USE VERSION NDB, USE VERSION ICLAUSTRON, DISPLAY STATS, TOP"
  "help",
  "",
  "All commands are not fully implemented yet",
  "",
  "The command help show connections; shows help about the command"
  "SHOW CONNECTIONS. There is specific help available on all commands",
  "listed above.",
  NULL,
};

static gchar *help_die_str[]=
{
  "",
  NULL,
};

static gchar *help_kill_str[]=
{
  "",
  NULL,
};

static gchar *help_move_str[]=
{
  "",
  NULL,
};

static gchar *help_restart_str[]=
{
  "",
  NULL,
};

static gchar *help_perform_backup_str[]=
{
  "",
  NULL,
};

static gchar *help_start_str[]=
{
  "",
  NULL,
};

static gchar *help_list_str[]=
{
  "",
  NULL,
};

static gchar *help_listen_str[]=
{
  "",
  NULL,
};

static gchar *help_show_cluster_str[]=
{
  "",
  NULL,
};

static gchar *help_show_cluster_status_str[]=
{
  "",
  NULL,
};

static gchar *help_show_connections_str[]=
{
  "",
  NULL,
};

static gchar *help_show_config_str[]=
{
  "",
  NULL,
};

static gchar *help_show_memory_str[]=
{
  "",
  NULL,
};

static gchar *help_show_statvars_str[]=
{
  "",
  NULL,
};

static gchar *help_show_stats_str[]=
{
  "",
  NULL,
};

static gchar *help_set_stat_level_str[]=
{
  "",
  NULL,
};

static gchar *help_use_cluster_str[]=
{
  "",
  NULL,
};

static gchar *help_use_version_ndb_str[]=
{
  "",
  NULL,
};

static gchar *help_use_version_iclaustron_str[]=
{
  "",
  NULL,
};

static gchar *help_display_stats_str[]=
{
  "",
  NULL,
};

static gchar *help_top_str[]=
{
  "",
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
  {
    line_ptrs[i]= &line_strs[i];
  }
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
        {
          ic_free(line_ptr->str);
        }
        continue;
      }
      IC_COPY_STRING(line_ptrs[lines], line_ptr);
      lines++;
    } while (!ic_check_last_line(line_ptr));
    if (lines == 1 &&
        ((ic_cmp_null_term_str_upper("QUIT;", line_ptrs[0]) == 0) ||
         (ic_cmp_null_term_str_upper("EXIT;", line_ptrs[0]) == 0) ||
         (ic_cmp_null_term_str_upper("Q;", line_ptrs[0]) == 0)))
    {
      ic_free(line_ptrs[0]->str);
      break;
    }
    if (lines == 1 && (ic_cmp_null_term_str_upper_part("HELP", line_ptrs[0]) == 0))
    {
      if (ic_cmp_null_term_str_upper("HELP;", line_ptrs[0]))
      {
        ic_output_help(help_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP DIE;", line_ptrs[0]))
      {
        ic_output_help(help_die_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP KILL;", line_ptrs[0]))
      {
        ic_output_help(help_kill_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP MOVE;", line_ptrs[0]))
      {
        ic_output_help(help_move_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP RESTART;", line_ptrs[0]))
      {
        ic_output_help(help_restart_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP PERFORM BACKUP;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_perform_backup_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP START;", line_ptrs[0]))
      {
        ic_output_help(help_start_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP LIST;", line_ptrs[0]))
      {
        ic_output_help(help_list_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP LISTEN;", line_ptrs[0]))
      {
        ic_output_help(help_listen_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CLUSTER;", line_ptrs[0]))
      {
        ic_output_help(help_show_cluster_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CLUSTER STATUS;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_show_cluster_status_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CONNECTIONS;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_show_connections_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CONFIG;", line_ptrs[0]))
      {
        ic_output_help(help_show_config_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW MEMORY;", line_ptrs[0]))
      {
        ic_output_help(help_show_memory_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW STATVARS;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_show_statvars_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW STATS;", line_ptrs[0]))
      {
        ic_output_help(help_show_stats_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SET STAT_LEVEL;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_set_stat_level_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP USE CLUSTER;", line_ptrs[0]))
      {
        ic_output_help(help_use_cluster_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP USE VERSION NDB;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_use_version_ndb_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP USE VERSION ICLAUSTRON;",
                                          line_ptrs[0]))
      {
        ic_output_help(help_use_version_iclaustron_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP DISPLAY STATS;", line_ptrs[0]))
      {
        ic_output_help(help_display_stats_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP TOP;", line_ptrs[0]))
      {
        ic_output_help(help_top_str);
      }
      else
      {
        ic_printf("Error: No such command to get help on");
      }
    }
    else if ((error= execute_command(conn, &line_ptrs[0], lines)))
    {
      ic_print_error(error);
      goto error;
    }
    for (i= 0; i < lines; i++)
    {
      ic_free(line_ptrs[i]->str);
    }
  } while (TRUE);
  DEBUG_RETURN_INT(0);

error:
  for (i= 0; i < lines; i++)
  {
    ic_free(line_ptrs[i]->str);
  }
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
