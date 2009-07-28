/* Copyright (C) 2007-2009 iClaustron AB

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

#ifdef LINUX
/*
  Portability fix for Linux
  We need to include before ic_base_header.h since it
  will include string.h without define.
*/
#define _XOPEN_SOURCE 600
#endif
#include <string.h>
#ifdef DEBUG_BUILD
#include <stdio.h>
#endif
#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_string.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_GETHRTIME
#include <sys/time.h>
#endif
#ifdef HAVE_CLOCK_GETTIME
#include <time.h>
#endif
#ifdef HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef DEBUG_BUILD
static guint64 num_mem_allocs= 0;
static GMutex *mem_mutex= NULL;
guint32 error_inject= 0;
#endif
static const gchar *port_binary_dir;

void
ic_mem_init()
{
#ifdef DEBUG_BUILD
  mem_mutex= g_mutex_new();
#endif
}

void ic_mem_end()
{
#ifdef DEBUG_BUILD
  ic_printf("num_mem_allocs = %u", (guint32)num_mem_allocs);
  if (num_mem_allocs != (guint64)0)
    ic_printf("Memory leak found");
#endif
}
void
ic_port_set_binary_dir(const gchar *binary_dir)
{
  port_binary_dir= binary_dir;
}

gchar*
ic_get_strerror(int error_number, gchar *buf, guint32 buf_len)
{
  strerror_r(error_number, (char*)buf, (size_t)buf_len);
  return buf;
}

IC_TIMER
ic_nanos_elapsed(IC_TIMER start_time, IC_TIMER end_time)
{
#ifndef HAVE_GETHRTIME
#ifdef HAVE_CLOCK_GETTIME
#ifndef CLOCK_MONOTONIC
  if (end_time < start_time)
    return 0;
#endif
#else
  if (end_time < start_time)
    return 0;
#endif
#endif
  return (end_time - start_time);
}

IC_TIMER
ic_micros_elapsed(IC_TIMER start_time, IC_TIMER end_time)
{
  IC_TIMER timer= ic_nanos_elapsed(start_time, end_time);
  return timer/1000;
}

IC_TIMER
ic_millis_elapsed(IC_TIMER start_time, IC_TIMER end_time)
{
  IC_TIMER timer= ic_nanos_elapsed(start_time, end_time);
  return timer/1000000;
}

IC_TIMER
ic_gethrtime()
{
  IC_TIMER timer;
#ifdef HAVE_GETHRTIME
  hrtime_t ghr_timer;
  ghr_timer= gethrtime();
  timer= (IC_TIMER)ghr_timer;
#else
#ifdef HAVE_CLOCK_GETTIME
  struct timespec ts_timer;
#ifdef CLOCK_MONOTONIC
  clock_gettime(CLOCK_MONOTONIC, &ts_timer);
#else
  clock_gettime(CLOCK_REALTIME, &ts_timer);
#endif
  timer= 1000000000LL;
  timer*= ts_timer.tv_sec;
  timer+= ts_timer.tv_nsec;
#else
#ifdef HAVE_GETTIMEOFDAY
  struct timeval time_of_day;
  gettimeofday(&time_of_day, NULL);
  timer= 1000000000LL;
  timer*= time_of_day.tv_sec;
  timer+= (time_of_day.tv_usec * 1000);
#else
  No implementation of get time found
#endif
#endif
#endif
  return timer;
}

void
ic_sleep(guint32 seconds_to_sleep)
{
  sleep(seconds_to_sleep);
}

gchar *
ic_calloc(size_t size)
{
#ifdef DEBUG_BUILD
  g_mutex_lock(mem_mutex);
  num_mem_allocs++;
  g_mutex_unlock(mem_mutex);
#endif
  gchar *alloc_ptr= g_try_malloc0(size);
  return alloc_ptr;
}

gchar*
ic_realloc(gchar *ptr,
           size_t size)
{
  gchar *alloc_ptr= g_try_realloc((gpointer)ptr, size);
  return alloc_ptr;
}

gchar *
ic_malloc(size_t size)
{
#ifdef DEBUG_BUILD
  g_mutex_lock(mem_mutex);
  num_mem_allocs++;
  g_mutex_unlock(mem_mutex);
#endif
  gchar *alloc_ptr= g_try_malloc(size);
  return alloc_ptr;
}

void
ic_free(void *ret_obj)
{
#ifdef DEBUG_BUILD
  g_mutex_lock(mem_mutex);
  num_mem_allocs--;
  g_mutex_unlock(mem_mutex);
#endif
  g_free(ret_obj);
}

GPid
ic_get_own_pid()
{
  GPid pid;
  pid= getpid();
  return (GPid)pid;
}

void 
ic_kill_process(GPid pid, gboolean hard_kill)
{
  gchar buf[128];
  gchar *arg_vector[4];
  GError *error;
  int i= 0;
  guint32 len;

  ic_guint64_str((guint64)pid,buf, &len);
  DEBUG_PRINT(CONFIG_LEVEL, ("Kill process %s\n", buf));
  arg_vector[i++]="kill";
  if (hard_kill)
    arg_vector[i++]="-9";
  arg_vector[i++]=buf;
  arg_vector[i]=NULL;
  g_spawn_async(NULL,&arg_vector[0], NULL,
                G_SPAWN_SEARCH_PATH,
                NULL,NULL,&pid,&error);
}

