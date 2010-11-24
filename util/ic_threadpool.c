/* Copyright (C) 2007-2010 iClaustron AB

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

#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_threadpool.h>
#include "ic_threadpool_int.h"

static void check_threads(IC_THREADPOOL_STATE *ext_tp_state);
static gboolean thread_startup_done(IC_THREAD_STATE *ext_thread_state);
static void join_thread(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id);
static void internal_join_thread(IC_THREADPOOL_STATE *ext_tp_state,
                                 guint32 thread_id,
                                 gboolean own_mutex);

/* Free all resources connected to the thread pool */
static void
free_threadpool(IC_INT_THREADPOOL_STATE *tp_state)
{
  guint32 i;
  IC_INT_THREAD_STATE *thread_state;

  if (tp_state->thread_state_allocation)
  {
    for (i= 0; i < tp_state->threadpool_size; i++)
    {
      thread_state= tp_state->thread_state[i];
      if (thread_state->mutex)
        ic_mutex_destroy(thread_state->mutex);
      if (thread_state->cond)
        ic_cond_destroy(thread_state->cond);
    }
    ic_free((void*)tp_state->thread_state_allocation);
  }
  if (tp_state->thread_state)
    ic_free((void*)tp_state->thread_state);
  if (tp_state->stop_list_mutex)
    ic_mutex_destroy(tp_state->stop_list_mutex);
  if (tp_state->stop_list_cond)
    ic_cond_destroy(tp_state->stop_list_cond);
  if (tp_state->free_list_mutex)
    ic_mutex_destroy(tp_state->free_list_mutex);
  ic_free((void*)tp_state);
}

static gboolean
tp_get_stop_flag(IC_THREADPOOL_STATE *ext_tp_state)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  return (gboolean)tp_state->stop_flag;
}

/* Set stop flag for all threads in thread pool */
static void
set_stop_flag(IC_THREADPOOL_STATE *ext_tp_state)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  guint32 i;

  tp_state->stop_flag= TRUE;

  for (i= 0; i < tp_state->threadpool_size; i++)
  {
    ic_mutex_lock(tp_state->thread_state[i]->mutex);
    tp_state->thread_state[i]->stop_flag= TRUE;
    if (tp_state->thread_state[i]->wait_wakeup)
      ic_cond_signal(tp_state->thread_state[i]->cond);
    ic_mutex_unlock(tp_state->thread_state[i]->mutex);
  }
}

/* Stop the thread pool */
static void
stop_threadpool(IC_THREADPOOL_STATE *ext_tp_state)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  guint32 loop_count= 0;
  guint32 num_free_threads, pool_size;

  set_stop_flag(ext_tp_state);

  while (1)
  {
    check_threads(ext_tp_state);
    ic_mutex_lock(tp_state->free_list_mutex);
    num_free_threads= tp_state->num_free_threads;
    pool_size= tp_state->threadpool_size;
    ic_mutex_unlock(tp_state->free_list_mutex);
    if (num_free_threads == pool_size)
      break;
    if (loop_count > IC_MAX_WAIT_THREADPOOL_STOP)
    {
      ic_printf("We waited for %d seconds to stop threadpool unsuccessfully"
                ", we stop anyways\n", IC_MAX_WAIT_THREADPOOL_STOP);
      break;
    }
    loop_count++;
    ic_sleep(1);
  }
  free_threadpool(tp_state);
}

static void
insert_stopped_list(IC_INT_THREADPOOL_STATE *tp_state, guint32 thread_id)
{
  IC_INT_THREAD_STATE *thread_state, *prev_thread_state;
  guint32 first_stopped_thread_id;

  thread_state= tp_state->thread_state[thread_id];
  if (!thread_state)
    abort();

  ic_mutex_lock(tp_state->stop_list_mutex);
  first_stopped_thread_id= tp_state->first_stopped_thread_id;
  if (first_stopped_thread_id != IC_MAX_UINT32)
  {
    prev_thread_state= tp_state->thread_state[first_stopped_thread_id];
    prev_thread_state->prev_thread_id= thread_id;
  }

  thread_state->next_thread_id= first_stopped_thread_id;
  thread_state->prev_thread_id= IC_MAX_UINT32;
  ic_assert(!thread_state->stopped);
  thread_state->stopped= TRUE;
  if (thread_state->wait_for_stop)
  {
    thread_state->wait_for_stop= FALSE;
    ic_cond_signal(tp_state->stop_list_cond);
  }

  tp_state->first_stopped_thread_id= thread_id;
  tp_state->num_stopped_threads++;
  ic_mutex_unlock(tp_state->stop_list_mutex);
}

