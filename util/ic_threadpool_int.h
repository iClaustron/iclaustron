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

#define IC_MAX_WAIT_THREADPOOL_STOP 30
typedef struct ic_int_threadpool_state IC_INT_THREADPOOL_STATE;
typedef struct ic_int_thread_state IC_INT_THREAD_STATE;

struct ic_int_thread_state
{
  IC_THREAD_STATE_OPS ts_ops;
  void *object;
  guint32 stop_flag;
  guint32 stopped;
  guint32 free;
  guint32 not_used;
  GThread *thread;
  IC_INT_THREADPOOL_STATE *tp_state;
};

struct ic_int_threadpool_state
{
  IC_THREADPOOL_OPS tp_ops;
  guint32 max_thread_id_plus_one;
  guint32 threadpool_size;
  GMutex *mutex;
  IC_INT_THREAD_STATE **thread_state;
  gchar *thread_state_allocation;
};
