/* Copyright (C) 2009 iClaustron AB

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


#ifndef IC_APID_HIDDEN_H
#define IC_APID_HIDDEN_H
#include <ic_common.h>
#include <ic_apid.h>

typedef struct ic_message_error_object IC_MESSAGE_ERROR_OBJECT;
/*
  This is part of the external interface for efficiency reasons.
  However this part is defined to not be stable. The stable interface
  is always methods. In many cases those are inline methods and
  for efficiency reason their implementation is visible for the user of
  the API. However no implementation on top of the API should use this
  information.
*/
struct ic_transaction_state
{
  IC_COMMIT_STATE commit_state;
  IC_SAVEPOINT_ID savepoint_stable;
  IC_SAVEPOINT_ID savepoint_requested;
};

struct ic_message_error_object
{
  int error;
  gchar *error_string;
  int error_category;
  int error_severity;
};

struct ic_table_def
{
  guint32 table_id;
  guint32 index_id;
  gboolean use_index;
};

struct ic_instruction
{
  IC_INSTRUCTION_TYPE instr_type;
  guint32 source_register1;
  guint32 source_register2;
  guint32 dest_register;
};

struct ic_range_condition
{
  guint32 not_used;
};

struct ic_where_condition
{
  guint32 not_used;
};
#endif
