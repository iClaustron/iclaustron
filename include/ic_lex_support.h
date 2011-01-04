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

#ifndef IC_LEX_SUPPORT_H
#define IC_LEX_SUPPORT_H

#define SIZE_MAP_SYMBOL 4096
typedef guint16 IC_MAP_SYMBOL_TYPE;

struct ic_lex_data
{
  /* Memory container for parser/lexer */
  IC_MEMORY_CONTAINER *mc_ptr;
  int hash_multiplier;
  int hash_divider;
  IC_MAP_SYMBOL_TYPE *symbol_map;
  gchar *parse_buf;
  guint32 parse_current_pos;
  guint32 parse_str_len;
};
typedef struct ic_lex_data IC_LEX_DATA;

typedef struct ic_parse_symbols
{
  gchar *symbol_str;
  guint32 symbol_id;
} IC_PARSE_SYMBOLS;

int
ic_found_identifier(IC_LEX_DATA *lex_data,
                    IC_PARSE_SYMBOLS *parse_symbols,
                    IC_STRING **ic_str_ptr,
                    gchar *str,
                    guint32 str_len,
                    int *symbol_value);

gboolean ic_find_hash_function(IC_PARSE_SYMBOLS *parse_symbols,
                               IC_MAP_SYMBOL_TYPE *symbol_map,
                               int *hash_multiplier,
                               int *hash_divider);

int ic_found_num(guint64 *int_val,
                 gchar *str,
                 guint32 str_len);

IC_INLINE int
ic_is_ignore(int parse_char)
{
  if (parse_char == ' ' ||
      parse_char == '\t' ||
      parse_char == '\n')
    return TRUE;
  return FALSE;
}

IC_INLINE int
ic_is_end_character(int parse_char)
{
  if (parse_char == ';')
    return TRUE;
  return FALSE;
}

IC_INLINE int
ic_is_end_symbol_character(int parse_char)
{
  if (ic_is_ignore(parse_char) ||
      ic_is_end_character(parse_char))
    return TRUE;
  return FALSE;
}
#endif
