#include <ic_apic.h>
#include <hashtable.h>
/*
  DESCRIPTION HOW TO ADD NEW CONFIGURATION VARIABLE:
  1) Add a new constant in this file e.g:
  #define KERNEL_SCHEDULER_NO_SEND_TIME 166
     This is the constant that will be used in the key-value pairs
     sent in the NDB Management Server protocol.

  2) Add a new entry in ic_init_config_parameters
     Check how other entries look like, the struct
     to fill in config_entry and is described in
     ic_apic.h

     Ensure that the hard-coded id used is unique, to ease the
     allocation of unique id's ensure that you put the id's in 
     increasing order in the file.

     Here one defines minimum value, maximum value, default values,
     value type and so forth.

  3) Add a new variable in the struct's for the various
     node types, for kernel variables the struct is
     ic_kernel_node_config (ic_apic.h). The name of this
     variable must be the same name as provided in the
     definition of the config variable in the method
     ic_init_config_parameters.

  ---------------------------------------------------------
  *********************************************************
  ---------------------------------------------------------

  Description of Management Server protocol
  -----------------------------------------

  We describe the protocol from a top-down view. Starting by describing what
  the high-level data structure is, describing how this can be discovered and
  then describing how parameters are described. Next step describing how the
  data is encoded. The final step is to describe the conversation between the
  client and the server in getting this encoded data.

  Each node in the configuration has a separate section. These sections are
  listed in section 1. Each section then contains all the configuration
  parameters of that node. After the last node section there is another
  section used to describe which communication sections that exists.
  In the example case below there are 4 nodes and thus section 6 is the
  section describing which communication sections that exist.
  There is one communication section for each pair of nodes that require
  communication. So in this case with 2 data nodes and 1 API node and 1
  management server there is one communication section to interconnect the
  data nodes and 2 communication sections for each data node to connect them
  with the API and the cluster server. Thus a total of 5 communication
  sections.

  All the values in the sections are sent as a more or less random list of
  key-value pairs. Each key-value pair contains section id, configuration
  id, value type and the value. This means that we require to pass through
  the list of key-value pairs one time in order to discover the number of
  nodes of various types to understand the allocation requirements. This
  further means that we require yet another pass to discover the number
  of communication sections since we cannot interpret Section 6 properly
  until we have discovered that this a descriptive section used to describe
  which communication sections exists in the configuration.

  The key-value pairs are sent as two 32-bit words for most data types, 64-bit
  words require another 32-bit word and character strings require also more
  words but they always add 32-bit words.

  The final step is that the all these 32-bit words are converted to network
  endian (big endian) format and the binary array of 32-bit words is then
  finally converted to base64-encoding. This encoding transfers the data
  over the wire using ASCII. base64-encoding sends its data with 76 ASCII
  characters per line followed by newline character. As part of the encoded
  data one also sends a verification string to ensure that the receiver can
  verify that the data uses the NDB Management Server protocol.

  Part of the protocol is that before transformation into base64 format there
  is a 8-byte verification string added at the beginning of the array of
  32-bit words and finally at the end a 4-byte checksum is added, this is
  calculated as the XOR of all words including the verification string and
  where checksum is 0 in the calculation.

  There also some special configuration id's.
  One is 16382 which describes the parent section which for all node sections
  is equal to 0.
  999 is a configuration id describing the node type.
  3 is the configuration id providing the node id.

  -------------------
  -                 -
  -  Section 1      -
  -                 -
  -------------------
    |  |  |  |
    |  |  |  |           Node Sections
    |  |  |  ----------> ------------------
    |  |  |              -  Section 2     -
    |  |  |              ------------------
    |  |  |
    |  |  -------------> ------------------
    |  |                 -  Section 3     -
    |  |                 ------------------
    |  |
    |  ----------------> ------------------
    |                    -  Section 4     -
    |                    ------------------
    |
    -------------------> ------------------
                         -  Section 5     -
                         ------------------

  -------------------
  -                 -
  -  Section 6      -
  -                 -
  -------------------
   | | | | |             Communication Sections
   | | | | ------------> ------------------
   | | | |               -  Section 7     -
   | | | |               ------------------
   | | | |
   | | | --------------> ------------------
   | | |                 -  Section 8     -
   | | |                 ------------------
   | | |
   | | ----------------> ------------------
   | |                   -  Section 9     -
   | |                   ------------------
   | |
   | ------------------> ------------------
   |                     -  Section 10    -
   |                     ------------------
   |
   --------------------> ------------------
                         -  Section 11    -
                         ------------------
    
  Description of protocol to get configuration profile from
  cluster server.

  1) Start with get_nodeid session. The client sends a number of
     lines of text with information about itself and login
     information:

     get nodeid<CR>
     nodeid: 0<CR>
     version: 327948<CR>
     nodetype: 1<CR>
     user: mysqld<CR>
     password: mysqld<CR>
     public key: a public key<CR>
     endian: little<CR>
     log_event: 0<CR>
     cluster_id: 0<CR> This is only used in iClaustron
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

     An error response is sent as:

     result: Error (Message)<CR>
     <CR>

  2) The next step is to get the configuration. To get the configuration
     the API sends the following messages:

     get config<CR>
     version: 327948<CR>
     <CR>

     In response to this the cluster server will send the configuration
     or an error response in the same manner as above.
     The configuration is sent as follows:

     get config reply<CR>
     result: Ok<CR>
     Content-Length: 3047<CR>
     ndbconfig/octet-stream<CR>
     Content-Transfer-Encoding: base64<CR>
     <CR>
     base64-encoded string with length as provided above

  General description of architecture of Cluster Servers
  ------------------------------------------------------

  The architecture of the Cluster Server is based on the following ideas.
  There will be four routines to read/write configurations.
  1) There will be one routine to read a configuration from a file.
  2) There will be one routine to read the configuration from a Cluster Server
     using the NDB Management Server Protocol.
  3) There will be one routine to write the data structures of a configuration
     into a file.
  4) There will be one routine to write the data structures of a configuration
     to another node using the NDB Management Server Protocol.

  Using these four routines it is possible to easily design a fault-tolerant
  Cluster Server, thus each time a change occurs one changes the data
  structures then one ensures that the configuration is locked (actually the
  first step) and starts changing the configuration in all servers. This
  requires sending the configuration to all servers, then each server updates
  its local copy on disk. Each time a Cluster Server starts up it will try to
  get in contact with the other Cluster Servers. They will check if the
  configuration has changed while the Cluster Server was down. If so it will
  replace the configuration from the existing servers.

  On top of this the idea is that the Cluster Server(s) will act as Cluster
  Server not only for one cluster but for many. When requesting configuration
  from a Cluster Server one can specify a number of clusters to fetch the
  configuration for. To handle this we require a new parameter in the NDB
  Management Server Protocol specifying the Cluster Id in a similar as the
  node id is specified.

  The routine to read configurations from a Cluster Server is
  get_cs_config
  This routine is part of the configuration client interface.
  The routine to read a configuration from a file is
  ic_load_config_server_from_files
  This routine makes use of the config file reader interface.

  Description of Cluster Server Data Structures
  ---------------------------------------------
  The configuration of a cluster is kept within the IC_CLUSTER_CONFIG
  struct. This struct contains arrays of all node structs, all communication
  struct's. There is also various variables keeping track of numbers of
  nodes of all types and number of communication sections and maximum node
  id. The arrays of node and communication struct's is actually a pointer to
  an array of pointers to the structs. There is also an array of the types
  of the nodes and the types of the communication types. It is thus necessary
  to read both the pointer array and the type array to know which struct to
  map the pointer of nodes and communications to.

  Finally all communication sections that exists in the configuration are
  in a hash table based on nodeid of both ends of the communication. If
  no communication object is found then the default communication parameters
  are used.

  During load of a configuration file from disk a special data structure,
  IC_CLUSTER_CONFIG_LOAD is used, this contains space for default config
  parameters and pointers to allocated memory for the configuration. It also
  contains space for some temporary variables used during the reading of the
  config file. The space of the IC_CLUSTER_CONFIG struct is part of this
  struct.

  Reading of the configuration file is performed using the module for config
  file readers. It's a generic module that reads a config file using sections,
  keys and comments. It contains an init call and an end call to clean up.

  Each configuration file represents one cluster. There is also a grid config
  file that represents the clusters that are part of the grid. This grid config
  file only contains references to the cluster config files.

  A cluster server accepts requests for configurations and sends the
  configuration to the requester. A request is always for a specific
  cluster. The cluster server does however serve a grid. Thus it can
  serve configurations for many different clusters.

  There will be a struct representing the grid and the most important
  thing this grid contains is pointers to an array of cluster configurations.
  The cluster server will also calculate the base64 representation of each
  cluster and store it in this struct. Thus when a request for a configuration
  arrives it isn't necessary to calculate this. This means of course that
  this base64 representation has to be recalculated on every change of the
  configuration of a cluster.

  For clients that access the cluster server to retrieve the configuration of
  one or more clusters the IC_API_CONFIG_SERVER is used to represent the
  cluster configurations retrieved from the cluster server. The most important
  part of this struct is a pointer to a set of structs of type
  IC_CLUSTER_CONFIG. There is also an interface to this implementation to
  make it replaceable by a separate implementation. The struct also contains
  an array of the cluster id's and the nodeid we're represented by in the
  specific cluster. There is also a set of temporary variables used when
  retrieving the config from the cluster server.
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
const gchar *node_id_str= "node_id";

#define MIN_PORT 0
#define MAX_PORT 65535

#define GET_NODEID_REQ_STATE 0
#define NODEID_REQ_STATE 1
#define VERSION_REQ_STATE 2
#define NODETYPE_REQ_STATE 3
#define USER_REQ_STATE 4
#define PASSWORD_REQ_STATE 5
#define PUBLIC_KEY_REQ_STATE 6
#define ENDIAN_REQ_STATE 7
#define LOG_EVENT_REQ_STATE 8
#define CLUSTER_ID_REQ_STATE 9

#define GET_CONFIG_REQ_STATE 0
#define EMPTY_STATE 1

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
#define WAIT_LAST_EMPTY_RETURN_STATE 10

#define GET_NODEID_LEN 10
#define GET_NODEID_REPLY_LEN 16
#define NODEID_LEN 8
#define VERSION_REQ_LEN 9
#define NODETYPE_REQ_LEN 10
#define USER_REQ_LEN 12
#define PASSWORD_REQ_LEN 16
#define PUBLIC_KEY_REQ_LEN 24
#define ENDIAN_REQ_LEN 8
#define LITTLE_ENDIAN_LEN 6
#define BIG_ENDIAN_LEN 3
#define LOG_EVENT_REQ_LEN 12
#define CLUSTER_ID_REQ_LEN 11
#define RESULT_OK_LEN 10

#define GET_CONFIG_LEN 10
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

#define MAX_MAP_CONFIG_ID 1024
#define MAX_CONFIG_ID 256

static guint32 glob_max_config_id;
IC_HASHTABLE *glob_conf_hash;
static gboolean glob_conf_entry_inited= FALSE;
static guint16 map_config_id_to_inx[MAX_MAP_CONFIG_ID];
static guint16 map_inx_to_config_id[MAX_CONFIG_ID];
static IC_CONFIG_ENTRY glob_conf_entry[MAX_CONFIG_ID];

/*
  Mandatory bits for kernel nodes, client nodes and so forth.
  Calculated once and then used for quick access to see if a
  configuration has provided all mandatory configuration items.
*/
static guint64 kernel_mandatory_bits;
static guint64 client_mandatory_bits;
static guint64 cluster_server_mandatory_bits;
static guint64 rep_server_mandatory_bits;
static guint64 sql_server_mandatory_bits;
static guint64 comm_mandatory_bits;

static const gchar *empty_string= "";
static const gchar *get_nodeid_str= "get nodeid";
static const gchar *get_nodeid_reply_str= "get nodeid reply";
static const gchar *get_config_str= "get config";
static const gchar *get_config_reply_str= "get config reply";
static const gchar *nodeid_str= "nodeid: ";
static const gchar *cluster_id_str= "clusterid: ";
static const gchar *version_str= "version: ";
static const gchar *nodetype_str= "nodetype: ";
static const gchar *user_str= "user: mysqld";
static const gchar *password_str= "password: mysqld";
static const gchar *public_key_str= "public key: a public key";
static const gchar *endian_str= "endian: ";
static const gchar *little_endian_str= "little";
static const gchar *big_endian_str= "big";
static const gchar *log_event_str= "log_event: 0";
static const gchar *result_ok_str= "result: Ok";
static const gchar *content_len_str= "Content-Length: ";
static const gchar *octet_stream_str= "Content-Type: ndbconfig/octet-stream";
static const gchar *content_encoding_str= "Content-Transfer-Encoding: base64";
/* Set version number Ndb = 6.3.1, iClaustron = 0.0.1 */
static const guint64 version_no= (guint64)0x60301 +
                                 (((guint64)0x000001) << 32);

/*
  CONFIGURATION PARAMETER MODULE
  ------------------------------

  This module contains all definitions of the configuration parameters.
  They are only used in this file, the rest of the code can only get hold
  of configuration objects that are filled in, they can also access methods
  that create protocol objects based on those configuration objects.
*/

#define DEF_CLUSTER_SERVER_PORT 1186
#define DEF_PORT 1187

static IC_SOCKET_LINK_CONFIG*
get_communication_object(IC_CLUSTER_CONFIG *clu_conf,
                         guint16 first_node_id, guint16 second_node_id)
{
  IC_SOCKET_LINK_CONFIG test1;
  test1.first_node_id= first_node_id;
  test1.second_node_id= second_node_id;
  return (IC_SOCKET_LINK_CONFIG*)ic_hashtable_search(clu_conf->comm_hash,
                                                     (void*)&test1);
}

static unsigned int
ic_hash_comms(void *ptr)
{
  IC_SOCKET_LINK_CONFIG *sock= (IC_SOCKET_LINK_CONFIG*)ptr;
  return sock->first_node_id ^ sock->second_node_id;
}

static int
ic_keys_equal_comms(void *ptr1, void *ptr2)
{
  IC_SOCKET_LINK_CONFIG *sock1= (IC_SOCKET_LINK_CONFIG*)ptr1;
  IC_SOCKET_LINK_CONFIG *sock2= (IC_SOCKET_LINK_CONFIG*)ptr2;
  if ((sock1->first_node_id == sock2->first_node_id &&
       sock1->second_node_id == sock2->second_node_id) ||
       (sock1->first_node_id == sock2->second_node_id &&
        sock2->second_node_id == sock2->first_node_id))
    return 1;
  return 0;
}

