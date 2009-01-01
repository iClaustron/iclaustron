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

#if defined(HAVE_BZERO) && !defined(HAVE_MEMSET)
#define memset(buf, val, bytes)  ((void) bzero(buf, bytes))
#endif
#if !defined(HAVE_BZERO) && defined(HAVE_MEMSET)
#define bzero(buf, bytes)  ((void) memset(buf, 0, bytes))
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
                     gchar *start_text);
void ic_end();
#endif
