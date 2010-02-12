/* Copyright (C) 2007, 2009 iClaustron AB

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
extern int WSAAPI (*ic_poll)(WSAPOLLFD fds[],
                             ULONG  num_fds,
                             INT time_out);
#endif

/* Portable method to close a socket */
void ic_close_socket(int sockfd);

/* Start/stop socket library (needed on Windows) */
int ic_start_socket_system();
int ic_stop_socket_system();

/* Get stop flag (set by SIGTERM and similar signals) */
guint32 ic_get_stop_flag();

/* Set reference to binary directory */
void ic_set_port_binary_dir(const gchar *binary_dir);
/*
  HEADER MODULE: iClaustron Portability Layer
  ------------------------------------
  iClaustron interface to memory allocation routines and various other
  portability interfaces.
*/
void ic_mem_init(); /* Debug support */
void ic_mem_end();  /* Debug support */
gchar *ic_calloc(size_t size);
gchar *ic_malloc(size_t size);
gchar *ic_realloc(gchar *ptr, size_t size);
void ic_free(void *ret_obj);

/* Process start/stop/check calls */
IC_PID_TYPE ic_get_own_pid();
int ic_is_process_alive(IC_PID_TYPE pid, const gchar *process_name);
int ic_start_process(gchar **argv, gchar *working_dir, IC_PID_TYPE *pid);
void ic_kill_process(IC_PID_TYPE pid, gboolean hard_kill);

/* iClaustron file routines */
int ic_open_file(IC_FILE_HANDLE *handle,
                 const gchar *file_name,
                 gboolean create_flag);
int ic_create_file(IC_FILE_HANDLE *handle,
                   const gchar *buf);
int ic_close_file(IC_FILE_HANDLE file_ptr);
int ic_write_file(IC_FILE_HANDLE file_ptr,
                  const gchar *buf,
                  size_t buf_size);
int ic_read_file(IC_FILE_HANDLE file_ptr,
                 gchar *buf,
                 size_t size,
                 guint64 *len);
int ic_delete_file(const gchar *file_name);
int ic_get_file_contents(const gchar *file, gchar **file_content,
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

void ic_sleep(guint32 seconds_to_sleep);
void ic_microsleep(guint32 microseconds_to_sleep);

/* Interface to daemonize a program */
int ic_daemonize(gchar *log_file);
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
  Conversion routines from little endian to big endian and vice versa. Also a routine to
  calculate the value of the byte order bit used in the NDB Protocol.
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
#endif
