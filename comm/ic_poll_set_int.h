/* Copyright (C) 2008-2011 iClaustron AB

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

#ifndef IC_POLL_SET_INT_H
#define IC_POLL_SET_INT_H

struct ic_int_poll_set
{
  IC_POLL_OPERATIONS poll_ops;
  /* An array with a link to a description of the socket connection */
  IC_POLL_CONNECTION **poll_connections;
  /*
    An array with a link to a description of the socket connection for
    connections which have data waiting on socket connection.
  */
  IC_POLL_CONNECTION **ready_connections;
  /*
    This variable contains the array of event handlers as needed by the various
    implementations.
  */
  void *impl_specific_ptr;
  /* This is a common fd if one exists */
  int poll_set_fd;
  /* Number of connections in the poll set */
  guint32 num_poll_connections;
  /* Number of connections ready with data not reported yet. */
  guint32 num_ready_connections;
  /* Maximum number of connections we can use in this poll set */
  guint32 num_allocated_connections;
  /* This implementation requires a common fd to be closed at free time */
  gboolean need_close_at_free;
  /*
    The poll implementation requires a compact array representation,
    it isn't vital that the index is the same since it's local to
    this object.
    kqueue, epoll and eventports require the index to be fixed since this
    is supplied to the implementation as part of setting it up.
  */
  gboolean use_compact_array;
  /*
    ic_check_poll_set has been called and we still have more connections
    to report, we haven't reported any NULL on get_next_connection yet.
  */
  gboolean poll_scan_ongoing;
};
typedef struct ic_int_poll_set IC_INT_POLL_SET;
#endif
