/* Copyright (C) 2008-2013 iClaustron AB

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

#ifndef IC_POLL_SET_H
#define IC_POLL_SET_H
struct ic_poll_connection
{
  int fd;
  guint32 index;
  void *user_obj;
  int ret_code;
};
typedef struct ic_poll_connection IC_POLL_CONNECTION;

struct ic_poll_operations
{
  /*
    The poll set implementation isn't multi-thread safe. It's intended to be
    used within one thread, the intention is that one can have several
    poll sets, but only one per thread. Thus no mutexes are needed to
    protect the poll set.

    ic_poll_set_add_connection is used to add a socket connection to the
    poll set, it requires only the file descriptor and a user object of
    any kind. The poll set implementation will ensure that this file
    descriptor is checked together with the other file descriptors in
    the poll set independent of the implementation in the underlying OS.

    ic_poll_set_remove_connection is used to remove the file descriptor
    from the poll set.

    ic_check_poll_set is the method that goes to check which socket
    connections are ready to receive.

    ic_get_next_connection is used in a loop where it is called until it
    returns NULL after a ic_check_poll_set call, the output from
    ic_get_next_connection is prepared already at the time of the
    ic_check_poll_set call. ic_get_next_connection will return a
    IC_POLL_CONNECTION object. It is possible that ic_check_poll_set
    can return without error whereas the IC_POLL_CONNECTION can still
    have an error in the ret_code in the object. So it is important to
    both check this return code as well as the return code from the
    call to ic_check_poll_set (this is due to the implementation using
    eventports on Solaris).

    ic_free_poll_set is used to free the poll set, it will also if the
    implementation so requires close any file descriptor of the poll
    set.

    ic_is_poll_set_full can be used to check if there is room for more
    socket connections in the poll set. The poll set has a limited size
    (currently set to 1024) set by a compile time parameter.
  */
  int (*ic_poll_set_add_connection)    (IC_POLL_SET *poll_set,
                                        int fd,
                                        void *user_obj);
  int (*ic_poll_set_remove_connection) (IC_POLL_SET *poll_set,
                                        int fd);
  int (*ic_check_poll_set)             (IC_POLL_SET *poll_set,
                                        int ms_time);
  const IC_POLL_CONNECTION*
      (*ic_get_next_connection)        (IC_POLL_SET *poll_set);
  void (*ic_free_poll_set)             (IC_POLL_SET *poll_set);
  gboolean (*ic_is_poll_set_full)      (IC_POLL_SET *poll_set);
};
typedef struct ic_poll_operations IC_POLL_OPERATIONS;

/* Creates a new poll set */
IC_POLL_SET* ic_create_poll_set();

struct ic_poll_set
{
  IC_POLL_OPERATIONS poll_ops;
};
#endif
