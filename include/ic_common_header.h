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
  This header file is a convenience header file including all modules
  of the iclaustron code except for the top level modules.
*/

#ifndef IC_COMMON_HEADER_H
#define IC_COMMON_HEADER_H
/*
  Memory Container, String, Hashtable and the Communication
  module are all part of the external interface. Error definitions
  are also an important part of the external interface. We also
  include the debug interface to enable debugging of application
  and API together.
*/
#include <ic_base_header.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_hashtable.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_connection.h>
#include <ic_ssl.h>
#endif
