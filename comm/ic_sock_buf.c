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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_sock_buf.h>

static void return_sock_buf_page(IC_SOCK_BUF *buf,
                                 IC_SOCK_BUF_PAGE *in_page);
static IC_SOCK_BUF_PAGE* low_get_sock_buf_page(IC_SOCK_BUF *buf,
                            IC_SOCK_BUF_PAGE **free_rec_pages,
                            guint32 num_pages_to_preallocate);

static IC_SOCK_BUF_PAGE*
get_sock_buf_page(IC_SOCK_BUF *buf,
                  guint32 buf_size,
                  IC_SOCK_BUF_PAGE **free_rec_pages,
                  guint32 num_pages_to_preallocate)
{
  IC_SOCK_BUF_PAGE *sock_buf_page, *second_sock_buf_page;
  guint32 small_buf_size;
  gchar *page_start, *buf_start;

  sock_buf_page= low_get_sock_buf_page(buf,
                                       free_rec_pages,
                                       num_pages_to_preallocate);
  if (buf_size == 0 || !sock_buf_page)
    return sock_buf_page;
  ic_require(buf->page_size == 0);
  if (buf_size > IC_STD_CACHE_LINE_SIZE)
    return sock_buf_page;
  page_start= (gchar*)sock_buf_page;
  buf_start= (gchar*)&sock_buf_page->buf_area;
  small_buf_size= (page_start + IC_STD_CACHE_LINE_SIZE) - buf_start;
  if (buf_size <= small_buf_size)
  {
    sock_buf_page->sock_buf= (gchar*)&sock_buf_page->buf_area[0];
    return sock_buf_page;
  }
  second_sock_buf_page= low_get_sock_buf_page(buf,
                                              free_rec_pages,
                                              num_pages_to_preallocate);
  if (!second_sock_buf_page)
  {
    return_sock_buf_page(buf, sock_buf_page);
    return NULL;
  }
  sock_buf_page->size= IC_STD_CACHE_LINE_SIZE;
  sock_buf_page->sock_buf= (gchar*)second_sock_buf_page;
  return sock_buf_page;
}

static IC_SOCK_BUF_PAGE*
low_get_sock_buf_page(IC_SOCK_BUF *buf,
                      IC_SOCK_BUF_PAGE **free_rec_pages,
                      guint32 num_pages_to_preallocate)
{
  guint32 i;
  IC_SOCK_BUF_PAGE *first_page, *next_page, *last_page;

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
      first_page->next_sock_buf_page= NULL;
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
  for (i= 0; i < num_pages_to_preallocate && next_page; i++)
  {
    next_page->size= 0;
    next_page->ref_count= 0;
    last_page= next_page;
    next_page= next_page->next_sock_buf_page;
  }
  /* Initialise local free list */
  if (free_rec_pages && first_page)
  {
    /*
      The preallocated list if any must be ended with a NULL pointer.
      It must be also initialised to the first preallocated page. It's
      important to not perform any action at all if no pages have been
      preallocated which is noted by that last_page is different than
      the first_page, this means we were successful in preallocating at
      least one page.
    */
    if (last_page != first_page)
    {
      *free_rec_pages= first_page->next_sock_buf_page;
      last_page->next_sock_buf_page= NULL;
    }
  }
  /* Unlink first page from list */
  if (first_page)
    first_page->next_sock_buf_page= NULL;
  return first_page;
}

static IC_SOCK_BUF_PAGE*
get_sock_buf_page_wait(IC_SOCK_BUF *sock_buf,
                       guint32 buf_size,
                       IC_SOCK_BUF_PAGE **free_pages,
                       guint32 num_pages_to_preallocate,
                       guint32 milliseconds_to_wait)
{
  gboolean first= TRUE;
  IC_SOCK_BUF_PAGE *loc_page;
  IC_TIMER start_time, current_time;

  while (!(loc_page= get_sock_buf_page(
                  sock_buf,
                  buf_size,
                  free_pages,
                  num_pages_to_preallocate)))
  {
    if (first)
    {
      first= FALSE;
      start_time= ic_gethrtime();
    }
    else
    {
      current_time= ic_gethrtime();
      if (((guint32)ic_millis_elapsed(start_time, current_time)) >
          milliseconds_to_wait)
        return NULL;
    }
    ic_microsleep(10000);
  }
  return loc_page;
}

