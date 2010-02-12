/* Copyright (C) 2008-2009 iClaustron AB

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
#include <ic_poll_set.h>
#include "ic_poll_set_int.h"
/* System header files */
#ifdef WIN32
#include <winsock.h>
#else
#include <unistd.h>
#endif

static void
free_poll_set(IC_POLL_SET *ext_poll_set)
{
  guint32 i;
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;

  for (i= 0; i < poll_set->num_poll_connections; i++)
    ic_free(poll_set->poll_connections[i]);
  if (poll_set->poll_connections)
    ic_free(poll_set->poll_connections);
  if (poll_set->ready_connections)
    ic_free(poll_set->ready_connections);
  if (poll_set->need_close_at_free)
    ic_close_socket(poll_set->poll_set_fd);
  if (poll_set->impl_specific_ptr)
    ic_free(poll_set->impl_specific_ptr);
  ic_free(poll_set);
}

#define MAX_POLL_SET_CONNECTIONS 1024
static int
alloc_poll_set(IC_INT_POLL_SET **poll_set)
{
  if (!(*poll_set= (IC_INT_POLL_SET*)ic_calloc(sizeof(IC_INT_POLL_SET))))
    return IC_ERROR_MEM_ALLOC;
  (*poll_set)->num_allocated_connections= MAX_POLL_SET_CONNECTIONS;
  if (!((*poll_set)->poll_connections= (IC_POLL_CONNECTION**)
       ic_calloc(sizeof(IC_POLL_CONNECTION*) * MAX_POLL_SET_CONNECTIONS)))
    goto mem_error;
  if (!((*poll_set)->ready_connections= (IC_POLL_CONNECTION**)
       ic_calloc(sizeof(IC_POLL_CONNECTION*) * MAX_POLL_SET_CONNECTIONS)))
    goto mem_error;
  return 0;
mem_error:
  free_poll_set((IC_POLL_SET*)*poll_set);
  return IC_ERROR_MEM_ALLOC;
}

static int
add_poll_set_member(IC_INT_POLL_SET *poll_set, int fd, void *user_obj,
                    guint32 *index)
{
  guint32 i;
  guint32 loc_index= MAX_POLL_SET_CONNECTIONS;
  IC_POLL_CONNECTION *poll_conn;

  if (poll_set->num_poll_connections == poll_set->num_allocated_connections)
    return IC_ERROR_POLL_SET_FULL;
  if (!(poll_conn= (IC_POLL_CONNECTION*)ic_malloc(sizeof(IC_POLL_CONNECTION))))
    return IC_ERROR_MEM_ALLOC;
  if (poll_set->use_compact_array)
  {
    loc_index= poll_set->num_poll_connections;
  }
  else
  {
    for (i= 0; i < poll_set->num_allocated_connections; i++)
    {
      if (poll_set->poll_connections[i] == NULL)
      {
        /* Found an empty slot, let's use this slot */
        loc_index= i;
        break;
      }
    }
  }
  ic_require(loc_index < MAX_POLL_SET_CONNECTIONS);
  poll_set->poll_connections[loc_index]= poll_conn;

  DEBUG_PRINT(COMM_LEVEL, ("Added fd = %d to slot %u for epoll fd = %d",
              fd, loc_index, poll_set->poll_set_fd));
  poll_conn->fd= fd;
  poll_conn->user_obj= user_obj;
  poll_conn->index= loc_index;
  poll_set->num_poll_connections++;
  *index= loc_index;
  return 0;
}

