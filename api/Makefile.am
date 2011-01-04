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
## This directory is used to build iClaustron API towards Cluster Server
##

lib_LTLIBRARIES = libic_api.la

libic_api_la_SOURCES = ic_apic_impl.c ic_apid_impl.c

libic_api_la_LIBADD = $(LDADD) \
				../port/libic_port.la \
				../util/libic_util.la \
				../comm/libic_comm.la

EXTRA_DIST =              ic_apic_int.h ic_apid_int.h \
                          ic_apid_impl.h ic_apid_static.h \
                          ic_apid_global.ic ic_apid_common.ic \
                          ic_apid_heartbeat.ic ic_apid_adaptive_send.ic \
                          ic_apid_send_message.ic ic_apid_send_thread.ic \
                          ic_apid_handle_messages.ic ic_apid_exec_messages.ic \
                          ic_apid_rec_thread.ic ic_apid_start.ic \
                          ic_apid_range.ic ic_apid_where.ic \
                          ic_apid_cond_assign.ic ic_apid_op.ic \
                          ic_apid_error.ic ic_apid_conn.ic \
                          ic_apid_table.ic ic_apid_tablespace.ic \
                          ic_apic.ic \
                          ic_apic_conf_param.ic ic_apic_conf_read_transl.ic \
                          ic_apic_proto_supp.ic ic_apic_conf_read_proto.ic \
                          ic_apic_conf_reader.ic ic_apic_conf_writer.ic \
                          ic_apic_grid_conf_reader.ic ic_apic_if.ic \
                          ic_apic_run_cs.ic ic_apic_conf_net_read.ic
