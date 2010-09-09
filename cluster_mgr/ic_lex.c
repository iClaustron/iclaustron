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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_protocol_support.h>
#include <ic_apic.h>
#include <../cluster_mgr/ic_clmgr_int.h>
#include "ic_parser.h"
#include <ctype.h>

typedef struct ic_parse_symbols
{
  gchar *symbol_str;
  guint32 symbol_id;
} IC_PARSE_SYMBOLS;

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

#define SIZE_MAP_SYMBOL 4096
static guint16 id_map_symbol[SIZE_MAP_SYMBOL];

static int hash_multiplier= 1024;
static int hash_divider= 64;

static int
lex_hash(gchar *str, guint32 str_len)
{
  guint32 hash_val= 0;
  guint32 i;

  for (i= 0; i < str_len; i++)
  {
     hash_val+= (guint32)str[i];
     hash_val+= (hash_val * hash_multiplier);
     hash_val^= (hash_val / hash_divider);
  }
  hash_val+= (hash_val * 8);
  hash_val^= (hash_val / 2048);
  hash_val+= (hash_val * 32);
  return hash_val & (SIZE_MAP_SYMBOL - 1);
}

gboolean
ic_find_hash_function()
{
  int loop_count= 0;
  gchar *sym_string;
  guint32 id, i;
  gboolean failed= TRUE;
  
  while (loop_count++ < 10000 && failed)
  {
    if (loop_count % 3 == 0)
      hash_multiplier+= 2;
    if (loop_count % 3 == 1)
      hash_divider+= 2;
    if (loop_count % 3 == 2)
      hash_multiplier+= 2;
    ic_zero(id_map_symbol, sizeof(guint16) * SIZE_MAP_SYMBOL);
    i= 0;
    failed= FALSE;
    while ((sym_string= parse_symbols[i].symbol_str))
    {
      id= lex_hash(parse_symbols[i].symbol_str,
                   strlen(parse_symbols[i].symbol_str));
      if (id_map_symbol[id] == 0)
      {
        id_map_symbol[id]= (guint16)parse_symbols[i].symbol_id;
      }
      else
      {
        failed= TRUE;
      }
      i++;
    }
  }
  return failed;
}

static void
convert_to_uppercase(gchar *out_str, gchar *in_str, guint32 str_len)
{
  guint32 i;

  for (i= 0; i < str_len; i++)
  {
    if (islower((int)in_str[i]))
      out_str[i]= in_str[i] - ('a' - 'A');
    else
      out_str[i]= in_str[i];
  }
}

static int
found_identifier(YYSTYPE *yylval,
                 IC_PARSE_DATA *parse_data,
                 gchar *str,
                 guint32 str_len,
                 int *symbol_value)
{
  IC_STRING *ic_str_ptr;
  IC_MEMORY_CONTAINER *mc_ptr= parse_data->mc_ptr;
  gchar *buf_ptr;
  gchar symbol_buf[1024];
  int id, symbol_id;

  /* Check if it is a symbol first */
  convert_to_uppercase(symbol_buf, str, str_len);
  id= lex_hash(symbol_buf, str_len);
  symbol_id= id_map_symbol[id];
  if (symbol_id != SIZE_MAP_SYMBOL &&
      memcmp(parse_symbols->symbol_str, symbol_buf, str_len))
  {
    *symbol_value= symbol_id;
    return 0;
  }

  if (!(ic_str_ptr=
        (IC_STRING*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, sizeof(IC_STRING))) ||
      !(buf_ptr= mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, str_len+1)))
  {
    yyerror((void*)parse_data, "Memory allocation failure");
    return IC_ERROR_MEM_ALLOC;
  }
  memcpy(buf_ptr, str, str_len);
  buf_ptr[str_len]= 0;
  IC_INIT_STRING(ic_str_ptr, buf_ptr, str_len, TRUE);
  yylval->ic_str= ic_str_ptr;
  return 0;
}

static int
found_num(YYSTYPE *yylval,
          IC_PARSE_DATA *parse_data,
          gchar *str,
          guint32 str_len)
{
  IC_STRING int_str;
  guint64 int_val;
  int error;

  (void)parse_data;
  IC_INIT_STRING(&int_str, str, str_len, FALSE);
  if ((error= ic_conv_config_str_to_int(&int_val, &int_str)))
  {
    yyerror((void*)parse_data, "Integer Overflow");
    return error;
  }
  yylval->int_val= int_val;
  return 0;
}

