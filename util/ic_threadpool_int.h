/* Copyright (C) 2007-2013 iClaustron AB

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
  IC_THREADPOOL_STATE* (*ic_get_threadpool) (IC_THREAD_STATE *thread_state);
  /*
    The following variables are static and set once and never changed
    and thus require no protection.
  */
  /* This is my thread id */
  guint32 thread_id;
  /* Indicator if thread state is initiated, for debug purposes */
  guint32 inited;
  /* Reference to thread pool object */
  IC_INT_THREADPOOL_STATE *tp_state;

  /*
    The following variables are always set after allocating the thread
    object, they are set by the thread starting the thread and reset
    when the thread is stopped and put back into the free list, so
    implicitly they are protected by free list

   User object for use when starting thread
   GLib Thread object
  */
  void *object;
  GThread *thread;

  /* The following variables are protected by the mutex on this object */

  /* Flag used to check for stop of thread from thread pool */
  guint32 stop_flag;
  /* Variable that shows whether thread has started */
  guint32 started;
  /* Shows whether startup part of thread is done */
  guint32 startup_done;
  /* Starter of thread now says go to proceed with running thread */
  guint32 startup_ready_to_proceed;
  /* Synchronisation of startup was requested */
  guint32 synch_startup;
  /* Thread is waiting for wake up signal */
  guint32 wait_wakeup;
  /*
    Thread have been stopped, not yet joined 
    Flag used when stopping thread to wait for it to complete stop
    Protected by stop list mutex
  */
  guint32 stopped;
  guint32 wait_for_stop;
  /*
     Thread object is in the free list, ready for use
     Protected by free list mutex
   */
  guint32 free;

  /* This is the next link when the thread is in either free or stopped list */
  guint32 next_thread_id;
  /* This is the prev link when the thread is in the stopped list */
  guint32 prev_thread_id;

  /* The mutex and condition is mainly used for startup synchronisation.  */
  IC_MUTEX *mutex;
  IC_COND *cond;
  gboolean use_external_mutex;
  gboolean use_external_cond;
};

struct ic_int_threadpool_state
{
  IC_THREADPOOL_OPS tp_ops;
  IC_THREAD_STATE_OPS ts_ops;
  /* 
    Keep track of number of free threads for easy stop, protected by free
    list mutex
  */
  guint32 num_free_threads;

  /* Variable keeping track if thread pool is stopped */
  volatile int stop_flag;

  /* Pointer to first free thread object, protected by free list mutex */
  guint32 first_free_thread_id;
  /* Use internal mutex or not */
  guint32 use_internal_mutex;
  /*
    Pointer to first stopped thread waiting to be joined
    Number of stopped threads in list
    Protected by stop list mutex
  */
  guint32 first_stopped_thread_id;
  guint32 num_stopped_threads;

  /**
   * This variable gives us a hint if we're at all making progress
   * in the thread pool. If we are out of thread id's in the thread
   * pool and this number is not moving ahead, then we've probably
   * got stuck in most or all threads if it happens for a long time.
   *
   * If we get no new thread id and we're making progress, then we
   * know that there is progress at least.
   */
  guint64 num_thread_allocations;

  /* Keep track of total thread pool size, set once, not protected */
  guint32 threadpool_size;
  /*
    The thread pool contains a number of shared resources. It has a list
    of free thread objects, it has a list of stopped threads and there
    is local information on each thread object. Currently most of this
    information is protected by this one mutex. A simple more scalable
    approach if the thread pool becomes a scalability issue is to
    separate those parts into separate mutexes for protecting different
    things. At the moment the thread pool is used for rare occasions of
    starting and stopping threads, so should not be a scalability issue.
  */
  /*
    The stop list mutex protects the stop list, this means the first
    pointer and the next and prev pointers on all thread objects that
    is included in this list. There is also a condition used to wait
    on stopped threads.
  */
  IC_MUTEX *stop_list_mutex;
  IC_COND *stop_list_cond;

  /*
    The free list mutex protects the free list, this means the first
    pointer and the next pointer on all thread objects in the list.
    Also the free variable in the thread objects which is merely a
    debug variable.
  */
  IC_MUTEX *free_list_mutex;

  /* Thread state objects, set once, not protected */
  IC_INT_THREAD_STATE **thread_state;
  /* Memory allocation variable for all the thread state objects */
  gchar *thread_state_allocation;
};