static void
remove_stopped_list(IC_INT_THREADPOOL_STATE *tp_state, guint32 thread_id,
                    gboolean own_mutex)
{
  IC_INT_THREAD_STATE *thread_state, *prev_thread_state, *next_thread_state;
  guint32 prev_thread_id, next_thread_id;

  thread_state= tp_state->thread_state[thread_id];
  if (!thread_state)
    abort();

  if (!own_mutex)
    ic_mutex_lock(tp_state->stop_list_mutex);
  prev_thread_id= thread_state->prev_thread_id;
  next_thread_id= thread_state->next_thread_id;
  if (prev_thread_id != IC_MAX_UINT32)
  {
    prev_thread_state= tp_state->thread_state[prev_thread_id];
    prev_thread_state->next_thread_id= next_thread_id;
  }
  else
  {
    tp_state->first_stopped_thread_id= next_thread_id;
  }
  if (next_thread_id != IC_MAX_UINT32)
  {
    next_thread_state= tp_state->thread_state[next_thread_id];
    next_thread_state->prev_thread_id= prev_thread_id;
  }
  ic_assert(thread_state->stopped);
  tp_state->num_stopped_threads--;
  thread_state->stopped= FALSE;
  ic_mutex_unlock(tp_state->stop_list_mutex);
}

static void
thread_wait(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  thread_state->wait_wakeup= TRUE;
  ic_cond_wait(thread_state->cond, thread_state->mutex);
  thread_state->wait_wakeup= FALSE;
}

static void
thread_wake(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  ic_cond_signal(thread_state->cond);
}

static void
thread_lock_and_wait(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  ic_mutex_lock(thread_state->mutex);
  thread_state->wait_wakeup= TRUE;
  ic_cond_wait(thread_state->cond, thread_state->mutex);
  thread_state->wait_wakeup= FALSE;
}

static void
thread_lock(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  ic_mutex_lock(thread_state->mutex);
}

static void
thread_unlock(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  ic_mutex_unlock(thread_state->mutex);
}

static void
thread_set_mutex(IC_THREAD_STATE *ext_thread_state, IC_MUTEX *mutex)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;
  IC_INT_THREADPOOL_STATE *tp_state= thread_state->tp_state;

  ic_require(tp_state->use_internal_mutex);
  ic_mutex_destroy(thread_state->mutex);
  thread_state->mutex= mutex;
}

static void
thread_set_cond(IC_THREAD_STATE *ext_thread_state, IC_COND *cond)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;
  IC_INT_THREADPOOL_STATE *tp_state= thread_state->tp_state;

  ic_require(tp_state->use_internal_mutex);
  ic_cond_destroy(thread_state->cond);
  thread_state->cond= cond;
}

/* Get a thread object by id */
static int
get_thread_id(IC_THREADPOOL_STATE *ext_tp_state,
              guint32 *thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;
  guint32 first_free_thread_id;

  ic_mutex_lock(tp_state->free_list_mutex);
  first_free_thread_id= tp_state->first_free_thread_id;
  if (first_free_thread_id == IC_MAX_UINT32)
  {
    /* No free thread objects */
    ic_mutex_unlock(tp_state->free_list_mutex);
    return IC_ERROR_THREADPOOL_FULL;
  }
  thread_state= tp_state->thread_state[first_free_thread_id];
  ic_assert(thread_state->free);
  tp_state->first_free_thread_id= thread_state->next_thread_id;
  tp_state->num_free_threads--;
  thread_state->next_thread_id= IC_MAX_UINT32;
  *thread_id= first_free_thread_id;
  thread_state->free= FALSE;
  ic_mutex_unlock(tp_state->free_list_mutex);
  return 0;
}

/* Get a thread object by id, wait for a while if no one available */
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

