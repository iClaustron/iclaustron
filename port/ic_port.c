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

#ifdef LINUX
/*
  Portability fix for Linux
  We need to include before ic_base_header.h since it
  will include string.h without define.
*/
#define _XOPEN_SOURCE 600
#endif
#include <string.h>
#undef _XOPEN_SOURCE
#ifdef DEBUG_BUILD
#include <stdio.h>
#endif
#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_string.h>
#include <glib/gstdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

static GMutex *exec_output_mutex= NULL;

#ifdef DEBUG_BUILD
static guint64 num_mem_allocs= 0;
static GMutex *mem_mutex= NULL;
guint32 error_inject= 0;
#endif
static const gchar *port_binary_dir;
static const gchar *port_config_dir;
static guint32 ic_stop_flag= 0;

#ifdef WINDOWS
IC_POLL_FUNCTION ic_poll;
#endif

int ic_stop_socket_system()
{
  int ret_code= 0;
#ifdef WINDOWS
  ret_code= WSACleanup();
#endif
  return ret_code;
}

/* This method can't use DEBUG system since it isn't initialised when called */
int ic_start_socket_system()
{
#ifdef WINDOWS
  WORD version_requested;
  WSADATA wsa_data;
  int error;
  version_requested= MAKEWORD(2,2);

  if ((error= WSAStartup(version_requested, &wsa_data)))
  {
    ic_printf("WSAStartup failed with error code = %d",
              GetLastError());
    return 1;
  }
  if (LOBYTE(wsa_data.wVersion) != 2 ||
      HIBYTE(wsa_data.wVersion) != 2)
  {
    ic_printf("WSAStartup failed to start with version 2,2");
    return 1;
  }
  if (!(ic_poll= (IC_POLL_FUNCTION)(void*)
        GetProcAddress(LoadLibrary("ws2_32"), "WSAPoll")))
  {
    ic_printf("Minimum Windows Vista is required, support of WSAPoll lacking");
    return 1;
  }
  return 0;
#else
  /* Not needed on non-Windows platforms */
  return 0;
#endif
}

guint32
ic_get_stop_flag()
{
  return ic_stop_flag;
}

/*
  This method can't be debugged since it's called before debug system
  has been started.
*/
void
ic_port_init()
{
  exec_output_mutex= g_mutex_new();
#ifdef DEBUG_BUILD
  mem_mutex= g_mutex_new();
#endif
}

void ic_port_end()
{
#ifdef DEBUG_BUILD
  ic_printf("num_mem_allocs = %u", (guint32)num_mem_allocs);
  if (num_mem_allocs != (guint64)0)
    ic_printf("Memory leak found");
  g_mutex_free(mem_mutex);
#endif
  g_mutex_free(exec_output_mutex);
}

void
ic_close_socket(int sockfd)
{
  int error;

#ifdef WINDOWS
  if (closesocket(sockfd))
    error= WSAGetLastError();
#else
  do
  {
    error= 0;
    if (close(sockfd))
      error= errno;
  } while (error == EINTR);
#endif
  if (error)
  {
    DEBUG_PRINT(COMM_LEVEL, ("close failed with errno = %d", error));
  }
}

int
ic_get_last_error()
{
#ifdef WINDOWS
  return GetLastError();
#else
  return errno;
#endif
}

int
ic_get_last_socket_error()
{
#ifdef WINDOWS
  return WSAGetLastError();
#else
  return errno;
#endif
}

void
ic_set_port_config_dir(const gchar *config_dir)
{
  port_config_dir= config_dir;
}

void
ic_set_port_binary_dir(const gchar *binary_dir)
{
  port_binary_dir= binary_dir;
}

gchar*
ic_get_strerror(int error_number, gchar *buf, guint32 buf_len)
{
#ifdef WINDOWS
  strerror_s((char*)buf, (size_t)buf_len, error_number);
#else
  strerror_r(error_number, (char*)buf, (size_t)buf_len);
#endif
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
#ifdef WINDOWS
  guint64 win_time, win_freq;

  QueryPerformanceFrequency((LARGE_INTEGER*)&win_freq);
  QueryPerformanceCounter((LARGE_INTEGER*)&win_time);
  timer= (win_time / win_freq) * 1000000;
#else
  No implementation of get time found
#endif
#endif
#endif
#endif
  return timer;
}

