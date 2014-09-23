/* Copyright (C) 2007, 2014 iClaustron AB

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
static GPrivate debug_priv;
static guint8 *thread_id_array= NULL;
/* File pointer to the debug output file */
static FILE *ic_fptr;
/* Start time of the program */
static IC_TIMER debug_start_time;

void ic_printf_low(const char *format,...);

guint32
ic_get_debug()
{
  return glob_debug;
}

guint32
ic_get_debug_timestamp()
{
  return glob_debug_timestamp;
}

void ic_set_debug(guint32 val)
{
  glob_debug= val;
}

static void
set_indent_buf(gchar *indent_buf, guint32 indent_level)
{
  memset(indent_buf, SPACE_CHAR, 2 * indent_level);
  indent_buf[2 * indent_level]= 0;
}

void
ic_debug_disable(guint32 level)
{
  IC_THREAD_DEBUG *thread_debug;

  if (!(glob_debug & level))
  {
    thread_debug= (IC_THREAD_DEBUG*)g_private_get(&debug_priv);
    if (thread_debug->disable_count == 0)
    {
      thread_debug->save_enabled= thread_debug->enabled;
      thread_debug->enabled= FALSE;
    }
    thread_debug->disable_count++;
  }
}

void
ic_debug_enable(guint32 level)
{
  IC_THREAD_DEBUG *thread_debug;

  if (!(glob_debug & level))
  {
    thread_debug= (IC_THREAD_DEBUG*)g_private_get(&debug_priv);
    thread_debug->disable_count--;
    if (thread_debug->disable_count == 0)
    {
      thread_debug->enabled= thread_debug->save_enabled;
    }
  }
}

void
ic_debug_print_char_buf(gchar *in_buf, IC_THREAD_DEBUG *thread_debug)
{
  IC_TIMER current_time, micros_time;
  guint32 seconds, micros;
  gchar time_buf[64];
  gchar *time_ptr= NULL;
  gchar print_buf[2049 + 32 + (2 * IC_DEBUG_MAX_INDENT_LEVEL) + 1];
  gchar indent_buf[2 * IC_DEBUG_MAX_INDENT_LEVEL + 1];

  if (!thread_debug)
    thread_debug= (IC_THREAD_DEBUG*)g_private_get(&debug_priv);

  if (!thread_debug->enabled)
    return;

  set_indent_buf(indent_buf, thread_debug->indent_level);

  if (glob_debug_timestamp)
  {
    current_time= ic_gethrtime();
    micros_time= ic_micros_elapsed(debug_start_time, current_time);
    seconds= (guint32)(micros_time / IC_MICROSEC_PER_SECOND);
    micros= (guint32)(micros_time - (seconds * IC_MICROSEC_PER_SECOND));
    g_snprintf(time_buf,
               sizeof(time_buf),
               "%.10u:%.6u: ",
               seconds, micros);
    time_ptr= &time_buf[0];
  }
  if (glob_debug_screen)
  {
    g_snprintf(print_buf,
               sizeof(print_buf),
              "%sT%u: %s%s",
              time_ptr ? time_ptr : "",
              thread_debug->thread_id,
              indent_buf,
              in_buf);
    ic_printf_low("%s", print_buf);
    ic_flush_stdout();
  }
  g_snprintf(print_buf,
             sizeof(print_buf),
             "%sT%u:%s%s\n",
             time_ptr ? time_ptr : "",
             thread_debug->thread_id,
             indent_buf,
             in_buf);
  ic_mutex_lock_low(debug_mutex);
  fprintf(ic_fptr, "%s", print_buf);
  ic_mutex_unlock_low(debug_mutex);
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
  ic_require(thread_debug= g_private_get(&debug_priv));
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
  ic_require(thread_debug= g_private_get(&debug_priv));
  ic_require(thread_debug->indent_level > 0);
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
        len= g_snprintf(buf, 256, "Exit from %s, ptr_val= %s",
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
  thread_debug->indent_level--;
}

void ic_debug_indent_level_check(int level)
{
  IC_THREAD_DEBUG *thread_debug;

  thread_debug= (IC_THREAD_DEBUG*)g_private_get(&debug_priv);
  ic_require((int)thread_debug->indent_level == level);
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
  thread_debug= (IC_THREAD_DEBUG*)g_private_get(&debug_priv);
  if (ic_get_debug() & THREAD_LEVEL)
  {
    len= g_snprintf(buf,
                    64,
                    "Exit from thread id=%u",
                    thread_debug->thread_id);
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
  thread_debug->enabled= TRUE;
  g_private_set(&debug_priv, (gpointer)thread_debug);
  if (entry_point)
  {
    ic_debug_entry(entry_point);
  }
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
  IC_PID_TYPE my_pid= ic_get_own_pid();
  guint32 pid= (guint32)my_pid;

  ic_require(debug_mutex= ic_mutex_create());
  ic_require(thread_id_array= (guint8*)ic_calloc_low(IC_MAX_THREADS));
  g_snprintf(file_buf, 32, "debug_n%u_p%u.log", node_id, pid);

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
    ic_flush_stdout();
    return 1;
  }
  /* Set debug starting time */
  debug_start_time= ic_gethrtime();

  ic_debug_thread_init("main");
  return 0;
}

gboolean ic_is_debug_system_active()
{
  if (debug_mutex != NULL)
  {
    return TRUE;
  }
  return FALSE;
}

void
ic_debug_close()
{
  ic_debug_thread_return();
  ic_flush_stdout();
  fflush(ic_fptr);
  fclose(ic_fptr);
  ic_mutex_destroy(&debug_mutex);
  debug_mutex= NULL;
  ic_free_low((void*)thread_id_array);
  ic_require(ic_num_threads_debugged == 0);
}
#endif

static FILE *stdout_fd= NULL;
static int stdout_defined= 0;

void ic_set_stdout_null(void)
{
  stdout_fd= NULL;
  stdout_defined= -1;
}

int
ic_setup_stdout(gchar *log_file)
{
  FILE *file_des;
  ic_delete_file(log_file, TRUE);
  file_des= fopen(log_file, "w");
  if (file_des == NULL)
  {
    return IC_ERROR_FAILED_OPEN_STDOUT;
  }
  stdout_fd= file_des;
  stdout_defined= 1;
  return 0;
}

void
ic_flush_stdout(void)
{
  if (stdout_defined == 0)
  {
    fflush(stdout);
  }
  else if (stdout_defined > 0)
  {
    fflush(stdout_fd);
  }
}

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
  if (stdout_defined == 0)
  {
    printf("%s\n", buf);
  }
  else if (stdout_defined > 0)
  {
    fprintf(stdout_fd, "%s\n", buf);
  }
#ifdef DEBUG_BUILD
  if (debug_mutex) /* Verify debug system is up and running */
  {
    DEBUG_PRINT(PROGRAM_LEVEL, ("%s", buf));
  }
#endif
  ic_flush_stdout();
  va_end(args);
}

void
ic_putchar(gchar c)
{
  if (stdout_defined == 0)
  {
    putchar(c);
  }
  else if (stdout_defined > 0)
  {
    fputc(c, stdout_fd);
  }
}

void
ic_printf_low(const char *format,...)
{
  va_list args;
  char buf[2049];

  va_start(args, format);
#ifndef WINDOWS
  vsprintf(buf, format, args);
#else
  vsprintf_s(buf, sizeof(buf), format, args);
#endif
  if (stdout_defined == 0)
  {
    printf("%s\n", buf);
  }
  else if (stdout_defined > 0)
  {
    fprintf(stdout_fd, "%s\n", buf);
  }
  ic_flush_stdout();
  va_end(args);
}
