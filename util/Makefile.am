## Copyright (C) 2007-2013 iClaustron AB
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; version 2 of the License.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, write to the Free Software
## Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

## This file is used to produce the Makefile.in for this directory
## This directory is used to build iClaustron Utility Functions
##

lib_LTLIBRARIES = libic_util.la

libic_util_la_SOURCES = ic_util.c ic_hashtable.c ic_hashtable_itr.c \
                        ic_dyn_array.c ic_mc.c ic_bitmap.c ic_debug.c \
			ic_err.c ic_string.c ic_config_reader.c \
			ic_threadpool.c ic_parse_connectstring.c \
			ic_hw_info.c \
			ic_lex_support.c ic_readline.c

libic_util_la_LIBADD = $(LDADD) \
			../port/libic_port.la

EXTRA_DIST      = ic_dyn_array_int.h ic_mc_int.h ic_threadpool_int.h
