/* Copyright (C) 2007-2013 iClaustron AB

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
#include <ic_protocol_support.h>
#include <ic_apic.h>
#include <ic_lex_support.h>
#include <../cluster_mgr/ic_clmgr_int.h>
#include "ic_mgr_parser.h"
#include <ctype.h>

static IC_PARSE_SYMBOLS parse_symbols[]=
{
  { "AS",           AS_SYM },
  { "ALL",          ALL_SYM },
  { "BACKUP",       BACKUP_SYM },
  { "CLUSTER",      CLUSTER_SYM },
  { "CLUSTERS",     CLUSTERS_SYM },
  { "CLUSTER_LOG",  CLUSTER_LOG_SYM },
  { "CONFIG",       CONFIG_SYM },
  { "CONNECTIONS",  CONNECTIONS_SYM },
  { "DATA",         DATA_SYM },
  { "DIE",          DIE_SYM },
  { "DISPLAY",      DISPLAY_SYM },
  { "FILE",         FILE_SYM },
  { "FROM",         FROM_SYM },
  { "GROUP",        GROUP_SYM },
  { "ICLAUSTRON",   ICLAUSTRON_SYM },
  { "INITIAL",      INITIAL_SYM },
  { "KILL",         KILL_SYM },
  { "LIST",         LIST_SYM },
  { "LISTEN",       LISTEN_SYM },
  { "MANAGER",      MANAGER_SYM },
  { "MEMORY",       MEMORY_SYM },
  { "MOVE",         MOVE_SYM },
  { "NDB",          NDB_SYM },
  { "NODE",         NODE_SYM },
  { "NODEGROUP",    NODEGROUP_SYM },
  { "NODEGROUPS",   NODEGROUPS_SYM },
  { "RESTORE",      RESTORE_SYM },
  { "PERFORM",      PERFORM_SYM },
  { "REPLICATION",  REPLICATION_SYM },
  { "RESTART",      RESTART_SYM },
  { "ROLLING",      ROLLING_SYM },
  { "SEEN",         SEEN_SYM },
  { "SERVER",       SERVER_SYM },
  { "SET",          SET_SYM },
  { "SHOW",         SHOW_SYM },
  { "SQL",          SQL_SYM },
  { "START",        START_SYM },
  { "STATUS",       STATUS_SYM },
  { "STATS",        STATS_SYM },
  { "STATVARS",     STATVARS_SYM },
  { "STAT_LEVEL",   STAT_LEVEL_SYM },
  { "STOP",         STOP_SYM },
  { "TO",           TO_SYM },
  { "TOP",          TOP_SYM },
  { "UPGRADE",      UPGRADE_SYM },
  { "USE",          USE_SYM },
  { "VARIABLE",     VARIABLE_SYM },
  { "VERSION",      VERSION_SYM },
  { NULL,           0}
};

static guint16 id_map_symbol[SIZE_MAP_SYMBOL];

static int hash_multiplier= 1024;
static int hash_divider= 64;

static int
found_identifier(YYSTYPE *yylval,
                 IC_LEX_DATA *lex_data,
                 gchar *str,
                 guint32 str_len,
                 int *symbol_value)
{
  int error;
  IC_STRING *ic_str_ptr;

  *symbol_value= 0;
  if ((error= ic_found_identifier(lex_data,
                                  parse_symbols,
                                  &ic_str_ptr,
                                  str,
                                  str_len,
                                  symbol_value)))
    return error;
  if ((*symbol_value) == 0)
  {
    yylval->ic_str= ic_str_ptr;
  }
  return 0;
}

void
ic_mgr_parse_error(void *ext_parse_data,
        char *s)
{
  IC_PARSE_DATA *parse_data= (IC_PARSE_DATA*)ext_parse_data;
  IC_CONNECTION *conn= parse_data->conn;
  gchar buf[1024];

  g_snprintf(buf, 1024, "Error: %s", s);
  if (ic_send_with_cr(conn, buf) ||
      ic_send_with_cr(conn, ic_empty_string))
    parse_data->exit_flag= TRUE;
}

#define get_next_char() parse_buf[current_pos]
int
ic_mgr_lex(YYSTYPE *yylval,
           IC_PARSE_DATA *parse_data)
{
  IC_LEX_DATA *lex_data= (IC_LEX_DATA*)parse_data;
  register guint32 current_pos= lex_data->parse_current_pos;
  register gchar *parse_buf= lex_data->parse_buf;
  register int parse_char;
  guint32 start_pos;
  gboolean upper_found= FALSE;
  gboolean version_char_found= FALSE;
  int ret_sym= 0;
  int symbol_value= 0;
  guint64 int_val;

  for (;;)
  {
    parse_char= get_next_char();
    /* Skip space, tab and newline characters */
    if (!ic_is_ignore(parse_char))
      break;
    current_pos++;
    ic_assert(current_pos < lex_data->parse_str_len);
  }

  if (ic_is_end_character(parse_char))
  {
    ret_sym= END_SYM;
    goto end;
  }

  start_pos= current_pos;
  if (isdigit(parse_char))
  {
    for (;;)
    {
      parse_char= get_next_char();
      if (!isdigit(parse_char))
        break;
      current_pos++;
    }
    if (!ic_is_end_symbol_character(parse_char))
      goto number_error;
    if (ic_found_num(&int_val,
                     &parse_buf[start_pos],
                     (current_pos - start_pos)))
    {
      ic_mgr_parse_error((void*)parse_data, "Integer Overflow");
      goto error;
    }
    yylval->int_val= int_val;
    ret_sym= INTEGER_SYM;
    goto end;
  }
  else if (isalpha(parse_char))
  {
    if (isupper(parse_char))
      upper_found= TRUE;
    for (;;)
    {
      parse_char= get_next_char();
      if (isupper(parse_char))
        upper_found= TRUE;
      if (parse_char == '-' ||
          parse_char == '.')
      {
        version_char_found= TRUE;
        current_pos++;
        continue;
      }
      if (parse_char == '_' ||
          islower(parse_char) ||
          isupper(parse_char) ||
          isdigit(parse_char))
      {
        current_pos++;
        continue;
      }
      /* Check for correct end of symbol character */
      if (!ic_is_end_symbol_character(parse_char))
        goto identifier_error;
      if (upper_found && version_char_found)
        goto version_identifier_error;
      if (found_identifier(yylval,
                           (IC_LEX_DATA*)parse_data,
                           &parse_buf[start_pos],
                           (current_pos - start_pos),
                           &symbol_value))
      {
        ic_mgr_parse_error((void*)parse_data, "Memory allocation failure");
        goto error;
      }
      if (version_char_found)
      {
        ic_assert(!symbol_value);
        ret_sym= VERSION_IDENTIFIER_SYM;
        goto end;
      }
      else
      {
        if (!symbol_value)
          ret_sym= IDENTIFIER_SYM;
        else
          ret_sym= symbol_value;
        goto end;
      }
    } 
  }
  else
    goto lex_error;

