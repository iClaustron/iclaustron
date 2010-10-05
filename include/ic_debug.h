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
#define CONFIG_LEVEL 4
#define PROGRAM_LEVEL 8
#define THREAD_LEVEL 16 
#define CONFIG_PROTO_LEVEL 32
#define MALLOC_LEVEL 64

#ifdef DEBUG_BUILD
void ic_debug_print_char_buf(gchar *buf, IC_THREAD_DEBUG *thread_debug);
void ic_debug_printf(const char *format,...);
void ic_debug_print_rec_buf(gchar *buf, guint32 read_size);
void ic_debug_entry(const char *entry_point);
void ic_debug_return(gboolean ptr_val,
                     int return_int,
                     gchar *return_ptr);
void ic_debug_thread_return();
void ic_debug_thread_init(const gchar *entry_point);
int ic_debug_open();
void ic_debug_close();
#define INT_DEBUG_RETURN_TYPE (int)0
#define PTR_DEBUG_RETURN_TYPE (int)1
#define VOID_DEBUG_RETURN_TYPE (int)2
#define DEBUG_RETURN_INT(a) \
{ \
  int __ret_val= (int)(a); \
  ic_debug_return(INT_DEBUG_RETURN_TYPE, \
                  __ret_val, \
                  NULL); \
  return __ret_val; \
}
/*
  It's not allowed to use function calls in this macro since it will be
  evaluated twice.
*/
#define DEBUG_RETURN_PTR(a) \
{ \
  gchar *__ret_ptr= (gchar*)(a); \
  ic_debug_return(PTR_DEBUG_RETURN_TYPE, \
                  (int)0, \
                  __ret_ptr); \
  return (a); \
}
#define DEBUG_RETURN_EMPTY \
{ \
  ic_debug_return(VOID_DEBUG_RETURN_TYPE, \
                  (int)0, \
                  NULL); \
  return; \
}
#define DEBUG_THREAD_RETURN \
{ \
  ic_debug_thread_return(); \
  return NULL; \
}
#define DEBUG_DECL(a) a
#define DEBUG_PRINT_BUF(level, buf) \
{ \
  if (ic_get_debug() & (level)) \
  { \
    ic_debug_print_char_buf((buf), NULL); \
  } \
}
#define DEBUG_PRINT(level, printf_args) \
  { if (ic_get_debug() & level) { ic_debug_printf printf_args; }}
#define DEBUG(level, a) if (ic_get_debug() & level) a
#define DEBUG_THREAD_ENTRY(a) ic_debug_thread_init(a)
#define DEBUG_ENTRY(a) ic_debug_entry(a)
#define DEBUG_OPEN(a) if (ic_debug_open(a)) return 1
/* Since we debug ic_end we have to return before closing debug system */
#define DEBUG_CLOSE \
{ \
  ic_debug_return(VOID_DEBUG_RETURN_TYPE, (int)0, NULL); \
  ic_debug_close(); \
}
#define DEBUG_IC_STRING(level, a) \
  if (ic_get_debug() & (level)) \
    ic_debug_print_rec_buf((a)->str, (a)->len)
#else
#define DEBUG_THREAD_RETURN return NULL
#define DEBUG_RETURN_EMPTY return
#define DEBUG_RETURN_INT(a) return (a)
#define DEBUG_RETURN_PTR(a) return (a)
/* Define a variable only used in debug builds */
#define DEBUG_DECL(a)
#define DEBUG_PRINT_BUF(level, buf)
#define DEBUG_PRINT(level, printf_args)
#define DEBUG(level, a)
#define DEBUG_THREAD_ENTRY(a)
#define DEBUG_ENTRY(a)
#define DEBUG_OPEN(a)
#define DEBUG_CLOSE
#define DEBUG_IC_STRING(level, a)
#endif
#endif
