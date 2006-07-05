#ifndef COMMON_H
#define COMMON_H
#include <glib.h>
#include <stdio.h>

typedef unsigned char ic_bool;
#if !HAVE_BZERO && HAVE_MEMSET
#define bzero(buf, bytes)  ((void) memset(buf, 0, bytes))
#endif
#endif

#ifdef DEBUG_BUILD
#define DEBUG(a) a
#else
#define DEBUG(a)
#endif