static int
remove_poll_set_member(IC_INT_POLL_SET *poll_set, int fd,
                       guint32 *index_removed)
{
  guint32 i, num_ready_connections;
  guint32 found_index= MAX_POLL_SET_CONNECTIONS;
  IC_POLL_CONNECTION *poll_conn;

  if (poll_set->poll_scan_ongoing)
  {
    num_ready_connections= poll_set->num_ready_connections;
    for (i= 0; i < poll_set->num_ready_connections; i++)
    {
      poll_conn= poll_set->ready_connections[i];
      if (poll_conn->fd == fd)
      {
        /*
          The removed connection was in the ready set, however we're
          removing it from the set and we don't want to report events
          on a connection no longer covered.
        */
        poll_set->ready_connections[i]= 
          poll_set->ready_connections[num_ready_connections - 1];
        poll_set->ready_connections[num_ready_connections - 1]= NULL;
        poll_set->num_ready_connections= num_ready_connections - 1;
        break;
      }
    }
  }
  for (i= 0; i < poll_set->num_allocated_connections; i++)
  {
    poll_conn= poll_set->poll_connections[i];
    if (poll_conn && poll_conn->fd == fd)
    {
      found_index= i;
      break;
    }
  }
  if (found_index == MAX_POLL_SET_CONNECTIONS)
    return IC_ERROR_NOT_FOUND_IN_POLL_SET;
  ic_free(poll_conn);
  poll_set->num_poll_connections--;
  if (poll_set->use_compact_array)
  {
    poll_set->poll_connections[found_index]=
      poll_set->poll_connections[poll_set->num_poll_connections];
    poll_set->poll_connections[poll_set->num_poll_connections]= NULL;
  }
  else
  {
    poll_set->poll_connections[found_index]= NULL;
  }
  *index_removed= found_index;
  return 0;
}

static const IC_POLL_CONNECTION*
get_next_connection(IC_POLL_SET *ext_poll_set)
{
  IC_POLL_CONNECTION *poll_conn;
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  guint32 num_ready_connections= poll_set->num_ready_connections;

  g_assert(poll_set->poll_scan_ongoing);
  if (num_ready_connections == 0)
  {
    poll_set->poll_scan_ongoing= FALSE;
    return NULL;
  }
  num_ready_connections--;
  poll_conn= poll_set->ready_connections[num_ready_connections];
  poll_set->num_ready_connections= num_ready_connections;
  return poll_conn;
}

static gboolean
is_poll_set_full(IC_POLL_SET *ext_poll_set)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  if (poll_set->num_allocated_connections <= poll_set->num_poll_connections)
    return TRUE;
  return FALSE;
}

static void
set_common_methods(IC_INT_POLL_SET *poll_set)
{
  poll_set->poll_ops.ic_get_next_connection= get_next_connection;
  poll_set->poll_ops.ic_free_poll_set= free_poll_set;
  poll_set->poll_ops.ic_is_poll_set_full= is_poll_set_full;
}

#ifdef HAVE_EPOLL_CREATE
#include <sys/epoll.h>
static int
epoll_poll_set_add_connection(IC_POLL_SET *ext_poll_set, int fd,
                              void *user_obj)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index= 0;
  struct epoll_event add_event;

  if ((ret_code= add_poll_set_member(poll_set, fd, user_obj, &index)))
    return ret_code;

  ic_zero(&add_event, sizeof(struct epoll_event));
  add_event.events= EPOLLIN;
  add_event.data.fd= fd;
  add_event.data.u32= index;
  if ((ret_code= epoll_ctl(poll_set->poll_set_fd,
                           EPOLL_CTL_ADD, fd, &add_event)))
  {
    ret_code= ic_get_last_error();
    remove_poll_set_member(poll_set, fd, &index);
    return ret_code;
  }
  return 0;
}

static int
epoll_poll_set_remove_connection(IC_POLL_SET *ext_poll_set, int fd)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index;
  struct epoll_event delete_event;

  if ((ret_code= remove_poll_set_member(poll_set, fd, &index)))
    return ret_code;
  /*
    epoll specific code, set-up data for epoll_ctl call and handle
    error codes properly.
  */
  ic_zero(&delete_event, sizeof(struct epoll_event));
  delete_event.events= EPOLLIN;
  delete_event.data.u32= index;
  delete_event.data.fd= fd;
  if ((ret_code= epoll_ctl(poll_set->poll_set_fd,
                           EPOLL_CTL_DEL, fd, &delete_event)))
  {
    ret_code= ic_get_last_error();
    if (ret_code == ENOENT)
    {
      /*
        Socket was already gone so purpose was achieved although in a
        weird manner.
      */
      return 0;
    }
    /* Tricky situation, we have removed the fd already and for some
       reason we were not successful in removing a connection from the
       epoll object. This is a serious error which should lead to a
       drop of the entire poll set since it's no longer guaranteed to
       function correctly. We start by putting in an abort as the
       manner of handling this error.
    */
    abort();
    return ret_code;
  }
  return 0;
}

