/* Copyright (C) 2007-2012 iClaustron AB

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
#include <ic_hashtable.h>
#include <ic_hashtable_itr.h>
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

static gchar *ic_underscore_str= "_";
static gchar *ic_tmp_str= "tmp";
static gchar *ic_hw_str= "hw";
static gchar *ic_ps_str= "hw";
static gchar *ic_log_str= "log";
static gchar *ic_out_str= "out";
static IC_MUTEX *exec_output_mutex= NULL;
static guint64 file_number= 0;

#ifdef DEBUG_BUILD
static guint64 num_mem_allocs= 0;
static guint64 num_mem_conn_allocs= 0;
static guint64 num_mem_hash_allocs= 0;
static guint64 num_mem_mc_allocs= 0;
static IC_MUTEX *mem_mutex= NULL;
static IC_MUTEX *mem_conn_mutex= NULL;
static IC_MUTEX *mem_hash_mutex= NULL;
static IC_MUTEX *mem_mc_mutex= NULL;
guint32 error_inject= 0;
#endif
static const gchar *port_binary_dir;
static const gchar *port_config_dir;
static guint32 ic_stop_flag= 0;

#ifdef WINDOWS
IC_POLL_FUNCTION ic_poll;
#endif

/**
  On Windows it's necessary to stop socket subsystem.
*/
int ic_stop_socket_system()
{
  int ret_code= 0;
#ifdef WINDOWS
  ret_code= WSACleanup();
#endif
  return ret_code;
}

/**
  On Windows it's necessary to start the socket subsystem.

  @note
    This method can't use DEBUG system since it isn't initialised when called
*/
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

/**
  Anyone wanting to stop the iClaustron subsystem can indicate this
  by setting the stop flag. The iClaustron code needs to check this
  regularly (at least every 3 seconds or so).

  @retval 1 if iClaustron is stopped, 0 otherwise
*/
guint32
ic_get_stop_flag()
{
  return ic_stop_flag;
}

/**
  A function used to stop the iClaustron subsystem.
*/
void
ic_set_stop_flag()
{
  ic_stop_flag= 1;
}

#ifdef DEBUG_BUILD
/* Hash table containing locked mutexes */
static IC_HASHTABLE *mutex_hash= NULL;
static IC_MUTEX *mutex_hash_protect= NULL;

/* Hash table containing memory allocations */
static IC_HASHTABLE *mem_entry_hash= NULL;
static IC_MUTEX *mem_entry_hash_mutex= NULL;
#endif

/**
  It is necessary to initialise the portability subsystem before
  we can use it, especially so when using it in debug mode.

  @note
    This method can't be debugged since it's called before debug system
    has been started.
*/
void
ic_port_init()
{
  ic_require(exec_output_mutex= ic_mutex_create());
#ifdef DEBUG_BUILD
  ic_require(mem_mutex= ic_mutex_create());
  ic_require(mem_conn_mutex= ic_mutex_create());
  ic_require(mem_hash_mutex= ic_mutex_create());
  ic_require(mem_mc_mutex= ic_mutex_create());

  ic_require(mem_entry_hash_mutex= g_mutex_new());

  ic_require(mutex_hash_protect= g_mutex_new());

  ic_require(mem_entry_hash= ic_create_hashtable(4096,
                                                 ic_hash_ptr,
                                                 ic_keys_equal_ptr,
                                                 TRUE));
  ic_require(mutex_hash= ic_create_hashtable(4096,
                                             ic_hash_ptr,
                                             ic_keys_equal_ptr,
                                             TRUE));
#endif
}

