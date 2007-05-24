#include <ic_apic.h>
#include <hashtable.h>
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
     ic_kernel_node_config (ic_apic.h). The name of this
     variable must be the same name as provided in the
     definition of the config variable in the method
     ic_init_config_parameters.

  ---------------------------------------------------------
  ---------------------------------------------------------

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

const gchar *data_server_str= "data server";
const gchar *client_node_str= "client";
const gchar *cluster_server_str= "cluster server";
const gchar *rep_server_str= "replication server";
const gchar *sql_server_str= "sql server";
const gchar *socket_str= "socket";
const gchar *data_server_def_str= "data server default";
const gchar *client_node_def_str= "client default";
const gchar *cluster_server_def_str= "cluster server default";
const gchar *rep_server_def_str= "replication server default";
const gchar *sql_server_def_str= "sql server default";
const gchar *socket_def_str= "socket default";


#define MIN_PORT 1024
#define MAX_PORT 65535
#define DEF_MGM_PORT 2286

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

static guint32 glob_conf_max_id;
struct ic_hashtable *glob_conf_hash;
static gboolean glob_conf_entry_inited= FALSE;
static guint16 map_config_id[MAX_MAP_CONFIG_ID];
static IC_CONFIG_ENTRY glob_conf_entry[MAX_CONFIG_ID];

static const gchar *empty_string= "";
static const gchar *get_nodeid_str= "get nodeid";
static const gchar *get_nodeid_reply_str= "get nodeid reply";
static const gchar *get_config_str= "get config";
static const gchar *get_config_reply_str= "get config reply";
static const gchar *nodeid_str= "nodeid: ";
static const gchar *version_str= "version: ";
static const gchar *nodetype_str= "nodetype: 1";
static const gchar *user_str= "user: mysqld";
static const gchar *password_str= "password: mysqld";
static const gchar *public_key_str= "public key: a public key";
static const gchar *endian_str= "endian: ";
static const gchar *log_event_str= "log_event: 0";
static const gchar *result_ok_str= "result: Ok";
static const gchar *content_len_str= "Content-Length: ";
static const gchar *octet_stream_str= "Content-Type: ndbconfig/octet-stream";
static const gchar *content_encoding_str= "Content-Transfer-Encoding: base64";
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

#define SOCKET_FIRST_NODE_ID 400
#define SOCKET_SECOND_NODE_ID 401
#define SOCKET_USE_MESSAGE_ID 402
#define SOCKET_USE_CHECKSUM 403
#define SOCKET_CLIENT_PORT_NUMBER 420
#define SOCKET_SERVER_PORT_NUMBER 406
#define SOCKET_SERVER_NODE_ID 410
#define SOCKET_WRITE_BUFFER_SIZE 454
#define SOCKET_READ_BUFFER_SIZE 455
#define SOCKET_FIRST_HOSTNAME 407
#define SOCKET_SECOND_HOSTNAME 408
#define SOCKET_GROUP 409

#define CLIENT_MAX_BATCH_BYTE_SIZE 800
#define CLIENT_BATCH_BYTE_SIZE 801
#define CLIENT_BATCH_SIZE 802

#define IC_PORT_NUMBER 997
#define DEF_PORT 2287

