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
  /* User object for use when starting thread */
  void *object;
  /* Flag used to check for stop of thread from thread pool */
  guint32 stop_flag;
  /* Thread have been stopped, not yet joined */
  guint32 stopped;
  /* Indicator if thread state is initiated, for debug purposes */
  guint32 inited;
  /* Debug variable, shows whether thread has started */
  guint32 started;
  /* Thread object is in the free list, ready for use */
  guint32 free;
  /* Synchronisation of startup was requested */
  guint32 synch_startup;
  /* Flag used when stopping thread to wait for it to complete stop */
  guint32 wait_for_stop;
  /* This is my thread id */
  guint32 thread_id;
  /* This is the next link when the thread is in either free or stopped list */
  guint32 next_thread_id;
  /* This is the prev link when the thread is in the stopped list */
  guint32 prev_thread_id;
  /* GLib Thread object */
  GThread *thread;
  /* The mutex and condition is mainly used for startup synchronisation.  */
  GMutex *mutex;
  GCond *cond;
  /* Reference to thread pool object */
  IC_INT_THREADPOOL_STATE *tp_state;
};

struct ic_int_threadpool_state
{
  IC_THREADPOOL_OPS tp_ops;
  /* Keep track of number of free threads for easy stop */
  guint32 num_free_threads;
  /* Pointer to first free thread object */
  guint32 first_free_thread_id;
  /* Pointer to first stopped thread waiting to be joined */
  guint32 first_stopped_thread_id;
  /* Keep track of total thread pool size */
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
  GMutex *mutex;
  /* Thread state objects */
  IC_INT_THREAD_STATE **thread_state;
  /* Memory allocation variable for all the thread state objects */
  gchar *thread_state_allocation;
};
