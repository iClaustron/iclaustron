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
#include <ic_common.h>

struct ic_ds_connection;

void ic_create_ds_connection(struct ic_ds_connection *conn);

struct ic_ds_operations
{
  int (*ic_set_up_ds_connection) (struct ic_ds_connection *ds_conn);
  int (*ic_close_ds_connection) (struct ic_ds_connection *ds_conn);
  int (*ic_is_conn_established) (struct ic_ds_connection *ds_conn,
                                 gboolean *is_established);
  int (*ic_authenticate_connection) (void *conn_obj);
};

struct ic_ds_connection
{
  guint32 dest_node_id;
  guint32 source_node_id;
  guint32 socket_group;
  struct ic_connection *conn_obj;
  struct ic_ds_operations operations;
};

typedef struct ic_ds_connection IC_DS_CONNECTION;