static int
epoll_check_poll_set(IC_POLL_SET *ext_poll_set, int ms_time)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int i, ret_code;
  guint32 index;
  struct epoll_event *rec_event=
    (struct epoll_event*)poll_set->impl_specific_ptr;
  IC_POLL_CONNECTION *poll_conn;

  if ((ret_code= epoll_wait(poll_set->poll_set_fd,
                            rec_event,
                            (int)poll_set->num_allocated_connections,
                            ms_time)) < 0)
  {
    ret_code= ic_get_last_error();
    if (ret_code == EINTR)
    {
      /*
        We were interrupted before anything arrived, report as nothing
        received.
      */
      ret_code= 0;
    }
    else
    {
      poll_set->poll_scan_ongoing= FALSE;
      return ret_code;
    }
  }
  /*
    We need to go through all the received events and set up the internal
    data structure such that we can handle get_next_connection calls
    properly.
  */
  for (i= 0; i < ret_code; i++)
  {
    index= (guint32)rec_event[i].data.u32;
    poll_conn= poll_set->poll_connections[index];
    poll_set->ready_connections[i]= poll_conn;
    poll_conn->ret_code= 0;
  }
  poll_set->num_ready_connections= (guint32)ret_code;
  poll_set->poll_scan_ongoing= TRUE;
  return 0;
}

IC_POLL_SET* ic_create_poll_set()
{
  IC_INT_POLL_SET *poll_set;
  int epoll_fd;

  if ((epoll_fd= epoll_create(MAX_POLL_SET_CONNECTIONS)) < 0)
  {
    ic_printf("Failed to allocate an epoll fd");
    return NULL;
  }
  if (alloc_poll_set(&poll_set))
  {
    ic_close_socket(epoll_fd);
    return NULL;
  }
  if (!(poll_set->impl_specific_ptr= ic_calloc(
        sizeof(struct epoll_event) * MAX_POLL_SET_CONNECTIONS)))
  {
    free_poll_set((IC_POLL_SET*)poll_set);
    ic_close_socket(epoll_fd);
    return NULL;
  }
  /* epoll has state and a fd to close at free time */
  poll_set->poll_set_fd= epoll_fd;
  poll_set->need_close_at_free= TRUE;

  set_common_methods(poll_set);
  poll_set->poll_ops.ic_poll_set_add_connection=
    epoll_poll_set_add_connection;
  poll_set->poll_ops.ic_poll_set_remove_connection=
    epoll_poll_set_remove_connection;
  poll_set->poll_ops.ic_check_poll_set= epoll_check_poll_set;
  return (IC_POLL_SET*)poll_set;
}
#else
#ifdef HAVE_PORT_CREATE
#include <port.h>
static int
eventports_poll_set_add_connection(IC_POLL_SET *ext_poll_set,
                                   int fd, void *user_obj)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index= 0;

  if ((ret_code= add_poll_set_member(poll_set, fd, user_obj, &index)))
    return ret_code;

  if ((ret_code= port_associate(poll_set->poll_set_fd,
                                PORT_SOURCE_FD,
                                fd,
                                POLLIN,
                                (void*)index)) < 0)
  {
    ret_code= ic_get_last_error();
    remove_poll_set_member(poll_set, fd, &index);
    return ret_code;
  }
  return 0;
}

static int
eventports_poll_set_remove_connection(IC_POLL_SET *ext_poll_set, int fd)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index= 0;

  if ((ret_code= remove_poll_set_member(poll_set, fd, &index)))
    return ret_code;
  if ((ret_code= port_dissociate(poll_set->poll_set_fd,
                                 PORT_SOURCE_FD,
                                 fd)) < 0)
  {
    ret_code= ic_get_last_error();
    if (ret_code == ENOENT)
    {
      /*
        Socket was already gone so purpose was achieved although in a
        weird manner.
      */
      return 0;
    }
    /* Tricky situation, we have removed the fd already and for some
       reason we were not successful in removing a connection from the
       eventport object. This is a serious error which should lead to a
       drop of the entire poll set since it's no longer guaranteed to
       function correctly. We start by putting in an abort as the
       manner of handling this error.
    */
    abort();
    return ret_code;
  }
  return 0;
}