void
yyerror(void *ext_parse_data,
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

static inline int
is_ignore(int parse_char)
{
  if (parse_char == ' ' ||
      parse_char == '\t' ||
      parse_char == '\n')
    return TRUE;
  return FALSE;
}

static inline int
is_end_character(int parse_char)
{
  if (parse_char == ';')
    return TRUE;
  return FALSE;
}

static inline int
is_end_symbol_character(int parse_char)
{
  if (is_ignore(parse_char) ||
      is_end_character(parse_char))
    return TRUE;
  return FALSE;
}

#define get_next_char() parse_buf[current_pos++]
int
yylex(YYSTYPE *yylval,
      IC_PARSE_DATA *parse_data)
{
  register guint32 current_pos= parse_data->parse_current_pos;
  register gchar *parse_buf= parse_data->parse_buf;
  register int parse_char;
  guint32 start_pos;
  gboolean upper_found= FALSE;
  gboolean version_char_found= FALSE;
  int ret_sym= 0;
  int symbol_value= 0;

  for (;;)
  {
    parse_char= parse_buf[current_pos++];
    /* Skip space, tab and newline characters */
    if (!is_ignore(parse_char))
      break;
    ic_assert(current_pos < parse_data->parse_str_len);
  }

  if (is_end_character(parse_char))
  {
    ret_sym= END_SYM;
    goto end;
  }

  start_pos= current_pos;
  if (isdigit(parse_char))
  {
    for (;;)
    {
      parse_char= parse_buf[current_pos++];
      if (!isdigit(parse_char))
        break;
    }
    if (!is_end_symbol_character(parse_char))
      goto number_error;
    if (found_num(yylval,
                  parse_data,
                  &parse_buf[start_pos],
                  (current_pos - start_pos)))
      goto error;
    ret_sym= INTEGER_SYM;
    goto end;
  }
  else if (isalpha(parse_char))
  {
    if (isupper(parse_char))
      upper_found= TRUE;
    for (;;)
    {
      parse_char= parse_buf[current_pos++];
      if (isupper(parse_char))
        upper_found= TRUE;
      if (parse_char == '-' ||
          parse_char == '.')
      {
        version_char_found= TRUE;
        continue;
      }
      if (parse_char == '_' ||
          islower(parse_char) ||
          isdigit(parse_char))
        continue;
      /* Check for correct end of symbol character */
      if (!is_end_symbol_character(parse_char))
        goto identifier_error;
      if (upper_found && version_char_found)
        goto version_identifier_error;
      if (found_identifier(yylval,
                           parse_data,
                           &parse_buf[start_pos],
                           (current_pos - start_pos),
                           &symbol_value))
        goto error;
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
  parse_data->parse_current_pos= current_pos;  
  return ret_sym;

error:
  return 0;

number_error:
  yyerror((void*)parse_data, "Incorrect character at end of number, lex error");
  return 0;

identifier_error:
  yyerror((void*)parse_data, "Incorrect character at end of identifier, lex error");
  return 0;

lex_error:
  yyerror((void*)parse_data, "Incorrect character, lex error");
  return 0;

version_identifier_error:
  yyerror((void*)parse_data, "Upper case in version identifier not allowed");
  return 0;
}

void
ic_call_parser(gchar *parse_string,
               guint32 str_len,
               void *ext_parse_data)
{
  IC_PARSE_DATA *parse_data= (IC_PARSE_DATA*)ext_parse_data;
  DEBUG_ENTRY("ic_call_parser");
  DEBUG_PRINT(CONFIG_LEVEL, ("Parser called with string %s", parse_string));

  if (parse_string[str_len - 1] != ';')
  {
    yyerror(ext_parse_data, "Missing ; at end of command");
    parse_data->exit_flag= TRUE;
  }
  parse_data->parse_buf= parse_string;
  parse_data->parse_current_pos= 0;
  parse_data->parse_str_len= str_len;

  yyparse(parse_data);
  DEBUG_RETURN_EMPTY;
}
