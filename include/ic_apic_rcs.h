/* Copyright (C) 2007-2011 iClaustron AB

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

#ifndef IC_APIC_RCS_H
#define IC_APIC_RCS_H

typedef struct ic_run_cluster_server IC_RUN_CLUSTER_SERVER;
struct ic_run_cluster_server_operations
{
  int (*ic_start_cluster_server) (IC_RUN_CLUSTER_SERVER *run_obj);
  gchar* (*ic_fill_error_buffer) (IC_RUN_CLUSTER_SERVER *run_obj,
                                  int error_code,
                                  gchar *error_buffer);
  int (*ic_run_cluster_server) (IC_RUN_CLUSTER_SERVER *run_obj);
  int (*ic_stop_cluster_server) (IC_RUN_CLUSTER_SERVER *run_obj);
  void (*ic_free_run_cluster) (IC_RUN_CLUSTER_SERVER *run_obj);
};
typedef struct ic_run_cluster_server_operations
  IC_RUN_CLUSTER_SERVER_OPERATIONS;

struct ic_run_cluster_server
{
  IC_RUN_CLUSTER_SERVER_OPERATIONS run_op;
};

/*
  The struct ic_run_cluster_server represents the configuration of
  all clusters that the Cluster Server maintains.
*/
IC_RUN_CLUSTER_SERVER*
ic_create_run_cluster(IC_STRING *config_dir,
                      const gchar *process_name,
                      guint32 my_node_id);
#endif
