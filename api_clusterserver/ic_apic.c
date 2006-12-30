#include <ic_apic.h>
/*
  DESCRIPTION TO ADD NEW CONFIGURATION VARIABLE:
  1) Add a new constant in this file e.g:
  #define KERNEL_SCHEDULER_NO_SEND_TIME 166
  2) Add a new entry in ic_init_config_parameters
     Check how other entries look like, the struct
     to fill in config_entry and is described in
     ic_apic.h
  3) Add a new variable in the struct's for the various
     node types, for kernel variables the struct is
     ic_kernel_node_config (ic_apic.h)
  4) Add a case statement in the read_node_section or in
     the read_comm_section method that fills in the
     new variable in the reading of the configuration.
  5) Add an initialisation of the variable to its default
     value in the proper initialisation routine.
*/

/*
  Description of protocol to get configuration profile from
  cluster server.
  1) Start with get_nodeid session. The api sends a number of
     lines of text with information about itself and login
     information.
     get nodeid<CR>
     nodeid: 0<CR>
     version: 327948<CR>
     nodetype: 1<CR>
     user: mysqld<CR>
     password: mysqld<CR>
     public key: a public key<CR>
     endian: little<CR>
     log_event: 0<CR>
     <CR>

     Here nodeid is normally 0, meaning that the receiver can accept
     any nodeid connected to the host we are connecting from. If nodeid
     != 0 then this is the only nodeid we accept.
     Version number is the version of the API, the version number is the
     hexadecimal notation of the version number (e.g. 5.1.12 = 0x5010c =
     327948).
     Nodetype is either 1 for API's to the data servers, for data servers
     the nodetype is 0 and for cluster servers it is 2.
     user, password and public key is preparation for future functionality
     so should always be mysqld, mysqld and a public key for API nodes.
     Endian should either be little or big, dependent on if the API is on
     little-endian box or big-endian box.
     Log event is ??

     The cluster server will respond with either an error response or
     a response that provides the nodeid to the API using the following
     messages.
     get nodeid reply<CR>
     nodeid: 4<CR>
     result: Ok<CR>
     <CR>

     Where nodeid is the one chosen by the cluster server.

     An error response is sent as 
     result: Error (Message)<CR>
     <CR>

  2) The next step is to get the configuration. To get the configuration
     the API sends the following messages:
     get config<CR>
     version: 327948<CR>
     <CR>

     In response to this the cluster server will send the configuration
     or an error response in the same manner as above.
     The configuration is sent as follows.
     get config reply<CR>
     result: Ok<CR>
     Content-Length: 3047<CR>
     ndbconfig/octet-stream<CR>
     Content-Transfer-Encoding: base64<CR>
     <CR>
     base64-encoded string with length as provided above
*/


#define MAX_NODE_ID 63

#define GET_NODEID_REPLY_STATE 0
#define NODEID_STATE 1
#define RESULT_OK_STATE 2
#define WAIT_EMPTY_RETURN_STATE 3
#define RESULT_ERROR_STATE 4

#define GET_CONFIG_REPLY_STATE 5
#define CONTENT_LENGTH_STATE 6
#define OCTET_STREAM_STATE 7
#define CONTENT_ENCODING_STATE 8
#define RECEIVE_CONFIG_STATE 9

#define GET_NODEID_REPLY_LEN 16
#define NODEID_LEN 8
#define RESULT_OK_LEN 10

#define GET_CONFIG_REPLY_LEN 16
#define CONTENT_LENGTH_LEN 16
#define OCTET_STREAM_LEN 36
#define CONTENT_ENCODING_LEN 33

#define CONFIG_LINE_LEN 76
#define MAX_CONTENT_LEN 16 * 1024 * 1024 /* 16 MByte max */

#define IC_CL_KEY_MASK 0x3FFF
#define IC_CL_KEY_SHIFT 28
#define IC_CL_SECT_SHIFT 14
#define IC_CL_SECT_MASK 0x3FFF
#define IC_CL_INT32_TYPE 1
#define IC_CL_CHAR_TYPE  2
#define IC_CL_SECT_TYPE  3
#define IC_CL_INT64_TYPE 4

#define IC_NODE_TYPE     999
#define IC_NODE_ID       3
#define IC_NODE_HOST     5
#define IC_NODE_DATA_PATH 7
#define IC_PARENT_ID     16382

#define MAX_MAP_CONFIG_ID 1024
#define MAX_CONFIG_ID 256

static guint16 map_config_id[MAX_MAP_CONFIG_ID];
static struct config_entry glob_conf_entry[MAX_CONFIG_ID];
static gboolean glob_conf_entry_inited= FALSE;

static const char *empty_string= "";
static const char *get_nodeid_str= "get nodeid";
static const char *get_nodeid_reply_str= "get nodeid reply";
static const char *get_config_str= "get config";
static const char *get_config_reply_str= "get config reply";
static const char *nodeid_str= "nodeid: ";
static const char *version_str= "version: ";
static const char *nodetype_str= "nodetype: 1";
static const char *user_str= "user: mysqld";
static const char *password_str= "password: mysqld";
static const char *public_key_str= "public key: a public key";
static const char *endian_str= "endian: ";
static const char *log_event_str= "log_event: 0";
static const char *result_ok_str= "result: Ok";
static const char *content_len_str= "Content-Length: ";
static const char *octet_stream_str= "Content-Type: ndbconfig/octet-stream";
static const char *content_encoding_str= "Content-Transfer-Encoding: base64";
static const guint32 version_no= (guint32)0x5010D; /* 5.1.13 */

/*
  CONFIGURATION PARAMETER MODULE
  ------------------------------

  This module contains all definitions of the configuration parameters.
  They are only used in this file, the rest of the code can only get hold
  of configuration objects that are filled in, they can also access methods
  that create protocol objects based on those configuration objects.
*/

/*
  This method defines all configuration parameters and puts them in a global
  variable only accessible from a few methods in this file. When adding a
  new configuration variable the following actions are needed:
  1) Add a constant for the configuration variable in the header file
  2) Add it to this method ic_init_config_parameters
  3) Add it to any of the config reader routines dependent on if it is a
     kernel, client, cluster server or communication variable.
  4) In the same manner add it to any of the config writer routines
*/

#define KERNEL_INJECT_FAULT 1
#define KERNEL_MAX_TRACE_FILES 100
#define KERNEL_REPLICAS 101
#define KERNEL_TABLE_OBJECTS 102
#define KERNEL_COLUMN_OBJECTS 103
#define KERNEL_KEY_OBJECTS 104
#define KERNEL_INTERNAL_TRIGGER_OBJECTS 105
#define KERNEL_CONNECTION_OBJECTS 106
#define KERNEL_OPERATION_OBJECTS 107
#define KERNEL_SCAN_OBJECTS 108
#define KERNEL_INTERNAL_TRIGGER_OPERATION_OBJECTS 109
#define KERNEL_KEY_OPERATION_OBJECTS 110
#define KERNEL_CONNECTION_BUFFER 111
#define KERNEL_RAM_MEMORY 112
#define KERNEL_HASH_MEMORY 113
#define KERNEL_LOCK_MEMORY 114
#define KERNEL_WAIT_PARTIAL_START 115
#define KERNEL_WAIT_PARTITIONED_START 116
#define KERNEL_WAIT_ERROR_START 117
#define KERNEL_HEARTBEAT_TIMER 118
#define KERNEL_CLIENT_HEARTBEAT_TIMER 119
#define KERNEL_LOCAL_CHECKPOINT_TIMER 120
#define KERNEL_GLOBAL_CHECKPOINT_TIMER 121
#define KERNEL_RESOLVE_TIMER 122
#define KERNEL_WATCHDOG_TIMER 123
#define KERNEL_DAEMON_RESTART_AT_ERROR 124
#define KERNEL_FILESYSTEM_PATH 125
#define KERNEL_REDO_LOG_FILES 126
/* 127 and 128 deprecated */
#define KERNEL_CHECK_INTERVAL 129
#define KERNEL_CLIENT_ACTIVITY_TIMER 130
#define KERNEL_DEADLOCK_TIMER 131
#define KERNEL_CHECKPOINT_OBJECTS 132
#define KERNEL_CHECKPOINT_MEMORY 133
#define KERNEL_CHECKPOINT_DATA_MEMORY 134
#define KERNEL_CHECKPOINT_LOG_MEMORY 135
#define KERNEL_CHECKPOINT_WRITE_SIZE 136
/* 137 and 138 deprecated */
#define KERNEL_CHECKPOINT_MAX_WRITE_SIZE 139
/* 140 - 146 not used */
/* 147 Cluster Server parameter */
#define KERNEL_VOLATILE_MODE 148
#define KERNEL_ORDERED_KEY_OBJECTS 149
#define KERNEL_UNIQUE_HASH_KEY_OBJECTS 150
#define KERNEL_LOCAL_OPERATION_OBJECTS 151
#define KERNEL_LOCAL_SCAN_OBJECTS 152
#define KERNEL_SCAN_BATCH_SIZE 153
/* 154 and 155 deprecated */
#define KERNEL_REDO_LOG_MEMORY 156
#define KERNEL_LONG_MESSAGE_MEMORY 157
#define KERNEL_CHECKPOINT_PATH 158
#define KERNEL_MAX_OPEN_FILES 159
#define KERNEL_PAGE_CACHE_SIZE 160
#define KERNEL_STRING_MEMORY 161
#define KERNEL_INITIAL_OPEN_FILES 162
#define KERNEL_FILE_SYNCH_SIZE 163
#define KERNEL_DISK_WRITE_SPEED 164
#define KERNEL_DISK_WRITE_SPEED_START 165
#define KERNEL_SCHEDULER_NO_SEND_TIME 166
#define KERNEL_SCHEDULER_NO_SLEEP_TIME 167
#define KERNEL_RT_SCHEDULER_THREADS 170
#define KERNEL_LOCK_MAIN_THREAD 171
#define KERNEL_LOCK_OTHER_THREADS 172
#define KERNEL_MEMORY_POOL 198
#define KERNEL_DUMMY 199

#define CLIENT_RESOLVE_RANK 200
#define CLIENT_RESOLVE_TIMER 201

#define KERNEL_START_LOG_LEVEL 250
#define KERNEL_STOP_LOG_LEVEL 251
#define KERNEL_STAT_LOG_LEVEL 252
#define KERNEL_CHECKPOINT_LOG_LEVEL 253
#define KERNEL_RESTART_LOG_LEVEL 254
#define KERNEL_CONNECTION_LOG_LEVEL 255
#define KERNEL_REPORT_LOG_LEVEL 256
#define KERNEL_WARNING_LOG_LEVEL 257
#define KERNEL_ERROR_LOG_LEVEL 258
#define KERNEL_CONGESTION_LOG_LEVEL 259
#define KERNEL_DEBUG_LOG_LEVEL 260
#define KERNEL_BACKUP_LOG_LEVEL 261

#define CLUSTER_SERVER_EVENT_LOG 147

#define CLUSTER_SERVER_PORT_NUMBER 300

#define TCP_FIRST_NODE_ID 400
#define TCP_SECOND_NODE_ID 401
#define TCP_USE_MESSAGE_ID 402
#define TCP_USE_CHECKSUM 403
#define TCP_CLIENT_PORT_NUMBER 420
#define TCP_SERVER_PORT_NUMBER 406
#define TCP_SERVER_NODE_ID 410
#define TCP_WRITE_BUFFER_SIZE 454
#define TCP_READ_BUFFER_SIZE 455
#define TCP_FIRST_HOSTNAME 407
#define TCP_SECOND_HOSTNAME 408
#define TCP_GROUP 409

#define CLIENT_MAX_BATCH_BYTE_SIZE 800
#define CLIENT_BATCH_BYTE_SIZE 801
#define CLIENT_BATCH_SIZE 802

