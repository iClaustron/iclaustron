/* Copyright (C) 2007-2010 iClaustron AB

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

#ifndef IC_APIC_INT_H
#define IC_APIC_INT_H

#include <ic_apid.h>

#define IC_VERSION_FILE_LEN 8
#define NO_WAIT 0
#define WAIT_LOCK_INFO 1
#define WAIT_CHANGE_INFO 2

enum ic_communication_type
{
  IC_SOCKET_COMM = 0
};
typedef enum ic_communication_type IC_COMMUNICATION_TYPE;

enum ic_config_entry_change
{
  IC_ONLINE_CHANGE = 0,
  IC_NODE_RESTART_CHANGE = 1,
  IC_ROLLING_UPGRADE_CHANGE = 2,
  IC_ROLLING_UPGRADE_CHANGE_SPECIAL = 3,
  IC_INITIAL_NODE_RESTART = 4,
  IC_CLUSTER_RESTART_CHANGE = 5,
  IC_NOT_CHANGEABLE = 6
};
typedef enum ic_config_entry_change IC_CONFIG_ENTRY_CHANGE;

typedef enum ic_config_data_type
{
  IC_NO_SUCH_CONFIG_DATA_TYPE = 0,
  IC_UINT32 = 1,
  IC_CHARPTR = 2,
  IC_SECTION_TYPE = 3,
  IC_UINT64 = 4,
  IC_CHAR = 5,
  IC_BOOLEAN = 6,
  IC_UINT16 = 7
} IC_CONFIG_DATA_TYPE;

struct ic_config_entry
{
  IC_STRING config_entry_name;
  gchar *config_entry_description;
  guint32 max_value;
  guint32 min_value;
  union
  {
    guint64 default_value;
    gchar *default_string;
  };
  IC_CONFIG_DATA_TYPE data_type;
  guint32 offset;
  /*
    When only one version exists then both these values are 0.
    When a new version of default values is created the
    max_version is set on the old defaults and the new entry
    gets the min_version set to the old max_version + 1 and
    its max_version_used is still 0.
  */
  guint32 min_ic_version_used;
  guint32 max_ic_version_used;
  guint32 min_ndb_version_used;
  guint32 max_ndb_version_used;
  enum ic_config_entry_change change_variant;
  guint32 config_types;
  guint32 config_id;
  gchar is_max_value_defined;
  gchar is_min_value_defined;
  gchar is_boolean;
  gchar is_deprecated;
  gchar is_string_type;
  gchar is_mandatory;
  /*
    For all configurations (nodes and links) we have a key item, it's the node
    id and for a link it is both the links involved.
  */
  gchar is_key;
  /*
    For communication configurations we have defaults that are derived from
    its nodes in some cases. This is specified with this variable.
  */
  gchar is_derived_default;
  gchar mandatory_bit;
  gchar is_not_configurable;
  gchar is_only_iclaustron;
  gchar is_array_value;
  gchar is_not_sent;
};
typedef struct ic_config_entry IC_CONFIG_ENTRY;

struct ic_cluster_config_temp
{
  IC_CLUSTER_CONNECT_INFO **clu_info;
  guint32 size_string_memory;
  gchar *string_memory;
  guint32 num_clusters;
  gboolean init_state;
  IC_STRING cluster_name;
  IC_STRING password;
  guint32 cluster_id;
};
typedef struct ic_cluster_config_temp IC_CLUSTER_CONFIG_TEMP;

struct ic_cs_conf_comment
{
  guint32 num_comments;
  gchar **ptr_comments;
  guint32 node_id_attached;
  guint32 config_id_attached;
};
typedef struct ic_cs_conf_comment IC_CS_CONF_COMMENT;

struct ic_cluster_config_load
{
  IC_CLUSTER_CONFIG *conf;
  IC_MEMORY_CONTAINER *temp_mc_ptr;
  guint32 current_num_comms;
  guint32 total_num_comms;
  void *current_node_config;
  gchar *string_memory;
  gchar *string_memory_to_return;
  gchar *struct_memory;
  gchar *struct_memory_to_return;
  IC_CONFIG_TYPES current_node_config_type;
  IC_CS_CONF_COMMENT comments;
  
  guint32 size_string_memory;
  gboolean default_section;

