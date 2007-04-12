#ifndef IC_COMMON_HEADER_H
#define IC_COMMON_HEADER_H

#include <glib.h>
#include <glib/gprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <config.h>

#define MAX_NODE_ID 255

typedef unsigned char ic_bool;

/*
  This is a standard string type, it is declared as whether it is
  null_terminated or not.
*/
struct ic_string
{
  char *str;
  guint32 len;
  gboolean null_terminated;
};
typedef struct ic_string IC_STRING;
#define IC_INIT_STRING(obj, a, b, c) \
  (obj)->str= (a); \
  (obj)->len= (b); \
  (obj)->null_terminated= (c);

int ic_cmp_null_term_str(const char *null_term_str, IC_STRING *cmp_str);
/*
  Error handling interface
*/
#include <ic_err.h>
void ic_init_error_messages();
void ic_print_error(guint32 error_number);
gchar *ic_get_error_message(guint32 error_number);

#if !HAVE_BZERO && HAVE_MEMSET
#define bzero(buf, bytes)  ((void) memset(buf, 0, bytes))
#endif
#endif

#ifdef DEBUG_BUILD
#define DEBUG(a) a
#else
#define DEBUG(a)
#endif
