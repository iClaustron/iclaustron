/* Copyright (C) 2007, 2015 iClaustron AB

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

#ifndef PORT_H
#define PORT_H

#ifdef WINDOWS
typedef int (WSAAPI* IC_POLL_FUNCTION)(
   WSAPOLLFD *fds,
   ULONG num_fds,
   INT time_out);

IC_POLL_FUNCTION ic_poll;
#endif

/* Portable method to close a socket */
void ic_close_socket(int sockfd);

/* Start/stop socket library (needed on Windows) */
int ic_start_socket_system();
int ic_stop_socket_system();

/* Get stop flag (set by SIGTERM and similar signals) */
guint32 ic_get_stop_flag();
/* Set stop flag */
void ic_set_stop_flag();

/* Set reference to binary directory */
void ic_set_port_binary_dir(const gchar *binary_dir);

/* Set reference to config directory */
void ic_set_port_config_dir(const gchar *config_dir);

/*
  HEADER MODULE: iClaustron Portability Layer
  ------------------------------------
  iClaustron interface to memory allocation routines and various other
  portability interfaces. There is different allocation and free routines
  per module to make it easier to find and root out memory leaks.
*/
void ic_port_init(); /* Debug support and global initialisations */
void ic_port_end();
gchar *ic_calloc_low(size_t size);
gchar *ic_malloc_low(size_t size);
void ic_malloc_report(gchar *ptr, size_t size);
void ic_free_low(void *ret_obj);
gchar *ic_realloc(gchar *ptr, size_t size);
#ifdef DEBUG_BUILD
gchar *ic_calloc(size_t size);
gchar *ic_calloc_conn(size_t size);
gchar *ic_malloc_conn(size_t size);
gchar *ic_calloc_mc(size_t size);
gchar *ic_malloc(size_t size);
gchar *ic_malloc_hash(size_t size, gboolean is_create);
void ic_free(void *ret_obj);
void ic_free_conn(void *ret_obj);
void ic_free_hash(void *ret_obj, gboolean is_destroy);
void ic_free_mc(void *ret_obj);
#else
#define ic_calloc(a) ic_calloc_low(a)
#define ic_calloc_conn(a) ic_calloc_low(a)
#define ic_malloc_conn(a) ic_malloc_low(a)
#define ic_calloc_mc(a) ic_calloc_low(a)
#define ic_malloc(a) ic_malloc_low(a)
#define ic_malloc_hash(a, b) ic_malloc_low(a)
#define ic_free(a) ic_free_low(a)
#define ic_free_conn(a) ic_free_low(a)
#define ic_free_hash(a, b) ic_free_low(a)
#define ic_free_mc(a) ic_free_low(a)
#endif

/* Process start/stop/check calls */
IC_PID_TYPE ic_get_own_pid();
int ic_is_process_alive(IC_PID_TYPE pid,
                        const gchar *process_name,
                        gboolean is_loop_check);
int ic_start_process(gchar **argv,
                     gchar *binary_dir,
                     gchar *pid_file,
                     gboolean is_daemon,
                     IC_PID_TYPE *pid);
void ic_kill_process(IC_PID_TYPE pid, gboolean hard_kill);

/* iClaustron file routines */
int ic_open_file(IC_FILE_HANDLE *handle,
                 const gchar *file_name,
                 gboolean create_flag);
int ic_create_file(IC_FILE_HANDLE *handle,
                   const gchar *buf);
int ic_mkdir(const gchar *dir_name);
int ic_close_file(IC_FILE_HANDLE file_ptr);
int ic_write_file(IC_FILE_HANDLE file_ptr,
                  const gchar *buf,
                  size_t buf_size);
int ic_read_file(IC_FILE_HANDLE file_ptr,
                 gchar *buf,
                 size_t size,
                 guint64 *len);
int ic_delete_file(const gchar *file_name);
void ic_delete_daemon_file(const gchar *pid_file);
int ic_get_file_contents(const gchar *file,
                         gchar **file_content,
                         guint64 *file_size);

/* Error routines */
int ic_get_last_error();
int ic_get_last_socket_error();
gchar *ic_get_strerror(int error_number, gchar *buf, guint32 buf_len);