static int
eventports_check_poll_set(IC_POLL_SET *ext_poll_set, int ms_time)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index;
  timespec_t timeout;
  uint_t num_events= 1, i; /* Wait for at least one event */
  port_event_t *rec_event= (port_event_t*)poll_set->impl_specific_ptr;
  IC_POLL_CONNECTION *poll_conn;

  timeout.tv_sec= 0;
  timeout.tv_nsec= ms_time * 1000000;
  if ((ret_code= port_getn(poll_set->poll_set_fd,
                           rec_event,
                           (uint_t)poll_set->num_allocated_connections,
                           &num_events,
                           &timeout)) < 0)
  {
    ret_code= ic_get_last_error();
    if (ret_code == EINTR || ret_code == ETIME)
    {
      /*
        We were interrupted before anything arrived, report as nothing
        received. Could also be a normal timeout.
      */
      num_events= 0;
    }
    else
    {
      poll_set->poll_scan_ongoing= FALSE;
      return ret_code;
    }
  }
  /*
    We need to go through all the received events and set up the internal
    data structure such that we can handle get_next_connection calls
    properly. For eventports we also need to put back the socket connection
    on the eventport, it needs a new port_associate for every time it
    reports. This obviously creates a lot more headache for error handling.
  */
  for (i= 0; i < num_events; i++)
  {
    index= (guint32)rec_event[i].portev_user;
    poll_conn= poll_set->poll_connections[index];
    poll_set->ready_connections[i]= poll_conn;
    if ((ret_code= port_associate(poll_set->poll_set_fd,
                                  PORT_SOURCE_FD,
                                  poll_conn->fd,
                                  POLLIN,
                                  (void*)index)) < 0)
    {
      ret_code= ic_get_last_error();
      poll_conn->ret_code= ret_code;
    }
    else
      poll_conn->ret_code= 0;
  }
  poll_set->poll_scan_ongoing= TRUE;
  poll_set->num_ready_connections= (guint32)ret_code;
  return 0;
}

IC_POLL_SET* ic_create_poll_set()
{
  IC_INT_POLL_SET *poll_set;
  int eventport_fd;

  if ((eventport_fd= port_create()) < 0)
  {
    ic_printf("Failed to allocate eventport fd");
    return NULL;
  }
  if (alloc_poll_set(&poll_set))
  {
    ic_close_socket(eventport_fd);
    return NULL;
  }
  if (!(poll_set->impl_specific_ptr= ic_calloc(
        sizeof(struct port_event_t) * MAX_POLL_SET_CONNECTIONS)))
  {
    free_poll_set((IC_POLL_SET*)poll_set);
    ic_close_socket(eventport_fd);
    return NULL;
  }

  /* event_ports has state and a fd to close at free time */
  poll_set->poll_set_fd= eventport_fd;
  poll_set->need_close_at_free= TRUE;

  set_common_methods(poll_set);
  poll_set->poll_ops.ic_poll_set_add_connection=
    eventports_poll_set_add_connection;
  poll_set->poll_ops.ic_poll_set_remove_connection=
    eventports_poll_set_remove_connection;
  poll_set->poll_ops.ic_check_poll_set= eventports_check_poll_set;
  return (IC_POLL_SET*)poll_set;
}
#else
#ifdef HAVE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
static int
kqueue_poll_set_add_connection(IC_POLL_SET *ext_poll_set, int fd, void *user_obj)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index= 0;
  struct kevent add_event;

  if ((ret_code= add_poll_set_member(poll_set, fd, user_obj, &index)))
    return ret_code;
  EV_SET(&add_event, fd, EVFILT_READ, EV_ADD, 0, 0, (void*)index);
  if ((ret_code= kevent(poll_set->poll_set_fd,
                        &add_event,
                        1,
                        NULL,
                        0,
                        NULL)) < 0)
  {
    ret_code= ic_get_last_error();
    remove_poll_set_member(poll_set, fd, &index);
    return ret_code;
  }
  return 0;
}

static int
kqueue_poll_set_remove_connection(IC_POLL_SET *ext_poll_set, int fd)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index;
  struct kevent delete_event;

  if ((ret_code= remove_poll_set_member(poll_set, fd, &index)))
    return ret_code;
  EV_SET(&delete_event, fd, EVFILT_READ, EV_DELETE, 0, 0, (void*)NULL);
  if ((ret_code= kevent(poll_set->poll_set_fd,
                        &delete_event,
                        1,
                        NULL,
                        0,
                        NULL)) < 0)
  {
    ret_code= ic_get_last_error();
    if (ret_code == ENOENT)
    {
      /*
        Socket was already gone so purpose was achieved although in a
        weird manner.
      */
      return 0;
    }
    /* Tricky situation, we have removed the fd already and for some
       reason we were not successful in removing a connection from the
       kqueue object. This is a serious error which should lead to a
       drop of the entire poll set since it's no longer guaranteed to
       function correctly. We start by putting in an abort as the
       manner of handling this error.
    */
    abort();
    return ret_code;
  }
  return 0;
}

