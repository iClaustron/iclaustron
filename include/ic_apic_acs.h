/* Copyright (C) 2007-2012 iClaustron AB

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

#ifndef IC_APIC_ACS_H
#define IC_APIC_ACS_H

typedef struct ic_api_config_server IC_API_CONFIG_SERVER;
struct ic_api_cluster_operations
{
  /* Set error code and line number of error */
  void (*ic_set_error_line) (IC_API_CONFIG_SERVER *apic,
                             guint32 error_line);
  /* Fill an error buffer with information about error */
  gchar* (*ic_fill_error_buffer) (IC_API_CONFIG_SERVER *apic,
                                  int error_code,
                                  gchar *error_buffer);
  /*
     Check if we're running towards iClaustron Cluster Server or
     towards NDB management server (mostly for debugging)
  */
  gboolean (*ic_use_iclaustron_cluster_server) (IC_API_CONFIG_SERVER *apic);
  /* Get error string for special errors, not set for all errors */
  const gchar* (*ic_get_error_str) (IC_API_CONFIG_SERVER *apic);

  /*
    Get dynamic port number from Cluster Server by using get connection
    parameter protocol, deliver the port as a 

    When the user haven't specified a port number to use for the node to connect
    to, the port number will be set when the node starts up, the port number
    will be communicated to the Cluster Server, so we need to retrieve it from
    there in the client part of the send thread connect phase. This is handled
    by the get_dynamic_port_number method.
  */
  int (*ic_get_dynamic_port_number) (IC_API_CONFIG_SERVER *apic,
                                     void *tp_state,
                                     void *thread_state,
                                     guint32 cluster_id,
                                     guint32 my_nodeid,
                                     guint32 other_nodeid,
                                     guint32 *port_number);

  /*
    This function gets the configuration for a set of clusters from the
    Cluster Server(s). It allocates the node id in each of those
    clusters. Thus one process can be part of many clusters but it has
    to allocate the same node id in all of them.
  */
  int (*ic_get_config) (IC_API_CONFIG_SERVER *apic,
                        IC_CLUSTER_CONNECT_INFO **clu_info,
                        guint32 node_id,
                        guint32 timeout);
  /*
    The following methods are used to retrieve information from the
    configuration after a successful execution of the ic_get_config
    method above such that the process has the entire configuration
    at hand.

    There is one method to get the struct describing a full cluster
    configuration, there are also methods to retrieve the configuration
    object of one node, either of any node type or by specified node
    type.
  */
  IC_CLUSTER_CONFIG* (*ic_get_cluster_config)
       (IC_API_CONFIG_SERVER *apic, guint32 cluster_id);

  IC_CLUSTER_CONFIG** (*ic_get_all_cluster_config)
       (IC_API_CONFIG_SERVER *apic);
  guint32 (*ic_get_max_cluster_id) (IC_API_CONFIG_SERVER *apic);

  guint32 (*ic_get_node_id_from_name) (IC_API_CONFIG_SERVER *apic,
                                       guint32 cluster_id,
                                       const IC_STRING *node_name);

  guint32 (*ic_get_cluster_id_from_name) (IC_API_CONFIG_SERVER *apic,
                                          const IC_STRING *cluster_name);

  gchar* (*ic_get_node_object)
       (IC_API_CONFIG_SERVER *apic, guint32 cluster_id, guint32 node_id);

  gchar* (*ic_get_typed_node_object)
       (IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
        guint32 node_id, IC_NODE_TYPES node_type);

  IC_SOCKET_LINK_CONFIG* (*ic_get_communication_object)
      (IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
       guint32 first_node_id, guint32 second_node_id);

  /* Method used to release all memory allocated for the configuration */
  void (*ic_free_config) (IC_API_CONFIG_SERVER *apic);
};
typedef struct ic_api_cluster_operations IC_API_CLUSTER_OPERATIONS;

struct ic_api_config_server
{
  IC_API_CLUSTER_OPERATIONS api_op;
  guint32 max_cluster_id;
};

#define REC_BUF_SIZE 256
struct ic_api_cluster_connection
{
  gchar **cluster_server_ips;
  gchar **cluster_server_ports;
  IC_CONNECTION **cluster_srv_conns;
  guint32 *cs_nodeid;
  guint32 num_cluster_servers;
  guint32 tail_index;
  guint32 head_index;
  gchar rec_buf[REC_BUF_SIZE];
};
typedef struct ic_api_cluster_connection IC_API_CLUSTER_CONNECTION;

IC_API_CONFIG_SERVER*
ic_create_api_cluster(IC_API_CLUSTER_CONNECTION *cluster_conn,
                      gboolean use_iclaustron_cluster_server);
#endif
