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
#include <ic_port.h>
#include <ic_debug.h>
#include <ic_err.h>
#include <ic_string.h>

/* Defined also for non-debug builds to avoid silly linker errors */
guint32 ic_num_threads_debugged= 0;

#ifdef DEBUG_BUILD
static IC_MUTEX *debug_mutex= NULL;
static GPrivate *debug_priv= NULL;
static guint8 *thread_id_array= NULL;
/* File pointer to the debug output file */
static FILE *ic_fptr;


guint32
ic_get_debug()
{
  return glob_debug;
}

void ic_set_debug(guint32 val)
{
  glob_debug= val;
}

static void
set_indent_buf(gchar *indent_buf, guint32 indent_level)
{
  memset(indent_buf, ' ', 2 * indent_level);
  indent_buf[2 * indent_level]= 0;
}

void
ic_debug_print_char_buf(gchar *in_buf, IC_THREAD_DEBUG *thread_debug)
{
  gchar print_buf[2049 + 32 + (2 * IC_DEBUG_MAX_INDENT_LEVEL) + 1];
  gchar indent_buf[2 * IC_DEBUG_MAX_INDENT_LEVEL + 1];

  if (!thread_debug)
    thread_debug= (IC_THREAD_DEBUG*)g_private_get(debug_priv);

  set_indent_buf(indent_buf, thread_debug->indent_level);

  if (glob_debug_screen)
  {
    g_snprintf(print_buf,
               sizeof(print_buf),
              "T%u: %s%s",
              thread_debug->thread_id,
              indent_buf,
              in_buf);
    ic_printf("%s", print_buf);
    fflush(stdout);
  }
  g_snprintf(print_buf,
             sizeof(print_buf),
             "T%u:%s%s\n",
             thread_debug->thread_id,
             indent_buf,
             in_buf);
  fprintf(ic_fptr, "%s", print_buf);
  fflush(ic_fptr);
}

void
ic_debug_entry(const char *entry_point)
{
  IC_THREAD_DEBUG *thread_debug;
  guint32 len;
  gchar buf[256];

  /*
    Update the indent_level to keep track of where we are in the stack,
    this is to make sure we get pretty printing of debug output.
  */
  ic_require(thread_debug= g_private_get(debug_priv));
  thread_debug->entry_point[thread_debug->indent_level]= entry_point;
  thread_debug->indent_level++;
  ic_require(thread_debug->indent_level < IC_DEBUG_MAX_INDENT_LEVEL);

  if (ic_get_debug() & ENTRY_LEVEL)
  {
    len= g_snprintf(buf, 256, "Entry into %s", entry_point);
    ic_require(len < 256);
    ic_debug_print_char_buf(buf, thread_debug);
  }
}

void ic_debug_return(int ret_type,
                     int return_int,
                     gchar *return_ptr)
{
  guint32 len;
  const gchar *entry_point;
  IC_THREAD_DEBUG *thread_debug;
  gchar buf[256], ptr_buf[64];

  /* Update indent_level, we're returning from debugged function */
  ic_require(thread_debug= g_private_get(debug_priv));
  entry_point= thread_debug->entry_point[thread_debug->indent_level-1];

  if (ic_get_debug() & ENTRY_LEVEL)
  {
    switch (ret_type)
    {
      case INT_DEBUG_RETURN_TYPE:
        len= g_snprintf(buf, 256, "Exit from %s, int_val= %u",
                        entry_point, return_int);
        break;

      case PTR_DEBUG_RETURN_TYPE:
        ic_guint64_hex_str((guint64)return_ptr, ptr_buf);
        len= g_snprintf(buf, 256, "Exit from %s, ptr_val= 0x%s",
                        entry_point, ptr_buf);
        break;

      case VOID_DEBUG_RETURN_TYPE:
        len= g_snprintf(buf, 256, "Exit from %s, void",
                        entry_point);
        break;

      default:
        abort();
        break;
    }
    ic_require(len < 256);
    ic_debug_print_char_buf(buf, thread_debug);
  }
  ic_require(thread_debug->indent_level > 0);
  thread_debug->indent_level--;
}

