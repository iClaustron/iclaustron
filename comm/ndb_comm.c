/* Copyright (C) 2008 iClaustron AB

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
#include <glib.h>

/*
  We handle reception of NDB Signals in one or more separate threads, each
  thread can handle one or more sockets. If there is only one socket to
  handle it can use socket read immediately, otherwise we can use either
  poll, or epoll on Linux or other similar variant on other operating
  system.

  This thread only handles reception of the lowest layer of the NDB Protocol.
  Thus here we get all sections of data and their sizes, the Signal Number
  received, the receiving module (thread in the API, block + thread in the
  data nodes), we also handle any necessary conversion to the byte order
  of our machine. Any tracing and debug information is handled in this
  thread.

  The actual handling of the NDB Signal is handled by the application thread.
  This thread receives a IC_NDB_SIGNAL struct which gives all information
  about the signal, including a reference to the IC_SOCK_BUF_PAGE such that
  the last executer of a signal in a page can return the page to the free
  area.

  We get IC_NDB_SIGNAL structs from a free list of IC_SOCK_BUF_PAGE which uses
  a size which is appropriate for the NDB Signal objects. In the same manner
  we allocate pages to receive into from a free list of IC_SOCK_BUF_PAGE with
  an appropriate page size for receiving into.

  In this manner we don't need to copy any data from the buffer we receive
  the NDB Signal into with the exception of signals that comes from more
  than one recv system call.

  Each application thread has a IC_THREAD_CONNECTION struct which is
  protected by a mutex such that this thread can easily transfer NDB
  Signals to these threads. This thread will acquire the mutex, put the
  signals into a linked list of signals waiting to be executed, check if
  the application thread is hanging on the mutex and if so wake it up
  after releasing the mutex.

  To avoid having to protect also the array of IC_THREAD_CONNECTION we
  preallocate the maximum number of IC_THREAD_CONNECTION we can have
  and set their initial state to not used.

  One problem to resolve is what to do if a thread doesn't handle it's
  signals for some reason. This could happen in an overload situation,
  it could happen also due to programming errors from the application,
  the application might send requests asynchronously and then by mistake
  never call receive to handle the sent requests. To protect the system
  in these cases we need to ensure that the thread isn't allowed to send
  any more requests if he has data waiting to be received. In this way we
  can limit the amount of memory a certain thread can hold up.
  
  In addition as an extra security precaution we can have a thread that
  walks around the IC_THREAD_CONNECTION and if it finds a slow thread it
  moves the signals from its buffers into packed buffers to avoid that
  it holds up lots of memory to contain a short signal. Eventually we
  could also decide to kill the thread from an NDB point of view. This
  would mean drop all signals and set the iClaustron Data Server API in
  such a state that the only function allowed is to deallocate the objects
  allocated in this thread. This does also require a possibility to inform
  DBTC that this thread has been dropped.
*/

#define NUM_RECEIVE_PAGES_ALLOC 2
#define NUM_NDB_SIGNAL_ALLOC 32
#define MIN_NDB_HEADER_SIZE 12

/*
  We get a linked list of IC_SOCK_BUF_PAGE which each contain a reference
  to a IC_NDB_SIGNALS object. We will ensure that these messages are posted
  to the proper thread. They will be put in a linked list of NDB signals
  waiting to be executed by this application thread.
*/
static void
post_ndb_signal(IC_SOCK_BUF_PAGE *ndb_signal_pages,
                IC_SOCK_BUF_PAGE *buf_page,
                IC_THREAD_CONNECTION **thd_conn)
{
}

/*
  A complete signal has been received and is pointed to by read_ptr, we will
  now fill in the IC_NDB_SIGNALS object such that it is efficient to
  execute the query.
*/
static int
create_ndb_signal(gchar *read_ptr,
                  guint32 message_size,
                  IC_SOCK_BUF_PAGE *ndb_signal_page)
{
  return 0;
}