static int
kqueue_check_poll_set(IC_POLL_SET *ext_poll_set, int ms_time)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int i, ret_code;
  guint32 index;
  struct timespec timeout;
  struct kevent *rec_event= (struct kevent*)poll_set->impl_specific_ptr;
  IC_POLL_CONNECTION *poll_conn;

  timeout.tv_sec= 0;
  timeout.tv_nsec= ms_time * 1000000;
  if ((ret_code= kevent(poll_set->poll_set_fd,
                        NULL,
                        0,
                        rec_event,
                        (int)poll_set->num_allocated_connections,
                        &timeout)) < 0)
  {
    ret_code= ic_get_last_error();
    if (ret_code == EINTR)
    {
      /*
        We were interrupted before anything arrived, report as nothing
        received.
      */
      ret_code= 0;
    }
    else
    {
      poll_set->poll_scan_ongoing= FALSE;
      return ret_code;
    }
  }
  /*
    We need to go through all the received events and set up the internal
    data structure such that we can handle get_next_connection calls
    properly.
  */
  for (i= 0; i < ret_code; i++)
  {
    index= (guint32)rec_event[i].udata;
    poll_conn= poll_set->poll_connections[index];
    poll_set->ready_connections[i]= poll_conn;
    poll_conn->ret_code= 0;
  }
  poll_set->num_ready_connections= (guint32)ret_code;
  poll_set->poll_scan_ongoing= TRUE;
  return 0;
}

IC_POLL_SET* ic_create_poll_set()
{
  int kqueue_fd;
  IC_INT_POLL_SET *poll_set;

  if ((kqueue_fd= kqueue()) < 0)
  {
    ic_printf("Failed to allocate a kqueue fd");
    return NULL;
  }
  if (alloc_poll_set(&poll_set))
  {
    ic_close_socket(kqueue_fd);
    return NULL;
  }
  if (!(poll_set->impl_specific_ptr= ic_calloc(
        sizeof(struct kevent) * MAX_POLL_SET_CONNECTIONS)))
  {
    free_poll_set((IC_POLL_SET*)poll_set);
    ic_close_socket(kqueue_fd);
    return NULL;
  }
  /* event_ports has state and a fd to close at free time */
  poll_set->poll_set_fd= kqueue_fd;
  poll_set->need_close_at_free= TRUE;
  poll_set->use_compact_array= FALSE;

  set_common_methods(poll_set);
  poll_set->poll_ops.ic_poll_set_add_connection=
    kqueue_poll_set_add_connection;
  poll_set->poll_ops.ic_poll_set_remove_connection=
    kqueue_poll_set_remove_connection;
  poll_set->poll_ops.ic_check_poll_set= kqueue_check_poll_set;
  return (IC_POLL_SET*)poll_set;
}
#else
#ifdef HAVE_IO_COMPLETION
static int
io_comp_poll_set_add_connection(IC_POLL_SET *ext_poll_set, int fd,
                                void *user_obj)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  return 0;
}

static int
io_comp_poll_set_remove_connection(IC_POLL_SET *ext_poll_set, int fd)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  return 0;
}

static int
io_comp_check_poll_set(IC_POLL_SET *ext_poll_set, int ms_time)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  return 0;
}

IC_POLL_SET* ic_create_poll_set()
{
  IC_INT_POLL_SET *poll_set;

  if (alloc_poll_set(&poll_set))
  {
    ic_printf("Failed to allocate an IO completion");
    return NULL;
  }
  set_common_methods(poll_set);
  poll_set->poll_ops.ic_poll_set_add_connection=
    io_comp_poll_set_add_connection;
  poll_set->poll_ops.ic_poll_set_remove_connection=
    io_comp_poll_set_remove_connection;
  poll_set->poll_ops.ic_check_poll_set= io_comp_check_poll_set;
  return (IC_POLL_SET*)poll_set;
}
#else
#ifdef HAVE_POLL
#include <poll.h>
static int
poll_poll_set_add_connection(IC_POLL_SET *ext_poll_set, int fd, void *user_obj)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index= 0;
  struct pollfd *poll_fd_array= (struct pollfd *)poll_set->impl_specific_ptr;

  if ((ret_code= add_poll_set_member(poll_set, fd, user_obj, &index)))
    return ret_code;
  poll_fd_array[index].fd= fd;
  return 0;
}

