#include <ic_apic.h>
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
     result: Error (MEssafge)<CR>
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
#define IC_PARENT_ID     16382

static guint16 map_config_id[1024];
static struct config_entry glob_conf_entry[256];
static gboolean glob_conf_entry_inited= FALSE;

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
static const guint32 version_no= (guint32)0x5010C; /* 5.1.12 */

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
#define KERNEL_WAIT_PARTITIAL_START 115
#define KERNEL_WAIT_PARTITIONED_START 116
#define KERNEL_WAIT_ERROR_START 117
#define KERNEL_HEARTBEAT_TIMER 118
#define KERNEL_CLIENT_HEARTBEAT_TIMER 119
#define KERNEL_LOCAL_CHECKPOINT_TIMER 120
#define KERNEL_GLOBAL_CHECKPOINT_TIMER 121
#define KERNEL_ARBITRATOR_TIMER 122
#define KERNEL_WATCHDOG_TIMER 123
#define KERNEL_DAEMON_RESTART_AT_ERROR 124

#define TCP_FIRST_NODE_ID 400
#define TCP_SECOND_NODE_ID 401
#define TCP_USE_MESSAGE_ID 402
#define TCP_USE_CHECKSUM 403
#define TCP_CLIENT_PORT 2000
#define TCP_SERVER_PORT 406
#define TCP_SERVER_NODE_ID 410
#define TCP_WRITE_BUFFER_SIZE 454
#define TCP_READ_BUFFER_SIZE 455
#define TCP_FIRST_HOSTNAME 407
#define TCP_SECOND_HOSTNAME 408
#define TCP_GROUP 409

#define TCP_MIN_WRITE_BUFFER_SIZE 65536
#define TCP_MAX_WRITE_BUFFER_SIZE 100000000

#define TCP_MIN_READ_BUFFER_SIZE 8192
#define TCP_MAX_READ_BUFFER_SIZE 1000000000

