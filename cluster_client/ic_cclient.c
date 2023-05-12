/* Copyright (C) 2007, 2015 iClaustron AB

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
#include <ic_proto_str.h>

static gchar *glob_server_ip= "127.0.0.1";
static gchar *glob_server_port= IC_DEF_CLUSTER_MANAGER_PORT_STR;
static guint32 glob_history_size= 100;
static guint64 glob_connect_code= 0;
static gboolean glob_executing= TRUE;
static IC_MUTEX *end_mutex= NULL;
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

static int connect_cluster_mgr(IC_CONNECTION **conn);

static gchar *ic_help_str[]=
{
  "All commands entered are executed when an end character ';' is found",
  "at the end of the command line. Commands can span many lines, they are",
  "not executed until a line is completed with a ;",
  "",
  "The following commands are currently supported:",
  "First a set of commands that perform actions to change the cluster status:",
  "START, RESTART, STOP, DIE, KILL, MOVE, PERFORM BACKUP,",
  "PERFORM ROLLING UPGRADE",
  "",
  "Next a set of commands to display statistics about the cluster operations",
  "DISPLAY STATS, TOP",
  "",
  "Next a set of commands to set the state of the statistics and of the",
  "parser",
  "SET STAT_LEVEL, USE CLUSTER USE VERSION NDB, USE VERSION ICLAUSTRON",
  "",
  "Then a set of commands to display the status of the cluster in various",
  "manners",
  "LIST CLUSTERS, LISTEN CLUSTER_LOG, SHOW CLUSTER, SHOW CLUSTER STATUS,",
  "SHOW CONNECTIONS, SHOW CONFIG, SHOW MEMORY, SHOW STATVARS",
  "",
  "There is also help commands available:",
  "help, displays this help text",
  "help command, help on a specific command",
  "help target, help on how to specify a target for the command",
  "",
  "All commands are not fully implemented yet",
  NULL,
};

static gchar *ic_help_target_str[]=
{
  "command CLUSTER MANAGER node reference",
  "command CLUSTER SERVER node reference",
  "command DATA SERVER node reference",
  "command FILE SERVER node reference",
  "command REPLICATION SERVER node reference",
  "command RESTORE node reference",
  "command node reference",
  "",
  "command is any of DIE, KILL, STOP, SHOW CONNECTIONS, SHOW CONFIG,",
  "SHOW MEMORY.",
  "",
  "node reference is any of the following:",
  "ALL: all nodes in all clusters",
  "NODE ALL: All nodes in the current cluster",
  "CLUSTER id NODE id: The node specified by cluster id and node id",
  "CLUSTER id NODE name: The node specified by cluster id and node name",
  "CLUSTER name NODE id: The node specified by cluster name and node id",
  "CLUSTER name NODE name: The node specified by cluster name and node name",
  "NODE id: The node specified by node id in current cluster",
  "NODE name: The node specified by node name in current cluster",
  "",
  "We can only affect the cluster manager we are connected to by using the",
  "commands STOP, DIE or KILL and SHOW commands. All other commands will",
  "ignore the node we are connected to since this node is needed to execute",
  "the command",
  "",
  "The PERFORM ROLLING UPGRADE CLUSTER MANAGERS is a specific case used",
  "to upgrade the cluster managers including the node we are currently",
  "connected to. This specific command requires that we are connected to",
  "at least two cluster managers",
  "",
  NULL,
};

static gchar *ic_help_start_str[]=
{
  "START target",
  "START target INITIAL",
  "START target RESTART",
  "START target INITIAL RESTART",
  "",
  "Starts the nodes in the target with initial flag and restart flag",
  "",
  "If initial flag is set then the node will be started as an empty node",
  "All recovery data will be lost in this node, this is good as the very",
  "first start of a node and after some sort of corruption of the node data.",
  "Could also be used when bringing in a replacement of a computer that",
  "completely failed",
  "",
  "The restart flag indicates whether the node should be restarted after a",
  "stop, die or kill command and if the restart command should be available",
  "for the node. If not set then one has to always use the start command to",
  "explicitly start the node. If restart flag is set the node will also",
  "automatically restart after a node crash",
  "",
  NULL,
};

static gchar *ic_help_restart_str[]=
{
  "RESTART target",
  "Restarts the nodes in the target",
  "",
  "If the cluster manager that we are connected to is restarted then",
  "obviously our connection will be closed when this node is restarted,",
  "our node will however be the last node to be restarted, so status on",
  "the other nodes restarted will be provided first",
  "",
  NULL,
};

static gchar *ic_help_stop_str[]=
{
  "STOP target",
  "",
  "Stops all nodes in the target in a graceful manner",
  "",
  NULL,
};

static gchar *ic_help_die_str[]=
{
  "DIE target",
  "",
  "Sends a terminate signal to all nodes in the target",
  "",
  NULL,
};

static gchar *ic_help_kill_str[]=
{
  "KILL target",
  "",
  "Kills all nodes in the target immediately without stopping gracefully or",
  "terminating them",
  "",
  NULL,
};

static gchar *ic_help_move_str[]=
{
  "MOVE",
  "MOVE CLUSTER id",
  "MOVE CLUSTER name",
  "",
  "MOVE command not implemented yet",
  "",
  NULL,
};

static gchar *ic_help_perform_backup_str[]=
{
  "PERFORM BACKUP",
  "PERFORM BACKUP CLUSTER id",
  "PERFORM BACKUP CLUSTER name",
  "",
  "Perform a backup of one cluster, specified as either:",
  "current cluster, cluster by id or cluster by name",
  "",
  NULL,
};

static gchar *ic_help_perform_rolling_upgrade_str[]=
{
  "PERFORM ROLLING UPGRADE",
  "PERFORM ROLLING UPGRADE CLUSTER id",
  "PERFORM ROLLING UPGRADE CLUSTER name",
  "PERFORM ROLLING UPGRADE CLUSTER SERVERS",
  "PERFORM ROLLING UPGRADE CLUSTER MANAGERS",
  "",
  "Perform a rolling upgrade of one cluster, specified as either:",
  "current cluster, cluster by id or cluster by name, will not affect",
  "cluster servers and cluster managers since they are not connected to",
  "a specific cluster",
  "",
  "PERFORM ROLLING UPGRADE CLUSTER SERVERS will upgrade all cluster servers",
  "PERFORM ROLLING UPGRADE CLUSTER MANAGERS will upgrade all cluster",
  "managers and will as the last thing upgrade the cluster manager we are"
  "connected to, this will close the connection to the cluster manager.",
  "The connection will then be automatically restored after which we can"
  "check if the status of the cluster manager is correct by e.g. using the",
  "SHOW MANAGER STATUS command",
  "",
  "So to upgrade a complete iClaustron one needs to perform the following"
  "commands:",
  "PERFORM ROLLING UPGRADE on each of the clusters in the grid (these",
  "commands can be executed in parallel)",
  "After all these commands are completed one runs:",
  "PERFORM ROLLING UPGRADE CLUSTER SERVERS to upgrade all cluster servers",
  "The final step is to then run:",
  "PERFORM ROLLING UPGRADE CLUSTER MANAGERS to upgrade all cluster managers",
  "After this command is completed we have upgraded the entire iClaustron",
  "grid to a new version",
  "",
  NULL,
};

static gchar *ic_help_display_stats_str[]=
{
  "DISPLAY STATS NODE id group_specifier variable_specifier",
  "DISPLAY STATS NODE name group_specifier variable_specifier",
  "DISPLAY STATS CLUSTER id NODE id group_specifier variable_specifier",
  "DISPLAY STATS CLUSTER id NODE name group_specifier variable_specifier",
  "DISPLAY STATS CLUSTER name NODE id group_specifier variable_specifier",
  "DISPLAY STATS CLUSTER name NODE name group_specifier variable_specifier",
  "",
  "Display statistics on a specific node given by cluster id/name and",
  "node id/name (cluster can be omitted in which case the default cluster",
  "is used)",
  "",
  "group_specifier can be either GROUP ALL in which case all statistics",
  "groups on the node is displayed or GROUP identifer in which case only",
  "statistics belonging to the specified group is displayed",
  "",
  "variable_specifier can be omitted in which case all variables are",
  "displayed, or VARIABLE identifier is given in which case only statistics",
  "of the given variable is provided",
  "",
  NULL,
};

static gchar *ic_help_top_str[]=
{
  "TOP",
  "TOP CLUSTER id",
  "TOP CLUSTER name",
  "",
  "Display CPU statistics on a cluster given its name or the default cluster",
  "(if a name is omitted)",
  "",
  NULL,
};

static gchar *ic_help_set_stat_level_str[]=
{
  "SET STAT_LEVEL NODE id group_specifier variable_specifier",
  "SET STAT_LEVEL NODE name group_specifier variable_specifier",
  "SET STAT_LEVEL CLUSTER id NODE id group_specifier variable_specifier",
  "SET STAT_LEVEL CLUSTER id NODE name group_specifier variable_specifier",
  "SET STAT_LEVEL CLUSTER name NODE id group_specifier variable_specifier",
  "SET STAT_LEVEL CLUSTER name NODE name group_specifier variable_specifier",
  "",
  "Set statistics level on a specific node given by cluster id/name and",
  "node id/name (cluster can be omitted in which case the default cluster",
  "is used)",
  "",
  "group_specifier can be either GROUP ALL in which case all statistics"
  "groups on the node is displayed or GROUP identifer in which case only"
  "statistics belonging to the specified group is displayed",
  "",
  "variable_specifier can be omitted in which case all variables are"
  "displayed, or VARIABLE identifier is given in which case only statistics"
  "of the given variable is provided",
  "",
  NULL,
};

static gchar *ic_help_use_cluster_str[]=
{
  "USE CLUSTER id",
  "USE CLUSTER name",
  "",
  "Set default cluster either by name or by id",
  "",
  NULL,
};

static gchar *ic_help_use_version_ndb_str[]=
{
  "USE VERSION NDB version_identifier",
  "",
  "Set NDB version to be used for the start command on the data servers of"
  "the clusters. A version identifier could be e.g."
  "mysql-cluster-gpl-7.4.2",
  "",
  "Also used by the perform rolling upgrade command to set the new version",
  "to use",
  "",
  "This variable is set by default to the version number set in the cluster"
  "manager used to execute the commands",
  "",
  NULL,
};

static gchar *ic_help_use_version_iclaustron_str[]=
{
  "USE VERSION ICLAUSTRON version_identifier",
  "",
  "Set iClaustron version to be used for the start command of all nodes",
  "except the data servers and for the perform rolling upgrade command",
  "",
  "A version identifier could be e.g. iclaustron-0.0.1",
  "",
  "This variable is set by default to the version number set in the cluster"
  "manager used to execute the commands",
  "",
  NULL,
};

static gchar *ic_help_list_clusters_str[]=
{
  "LIST CLUSTERS",
  "",
  "List the available clusters",
  "",
  NULL,
};

static gchar *ic_help_listen_cluster_log_str[]=
{
  "LISTEN CLUSTER_LOG",
  "",
  "Listen to the cluster log written by the cluster manager",
  "",
  NULL,
};

static gchar *ic_help_show_cluster_str[]=
{
  "SHOW CLUSTER",
  "SHOW CLUSTER id",
  "SHOW CLUSTER name",
  "",
  "Show configuration of a specific cluster given by default cluster",
  "cluster id or cluster name",
  "",
  NULL,
};

static gchar *ic_help_show_cluster_status_str[]=
{
  "SHOW CLUSTER STATUS",
  "SHOW CLUSTER STATUS id",
  "SHOW CLUSTER STATUS name",
  "",
  "Show status of nodes in a cluster as given by default cluster,"
  "cluster id or cluster name.",
  "",
  NULL,
};

static gchar *ic_help_show_connections_str[]=
{
  "SHOW CONNECTIONS target",
  "",
  "Show status of connections to a specific set of nodes as specified",
  "by target",
  "",
  NULL,
};

static gchar *ic_help_show_config_str[]=
{
  "SHOW CONFIG target",
  "",
  "Show config of a specific set of nodes as specified by target",
  "",
  NULL,
};

static gchar *ic_help_show_memory_str[]=
{
  "SHOW MEMORY target",
  "",
  "Show memory usage of a specific set of nodes as specified by target",
  "",
  NULL,
};

static gchar *ic_help_show_statvars_str[]=
{
  "SHOW STATVARS NODE id group_specifier",
  "SHOW STATVARS CLUSTER id NODE id group_specifier",
  "SHOW STATVARS CLUSTER id NODE name group_specifier",
  "SHOW STATVARS CLUSTER name NODE id group_specifier",
  "SHOW STATVARS CLUSTER name NODE name group_specifier",
  "",
  "Show statistics variables in a specific node and for the specified"
  "groups.",
  "",
  "group_specifier can be either GROUP ALL in which case all statistics"
  "variables on the node is displayed or GROUP identifer in which case only"
  "statistic variables belonging to the specified group is displayed",
  "",
  NULL,
};

static int
execute_command(IC_STRING **str_array,
                guint32 num_lines,
                gboolean print_output)
{
  gchar *read_buf;
  guint32 read_size, i;
  int ret_code;
  IC_CONNECTION *conn= NULL;
  DEBUG_ENTRY("execute_command");

  /**
   * Client connects on each command, the client is designed for humans to
   * send messages, so no need to keep the line open since it can be a very
   * long time until the next message is to be sent.
   */
  if ((ret_code= connect_cluster_mgr(&conn)))
    goto error;

  if (glob_connect_code == (guint64)0)
  {
    /**
     * This is the first connect, we will acquire a connect code that we will
     * use in all future connections to the cluster manager. This makes it
     * possible to maintain connections for a very long time without
     * requiring a TCP/IP connection kept up all the time, thus minimizing
     * the resources on the cluster manager side.
     */
    if ((ret_code= ic_send_with_cr(conn, ic_new_connect_clmgr_str)) ||
        (ret_code= ic_send_empty_line(conn)))
      goto error;
    if ((ret_code= ic_rec_long_number(conn,
                                      ic_connected_clmgr_str,
                                      &glob_connect_code)) ||
        (ret_code= ic_rec_empty_line(conn)))

      goto error;
  }
  else
  {
    if ((ret_code= ic_send_with_cr_with_number(conn,
                                               ic_reconnect_clmgr_str,
                                               glob_connect_code)) ||
        (ret_code= ic_send_empty_line(conn)))
      goto error;
    if ((ret_code= ic_rec_simple_str(conn, ic_ok_str)) ||
        (ret_code= ic_rec_empty_line(conn)))
      goto error;
  }
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
    if (print_output)
    {
      ic_printf("%s", read_buf);
    }
  }
