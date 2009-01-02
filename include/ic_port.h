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
/*
  HEADER MODULE: iClaustron Portability Layer
  ------------------------------------
  iClaustron interface to memory allocation routines and various other
  portability interfaces.
*/
gchar *ic_calloc(size_t size);
gchar *ic_malloc(size_t size);
gchar *ic_realloc(gchar *ptr, size_t size);
void ic_free(void *ret_obj);

guint32 ic_get_own_pid();
int ic_is_process_alive(guint32 pid, gchar *process_name);
/* iClaustron file routines */
int ic_write_file(int file_ptr, const gchar *file_name, size_t size);
int ic_read_file(int file_ptr, gchar *file_name, size_t size, guint64 *len);
int ic_delete_file(const gchar *file_name);
int ic_get_file_contents(const gchar *file, gchar **file_content,
                         guint64 *file_size);

typedef unsigned char ic_bool;
typedef guint64 IC_TIMER;
#define UNDEFINED_TIME 0
#define ic_check_defined_time(timer) (timer != UNDEFINED_TIME)
IC_TIMER ic_gethrtime();
IC_TIMER ic_nanos_elapsed(IC_TIMER start_time, IC_TIMER end_time);
IC_TIMER ic_micros_elapsed(IC_TIMER start_time, IC_TIMER end_time);
IC_TIMER ic_millis_elapsed(IC_TIMER start_time, IC_TIMER end_time);
#endif
