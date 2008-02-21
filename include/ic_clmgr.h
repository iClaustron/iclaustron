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
#include <ic_apic.h>

void ic_call_parser(char *parse_string, int str_len, void *some_data);

enum ic_parse_commands
{
  IC_DIE_CMD = 0,
  IC_KILL_CMD = 1,
  IC_MOVE_CMD = 2,
  IC_PERFORM_BACKUP_CMD = 3,
  IC_PERFORM_ROLLING_UPGRADE_CMD = 4,
  IC_RESTART_CMD = 5,
  IC_START_CMD = 6,
  IC_STOP_CMD = 7,
  IC_LIST_CMD = 8,
  IC_LISTEN_CMD = 9,
  IC_SHOW_CLUSTER_CMD = 10,
  IC_SHOW_CLUSTER_STATUS_CMD = 11,
  IC_SHOW_CONNECTIONS_CMD = 12,
  IC_SHOW_CONFIG_CMD = 13,
  IC_SHOW_MEMORY_CMD = 14,
  IC_SHOW_STATVARS_CMD = 15,
  IC_SHOW_STATS_CMD = 16,
  IC_SET_STAT_LEVEL_CMD = 17,
  IC_USE_CLUSTER_CMD = 18,
  IC_DISPLAY_STATS_CMD = 19,
  IC_TOP_CMD = 20
};
typedef enum ic_parse_commands IC_PARSE_COMMANDS;

struct ic_parse_data
{
  /* Support variables for buffer to lexer */
  void *yyscanner;
  gchar *parse_buf;
  guint32 parse_inx;
  guint32 parse_str_len;

  /* Memory container for parser/lexer */
  IC_MEMORY_CONTAINER *mc_ptr;
  /* Configuration of the grid of clusters */
  IC_API_CONFIG_SERVER *apic;
  /* Command sent by user */
  IC_PARSE_COMMANDS command;
  /* Representation of connection to command client */
  IC_CONNECTION *conn;
  /* Flag for parser and executer to flag exit */
  gboolean exit_flag;
  /* Flag for start command to perform initial start */
  gboolean initial_flag;
  /* Node type started */
  IC_NODE_TYPES binary_type;

  /* Current cluster reference */
  IC_STRING current_cluster_name;
  guint64   current_cluster_id;

  /* Cluster reference */
  IC_STRING cluster_name;
  guint64   cluster_id;
  gboolean  cluster_all;
  gboolean  default_cluster;

  /* Node reference */
  IC_STRING node_name;
  guint64   node_id;
  gboolean  node_all;
  gboolean  default_node;

  /* Is ALL set on command */
  gboolean  all;
};
typedef struct ic_parse_data IC_PARSE_DATA;

#define YY_EXTRA_TYPE IC_PARSE_DATA*
void yyset_extra(YY_EXTRA_TYPE, void*);
void yyerror(void *parse_data, void*, char *s);
int yyparse(IC_PARSE_DATA *parse_data, void *);

#define PARSE_DATA ((IC_PARSE_DATA*)parse_data)