static int
init_ndb_receiver(IC_NDB_RECEIVE_STATE *rec_state,
                  IC_THREAD_CONNECTION *thd_conn,
                  gchar *start_buf,
                  guint32 start_size)
{
  IC_SOCK_BUF_PAGE *buf_page;
  IC_SOCK_BUF *rec_buf_pool= rec_state->rec_buf_pool;
  if (start_size)
  {
    /*
      Before starting the NDB Protocol we might have received the first
      bytes already in a previous read socket call. Initialise the buf_page
      with this information.
    */
    if (!(buf_page= rec_buf_pool->sock_buf_op.ic_get_sock_buf_page(
            rec_buf_pool,
            &thd_conn->free_rec_pages,
            NUM_RECEIVE_PAGES_ALLOC)))
      return 1;
    memcpy(buf_page->sock_buf_page, start_buf, start_size);
    rec_state->buf_page= buf_page;
  }
  return 0;
}

static int
post_ndb_signals(IC_SOCK_BUF_PAGE *ndb_signals,
                 IC_SOCK_BUF_PAGE *receive_page,
                 IC_THREAD_CONNECTION *thd_conn)
{
  return 0;
}

static int
read_message_size(gchar *read_ptr)
{
  return 0;
}

int
ndb_receive(IC_NDB_RECEIVE_STATE *rec_state,
            IC_THREAD_CONNECTION *thd_conn)
{
  IC_CONNECTION *conn= rec_state->conn;
  IC_SOCK_BUF *rec_buf_pool= rec_state->rec_buf_pool;
  IC_SOCK_BUF *message_pool= rec_state->message_pool;
  guint32 read_size= rec_state->read_size;
  gboolean read_header_flag= rec_state->read_header_flag;
  gchar *read_ptr;
  guint32 page_size= rec_buf_pool->page_size;
  guint32 start_size, message_size;
  int ret_code;
  gboolean any_signal_received= FALSE;
  IC_SOCK_BUF_PAGE *buf_page, *new_buf_page;
  IC_SOCK_BUF_PAGE *ndb_signal_page;
  IC_SOCK_BUF_PAGE *first_ndb_signal_page= NULL;
  IC_SOCK_BUF_PAGE *last_ndb_signal_page= NULL;

  g_assert(message_pool->page_size == sizeof(IC_NDB_SIGNAL));

  /*
    We get a receive buffer from the global pool of free buffers.
    This buffer will be returned to the free pool by the last
    thread that executes a message in this pool.
  */
  if (read_size == 0)
  {
    if (!(buf_page= rec_buf_pool->sock_buf_op.ic_get_sock_buf_page(
            rec_buf_pool,
            &thd_conn->free_rec_pages,
            NUM_RECEIVE_PAGES_ALLOC)))
      goto mem_pool_error;
  }
  else
    buf_page= rec_state->buf_page;
  read_ptr= buf_page->sock_buf_page;
  start_size= read_size;
  ret_code= conn->conn_op.ic_read_connection(conn,
                                             read_ptr + read_size,
                                             page_size - read_size,
                                             &read_size);
  if (!ret_code)
  {
    /*
      We received data in the NDB Protocol, now chunk it up in its
      respective NDB Signals and send those NDB Signals to the
      thread that expects them. The actual execution of the NDB
      Signals happens in the thread that the NDB Signal is destined
      for.
    */
    read_size+= start_size;
    /*
      Check that we have at least received the header of the next
      NDB Signal first, this is at least 12 bytes in size in the
      NDB Protocol.
    */
    while (read_size >= MIN_NDB_HEADER_SIZE)
    {
      if (!read_header_flag)
      {
        message_size= read_message_size(read_ptr);
        read_header_flag= TRUE;
      }
      if (message_size > read_size)
        break;
      if (!(ndb_signal_page= message_pool->sock_buf_op.ic_get_sock_buf_page(
              message_pool,
              &thd_conn->free_ndb_signals,
              NUM_NDB_SIGNAL_ALLOC)))
        goto mem_pool_error;
      if (!first_ndb_signal_page)
      {
        first_ndb_signal_page= ndb_signal_page;
        last_ndb_signal_page= ndb_signal_page;
      }
      else
      {
        ndb_signal_page->next_sock_buf_page= last_ndb_signal_page;
        last_ndb_signal_page= ndb_signal_page;
      }
      if ((ret_code= create_ndb_signal(read_ptr,
                                       message_size,
                                       ndb_signal_page)))
        goto end; /* Protocol error discovered */

      any_signal_received= TRUE;
      read_header_flag= FALSE;
      read_size-= message_size;
      read_ptr+= message_size;
    }
    if (read_size > 0)
    {
      /* We received an incomplete NDB Signal */
      if (any_signal_received)
      {
        /*
          At least one signal was received and we need to post
          these NDB Signals and thus we need to transfer the
          incomplete message before posting the signals. When
          the signals have been posted another thread can get
          access to the socket buffer page and even release
          it before we have transferred the incomplete message
          if we post before we transfer the incomplete message.
        */
        if (!(new_buf_page= rec_buf_pool->sock_buf_op.ic_get_sock_buf_page(
                rec_buf_pool,
                &thd_conn->free_rec_pages,
                NUM_RECEIVE_PAGES_ALLOC)))
          goto mem_pool_error;

        memcpy(new_buf_page->sock_buf_page,
               buf_page->sock_buf_page,
               read_size);
        post_ndb_signals(first_ndb_signal_page, buf_page, thd_conn);
        rec_state->buf_page= new_buf_page;
        rec_state->read_size= read_size;
        rec_state->read_header_flag= read_header_flag;
      }
    }
    else
    {
      post_ndb_signals(ndb_signal_page, buf_page, thd_conn);
      rec_state->buf_page= NULL;
      rec_state->read_size= 0;
      rec_state->read_header_flag= FALSE;
    }
    return 0;
  }
  if (ret_code == END_OF_FILE)
    ret_code= 0;
end:
  while (thd_conn->free_rec_pages)
  {
    buf_page= thd_conn->free_rec_pages;
    thd_conn->free_rec_pages= thd_conn->free_rec_pages->next_sock_buf_page;
    rec_buf_pool->sock_buf_op.ic_return_sock_buf_page(rec_buf_pool,
                                                      buf_page);
  }
  while (thd_conn->free_ndb_signals)
  {
    ndb_signal_page= thd_conn->free_ndb_signals;
    thd_conn->free_ndb_signals= thd_conn->free_ndb_signals->next_sock_buf_page;
    message_pool->sock_buf_op.ic_return_sock_buf_page(message_pool,
                                                      ndb_signal_page);
  }
  while (first_ndb_signal_page)
  {
    ndb_signal_page= first_ndb_signal_page;
    first_ndb_signal_page= first_ndb_signal_page->next_sock_buf_page;
    message_pool->sock_buf_op.ic_return_sock_buf_page(message_pool,
                                                      ndb_signal_page);
  }
  return ret_code;
mem_pool_error:
  ret_code= 1;
  goto end;
}