int ic_start_process(gchar **argv,
                     gchar *working_dir,
                     GPid *pid)
{
  GError *error;

  if (g_spawn_async_with_pipes(working_dir,
                               argv,
                               NULL, /* environment */
                               0,    /* Flags */
                               NULL, NULL, /* Child setup stuff */
                               pid, /* Pid of program started */
                               NULL, /* stdin */
                               NULL, /* stdout */
                               NULL, /* stderr */
                               &error)) /* Error object */
  {
    /* Unsuccessful start */
    return 1;
  }
  return 0;
}

int run_process(gchar **argv,
                int *exit_status)
{
  GError *error= NULL;
  if (!g_spawn_sync(NULL,
                    argv,
                    NULL,
                    0,
                    NULL,
                    NULL,
                    NULL,
                    NULL,
                    exit_status,
                    &error))
  {
    ic_printf("Failed to run script, error: %s", error->message);
    return 0;
  }
  if (*exit_status)
  {
    ic_printf("Exit status %d\n", *exit_status);
    if (*exit_status < 0 || *exit_status > 2)
      *exit_status= 2;
    return *exit_status;
  }
  return 0;
}

int
ic_is_process_alive(guint32 pid,
                    const gchar *process_name)
{
  gchar *argv[6];
  gint exit_status;
  guint64 value= (guint64)pid;
  gchar *pid_number_str;
  IC_STRING script_string;
  gchar *script_name;
  int error;
  gchar pid_buf[64];
  gchar full_script_name[IC_MAX_FILE_NAME_SIZE];

  pid_number_str= ic_guint64_str(value, pid_buf, NULL);
#ifdef LINUX
  script_name= "check_process.sh";
#endif
#ifdef MACOSX
  script_name= "check_process.sh";
#endif
#ifdef SOLARIS
  script_name= "check_process.sh";
#endif
#ifdef FREEBSD
  script_name= "check_process.sh";
#endif
#ifdef WINDOWS
  script_name= "windows_check_process.sh";
#endif
  IC_INIT_STRING(&script_string, full_script_name, 0, TRUE);
  ic_add_string(&script_string, port_binary_dir);
  ic_add_string(&script_string, script_name);

  argv[0]= script_string.str;
  argv[1]= "--process_name";
  argv[2]= (gchar*)process_name;
  argv[3]= "--pid";
  argv[4]= pid_number_str;
  argv[5]= NULL;

  error= run_process(argv, &exit_status);
  if (error == (gint)1)
  {
    return 0; /* The process was alive */
  }
  else if (error == (gint)2)
  {
    return IC_ERROR_CHECK_PROCESS_SCRIPT;
  }
  return IC_ERROR_PROCESS_NOT_ALIVE;
}

int
ic_delete_file(const gchar *file_name)
{
  return g_unlink(file_name);
}

static int
get_file_length(int file_ptr, guint64 *read_size)
{
  gint64 size;
  int error;
  size= lseek(file_ptr, (off_t)0, SEEK_END);
  if (size == (gint64)-1)
    goto error;
  *read_size= size;
  size= lseek(file_ptr, (off_t)0, SEEK_SET);
  if (size == (gint64)-1)
    goto error;
  if (size != 0)
    return 1;
  return 0;
error:
  error= errno;
  return error;
}

int
ic_get_file_contents(const gchar *file, gchar **file_content,
                     guint64 *file_size)
{
  int file_ptr;
  int error;
  gchar *loc_ptr;
  guint64 read_size, size_left;
  DEBUG_ENTRY("ic_get_file_contents");

  file_ptr= g_open(file, O_RDONLY);
  if (file_ptr == (int)-1)
    goto error;
  if ((error= get_file_length(file_ptr, file_size)))
    return error;
  if (!(loc_ptr= ic_malloc((*file_size) + 1)))
    return IC_ERROR_MEM_ALLOC;
  loc_ptr[*file_size]= 0;
  *file_content= loc_ptr;
  size_left= *file_size;
  do
  {
    if ((error= ic_read_file(file_ptr, loc_ptr, size_left, &read_size)))
    {
      ic_free(*file_content);
      *file_size= 0;
      return error;
    }
    if (read_size == size_left)
      return 0;
    loc_ptr+= read_size;
    size_left-= read_size;
  } while (1);
error:
  error= errno;
  return error;
}

int
ic_write_file(int file_ptr, const gchar *buf, size_t size)
{
  int ret_code;
  do
  {
    ret_code= write(file_ptr, (const void*)buf, size);
    if (ret_code == (int)-1)
    {
      ret_code= errno;
      return ret_code;
    }
    if (ret_code == (int)size)
      return 0;
    buf+= ret_code;
    size-= ret_code;
  } while (1);
  return 0;
}

