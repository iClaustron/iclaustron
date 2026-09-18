/* Copyright (C) 2007, 2023 iClaustron AB

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
extern guint32 glob_core;
extern gchar *glob_debug_file;
extern guint32 glob_debug_screen;
extern guint32 glob_debug_timestamp;

#define IC_DEBUG_MAX_INDENT_LEVEL 128
struct ic_thread_debug
{
  guint32 thread_id;
  guint32 indent_level;
  guint32 enabled;
  guint32 save_enabled;
  guint32 disable_count;
  const gchar *entry_point[IC_DEBUG_MAX_INDENT_LEVEL];
};
typedef struct ic_thread_debug IC_THREAD_DEBUG;

void ic_set_debug(guint32 val);
guint32 ic_get_debug();
guint32 ic_get_debug_timestamp();

/**
 * These definitions control output to stdout, so they are used also
 * when we're not running in debug mode.
 */
void ic_printf(const char *format,...);
void ic_printf_low(const char *format,...);
void ic_putchar(gchar c);
void ic_flush_stdout();
void ic_set_stdout_null(void);
int ic_setup_stdout(gchar *log_file);

/* Various parts to debug. */
#define PROGRAM_LEVEL 1
#define ENTRY_LEVEL 2
#define THREAD_LEVEL 4
#define CONFIG_LEVEL 8
#define PORT_LEVEL 16
#define CONFIG_PROTO_LEVEL 32
#define FILE_LEVEL 64
#define COMM_LEVEL 128
#define CONFIG_READ_LEVEL 256
#define BUILD_CONFIG_HASH_LEVEL 512
#define NDB_MESSAGE_LEVEL 1024
#define ADAPTIVE_SEND_LEVEL 2048
#define CHECK_POLL_SET_LEVEL 4096
#define MALLOC_LEVEL 8192
#define HEARTBEAT_LEVEL 16384
#define COMM_DETAIL_LEVEL 32768
#define FIND_NODE_CONFIG_LEVEL 65536
#define ALL_DEBUG_LEVELS 0xFFFFFFFF

#ifdef DEBUG_BUILD
void ic_debug_print_char_buf(gchar *buf, IC_THREAD_DEBUG *thread_debug);
void ic_debug_printf(const char *format,...);
void ic_debug_print_rec_buf(gchar *buf, guint32 read_size);
void ic_debug_entry(const char *entry_point);
void ic_debug_return(gboolean ptr_val,
                     int return_int,
                     gchar *return_ptr);
void ic_debug_thread_return();
void ic_debug_indent_level_check(int level);
void ic_debug_thread_init(const gchar *entry_point);
int ic_debug_open(guint32 node_id);
void ic_debug_close();
gboolean ic_is_debug_system_active();
void ic_debug_disable(guint32 level);
void ic_debug_enable(guint32 level);

#define INT_DEBUG_RETURN_TYPE (int)0
#define PTR_DEBUG_RETURN_TYPE (int)1
#define VOID_DEBUG_RETURN_TYPE (int)2
#define DEBUG_INDENT_LEVEL_CHECK(a) \
  ic_debug_indent_level_check((a));

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
    ic_debug_print_char_buf(("\n"), NULL); \
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
#define DEBUG_DISABLE(level) ic_debug_disable(((guint32)(level)))
#define DEBUG_ENABLE(level) ic_debug_enable(((guint32)(level)))
#else
#define DEBUG_THREAD_RETURN return NULL
#define DEBUG_RETURN_EMPTY return
#define DEBUG_INDENT_LEVEL_CHECK(a)
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
#define DEBUG_DISABLE(level)
#define DEBUG_ENABLE(level)
#endif
#endif
