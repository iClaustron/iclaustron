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
#ifndef IC_THREADPOOL_H
#define IC_THREADPOOL_H
#include <ic_common.h>

typedef struct ic_thread_state IC_THREAD_STATE;
typedef struct ic_threadpool_state IC_THREADPOOL_STATE;

struct ic_thread_state_ops
{
  void (*ic_thread_stops) (IC_THREAD_STATE *thread_state);
};
typedef struct ic_thread_state_ops IC_THREAD_STATE_OPS;

struct ic_thread_state
{
  IC_THREAD_STATE_OPS ts_ops;
  guint32 stop_flag;
};

struct ic_threadpool_ops
{
  int  (*ic_threadpool_start_thread) (IC_THREADPOOL_STATE *tp_state,
                                      guint32 thread_id,
                                      GThreadFunc thread_func,
                                      gpointer thread_obj,
                                      gulong stack_size);
  int  (*ic_threadpool_get_thread_id) (IC_THREADPOOL_STATE *tp_state,
                                       guint32 *thread_id);
  void (*ic_threadpool_check_threads) (IC_THREADPOOL_STATE *tp_state);
  void (*ic_threadpool_stop) (IC_THREADPOOL_STATE *tp_state);
};
typedef struct ic_threadpool_ops IC_THREADPOOL_OPS;

#define IC_DEFAULT_MAX_THREADPOOL_SIZE 8192

struct ic_threadpool_state
{
  IC_THREADPOOL_OPS tp_ops;
};

IC_THREADPOOL_STATE*
ic_create_threadpool(guint32 pool_size);
#endif

