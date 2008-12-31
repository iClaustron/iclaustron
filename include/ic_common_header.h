/* Copyright (C) 2007, 2008 iClaustron AB

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

#ifndef IC_COMMON_HEADER_H
#define IC_COMMON_HEADER_H

#include <glib.h>
#include <glib/gprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <config.h>
#include <fcntl.h>
#include <errno.h>

#include <ic_mc.h>

#define CONFIG_READ_BUF_SIZE 256
#define COMMAND_READ_BUF_SIZE 2048

#define ERROR_MESSAGE_SIZE 512
#define IC_VERSION 0x000001
#define MYSQL_VERSION 0x060401
#define MYSQL_VERSION_STRING "mysql-5.1.30-ndb-6.4.1"

#define IC_VERSION_BIT_START 24
#define IC_PROTOCOL_BIT 20

#define IC_MAX_UINT32 0xFFFFFFFF
#define IC_MAX_CLUSTER_ID 255
#define IC_MAX_NODE_ID 255
#define IC_MAX_THREAD_CONNECTIONS 256
#define IC_MAX_CLUSTER_SERVERS 4
#define IC_MAX_FILE_NAME_SIZE 255
#define IC_MAX_INT_STRING 32

/* Define a number of constants used in various places */
#define SPACE_CHAR (gchar)32
#define CARRIAGE_RETURN (gchar)10
#define LINE_FEED (gchar)13
#define NULL_BYTE (gchar)0

extern gchar *ic_empty_string;
extern gchar *ic_err_str;

typedef unsigned char ic_bool;

typedef guint64 IC_TIMER;
#define UNDEFINED_TIME 0
#define ic_check_defined_time(timer) (timer != UNDEFINED_TIME)
IC_TIMER ic_gethrtime();
IC_TIMER ic_nanos_elapsed(IC_TIMER start_time, IC_TIMER end_time);
IC_TIMER ic_micros_elapsed(IC_TIMER start_time, IC_TIMER end_time);
IC_TIMER ic_millis_elapsed(IC_TIMER start_time, IC_TIMER end_time);

/* SSL initialisation routines */
int ic_ssl_init();
void ic_ssl_end();
/*
  iClaustron interface to memory allocation routines.
*/
gchar *ic_calloc(size_t size);
gchar *ic_malloc(size_t size);
gchar *ic_realloc(gchar *ptr, size_t size);
void ic_free(void *ret_obj);

guint32 ic_get_own_pid();
int ic_is_process_alive(guint32 pid, gchar *process_name, gchar **err_msg);
/* iClaustron file routines */
int ic_write_file(int file_ptr, const gchar *file_name, size_t size);
int ic_read_file(int file_ptr, gchar *file_name, size_t size, guint64 *len);
int ic_delete_file(const gchar *file_name);
int ic_get_file_contents(const gchar *file, gchar **file_content,
                         guint64 *file_size);

/* iClaustron Timer routines */
void ic_sleep(int sleep_ms);

/*
  A couple of useful macros.

  ic_align(a, b)
    a is size of an element, the macro returns an increase of a to be a
    multiple of b and thus it will be aligned to b.

  IC_MIN(a,b)
    Return minimum of a and b

  IC_MAX(a,b)
    Return maximum of a and b

  IC_DIFF(a, b)
    Return difference of a and b == | a - b | in mathematical terms
*/
#define ic_align(a, b) ((((a)+(b-1))/(b))*(b))
#define IC_MIN(a, b) ((a) < (b)) ? (a) : (b)
#define IC_MAX(a, b) ((a) > (b)) ? (a) : (b)
#define IC_DIFF(a,b) (IC_MAX(a,b) - IC_MIN(a,b))

struct ic_malloc;
struct ic_malloc_ops
{
  gchar* (*ic_mem_alloc) (struct ic_malloc *malloc_ptr, size_t size);
  gchar* (*ic_mem_calloc) (struct ic_malloc *malloc_ptr, size_t size);
  void (*ic_free) (struct ic_malloc *malloc_ptr, void *ptr);
  int (*ic_free_allocator) (struct ic_malloc *malloc_ptr);
  /*
    This method is used to connect a thread specific allocator to a
    global allocator, in this manner we can create thread-local
    allocator objects that will get memory from global allocator when
    necessary and return to global allocator when no longer needed.
    The thread-local allocator will use the normal mem_alloc and free
    methods on the global allocator to get and return memory objects.
    When thread-local object is freed there is also a disconnect method
    such that the global allocator isn't possible to free before all
    thread-local allocators connected to it are freed first.
  */
  int (*ic_connect_global_allocator) (struct ic_malloc *malloc_ptr,
                                      struct ic_malloc *glob_malloc_ptr);
  int (*ic_disconnect_global_allocator) (struct ic_malloc *malloc_ptr,
                                         struct ic_malloc *glob_malloc_ptr);
};
typedef struct ic_malloc_ops IC_MALLOC_OPS;

