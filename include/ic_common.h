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
  This header file is used internally in all iClaustron source files
  as a lazy manner of getting all needed things included from one
  header file. It isn't designed for use by external users of the
  iClaustron API's since it also includes header files not part of
  the iClaustron installation.
*/

#ifndef IC_COMMON_H
#define IC_COMMON_H
#include <ic_common_header.h>
/*
  The following are header files which are not required in the
  external applications. Those header files are however available
  for use externally if the application so desires. They are part
  of the installation.
*/
#include <ic_port.h>
#include <ic_bitmap.h>
#include <ic_dyn_array.h>
#include <ic_poll_set.h>
#include <ic_threadpool.h>

/* The following header files are header files only used internally
   and we don't install them for applications to reuse. They can
   only be reused from their open source licensing scheme.
*/
#include <ic_sock_buf.h>
#include <ic_base64.h>
#include <ic_config_reader.h>

/*
  The following header are common C header files we often require.
  So we include them in all the iClaustron source files.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#endif
