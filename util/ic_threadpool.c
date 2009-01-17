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

#include <ic_common.h>
#include "ic_threadpool_int.h"

static void free_threadpool(IC_INT_THREADPOOL_STATE *tp_state);
static void check_threads(IC_THREADPOOL_STATE *ext_tp_state);

static void
free_threadpool(IC_INT_THREADPOOL_STATE *tp_state)
{
  if (tp_state->thread_state)
    ic_free((void*)tp_state->thread_state);
  if (tp_state->thread_state_allocation)
    ic_free((void*)tp_state->thread_state_allocation);
  if (tp_state->mutex)
    g_mutex_free(tp_state->mutex);
  ic_free((void*)tp_state);
}

static int
get_thread_id(IC_THREADPOOL_STATE *ext_tp_state,
              guint32 *thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  GMutex *mutex= tp_state->mutex;
  IC_INT_THREAD_STATE *thread_state;

  guint32 i;

  g_mutex_lock(mutex);
  for (i= 0; i < tp_state->max_thread_id_plus_one; i++)
  {
    thread_state= tp_state->thread_state[i];
    if (thread_state->free)
    {
      *thread_id= i;
      goto end;
    }
  }
  if (tp_state->max_thread_id_plus_one < tp_state->threadpool_size)
  {
    *thread_id= tp_state->max_thread_id_plus_one;
    tp_state->max_thread_id_plus_one++;
    thread_state= tp_state->thread_state[*thread_id];
    goto end;
  }
  else
    goto error;
end:
  thread_state->free= FALSE;
  g_mutex_unlock(mutex);
  return 0;
error:
  g_mutex_unlock(mutex);
  return IC_ERROR_THREADPOOL_FULL;
}

static int
get_thread_id_wait(IC_THREADPOOL_STATE *ext_tp_state,
                   guint32 *thread_id,
                   guint32 time_out_seconds)
{
  guint32 loc_thread_id= 0;
  guint32 loop_count= 0;
  int ret_code;

  while ((ret_code= get_thread_id(ext_tp_state,
                                  &loc_thread_id)) &&
         loop_count < time_out_seconds)
  {
    ic_sleep(1); /* Sleep one second waiting for a thread to finish */
    check_threads(ext_tp_state);
    loop_count++;
  }
  *thread_id= loc_thread_id;
  return ret_code;
}

void
thread_stops(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;
  IC_INT_THREADPOOL_STATE *tp_state= thread_state->tp_state;
  GMutex *mutex= tp_state->mutex;
  g_mutex_lock(mutex);
  thread_state->stopped= TRUE;
  g_mutex_unlock(mutex);
}

static void
free_thread(IC_INT_THREADPOOL_STATE *tp_state,
            IC_INT_THREAD_STATE *thread_state,
            guint32 thread_id)
{
  thread_state->free= TRUE;
  thread_state->thread= NULL;
  thread_state->stopped= FALSE;
  if (thread_id == (tp_state->max_thread_id_plus_one - 1))
    tp_state->max_thread_id_plus_one--;
}

static void
check_threads(IC_THREADPOOL_STATE *ext_tp_state)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  GMutex *mutex= tp_state->mutex;
  IC_INT_THREAD_STATE *thread_state;
  guint32 max_thread_id_plus_one= tp_state->max_thread_id_plus_one;
  guint32 i;

  g_mutex_lock(mutex);
  for (i= 0; i < max_thread_id_plus_one; i++)
  {
    thread_state= tp_state->thread_state[i];
    if (thread_state->stopped)
    {
      thread_state->stopped= FALSE;
      g_mutex_unlock(mutex);
      g_thread_join(thread_state->thread);
      g_mutex_lock(mutex);
      free_thread(tp_state, thread_state, i);
    }
  }
  g_mutex_unlock(mutex);
}

static int
start_thread_with_thread_id(IC_THREADPOOL_STATE *ext_tp_state,
             guint32 thread_id,
             GThreadFunc thread_func,
             gpointer thread_obj,
             gulong stack_size)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;
  GMutex *mutex= tp_state->mutex;
  GThread *thread;
  GError *error= NULL;
  int ret_code= 0;

  thread_state= tp_state->thread_state[thread_id];
  g_mutex_lock(mutex);
  thread_state->object= thread_obj;
  thread= g_thread_create_full(thread_func,
                               (gpointer)thread_state,
                               stack_size,
                               TRUE,     /* Joinable */
                               TRUE,     /* Bound    */
                               G_THREAD_PRIORITY_NORMAL,
                               &error);
  if (thread)
  {
    thread_state->thread= thread;
    thread_state->free= FALSE;
  }
  else
  {
    free_thread(tp_state, thread_state, thread_id);
    ret_code= IC_ERROR_START_THREAD_FAILED;
  }
  g_mutex_unlock(mutex);
  return ret_code;
}