void ic_debug_thread_return()
{
  guint32 len;
  IC_THREAD_DEBUG *thread_debug;
  gchar buf[64];

  /* Return from function before exiting thread */
  ic_debug_return(VOID_DEBUG_RETURN_TYPE,
                  (int)0,
                  NULL);
  thread_debug= (IC_THREAD_DEBUG*)g_private_get(debug_priv);
  if (ic_get_debug() & THREAD_LEVEL)
  {
    len= g_snprintf(buf, 64, "Exit from thread id=%u", thread_debug->thread_id);
    ic_require(len < 64);
    ic_debug_print_char_buf(buf, thread_debug);
  }
  ic_require(thread_debug->indent_level == 0);
  ic_mutex_lock_low(debug_mutex);
  ic_num_threads_debugged--;
  /* Mark thread id as used */
  thread_id_array[thread_debug->thread_id]= 0;
  ic_mutex_unlock_low(debug_mutex);
  ic_free_low((void*)thread_debug);
}

void ic_debug_thread_init(const gchar *entry_point)
{
  IC_THREAD_DEBUG *thread_debug;
  guint32 thread_id;

  /* We're in debug mode and if those calls fail we can't debug */
  ic_require(thread_debug= (IC_THREAD_DEBUG*)
                           ic_calloc_low(sizeof(IC_THREAD_DEBUG)));
  ic_mutex_lock_low(debug_mutex);
  for (thread_id= 1; thread_id < IC_MAX_THREADS; thread_id++)
  {
    if (thread_id_array[thread_id] == 0)
      break;
  }
  ic_require(thread_id < IC_MAX_THREADS);
  /* Mark thread id as used */
  thread_id_array[thread_id]= 1;
  ic_num_threads_debugged++;
  ic_mutex_unlock_low(debug_mutex);
  thread_debug->thread_id= thread_id;
  g_private_set(debug_priv, (gpointer)thread_debug);
  if (entry_point)
    ic_debug_entry(entry_point);
}

void
ic_debug_print_rec_buf(char *buf, guint32 size)
{
  char p_buf[2049];

  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  ic_debug_printf("Receive buffer, size %u: %s", size, p_buf);
}

void
ic_debug_printf(const char *format,...)
{
  va_list args;
  char buf[2049];

  va_start(args, format);
#ifndef WINDOWS
  vsprintf(buf, format, args);
#else
  vsprintf_s(buf, sizeof(buf), format, args);
#endif
  ic_debug_print_char_buf(buf, NULL);
  va_end(args);
}

int ic_debug_open(guint32 node_id)
{
  gchar file_buf[32];

  ic_require(debug_mutex= ic_mutex_create());
  ic_require(debug_priv= g_private_new(NULL));
  ic_require(thread_id_array= (guint8*)ic_calloc_low(IC_MAX_THREADS));
  g_snprintf(file_buf, 32, "debug_n%u.log", node_id);

#ifndef WINDOWS
  ic_fptr= fopen(file_buf, "w");
#else
  errno_t error= fopen_s(&ic_fptr, file_buf, "w");
  if (error)
    ic_fptr= NULL;
#endif
  if (ic_fptr == NULL)
  {
    ic_printf("Failed to open %s", file_buf);
    fflush(stdout);
    return 1;
  }
  ic_debug_thread_init("main");
  return 0;
}

void
ic_debug_close()
{
  ic_debug_thread_return();
  fflush(stdout);
  fflush(ic_fptr);
  fclose(ic_fptr);
  ic_mutex_destroy(debug_mutex);
  ic_free_low((void*)thread_id_array);
  ic_require(ic_num_threads_debugged == 0);
}
#endif

void
ic_printf(const char *format,...)
{
  va_list args;
  char buf[2049];

  va_start(args, format);
#ifndef WINDOWS
  vsprintf(buf, format, args);
#else
  vsprintf_s(buf, sizeof(buf), format, args);
#endif
  printf("%s\n", buf);
  fflush(stdout);
  va_end(args);
}