static void
ic_init_config_parameters()
{
  struct config_entry *conf_entry;
  if (glob_conf_entry_inited)
    return;
  glob_conf_entry_inited= TRUE;
  memset(map_config_id, 0, 1024 * sizeof(guint16));
  memset(glob_conf_entry, 0, 256 * sizeof(struct config_entry));

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
  conf_entry->node_type= IC_KERNEL_TYPE;
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
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_NOT_CHANGEABLE;

  map_config_id[KERNEL_TABLE_OBJECTS]= 3;
  conf_entry= &glob_conf_entry[3];
  conf_entry->config_entry_name= "number_of_table_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of tables that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->default_value= 256;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_COLUMN_OBJECTS]= 4;
  conf_entry= &glob_conf_entry[4];
  conf_entry->config_entry_name= "number_of_column_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of columns that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 256;
  conf_entry->default_value= 2048;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_KEY_OBJECTS]= 5;
  conf_entry= &glob_conf_entry[5];
  conf_entry->config_entry_name= "number_of_key_objects";
  conf_entry->config_entry_description=
  "Sets the maximum number of keys that can be stored in cluster";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 32;
  conf_entry->default_value= 256;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_INTERNAL_TRIGGER_OBJECTS]= 6;
  conf_entry= &glob_conf_entry[6];
  conf_entry->config_entry_name= "number_of_internal_trigger_objects";
  conf_entry->config_entry_description=
  "Each unique index will use 3 internal trigger objects, index/backup will use 1 per table";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 512;
  conf_entry->default_value= 1536;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_CONNECTION_OBJECTS]= 7;
  conf_entry= &glob_conf_entry[7];
  conf_entry->config_entry_name= "number_of_connection_objects";
  conf_entry->config_entry_description=
  "Each active transaction and active scan uses a connection object";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 128;
  conf_entry->default_value= 8192;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_CLUSTER_RESTART_CHANGE;

  map_config_id[KERNEL_OPERATION_OBJECTS]= 8;
  conf_entry= &glob_conf_entry[8];
  conf_entry->config_entry_name= "number_of_operation_objects";
  conf_entry->config_entry_description=
  "Each record read/updated in a transaction uses an operation object during the transaction";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 1024;
  conf_entry->default_value= 32768;
  conf_entry->node_type= IC_KERNEL_TYPE;
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
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_INTERNAL_TRIGGER_OPERATION_OBJECTS]= 10;
  conf_entry= &glob_conf_entry[10];
  conf_entry->config_entry_name= "number_of_internal_trigger_operation_objects";
  conf_entry->config_entry_description=
  "Each internal trigger that is fired uses an operation object for a short time";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->is_max_value_defined= TRUE;
  conf_entry->is_not_configurable= TRUE;
  conf_entry->min_value= 4096;
  conf_entry->max_value= 4096;
  conf_entry->default_value= 4096;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_KEY_OPERATION_OBJECTS]= 11;
  conf_entry= &glob_conf_entry[11];
  conf_entry->config_entry_name= "number_of_key_operation_objects";
  conf_entry->config_entry_description=
  "Each read and update of an unique hash index in a transaction uses one of those objects";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 128;
  conf_entry->default_value= 4096;
  conf_entry->node_type= IC_KERNEL_TYPE;
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
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_RAM_MEMORY]= 13;
  conf_entry= &glob_conf_entry[13];
  conf_entry->config_entry_name= "size_of_ram_memory";
  conf_entry->config_entry_description=
  "Size of memory used to store RAM-based records";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 16 * 1024 * 1024;
  conf_entry->default_value= 256 * 1024 * 1024;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_HASH_MEMORY]= 14;
  conf_entry= &glob_conf_entry[14];
  conf_entry->config_entry_name= "size_of_hash_memory";
  conf_entry->config_entry_description=
  "Size of memory used to store primary hash index on all tables and unique hash indexes";
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 8 * 1024 * 1024;
  conf_entry->default_value= 64 * 1024 * 1024;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_LOCK_MEMORY]= 15;
  conf_entry= &glob_conf_entry[15];
  conf_entry->config_entry_name= "use_unswappable_memory";
  conf_entry->config_entry_description=
  "Setting this to 1 means that all memory is locked and will not be swapped out";
  conf_entry->is_boolean= TRUE;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_WAIT_PARTIAL_START]= 16;
  conf_entry= &glob_conf_entry[16];
  conf_entry->config_entry_name= "timer_wait_partial_start";
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting with a partial set of nodes, 0 waits forever"
  conf_entry->default_value= 20000;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_WAIT_PARTITIONED_START]= 17;
  conf_entry= &glob_conf_entry[17];
  conf_entry->config_entry_name= "timer_wait_partitioned_start";
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting a potentially partitioned cluster, 0 waits forever"
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_WAIT_ERROR_START]= 18;
  conf_entry= &glob_conf_entry[18];
  conf_entry->config_entry_name= "timer_wait_error_start";
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before forcing a stop after an error, 0 waits forever"
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE;

  map_config_id[KERNEL_HEARTBEAT_TIMER]= 19;
  conf_entry= &glob_conf_entry[19];
  conf_entry->config_entry_name= "timer_heartbeat_kernel_nodes";
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to kernel nodes, 4 missed leads to node crash"
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 10;
  conf_entry->default_value= 700;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE_SPECIAL;

  map_config_id[KERNEL_CLIENT_HEARTBEAT_TIMER]= 20;
  conf_entry= &glob_conf_entry[20];
  conf_entry->config_entry_name= "timer_heartbeat_client_nodes";
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to client nodes, 4 missed leads to node crash"
  conf_entry->is_min_value_defined= TRUE;
  conf_entry->min_value= 10;
  conf_entry->default_value= 1000;
  conf_entry->node_type= IC_KERNEL_TYPE;
  conf_entry->change_variant= IC_ROLLING_UPGRADE_CHANGE_SPECIAL;

  return;
}

static int
ic_check_config_value(int config_id, guint64 value)
{
  guint32 inx;
  struct config_entry *conf_entry;

  if (config_id > 998)
    return PROTOCOL_ERROR;
  inx= map_config_id[config_id];
  if (!inx)
    return PROTOCOL_ERROR;
  conf_entry= &glob_conf_entry[inx];
  if ((conf_entry->is_boolean && (value > 1)) ||
      (conf_entry->is_min_value_defined &&
       (conf_entry->min_value > value)) ||
      (conf_entry->is_max_value_defined &&
       (conf_entry->max_value < value)))
    return PROTOCOL_ERROR;
  return 0;
}

