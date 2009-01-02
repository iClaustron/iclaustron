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

#ifndef IC_SOCK_BUF_H
#define IC_SOCK_BUF_H
/*
  Definitions for the socket buffer pool
*/
#define PRIO_LEVELS 2
#define MAX_ALLOC_SEGMENTS 8
#define HIGH_PRIO_BUF_SIZE 128
typedef struct ic_sock_buf_operations IC_SOCK_BUF_OPERATIONS;
typedef struct ic_sock_buf_page IC_SOCK_BUF_PAGE;
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
       IC_SOCK_BUF_PAGE **free_pages,
       guint32 num_pages);
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
  We want to ensure that the buffer is 64 bytes to avoid false cache sharing
  on most platforms (there are some exceptions using 256 byte cache line sizes
  that we will ignore).
*/
#define IC_SOCK_BUF_PAGE_SIZE 64
struct ic_sock_buf_page
{
  IC_SOCK_BUF_PAGE *next_sock_buf_page;
  IC_SOCK_BUF *sock_buf_container;
  gchar *sock_buf;
  guint32 size;
  /* This is an atomic counter used to keep track of reference count */
  gint ref_count;
  guint32 opaque_area[6];
};

struct ic_sock_buf
{
  IC_SOCK_BUF_OPERATIONS sock_buf_ops;
  IC_SOCK_BUF_PAGE *first_page;
  guint32 page_size;
  guint32 alloc_segments;
  gchar *alloc_segments_ref[MAX_ALLOC_SEGMENTS];
  GMutex *ic_buf_mutex;
};

IC_SOCK_BUF*
ic_create_socket_membuf(guint32 page_size,
                        guint64 no_of_pages);
#endif