void
ic_print_config_parameters(guint32 mask)
{
  struct config_entry *conf_entry;
  guint32 inx, i;
  if (!glob_conf_entry_inited)
    return;
  for (i= 0; i < 1024; i++)
  {
    if ((inx= map_config_id[i]))
    {
      conf_entry= &glob_conf_entry[inx];
      if (!(conf_entry->node_type & mask))
        continue;
      printf("\n");
      if (conf_entry->is_deprecated)
      {
        printf("Entry %u is deprecated\n", i);
        continue;
      }
      printf("Entry %u:\n", i);
      printf("Name: %s\n", conf_entry->config_entry_name);
      printf("Comment: %s\n", conf_entry->config_entry_description);
      if (conf_entry->is_not_configurable)
      {
        printf("Entry is not configurable with value %u\n",
               (guint32)conf_entry->default_value);
        continue;
      }
      switch (conf_entry->change_variant)
      {
        case IC_ONLINE_CHANGE:
          printf("This parameter is changeable online\n");
          break;
        case IC_NODE_RESTART_CHANGE:
          printf("This parameter can be changed during a node restart\n");
          break;
        case IC_ROLLING_UPGRADE_CHANGE:
        case IC_ROLLING_UPGRADE_CHANGE_SPECIAL:
          printf("Parameter can be changed during a rolling upgrade\n");
          break;
        case IC_INITIAL_NODE_RESTART:
          printf("Parameter can be changed when node performs initial restart\n");
          break;
        case IC_CLUSTER_RESTART_CHANGE:
          printf("Parameter can be changed after stopping cluster before restart\n");
          break;
        case IC_NOT_CHANGEABLE:
          printf("Parameter can only be changed using backup, change, restore\n");
          break;
        default:
          g_assert(0);
      }
      if (conf_entry->node_type & (1 << IC_CLIENT_TYPE))
        printf("This config variable is used in a client node\n");
      if (conf_entry->node_type & (1 << IC_KERNEL_TYPE))
        printf("This config variable is used in a kernel node\n");
      if (conf_entry->node_type & (1 << IC_CLUSTER_SERVER_TYPE))
        printf("This config variable is used in a cluster server\n");
      if (conf_entry->node_type & (1 << IC_COMM_TYPE))
        printf("This config variable is used in connections\n");

      if (conf_entry->is_mandatory_to_specify)
        printf("Entry is mandatory and has no default value\n");
      if (conf_entry->is_string_type)
      {
        if (!conf_entry->is_mandatory_to_specify)
        {
          printf("Entry has default value: %s\n",
                 conf_entry->default_string);
        }
        continue;
      }
      if (conf_entry->is_boolean)
      {
        printf("Entry is either TRUE or FALSE\n");
        continue;
      }
      if (conf_entry->is_min_value_defined)
        printf("Min value defined: %u\n", (guint32)conf_entry->min_value);
      else
        printf("No min value defined\n");
      if (conf_entry->is_max_value_defined)
        printf("Max value defined: %u\n", (guint32)conf_entry->max_value);
      else
        printf("No max value defined\n");
    }
  }
}

static guint32 get_default_value_uint32(guint32 id)
{
  struct config_entry *conf_entry;
  g_assert(map_config_id[id]);
  conf_entry= &glob_conf_entry[map_config_id[id]];
  g_assert(!conf_entry->is_mandatory_to_specify);
  return conf_entry->default_value;
}

static guint64 get_default_value_uint64(guint32 id)
{
  struct config_entry *conf_entry;
  g_assert(map_config_id[id]);
  conf_entry= &glob_conf_entry[map_config_id[id]];
  g_assert(!conf_entry->is_mandatory_to_specify);
  return (guint64)conf_entry->default_value;
}

static gchar get_default_value_uchar(guint32 id)
{
  struct config_entry *conf_entry;
  g_assert(map_config_id[id]);
  conf_entry= &glob_conf_entry[map_config_id[id]];
  g_assert(!conf_entry->is_mandatory_to_specify);
  return (gchar)conf_entry->default_value;
}