void
ic_sleep(guint32 seconds_to_sleep)
{
#ifndef WINDOWS
  sleep(seconds_to_sleep);
#else
  Sleep(seconds_to_sleep);
#endif
}

void
ic_microsleep(guint32 microseconds_to_sleep)
{
  struct timeval time;
  guint32 seconds, microseconds;

  seconds= microseconds_to_sleep / 1000000;
  microseconds= microseconds_to_sleep - (seconds * 1000000);
  time.tv_sec= seconds;
  time.tv_usec= microseconds;
  select(0, NULL, NULL, NULL, &time);
}

gchar *
ic_calloc(size_t size)
{
#ifdef DEBUG_BUILD
  g_mutex_lock(mem_mutex);
  num_mem_allocs++;
  g_mutex_unlock(mem_mutex);
#endif
  return g_try_malloc0(size);
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
  return g_try_malloc(size);
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

#ifndef WINDOWS
IC_PID_TYPE
ic_get_own_pid()
{
  IC_PID_TYPE pid;

  pid= (IC_PID_TYPE)getpid();
  return pid;
}

void 
ic_kill_process(IC_PID_TYPE pid, gboolean hard_kill)
{
  if (hard_kill)
    kill((pid_t)pid, SIGKILL);
  else
    kill((pid_t)pid, SIGTERM);
}

void ic_controlled_terminate()
{
  IC_PID_TYPE my_pid;

  my_pid= ic_get_own_pid();
  kill((pid_t)my_pid, SIGTERM);
}
#else /* WINDOWS */
IC_PID_TYPE
ic_get_own_pid()
{
  IC_PID_TYPE pid;

  pid= (IC_PID_TYPE)GetCurrentProcessId();
  return pid;
}

void 
ic_kill_process(IC_PID_TYPE pid, gboolean hard_kill)
{
}

void
ic_controlled_terminate()
{
}
#endif

int ic_start_process(gchar **argv,
                     gchar *working_dir,
                     IC_PID_TYPE *pid)
{
  GError *error;
  GPid loc_pid;

  if (g_spawn_async_with_pipes(working_dir,
                               argv,
                               NULL, /* environment */
                               0,    /* Flags */
                               NULL, NULL, /* Child setup stuff */
                               &loc_pid, /* Pid of program started */
                               NULL, /* stdin */
                               NULL, /* stdout */
                               NULL, /* stderr */
                               &error)) /* Error object */
  {
    /* Unsuccessful start */
    return 1;
  }
  *pid= (IC_PID_TYPE)loc_pid;
  return 0;
}

int run_process(gchar **argv,
                int *exit_status,
                gchar *log_file_name)
{
  GError *error= NULL;
  int ret_code= 0;
  guint64 len= 1;
  IC_FILE_HANDLE file_handle;
  gchar read_buf[4];

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
    goto early_end;
  }
  if ((ret_code= ic_open_file(&file_handle, log_file_name, FALSE)))
  {
    *exit_status= 2;
    goto end;
  }
  if ((ret_code= ic_read_file(file_handle, read_buf, 1, &len)))
  {
    *exit_status= 2;
    goto end;
  }
  if (read_buf[0] == '0')
    *exit_status= 0;
  else if (read_buf[0] == '1')
    *exit_status= 1;
  else
    *exit_status= 2;
  if (*exit_status)
  {
    ic_printf("Exit status %d\n", *exit_status);
  }

end:
  ic_close_file(file_handle);
  ic_delete_file(log_file_name);
early_end:
  return ret_code;
}