static int
start_thread(IC_THREADPOOL_STATE *ext_tp_state,
             guint32 *thread_id,
             GThreadFunc thread_func,
             gpointer thread_obj,
             gulong stack_size)
{
  guint32 loc_thread_id;
  int ret_code;

  if ((ret_code= get_thread_id(ext_tp_state, &loc_thread_id)) ||
      (ret_code= start_thread_with_thread_id(ext_tp_state,
                                             loc_thread_id,
                                             thread_func,
                                             thread_obj,
                                             stack_size)))
  {
    return ret_code;
  }
  *thread_id= loc_thread_id;
  return 0;
}
  
static void
stop_threadpool(IC_THREADPOOL_STATE *ext_tp_state)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  guint32 loop_count= 0;
  guint32 i;

  for (i= 0; i < tp_state->threadpool_size; i++)
    tp_state->thread_state[i]->stop_flag= TRUE;
  check_threads(ext_tp_state);
  while (tp_state->max_thread_id_plus_one)
  {
    ic_sleep(1);
    check_threads(ext_tp_state);
    loop_count++;
    if (loop_count > IC_MAX_WAIT_THREADPOOL_STOP)
    {
      printf("We waited for %d seconds to stop threadpool unsuccessfully"
             ", we stop anyways\n", IC_MAX_WAIT_THREADPOOL_STOP);
      break;
    }
  }
  free_threadpool(tp_state);
}

static void
join_thread(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  g_mutex_lock(tp_state->mutex);
  thread_state= tp_state->thread_state[thread_id];
  if (!thread_state)
    abort();
  g_assert(!thread_state->free);
  g_mutex_unlock(tp_state->mutex);
  g_thread_join(thread_state->thread);
  g_mutex_lock(tp_state->mutex);
  g_assert(thread_state->stopped);
  free_thread(tp_state, thread_state, thread_id);
  g_mutex_unlock(tp_state->mutex);
}

static IC_THREAD_STATE*
get_thread_state(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  thread_state= tp_state->thread_state[thread_id];
  return (IC_THREAD_STATE*)thread_state;
}

IC_THREADPOOL_STATE*
ic_create_threadpool(guint32 pool_size)
{
  gchar *thread_state_ptr;
  guint32 thread_state_size, i;
  IC_INT_THREAD_STATE *thread_state;
  IC_INT_THREADPOOL_STATE *tp_state;

  if (!(tp_state= (IC_INT_THREADPOOL_STATE*)
        ic_calloc(sizeof(IC_INT_THREADPOOL_STATE))))
    return NULL;
  if (!(tp_state->thread_state= (IC_INT_THREAD_STATE**)
        ic_calloc(pool_size * sizeof(IC_THREAD_STATE*))))
    goto mem_alloc_error;
  /*
    Ensure that we don't get false cache line sharing by allocating at
    least one cache line size per thread state object.
  */
  thread_state_size= IC_MAX(sizeof(IC_INT_THREAD_STATE),
                            IC_STD_CACHE_LINE_SIZE);
  if (!(tp_state->thread_state_allocation=
        ic_calloc(pool_size * thread_state_size)))
    goto mem_alloc_error;
  if (!(tp_state->mutex= g_mutex_new()))
    goto mem_alloc_error;

  thread_state_ptr= tp_state->thread_state_allocation;
  for (i= 0; i < pool_size; i++)
  {
    tp_state->thread_state[i]= (IC_INT_THREAD_STATE*)thread_state_ptr;
    thread_state= (IC_INT_THREAD_STATE*)thread_state_ptr;
    thread_state_ptr+= thread_state_size;
    thread_state->tp_state= tp_state;
    thread_state->ts_ops.ic_thread_stops= thread_stops;
  }
  tp_state->tp_ops.ic_threadpool_start_thread= start_thread;
  tp_state->tp_ops.ic_threadpool_get_thread_id= get_thread_id_wait;
  tp_state->tp_ops.ic_threadpool_get_thread_state= get_thread_state;
  tp_state->tp_ops.ic_threadpool_join= join_thread;
  tp_state->tp_ops.ic_threadpool_check_threads= check_threads;
  tp_state->tp_ops.ic_threadpool_stop= stop_threadpool;

  tp_state->max_thread_id_plus_one= 0;
  tp_state->threadpool_size= pool_size;
  return (IC_THREADPOOL_STATE*)tp_state;

mem_alloc_error:
  free_threadpool(tp_state);
  return NULL;
}