void
ic_print_config_parameters(guint32 mask)
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 inx, i;
  if (!glob_conf_entry_inited)
    return;
  for (i= 0; i < 1024; i++)
  {
    if ((inx= map_config_id[i]))
    {
      conf_entry= &glob_conf_entry[inx];
      if (!(conf_entry->node_types & mask))
        continue;
      printf("\n");
      if (conf_entry->is_deprecated)
      {
        printf("Entry %u is deprecated\n", i);
        continue;
      }
      printf("Entry %u:\n", i);
      printf("Name: %s\n", conf_entry->config_entry_name.str);
      printf("Comment: %s\n", conf_entry->config_entry_description);
      switch (conf_entry->data_type)
      {
        case IC_CHARPTR:
          printf("Data type is string\n");
          break;
        case IC_UINT16:
          printf("Data type is 16-bit unsigned integer\n");
          break;
        case IC_UINT32:
          printf("Data type is 32-bit unsigned integer\n");
          break;
        case IC_UINT64:
          printf("Data type is 64-bit unsigned integer\n");
          break;
        case IC_BOOLEAN:
          printf("Data type is boolean\n");
          break;
        default:
          printf("Data type set to non-existent type!!!\n");
          break;
      }
      if (conf_entry->is_not_configurable)
      {
        printf("Entry is not configurable with value %u\n",
               (guint32)conf_entry->default_value);
        continue;
      }
      printf("Offset of variable is %u\n", conf_entry->offset);
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
      if (conf_entry->node_types & (1 << IC_CLIENT_TYPE))
        printf("This config variable is used in a client node\n");
      if (conf_entry->node_types & (1 << IC_KERNEL_TYPE))
        printf("This config variable is used in a kernel node\n");
      if (conf_entry->node_types & (1 << IC_CLUSTER_SERVER_TYPE))
        printf("This config variable is used in a cluster server\n");
      if (conf_entry->node_types & (1 << IC_COMM_TYPE))
        printf("This config variable is used in connections\n");

      if (conf_entry->is_mandatory_to_specify)
        printf("Entry is mandatory and has no default value\n");
      else if (conf_entry->is_string_type)
      {
        if (!conf_entry->is_mandatory_to_specify)
        {
          printf("Entry has default value: %s\n",
                 conf_entry->default_string);
        }
        continue;
      }
      else
        printf("Default value is %u\n", (guint32)conf_entry->default_value);
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

static IC_CONFIG_ENTRY *get_config_entry(int config_id);

static int
build_config_name_hash()
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 i;
  for (i= 0; i <= glob_conf_max_id; i++)
  {
    conf_entry= &glob_conf_entry[i];
    if (conf_entry->config_entry_name.str != NULL)
    {
      if (ic_hashtable_insert(glob_conf_hash,
                              (void*)&conf_entry->config_entry_name,
                              (void*)conf_entry))
        return 1;
    }
  }
  return 0;
}

static void
init_config_parameters()
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 mandatory_bits= 0;
/*
  This is the kernel node configuration section.
*/
  IC_SET_CONFIG_MAP(KERNEL_MAX_TRACE_FILES, 1);
  IC_SET_KERNEL_CONFIG(conf_entry, max_number_of_trace_files,
                       IC_UINT32, 25, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 2048);
  conf_entry->config_entry_description=
  "The number of crashes that can be reported before we overwrite error log and trace files";

  IC_SET_CONFIG_MAP(KERNEL_REPLICAS, 2);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_replicas,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 4);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "This defines number of nodes per node group, within a node group all nodes contain the same data";

  IC_SET_CONFIG_MAP(KERNEL_TABLE_OBJECTS, 3);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_table_objects,
                       IC_UINT32, 256, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of tables that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_COLUMN_OBJECTS, 4);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_column_objects,
                       IC_UINT32, 2048, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 256);
  conf_entry->config_entry_description=
  "Sets the maximum number of columns that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_KEY_OBJECTS, 5);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_key_objects,
                       IC_UINT32, 256, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_INTERNAL_TRIGGER_OBJECTS, 6);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_internal_trigger_objects,
                       IC_UINT32, 1536, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 512);
  conf_entry->config_entry_description=
  "Each unique index will use 3 internal trigger objects, index/backup will use 1 per table";

  IC_SET_CONFIG_MAP(KERNEL_CONNECTION_OBJECTS, 7);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_connection_objects,
                       IC_UINT32, 8192, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128);
  conf_entry->config_entry_description=
  "Each active transaction and active scan uses a connection object";

  IC_SET_CONFIG_MAP(KERNEL_OPERATION_OBJECTS, 8);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_operation_objects,
                       IC_UINT32, 32768, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024);
  conf_entry->config_entry_description=
  "Each record read/updated in a transaction uses an operation object during the transaction";

  IC_SET_CONFIG_MAP(KERNEL_SCAN_OBJECTS, 9);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_scan_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 32, 512);
  conf_entry->config_entry_description=
  "Each active scan uses a scan object for the lifetime of the scan operation";

  IC_SET_CONFIG_MAP(KERNEL_INTERNAL_TRIGGER_OPERATION_OBJECTS, 10);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_internal_trigger_operation_objects,
                       IC_UINT32, 4000, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4000, 4000);
  conf_entry->config_entry_description=
  "Each internal trigger that is fired uses an operation object for a short time";

  IC_SET_CONFIG_MAP(KERNEL_KEY_OPERATION_OBJECTS, 11);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_key_operation_objects,
                       IC_UINT32, 4096, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128);
  conf_entry->config_entry_description=
  "Each read and update of an unique hash index in a transaction uses one of those objects";

  IC_SET_CONFIG_MAP(KERNEL_CONNECTION_BUFFER, 12);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_connection_buffer,
                       IC_UINT32, 1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1024*1024, 1024*1024);
  conf_entry->config_entry_description=
  "Internal buffer used by connections by transactions and scans";

  IC_SET_CONFIG_MAP(KERNEL_RAM_MEMORY, 13);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_ram_memory,
                       IC_UINT64, 256*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 16*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory used to store RAM-based records";

  IC_SET_CONFIG_MAP(KERNEL_HASH_MEMORY, 14);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_hash_memory,
                       IC_UINT64, 64*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 8*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory used to store primary hash index on all tables and unique hash indexes";

  IC_SET_CONFIG_MAP(KERNEL_LOCK_MEMORY, 15);
  IC_SET_KERNEL_BOOLEAN(conf_entry, use_unswappable_memory, FALSE,
                        IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Setting this to 1 means that all memory is locked and will not be swapped out";

  IC_SET_CONFIG_MAP(KERNEL_WAIT_PARTIAL_START, 16);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_wait_partial_start,
                       IC_UINT32, 20000, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting with a partial set of nodes, 0 waits forever";

  IC_SET_CONFIG_MAP(KERNEL_WAIT_PARTITIONED_START, 17);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_wait_partitioned_start,
                       IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting a potentially partitioned cluster, 0 waits forever";

  IC_SET_CONFIG_MAP(KERNEL_WAIT_ERROR_START, 18);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_wait_error_start,
                       IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before forcing a stop after an error, 0 waits forever";

  IC_SET_CONFIG_MAP(KERNEL_HEARTBEAT_TIMER, 19);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_heartbeat_kernel_nodes,
                       IC_UINT32, 700, IC_ROLLING_UPGRADE_CHANGE_SPECIAL);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to kernel nodes, 4 missed leads to node crash";

  IC_SET_CONFIG_MAP(KERNEL_CLIENT_HEARTBEAT_TIMER, 20);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_heartbeat_client_nodes,
                       IC_UINT32, 1000, IC_ROLLING_UPGRADE_CHANGE_SPECIAL);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to client nodes, 4 missed leads to node crash";

  IC_SET_CONFIG_MAP(KERNEL_LOCAL_CHECKPOINT_TIMER, 21);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_local_checkpoint,
                       IC_UINT32, 24, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 6, 31);
  conf_entry->config_entry_description=
  "Specifies how often local checkpoints are executed, logarithmic scale on log size";

  IC_SET_CONFIG_MAP(KERNEL_GLOBAL_CHECKPOINT_TIMER, 22);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_global_checkpoint,
                       IC_UINT32, 1000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between starting global checkpoints";

  IC_SET_CONFIG_MAP(KERNEL_RESOLVE_TIMER, 23);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_resolve,
                       IC_UINT32, 2000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms waiting for response from resolve";

  IC_SET_CONFIG_MAP(KERNEL_WATCHDOG_TIMER, 24);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_kernel_watchdog,
                       IC_UINT32, 6000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms without activity in kernel before watchdog is fired";

  IC_SET_CONFIG_MAP(KERNEL_DAEMON_RESTART_AT_ERROR, 25);
  IC_SET_KERNEL_BOOLEAN(conf_entry, kernel_automatic_restart, TRUE,
                        IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "If set, kernel restarts automatically after a failure";

  IC_SET_CONFIG_MAP(KERNEL_FILESYSTEM_PATH, 26);
  IC_SET_KERNEL_STRING(conf_entry, filesystem_path, IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Path to filesystem of kernel";

  IC_SET_CONFIG_MAP(KERNEL_REDO_LOG_FILES, 27);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_redo_log_files,
                       IC_UINT32, 32, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN(conf_entry, 4);
  conf_entry->config_entry_description=
  "Number of REDO log files, each file represents 64 MB log space";

  IC_SET_CONFIG_MAP(KERNEL_CHECK_INTERVAL, 28);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_check_interval,
                       IC_UINT32, 500, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 500, 500);
  conf_entry->config_entry_description=
  "Time in ms between checks after transaction timeouts";

  IC_SET_CONFIG_MAP(KERNEL_CLIENT_ACTIVITY_TIMER, 29);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_client_activity,
                       IC_UINT32, 1024*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to client inactivity";

  IC_SET_CONFIG_MAP(KERNEL_DEADLOCK_TIMER, 30);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_deadlock,
                       IC_UINT32, 2000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to internal wait (indication of deadlock)";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_OBJECTS, 31);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_checkpoint_objects,
                       IC_UINT32, 1, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 1);
  conf_entry->config_entry_description=
  "Number of possible parallel backups and local checkpoints";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_MEMORY, 32);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_memory,
                       IC_UINT32, 4*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4*1024*1024, 4*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_DATA_MEMORY, 33);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_data_memory,
                       IC_UINT32, 2*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 2*1024*1024, 2*1024*1024);
  conf_entry->config_entry_description=
  "Size of data memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_LOG_MEMORY, 34);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_log_memory,
                       IC_UINT32, 2*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 2*1024*1024, 2*1024*1024);
  conf_entry->config_entry_description=
  "Size of log memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_WRITE_SIZE, 35);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_write_size,
                       IC_UINT32, 64*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 64*1024);
  conf_entry->config_entry_description=
  "Size of default writes in local checkpoint and backups";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_MAX_WRITE_SIZE, 36);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_max_write_size,
                       IC_UINT32, 256*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 256*1024, 256*1024);
  conf_entry->config_entry_description=
  "Size of maximum writes in local checkpoint and backups";

  IC_SET_CONFIG_MAP(KERNEL_VOLATILE_MODE, 37);
  IC_SET_KERNEL_BOOLEAN(conf_entry, kernel_volatile_mode, FALSE,
                        IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "In this mode all file writes are ignored and all starts becomes initial starts";


  IC_SET_CONFIG_MAP(KERNEL_ORDERED_KEY_OBJECTS, 38);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_ordered_key_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of ordered keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_UNIQUE_HASH_KEY_OBJECTS, 39);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_unique_hash_key_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of unique hash keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_LOCAL_OPERATION_OBJECTS, 40);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_local_operation_objects,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Number of local operation records stored used in the node";

  IC_SET_CONFIG_MAP(KERNEL_LOCAL_SCAN_OBJECTS, 41);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_local_scan_objects,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Number of local scan records stored used in the node";

  IC_SET_CONFIG_MAP(KERNEL_SCAN_BATCH_SIZE, 42);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_scan_batch,
                       IC_UINT32, 64, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64, 64);
  conf_entry->config_entry_description=
  "Number of records sent in a scan from the local kernel node";

  IC_SET_CONFIG_MAP(KERNEL_REDO_LOG_MEMORY, 43);
  IC_SET_KERNEL_CONFIG(conf_entry, redo_log_memory,
                       IC_UINT32, 16*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Size of REDO log memory buffer";

  IC_SET_CONFIG_MAP(KERNEL_LONG_MESSAGE_MEMORY, 44);
  IC_SET_KERNEL_CONFIG(conf_entry, long_message_memory,
                       IC_UINT32, 1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1024*1024, 1024*1024);
  conf_entry->config_entry_description=
  "Size of long memory buffers";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_PATH, 45);
  IC_SET_KERNEL_STRING(conf_entry, kernel_checkpoint_path, IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Path to filesystem of checkpoints";

  IC_SET_CONFIG_MAP(KERNEL_MAX_OPEN_FILES, 46);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_max_open_files,
                       IC_UINT32, 40, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 40, 40);
  conf_entry->config_entry_description=
  "Maximum number of open files in kernel node";

  IC_SET_CONFIG_MAP(KERNEL_PAGE_CACHE_SIZE, 47);
  IC_SET_KERNEL_CONFIG(conf_entry, page_cache_size,
                       IC_UINT64, 128*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 64*1024);
  conf_entry->config_entry_description=
  "Size of page cache for disk-based data";

  IC_SET_CONFIG_MAP(KERNEL_STRING_MEMORY, 48);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_string_memory,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Size of string memory";

  IC_SET_CONFIG_MAP(KERNEL_INITIAL_OPEN_FILES, 49);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_open_files,
                       IC_UINT32, 27, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 27, 27);
  conf_entry->config_entry_description=
  "Number of open file handles in kernel node from start";

  IC_SET_CONFIG_MAP(KERNEL_FILE_SYNCH_SIZE, 50);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_file_synch_size,
                       IC_UINT32, 4*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Size of file writes before a synch is always used";

  IC_SET_CONFIG_MAP(KERNEL_DISK_WRITE_SPEED, 51);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_disk_write_speed,
                       IC_UINT32, 8*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 64*1024);
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk";

  IC_SET_CONFIG_MAP(KERNEL_DISK_WRITE_SPEED_START, 52);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_disk_write_speed_start,
                       IC_UINT32, 256*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk during start of the node";

  IC_SET_CONFIG_MAP(KERNEL_MEMORY_POOL, 53);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_memory_pool,
                       IC_UINT64, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Size of memory pool for internal memory usage";

  IC_SET_CONFIG_MAP(KERNEL_DUMMY, 54);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_dummy,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description= (gchar*)empty_string;

  IC_SET_CONFIG_MAP(KERNEL_START_LOG_LEVEL, 55);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_start,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at start of a node";

  IC_SET_CONFIG_MAP(KERNEL_STOP_LOG_LEVEL, 56);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_stop,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at stop of a node";

  IC_SET_CONFIG_MAP(KERNEL_STAT_LOG_LEVEL, 57);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_statistics,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of statistics on a node";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_LOG_LEVEL, 58);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_checkpoint,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at checkpoint of a node";

  IC_SET_CONFIG_MAP(KERNEL_RESTART_LOG_LEVEL, 59);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_restart,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at restart of a node";

  IC_SET_CONFIG_MAP(KERNEL_CONNECTION_LOG_LEVEL, 60);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_connection,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of connections to a node";

  IC_SET_CONFIG_MAP(KERNEL_REPORT_LOG_LEVEL, 61);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_reports,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of reports from a node";

  IC_SET_CONFIG_MAP(KERNEL_WARNING_LOG_LEVEL, 62);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_warning,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of warnings from a node";

  IC_SET_CONFIG_MAP(KERNEL_ERROR_LOG_LEVEL, 63);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_error,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of errors from a node";

  IC_SET_CONFIG_MAP(KERNEL_CONGESTION_LOG_LEVEL, 64);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_congestion,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of congestions to a node";

  IC_SET_CONFIG_MAP(KERNEL_DEBUG_LOG_LEVEL, 65);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_debug,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of debug messages from a node";

  IC_SET_CONFIG_MAP(KERNEL_BACKUP_LOG_LEVEL, 66);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_backup,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of backups at a node";

  IC_SET_CONFIG_MAP(127, 67);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(128, 68);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(137, 69);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(138, 70);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(154, 71);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(155, 72);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(KERNEL_INJECT_FAULT, 73);
  IC_SET_KERNEL_CONFIG(conf_entry, inject_fault,
                       IC_UINT32, 2, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 2);
  conf_entry->config_entry_description=
  "Inject faults (only available in special test builds)";

  IC_SET_CONFIG_MAP(KERNEL_SCHEDULER_NO_SEND_TIME, 74);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_scheduler_no_send_time,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_version_used= 0x50111;
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without sending socket buffers";

  IC_SET_CONFIG_MAP(KERNEL_SCHEDULER_NO_SLEEP_TIME, 75);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_scheduler_no_sleep_time,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_version_used= 0x50111;
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without going to sleep";

  IC_SET_CONFIG_MAP(KERNEL_RT_SCHEDULER_THREADS, 76);
  IC_SET_KERNEL_BOOLEAN(conf_entry, kernel_rt_scheduler_threads,
                       FALSE, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_version_used= 0x50111;
  conf_entry->config_entry_description=
  "If set the kernel is setting its thread in RT priority, requires root privileges";

  IC_SET_CONFIG_MAP(KERNEL_LOCK_MAIN_THREAD, 77);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_lock_main_thread,
                       IC_UINT32, 65535, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 65535);
  conf_entry->min_version_used= 0x50111;
  conf_entry->config_entry_description=
  "Lock Main Thread to a CPU id";

  IC_SET_CONFIG_MAP(KERNEL_LOCK_OTHER_THREADS, 78);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_lock_main_thread,
                       IC_UINT32, 65535, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 65535);
  conf_entry->min_version_used= 0x50111;
  conf_entry->config_entry_description=
  "Lock other threads to a CPU id";

