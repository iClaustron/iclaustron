/* Copyright (C) 2010 iClaustron AB

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

#define SIZE_MAP_SYMBOL 4096
typedef guint16 IC_MAP_SYMBOL_TYPE;

typedef struct ic_parse_symbols
{
  gchar *symbol_str;
  guint32 symbol_id;
} IC_PARSE_SYMBOLS;

int ic_lex_hash(gchar *str,
                guint32 str_len,
                int hash_multiplier,
                int hash_divider);

gboolean ic_find_hash_function(IC_PARSE_SYMBOLS *parse_symbols,
                               IC_MAP_SYMBOL_TYPE *symbol_map,
                               int *hash_multiplier,
                               int *hash_divider);

int ic_found_num(guint64 *int_val,
                 gchar *str,
                 guint32 str_len);

IC_INLINE int
is_ignore(int parse_char)
{
  if (parse_char == ' ' ||
      parse_char == '\t' ||
      parse_char == '\n')
    return TRUE;
  return FALSE;
}

IC_INLINE int
is_end_character(int parse_char)
{
  if (parse_char == ';')
    return TRUE;
  return FALSE;
}

IC_INLINE int
is_end_symbol_character(int parse_char)
{
  if (is_ignore(parse_char) ||
      is_end_character(parse_char))
    return TRUE;
  return FALSE;
}
