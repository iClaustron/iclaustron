/* Copyright (C) 2011, 2014 iClaustron AB

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
#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

int
ic_read_one_line(gchar *prompt, IC_STRING *out_str)
{
  IC_STRING line_str;
  int ret_value;
  DEBUG_ENTRY("ic_read_one_line");
#ifdef HAVE_LIBREADLINE
  line_str.str= readline(prompt);
#else
  gchar line[COMMAND_READ_BUF_SIZE];

  printf("%s", prompt);
  line_str.str= fgets(line, sizeof(line), stdin);
#endif
  if (!line_str.str)
  {
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  }
  line_str.len= COMMAND_READ_BUF_SIZE;
  ic_set_up_ic_string(&line_str);
  if (!line_str.is_null_terminated)
  {
    ret_value= IC_ERROR_COMMAND_TOO_LONG;
    goto error;
  }
#ifdef HAVE_LIBREADLINE
  if (line_str.len)
  {
    add_history(line_str.str);
  }
#endif
  if (line_str.len && 
      line_str.str[line_str.len - 1] == CARRIAGE_RETURN)
  {
    line_str.str[line_str.len - 1]= NULL_BYTE;
    line_str.len--;
  }
  ret_value= ic_strdup(out_str, &line_str);
error:
#ifdef HAVE_LIBREADLINE
  free(line_str.str); /* Allocated by readline => need to use free */
#endif
  DEBUG_RETURN_INT(ret_value);
}

void
ic_init_readline(guint32 history_size)
{
#ifdef HAVE_LIBREADLINE
  using_history();
  stifle_history(history_size);
#else
  (void)history_size;
#endif
}

void
ic_close_readline()
{
#ifdef HAVE_LIBREADLINE
  clear_history();
#endif
}

gboolean
ic_check_last_line(IC_STRING *ic_str)
{
  if (ic_str->str[ic_str->len - 1] == ';')
  {
    return TRUE;
  }
  return FALSE;
}

void
ic_output_help(gchar **help_str)
{
  DEBUG_ENTRY("ic_output_help");
  gchar **loc_help_str= help_str;
  for ( ; *loc_help_str ; loc_help_str++)
  {
    ic_printf("%s\n", *loc_help_str);
  }
  DEBUG_RETURN_EMPTY;
}
