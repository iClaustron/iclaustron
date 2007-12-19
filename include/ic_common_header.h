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

#define MAX_NODE_ID 255

extern gchar *ic_empty_string;

typedef unsigned char ic_bool;

gchar *ic_calloc(size_t size);
gchar *ic_malloc(size_t size);
void ic_free(void *ret_obj);

#define MC_MIN_BASE_SIZE 128
#define MC_DEFAULT_BASE_SIZE 8180
#define ic_align(a, b) ((((a)+(b-1))/(b))*(b))
#define IC_MIN(a, b) ((a) < (b)) ? (a) : (b)
#define IC_MAX(a, b) ((a) > (b)) ? (a) : (b)

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
#define IC_INIT_STRING(obj, a, b, c) \
  (obj)->str= (a); \
  (obj)->len= (b); \
  (obj)->is_null_terminated= (c);

#define IC_COPY_STRING(dest_obj, src_obj) \
  (dest_obj)->str= (src_obj)->str; \
  (dest_obj)->len= (src_obj)->len; \
  (dest_obj)->is_null_terminated= (src_obj)->is_null_terminated;

gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr);

void ic_add_string(IC_STRING *dest_str, gchar *input_str);
void ic_add_ic_string(IC_STRING *dest_str, IC_STRING *input_str);

/*
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
void ic_print_ic_string(IC_STRING *str);
int ic_cmp_null_term_str(const char *null_term_str, IC_STRING *cmp_str);
int ic_strdup(IC_STRING *out_str, IC_STRING *in_str);

gchar *ic_guint64_str(guint64 val, gchar *ptr);
gchar *ic_guint64_hex_str(guint64 val, gchar *ptr);
int ic_conv_config_str_to_int(guint64 *value, IC_STRING *ic_str);
int ic_conv_str_to_int(gchar *str, guint64 *number);
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
