/* Copyright (C) 2007-2009 iClaustron AB

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

/*
  This header file is included by all iClaustron source code files.
  It includes the glib header files, it contains a set of predefined
  constants about sizes, version numbers and portable definitions of
  various methods. It also includes a number of standard header files
  and declarations of predefined strings and the start/end functions
  of iClaustron.
*/

#ifndef IC_BASE_HEADER_H
#define IC_BASE_HEADER_H

/* Configure definitions are needed also in header files */
#include <config.h>

#ifdef WINDOWS
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib");
#endif

/* GLib header files contains all data type definitions */
#include <glib.h>
#include <glib/gprintf.h>
/* Very basic header files */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ic_linked_list.h>
/*
  Error inject declarations
*/
extern guint32 error_inject;
#ifdef DEBUG_BUILD
#define IC_ERROR_INJECT(a) \
  (error_inject == a)
#else
#define IC_ERROR_INJECT(a) FALSE
#endif

#ifdef _MSC_VER
#define IC_INLINE static __inline
#else
#define IC_INLINE static inline
#endif

#ifdef WINDOWS
#define IC_POLL_FLAG POLLRDNORM
#define IC_POLLFD_STRUCT WSAPOLLFD
#define ic_poll WSAPoll
#define IC_FILE_HANDLE HANDLE
#define IC_SOCKLEN_TYPE int
#define snprintf sprintf_s
#define strncat(a,b,c) strcat_s((a),(c),(b))
#define IC_INVALID_SOCKET INVALID_SOCKET
#define IC_SOCKET_ERROR SOCKET_ERROR
#define IC_VOID_PTR_TYPE char*
#else
#define IC_POLL_FLAG POLLIN
#define IC_POLLFD_STRUCT struct pollfd
#define ic_poll poll
#define IC_FILE_HANDLE int
#define IC_SOCKLEN_TYPE socklen_t
#define IC_INVALID_SOCKET (int)-1
#define IC_SOCKET_ERROR (int)-1
#define IC_VOID_PTR_TYPE void*
#endif

typedef struct ic_bitmap IC_BITMAP;
typedef struct ic_connection IC_CONNECTION;
typedef struct ic_dynamic_array IC_DYNAMIC_ARRAY;
typedef struct ic_hashtable IC_HASHTABLE;
typedef struct ic_memory_container IC_MEMORY_CONTAINER;
typedef struct ic_poll_set IC_POLL_SET;
typedef struct ic_sock_buf_page IC_SOCK_BUF_PAGE;
typedef struct ic_string IC_STRING;
typedef struct ic_threadpool_state IC_THREADPOOL_STATE;

typedef guint64 IC_PID_TYPE;
typedef guint64 IC_CONF_VERSION_TYPE;
typedef guint64 IC_CONF_STATE_TYPE;

struct ic_iovec
{
  gchar *iov_base;
  guint32 iov_len;
};

typedef struct ic_iovec IC_IOVEC;

#define CONFIG_READ_BUF_SIZE 256
#define IC_MAX_ERROR_STRING_SIZE 256
#define COMMAND_READ_BUF_SIZE 2048

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 64*1024
#endif

#define IC_DEF_CLUSTER_SERVER_PORT 1186
#define IC_DEF_CLUSTER_SERVER_PORT_STR "1186"
#define IC_DEF_PORT 1187
#define IC_DEF_PCNTRL_PORT 11860
#define IC_DEF_PCNTRL_PORT_STR "11860"
#define IC_DEF_CLUSTER_MANAGER_PORT 11861
#define IC_DEF_CLUSTER_MANAGER_PORT_STR "11861"

/*
  Define stack sizes of our thread implementation, most of them are
  really lightweight and 64 kBytes should be sufficient for our
  means. 0 means default size which usually is around 1 MByte.
*/
#define IC_SMALL_STACK_SIZE (64 * 1024 + PTHREAD_STACK_MIN)
#define IC_MEDIUM_STACK_SIZE (256 * 1024 + PTHREAD_STACK_MIN)
#define IC_NORMAL_STACK_SIZE (0)

#define IC_STD_CACHE_LINE_SIZE 128

#define IC_NDB_MESSAGE_HEADER_SIZE 3
#define IC_NDB_MAX_PRIO_LEVEL 1
#define IC_NDB_MAX_MAIN_MESSAGE_SIZE 25
#define IC_NDB_MAX_MODULE_ID 4096
#define IC_NDB_MIN_MODULE_ID_FOR_THREADS 32768
#define IC_NDB_PACKED_MODULE_ID 2047
#define IC_MAX_MODULE_ID 8192
#define IC_PREALLOC_NUM_MESSAGES (guint32)8
#define IC_WAIT_SEND_BUF_POOL (guint32)3000

#define IC_NDB_NORMAL_PRIO 0
#define IC_NDB_HIGH_PRIO 1

#define IC_NDB_QMGR_MODULE 252
#define IC_NDB_TC_MODULE 245
#define IC_NDB_DICT_MODULE 250
#define IC_NDB_CMVMI_MODULE 254
#define IC_NDB_SUMA_MODULE 257
#define IC_NDB_INFO_MODULE 263

#define IC_MAX_THREAD_WAIT_TIME 60

#define IC_MICROSEC_PER_SECOND 1000000
#define IC_MICROSEC_PER_MILLI 1000

#define ERROR_MESSAGE_SIZE 512
#define IC_VERSION 0x000001
#define IC_VERSION_STR "iclaustron-0.0.1"
#define NDB_VERSION 0x070100
#define MYSQL_VERSION 0x050135
#define MYSQL_VERSION_STRING "mysql-5.1.35-ndb-7.1.0"

#define IC_VERSION_BIT_START 24
#define IC_PROTOCOL_BIT 20

#define IC_MAX_UINT32 0xFFFFFFFF
#define IC_MAX_CLUSTER_ID 255
#define IC_MAX_NODE_ID 255
#define IC_MAX_THREAD_CONNECTIONS 256
#define IC_MAX_CLUSTER_SERVERS 4
#define IC_MAX_FILE_NAME_SIZE 255
#define IC_MAX_INT_STRING 32
#define IC_MAX_APID_NUM_THREADS 256

/* Define a number of constants used in various places */
#define SPACE_CHAR (gchar)32
#define CARRIAGE_RETURN (gchar)10
#define LINE_FEED (gchar)13
#define NULL_BYTE (gchar)0

#if defined(HAVE_MEMSET)
#define bzero(buf, bytes) ((void) memset(buf, 0, bytes))
#define ic_zero(buf, bytes) ((void) memset(buf, 0, bytes))
#else
#if defined(HAVE_BZERO) && !defined(HAVE_MEMSET)
#define ic_zero(buf, num_bytes) ((void) bzero(buf, num_bytes))
#define memset(buf, val, bytes)  ((void) bzero(buf, bytes))
#endif
#endif

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
  HEADER MODULE: iClaustron Initialise and End Functions
  ------------------------------------------------------
  This function is used by all iClaustron programs to read program parameters
  and issue start text. The function contains a set of standard parameters
  shared by all iClaustron programs. ic_end is used at the end of each
  iClaustron program.
*/
int ic_start_program(int argc, gchar *argv[], GOptionEntry entries[],
                     GOptionEntry add_entries[],
                     const gchar *program_name,
                     gchar *start_text, gboolean use_config);
void ic_end();
#endif
