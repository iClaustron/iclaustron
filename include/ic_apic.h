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

#ifndef IC_APIC_H
#define IC_APIC_H

#include <ic_apic_data.h>
#include <ic_apic_load_cfg.h>
#include <ic_apic_acs.h>
#include <ic_apic_rcs.h>
#include <ic_proto_str.h>

/* Initialisation of configuration parameters */
int ic_init_config_parameters();
void ic_print_config_parameters(guint32 mask);
void ic_destroy_conf_hash();

IC_API_CONFIG_SERVER*
ic_get_configuration(IC_API_CLUSTER_CONNECTION *apic,
                     IC_STRING *config_dir,
                     guint32 node_id,
                     guint32 num_cs_servers,
                     gchar **cluster_server_ips,
                     gchar **cluster_server_ports,
                     guint32 cs_timeout,
                     gboolean use_iclaustron_cluster_server,
                     int *error,
                     gchar **err_str);

/* iClaustron Protocol Support */
int ic_send_start_info(IC_CONNECTION *conn,
                       const gchar *program_name,
                       const gchar *version_name,
                       const gchar *grid_name,
                       const gchar *cluster_name,
                       const gchar *node_name);
/* Get connect string from a cluster configuration */
gchar* ic_get_connectstring(IC_CLUSTER_CONFIG *grid_common);
#endif