end:
  lex_data->parse_current_pos= current_pos;  
  return ret_sym;

error:
  return 0;

number_error:
  ic_mgr_parse_error((void*)parse_data,
                     "Incorrect character at end of number, lex error");
  return 0;

identifier_error:
  ic_mgr_parse_error((void*)parse_data,
                     "Incorrect character at end of identifier, lex error");
  return 0;

lex_error:
  ic_mgr_parse_error((void*)parse_data,
                     "Incorrect character, lex error");
  return 0;

version_identifier_error:
  ic_mgr_parse_error((void*)parse_data,
          "Upper case in version identifier not allowed");
  return 0;
}

void
ic_mgr_call_parser(gchar *parse_string,
                   guint32 str_len,
                   void *ext_parse_data)
{
  IC_PARSE_DATA *parse_data= (IC_PARSE_DATA*)ext_parse_data;
  IC_LEX_DATA *lex_data= (IC_LEX_DATA*)ext_parse_data;
  DEBUG_ENTRY("ic_mgr_call_parser");

  if (parse_string[str_len - 1] != CMD_SEPARATOR)
  {
    ic_mgr_parse_error(ext_parse_data, "Missing ; at end of command");
    parse_data->exit_flag= TRUE;
  }
  parse_string[str_len]= 0; /* Ensure NULL-terminated string for print-outs */
  DEBUG_PRINT(CONFIG_LEVEL, ("Parser called with string %s", parse_string));
  lex_data->parse_buf= parse_string;
  lex_data->parse_current_pos= 0;
  lex_data->parse_str_len= str_len;
  lex_data->hash_multiplier= hash_multiplier;
  lex_data->hash_divider= hash_divider;
  lex_data->symbol_map= id_map_symbol;

  yyparse(parse_data);
  DEBUG_RETURN_EMPTY;
}

int
ic_mgr_find_hash_function()
{
  return ic_find_hash_function(parse_symbols,
                               id_map_symbol,
                               &hash_multiplier,
                               &hash_divider);
}