  /*
    To avoid so many malloc calls we keep all default structures in this
    struct. These structures are initialised by setting the defaults for
    each parameter as defined by the iClaustron Cluster Server API.
  */
  IC_DATA_SERVER_CONFIG default_data_server_config;
  IC_CLIENT_CONFIG default_client_config;
  IC_CLUSTER_SERVER_CONFIG default_cluster_server_config;
  IC_SQL_SERVER_CONFIG default_sql_server_config;
  IC_REP_SERVER_CONFIG default_rep_server_config;
  IC_FILE_SERVER_CONFIG default_file_server_config;
  IC_RESTORE_CONFIG default_restore_config;
  IC_CLUSTER_MANAGER_CONFIG default_cluster_mgr_config;
  IC_SOCKET_LINK_CONFIG default_socket_config;
};
typedef struct ic_cluster_config_load IC_CLUSTER_CONFIG_LOAD;

struct ic_temp_api_config_server
{
  /*
    We have a number of variables to keep track of allocated memory
    and use of allocated memory for strings.
  */
  guint32 string_memory_size;
  gchar *end_string_memory;
  gchar *next_string_memory;
  guint32 *node_ids;
  gchar *config_memory_to_return;
};
typedef struct ic_temp_api_config_server IC_TEMP_API_CONFIG_SERVER;
/*
  The struct ic_api_config_server represents the configuration of
  all clusters that this node participates in and the node id it
  has in these clusters.
*/
struct ic_int_api_config_server
{
  IC_API_CLUSTER_OPERATIONS api_op;
  IC_CLUSTER_CONFIG **conf_objects;
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_TEMP_API_CONFIG_SERVER *temp;
  IC_API_CLUSTER_CONNECTION cluster_conn;
  GMutex *config_mutex;

  gchar *err_str;
  guint32 max_cluster_id;
  guint32 err_line;
  gchar use_ic_cs;
};
typedef struct ic_int_api_config_server IC_INT_API_CONFIG_SERVER;

/*
  Access to the state of the Run Cluster Server object needs to be
  done with mutex protection in the threads handling the Cluster
  Server requests. The Run Cluster Server object contains read-only
  parameters that are set during start-up of the Cluster Server and
  cannot currently be changed in the middle of operation.
*/
typedef struct ic_int_run_cluster_server IC_INT_RUN_CLUSTER_SERVER;

#define IC_MAX_CLUSTER_SERVER_THREADS 4096
struct ic_run_cluster_thread
{
  GThread *thread;
  IC_INT_RUN_CLUSTER_SERVER *run_obj;
  gboolean stopped;
  gboolean free;
};
typedef struct ic_run_cluster_thread IC_RUN_CLUSTER_THREAD;

struct ic_run_cluster_state
{
  /* Points to our Cluster Server Info, should always be 0 */
  guint32 my_cs_index;

  /* Points to Cluster Server info of master */
  guint32 master_cs_index;

  /* Number of threads set-up to connect to other Cluster Servers */
  guint32 start_threads_stopped;

  /* Set when start-up logic is active with a set of Cluster Servers */
  gboolean cs_starting;

  /* Set when we've started and are ready to respond to requests */
  gboolean cs_started;

  /* 
    If this variable is set, the connections are occupied by someone
    performing an update, we need to wait for this to become free.
  */
  gboolean update_state;

  /* Number of threads waiting to update */
  guint32 update_waiters;

  GMutex *protect_state;
  GCond  *start_cond;
  GCond  *connect_cond;
  GCond  *update_cond;
};
typedef struct ic_run_cluster_state IC_RUN_CLUSTER_STATE;

enum ic_cs_start_state
{
  IC_NOT_STARTED= 0, /* Initial state */
  IC_START_REPLY= 1
};
typedef enum ic_cs_start_state IC_CS_START_STATE;

struct ic_info_cluster_server
{
  /* A permanent connection to this Cluster Server used for updates */
  IC_CONNECTION *conn;

  /* Have we got a connection to this server yet. */
  gboolean cs_connect_state;

  /* Are we client in the TCP/IP connection to this server or not */
  gboolean is_client_side;

  /* Is start thread still running */
  gboolean is_start_thread_active;

  /* The pid of this Cluster Server */
  IC_PID_TYPE pid;

  /* The state of this server currently */
  IC_CONF_STATE_TYPE state;

