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

#ifndef IC_READLINE_H
#define IC_READLINE_H

int ic_read_one_line(gchar *prompt, IC_STRING *out_str);
void ic_init_readline(guint32 history_size);
void ic_close_readline();

gboolean ic_check_last_line(IC_STRING *ic_str);
void ic_output_help(gchar **help_str);
#endif