static int
build_hash_on_comms(IC_CLUSTER_CONFIG *clu_conf)
{
  IC_HASHTABLE *comm_hash;
  IC_SOCKET_LINK_CONFIG *comm_obj;
  guint32 i;
  DEBUG_ENTRY("build_hash_on_comms");

  if ((comm_hash= ic_create_hashtable(MAX_CONFIG_ID, ic_hash_comms,
                                      ic_keys_equal_comms)))
  {
    for (i= 0; i < clu_conf->num_comms; i++)
    {
      comm_obj= (IC_SOCKET_LINK_CONFIG*)clu_conf->comm_config[i];
      if (ic_hashtable_insert(comm_hash, (void*)comm_obj, (void*)comm_obj))
        goto error;
    }
    clu_conf->comm_hash= comm_hash;
    return 0;
  }
error:
  if (comm_hash)
    ic_hashtable_destroy(comm_hash);
  return 1;
}

void
ic_print_config_parameters(guint32 mask)
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 inx, i;
  if (!glob_conf_entry_inited)
    return;
  for (i= 0; i < MAX_MAP_CONFIG_ID; i++)
  {
    if ((inx= map_config_id_to_inx[i]))
    {
      conf_entry= &glob_conf_entry[inx];
      if (!(conf_entry->config_types & mask))
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
      if (conf_entry->config_types & (1 << IC_CLIENT_TYPE))
        printf("This config variable is used in a client node\n");
      if (conf_entry->config_types & (1 << IC_KERNEL_TYPE))
        printf("This config variable is used in a kernel node\n");
      if (conf_entry->config_types & (1 << IC_CLUSTER_SERVER_TYPE))
        printf("This config variable is used in a cluster server\n");
      if (conf_entry->config_types & (1 << IC_COMM_TYPE))
        printf("This config variable is used in connections\n");

      if (conf_entry->is_mandatory)
        printf("Entry is mandatory and has no default value\n");
      else if (conf_entry->is_string_type)
      {
        if (!conf_entry->is_mandatory)
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

static IC_CONFIG_ENTRY *get_config_entry_mandatory(guint32 bit_id,
                                                   IC_CONFIG_TYPES conf_type)
{
  guint32 i;
  IC_CONFIG_ENTRY *conf_entry;
  DEBUG_ENTRY("get_config_entry_mandatory");
  for (i= 1; i <= glob_max_config_id; i++)
  {
    conf_entry= &glob_conf_entry[i];
    if (conf_entry && conf_entry->is_mandatory &&
        (guint32)conf_entry->mandatory_bit == bit_id &&
        conf_entry->config_types & (1 << conf_type))
      return conf_entry;
  }
  return NULL;
}

static gboolean
is_entry_used_in_version(IC_CONFIG_ENTRY *conf_entry,
                         guint64 version_num)
{
  guint32 ic_version_num= (guint32)(version_num >> 32);
  guint32 ndb_version_num= (guint32)(version_num & 0xFFFFFFFF);
  /* Check Ndb Version */
  if ((ic_version_num == 0) && conf_entry->is_only_iclaustron)
    return FALSE;
  if (conf_entry->min_ndb_version_used > ndb_version_num ||
      (conf_entry->max_ndb_version_used &&
       conf_entry->max_ndb_version_used < ndb_version_num))
    return FALSE;
  /* Check iClaustron Version */
  if (conf_entry->min_ic_version_used > ic_version_num ||
      (conf_entry->max_ic_version_used &&
       conf_entry->max_ic_version_used < ic_version_num))
    return FALSE;
  return TRUE;
}

static void
calculate_mandatory_bits()
{
  guint32 i;
  IC_CONFIG_ENTRY *conf_entry;
  DEBUG_ENTRY("calculate_mandatory_bits");

  kernel_mandatory_bits= 0;
  client_mandatory_bits= 0;
  cluster_server_mandatory_bits= 0;
  rep_server_mandatory_bits= 0;
  sql_server_mandatory_bits= 0;
  comm_mandatory_bits= 0;

  for (i= 1; i <= glob_max_config_id; i++)
  {
    conf_entry= &glob_conf_entry[i];
    if (conf_entry->is_mandatory)
    {
      if (conf_entry->config_types & (1 << IC_KERNEL_TYPE))
        kernel_mandatory_bits|= (1 << conf_entry->mandatory_bit);
      if (conf_entry->config_types & (1 << IC_CLIENT_TYPE))
        client_mandatory_bits|= (1 << conf_entry->mandatory_bit);
      if ((conf_entry->config_types & (1 << IC_CLUSTER_SERVER_TYPE)) ||
          (conf_entry->config_types & (1 << IC_CLIENT_TYPE)))
        cluster_server_mandatory_bits|= (1 << conf_entry->mandatory_bit);
      if ((conf_entry->config_types & (1 << IC_REP_SERVER_TYPE)) ||
          (conf_entry->config_types & (1 << IC_CLIENT_TYPE)))
        rep_server_mandatory_bits|= (1 << conf_entry->mandatory_bit);
      if ((conf_entry->config_types & (1 << IC_SQL_SERVER_TYPE)) ||
          (conf_entry->config_types & (1 << IC_CLIENT_TYPE)))
        sql_server_mandatory_bits|= (1 << conf_entry->mandatory_bit);
      if (conf_entry->config_types & (1 << IC_COMM_TYPE))
        comm_mandatory_bits|= (1 << conf_entry->mandatory_bit);
    }
  }
  {
    DEBUG(gchar buf[128]);
    DEBUG(printf("kernel_mandatory_bits = %s\n",
                 ic_guint64_hex_str(kernel_mandatory_bits, buf)));
    DEBUG(printf("client_mandatory_bits = %s\n",
                 ic_guint64_hex_str(client_mandatory_bits, buf)));
    DEBUG(printf("cluster_server_mandatory_bits = %s\n",
                 ic_guint64_hex_str(cluster_server_mandatory_bits, buf)));
    DEBUG(printf("rep_server_mandatory_bits = %s\n",
                 ic_guint64_hex_str(rep_server_mandatory_bits, buf)));
    DEBUG(printf("sql_server_mandatory_bits = %s\n",
                 ic_guint64_hex_str(sql_server_mandatory_bits, buf)));
    DEBUG(printf("comm_mandatory_bits = %s\n",
                  ic_guint64_hex_str(comm_mandatory_bits, buf)));
  }
}

static int
build_config_name_hash()
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 i;
  for (i= 1; i <= glob_max_config_id; i++)
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
id_already_used_aborting(int id)
{
  printf("Id = %d is already used, aborting\n", id);
  abort();
}

static void
id_out_of_range(int id)
{
  printf("Id = % is out of range\n", id);
  abort();
}

static void
name_out_of_range(int id)
{
  printf("Name = % is out of range\n", id);
  abort();
}

/*
  This method defines all configuration parameters and puts them in a global
  variable only accessible from a few methods in this file.
*/

static void
init_config_parameters()
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 mandatory_bits= 0;
/*
  This is the kernel node configuration section.
*/
/* Id 0-9 for configuration id 0-9 */
/* Id 0 not used */
#define KERNEL_INJECT_FAULT 1
/* Id 2 not used */
#define IC_NODE_ID       3
/* Id 4 not used */
#define IC_NODE_HOST     5
/* Id 6 not used */
#define IC_NODE_DATA_PATH 7
/* Id 8-9 not used */

  IC_SET_CONFIG_MAP(KERNEL_INJECT_FAULT, 1);
  IC_SET_KERNEL_CONFIG(conf_entry, inject_fault,
                       IC_UINT32, 2, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 2);
  conf_entry->config_entry_description=
  "Inject faults (only available in special test builds)";

  /* These parameters are common for most node types */
  IC_SET_CONFIG_MAP(IC_NODE_ID, 3);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  IC_SET_KERNEL_CONFIG(conf_entry, node_id, IC_UINT32,
                       0, IC_NOT_CHANGEABLE);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_CLIENT_TYPE) +
                            (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Node id";

  IC_SET_CONFIG_MAP(IC_NODE_HOST, 5);
  IC_SET_KERNEL_STRING(conf_entry, hostname, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->default_string= (gchar*)empty_string;
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_CLIENT_TYPE) +
                            (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Hostname of the node";

  IC_SET_CONFIG_MAP(IC_NODE_DATA_PATH, 7);
  IC_SET_KERNEL_STRING(conf_entry, node_data_path, IC_INITIAL_NODE_RESTART);
  conf_entry->default_string= (gchar*)empty_string;
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_KERNEL_TYPE);
  conf_entry->config_entry_description=
  "Data directory of the node";

/* Id 10-19 for configuration id 10-99 */
/* Id 10-99 not used */

/* Id 20-29 for configuration id 100-109 */
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

  IC_SET_CONFIG_MAP(KERNEL_MAX_TRACE_FILES, 20);
  IC_SET_KERNEL_CONFIG(conf_entry, max_number_of_trace_files,
                       IC_UINT32, 25, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 2048);
  conf_entry->config_entry_description=
  "The number of crashes that can be reported before we overwrite error log and trace files";

  IC_SET_CONFIG_MAP(KERNEL_REPLICAS, 21);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_replicas,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 4);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "This defines number of nodes per node group, within a node group all nodes contain the same data";

  IC_SET_CONFIG_MAP(KERNEL_TABLE_OBJECTS, 22);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_table_objects,
                       IC_UINT32, 256, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of tables that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_COLUMN_OBJECTS, 23);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_column_objects,
                       IC_UINT32, 2048, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 256);
  conf_entry->config_entry_description=
  "Sets the maximum number of columns that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_KEY_OBJECTS, 24);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_key_objects,
                       IC_UINT32, 256, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(KERNEL_INTERNAL_TRIGGER_OBJECTS, 25);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_internal_trigger_objects,
                       IC_UINT32, 1536, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 512);
  conf_entry->config_entry_description=
  "Each unique index will use 3 internal trigger objects, index/backup will use 1 per table";

  IC_SET_CONFIG_MAP(KERNEL_CONNECTION_OBJECTS, 26);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_connection_objects,
                       IC_UINT32, 8192, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128);
  conf_entry->config_entry_description=
  "Each active transaction and active scan uses a connection object";

  IC_SET_CONFIG_MAP(KERNEL_OPERATION_OBJECTS, 27);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_operation_objects,
                       IC_UINT32, 32768, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024);
  conf_entry->config_entry_description=
  "Each record read/updated in a transaction uses an operation object during the transaction";

  IC_SET_CONFIG_MAP(KERNEL_SCAN_OBJECTS, 28);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_scan_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 32, 512);
  conf_entry->config_entry_description=
  "Each active scan uses a scan object for the lifetime of the scan operation";

  IC_SET_CONFIG_MAP(KERNEL_INTERNAL_TRIGGER_OPERATION_OBJECTS, 29);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_internal_trigger_operation_objects,
                       IC_UINT32, 4000, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4000, 4000);
  conf_entry->config_entry_description=
  "Each internal trigger that is fired uses an operation object for a short time";

/* Id 30-39 for configuration id 110-119 */
#define KERNEL_KEY_OPERATION_OBJECTS 110
#define KERNEL_CONNECTION_BUFFER 111
#define KERNEL_RAM_MEMORY 112
#define KERNEL_HASH_MEMORY 113
#define KERNEL_MEMORY_LOCKED 114
#define KERNEL_WAIT_PARTIAL_START 115
#define KERNEL_WAIT_PARTITIONED_START 116
#define KERNEL_WAIT_ERROR_START 117
#define KERNEL_HEARTBEAT_TIMER 118
#define KERNEL_CLIENT_HEARTBEAT_TIMER 119

  IC_SET_CONFIG_MAP(KERNEL_KEY_OPERATION_OBJECTS, 30);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_key_operation_objects,
                       IC_UINT32, 4096, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128);
  conf_entry->config_entry_description=
  "Each read and update of an unique hash index in a transaction uses one of those objects";

  IC_SET_CONFIG_MAP(KERNEL_CONNECTION_BUFFER, 31);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_connection_buffer,
                       IC_UINT32, 1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1024*1024, 1024*1024);
  conf_entry->config_entry_description=
  "Internal buffer used by connections by transactions and scans";

  IC_SET_CONFIG_MAP(KERNEL_RAM_MEMORY, 32);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_ram_memory,
                       IC_UINT64, 256*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 16*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory used to store RAM-based records";

  IC_SET_CONFIG_MAP(KERNEL_HASH_MEMORY, 33);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_hash_memory,
                       IC_UINT64, 64*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 8*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory used to store primary hash index on all tables and unique hash indexes";

  IC_SET_CONFIG_MAP(KERNEL_MEMORY_LOCKED, 34);
  IC_SET_KERNEL_BOOLEAN(conf_entry, use_unswappable_memory, FALSE,
                        IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Setting this to 1 means that all memory is locked and will not be swapped out";

  IC_SET_CONFIG_MAP(KERNEL_WAIT_PARTIAL_START, 35);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_wait_partial_start,
                       IC_UINT32, 20000, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting with a partial set of nodes, 0 waits forever";

  IC_SET_CONFIG_MAP(KERNEL_WAIT_PARTITIONED_START, 36);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_wait_partitioned_start,
                       IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting a potentially partitioned cluster, 0 waits forever";

  IC_SET_CONFIG_MAP(KERNEL_WAIT_ERROR_START, 37);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_wait_error_start,
                       IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before forcing a stop after an error, 0 waits forever";

  IC_SET_CONFIG_MAP(KERNEL_HEARTBEAT_TIMER, 38);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_heartbeat_kernel_nodes,
                       IC_UINT32, 700, IC_ROLLING_UPGRADE_CHANGE_SPECIAL);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to kernel nodes, 4 missed leads to node crash";

  IC_SET_CONFIG_MAP(KERNEL_CLIENT_HEARTBEAT_TIMER, 39);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_heartbeat_client_nodes,
                       IC_UINT32, 1000, IC_ROLLING_UPGRADE_CHANGE_SPECIAL);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to client nodes, 4 missed leads to node crash";