end:
  if (conn)
  {
    conn->conn_op.ic_free_connection(conn);
  }
  DEBUG_RETURN_INT(ret_code);
error:
  ic_print_error(ret_code);
  goto end;
}

static int
command_interpreter(void)
{
  guint32 lines, i;
  int ret_code;
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
        ret_code= 1;
        lines--;
        goto error;
      }
      ic_mutex_lock(end_mutex);
      glob_executing= FALSE;
      ic_mutex_unlock(end_mutex);
      if (ic_get_stop_flag() == TRUE)
      {
        DEBUG_RETURN_INT(0);
      }
      if ((ret_code= ic_read_one_line(ic_prompt, line_ptr)))
      {
        ic_print_error(ret_code);
        if (ret_code != IC_ERROR_MALFORMED_CLIENT_STRING)
        {
          DEBUG_RETURN_INT(ret_code);
        }
      }
      ic_mutex_lock(end_mutex);
      glob_executing= TRUE;
      if (ic_get_stop_flag() == TRUE)
      {
        ic_mutex_unlock(end_mutex);
        DEBUG_RETURN_INT(0);
      }
      ic_mutex_unlock(end_mutex);
      if (line_ptr->len == 0)
      {
        if (line_ptr->str)
        {
          ic_free(line_ptr->str);
          line_ptr->str= NULL;
        }
        continue;
      }
      IC_COPY_STRING(line_ptrs[lines], line_ptr);
      lines++;
    } while (!ic_check_last_line(line_ptr));
    if (lines == 1 &&
        (ic_cmp_null_term_str_upper_part("HELP",
                                         line_ptrs[0]) == 0))
    {
      if (ic_cmp_null_term_str_upper("HELP;",
                                     line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP TARGET;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_target_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP START;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_start_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP RESTART;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_restart_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP STOP;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_stop_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP DIE;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_die_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP KILL;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_kill_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP MOVE;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_move_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP PERFORM BACKUP;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_perform_backup_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP PERFORM ROLLING UPGRADE;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_perform_rolling_upgrade_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP DISPLAY STATS;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_display_stats_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP TOP;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_top_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SET STAT_LEVEL;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_set_stat_level_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP USE CLUSTER;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_use_cluster_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP USE VERSION NDB;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_use_version_ndb_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP USE VERSION ICLAUSTRON;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_use_version_iclaustron_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP LIST CLUSTERS;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_list_clusters_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP LISTEN CLUSTER_LOG;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_listen_cluster_log_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CLUSTER;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_show_cluster_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CLUSTER STATUS;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_show_cluster_status_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CONNECTIONS;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_show_connections_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW CONFIG;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_show_config_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW MEMORY;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_show_memory_str);
      }
      else if (ic_cmp_null_term_str_upper("HELP SHOW STATVARS;",
                                          line_ptrs[0]) == 0)
      {
        ic_output_help(ic_help_show_statvars_str);
      }
      else
      {
        ic_printf("Error: No such command to get help on");
      }
    }
    else if ((ret_code= execute_command(&line_ptrs[0], lines, TRUE)))
    {
      ic_print_error(ret_code);
      goto error;
    }
    if (lines == 1 &&
        ((ic_cmp_null_term_str_upper("QUIT;", line_ptrs[0]) == 0) ||
         (ic_cmp_null_term_str_upper("EXIT;", line_ptrs[0]) == 0)))
    {
      ic_free(line_ptrs[0]->str);
      break;
    }
    for (i= 0; i < lines; i++)
    {
      ic_free(line_ptrs[i]->str);
    }
  } while (ic_get_stop_flag() == FALSE);
  DEBUG_RETURN_INT(0);

