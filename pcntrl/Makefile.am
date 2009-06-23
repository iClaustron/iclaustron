## Copyright (C) 2007-2009 iClaustron AB
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

#
# This is the template to produce Makefile.in for this directory
# This directory is used to build the iClaustron Process Control Server
#

bin_PROGRAMS = ic_pcntrld
ic_pcntrld_SOURCES = ic_pcntrl.c
ic_pcntrld_LDADD = $(LDADD) \
			../port/libic_port.la \
			../util/libic_util.la \
			../comm/libic_comm.la \
			../api_clusterserver/libic_apic.la \
			../api_dataserver/libic_apid.la
