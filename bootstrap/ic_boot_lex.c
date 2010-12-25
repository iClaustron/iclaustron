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
#include <../bootstrap/ic_boot_int.h>
#include "ic_boot_parser.h"
#include <ctype.h>
#include <ic_lex_support.h>

static IC_PARSE_SYMBOLS parse_symbols[]=
{
  { "CLUSTER",      CLUSTER_SYM },
  { "CS_INTERNAL_PORT", CS_INTERNAL_PORT_SYM },
  { "=",            EQUAL_SYM },
  { "FILES",        FILES_SYM },
  { "HOST",         HOST_SYM },
  { "MANAGERS",     MANAGERS_SYM },
  { "NODE_ID",      NODE_ID_SYM },
  { "PCNTRL_HOST",  PCNTRL_HOST_SYM },
  { "PCNTRL_PORT",  PCNTRL_PORT_SYM },
  { "PREPARE",      PREPARE_SYM },
  { "SEND",         SEND_SYM },
  { "SERVER",       SERVER_SYM },
  { "SERVERS",      SERVERS_SYM },
  { "START",        START_SYM },
  { "VERIFY",       VERIFY_SYM },
  { NULL,           0}
};

static guint16 id_map_symbol[SIZE_MAP_SYMBOL];

static int hash_multiplier= 1024;
static int hash_divider= 64;

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
  ic_convert_to_uppercase(symbol_buf, str, str_len);
  id= ic_lex_hash(symbol_buf, str_len, hash_multiplier, hash_divider);
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
    return IC_ERROR_MEM_ALLOC;
  memcpy(buf_ptr, str, str_len);
  buf_ptr[str_len]= 0;
  IC_INIT_STRING(ic_str_ptr, buf_ptr, str_len, TRUE);
  yylval->ic_str= ic_str_ptr;
  return 0;
}

void
ic_boot_parse_error(void *ext_parse_data,
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

#define get_next_char() parse_buf[current_pos++]
int
ic_boot_lex(YYSTYPE *yylval,
            IC_PARSE_DATA *parse_data)
{
  register guint32 current_pos= parse_data->parse_current_pos;
  register gchar *parse_buf= parse_data->parse_buf;
  register int parse_char;
  guint32 start_pos;
  int ret_sym= 0;
  gboolean dot_found;
  int symbol_value= 0;
  guint64 int_val;

  for (;;)
  {
    parse_char= get_next_char();
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
    dot_found= FALSE;
    for (;;)
    {
      parse_char= get_next_char();
      if (parse_char == '.')
        dot_found= TRUE;
      else if (!isdigit(parse_char))
        break;
    }
    if (!is_end_symbol_character(parse_char))
      goto number_error;
    if (dot_found)
    {
      ret_sym= IDENTIFIER_SYM;
      goto end;
    }
    if (ic_found_num(&int_val,
                     &parse_buf[start_pos],
                     (current_pos - start_pos)))
    {
      ic_boot_parse_error((void*)parse_data, "Integer Overflow");
      goto error;
    }
    yylval->int_val= int_val;
    ret_sym= INTEGER_SYM;
    goto end;
  }
  else if (isalpha(parse_char))
  {
    for (;;)
    {
      parse_char= get_next_char();
      if (parse_char == '_' ||
          parse_char == '.' ||
          parse_char == '-' ||
          islower(parse_char) ||
          isupper(parse_char) ||
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
      {
        ic_boot_parse_error((void*)parse_data, "Memory allocation failure");
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
  else if (parse_char == '=')
  {
    parse_char= get_next_char();
    /*
      Equal signs always need something after it, no rule has this as the
      last symbol
    */
    if (!ic_is_ignore(parse_char))
      goto identifier_error;
    ret_sym= EQUAL_SYM;
    goto end;
  }
  else
    goto lex_error;

end:
  parse_data->parse_current_pos= current_pos;  
  return ret_sym;

error:
  return 0;

number_error:
  ic_boot_parse_error((void*)parse_data,
                      "Incorrect character at end of number, lex error");
  return 0;

identifier_error:
  ic_boot_parse_error((void*)parse_data,
                      "Incorrect character at end of identifier, lex error");
  return 0;

lex_error:
  ic_boot_parse_error((void*)parse_data,
                      "Incorrect character, lex error");
  return 0;

version_identifier_error:
  ic_boot_parse_error((void*)parse_data,
                      "Upper case in version identifier not allowed");
  return 0;
}

void
ic_boot_call_parser(gchar *parse_string,
                    guint32 str_len,
                    void *ext_parse_data)
{
  IC_PARSE_DATA *parse_data= (IC_PARSE_DATA*)ext_parse_data;
  DEBUG_ENTRY("ic_boot_call_parser");
  DEBUG_PRINT(CONFIG_LEVEL, ("Parser called with string %s", parse_string));

  if (parse_string[str_len - 1] != ';')
  {
    ic_boot_parse_error(ext_parse_data, "Missing ; at end of command");
    parse_data->exit_flag= TRUE;
  }
  parse_data->parse_buf= parse_string;
  parse_data->parse_current_pos= 0;
  parse_data->parse_str_len= str_len;

  yyparse(parse_data);
  DEBUG_RETURN_EMPTY;
}

int
ic_boot_find_hash_function()
{
  return ic_find_hash_function(parse_symbols,
                               id_map_symbol,
                               &hash_multiplier,
                               &hash_divider);
}