/* Put thread object into free list */
static void
free_thread(IC_INT_THREADPOOL_STATE *tp_state,
            IC_INT_THREAD_STATE *thread_state,
            guint32 thread_id)
{
  guint32 first_free_thread_id;

  if (thread_state->inited)
  {
    DEBUG_PRINT(THREAD_LEVEL, ("free thread with id = %d", thread_id));
  }
  thread_state->inited= TRUE;
  thread_state->thread= NULL;
  thread_state->object= NULL;
  /* Put thread into free list */
  ic_mutex_lock(tp_state->free_list_mutex);
  first_free_thread_id= tp_state->first_free_thread_id;
  thread_state->next_thread_id= first_free_thread_id;
  tp_state->first_free_thread_id= thread_id;
  tp_state->num_free_threads++;
  ic_assert(!thread_state->free);
  thread_state->free= TRUE;
  ic_mutex_unlock(tp_state->free_list_mutex);
  /* Set all state variables to free and not occupied */
  ic_mutex_lock(thread_state->mutex);
  thread_state->started= FALSE;
  ic_assert(!thread_state->synch_startup);
  thread_state->synch_startup= FALSE;
  thread_state->stop_flag= FALSE;
  ic_mutex_unlock(thread_state->mutex);
}

/* Check for stopped threads that require to be joined */
static void
check_threads(IC_THREADPOOL_STATE *ext_tp_state)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;
  guint32 first_stopped_thread_id;

  while (1)
  {
    ic_mutex_lock(tp_state->stop_list_mutex);
    first_stopped_thread_id= tp_state->first_stopped_thread_id;
    if (first_stopped_thread_id != IC_MAX_UINT32)
    {
      /* Remove the thread from the stopped thread list */
      thread_state= tp_state->thread_state[first_stopped_thread_id];
      ic_require(thread_state->stopped);
      internal_join_thread(ext_tp_state, first_stopped_thread_id, TRUE);
      continue;
    }
    ic_mutex_unlock(tp_state->stop_list_mutex);
    break;
  }
}

/* Start a thread using a specified thread object */
static int
start_thread_with_thread_id(IC_THREADPOOL_STATE *ext_tp_state,
             guint32 thread_id,
             GThreadFunc thread_func,
             gpointer thread_obj,
             gulong stack_size,
             gboolean synch_startup)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;
  GThread *thread;
  GError *error= NULL;
  int ret_code= 0;
  gboolean stop_flag;

  ic_require(thread_id < tp_state->threadpool_size);
  thread_state= tp_state->thread_state[thread_id];
  ic_require(thread_state->mutex && thread_state->cond);
  ic_mutex_lock(thread_state->mutex);
  ic_assert(!thread_state->free);
  thread_state->object= thread_obj;
  thread_state->synch_startup= synch_startup;
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
    if (synch_startup)
    {
      /* Wait until thread startup handling is completed */
      ic_cond_wait(thread_state->cond, thread_state->mutex);
      if ((stop_flag= thread_state->stop_flag))
        ic_cond_signal(thread_state->cond);
      ic_mutex_unlock(thread_state->mutex);
      if (stop_flag)
      {
        ret_code= IC_ERROR_START_THREAD_FAILED;
        join_thread(ext_tp_state, thread_id);
      }
    }
    else
      ic_mutex_unlock(thread_state->mutex);
  }
  else
  {
    ic_mutex_unlock(thread_state->mutex);
    free_thread(tp_state, thread_state, thread_id);
    ret_code= IC_ERROR_START_THREAD_FAILED;
  }
  return ret_code;
}

/* Start thread, get thread object first */
static int
start_thread(IC_THREADPOOL_STATE *ext_tp_state,
             guint32 *thread_id,
             GThreadFunc thread_func,
             gpointer thread_obj,
             gulong stack_size,
             gboolean synch_startup)
{
  guint32 loc_thread_id;
  int ret_code;

  if ((ret_code= get_thread_id(ext_tp_state, &loc_thread_id)) ||
      (ret_code= start_thread_with_thread_id(ext_tp_state,
                                             loc_thread_id,
                                             thread_func,
                                             thread_obj,
                                             stack_size,
                                             synch_startup)))
  {
    return ret_code;
  }
  *thread_id= loc_thread_id;
  return 0;
}
  
/* Get thread state object given the thread id */
static IC_THREAD_STATE*
get_thread_state(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  ic_require(thread_id < tp_state->threadpool_size);
  thread_state= tp_state->thread_state[thread_id];
  return (IC_THREAD_STATE*)thread_state;
}

