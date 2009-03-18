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
typedef struct ic_thread_state IC_THREAD_STATE;
typedef struct ic_threadpool_state IC_THREADPOOL_STATE;

/*
  This object is the object used by the threads managed by the thread pool.
  So the methods on this object should only be called from the threads in
  the pool and not to manage threads in the pool from other threads. The
  main purpose of those methods is to communicate between the thread and
  the thread pool managing the thread.
*/
struct ic_thread_state_ops
{
  /*
    Announce that the thread has started. This method is also used
    to ensure that we don't start the thread start-up code until we
    have prepared for it in the thread managing the starting thread.
    This is particularly for the case when the user has requested
    the start-up to be synchronized.
  */
  void (*ic_thread_started) (IC_THREAD_STATE *thread_state);
  /*
    This method must be called immediately before stopping the thread by
    returning from the method invoked at start of thread.
    If the user has requested to synchronize startup this will also
    implicitly call the ic_thread_startup_done method.
  */
  void (*ic_thread_stops) (IC_THREAD_STATE *thread_state);
  /*
    This method should be called when the start-up part of the thread is
    completed and there is a need to synchronize with other threads in the
    thread pool. Return from this method doesn't happen until the
    controlling thread calls the method indicating it's ok for the thread
    to enter its running algorithm.

    Returns TRUE if the thread has been asked to stop
  */
  gboolean (*ic_thread_startup_done) (IC_THREAD_STATE *thread_state);
};
typedef struct ic_thread_state_ops IC_THREAD_STATE_OPS;

struct ic_thread_state
{
  IC_THREAD_STATE_OPS ts_ops;
  void *object;
  guint32 stop_flag;
};

struct ic_threadpool_ops
{
  /* Start thread when it's ok that thread id allocation fails */
  int  (*ic_threadpool_start_thread) (IC_THREADPOOL_STATE *tp_state,
                                      guint32 *thread_id,
                                      GThreadFunc thread_func,
                                      gpointer thread_obj,
                                      gulong stack_size,
                                      gboolean synch_startup);

  /*
    Start thread when thread already allocated, at failure thread_id is
    deallocated.
  */
  int  (*ic_threadpool_start_thread_with_thread_id) (
                                      IC_THREADPOOL_STATE *tp_state,
                                      guint32 thread_id,
                                      GThreadFunc thread_func,
                                      gpointer thread_obj,
                                      gulong stack_size,
                                      gboolean synch_startup);

  /* Get thread state, this state is always sent to a starting thread */
  IC_THREAD_STATE* (*ic_threadpool_get_thread_state)
                                      (IC_THREADPOOL_STATE *tp_state,
                                       guint32 thread_id);

  /*
     Get thread id when it's necessary to verify we don't start too
     many threads at the same time.
  */
  int  (*ic_threadpool_get_thread_id) (IC_THREADPOOL_STATE *tp_state,
                                       guint32 *thread_id,
                                       guint32 time_out_seconds);

  /* Wait for thread to explicitly have released all its resources */
  void (*ic_threadpool_join) (IC_THREADPOOL_STATE *tp_state,
                              guint32 thread_id);

  /* Check for any threads that have stopped and released all their resources*/
  void (*ic_threadpool_check_threads) (IC_THREADPOOL_STATE *tp_state);

  /* Wake thread waiting for run command after startup phase */
  void (*ic_threadpool_run_thread) (IC_THREADPOOL_STATE *tp_state,
                                    guint32 thread_id);

  /*
    Issue stop command to thread
    ----------------------------
    This is a command from a management thread to the running thread.
    It can be done at any time but if running thread is waiting for
    synchronization at startup we need to signal running thread to
    wake up as well.
  */
  void (*ic_threadpool_stop_thread) (IC_THREADPOOL_STATE *tp_state,
                                     guint32 thread_id);

  /* Stop threadpool and release all its resources */
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
