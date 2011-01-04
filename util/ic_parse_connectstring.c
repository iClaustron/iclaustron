/* Copyight (C) 2009-2011 iClaustron AB

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
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_parse_connectstring.h>

static int
analyse_host(IC_CONNECT_STRING *conn_str,
             gchar *start_ptr,
             guint32 curr_len)
{
  gchar *ptr;
  IC_MEMORY_CONTAINER *mc_ptr= conn_str->mc_ptr;

  if (curr_len == 0)
    return IC_ERROR_PARSE_CONNECTSTRING;
  if (conn_str->num_cs_servers == IC_MAX_CLUSTER_SERVERS)
    return IC_ERROR_TOO_MANY_CS_HOSTS;
  if (!(ptr= (gchar*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, curr_len+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(ptr, start_ptr, curr_len);
  ptr[curr_len]= 0;
  conn_str->cs_hosts[conn_str->num_cs_servers]= ptr;
  return 0;
}

static int
analyse_port_number(IC_CONNECT_STRING *conn_str,
                    gchar *start_ptr,
                    guint32 curr_len)
{
  gchar *ptr;
  IC_MEMORY_CONTAINER *mc_ptr= conn_str->mc_ptr;

  if (curr_len == 0)
    return IC_ERROR_PARSE_CONNECTSTRING;
  if (!(ptr= (gchar*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, curr_len+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(ptr, start_ptr, curr_len);
  ptr[curr_len]= 0;
  conn_str->cs_ports[conn_str->num_cs_servers]= ptr;
  return 0;
}

int
ic_parse_connectstring(gchar *connect_string,
                       IC_CONNECT_STRING *conn_str,
                       gchar *cs_server_name,
                       gchar *cs_server_port)
{
  guint32 len, buf_inx, curr_len;
  gboolean read_host, read_port;
  gchar c;
  gchar *start_ptr;
  int ret_code;
  gboolean skip= FALSE;
  IC_MEMORY_CONTAINER *mc_ptr;

  if (!(conn_str->mc_ptr= ic_create_memory_container(1024, 0, FALSE)))
    return IC_ERROR_MEM_ALLOC;
  mc_ptr= conn_str->mc_ptr;
  ret_code= IC_ERROR_MEM_ALLOC;
  if (!(conn_str->cs_hosts= (gchar**)mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
                 IC_MAX_CLUSTER_SERVERS * sizeof(gchar*))))
    goto error;
  if (!(conn_str->cs_ports= (gchar**)mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
                 IC_MAX_CLUSTER_SERVERS * sizeof(gchar*))))
    goto error;
  if (connect_string == NULL)
  {
    /*
      No connect string provided, set based on 
      glob_cluster_server_ip and glob_cluster_server_port which can be
      set as options.
    */
    conn_str->cs_hosts[0]= cs_server_name;
    conn_str->cs_ports[0]= cs_server_port;
    conn_str->num_cs_servers= 1;
    return 0;
  }
  /*
    Initialise parser variables
    We're looking for strings of the type
    myhost1:port1,myhost2:port2
    
    Hostnames can contain a-z,A-Z,0-9 and _ and - and .
    Port numbers can contain 0-9

    Port numbers can be skipped and then the default port number 1186 is
    used.
    : serve as separator between host and port number
    , serve as separator between host-port pairs
  */
  conn_str->num_cs_servers= 0;
  len= strlen(connect_string);
  curr_len= 0;
  buf_inx= 0;
  read_host= TRUE;
  read_port= FALSE;
  start_ptr= connect_string;
  do
  {
    c= connect_string[buf_inx];
    if (c == ',')
    {
      /* Finished this host, now time for next host */
      if (read_host)
      {
        if ((ret_code= analyse_host(conn_str, start_ptr, curr_len)))
          goto error;
        conn_str->cs_ports[conn_str->num_cs_servers]=
          IC_DEF_CLUSTER_SERVER_PORT_STR;
      }
      else
      {
        if ((ret_code= analyse_port_number(conn_str, start_ptr, curr_len)))
          goto error;
      }
      conn_str->num_cs_servers++;
      read_host= TRUE;
      read_port= FALSE;
      start_ptr+= (curr_len + 1);
      curr_len= 0;
      skip= TRUE;
    }
    else if (c == ':')
    {
      if (read_port)
        goto parse_error;
      /* Finished host part now time for a port number */
      if ((ret_code= analyse_host(conn_str, start_ptr, curr_len)))
        goto error;
      read_port= TRUE;
      read_host= FALSE;
      start_ptr+= (curr_len + 1);
      curr_len= 0;
      skip= TRUE;
    }
    else
      curr_len++;
    buf_inx++;
    if (!skip)
    {
      if (read_host)
      {
        if (!((c >= 'a' && c <= 'z') ||
             (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') ||
              c == '_' ||
              c == '-' ||
              c == '.'))
          goto parse_error;
      }
      else if (read_port)
      {
        if (!(c >= '0' && c <= '9'))
          goto parse_error;
      }
    }
    skip= FALSE;
  } while (buf_inx < len);
  if (read_host)
  {
    if ((ret_code= analyse_host(conn_str, start_ptr, curr_len)))
      goto error;
    conn_str->cs_ports[conn_str->num_cs_servers]=
      IC_DEF_CLUSTER_SERVER_PORT_STR;
  }
  else
  {
    if ((ret_code= analyse_port_number(conn_str, start_ptr, curr_len)))
      goto error;
  }
  conn_str->num_cs_servers++;
  return 0;
parse_error:
  ret_code= IC_ERROR_PARSE_CONNECTSTRING;
error:
  mc_ptr->mc_ops.ic_mc_free(conn_str->mc_ptr);
  return ret_code;
}
