#include <glib.h>
#ifdef COMMON_H
#define COMMON_H
#if !HAVE_BZERO && HAVE_MEMSET
#define bzero(buf, bytes)  ((void) memset(buf, 0, bytes))
#endif
#endif
