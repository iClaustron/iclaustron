/* Copyright (C) 2007, 2008 iClaustron AB

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
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_sock_buf.h>

static IC_SOCK_BUF_PAGE*
get_sock_buf_page(IC_SOCK_BUF *buf,
                  IC_SOCK_BUF_PAGE **free_rec_pages,
                  guint32 num_pages_to_preallocate)
{
  guint32 i;
  IC_SOCK_BUF_PAGE *first_page, *next_page;

  if (free_rec_pages)
  {
    if (*free_rec_pages)
    {
      /*
        We allow the caller to provide a local linked list of free
        objects and thus the caller can fetch objects from the
        global list in batches.
      */
      first_page= *free_rec_pages;
      *free_rec_pages= (*free_rec_pages)->next_sock_buf_page;
      return first_page;
    }
  }
  else
  {
    /* Ignore this parameter if no local free list provided */
    num_pages_to_preallocate= 1;
  }

  g_mutex_lock(buf->ic_buf_mutex);
  /* Retrieve objects in a linked list */
  first_page= buf->first_page;
  next_page= first_page;
  for (i= 0; i < num_pages_to_preallocate && next_page; i++)
    next_page= next_page->next_sock_buf_page;
  buf->first_page= next_page;
  g_mutex_unlock(buf->ic_buf_mutex);

  /* Initialise the returned page objects */
  next_page= first_page;
  for (i= 0; i < num_pages_to_preallocate; i++)
  {
    next_page->size= 0;
    next_page->ref_count= 0;
    next_page= next_page->next_sock_buf_page;
  }
  /* Initialise local free list */
  if (free_rec_pages && first_page)
    *free_rec_pages= first_page->next_sock_buf_page;
  /* Unlink first page and last page points to end of list */
  if (first_page)
    first_page->next_sock_buf_page= 0;
  if (next_page)
    next_page->next_sock_buf_page= 0;
  return first_page;
}


static void
return_sock_buf_page(IC_SOCK_BUF *buf,
                     IC_SOCK_BUF_PAGE *page)
{
  IC_SOCK_BUF_PAGE *prev_first_page;
  IC_SOCK_BUF_PAGE *next_page= page->next_sock_buf_page;

  g_mutex_lock(buf->ic_buf_mutex);
  do
  {
    /* Return page to linked list at first page */
    prev_first_page= buf->first_page;
    buf->first_page= page;
    page->next_sock_buf_page= prev_first_page;
    page= next_page;
    next_page= next_page->next_sock_buf_page;
  } while (page != NULL);
  g_mutex_unlock(buf->ic_buf_mutex);
}

void
free_sock_buf(IC_SOCK_BUF *buf)
{
  guint32 i;
  guint32 alloc_segments= buf->alloc_segments;

  for (i= 0; i < alloc_segments; i++)
    free(buf->alloc_segments_ref[i]);
  free(buf);
}

/*
  Build the linked list of pages we maintain for send and receive
  buffers for iClaustron transporters.
*/
static IC_SOCK_BUF_PAGE*
set_up_pages_in_linked_list(gchar *ptr, guint32 page_size,
                            guint64 no_of_pages, guint32 sock_buf_page_size)
{
  gchar *loop_ptr, *buf_page_ptr, *new_buf_page_ptr;
  IC_SOCK_BUF_PAGE *sock_buf_page_ptr= NULL;
  guint64 i;

  loop_ptr= ptr + (no_of_pages * sock_buf_page_size);
  buf_page_ptr= ptr;
  if (page_size == 0)
    loop_ptr= NULL;
  g_assert(no_of_pages);
  for (i= 0; i < no_of_pages; i++)
  {
    sock_buf_page_ptr= (IC_SOCK_BUF_PAGE*)buf_page_ptr;
    sock_buf_page_ptr->sock_buf= loop_ptr;
    new_buf_page_ptr= (buf_page_ptr + sock_buf_page_size);
    sock_buf_page_ptr->next_sock_buf_page= (IC_SOCK_BUF_PAGE*)new_buf_page_ptr;
    loop_ptr+= page_size;
    buf_page_ptr= new_buf_page_ptr;
  }
  return sock_buf_page_ptr;
}

static int
inc_sock_buf(IC_SOCK_BUF *buf, guint64 no_of_pages)
{
  gchar *ptr;
  guint32 page_size= buf->page_size;
  IC_SOCK_BUF_PAGE *last_sock_buf_page;
  int error= 0;
  guint32 sock_buf_page_size;

  sock_buf_page_size= IC_MAX(64, sizeof(IC_SOCK_BUF_PAGE)); 
  if (!(ptr= ic_malloc(no_of_pages * (page_size + sock_buf_page_size))))
    return 1;
  last_sock_buf_page= set_up_pages_in_linked_list(ptr,
                                                  page_size,
                                                  no_of_pages,
                                                  sock_buf_page_size);
  g_mutex_lock(buf->ic_buf_mutex);
  if (buf->alloc_segments == MAX_ALLOC_SEGMENTS)
  {
    last_sock_buf_page->next_sock_buf_page= buf->first_page;
    buf->first_page= (IC_SOCK_BUF_PAGE*)ptr;
    buf->alloc_segments_ref[buf->alloc_segments]= ptr;
    buf->alloc_segments++;
  }
  else
  {
    free(ptr);
    error= 1;
  }
  g_mutex_unlock(buf->ic_buf_mutex);
  return error;
}

IC_SOCK_BUF*
ic_create_sock_buf(guint32 page_size,
                   guint64 no_of_pages)
{
  gchar *ptr;
  IC_SOCK_BUF *buf;
  IC_SOCK_BUF_PAGE *last_sock_buf_page;
  guint32 sock_buf_page_size;

  sock_buf_page_size= IC_MAX(64, sizeof(IC_SOCK_BUF_PAGE)); 
  buf= (IC_SOCK_BUF*)ic_malloc(sizeof(IC_SOCK_BUF));
  if (!(ptr= ic_malloc(page_size * no_of_pages +
      (no_of_pages * sock_buf_page_size))))
  {
    return NULL;
  }
  last_sock_buf_page= set_up_pages_in_linked_list(ptr,
                                                  page_size,
                                                  no_of_pages,
                                                  sock_buf_page_size);
  last_sock_buf_page->next_sock_buf_page= NULL;

  buf->first_page= (IC_SOCK_BUF_PAGE*)ptr;
  buf->alloc_segments_ref[0]= ptr;
  buf->alloc_segments= 1;

  buf->sock_buf_ops.ic_get_sock_buf_page= get_sock_buf_page;
  buf->sock_buf_ops.ic_return_sock_buf_page= return_sock_buf_page;
  buf->sock_buf_ops.ic_inc_sock_buf= inc_sock_buf;
  buf->sock_buf_ops.ic_free_sock_buf= free_sock_buf;
  return buf;
}
