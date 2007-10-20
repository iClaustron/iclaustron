/* Copyright (C) 2007 iClaustron AB

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

#include <ic_comm.h>
static gboolean
get_sock_buf_page(struct ic_sock_buf *buf,
                  struct ic_sock_buf_page **page)
{
  struct ic_sock_buf_page *first_page;
  g_mutex_lock(buf->ic_buf_mutex);

  /* Retrieve first object in a doubly linked list */
  first_page= buf->first_page;
  if (!first_page)
    return TRUE;
  *page= first_page;
  buf->first_page= first_page->next_sock_buf_page;
  if (buf->first_page)
    buf->first_page->prev_sock_buf_page= 0;

  /* Initialise the returned page object */
  first_page->prio_ref[0]= (char*)first_page;
  first_page->prio_ref[1]= first_page->prio_ref[0] + HIGH_PRIO_BUF_SIZE;
  first_page->prio_size[0]= first_page->prio_size[1]= 0;
  first_page->next_sock_buf_page= 0;
  first_page->prev_sock_buf_page= 0;
  g_mutex_unlock(buf->ic_buf_mutex);
  return FALSE;
}

static gboolean
return_sock_buf_page(struct ic_sock_buf *buf,
                     struct ic_sock_buf_page *page)
{
  struct ic_sock_buf_page *prev_first_page;
  g_mutex_lock(buf->ic_buf_mutex);
  /* Return page to doubly linked list at first page */
  prev_first_page= buf->first_page;
  buf->first_page= page;
  page->next_sock_buf_page= prev_first_page;
  page->prev_sock_buf_page= 0;
  if (prev_first_page)
    prev_first_page->prev_sock_buf_page= page;
  g_mutex_unlock(buf->ic_buf_mutex);
  return FALSE;
}

struct ic_sock_buf*
ic_create_socket_membuf(guint32 page_size,
                        guint32 no_of_pages)
{
  char *ptr;
  struct ic_sock_buf *buf;

  /* WORK TODO LEFT */
  buf= malloc(sizeof(struct ic_sock_buf));
  ptr= malloc(page_size * no_of_pages +
              (no_of_pages * sizeof(struct ic_sock_buf_page)));
  buf->sock_buf_op.ic_get_conn_buf= get_sock_buf_page;
  buf->sock_buf_op.ic_return_conn_buf= return_sock_buf_page;
  return 0;
}