  /* The current configuration version of this server */
  IC_CONF_VERSION_TYPE config_version_number;

  /* State of this node currently */
  IC_CS_START_STATE start_state;

  /*
    When is this node becoming master. 0 means it is master, 1 means
    it will take over immediately if the master fails and so forth.
  */
  guint32 master_index;

  /* Node id of this particular Cluster Server in all clusters in the grid */
  guint32 node_id;

  /* Thread id of thread connecting to this cluster server. */
  guint32 thread_id;

  /* Number of nodes in this index */
  guint32 master_index_size;

  /* This node's view on master indexes */
  guint32 master_index_view[IC_MAX_CLUSTER_SERVERS];
};
typedef struct ic_info_cluster_server IC_INFO_CLUSTER_SERVER;

struct ic_int_run_cluster_server
{
  /* The external interface to the object */
  IC_RUN_CLUSTER_SERVER_OPERATIONS run_op;

  /* The configuration of each cluster resident in memory */
  IC_CLUSTER_CONFIG *conf_objects[IC_MAX_CLUSTER_ID];

  /* The information about the clusters, name, id and password */
  IC_CLUSTER_CONNECT_INFO **clu_infos;

  /* The state of our cluster server */
  IC_RUN_CLUSTER_STATE state;

  /* A memory container to make it easy to deallocate resources */
  IC_MEMORY_CONTAINER *mc_ptr;

  /* The thread pool handling all threads for the cluster server */
  IC_THREADPOOL_STATE *tp_state;

  /* Info on the other cluster servers and their state */
  IC_INFO_CLUSTER_SERVER cs_servers[IC_MAX_CLUSTER_SERVERS];

  /* This is the socket of that listens to connections */
  IC_CONNECTION *conn;

  /*
    This object is normally used by clients, we fill it up with what
    is needed to allow us to reuse a number of methods from the
    client handling part of the iClaustron configuration management.
  */
  IC_INT_API_CONFIG_SERVER *apic;

  /* The object handling heartbeats on the NDB API connections */
  IC_APID_CONNECTION *heartbeat_conn;

  /* The global API object */
  IC_APID_GLOBAL *apid_global;

  /* The string that defines the directory for the config files */
  IC_STRING *config_dir;

  /* Number of cluster servers in the grid */
  guint32 num_cluster_servers;

  /* Max cluster id in the grid */
  guint32 max_cluster_id;

  /* Number of clusters in this grid */
  guint32 num_clusters;

  /* Our node id in all clusters we are part of */
  guint32 cs_nodeid;

  /* The name of this process, normally ic_csd */
  const gchar *process_name;

  /* Configuration has been locked indicator */
  gboolean locked_configuration;

  /* Stop has been ordered indicator */
  gboolean stop_flag;

  /* Error handling object */
  IC_CONFIG_ERROR err_obj;

  /* Object used read configuration from file */
  IC_CONFIG_STRUCT conf_server_struct;
  IC_CONFIG_STRUCT cluster_conf_struct;
};

#define IC_SET_CONFIG_MAP(name, id) \
  if (name >= MAX_MAP_CONFIG_ID) \
    id_out_of_range(name); \
  if (id >= MAX_CONFIG_ID) \
    name_out_of_range(id); \
  map_config_id_to_inx[name]= id; \
  map_inx_to_config_id[id]= name; \
  conf_entry= &glob_conf_entry[id]; \
  if (conf_entry->config_entry_name.str) \
    id_already_used_aborting(id);   \
  if (id > glob_max_config_id) glob_max_config_id= id; \
  conf_entry->config_id= name;

#define IC_SET_SYSTEM_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_SYSTEM_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_SYSTEM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SYSTEM_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->offset= offsetof(IC_SYSTEM_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_SYSTEM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_DATA_SERVER_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_DATA_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_DATA_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CONFIG_MIN(conf_entry, min) \
  (conf_entry)->is_min_value_defined= TRUE; \
  (conf_entry)->min_value= (min);

#define IC_SET_CONFIG_MAX(conf_entry, max) \
  (conf_entry)->is_max_value_defined= TRUE; \
  (conf_entry)->max_value= (max);