static int
ndb_receive_one_socket(IC_NDB_RECEIVE_STATE *rec_state)
{
  return 0;
}

static void
prepare_real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                           guint32 *send_size,
                           struct iovec *write_vector,
                           guint32 *iovec_size)
{
  IC_SOCK_BUF_PAGE *loc_last_send;
  guint32 loc_send_size= 0, iovec_index= 0;
  DEBUG_ENTRY("prepare_real_send_handling");

  loc_last_send= send_node_conn->first_sbp;
  do
  {
    write_vector[iovec_index].iov_base= loc_last_send->sock_buf_page;
    write_vector[iovec_index].iov_len= loc_last_send->size;
    iovec_index++;
    loc_send_size+= loc_last_send->size;
    loc_last_send= loc_last_send->next_sock_buf_page;
  } while (loc_send_size < MAX_SEND_SIZE &&
           iovec_index < MAX_SEND_BUFFERS &&
           loc_last_send);
  send_node_conn->first_sbp= loc_last_send;
  if (!loc_last_send)
    send_node_conn->last_sbp= NULL;
  *iovec_size= iovec_index;
  *send_size= loc_send_size;
  send_node_conn->queued_bytes-= loc_send_size;
  DEBUG_RETURN_EMPTY;
}

static int
real_send_handling(IC_SEND_NODE_CONNECTION *send_node_conn,
                   guint32 send_size,
                   struct iovec *write_vector,
                   guint32 iovec_size)
{
  return 0;
}

