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

  /*
    These methods are used to call cond_wait on the mutex and condition
    used by the thread. This can be an internal mutex/condition or a mutex
    and condition supplied by the owner of the thread pool. There are also
    methods to lock and unlock the thread using the mutex.
  */
  void (*ic_thread_wait) (IC_THREAD_STATE *thread_state);
  void (*ic_thread_lock_and_wait) (IC_THREAD_STATE *thread_state);
  void (*ic_thread_lock) (IC_THREAD_STATE *thread_state);
  void (*ic_thread_unlock) (IC_THREAD_STATE *thread_state);
  void (*ic_thread_wake) (IC_THREAD_STATE *thread_state);

  void (*ic_thread_set_mutex) (IC_THREAD_STATE *thread_state,
                               IC_MUTEX *mutex);
  void (*ic_thread_set_cond) (IC_THREAD_STATE *thread_state,
                              IC_COND *cond);

  void* (*ic_thread_get_object) (IC_THREAD_STATE *thread_state);
  gboolean (*ic_thread_get_stop_flag) (IC_THREAD_STATE *thread_state);
};
typedef struct ic_thread_state_ops IC_THREAD_STATE_OPS;

struct ic_thread_state
{
  IC_THREADPOOL_STATE* (*ic_get_threadpool) (IC_THREAD_STATE *thread_state);
};

struct ic_threadpool_ops
{
  /*
    Start thread when it's ok that thread id allocation fails
    ---------------------------------------------------------
    Description: Calls first get_thread_id and start_thread_with_thread_id
    Parameters: See ic_threadpool_start_with_thread_id, except thread_id
    which is an OUT parameter where the thread id is delivered.
  */
  int  (*ic_threadpool_start_thread) (IC_THREADPOOL_STATE *tp_state,
                                      guint32 *thread_id,
                                      GThreadFunc thread_func,
                                      gpointer thread_obj,
                                      gulong stack_size,
                                      gboolean synch_startup);

  /*
    Start thread when thread already allocated
    ------------------------------------------
    Description: Start a thread using a thread id which already have been
    allocated, at failure thread_id is deallocated.
    Parameters:
      tp_state                Threadpool object
      thread_id               Reference to a thread object using an id
      thread_func             The function called at thread start
      thread_obj              User specified object delivered to thread
      stack_size              Stack size of thread
      synch_startup           Wait for startup thread handling
    
    All parameters are IN parameters
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

  /* Get thread object by id */
  int  (*ic_threadpool_get_thread_id) (IC_THREADPOOL_STATE *tp_state,
                                       guint32 *thread_id);

  /* Get thread object by id, wait for a while if no free objects */
  int  (*ic_threadpool_get_thread_id_wait) (IC_THREADPOOL_STATE *tp_state,
                                            guint32 *thread_id,
                                            guint32 time_out_seconds);

  /* Wait for thread to explicitly have released all its resources */
  void (*ic_threadpool_join) (IC_THREADPOOL_STATE *tp_state,
                              guint32 thread_id);

  /* Check for any threads that have stopped and released all their resources*/
  void (*ic_threadpool_check_threads) (IC_THREADPOOL_STATE *tp_state);

  /*
    Wake thread waiting for run command after startup phase
    -------------------------------------------------------
    Description: When starting a thread using the synch_startup, the
    thread will call ic_thread_startup_done when ready with the
    startup phase. It will then wait in this call until the
    management thread calls this function to start the thread
    activitity again.
  */
  void (*ic_threadpool_run_thread) (IC_THREADPOOL_STATE *tp_state,
                                    guint32 thread_id);

  /*
    Issue stop command to thread
    ----------------------------
    This is a command from a management thread to the running thread.
    It can be done at any time but if running thread is waiting for
    synchronization at startup we need to signal running thread to
    wake up as well.

    There is two variants of this call, one waits for join thread to
    be handled and the other returns immediately after stopping the
    thread without waiting for the thread to actually stop.
  */
  void (*ic_threadpool_stop_thread) (IC_THREADPOOL_STATE *tp_state,
                                     guint32 thread_id);

  void (*ic_threadpool_stop_thread_wait) (IC_THREADPOOL_STATE *tp_state,
                                          guint32 thread_id);

  /* Get stop flag for thread pool */
  gboolean (*ic_threadpool_get_stop_flag) (IC_THREADPOOL_STATE *tp_state);

  /* Set stop flag for all threads in the pool */
  void (*ic_threadpool_set_stop_flag) (IC_THREADPOOL_STATE *tp_state);

  /* Stop threadpool and release all its resources */
  void (*ic_threadpool_stop) (IC_THREADPOOL_STATE *tp_state);
};
typedef struct ic_threadpool_ops IC_THREADPOOL_OPS;

#define IC_DEFAULT_MAX_THREADPOOL_SIZE 8192

struct ic_threadpool_state
{
  IC_THREADPOOL_OPS tp_ops;
  IC_THREAD_STATE_OPS ts_ops;
};

IC_THREADPOOL_STATE*
ic_create_threadpool(guint32 pool_size, gboolean use_internal_mutex);
#endif