/* Signal thread waiting for start signal */
static void
run_thread(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  ic_require(thread_id < tp_state->threadpool_size);
  thread_state= tp_state->thread_state[thread_id];
  ic_mutex_lock(thread_state->mutex);
  /* Start thread waiting for command to start run phase */
  ic_cond_signal(thread_state->cond);
  thread_state->synch_startup= FALSE; /* No longer used after this */
  ic_mutex_unlock(thread_state->mutex);
}

static void
join_thread(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  internal_join_thread(ext_tp_state, thread_id, FALSE);
}

/* Join the thread with the given thread id (wait for thread to complete) */
static void
internal_join_thread(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id,
                     gboolean own_mutex)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  ic_require(thread_id < tp_state->threadpool_size);
  thread_state= tp_state->thread_state[thread_id];

  g_thread_join(thread_state->thread);
  remove_stopped_list(tp_state, thread_id, own_mutex);
  free_thread(tp_state, thread_state, thread_id);
}

static void
stop_thread_without_wait(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  ic_require(thread_id < tp_state->threadpool_size);
  thread_state= tp_state->thread_state[thread_id];
  ic_mutex_lock(thread_state->mutex);
  if (thread_state->synch_startup ||
      thread_state->wait_wakeup)
    ic_cond_signal(thread_state->cond);
  thread_state->stop_flag= TRUE;
  ic_mutex_unlock(thread_state->mutex);
}

static void
stop_thread_wait(IC_THREADPOOL_STATE *ext_tp_state, guint32 thread_id)
{
  IC_INT_THREADPOOL_STATE *tp_state= (IC_INT_THREADPOOL_STATE*)ext_tp_state;
  IC_INT_THREAD_STATE *thread_state;

  stop_thread_without_wait(ext_tp_state, thread_id);
  thread_state= tp_state->thread_state[thread_id];
  ic_mutex_lock(tp_state->stop_list_mutex);
  while (!thread_state->stopped)
  {
    thread_state->wait_for_stop= TRUE;
    ic_cond_wait(tp_state->stop_list_cond, tp_state->stop_list_mutex);
  }
  internal_join_thread(ext_tp_state, thread_id, TRUE);
}

/*
  This method is used by the thread managed by the thread pool when it
  has completed its startup handling. This method is called if and only
  if the flag synch_startup is set when the thread is started.
*/
void
thread_started(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  /* By locking the mutex we ensure that start synch is done */
  ic_mutex_lock(thread_state->mutex);
  thread_state->started= TRUE;
  ic_mutex_unlock(thread_state->mutex);
}

void
thread_stops(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;
  IC_INT_THREADPOOL_STATE *tp_state= thread_state->tp_state;

  insert_stopped_list(tp_state, thread_state->thread_id);
  DEBUG_PRINT(THREAD_LEVEL, ("thread with id %d stops",
                             thread_state->thread_id));
  ic_mutex_lock(thread_state->mutex);
  ic_assert(thread_state->started);
  thread_state->started= FALSE;
  if (thread_state->synch_startup)
  {
    ic_cond_signal(thread_state->cond);
    thread_state->synch_startup= FALSE;
  }
  ic_mutex_unlock(thread_state->mutex);
}

static gboolean
thread_startup_done(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  ic_mutex_lock(thread_state->mutex);
  ic_assert(thread_state->synch_startup);
  ic_cond_signal(thread_state->cond);
  if (thread_state->stop_flag)
    goto stop_end;
  /* Wait for management thread to signal us to continue */
  ic_cond_wait(thread_state->cond, thread_state->mutex);
  /* Check whether we're requested to stop or not */
  if (thread_state->stop_flag)
    goto stop_end;
  ic_mutex_unlock(thread_state->mutex);
  return FALSE;

stop_end:
  thread_state->synch_startup= FALSE;
  ic_mutex_unlock(thread_state->mutex);
  return TRUE;
}

static void*
get_object(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  return thread_state->object;
}

static gboolean
ts_get_stop_flag(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  if (ic_get_stop_flag())
    thread_state->stop_flag= TRUE;
  return thread_state->stop_flag;
}

static IC_THREADPOOL_STATE*
get_threadpool(IC_THREAD_STATE *ext_thread_state)
{
  IC_INT_THREAD_STATE *thread_state= (IC_INT_THREAD_STATE*)ext_thread_state;

  return (IC_THREADPOOL_STATE*)thread_state->tp_state;
}

