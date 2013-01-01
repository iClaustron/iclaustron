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

#ifndef IC_SOCK_BUF_H
#define IC_SOCK_BUF_H
/*
  Definitions for the socket buffer pool
*/
#define PRIO_LEVELS 2
#define MAX_ALLOC_SEGMENTS 8
#define HIGH_PRIO_BUF_SIZE 128
typedef struct ic_sock_buf_operations IC_SOCK_BUF_OPERATIONS;
typedef struct ic_sock_buf IC_SOCK_BUF;

struct ic_sock_buf_operations
{
  /*
    This function retrieves one socket buffer page from the free list.
    It does so by using a local free list provided in the free_pages
    variable, this variable is purely local to the thread and need no
    extra mutex protection. When this free list is empty a page is
    retrieved from the global free list. This allocation actually
    allocates as many pages as is requested in the num_pages variable.
    Thus if num_pages is 10, this routine will use the local free_pages
    free list 90% of the time and every 10th time the function is called
    it will allocate 10 socket buffer pages from the global free list.
  */
  IC_SOCK_BUF_PAGE* (*ic_get_sock_buf_page)
      (IC_SOCK_BUF *buf,
       guint32 buf_size,
       IC_SOCK_BUF_PAGE **free_pages,
       guint32 num_pages_to_preallocate);

  /*
    This function is used when we want to get a page and are willing to
    wait if no page exists. Normally it returns immediately when there
    are pages available.
  */
  IC_SOCK_BUF_PAGE* (*ic_get_sock_buf_page_wait)
      (IC_SOCK_BUF *sock_buf,
       guint32 buf_size,
       IC_SOCK_BUF_PAGE **free_pages,
       guint32 num_pages_to_preallocate,
       guint32 milliseconds_to_wait);

  /*
    This routine is used return socket buffer pages to the global free list.
    It will treat the pointer to the first socket buffer page as the first
    page in a linked list of pages. Thus more than one page at a time can
    be returned to the global free list.
  */
  void (*ic_return_sock_buf_page) (IC_SOCK_BUF *buf,
                                   IC_SOCK_BUF_PAGE *page);

  /*
    If the socket buffer page global free list turns out to be too small,
    it is possible to increase the size by allocating more socket buffer
    pages. This is done in increments rather than reallocating everything.
  */
  int (*ic_inc_sock_buf) (IC_SOCK_BUF *buf, guint64 no_of_pages);
  /*
    This routine frees all socket buffer pages allocated to this global pool.
  */
  void (*ic_free_sock_buf) (IC_SOCK_BUF *buf);
};

/*
  We adapt the size of the page object to 128 bytes which is a common
  size of the second level cacheline size. This will minimize the
  amount of false cacheline sharing. These buffers are very often
  shipped around between different threads.
*/
struct ic_sock_buf_page
{
  IC_SOCK_BUF_PAGE *next_sock_buf_page;
  IC_SOCK_BUF *sock_buf_container;
  gchar *sock_buf;
  guint32 size;
  /* This is an atomic counter used to keep track of reference count */
  gint ref_count;
  guint32 opaque_area[4];
  guint32 buf_area[18];
};

struct ic_sock_buf
{
  IC_SOCK_BUF_OPERATIONS sock_buf_ops;
  IC_SOCK_BUF_PAGE *first_page;
  guint32 page_size;
  guint32 alloc_segments;
  gchar *alloc_segments_ref[MAX_ALLOC_SEGMENTS];
  IC_MUTEX *ic_buf_mutex;
};

/*
  Defining a page size of 0 is a special case. This means that we
  won't allocate any special buffer area. We will instead use the
  buf_area inside the IC_SOCK_BUF_PAGE object as the buffer area.

  We will in this case support storing up to 128 bytes in the
  buffer. This will be done by in either of two ways:
  1) For small buffer sizes we will simply use the extra storage area
     that is part of all IC_SOCK_BUF_PAGE_OBJECTS.
  2) If larger, but still smaller than 128 bytes, then we will allocate
     an extra IC_SOCK_BUF_PAGE object and temporarily convert it to a
     buffer object.
  When returning such an object to the pool we will look at the size to
  see whether the object is using an extra IC_SOCK_BUF_PAGE object before
  returning the object to the pool.

  Thus normally we will allocate one object from the pool but for
  this special case we will allocate two objects instead.
  We add a parameter to the get_sock_buf call where we specify
  size and thus the pool can decide where to put the buffer, it
  will only be required to be big enough to fit the size. Size 0
  means that we don't need buffer area for the special case and
  is the required value to use for all other cases.
*/
IC_SOCK_BUF*
ic_create_sock_buf(guint32 page_size,
                   guint64 no_of_pages);
#endif
