/* Copyright (C) 2010-2011 iClaustron AB

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

#ifndef IC_BOOT_INT_H
#define IC_BOOT_INT_H

void ic_boot_call_parser(gchar *parse_string,
                         guint32 str_len,
                         void *parse_data);

gboolean ic_boot_find_hash_function();

enum ic_parse_commands
{
  IC_PREPARE_CLUSTER_SERVER_CMD = 0,
  IC_PREPARE_CLUSTER_MANAGER_CMD = 1,
  IC_SEND_FILES_CMD = 2,
  IC_START_CLUSTER_SERVERS_CMD = 3,
  IC_START_CLUSTER_MANAGERS_CMD = 4,
  IC_VERIFY_CLUSTER_SERVERS_CMD = 5,
  IC_NO_SUCH_CMD = 999
};
typedef enum ic_parse_commands IC_PARSE_COMMANDS;

struct ic_cluster_server_data
{
  gchar *hostname;
  gchar *pcntrl_hostname;
  guint32 pcntrl_port;
  guint32 node_id;
};
typedef struct ic_cluster_server_data IC_CLUSTER_SERVER_DATA;

struct ic_cluster_manager_data
{
  gchar *hostname;
  gchar *pcntrl_hostname;
  guint32 pcntrl_port;
  guint32 node_id;
};
typedef struct ic_cluster_manager_data IC_CLUSTER_MANAGER_DATA;

struct ic_parse_data
{
  /*
    Support variables for buffer to lexer
    These variables are initialised before
    call to parser and only used by lexer.
    This must be first variable in this struct, this knowledge is used
    in some casts in the lexer.
  */
  IC_LEX_DATA lex_data;

  /* Data representing the Cluster Servers */
  IC_CLUSTER_SERVER_DATA cs_data[IC_MAX_CLUSTER_SERVERS];

  /* Data representing the Cluster Servers */
  IC_CLUSTER_MANAGER_DATA mgr_data[IC_MAX_CLUSTER_MANAGERS];

  /* Command sent by user */
  IC_PARSE_COMMANDS command;

  /* Flag for parser and executer to flag exit */
  gboolean exit_flag;

  guint32 next_cs_index;
  guint32 next_mgr_index;
};
typedef struct ic_parse_data IC_PARSE_DATA;

/*#define YY_EXTRA_TYPE IC_PARSE_DATA */
void ic_boot_parse_error(void *parse_data, char *s);
int yyparse(IC_PARSE_DATA *parse_data);

#define PARSE_DATA ((IC_PARSE_DATA*)parse_data)
#endif