#define IC_SET_CONFIG_MIN_MAX(conf_entry, min, max) \
  (conf_entry)->is_min_value_defined= TRUE; \
  (conf_entry)->is_max_value_defined= TRUE; \
  (conf_entry)->min_value= (min); \
  (conf_entry)->max_value= (max); \
  if ((min) == (max)) (conf_entry)->is_not_configurable= TRUE;

#define IC_SET_DATA_SERVER_BOOLEAN(conf_entry, name, def, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_BOOLEAN; \
  (conf_entry)->default_value= (def); \
  (conf_entry)->is_boolean= TRUE; \
  (conf_entry)->offset= offsetof(IC_DATA_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_DATA_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_DATA_SERVER_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->offset= offsetof(IC_DATA_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_DATA_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SOCKET_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_SOCKET_LINK_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SOCKET_BOOLEAN(conf_entry, name, def, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_BOOLEAN; \
  (conf_entry)->default_value= (def); \
  (conf_entry)->is_boolean= TRUE; \
  (conf_entry)->offset= offsetof(IC_SOCKET_LINK_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_SOCKET_STRING(conf_entry, name, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->offset= offsetof(IC_SOCKET_LINK_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_COMM_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLUSTER_SERVER_STRING(conf_entry, name, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->data_type= IC_CHARPTR; \
  (conf_entry)->is_string_type= TRUE; \
  (conf_entry)->default_string= (char*)(val); \
  (conf_entry)->offset= offsetof(IC_CLUSTER_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLUSTER_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLUSTER_SERVER_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_CLUSTER_SERVER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLUSTER_SERVER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLUSTER_MANAGER_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_CLUSTER_MANAGER_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLUSTER_MANAGER_TYPE); \
  (conf_entry)->change_variant= (change);

#define IC_SET_CLIENT_CONFIG(conf_entry, name, type, val, change) \
  (conf_entry)->config_entry_name.str= #name; \
  (conf_entry)->config_entry_name.len= strlen(#name); \
  (conf_entry)->config_entry_name.is_null_terminated= TRUE; \
  (conf_entry)->default_value= (val); \
  (conf_entry)->data_type= (type); \
  (conf_entry)->offset= offsetof(IC_CLIENT_CONFIG, name); \
  (conf_entry)->config_types= (1 << IC_CLIENT_TYPE); \
  (conf_entry)->change_variant= (change);

#define PROTOCOL_CHECK_GOTO(cond) \
{ \
  if (!(cond)) \
  { \
    set_error_line((IC_API_CONFIG_SERVER*)apic, (guint32)__LINE__); \
    error= IC_PROTOCOL_ERROR; \
    goto error; \
  } \
}

#define PROTOCOL_CONN_CHECK_GOTO(cond) \
{ \
  if (!(cond)) \
  { \
    conn->conn_op.ic_set_error_line(conn, (guint32)__LINE__); \
    error= IC_PROTOCOL_ERROR; \
    goto error; \
  } \
}

#define PROTOCOL_CHECK_DEBUG_RETURN(cond) \
{ \
  if (!(cond)) \
  { \
    set_error_line((IC_API_CONFIG_SERVER*)apic, (guint32)__LINE__); \
    DEBUG_RETURN(IC_PROTOCOL_ERROR); \
    goto error; \
  } \
  DEBUG_RETURN(0); \
}

#define PROTOCOL_CONN_CHECK_DEBUG_RETURN(cond) \
{ \
  if (!(cond)) \
  { \
    conn->conn_op.ic_set_error_line(conn, (guint32)__LINE__); \
    DEBUG_RETURN(IC_PROTOCOL_ERROR); \
  } \
  DEBUG_RETURN(0); \
}

#define PROTOCOL_CHECK_RETURN(cond) \
{ \
  if (!(cond)) \
  { \
    set_error_line((IC_API_CONFIG_SERVER*)apic, (guint32)__LINE__); \
    return IC_PROTOCOL_ERROR; \
  } \
  return 0; \
}

#define PROTOCOL_CONN_CHECK_ERROR_GOTO(error) \
{ if ((error) == IC_PROTOCOL_ERROR) PROTOCOL_CONN_CHECK_GOTO(FALSE) \
else goto error; }

#define PROTOCOL_CHECK_ERROR_GOTO(error) \
{ if ((error) == IC_PROTOCOL_ERROR) PROTOCOL_CHECK_GOTO(FALSE) \
else goto error; }
#endif