/* Id 40-49 for configuration id 120-129 */
#define KERNEL_LOCAL_CHECKPOINT_TIMER 120
#define KERNEL_GLOBAL_CHECKPOINT_TIMER 121
#define KERNEL_RESOLVE_TIMER 122
#define KERNEL_WATCHDOG_TIMER 123
#define KERNEL_DAEMON_RESTART_AT_ERROR 124
#define KERNEL_FILESYSTEM_PATH 125
#define KERNEL_REDO_LOG_FILES 126
/* 127 and 128 deprecated */
#define KERNEL_CHECK_INTERVAL 129

  IC_SET_CONFIG_MAP(KERNEL_LOCAL_CHECKPOINT_TIMER, 40);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_local_checkpoint,
                       IC_UINT32, 24, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 6, 31);
  conf_entry->config_entry_description=
  "Specifies how often local checkpoints are executed, logarithmic scale on log size";

  IC_SET_CONFIG_MAP(KERNEL_GLOBAL_CHECKPOINT_TIMER, 41);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_global_checkpoint,
                       IC_UINT32, 1000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between starting global checkpoints";

  IC_SET_CONFIG_MAP(KERNEL_RESOLVE_TIMER, 42);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_resolve,
                       IC_UINT32, 2000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms waiting for response from resolve";

  IC_SET_CONFIG_MAP(KERNEL_WATCHDOG_TIMER, 43);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_kernel_watchdog,
                       IC_UINT32, 6000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms without activity in kernel before watchdog is fired";

  IC_SET_CONFIG_MAP(KERNEL_DAEMON_RESTART_AT_ERROR, 44);
  IC_SET_KERNEL_BOOLEAN(conf_entry, kernel_automatic_restart, TRUE,
                        IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "If set, kernel restarts automatically after a failure";

  IC_SET_CONFIG_MAP(KERNEL_FILESYSTEM_PATH, 45);
  IC_SET_KERNEL_STRING(conf_entry, filesystem_path, IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Path to filesystem of kernel";

  IC_SET_CONFIG_MAP(KERNEL_REDO_LOG_FILES, 46);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_redo_log_files,
                       IC_UINT32, 32, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN(conf_entry, 4);
  conf_entry->config_entry_description=
  "Number of REDO log files, each file represents 64 MB log space";

  IC_SET_CONFIG_MAP(127, 47);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(128, 48);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(KERNEL_CHECK_INTERVAL, 49);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_check_interval,
                       IC_UINT32, 500, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 500, 500);
  conf_entry->config_entry_description=
  "Time in ms between checks after transaction timeouts";

/* Id 50-59 for configuration id 130-139 */
#define KERNEL_CLIENT_ACTIVITY_TIMER 130
#define KERNEL_DEADLOCK_TIMER 131
#define KERNEL_CHECKPOINT_OBJECTS 132
#define KERNEL_CHECKPOINT_MEMORY 133
#define KERNEL_CHECKPOINT_DATA_MEMORY 134
#define KERNEL_CHECKPOINT_LOG_MEMORY 135
#define KERNEL_CHECKPOINT_WRITE_SIZE 136
/* 137 and 138 deprecated */
#define KERNEL_CHECKPOINT_MAX_WRITE_SIZE 139

  IC_SET_CONFIG_MAP(KERNEL_CLIENT_ACTIVITY_TIMER, 50);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_client_activity,
                       IC_UINT32, 1024*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to client inactivity";

  IC_SET_CONFIG_MAP(KERNEL_DEADLOCK_TIMER, 51);
  IC_SET_KERNEL_CONFIG(conf_entry, timer_deadlock,
                       IC_UINT32, 2000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to internal wait (indication of deadlock)";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_OBJECTS, 52);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_checkpoint_objects,
                       IC_UINT32, 1, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 1);
  conf_entry->config_entry_description=
  "Number of possible parallel backups and local checkpoints";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_MEMORY, 53);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_memory,
                       IC_UINT32, 4*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4*1024*1024, 4*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_DATA_MEMORY, 54);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_data_memory,
                       IC_UINT32, 2*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 2*1024*1024, 2*1024*1024);
  conf_entry->config_entry_description=
  "Size of data memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_LOG_MEMORY, 55);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_log_memory,
                       IC_UINT32, 2*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 2*1024*1024, 2*1024*1024);
  conf_entry->config_entry_description=
  "Size of log memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_WRITE_SIZE, 56);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_write_size,
                       IC_UINT32, 64*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 64*1024);
  conf_entry->config_entry_description=
  "Size of default writes in local checkpoint and backups";

  IC_SET_CONFIG_MAP(137, 57);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(138, 58);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_MAX_WRITE_SIZE, 59);
  IC_SET_KERNEL_CONFIG(conf_entry, checkpoint_max_write_size,
                       IC_UINT32, 256*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 256*1024, 256*1024);
  conf_entry->config_entry_description=
  "Size of maximum writes in local checkpoint and backups";

/* Id 60-69 for configuration id 140-149 */
#define KERNEL_SIZE_OF_REDO_LOG_FILES 140
#define KERNEL_INITIAL_WATCHDOG_TIMER 141
/* 142 - 146 not used */
/* 147 Cluster Server parameter */
#define CLUSTER_SERVER_EVENT_LOG 147
#define KERNEL_VOLATILE_MODE 148
#define KERNEL_ORDERED_KEY_OBJECTS 149

  IC_SET_CONFIG_MAP(KERNEL_SIZE_OF_REDO_LOG_FILES, 60);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_redo_log_files,
                       IC_UINT32, 16 * 1024 * 1024, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4*1024*1024, 2000*1024*1024);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Size of REDO log files";

  IC_SET_CONFIG_MAP(KERNEL_INITIAL_WATCHDOG_TIMER, 61);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_initial_watchdog_timer,
                       IC_UINT32, 15000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 100);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Initial value of watchdog timer before communication set-up";

  /* This is a cluster server parameter */
  IC_SET_CONFIG_MAP(CLUSTER_SERVER_EVENT_LOG, 67);
  IC_SET_CLUSTER_SERVER_STRING(conf_entry, cluster_server_event_log,
                               empty_string, IC_INITIAL_NODE_RESTART);
  conf_entry->is_not_sent= TRUE;
  conf_entry->config_entry_description=
  "Type of cluster event log";
  
  IC_SET_CONFIG_MAP(KERNEL_VOLATILE_MODE, 68);
  IC_SET_KERNEL_BOOLEAN(conf_entry, kernel_volatile_mode, FALSE,
                        IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "In this mode all file writes are ignored and all starts becomes initial starts";


  IC_SET_CONFIG_MAP(KERNEL_ORDERED_KEY_OBJECTS, 69);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_ordered_key_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of ordered keys that can be stored in cluster";

/* Id 70-79 for configuration id 150-159 */
#define KERNEL_UNIQUE_HASH_KEY_OBJECTS 150
/* 151 and 152 deprecated */
#define KERNEL_SCAN_BATCH_SIZE 153
/* 154 and 155 deprecated */
#define KERNEL_REDO_LOG_MEMORY 156
#define KERNEL_LONG_MESSAGE_MEMORY 157
#define KERNEL_CHECKPOINT_PATH 158
#define KERNEL_MAX_OPEN_FILES 159

  IC_SET_CONFIG_MAP(KERNEL_UNIQUE_HASH_KEY_OBJECTS, 70);
  IC_SET_KERNEL_CONFIG(conf_entry, number_of_unique_hash_key_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of unique hash keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(151, 71);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(152, 72);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(KERNEL_SCAN_BATCH_SIZE, 73);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_scan_batch,
                       IC_UINT32, 64, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64, 64);
  conf_entry->config_entry_description=
  "Number of records sent in a scan from the local kernel node";

  IC_SET_CONFIG_MAP(154, 74);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(155, 75);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(KERNEL_REDO_LOG_MEMORY, 76);
  IC_SET_KERNEL_CONFIG(conf_entry, redo_log_memory,
                       IC_UINT32, 16*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Size of REDO log memory buffer";

  IC_SET_CONFIG_MAP(KERNEL_LONG_MESSAGE_MEMORY, 77);
  IC_SET_KERNEL_CONFIG(conf_entry, long_message_memory,
                       IC_UINT32, 1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1024*1024, 1024*1024);
  conf_entry->config_entry_description=
  "Size of long memory buffers";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_PATH, 78);
  IC_SET_KERNEL_STRING(conf_entry, kernel_checkpoint_path, IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Path to filesystem of checkpoints";

  IC_SET_CONFIG_MAP(KERNEL_MAX_OPEN_FILES, 79);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_max_open_files,
                       IC_UINT32, 40, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 40, 40);
  conf_entry->config_entry_description=
  "Maximum number of open files in kernel node";

/* Id 80-89 for configuration id 160-169 */
#define KERNEL_PAGE_CACHE_SIZE 160
#define KERNEL_STRING_MEMORY 161
#define KERNEL_INITIAL_OPEN_FILES 162
#define KERNEL_FILE_SYNCH_SIZE 163
#define KERNEL_DISK_WRITE_SPEED 164
#define KERNEL_DISK_WRITE_SPEED_START 165
#define KERNEL_REPORT_MEMORY_FREQUENCY 166
#define KERNEL_BACKUP_STATUS_FREQUENCY 167
#define KERNEL_USE_O_DIRECT 168
#define KERNEL_MAX_ALLOCATE_SIZE 169

  IC_SET_CONFIG_MAP(KERNEL_PAGE_CACHE_SIZE, 80);
  IC_SET_KERNEL_CONFIG(conf_entry, page_cache_size,
                       IC_UINT64, 128*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 64*1024);
  conf_entry->config_entry_description=
  "Size of page cache for disk-based data";

  IC_SET_CONFIG_MAP(KERNEL_STRING_MEMORY, 81);
  IC_SET_KERNEL_CONFIG(conf_entry, size_of_string_memory,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Size of string memory";

  IC_SET_CONFIG_MAP(KERNEL_INITIAL_OPEN_FILES, 82);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_open_files,
                       IC_UINT32, 27, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 27, 27);
  conf_entry->config_entry_description=
  "Number of open file handles in kernel node from start";

  IC_SET_CONFIG_MAP(KERNEL_FILE_SYNCH_SIZE, 83);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_file_synch_size,
                       IC_UINT32, 4*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Size of file writes before a synch is always used";

  IC_SET_CONFIG_MAP(KERNEL_DISK_WRITE_SPEED, 84);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_disk_write_speed,
                       IC_UINT32, 8*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 64*1024);
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk";

  IC_SET_CONFIG_MAP(KERNEL_DISK_WRITE_SPEED_START, 85);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_disk_write_speed_start,
                       IC_UINT32, 256*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk during start of the node";

  IC_SET_CONFIG_MAP(KERNEL_REPORT_MEMORY_FREQUENCY, 86)
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_report_memory_frequency,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "Frequency of memory reports, 0 means only at certain thresholds";

  IC_SET_CONFIG_MAP(KERNEL_BACKUP_STATUS_FREQUENCY, 87)
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_backup_status_frequency,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "Frequency of backup status, 0 means no status reporting except at end";

  IC_SET_CONFIG_MAP(KERNEL_USE_O_DIRECT, 88);
  IC_SET_KERNEL_BOOLEAN(conf_entry, use_o_direct, TRUE,
                        IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Use O_DIRECT on file system of kernel nodes";

  IC_SET_CONFIG_MAP(KERNEL_MAX_ALLOCATE_SIZE, 89);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_max_allocate_size,
                       IC_UINT32, 32*1024*1024, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1*1024*1024, 1000*1024*1024);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Size of maximum extent allocated at a time for table memory";

/* Id 90-99 for configuration id 170-179 */
#define KERNEL_GROUP_COMMIT_DELAY 170
#define KERNEL_RT_SCHEDULER_THREADS 173
#define KERNEL_LOCK_MAIN_THREAD 174
#define KERNEL_LOCK_OTHER_THREADS 175
#define KERNEL_SCHEDULER_NO_SEND_TIME 176
#define KERNEL_SCHEDULER_NO_SLEEP_TIME 177
/* 171-172 not used */
/* 178-179 not used */

  IC_SET_CONFIG_MAP(KERNEL_GROUP_COMMIT_DELAY, 90);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_group_commit_delay,
                       IC_UINT32, 100, IC_ONLINE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60301;
  conf_entry->config_entry_description=
  "How long is the delay between each group commit started by the cluster";

  IC_SET_CONFIG_MAP(KERNEL_RT_SCHEDULER_THREADS, 93);
  IC_SET_KERNEL_BOOLEAN(conf_entry, kernel_rt_scheduler_threads,
                       FALSE, IC_ONLINE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "If set the kernel is setting its thread in RT priority, requires root privileges";

  IC_SET_CONFIG_MAP(KERNEL_LOCK_MAIN_THREAD, 94);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_lock_main_thread,
                       IC_UINT32, 65535, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 65535);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "Lock Main Thread to a CPU id";

  IC_SET_CONFIG_MAP(KERNEL_LOCK_OTHER_THREADS, 95);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_lock_main_thread,
                       IC_UINT32, 65535, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 65535);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "Lock other threads to a CPU id";

  IC_SET_CONFIG_MAP(KERNEL_SCHEDULER_NO_SEND_TIME, 96);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_scheduler_no_send_time,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without sending socket buffers";

  IC_SET_CONFIG_MAP(KERNEL_SCHEDULER_NO_SLEEP_TIME, 97);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_scheduler_no_sleep_time,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without going to sleep";

/* Id 100-109 for configuration id 180-189 */
/* 180-189 not used */