struct ic_malloc
{
  IC_MALLOC_OPS malloc_ops;
  void *malloc_data;
};
typedef struct ic_malloc IC_MALLOC;

/*
  This memory allocator is simply an interface to the normal malloc
  and free. It's necessary to allow use of any memory allocator passed to
  underlying modules that will allocate memory from the IC_MALLOC object
  passed to them and will not care about what object this is.
*/
int
ic_create_std_malloc();

/*
  This simple memory allocator can only be used on a single thread and it
  uses the normal malloc, but records each address it allocates. By so
  doing the release of the memory allocator object can release all memory
  it has allocated. To simplify things this allocator will ignore any
  free call and will instead free everything when the memory allocator is
  freed.
*/
int
ic_create_simple_malloc();

/*
  This memory allocator will only allow allocation of the same size always.
  It keeps track of this allocation and allocates an initial amount
  immediately.
  All accesses to this pool are protected by a mutex since it is expected that
  many thread-local allocators will be on top of this allocator.
*/
int
ic_create_fixed_size_pool_malloc(guint32 fixed_size,
                                 guint32 initial_pool_size);

/*
  The thread-local pool that can connect to the above global fixed size pool.
*/
int
ic_create_thread_fixed_size_pool_malloc(guint32 fixed_size,
                                        guint32 initial_pool_size,
                                        IC_MALLOC *glob_malloc_ptr);

/*
  This function is used by all iClaustron programs to read program parameters
  and issue start text. The function contains a set of standard parameters
  shared by all iClaustron programs.
*/
int ic_start_program(int argc, gchar *argv[], GOptionEntry entries[],
                     gchar *start_text);

/*
  This is a standard string type, it is declared as whether it is
  null_terminated or not.
*/
struct ic_string
{
  gchar *str;
  guint32 len;
  gboolean is_null_terminated;
};
typedef struct ic_string IC_STRING;

/* Macro to quickly initialise an IC_STRING */
#define IC_INIT_STRING(obj, a, b, c) \
  (obj)->str= (a); \
  (obj)->len= (b); \
  (obj)->is_null_terminated= (c);

/* Macro to copy from one IC_STRING to another */
#define IC_COPY_STRING(dest_obj, src_obj) \
  (dest_obj)->str= (src_obj)->str; \
  (dest_obj)->len= (src_obj)->len; \
  (dest_obj)->is_null_terminated= (src_obj)->is_null_terminated;

/*
 * A few functions to set default directory references to iClaustron
 * directories.
*/
int ic_set_base_dir(IC_STRING *base_dir, const gchar *input_base_dir);
int ic_set_data_dir(IC_STRING *base_dir, const gchar *input_base_dir);
void ic_set_relative_dir(IC_STRING *res_str, IC_STRING *dir,
                         gchar *buf, const gchar *dir_name);
int ic_set_config_path(IC_STRING *config_dir, gchar *config_path,
                       gchar *config_path_buf);
void ic_make_iclaustron_version_string(IC_STRING *res_str, gchar *buf);
void ic_make_mysql_version_string(IC_STRING *res_str, gchar *buf);

/*
  Pointers to strings that contain the standard names of configuration
  files and configuration version files.
*/
extern IC_STRING ic_version_file_str;
extern IC_STRING ic_config_string;
extern IC_STRING ic_config_ending_string;
void ic_set_number_ending_string(gchar *buf, guint64 number);
void ic_create_config_file_name(IC_STRING *file_name,
                                gchar *buf,
                                IC_STRING *config_dir,
                                IC_STRING *name,
                                guint32 config_version_number);
void ic_create_config_version_file_name(IC_STRING *file_name,
                                        gchar *buf,
                                        IC_STRING *config_dir);