int
ic_read_file(int file_ptr, gchar *buf, size_t size, guint64 *len)
{
  int ret_code;
  ret_code= read(file_ptr, (void*)buf, size);
  if (ret_code == (int)-1)
  {
    ret_code= errno;
    return ret_code;
  }
  *len= (guint32)ret_code;
  return 0;
}

#ifndef WIN32
#include <signal.h>
static IC_SIG_HANDLER_FUNC glob_die_handler= NULL;
static void *glob_die_param;
static IC_SIG_HANDLER_FUNC glob_sig_error_handler= NULL;
static void *glob_sig_error_param;

static void
sig_error_handler(int signum)
{
  DEBUG_ENTRY("sig_error_handler");
  switch (signum)
  {
    case SIGSEGV:
    case SIGFPE:
    case SIGILL:
    case SIGBUS:
    case SIGSYS:
      break;
    default:
      DEBUG_RETURN_EMPTY;
  }
  if (glob_sig_error_handler)
    glob_sig_error_handler(glob_sig_error_param);
  DEBUG_RETURN_EMPTY;
}

void
ic_set_sig_error_handler(IC_SIG_HANDLER_FUNC error_handler, void *param)
{
  DEBUG_ENTRY("unix:ic_set_sig_error_handler");
  glob_sig_error_handler= error_handler;
  glob_sig_error_param= param;
  signal(SIGSEGV, sig_error_handler);
  signal(SIGFPE, sig_error_handler);
  signal(SIGILL, sig_error_handler);
  signal(SIGBUS, sig_error_handler);
  signal(SIGSYS, sig_error_handler);
  DEBUG_RETURN_EMPTY;
}

static void
kill_handler(int signum)
{
  DEBUG_ENTRY("kill_handler");
  switch (signum)
  {
    case SIGTERM:
    case SIGABRT:
    case SIGQUIT:
    case SIGXCPU:
#ifdef SIGINFO
    case SIGINFO:
#else
#ifdef SIGPWR
    case SIGPWR:
#endif
#endif
      break;
    default:
      DEBUG_RETURN_EMPTY;
  }
  if (glob_die_handler)
    glob_die_handler(glob_die_param);
  DEBUG_RETURN_EMPTY;
}

void
ic_set_die_handler(IC_SIG_HANDLER_FUNC die_handler, void *param)
{
  DEBUG_ENTRY("unix:ic_set_die_handler");
  glob_die_param= param;
  glob_die_handler= die_handler;
  signal(SIGTERM, kill_handler);
  signal(SIGABRT, kill_handler);
  signal(SIGQUIT, kill_handler);
  signal(SIGXCPU, kill_handler);
#ifdef SIGINFO
  signal(SIGINFO, kill_handler);
#else
#ifdef SIGPWR
  signal(SIGPWR, kill_handler);
#endif
#endif
  DEBUG_RETURN_EMPTY;
}
#else
void
ic_set_die_handler(IC_SIG_HANDLER_FUNC *die_handler,
                   void *param)
{
  DEBUG_ENTRY("windows:ic_set_die_handler");
  (void)die_handler;
  (void)param;
  DEBUG_RETURN_EMPTY;
}

void
ic_set_sig_error_handler(IC_SIG_HANDLER_FUNC *error_handler, void *param)
{
  DEBUG_ENTRY("windows:ic_set_sig_error_handler");
  (void)error_handler;
  (void)param;
  DEBUG_RETURN_EMPTY;
}
#endif

#ifndef WIN32
static void
child_exit_handler(int signum)
{
  if (signum == SIGCHLD)
    wait(NULL);
}

static void
sig_handler(int signum)
{
  switch (signum)
  {
    case SIGCHLD:
    case SIGUSR1:
    case SIGALRM:
      exit(0);
      break;
  }
}
#endif

int
ic_daemonize(gchar *log_file)
{
#ifndef WIN32
  static int failed= 0;
  pid_t child_pid, session_id, parent_pid;

  signal(SIGCHLD, sig_handler);
  signal(SIGUSR1, sig_handler);
  signal(SIGALRM, sig_handler);

  child_pid= fork();
  if (child_pid < 0)
    return IC_ERROR_FAILED_TO_DAEMONIZE;
  if (child_pid > 0)
  {
    /*
      Pause and wait for signal, ensure we deliver signal to
      ourselves if no signal arrives.
    */
    alarm(2);
    pause();
    if (failed)
      return IC_ERROR_FAILED_TO_DAEMONIZE;
    exit(0);
  }
  /* We come here as the child process, the parent process will exit */
  signal(SIGCHLD, child_exit_handler);
  signal(SIGHUP, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  signal(SIGCONT, SIG_IGN);
  signal(SIGSTOP, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  parent_pid= getppid(); /* Get pid of original process */

  session_id= setsid();
  if (session_id < 0)
  {
     /* Failed to create a new session for new process */
     failed= 1;
     exit(1);
  }

  freopen("/dev/null", "r", stdin);
  freopen(log_file, "w", stdout);
  freopen(log_file, "w", stderr);


  kill(parent_pid, SIGUSR1); /* Wake parent process to make it exit */
  return 0;
#else
  return 0;
#endif
}