/*
  CONFIGURATION TRANSLATE KEY DATA TO CONFIGURATION OBJECTS MODULE
  ----------------------------------------------------------------
*/

static int
allocate_mem_phase1(struct ic_api_cluster_config *conf_obj)
{
  /*
    Allocate memory for pointer arrays pointing to the configurations of the
    nodes in the cluster, also allocate memory for array of node types.
  */
  conf_obj->node_types= g_try_malloc0(conf_obj->no_of_nodes *
                                         sizeof(enum ic_node_type*));
  conf_obj->node_ids= g_try_malloc0(conf_obj->no_of_nodes *
                                       sizeof(guint32));
  conf_obj->node_config= g_try_malloc0(conf_obj->no_of_nodes *
                                          sizeof(char*));
  if (!conf_obj->node_types || !conf_obj->node_ids || !conf_obj->node_config)
    return MEM_ALLOC_ERROR;
  return 0;
}


static int
allocate_mem_phase2(struct ic_api_cluster_config *conf_obj)
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
        conf_obj_ptr+= sizeof(struct ic_kernel_node_config);
        break;
      case IC_CLIENT_TYPE:
        conf_obj_ptr+= sizeof(struct ic_client_node_config);
        break;
      case IC_CLUSTER_SERVER_TYPE:
        conf_obj_ptr+= sizeof(struct ic_cluster_server_config);
        break;
      default:
        g_assert(FALSE);

        break;
    }
  }
  for (i= 0; i < conf_obj->no_of_comms; i++)
  {
    conf_obj->comm_config[i]= conf_obj_ptr;
    conf_obj_ptr+= sizeof(struct ic_comm_link_config);
  }
  g_assert(conf_obj_ptr == (conf_obj->config_memory_to_return +
                            size_config_objects));
  return 0;
}

static int
analyse_node_section_phase1(struct ic_api_cluster_config *conf_obj,
                            guint32 sect_id, guint32 value, guint32 hash_key)
{
  if (hash_key == IC_NODE_TYPE)
  {
    printf("Node type of section %u is %u\n", sect_id, value); 
    switch (value)
    {
      case IC_CLIENT_TYPE:
        conf_obj->no_of_client_nodes++;
        break;
      case IC_KERNEL_TYPE:
        conf_obj->no_of_kernel_nodes++;
        break;
      case IC_CLUSTER_SERVER_TYPE:
        conf_obj->no_of_cluster_servers++;

        break;
      default:
        return PROTOCOL_ERROR;
    }
    conf_obj->node_types[sect_id - 2]= (enum ic_node_type)value;
  }
  else if (hash_key == IC_NODE_ID)
  {
    conf_obj->node_ids[sect_id - 2]= value;
    printf("Node id = %u for section %u\n", value, sect_id);
  }
  return 0;
}

static int
step_key_value(struct ic_api_cluster_config *conf_obj,
               guint32 key_type, guint32 **key_value,
               guint32 value, guint32 *key_value_end, int pass)
{
  guint32 len_words;
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
    case IC_CL_SECT_TYPE:
      break;
    case IC_CL_INT64_TYPE:
      (*key_value)++;
      break;
    case IC_CL_CHAR_TYPE:
      len_words= (value + 3)/4;
      if (((*key_value) + len_words) >= key_value_end)
      {
        DEBUG(printf("Array ended in the middle of a type\n"));
        return PROTOCOL_ERROR;
      }
      if (value != (strlen((char*)(*key_value)) + 1))
      {
        DEBUG(printf("Wrong length of character type\n"));
        return PROTOCOL_ERROR;
      }
      (*key_value)+= len_words;
      if (pass == 1)
        conf_obj->string_memory_size+= value;
        break;
   default:
     DEBUG(printf("Wrong key type %u\n", key_type));
     return PROTOCOL_ERROR;
  }
  return 0;
}