/*
  ic_get_ic_string
    Convert IC_STRING to normal NULL-terminated string

  ic_add_string
    This function adds input_str to dest_str, input_str is always a normal
    NULL-terminated string. If the input string doesn't exist it returns
    an empty string.

  ic_add_dup_string
    This function will first allocate a new string to contain the destination
    string and the added string, then copy it to the new string and finally
    also release the storage of the old string. Thus it is a prerequisite for
    this function that the string is allocated using normal malloc.

  ic_add_ic_string
    Adds input_str to dest_str

  ic_str_find_first
    This function finds the first occurrence of the searched_char in the
    string. If it doesn't find any occurrence it reports the length of
    the string.

  ic_print_ic_string
    This function prints the IC_STRING to stdout

  ic_cmp_null_term_str
    This function compares a NULL-terminated string with an IC_STRING

  ic_strdup, ic_mc_strdup
    Create a copy of the input IC_STR and allocate memory for the string.
    Assumes someone else allocated memory for new IC_STRING.
    ic_mc_strdup does the same thing but allocates memory from a memory
    container instead.

  ic_conv_config_str_to_int
    Converts an IC_STRING containing a number from a configuration file
    to a number. A configuration file number can contain k, K, m, M, g and G
    to represent 1024, 1024*1024 and 1024*1024*1024.

  ic_convert_file_to_dir
    Converts a filename string to a directory name. Searches for the last
    '\' in the filename ('/' on Windows).
*/
gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr);
void ic_add_string(IC_STRING *dest_str, const gchar *input_str);
int ic_add_dup_string(IC_STRING *dest_str, const gchar *add_str);
void ic_add_ic_string(IC_STRING *dest_str, IC_STRING *input_str);
guint32 ic_str_find_first(IC_STRING *ic_str, gchar searched_char);
void ic_print_ic_string(IC_STRING *str);
int ic_cmp_null_term_str(const char *null_term_str, IC_STRING *cmp_str);
int ic_strdup(IC_STRING *out_str, IC_STRING *in_str);
int ic_mc_strdup(IC_MEMORY_CONTAINER *mc_ptr,
                 IC_STRING *out_str, IC_STRING *in_str);
int ic_conv_config_str_to_int(guint64 *value, IC_STRING *ic_str);
gchar *ic_convert_file_to_dir(gchar *buf, gchar *file_name);

/*
  ic_set_up_ic_string()

  Input:
    str                 String to set up
    len                 Maximum length
    is_null_terminated  Is string null terminated

  Output:
    str                 String that was set-up
    len                 Real length
    is_null_terminated  Normally true, except when no NULL was found in string
*/
void ic_set_up_ic_string(IC_STRING *in_out_str);

/*
  Conversion routines from string to number and vice versa.
*/
gchar *ic_guint64_str(guint64 val, gchar *ptr, guint32 *len);
gchar *ic_guint64_hex_str(guint64 val, gchar *ptr);
int ic_conv_str_to_int(gchar *str, guint64 *number, guint32 *len);

/*
  Error handling interface
*/
#include <ic_err.h>
void ic_init_error_messages();
void ic_print_error(int error_number);
gchar *ic_get_error_message(int error_number);
gchar *ic_common_fill_error_buffer(const gchar *error_message,
                                   guint32 error_line,
                                   int error_code,
                                   gchar *error_buffer);
gchar *ic_get_strerror(int error_number, gchar *buf, guint32 buf_len);
int ic_init();
void ic_end();

#if defined(HAVE_BZERO) && !defined(HAVE_MEMSET)
#define memset(buf, val, bytes)  ((void) bzero(buf, bytes))
#endif
#if !defined(HAVE_BZERO) && defined(HAVE_MEMSET)
#define bzero(buf, bytes)  ((void) memset(buf, 0, bytes))
#endif

void ic_set_debug(guint32 val);
guint32 ic_get_debug();
void ic_debug_print_char_buf(gchar *buf);
void ic_debug_printf(const char *format,...);
void ic_debug_print_rec_buf(gchar *buf, guint32 read_size);

#define COMM_LEVEL 1
#define ENTRY_LEVEL 2
#define CONFIG_LEVEL 3
#define PROGRAM_LEVEL 4
#define THREAD_LEVEL 5

#ifdef DEBUG_BUILD
void ic_debug_entry(const char *entry_point);
int ic_debug_open();
void ic_debug_close();
#define DEBUG_RETURN_EMPTY return
#define DEBUG_RETURN(a) return a
#define DEBUG_DECL(a) a
#define DEBUG_PRINT_BUF(level, buf) \
  { if (ic_get_debug() & level) { ic_debug_print_char_buf(buf); }}
#define DEBUG_PRINT(level, printf_args) \
  { if (ic_get_debug() & level) { ic_debug_printf printf_args; }}
#define DEBUG(level, a) if (ic_get_debug() & level) a
#define DEBUG_ENTRY(a) if (ic_get_debug() & ENTRY_LEVEL) ic_debug_entry(a)
#define DEBUG_OPEN if (ic_debug_open()) return 1
#define DEBUG_CLOSE ic_debug_close()
#define DEBUG_IC_STRING(level, a) \
  if (ic_get_debug() & level) ic_print_ic_string(a)
#else
#define DEBUG_RETURN_EMPTY return
#define DEBUG_RETURN(a) return a
#define DEBUG_DECL(a)
#define DEBUG_PRINT_BUF(level, buf)
#define DEBUG_PRINT(level, printf_args)
#define DEBUG(level, a)
#define DEBUG_ENTRY(a)
#define DEBUG_OPEN
#define DEBUG_CLOSE
#define DEBUG_IC_STRING(level, a)
#endif
#endif
