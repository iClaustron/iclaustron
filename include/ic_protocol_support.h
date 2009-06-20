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
  Management Protocol.

    - ic_check_buf
      For the cases when we expect a static string we use check_buf

    - ic_check_buf_with_many_int
      For a rare case where multiple integers are expected we use
      this routine to get all the integers in one routine

    - ic_check_buf_with_int
      For the cases when we expect a static string and an integer we
      use check_buf_with_int

    - ic_rec_simple_str
      When we expect a static string and nothing more we use this routine
      as a way to simplify the code.
*/
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
int ic_rec_simple_str(IC_CONNECTION *conn, const gchar *str);

int ic_send_key(IC_CONNECTION *conn,
                const gchar *grid_name,
                const gchar *cluster_name,
                const gchar *node_name);
int ic_send_version(IC_CONNECTION *conn, const gchar *version);
int ic_send_program(IC_CONNECTION *conn, const gchar *program_name);
int ic_send_pid(IC_CONNECTION *conn, GPid pid);
int ic_send_empty_line(IC_CONNECTION *conn);

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
  GPid pid;
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