static int
read_node_section(struct ic_api_cluster_config *conf_obj,
                  guint32 key_type, guint32 **key_value,
                  guint32 value, guint32 hash_key,
                  guint32 node_sect_id)
{
  guint32 len_words;
  if (conf_obj->node_types[node_sect_id] == IC_KERNEL_TYPE)
  {
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
        len_words= (value + 3)/4;
        (*key_value)+= len_words;
        break;
     default:
       g_assert(FALSE);
       return PROTOCOL_ERROR;
    }
  }
  else if (conf_obj->node_types[node_sect_id] == IC_CLIENT_TYPE)
  {
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
        len_words= (value + 3)/4;
        (*key_value)+= len_words;
        break;
     default:
       g_assert(FALSE);
       return PROTOCOL_ERROR;
    }
  }
  else if (conf_obj->node_types[node_sect_id] == IC_CLUSTER_SERVER_TYPE)
  {
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
        len_words= (value + 3)/4;
        (*key_value)+= len_words;
        break;
     default:
       g_assert(FALSE);
       return PROTOCOL_ERROR;
    }
  }
  else
    g_assert(FALSE);
  return 0;
}

static int
read_comm_section(struct ic_api_cluster_config *conf_obj,
                  guint32 key_type, guint32 **key_value,
                  guint32 value, guint32 hash_key,
                  guint32 comm_sect_id)
{
  guint32 len_words;
  struct ic_tcp_comm_link_config *tcp_conf;

  g_assert(comm_sect_id < conf_obj->no_of_comms);
  tcp_conf= (struct ic_tcp_comm_link_config*)conf_obj->comm_config[comm_sect_id];
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
    {
      switch (hash_key)
      {
        case TCP_FIRST_NODE_ID:
          if (value > MAX_NODE_ID || !value)
            return PROTOCOL_ERROR;
          tcp_conf->first_node_id= value;
          break;
        case TCP_SECOND_NODE_ID:
          if (value > MAX_NODE_ID || !value)
            return PROTOCOL_ERROR;
          tcp_conf->second_node_id= value;
          break;
        case TCP_USE_MESSAGE_ID:
          if (value > 1)
            return PROTOCOL_ERROR;
          tcp_conf->use_message_id= (gchar)value;
          break;
        case TCP_USE_CHECKSUM:
          if (value > 1)
            return PROTOCOL_ERROR;
          tcp_conf->use_checksum= (gchar)value;
          break;
        case TCP_CLIENT_PORT:
          if (value > 65535)
            return PROTOCOL_ERROR;
          tcp_conf->client_port= (guint16)value;
          break;
        case TCP_SERVER_PORT:
          if (value > 65535)
            return PROTOCOL_ERROR;
          tcp_conf->server_port= (guint16)value;
          break;
        case TCP_SERVER_NODE_ID:
          if (value > MAX_NODE_ID || !value)
            return PROTOCOL_ERROR;
          tcp_conf->first_node_id= (guint16)value;
          break;
        case TCP_WRITE_BUFFER_SIZE:
          if (value > TCP_MAX_WRITE_BUFFER_SIZE ||
              value < TCP_MIN_WRITE_BUFFER_SIZE)
            return PROTOCOL_ERROR;
          tcp_conf->write_buffer_size= value;
          break;
        case TCP_READ_BUFFER_SIZE:
          if (value > TCP_MAX_READ_BUFFER_SIZE ||
              value < TCP_MIN_READ_BUFFER_SIZE)
            return PROTOCOL_ERROR;
          tcp_conf->read_buffer_size= value;
          break;
        case TCP_GROUP:
        case IC_PARENT_ID:
          /* Ignore for now */
          break;
        default:
          return PROTOCOL_ERROR;
      }
      break;
    }
    case IC_CL_SECT_TYPE:
      break;
    case IC_CL_INT64_TYPE:
      (*key_value)++;
      break;
    case IC_CL_CHAR_TYPE:
    {
      len_words= (value + 3)/4;
      if (hash_key == TCP_FIRST_HOSTNAME)
      {
        tcp_conf->first_host_name= conf_obj->next_string_memory;
        strcpy(tcp_conf->first_host_name, (char*)(*key_value));
      }
      else if (hash_key == TCP_SECOND_HOSTNAME)
      {
        tcp_conf->second_host_name= conf_obj->next_string_memory;
        strcpy(tcp_conf->second_host_name, (char*)(*key_value));
      }
      else
      {
        DEBUG(printf("hash_key = %u, string = %s\n", hash_key,
                     (char*)*key_value));
        return PROTOCOL_ERROR;
      }
      conf_obj->next_string_memory+= (value + 1);
      g_assert(conf_obj->next_string_memory <= conf_obj->end_string_memory);
      (*key_value)+= len_words;
      break;
    }
    default:
      g_assert(FALSE);
      return PROTOCOL_ERROR;
  }
  return 0;
}