IC_THREADPOOL_STATE*
ic_create_threadpool(guint32 pool_size,
                     gboolean use_internal_mutex)
{
  gchar *thread_state_ptr;
  guint32 thread_state_size, i;
  IC_INT_THREAD_STATE *thread_state;
  IC_INT_THREADPOOL_STATE *tp_state;

  DEBUG_ENTRY("ic_create_threadpool");
  if (!(tp_state= (IC_INT_THREADPOOL_STATE*)
        ic_calloc(sizeof(IC_INT_THREADPOOL_STATE))))
    DEBUG_RETURN_PTR(NULL);
  if (!(tp_state->thread_state= (IC_INT_THREAD_STATE**)
        ic_calloc(pool_size * sizeof(IC_INT_THREAD_STATE*))))
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
  if (!(tp_state->stop_list_mutex= ic_mutex_create()))
    goto mem_alloc_error;
  if (!(tp_state->stop_list_cond= ic_cond_create()))
    goto mem_alloc_error;
  if (!(tp_state->free_list_mutex= ic_mutex_create()))
    goto mem_alloc_error;

  thread_state_ptr= tp_state->thread_state_allocation;
  tp_state->threadpool_size= pool_size;
  tp_state->first_free_thread_id= IC_MAX_UINT32;
  tp_state->first_stopped_thread_id= IC_MAX_UINT32;
  tp_state->use_internal_mutex= use_internal_mutex;

  for (i= 0; i < pool_size; i++)
  {
    tp_state->thread_state[i]= (IC_INT_THREAD_STATE*)thread_state_ptr;
    thread_state= (IC_INT_THREAD_STATE*)thread_state_ptr;
    thread_state_ptr+= thread_state_size;
    thread_state->tp_state= tp_state;
    thread_state->thread_id= i;
    thread_state->mutex= ic_mutex_create();
    thread_state->cond= ic_cond_create();
    thread_state->ic_get_threadpool= get_threadpool;
    if (thread_state->mutex == NULL ||
        thread_state->cond == NULL)
      goto mem_alloc_error;
    free_thread(tp_state, thread_state, i);
  }

  /* Thread pool functions */
  tp_state->tp_ops.ic_threadpool_start_thread_with_thread_id=
    start_thread_with_thread_id;
  tp_state->tp_ops.ic_threadpool_start_thread= start_thread;
  tp_state->tp_ops.ic_threadpool_get_thread_id= get_thread_id;
  tp_state->tp_ops.ic_threadpool_get_thread_id_wait= get_thread_id_wait;
  tp_state->tp_ops.ic_threadpool_get_thread_state= get_thread_state;
  tp_state->tp_ops.ic_threadpool_join= join_thread;
  tp_state->tp_ops.ic_threadpool_run_thread= run_thread;
  tp_state->tp_ops.ic_threadpool_stop_thread= stop_thread_without_wait;
  tp_state->tp_ops.ic_threadpool_stop_thread_wait= stop_thread_wait;
  tp_state->tp_ops.ic_threadpool_check_threads= check_threads;
  tp_state->tp_ops.ic_threadpool_get_stop_flag= tp_get_stop_flag;
  tp_state->tp_ops.ic_threadpool_set_stop_flag= set_stop_flag;
  tp_state->tp_ops.ic_threadpool_stop= stop_threadpool;

  /* Thread state functions */
  tp_state->ts_ops.ic_thread_stops= thread_stops;
  tp_state->ts_ops.ic_thread_startup_done= thread_startup_done;
  tp_state->ts_ops.ic_thread_started= thread_started;
  tp_state->ts_ops.ic_thread_get_object= get_object;
  tp_state->ts_ops.ic_thread_get_stop_flag= ts_get_stop_flag;
  tp_state->ts_ops.ic_thread_wait= thread_wait;
  tp_state->ts_ops.ic_thread_lock_and_wait= thread_lock_and_wait;
  tp_state->ts_ops.ic_thread_lock= thread_lock;
  tp_state->ts_ops.ic_thread_unlock= thread_unlock;
  tp_state->ts_ops.ic_thread_wake= thread_wake;
  tp_state->ts_ops.ic_thread_set_mutex= thread_set_mutex;
  tp_state->ts_ops.ic_thread_set_cond= thread_set_cond;

  DEBUG_RETURN_PTR((IC_THREADPOOL_STATE*)tp_state);

mem_alloc_error:
  free_threadpool(tp_state);
  DEBUG_RETURN_PTR(NULL);
}