/*
  The application thread has prepared to send a number of items and is now
  ready to send these set of pages. The IC_SEND_NODE_CONNECTION is the
  protected data structure that contains the information about the node
  to which this communication is to be sent to.

  This object keeps track of information which is required for the adaptive
  algorithm to work. So it keeps track of how often we send information to
  this node, how long time pages have been waiting to be sent.

  The workings of the sender is the following, the sender locks the
  IC_SEND_NODE_CONNECTION object, then he puts the pages in the linked list of
  pages to be sent here. Next step is to check if another thread is already
  active sending and hasn't flagged that he's not ready to continue
  sending. If there is a thread already sending we unlock and proceed.

*/
static int
ndb_send(IC_SEND_NODE_CONNECTION *send_node_conn,
         IC_SOCK_BUF_PAGE *first_page_to_send,
         gboolean force_send)
{
  IC_SOCK_BUF_PAGE *last_page_to_send;
  IC_SOCK_BUF_PAGE *first_send, *last_send;
  guint32 send_size= 0;
  guint32 iovec_size;
  struct iovec write_vector[MAX_SEND_BUFFERS];
  gboolean return_imm= TRUE;
  DEBUG_ENTRY("ndb_send");

  /*
    We start by calculating the last page to send and the total send size
    before acquiring the mutex.
  */
  last_page_to_send= first_page_to_send;
  send_size+= last_page_to_send->size;
  while (last_page_to_send->next_sock_buf_page)
  {
    send_size+= last_page_to_send->size;
    last_page_to_send= last_page_to_send->next_sock_buf_page;
  }
  /* Start critical section for sending */
  g_mutex_lock(send_node_conn->mutex);
  if (!send_node_conn->node_up)
  {
    g_mutex_unlock(send_node_conn->mutex);
    return IC_ERROR_NODE_DOWN;
  }
  /* Link the buffers into the linked list of pages to send */
  if (send_node_conn->last_sbp == NULL)
  {
    g_assert(send_node_conn->queued_bytes == 0);
    send_node_conn->first_sbp= first_page_to_send;
    send_node_conn->last_sbp= last_page_to_send;
  }
  else
  {
    send_node_conn->last_sbp->next_sock_buf_page= first_page_to_send;
    send_node_conn->last_sbp= last_page_to_send;
  }
  send_node_conn->queued_bytes+= send_size;
  if (!send_node_conn->send_active)
  {
    return_imm= FALSE;
    send_node_conn->send_active= TRUE;
    prepare_real_send_handling(send_node_conn, &send_size,
                               write_vector, &iovec_size);
  }
  g_mutex_unlock(send_node_conn->mutex);
  /* End critical section for sending */
  if (return_imm)
    return 0;
  if (force_send)
  {
    /* We will send now */
    send_node_conn->last_sent_timers[send_node_conn->last_sent_timer_index]=
      ic_gethrtime();
    send_node_conn->last_sent_timer_index++;
    if (send_node_conn->last_sent_timer_index == MAX_SENT_TIMERS)
      send_node_conn->last_sent_timer_index= 0;
    real_send_handling(send_node_conn, send_size, write_vector, iovec_size);
    /* Handle send done */
    return 0;
  }
  /* Here we will insert adaptive send handling */
  g_assert(FALSE);
  return 0;
}

