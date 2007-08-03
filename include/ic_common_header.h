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

gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr);
void ic_print_ic_string(IC_STRING *str);
int ic_cmp_null_term_str(const char *null_term_str, IC_STRING *cmp_str);

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