static void
return_sock_buf_page(IC_SOCK_BUF *buf,
                     IC_SOCK_BUF_PAGE *in_page)
{
  IC_SOCK_BUF_PAGE *prev_first_page;
  IC_SOCK_BUF_PAGE *next_page;
  guint32 page_size, this_page_size;
  register IC_SOCK_BUF_PAGE *page= in_page;

  ic_require(page);
  page_size= buf->page_size;
  g_mutex_lock(buf->ic_buf_mutex);
  do
  {
    /* Return page to linked list at first page */
    prev_first_page= buf->first_page;
    buf->first_page= page;

    next_page= page->next_sock_buf_page;
    this_page_size= page->size;

    page->next_sock_buf_page= prev_first_page;
    page->ref_count= 0;
    if (page_size == 0)
    {
      if (this_page_size != 0)
      {
        /*
          The page contains a buffer which in reality is a IC_SOCK_BUF_PAGE
          object. We need to convert it back to a IC_SOCK_BUF_PAGE object
          and link it into free list. Put next object as next pointer in
          this object and return this object next.
        */
        page= (IC_SOCK_BUF_PAGE*)page->sock_buf;
        page->next_sock_buf_page= next_page;
        page->sock_buf_container= buf;
        page->size= 0;
        next_page= page;
      }
      page->sock_buf= NULL;
    }
    page= next_page;
  } while (page != NULL);
  g_mutex_unlock(buf->ic_buf_mutex);
}

void
free_sock_buf(IC_SOCK_BUF *buf)
{
  guint32 i;
  guint32 alloc_segments= buf->alloc_segments;

  g_mutex_free(buf->ic_buf_mutex);
  for (i= 0; i < alloc_segments; i++)
    ic_free(buf->alloc_segments_ref[i]);
  ic_free(buf);
}

/*
  Build the linked list of pages we maintain for send and receive
  buffers for iClaustron transporters.
*/
static IC_SOCK_BUF_PAGE*
set_up_pages_in_linked_list(IC_SOCK_BUF *sock_buf_container,
                            gchar *ptr, guint32 page_size,
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
    sock_buf_page_ptr->size= page_size;
    sock_buf_page_ptr->ref_count= 0;
    sock_buf_page_ptr->sock_buf_container= sock_buf_container;
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

  sock_buf_page_size= IC_MAX(IC_STD_CACHE_LINE_SIZE,
                             sizeof(IC_SOCK_BUF_PAGE));
  if (!(ptr= ic_malloc(no_of_pages * (page_size + sock_buf_page_size))))
    return IC_ERROR_MEM_ALLOC;
  last_sock_buf_page= set_up_pages_in_linked_list(buf,
                                                  ptr,
                                                  page_size,
                                                  no_of_pages,
                                                  sock_buf_page_size);
  g_mutex_lock(buf->ic_buf_mutex);
  if (buf->alloc_segments < MAX_ALLOC_SEGMENTS)
  {
    last_sock_buf_page->next_sock_buf_page= buf->first_page;
    buf->first_page= (IC_SOCK_BUF_PAGE*)ptr;
    buf->alloc_segments_ref[buf->alloc_segments]= ptr;
    buf->alloc_segments++;
  }
  else
  {
    ic_free(ptr);
    error= IC_ERROR_MEM_ALLOC;
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

  sock_buf_page_size= IC_MAX(IC_STD_CACHE_LINE_SIZE,
                             sizeof(IC_SOCK_BUF_PAGE));
  ic_require(sock_buf_page_size == IC_STD_CACHE_LINE_SIZE);
  if (no_of_pages == 0)
    return NULL;
  if (!(buf= (IC_SOCK_BUF*)ic_malloc(sizeof(IC_SOCK_BUF))))
    return NULL;
  if (!(buf->ic_buf_mutex= g_mutex_new()))
    goto error;
  if (!(ptr= ic_malloc(page_size * no_of_pages +
      (no_of_pages * sock_buf_page_size))))
    goto error;
  last_sock_buf_page= set_up_pages_in_linked_list(buf,
                                                  ptr,
                                                  page_size,
                                                  no_of_pages,
                                                  sock_buf_page_size);
  last_sock_buf_page->next_sock_buf_page= NULL;

  buf->first_page= (IC_SOCK_BUF_PAGE*)ptr;
  buf->alloc_segments_ref[0]= ptr;
  buf->alloc_segments= 1;
  buf->page_size= page_size;

  buf->sock_buf_ops.ic_get_sock_buf_page= get_sock_buf_page;
  buf->sock_buf_ops.ic_get_sock_buf_page_wait= get_sock_buf_page_wait;
  buf->sock_buf_ops.ic_return_sock_buf_page= return_sock_buf_page;
  buf->sock_buf_ops.ic_inc_sock_buf= inc_sock_buf;
  buf->sock_buf_ops.ic_free_sock_buf= free_sock_buf;
  return buf;

error:
  if (buf->ic_buf_mutex)
    g_mutex_free(buf->ic_buf_mutex);
  ic_free(buf);
  return NULL;
}