/* Id 110-119 for configuration id 190-199 */
/* 190-197 not used */
#define KERNEL_MEMORY_POOL 198
/* 199 not used */

  IC_SET_CONFIG_MAP(KERNEL_MEMORY_POOL, 118);
  IC_SET_KERNEL_CONFIG(conf_entry, kernel_memory_pool,
                       IC_UINT64, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Size of memory pool for internal memory usage";

/* Client/Cluster Server configuration items */
/* Id 120-129 for configuration id 200-209 */
#define CLIENT_RESOLVE_RANK 200
#define CLIENT_RESOLVE_TIMER 201

  IC_SET_CONFIG_MAP(CLIENT_RESOLVE_RANK, 120);
  IC_SET_CLIENT_CONFIG(conf_entry, client_resolve_rank,
                       IC_UINT32, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 2);
  conf_entry->config_types= (1 << IC_CLIENT_TYPE) + (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Rank in resolving network partition of the client";

  IC_SET_CONFIG_MAP(CLIENT_RESOLVE_TIMER, 121);
  IC_SET_CLIENT_CONFIG(conf_entry, client_resolve_timer,
                       IC_UINT32, 0, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->config_types= (1 << IC_CLIENT_TYPE) + (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Time in ms waiting for resolve before crashing";

/* Id 130-139 for configuration id 210-249 */
/* Id 210-249 not used */

/* Log level configuration items */
/* Id 140-149 for configuration id 250-259 */
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

  IC_SET_CONFIG_MAP(KERNEL_START_LOG_LEVEL, 140);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_start,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at start of a node";

  IC_SET_CONFIG_MAP(KERNEL_STOP_LOG_LEVEL, 141);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_stop,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at stop of a node";

  IC_SET_CONFIG_MAP(KERNEL_STAT_LOG_LEVEL, 142);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_statistics,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of statistics on a node";

  IC_SET_CONFIG_MAP(KERNEL_CHECKPOINT_LOG_LEVEL, 143);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_checkpoint,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at checkpoint of a node";

  IC_SET_CONFIG_MAP(KERNEL_RESTART_LOG_LEVEL, 144);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_restart,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at restart of a node";

  IC_SET_CONFIG_MAP(KERNEL_CONNECTION_LOG_LEVEL, 145);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_connection,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of connections to a node";

  IC_SET_CONFIG_MAP(KERNEL_REPORT_LOG_LEVEL, 146);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_reports,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of reports from a node";

  IC_SET_CONFIG_MAP(KERNEL_WARNING_LOG_LEVEL, 147);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_warning,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of warnings from a node";

  IC_SET_CONFIG_MAP(KERNEL_ERROR_LOG_LEVEL, 148);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_error,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of errors from a node";

  IC_SET_CONFIG_MAP(KERNEL_CONGESTION_LOG_LEVEL, 149);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_congestion,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of congestions to a node";

/* Id 150-159 for configuration id 260-269 */
#define KERNEL_DEBUG_LOG_LEVEL 260
#define KERNEL_BACKUP_LOG_LEVEL 261
/* Id 262-269 not used */

  IC_SET_CONFIG_MAP(KERNEL_DEBUG_LOG_LEVEL, 150);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_debug,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of debug messages from a node";

  IC_SET_CONFIG_MAP(KERNEL_BACKUP_LOG_LEVEL, 151);
  IC_SET_KERNEL_CONFIG(conf_entry, log_level_backup,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of backups at a node";

/* Id 160-169 for configuration id 270-299 */
/* Id 270-299 not used */

/* Id 170-174 for configuration id 300-304 */
/* This is the cluster server configuration section  */
#define CLUSTER_SERVER_PORT_NUMBER 300
/* Id 301-304 not used */

  IC_SET_CONFIG_MAP(CLUSTER_SERVER_PORT_NUMBER, 170);
  IC_SET_CLUSTER_SERVER_CONFIG(conf_entry, cluster_server_port_number, IC_UINT32,
                               DEF_CLUSTER_SERVER_PORT,
                               IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  conf_entry->config_entry_description=
  "Port number of Cluster Server";

/* Id 175-179 for configuration id 305-399 */
/* Id 305-399 not used */

/* This is the Socket configuration section*/
/* Id 180-189 for configuration id 400-409 */
#define SOCKET_FIRST_NODE_ID 400
#define SOCKET_SECOND_NODE_ID 401
#define SOCKET_USE_MESSAGE_ID 402
#define SOCKET_USE_CHECKSUM 403
/* Id 404, 405 not used */
#define SOCKET_SERVER_PORT_NUMBER 406
#define SOCKET_FIRST_HOSTNAME 407
#define SOCKET_SECOND_HOSTNAME 408
#define SOCKET_GROUP 409

  IC_SET_CONFIG_MAP(SOCKET_FIRST_NODE_ID, 180);
  IC_SET_SOCKET_CONFIG(conf_entry, first_node_id,
                    IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "First node id of the connection";

  IC_SET_CONFIG_MAP(SOCKET_SECOND_NODE_ID, 181);
  IC_SET_SOCKET_CONFIG(conf_entry, second_node_id,
                    IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Second node id of the connection";

  IC_SET_CONFIG_MAP(SOCKET_USE_MESSAGE_ID, 182);
  IC_SET_SOCKET_BOOLEAN(conf_entry, use_message_id,
                     FALSE, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Using message id can be a valuable resource to find problems related to distributed execution";

  IC_SET_CONFIG_MAP(SOCKET_USE_CHECKSUM, 183);
  IC_SET_SOCKET_BOOLEAN(conf_entry, use_checksum,
                     FALSE, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Using checksum ensures that internal bugs doesn't corrupt data while data is placed in buffers";

  IC_SET_CONFIG_MAP(SOCKET_SERVER_PORT_NUMBER, 186);
  IC_SET_SOCKET_CONFIG(conf_entry, server_port_number,
                    IC_UINT16, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Port number to use on server side";

  IC_SET_CONFIG_MAP(SOCKET_FIRST_HOSTNAME, 187);
  IC_SET_SOCKET_STRING(conf_entry, first_hostname, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Hostname of first node";

  IC_SET_CONFIG_MAP(SOCKET_SECOND_HOSTNAME, 188);
  IC_SET_SOCKET_STRING(conf_entry, second_hostname, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Hostname of second node";

  IC_SET_CONFIG_MAP(SOCKET_GROUP, 189);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_group,
                    IC_UINT32, 55, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 55, 55);
  conf_entry->config_entry_description=
  "Group id of the connection";

/* Id 190-209 for configuration id 410-499 */
#define SOCKET_SERVER_NODE_ID 410
/* Id 411-419 not used */
#define SOCKET_CLIENT_PORT_NUMBER 420
/* Id 421-453 not used */
#define SOCKET_WRITE_BUFFER_SIZE 454
#define SOCKET_READ_BUFFER_SIZE 455
/* Id 456 not used */
#define SOCKET_KERNEL_READ_BUFFER_SIZE 457
#define SOCKET_KERNEL_WRITE_BUFFER_SIZE 458
#define SOCKET_MAXSEG_SIZE 459
#define SOCKET_BIND_ADDRESS 460
/* Id 461-499 not used */

  IC_SET_CONFIG_MAP(SOCKET_SERVER_NODE_ID, 190);
  IC_SET_SOCKET_CONFIG(conf_entry, server_node_id,
                    IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, MAX_NODE_ID);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Node id of node that is server part of connection";

  IC_SET_CONFIG_MAP(SOCKET_CLIENT_PORT_NUMBER, 193);
  IC_SET_SOCKET_CONFIG(conf_entry, client_port_number,
                    IC_UINT16, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->max_ndb_version_used= 1;
  conf_entry->is_only_iclaustron= TRUE;
  conf_entry->config_entry_description=
  "Port number to use on client side";

  IC_SET_CONFIG_MAP(SOCKET_WRITE_BUFFER_SIZE, 194);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_write_buffer_size,
                    IC_UINT32, 256*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128*1024);
  conf_entry->config_entry_description=
  "Size of write buffer in front of socket";

  IC_SET_CONFIG_MAP(SOCKET_READ_BUFFER_SIZE, 195);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_read_buffer_size,
                    IC_UINT32, 256*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 64*1024);
  conf_entry->config_entry_description=
  "Size of read buffer in front of socket";

  IC_SET_CONFIG_MAP(SOCKET_KERNEL_READ_BUFFER_SIZE, 197);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_kernel_read_buffer_size,
                    IC_UINT32, 128*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 256*1024);
  conf_entry->config_entry_description=
  "Size of receive buffer for socket in OS kernel";

  IC_SET_CONFIG_MAP(SOCKET_KERNEL_WRITE_BUFFER_SIZE, 198);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_kernel_write_buffer_size,
                    IC_UINT32, 128*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 256*1024*1024);
  conf_entry->config_entry_description=
  "Size of send buffer of socket inside the OS kernel";

  IC_SET_CONFIG_MAP(SOCKET_MAXSEG_SIZE, 199);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_maxseg_size,
                    IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MAX(conf_entry, 128*1024*1024);
  conf_entry->config_entry_description=
  "TCP_MAXSEG on socket";

  IC_SET_CONFIG_MAP(SOCKET_BIND_ADDRESS, 200);
  IC_SET_SOCKET_BOOLEAN(conf_entry, socket_bind_address, FALSE,
                        IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Bind to IP address of server";

/* Id 210-219 for configuration id 500-799 */
/* Id 500-799 not used */

/* Id 220-229 for configuration id 800-809 */
/* This is the client configuration section*/
#define CLIENT_MAX_BATCH_BYTE_SIZE 800
#define CLIENT_BATCH_BYTE_SIZE 801
#define CLIENT_BATCH_SIZE 802
/* Id 803-809 not used */

  IC_SET_CONFIG_MAP(CLIENT_MAX_BATCH_BYTE_SIZE, 220);
  IC_SET_CLIENT_CONFIG(conf_entry, client_max_batch_byte_size,
                       IC_UINT32, 256*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 32*1024, 4*1024*1024);
  conf_entry->config_entry_description=
  "Size in bytes of max of the sum of the batches in a scan operations";

  IC_SET_CONFIG_MAP(CLIENT_BATCH_BYTE_SIZE, 221);
  IC_SET_CLIENT_CONFIG(conf_entry, client_batch_byte_size,
                       IC_UINT32, 8192, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 128, 65536);
  conf_entry->config_entry_description=
  "Size in bytes of batches in scan operations";

  IC_SET_CONFIG_MAP(CLIENT_BATCH_SIZE, 222);
  IC_SET_CLIENT_CONFIG(conf_entry, client_batch_size,
                       IC_UINT32, 64, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 992);
  conf_entry->config_entry_description=
  "Size in number of records of batches in scan operations";

/* Id 230-234 for configuration id 810-899 */
/* Id 810-899 not used */

/* Id 235-239 for configuration id 900-999 */
/* Id 900-996 not used */
#define IC_PORT_NUMBER 997
/* Id 998 not used */
#define IC_NODE_TYPE     999
/* Node type is stored in separate array and is handled in a special manner */

  /* Port number is used both by clients and kernels */
  IC_SET_CONFIG_MAP(IC_PORT_NUMBER, 235);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  IC_SET_KERNEL_CONFIG(conf_entry, port_number, IC_UINT32,
                       DEF_PORT, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_types= (1 << IC_CLIENT_TYPE) +
                            (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_KERNEL_TYPE);
  conf_entry->is_not_sent= TRUE;
  conf_entry->config_entry_description=
  "Port number";

/* Id 240-255 for configuration id 1000-16383 */
/* 1000-16381 not used */
#define IC_PARENT_ID     16382
/* 168383 not used */

  /* Parameters common for all node types */
}

static int
ic_init_config_parameters()
{
  DEBUG_ENTRY("ic_init_config_parameters");

  if (glob_conf_entry_inited)
    return 0;
  if (!(glob_conf_hash= ic_create_hashtable(MAX_CONFIG_ID, ic_hash_str,
                                            ic_keys_equal_str)))
    return 1;
  glob_conf_entry_inited= TRUE;
  glob_max_config_id= 0;
  memset(map_config_id_to_inx, 0, MAX_MAP_CONFIG_ID * sizeof(guint16));
  memset(glob_conf_entry, 0, MAX_CONFIG_ID * sizeof(IC_CONFIG_ENTRY));
  init_config_parameters();
  calculate_mandatory_bits();
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
  inx= map_config_id_to_inx[config_id];
  if (!inx)
  {
    return NULL;
  }
  return &glob_conf_entry[inx];
}

static int
ic_assign_config_value(int config_id, guint64 value,
                       IC_CONFIG_TYPES conf_type,
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
  if (!(conf_entry->config_types & (1 << conf_type)) ||
      (conf_entry->is_boolean && (value > 1)) ||
      (conf_entry->is_min_value_defined &&
       (conf_entry->min_value > value)) ||
      (conf_entry->is_max_value_defined &&
       (conf_entry->max_value < value)))
  {
    printf("Config value error config_id = %d. ", config_id);
    if (conf_entry->is_min_value_defined && value < conf_entry->min_value)
      printf("Value too small\n");
    else if (conf_entry->is_max_value_defined && value > conf_entry->max_value)
      printf("Value too large\n");
    else if (conf_entry->is_boolean && (value > 1))
      printf("Erroneus bool value\n");
    else
      printf("Error with node type, config_types = %u, conf_type %u\n",
             conf_entry->config_types, conf_type);
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
ic_assign_config_string(int config_id, IC_CONFIG_TYPES conf_type,
                        gchar *string_memory,
                        guint32 string_len,
                        gchar *struct_ptr,
                        gchar **string_val)
{
  IC_CONFIG_ENTRY *conf_entry= get_config_entry(config_id);

  if (!conf_entry)
    return PROTOCOL_ERROR;
  if (!conf_entry->is_string_type ||
      !(conf_entry->config_types & (1 << conf_type)))
  {
    DEBUG(printf("conf_type = %u, config_types = %u, config_id = %u\n",
           conf_type, conf_entry->config_types, config_id));
    DEBUG(printf("Debug string inconsistency\n"));
    return PROTOCOL_ERROR;
  }
  struct_ptr+= conf_entry->offset;
  *(gchar**)struct_ptr= string_memory;
  strncpy(string_memory, *string_val, string_len);
  printf("String value = %s\n", string_memory);
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
init_config_object(gchar *conf_object, IC_CONFIG_TYPES config_type)
{
  guint32 i;
  for (i= 1; i <= glob_max_config_id; i++)
  {
    IC_CONFIG_ENTRY *conf_entry= &glob_conf_entry[i];
    if (conf_entry && conf_entry->config_types & (1 << config_type))
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
  conf_obj->node_types= (IC_NODE_TYPES*)ic_calloc(conf_obj->num_nodes *
                                                  sizeof(IC_NODE_TYPES));
  conf_obj->comm_types= (IC_COMMUNICATION_TYPE*)
         ic_calloc(conf_obj->num_nodes * sizeof(IC_COMMUNICATION_TYPE));
  conf_obj->node_ids= (guint32*)ic_calloc(conf_obj->num_nodes *
                                          sizeof(guint32));
  conf_obj->node_config= (gchar**)ic_calloc(conf_obj->num_nodes *
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
  for (i= 0; i < conf_obj->num_nodes; i++)
  {
    switch (conf_obj->node_types[i])
    {
      case IC_KERNEL_NODE:
        size_config_objects+= sizeof(IC_KERNEL_CONFIG);
        break;
      case IC_CLIENT_NODE:
        size_config_objects+= sizeof(IC_CLIENT_CONFIG);
        break;
      case IC_CLUSTER_SERVER_NODE:
        size_config_objects+= sizeof(IC_CLUSTER_SERVER_CONFIG);
        break;
      case IC_SQL_SERVER_NODE:
        size_config_objects+= sizeof(IC_SQL_SERVER_CONFIG);
        break;
      case IC_REP_SERVER_NODE:
        size_config_objects+= sizeof(IC_REP_SERVER_CONFIG);
        break;
      default:
        g_assert(FALSE);
        break;
    }
  }
  size_config_objects+= conf_obj->num_comms *
                        sizeof(struct ic_comm_link_config);
  if (!(conf_obj->comm_config= (gchar**)ic_calloc(
                     conf_obj->num_comms * sizeof(gchar*))) ||
      !(apic->string_memory_to_return= ic_calloc(
                     apic->string_memory_size)) ||
      !(apic->config_memory_to_return= ic_calloc(
                     size_config_objects)))
    return MEM_ALLOC_ERROR;

  conf_obj_ptr= apic->config_memory_to_return;
  string_mem= apic->string_memory_to_return;
  apic->end_string_memory= string_mem + apic->string_memory_size;
  apic->next_string_memory= string_mem;

  for (i= 0; i < conf_obj->num_nodes; i++)
  {
    conf_obj->node_config[i]= conf_obj_ptr;
    init_config_object(conf_obj_ptr, conf_obj->node_types[i]);
    switch (conf_obj->node_types[i])
    {
      case IC_KERNEL_NODE:
        conf_obj_ptr+= sizeof(IC_KERNEL_CONFIG);
        break;
      case IC_CLIENT_NODE:
        conf_obj_ptr+= sizeof(IC_CLIENT_CONFIG);
        break;
      case IC_CLUSTER_SERVER_NODE:
        conf_obj_ptr+= sizeof(IC_CLUSTER_SERVER_CONFIG);
        break;
      case IC_SQL_SERVER_NODE:
        conf_obj_ptr+= sizeof(IC_SQL_SERVER_CONFIG);
        break;
      case IC_REP_SERVER_NODE:
        conf_obj_ptr+= sizeof(IC_REP_SERVER_CONFIG);
        break;
      default:
        g_assert(FALSE);
        break;
    }
  }
  for (i= 0; i < conf_obj->num_comms; i++)
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
  guint32 val= (guint64)**key_value;
  val= g_ntohl(val);
  guint64 long_val= value + (((guint64)val) << 32);
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
      case IC_KERNEL_NODE:
        conf_obj->num_data_servers++; break;
      case IC_CLIENT_NODE:
        conf_obj->num_clients++; break;
      case IC_CLUSTER_SERVER_NODE:
        conf_obj->num_cluster_servers++; break;
      case IC_SQL_SERVER_NODE:
        conf_obj->num_sql_servers++; break;
      case IC_REP_SERVER_NODE:
        conf_obj->num_rep_servers++; break;
      default:
        printf("No such node type\n");
        return PROTOCOL_ERROR;
    }
    conf_obj->node_types[sect_id - 2]= (IC_NODE_TYPES)value;
  }
  else if (hash_key == IC_NODE_ID)
  {
    conf_obj->node_ids[sect_id - 2]= value;
    conf_obj->max_node_id= MAX(value, conf_obj->max_node_id);
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
    id= map_config_id_to_inx[hash_key];
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
  DEBUG(printf("Error in map_config_id_to_inx, returning NULL\n"));
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
  if (node_sect_id >= conf_obj->num_nodes)
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
  g_assert(comm_sect_id < conf_obj->num_comms);
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
arrange_node_arrays(IC_CLUSTER_CONFIG *conf_obj)
{
  gchar **new_node_config;
  IC_NODE_TYPES *new_node_types;
  guint32 i, node_id;
  DEBUG_ENTRY("arrange_node_arrays");

  new_node_types= (IC_NODE_TYPES*)ic_calloc((conf_obj->max_node_id + 1) *
                                            sizeof(guint32));
  new_node_config= (gchar**)ic_calloc((conf_obj->max_node_id + 1) *
                                      sizeof(gchar*));
  if (!new_node_types || !new_node_config)
    return MEM_ALLOC_ERROR;

  for (i= 0; i < conf_obj->num_nodes; i++)
  {
    node_id= conf_obj->node_ids[i];
    if (!node_id)
      continue;
    new_node_types[node_id]= conf_obj->node_types[i];
    new_node_config[node_id]= conf_obj->node_config[i];
  }
  ic_free((gchar*)conf_obj->node_types);
  ic_free((gchar*)conf_obj->node_ids);
  ic_free((gchar*)conf_obj->node_config);
  conf_obj->node_config= new_node_config;
  conf_obj->node_types= new_node_types;
  conf_obj->node_ids= NULL;
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
//    if (sect_id == 0 || sect_id == 1)
      printf("Section: %u, Config id: %u, Type: %u, value: %u\n", sect_id, hash_key,
             key_type, value);
    if (pass == 0 && sect_id == 1)
      conf_obj->num_nodes= MAX(conf_obj->num_nodes, hash_key + 1);
    else if (pass == 1)
    {
      if (sect_id == (conf_obj->num_nodes + 2))
        conf_obj->num_comms= MAX(conf_obj->num_comms, hash_key + 1);
      if ((sect_id > 1 && sect_id < (conf_obj->num_nodes + 2)))
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
      if (sect_id > 1 && sect_id != (conf_obj->num_nodes + 2))
      {
        if (sect_id < (conf_obj->num_nodes + 2))
        {
          guint32 node_sect_id= sect_id - 2;
          if ((error= read_node_section(conf_obj, apic, key_type, &key_value,
                                        value, hash_key, node_sect_id)))
            return error;
        }
        else
        {
          guint32 comm_sect_id= sect_id - (conf_obj->num_nodes + 3);
          if ((error= read_comm_section(conf_obj, apic, key_type, &key_value,
                                        value, hash_key, comm_sect_id)))
            return error;
        }
      }
    }
  }
  if (pass != 2)
    return 0;
  return arrange_node_arrays(conf_obj);
}

/*
  CONFIGURATION RETRIEVE MODULE
  -----------------------------
  This module contains the code to retrieve the configuration for a given
  cluster from the cluster server.
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
  if (!(bin_buf= ic_calloc(bin_config_size)))
    return MEM_ALLOC_ERROR;
  if ((error= ic_base64_decode((guint8*)bin_buf, &bin_config_size,
                               (const guint8*)config_buf, config_size)))
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
  ic_free(bin_buf);
  return 0;
}

static int
set_up_cluster_server_connection(IC_CONNECTION *conn,
                                 gchar *server_name,
                                 gchar *server_port)
{
  int error;

  if (ic_init_socket_object(conn, TRUE, FALSE, FALSE, FALSE,
                            NULL, NULL))
    return MEM_ALLOC_ERROR;
  conn->server_name= server_name;
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
                guint32 current_cluster_index,
                guint64 the_version_num)
{
  gchar version_buf[128];
  gchar nodeid_buf[32];
  gchar endian_buf[32];
  gchar nodetype_buf[32];
  gchar buf[128];
  guint32 node_type= 1;
  guint32 node_id= apic->node_ids[current_cluster_index];

  g_snprintf(version_buf, 32, "%s%s", version_str, 
             ic_guint64_str(the_version_num, buf));
  g_snprintf(nodeid_buf, 32, "%s%u", nodeid_str, node_id);
  g_snprintf(nodetype_buf, 32, "%s%u", nodetype_str, node_type);
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
  g_snprintf(endian_buf, 32, "%s%s", endian_str, little_endian_str);
#else
  g_snprintf(endian_buf, 32, "%s%s", endian_str, big_endian_str);
#endif
  if (ic_send_with_cr(conn, get_nodeid_str) ||
      ic_send_with_cr(conn, version_buf) ||
      ic_send_with_cr(conn, nodetype_buf) ||
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
send_get_config(IC_CONNECTION *conn, guint64 the_version_num)
{
  gchar version_buf[128];
  gchar buf[128];

  g_snprintf(version_buf, 32, "%s%s", version_str,
             ic_guint64_str(the_version_num, buf));
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
          The protocol is decoded in the order of the case statements in the
          switch statements.
 
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
        if (!(config_buf= ic_calloc(content_length)))
          return MEM_ALLOC_ERROR;
        config_size= 0;
        rec_config_size= 0;
        state= WAIT_EMPTY_RETURN_STATE;
        break;
      case WAIT_EMPTY_RETURN_STATE:
      case WAIT_LAST_EMPTY_RETURN_STATE:
        /*
          Receive:
          <CR>
        */
        if (read_size != 0)
        {
          printf("Protocol error in wait empty return state\n");
          return PROTOCOL_ERROR;
        }
        if (state == WAIT_EMPTY_RETURN_STATE)
          state= RECEIVE_CONFIG_STATE;
        else
          return 0;
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
          ic_free(config_buf);
          if (error)
            return error;
          state= WAIT_LAST_EMPTY_RETURN_STATE;
        }
        else if (read_size != CONFIG_LINE_LEN)
        {
          ic_free(config_buf);
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
get_cs_config(IC_API_CONFIG_SERVER *apic, guint64 the_version_num)
{
  guint32 i;
  int error= END_OF_FILE;
  IC_CONNECTION conn_obj, *conn;
  DEBUG_ENTRY("get_cs_config");

  if (the_version_num == 0LL)
    the_version_num= version_no;
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
    if ((error= send_get_nodeid(conn, apic, i, the_version_num)) ||
        (error= rec_get_nodeid(conn, apic, i)) ||
        (error= send_get_config(conn, the_version_num)) ||
        (error= rec_get_config(conn, apic, i)))
      continue;
  }
  if (error)
    goto error;
  for (i= 0; i < apic->num_clusters_to_connect; i++)
  {
    IC_CLUSTER_CONFIG *clu_conf= apic->conf_objects+i;
    if (build_hash_on_comms(clu_conf))
    {
      error= MEM_ALLOC_ERROR;
      break;
    }
  }
error:
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
      ic_free(apic->cluster_conn.cluster_srv_conns);
    if (apic->cluster_conn.cluster_srv_conns)
      ic_free(apic->cluster_conn.cluster_server_ips);
    if (apic->cluster_conn.cluster_server_ports)
      ic_free(apic->cluster_conn.cluster_server_ports);
    if (apic->cluster_ids)
      ic_free(apic->cluster_ids);
    if (apic->node_ids)
      ic_free(apic->node_ids);
    if (apic->string_memory_to_return)
      ic_free(apic->string_memory_to_return);
    if (apic->config_memory_to_return)
      ic_free(apic->config_memory_to_return);
    if (apic->conf_objects)
    {
      num_clusters= apic->num_clusters_to_connect;
      for (i= 0; i < num_clusters; i++)
      {
        struct ic_cluster_config *conf_obj= apic->conf_objects+i;
        if (!conf_obj)
          continue;
        if (conf_obj->node_config)
          ic_free(conf_obj->node_config);
        if (conf_obj->comm_config)
          ic_free(conf_obj->comm_config);
        if (conf_obj->node_ids)
          ic_free(conf_obj->node_ids);
        if (conf_obj->node_types)
          ic_free(conf_obj->node_types);
        if (conf_obj->comm_types)
          ic_free(conf_obj->comm_types);
        if (conf_obj->comm_hash)
          ic_hashtable_destroy(conf_obj->comm_hash);
      }
      ic_free(apic->conf_objects);
    }
    ic_free(apic);
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
  if (!(apic= (IC_API_CONFIG_SERVER*)
               ic_calloc(sizeof(IC_API_CONFIG_SERVER))) ||
      !(apic->cluster_ids= (guint32*)
               ic_calloc(sizeof(guint32) * num_clusters)) ||
      !(apic->node_ids= (guint32*)
               ic_calloc(sizeof(guint32) * num_clusters)) ||
      !(apic->cluster_conn.cluster_server_ips= 
         (gchar**)ic_calloc(num_cluster_servers *
                             sizeof(gchar*))) ||
      !(apic->cluster_conn.cluster_server_ports=
         (gchar**)ic_calloc(num_cluster_servers *
                            sizeof(gchar*))) ||
      !(apic->conf_objects= (IC_CLUSTER_CONFIG*)
                   ic_calloc(num_cluster_servers *
                             sizeof(IC_CLUSTER_CONFIG))) ||
      !(apic->cluster_conn.cluster_srv_conns=
         (IC_CONNECTION*)ic_calloc(num_cluster_servers *
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
         num_cluster_servers * sizeof(gchar*));
  memcpy((gchar*)apic->cluster_conn.cluster_server_ports,
         (gchar*)cluster_conn->cluster_server_ports,
         num_cluster_servers * sizeof(gchar*));
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
  The module below implements translation of a IC_CLUSTER_CONFIG object to a
  an array of key-value pairs divided in sections according to cluster
  server protocol. It also uses the ic_base64_encode routine to convert this
  into a base64 representation which is how it is sent in the cluster server
  protocol.
*/
static guint32
get_length_of_section(IC_CONFIG_TYPES config_type,
                      gchar *conf, guint64 version_number)
{
  IC_CONFIG_ENTRY *conf_entry;
  gchar **charptr;
  guint32 len= 0, i, str_len;
  for (i= 0; i < MAX_CONFIG_ID; i++)
  {
    conf_entry= &glob_conf_entry[i];
    if ((conf_entry->config_types & (1 << ((guint32)config_type))) &&
        (!conf_entry->is_not_sent) &&
        is_entry_used_in_version(conf_entry, version_number))
    {
      switch (conf_entry->data_type)
      {
        case IC_BOOLEAN:
        case IC_UINT16:
        case IC_UINT32:
          break;
        case IC_UINT64:
          len++;
          break;
        case IC_CHARPTR:
        {
          charptr= (gchar**)(conf+conf_entry->offset);
          str_len= 0;
          if (*charptr)
            str_len= strlen(*charptr);
          len+= ((str_len+4)/4); /* Also make place for final NULL */
          break;
        }
        default:
          abort();
          break;
      }
      len+= 2;
    }
  }
  len+= 2; /* One key-value pair for node type */
  len+= 2; /* One key-value pair for parent node id */
  return len;
}

static int
fill_key_value_section(IC_CONFIG_TYPES config_type,
                       gchar *conf,
                       guint32 sect_id,
                       guint32 *key_value_array,
                       guint32 *key_value_array_len,
                       guint64 version_number)
{
  IC_CONFIG_ENTRY *conf_entry;
  guint32 len= 0, i, key, config_id, value, data_type, str_len;
  guint32 *assign_array;
  gchar **charptr;
  guint32 loc_key_value_array_len= *key_value_array_len;
  for (i= 0; i < MAX_CONFIG_ID; i++)
  {
    conf_entry= &glob_conf_entry[i];
    if ((conf_entry->config_types & (1 << ((guint32)config_type))) &&
        (!conf_entry->is_not_sent) &&
        is_entry_used_in_version(conf_entry, version_number))
    {
      assign_array= &key_value_array[loc_key_value_array_len];
      switch (conf_entry->data_type)
      {
        case IC_BOOLEAN:
        {
          guint8 *entry= (guint8*)(conf+conf_entry->offset);
          value= (guint32)*entry;
          data_type= IC_CL_INT32_TYPE;
          break;
        }
        case IC_UINT16:
        {
          guint16 *entry= (guint16*)(conf+conf_entry->offset);
          value= (guint32)*entry;
          data_type= IC_CL_INT32_TYPE;
          break;
        }
        case IC_UINT32:
        {
          guint32 *entry= (guint32*)(conf+conf_entry->offset);
          value= (guint32)*entry;
          data_type= IC_CL_INT32_TYPE;
          break;
        }
        case IC_UINT64:
        {
          guint64 *entry= (guint64*)(conf+conf_entry->offset);
          value= (guint32)((guint64)(*entry >> 32));
          assign_array[2]= g_htonl(value);
          value= *entry & 0xFFFFFFFF;
          loc_key_value_array_len++;
          data_type= IC_CL_INT64_TYPE;
          break;
        }
        case IC_CHARPTR:
        {
          charptr= (gchar**)(conf+conf_entry->offset);
          str_len= 0;
          if (*charptr)
            str_len= strlen(*charptr);
          value= str_len + 1; /* Reported length includes NULL byte */
          /* 
             Adjust to number of words with one word removed and
             an extra null byte calculated for
           */
          len= (value + 3)/4;
          /* We don't need to copy null byte since we initialised to 0 */
          if (str_len)
            memcpy((gchar*)&assign_array[2],
                   *charptr,
                   str_len);
          DEBUG(printf("String value = %s\n", (gchar*)&assign_array[2]));
          loc_key_value_array_len+= len;
          data_type= IC_CL_CHAR_TYPE;
          break;
        }
        default:
          return IC_ERROR_INCONSISTENT_DATA;
      }
      /*
         Assign the key consisting of:
         1) Data Type
         2) Section id
         3) Config id
       */
      config_id= map_inx_to_config_id[i];
      key= (data_type << IC_CL_KEY_SHIFT) +
           (sect_id << IC_CL_SECT_SHIFT) +
           (config_id);
      assign_array[0]= g_htonl(key);
      assign_array[1]= g_htonl(value);
      DEBUG(printf("sectid = %u, config_id = %u, value = %u\n",
                   sect_id, config_id, value));
      loc_key_value_array_len+= 2;
    }
  }
  /* Add node type for all sections */
  assign_array= &key_value_array[loc_key_value_array_len];
  config_id= IC_NODE_TYPE;
  key= (IC_CL_INT32_TYPE << IC_CL_KEY_SHIFT) +
       (sect_id << IC_CL_SECT_SHIFT) +
       config_id;
  value= (config_type == IC_COMM_TYPE) ? 0 : (guint32)config_type;
  DEBUG(printf("sectid = %u, config_id = %u, value = %u\n",
                sect_id, config_id, value));
  assign_array[0]= g_htonl(key);
  assign_array[1]= g_htonl(value);
  loc_key_value_array_len+= 2;

  /* Add parent id == 0 for all sections */
  assign_array= &key_value_array[loc_key_value_array_len];
  config_id= IC_PARENT_ID;
  key= (IC_CL_INT32_TYPE << IC_CL_KEY_SHIFT) +
       (sect_id << IC_CL_SECT_SHIFT) +
       config_id;
  value= (guint32)0;
  DEBUG(printf("sectid = %u, config_id = %u, value = %u\n",
                sect_id, config_id, value));
  assign_array[0]= g_htonl(key);
  assign_array[1]= g_htonl(value);
  loc_key_value_array_len+= 2;

  *key_value_array_len= loc_key_value_array_len;
  return 0;
}

/* This routine depends on that node1 < node2 */
static IC_SOCKET_LINK_CONFIG*
get_comm_section(IC_CLUSTER_CONFIG *clu_conf,
                 IC_SOCKET_LINK_CONFIG *comm_section,
                 guint32 node1, guint32 node2)
{
  IC_SOCKET_LINK_CONFIG *local_comm_section;
  IC_KERNEL_CONFIG *server_conf;
  IC_KERNEL_CONFIG *client_conf;
  comm_section->first_node_id= node1;
  comm_section->second_node_id= node2;
  if ((local_comm_section= (IC_SOCKET_LINK_CONFIG*)
                           ic_hashtable_search(clu_conf->comm_hash,
                                               (void*)comm_section)))
    return local_comm_section;
  if ((local_comm_section= (IC_SOCKET_LINK_CONFIG*)
                           ic_hashtable_search(clu_conf->comm_hash,
                                               (void*)comm_section)))
    return local_comm_section;
  init_config_object((gchar*)comm_section, IC_COMM_TYPE);
  comm_section->first_node_id= node1;
  comm_section->second_node_id= node2;
  if (clu_conf->node_types[node1] == IC_KERNEL_NODE ||
      clu_conf->node_types[node2] != IC_KERNEL_NODE)
  {
    comm_section->server_node_id= node1;
    server_conf= (IC_KERNEL_CONFIG*)clu_conf->node_config[node1];
    client_conf= (IC_KERNEL_CONFIG*)clu_conf->node_config[node2];
  }
  else
  {
    comm_section->server_node_id= node2;
    server_conf= (IC_KERNEL_CONFIG*)clu_conf->node_config[node2];
    client_conf= (IC_KERNEL_CONFIG*)clu_conf->node_config[node1];
  }
  comm_section->server_port_number= server_conf->port_number;
  comm_section->client_port_number= client_conf->port_number;
  comm_section->first_hostname= server_conf->hostname;
  comm_section->second_hostname= client_conf->hostname;
  return comm_section;
}

static int
ic_get_key_value_sections_config(IC_CLUSTER_CONFIG *clu_conf,
                                 guint32 **key_value_array,
                                 guint32 *key_value_array_len,
                                 guint64 version_number)
{
  guint32 len= 0, num_comms= 0;
  guint32 node_sect_len, i, j, checksum;
  guint32 section_id, comm_desc_section, node_desc_section;
  guint32 *loc_key_value_array;
  guint32 loc_key_value_array_len= 0;
  int ret_code;
  gboolean from_iclaustron= version_number > (guint64)0xFFFFFFFF;
  IC_SOCKET_LINK_CONFIG test1, *comm_section;

  /*
   * Add 2 words for verification string in beginning
   * Add 2 key-value pairs for section 0
   * Add one key-value pair for each node section
   *   - This is section 1
   */
  len+= 2;
  len+= 4;
  len+= clu_conf->num_nodes * 2;
  printf("1: len=%u\n", len);
  for (i= 1; i <= clu_conf->max_node_id; i++)
  {
    /* Add length of each node section */
    if (clu_conf->node_config[i])
    {
      node_sect_len= get_length_of_section(
                          (IC_CONFIG_TYPES)clu_conf->node_types[i],
                                           clu_conf->node_config[i],
                                           version_number);
      if (node_sect_len == 0)
        return IC_ERROR_INCONSISTENT_DATA;
      len+= node_sect_len;
      printf("2: len=%u\n", len);
    }
  }
  /* Add length of each comm section */
  for (i= 1; i <= clu_conf->max_node_id; i++)
  {
    if (clu_conf->node_config[i])
    {
      for (j= i+1; j <= clu_conf->max_node_id; j++)
      {
        if (clu_conf->node_config[j])
        {
          /* iClaustron uses a fully connected cluster */
          if ((!(clu_conf->node_types[i] == IC_KERNEL_NODE ||
                clu_conf->node_types[j] == IC_KERNEL_NODE)) ||
                from_iclaustron)
            continue;
          /* We have found two nodes needing a comm section */
          comm_section= get_comm_section(clu_conf, &test1, i, j);
          len+= get_length_of_section(IC_COMM_TYPE, (gchar*)comm_section,
                                      version_number);
          num_comms++;
          printf("4: len=%u\n", len);
        }
      }
    }
  }
  /*
   * Add one key-value pair for each comm section
   *   - This is section 2 + num_nodes
   */
  len+= num_comms * 2;
  printf("3: len=%u\n", len);
  /* Add 1 word for checksum at the end */
  len+= 1;
  printf("5: len=%u\n", len);

  /* Allocate memory for key-value pairs */
  if (!(loc_key_value_array= (guint32*)ic_calloc(4*len)))
    return MEM_ALLOC_ERROR;
  *key_value_array= loc_key_value_array;
  /*
    Put in verification section
  */
  memcpy((gchar*)loc_key_value_array, ver_string, 8);

  /*
    Fill Section 0
      Id 2000 specifies section 1 as a section that specifies node sections
      Id 3000 specifies section number of the section that describes the
      communication sections
  */
  section_id= 0;
  comm_desc_section= 2 + clu_conf->num_nodes;
  node_desc_section= 1;
  loc_key_value_array[2]= (3 << IC_CL_KEY_SHIFT) +
                          (section_id << IC_CL_SECT_SHIFT) +
                          2000;
  loc_key_value_array[3]= node_desc_section << IC_CL_SECT_SHIFT;

  loc_key_value_array[4]= (3 << IC_CL_KEY_SHIFT) +
                          (section_id << IC_CL_SECT_SHIFT) +
                          3000;
  loc_key_value_array[5]= comm_desc_section << IC_CL_SECT_SHIFT;
  loc_key_value_array_len= 6;

  /*
    Fill Section 1
    One key-value for each section that specifies a node, starting at
    section 2 and ending at section 2+num_nodes-1.
  */
  section_id= 1;
  for (i= 0; i < clu_conf->num_nodes; i++)
  {
    loc_key_value_array[loc_key_value_array_len++]= (1 << IC_CL_KEY_SHIFT) +
                                          (section_id << IC_CL_SECT_SHIFT) +
                                              i;
    loc_key_value_array[loc_key_value_array_len++]= (2+i) << IC_CL_SECT_SHIFT;
  }
  printf("1: fill_len=%u\n", loc_key_value_array_len);
  for (i= 2; i < 6 + (2 * clu_conf->num_nodes); i++)
    loc_key_value_array[i]= g_htonl(loc_key_value_array[i]);

  /* Fill node sections */
  section_id= 2;
  for (i= 1; i <= clu_conf->max_node_id; i++)
  {
    if (clu_conf->node_config[i] &&
        (ret_code= fill_key_value_section(clu_conf->node_types[i],
                                          clu_conf->node_config[i],
                                          section_id++,
                                          loc_key_value_array,
                                          &loc_key_value_array_len,
                                          version_number)))
      goto error;
    printf("2: fill_len=%u\n", loc_key_value_array_len);
  }

  /*
    Fill section 1 + 1 + no_of_nodes
    This specifies the communication sections, one for each pair of nodes
    that need to communicate
  */
  g_assert(comm_desc_section == section_id);
  section_id= comm_desc_section;
  for (i= 0; i < num_comms; i++)
  {
    loc_key_value_array[loc_key_value_array_len++]= g_htonl(
                                   (1 << IC_CL_KEY_SHIFT) +
                                   (comm_desc_section << IC_CL_SECT_SHIFT) +
                                   i);
    loc_key_value_array[loc_key_value_array_len++]= g_htonl(
                              (comm_desc_section+i+1) << IC_CL_SECT_SHIFT);
  }

  printf("3: fill_len=%u\n", loc_key_value_array_len);
  /* Fill comm sections */
  section_id= comm_desc_section + 1;
  for (i= 1; i <= clu_conf->max_node_id; i++)
  {
    if (clu_conf->node_config[i])
    {
      for (j= i+1; j <= clu_conf->max_node_id; j++)
      {
        if (clu_conf->node_config[j])
        {
          /* iClaustron uses a fully connected cluster */
          if ((!(clu_conf->node_types[i] == IC_KERNEL_NODE ||
               clu_conf->node_types[j] == IC_KERNEL_NODE)) ||
               from_iclaustron)
            continue;
          /* We have found two nodes needing a comm section */
          comm_section= get_comm_section(clu_conf, &test1, i, j);
          if ((ret_code= fill_key_value_section(IC_COMM_TYPE,
                                                (gchar*)comm_section,
                                                section_id++,
                                                loc_key_value_array,
                                                &loc_key_value_array_len,
                                                version_number)))
            goto error;
          printf("4: fill_len=%u\n", loc_key_value_array_len);
        }
      }
    }
  }
  /* Calculate and fill out checksum */
  checksum= 0;
  for (i= 0; i < loc_key_value_array_len; i++)
    checksum^= g_ntohl(loc_key_value_array[i]);
  loc_key_value_array[loc_key_value_array_len++]= g_ntohl(checksum);
  printf("5: fill_len=%u\n", loc_key_value_array_len);
  /* Perform final set of checks */
  *key_value_array_len= loc_key_value_array_len;
  if (len == loc_key_value_array_len)
    return 0;

  ret_code= IC_ERROR_INCONSISTENT_DATA;
error:
  ic_free(*key_value_array);
  return ret_code;
}

static int
ic_get_base64_config(IC_CLUSTER_CONFIG *clu_conf,
                     guint8 **base64_array,
                     guint32 *base64_array_len,
                     guint64 version_number)
{
  guint32 *key_value_array;
  guint32 key_value_array_len;
  int ret_code;

  *base64_array= 0;
  if ((ret_code= ic_get_key_value_sections_config(clu_conf, &key_value_array,
                                                  &key_value_array_len,
                                                  version_number)))
    return ret_code;
  ret_code= ic_base64_encode(base64_array,
                             base64_array_len,
                             (const guint8*)key_value_array,
                             key_value_array_len*4);
  DEBUG(printf("%s", *(gchar**)base64_array));
  ic_free(key_value_array);
  return ret_code;
}

/*
  MODULE: CONFIGURATION SERVER
  ----------------------------
    This is the module that provided with a configuration data structures
    ensures that anyone can request this configuration through a given
    socket and port.
*/
static void
free_run_cluster(struct ic_run_config_server *run_obj)
{
  if (run_obj)
  {
    run_obj->run_conn.conn_op.free_ic_connection(&run_obj->run_conn); 
    ic_free(run_obj);
  }
  return;
}

static int
rec_get_nodeid_req(IC_CONNECTION *conn,
                   guint64 *node_number,
                   guint64 *version_number,
                   guint64 *node_type,
                   guint64 *cluster_id)
{
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  guint32 state= GET_NODEID_REQ_STATE;
  int error;
  gchar read_buf[256];
  DEBUG_ENTRY("rec_get_nodeid_req");

  while (!(error= ic_rec_with_cr(conn, read_buf, &read_size,
                                 &size_curr_buf, sizeof(read_buf))))
  {
    DEBUG(ic_print_buf(read_buf, read_size));
    switch (state)
    {
      case GET_NODEID_REQ_STATE:
        if ((read_size != GET_NODEID_LEN) ||
            (memcmp(read_buf, get_nodeid_str,
                    GET_NODEID_LEN) != 0))
        {
          printf("Protocol error in get nodeid request state\n");
          return PROTOCOL_ERROR;
        }
        state= VERSION_REQ_STATE;
        break;
      case VERSION_REQ_STATE:
        if ((read_size <= VERSION_REQ_LEN) ||
            (memcmp(read_buf, version_str, VERSION_REQ_LEN) != 0) ||
            (convert_str_to_int_fixed_size(read_buf + VERSION_REQ_LEN,
                                           read_size - VERSION_REQ_LEN,
                                           version_number)))
        {
          printf("Protocol error in version request state\n");
          return PROTOCOL_ERROR;
        }
        state= NODETYPE_REQ_STATE;
        break;
      case NODETYPE_REQ_STATE:
        if ((read_size <= NODETYPE_REQ_LEN) ||
            (memcmp(read_buf, nodetype_str, NODETYPE_REQ_LEN) != 0) ||
            (convert_str_to_int_fixed_size(read_buf + NODETYPE_REQ_LEN,
                                           read_size - NODETYPE_REQ_LEN,
                                           node_type)))
        {
          printf("Protocol error in nodetype request state\n");
          return PROTOCOL_ERROR;
        }
        state= NODEID_REQ_STATE;
        break;
      case NODEID_REQ_STATE:
        if ((read_size <= NODEID_LEN) ||
            (memcmp(read_buf, nodeid_str, NODEID_LEN) != 0) ||
            (convert_str_to_int_fixed_size(read_buf + NODEID_LEN,
                                           read_size - NODEID_LEN,
                                           node_number)) ||
            (*node_number > MAX_NODE_ID))
        {
          printf("Protocol error in nodeid request state\n");
          return PROTOCOL_ERROR;
        }
        state= USER_REQ_STATE;
        break;
      case USER_REQ_STATE:
        if ((read_size != USER_REQ_LEN) ||
            (memcmp(read_buf, user_str, USER_REQ_LEN) != 0))
        {
          printf("Protocol error in user request state\n");
          return PROTOCOL_ERROR;
        }
        state= PASSWORD_REQ_STATE;
        break;
      case PASSWORD_REQ_STATE:
        if ((read_size != PASSWORD_REQ_LEN) ||
            (memcmp(read_buf, password_str, PASSWORD_REQ_LEN) != 0))
        {
          printf("Protocol error in password request state\n");
          return PROTOCOL_ERROR;
        }
        state= PUBLIC_KEY_REQ_STATE;
        break;
      case PUBLIC_KEY_REQ_STATE:
        if ((read_size != PUBLIC_KEY_REQ_LEN) ||
            (memcmp(read_buf, public_key_str, PUBLIC_KEY_REQ_LEN) != 0))
        {
          printf("Protocol error in public key request state\n");
          return PROTOCOL_ERROR;
        }
        state= ENDIAN_REQ_STATE;
        break;
      case ENDIAN_REQ_STATE:
        if ((read_size < ENDIAN_REQ_LEN) ||
            (memcmp(read_buf, endian_str, ENDIAN_REQ_LEN) != 0))
        {
          printf("Protocol error in endian request state\n");
          return PROTOCOL_ERROR;
        }
        if (!((read_size == ENDIAN_REQ_LEN + LITTLE_ENDIAN_LEN &&
              memcmp(read_buf+ENDIAN_REQ_LEN, little_endian_str,
                     LITTLE_ENDIAN_LEN) == 0) ||
             (read_size == ENDIAN_REQ_LEN + BIG_ENDIAN_LEN &&
              memcmp(read_buf+ENDIAN_REQ_LEN, big_endian_str,
                     BIG_ENDIAN_LEN) == 0)))
        {
          printf("Failure in representation of what endian type\n");
          return PROTOCOL_ERROR;
        }
        state= LOG_EVENT_REQ_STATE;
        break;
      case LOG_EVENT_REQ_STATE:
        if ((read_size != LOG_EVENT_REQ_LEN) ||
            (memcmp(read_buf, log_event_str, LOG_EVENT_REQ_LEN) != 0))
        {
          printf("Protocol error in log_event request state\n");
          return PROTOCOL_ERROR;
        }
        if (*version_number < 1000000)
          return 0;
        state= CLUSTER_ID_REQ_STATE;
        break;
      case CLUSTER_ID_REQ_STATE:
        if ((read_size <= CLUSTER_ID_REQ_LEN) ||
            (memcmp(read_buf, cluster_id_str, CLUSTER_ID_REQ_LEN) != 0) ||
            (convert_str_to_int_fixed_size(read_buf + CLUSTER_ID_REQ_LEN,
                                           read_size - CLUSTER_ID_REQ_LEN,
                                           cluster_id)))
        {
          printf("Protocol error in cluster id request state\n");
          return PROTOCOL_ERROR;
        }
        return 0;
        break;
      default:
        abort();
        break;
    }
  }
  printf("Error in receiving get node id request, error = %d", error);
  return error;
}

static int
send_get_nodeid_reply(IC_CONNECTION *conn, guint32 node_id)
{
  gchar nodeid_buf[32];
  DEBUG_ENTRY("send_get_nodeid_reply");

  g_snprintf(nodeid_buf, 32, "%s%u", nodeid_str, node_id);
  if (ic_send_with_cr(conn, get_nodeid_reply_str) ||
      ic_send_with_cr(conn, nodeid_buf) ||
      ic_send_with_cr(conn, result_ok_str) ||
      ic_send_with_cr(conn, empty_string))
    return conn->error_code;
  return 0;
}

static int
rec_get_config_req(IC_CONNECTION *conn, guint64 version_number)
{
  guint32 read_size= 0;
  guint32 size_curr_buf= 0;
  guint32 state= GET_CONFIG_REQ_STATE;
  guint64 read_version_num;
  int error;
  gchar read_buf[256];
  DEBUG_ENTRY("rec_get_config_req");

  while (!(error= ic_rec_with_cr(conn, read_buf, &read_size,
                                 &size_curr_buf, sizeof(read_buf))))
  {
    DEBUG(ic_print_buf(read_buf, read_size));
    switch(state)
    {
      case GET_CONFIG_REQ_STATE:
        if (read_size != GET_CONFIG_LEN ||
            memcmp(read_buf, get_config_str, GET_CONFIG_LEN))
        {
          printf("Protocol error in get config request state\n");
          return PROTOCOL_ERROR;
        }
        state= VERSION_REQ_STATE;
        break;
      case VERSION_REQ_STATE:
        if ((read_size <= VERSION_REQ_LEN) ||
            (memcmp(read_buf, version_str, VERSION_REQ_LEN) != 0) ||
            (convert_str_to_int_fixed_size(read_buf + VERSION_REQ_LEN,
                                           read_size - VERSION_REQ_LEN,
                                           &read_version_num)) ||
            version_number != read_version_num)
        {
          printf("Protocol error in version request state\n");
          return PROTOCOL_ERROR;
        }
        state= EMPTY_STATE;
        break;
      case EMPTY_STATE:
        if (read_size != 0)
        {
          printf("Protocol error in wait empty state\n");
          return PROTOCOL_ERROR;
        }
        return 0;
      default:
        abort();
        break;
    }
  }
  printf("Error in receiving get config request, error = %d", error);
  return error;
}

static int
send_config_reply(IC_CONNECTION *conn, gchar *config_base64_str,
                  guint32 config_len)
{
  gchar content_buf[64];
  DEBUG_ENTRY("send_config_reply");
 
  g_snprintf(content_buf, 64, "%s%u", content_len_str, config_len);
  if (ic_send_with_cr(conn, get_config_reply_str) ||
      ic_send_with_cr(conn, result_ok_str) ||
      ic_send_with_cr(conn, content_buf) ||
      ic_send_with_cr(conn, octet_stream_str) ||
      ic_send_with_cr(conn, content_encoding_str) ||
      ic_send_with_cr(conn, empty_string) ||
      conn->conn_op.write_ic_connection(conn, (const void*)config_base64_str,
                                        config_len, 0,1))
    return conn->error_code;
  return 0;
}

static int
handle_config_request(IC_RUN_CONFIG_SERVER *run_obj,
                      IC_CONNECTION *conn)
{
  int ret_code;
  guint32 node_id;
  guint64 node_number, version_number, node_type;
  guint64 cluster_id= 0;
  gchar *config_base64_str;
  guint32 config_len;
  DEBUG_ENTRY("handle_config_request");

  if ((ret_code= rec_get_nodeid_req(conn,
                                    &node_number, &version_number,
                                    &node_type, &cluster_id)))
    return ret_code;
  if (node_number == 0)
  {
    /* Here we need to discover which node id to use */
    node_id= 1; /* Fake for now */
  }
  else
  {
    /* Here we ensure that the requested node id is correct */
    node_id= 1;
  }
  if ((ret_code= send_get_nodeid_reply(conn, node_id)))
    return ret_code;
  if ((ret_code= rec_get_config_req(conn, version_number)))
    return ret_code;
  if ((ret_code= ic_get_base64_config(run_obj->conf_objects[0],
                                      (guint8**)&config_base64_str,
                                      &config_len,
                                      version_number)))
    return ret_code;
  printf("Converted configuration to a base64 representation\n");
  if ((ret_code= send_config_reply(conn, config_base64_str, config_len)) ||
      (ret_code= ic_send_with_cr(conn, empty_string)))
  {
    ic_free(config_base64_str);
    return ret_code;
  }
  g_usleep(30*1000*1000);
  return 0;
}

static int
run_cluster_server(IC_RUN_CONFIG_SERVER *run_obj)
{
  int ret_code;
  IC_CONNECTION *conn;
  DEBUG_ENTRY("run_cluster_server");

  conn= &run_obj->run_conn;
  ret_code= conn->conn_op.set_up_ic_connection(conn);
  if (ret_code)
  {
    printf("Failed to set-up connection\n");
    goto error;
  }
  printf("Run cluster server has set up connection and has received a connection\n");
  if ((ret_code= handle_config_request(run_obj, conn)))
    goto error;
  return 0;

error:
  return ret_code;
}

IC_RUN_CONFIG_SERVER*
ic_init_run_cluster(IC_CLUSTER_CONFIG **conf_objs,
                    guint32 *cluster_ids,
                    guint32 num_clusters,
                    gchar *server_name,
                    gchar *server_port)
{
  IC_RUN_CONFIG_SERVER *run_obj;
  IC_CONNECTION *conn;
  guint32 size;
  DEBUG_ENTRY("ic_init_run_cluster");

  /*
    Allocate memory for array of cluster ids, an array of cluster
    configuration objects and the run configuration server object.
    We allocate everything in one memory allocation.
  */
  size= sizeof(IC_RUN_CONFIG_SERVER);
  size+= num_clusters * sizeof(guint32);
  size+= num_clusters * sizeof(IC_CLUSTER_CONFIG*);
  if (!(run_obj= (IC_RUN_CONFIG_SERVER*)
      ic_calloc(size)))
    return NULL;
  run_obj->cluster_ids= (guint32*)(((gchar*)run_obj)+
                        sizeof(IC_RUN_CONFIG_SERVER));
  run_obj->conf_objects= (IC_CLUSTER_CONFIG**)(((gchar*)run_obj->cluster_ids)+
                          (num_clusters * sizeof(guint32)));
  conn= &run_obj->run_conn;
  if (ic_init_socket_object(conn,
                            FALSE, /* Not client */
                            FALSE, /* Don't use mutex */
                            FALSE, /* Don't use connect thread */
                            FALSE, /* Don't use front buffer */
                            NULL,  /* Don't use authentication function */
                            NULL)) /* No authentication object */
    goto error;

  memcpy((gchar*)run_obj->cluster_ids, (gchar*)cluster_ids,
         num_clusters * sizeof(guint32));
  memcpy((gchar*)run_obj->conf_objects, (gchar*)conf_objs,
         num_clusters * sizeof(IC_CLUSTER_CONFIG*));

  run_obj->run_conn.server_name= server_name;
  run_obj->run_conn.server_port= server_port;
  run_obj->run_conn.client_name= NULL;
  run_obj->run_conn.client_port= 0;
  run_obj->run_conn.is_wan_connection= FALSE;
  run_obj->run_op.run_ic_cluster_server= run_cluster_server;
  run_obj->run_op.free_ic_run_cluster= free_run_cluster;
  return run_obj;

error:
  ic_free(run_obj);
  return NULL;
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
int complete_section(IC_CONFIG_STRUCT *ic_conf, guint32 line_number,
                     guint32 pass)
{
  IC_CLUSTER_CONFIG_LOAD *clu_conf;
  IC_CONFIG_TYPES conf_type;
  guint32 i;
  guint64 mandatory_bits, missing_bits, bit64;
  void *current_config;
  IC_CONFIG_ENTRY *conf_entry;
  /*
    Need to check that all mandatory values have been assigned here in
    second pass.
  */
  clu_conf= ic_conf->config_ptr.clu_conf;
  conf_type= clu_conf->current_node_config_type;
  current_config= clu_conf->current_node_config;
  if (clu_conf->default_section || pass == INITIAL_PASS)
    return 0;
  switch (conf_type)
  {
    case IC_NO_CONFIG_TYPE:
      return 0;
    case IC_KERNEL_TYPE:
    {
      IC_KERNEL_CONFIG *kernel_conf= (IC_KERNEL_CONFIG*)current_config;
      mandatory_bits= kernel_mandatory_bits;
      if (kernel_conf->mandatory_bits != kernel_mandatory_bits)
        goto mandatory_error;
      if (kernel_conf->filesystem_path == NULL)
        kernel_conf->filesystem_path= kernel_conf->node_data_path;
      if (kernel_conf->kernel_checkpoint_path == NULL)
        kernel_conf->kernel_checkpoint_path= kernel_conf->filesystem_path;
      break;
    }
    case IC_CLIENT_TYPE:
      mandatory_bits= client_mandatory_bits;
      if (((IC_CLIENT_CONFIG*)current_config)->mandatory_bits !=
          client_mandatory_bits)
        goto mandatory_error;
      break;
    case IC_CLUSTER_SERVER_TYPE:
      mandatory_bits= cluster_server_mandatory_bits;
      if (((IC_CLUSTER_SERVER_CONFIG*)current_config)->mandatory_bits !=
          cluster_server_mandatory_bits)
        goto mandatory_error;
      break;
    case IC_REP_SERVER_TYPE:
      mandatory_bits= rep_server_mandatory_bits;
      if (((IC_REP_SERVER_CONFIG*)current_config)->client_conf.mandatory_bits
          != rep_server_mandatory_bits)
        goto mandatory_error;
      break;
    case IC_SQL_SERVER_TYPE:
      mandatory_bits= sql_server_mandatory_bits;
      if (((IC_SQL_SERVER_CONFIG*)current_config)->client_conf.mandatory_bits
            != sql_server_mandatory_bits)
        goto mandatory_error;
      break;
    case IC_COMM_TYPE:
      mandatory_bits= comm_mandatory_bits;
      if (((IC_SOCKET_LINK_CONFIG*)current_config)->mandatory_bits !=
          comm_mandatory_bits)
        goto mandatory_error;
      break;
    default:
      abort();
      break;
  }
  return 0;

mandatory_error:
  missing_bits= mandatory_bits ^
         ((IC_KERNEL_CONFIG*)current_config)->mandatory_bits;
#if 0
{
  gchar buf[128];
  printf("mandatory bits %s\n",
    ic_guint64_hex_str(((IC_KERNEL_CONFIG*)current_config)->mandatory_bits,
                       (gchar*)&buf));
  printf("missing bits %s\n",
    ic_guint64_hex_str(missing_bits,
                       (gchar*)&buf));
}
#endif
  g_assert(missing_bits);
  for (i= 0; i < 64; i++)
  {
    bit64= 1;
    bit64 <<= i;
    if (missing_bits & bit64)
    {
      if (!(conf_entry= get_config_entry_mandatory(i, conf_type)))
      {
        DEBUG(printf("Didn't find mandatory entry after config error, i= %u\n"
                     ,i));
        abort();
      }
      printf("Configuration error found at line %u, missing mandatory",
             line_number);
      printf(" configuration item in previous section\n");
      printf("Missing item is %s\n", conf_entry->config_entry_name.str);
    }
  }
  return 1;
}

static
int conf_serv_init(void *ic_conf, guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_conf;
  IC_CLUSTER_CONFIG_LOAD *clu_conf;
  guint32 max_node_id;
  guint32 size_structs= 0;
  DEBUG_ENTRY("conf_serv_init");
  if (pass == INITIAL_PASS)
  {
    if (!(clu_conf= (IC_CLUSTER_CONFIG_LOAD*)ic_calloc(
                    sizeof(IC_CLUSTER_CONFIG_LOAD))))
      return IC_ERROR_MEM_ALLOC;
    conf->config_ptr.clu_conf= (struct ic_cluster_config_load*)clu_conf;
    clu_conf->current_node_config_type= IC_NO_CONFIG_TYPE;
    return 0;
  }
  clu_conf= conf->config_ptr.clu_conf;
  max_node_id= clu_conf->conf.max_node_id;
  if (max_node_id == 0)
    return IC_ERROR_NO_NODES_FOUND;
  clu_conf->current_node_config_type= IC_NO_CONFIG_TYPE;
  /*
    Calculate size of all node struct's and allocate them in one chunk.
  */
  size_structs+= clu_conf->conf.num_data_servers * sizeof(IC_KERNEL_CONFIG);
  size_structs+= clu_conf->conf.num_clients * sizeof(IC_CLIENT_CONFIG);
  size_structs+= clu_conf->conf.num_cluster_servers *
                 sizeof(IC_CLUSTER_SERVER_CONFIG);
  size_structs+= clu_conf->conf.num_rep_servers * sizeof(IC_REP_SERVER_CONFIG);
  size_structs+= clu_conf->conf.num_sql_servers * sizeof(IC_SQL_SERVER_CONFIG);
  size_structs+= clu_conf->conf.num_comms *
                 sizeof(IC_SOCKET_LINK_CONFIG);
  if (!(clu_conf->struct_memory= clu_conf->struct_memory_to_return= (gchar*)
        ic_calloc(size_structs)))
    return IC_ERROR_MEM_ALLOC;
  if (!(clu_conf->conf.node_config= (gchar**)
        ic_calloc(sizeof(gchar*)*(max_node_id + 1))))
    return IC_ERROR_MEM_ALLOC;
  if (!(clu_conf->string_memory= clu_conf->string_memory_to_return= (gchar*)
        ic_calloc(clu_conf->size_string_memory)))
    return IC_ERROR_MEM_ALLOC;
  if (!(clu_conf->conf.node_types= 
        (IC_NODE_TYPES*)ic_calloc(sizeof(IC_NODE_TYPES)*(max_node_id+1))))
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
int conf_serv_add_section(void *ic_config,
                          __attribute__ ((unused)) guint32 section_number,
                          guint32 line_number,
                          IC_STRING *section_name,
                          guint32 pass)
{
  int error;
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_add_section");
  DEBUG_IC_STRING(section_name);

  if ((error= complete_section(ic_config, line_number, pass)))
    return error;
  clu_conf->default_section= FALSE;
  if (!ic_cmp_null_term_str(data_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_KERNEL_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf.num_data_servers++;
      clu_conf->conf.num_nodes++;
      return 0;
    }
    clu_conf->current_node_config= (void*)clu_conf->struct_memory;
    clu_conf->struct_memory+= sizeof(IC_KERNEL_CONFIG);
    memcpy(clu_conf->current_node_config, &clu_conf->default_kernel_config,
           sizeof(IC_KERNEL_CONFIG));
    DEBUG(printf("Found data server group\n"));
  }
  else if (!ic_cmp_null_term_str(client_node_str, section_name))
  {
    clu_conf->current_node_config_type= IC_CLIENT_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf.num_clients++;
      clu_conf->conf.num_nodes++;
      return 0;
    }
    clu_conf->current_node_config= (void*)clu_conf->struct_memory;
    clu_conf->struct_memory+= sizeof(IC_CLIENT_CONFIG);
    memcpy(clu_conf->current_node_config, &clu_conf->default_client_config,
           sizeof(IC_CLIENT_CONFIG));
    DEBUG(printf("Found client group\n"));
  }
  else if (!ic_cmp_null_term_str(cluster_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_CLUSTER_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf.num_cluster_servers++;
      clu_conf->conf.num_nodes++;
      return 0;
    }
    clu_conf->current_node_config= (void*)clu_conf->struct_memory;
    clu_conf->struct_memory+= sizeof(IC_CLUSTER_SERVER_CONFIG);
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_cluster_server_config,
           sizeof(IC_CLUSTER_SERVER_CONFIG));
    DEBUG(printf("Found cluster server group\n"));
  }
  else if (!ic_cmp_null_term_str(rep_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_REP_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf.num_rep_servers++;
      clu_conf->conf.num_nodes++;
      return 0;
    }
    clu_conf->current_node_config= (void*)clu_conf->struct_memory;
    clu_conf->struct_memory+= sizeof(IC_REP_SERVER_CONFIG);
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_rep_config,
           sizeof(IC_REP_SERVER_CONFIG));
    DEBUG(printf("Found replication server group\n"));
  }
  else if (!ic_cmp_null_term_str(sql_server_str, section_name))
  {
    clu_conf->current_node_config_type= IC_SQL_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf.num_sql_servers++;
      clu_conf->conf.num_nodes++;
      return 0;
    }
    clu_conf->current_node_config= (void*)clu_conf->struct_memory;
    clu_conf->struct_memory+= sizeof(IC_SQL_SERVER_CONFIG);
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_sql_config,
           sizeof(IC_SQL_SERVER_CONFIG));
    DEBUG(printf("Found sql server group\n"));
  }
  else if (!ic_cmp_null_term_str(socket_str, section_name))
  {
    clu_conf->current_node_config_type= IC_COMM_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf.num_comms++;
      return 0;
    }
    clu_conf->current_node_config= (void*)clu_conf->struct_memory;
    clu_conf->struct_memory+= sizeof(IC_SOCKET_LINK_CONFIG);
    memcpy(clu_conf->current_node_config,
           &clu_conf->default_socket_config,
           sizeof(IC_SOCKET_LINK_CONFIG));
    DEBUG(printf("Found socket group\n"));
  }
  else
  {
    clu_conf->default_section= TRUE;
    if (!ic_cmp_null_term_str(data_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_kernel_config;
      clu_conf->current_node_config_type= IC_KERNEL_TYPE;
      DEBUG(printf("Found data server default group\n"));
    }
    else if (!ic_cmp_null_term_str(client_node_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_client_config;
      clu_conf->current_node_config_type= IC_CLIENT_TYPE;
      DEBUG(printf("Found client default group\n"));
    }
    else if (!ic_cmp_null_term_str(cluster_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_cluster_server_config;
      clu_conf->current_node_config_type= IC_CLUSTER_SERVER_TYPE;
      DEBUG(printf("Found cluster server default group\n"));
    }
    else if (!ic_cmp_null_term_str(sql_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_sql_config;
      clu_conf->current_node_config_type= IC_SQL_SERVER_TYPE;
      DEBUG(printf("Found sql server default group\n"));
    }
    else if (!ic_cmp_null_term_str(rep_server_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_rep_config;
      clu_conf->current_node_config_type= IC_REP_SERVER_TYPE;
      DEBUG(printf("Found replication server default group\n"));
    }
    else if (!ic_cmp_null_term_str(socket_def_str, section_name))
    {
      clu_conf->current_node_config= &clu_conf->default_socket_config;
      clu_conf->current_node_config_type= IC_COMM_TYPE;
      DEBUG(printf("Found socket default group\n"));
    }
    else
    {
      DEBUG(printf("No such config section\n"));
      return IC_ERROR_CONFIG_NO_SUCH_SECTION;
    }
  }
  return 0;
}

static
int conf_serv_add_key(void *ic_config,
                      guint32 section_number,
                      guint32 line_number,
                      IC_STRING *key_name,
                      IC_STRING *data,
                      guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  IC_CONFIG_ENTRY *conf_entry;
  guint64 value;
  gchar *struct_ptr;
  guint64 num32_check;
  gchar buf[128];
  DEBUG_ENTRY("conf_serv_add_key");
  DEBUG_IC_STRING(key_name);
  DEBUG_IC_STRING(data);
  printf("Line: %d Section: %d, Key-value pair\n", (int)line_number,
                                                   (int)section_number);
  if (clu_conf->current_node_config_type == IC_NO_CONFIG_TYPE)
    return IC_ERROR_NO_SECTION_DEFINED_YET;
  if (!(conf_entry= (IC_CONFIG_ENTRY*)ic_hashtable_search(glob_conf_hash,
                                                          (void*)key_name)))
    return IC_ERROR_NO_SUCH_CONFIG_KEY;
  struct_ptr= (gchar*)clu_conf->current_node_config + conf_entry->offset;
  if (!(conf_entry->config_types & (1 << clu_conf->current_node_config_type)))
    return IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION;
  if (conf_entry->is_mandatory && (pass != INITIAL_PASS))
  {
    ((IC_KERNEL_CONFIG*)clu_conf->current_node_config)->mandatory_bits|=
      (1 << conf_entry->mandatory_bit);
  }
  if (conf_entry->is_string_type)
  {
    if (pass == INITIAL_PASS)
    {
      clu_conf->size_string_memory+= (data->len+1);
    }
    else
    {
      strncpy(clu_conf->string_memory, data->str, data->len);
      *(gchar**)struct_ptr= clu_conf->string_memory;
      clu_conf->string_memory+= (data->len+1);
    }
    return 0;
  }
  if (ic_conv_config_str_to_int(&value, data))
    return IC_ERROR_WRONG_CONFIG_NUMBER;
  if (conf_entry->is_boolean && value > 1)
    return IC_ERROR_NO_BOOLEAN_VALUE;
  num32_check= 1;
  num32_check<<= 32;
  if (!ic_cmp_null_term_str(node_id_str, key_name))
  {
    /*
      We have found a node id
    */
    clu_conf->conf.max_node_id= MAX(value, clu_conf->conf.max_node_id);
    if (!clu_conf->default_section && pass != INITIAL_PASS)
    {
      if (clu_conf->conf.node_config[value])
      {
        printf("Trying to define node %u twice", (guint32)value);
        return IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS; /* TODO fix error value */
      }
      clu_conf->conf.node_config[value]= (gchar*)clu_conf->current_node_config;
      clu_conf->conf.node_types[value]= 
        clu_conf->current_node_config_type;
    }
  }
  if (conf_entry->is_min_value_defined && conf_entry->min_value > value)
  {
    printf("Parameter %s is smaller than min_value = %u\n",
           ic_get_ic_string(key_name, (gchar*)&buf), conf_entry->min_value);
    return IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS;
  }
  else if (conf_entry->is_max_value_defined && conf_entry->max_value < value)
  {
    printf("Parameter %s is larger than min_value = %u\n",
           ic_get_ic_string(key_name, (gchar*)&buf), conf_entry->max_value);
    return IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS;
  }
  else if ((conf_entry->data_type == IC_UINT16 && value > 65535) ||
           (conf_entry->data_type == IC_UINT32 && value >= num32_check))
  {
    printf("Parameter %s is larger than its type\n",
           ic_get_ic_string(key_name, (gchar*)&buf));
    return IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS;
  }
  if (pass == INITIAL_PASS)
    return 0;
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
int conf_serv_add_comment(void *ic_config,
                          guint32 line_number,
                          guint32 section_number,
                          __attribute__ ((unused)) IC_STRING *comment,
                          guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_add_comment");
  printf("Line number %d in section %d was comment line\n", line_number, section_number);
  if (pass == INITIAL_PASS)
    clu_conf->comments.num_comments++;
  return 0;
}

static
int conf_serv_verify_conf(__attribute__ ((unused)) void *ic_conf)
{
/*
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_conf;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
*/
  return 0;
}

static
void conf_serv_end(void *ic_conf)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_conf;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  guint32 i;
  DEBUG_ENTRY("conf_serv_end");
  if (clu_conf)
  {
    if (clu_conf->conf.node_config)
      ic_free((gchar*)clu_conf->conf.node_config);
    if (clu_conf->conf.node_types)
      ic_free(clu_conf->conf.node_types);
    if (clu_conf->conf.node_ids)
      ic_free(clu_conf->conf.node_ids);
    for (i= 0; i < clu_conf->comments.num_comments; i++)
      ic_free(clu_conf->comments.ptr_comments[i]);
    if (clu_conf->conf.comm_hash)
      ic_hashtable_destroy(clu_conf->conf.comm_hash);
    ic_free(clu_conf->string_memory_to_return);
    ic_free(clu_conf->struct_memory_to_return);
    ic_free(clu_conf);
  }
  return;
}

static IC_CONFIG_OPERATIONS config_server_ops =
{
  .ic_config_init = conf_serv_init,
  .ic_add_section = conf_serv_add_section,
  .ic_add_key     = conf_serv_add_key,
  .ic_add_comment = conf_serv_add_comment,
  .ic_config_verify = conf_serv_verify_conf,
  .ic_config_end  = conf_serv_end,
};

IC_CLUSTER_CONFIG*
ic_load_config_server_from_files(gchar *config_file_path,
                                 IC_CONFIG_STRUCT *conf_server)
{
  gchar *conf_data_str;
  gsize conf_data_len;
  GError *loc_error= NULL;
  IC_STRING conf_data;
  int ret_val;
  IC_CONFIG_ERROR err_obj;
  IC_CLUSTER_CONFIG *ret_ptr;
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
  ic_free(conf_data.str);
  if (ret_val == 1)
  {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Error at Line number %u:\n%s\n",err_obj.line_number,
          ic_get_error_message(err_obj.err_num));
    ret_ptr= NULL;
  }
  else
  {
    ret_ptr= &conf_server->config_ptr.clu_conf->conf;
    if (build_hash_on_comms(ret_ptr))
    {
      conf_serv_end(conf_server);
      ret_ptr= NULL;
    }
  }
  return ret_ptr;

file_open_error:
  if (!config_file_path)
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "--config-file parameter required when using --bootstrap\n");
  else
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Couldn't open file %s\n", config_file_path);
  return NULL;
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
  DEBUG_OPEN;
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
  DEBUG_CLOSE;
}
