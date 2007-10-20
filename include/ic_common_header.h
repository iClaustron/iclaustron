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

typedef unsigned char ic_bool;

gchar *ic_calloc(size_t size);
gchar *ic_malloc(size_t size);
void ic_free(void *ret_obj);
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

#ifdef DEBUG_BUILD
void ic_debug_entry(const char *entry_point);
int ic_debug_open();
void ic_debug_close();
#define DEBUG(a) a
#define DEBUG_ENTRY(a) ic_debug_entry(a)
#define DEBUG_OPEN if (ic_debug_open()) return 1
#define DEBUG_CLOSE ic_debug_close()
#define DEBUG_IC_STRING(a) ic_print_ic_string(a)
#else
#define DEBUG(a)
#define DEBUG_ENTRY(a)
#define DEBUG_OPEN
#define DEBUG_CLOSE
#define DEBUG_IC_STRING(a)
#endif
