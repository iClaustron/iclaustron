/* Copyight (C) 2009-2013 iClaustron AB

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

typedef struct ic_connect_string
{
  gchar **cs_hosts;
  gchar **cs_ports;
  guint32 num_cs_servers;
  IC_MEMORY_CONTAINER *mc_ptr;
} IC_CONNECT_STRING;

int
ic_parse_connectstring(gchar *connect_string,
                       IC_CONNECT_STRING *conn_str,
                       gchar *cs_server_name,
                       gchar *cs_server_port);
