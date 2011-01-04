## Copyright (C) 2007-2011 iClaustron AB
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
## This directory is used to build iClaustron Communication Functions
##

DTRACE= @DTRACE@
DTRACEFLAGS= @DTRACEFLAGS@
DTRACEFILES= socket_conn.o

lib_LTLIBRARIES = libic_comm.la

libic_comm_la_SOURCES = ic_connection.c ic_sock_buf.c ic_protocol_support.c \
                        ic_poll_set.c ic_base64.c

libic_comm_la_LIBADD = $(LDADD) \
				../port/libic_port.la \
				../util/libic_util.la

EXTRA_DIST      = ic_poll_set_int.h

if HAVE_DTRACE
  libic_comm_la_SOURCES += probes.d
endif

probes.h: probes.d
	$(DTRACE) $(DTRACEFLAGS) -h -s probe.d

SUFFIXES : .d

.d.o : $(DTRACEFILES)
	$(DTRACE) $(DTRACEFLAGS) -G -s $< $(DTRACEFILES)
