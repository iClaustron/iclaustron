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

#ifndef IC_DEBUG_H
#define IC_DEBUG_H
/*
  HEADER MODULE: iClaustron Debug Support
  ---------------------------------------
  Debug interface, mostly a set of macros calling a set of backend
  functions when compiled in debug mode and otherwise empty macros.
*/

extern guint32 glob_debug;
extern gchar *glob_debug_file;
extern guint32 glob_debug_screen;

#define IC_DEBUG_MAX_INDENT_LEVEL 128
struct ic_thread_debug
{
  guint32 thread_id;
  guint32 indent_level;
  const gchar *entry_point[IC_DEBUG_MAX_INDENT_LEVEL];
};
typedef struct ic_thread_debug IC_THREAD_DEBUG;

void ic_set_debug(guint32 val);
guint32 ic_get_debug();
void ic_printf(const char *format,...);

/* Various parts to debug. */
#define COMM_LEVEL 1
#define ENTRY_LEVEL 2
#define CONFIG_LEVEL 3
#define PROGRAM_LEVEL 4
#define THREAD_LEVEL 5
#define CONFIG_PROTO_LEVEL 6

#ifdef DEBUG_BUILD
void ic_debug_print_char_buf(gchar *buf, IC_THREAD_DEBUG *thread_debug);
void ic_debug_printf(const char *format,...);
void ic_debug_print_rec_buf(gchar *buf, guint32 read_size);
void ic_debug_entry(const char *entry_point);
void ic_debug_return();
void ic_debug_thread_return();
void ic_debug_thread_init(const gchar *entry_point);
int ic_debug_open();
void ic_debug_close();
#define DEBUG_RETURN_EMPTY { ic_debug_return(); return; }
#define DEBUG_RETURN(a) { ic_debug_return(); return a; }
#define DEBUG_THREAD_RETURN { ic_debug_thread_return(); return NULL; }
#define DEBUG_DECL(a) a
#define DEBUG_PRINT_BUF(level, buf) \
  { if (ic_get_debug() & level) { ic_debug_print_char_buf(buf, NULL); }}
#define DEBUG_PRINT(level, printf_args) \
  { if (ic_get_debug() & level) { ic_debug_printf printf_args; }}
#define DEBUG(level, a) if (ic_get_debug() & level) a
#define DEBUG_THREAD_ENTRY(a) ic_debug_thread_init(a)
#define DEBUG_ENTRY(a) ic_debug_entry(a)
#define DEBUG_OPEN if (ic_debug_open()) return 1
/* Since we debug ic_end we have to return before closing debug system */
#define DEBUG_CLOSE { ic_debug_return(); ic_debug_close(); }
#define DEBUG_IC_STRING(level, a) \
  if (ic_get_debug() & level) ic_debug_print_rec_buf(a->str, a->len)
#else
#define DEBUG_THREAD_RETURN return NULL
#define DEBUG_RETURN_EMPTY return
#define DEBUG_RETURN(a) return a
/* Define a variable only used in debug builds */
#define DEBUG_DECL(a)
#define DEBUG_PRINT_BUF(level, buf)
#define DEBUG_PRINT(level, printf_args)
#define DEBUG(level, a)
#define DEBUG_THREAD_ENTRY(a)
#define DEBUG_ENTRY(a)
#define DEBUG_OPEN
#define DEBUG_CLOSE
#define DEBUG_IC_STRING(level, a)
#endif
#endif