static int
analyse_key_value(guint32 *key_value, guint32 len, int pass,
                  struct ic_api_cluster_server *apic,
                  guint32 current_cluster_index)
{
  guint32 *key_value_end= key_value + len;
  struct ic_api_cluster_config *conf_obj;
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
    if (pass == 2)
      printf("hash_key = %u, sect_id %u, key_type %u pass %d\n", hash_key, sect_id, key_type, pass);
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
      if (sect_id != 1 && sect_id != (conf_obj->no_of_nodes + 2))
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

static int
send_cluster_server(struct ic_connection *conn, const char *send_buf)
{
  guint32 inx;
  int res;
  char buf[256];

  strcpy(buf, send_buf);
  inx= strlen(buf);
  buf[inx++]= CARRIAGE_RETURN;
  buf[inx]= NULL_BYTE;
  DEBUG(printf("Send: %s", buf));
  res= conn->conn_op.write_ic_connection(conn, (const void*)buf, inx, 0, 1);
  return res;
}

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
    DEBUG(printf("1:Protocol error in base64 decode\n"));
    return PROTOCOL_ERROR;
  }
  bin_config_size32= bin_config_size >> 2;
  printf("size32 = %u\n", bin_config_size32);
  if ((bin_config_size & 3) != 0 || bin_config_size32 <= 3)
  {
    DEBUG(printf("2:Protocol error in base64 decode\n"));
    return PROTOCOL_ERROR;
  }
  if (memcmp(bin_buf, ver_string, 8))
  {
    DEBUG(printf("3:Protocol error in base64 decode\n"));
    return PROTOCOL_ERROR;
  }
  bin_buf32= (guint32*)bin_buf;
  checksum= 0;
  for (i= 0; i < bin_config_size32; i++)
    checksum^= g_ntohl(bin_buf32[i]);
  if (checksum)
  {
    DEBUG(printf("4:Protocol error in base64 decode\n"));
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
rec_cs_read_line(struct ic_api_cluster_server *apic,
                 struct ic_connection *conn,
                 char **read_line,
                 guint32 *read_size)
{
  char *rec_buf= (char*)&apic->cluster_conn.rec_buf;
  char *tail_rec_buf= rec_buf + apic->cluster_conn.tail_index;
  guint32 size_curr_buf= apic->cluster_conn.head_index - 
                         apic->cluster_conn.tail_index;
  guint32 inx, size_to_read, size_read;
  int res;
  char *end_line;

  do
  {
    if (size_curr_buf > 0)
    {
      for (end_line= tail_rec_buf, inx= 0;
           inx < size_curr_buf && end_line[inx] != CARRIAGE_RETURN;
           inx++)
        ;
      if (inx != size_curr_buf)
      {
        *read_line= tail_rec_buf;
        *read_size= inx;
        apic->cluster_conn.tail_index+= (inx + 1);
        return 0;
      }
      /*
        We had no complete lines to read in the buffer received so
        far. We move the current buffer to the beginning to prepare
        for receiving more data.
      */
      memmove(rec_buf, tail_rec_buf, size_curr_buf);
      tail_rec_buf= rec_buf;
      apic->cluster_conn.tail_index= 0;
      apic->cluster_conn.head_index= size_curr_buf;
    }
    else
    {
      apic->cluster_conn.head_index= apic->cluster_conn.tail_index= 0;
    }

    size_to_read= REC_BUF_SIZE - size_curr_buf;
    if ((res= conn->conn_op.read_ic_connection(conn,
                                               rec_buf + size_curr_buf,
                                               size_to_read,
                                               &size_read)))
      return res;
    size_curr_buf+= size_read;
    apic->cluster_conn.head_index+= size_read;
    tail_rec_buf= rec_buf;
  } while (1);
  return 0;
}

static int
set_up_cluster_server_connection(struct ic_connection *conn,
                                 guint32 server_ip,
                                 guint16 server_port)
{
  int error;

  if (ic_init_socket_object(conn, TRUE, FALSE, FALSE, FALSE))
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
  if (send_cluster_server(conn, get_nodeid_str) ||
      send_cluster_server(conn, version_buf) ||
      send_cluster_server(conn, nodetype_str) ||
      send_cluster_server(conn, nodeid_buf) ||
      send_cluster_server(conn, user_str) ||
      send_cluster_server(conn, password_str) ||
      send_cluster_server(conn, public_key_str) ||
      send_cluster_server(conn, endian_buf) ||
      send_cluster_server(conn, log_event_str) ||
      send_cluster_server(conn, ""))
    return conn->error_code;
  return 0;
}
static int
send_get_config(struct ic_connection *conn)
{
  char version_buf[32];

  g_snprintf(version_buf, 32, "%s%u", version_str, version_no);
  if (send_cluster_server(conn, get_config_str) ||
      send_cluster_server(conn, version_buf) ||
      send_cluster_server(conn, ""))
    return conn->error_code;
  return 0;
}

static int
rec_get_nodeid(struct ic_connection *conn,
               struct ic_api_cluster_server *apic,
               guint32 current_cluster_index)
{
  char *read_line_buf;
  guint32 read_size;
  int error;
  guint64 node_number;
  guint32 state= GET_NODEID_REPLY_STATE;
  while (!(error= rec_cs_read_line(apic, conn, &read_line_buf, &read_size)))
  {
     DEBUG(ic_print_buf(read_line_buf, read_size));
     switch (state)
     {
       case GET_NODEID_REPLY_STATE:
         if ((read_size != GET_NODEID_REPLY_LEN) ||
             (memcmp(read_line_buf, get_nodeid_reply_str,
                     GET_NODEID_REPLY_LEN) != 0))
         {
           DEBUG(printf("Protocol error in Get nodeid reply state\n"));
           return PROTOCOL_ERROR;
         }
         state= NODEID_STATE;
         break;
       case NODEID_STATE:
         if ((read_size <= NODEID_LEN) ||
             (memcmp(read_line_buf, nodeid_str, NODEID_LEN) != 0) ||
             (convert_str_to_int_fixed_size(read_line_buf + NODEID_LEN,
                                            read_size - NODEID_LEN,
                                            &node_number)) ||
             (node_number > MAX_NODE_ID))
         {
           DEBUG(printf("Protocol error in nodeid state\n"));
           return PROTOCOL_ERROR;
         }
         DEBUG(printf("Nodeid = %u\n", (guint32)node_number));
         apic->node_ids[current_cluster_index]= (guint32)node_number;
         state= RESULT_OK_STATE;
         break;
       case RESULT_OK_STATE:
         if ((read_size != RESULT_OK_LEN) ||
             (memcmp(read_line_buf, result_ok_str, RESULT_OK_LEN) != 0))
         {
           DEBUG(printf("Protocol error in result ok state\n"));
           return PROTOCOL_ERROR;
         }
         state= WAIT_EMPTY_RETURN_STATE;
         break;
       case WAIT_EMPTY_RETURN_STATE:
         if (read_size != 0)
         {
           DEBUG(printf("Protocol error in result ok state\n"));
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
  char *read_line_buf, *config_buf;
  guint32 read_size, config_size, rec_config_size;
  int error;
  guint64 content_length;
  guint32 state= GET_CONFIG_REPLY_STATE;
  while (!(error= rec_cs_read_line(apic, conn, &read_line_buf, &read_size)))
  {
    switch (state)
    {
      case GET_CONFIG_REPLY_STATE:
        if ((read_size != GET_CONFIG_REPLY_LEN) ||
            (memcmp(read_line_buf, get_config_reply_str,
                    GET_CONFIG_REPLY_LEN) != 0))
        {
          DEBUG(printf("Protocol error in get config reply state\n"));
          return PROTOCOL_ERROR;
        }
        state= RESULT_OK_STATE;
        break;
      case RESULT_OK_STATE:
        if ((read_size != RESULT_OK_LEN) ||
            (memcmp(read_line_buf, result_ok_str, RESULT_OK_LEN) != 0))
        {
          DEBUG(printf("Protocol error in result ok state\n"));
          return PROTOCOL_ERROR;
        }
        state= CONTENT_LENGTH_STATE;
        break;
      case CONTENT_LENGTH_STATE:
        if ((read_size <= CONTENT_LENGTH_LEN) ||
            (memcmp(read_line_buf, content_len_str,
                    CONTENT_LENGTH_LEN) != 0) ||
            convert_str_to_int_fixed_size(read_line_buf+CONTENT_LENGTH_LEN,
                                          read_size - CONTENT_LENGTH_LEN,
                                          &content_length) ||
            (content_length > MAX_CONTENT_LEN))
        {
          DEBUG(printf("Protocol error in content length state\n"));
          return PROTOCOL_ERROR;
        }
        DEBUG(printf("Content Length: %u\n", (guint32)content_length));
        state= OCTET_STREAM_STATE;
        break;
      case OCTET_STREAM_STATE:
        if ((read_size != OCTET_STREAM_LEN) ||
            (memcmp(read_line_buf, octet_stream_str,
                    OCTET_STREAM_LEN) != 0))
        {
          DEBUG(printf("Protocol error in octet stream state\n"));
          return PROTOCOL_ERROR;
        }
        state= CONTENT_ENCODING_STATE;
        break;
      case CONTENT_ENCODING_STATE:
        if ((read_size != CONTENT_ENCODING_LEN) ||
            (memcmp(read_line_buf, content_encoding_str,
                    CONTENT_ENCODING_LEN) != 0))
        {
          DEBUG(printf("Protocol error in content encoding state\n"));
          return PROTOCOL_ERROR;
        }
        state= WAIT_EMPTY_RETURN_STATE;
        break;
      case WAIT_EMPTY_RETURN_STATE:
        /*
          At this point we should now start receiving the configuration in
          base64 encoded format. It will arrive in 76 character chunks
          followed by a carriage return.
        */
        state= RECEIVE_CONFIG_STATE;
        if (!(config_buf= g_try_malloc0(content_length)))
          return MEM_ALLOC_ERROR;
        config_size= 0;
        rec_config_size= 0;
        /*
          Here we need to allocate receive buffer for configuration plus the
          place to put the encoded binary data.
        */
        break;
      case RECEIVE_CONFIG_STATE:
        memcpy(config_buf+config_size, read_line_buf, read_size);
        config_size+= read_size;
        rec_config_size+= (read_size + 1);
        if (rec_config_size >= content_length)
        {
          /*
            This is the last line, we have now received the config
            and are ready to translate it.
          */
          printf("Start decoding\n");
          error= translate_config(apic, current_cluster_index,
                                  config_buf, config_size);
          g_free(config_buf);
          return error;
        }
        else if (read_size != CONFIG_LINE_LEN)
        {
          g_free(config_buf);
          DEBUG(printf("Protocol error in config receive state\n"));
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
        struct ic_api_cluster_config *conf_obj= apic->conf_objects+i;
        if (conf_obj->node_ids)
          g_free(conf_obj->node_ids);
        if (conf_obj->node_types)
          g_free(conf_obj->node_types);
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
                                sizeof(struct ic_api_cluster_config))) ||
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
         num_cluster_servers * sizeof(struct ic_api_cluster_config));
  memset((char*)apic->cluster_conn.cluster_srv_conns, 0,
         num_cluster_servers * sizeof(struct ic_connection));

  apic->cluster_conn.num_cluster_servers= num_cluster_servers;
  apic->api_op.get_ic_config= get_cs_config;
  apic->api_op.free_ic_config= free_cs_config;
  return apic;
}