/**
  End the use of the portability subsystem
*/
void ic_port_end()
{
#ifdef DEBUG_BUILD
  IC_HASHTABLE_ITR itr, *hash_itr;
  void *key;
  gchar ptr_str[32];

  ic_hashtable_destroy(mutex_hash, TRUE);

  if (num_mem_allocs != (guint64)0 ||
      num_mem_conn_allocs != (guint64)0 ||
      num_mem_hash_allocs != (guint64)0 ||
      num_mem_mc_allocs != (guint64)0)
  {
    hash_itr= ic_hashtable_iterator(mem_entry_hash, &itr, TRUE);
    while (ic_hashtable_iterator_advance(hash_itr) != 0)
    {
      key= ic_hashtable_iterator_key(hash_itr);
      ic_guint64_hex_str((guint64)key, ptr_str);
      ic_printf("Leaked memory address = %s", ptr_str);
    }
  }

  ic_hashtable_destroy(mem_entry_hash, TRUE);

  if (num_mem_allocs != (guint64)0 ||
      num_mem_conn_allocs != (guint64)0 ||
      num_mem_hash_allocs != (guint64)0 ||
      num_mem_mc_allocs != (guint64)0)
  {
    ic_printf("num_mem_allocs = %u", (guint32)num_mem_allocs);
    ic_printf("num_mem_conn_allocs = %u", (guint32)num_mem_conn_allocs);
    ic_printf("num_mem_hash_allocs = %u", (guint32)num_mem_hash_allocs);
    ic_printf("num_mem_mc_allocs = %u", (guint32)num_mem_mc_allocs);

    if (num_mem_conn_allocs != (guint64)0)
      ic_printf("Memory leak found in use of comm subsystem or in it");
    if (num_mem_hash_allocs != (guint64)0)
      ic_printf("Memory leak found in use of hash tables or in it");
    if (num_mem_mc_allocs != (guint64)0)
      ic_printf("Memory leak found in use of memory container or in it");
  }
  else
    ic_printf("No memory leaks found");

  ic_mutex_destroy(&mutex_hash_protect);
  ic_mutex_destroy(&mem_mutex);
  ic_mutex_destroy(&mem_conn_mutex);
  ic_mutex_destroy(&mem_hash_mutex);
  ic_mutex_destroy(&mem_mc_mutex);
  ic_mutex_destroy(&mem_entry_hash_mutex);
#endif
  ic_mutex_destroy(&exec_output_mutex);
}

/**
  Portable manner to close a socket

  @parameter sockfd        The socket fd to close

  @note
    Errors are indicated through errno/WSAGetLastError in the cases
    it's necessary to actually check the error from the close socket.
*/
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

/**
  Portable manner to get last error reported by OS
*/
int
ic_get_last_error()
{
#ifdef WINDOWS
  return GetLastError();
#else
  return errno;
#endif
}

/**
  Portable manner to get last socket error reported by OS
*/
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
ic_sleep_low(guint32 seconds_to_sleep)
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