static void
ic_init_config_parameters()
{
  struct config_entry *conf_entry;
  if (glob_conf_entry_inited)
    return;
  glob_conf_entry_inited= TRUE;
  memset(map_config_id, 0, 1024 * sizeof(guint16));
  memset(glob_conf_entry, 0, 256 * sizeof(struct config_entry));

/*
  This is the kernel node configuration section.
*/
  map_config_id[KERNEL_MAX_TRACE_FILES]= 1;
  conf_entry= &glob_conf_entry[1];
  conf_entry->config_entry_name= "max_number_of_trace_files";
  conf_entry->config_entry_description=
  "The number of crashes that can be reported before we overwrite error log and trace files";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= 2048;
  conf_entry->default_value= 25;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_REPLICAS]= 2;
  conf_entry= &glob_conf_entry[2];
  conf_entry->config_entry_name= "number_of_replicas";
  conf_entry->config_entry_description=
  "This defines number of nodes per node group, within a node group all nodes contain the same data";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= 4;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_TABLE_OBJECTS]= 3;
  conf_entry= &glob_conf_entry[3];
  conf_entry->config_entry_name= "number_of_table_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of tables that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->default_value= 256;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_COLUMN_OBJECTS]= 4;
  conf_entry= &glob_conf_entry[4];
  conf_entry->config_entry_name= "number_of_column_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of columns that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 256;
  conf_entry->default_value= 2048;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_KEY_OBJECTS]= 5;
  conf_entry= &glob_conf_entry[5];
  conf_entry->config_entry_name= "number_of_key_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of keys that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->default_value= 256;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_INTERNAL_TRIGGER_OBJECTS]= 6;
  conf_entry= &glob_conf_entry[6];
  conf_entry->config_entry_name= "number_of_internal_trigger_objects";
  conf_entry->config_entry_description=
  "Each unique index will use 3 internal trigger objects, index/backup will use 1 per table";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 512;
  conf_entry->default_value= 1536;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_CONNECTION_OBJECTS]= 7;
  conf_entry= &glob_conf_entry[7];
  conf_entry->config_entry_name= "number_of_connection_objects";
  conf_entry->config_entry_description=
  "Each active transaction and active scan uses a connection object";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 128;
  conf_entry->default_value= 8192;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[KERNEL_OPERATION_OBJECTS]= 8;
  conf_entry= &glob_conf_entry[8];
  conf_entry->config_entry_name= "number_of_operation_objects";
  conf_entry->config_entry_description=
  "Each record read/updated in a transaction uses an operation object during the transaction";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1024;
  conf_entry->default_value= 32768;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[KERNEL_SCAN_OBJECTS]= 9;
  conf_entry= &glob_conf_entry[9];
  conf_entry->config_entry_name= "number_of_scan_objects";
  conf_entry->config_entry_description=
  "Each active scan uses a scan object for the lifetime of the scan operation";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->max_value= 512;
  conf_entry->default_value= 128;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_INTERNAL_TRIGGER_OPERATION_OBJECTS]= 10;
  conf_entry= &glob_conf_entry[10];
  conf_entry->config_entry_name= "number_of_internal_trigger_operation_objects";
  conf_entry->config_entry_description=
  "Each internal trigger that is fired uses an operation object for a short time";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->min_value= 4000;
  conf_entry->max_value= 4000;
  conf_entry->default_value= 4000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_KEY_OPERATION_OBJECTS]= 11;
  conf_entry= &glob_conf_entry[11];
  conf_entry->config_entry_name= "number_of_key_operation_objects";
  conf_entry->config_entry_description=
  "Each read and update of an unique hash index in a transaction uses one of those objects";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 128;
  conf_entry->default_value= 4096;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[KERNEL_CONNECTION_BUFFER]= 12;
  conf_entry= &glob_conf_entry[12];
  conf_entry->config_entry_name= "size_of_connection_buffer";
  conf_entry->config_entry_description=
  "Internal buffer used by connections by transactions and scans";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->min_value= 1024 * 1024;
  conf_entry->max_value= 1024 * 1024;
  conf_entry->default_value= 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_RAM_MEMORY]= 13;
  conf_entry= &glob_conf_entry[13];
  conf_entry->config_entry_name= "size_of_ram_memory";
  conf_entry->config_entry_description=
  "Size of memory used to store RAM-based records";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 16 * 1024 * 1024;
  conf_entry->default_value= 256 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_HASH_MEMORY]= 14;
  conf_entry= &glob_conf_entry[14];
  conf_entry->config_entry_name= "size_of_hash_memory";
  conf_entry->config_entry_description=
  "Size of memory used to store primary hash index on all tables and unique hash indexes";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 8 * 1024 * 1024;
  conf_entry->default_value= 64 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_LOCK_MEMORY]= 15;
  conf_entry= &glob_conf_entry[15];
  conf_entry->config_entry_name= "use_unswappable_memory";
  conf_entry->config_entry_description=
  "Setting this to 1 means that all memory is locked and will not be swapped out";
  conf_entry->is_boolean= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_WAIT_PARTIAL_START]= 16;
  conf_entry= &glob_conf_entry[16];
  conf_entry->config_entry_name= "timer_wait_partial_start";
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting with a partial set of nodes, 0 waits forever";
  conf_entry->default_value= 20000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_WAIT_PARTITIONED_START]= 17;
  conf_entry= &glob_conf_entry[17];
  conf_entry->config_entry_name= "timer_wait_partitioned_start";
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting a potentially partitioned cluster, 0 waits forever";
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_WAIT_ERROR_START]= 18;
  conf_entry= &glob_conf_entry[18];
  conf_entry->config_entry_name= "timer_wait_error_start";
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before forcing a stop after an error, 0 waits forever";
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_HEARTBEAT_TIMER]= 19;
  conf_entry= &glob_conf_entry[19];
  conf_entry->config_entry_name= "timer_heartbeat_kernel_nodes";
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to kernel nodes, 4 missed leads to node crash";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 10;
  conf_entry->default_value= 700;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE_SPECIAL;

  map_config_id[KERNEL_CLIENT_HEARTBEAT_TIMER]= 20;
  conf_entry= &glob_conf_entry[20];
  conf_entry->config_entry_name= "timer_heartbeat_client_nodes";
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to client nodes, 4 missed leads to node crash";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 10;
  conf_entry->default_value= 1000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE_SPECIAL;

  map_config_id[KERNEL_LOCAL_CHECKPOINT_TIMER]= 21;
  conf_entry= &glob_conf_entry[21];
  conf_entry->config_entry_name= "timer_local_checkpoint";
  conf_entry->config_entry_description=
  "Specifies how often local checkpoints are executed, logarithmic scale on log size";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 6;
  conf_entry->max_value= 31;
  conf_entry->default_value= 24;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_GLOBAL_CHECKPOINT_TIMER]= 22;
  conf_entry= &glob_conf_entry[22];
  conf_entry->config_entry_name= "timer_global_checkpoint";
  conf_entry->config_entry_description=
  "Time in ms between starting global checkpoints";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 10;
  conf_entry->default_value= 1000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_RESOLVE_TIMER]= 23;
  conf_entry= &glob_conf_entry[23];
  conf_entry->config_entry_name= "timer_resolve";
  conf_entry->config_entry_description=
  "Time in ms waiting for response from resolve";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 10;
  conf_entry->default_value= 2000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_WATCHDOG_TIMER]= 24;
  conf_entry= &glob_conf_entry[24];
  conf_entry->config_entry_name= "timer_kernel_watchdog";
  conf_entry->config_entry_description=
  "Time in ms without activity in kernel before watchdog is fired";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1000;
  conf_entry->default_value= 6000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_DAEMON_RESTART_AT_ERROR]= 25;
  conf_entry= &glob_conf_entry[25];
  conf_entry->config_entry_name= "kernel_automatic_restart";
  conf_entry->config_entry_description=
  "If set, kernel restarts automatically after a failure";
  conf_entry->is_boolean= TRUE;
  conf_entry->default_value= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_FILESYSTEM_PATH]= 26;
  conf_entry= &glob_conf_entry[26];
  conf_entry->config_entry_name= "filesystem_path";
  conf_entry->config_entry_description=
  "Path to filesystem of kernel";
  conf_entry->is_string_type= TRUE;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_INITIAL_NODE_RESTART;

  map_config_id[KERNEL_REDO_LOG_FILES]= 27;
  conf_entry= &glob_conf_entry[27];
  conf_entry->config_entry_name= "number_of_redo_log_files";
  conf_entry->config_entry_description=
  "Number of REDO log files, each file represents 64 MB log space";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 4;
  conf_entry->default_value= 32;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_INITIAL_NODE_RESTART;

  map_config_id[KERNEL_CHECK_INTERVAL]= 28;
  conf_entry= &glob_conf_entry[28];
  conf_entry->config_entry_name= "timer_check_interval";
  conf_entry->config_entry_description=
  "Time in ms between checks after transaction timeouts";
  conf_entry->is_not_configurable= TRUE;
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 500;
  conf_entry->max_value= 500;
  conf_entry->default_value= 500;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CLIENT_ACTIVITY_TIMER]= 29;
  conf_entry= &glob_conf_entry[29];
  conf_entry->config_entry_name= "timer_client_activity";
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to client inactivity";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1000;
  conf_entry->default_value= 1024 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_DEADLOCK_TIMER]= 30;
  conf_entry= &glob_conf_entry[30];
  conf_entry->config_entry_name= "timer_deadlock";
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to internal wait (indication of deadlock)";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1000;
  conf_entry->default_value= 2000;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_CHECKPOINT_OBJECTS]= 31;
  conf_entry= &glob_conf_entry[31];
  conf_entry->config_entry_name= "number_of_checkpoint_objects";
  conf_entry->config_entry_description=
  "Number of possible parallel backups and local checkpoints";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= 1;
  conf_entry->default_value= 1;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CHECKPOINT_MEMORY]= 32;
  conf_entry= &glob_conf_entry[32];
  conf_entry->config_entry_name= "checkpoint_memory";
  conf_entry->config_entry_description=
  "Size of memory buffers for local checkpoint and backup";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 4 * 1024 * 1024;
  conf_entry->max_value= 4 * 1024 * 1024;
  conf_entry->default_value= 4 * 1024 * 1024;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CHECKPOINT_DATA_MEMORY]= 33;
  conf_entry= &glob_conf_entry[33];
  conf_entry->config_entry_name= "checkpoint_data_memory";
  conf_entry->config_entry_description=
  "Size of data memory buffers for local checkpoint and backup";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 2 * 1024 * 1024;
  conf_entry->max_value= 2 * 1024 * 1024;
  conf_entry->default_value= 2 * 1024 * 1024;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CHECKPOINT_LOG_MEMORY]= 34;
  conf_entry= &glob_conf_entry[34];
  conf_entry->config_entry_name= "checkpoint_log_memory";
  conf_entry->config_entry_description=
  "Size of log memory buffers for local checkpoint and backup";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 2 * 1024 * 1024;
  conf_entry->max_value= 2 * 1024 * 1024;
  conf_entry->default_value= 2 * 1024 * 1024;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CHECKPOINT_WRITE_SIZE]= 35;
  conf_entry= &glob_conf_entry[35];
  conf_entry->config_entry_name= "checkpoint_write_size";
  conf_entry->config_entry_description=
  "Size of default writes in local checkpoint and backups";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 64 * 1024;
  conf_entry->max_value= 64 * 1024;
  conf_entry->default_value= 64 * 1024;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CHECKPOINT_MAX_WRITE_SIZE]= 36;
  conf_entry= &glob_conf_entry[36];
  conf_entry->config_entry_name= "checkpoint_max_write_size";
  conf_entry->config_entry_description=
  "Size of maximum writes in local checkpoint and backups";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 256 * 1024;
  conf_entry->max_value= 256 * 1024;
  conf_entry->default_value= 256 * 1024;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_VOLATILE_MODE]= 37;
  conf_entry= &glob_conf_entry[37];
  conf_entry->config_entry_name= "kernel_volatile_mode";
  conf_entry->config_entry_description=
  "In this mode all file writes are ignored and all starts becomes initial starts";
  conf_entry->is_boolean= TRUE;
  conf_entry->default_value= FALSE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_ORDERED_KEY_OBJECTS]= 38;
  conf_entry= &glob_conf_entry[38];
  conf_entry->config_entry_name= "number_of_ordered_key_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of ordered keys that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->default_value= 128;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_UNIQUE_HASH_KEY_OBJECTS]= 39;
  conf_entry= &glob_conf_entry[39];
  conf_entry->config_entry_name= "number_of_unique_hash_key_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of unique hash keys that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->default_value= 128;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_LOCAL_OPERATION_OBJECTS]= 40;
  conf_entry= &glob_conf_entry[40];
  conf_entry->config_entry_name= "number_of_local_operation_objects";
  conf_entry->config_entry_description=
  "Number of local operation records stored used in the node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 0;
  conf_entry->default_value= 0;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_LOCAL_SCAN_OBJECTS]= 41;
  conf_entry= &glob_conf_entry[41];
  conf_entry->config_entry_name= "number_of_local_scan_objects";
  conf_entry->config_entry_description=
  "Number of local scan records stored used in the node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 0;
  conf_entry->default_value= 0;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_SCAN_BATCH_SIZE]= 42;
  conf_entry= &glob_conf_entry[42];
  conf_entry->config_entry_name= "size_of_scan_batch";
  conf_entry->config_entry_description=
  "Number of records sent in a scan from the local kernel node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 64;
  conf_entry->max_value= 64;
  conf_entry->default_value= 64;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_REDO_LOG_MEMORY]= 43;
  conf_entry= &glob_conf_entry[43];
  conf_entry->config_entry_name= "redo_log_memory";
  conf_entry->config_entry_description=
  "Size of REDO log memory buffer";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1 * 1024  * 1024;
  conf_entry->default_value= 16 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_LONG_MESSAGE_MEMORY]= 44;
  conf_entry= &glob_conf_entry[44];
  conf_entry->config_entry_name= "long_message_memory";
  conf_entry->config_entry_description=
  "Size of long memory buffers";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1 * 1024  * 1024;
  conf_entry->max_value= 1 * 1024  * 1024;
  conf_entry->default_value= 1 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_CHECKPOINT_PATH]= 45;
  conf_entry= &glob_conf_entry[45];
  conf_entry->config_entry_name= "kernel_checkpoint_path";
  conf_entry->config_entry_description=
  "Path to filesystem of checkpoints";
  conf_entry->is_string_type= TRUE;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_INITIAL_NODE_RESTART;

  map_config_id[KERNEL_MAX_OPEN_FILES]= 46;
  conf_entry= &glob_conf_entry[46];
  conf_entry->config_entry_name= "kernel_max_open_files";
  conf_entry->config_entry_description=
  "Maximum number of open files in kernel node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 40;
  conf_entry->max_value= 40;
  conf_entry->default_value= 40;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_PAGE_CACHE_SIZE]= 47;
  conf_entry= &glob_conf_entry[47];
  conf_entry->config_entry_name= "page_cache_size";
  conf_entry->config_entry_description=
  "Size of page cache for disk-based data";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 64 * 1024;
  conf_entry->default_value= 128 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_STRING_MEMORY]= 48;
  conf_entry= &glob_conf_entry[48];
  conf_entry->config_entry_name= "size_of_string_memory";
  conf_entry->config_entry_description=
  "Size of string memory";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 0;
  conf_entry->default_value= 0;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_INITIAL_OPEN_FILES]= 49;
  conf_entry= &glob_conf_entry[49];
  conf_entry->config_entry_name= "kernel_open_files";
  conf_entry->config_entry_description=
  "Number of open file handles in kernel node from start";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 27;
  conf_entry->max_value= 27;
  conf_entry->default_value= 27;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_FILE_SYNCH_SIZE]= 50;
  conf_entry= &glob_conf_entry[50];
  conf_entry->config_entry_name= "kernel_file_synch_size";
  conf_entry->config_entry_description=
  "Size of file writes before a synch is always used";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1024 * 1024;
  conf_entry->default_value= 4 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_DISK_WRITE_SPEED]= 51;
  conf_entry= &glob_conf_entry[51];
  conf_entry->config_entry_name= "kernel_disk_write_speed";
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 64 * 1024;
  conf_entry->default_value= 8 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_DISK_WRITE_SPEED_START]= 52;
  conf_entry= &glob_conf_entry[52];
  conf_entry->config_entry_name= "kernel_disk_write_speed_start";
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk during start of the node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1024 * 1024;
  conf_entry->default_value= 256 * 1024 * 1024;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_MEMORY_POOL]= 53;
  conf_entry= &glob_conf_entry[53];
  conf_entry->config_entry_name= "kernel_memory_pool";
  conf_entry->config_entry_description=
  "Size of memory pool for internal memory usage";
  conf_entry->default_value= 0;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_DUMMY]= 54;
  conf_entry= &glob_conf_entry[54];
  conf_entry->config_entry_name= "kernel_dummy";
  conf_entry->config_entry_description= (char*)empty_string;
  conf_entry->default_value= 0;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_START_LOG_LEVEL]= 55;
  conf_entry= &glob_conf_entry[55];
  conf_entry->config_entry_name= "log_level_start";
  conf_entry->config_entry_description=
  "Log level at start of a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_STOP_LOG_LEVEL]= 56;
  conf_entry= &glob_conf_entry[56];
  conf_entry->config_entry_name= "log_level_stop";
  conf_entry->config_entry_description=
  "Log level at stop of a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_STAT_LOG_LEVEL]= 57;
  conf_entry= &glob_conf_entry[57];
  conf_entry->config_entry_name= "log_level_statistics";
  conf_entry->config_entry_description=
  "Log level of statistics on a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_CHECKPOINT_LOG_LEVEL]= 58;
  conf_entry= &glob_conf_entry[58];
  conf_entry->config_entry_name= "log_level_checkpoint";
  conf_entry->config_entry_description=
  "Log level at checkpoint of a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_RESTART_LOG_LEVEL]= 59;
  conf_entry= &glob_conf_entry[59];
  conf_entry->config_entry_name= "log_level_restart";
  conf_entry->config_entry_description=
  "Log level at restart of a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_CONNECTION_LOG_LEVEL]= 60;
  conf_entry= &glob_conf_entry[60];
  conf_entry->config_entry_name= "log_level_connection";
  conf_entry->config_entry_description=
  "Log level of connections to a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_REPORT_LOG_LEVEL]= 61;
  conf_entry= &glob_conf_entry[61];
  conf_entry->config_entry_name= "log_level_reports";
  conf_entry->config_entry_description=
  "Log level of reports from a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_WARNING_LOG_LEVEL]= 62;
  conf_entry= &glob_conf_entry[62];
  conf_entry->config_entry_name= "log_level_warning";
  conf_entry->config_entry_description=
  "Log level of warnings from a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_ERROR_LOG_LEVEL]= 63;
  conf_entry= &glob_conf_entry[63];
  conf_entry->config_entry_name= "log_level_error";
  conf_entry->config_entry_description=
  "Log level of errors from a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_CONGESTION_LOG_LEVEL]= 64;
  conf_entry= &glob_conf_entry[64];
  conf_entry->config_entry_name= "log_level_congestion";
  conf_entry->config_entry_description=
  "Log level of congestions to a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_DEBUG_LOG_LEVEL]= 65;
  conf_entry= &glob_conf_entry[65];
  conf_entry->config_entry_name= "log_level_debug";
  conf_entry->config_entry_description=
  "Log level of debug messages from a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_BACKUP_LOG_LEVEL]= 66;
  conf_entry= &glob_conf_entry[66];
  conf_entry->config_entry_name= "log_level_backup";
  conf_entry->config_entry_description=
  "Log level of backups at a node";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 15;
  conf_entry->default_value= 8;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[127]= 67;
  conf_entry= &glob_conf_entry[67];
  conf_entry->is_deprecated= TRUE;

  map_config_id[128]= 68;
  conf_entry= &glob_conf_entry[68];
  conf_entry->is_deprecated= TRUE;

  map_config_id[137]= 69;
  conf_entry= &glob_conf_entry[69];
  conf_entry->is_deprecated= TRUE;

  map_config_id[138]= 70;
  conf_entry= &glob_conf_entry[70];
  conf_entry->is_deprecated= TRUE;

  map_config_id[154]= 71;
  conf_entry= &glob_conf_entry[71];
  conf_entry->is_deprecated= TRUE;

  map_config_id[155]= 72;
  conf_entry= &glob_conf_entry[72];
  conf_entry->is_deprecated= TRUE;

  map_config_id[KERNEL_INJECT_FAULT]= 73;
  conf_entry= &glob_conf_entry[73];
  conf_entry->config_entry_name= "inject_fault";
  conf_entry->config_entry_description=
  "Inject faults (only available in special test builds)";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 2;
  conf_entry->default_value= 2;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_SCHEDULER_NO_SEND_TIME]= 74;
  conf_entry= &glob_conf_entry[74];
  conf_entry->config_entry_name= "kernel_scheduler_no_send_time";
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without sending socket buffers";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 10999;
  conf_entry->default_value= 0;
  conf_entry->min_version_used= 0x5010D;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_SCHEDULER_NO_SLEEP_TIME]= 75;
  conf_entry= &glob_conf_entry[75];
  conf_entry->config_entry_name= "kernel_scheduler_no_sleep_time";
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without going to sleep";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 10999;
  conf_entry->default_value= 0;
  conf_entry->min_version_used= 0x5010D;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_RT_SCHEDULER_THREADS]= 76;
  conf_entry= &glob_conf_entry[76];
  conf_entry->config_entry_name= "kernel_rt_scheduler_thread";
  conf_entry->config_entry_description=
  "If set the kernel is setting its thread in RT priority, requires root privileges";
  conf_entry->is_boolean= TRUE;
  conf_entry->default_value= FALSE;
  conf_entry->min_version_used= 0x5010D;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_LOCK_MAIN_THREAD]= 77;
  conf_entry= &glob_conf_entry[77];
  conf_entry->config_entry_name= "kernel_lock_main_thread";
  conf_entry->config_entry_description=
  "Lock Main Thread to a CPU id";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 65535;
  conf_entry->default_value= 65535;
  conf_entry->min_version_used= 0x5010D;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[KERNEL_LOCK_OTHER_THREADS]= 78;
  conf_entry= &glob_conf_entry[78];
  conf_entry->config_entry_name= "kernel_lock_other_threads";
  conf_entry->config_entry_description=
  "Lock other threads to a CPU id";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 65535;
  conf_entry->default_value= 65535;
  conf_entry->min_version_used= 0x5010D;
  conf_entry->node_type= (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

/*
  This is the TCP configuration section.
*/
  map_config_id[TCP_FIRST_NODE_ID]= 100;
  conf_entry= &glob_conf_entry[100];
  conf_entry->config_entry_name= "first_node_id";
  conf_entry->config_entry_description=
  "First node id of the connection";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= MAX_NODE_ID;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[TCP_SECOND_NODE_ID]= 101;
  conf_entry= &glob_conf_entry[101];
  conf_entry->config_entry_name= "second_node_id";
  conf_entry->config_entry_description=
  "Second node id of the connection";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= MAX_NODE_ID;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[TCP_USE_MESSAGE_ID]= 102;
  conf_entry= &glob_conf_entry[102];
  conf_entry->config_entry_name= "use_message_id";
  conf_entry->config_entry_description=
  "Using message id can be a valuable resource to find problems related to distributed execution";
  conf_entry->is_boolean= TRUE;
  conf_entry->default_value= FALSE;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_USE_CHECKSUM]= 103;
  conf_entry= &glob_conf_entry[103];
  conf_entry->config_entry_name= "use_checksum";
  conf_entry->config_entry_description=
  "Using checksum ensures that internal bugs doesn't corrupt data while data is placed in buffers";
  conf_entry->is_boolean= TRUE;
  conf_entry->default_value= FALSE;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_CLIENT_PORT_NUMBER]= 104;
  conf_entry= &glob_conf_entry[104];
  conf_entry->config_entry_name= "client_port_number";
  conf_entry->config_entry_description=
  "Port number to use on client side";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1024;
  conf_entry->max_value= 65535;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;
  conf_entry->is_only_iclaustron= TRUE;

  map_config_id[TCP_SERVER_PORT_NUMBER]= 105;
  conf_entry= &glob_conf_entry[105];
  conf_entry->config_entry_name= "server_port_number";
  conf_entry->config_entry_description=
  "Port number to use on server side";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1024;
  conf_entry->max_value= 65535;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[TCP_WRITE_BUFFER_SIZE]= 106;
  conf_entry= &glob_conf_entry[106];
  conf_entry->config_entry_name= "tcp_write_buffer_size";
  conf_entry->config_entry_description=
  "Size of write buffer in front of TCP socket";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 128 * 1024;
  conf_entry->default_value= 256 * 1024;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_READ_BUFFER_SIZE]= 107;
  conf_entry= &glob_conf_entry[107];
  conf_entry->config_entry_name= "tcp_buffer_read_size";
  conf_entry->config_entry_description=
  "Size of read buffer in front of TCP socket";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->min_value= 64 * 1024;
  conf_entry->max_value= 64 * 1024;
  conf_entry->default_value= 64 * 1024;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_FIRST_HOSTNAME]= 108;
  conf_entry= &glob_conf_entry[108];
  conf_entry->config_entry_name= "first_hostname";
  conf_entry->config_entry_description=
  "Hostname of first node";
  conf_entry->is_string_type= TRUE;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_SECOND_HOSTNAME]= 109;
  conf_entry= &glob_conf_entry[109];
  conf_entry->config_entry_name= "second_hostname";
  conf_entry->config_entry_description=
  "Hostname of second node";
  conf_entry->is_string_type= TRUE;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_GROUP]= 110;
  conf_entry= &glob_conf_entry[110];
  conf_entry->config_entry_name= "tcp_group";
  conf_entry->config_entry_description=
  "Group id of the connection";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->min_value= 55;
  conf_entry->max_value= 55;
  conf_entry->default_value= 55;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[TCP_SERVER_NODE_ID]= 111;
  conf_entry= &glob_conf_entry[111];
  conf_entry->config_entry_name= "server_node_id";
  conf_entry->config_entry_description=
  "Node id of node that is server part of connection";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= MAX_NODE_ID;
  conf_entry->is_mandatory_to_specify= 1;
  conf_entry->node_type= (1 << IC_COMM_TYPE);
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