error:
  for (i= 0; i < lines; i++)
  {
    ic_free(line_ptrs[i]->str);
  }
  DEBUG_RETURN_INT(ret_code);
}

static int
connect_cluster_mgr(IC_CONNECTION **conn)
{
  IC_CONNECTION *loc_conn;
  int ret_code;
  DEBUG_ENTRY("connect_cluster_mgr");

  if (!(loc_conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                          COMMAND_READ_BUF_SIZE)))
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

static void
ic_end_client(void *param)
{
  IC_STRING *line_ptrs;
  IC_STRING line_strs;
  (void)param;
  DEBUG_ENTRY("ic_end_client");
  if (end_mutex != NULL)
  {
    ic_mutex_lock(end_mutex);
    if (glob_executing == FALSE)
    {
      /* We need to remove cookie before exiting */
      DEBUG_PRINT(PROGRAM_LEVEL, ("Remove cookie before exit"));
      line_ptrs= &line_strs;
      IC_INIT_STRING(&line_strs, "EXIT;", 5, FALSE);
      execute_command(&line_ptrs, 1, FALSE);
      ic_printf("");
      /*
        We will exit here since the process otherwise will be stuck
        waiting for user input, someone has already decided to exit
        process, so we will have to do it from here.
      */
      exit(1);
    }
    ic_mutex_unlock(end_mutex);
  }
  DEBUG_RETURN_EMPTY;
}

int main(int argc, char *argv[])
{
  int ret_code= 1;

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
  end_mutex= ic_mutex_create();
  ic_require(end_mutex != NULL);
  ic_set_die_handler(ic_end_client, NULL);
  ret_code= command_interpreter();
  ic_close_readline();
  ic_mutex_destroy(&end_mutex);
  ic_end();
  return ret_code;
}