gchar*
ic_calloc_low(size_t size)
{
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
ic_malloc_low(size_t size)
{
  return g_try_malloc(size);
}

void
ic_free_low(void *ret_obj)
{
  g_free(ret_obj);
}

#ifdef DEBUG_BUILD
static void
print_alloc(size_t size, gchar *ret_ptr, gchar *place)
{
  gchar ptr_str[32];

  ic_guint64_hex_str((guint64)ret_ptr, ptr_str);
  DEBUG_PRINT(MALLOC_LEVEL, ("Allocated %u bytes at %s for %s",
              size, ptr_str, place));
}

static void
print_free(gchar *ret_ptr, gchar *place)
{
  gchar ptr_str[32];

  ic_guint64_hex_str((guint64)ret_ptr, ptr_str);
  DEBUG_PRINT(MALLOC_LEVEL, ("Freed at %s for %s",
              ptr_str, place));
}

static void
insert_alloc_entry(gchar *entry)
{
  ic_mutex_lock_low(mem_entry_hash_mutex);
  ic_require(ic_hashtable_search(mem_entry_hash, (void*)entry) == NULL);
  ic_hashtable_insert(mem_entry_hash, (void*)entry, (void*)entry);
  ic_mutex_unlock_low(mem_entry_hash_mutex);
}

static void
remove_alloc_entry(void *entry)
{
  void *key;
  ic_mutex_lock_low(mem_entry_hash_mutex);
  ic_require(ic_hashtable_search(mem_entry_hash, entry) != NULL);
  key= ic_hashtable_remove(mem_entry_hash, (void*)entry);
  ic_mutex_unlock_low(mem_entry_hash_mutex);
  ic_require(key);
}

gchar*
ic_calloc_conn(size_t size)
{
  gchar *ret_ptr;

  ic_mutex_lock_low(mem_conn_mutex);
  num_mem_conn_allocs++;
  ic_mutex_unlock_low(mem_conn_mutex);
  ret_ptr= ic_calloc_low(size);
  ic_require(ret_ptr);
  insert_alloc_entry(ret_ptr);
  print_alloc(size, ret_ptr, "conn");
  return ret_ptr;
}

gchar*
ic_malloc_conn(size_t size)
{
  return ic_calloc_conn(size);
}

gchar*
ic_calloc_mc(size_t size)
{
  gchar *ret_ptr;

  ic_mutex_lock_low(mem_mc_mutex);
  num_mem_mc_allocs++;
  ic_mutex_unlock_low(mem_mc_mutex);
  ret_ptr= ic_calloc_low(size);
  ic_require(ret_ptr);
  insert_alloc_entry(ret_ptr);
  print_alloc(size, ret_ptr, "mc");
  return ret_ptr;
}

gchar*
ic_calloc(size_t size)
{
  gchar *ret_ptr;

  ic_mutex_lock_low(mem_mutex);
  num_mem_allocs++;
  ic_mutex_unlock_low(mem_mutex);
  ret_ptr= ic_calloc_low(size);
  ic_require(ret_ptr);
  insert_alloc_entry(ret_ptr);
  print_alloc(size, ret_ptr, "genc");
  return ret_ptr;
}

gchar *
ic_malloc_hash(size_t size, gboolean is_create)
{
  gchar *ret_ptr;

  ic_mutex_lock_low(mem_hash_mutex);
  num_mem_hash_allocs++;
  ic_mutex_unlock_low(mem_hash_mutex);
  ret_ptr= ic_malloc_low(size);
  ic_require(ret_ptr);
  if (is_create)
    insert_alloc_entry(ret_ptr);
  print_alloc(size, ret_ptr, "hash");
  return ret_ptr;
}

gchar *
ic_malloc(size_t size)
{
  gchar *ret_ptr;

  ic_mutex_lock_low(mem_mutex);
  num_mem_allocs++;
  ic_mutex_unlock_low(mem_mutex);
  ret_ptr= ic_malloc_low(size);
  insert_alloc_entry(ret_ptr);
  print_alloc(size, ret_ptr, "gen");
  return ret_ptr;
}

void
ic_free_conn(void *ret_ptr)
{
  ic_mutex_lock_low(mem_conn_mutex);
  num_mem_conn_allocs--;
  ic_mutex_unlock_low(mem_conn_mutex);
  print_free(ret_ptr, "conn");
  remove_alloc_entry(ret_ptr);
  ic_free_low(ret_ptr);
}

void
ic_free_hash(void *ret_ptr, gboolean is_destroy)
{
  ic_mutex_lock_low(mem_hash_mutex);
  num_mem_hash_allocs--;
  ic_mutex_unlock_low(mem_hash_mutex);
  print_free(ret_ptr, "hash");
  if (is_destroy)
    remove_alloc_entry(ret_ptr);
  ic_free_low(ret_ptr);
}

void
ic_free_mc(void *ret_ptr)
{
  ic_mutex_lock_low(mem_mc_mutex);
  num_mem_mc_allocs--;
  ic_mutex_unlock_low(mem_mc_mutex);
  remove_alloc_entry(ret_ptr);
  print_free(ret_ptr, "mc");
  ic_free_low(ret_ptr);
}

void
ic_free(void *ret_ptr)
{
  ic_mutex_lock_low(mem_mutex);
  num_mem_allocs--;
  ic_mutex_unlock_low(mem_mutex);
  print_free(ret_ptr, "gen");
  remove_alloc_entry(ret_ptr);
  ic_free_low(ret_ptr);
}
#endif

void ic_controlled_terminate()
{
  abort();
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
  DEBUG_ENTRY("ic_kill_process");
  DEBUG_PRINT(PROGRAM_LEVEL, ("Kill pid %u", (guint32)pid));

  if (hard_kill)
    kill((pid_t)pid, SIGKILL);
  else
    kill((pid_t)pid, SIGTERM);
  DEBUG_RETURN_EMPTY;
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
  DEBUG_ENTRY("ic_kill_process");
  DEBUG_PRINT(PROGRAM_LEVEL, ("Kill pid %u", (guint32)pid));
  /* Windows variant of kill */
  DEBUG_RETURN_EMPTY;
}
#endif

void strip_leading_blanks(gchar *argv_str)
{
  guint32 i= 0;
  guint32 num_blanks= 0;

  while (argv_str[i] == SPACE_CHAR)
  {
    argv_str[0]= argv_str[i + 1];
    num_blanks++;
    i++;
  }
  while (argv_str[i])
  {
    argv_str[i]= argv_str[i + num_blanks];
    i++;
  }
}

int ic_start_process(gchar **argv,
                     gchar *working_dir,
                     IC_PID_TYPE *pid)
{
  GError *error= NULL;
  GPid loc_pid;
  guint32 i= 1;
  DEBUG_ENTRY("ic_start_process");
  DEBUG_PRINT(PROGRAM_LEVEL, ("Starting process %s", argv[0]));
  DEBUG_PRINT(PROGRAM_LEVEL, ("Working dir = %s", working_dir));

  while (argv[i])
  {
    strip_leading_blanks(argv[i]);
    DEBUG_PRINT(PROGRAM_LEVEL, ("Argument %d = %s", i, argv[i]));
    i++;
  }
  if (!g_spawn_async_with_pipes(working_dir,
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
    DEBUG_PRINT(PROGRAM_LEVEL,
      ("Spawn failed with message %s",
      error->message));
    DEBUG_RETURN_INT(1);
  }
  *pid= (IC_PID_TYPE)loc_pid;
  DEBUG_PRINT(PROGRAM_LEVEL, ("Process with pid %d successfully started",
                              (int)*pid));
  DEBUG_RETURN_INT(0);
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
#ifdef DEBUG_BUILD
  guint32 i;
#endif
  DEBUG_ENTRY("run_process");

#ifdef DEBUG_BUILD
  DEBUG_PRINT(THREAD_LEVEL, ("Running script with parameters:"));
  i= 0;
  while (argv[i] != NULL)
  {
    DEBUG_PRINT(THREAD_LEVEL, ("argv[%u] = %s", i, argv[i]));
    i++;
  }
#endif
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
  DEBUG_PRINT(PROGRAM_LEVEL,
    ("Exit status %d from run_process", *exit_status));

end:
  ic_close_file(file_handle);
  ic_delete_file(log_file_name);
early_end:
  DEBUG_RETURN_INT(ret_code);
}

int
ic_is_process_alive(IC_PID_TYPE pid,
                    const gchar *process_name)
{
  gchar *argv[8];
  gint exit_status;
  gchar *pid_number_str;
  IC_STRING script_string;
  IC_STRING log_file_name_str;
  gchar *script_name;
  gchar *file_number_str;
  int error;
  guint64 loc_file_number;
  gchar pid_buf[IC_MAX_INT_STRING];
  gchar file_number_buf[IC_MAX_INT_STRING];
  gchar full_script_name[IC_MAX_FILE_NAME_SIZE];
  gchar log_file_name_buf[IC_MAX_FILE_NAME_SIZE];
  DEBUG_ENTRY("ic_is_process_alive");

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
  /* Create script string name */
  IC_INIT_STRING(&script_string, full_script_name, 0, TRUE);
  ic_add_string(&script_string, port_binary_dir);
  ic_add_string(&script_string, script_name);

  /* Create pid string */
  pid_number_str= ic_guint64_str((guint64)pid, pid_buf, NULL);

  /* Create unique file number string for this pid */
  ic_mutex_lock_low(exec_output_mutex);
  loc_file_number= file_number++;
  ic_mutex_unlock_low(exec_output_mutex);
  file_number_str= ic_guint64_str(loc_file_number, file_number_buf, NULL);

  /* Create log file name as $ICLAUSTRON_DIR/tmp_ps_#PID_#FILE_NUMBER.txt */
  IC_INIT_STRING(&log_file_name_str, log_file_name_buf, 0, TRUE);
  ic_add_ic_string(&log_file_name_str, &ic_glob_config_dir);
  ic_add_string(&log_file_name_str, ic_tmp_str);
  ic_add_string(&log_file_name_str, ic_underscore_str);
  ic_add_string(&log_file_name_str, ic_ps_str);
  ic_add_string(&log_file_name_str, pid_number_str);
  ic_add_string(&log_file_name_str, ic_underscore_str);
  ic_add_string(&log_file_name_str, file_number_str);
  ic_add_string(&log_file_name_str, ".txt");

  argv[0]= script_string.str;
  argv[1]= "--process_name";
  argv[2]= (gchar*)process_name;
  argv[3]= "--pid";
  argv[4]= pid_number_str;
  argv[5]= "--log_file";
  argv[6]= log_file_name_str.str;
  argv[7]= NULL;

  error= run_process(argv, &exit_status, log_file_name_str.str);
  if (error || exit_status == (gint)2)
  {
    DEBUG_RETURN_INT(IC_ERROR_CHECK_PROCESS_SCRIPT);
  }
  if (exit_status == (gint)1)
  {
    DEBUG_RETURN_INT(0); /* The process was alive */
  }
  DEBUG_RETURN_INT(IC_ERROR_PROCESS_NOT_ALIVE);
}

int
ic_delete_file(const gchar *file_name)
{
  int error;
  DEBUG_ENTRY("ic_delete_file");
  DEBUG_PRINT(FILE_LEVEL, ("Delete file %s", file_name));

  error= g_unlink(file_name);
  DEBUG_RETURN_INT(error);
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

int
ic_mkdir(const gchar *dir_name)
{
  int ret_code;
  DEBUG_ENTRY("mkdir");
  DEBUG_PRINT(FILE_LEVEL, ("dir_name = %s", dir_name));

  if ((ret_code= mkdir(dir_name, 448)) == (int)-1)
  {
    ret_code= errno;
    if (ret_code == EEXIST)
      DEBUG_RETURN_INT(0);
    DEBUG_RETURN_INT(ret_code);
  }
  DEBUG_RETURN_INT(0);
}
#endif

int
ic_open_file(IC_FILE_HANDLE *handle,
             const gchar *file_name,
             gboolean create_flag)
{
  int error;
  DEBUG_ENTRY("ic_open_file");

  error= portable_open_file(handle, file_name, create_flag, TRUE);
  DEBUG_PRINT(FILE_LEVEL, ("Open file %s with fd = %d", file_name, *handle));
  DEBUG_RETURN_INT(error);
}

int
ic_create_file(IC_FILE_HANDLE *handle,
               const gchar *file_name)
{
  int error;
  DEBUG_ENTRY("ic_create_file");

  error= portable_open_file(handle, file_name, TRUE, FALSE);
  DEBUG_PRINT(FILE_LEVEL, ("Create file %s with fd = %d", file_name, *handle));
  DEBUG_RETURN_INT(error);
}

int
ic_close_file(IC_FILE_HANDLE file_ptr)
{
  int error;
  DEBUG_ENTRY("ic_close_file");

#ifndef WINDOWS
  if ((close(file_ptr)) == (int)-1)
    error= errno;
  else
    error= 0;
#else
  if (!CloseHandle(file_ptr))
    error= GetLastError();
  else
    error= 0;
#endif
  DEBUG_RETURN_INT(error);
}

static int
get_file_length(IC_FILE_HANDLE file_ptr, guint64 *read_size)
{
  int error= 0;
  gint64 size;
  DEBUG_ENTRY("get_file_length");
#ifndef WINDOWS

  size= lseek(file_ptr, (off_t)0, SEEK_END);
  if (size == (gint64)-1)
  {
    error= errno;
    goto end;
  }
  *read_size= size;
  size= lseek(file_ptr, (off_t)0, SEEK_SET);
  if (size == (gint64)-1)
  {
    error= errno;
    goto end;
  }
  error= 0;
  if (size != 0)
    error= 1;
  goto end;
#else
  (void)size;
  if (!GetFileSizeEx(file_ptr, (LARGE_INTEGER*)read_size))
  {
    error= GetLastError();
  }
  goto end;
#endif
end:
  DEBUG_RETURN_INT(error);
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
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
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
      DEBUG_RETURN_INT(0);
    }
    loc_ptr+= read_size;
    size_left-= (size_t)read_size;
  } while (1);
get_error:
  error= ic_get_last_error();
error:
  DEBUG_RETURN_INT(error);
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
  int ret_code= 0;
  size_t ret_size= 0;
  DEBUG_ENTRY("ic_write_file");
  DEBUG_PRINT(FILE_LEVEL, ("Write file fd = %d, size = %d", (int)file_ptr,
                           (int)size));

  do
  {
    if ((ret_code= portable_write(file_ptr,
                                  (const void*)buf,
                                  size,
                                  &ret_size)))
      goto end;
    if (ret_size == size)
      goto end;
    buf+= ret_size;
    size-= ret_size;
  } while (1);
end:
  DEBUG_RETURN_INT(ret_code);
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
  DEBUG_ENTRY("ic_read_file");
  DEBUG_PRINT(FILE_LEVEL, ("Read file fd = %d, size = %d", (int)file_ptr,
                           (int)size));

  if ((ret_code= portable_read(file_ptr, (void*)buf, size, &ret_size)))
    DEBUG_RETURN_INT(ret_code);

  *len= (guint32)ret_size;
  DEBUG_PRINT(FILE_LEVEL, ("Read = %d", (int)*len));
  DEBUG_RETURN_INT(0);
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
#ifdef DEBUG_BUILD
      abort();
#endif
      break;
    default:
      DEBUG_RETURN_EMPTY;
  }
  ic_set_stop_flag();
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
  ic_set_stop_flag();
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

#ifdef DEBUG_BUILD
static void debug_lock_mutex(IC_MUTEX *mutex)
{
  int ret_code;
  void *key;

  DEBUG_DISABLE(0);
  g_mutex_lock(mutex_hash_protect);
  key= ic_hashtable_search(mutex_hash, (void*)mutex);
  ret_code= ic_hashtable_insert(mutex_hash,
                                (void*)mutex,
                                (void*)mutex);
  g_mutex_unlock(mutex_hash_protect);
  DEBUG_ENABLE(0);
  if (key || ret_code)
    abort();
}

static void debug_release_mutex(IC_MUTEX *mutex)
{
  void *key;

  DEBUG_DISABLE(0);
  g_mutex_lock(mutex_hash_protect);
  key= ic_hashtable_remove(mutex_hash, (void*)mutex);
  g_mutex_unlock(mutex_hash_protect);
  DEBUG_ENABLE(0);
  if (!key)
    abort();
}
#endif

void ic_cond_signal(IC_COND *cond)
{
  g_cond_signal(cond);
}

void ic_cond_broadcast(IC_COND *cond)
{
  g_cond_broadcast(cond);
}

void ic_cond_wait(IC_COND *cond, IC_MUTEX *mutex)
{
#ifdef DEBUG_BUILD
  debug_release_mutex(mutex);
#endif
  g_cond_wait(cond, mutex);
#ifdef DEBUG_BUILD
  debug_lock_mutex(mutex);
#endif
}

void ic_cond_timed_wait(IC_COND *cond,
                        IC_MUTEX *mutex,
                        guint32 micros)
{
  GTimeVal stop_timer;

  g_get_current_time(&stop_timer);
  g_time_val_add(&stop_timer, micros);
#ifdef DEBUG_BUILD
  debug_release_mutex(mutex);
#endif
  g_cond_timed_wait(cond, mutex, &stop_timer);
#ifdef DEBUG_BUILD
  debug_lock_mutex(mutex);
#endif
}

IC_COND* ic_cond_create()
{
  return g_cond_new();
}

void ic_cond_destroy(IC_COND **cond)
{
  g_cond_free(*cond);
  *cond= NULL;
}

void ic_mutex_lock(IC_MUTEX *mutex)
{
  g_mutex_lock(mutex);
#ifdef DEBUG_BUILD
  debug_lock_mutex(mutex);
#endif
}

void ic_mutex_lock_low(IC_MUTEX *mutex)
{
  g_mutex_lock(mutex);
}

void ic_mutex_unlock(IC_MUTEX *mutex)
{
#ifdef DEBUG_BUILD
  debug_release_mutex(mutex);
#endif
  g_mutex_unlock(mutex);
}

void ic_mutex_unlock_low(IC_MUTEX *mutex)
{
  g_mutex_unlock(mutex);
}

IC_MUTEX* ic_mutex_create()
{
  IC_MUTEX *mutex;
  mutex= g_mutex_new();
  return mutex;
}

void ic_mutex_destroy(IC_MUTEX **mutex)
{
  g_mutex_free(*mutex);
  *mutex= NULL;
}

void ic_spin_lock(IC_SPINLOCK *spinlock)
{
  g_mutex_lock(spinlock);
}

void ic_spin_unlock(IC_SPINLOCK *spinlock)
{
  g_mutex_unlock(spinlock);
}

IC_SPINLOCK* ic_spin_create()
{
  return g_mutex_new();
}

void ic_spin_destroy(IC_SPINLOCK *spinlock)
{
  g_mutex_free(spinlock);
}

/**
  Get HW information into a file

  @parameter   type            IN: What type of HW info is requested
                                   (CPU/Mem/Disk)
  @parameter   disk_handle_ptr IN: File which to check disk space for
  @parameter   out_file_ptr    OUT: File pointer of output information

  NOTE:
    The caller of the function is expected to delete the output file as soon
    as the information have been processed in it. He's also expected to free
    the memory allocated for the file name of the output file. This is only
    necessary at successful return from this method.

  NOTE:
    type can have the values IC_GET_CPU_INFO, IC_GET_MEM_INFO,
    IC_GET_DISK_INFO

  NOTE:
    disk_handle_ptr should only be non-NULL for IC_GET_DISK_INFO

  NOTE:
    The output file format for IC_GET_CPU_INFO is:
    Number of CPU processors: 6
    Number of CPU sockets: 1
    Number of CPU cores: 6
    Number of NUMA nodes: 1
    CPU processor id = 0, core id = 0, numa node id = 0, cpu id = 0
    CPU processor id = 1, core id = 1, numa node id = 0, cpu id = 0
    CPU processor id = 2, core id = 2, numa node id = 0, cpu id = 0
    CPU processor id = 3, core id = 3, numa node id = 0, cpu id = 0
    CPU processor id = 4, core id = 4, numa node id = 0, cpu id = 0
    CPU processor id = 5, core id = 5, numa node id = 0, cpu id = 0

    This is from an example with a single socket box with a 6-core CPU.

  NOTE:
    The output file format for IC_GET_MEM_INFO is:
    Number of NUMA nodes: 1
    Memory size = 6127 MBytes
    Numa node id = 0, Memory size = 6127 MBytes

    This is from an example with 1 Numa node on a machine with 6GB of memory.

  NOTE:
    The output file format for IC_GET_DISK_INFO is:
    Disk space = 393723 MBytes

    This is an example with 400 GB disk space available.
*/
int
ic_get_hw_info(IC_HW_INFO_TYPE type,
               gchar *disk_handle_str,
               gchar **out_file_str)
{
  gchar *argv[10];
  gint exit_status;
  gchar *pid_number_str;
  gchar *file_number_str;
  IC_STRING script_string;
  IC_STRING log_file_name_str;
  IC_STRING tmp_file_name_str;
  IC_STRING out_file_name_str;
  IC_PID_TYPE pid;
  IC_STRING out_str;
  gchar *script_name;
  int ret_code;
  guint64 loc_file_number;
  guint32 last_inx;
  gchar pid_buf[IC_MAX_INT_STRING];
  gchar file_number_buf[IC_MAX_INT_STRING];
  gchar full_script_name[IC_MAX_FILE_NAME_SIZE];
  gchar log_file_name_buf[IC_MAX_FILE_NAME_SIZE];
  gchar tmp_file_name_buf[IC_MAX_FILE_NAME_SIZE];
  gchar out_file_name_buf[IC_MAX_FILE_NAME_SIZE];
  DEBUG_ENTRY("ic_get_hw_info");

#ifdef LINUX
  script_name= "linux_get_hw_info.sh";
#endif
#ifdef MACOSX
  script_name= "macosx_get_hw_info.sh";
#endif
#ifdef SOLARIS
  script_name= "solaris_get_hw_info.sh";
#endif
#ifdef FREEBSD
  script_name= "free_bsd_get_hw_info.sh";
#endif
#ifdef WINDOWS
  script_name= "windows_check_process.sh";
#endif
  /* Create script string with full path*/
  IC_INIT_STRING(&script_string, full_script_name, 0, TRUE);
  ic_add_string(&script_string, port_binary_dir);
  ic_add_string(&script_string, script_name);

  pid= ic_get_own_pid();
  pid_number_str= ic_guint64_str((guint64)pid, pid_buf, NULL);

  ic_mutex_lock_low(exec_output_mutex);
  loc_file_number= file_number++;
  ic_mutex_unlock_low(exec_output_mutex);
  file_number_str= ic_guint64_str(loc_file_number, file_number_buf, NULL);

  /* Create log file name as tmp_hw_log#PID_#FILE_NUMBER.txt */
  IC_INIT_STRING(&log_file_name_str, log_file_name_buf, 0, TRUE);
  ic_add_ic_string(&log_file_name_str, &ic_glob_config_dir);
  ic_add_string(&log_file_name_str, ic_tmp_str);
  ic_add_string(&log_file_name_str, ic_underscore_str);
  ic_add_string(&log_file_name_str, ic_hw_str);
  ic_add_string(&log_file_name_str, ic_underscore_str);
  ic_add_string(&log_file_name_str, ic_log_str);
  ic_add_string(&log_file_name_str, pid_number_str);
  ic_add_string(&log_file_name_str, ic_underscore_str);
  ic_add_string(&log_file_name_str, file_number_str);
  ic_add_string(&log_file_name_str, ".txt");

  /* Create tmp file name as tmp_hw#PID_#FILE_NUMBER.txt */
  IC_INIT_STRING(&tmp_file_name_str, tmp_file_name_buf, 0, TRUE);
  ic_add_ic_string(&tmp_file_name_str, &ic_glob_config_dir);
  ic_add_string(&tmp_file_name_str, ic_tmp_str);
  ic_add_string(&tmp_file_name_str, ic_underscore_str);
  ic_add_string(&tmp_file_name_str, ic_hw_str);
  ic_add_string(&tmp_file_name_str, pid_number_str);
  ic_add_string(&tmp_file_name_str, ic_underscore_str);
  ic_add_string(&tmp_file_name_str, file_number_str);
  ic_add_string(&tmp_file_name_str, ".txt");

  /* Create tmp file name as out_hw#PID_#FILE_NUMBER.txt */
  IC_INIT_STRING(&out_file_name_str, out_file_name_buf, 0, TRUE);
  ic_add_ic_string(&out_file_name_str, &ic_glob_config_dir);
  ic_add_string(&out_file_name_str, ic_out_str);
  ic_add_string(&out_file_name_str, ic_underscore_str);
  ic_add_string(&out_file_name_str, ic_hw_str);
  ic_add_string(&out_file_name_str, pid_number_str);
  ic_add_string(&out_file_name_str, ic_underscore_str);
  ic_add_string(&out_file_name_str, file_number_str);
  ic_add_string(&out_file_name_str, ".txt");

  argv[0]= script_string.str;
  argv[1]= "--tmp_file";
  argv[2]= tmp_file_name_str.str;
  argv[3]= "--output_file";
  argv[4]= out_file_name_str.str;
  argv[5]= "--log_file";
  argv[6]= log_file_name_str.str;
  if (type == IC_GET_CPU_INFO)
  {
    argv[7]= "--get_cpu_info";
    last_inx= 8;
  }
  else if (type == IC_GET_MEM_INFO)
  {
    argv[7]= "--get_mem_info";
    last_inx= 8;
  }
  else if (type == IC_GET_DISK_INFO)
  {
    argv[7]= "--get_disk_info";
    argv[8]= disk_handle_str;
    last_inx= 9;
  }
  else
  {
    ic_require(FALSE);
    last_inx= 0; /* To silence compiler */
  }
  argv[last_inx]= NULL;
  ret_code= run_process(argv, &exit_status, log_file_name_str.str);
  if (ret_code)
    goto end;
  if ((ret_code= ic_mc_strdup(NULL, &out_str, &out_file_name_str)))
    goto end;
  *out_file_str= out_str.str;
end:
  DEBUG_RETURN_INT(ret_code);
}

#ifndef HAVE_MEMSET
void ic_memset(gchar *buf, gchar val, int num_bytes)
{
  int i;
  for (i= 0; i < num_bytes; i++)
    buf[i]= val;
}
#endif
