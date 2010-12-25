/* Copyright (C) 2007-2010 iClaustron AB

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
  IC_SEND_FILES_CMD = 1,
  IC_START_CLUSTER_SERVERS_CMD = 2,
  IC_START_CLUSTER_MANAGERS_CMD = 3,
  IC_VERIFY_CLUSTER_SERVERS_CMD = 4,
  IC_NO_SUCH_CMD = 999
};
typedef enum ic_parse_commands IC_PARSE_COMMANDS;

struct ic_parse_data
{
  /*
    Support variables for buffer to lexer
    These variables are initialised before
    call to parser and only used by lexer.
  */
  IC_LEX_DATA lex_data;

  /* Command sent by user */
  IC_PARSE_COMMANDS command;
  /* Representation of connection to command client */
  IC_CONNECTION *conn;
  /* Flag for parser and executer to flag exit */
  gboolean exit_flag;

  guint32 cs_index;
  guint32 next_cs_index;
};
typedef struct ic_parse_data IC_PARSE_DATA;

/*#define YY_EXTRA_TYPE IC_PARSE_DATA */
void ic_boot_parse_error(void *parse_data, char *s);
int yyparse(IC_PARSE_DATA *parse_data);

#define PARSE_DATA ((IC_PARSE_DATA*)parse_data)
#endif
