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

#ifndef IC_APID_INLINE_H
#define IC_APID_INLINE_H
#include <ic_common.h>

static inline IC_COMMIT_STATE
ic_get_commit_state(IC_TRANSACTION_STATE *trans_state)
{
  return trans_state->commit_state;
}

static inline IC_SAVEPOINT_ID
ic_get_stable_savepoint(IC_TRANSACTION_STATE *trans_state)
{
  return trans_state->savepoint_stable;
}

static inline IC_SAVEPOINT_ID
ic_get_requested_savepoint(IC_TRANSACTION_STATE *trans_state)
{
  return trans_state->savepoint_requested;
}

#endif
