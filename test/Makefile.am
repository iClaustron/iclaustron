## Copyright (C) 2007 iClaustron AB
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

##
## This is the template to produce Makefile.in for this directory
## This directory is used to build the iClaustron Test Programs
##

bin_PROGRAMS = test_comm test_unit

test_comm_SOURCES = test_comm.c
test_comm_LDADD = $(LDADD) ../comm/libic_comm.la \
                           ../port/libic_port.la \
			   ../util/libic_util.la \
                           ../api_clusterserver/libic_apic.la

test_unit_SOURCES = test_unit.c
test_unit_LDADD = $(LDADD) ../comm/libic_comm.la \
                           ../port/libic_port.la \
			   ../util/libic_util.la \
                           ../api_clusterserver/libic_apic.la