typedef unsigned char ic_bool;
typedef guint64 IC_TIMER;
#define UNDEFINED_TIME 0
#define ic_check_defined_time(timer) (timer != UNDEFINED_TIME)
IC_TIMER ic_gethrtime();
IC_TIMER ic_nanos_elapsed(IC_TIMER start_time, IC_TIMER end_time);
IC_TIMER ic_micros_elapsed(IC_TIMER start_time, IC_TIMER end_time);
IC_TIMER ic_millis_elapsed(IC_TIMER start_time, IC_TIMER end_time);

void ic_sleep_low(guint32 seconds_to_sleep);
void ic_microsleep(guint32 microseconds_to_sleep);

/**
 * Interfaces to daemonize a program
 * ---------------------------------
 * Since daemonization happens very early we have to divide it into
 * multiple functions. We have to create the new process before we
 * initialise the iClaustron subsystems to ensure we use the right
 * PID for creating the debug file and also to ensure that we haven't
 * opened the debug system before daemonization.
 * 
 * After initializing the iClaustron subsystem we need to setup the
 * working directory and the stdout and stderr files to write output to.
 * 
 * We will set the umask of the process also when not running as daemon.
 * Writing of the pid file happens after daemonization AND after initing
 * the iClaustron subsystems.
 */
int ic_daemonize(void);
int ic_setup_workdir(gchar *new_work_dir);
/* Interface to enable generation of core files */
void ic_generate_core_files(void);
/* Interface to set umask of iClaustron processes */
void ic_set_umask(void);
/* Interface to write pid file and setup things to delete it */
int ic_write_pid_file(const gchar *pid_file);
int ic_read_pid_file(const gchar *pid_file, IC_PID_TYPE *pid);

/* Interface to set function to call at kill signal */
typedef void (*IC_SIG_HANDLER_FUNC)(void *param);
void ic_set_die_handler(IC_SIG_HANDLER_FUNC die_handler, void *param);
void ic_set_sig_error_handler(IC_SIG_HANDLER_FUNC error_handler, void *param);

/*
  Interface to kill ourselves in a controlled manner by sending terminate
  signal to ourselves to start termination logic.
*/
void ic_controlled_terminate();

/*
  Conversion routines from little endian to big endian and vice versa. Also a
  routine to calculate the value of the byte order bit used in the NDB
  Protocol.
*/
#define ic_swap_endian_word(input_word) \
{ \
  guint32 ic_loc_word= (input_word); \
  gchar *result_char= (gchar*)&(input_word); \
  gchar *input_char= (gchar*)&ic_loc_word; \
  result_char[0]= input_char[3]; \
  result_char[1]= input_char[2]; \
  result_char[2]= input_char[1]; \
  result_char[3]= input_char[0]; \
}
guint32 ic_byte_order();

#define IC_MUTEX GMutex
#define IC_COND GCond
#define IC_SPINLOCK GMutex

void ic_cond_signal(IC_COND *cond);
void ic_cond_broadcast(IC_COND *cond);
void ic_cond_wait(IC_COND *cond, IC_MUTEX *mutex);
void ic_cond_timed_wait(IC_COND *cond, IC_MUTEX *mutex, guint32 micros);
IC_COND* ic_cond_create();
void ic_cond_destroy(IC_COND **cond);

void ic_mutex_lock(IC_MUTEX *mutex);
void ic_mutex_lock_low(IC_MUTEX *mutex);
void ic_mutex_unlock(IC_MUTEX *mutex);
void ic_mutex_unlock_low(IC_MUTEX *mutex);
IC_MUTEX* ic_mutex_create();
void ic_mutex_destroy(IC_MUTEX **mutex);

void ic_spin_lock(IC_SPINLOCK *spinlock);
void ic_spin_unlock(IC_SPINLOCK *spinlock);
IC_SPINLOCK* ic_spinlock_create();
void ic_spinlock_destroy();

/* Interface to read HW information into a file */
typedef enum ic_hw_info_type IC_HW_INFO_TYPE;
enum ic_hw_info_type
{
  IC_GET_CPU_INFO= 0,
  IC_GET_MEM_INFO= 1,
  IC_GET_DISK_INFO= 2
};

int ic_get_hw_info(IC_HW_INFO_TYPE type,
                   gchar *disk_handle_ptr,
                   gchar **out_file_str);

#ifndef HAVE_MEMSET
void ic_memset(gchar *buf, gchar val, int num_bytes);
#endif
#endif