/*
  This is the cluster server configuration section.
*/
  map_config_id[CLUSTER_SERVER_EVENT_LOG]= 150;
  conf_entry= &glob_conf_entry[150];
  conf_entry->config_entry_name= "cluster_server_event_log";
  conf_entry->config_entry_description=
  "Type of cluster event log";
  conf_entry->is_string_type= TRUE;
  conf_entry->default_string= (char*)empty_string;
  conf_entry->node_type= (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->change_variant= IC_INITIAL_NODE_RESTART;

  map_config_id[CLUSTER_SERVER_PORT_NUMBER]= 151;
  conf_entry= &glob_conf_entry[151];
  conf_entry->config_entry_name= "cluster_server_port_number";
  conf_entry->config_entry_description=
  "Port number that cluster server will listen";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1024;
  conf_entry->max_value= 65535;
  conf_entry->default_value= 2286;
  conf_entry->node_type= (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

/*
  This is the client configuration section.
*/

  map_config_id[CLIENT_RESOLVE_RANK]= 200;
  conf_entry= &glob_conf_entry[200];
  conf_entry->config_entry_name= "client_resolve_rank";
  conf_entry->config_entry_description=
  "Rank in resolving network partition of the client";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 0;
  conf_entry->max_value= 2;
  conf_entry->default_value= 0;
  conf_entry->node_type= (1 << IC_CLIENT_TYPE) + (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[CLIENT_RESOLVE_TIMER]= 201;
  conf_entry= &glob_conf_entry[201];
  conf_entry->config_entry_name= "client_resolve_timer";
  conf_entry->config_entry_description=
  "Time in ms waiting for resolve before crashing";
  conf_entry->default_value= 0;
  conf_entry->node_type= (1 << IC_CLIENT_TYPE) + (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[CLIENT_BATCH_SIZE]= 202;
  conf_entry= &glob_conf_entry[202];
  conf_entry->config_entry_name= "client_batch_size";
  conf_entry->config_entry_description=
  "Size in number of records of batches in scan operations";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 1;
  conf_entry->max_value= 992;
  conf_entry->default_value= 64;
  conf_entry->node_type= (1 << IC_CLIENT_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[CLIENT_BATCH_BYTE_SIZE]= 203;
  conf_entry= &glob_conf_entry[203];
  conf_entry->config_entry_name= "client_batch_byte_size";
  conf_entry->config_entry_description=
  "Size in bytes of batches in scan operations";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 128;
  conf_entry->max_value= 65536;
  conf_entry->default_value= 8192;
  conf_entry->node_type= (1 << IC_CLIENT_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[CLIENT_MAX_BATCH_BYTE_SIZE]= 204;
  conf_entry= &glob_conf_entry[204];
  conf_entry->config_entry_name= "client_max_batch_byte_size";
  conf_entry->config_entry_description=
  "Size in bytes of max of the sum of the batches in a scan operations";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->min_value= 32 * 1024;
  conf_entry->max_value= 4 * 1024 * 1024;
  conf_entry->default_value= 256 * 1024;
  conf_entry->node_type= (1 << IC_CLIENT_TYPE);
  conf_entry->change_variant= IC_ONLINE_CHANGE;

  map_config_id[IC_NODE_DATA_PATH]= 250;
  conf_entry= &glob_conf_entry[250];
  conf_entry->config_entry_name= "node_data_path";
  conf_entry->config_entry_description=
  "Data directory of the node";
  conf_entry->is_string_type= TRUE;
  conf_entry->default_string= (char*)empty_string;
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->node_type= (1 << IC_CLUSTER_SERVER_TYPE) +
                         (1 << IC_CLIENT_TYPE) +
                         (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_INITIAL_NODE_RESTART;

  map_config_id[IC_NODE_HOST]= 251;
  conf_entry= &glob_conf_entry[251];
  conf_entry->config_entry_name= "hostname";
  conf_entry->config_entry_description=
  "Hostname of the node";
  conf_entry->is_string_type= TRUE;
  conf_entry->default_string= (char*)empty_string;
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->node_type= (1 << IC_CLUSTER_SERVER_TYPE) +
                         (1 << IC_CLIENT_TYPE) +
                         (1 << IC_KERNEL_TYPE);
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;
  return;
}

static struct config_entry *get_config_entry(int config_id)
{
  guint32 inx;

  if (config_id > 998)
  {
    printf("config_id larger than 998 = %d\n", config_id);
    return NULL;
  }
  inx= map_config_id[config_id];
  if (!inx)
  {
    printf("No config entry for config_id %u\n", config_id);
    return NULL;
  }
  return &glob_conf_entry[inx];
}

static int
ic_check_config_value(int config_id, guint64 value,
                      enum ic_config_type conf_type)
{
  struct config_entry *conf_entry= get_config_entry(config_id);

  if (!conf_entry)
  {
    printf("Didn't find config entry for config_id = %d\n", config_id);
    return PROTOCOL_ERROR;
  }
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0;
  if (!(conf_entry->node_type & (1 << conf_type)) ||
      (conf_entry->is_boolean && (value > 1)) ||
      (conf_entry->is_min_value_defined &&
       (conf_entry->min_value > value)) ||
      (conf_entry->is_max_value_defined &&
       (conf_entry->max_value < value)))
  {
    printf("Config value error config_id = %d\n", config_id);
    return PROTOCOL_ERROR;
  }
  return 0;
}

static int
ic_check_config_string(int config_id, enum ic_config_type conf_type)
{
  struct config_entry *conf_entry= get_config_entry(config_id);

  if (!conf_entry)
    return PROTOCOL_ERROR;
  if (!conf_entry->is_string_type ||
      !(conf_entry->node_type & (1 << conf_type)))
  {
    printf("conf_type = %u, node_type = %u, config_id = %u\n",
           conf_type, conf_entry->node_type, config_id);
    printf("Debug string inconsistency\n");
    return PROTOCOL_ERROR;
  }
  return 0;
}

static void
init_config_kernel_object(struct ic_kernel_node_config *kernel_conf)
{
  kernel_conf->filesystem_path= NULL;
  kernel_conf->checkpoint_path= NULL;
  kernel_conf->node_data_path= (char*)empty_string;
  kernel_conf->hostname= (char*)empty_string;

  kernel_conf->size_of_ram_memory= 
    get_default_value_uint64(KERNEL_RAM_MEMORY);
  kernel_conf->size_of_hash_memory= 
    get_default_value_uint64(KERNEL_HASH_MEMORY);
  kernel_conf->page_cache_size= 
    get_default_value_uint64(KERNEL_PAGE_CACHE_SIZE);
  kernel_conf->kernel_memory_pool= 
    get_default_value_uint64(KERNEL_MEMORY_POOL);

  kernel_conf->number_of_replicas= 0;
  kernel_conf->max_number_of_trace_files= 
    get_default_value_uint32(KERNEL_MAX_TRACE_FILES);
  kernel_conf->number_of_table_objects= 
    get_default_value_uint32(KERNEL_TABLE_OBJECTS);
  kernel_conf->number_of_column_objects= 
    get_default_value_uint32(KERNEL_COLUMN_OBJECTS);
  kernel_conf->number_of_key_objects= 
    get_default_value_uint32(KERNEL_KEY_OBJECTS);
  kernel_conf->number_of_internal_trigger_objects= 
    get_default_value_uint32(KERNEL_INTERNAL_TRIGGER_OBJECTS);
  kernel_conf->number_of_connection_objects= 
    get_default_value_uint32(KERNEL_CONNECTION_OBJECTS);
  kernel_conf->number_of_operation_objects= 
    get_default_value_uint32(KERNEL_OPERATION_OBJECTS);
  kernel_conf->number_of_scan_objects= 
    get_default_value_uint32(KERNEL_SCAN_OBJECTS);
  kernel_conf->number_of_key_operation_objects= 
    get_default_value_uint32(KERNEL_KEY_OPERATION_OBJECTS);
  kernel_conf->use_unswappable_memory= 
    get_default_value_uint32(KERNEL_LOCK_MEMORY);
  kernel_conf->timer_wait_partial_start= 
    get_default_value_uint32(KERNEL_WAIT_PARTIAL_START);
  kernel_conf->timer_wait_partitioned_start= 
    get_default_value_uint32(KERNEL_WAIT_PARTITIONED_START);
  kernel_conf->timer_wait_error_start= 
    get_default_value_uint32(KERNEL_WAIT_ERROR_START);
  kernel_conf->timer_heartbeat_kernel_nodes= 
    get_default_value_uint32(KERNEL_HEARTBEAT_TIMER);
  kernel_conf->timer_heartbeat_client_nodes= 
    get_default_value_uint32(KERNEL_CLIENT_HEARTBEAT_TIMER);
  kernel_conf->timer_local_checkpoint= 
    get_default_value_uint32(KERNEL_LOCAL_CHECKPOINT_TIMER);
  kernel_conf->timer_global_checkpoint= 
    get_default_value_uint32(KERNEL_GLOBAL_CHECKPOINT_TIMER);
  kernel_conf->timer_resolve= 
    get_default_value_uint32(KERNEL_RESOLVE_TIMER);
  kernel_conf->timer_kernel_watchdog= 
    get_default_value_uint32(KERNEL_WATCHDOG_TIMER);
  kernel_conf->number_of_redo_log_files= 
    get_default_value_uint32(KERNEL_REDO_LOG_FILES);
  kernel_conf->timer_check_interval= 
    get_default_value_uint32(KERNEL_CHECK_INTERVAL);
  kernel_conf->timer_client_activity= 
    get_default_value_uint32(KERNEL_CLIENT_ACTIVITY_TIMER);
  kernel_conf->timer_deadlock= 
    get_default_value_uint32(KERNEL_DEADLOCK_TIMER);
  kernel_conf->number_of_ordered_key_objects= 
    get_default_value_uint32(KERNEL_ORDERED_KEY_OBJECTS);
  kernel_conf->number_of_unique_hash_key_objects= 
    get_default_value_uint32(KERNEL_UNIQUE_HASH_KEY_OBJECTS);
  kernel_conf->redo_log_memory= 
    get_default_value_uint32(KERNEL_REDO_LOG_MEMORY);
  kernel_conf->kernel_file_synch_size= 
    get_default_value_uint32(KERNEL_FILE_SYNCH_SIZE);
  kernel_conf->kernel_disk_write_speed= 
    get_default_value_uint32(KERNEL_DISK_WRITE_SPEED);
  kernel_conf->kernel_disk_write_speed_start= 
    get_default_value_uint32(KERNEL_DISK_WRITE_SPEED_START);
  kernel_conf->log_level_start= 
    get_default_value_uint32(KERNEL_START_LOG_LEVEL);
  kernel_conf->log_level_stop= 
    get_default_value_uint32(KERNEL_STOP_LOG_LEVEL);
  kernel_conf->log_level_statistics= 
    get_default_value_uint32(KERNEL_STAT_LOG_LEVEL);
  kernel_conf->log_level_checkpoint= 
    get_default_value_uint32(KERNEL_CHECKPOINT_LOG_LEVEL);
  kernel_conf->log_level_restart= 
    get_default_value_uint32(KERNEL_RESTART_LOG_LEVEL);
  kernel_conf->log_level_connection= 
    get_default_value_uint32(KERNEL_CONNECTION_LOG_LEVEL);
  kernel_conf->log_level_reports= 
    get_default_value_uint32(KERNEL_REPORT_LOG_LEVEL);
  kernel_conf->log_level_warning= 
    get_default_value_uint32(KERNEL_WARNING_LOG_LEVEL);
  kernel_conf->log_level_error= 
    get_default_value_uint32(KERNEL_ERROR_LOG_LEVEL);
  kernel_conf->log_level_congestion= 
    get_default_value_uint32(KERNEL_CONGESTION_LOG_LEVEL);
  kernel_conf->log_level_debug= 
    get_default_value_uint32(KERNEL_DEBUG_LOG_LEVEL);
  kernel_conf->log_level_backup= 
    get_default_value_uint32(KERNEL_BACKUP_LOG_LEVEL);
  kernel_conf->inject_fault= 
    get_default_value_uint32(KERNEL_INJECT_FAULT);
  kernel_conf->kernel_scheduler_no_send_time= 
    get_default_value_uint32(KERNEL_SCHEDULER_NO_SEND_TIME);
  kernel_conf->kernel_scheduler_no_sleep_time= 
    get_default_value_uint32(KERNEL_SCHEDULER_NO_SLEEP_TIME);
  kernel_conf->kernel_lock_main_thread= 
    get_default_value_uint32(KERNEL_LOCK_MAIN_THREAD);
  kernel_conf->kernel_lock_other_threads= 
    get_default_value_uint32(KERNEL_LOCK_OTHER_THREADS);

  kernel_conf->kernel_volatile_mode= 
    get_default_value_uchar(KERNEL_VOLATILE_MODE);
  kernel_conf->kernel_automatic_restart= 
    get_default_value_uchar(KERNEL_DAEMON_RESTART_AT_ERROR);
  kernel_conf->kernel_rt_scheduler_threads= 
    get_default_value_uchar(KERNEL_RT_SCHEDULER_THREADS);
}

static void
init_config_client_object(struct ic_client_node_config *client_conf)
{
  client_conf->hostname= (char*)empty_string;
  client_conf->node_data_path= (char*)empty_string;

  client_conf->client_max_batch_byte_size=
    get_default_value_uint32(CLIENT_MAX_BATCH_BYTE_SIZE);
  client_conf->client_batch_byte_size=
    get_default_value_uint32(CLIENT_BATCH_BYTE_SIZE);
  client_conf->client_batch_size=
    get_default_value_uint32(CLIENT_BATCH_SIZE);
  client_conf->client_resolve_rank=
    get_default_value_uint32(CLIENT_RESOLVE_RANK);
  client_conf->client_resolve_timer=
    get_default_value_uint32(CLIENT_RESOLVE_TIMER);
}

static void
init_config_cluster_server_object( struct ic_cluster_server_config *cs_conf)
{
  cs_conf->hostname= (char*)empty_string;
  cs_conf->node_data_path= (char*)empty_string;

  cs_conf->client_resolve_rank=
    get_default_value_uint32(CLIENT_RESOLVE_RANK);
  cs_conf->client_resolve_timer=
    get_default_value_uint32(CLIENT_RESOLVE_TIMER);
  cs_conf->cluster_server_event_log=
    get_default_value_uint32(CLUSTER_SERVER_EVENT_LOG);
  cs_conf->cluster_server_port_number=
    get_default_value_uint32(CLUSTER_SERVER_PORT_NUMBER);
}

static void
init_config_comm_object(struct ic_comm_link_config *comm_conf)
{
  comm_conf->tcp_conf.first_host_name= (char*)empty_string;
  comm_conf->tcp_conf.second_host_name= (char*)empty_string;

  comm_conf->tcp_conf.first_node_id= 0;
  comm_conf->tcp_conf.second_node_id= 0;

  comm_conf->tcp_conf.tcp_write_buffer_size= 
    get_default_value_uint32(TCP_WRITE_BUFFER_SIZE);
  comm_conf->tcp_conf.tcp_read_buffer_size= 
    get_default_value_uint32(TCP_READ_BUFFER_SIZE);
  comm_conf->tcp_conf.client_port_number= 
    get_default_value_uint32(TCP_WRITE_BUFFER_SIZE);
  comm_conf->tcp_conf.tcp_write_buffer_size= 
    get_default_value_uint32(TCP_WRITE_BUFFER_SIZE);
  comm_conf->tcp_conf.tcp_write_buffer_size= 
    get_default_value_uint32(TCP_WRITE_BUFFER_SIZE);
}

/*
  CONFIGURATION TRANSLATE KEY DATA TO CONFIGURATION OBJECTS MODULE
  ----------------------------------------------------------------
*/

static int
allocate_mem_phase1(struct ic_cluster_config *conf_obj)
{
  /*
    Allocate memory for pointer arrays pointing to the configurations of the
    nodes in the cluster, also allocate memory for array of node types.
  */
  conf_obj->node_types= g_try_malloc0(conf_obj->no_of_nodes *
                                         sizeof(enum ic_node_type));
  conf_obj->comm_types= g_try_malloc0(conf_obj->no_of_nodes *
                                         sizeof(enum ic_communication_type));
  conf_obj->node_ids= g_try_malloc0(conf_obj->no_of_nodes *
                                       sizeof(guint32));
  conf_obj->node_config= g_try_malloc0(conf_obj->no_of_nodes *
                                          sizeof(char*));
  if (!conf_obj->node_types || !conf_obj->node_ids ||
      !conf_obj->comm_types || !conf_obj->node_config)
    return MEM_ALLOC_ERROR;
  return 0;
}


static int
allocate_mem_phase2(struct ic_cluster_config *conf_obj)
{
  guint32 i;
  guint32 size_config_objects= 0;
  char *conf_obj_ptr, *string_mem;
  /*
    Allocate memory for the actual configuration objects for nodes and
    communication sections.
  */
  for (i= 0; i < conf_obj->no_of_nodes; i++)
  {
    switch (conf_obj->node_types[i])
    {
      case IC_KERNEL_TYPE:
        size_config_objects+= sizeof(struct ic_kernel_node_config);
        break;
      case IC_CLIENT_TYPE:
        size_config_objects+= sizeof(struct ic_client_node_config);
        break;
      case IC_CLUSTER_SERVER_TYPE:
        size_config_objects+= sizeof(struct ic_cluster_server_config);
        break;
      default:
        g_assert(FALSE);
        break;
    }
  }
  size_config_objects+= conf_obj->no_of_comms *
                        sizeof(struct ic_comm_link_config);
  if (!(conf_obj->comm_config= g_try_malloc0(
                     conf_obj->no_of_comms * sizeof(char*))) ||
      !(conf_obj->string_memory_to_return= g_try_malloc0(
                     conf_obj->string_memory_size)) ||
      !(conf_obj->config_memory_to_return= g_try_malloc0(
                     size_config_objects)))
    return MEM_ALLOC_ERROR;

  conf_obj_ptr= conf_obj->config_memory_to_return;
  string_mem= conf_obj->string_memory_to_return;
  conf_obj->end_string_memory= string_mem + conf_obj->string_memory_size;
  conf_obj->next_string_memory= string_mem;

  for (i= 0; i < conf_obj->no_of_nodes; i++)
  {
    conf_obj->node_config[i]= conf_obj_ptr;
    switch (conf_obj->node_types[i])
    {
      case IC_KERNEL_TYPE:
      {
        init_config_kernel_object(
          (struct ic_kernel_node_config*)conf_obj_ptr);
        conf_obj_ptr+= sizeof(struct ic_kernel_node_config);
        break;
      }
      case IC_CLIENT_TYPE:
      {
        init_config_client_object(
          (struct ic_client_node_config*)conf_obj_ptr);
        conf_obj_ptr+= sizeof(struct ic_client_node_config);
        break;
      }
      case IC_CLUSTER_SERVER_TYPE:
      {
        init_config_cluster_server_object(
          (struct ic_cluster_server_config*)conf_obj_ptr);
        conf_obj_ptr+= sizeof(struct ic_cluster_server_config);
        break;
      }
      default:
        g_assert(FALSE);

        break;
    }
  }
  for (i= 0; i < conf_obj->no_of_comms; i++)
  {
    init_config_comm_object(
      (struct ic_comm_link_config*)conf_obj_ptr);
    conf_obj->comm_config[i]= conf_obj_ptr;
    conf_obj_ptr+= sizeof(struct ic_comm_link_config);
  }
  g_assert(conf_obj_ptr == (conf_obj->config_memory_to_return +
                            size_config_objects));
  return 0;
}

static int
hash_key_error(guint32 hash_key, guint32 node_type, guint32 key_type)
{
  printf("Protocol error for key %u node type %u type %u\n",
         hash_key, node_type, key_type);
  return PROTOCOL_ERROR;
}

static int
key_type_error(guint32 hash_key, guint32 node_type)
{
  printf("Wrong key type %u node type %u\n", hash_key, node_type);
  return PROTOCOL_ERROR;
}

static guint64
get_64bit_value(guint32 value, guint32 **key_value)
{
  guint64 long_val= value + (((guint64)(**key_value)) << 32);
  (*key_value)++;
  return long_val;
}

static void
update_string_data(struct ic_cluster_config *conf_obj, guint32 value,
                   guint32 **key_value)
{
  guint32 len_words= (value + 3)/4;
  conf_obj->next_string_memory+= value;
  g_assert(conf_obj->next_string_memory <= conf_obj->end_string_memory);
  (*key_value)+= len_words;
}

static int
analyse_node_section_phase1(struct ic_cluster_config *conf_obj,
                            guint32 sect_id, guint32 value, guint32 hash_key)
{
  if (hash_key == IC_NODE_TYPE)
  {
    DEBUG(printf("Node type of section %u is %u\n", sect_id, value));
    switch (value)
    {
      case IC_CLIENT_TYPE:
        conf_obj->no_of_client_nodes++; break;
      case IC_KERNEL_TYPE:
        conf_obj->no_of_kernel_nodes++; break;
      case IC_CLUSTER_SERVER_TYPE:
        conf_obj->no_of_cluster_servers++; break;
      default:
        printf("No such node type\n");
        return PROTOCOL_ERROR;
    }
    conf_obj->node_types[sect_id - 2]= (enum ic_node_type)value;
  }
  else if (hash_key == IC_NODE_ID)
  {
    conf_obj->node_ids[sect_id - 2]= value;
    DEBUG(printf("Node id = %u for section %u\n", value, sect_id));
  }
  return 0;
}

static int
step_key_value(struct ic_cluster_config *conf_obj,
               guint32 key_type, guint32 **key_value,
               guint32 value, guint32 *key_value_end, int pass)
{
  guint32 len_words;
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
      break;
    case IC_CL_SECT_TYPE:
      break;
    case IC_CL_INT64_TYPE:
      (*key_value)++;
      break;
    case IC_CL_CHAR_TYPE:
    {
      len_words= (value + 3)/4;
      if (((*key_value) + len_words) >= key_value_end)
      {
        printf("Array ended in the middle of a type\n");
        return PROTOCOL_ERROR;
      }
      if (value != (strlen((char*)(*key_value)) + 1))
      {
        printf("Wrong length of character type\n");
        return PROTOCOL_ERROR;
      }
      (*key_value)+= len_words;
      if (pass == 1)
        conf_obj->string_memory_size+= value;
      break;
   }
   default:
     return key_type_error(key_type, 32);
  }
  return 0;
}

static struct config_entry *get_conf_entry(guint32 hash_key)
{
  guint32 id;
  struct config_entry *conf_entry;

  if (hash_key < MAX_MAP_CONFIG_ID)
  {
    id= map_config_id[hash_key];
    if (id < MAX_CONFIG_ID)
    {
      conf_entry= &glob_conf_entry[id];
      if (!conf_entry)
      {
        DEBUG(printf("No such config entry, return NULL\n"));
      }
      return conf_entry;
    }  
  }
  DEBUG(printf("Error in map_config_id, returning NULL\n"));
  return NULL;
}

static int
read_node_section(struct ic_cluster_config *conf_obj,
                  guint32 key_type, guint32 **key_value,
                  guint32 value, guint32 hash_key,
                  guint32 node_sect_id)
{
  struct config_entry *conf_entry;
  void *node_config;
  enum ic_node_type node_type;

  if (hash_key == IC_PARENT_ID || hash_key == IC_NODE_TYPE ||
      hash_key == IC_NODE_ID)
    return 0; /* Ignore for now */
  if (!(conf_entry= get_conf_entry(hash_key)))
    return PROTOCOL_ERROR;
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0; /* Ignore */
  if (node_sect_id >= conf_obj->no_of_nodes)
  {
    printf("node_sect_id out of range\n");
    return PROTOCOL_ERROR;
  }
  if (!(node_config= (void*)conf_obj->node_config[node_sect_id]))
  {
    printf("No such node_config object\n");
    return PROTOCOL_ERROR;
  }
  node_type= conf_obj->node_types[node_sect_id];
  if (node_type == IC_KERNEL_NODE)
  {
    struct ic_kernel_node_config *kernel_conf;
    kernel_conf= (struct ic_kernel_node_config*)node_config;
    switch (key_type)
    {
      case IC_CL_INT32_TYPE:
        if (ic_check_config_value(hash_key, (guint64)value, IC_KERNEL_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case KERNEL_MAX_TRACE_FILES:
            kernel_conf->max_number_of_trace_files= value; break;
          case KERNEL_REPLICAS:
            kernel_conf->number_of_replicas= value; break;
          case KERNEL_TABLE_OBJECTS:
            kernel_conf->number_of_table_objects= value; break;
          case KERNEL_COLUMN_OBJECTS:
            kernel_conf->number_of_column_objects= value; break;
          case KERNEL_KEY_OBJECTS:
            kernel_conf->number_of_key_objects= value; break;
          case KERNEL_INTERNAL_TRIGGER_OBJECTS:
            kernel_conf->number_of_internal_trigger_objects= value; break;
          case KERNEL_CONNECTION_OBJECTS:
            kernel_conf->number_of_connection_objects= value; break;
          case KERNEL_OPERATION_OBJECTS:
            kernel_conf->number_of_operation_objects= value; break;
          case KERNEL_SCAN_OBJECTS:
            kernel_conf->number_of_scan_objects= value; break;
          case KERNEL_KEY_OPERATION_OBJECTS:
            kernel_conf->number_of_key_operation_objects= value; break;
          case KERNEL_LOCK_MEMORY:
            kernel_conf->use_unswappable_memory= value; break;
          case KERNEL_WAIT_PARTIAL_START:
            kernel_conf->timer_wait_partial_start= value; break;
          case KERNEL_WAIT_PARTITIONED_START:
            kernel_conf->timer_wait_partitioned_start= value; break;
          case KERNEL_WAIT_ERROR_START:
            kernel_conf->timer_wait_error_start= value; break;
          case KERNEL_HEARTBEAT_TIMER:
            kernel_conf->timer_heartbeat_kernel_nodes= value; break;
          case KERNEL_CLIENT_HEARTBEAT_TIMER:
            kernel_conf->timer_heartbeat_client_nodes= value; break;
          case KERNEL_LOCAL_CHECKPOINT_TIMER:
            kernel_conf->timer_local_checkpoint= value; break;
          case KERNEL_GLOBAL_CHECKPOINT_TIMER:
            kernel_conf->timer_global_checkpoint= value; break;
          case KERNEL_RESOLVE_TIMER:
            kernel_conf->timer_resolve= value; break;
          case KERNEL_WATCHDOG_TIMER:
            kernel_conf->timer_kernel_watchdog= value; break;
          case KERNEL_REDO_LOG_FILES:
            kernel_conf->number_of_redo_log_files= value; break;
          case KERNEL_CHECK_INTERVAL:
            kernel_conf->timer_check_interval= value; break;
          case KERNEL_CLIENT_ACTIVITY_TIMER:
            kernel_conf->timer_client_activity= value; break;
          case KERNEL_DEADLOCK_TIMER:
            kernel_conf->timer_deadlock= value; break;
          case KERNEL_VOLATILE_MODE:
            kernel_conf->kernel_volatile_mode= (gchar)value; break;
          case KERNEL_ORDERED_KEY_OBJECTS:
            kernel_conf->number_of_ordered_key_objects= value; break;
          case KERNEL_UNIQUE_HASH_KEY_OBJECTS:
            kernel_conf->number_of_unique_hash_key_objects= value; break;
          case KERNEL_REDO_LOG_MEMORY:
            kernel_conf->redo_log_memory= value; break;
          case KERNEL_START_LOG_LEVEL:
            kernel_conf->log_level_start= value; break;
          case KERNEL_STOP_LOG_LEVEL:
            kernel_conf->log_level_stop= value; break;
          case KERNEL_STAT_LOG_LEVEL:
            kernel_conf->log_level_statistics= value; break;
          case KERNEL_CHECKPOINT_LOG_LEVEL:
            kernel_conf->log_level_checkpoint= value; break;
          case KERNEL_RESTART_LOG_LEVEL:
            kernel_conf->log_level_restart= value; break;
          case KERNEL_CONNECTION_LOG_LEVEL:
            kernel_conf->log_level_connection= value; break;
          case KERNEL_REPORT_LOG_LEVEL:
            kernel_conf->log_level_reports= value; break;
          case KERNEL_WARNING_LOG_LEVEL:
            kernel_conf->log_level_warning= value; break;
          case KERNEL_ERROR_LOG_LEVEL:
            kernel_conf->log_level_error= value; break;
          case KERNEL_DEBUG_LOG_LEVEL:
            kernel_conf->log_level_debug= value; break;
          case KERNEL_BACKUP_LOG_LEVEL:
            kernel_conf->log_level_backup= value; break;
          case KERNEL_CONGESTION_LOG_LEVEL:
            kernel_conf->log_level_congestion= value; break;
          case KERNEL_INJECT_FAULT:
            kernel_conf->inject_fault= value; break;
          case KERNEL_DISK_WRITE_SPEED:
            kernel_conf->kernel_disk_write_speed= value; break;
          case KERNEL_DISK_WRITE_SPEED_START:
            kernel_conf->kernel_disk_write_speed_start= value; break;
          case KERNEL_FILE_SYNCH_SIZE:
            kernel_conf->kernel_file_synch_size= value; break;
          case KERNEL_DAEMON_RESTART_AT_ERROR:
            kernel_conf->kernel_automatic_restart= (gchar)value; break;
          case KERNEL_SCHEDULER_NO_SEND_TIME:
            kernel_conf->kernel_scheduler_no_send_time= value; break;
          case KERNEL_SCHEDULER_NO_SLEEP_TIME:
            kernel_conf->kernel_scheduler_no_sleep_time= value; break;
          case KERNEL_RT_SCHEDULER_THREADS:
            kernel_conf->kernel_rt_scheduler_threads= (gchar)value; break;
          case KERNEL_LOCK_MAIN_THREAD:
            kernel_conf->kernel_lock_main_thread= value; break;
          case KERNEL_LOCK_OTHER_THREADS:
            kernel_conf->kernel_lock_other_threads= value; break;
          case KERNEL_DUMMY:
          case KERNEL_INITIAL_OPEN_FILES:
          case KERNEL_MAX_OPEN_FILES:
          case KERNEL_LONG_MESSAGE_MEMORY:
          case KERNEL_SCAN_BATCH_SIZE:
          case KERNEL_LOCAL_SCAN_OBJECTS:
          case KERNEL_LOCAL_OPERATION_OBJECTS:
          case KERNEL_CHECKPOINT_OBJECTS:
          case KERNEL_CHECKPOINT_MEMORY:
          case KERNEL_CHECKPOINT_DATA_MEMORY:
          case KERNEL_CHECKPOINT_LOG_MEMORY:
          case KERNEL_CHECKPOINT_WRITE_SIZE:
          case KERNEL_CHECKPOINT_MAX_WRITE_SIZE:
          case KERNEL_CONNECTION_BUFFER:
          case KERNEL_INTERNAL_TRIGGER_OPERATION_OBJECTS:
          case KERNEL_STRING_MEMORY:
            break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        break;
      case IC_CL_INT64_TYPE:
      {
        guint64 long_val= get_64bit_value(value, key_value);
        if (ic_check_config_value(hash_key, long_val, IC_KERNEL_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case KERNEL_RAM_MEMORY:
            kernel_conf->size_of_ram_memory= long_val; break;
          case KERNEL_HASH_MEMORY:
            kernel_conf->size_of_hash_memory= long_val; break;
          case KERNEL_PAGE_CACHE_SIZE:
            kernel_conf->page_cache_size= long_val; break;
          case KERNEL_MEMORY_POOL:
            kernel_conf->kernel_memory_pool= long_val; break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        break;
      }
      case IC_CL_CHAR_TYPE:
      {
        if (ic_check_config_string(hash_key, IC_KERNEL_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case IC_NODE_DATA_PATH:
            kernel_conf->node_data_path= conf_obj->next_string_memory;
            strcpy(kernel_conf->node_data_path, (char*)(*key_value));
            break;
          case IC_NODE_HOST:
            kernel_conf->hostname= conf_obj->next_string_memory;
            strcpy(kernel_conf->hostname, (char*)(*key_value));
            break;
          case KERNEL_FILESYSTEM_PATH:
            kernel_conf->filesystem_path= conf_obj->next_string_memory;
            strcpy(kernel_conf->filesystem_path, (char*)(*key_value));
            break;
          case KERNEL_CHECKPOINT_PATH:
            kernel_conf->checkpoint_path= conf_obj->next_string_memory;
            strcpy(kernel_conf->checkpoint_path, (char*)(*key_value));
            break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        update_string_data(conf_obj, value, key_value);
        break;
      }
      default:
        return key_type_error(hash_key, node_type);
    }
  }
  else if (node_type == IC_CLIENT_NODE)
  {
    struct ic_client_node_config *client_conf;
    client_conf= (struct ic_client_node_config*)node_config;
    switch (key_type)
    {
      case IC_CL_INT32_TYPE:
      {
        if (ic_check_config_value(hash_key, (guint64)value, IC_CLIENT_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case CLIENT_MAX_BATCH_BYTE_SIZE:
            client_conf->client_max_batch_byte_size= value; break;
          case CLIENT_BATCH_BYTE_SIZE:
            client_conf->client_batch_byte_size= value; break;
          case CLIENT_BATCH_SIZE:
            client_conf->client_batch_size= value; break;
          case CLIENT_RESOLVE_RANK:
            client_conf->client_resolve_rank= value; break;
          case CLIENT_RESOLVE_TIMER:
            client_conf->client_resolve_timer= value; break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        break;
      }
      case IC_CL_CHAR_TYPE:
      {
        if (ic_check_config_string(hash_key, IC_KERNEL_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case IC_NODE_DATA_PATH:
            client_conf->node_data_path= conf_obj->next_string_memory;
            strcpy(client_conf->node_data_path, (char*)(*key_value));
            break;
          case IC_NODE_HOST:
            client_conf->hostname= conf_obj->next_string_memory;
            strcpy(client_conf->hostname, (char*)(*key_value));
            break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        update_string_data(conf_obj, value, key_value);
        break;
      }
      default:
        return key_type_error(hash_key, node_type);
    }
  }
  else if (node_type == IC_CLUSTER_SERVER_NODE)
  {
    struct ic_cluster_server_config *cs_conf;
    cs_conf= (struct ic_cluster_server_config*)node_config;
    switch (key_type)
    {
      case IC_CL_INT32_TYPE:
      {
        if (ic_check_config_value(hash_key, (guint64)value,
                                  IC_CLUSTER_SERVER_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case CLIENT_RESOLVE_RANK:
            cs_conf->client_resolve_rank= value; break;
          case CLIENT_RESOLVE_TIMER:
            cs_conf->client_resolve_timer= value; break;
          case CLUSTER_SERVER_PORT_NUMBER:
            cs_conf->cluster_server_port_number= value; break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        break;
      }
      case IC_CL_CHAR_TYPE:
      {
        if (ic_check_config_string(hash_key, IC_CLUSTER_SERVER_TYPE))
          return PROTOCOL_ERROR;
        switch (hash_key)
        {
          case IC_NODE_DATA_PATH:
            cs_conf->node_data_path= conf_obj->next_string_memory;
            strcpy(cs_conf->node_data_path, (char*)(*key_value));
            break;
          case IC_NODE_HOST:
            cs_conf->hostname= conf_obj->next_string_memory;
            strcpy(cs_conf->hostname, (char*)(*key_value));
            break;
          default:
            return hash_key_error(hash_key, node_type, key_type);
        }
        update_string_data(conf_obj, value, key_value);
        break;
      }
      default:
        return key_type_error(hash_key, node_type);
    }
  }
  else
    g_assert(FALSE);
  return 0;

}

static int
read_comm_section(struct ic_cluster_config *conf_obj,
                  guint32 key_type, guint32 **key_value,
                  guint32 value, guint32 hash_key,
                  guint32 comm_sect_id)
{
  struct config_entry *conf_entry;
  struct ic_tcp_comm_link_config *tcp_conf;

  if (hash_key == IC_PARENT_ID || hash_key == IC_NODE_TYPE)
    return 0; /* Ignore */
  if (!(conf_entry= get_conf_entry(hash_key)))
    return PROTOCOL_ERROR;
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0; /* Ignore */
  g_assert(comm_sect_id < conf_obj->no_of_comms);
  tcp_conf= (struct ic_tcp_comm_link_config*)conf_obj->comm_config[comm_sect_id];
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
    {
      if (ic_check_config_value(hash_key, (guint64)value, IC_COMM_TYPE))
        return PROTOCOL_ERROR;
      switch (hash_key)
      {
        case TCP_FIRST_NODE_ID:
          tcp_conf->first_node_id= value; break;
        case TCP_SECOND_NODE_ID:
          tcp_conf->second_node_id= value; break;
        case TCP_USE_MESSAGE_ID:
          tcp_conf->use_message_id= (gchar)value; break;
        case TCP_USE_CHECKSUM:
          tcp_conf->use_checksum= (gchar)value; break;
        case TCP_CLIENT_PORT_NUMBER:
          tcp_conf->client_port_number= (guint16)value; break;
        case TCP_SERVER_PORT_NUMBER:
          tcp_conf->server_port_number= (guint16)value; break;
        case TCP_SERVER_NODE_ID:
          tcp_conf->first_node_id= (guint16)value; break;
        case TCP_WRITE_BUFFER_SIZE:
          tcp_conf->tcp_write_buffer_size= value; break;
        case TCP_READ_BUFFER_SIZE:
          tcp_conf->tcp_read_buffer_size= value; break;
        case TCP_GROUP:
          /* Ignore for now */
          break;
        default:
          return hash_key_error(hash_key, IC_COMM_TYPE, key_type);
      }
      break;
    }
    case IC_CL_CHAR_TYPE:
    {
      if (ic_check_config_string(hash_key, IC_COMM_TYPE))
        return PROTOCOL_ERROR;
      switch (hash_key)
      {
        case TCP_FIRST_HOSTNAME:
          tcp_conf->first_host_name= conf_obj->next_string_memory;
          strcpy(tcp_conf->first_host_name, (char*)(*key_value));
          break;
        case TCP_SECOND_HOSTNAME:
          tcp_conf->second_host_name= conf_obj->next_string_memory;
          strcpy(tcp_conf->second_host_name, (char*)(*key_value));
          break;
        default:
          return hash_key_error(hash_key, IC_COMM_TYPE, key_type);
      }
      update_string_data(conf_obj, value, key_value);
      break;
    }
    default:
      return key_type_error(hash_key, IC_COMM_TYPE);
  }
  return 0;
}

static int
analyse_key_value(guint32 *key_value, guint32 len, int pass,
                  struct ic_api_cluster_server *apic,
                  guint32 current_cluster_index)
{
  guint32 *key_value_end= key_value + len;
  struct ic_cluster_config *conf_obj;
  int error;

  conf_obj= apic->conf_objects+current_cluster_index;
  if (pass == 1)
  {
    if ((error= allocate_mem_phase1(conf_obj)))
      return error;
  }
  else if (pass == 2)
  {
    if ((error= allocate_mem_phase2(conf_obj)))
      return error;
  }
  while (key_value < key_value_end)
  {
    guint32 key= g_ntohl(key_value[0]);
    guint32 value= g_ntohl(key_value[1]);
    guint32 hash_key= key & IC_CL_KEY_MASK;
    guint32 sect_id= (key >> IC_CL_SECT_SHIFT) & IC_CL_SECT_MASK;
    guint32 key_type= key >> IC_CL_KEY_SHIFT;
    if (pass == 0 && sect_id == 1)
      conf_obj->no_of_nodes= MAX(conf_obj->no_of_nodes, hash_key + 1);
    else if (pass == 1)
    {
      if (sect_id == (conf_obj->no_of_nodes + 2))
        conf_obj->no_of_comms= MAX(conf_obj->no_of_comms, hash_key + 1);
      if ((sect_id > 1 && sect_id < (conf_obj->no_of_nodes + 2)))
      {
        if ((error= analyse_node_section_phase1(conf_obj, sect_id,
                                                value, hash_key)))
          return error;
      }
    }
    key_value+= 2;
    if (pass < 2)
    {
      if ((error= step_key_value(conf_obj, key_type, &key_value,
                                 value, key_value_end, pass)))
        return error;
    }
    else
    {
      if (sect_id > 1 && sect_id != (conf_obj->no_of_nodes + 2))
      {
        if (sect_id < (conf_obj->no_of_nodes + 2))
        {
          guint32 node_sect_id= sect_id - 2;
          if ((error= read_node_section(conf_obj, key_type, &key_value,
                                        value, hash_key, node_sect_id)))
            return error;
        }
        else
        {
          guint32 comm_sect_id= sect_id - (conf_obj->no_of_nodes + 3);
          if ((error= read_comm_section(conf_obj, key_type, &key_value,
                                        value, hash_key, comm_sect_id)))
            return error;
        }
      }
    }
  }
  return 0;
}

/*
  CONFIGURATION RETRIEVE MODULE
  -----------------------------
  This module contains the code to retrieve the configuation for a given cluster
  from the cluster server.
*/

static char ver_string[8] = { 0x4E, 0x44, 0x42, 0x43, 0x4F, 0x4E, 0x46, 0x56 };
static int
translate_config(struct ic_api_cluster_server *apic,
                 guint32 current_cluster_index,
                 char *config_buf,
                 guint32 config_size)
{
  char *bin_buf;
  guint32 bin_config_size, bin_config_size32, checksum, i;
  guint32 *bin_buf32, *key_value_ptr, key_value_len;
  int error, pass;

  g_assert((config_size & 3) == 0);
  bin_config_size= (config_size >> 2) * 3;
  if (!(bin_buf= g_try_malloc0(bin_config_size)))
    return MEM_ALLOC_ERROR;
  if ((error= base64_decode(bin_buf, &bin_config_size,
                            config_buf, config_size)))
  {
    printf("1:Protocol error in base64 decode\n");
    return PROTOCOL_ERROR;
  }
  bin_config_size32= bin_config_size >> 2;
  if ((bin_config_size & 3) != 0 || bin_config_size32 <= 3)
  {
    printf("2:Protocol error in base64 decode\n");
    return PROTOCOL_ERROR;
  }
  if (memcmp(bin_buf, ver_string, 8))
  {
    printf("3:Protocol error in base64 decode\n");
    return PROTOCOL_ERROR;
  }
  bin_buf32= (guint32*)bin_buf;
  checksum= 0;
  for (i= 0; i < bin_config_size32; i++)
    checksum^= g_ntohl(bin_buf32[i]);
  if (checksum)
  {
    printf("4:Protocol error in base64 decode\n");
    return PROTOCOL_ERROR;
  }
  key_value_ptr= bin_buf32 + 2;
  key_value_len= bin_config_size32 - 3;
  for (pass= 0; pass < 3; pass++)
  {
    if ((error= analyse_key_value(key_value_ptr, key_value_len, pass,
                                  apic, current_cluster_index)))
      return error;
  }
  g_free(bin_buf);
  return 0;
}

static int
set_up_cluster_server_connection(struct ic_connection *conn,
                                 guint32 server_ip,
                                 guint16 server_port)
{
  int error;

  if (ic_init_socket_object(conn, TRUE, FALSE, FALSE, FALSE,
                            NULL, NULL))
    return MEM_ALLOC_ERROR;
  conn->server_ip= server_ip;
  conn->server_port= server_port;
  if ((error= conn->conn_op.set_up_ic_connection(conn)))
  {
    DEBUG(printf("Connect failed with error %d\n", error));
    return error;
  }
  DEBUG(printf("Successfully set-up connection to cluster server\n"));
  return 0;
}

static int
send_get_nodeid(struct ic_connection *conn,
                struct ic_api_cluster_server *apic,
                guint32 current_cluster_index)
{
  char version_buf[32];
  char nodeid_buf[32];
  char endian_buf[32];
  guint32 node_id= apic->node_ids[current_cluster_index];

  g_snprintf(version_buf, 32, "%s%u", version_str, version_no);
  g_snprintf(nodeid_buf, 32, "%s%u", nodeid_str, node_id);
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
  g_snprintf(endian_buf, 32, "%s%s", endian_str, "little");
#else
  g_snprintf(endian_buf, 32, "%s%s", endian_str, "big");
#endif
  if (ic_send_with_cr(conn, get_nodeid_str) ||
      ic_send_with_cr(conn, version_buf) ||
      ic_send_with_cr(conn, nodetype_str) ||
      ic_send_with_cr(conn, nodeid_buf) ||
      ic_send_with_cr(conn, user_str) ||
      ic_send_with_cr(conn, password_str) ||
      ic_send_with_cr(conn, public_key_str) ||
      ic_send_with_cr(conn, endian_buf) ||
      ic_send_with_cr(conn, log_event_str) ||
      ic_send_with_cr(conn, empty_string))
    return conn->error_code;
  return 0;
}
static int
send_get_config(struct ic_connection *conn)
{
  char version_buf[32];

  g_snprintf(version_buf, 32, "%s%u", version_str, version_no);
  if (ic_send_with_cr(conn, get_config_str) ||
      ic_send_with_cr(conn, version_buf) ||
      ic_send_with_cr(conn, empty_string))
    return conn->error_code;
  return 0;
}

static int
rec_get_nodeid(struct ic_connection *conn,
               struct ic_api_cluster_server *apic,
               guint32 current_cluster_index)
{
  char read_buf[256];
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  int error;
  guint64 node_number;
  guint32 state= GET_NODEID_REPLY_STATE;
  while (!(error= ic_rec_with_cr(conn, read_buf, &read_size,
                                 &size_curr_buf, sizeof(read_buf))))
  {
    DEBUG(ic_print_buf(read_buf, read_size));
    switch (state)
    {
      case GET_NODEID_REPLY_STATE:
        /*
          The protocol is decoded in the order of the case statements in the switch
          statements.
        
          Receive:
          get nodeid reply<CR>
        */
        if ((read_size != GET_NODEID_REPLY_LEN) ||
            (memcmp(read_buf, get_nodeid_reply_str,
                    GET_NODEID_REPLY_LEN) != 0))
        {
          printf("Protocol error in Get nodeid reply state\n");
          return PROTOCOL_ERROR;
        }
        state= NODEID_STATE;
        break;
      case NODEID_STATE:
        /*
          Receive:
          nodeid: __nodeid<CR>
          Where __nodeid is an integer giving the nodeid of the starting process
        */
        if ((read_size <= NODEID_LEN) ||
            (memcmp(read_buf, nodeid_str, NODEID_LEN) != 0) ||
            (convert_str_to_int_fixed_size(read_buf + NODEID_LEN,
                                           read_size - NODEID_LEN,
                                           &node_number)) ||
            (node_number > MAX_NODE_ID))
        {
          printf("Protocol error in nodeid state\n");
          return PROTOCOL_ERROR;
        }
        DEBUG(printf("Nodeid = %u\n", (guint32)node_number));
        apic->node_ids[current_cluster_index]= (guint32)node_number;
        state= RESULT_OK_STATE;
        break;
      case RESULT_OK_STATE:
        /*
          Receive:
          result: Ok<CR>
        */
        if ((read_size != RESULT_OK_LEN) ||
            (memcmp(read_buf, result_ok_str, RESULT_OK_LEN) != 0))
        {
          printf("Protocol error in result ok state\n");
          return PROTOCOL_ERROR;
        }
        state= WAIT_EMPTY_RETURN_STATE;
        break;
      case WAIT_EMPTY_RETURN_STATE:
        /*
          Receive:
          <CR>
        */
        if (read_size != 0)
        {
          printf("Protocol error in wait empty state\n");
          return PROTOCOL_ERROR;
        }
        return 0;
      case RESULT_ERROR_STATE:
        break;
      default:
        break;
    }
  }
  return error;
}

static int
rec_get_config(struct ic_connection *conn,
               struct ic_api_cluster_server *apic,
               guint32 current_cluster_index)
{
  char read_buf[256];
  char *config_buf= NULL;
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  guint32 config_size= 0;
  guint32 rec_config_size= 0;
  int error;
  guint64 content_length;
  guint32 state= GET_CONFIG_REPLY_STATE;
  while (!(error= ic_rec_with_cr(conn, read_buf, &read_size,
                                 &size_curr_buf, sizeof(read_buf))))
  {
    DEBUG(ic_print_buf(read_buf, read_size));
    switch (state)
    {
      case GET_CONFIG_REPLY_STATE:
        /*
          The protocol is decoded in the order of the case statements in the switch
          statements.
 
          Receive:
          get config reply<CR>
        */
        if ((read_size != GET_CONFIG_REPLY_LEN) ||
            (memcmp(read_buf, get_config_reply_str,
                    GET_CONFIG_REPLY_LEN) != 0))
        {
          printf("Protocol error in get config reply state\n");
          return PROTOCOL_ERROR;
        }
        state= RESULT_OK_STATE;
        break;
      case RESULT_OK_STATE:
        /*
          Receive:
          result: Ok<CR>
        */
        if ((read_size != RESULT_OK_LEN) ||
            (memcmp(read_buf, result_ok_str, RESULT_OK_LEN) != 0))
        {
          printf("Protocol error in result ok state\n");
          return PROTOCOL_ERROR;
        }
        state= CONTENT_LENGTH_STATE;
        break;
      case CONTENT_LENGTH_STATE:
        /*
          Receive:
          Content-Length: __length<CR>
          Where __length is a decimal-coded number indicating length in bytes
        */
        if ((read_size <= CONTENT_LENGTH_LEN) ||
            (memcmp(read_buf, content_len_str,
                    CONTENT_LENGTH_LEN) != 0) ||
            convert_str_to_int_fixed_size(read_buf+CONTENT_LENGTH_LEN,
                                          read_size - CONTENT_LENGTH_LEN,
                                          &content_length) ||
            (content_length > MAX_CONTENT_LEN))
        {
          printf("Protocol error in content length state\n");
          return PROTOCOL_ERROR;
        }
        DEBUG(printf("Content Length: %u\n", (guint32)content_length));
        state= OCTET_STREAM_STATE;
        break;
      case OCTET_STREAM_STATE:
        /*
          Receive:
          Content-Type: ndbconfig/octet-stream<CR>
        */
        if ((read_size != OCTET_STREAM_LEN) ||
            (memcmp(read_buf, octet_stream_str,
                    OCTET_STREAM_LEN) != 0))
        {
          printf("Protocol error in octet stream state\n");
          return PROTOCOL_ERROR;
        }
        state= CONTENT_ENCODING_STATE;
        break;
      case CONTENT_ENCODING_STATE:
        /*
          Receive:
          Content-Transfer-Encoding: base64
        */
        if ((read_size != CONTENT_ENCODING_LEN) ||
            (memcmp(read_buf, content_encoding_str,
                    CONTENT_ENCODING_LEN) != 0))
        {
          printf("Protocol error in content encoding state\n");
          return PROTOCOL_ERROR;
        }
        /*
          Here we need to allocate receive buffer for configuration plus the
          place to put the encoded binary data.
        */
        if (!(config_buf= g_try_malloc0(content_length)))
          return MEM_ALLOC_ERROR;
        config_size= 0;
        rec_config_size= 0;
        state= WAIT_EMPTY_RETURN_STATE;
        break;
      case WAIT_EMPTY_RETURN_STATE:
        /*
          Receive:
          <CR>
        */
        if (read_size != 0)
        {
          printf("Protocol error in wait empty return state\n");
          return PROTOCOL_ERROR;
        }
        state= RECEIVE_CONFIG_STATE;
        break;
      case RECEIVE_CONFIG_STATE:
        /*
          At this point we should now start receiving the configuration in
          base64 encoded format. It will arrive in 76 character chunks
          followed by a carriage return.
        */
        g_assert(config_buf);
        memcpy(config_buf+config_size, read_buf, read_size);
        config_size+= read_size;
        rec_config_size+= (read_size + 1);
        if (rec_config_size >= content_length)
        {
          /*
            This is the last line, we have now received the config
            and are ready to translate it.
          */
          DEBUG(printf("Start decoding\n"));
          error= translate_config(apic, current_cluster_index,
                                  config_buf, config_size);
          g_free(config_buf);
          return error;
        }
        else if (read_size != CONFIG_LINE_LEN)
        {
          g_free(config_buf);
          printf("Protocol error in config receive state\n");
          return PROTOCOL_ERROR;
        }
        break;
      case RESULT_ERROR_STATE:
        break;
      default:
        break;
    }
  }
  return error;
}

static int
get_cs_config(struct ic_api_cluster_server *apic)
{
  guint32 i;
  int error= END_OF_FILE;
  struct ic_connection conn_obj, *conn;

  DEBUG(printf("Get config start\n"));
  conn= &conn_obj;

  for (i= 0; i < apic->cluster_conn.num_cluster_servers; i++)
  {
    conn= apic->cluster_conn.cluster_srv_conns + i;
    if (!(error= set_up_cluster_server_connection(conn,
                  apic->cluster_conn.cluster_server_ips[i],
                  apic->cluster_conn.cluster_server_ports[i])))
      break;
  }
  if (error)
    return error;
  for (i= 0; i < apic->num_clusters_to_connect; i++)
  {
    if ((error= send_get_nodeid(conn, apic, i)) ||
        (error= rec_get_nodeid(conn, apic, i)) ||
        (error= send_get_config(conn)) ||
        (error= rec_get_config(conn, apic, i)))
      continue;
  }
  conn->conn_op.close_ic_connection(conn);
  conn->conn_op.free_ic_connection(conn);
  return error;
}

static void
free_cs_config(struct ic_api_cluster_server *apic)
{
  guint32 i, num_clusters;
  if (apic)
  {
    if (apic->cluster_conn.cluster_srv_conns)
      g_free(apic->cluster_conn.cluster_srv_conns);
    if (apic->cluster_conn.cluster_srv_conns)
      g_free(apic->cluster_conn.cluster_server_ips);
    if (apic->cluster_conn.cluster_server_ports)
      g_free(apic->cluster_conn.cluster_server_ports);
    if (apic->cluster_ids)
      g_free(apic->cluster_ids);
    if (apic->node_ids)
      g_free(apic->node_ids);
    if (apic->conf_objects)
    {
      num_clusters= apic->num_clusters_to_connect;
      for (i= 0; i < num_clusters; i++)
      {
        struct ic_cluster_config *conf_obj= apic->conf_objects+i;
        if (conf_obj->node_ids)
          g_free(conf_obj->node_ids);
        if (conf_obj->node_types)
          g_free(conf_obj->node_types);
        if (conf_obj->comm_types)
          g_free(conf_obj->comm_types);
        if (conf_obj->node_config)
          g_free(conf_obj->node_config);
        if (conf_obj->string_memory_to_return)
          g_free(conf_obj->string_memory_to_return);
        if (conf_obj->config_memory_to_return)
          g_free(conf_obj->config_memory_to_return);
        if (conf_obj->comm_config)
          g_free(conf_obj->comm_config);
      }
      g_free(apic->conf_objects);
    }
    g_free(apic);
  }
  return;
}

struct ic_api_cluster_server*
ic_init_api_cluster(struct ic_api_cluster_connection *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 *node_ids,
                    guint32 num_clusters)
{
  struct ic_api_cluster_server *apic= NULL;
  guint32 num_cluster_servers= cluster_conn->num_cluster_servers;
  /*
    The idea with this method is that the user can set-up his desired usage
    of the clusters using stack variables. Then we copy those variables to
    heap storage and maintain this data hereafter.
    Thus we can in many cases relieve the user from the burden of error
    handling of memory allocation failures.

    We will also ensure that the supplied data is validated.
  */
  ic_init_config_parameters();
  if (!(apic= g_try_malloc0(sizeof(struct ic_api_cluster_server))) ||
      !(apic->cluster_ids= g_try_malloc0(sizeof(guint32) * num_clusters)) ||
      !(apic->node_ids= g_try_malloc0(sizeof(guint32) * num_clusters)) ||
      !(apic->cluster_conn.cluster_server_ips= 
         g_try_malloc0(num_cluster_servers *
                       sizeof(guint32))) ||
      !(apic->cluster_conn.cluster_server_ports=
         g_try_malloc0(num_cluster_servers *
                       sizeof(guint16))) ||
      !(apic->conf_objects= g_try_malloc0(num_cluster_servers *
                                sizeof(struct ic_cluster_config))) ||
      !(apic->cluster_conn.cluster_srv_conns=
         g_try_malloc0(num_cluster_servers *
                       sizeof(struct ic_connection))))
  {
    free_cs_config(apic);
    return NULL;
  }

  memcpy((char*)apic->cluster_ids,
         (char*)cluster_ids,
         num_clusters * sizeof(guint32));

  memcpy((char*)apic->node_ids,
         (char*)node_ids,
         num_clusters * sizeof(guint32));

  memcpy((char*)apic->cluster_conn.cluster_server_ips,
         (char*)cluster_conn->cluster_server_ips,
         num_cluster_servers * sizeof(guint32));
  memcpy((char*)apic->cluster_conn.cluster_server_ports,
         (char*)cluster_conn->cluster_server_ports,
         num_cluster_servers * sizeof(guint16));
  memset((char*)apic->conf_objects, 0,
         num_cluster_servers * sizeof(struct ic_cluster_config));
  memset((char*)apic->cluster_conn.cluster_srv_conns, 0,
         num_cluster_servers * sizeof(struct ic_connection));

  apic->cluster_conn.num_cluster_servers= num_cluster_servers;
  apic->api_op.get_ic_config= get_cs_config;
  apic->api_op.free_ic_config= free_cs_config;
  return apic;
}

static void
free_run_cluster(struct ic_run_cluster_server *run_obj)
{
  return;
}

static int
run_cluster_server(struct ic_cluster_config *cs_conf,
                   struct ic_run_cluster_server *run_obj,
                   guint32 num_connects)
{
  return 0;
}

struct ic_run_cluster_server*
ic_init_run_cluster(struct ic_cluster_config *conf_objs,
                    guint32 *cluster_ids,
                    guint32 num_clusters)
{
  struct ic_run_cluster_server *run_obj;
  ic_init_config_parameters();
  if (!(run_obj= g_try_malloc0(sizeof(struct ic_api_cluster_server))))
    return NULL;
  return run_obj;
}