/*
  This is the Socket configuration section.
*/
  IC_SET_CONFIG_MAP(SOCKET_FIRST_NODE_ID, 100);
  IC_SET_SOCKET_CONFIG(conf_entry, first_node_id,
                    IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "First node id of the connection";

  IC_SET_CONFIG_MAP(SOCKET_SECOND_NODE_ID, 101);
  IC_SET_SOCKET_CONFIG(conf_entry, second_node_id,
                    IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Second node id of the connection";

  IC_SET_CONFIG_MAP(SOCKET_USE_MESSAGE_ID, 102);
  IC_SET_SOCKET_BOOLEAN(conf_entry, use_message_id,
                     FALSE, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Using message id can be a valuable resource to find problems related to distributed execution";

  IC_SET_CONFIG_MAP(SOCKET_USE_CHECKSUM, 103);
  IC_SET_SOCKET_BOOLEAN(conf_entry, use_checksum,
                     FALSE, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Using checksum ensures that internal bugs doesn't corrupt data while data is placed in buffers";

  IC_SET_CONFIG_MAP(SOCKET_CLIENT_PORT_NUMBER, 104);
  IC_SET_SOCKET_CONFIG(conf_entry, client_port_number,
                    IC_UINT16, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->is_only_iclaustron= TRUE;
  conf_entry->config_entry_description=
  "Port number to use on client side";

  IC_SET_CONFIG_MAP(SOCKET_SERVER_PORT_NUMBER, 105);
  IC_SET_SOCKET_CONFIG(conf_entry, server_port_number,
                    IC_UINT16, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Port number to use on server side";

  IC_SET_CONFIG_MAP(SOCKET_WRITE_BUFFER_SIZE, 106);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_write_buffer_size,
                    IC_UINT32, 256*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128*1024);
  conf_entry->config_entry_description=
  "Size of write buffer in front of socket";

  IC_SET_CONFIG_MAP(SOCKET_READ_BUFFER_SIZE, 107);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_read_buffer_size,
                    IC_UINT32, 256*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 64*1024);
  conf_entry->config_entry_description=
  "Size of read buffer in front of socket";

  IC_SET_CONFIG_MAP(SOCKET_FIRST_HOSTNAME, 108);
  IC_SET_SOCKET_STRING(conf_entry, first_hostname, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Hostname of first node";

  IC_SET_CONFIG_MAP(SOCKET_SECOND_HOSTNAME, 109);
  IC_SET_SOCKET_STRING(conf_entry, second_hostname, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Hostname of second node";

  IC_SET_CONFIG_MAP(SOCKET_GROUP, 110);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_group,
                    IC_UINT32, 55, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 55, 55);
  conf_entry->config_entry_description=
  "Group id of the connection";

  IC_SET_CONFIG_MAP(SOCKET_SERVER_NODE_ID, 111);
  IC_SET_SOCKET_CONFIG(conf_entry, server_node_id,
                    IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Node id of node that is server part of connection";

/*
  This is the cluster server configuration section.
*/
  IC_SET_CONFIG_MAP(CLUSTER_SERVER_EVENT_LOG, 150);
  IC_SET_CLUSTER_SERVER_STRING(conf_entry, cluster_server_event_log,
                               empty_string, IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Type of cluster event log";
/*
  This is the client configuration section.
*/

  IC_SET_CONFIG_MAP(CLIENT_RESOLVE_RANK, 200);
  IC_SET_CLIENT_CONFIG(conf_entry, client_resolve_rank,
                       IC_UINT32, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 2);
  conf_entry->node_types= (1 << IC_CLIENT_TYPE) + (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Rank in resolving network partition of the client";

  IC_SET_CONFIG_MAP(CLIENT_RESOLVE_TIMER, 201);
  IC_SET_CLIENT_CONFIG(conf_entry, client_resolve_timer,
                       IC_UINT32, 0, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->node_types= (1 << IC_CLIENT_TYPE) + (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Time in ms waiting for resolve before crashing";

  IC_SET_CONFIG_MAP(CLIENT_BATCH_SIZE, 202);
  IC_SET_CLIENT_CONFIG(conf_entry, client_batch_size,
                       IC_UINT32, 64, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 992);
  conf_entry->config_entry_description=
  "Size in number of records of batches in scan operations";

  IC_SET_CONFIG_MAP(CLIENT_BATCH_BYTE_SIZE, 203);
  IC_SET_CLIENT_CONFIG(conf_entry, client_batch_byte_size,
                       IC_UINT32, 8192, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 128, 65536);
  conf_entry->config_entry_description=
  "Size in bytes of batches in scan operations";

  IC_SET_CONFIG_MAP(CLIENT_MAX_BATCH_BYTE_SIZE, 204);
  IC_SET_CLIENT_CONFIG(conf_entry, client_max_batch_byte_size,
                       IC_UINT32, 256*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 32*1024, 4*1024*1024);
  conf_entry->config_entry_description=
  "Size in bytes of max of the sum of the batches in a scan operations";

  /* Parameters common for all node types */
  IC_SET_CONFIG_MAP(IC_NODE_DATA_PATH, 250);
  IC_SET_KERNEL_STRING(conf_entry, node_data_path, IC_INITIAL_NODE_RESTART);
  conf_entry->default_string= (gchar*)empty_string;
  conf_entry->node_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                         (1 << IC_CLIENT_TYPE) +
                         (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Data directory of the node";

  IC_SET_CONFIG_MAP(IC_NODE_HOST, 251);
  IC_SET_KERNEL_STRING(conf_entry, hostname, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->default_string= (gchar*)empty_string;
  conf_entry->node_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                         (1 << IC_CLIENT_TYPE) +
                         (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Hostname of the node";

  /*
    These two parameters require special treatment, they are not sent as
    normal parameters, node id is the index used and sent in special
    parameter and port numbers are sent as part of TCP section. This makes
    it harder to put it back from protocol into data structure format.
  */
  IC_SET_CONFIG_MAP(IC_NODE_ID, 252);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  IC_SET_KERNEL_CONFIG(conf_entry, node_id, IC_UINT32,
                       0, IC_NOT_CHANGEABLE);
  conf_entry->is_mandatory_to_specify= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->node_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                          (1 << IC_CLIENT_TYPE) +
                          (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Node id";

  IC_SET_CONFIG_MAP(IC_PORT_NUMBER, 253);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  IC_SET_KERNEL_CONFIG(conf_entry, port_number, IC_UINT32,
                       DEF_PORT, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->node_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                          (1 << IC_CLIENT_TYPE) +
                          (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Port number";
}

static int
ic_init_config_parameters()
{
  DEBUG_ENTRY("ic_init_config_parameters");

  if (glob_conf_entry_inited)
    return 0;
  if (!(glob_conf_hash= ic_create_hashtable(256, ic_hash_str,
                                            ic_keys_equal_str)))
    return 1;
  glob_conf_entry_inited= TRUE;
  glob_conf_max_id= 0;
  memset(map_config_id, 0, 1024 * sizeof(guint16));
  memset(glob_conf_entry, 0, 256 * sizeof(IC_CONFIG_ENTRY));
  init_config_parameters();
  return build_config_name_hash();
}

static IC_CONFIG_ENTRY *get_config_entry(int config_id)
{
  guint32 inx;

  if (config_id >= IC_NODE_TYPE)
  {
    printf("config_id larger than 998 = %d\n", config_id);
    return NULL;
  }
  inx= map_config_id[config_id];
  if (!inx)
  {
    return NULL;
  }
  return &glob_conf_entry[inx];
}

static int
ic_assign_config_value(int config_id, guint64 value,
                      enum ic_config_type conf_type,
                      int data_type,
                      gchar *struct_ptr)
{
  IC_CONFIG_ENTRY *conf_entry= get_config_entry(config_id);

  if (!conf_entry)
  {
    printf("Didn't find config entry for config_id = %d\n", config_id);
    return PROTOCOL_ERROR;
  }
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0;
  if (!(conf_entry->node_types & (1 << conf_type)) ||
      (conf_entry->is_boolean && (value > 1)) ||
      (conf_entry->is_min_value_defined &&
       (conf_entry->min_value > value)) ||
      (conf_entry->is_max_value_defined &&
       (conf_entry->max_value < value)))
  {
    printf("Config value error config_id = %d\n", config_id);
    return PROTOCOL_ERROR;
  }
  struct_ptr+= conf_entry->offset;
  if (data_type == IC_CL_INT32_TYPE)
  {
    if (conf_entry->data_type == IC_CHAR)
    {
      *(gchar*)struct_ptr= (gchar)value;
    }
    else if (conf_entry->data_type == IC_UINT16)
    {
      *(guint16*)struct_ptr= (guint16)value;
    }
    else if (conf_entry->data_type == IC_UINT32)
    {
      *(guint32*)struct_ptr= (guint32)value;
    }
    else
      return PROTOCOL_ERROR;
    return 0;
  }
  else if (data_type == IC_CL_INT64_TYPE)
  {
    if (conf_entry->data_type == IC_UINT64)
    {
      *(guint64*)struct_ptr= (guint64)value;
    }
    else
      return PROTOCOL_ERROR;
    return 0;
  }
  g_assert(FALSE);
  return 0;
}

static int
ic_assign_config_string(int config_id, IC_CONFIG_TYPE conf_type,
                        gchar *string_memory,
                        guint32 string_len,
                        gchar *struct_ptr,
                        gchar **string_val)
{
  IC_CONFIG_ENTRY *conf_entry= get_config_entry(config_id);

  if (!conf_entry)
    return PROTOCOL_ERROR;
  if (!conf_entry->is_string_type ||
      !(conf_entry->node_types & (1 << conf_type)))
  {
    DEBUG(printf("conf_type = %u, node_type = %u, config_id = %u\n",
           conf_type, conf_entry->node_types, config_id));
    DEBUG(printf("Debug string inconsistency\n"));
    return PROTOCOL_ERROR;
  }
  struct_ptr+= conf_entry->offset;
  *(gchar**)struct_ptr= string_memory;
  strncpy(string_memory, *string_val, string_len);
  return 0;
}

static void
init_config_default_value(gchar *var_ptr, IC_CONFIG_ENTRY *conf_entry)
{
  switch (conf_entry->data_type)
  {
    case IC_CHARPTR:
    {
      gchar **varchar_ptr= (gchar**)var_ptr;
      *varchar_ptr= conf_entry->default_string;
      break;
    }
    case IC_UINT16:
    {
      guint16 *var16_ptr= (guint16*)var_ptr;
      *var16_ptr= (guint16)conf_entry->default_value;
      break;
    }
    case IC_UINT32:
    {
      guint32 *var32_ptr= (guint32*)var_ptr;
      *var32_ptr= (guint32)conf_entry->default_value;
      break;
    }
    case IC_UINT64:
    {
      guint64 *var64_ptr= (guint64*)var_ptr;
      *var64_ptr= (guint64)conf_entry->default_value;
      break;
    }
    case IC_BOOLEAN:
    {
      gchar *varboolean_ptr= (gchar*)var_ptr;
      *varboolean_ptr= (gchar)conf_entry->default_value;
      break;
    }
    default:
      abort();
      break;
  }
}

static void
init_config_object(gchar *conf_object, IC_CONFIG_TYPE node_type)
{
  guint32 i;
  for (i= 0; i < glob_conf_max_id; i++)
  {
    IC_CONFIG_ENTRY *conf_entry= &glob_conf_entry[i];
    if (conf_entry && conf_entry->node_types & node_type)
    {
      gchar *var_ptr= conf_object + conf_entry->offset;
      init_config_default_value(var_ptr, conf_entry);
    }
  }
}

/*
  CONFIGURATION TRANSLATE KEY DATA TO CONFIGURATION OBJECTS MODULE
  ----------------------------------------------------------------
*/

static int
allocate_mem_phase1(IC_CLUSTER_CONFIG *conf_obj)
{
  /*
    Allocate memory for pointer arrays pointing to the configurations of the
    nodes in the cluster, also allocate memory for array of node types.
  */
  conf_obj->node_types= g_try_malloc0(conf_obj->no_of_nodes *
                                         sizeof(IC_NODE_TYPES));
  conf_obj->comm_types= g_try_malloc0(conf_obj->no_of_nodes *
                                      sizeof(IC_COMMUNICATION_TYPE));
  conf_obj->node_ids= g_try_malloc0(conf_obj->no_of_nodes *
                                       sizeof(guint32));
  conf_obj->node_config= g_try_malloc0(conf_obj->no_of_nodes *
                                          sizeof(gchar*));
  if (!conf_obj->node_types || !conf_obj->node_ids ||
      !conf_obj->comm_types || !conf_obj->node_config)
    return MEM_ALLOC_ERROR;
  return 0;
}


static int
allocate_mem_phase2(IC_API_CONFIG_SERVER *apic, IC_CLUSTER_CONFIG *conf_obj)
{
  guint32 i;
  guint32 size_config_objects= 0;
  gchar *conf_obj_ptr, *string_mem;
  /*
    Allocate memory for the actual configuration objects for nodes and
    communication sections.
  */
  for (i= 0; i < conf_obj->no_of_nodes; i++)
  {
    switch (conf_obj->node_types[i])
    {
      case IC_KERNEL_TYPE:
        size_config_objects+= sizeof(IC_KERNEL_CONFIG);
        break;
      case IC_CLIENT_TYPE:
        size_config_objects+= sizeof(IC_CLIENT_CONFIG);
        break;
      case IC_CLUSTER_SERVER_TYPE:
        size_config_objects+= sizeof(IC_CLUSTER_SERVER_CONFIG);
        break;
      case IC_SQL_SERVER_TYPE:
        size_config_objects+= sizeof(IC_SQL_SERVER_CONFIG);
        break;
      case IC_REP_SERVER_TYPE:
        size_config_objects+= sizeof(IC_REP_SERVER_CONFIG);
        break;
      default:
        g_assert(FALSE);
        break;
    }
  }
  size_config_objects+= conf_obj->no_of_comms *
                        sizeof(struct ic_comm_link_config);
  if (!(conf_obj->comm_config= g_try_malloc0(
                     conf_obj->no_of_comms * sizeof(gchar*))) ||
      !(apic->string_memory_to_return= g_try_malloc0(
                     apic->string_memory_size)) ||
      !(apic->config_memory_to_return= g_try_malloc0(
                     size_config_objects)))
    return MEM_ALLOC_ERROR;

  conf_obj_ptr= apic->config_memory_to_return;
  string_mem= apic->string_memory_to_return;
  apic->end_string_memory= string_mem + apic->string_memory_size;
  apic->next_string_memory= string_mem;

  for (i= 0; i < conf_obj->no_of_nodes; i++)
  {
    conf_obj->node_config[i]= conf_obj_ptr;
    init_config_object(conf_obj_ptr, conf_obj->node_types[i]);
    switch (conf_obj->node_types[i])
    {
      case IC_KERNEL_TYPE:
        conf_obj_ptr+= sizeof(IC_KERNEL_CONFIG);
        break;
      case IC_CLIENT_TYPE:
        conf_obj_ptr+= sizeof(IC_CLIENT_CONFIG);
        break;
      case IC_CLUSTER_SERVER_TYPE:
        conf_obj_ptr+= sizeof(IC_CLUSTER_SERVER_CONFIG);
        break;
      case IC_SQL_SERVER_TYPE:
        conf_obj_ptr+= sizeof(IC_SQL_SERVER_CONFIG);
        break;
      case IC_REP_SERVER_TYPE:
        conf_obj_ptr+= sizeof(IC_REP_SERVER_CONFIG);
        break;
      default:
        g_assert(FALSE);
        break;
    }
  }
  for (i= 0; i < conf_obj->no_of_comms; i++)
  {
    init_config_object(conf_obj_ptr, IC_COMM_TYPE);
    conf_obj->comm_config[i]= conf_obj_ptr;
    conf_obj_ptr+= sizeof(IC_COMM_LINK_CONFIG);
  }
  g_assert(conf_obj_ptr == (apic->config_memory_to_return +
                            size_config_objects));
  return 0;
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
update_string_data(IC_API_CONFIG_SERVER *apic,
                   guint32 value,
                   guint32 **key_value)
{
  guint32 len_words= (value + 3)/4;
  apic->next_string_memory+= value;
  g_assert(apic->next_string_memory <= apic->end_string_memory);
  (*key_value)+= len_words;
}

static int
analyse_node_section_phase1(IC_CLUSTER_CONFIG *conf_obj,
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
    conf_obj->node_types[sect_id - 2]= (IC_NODE_TYPES)value;
  }
  else if (hash_key == IC_NODE_ID)
  {
    conf_obj->node_ids[sect_id - 2]= value;
    DEBUG(printf("Node id = %u for section %u\n", value, sect_id));
  }
  return 0;
}

static int
step_key_value(IC_API_CONFIG_SERVER *apic,
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
      if (value != (strlen((gchar*)(*key_value)) + 1))
      {
        printf("Wrong length of character type\n");
        return PROTOCOL_ERROR;
      }
      (*key_value)+= len_words;
      if (pass == 1)
        apic->string_memory_size+= value;
      break;
   }
   default:
     return key_type_error(key_type, 32);
  }
  return 0;
}

static IC_CONFIG_ENTRY *get_conf_entry(guint32 hash_key)
{
  guint32 id;
  IC_CONFIG_ENTRY *conf_entry;

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
read_node_section(IC_CLUSTER_CONFIG *conf_obj,
                  IC_API_CONFIG_SERVER *apic,
                  guint32 key_type, guint32 **key_value,
                  guint32 value, guint32 hash_key,
                  guint32 node_sect_id)
{
  IC_CONFIG_ENTRY *conf_entry;
  void *node_config;
  IC_NODE_TYPES node_type;

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
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
      if (ic_assign_config_value(hash_key, (guint64)value,
                                 node_type, key_type,
                                 (gchar*)node_config))
        return PROTOCOL_ERROR;
      break;
    case IC_CL_INT64_TYPE:
    {
      guint64 long_val= get_64bit_value(value, key_value);
      if (ic_assign_config_value(hash_key, long_val,
                                 node_type,
                                 key_type,
                                 (gchar*)node_config))
        return PROTOCOL_ERROR;
      break;
    }
    case IC_CL_CHAR_TYPE:
    {
      if (ic_assign_config_string(hash_key, node_type,
                                  apic->next_string_memory,
                                  value,
                                  (gchar*)node_config,
                                  (gchar**)key_value))
        return PROTOCOL_ERROR;
      update_string_data(apic, value, key_value);
      break;
    }
    default:
      return key_type_error(hash_key, node_type);
  }
  return 0;
}

static int
read_comm_section(IC_CLUSTER_CONFIG *conf_obj,
                  IC_API_CONFIG_SERVER *apic,
                  guint32 key_type, guint32 **key_value,
                  guint32 value, guint32 hash_key,
                  guint32 comm_sect_id)
{
  IC_CONFIG_ENTRY *conf_entry;
  IC_SOCKET_LINK_CONFIG *socket_conf;

  if (hash_key == IC_PARENT_ID || hash_key == IC_NODE_TYPE)
    return 0; /* Ignore */
  if (!(conf_entry= get_conf_entry(hash_key)))
    return PROTOCOL_ERROR;
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0; /* Ignore */
  g_assert(comm_sect_id < conf_obj->no_of_comms);
  socket_conf= (IC_SOCKET_LINK_CONFIG*)conf_obj->comm_config[comm_sect_id];
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
    {
      if (ic_assign_config_value(hash_key, (guint64)value, IC_COMM_TYPE,
                                 key_type, (gchar*)socket_conf))
        return PROTOCOL_ERROR;
      break;
    }
    case IC_CL_INT64_TYPE:
    {
      guint64 long_val= get_64bit_value(value, key_value);
      if (ic_assign_config_value(hash_key, long_val,
                                 IC_COMM_TYPE,
                                 key_type,
                                 (gchar*)socket_conf))
        return PROTOCOL_ERROR;
      break;
    }
    case IC_CL_CHAR_TYPE:
    {
      if (ic_assign_config_string(hash_key, IC_COMM_TYPE,
                                  apic->next_string_memory,
                                  value,
                                  (gchar*)socket_conf,
                                  (gchar**)key_value))
        return PROTOCOL_ERROR;
      update_string_data(apic, value, key_value);
      break;
    }
    default:
      return key_type_error(hash_key, IC_COMM_TYPE);
  }
  return 0;
}

static int
analyse_key_value(guint32 *key_value, guint32 len, int pass,
                  IC_API_CONFIG_SERVER *apic,
                  guint32 current_cluster_index)
{
  guint32 *key_value_end= key_value + len;
  IC_CLUSTER_CONFIG *conf_obj;
  int error;

  conf_obj= apic->conf_objects+current_cluster_index;
  if (pass == 1)
  {
    if ((error= allocate_mem_phase1(conf_obj)))
      return error;
  }
  else if (pass == 2)
  {
    if ((error= allocate_mem_phase2(apic, conf_obj)))
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
      if ((error= step_key_value(apic, key_type, &key_value,
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
          if ((error= read_node_section(conf_obj, apic, key_type, &key_value,
                                        value, hash_key, node_sect_id)))
            return error;
        }
        else
        {
          guint32 comm_sect_id= sect_id - (conf_obj->no_of_nodes + 3);
          if ((error= read_comm_section(conf_obj, apic, key_type, &key_value,
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

static gchar ver_string[8] = { 0x4E, 0x44, 0x42, 0x43, 0x4F, 0x4E, 0x46, 0x56 };
static int
translate_config(IC_API_CONFIG_SERVER *apic,
                 guint32 current_cluster_index,
                 gchar *config_buf,
                 guint32 config_size)
{
  gchar *bin_buf;
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
set_up_cluster_server_connection(IC_CONNECTION *conn,
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
send_get_nodeid(IC_CONNECTION *conn,
                IC_API_CONFIG_SERVER *apic,
                guint32 current_cluster_index)
{
  gchar version_buf[32];
  gchar nodeid_buf[32];
  gchar endian_buf[32];
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
send_get_config(IC_CONNECTION *conn)
{
  gchar version_buf[32];

  g_snprintf(version_buf, 32, "%s%u", version_str, version_no);
  if (ic_send_with_cr(conn, get_config_str) ||
      ic_send_with_cr(conn, version_buf) ||
      ic_send_with_cr(conn, empty_string))
    return conn->error_code;
  return 0;
}

static int
rec_get_nodeid(IC_CONNECTION *conn,
               IC_API_CONFIG_SERVER *apic,
               guint32 current_cluster_index)
{
  gchar read_buf[256];
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
rec_get_config(IC_CONNECTION *conn,
               IC_API_CONFIG_SERVER *apic,
               guint32 current_cluster_index)
{
  gchar read_buf[256];
  gchar *config_buf= NULL;
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

/*
  MODULE: CONFIGURATION CLIENT
  ----------------------------
    This module makes use of a lot of methods above to fetch configuration
    from a configuration server.


  This is the routine that is used to read a configuration from a
  cluster server. It receives a struct that contains a list of configuration
  servers to connect to. It tries to connect to any of the configuration
  servers in this list. If successful it goes on to fetch the configuration
  from this configuration server. There can be many clusters handled by
  this set of configuration servers, however the configuration is fetched
  for one configuration at a time.
  
  Any retry logic is placed outside of this routine. If this routine returns
  0 all cluster configurations have been successfully fetched and also
  successfully parsed and all necessary data structures have been initialised.
*/
static int
get_cs_config(IC_API_CONFIG_SERVER *apic)
{
  guint32 i;
  int error= END_OF_FILE;
  IC_CONNECTION conn_obj, *conn;

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
free_cs_config(IC_API_CONFIG_SERVER *apic)
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
        if (apic->string_memory_to_return)
          g_free(apic->string_memory_to_return);
        if (apic->config_memory_to_return)
          g_free(apic->config_memory_to_return);
        if (conf_obj->comm_config)
          g_free(conf_obj->comm_config);
      }
      g_free(apic->conf_objects);
    }
    g_free(apic);
  }
  return;
}

/*
  This routine initialises the data structures needed for a configuration client.
  It sets the proper routines to get the configuration and to free the
  configuration.
*/
IC_API_CONFIG_SERVER*
ic_init_api_cluster(IC_API_CLUSTER_CONNECTION *cluster_conn,
                    guint32 *cluster_ids,
                    guint32 *node_ids,
                    guint32 num_clusters)
{
  IC_API_CONFIG_SERVER *apic= NULL;
  guint32 num_cluster_servers= cluster_conn->num_cluster_servers;
  /*
    The idea with this method is that the user can set-up his desired usage
    of the clusters using stack variables. Then we copy those variables to
    heap storage and maintain this data hereafter.
    Thus we can in many cases relieve the user from the burden of error
    handling of memory allocation failures.

    We will also ensure that the supplied data is validated.
  */
  if (!(apic= g_try_malloc0(sizeof(IC_API_CONFIG_SERVER))) ||
      !(apic->cluster_ids= g_try_malloc0(sizeof(guint32) * num_clusters)) ||
      !(apic->node_ids= g_try_malloc0(sizeof(guint32) * num_clusters)) ||
      !(apic->cluster_conn.cluster_server_ips= 
         g_try_malloc0(num_cluster_servers *
                       sizeof(guint32))) ||
      !(apic->cluster_conn.cluster_server_ports=
         g_try_malloc0(num_cluster_servers *
                       sizeof(guint16))) ||
      !(apic->conf_objects= g_try_malloc0(num_cluster_servers *
                                sizeof(IC_CLUSTER_CONFIG))) ||
      !(apic->cluster_conn.cluster_srv_conns=
         g_try_malloc0(num_cluster_servers *
                       sizeof(IC_CONNECTION))))
  {
    free_cs_config(apic);
    return NULL;
  }

  memcpy((gchar*)apic->cluster_ids,
         (gchar*)cluster_ids,
         num_clusters * sizeof(guint32));
  memcpy((gchar*)apic->node_ids,
         (gchar*)node_ids,
         num_clusters * sizeof(guint32));
  memcpy((gchar*)apic->cluster_conn.cluster_server_ips,
         (gchar*)cluster_conn->cluster_server_ips,
         num_cluster_servers * sizeof(guint32));
  memcpy((gchar*)apic->cluster_conn.cluster_server_ports,
         (gchar*)cluster_conn->cluster_server_ports,
         num_cluster_servers * sizeof(guint16));
  memset((gchar*)apic->conf_objects, 0,
         num_cluster_servers * sizeof(IC_CLUSTER_CONFIG));
  memset((gchar*)apic->cluster_conn.cluster_srv_conns, 0,
         num_cluster_servers * sizeof(IC_CONNECTION));

  apic->cluster_conn.num_cluster_servers= num_cluster_servers;
  apic->api_op.get_ic_config= get_cs_config;
  apic->api_op.free_ic_config= free_cs_config;
  return apic;
}

/*
  MODULE: CONFIGURATION SERVER
  ----------------------------
    This is the module that provided with a configuration data structures
    ensures that anyone can request this configuration through a given
    socket and port.
*/
static void
free_run_cluster(IC_RUN_CONFIG_SERVER *run_obj)
{
  return;
}

static int
run_cluster_server(IC_CLUSTER_CONFIG *cs_conf,
                   IC_RUN_CONFIG_SERVER *run_obj,
                   guint32 num_connects)
{
  return 0;
}

IC_RUN_CONFIG_SERVER*
ic_init_run_cluster(IC_CLUSTER_CONFIG *conf_objs,
                    guint32 *cluster_ids,
                    guint32 num_clusters)
{
  IC_RUN_CONFIG_SERVER *run_obj;
  if (!(run_obj= g_try_malloc0(sizeof(IC_API_CONFIG_SERVER))))
    return NULL;
  return run_obj;
}

/*
  MODULE: LOAD CONFIG DATA
  ------------------------
    This is a configuration server implementing the ic_config_operations
    interface. It converts a configuration file to a set of data structures
    describing the configuration of a iClaustron Cluster Server.

    This is the module that reads a configuration file to fill up the
    data structures that later can be used by the Configuration Server
    module to handle requests from Configuration Clients.
*/

static
int complete_section(IC_CONFIG_STRUCT *ic_config, guint32 line_number,
                     guint32 pass)
{
  /*
    Need to check that all mandatory values have been assigned here in
    second pass.
  */
  return 0;
}

static
int conf_serv_init(IC_CONFIG_STRUCT *ic_conf, guint32 pass)
{
  IC_CLUSTER_CONFIG_LOAD *clu_conf;
  guint32 max_node_id;
  guint32 num_communication_sections;
  DEBUG_ENTRY("conf_serv_init");
  if (pass == INITIAL_PASS)
  {
    if (!(clu_conf= (IC_CLUSTER_CONFIG_LOAD*)g_try_malloc0(
                    sizeof(IC_CLUSTER_CONFIG_LOAD))))
      return IC_ERROR_MEM_ALLOC;
    ic_conf->config_ptr.clu_conf= (struct ic_cluster_config_load*)clu_conf;
    clu_conf->current_node_config_type= IC_NO_CONFIG_TYPE;
    return 0;
  }
  clu_conf= ic_conf->config_ptr.clu_conf;
  max_node_id= clu_conf->max_node_id;
  if (max_node_id == 0)
    return IC_ERROR_NO_NODES_FOUND;
  clu_conf->current_node_config_type= IC_NO_CONFIG_TYPE;
  num_communication_sections= clu_conf->num_communication_sections;
  if (!(clu_conf->conf.node_config= (gchar**)
        g_try_malloc0(sizeof(gchar*)*(max_node_id + 1))))
    return IC_ERROR_MEM_ALLOC;
  if (!(clu_conf->string_memory= (gchar*)
        g_try_malloc0(clu_conf->size_string_memory)))
    return IC_ERROR_MEM_ALLOC;
  if (!(clu_conf->conf.node_types= 
        (IC_NODE_TYPES*)g_try_malloc0(sizeof(IC_NODE_TYPES)*(max_node_id+1))))
    return IC_ERROR_MEM_ALLOC;
  init_config_object((gchar*)&clu_conf->default_kernel_config, IC_KERNEL_TYPE);
  init_config_object((gchar*)&clu_conf->default_rep_config.client_conf,
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_rep_config, IC_REP_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_sql_config.client_conf,
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_sql_config, IC_SQL_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_cluster_server_config,
                     IC_CLUSTER_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_client_config, IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_socket_config, IC_COMM_TYPE);
  return 0;
}

static
int conf_serv_add_section(IC_CONFIG_STRUCT *ic_config,
                          guint32 section_number,
                          guint32 line_number,
                          IC_STRING *section_name,
                          guint32 pass)
{
  int error;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= ic_config->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_add_section");
  DEBUG_IC_STRING(section_name);

  if ((error= complete_section(ic_config, line_number, pass)))
    return error;
  clu_conf->default_section= FALSE;
  if (!ic_cmp_null_term_str(data_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_KERNEL_TYPE;
    if (pass == INITIAL_PASS)
      return 0;
    if (!(clu_conf->current_node_config= 
          (void*)g_try_malloc0(sizeof(IC_KERNEL_CONFIG))))
      return IC_ERROR_MEM_ALLOC;
    memcpy(clu_conf->current_node_config, &clu_conf->default_kernel_config,
           sizeof(IC_KERNEL_CONFIG));
    DEBUG(printf("Found data server group\n"));
    return 0;
  }
  else if (!ic_cmp_null_term_str(client_node_str, section_name))
  {
    clu_conf->current_node_config_type= IC_CLIENT_TYPE;
    if (pass == INITIAL_PASS)
      return 0;
    if (!(clu_conf->current_node_config= 
          (void*)g_try_malloc0(sizeof(IC_CLIENT_CONFIG))))
      return IC_ERROR_MEM_ALLOC;
    memcpy(clu_conf->current_node_config, &clu_conf->default_client_config,
           sizeof(IC_CLIENT_CONFIG));
    DEBUG(printf("Found client group\n"));
    return 0;
  }
  else if (!ic_cmp_null_term_str(cluster_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_CLUSTER_SERVER_TYPE;
    if (pass == INITIAL_PASS)
      return 0;
    if (!(clu_conf->current_node_config= 
          (void*)g_try_malloc0(sizeof(IC_CLUSTER_SERVER_CONFIG))))
      return IC_ERROR_MEM_ALLOC;
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_cluster_server_config,
           sizeof(IC_CLUSTER_SERVER_CONFIG));
    DEBUG(printf("Found cluster server group\n"));
    return 0;
  }
  else if (!ic_cmp_null_term_str(rep_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_REP_SERVER_TYPE;
    if (pass == INITIAL_PASS)
      return 0;
    if (!(clu_conf->current_node_config= 
          (void*)g_try_malloc0(sizeof(IC_REP_SERVER_CONFIG))))
      return IC_ERROR_MEM_ALLOC;
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_rep_config,
           sizeof(IC_REP_SERVER_CONFIG));
    DEBUG(printf("Found replication server group\n"));
    return 0;
  }
  else if (!ic_cmp_null_term_str(sql_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_SQL_SERVER_TYPE;
    if (pass == INITIAL_PASS)
      return 0;
    if (!(clu_conf->current_node_config= 
          (void*)g_try_malloc0(sizeof(IC_SQL_SERVER_CONFIG))))
      return IC_ERROR_MEM_ALLOC;
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_sql_config,
           sizeof(IC_SQL_SERVER_CONFIG));
    DEBUG(printf("Found sql server group\n"));
    return 0;
  }
  else if (!ic_cmp_null_term_str(socket_str, section_name))
  {
    clu_conf->current_node_config_type= IC_COMM_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->num_communication_sections++;
      return 0;
    }
    if (!(clu_conf->current_node_config= 
          (void*)g_try_malloc0(sizeof(IC_SOCKET_LINK_CONFIG))))
      return IC_ERROR_MEM_ALLOC;
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_socket_config,
           sizeof(IC_SOCKET_LINK_CONFIG));
    DEBUG(printf("Found socket group\n"));
    return 0;
  }
  else
  {
    clu_conf->default_section= TRUE;
    printf("Here\n");
    if (!ic_cmp_null_term_str(data_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_kernel_config;
      clu_conf->current_node_config_type= IC_KERNEL_TYPE;
      DEBUG(printf("Found data server default group\n"));
      return 0;
    }
    else if (!ic_cmp_null_term_str(client_node_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_client_config;
      clu_conf->current_node_config_type= IC_CLIENT_TYPE;
      DEBUG(printf("Found client default group\n"));
      return 0;
    }
    else if (!ic_cmp_null_term_str(cluster_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_cluster_server_config;
      clu_conf->current_node_config_type= IC_CLUSTER_SERVER_TYPE;
      DEBUG(printf("Found cluster server default group\n"));
      return 0;
    }
    else if (!ic_cmp_null_term_str(sql_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_sql_config;
      clu_conf->current_node_config_type= IC_SQL_SERVER_TYPE;
      DEBUG(printf("Found sql server default group\n"));
      return 0;
    }
    else if (!ic_cmp_null_term_str(rep_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_rep_config;
      clu_conf->current_node_config_type= IC_REP_SERVER_TYPE;
      DEBUG(printf("Found replication server default group\n"));
      return 0;
    }
    else if (!ic_cmp_null_term_str(socket_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_socket_config;
      clu_conf->current_node_config_type= IC_COMM_TYPE;
      DEBUG(printf("Found socket default group\n"));
      return 0;
    }
    else
    {
      DEBUG(printf("No such config section\n"));
      return IC_ERROR_CONFIG_NO_SUCH_SECTION;
    }
  }
}

static
int conf_serv_add_key(IC_CONFIG_STRUCT *ic_config,
                      guint32 section_number,
                      guint32 line_number,
                      IC_STRING *key_name,
                      IC_STRING *data,
                      guint32 pass)
{
  IC_CLUSTER_CONFIG_LOAD *clu_conf= ic_config->config_ptr.clu_conf;
  IC_CONFIG_ENTRY *conf_entry;
  guint32 conf_map_id;
  guint64 value;
  gchar *struct_ptr;
  guint64 num32_check;
  DEBUG_ENTRY("conf_serv_add_key");
  printf("Line: %d Section: %d, Key-value pair\n", (int)line_number,
                                                   (int)section_number);
  if (clu_conf->current_node_config_type == IC_NO_CONFIG_TYPE)
    return IC_ERROR_NO_SECTION_DEFINED_YET;
  if (!(conf_entry= (IC_CONFIG_ENTRY*)ic_hashtable_search(glob_conf_hash,
                                                          (void*)key_name)))
    return IC_ERROR_NO_SUCH_CONFIG_KEY;
  if (!(conf_entry->node_types & (1 << clu_conf->current_node_config_type)))
    return IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION;
  if (conf_entry->is_string_type)
  {
    if (pass == INITIAL_PASS)
    {
      clu_conf->size_string_memory+= (data->len+1);
    }
    else
    {
      strncpy(clu_conf->string_memory, data->str, data->len);
      clu_conf->string_memory+= (data->len+1);
    }
    return 0;
  }
  if (pass == INITIAL_PASS)
    return 0;
  if (conv_config_str_to_int(&value, data))
    return IC_ERROR_WRONG_CONFIG_NUMBER;
  if (conf_entry->is_boolean && value > 1)
    return IC_ERROR_NO_BOOLEAN_VALUE;
  num32_check= 1;
  num32_check<<= 32;
  if ((conf_entry->is_min_value_defined && conf_entry->min_value > value) ||
      (conf_entry->is_max_value_defined && conf_entry->max_value < value) ||
      (conf_entry->data_type == IC_UINT16 && value > 65535) ||
      (conf_entry->data_type == IC_UINT32 && value >= num32_check))
    return IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS;
  struct_ptr= (gchar*)clu_conf->current_node_config + conf_entry->offset;
  /*
    Assign value of configuration variable according to its data type.
  */
  if (conf_entry->data_type == IC_CHAR)
    *(gchar*)struct_ptr= (gchar)value;
  else if (conf_entry->data_type == IC_UINT16)
    *(guint16*)struct_ptr= (guint16)value;
  else if (conf_entry->data_type == IC_UINT32)
    *(guint32*)struct_ptr= (guint32)value;
  else if (conf_entry->data_type == IC_UINT64)
    *(guint64*)struct_ptr= value;
  else
  {
    g_assert(FALSE);
    abort();
    return 1;
  }
  return 0;
}

static
int conf_serv_add_comment(IC_CONFIG_STRUCT *ic_config,
                          guint32 line_number,
                          guint32 section_number,
                          IC_STRING *comment,
                          guint32 pass)
{
  IC_CLUSTER_CONFIG_LOAD *clu_conf= ic_config->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_add_comment");
  printf("Line number %d in section %d was comment line\n", line_number, section_number);
  if (pass == INITIAL_PASS)
    clu_conf->comments.num_comments++;
  else
  {
    ;
  }
  return 0;
}

static
void conf_serv_end(IC_CONFIG_STRUCT *ic_conf)
{
  IC_CLUSTER_CONFIG_LOAD *clu_conf= ic_conf->config_ptr.clu_conf;
  guint32 i;
  DEBUG_ENTRY("conf_serv_end");
  if (clu_conf)
  {
    for (i= 0; i < clu_conf->max_node_id; i++)
    {
      if (clu_conf->conf.node_config[i])
        g_free((gchar*)clu_conf->conf.node_config[i]);
    }
    if (clu_conf->conf.node_types)
      g_free(clu_conf->conf.node_types);
    for (i= 0; i < clu_conf->comments.num_comments; i++)
      g_free(clu_conf->comments.ptr_comments[i]);
    g_free(clu_conf->string_memory);
    g_free(ic_conf->config_ptr.clu_conf);
  }
  return;
}

static IC_CONFIG_OPERATIONS config_server_ops =
{
  .ic_config_init = conf_serv_init,
  .ic_add_section = conf_serv_add_section,
  .ic_add_key     = conf_serv_add_key,
  .ic_add_comment = conf_serv_add_comment,
  .ic_config_end  = conf_serv_end,
};

int
ic_load_config_server_from_files(gchar *config_file_path,
                                 IC_CONFIG_STRUCT *conf_server)
{
  gchar *conf_data_str;
  gsize conf_data_len;
  GError *loc_error= NULL;
  IC_STRING conf_data;
  int ret_val;
  IC_CONFIG_ERROR err_obj;
  DEBUG_ENTRY("ic_load_config_server_from_files");
  memset(conf_server, 0, sizeof(IC_CONFIG_STRUCT));
  conf_server->clu_conf_ops= &config_server_ops;
  DEBUG(printf("config_file_path = %s\n", config_file_path));
  if (!config_file_path ||
      !g_file_get_contents(config_file_path, &conf_data_str,
                           &conf_data_len, &loc_error))
    goto file_open_error;

  IC_INIT_STRING(&conf_data, conf_data_str, conf_data_len, TRUE);
  ret_val= ic_build_config_data(&conf_data, &config_server_ops,
                                conf_server, &err_obj);
  g_free(conf_data.str);
  if (ret_val == 1)
  {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Error at Line number %u:\n%s\n",err_obj.line_number,
          ic_get_error_message(err_obj.err_num));
  }
  return ret_val;

file_open_error:
  if (!config_file_path)
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "--config-file parameter required when using --bootstrap\n");
  else
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Couldn't open file %s\n", config_file_path);
  return 1;
}

/*
  iClaustron initialisation routine
  The ic_init routine must be called at the before using any other
  function in the iClaustron API's. The ic_end must be called when
  the iClaustron API's have been used to completion, normally at the
  end of the program.
*/
int ic_init()
{
  int ret_value;
  DEBUG_ENTRY("ic_init");
  ic_init_error_messages();
  if ((ret_value= ic_init_config_parameters()))
    ic_end();
  return ret_value;
}

void ic_end()
{
  DEBUG_ENTRY("ic_end");
  if (glob_conf_hash)
    ic_hashtable_destroy(glob_conf_hash);
  glob_conf_entry_inited= FALSE;
}