static int
poll_poll_set_remove_connection(IC_POLL_SET *ext_poll_set, int fd)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 index= 0;
  struct pollfd *poll_fd_array= (struct pollfd *)poll_set->impl_specific_ptr;

  if ((ret_code= remove_poll_set_member(poll_set, fd, &index)))
    return ret_code;
  poll_fd_array[index].fd= poll_fd_array[poll_set->num_poll_connections].fd;
  poll_fd_array[poll_set->num_poll_connections].fd= 0;
  return 0;
}

static int
poll_check_poll_set(IC_POLL_SET *ext_poll_set, int ms_time)
{
  IC_INT_POLL_SET *poll_set= (IC_INT_POLL_SET*)ext_poll_set;
  int ret_code;
  guint32 i, num_ready_connections;
  IC_POLLFD_TYPE *poll_fd_array= (IC_POLLFD_TYPE*)poll_set->impl_specific_ptr;

  poll_set->num_ready_connections= 0;
  poll_set->poll_scan_ongoing= TRUE;
  do
  {
    if ((ret_code= ic_poll((IC_POLLFD_TYPE*)poll_set->impl_specific_ptr,
                         poll_set->num_poll_connections,
                         ms_time)) > 0)
    {
      for (i= 0; i < poll_set->num_poll_connections; i++)
      {
        if (poll_fd_array[i].revents == POLLIN)
        {
          num_ready_connections= poll_set->num_ready_connections;
          poll_set->ready_connections[num_ready_connections]=
            poll_set->poll_connections[i];
          poll_set->num_ready_connections= num_ready_connections + 1;
          poll_set->poll_connections[i]->ret_code= 0;
        }
      }
      g_assert((int)poll_set->num_ready_connections == ret_code);
      return 0;
    }
    else if (ret_code == 0)
    {
      /*
        No events found, just return we already have initialised for this
        event.
      */
      return 0;
    }
    else /* ret_code < 0 an error occurred */
    {
      if (ret_code != EINTR)
      {
        poll_set->poll_scan_ongoing= FALSE;
        return ret_code;
      }
      /* Continue waiting some more if interrupted */
    }
  } while (1);
  return 0;
}

IC_POLL_SET* ic_create_poll_set()
{
  IC_INT_POLL_SET *poll_set;
  IC_POLLFD_TYPE poll_fd_array;
  guint32 i;

  if (alloc_poll_set(&poll_set))
  {
    ic_printf("Failed to allocate a poll fd");
    return NULL;
  }
  if (!(poll_set->impl_specific_ptr= ic_calloc(
      sizeof(IC_POLLFD_TYPE) * MAX_POLL_SET_CONNECTIONS)))
  {
    free_poll_set(poll_set);
    return NULL;
  }
  poll_fd_array= (IC_POLLFD_TYPE*)poll_set->impl_specific_ptr;
  for (i= 0; i < MAX_POLL_SET_CONNECTIONS; i++)
  {
    poll_fd_array[i].fd= 0;
    poll_fd_array[i].events= POLLIN;
    poll_fd_array[i].revents= 0;
  }
  /*
    There is no common file descriptor for the poll variant. We only
    store a local state in this object and there are no file descriptors
    to close when freeing this object.
  */
  poll_set->poll_set_fd= (int)-1;
  poll_set->need_close_at_free= FALSE;
  poll_set->use_compact_array= TRUE;

  set_common_methods(poll_set);
  poll_set->poll_ops.ic_poll_set_add_connection=
    poll_poll_set_add_connection;
  poll_set->poll_ops.ic_poll_set_remove_connection=
    poll_poll_set_remove_connection;
  poll_set->poll_ops.ic_check_poll_set= poll_check_poll_set;
  return (IC_POLL_SET*)poll_set;
}
#endif
#endif
#endif
#endif
#endif
