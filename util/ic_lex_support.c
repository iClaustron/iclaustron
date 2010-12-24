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

#include <ic_base_header.h>
#include <ic_string.h>
#include <ic_lex_support.h>

int
ic_lex_hash(gchar *str,
            guint32 str_len,
            int hash_multiplier,
            int hash_divider)
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
ic_find_hash_function(IC_PARSE_SYMBOLS *parse_symbols,
                      IC_MAP_SYMBOL_TYPE *symbol_map,
                      int *hash_multiplier,
                      int *hash_divider)
{
  int loop_count= 0;
  gchar *sym_string;
  guint32 id, i;
  gboolean failed= TRUE;
  int loc_hash_multiplier= 1024;
  int loc_hash_divider= 64;
  
  while (loop_count++ < 10000 && failed)
  {
    if (loop_count % 3 == 0)
      loc_hash_multiplier+= 2;
    if (loop_count % 3 == 1)
      loc_hash_divider+= 2;
    if (loop_count % 3 == 2)
      loc_hash_multiplier+= 2;
    ic_zero(symbol_map, sizeof(IC_MAP_SYMBOL_TYPE) * SIZE_MAP_SYMBOL);
    i= 0;
    failed= FALSE;
    while ((sym_string= parse_symbols[i].symbol_str))
    {
      id= ic_lex_hash(parse_symbols[i].symbol_str,
                      strlen(parse_symbols[i].symbol_str),
                      loc_hash_multiplier,
                      loc_hash_divider);
      if (symbol_map[id] == 0)
      {
        symbol_map[id]= (IC_MAP_SYMBOL_TYPE)parse_symbols[i].symbol_id;
      }
      else
      {
        failed= TRUE;
      }
      i++;
    }
  }
  *hash_multiplier= loc_hash_multiplier;
  *hash_divider= loc_hash_divider;
  return failed;
}

int
ic_found_num(guint64 *int_val,
             gchar *str,
             guint32 str_len)
{
  IC_STRING int_str;
  int error;

  IC_INIT_STRING(&int_str, str, str_len, FALSE);
  if ((error= ic_conv_config_str_to_int(int_val, &int_str)))
    return error;
  return 0;
}
