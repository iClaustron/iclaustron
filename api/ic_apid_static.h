/* Copyright (C) 2009-2010 iClaustron AB

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

/*
  Definition of static functions used in more than one implementation file
  in the iClaustron Data API. Also global variables used to start programs
  using the iClaustron Data API.
*/

gchar *ic_glob_cs_server_name= "127.0.0.1";
gchar *ic_glob_cs_server_port= IC_DEF_CLUSTER_SERVER_PORT_STR;
gchar *ic_glob_cs_connectstring= NULL;
gchar *ic_glob_version_path= IC_VERSION_STR;
guint32 ic_glob_node_id= 0;
guint32 ic_glob_cs_timeout= 10;
guint32 ic_glob_num_threads= 1;
guint32 ic_glob_use_iclaustron_cluster_server= 1;
guint32 ic_glob_daemonize= 1;
guint32 ic_glob_byte_order= 0;

/* Some declarations needed to reach functions inside Modules */
static int map_id_to_send_node_connection(IC_INT_APID_GLOBAL *apid_global,
                                 guint32 cluster_id,
                                 guint32 node_id,
                                 IC_SEND_NODE_CONNECTION **send_node_conn);

/* Initialise function pointers in Message Logic Modules */
static void initialize_message_func_array();
/* Start one receive thread */
static int start_receive_thread(IC_INT_APID_GLOBAL *apid_global);
/* Start send threads */
static int start_send_threads(IC_INT_APID_GLOBAL *apid_global);
/* Close listen server threads at close down of Data API */
static void close_listen_server_thread(
          IC_LISTEN_SERVER_THREAD *listen_server_thread,
          IC_INT_APID_GLOBAL *apid_global);

/* Internal function to start heartbeat thread */
static int start_heartbeat_thread(IC_INT_APID_GLOBAL *apid_global,
                                  IC_THREADPOOL_STATE *tp_state);
/* Internal function to handle node connection failure */
static int node_failure_handling(IC_SEND_NODE_CONNECTION *send_node_conn);

/* Internal function to send a NDB message */
static int ndb_send(IC_SEND_NODE_CONNECTION *send_node_conn,
                    IC_SOCK_BUF_PAGE *first_page_to_send,
                    gboolean force_send);

/* Internal function to get NDB message header size */
static guint32 get_ndb_message_header_size(
                    IC_SEND_NODE_CONNECTION *send_node_conn);

/* Internal function to prepare NDB message for sending */
static guint32 fill_ndb_message_header(IC_SEND_NODE_CONNECTION *send_node_conn,
                                       guint32 message_number,
                                       guint32 message_priority,
                                       guint32 sender_module_id,
                                       guint32 receiver_mdoule_id,
                                       guint32 *start_message,
                                       guint32 main_message_size,
                                       guint32 *main_message,
                                       guint32 num_segments,
                                       guint32 *segment_ptrs[3],
                                       guint32 segment_lens[3]);
