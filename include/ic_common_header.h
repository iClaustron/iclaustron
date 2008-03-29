/* Copyright (C) 2007 iClaustron AB

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

#define IC_VERSION 0x000001
#define MYSQL_VERSION 0x06030c
#define IC_VERSION_BIT_START 24
#define IC_PROTOCOL_BIT 20

#define IC_MAX_UINT32 0xFFFFFFFF
#define IC_MAX_CLUSTER_ID 255
#define IC_MAX_NODE_ID 255
#define IC_MAX_CLUSTER_SERVERS 4
#define IC_MAX_FILE_NAME_SIZE 255
#define IC_MAX_INT_STRING 32

#define ic_is_bit_set(value, bit_number) \
  (((value | (1 << bit_number)) == 0) ? 0 : 1)
#define ic_set_bit(value, bit_number) \
  value|= (1 << bit_number);

extern gchar *ic_empty_string;

typedef unsigned char ic_bool;

/* SSL initialisation routines */
int ic_ssl_init();
void ic_ssl_end();
/*
  iClaustron interface to memory allocation routines.
*/
gchar *ic_calloc(size_t size);
gchar *ic_malloc(size_t size);
void ic_free(void *ret_obj);

/*
  iClaustron Timer routines
*/
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

/*
  A helpful support function to handle many memory allocations within one
  container. This allocates a linked list of containers of base size (except
  when allocations are larger than base size. When freeing it will free all
  of those containers.

  Reset means that we deallocate everything one container of base size.
  Thus we're back to the situation after performing the first allocation of
  the container. Reset will also set all bytes to 0 in container.
*/
#define MC_MIN_BASE_SIZE 128
#define MC_DEFAULT_BASE_SIZE 8180
struct ic_memory_container;
struct ic_memory_container_ops
{
  gchar* (*ic_mc_alloc) (struct ic_memory_container *mc_ptr, guint32 size);
  gchar* (*ic_mc_calloc) (struct ic_memory_container *mc_ptr, guint32 size);
  void (*ic_mc_reset) (struct ic_memory_container *mc_ptr);
  void (*ic_mc_free) (struct ic_memory_container *mc_ptr);
};
typedef struct ic_memory_container_ops IC_MEMORY_CONTAINER_OPS;

struct ic_memory_container
{
  IC_MEMORY_CONTAINER_OPS mc_ops;
  gchar *current_buf;
  gchar **buf_array;
  guint64 total_size;
  guint64 max_size;
  guint32 base_size;
  guint32 buf_array_size;
  guint32 current_buf_inx;
  guint32 current_free_len;
  guint32 first_buf_inx;
};
typedef struct ic_memory_container IC_MEMORY_CONTAINER;

IC_MEMORY_CONTAINER*
ic_create_memory_container(guint32 base_size, guint32 max_size);

/*
  This function is used by all iClaustron to read program parameters and
  issue start text. The function contains a set of standard parameters
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
gchar *ic_guint64_str(guint64 val, gchar *ptr);
gchar *ic_guint64_hex_str(guint64 val, gchar *ptr);
int ic_conv_str_to_int(gchar *str, guint64 *number);

/* Bit manipulation routines */
guint32 ic_count_highest_bit(guint32 bit_var);

/*
  Error handling interface
*/
#include <ic_err.h>
void ic_init_error_messages();
void ic_print_error(guint32 error_number);
gchar *ic_get_error_message(guint32 error_number);
int ic_init();
void ic_end();

#if !HAVE_BZERO && HAVE_MEMSET
#define bzero(buf, bytes)  ((void) memset(buf, 0, bytes))
#endif
#endif

void ic_set_debug(guint32 val);
guint32 ic_get_debug();
void ic_debug_printf(const char *format,...);
void ic_debug_print_buf(gchar *buf, guint32 read_size);

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
#define DEBUG_PRINT(level, printf_args)
#define DEBUG(level, a)
#define DEBUG_ENTRY(a)
#define DEBUG_OPEN
#define DEBUG_CLOSE
#define DEBUG_IC_STRING(level, a)
#endif