int
ic_is_process_alive(IC_PID_TYPE pid,
                    const gchar *process_name)
{
  gchar *argv[8];
  gint exit_status;
  guint64 value= (guint64)pid;
  gchar *pid_number_str;
  IC_STRING script_string;
  IC_STRING log_file_name_str;
  gchar *script_name;
  int error;
  gchar pid_buf[64];
  gchar full_script_name[IC_MAX_FILE_NAME_SIZE];
  gchar log_file_name_buf[IC_MAX_FILE_NAME_SIZE];

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

  value= (guint64)ic_get_own_pid();
  pid_number_str= ic_guint64_str(value, pid_buf, NULL);
  IC_INIT_STRING(&log_file_name_str, log_file_name_buf, 0, TRUE);
  ic_add_ic_string(&log_file_name_str, &ic_glob_config_dir);
  ic_add_string(&log_file_name_str, "tmp_");
  ic_add_string(&log_file_name_str, pid_number_str);
  ic_add_string(&log_file_name_str, ".txt");

  argv[0]= script_string.str;
  argv[1]= "--process_name";
  argv[2]= (gchar*)process_name;
  argv[3]= "--pid";
  argv[4]= pid_number_str;
  argv[5]= "--log_file";
  argv[6]= log_file_name_str.str;
  argv[7]= NULL;

  g_mutex_lock(exec_output_mutex);
  error= run_process(argv, &exit_status, log_file_name_str.str);
  g_mutex_unlock(exec_output_mutex);
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

#ifdef WINDOWS
static int
portable_open_file(IC_FILE_HANDLE *handle,
                   const gchar *file_name,
                   gboolean create_flag,
                   gboolean open_flag)
{
  DWORD create_value;
  HANDLE file_handle;

  if (!open_flag)
    create_value= CREATE_ALWAYS;
  else if (create_flag)
    create_value= OPEN_ALWAYS;
  else
    create_value= OPEN_EXISTING;

  file_handle=
    CreateFile(file_name,
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
      NULL,
      create_value,
      FILE_ATTRIBUTE_NORMAL,
      NULL);

  if (file_handle != INVALID_HANDLE_VALUE)
  {
    *handle= file_handle;
    return 0;
  }
  else
    return GetLastError();
}
#else
static int
portable_open_file(IC_FILE_HANDLE *handle,
                   const gchar *file_name,
                   gboolean create_flag,
                   gboolean open_flag)
{
  int flags= O_RDWR | O_SYNC;
  int file_ptr;
  
  if (create_flag)
    flags|= O_CREAT;
  if (!open_flag)
    flags|= O_TRUNC;

  if (create_flag)
    file_ptr= open(file_name, flags, S_IWRITE | S_IREAD);
  else
    file_ptr= open(file_name, flags);
  if (file_ptr  == (int)-1)
    return errno;
  else
  {
    *handle= file_ptr;
    return 0;
  }
}
#endif

int
ic_open_file(IC_FILE_HANDLE *handle,
             const gchar *file_name,
             gboolean create_flag)
{
  return portable_open_file(handle, file_name, create_flag, TRUE);
}

int
ic_create_file(IC_FILE_HANDLE *handle,
               const gchar *file_name)
{
  return portable_open_file(handle, file_name, TRUE, FALSE);
}

int
ic_close_file(IC_FILE_HANDLE file_ptr)
{
#ifndef WINDOWS
  if ((close(file_ptr)) == (int)-1)
    return errno;
  else
    return 0;
#else
  if (!CloseHandle(file_ptr))
    return GetLastError();
  else
    return 0;
#endif
}

static int
get_file_length(IC_FILE_HANDLE file_ptr, guint64 *read_size)
{
  int error= 0;
#ifndef WINDOWS
  gint64 size;

  size= lseek(file_ptr, (off_t)0, SEEK_END);
  if (size == (gint64)-1)
  {
    error= errno;
    goto error;
  }
  *read_size= size;
  size= lseek(file_ptr, (off_t)0, SEEK_SET);
  if (size == (gint64)-1)
  {
    error= errno;
    goto error;
  }
  if (size != 0)
    return 1;
  return 0;
error:
  return error;
#else
  if (!GetFileSizeEx(file_ptr, (LARGE_INTEGER*)read_size))
  {
    error= GetLastError();
  }
  return error;
#endif
}

int
ic_get_file_contents(const gchar *file,
                     gchar **file_content,
                     guint64 *file_size)
{
  IC_FILE_HANDLE file_ptr;
  int error;
  gchar *loc_ptr;
  guint64 read_size;
  size_t size_left;
  DEBUG_ENTRY("ic_get_file_contents");

  if ((error= ic_open_file(&file_ptr, file, FALSE)))
    goto get_error;
  if ((error= get_file_length(file_ptr, file_size)))
    goto error;
  if (!(loc_ptr= ic_malloc((size_t)((*file_size) + 1))))
  {
    DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
  }
  loc_ptr[*file_size]= 0;
  *file_content= loc_ptr;
  size_left= (size_t)*file_size;
  do
  {
    if ((error= ic_read_file(file_ptr, loc_ptr, size_left, &read_size)))
    {
      ic_free(*file_content);
      *file_size= 0;
      goto error;
    }
    if (read_size == size_left)
    {
      DEBUG_RETURN(0);
    }
    loc_ptr+= read_size;
    size_left-= (size_t)read_size;
  } while (1);
get_error:
  error= ic_get_last_error();
error:
  DEBUG_RETURN(error);
}

static int
portable_write(IC_FILE_HANDLE file_ptr,
               const void *buf,
               size_t size,
               size_t *ret_size)
{
#ifndef WINDOWS
  ssize_t ret_code;
  ret_code= write(file_ptr, buf, size);
  if (ret_code != (int)-1)
  {
    *ret_size= (size_t)ret_code;
    return 0;
  }
  else
    return errno;
#else
  if (WriteFile(file_ptr,
                buf,
                size,
                ret_size,
                NULL))
    return 0;
  else
    return GetLastError();
#endif
}

int
ic_write_file(IC_FILE_HANDLE file_ptr, const gchar *buf, size_t size)
{
  int ret_code;
  size_t ret_size= 0;
  do
  {
    if ((ret_code= portable_write(file_ptr,
                                  (const void*)buf,
                                  size,
                                  &ret_size)))
      return ret_code;
    if (ret_size == size)
      return 0;
    buf+= ret_size;
    size-= ret_size;
  } while (1);
  return 0;
}

static int
portable_read(IC_FILE_HANDLE file_ptr,
              void *buf,
              size_t size,
              size_t *ret_size)
{
#ifndef WINDOWS
  ssize_t ret_code;
  ret_code= read(file_ptr, buf, size);
  if (ret_code != (int)-1)
  {
    *ret_size= (size_t)ret_code;
    return 0;
  }
  else
    return errno;
#else
  if (ReadFile(file_ptr,
               buf,
               size,
               ret_size,
               NULL))
    return 0;
  else
    return GetLastError();
#endif
}

int
ic_read_file(IC_FILE_HANDLE file_ptr, gchar *buf, size_t size, guint64 *len)
{
  int ret_code;
  size_t ret_size= 0;

  if ((ret_code= portable_read(file_ptr, (void*)buf, size, &ret_size)))
    return ret_code;
  *len= (guint32)ret_size;
  return 0;
}

#ifndef WINDOWS
#include <signal.h>
static IC_SIG_HANDLER_FUNC glob_die_handler= NULL;
static void *glob_die_param;
static IC_SIG_HANDLER_FUNC glob_sig_error_handler= NULL;
static void *glob_sig_error_param;

static void
sig_error_handler(int signum)
{
  DEBUG_ENTRY("sig_error_handler");
  DEBUG_PRINT(COMM_LEVEL, ("signum = %d", signum));

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
  ic_stop_flag= 1;
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
  /* signal(SIGBUS, sig_error_handler); */
  signal(SIGSYS, sig_error_handler);
  signal(SIGPIPE, SIG_IGN);
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
  ic_stop_flag= 1;
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
ic_set_die_handler(IC_SIG_HANDLER_FUNC die_handler,
                   void *param)
{
  DEBUG_ENTRY("windows:ic_set_die_handler");

  (void)die_handler;
  (void)param;
  DEBUG_RETURN_EMPTY;
}

void
ic_set_sig_error_handler(IC_SIG_HANDLER_FUNC error_handler, void *param)
{
  DEBUG_ENTRY("windows:ic_set_sig_error_handler");

  (void)error_handler;
  (void)param;
  DEBUG_RETURN_EMPTY;
}
#endif

#ifndef WINDOWS
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
#ifndef WINDOWS
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

guint32 ic_byte_order()
{
  guint32 loc_variable= 1;
  gchar *loc_char_ptr= (gchar*)&loc_variable;

  if (loc_char_ptr[0] == 1)
    return 0;
  else
    return 1;
}

void ic_cond_timed_wait(GCond *cond,
                        GMutex *mutex,
                        guint32 micros)
{
  GTimeVal stop_timer;

  g_get_current_time(&stop_timer);
  g_time_val_add(&stop_timer, micros);
  g_cond_timed_wait(cond, mutex, &stop_timer);
}
