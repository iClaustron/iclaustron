/* Copyight (C) 2009 iClaustron AB

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
#include <ic_mc.h>
#include <ic_string.h>

/*
 MODULE: Protocol support module
 -------------------------------
  This module implements a number of routines useful for the NDB
  Management Protocol. All NDB Management Protocol actions are implemented
  as Lines ended with Carriage Return with a last line with an empty
  line with only a Carriage Return where the other side is free to respond.

    - ic_check_buf
      For the cases when we expect a static string we use check_buf

    - ic_check_buf_with_many_int
      For a rare case where multiple integers are expected we use
      this routine to get all the integers in one routine

    - ic_check_buf_with_int
      For the cases when we expect a static string and an integer we
      use check_buf_with_int

    - ic_rec_with_cr
      Receive data on socket until Carriage Return found, used by all other
      receive routines below and many other routines.

    - ic_rec_simple_str
      When we expect a static string and nothing more we use this routine
      as a way to simplify the code.

    - ic_rec_simple_str_opt
      Wait before static string, but if something else arrives we'll simply
      return with found set to FALSE and ready to try another string.

    - ic_rec_string
      Wait for fixed string plus a string with variable content

    - ic_rec_number
      Wait for fixed number plus a 32-bit unsigned number

    - ic_rec_int_number
      Wait for fixed number plus a 32-bit signed number

    - ic_rec_long_number
      Wait for fixed number plus a 64-bit unsigned number

    - ic_rec_opt_number
      Wait for fixed number plus a 32-bit unsigned number, however this
      is optional and if not found we'll return 0 in number field.

    - ic_rec_empty_line
      Wait for a Carriage Return, always means reverse order of communication
      in NDB Management Protocol.

    - ic_send_with_cr
      Send a fixed string with a Carriage Return

    - ic_send_with_cr_with_num
      Send a fixed string with a 64-bit unsigned number

    - ic_send_with_cr_composed
      Send an array of strings

    - ic_send_with_cr_two_strings
      Send two strings

    - ic_send_empty_line
      Send empty line

   The send methods are optimised to wait until ic_send_empty_line to actually
   flush the send buffer or when buffer is full.
*/

/* Check buffer routines after ic_rec_with_cr */
int ic_check_buf(gchar *read_buf, guint32 read_size, const gchar *str,
                 int str_len);
int ic_check_buf_with_many_int(gchar *read_buf, guint32 read_size,
                               const gchar *str, int str_len,
                               guint32 num_elements, guint64 *number);
int ic_check_buf_with_int(gchar *read_buf, guint32 read_size,
                          const gchar *str, int str_len, guint64 *number);
int ic_check_buf_with_string(gchar *read_buf, guint32 read_size,
                             const gchar *str, int str_len,
                             IC_STRING **string);

/* Receive routines */
int ic_rec_with_cr(IC_CONNECTION *conn,
                   gchar **rec_buf,
                   guint32 *read_size);
int ic_rec_simple_str(IC_CONNECTION *conn, const gchar *str);
int ic_rec_simple_str_opt(IC_CONNECTION *conn,
                          const gchar *str,
                          gboolean *found);
int ic_rec_string(IC_CONNECTION *conn, const gchar *prefix_str,
                  gchar *read_str);
int ic_rec_number(IC_CONNECTION *conn, const gchar *str, guint32 *number);
int ic_rec_long_number(IC_CONNECTION *conn,
                       const gchar *str,
                       guint64 *number);
int ic_rec_opt_number(IC_CONNECTION *conn, const gchar *str, guint32 *number);
int ic_rec_int_number(IC_CONNECTION *conn, const gchar *str, int *number);
int ic_rec_empty_line(IC_CONNECTION *conn);
int ic_step_back_rec_with_cr(IC_CONNECTION *conn, guint32 read_size);

/* Send routines */
int ic_send_with_cr(IC_CONNECTION *conn, const gchar *buf);
int ic_send_empty_line(IC_CONNECTION *conn);
int ic_send_with_cr_with_num(IC_CONNECTION *conn, const gchar *buf,
                             guint64 number);
int ic_send_with_cr_composed(IC_CONNECTION *conn, const gchar **buf,
                             guint32 num_strings);
int ic_send_with_cr_two_strings(IC_CONNECTION *conn,
                                const gchar *buf1,
                                const gchar *buf2);

/* Data structures used by Process Control protocol */
struct ic_pc_key
{
  IC_STRING grid_name;
  IC_STRING cluster_name;
  IC_STRING node_name;
};
typedef struct ic_pc_key IC_PC_KEY;

struct ic_pc_start;
typedef struct ic_pc_start IC_PC_START;
struct ic_pc_start
{
  IC_PC_KEY key;
  IC_STRING version_string;
  IC_STRING program_name;
  IC_STRING *parameters;
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_PC_START *next_pc_start;
  IC_PID_TYPE pid;
  guint64 start_id;
  guint64 dyn_trans_index;
  guint32 num_parameters;
  gboolean autorestart;
};

struct ic_pc_find
{
  IC_PC_KEY key;
  IC_MEMORY_CONTAINER *mc_ptr;
};
typedef struct ic_pc_find IC_PC_FIND;

