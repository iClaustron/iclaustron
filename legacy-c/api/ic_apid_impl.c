/* Copyright (C) 2009, 2016 iClaustron AB

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
  We implement the iClaustron Data API in a set of C-files, but we'll still
  let the compiler work on them all as one module to allow the compiler to
  do optimisations better.
*/

/* Header files used by the iClaustron Data API */
#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_parse_connectstring.h>
#include <ic_bitmap.h>
#include <ic_dyn_array.h>
#include <ic_hashtable.h>
#include <ic_connection.h>
#include <ic_protocol_support.h>
#include <ic_sock_buf.h>
#include <ic_poll_set.h>
#include <ic_threadpool.h>
#include <ic_apic.h>
#include <ic_apid.h>
#include "ic_apid_general_signals.h"
#include "ic_apid_int.h"
#include "ic_apid_impl.h"
#include "ic_apid_dict_signals.h"
#include "ic_apid_key_signals.h"
#include "ic_apid_scan_signals.h"

/* Implementation files of the iClaustron Data API */
/* Static variables and functions */
#include "ic_apid_static.ic"
/* The error object */
#include "ic_apid_error.ic"
/* Common methods */
#include "ic_apid_common.ic"
/* Data API internals */
#include "ic_apid_heartbeat.ic"
#include "ic_apid_adaptive_send.ic"
#include "ic_apid_send_message.ic"
#include "ic_apid_send_thread.ic"
#include "ic_apid_handle_messages.ic"
#include "ic_apid_exec_message.ic"
#include "ic_apid_rec_thread.ic"
#include "ic_apid_start.ic"
/* External interface object implementations */
#include "ic_apid_range.ic"
#include "ic_apid_where.ic"
#include "ic_apid_cond_assign.ic"
#include "ic_apid_op.ic"
#include "ic_apid_conn.ic"
#include "ic_apid_trans.ic"
#include "ic_apid_table.ic"
#include "ic_apid_tablespace.ic"
#include "ic_apid_global.ic"
/* Data API internals */
#include "ic_apid_handle_dict_messages.ic"
#include "ic_apid_handle_message_array.ic"
