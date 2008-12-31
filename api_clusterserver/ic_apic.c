/* Copyight (C) 2007, 2008 iClaustron AB

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

#include <ic_apic.h>
#include <hashtable.h>
#include <glib/gstdio.h>
#include <unistd.h>
/*
  This file contains the code to handle the Cluster Server protocols,
  handling of configuration files. It's an important resource for all
  iClaustron programs. Cluster Servers use the run_cluster_server
  framework and most programs use the get configuration over the network
  framework. Also many use the framework to read configuration files,
  both the configuration files for a specific cluster and the configuration
  file containing information about which clusters exist and security
  information for those.

  The iClaustron servers can coexist with two types of classic NDB nodes.
  These are the ndbd nodes which contain the actual data and that handle
  all the transaction algorithms. The second type is NDB API clients, these
  could be classic NDB API programs or a MySQL Server using the NDB API.
  The classic nodes have some limitations which iClaustron must handle.
  These are:
  1) No SSL connections
  2) No security handling
  3) Only one cluster can be handled (for ndbd it's likely we will patch
     ndbd to send a cluster id to the iClaustron Cluster Server).
  4) It knows only 3 node types, Data Servers, Cluster Servers and
     Clients. Thus the Cluster Server must be aware of that it needs to
     translate node types when communicating with classic NDB nodes.

  DESCRIPTION HOW TO ADD NEW CONFIGURATION VARIABLE:
  1) Add a new constant in this file e.g:
  #define DATA_SERVER_SCHEDULER_NO_SEND_TIME 166
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
     node types, for data server variables the struct is
     ic_data_server_node_config (ic_apic.h). The name of this
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
  parameters of that node. The node section are separated such that all
  API node types are appearing from section 2 and onwards whereas the
  ndbd node sections are put after all other sections.

  After the last API node section there is another section used to describe
  the system, this also has a meta section which always lists only one
  section, the system (system is a synonym of cluster here).

  After the system section the meta section of the communication section is
  found after which the communication sections are listed.

  In the example case below there are 4 nodes and thus section 6 is the
  section describing which communication sections that exist.
  There is one communication section for each pair of nodes that require
  communication. So in this case with 2 data nodes and 1 API node and 1
  management server there is one communication section to interconnect the
  data nodes and 2 communication sections for each data node to connect them
  with the API and the cluster server. Thus a total of 5 communication
  sections.

  All the values in the sections are sent as a sorted list of
  key-value pairs. Each key-value pair contains section id, configuration
  id, value type and the value. We need to pass through
  the list of key-value pairs one time in order to discover the number of
  nodes of various types to understand the allocation requirements.

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
  -  Section 0      -
  -                 -
  -------------------
  |  |
  |  | -------------------
  |  | -                 -
  |  | -  Section 1      -
  |  |  -                 -
  |  |  -------------------
  |  |   |  |  |  |
  |  |   |  |  |  |           Node Sections
  |  |   |  |  |  ----------> ------------------
  |  |   |  |  |              -  Section 2     -
  |  |   |  |  |              ------------------
  |  |   |  |  |
  |  |   |  |  -------------> ------------------
  |  |   |  |                 -  Section 3     -
  |  |   |  |                 ------------------
  |  |   |  |
  |  |   |  |                 ndbd nodes
  |  |   |  ----------------> ------------------
  |  |   |                    -  Section 12    -
  |  |   |                    ------------------
  |  |   |
  |  |   -------------------> ------------------
  |  |                        -  Section 13    -
  |  |                        ------------------
  |  |
  |  -------------------
  |  -                 -
  |  -  Section 4      -
  |  -                 -
  |  -------------------
  |    |                   System Section
  |    |-----------------> ------------------
  |                        - Section 5      -
  |                        ------------------
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
     327948). We indicate it is an iClaustron node by setting bit 20 in
     the version number.

     Nodetype is either 1 for API's to the data servers, for data servers
     the nodetype is 0 and for cluster servers it is 2. There is a whole
     range of various API node types for iClaustron nodes which for
     NDB nodes are treated as API nodes.

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

gchar *ic_empty_string= "";
gchar *ic_err_str= "Error:";
/* Strings used in config version file */
/*
  version_str is defined in section defining strings used in MGM API
  protocol
*/
static const gchar *state_str= "state: ";
static const gchar *pid_str= "pid: ";
#define STATE_STR_LEN 7
#define PID_STR_LEN 5
#define CONFIG_STATE_IDLE 0
#define CONFIG_STATE_BUSY 1
#define CONFIG_STATE_UPDATE_CLUSTER_CONFIG 2
#define CONFIG_STATE_UPDATE_CONFIGS 3
/* Strings used in cluster configuration files */
static const gchar *cluster_str= "cluster";
static const gchar *cluster_id_string= "cluster_id";
static const gchar *cluster_name_str= "cluster_name";
static const gchar *cluster_password_str= "password";

/* Strings used in configuration files for each cluster */
static const gchar *data_server_str= "data server";
static const gchar *client_node_str= "client";
static const gchar *cluster_server_str= "cluster server";
static const gchar *sql_server_str= "sql server";
static const gchar *rep_server_str= "replication server";
static const gchar *file_server_str= "file server";
static const gchar *restore_node_str= "restore";
static const gchar *cluster_mgr_str= "cluster manager";
static const gchar *socket_str= "socket";
static const gchar *data_server_def_str= "data server default";
static const gchar *client_node_def_str= "client default";
static const gchar *cluster_server_def_str= "cluster server default";
static const gchar *sql_server_def_str= "sql server default";
static const gchar *rep_server_def_str= "replication server default";
static const gchar *file_server_def_str= "file server default";
static const gchar *restore_node_def_str= "restore default";
static const gchar *cluster_mgr_def_str= "cluster manager default";
static const gchar *socket_def_str= "socket default";
static const gchar *node_id_str= "node_id";

/* Strings used in NDB MGMAPI Protocol, first group additions */
static const gchar *get_cluster_list_str= "get cluster list";
static const gchar *get_cluster_list_reply_str= "get cluster list reply";
static const gchar *cluster_name_string= "clustername: ";
static const gchar *cluster_id_str= "clusterid: ";
static const gchar *end_get_cluster_list_str= "end get cluster list";

static const gchar *get_mgmd_nodeid_str= "get mgmd nodeid";
static const gchar *get_mgmd_nodeid_reply_str= "get mgmd nodeid reply";
static const gchar *set_connection_parameter_str= "set connection parameter";
static const gchar *set_connection_parameter_reply_str=
  "set connection parameter reply";
static const gchar *convert_transporter_str= "transporter connect";
static const gchar *report_event_str= "report event";
static const gchar *report_event_reply_str= "report event reply";
static const gchar *length_str= "length: ";
static const gchar *data_str= "data:  ";

static const gchar *get_nodeid_str= "get nodeid";
static const gchar *get_nodeid_reply_str= "get nodeid reply";
static const gchar *get_config_str= "get config";
static const gchar *get_config_reply_str= "get config reply";
static const gchar *nodeid_str= "nodeid: ";
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
#define EMPTY_LINE_REQ_STATE 10

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

/*
  These states are used in the main routine that handles
  the Cluster Server's run protocol.

  In the initial state WAIT_GET_CLUSTER_LIST we're waiting for the
  get cluster list request from iClaustron nodes.
*/
#define INITIAL_STATE 0
#define WAIT_GET_NODEID 1
#define WAIT_GET_MGMD_NODEID 2
#define WAIT_SET_CONNECTION 3
#define WAIT_CONVERT_TRANSPORTER 4

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

#define CLUSTER_NAME_REQ_LEN 13

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

#define IC_TCP_TRANSPORTER_TYPE 1

static guint32 glob_max_config_id;
IC_HASHTABLE *glob_conf_hash;
static gboolean glob_conf_entry_inited= FALSE;
static guint16 map_config_id_to_inx[MAX_MAP_CONFIG_ID];
static guint16 map_inx_to_config_id[MAX_CONFIG_ID];
static IC_CONFIG_ENTRY glob_conf_entry[MAX_CONFIG_ID];

/*
  Mandatory bits for data servers, client nodes and so forth.
  Calculated once and then used for quick access to see if a
  configuration has provided all mandatory configuration items.
*/
static guint64 data_server_mandatory_bits;
static guint64 client_mandatory_bits;
static guint64 cluster_server_mandatory_bits;
static guint64 rep_server_mandatory_bits;
static guint64 sql_server_mandatory_bits;
static guint64 comm_mandatory_bits;

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

static guint64
get_iclaustron_protocol_version(gboolean use_iclaustron_cluster_server)
{
  guint64 version_no= MYSQL_VERSION;
  if (use_iclaustron_cluster_server)
  {
    version_no+= (IC_VERSION << IC_VERSION_BIT_START);
    ic_set_bit(version_no, IC_PROTOCOL_BIT);
  }
  return version_no;
}

static gboolean
is_iclaustron_version(guint64 version_number)
{
  if (ic_is_bit_set(version_number, IC_PROTOCOL_BIT))
    return TRUE;
  return FALSE;
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
        sock2->second_node_id == sock1->first_node_id))
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
    DEBUG_RETURN(0);
  }
error:
  if (comm_hash)
    ic_hashtable_destroy(comm_hash);
  DEBUG_RETURN(1);
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
        case IC_BOOLEAN:
          printf("Data type is boolean\n");
          break;
        case IC_UINT32:
          printf("Data type is 32-bit unsigned integer\n");
          break;
        case IC_UINT64:
          printf("Data type is 64-bit unsigned integer\n");
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
      if (conf_entry->config_types & (1 << IC_DATA_SERVER_TYPE))
        printf("This config variable is used in a data server\n");
      if (conf_entry->config_types & (1 << IC_CLUSTER_SERVER_TYPE))
        printf("This config variable is used in a cluster server\n");
      if (conf_entry->config_types & (1 << IC_SQL_SERVER_TYPE))
        printf("This config variable is used in a sql server\n");
      if (conf_entry->config_types & (1 << IC_REP_SERVER_TYPE))
        printf("This config variable is used in a replication server\n");
      if (conf_entry->config_types & (1 << IC_FILE_SERVER_TYPE))
        printf("This config variable is used in a file server\n");
      if (conf_entry->config_types & (1 << IC_RESTORE_TYPE))
        printf("This config variable is used in a restore node\n");
      if (conf_entry->config_types & (1 << IC_CLUSTER_MGR_TYPE))
        printf("This config variable is used in a cluster manager\n");
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
      DEBUG_RETURN(conf_entry);
  }
  DEBUG_RETURN(NULL);
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

  data_server_mandatory_bits= 0;
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
      if (conf_entry->config_types & (1 << IC_DATA_SERVER_TYPE))
        data_server_mandatory_bits|= (1 << conf_entry->mandatory_bit);
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
    DEBUG_DECL(gchar buf[128]);
    DEBUG_PRINT(CONFIG_LEVEL, ("data_server_mandatory_bits = %s",
                      ic_guint64_hex_str(data_server_mandatory_bits, buf)));
    DEBUG_PRINT(CONFIG_LEVEL, ("client_mandatory_bits = %s",
                 ic_guint64_hex_str(client_mandatory_bits, buf)));
    DEBUG_PRINT(CONFIG_LEVEL, ("cluster_server_mandatory_bits = %s",
                 ic_guint64_hex_str(cluster_server_mandatory_bits, buf)));
    DEBUG_PRINT(CONFIG_LEVEL, ("rep_server_mandatory_bits = %s",
                 ic_guint64_hex_str(rep_server_mandatory_bits, buf)));
    DEBUG_PRINT(CONFIG_LEVEL, ("sql_server_mandatory_bits = %s",
                 ic_guint64_hex_str(sql_server_mandatory_bits, buf)));
    DEBUG_PRINT(CONFIG_LEVEL, ("comm_mandatory_bits = %s",
                  ic_guint64_hex_str(comm_mandatory_bits, buf)));
  }
  DEBUG_RETURN_EMPTY;
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
  We add 1000 to the system configuration items.
  Id 10-15 for configuration ids of system
*/
#define SYSTEM_PRIMARY_CS_NODE 1001
#define SYSTEM_CONFIGURATION_NUMBER 1002
#define SYSTEM_NAME 1003

  IC_SET_CONFIG_MAP(SYSTEM_PRIMARY_CS_NODE, 10);
  IC_SET_SYSTEM_CONFIG(conf_entry, system_primary_cs_node,
                       IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, IC_MAX_NODE_ID);
  conf_entry->config_entry_description=
  "Primary Cluster Server node in the grid";

  IC_SET_CONFIG_MAP(SYSTEM_CONFIGURATION_NUMBER, 11);
  IC_SET_SYSTEM_CONFIG(conf_entry, system_configuration_number,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  conf_entry->config_entry_description=
  "Configuration number of grid";

  IC_SET_CONFIG_MAP(SYSTEM_NAME, 12);
  IC_SET_SYSTEM_STRING(conf_entry, system_name,
                       IC_NOT_CHANGEABLE);
  conf_entry->default_string= (gchar*)ic_empty_string;
  conf_entry->config_entry_description=
  "Cluster name";
/*
  This is the data server node configuration section.
*/
/* Id 0-9 for configuration id 0-9 */
/* Id 0 not used */
#define DATA_SERVER_INJECT_FAULT 1
/* Id 2 not used */
#define IC_NODE_ID       3
/* Id 4 not used */
#define IC_NODE_HOST     5
/* Id 6 not used */
#define IC_NODE_DATA_PATH 7
/* Id 8 not used */
#define IC_NETWORK_BUFFER_SIZE 9

  IC_SET_CONFIG_MAP(DATA_SERVER_INJECT_FAULT, 1);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, inject_fault,
                       IC_UINT32, 2, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 2);
  conf_entry->config_entry_description=
  "Inject faults (only available in special test builds)";

  /* These parameters are common for most node types */
  IC_SET_CONFIG_MAP(IC_NODE_ID, 3);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, IC_MAX_NODE_ID);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, node_id, IC_UINT32,
                       0, IC_NOT_CHANGEABLE);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_CLIENT_TYPE) +
                            (1 << IC_DATA_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Node id";

  IC_SET_CONFIG_MAP(IC_NODE_HOST, 5);
  IC_SET_DATA_SERVER_STRING(conf_entry, hostname, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->default_string= (gchar*)ic_empty_string;
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_CLIENT_TYPE) +
                            (1 << IC_DATA_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Hostname of the node";

  IC_SET_CONFIG_MAP(IC_NODE_DATA_PATH, 7);
  IC_SET_DATA_SERVER_STRING(conf_entry, node_data_path,
                            IC_INITIAL_NODE_RESTART);
  conf_entry->default_string= (gchar*)ic_empty_string;
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_DATA_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Data directory of the node";

  IC_SET_CONFIG_MAP(IC_NETWORK_BUFFER_SIZE, 9);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, network_buffer_size, IC_UINT32,
                            12 * 1024 * 1024,
                            IC_ONLINE_CHANGE);
  conf_entry->config_types= (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_CLIENT_TYPE) +
                            (1 << IC_DATA_SERVER_TYPE);
  conf_entry->config_entry_description=
  "The total size of the network buffers used in the node";

/* Id 16-19 for configuration id 10-99 */
/* Id 10-99 not used */

/* Id 20-29 for configuration id 100-109 */
#define DATA_SERVER_MAX_TRACE_FILES 100
#define DATA_SERVER_REPLICAS 101
#define DATA_SERVER_TABLE_OBJECTS 102
#define DATA_SERVER_COLUMN_OBJECTS 103
#define DATA_SERVER_KEY_OBJECTS 104
#define DATA_SERVER_INTERNAL_TRIGGER_OBJECTS 105
#define DATA_SERVER_CONNECTION_OBJECTS 106
#define DATA_SERVER_OPERATION_OBJECTS 107
#define DATA_SERVER_SCAN_OBJECTS 108
#define DATA_SERVER_INTERNAL_TRIGGER_OPERATION_OBJECTS 109

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_TRACE_FILES, 20);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, max_number_of_trace_files,
                       IC_UINT32, 25, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 2048);
  conf_entry->config_entry_description=
  "The number of crashes that can be reported before we overwrite error log and trace files";

  IC_SET_CONFIG_MAP(DATA_SERVER_REPLICAS, 21);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_replicas,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 4);
  conf_entry->is_mandatory= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "This defines number of nodes per node group, within a node group all nodes contain the same data";

  IC_SET_CONFIG_MAP(DATA_SERVER_TABLE_OBJECTS, 22);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_table_objects,
                       IC_UINT32, 256, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of tables that can be stored in cluster";

  IC_SET_CONFIG_MAP(DATA_SERVER_COLUMN_OBJECTS, 23);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_column_objects,
                       IC_UINT32, 2048, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 256);
  conf_entry->config_entry_description=
  "Sets the maximum number of columns that can be stored in cluster";

  IC_SET_CONFIG_MAP(DATA_SERVER_KEY_OBJECTS, 24);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_key_objects,
                       IC_UINT32, 256, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(DATA_SERVER_INTERNAL_TRIGGER_OBJECTS, 25);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_internal_trigger_objects,
                       IC_UINT32, 1536, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 512);
  conf_entry->config_entry_description=
  "Each unique index will use 3 internal trigger objects, index/backup will use 1 per table";

  IC_SET_CONFIG_MAP(DATA_SERVER_CONNECTION_OBJECTS, 26);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_connection_objects,
                       IC_UINT32, 8192, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128);
  conf_entry->config_entry_description=
  "Each active transaction and active scan uses a connection object";

  IC_SET_CONFIG_MAP(DATA_SERVER_OPERATION_OBJECTS, 27);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_operation_objects,
                       IC_UINT32, 32768, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024);
  conf_entry->config_entry_description=
  "Each record read/updated in a transaction uses an operation object during the transaction";

  IC_SET_CONFIG_MAP(DATA_SERVER_SCAN_OBJECTS, 28);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_scan_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 32, 512);
  conf_entry->config_entry_description=
  "Each active scan uses a scan object for the lifetime of the scan operation";

  IC_SET_CONFIG_MAP(DATA_SERVER_INTERNAL_TRIGGER_OPERATION_OBJECTS, 29);
  IC_SET_DATA_SERVER_CONFIG(conf_entry,
                            number_of_internal_trigger_operation_objects,
                       IC_UINT32, 4000, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4000, 4000);
  conf_entry->config_entry_description=
  "Each internal trigger that is fired uses an operation object for a short time";

/* Id 30-39 for configuration id 110-119 */
#define DATA_SERVER_KEY_OPERATION_OBJECTS 110
#define DATA_SERVER_CONNECTION_BUFFER 111
#define DATA_SERVER_RAM_MEMORY 112
#define DATA_SERVER_HASH_MEMORY 113
#define DATA_SERVER_MEMORY_LOCKED 114
#define DATA_SERVER_WAIT_PARTIAL_START 115
#define DATA_SERVER_WAIT_PARTITIONED_START 116
#define DATA_SERVER_WAIT_ERROR_START 117
#define DATA_SERVER_HEARTBEAT_TIMER 118
#define DATA_SERVER_CLIENT_HEARTBEAT_TIMER 119

  IC_SET_CONFIG_MAP(DATA_SERVER_KEY_OPERATION_OBJECTS, 30);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_key_operation_objects,
                       IC_UINT32, 4096, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 128);
  conf_entry->config_entry_description=
  "Each read and update of an unique hash index in a transaction uses one of those objects";

  IC_SET_CONFIG_MAP(DATA_SERVER_CONNECTION_BUFFER, 31);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, size_of_connection_buffer,
                       IC_UINT32, 1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1024*1024, 1024*1024);
  conf_entry->config_entry_description=
  "Internal buffer used by connections by transactions and scans";

  IC_SET_CONFIG_MAP(DATA_SERVER_RAM_MEMORY, 32);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, size_of_ram_memory,
                       IC_UINT64, 256*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 16*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory used to store RAM-based records";

  IC_SET_CONFIG_MAP(DATA_SERVER_HASH_MEMORY, 33);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, size_of_hash_memory,
                       IC_UINT64, 64*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 8*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory used to store primary hash index on all tables and unique hash indexes";

  IC_SET_CONFIG_MAP(DATA_SERVER_MEMORY_LOCKED, 34);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry, use_unswappable_memory, FALSE,
                             IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Setting this to 1 means that all memory is locked and will not be swapped out";

  IC_SET_CONFIG_MAP(DATA_SERVER_WAIT_PARTIAL_START, 35);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_wait_partial_start,
                       IC_UINT32, 20000, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting with a partial set of nodes, 0 waits forever";

  IC_SET_CONFIG_MAP(DATA_SERVER_WAIT_PARTITIONED_START, 36);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_wait_partitioned_start,
                       IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before starting a potentially partitioned cluster, 0 waits forever";

  IC_SET_CONFIG_MAP(DATA_SERVER_WAIT_ERROR_START, 37);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_wait_error_start,
                       IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Time in ms cluster will wait before forcing a stop after an error, 0 waits forever";

  IC_SET_CONFIG_MAP(DATA_SERVER_HEARTBEAT_TIMER, 38);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_heartbeat_data_server_nodes,
                       IC_UINT32, 700, IC_ROLLING_UPGRADE_CHANGE_SPECIAL);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to data servers, 4 missed leads to node crash";

  IC_SET_CONFIG_MAP(DATA_SERVER_CLIENT_HEARTBEAT_TIMER, 39);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_heartbeat_client_nodes,
                       IC_UINT32, 1000, IC_ROLLING_UPGRADE_CHANGE_SPECIAL);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between sending heartbeat messages to client nodes, 4 missed leads to node crash";

/* Id 40-49 for configuration id 120-129 */
#define DATA_SERVER_LOCAL_CHECKPOINT_TIMER 120
#define DATA_SERVER_GLOBAL_CHECKPOINT_TIMER 121
#define DATA_SERVER_RESOLVE_TIMER 122
#define DATA_SERVER_WATCHDOG_TIMER 123
#define DATA_SERVER_DAEMON_RESTART_AT_ERROR 124
#define DATA_SERVER_FILESYSTEM_PATH 125
#define DATA_SERVER_REDO_LOG_FILES 126
/* 127 and 128 deprecated */
#define DATA_SERVER_CHECK_INTERVAL 129

  IC_SET_CONFIG_MAP(DATA_SERVER_LOCAL_CHECKPOINT_TIMER, 40);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_local_checkpoint,
                       IC_UINT32, 24, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 6, 31);
  conf_entry->config_entry_description=
  "Specifies how often local checkpoints are executed, logarithmic scale on log size";

  IC_SET_CONFIG_MAP(DATA_SERVER_GLOBAL_CHECKPOINT_TIMER, 41);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_global_checkpoint,
                       IC_UINT32, 1000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms between starting global checkpoints";

  IC_SET_CONFIG_MAP(DATA_SERVER_RESOLVE_TIMER, 42);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_resolve,
                       IC_UINT32, 2000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 10);
  conf_entry->config_entry_description=
  "Time in ms waiting for response from resolve";

  IC_SET_CONFIG_MAP(DATA_SERVER_WATCHDOG_TIMER, 43);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_data_server_watchdog,
                       IC_UINT32, 6000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms without activity in data server before watchdog is fired";

  IC_SET_CONFIG_MAP(DATA_SERVER_DAEMON_RESTART_AT_ERROR, 44);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry, data_server_automatic_restart, TRUE,
                             IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "If set, data server restarts automatically after a failure";

  IC_SET_CONFIG_MAP(DATA_SERVER_FILESYSTEM_PATH, 45);
  IC_SET_DATA_SERVER_STRING(conf_entry, filesystem_path,
                            IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Path to filesystem of data server";

  IC_SET_CONFIG_MAP(DATA_SERVER_REDO_LOG_FILES, 46);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_redo_log_files,
                       IC_UINT32, 32, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN(conf_entry, 4);
  conf_entry->config_entry_description=
  "Number of REDO log files, each file represents 64 MB log space";

  IC_SET_CONFIG_MAP(127, 47);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(128, 48);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECK_INTERVAL, 49);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_check_interval,
                       IC_UINT32, 500, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 500, 500);
  conf_entry->config_entry_description=
  "Time in ms between checks after transaction timeouts";

/* Id 50-59 for configuration id 130-139 */
#define DATA_SERVER_CLIENT_ACTIVITY_TIMER 130
#define DATA_SERVER_DEADLOCK_TIMER 131
#define DATA_SERVER_CHECKPOINT_OBJECTS 132
#define DATA_SERVER_CHECKPOINT_MEMORY 133
#define DATA_SERVER_CHECKPOINT_DATA_MEMORY 134
#define DATA_SERVER_CHECKPOINT_LOG_MEMORY 135
#define DATA_SERVER_CHECKPOINT_WRITE_SIZE 136
/* 137 and 138 deprecated */
#define DATA_SERVER_CHECKPOINT_MAX_WRITE_SIZE 139

  IC_SET_CONFIG_MAP(DATA_SERVER_CLIENT_ACTIVITY_TIMER, 50);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_client_activity,
                       IC_UINT32, 1024*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to client inactivity";

  IC_SET_CONFIG_MAP(DATA_SERVER_DEADLOCK_TIMER, 51);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, timer_deadlock,
                       IC_UINT32, 2000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1000);
  conf_entry->config_entry_description=
  "Time in ms before transaction is aborted due to internal wait (indication of deadlock)";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_OBJECTS, 52);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_checkpoint_objects,
                       IC_UINT32, 1, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 1);
  conf_entry->config_entry_description=
  "Number of possible parallel backups and local checkpoints";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_MEMORY, 53);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, checkpoint_memory,
                       IC_UINT32, 4*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4*1024*1024, 4*1024*1024);
  conf_entry->config_entry_description=
  "Size of memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_DATA_MEMORY, 54);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, checkpoint_data_memory,
                       IC_UINT32, 2*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 2*1024*1024, 2*1024*1024);
  conf_entry->config_entry_description=
  "Size of data memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_LOG_MEMORY, 55);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, checkpoint_log_memory,
                       IC_UINT32, 2*1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 2*1024*1024, 2*1024*1024);
  conf_entry->config_entry_description=
  "Size of log memory buffers for local checkpoint and backup";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_WRITE_SIZE, 56);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, checkpoint_write_size,
                       IC_UINT32, 64*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64*1024, 64*1024);
  conf_entry->config_entry_description=
  "Size of default writes in local checkpoint and backups";

  IC_SET_CONFIG_MAP(137, 57);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(138, 58);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_MAX_WRITE_SIZE, 59);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, checkpoint_max_write_size,
                       IC_UINT32, 256*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 256*1024, 256*1024);
  conf_entry->config_entry_description=
  "Size of maximum writes in local checkpoint and backups";

/* Id 60-69 for configuration id 140-149 */
#define DATA_SERVER_SIZE_OF_REDO_LOG_FILES 140
#define DATA_SERVER_INITIAL_WATCHDOG_TIMER 141
/* 142 - 146 not used */
/* 147 Cluster Server parameter */
#define CLUSTER_SERVER_EVENT_LOG 147
#define DATA_SERVER_VOLATILE_MODE 148
#define DATA_SERVER_ORDERED_KEY_OBJECTS 149

  IC_SET_CONFIG_MAP(DATA_SERVER_SIZE_OF_REDO_LOG_FILES, 60);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, size_of_redo_log_files,
                       IC_UINT32, 16 * 1024 * 1024, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 4*1024*1024, 2000*1024*1024);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Size of REDO log files";

  IC_SET_CONFIG_MAP(DATA_SERVER_INITIAL_WATCHDOG_TIMER, 61);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_initial_watchdog_timer,
                       IC_UINT32, 15000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 100);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Initial value of watchdog timer before communication set-up";

  /* This is a cluster server parameter */
/*
  We currently don't use this variable since we might use the cluster
  manager for logging rather.

  IC_SET_CONFIG_MAP(CLUSTER_SERVER_EVENT_LOG, 67);
  IC_SET_CLUSTER_SERVER_STRING(conf_entry, cluster_server_event_log,
                               ic_empty_string, IC_INITIAL_NODE_RESTART);
  conf_entry->is_not_sent= TRUE;
  conf_entry->config_entry_description=
  "Type of cluster event log";
*/

  IC_SET_CONFIG_MAP(DATA_SERVER_VOLATILE_MODE, 68);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry, data_server_volatile_mode, FALSE,
                             IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "In this mode all file writes are ignored and all starts becomes initial starts";

  IC_SET_CONFIG_MAP(DATA_SERVER_ORDERED_KEY_OBJECTS, 69);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_ordered_key_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of ordered keys that can be stored in cluster";

/* Id 70-79 for configuration id 150-159 */
#define DATA_SERVER_UNIQUE_HASH_KEY_OBJECTS 150
/* 151 and 152 deprecated */
#define DATA_SERVER_SCAN_BATCH_SIZE 153
/* 154 and 155 deprecated */
#define DATA_SERVER_REDO_LOG_MEMORY 156
#define DATA_SERVER_LONG_MESSAGE_MEMORY 157
#define DATA_SERVER_CHECKPOINT_PATH 158
#define DATA_SERVER_MAX_OPEN_FILES 159

  IC_SET_CONFIG_MAP(DATA_SERVER_UNIQUE_HASH_KEY_OBJECTS, 70);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, number_of_unique_hash_key_objects,
                       IC_UINT32, 128, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 32);
  conf_entry->config_entry_description=
  "Sets the maximum number of unique hash keys that can be stored in cluster";

  IC_SET_CONFIG_MAP(151, 71);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(152, 72);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(DATA_SERVER_SCAN_BATCH_SIZE, 73);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, size_of_scan_batch,
                       IC_UINT32, 64, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 64, 64);
  conf_entry->config_entry_description=
  "Number of records sent in a scan from the local data server node";

  IC_SET_CONFIG_MAP(154, 74);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(155, 75);
  conf_entry->is_deprecated= TRUE;

  IC_SET_CONFIG_MAP(DATA_SERVER_REDO_LOG_MEMORY, 76);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, redo_log_memory,
                       IC_UINT32, 16*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Size of REDO log memory buffer";

  IC_SET_CONFIG_MAP(DATA_SERVER_LONG_MESSAGE_MEMORY, 77);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, long_message_memory,
                       IC_UINT32, 1024*1024, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1024*1024, 1024*1024);
  conf_entry->config_entry_description=
  "Size of long memory buffers";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_PATH, 78);
  IC_SET_DATA_SERVER_STRING(conf_entry, data_server_checkpoint_path,
                           IC_INITIAL_NODE_RESTART);
  conf_entry->config_entry_description=
  "Path to filesystem of checkpoints";
  conf_entry->is_mandatory= TRUE;

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_OPEN_FILES, 79);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_max_open_files,
                       IC_UINT32, 40, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 40, 40);
  conf_entry->config_entry_description=
  "Maximum number of open files in data server node";

/* Id 80-89 for configuration id 160-169 */
#define DATA_SERVER_PAGE_CACHE_SIZE 160
#define DATA_SERVER_STRING_MEMORY 161
#define DATA_SERVER_INITIAL_OPEN_FILES 162
#define DATA_SERVER_FILE_SYNCH_SIZE 163
#define DATA_SERVER_DISK_WRITE_SPEED 164
#define DATA_SERVER_DISK_WRITE_SPEED_START 165
#define DATA_SERVER_REPORT_MEMORY_FREQUENCY 166
#define DATA_SERVER_BACKUP_STATUS_FREQUENCY 167
#define DATA_SERVER_USE_O_DIRECT 168
#define DATA_SERVER_MAX_ALLOCATE_SIZE 169

  IC_SET_CONFIG_MAP(DATA_SERVER_PAGE_CACHE_SIZE, 80);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, page_cache_size,
                       IC_UINT64, 128*1024*1024, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 64*1024);
  conf_entry->config_entry_description=
  "Size of page cache for disk-based data";

  IC_SET_CONFIG_MAP(DATA_SERVER_STRING_MEMORY, 81);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, size_of_string_memory,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Size of string memory";

  IC_SET_CONFIG_MAP(DATA_SERVER_INITIAL_OPEN_FILES, 82);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_open_files,
                       IC_UINT32, 27, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 27, 27);
  conf_entry->config_entry_description=
  "Number of open file handles in data server from start";

  IC_SET_CONFIG_MAP(DATA_SERVER_FILE_SYNCH_SIZE, 83);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_file_synch_size,
                       IC_UINT32, 4*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Size of file writes before a synch is always used";

  IC_SET_CONFIG_MAP(DATA_SERVER_DISK_WRITE_SPEED, 84);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_disk_write_speed,
                       IC_UINT32, 8*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 64*1024);
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk";

  IC_SET_CONFIG_MAP(DATA_SERVER_DISK_WRITE_SPEED_START, 85);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_disk_write_speed_start,
                       IC_UINT32, 256*1024*1024, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN(conf_entry, 1024*1024);
  conf_entry->config_entry_description=
  "Limit on how fast checkpoints are allowed to write to disk during start of the node";

  IC_SET_CONFIG_MAP(DATA_SERVER_REPORT_MEMORY_FREQUENCY, 86)
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_report_memory_frequency,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "Frequency of memory reports, 0 means only at certain thresholds";

  IC_SET_CONFIG_MAP(DATA_SERVER_BACKUP_STATUS_FREQUENCY, 87)
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_backup_status_frequency,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  conf_entry->config_entry_description=
  "Frequency of backup status, 0 means no status reporting except at end";

  IC_SET_CONFIG_MAP(DATA_SERVER_USE_O_DIRECT, 88);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry, use_o_direct, TRUE,
                             IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Use O_DIRECT on file system of data servers";

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_ALLOCATE_SIZE, 89);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_max_allocate_size,
                       IC_UINT32, 32*1024*1024, IC_INITIAL_NODE_RESTART);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1*1024*1024, 1000*1024*1024);
  conf_entry->min_ndb_version_used= 0x50119;
  conf_entry->config_entry_description=
  "Size of maximum extent allocated at a time for table memory";

/* Id 90-99 for configuration id 170-179 */
#define DATA_SERVER_GROUP_COMMIT_DELAY 170
#define DATA_SERVER_GROUP_COMMIT_TIMEOUT 171
#define DATA_SERVER_BACKUP_COMPRESSION 172
#define DATA_SERVER_LOCAL_CHECKPOINT_COMPRESSION 173
#define DATA_SERVER_SCHEDULER_NO_SEND_TIME 174
#define DATA_SERVER_SCHEDULER_NO_SLEEP_TIME 175
#define DATA_SERVER_RT_SCHEDULER_THREADS 176
#define DATA_SERVER_LOCK_MAIN_THREAD 177
#define DATA_SERVER_LOCK_OTHER_THREADS 178
#define DATA_SERVER_MAX_LOCAL_TRIGGERS 179

  IC_SET_CONFIG_MAP(DATA_SERVER_GROUP_COMMIT_DELAY, 90);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_group_commit_delay,
                       IC_UINT32, 100, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 32000);
  conf_entry->min_ndb_version_used= 0x60301;
  conf_entry->config_entry_description=
  "How long is the delay between each group commit started by the cluster";

  IC_SET_CONFIG_MAP(DATA_SERVER_GROUP_COMMIT_TIMEOUT, 91);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_group_commit_timeout,
                            IC_UINT32, 4000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 32000);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Maximum time group commit is allowed to take before node is shut down";

  IC_SET_CONFIG_MAP(DATA_SERVER_BACKUP_COMPRESSION, 92);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry, data_server_backup_compression,
                             FALSE, IC_INITIAL_NODE_RESTART);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Use zlib compression for NDB online backups";

  IC_SET_CONFIG_MAP(DATA_SERVER_LOCAL_CHECKPOINT_COMPRESSION, 93);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry,
                             data_server_local_checkpoint_compression,
                             FALSE, IC_INITIAL_NODE_RESTART);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Use zlib compression for NDB local checkpoints";

  IC_SET_CONFIG_MAP(DATA_SERVER_SCHEDULER_NO_SEND_TIME, 94);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_scheduler_no_send_time,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without sending socket buffers";

  IC_SET_CONFIG_MAP(DATA_SERVER_SCHEDULER_NO_SLEEP_TIME, 95);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_scheduler_no_sleep_time,
                       IC_UINT32, 0, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 1000);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "How long time can the scheduler execute without going to sleep";

  IC_SET_CONFIG_MAP(DATA_SERVER_RT_SCHEDULER_THREADS, 96);
  IC_SET_DATA_SERVER_BOOLEAN(conf_entry, data_server_rt_scheduler_threads,
                             FALSE, IC_ONLINE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "If set the data server is setting its thread in RT priority, requires root privileges";

  IC_SET_CONFIG_MAP(DATA_SERVER_LOCK_MAIN_THREAD, 97);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_lock_main_thread,
                       IC_UINT32, 65535, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 65535);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "Lock Main Thread to a CPU id";

  IC_SET_CONFIG_MAP(DATA_SERVER_LOCK_OTHER_THREADS, 98);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_lock_main_thread,
                       IC_UINT32, 65535, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 65535);
  conf_entry->min_ndb_version_used= 0x60304;
  conf_entry->config_entry_description=
  "Lock other threads to a CPU id";

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_LOCAL_TRIGGERS, 99);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_max_local_triggers,
                            IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Max number of local triggers that can be defined";

/* Id 100-109 for configuration id 180-189 */
#define DATA_SERVER_MAX_LOCAL_TRIGGER_USERS 180
#define DATA_SERVER_MAX_LOCAL_TRIGGER_OPERATIONS 181
#define DATA_SERVER_MAX_STORED_GROUP_COMMITS 182
#define DATA_SERVER_LOCAL_TRIGGER_HANDOVER_TIMEOUT 183
#define DATA_SERVER_REPORT_STARTUP_FREQUENCY 184
#define DATA_SERVER_NODE_GROUP 185
#define DATA_SERVER_THREADS 186
#define DATA_SERVER_LOCAL_DB_THREADS 187
#define DATA_SERVER_LOCAL_DB_WORKERS 188
#define DATA_SERVER_ZERO_REDO_LOG 189

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_LOCAL_TRIGGER_USERS, 100);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_max_local_trigger_users,
                            IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Max number of local trigger user nodes";

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_LOCAL_TRIGGER_OPERATIONS, 101);
  IC_SET_DATA_SERVER_CONFIG(conf_entry,
                            data_server_max_local_trigger_operations,
                            IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Max number of local trigger operations";

  IC_SET_CONFIG_MAP(DATA_SERVER_MAX_STORED_GROUP_COMMITS, 102);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_max_stored_group_commits,
                            IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Max number of group commits we will store information about";

  IC_SET_CONFIG_MAP(DATA_SERVER_LOCAL_TRIGGER_HANDOVER_TIMEOUT, 103);
  IC_SET_DATA_SERVER_CONFIG(conf_entry,
                            data_server_local_trigger_handover_timeout,
                            IC_UINT32, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60316;
  conf_entry->config_entry_description=
  "Maximum time to wait when performing a handover during local trigger definitions";

  IC_SET_CONFIG_MAP(DATA_SERVER_REPORT_STARTUP_FREQUENCY, 104);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_report_startup_frequency,
                            IC_UINT32, 0, IC_ONLINE_CHANGE);
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "How often to issue status reports during startup of the ndb kernel";

  IC_SET_CONFIG_MAP(DATA_SERVER_NODE_GROUP, 105);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_node_group,
                            IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, IC_MAX_NODE_ID);
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "Node group of ndbd node";

  IC_SET_CONFIG_MAP(DATA_SERVER_THREADS, 106);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_threads,
                            IC_UINT32, 8, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 3, 8);
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "Number of threads that can be used maximally for ndbd node";

  IC_SET_CONFIG_MAP(DATA_SERVER_LOCAL_DB_THREADS, 107);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_local_db_threads,
                            IC_UINT32, 4, IC_NODE_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 4);
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "Number of threads used by local database part in ndbd";
  
  IC_SET_CONFIG_MAP(DATA_SERVER_LOCAL_DB_WORKERS, 108);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_local_db_workers,
                            IC_UINT32, 4, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, 4);
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "Number of partitions used by local database part in ndbd";

  IC_SET_CONFIG_MAP(DATA_SERVER_ZERO_REDO_LOG, 109);
  IC_SET_DATA_SERVER_STRING(conf_entry, data_server_zero_redo_log,
                            IC_CLUSTER_RESTART_CHANGE);
  conf_entry->default_string= "sparse";
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "Initialise REDO log during initial start (sparse or full)";

/* Id 110-119 for configuration id 190-199 */
/* 191-197 not used */
#define DATA_SERVER_FILE_THREAD_POOL 190
#define DATA_SERVER_MEMORY_POOL 198
/* 199 used, not for normal builds though */
  IC_SET_CONFIG_MAP(DATA_SERVER_FILE_THREAD_POOL, 110);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_file_thread_pool,
                            IC_UINT32, 8, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->min_ndb_version_used= 0x60401;
  conf_entry->config_entry_description=
  "Number of threads used for Disk Data file threads";

  IC_SET_CONFIG_MAP(DATA_SERVER_MEMORY_POOL, 118);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, data_server_memory_pool,
                       IC_UINT64, 0, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_entry_description=
  "Size of memory pool for internal memory usage";

/* Client/Cluster Server configuration items */
/* Id 120-129 for configuration id 200-209 */
#define CLIENT_RESOLVE_RANK 200
#define CLIENT_RESOLVE_TIMER 201
#define RESERVED_SEND_BUFFER 202

  IC_SET_CONFIG_MAP(CLIENT_RESOLVE_RANK, 120);
  IC_SET_CLIENT_CONFIG(conf_entry, client_resolve_rank,
                       IC_UINT32, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 2);
  conf_entry->config_types= (1 << IC_CLIENT_TYPE) +
                            (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Rank in resolving network partition of the client";

  IC_SET_CONFIG_MAP(CLIENT_RESOLVE_TIMER, 121);
  IC_SET_CLIENT_CONFIG(conf_entry, client_resolve_timer,
                       IC_UINT32, 0, IC_CLUSTER_RESTART_CHANGE);
  conf_entry->config_types= (1 << IC_CLIENT_TYPE) +
                            (1 << IC_CLUSTER_SERVER_TYPE);
  conf_entry->config_entry_description=
  "Time in ms waiting for resolve before crashing";

  IC_SET_CONFIG_MAP(RESERVED_SEND_BUFFER, 122);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, reserved_send_buffer,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Send buffer memory reserved for ndbd traffic, not used";

/* Id 130-139 for configuration id 210-249 */
/* Id 210-249 not used */

/* Log level configuration items */
/* Id 140-149 for configuration id 250-259 */
#define DATA_SERVER_START_LOG_LEVEL 250
#define DATA_SERVER_STOP_LOG_LEVEL 251
#define DATA_SERVER_STAT_LOG_LEVEL 252
#define DATA_SERVER_CHECKPOINT_LOG_LEVEL 253
#define DATA_SERVER_RESTART_LOG_LEVEL 254
#define DATA_SERVER_CONNECTION_LOG_LEVEL 255
#define DATA_SERVER_REPORT_LOG_LEVEL 256
#define DATA_SERVER_WARNING_LOG_LEVEL 257
#define DATA_SERVER_ERROR_LOG_LEVEL 258
#define DATA_SERVER_CONGESTION_LOG_LEVEL 259

  IC_SET_CONFIG_MAP(DATA_SERVER_START_LOG_LEVEL, 140);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_start,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at start of a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_STOP_LOG_LEVEL, 141);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_stop,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at stop of a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_STAT_LOG_LEVEL, 142);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_statistics,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of statistics on a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_CHECKPOINT_LOG_LEVEL, 143);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_checkpoint,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at checkpoint of a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_RESTART_LOG_LEVEL, 144);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_restart,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level at restart of a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_CONNECTION_LOG_LEVEL, 145);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_connection,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of connections to a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_REPORT_LOG_LEVEL, 146);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_reports,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of reports from a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_WARNING_LOG_LEVEL, 147);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_warning,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of warnings from a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_ERROR_LOG_LEVEL, 148);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_error,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of errors from a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_CONGESTION_LOG_LEVEL, 149);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_congestion,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of congestions to a node";

/* Id 150-159 for configuration id 260-269 */
#define DATA_SERVER_DEBUG_LOG_LEVEL 260
#define DATA_SERVER_BACKUP_LOG_LEVEL 261
/* Id 262-269 not used */

  IC_SET_CONFIG_MAP(DATA_SERVER_DEBUG_LOG_LEVEL, 150);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_debug,
                       IC_UINT32, 8, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 15);
  conf_entry->config_entry_description=
  "Log level of debug messages from a node";

  IC_SET_CONFIG_MAP(DATA_SERVER_BACKUP_LOG_LEVEL, 151);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, log_level_backup,
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
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, IC_MAX_NODE_ID);
  conf_entry->is_mandatory= TRUE;
  conf_entry->is_key= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "First node id of the connection";

  IC_SET_CONFIG_MAP(SOCKET_SECOND_NODE_ID, 181);
  IC_SET_SOCKET_CONFIG(conf_entry, second_node_id,
                       IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, IC_MAX_NODE_ID);
  conf_entry->is_mandatory= TRUE;
  conf_entry->is_key= TRUE;
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
  conf_entry->is_derived_default= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Port number to use on server side";

  IC_SET_CONFIG_MAP(SOCKET_FIRST_HOSTNAME, 187);
  IC_SET_SOCKET_STRING(conf_entry, first_hostname, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->is_derived_default= TRUE;
  conf_entry->config_entry_description=
  "Hostname of first node";

  IC_SET_CONFIG_MAP(SOCKET_SECOND_HOSTNAME, 188);
  IC_SET_SOCKET_STRING(conf_entry, second_hostname, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->is_derived_default= TRUE;
  conf_entry->config_entry_description=
  "Hostname of second node";

  IC_SET_CONFIG_MAP(SOCKET_GROUP, 189);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_group,
                       IC_UINT16, 55, IC_ROLLING_UPGRADE_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 55, 55);
  conf_entry->config_entry_description=
  "Group id of the connection";

/* Id 190-209 for configuration id 410-499 */
#define SOCKET_SERVER_NODE_ID 410
#define SOCKET_OVERLOAD 411
/* Id 412-419 not used */
#define SOCKET_CLIENT_PORT_NUMBER 420
/* Id 421-453 not used */
#define SOCKET_WRITE_BUFFER_SIZE 454
#define SOCKET_READ_BUFFER_SIZE 455
/* Id 456 not used */
#define SOCKET_KERNEL_READ_BUFFER_SIZE 457
#define SOCKET_KERNEL_WRITE_BUFFER_SIZE 458
#define SOCKET_MAXSEG_SIZE 459
#define SOCKET_BIND_ADDRESS 460
#define SOCKET_MAX_WAIT_IN_NANOS 489
/* Id 461-488 not used */
/* Id 490-499 not used */

  IC_SET_CONFIG_MAP(SOCKET_SERVER_NODE_ID, 190);
  IC_SET_SOCKET_CONFIG(conf_entry, server_node_id,
                       IC_UINT16, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 1, IC_MAX_NODE_ID);
  conf_entry->is_mandatory= TRUE;
  conf_entry->is_derived_default= TRUE;
  conf_entry->mandatory_bit= mandatory_bits++;
  conf_entry->config_entry_description=
  "Node id of node that is server part of connection";

  IC_SET_CONFIG_MAP(SOCKET_OVERLOAD, 191);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_overload,
                       IC_UINT32, 0, IC_NOT_CHANGEABLE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, 0, 0);
  conf_entry->config_entry_description=
  "Number of bytes before overload declared, deprecated";

  IC_SET_CONFIG_MAP(SOCKET_CLIENT_PORT_NUMBER, 193);
  IC_SET_SOCKET_CONFIG(conf_entry, client_port_number,
                       IC_UINT16, 0, IC_CLUSTER_RESTART_CHANGE);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  conf_entry->is_derived_default= TRUE;
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

  /*
    The parameter socket_max_wait_in_nanos is used in conjunction with
    the adaptive send protocol. It's possible to set the maximum time
    a message can be waiting for sending before it's being forced to
    send as soon as discovered in this state. By setting it to zero one
    effectively disables the adaptive send protocol.

    This parameter is only used when the application has asked for a non-
    forced send. Thus forced send will always be sent immediately and
    non-forced sends will not wait for longer than this configured time.
    This parameter can be changed at any time.
  */
  IC_SET_CONFIG_MAP(SOCKET_MAX_WAIT_IN_NANOS, 201);
  IC_SET_SOCKET_CONFIG(conf_entry, socket_max_wait_in_nanos,
                       IC_UINT32, 50 * 1000, IC_ONLINE_CHANGE);
  IC_SET_CONFIG_MAX(conf_entry, 1000 * 1000 * 10);
  conf_entry->is_only_iclaustron= TRUE;
  conf_entry->config_entry_description=
  "Maximum time a message can wait before being sent in nanoseconds";

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

  /* Port number is used both by clients and data servers */
  IC_SET_CONFIG_MAP(IC_PORT_NUMBER, 235);
  IC_SET_CONFIG_MIN_MAX(conf_entry, MIN_PORT, MAX_PORT);
  IC_SET_DATA_SERVER_CONFIG(conf_entry, port_number, IC_UINT32,
                       DEF_PORT, IC_ROLLING_UPGRADE_CHANGE);
  conf_entry->config_types= (1 << IC_CLIENT_TYPE) +
                            (1 << IC_CLUSTER_SERVER_TYPE) +
                            (1 << IC_DATA_SERVER_TYPE);
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
    DEBUG_RETURN(1);
  glob_conf_entry_inited= TRUE;
  glob_max_config_id= 0;
  memset(map_config_id_to_inx, 0, MAX_MAP_CONFIG_ID * sizeof(guint16));
  memset(glob_conf_entry, 0, MAX_CONFIG_ID * sizeof(IC_CONFIG_ENTRY));
  init_config_parameters();
  calculate_mandatory_bits();
  DEBUG_RETURN(build_config_name_hash());
}

static int
ic_assign_config_value(IC_API_CONFIG_SERVER *apic,
                       IC_CONFIG_ENTRY *conf_entry, guint64 value,
                       IC_CONFIG_TYPES conf_type,
                      int data_type,
                      gchar *struct_ptr)
{
  gchar *char_ptr;

  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0;
  if (!(conf_entry->config_types & (1 << conf_type)) ||
      (conf_entry->is_boolean && (value > 1)) ||
      (conf_entry->is_min_value_defined &&
       (conf_entry->min_value > value)) ||
      (conf_entry->is_max_value_defined &&
       (conf_entry->max_value < value)))
  {
    if (conf_entry->is_min_value_defined && value < conf_entry->min_value)
      char_ptr= "Value too small";
    else if (conf_entry->is_max_value_defined && value > conf_entry->max_value)
      char_ptr= "Value too large";
    else if (conf_entry->is_boolean && (value > 1))
      char_ptr= "Erroneus bool value";
    else
    {
      char_ptr= NULL;
      DEBUG_PRINT(CONFIG_LEVEL,
        ("Error with node type, config_types = %u, conf_type %u\n",
         conf_entry->config_types, conf_type));
    }
    if (char_ptr)
    {
      DEBUG_PRINT(CONFIG_LEVEL,
        ("Config value error:  %s", char_ptr));
    }
    PROTOCOL_CHECK_RETURN(FALSE);
  }
  struct_ptr+= conf_entry->offset;
  if (data_type == IC_CL_INT32_TYPE)
  {
    if (conf_entry->data_type == IC_UINT32)
    {
      *(guint32*)struct_ptr= (guint32)value;
    }
    else if (conf_entry->data_type == IC_UINT16)
    {
      *(guint16*)struct_ptr= (guint16)value;
    }
    else if (conf_entry->data_type == IC_CHAR ||
             conf_entry->data_type == IC_BOOLEAN)
    {
      *(guint8*)struct_ptr= (guint8)value;
    }
    else
      PROTOCOL_CHECK_RETURN(FALSE);
    return 0;
  }
  else if (data_type == IC_CL_INT64_TYPE)
  {
    if (conf_entry->data_type == IC_UINT64)
    {
      *(guint64*)struct_ptr= (guint64)value;
    }
    else
      PROTOCOL_CHECK_RETURN(FALSE);
    return 0;
  }
  g_assert(FALSE);
  return 0;
}

static int
ic_assign_config_string(IC_API_CONFIG_SERVER *apic,
                        IC_CONFIG_ENTRY *conf_entry,
                        IC_CONFIG_TYPES conf_type,
                        gchar *string_memory,
                        guint32 string_len,
                        gchar *struct_ptr,
                        gchar **string_val)
{
  if (!conf_entry->is_string_type ||
      !(conf_entry->config_types & (1 << conf_type)))
  {
    DEBUG_PRINT(CONFIG_LEVEL,
          ("conf_type = %u, config_types = %u, config_id = %u",
           conf_type, conf_entry->config_types, conf_entry->config_id));
    DEBUG_PRINT(CONFIG_LEVEL, ("Debug string inconsistency"));
    PROTOCOL_CHECK_RETURN(FALSE);
  }
  struct_ptr+= conf_entry->offset;
  *(gchar**)struct_ptr= string_memory;
  strncpy(string_memory, *string_val, string_len);
  DEBUG_PRINT(CONFIG_LEVEL,
    ("String value = %s", string_memory));
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
    case IC_BOOLEAN:
    {
      gchar *varboolean_ptr= (gchar*)var_ptr;
      *varboolean_ptr= (gchar)conf_entry->default_value;
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
    default:
      abort();
      break;
  }
}

static void
init_config_object(gchar *conf_object, guint32 size_struct,
                   IC_CONFIG_TYPES config_type)
{
  guint32 i;
  for (i= 1; i <= glob_max_config_id; i++)
  {
    IC_CONFIG_ENTRY *conf_entry= &glob_conf_entry[i];
    if (conf_entry && conf_entry->config_types & (1 << config_type))
    {
      gchar *var_ptr= conf_object + conf_entry->offset;
      if (conf_entry->offset >= size_struct)
        abort();
      init_config_default_value(var_ptr, conf_entry);
    }
  }
}

/*
  CONFIGURATION TRANSLATE KEY DATA TO CONFIGURATION OBJECTS MODULE
  ----------------------------------------------------------------
*/

static gboolean
check_buf(gchar *read_buf, guint32 read_size, const gchar *str, int str_len)
{
  if ((read_size != (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0))
    return TRUE;
  return FALSE;
}

static gboolean
check_buf_with_many_int(gchar *read_buf, guint32 read_size, const gchar *str,
                        int str_len, guint32 num_elements,
                        guint64 *number)
{
  gchar *ptr, *end_ptr;
  guint32 i, num_chars;

  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) == 0))
  {
    ptr= read_buf+str_len;
    end_ptr= read_buf + read_size;
    for (i= 0; i < num_elements; i++)
    {
      num_chars= ic_count_characters(ptr, end_ptr - ptr);
      if (ptr + num_chars > end_ptr)
        goto error;
      if (convert_str_to_int_fixed_size(ptr, num_chars, &number[i]))
        goto error;
      ptr+= num_chars;
      if (*ptr == ' ')
        ptr++;
      else if (*ptr != '\n' || i != (num_elements - 1))
        goto error;
    }
  }
  return FALSE;
error:
  return TRUE;
}

static gboolean
check_buf_with_int(gchar *read_buf, guint32 read_size, const gchar *str,
                   int str_len, guint64 *number)
{
  if ((read_size < (guint32)str_len) ||
      (memcmp(read_buf, str, str_len) != 0) ||
      (convert_str_to_int_fixed_size(read_buf+str_len, read_size - str_len,
                                     number)))
    return TRUE;
  return FALSE;
}

static int
rec_simple_str(IC_CONNECTION *conn, const gchar *str)
{
  gchar *read_buf;
  guint32 read_size;
  int error;

  if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (check_buf(read_buf, read_size, str,
                   strlen(str)))
    {
      DEBUG_PRINT(CONFIG_LEVEL,
        ("Protocol error in waiting for %s", str));
      DEBUG_RETURN(PROTOCOL_ERROR);
    }
    DEBUG_RETURN(0);
  }
  DEBUG_RETURN(error);
}

static int
allocate_mem_phase1(IC_CLUSTER_CONFIG *conf_obj)
{
  /*
    Allocate memory for pointer arrays pointing to the configurations of the
    nodes in the cluster, also allocate memory for array of node types.
    This memory is only temporary allocated for the analysis of the
    configuration data.
  */
  conf_obj->node_types= (IC_NODE_TYPES*)ic_calloc(conf_obj->num_nodes *
                                                  sizeof(IC_NODE_TYPES));
  conf_obj->node_ids= (guint32*)ic_calloc(conf_obj->num_nodes *
                                          sizeof(guint32));
  conf_obj->node_config= (gchar**)ic_calloc(conf_obj->num_nodes *
                                            sizeof(gchar*));
  if (!conf_obj->node_types || !conf_obj->node_ids ||
      !conf_obj->node_config)
    return IC_ERROR_MEM_ALLOC;
  return 0;
}


static int
allocate_mem_phase2(IC_API_CONFIG_SERVER *apic, IC_CLUSTER_CONFIG *conf_obj)
{
  guint32 i, size_struct;
  guint32 size_config_objects= 0;
  gchar *conf_obj_ptr, *string_mem;
  IC_MEMORY_CONTAINER *mc_ptr= apic->mc_ptr;
  /*
    Allocate memory for the actual configuration objects for nodes and
    communication sections.
  */
  for (i= 0; i < conf_obj->num_nodes; i++)
  {
    switch (conf_obj->node_types[i])
    {
      case IC_DATA_SERVER_NODE:
        size_config_objects+= sizeof(IC_DATA_SERVER_CONFIG);
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
  if (!(conf_obj->comm_config= (gchar**)
      mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
                     conf_obj->num_comms * sizeof(gchar*))) ||
      !(apic->string_memory_to_return= 
      mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
                     apic->string_memory_size)) ||
      !(apic->config_memory_to_return= 
      mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
                     size_config_objects)))
    return IC_ERROR_MEM_ALLOC;

  conf_obj_ptr= apic->config_memory_to_return;
  string_mem= apic->string_memory_to_return;
  apic->end_string_memory= string_mem + apic->string_memory_size;
  apic->next_string_memory= string_mem;

  for (i= 0; i < conf_obj->num_nodes; i++)
  {
    conf_obj->node_config[i]= conf_obj_ptr;
    size_struct= 0;
    switch (conf_obj->node_types[i])
    {
      case IC_DATA_SERVER_NODE:
        size_struct= sizeof(IC_DATA_SERVER_CONFIG);
        break;
      case IC_CLIENT_NODE:
        size_struct= sizeof(IC_CLIENT_CONFIG);
        break;
      case IC_CLUSTER_SERVER_NODE:
        size_struct= sizeof(IC_CLUSTER_SERVER_CONFIG);
        break;
      case IC_SQL_SERVER_NODE:
        size_struct= sizeof(IC_SQL_SERVER_CONFIG);
        break;
      case IC_REP_SERVER_NODE:
        size_struct= sizeof(IC_REP_SERVER_CONFIG);
        break;
      default:
        g_assert(FALSE);
        break;
    }
    init_config_object(conf_obj_ptr, size_struct,
                       conf_obj->node_types[i]);
    conf_obj_ptr+= size_struct;
  }
  for (i= 0; i < conf_obj->num_comms; i++)
  {
    init_config_object(conf_obj_ptr, sizeof(IC_COMM_LINK_CONFIG),
                       IC_COMM_TYPE);
    conf_obj->comm_config[i]= conf_obj_ptr;
    conf_obj_ptr+= sizeof(IC_COMM_LINK_CONFIG);
  }
  g_assert(conf_obj_ptr == (apic->config_memory_to_return +
                            size_config_objects));
  return 0;
}

static int
key_type_error(IC_API_CONFIG_SERVER *apic,
               __attribute__ ((unused)) guint32 hash_key,
               __attribute__ ((unused)) guint32 node_type)
{
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Wrong key type %u node type %u", hash_key, node_type));
  PROTOCOL_CHECK_RETURN(FALSE);
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
                            IC_API_CONFIG_SERVER *apic,
                            guint32 node_index, guint32 value,
                            guint32 hash_key)
{
  if (hash_key == IC_NODE_TYPE)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
                ("Node type of index %u is %u", node_index, value));
    switch (value)
    {
      case IC_DATA_SERVER_NODE:
        conf_obj->num_data_servers++; break;
      case IC_CLIENT_NODE:
        conf_obj->num_clients++; break;
      case IC_CLUSTER_SERVER_NODE:
        conf_obj->num_cluster_servers++; break;
      case IC_SQL_SERVER_NODE:
        conf_obj->num_sql_servers++; break;
      case IC_REP_SERVER_NODE:
        conf_obj->num_rep_servers++; break;
      case IC_FILE_SERVER_NODE:
        conf_obj->num_file_servers++; break;
      case IC_RESTORE_NODE:
        conf_obj->num_restore_nodes++; break;
      case IC_CLUSTER_MGR_NODE:
        conf_obj->num_cluster_mgrs++; break;
      default:
        DEBUG_PRINT(CONFIG_LEVEL, ("No such node type"));
        PROTOCOL_CHECK_RETURN(FALSE);
    }
    conf_obj->node_types[node_index]= (IC_NODE_TYPES)value;
  }
  else if (hash_key == IC_NODE_ID)
  {
    conf_obj->node_ids[node_index]= value;
    conf_obj->max_node_id= MAX(value, conf_obj->max_node_id);
    DEBUG_PRINT(CONFIG_LEVEL,
                ("Node id = %u for index %u", value, node_index));
  }
  return 0;
}

static int
step_key_value(IC_API_CONFIG_SERVER *apic,
               guint32 key_type, guint32 **key_value,
               guint32 value, guint32 *key_value_end)
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
        DEBUG_PRINT(CONFIG_LEVEL, ("Array ended in the middle of a type"));
        PROTOCOL_CHECK_RETURN(FALSE);
      }
      if (value != (strlen((gchar*)(*key_value)) + 1))
      {
        DEBUG_PRINT(CONFIG_LEVEL, ("Wrong length of character type"));
        PROTOCOL_CHECK_RETURN(FALSE);
      }
      (*key_value)+= len_words;
      apic->string_memory_size+= value;
      break;
   }
   default:
     return key_type_error(apic, key_type, 32);
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
        DEBUG_PRINT(CONFIG_LEVEL, ("No such config entry, return NULL"));
      }
      return conf_entry;
    }  
  }
  DEBUG_PRINT(CONFIG_LEVEL,
              ("Error in map_config_id_to_inx, returning NULL"));
  return NULL;
}

static int
assign_config_value(IC_CONFIG_ENTRY *conf_entry,
                    IC_API_CONFIG_SERVER *apic,
                    IC_CONFIG_TYPES conf_type,
                    guint32 key_type,
                    gchar *struct_ptr,
                    guint32 value,
                    gchar **key_value,
                    guint32 hash_key)
{
  int error;
  switch (key_type)
  {
    case IC_CL_INT32_TYPE:
    {
      if ((error= ic_assign_config_value(apic, conf_entry,
                                         (guint64)value, conf_type,
                                         key_type, struct_ptr)))
        return error;
      break;
    }
    case IC_CL_INT64_TYPE:
    {
      guint64 long_val= get_64bit_value(value, (guint32**)key_value);
      if ((error= ic_assign_config_value(apic, conf_entry, long_val,
                                         conf_type,
                                         key_type,
                                         struct_ptr)))
        return error;
      break;
    }
    case IC_CL_CHAR_TYPE:
    {
      if ((error= ic_assign_config_string(apic, conf_entry, conf_type,
                                          apic->next_string_memory,
                                          value,
                                          struct_ptr,
                                          key_value)))
        return error;
      update_string_data(apic, value, (guint32**)key_value);
      break;
    }
    default:
      return key_type_error(apic, hash_key, conf_type);
  }
  return 0;
}

static int
assign_node_section(IC_CLUSTER_CONFIG *conf_obj,
                    IC_API_CONFIG_SERVER *apic,
                    guint32 key_type, guint32 **key_value,
                    guint32 value, guint32 hash_key,
                    guint32 node_sect_id)
{
  IC_CONFIG_ENTRY *conf_entry;
  gchar *node_config;
  IC_NODE_TYPES node_type;

  if (hash_key == IC_PARENT_ID || hash_key == IC_NODE_TYPE ||
      hash_key == IC_NODE_ID)
    return 0; /* Ignore for now */
  if (!(conf_entry= get_conf_entry(hash_key)))
    PROTOCOL_CHECK_RETURN(FALSE);
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0; /* Ignore */
  if (node_sect_id >= conf_obj->num_nodes)
  {
    DEBUG_PRINT(CONFIG_LEVEL, ("node_sect_id out of range"));
    PROTOCOL_CHECK_RETURN(FALSE);
  }
  if (!(node_config= (gchar*)conf_obj->node_config[node_sect_id]))
  {
    DEBUG_PRINT(CONFIG_LEVEL, ("No such node_config object"));
    PROTOCOL_CHECK_RETURN(FALSE);
  }
  node_type= conf_obj->node_types[node_sect_id];
  return assign_config_value(conf_entry,
                             apic,
                             (IC_CONFIG_TYPES)node_type,
                             key_type,
                             node_config,
                             value,
                             (gchar**)key_value,
                             hash_key);
}

static int
assign_system_section(IC_CLUSTER_CONFIG *conf_obj,
                      IC_API_CONFIG_SERVER *apic,
                      guint32 key_type,
                      guint32 **key_value,
                      guint32 value,
                      guint32 hash_key)
{
  IC_CONFIG_ENTRY *conf_entry;
  IC_SYSTEM_CONFIG *sys_conf;

  if (hash_key == IC_PARENT_ID || hash_key == IC_NODE_TYPE)
    return 0; /* Ignore */
  /*
    We have stored the system entries as 1001, 1002 and so forth
    although in the protocol they are 1,2 and so forth. This is
    to ensure that entries in the configuration entry hash are
    unique.
  */
  if (!(conf_entry= get_conf_entry(hash_key + 1000)))
    PROTOCOL_CHECK_RETURN(FALSE);
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0; /* Ignore */
  sys_conf= &conf_obj->sys_conf;
  return assign_config_value(conf_entry,
                             apic,
                             IC_SYSTEM_TYPE,
                             key_type,
                             (gchar*)sys_conf,
                             value,
                             (gchar**)key_value,
                             hash_key);
}

static int
assign_comm_section(IC_CLUSTER_CONFIG *conf_obj,
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
    PROTOCOL_CHECK_RETURN(FALSE);
  if (conf_entry->is_deprecated || conf_entry->is_not_configurable)
    return 0; /* Ignore */
  g_assert(comm_sect_id < conf_obj->num_comms);
  socket_conf= (IC_SOCKET_LINK_CONFIG*)conf_obj->comm_config[comm_sect_id];
  return assign_config_value(conf_entry,
                             apic,
                             IC_COMM_TYPE,
                             key_type,
                             (gchar*)socket_conf,
                             value,
                             (gchar**)key_value,
                             hash_key);
}

static int
arrange_node_arrays(IC_API_CONFIG_SERVER *apic, IC_CLUSTER_CONFIG *conf_obj)
{
  gchar **new_node_config;
  IC_NODE_TYPES *new_node_types;
  guint32 i, node_id;
  IC_MEMORY_CONTAINER *mc_ptr= apic->mc_ptr;
  DEBUG_ENTRY("arrange_node_arrays");

  new_node_types= (IC_NODE_TYPES*)
    mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, (conf_obj->max_node_id + 1) *
                                        sizeof(guint32));
  new_node_config= (gchar**)
    mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, (conf_obj->max_node_id + 1) *
                                        sizeof(gchar*));
  if (!new_node_types || !new_node_config)
    DEBUG_RETURN(IC_ERROR_MEM_ALLOC);

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
  DEBUG_RETURN(0);
}

static int
analyse_key_value(guint32 *key_value, guint32 len,
                  IC_API_CONFIG_SERVER *apic,
                  guint32 cluster_id)
{
  guint32 *key_value_start= key_value;
  guint32 *key_value_end= key_value + len;
  IC_CLUSTER_CONFIG *conf_obj;
  gboolean first= TRUE;
  gboolean first_ndbd= TRUE;
  int error;
  guint32 pass;
  guint32 system_section= 0;
  guint32 first_comm_section= 0;
  guint32 first_ndbd_section= 0;
  guint32 num_apis= 0;
  guint32 node_section, node_index;

  conf_obj= apic->conf_objects[cluster_id];
  conf_obj->num_nodes= 0;
  for (pass= 0; pass < 2; pass++)
  {
    if (pass == 1)
    {
      if ((error= allocate_mem_phase2(apic, conf_obj)))
        goto error;
    }
    key_value= key_value_start;
    while (key_value < key_value_end)
    {
      guint32 key= g_ntohl(key_value[0]);
      guint32 value= g_ntohl(key_value[1]);
      guint32 hash_key= key & IC_CL_KEY_MASK;
      guint32 sect_id= (key >> IC_CL_SECT_SHIFT) & IC_CL_SECT_MASK;
      guint32 key_type= key >> IC_CL_KEY_SHIFT;
      DEBUG_PRINT(CONFIG_LEVEL,
      ("Section: %u, Config id: %u, Type: %u, value: %u", sect_id, hash_key,
       key_type, value));
      if (pass == 0)
      {
        if (sect_id == 0)
        {
          if (hash_key == 1000)
          {
            /* Verify we have at least one cluster server and an api node */
            system_section= (value >> IC_CL_SECT_SHIFT);
            num_apis= system_section - 2;
            PROTOCOL_CHECK_GOTO(system_section >= 4);
          }
          else if (hash_key == 2000)
          {
            PROTOCOL_CHECK_GOTO(value == 16384);
          }
          else if (hash_key == 3000)
          {
            first_comm_section= (value >> IC_CL_SECT_SHIFT) + 1;
            PROTOCOL_CHECK_GOTO((system_section + 3) == first_comm_section);
          }
        } else if (sect_id == 1)
        {
          node_section= (value >> IC_CL_SECT_SHIFT);
          if (node_section >= first_comm_section && first_ndbd)
          {
            first_ndbd= FALSE;
            first_ndbd_section= node_section;
            conf_obj->num_comms= first_ndbd_section - first_comm_section;
          }
          conf_obj->num_nodes++;
        }
        else if (sect_id == 2 && first)
        {
          /* Verify we have at least one ndbd node */
          PROTOCOL_CHECK_GOTO(!first_ndbd);
          first= FALSE;
          DEBUG_PRINT(CONFIG_LEVEL,
            ("num_nodes = %u, num_comms = %u, garbage section = %u",
            conf_obj->num_nodes, conf_obj->num_comms, system_section));
          DEBUG_PRINT(CONFIG_LEVEL,
            ("first comm = %u, first ndbd = %u",
            first_comm_section, first_ndbd_section));
          if ((error= allocate_mem_phase1(conf_obj)))
            goto error;

        }
        if (sect_id >= 2 && sect_id < system_section)
        {
          node_index= sect_id - 2;
          if ((error= analyse_node_section_phase1(conf_obj, apic, node_index,
                                                  value, hash_key)))
            goto error;
        }
        else if (sect_id >= first_ndbd_section)
        {
          node_index= num_apis + (sect_id - first_ndbd_section);
          if ((error= analyse_node_section_phase1(conf_obj, apic,
                                                  node_index,
                                                  value, hash_key)))
            goto error;
        }
      }
      key_value+= 2;
      if (pass == 0)
      {
        if ((error= step_key_value(apic, key_type, &key_value,
                                   value, key_value_end)))
          goto error;
      }
      else /* Pass == 1 */
      {
        guint32 node_sect_id;
        if (sect_id == 0)
        {
          PROTOCOL_CHECK_GOTO(
            (hash_key == 1000 &&
              (value == (system_section << IC_CL_SECT_SHIFT))) ||
            (hash_key == 2000 && (1 << IC_CL_SECT_SHIFT)) ||
            (hash_key == 3000 &&
              (value == ((system_section + 2) << IC_CL_SECT_SHIFT))));
          PROTOCOL_CHECK_GOTO(key_type == IC_CL_SECT_TYPE);
        } else if (sect_id == 1)
        {
          PROTOCOL_CHECK_GOTO(key_type == IC_CL_INT32_TYPE);
          PROTOCOL_CHECK_GOTO(hash_key < conf_obj->num_nodes);
          if (hash_key < num_apis)
          {
            PROTOCOL_CHECK_GOTO(value ==
                                ((2 + hash_key) << IC_CL_SECT_SHIFT));
          } else {
            PROTOCOL_CHECK_GOTO(value ==
              (((hash_key - num_apis) + first_ndbd_section)
                  << IC_CL_SECT_SHIFT));
          }
        } else if (sect_id >= 2 && sect_id < system_section)
        {
          node_sect_id= sect_id - 2;
          if ((error= assign_node_section(conf_obj, apic,
                                          key_type, &key_value,
                                          value, hash_key, node_sect_id)))
            goto error;
        } else if (sect_id == system_section)
        {
          PROTOCOL_CHECK_GOTO(hash_key == 0 &&
                              key_type == IC_CL_INT32_TYPE &&
               value == ((system_section + 1) << IC_CL_SECT_SHIFT));
        } else if (sect_id == (system_section + 1))
        {
          /*
             This is the system section where we currently define name,
             primary cluster server and system configuration number
          */
          if ((error= assign_system_section(conf_obj, apic,
                                            key_type, &key_value,
                                            value, hash_key)))
            goto error;
        } else if (sect_id == (system_section + 2))
        {
          PROTOCOL_CHECK_GOTO(key_type == IC_CL_INT32_TYPE &&
                              hash_key < conf_obj->num_comms &&
                              (first_comm_section + hash_key) ==
                                (value >> IC_CL_SECT_SHIFT));
        } else if (sect_id < first_ndbd_section)
        {
          guint32 comm_sect_id= sect_id - first_comm_section;
          if ((error= assign_comm_section(conf_obj, apic,
                                          key_type, &key_value,
                                          value, hash_key, comm_sect_id)))
            goto error;
        } else
        {
          node_sect_id= num_apis + (sect_id - first_ndbd_section);
          PROTOCOL_CHECK_GOTO(node_sect_id < conf_obj->num_nodes);
          if ((error= assign_node_section(conf_obj, apic,
                                          key_type, &key_value,
                                          value, hash_key, node_sect_id)))
            goto error;
        }
      }
    }
  }
  if ((error= arrange_node_arrays(apic, conf_obj)))
    goto error;
  return 0;
error:
  if (!first)
  {
    if (conf_obj->node_types)
      ic_free(conf_obj->node_types);
    if (conf_obj->node_ids)
      ic_free(conf_obj->node_ids);
    if (conf_obj->node_config)
      ic_free(conf_obj->node_config);
    conf_obj->node_types= NULL;
    conf_obj->node_ids= NULL;
    conf_obj->node_config= NULL;
  }
  return error;
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
                 guint32 cluster_id,
                 gchar *config_buf,
                 guint32 config_size)
{
  gchar *bin_buf;
  guint32 bin_config_size, bin_config_size32, checksum, i;
  guint32 *bin_buf32, *key_value_ptr, key_value_len;
  int error;

  g_assert((config_size & 3) == 0);
  bin_config_size= (config_size >> 2) * 3;
  /*
    This memory allocation is temporary allocation simply because size is
    variable and can be larger than stack.
  */
  if (!(bin_buf= ic_calloc(bin_config_size)))
    return IC_ERROR_MEM_ALLOC;
  if ((error= ic_base64_decode((guint8*)bin_buf, &bin_config_size,
                               (const guint8*)config_buf, config_size)))
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("1:Protocol error in base64 decode"));
    PROTOCOL_CHECK_GOTO(FALSE);
  }
  bin_config_size32= bin_config_size >> 2;
  if ((bin_config_size & 3) != 0 || bin_config_size32 <= 3)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("2:Protocol error in base64 decode"));
    PROTOCOL_CHECK_GOTO(FALSE);
  }
  if (memcmp(bin_buf, ver_string, 8))
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("3:Protocol error in base64 decode"));
    PROTOCOL_CHECK_GOTO(FALSE);
  }
  bin_buf32= (guint32*)bin_buf;
  checksum= 0;
  for (i= 0; i < bin_config_size32; i++)
    checksum^= g_ntohl(bin_buf32[i]);
  if (checksum)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("4:Protocol error in base64 decode"));
    PROTOCOL_CHECK_GOTO(FALSE);
  }
  key_value_ptr= bin_buf32 + 2;
  key_value_len= bin_config_size32 - 3;
  if ((error= analyse_key_value(key_value_ptr, key_value_len,
                                apic, cluster_id)))
    goto error;
  error= 0;
  goto end;

error:
end:
  ic_free(bin_buf);
  return error;
}

static int
set_up_cluster_server_connection(IC_API_CONFIG_SERVER *apic,
                                 IC_CONNECTION **conn,
                                 gchar *server_name,
                                 gchar *server_port)
{
  int error;
  IC_CONNECTION *loc_conn;

  if (!(*conn= ic_create_socket_object(TRUE, FALSE, FALSE,
                                       CONFIG_READ_BUF_SIZE,
                                       NULL, NULL)))
    return IC_ERROR_MEM_ALLOC;
  loc_conn= *conn;
  loc_conn->conn_op.ic_prepare_server_connection(loc_conn,
                                                 server_name,
                                                 server_port,
                                                 NULL,
                                                 NULL,
                                                 0,
                                                 FALSE);
  if ((error= loc_conn->conn_op.ic_set_up_connection(loc_conn, NULL, NULL)))
  {
    DEBUG_PRINT(COMM_LEVEL, ("Connect failed with error %d", error));
    apic->err_str= loc_conn->conn_op.ic_get_error_str(loc_conn);
    return error;
  }
  DEBUG_PRINT(COMM_LEVEL,
              ("Successfully set-up connection to cluster server"));
  return 0;
}

static int
send_get_nodeid(IC_CONNECTION *conn,
                IC_API_CONFIG_SERVER *apic,
                guint32 cluster_id)
{
  gchar version_buf[128];
  gchar nodeid_buf[32];
  gchar endian_buf[32];
  gchar nodetype_buf[32];
  gchar cluster_id_buf[32];
  gchar buf[128];
  guint32 node_type= 1;
  guint64 version_no;
  guint32 node_id= apic->node_ids[cluster_id];

  version_no= get_iclaustron_protocol_version(
    apic->api_op.ic_use_iclaustron_cluster_server(apic));
  g_snprintf(version_buf, 32, "%s%s", version_str, 
             ic_guint64_str(version_no, buf, NULL));
  g_snprintf(nodeid_buf, 32, "%s%u", nodeid_str, node_id);
  g_snprintf(cluster_id_buf, 32, "%s%u", cluster_id_str, cluster_id);
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
      (apic->api_op.ic_use_iclaustron_cluster_server(apic) &&
      ic_send_with_cr(conn, cluster_id_buf)) ||
      ic_send_with_cr(conn, ic_empty_string))
    return conn->conn_op.ic_get_error_code(conn);
  return 0;
}

static int
send_get_config(IC_CONNECTION *conn,
                gboolean use_iclaustron_cluster_server)
{
  gchar version_buf[128];
  gchar buf[128];
  guint64 version_no;

  version_no= get_iclaustron_protocol_version(use_iclaustron_cluster_server);
  g_snprintf(version_buf, 32, "%s%s", version_str,
             ic_guint64_str(version_no, buf, NULL));
  if (ic_send_with_cr(conn, get_config_str) ||
      ic_send_with_cr(conn, version_buf) ||
      ic_send_with_cr(conn, ic_empty_string))
    return conn->conn_op.ic_get_error_code(conn);
  return 0;
}

static int
rec_get_nodeid(IC_CONNECTION *conn,
               IC_API_CONFIG_SERVER *apic,
               guint32 cluster_id)
{
  gchar *read_buf;
  guint32 read_size;
  int error;
  guint64 node_number;
  guint32 state= GET_NODEID_REPLY_STATE;

  while (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    switch (state)
    {
      case GET_NODEID_REPLY_STATE:
        /*
          The protocol is decoded in the order of the case statements in the switch
          statements.
        
          Receive:
          get nodeid reply<CR>
        */
        if (check_buf(read_buf, read_size, get_nodeid_reply_str,
                      GET_NODEID_REPLY_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in Get nodeid reply state"));
          PROTOCOL_CHECK_RETURN(FALSE);
        }
        state= NODEID_STATE;
        break;
      case NODEID_STATE:
        /*
          Receive:
          nodeid: __nodeid<CR>
          Where __nodeid is an integer giving the nodeid of the starting process
        */
        if (check_buf_with_int(read_buf, read_size, nodeid_str, NODEID_LEN,
                               &node_number) ||
            (node_number > IC_MAX_NODE_ID))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in nodeid state"));
          PROTOCOL_CHECK_RETURN(FALSE);
        }
        DEBUG_PRINT(CONFIG_LEVEL, ("Nodeid = %u", (guint32)node_number));
        apic->node_ids[cluster_id]= (guint32)node_number;
        state= RESULT_OK_STATE;
        break;
      case RESULT_OK_STATE:
        /*
          Receive:
          result: Ok<CR>
        */
        if (check_buf(read_buf, read_size, result_ok_str, RESULT_OK_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in result ok state"));
          PROTOCOL_CHECK_RETURN(FALSE);
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
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in wait empty state"));
          PROTOCOL_CHECK_RETURN(FALSE);
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
               guint32 cluster_id)
{
  gchar *read_buf;
  guint32 read_size;
  gchar *config_buf= NULL;
  guint32 config_size= 0;
  guint32 rec_config_size= 0;
  int error= 0;
  guint64 content_length;
  guint32 state= GET_CONFIG_REPLY_STATE;

  while (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    switch (state)
    {
      case GET_CONFIG_REPLY_STATE:
        /*
          The protocol is decoded in the order of the case statements in the
          switch statements.
 
          Receive:
          get config reply<CR>
        */
        if (check_buf(read_buf, read_size, get_config_reply_str,
                      GET_CONFIG_REPLY_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in get config reply state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        state= RESULT_OK_STATE;
        break;
      case RESULT_OK_STATE:
        /*
          Receive:
          result: Ok<CR>
        */
        if (check_buf(read_buf, read_size, result_ok_str, RESULT_OK_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in result ok state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        state= CONTENT_LENGTH_STATE;
        break;
      case CONTENT_LENGTH_STATE:
        /*
          Receive:
          Content-Length: __length<CR>
          Where __length is a decimal-coded number indicating length in bytes
        */
        if (check_buf_with_int(read_buf, read_size, content_len_str,
                               CONTENT_LENGTH_LEN, &content_length) ||
            (content_length > MAX_CONTENT_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in content length state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        state= OCTET_STREAM_STATE;
        break;
      case OCTET_STREAM_STATE:
        /*
          Receive:
          Content-Type: ndbconfig/octet-stream<CR>
        */
        if (check_buf(read_buf, read_size, octet_stream_str, OCTET_STREAM_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in octet stream state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        state= CONTENT_ENCODING_STATE;
        break;
      case CONTENT_ENCODING_STATE:
        /*
          Receive:
          Content-Transfer-Encoding: base64
        */
        if (check_buf(read_buf, read_size, content_encoding_str,
                      CONTENT_ENCODING_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in content encoding state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        /*
          Here we need to allocate receive buffer for configuration plus the
          place to put the encoded binary data.
          This is a temporary memory allocation only for this method.
        */
        if (!(config_buf= ic_calloc(content_length)))
          return IC_ERROR_MEM_ALLOC;
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
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in wait empty return state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        if (state == WAIT_EMPTY_RETURN_STATE)
          state= RECEIVE_CONFIG_STATE;
        else
          goto end;
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
          DEBUG_PRINT(CONFIG_LEVEL, ("Start decoding"));
          error= translate_config(apic, cluster_id,
                                  config_buf, config_size);
          goto end;
        }
        else if (read_size != CONFIG_LINE_LEN)
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in config receive state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        break;
      case RESULT_ERROR_STATE:
        break;
      default:
        break;
    }
  }
error:
end:
  if (config_buf)
    ic_free(config_buf);
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

  After a successful retrieval of the configuration one can either drop the
  connection, one can convert the connection to a transporter channel. To
  convert it into a transporter channel is the normal behaviour.

  Through the transporter channel the cluster server can inform the
  configuration client can ensure that it is informed of all changes to
  the configuration. In this manner we can ensure that the configuration
  database is kept up-to-date. Thus each client becomes an entity that will be
  involved in any changes to the configuration. It is enough to be involved in
  the changes of the clusters the client is a part of.
*/

/*
static int
get_config_info_channels(__attribute__ ((unused)) IC_API_CONFIG_SERVER *apic)
{
  DEBUG_ENTRY("get_config_info_channels");
  DEBUG_RETURN(0);
}
*/

static void
disconnect_api_connections(IC_API_CONFIG_SERVER *apic)
{
  guint32 i;
  IC_CONNECTION *conn;

  for (i= 0; i < apic->cluster_conn.num_cluster_servers; i++)
  {
    conn= apic->cluster_conn.cluster_srv_conns[i];
    conn->conn_op.ic_free_connection(conn);
  }
}

static int
connect_api_connections(IC_API_CONFIG_SERVER *apic, IC_CONNECTION **conn_ptr)
{
  guint32 i;
  int error= 1;
  IC_CONNECTION **conn_p;

  for (i= 0; i < apic->cluster_conn.num_cluster_servers; i++)
  {
    conn_p= (IC_CONNECTION**)(&apic->cluster_conn.cluster_srv_conns[i]);
    if (!(error= set_up_cluster_server_connection(apic, conn_p,
                  apic->cluster_conn.cluster_server_ips[i],
                  apic->cluster_conn.cluster_server_ports[i])))
    {
      *conn_ptr= *conn_p;
      apic->cluster_conn.current_conn= *conn_p;
      return 0;
    }
  }
  return error;
}

static int
get_info_config_channels(__attribute__ ((unused)) IC_API_CONFIG_SERVER *apic)
{
  return 0;
}

static guint32
count_clusters(IC_CLUSTER_CONNECT_INFO **clu_infos)
{
  IC_CLUSTER_CONNECT_INFO **clu_info_iter;
  guint32 num_clusters= 0;
  clu_info_iter= clu_infos;
  while (*clu_info_iter)
  {
    num_clusters++;
    clu_info_iter++;
  }
  return num_clusters;
}
/*
  This method connects to one of the provided cluster servers. Then it
  retrieves the configuration from this cluster server. In a successful
  case we keep this connection so that it can be kept for other usage
  such that we can stay informed about changes of the configuration.
  The configuration is by default locked in the sense that the configuration
  cannot be changed until the connection is converted to a information
  channel where the requester of the configuration also is informed of
  any changes to the configuration in proper order.
*/
static int
get_cs_config(IC_API_CONFIG_SERVER *apic,
              IC_CLUSTER_CONNECT_INFO **clu_infos,
              guint32 node_id)
{
  guint32 i, max_cluster_id= 0;
  guint32 cluster_id, num_clusters;
  int error= END_OF_FILE;
  IC_CONNECTION *conn= NULL;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_MEMORY_CONTAINER *mc_ptr= apic->mc_ptr;
  DEBUG_ENTRY("get_cs_config");

  if (apic->use_ic_cs)
    num_clusters= count_clusters(clu_infos);
  else
    num_clusters= 1;
  if ((error= connect_api_connections(apic, &conn)))
    DEBUG_RETURN(error);
  if (apic->use_ic_cs)
  {
    if ((error= apic->api_op.ic_get_cluster_ids(apic, clu_infos)))
      DEBUG_RETURN(error);
    for (i= 0; i < num_clusters; i++)
      max_cluster_id= IC_MAX(max_cluster_id, clu_infos[i]->cluster_id);
    apic->max_cluster_id= max_cluster_id;
  }
  else
  {
    apic->max_cluster_id= max_cluster_id= 0;
  }
  if (!(apic->conf_objects= (IC_CLUSTER_CONFIG**)
        mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
           (max_cluster_id + 1) * sizeof(IC_CLUSTER_CONFIG**))) ||
      !(apic->node_ids= (guint32*)
        mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
            sizeof(guint32) * (max_cluster_id + 1))))
    goto mem_alloc_error;

  for (i= 0; i < num_clusters; i++)
  {
    if (apic->use_ic_cs)
      cluster_id= clu_infos[i]->cluster_id;
    else
      cluster_id= 0;
    if (!(clu_conf= (IC_CLUSTER_CONFIG*)
        mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_CLUSTER_CONFIG))))
      goto mem_alloc_error;
    if (ic_mc_strdup(mc_ptr, &clu_conf->clu_info.cluster_name,
                       &clu_infos[i]->cluster_name) ||
        ic_mc_strdup(mc_ptr, &clu_conf->clu_info.password,
                       &clu_infos[i]->password))
      goto mem_alloc_error;
    clu_conf->clu_info.cluster_id= cluster_id;
    if (apic->conf_objects[cluster_id])
      goto error;
    apic->conf_objects[cluster_id]= clu_conf;
    apic->node_ids[cluster_id]= node_id;
    clu_conf->my_node_id= node_id;
  }

  for (cluster_id= 0; cluster_id <= apic->max_cluster_id; cluster_id++)
  {
    if (!apic->conf_objects[cluster_id] ||
       ((error= send_get_nodeid(conn, apic, cluster_id)) ||
        (error= rec_get_nodeid(conn, apic, cluster_id)) ||
        (error= send_get_config(conn,
         apic->api_op.ic_use_iclaustron_cluster_server(apic))) ||
        (error= rec_get_config(conn, apic, cluster_id))))
      continue;
  }
  if (error)
    goto error;
  for (cluster_id= 0; cluster_id <= apic->max_cluster_id; cluster_id++)
  {
    IC_CLUSTER_CONFIG *clu_conf= apic->conf_objects[cluster_id];
    if (clu_conf && build_hash_on_comms(clu_conf))
      goto mem_alloc_error;
  }
  apic->cluster_conn.current_conn= conn;  
error:
  if (error)
  {
    DEBUG_PRINT(CONFIG_LEVEL, ("Error: %d\n", error));
  }
  DEBUG_RETURN(error);
mem_alloc_error:
  DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
}

#define RECEIVE_CLUSTER_NAME 1
#define RECEIVE_CLUSTER_ID 2

static int
get_cluster_ids(IC_API_CONFIG_SERVER *apic,
                IC_CLUSTER_CONNECT_INFO **clu_infos)
{
  gchar *read_buf;
  guint32 read_size;
  guint32 state, num_clusters_found= 0;
  guint32 num_clusters= 0;
  guint64 cluster_id;
  IC_CLUSTER_CONNECT_INFO *found_clu_info= NULL;
  IC_CLUSTER_CONNECT_INFO **clu_info_iter, *clu_info;
  int error;
  IC_CONNECTION *conn= apic->cluster_conn.current_conn;
  DEBUG_ENTRY("get_cluster_ids");

  num_clusters= count_clusters(clu_infos);
  if (ic_send_with_cr(conn, get_cluster_list_str))
  {
    error= conn->conn_op.ic_get_error_code(conn);
    goto error;
  }
  if ((error= rec_simple_str(conn, get_cluster_list_reply_str)))
    PROTOCOL_CHECK_ERROR_GOTO(error);
  state= RECEIVE_CLUSTER_NAME;
  while (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (!check_buf(read_buf, read_size, end_get_cluster_list_str,
                   strlen(end_get_cluster_list_str)))
      break;
    switch (state)
    {
      case RECEIVE_CLUSTER_NAME:
        if ((read_size < CLUSTER_NAME_REQ_LEN) ||
            (memcmp(read_buf, cluster_name_string, CLUSTER_NAME_REQ_LEN) != 0))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in receive cluster name state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        found_clu_info= NULL;
        clu_info_iter= clu_infos;
        while (*clu_info_iter)
        {
          clu_info= *clu_info_iter;
          if ((clu_info->cluster_name.len ==
               (read_size - CLUSTER_NAME_REQ_LEN)) &&
              (memcmp(clu_info->cluster_name.str,
                      read_buf+CLUSTER_NAME_REQ_LEN,
                      clu_info->cluster_name.len) == 0))
          {
            found_clu_info= clu_info;
            break;
          }
          clu_info_iter++;
        }
        state= RECEIVE_CLUSTER_ID;
        break;
      case RECEIVE_CLUSTER_ID:
        if (check_buf_with_int(read_buf, read_size, cluster_id_str,
                               CLUSTER_ID_REQ_LEN, &cluster_id))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in receive cluster id state"));
          PROTOCOL_CHECK_GOTO(FALSE);
        }
        PROTOCOL_CHECK_GOTO(cluster_id <= IC_MAX_CLUSTER_ID);
        if (found_clu_info)
        {
          found_clu_info->cluster_id= cluster_id;
          num_clusters_found++;
        }
        state= RECEIVE_CLUSTER_NAME;
        break;
      default:
        g_assert(FALSE);
    }
  }
  error= 1;
  if (num_clusters_found != num_clusters)
    goto error;
  DEBUG_RETURN(0);
error:
  DEBUG_RETURN(error);
}

static void
free_cs_config(IC_API_CONFIG_SERVER *apic)
{
  guint32 i;
  if (apic)
  {
    disconnect_api_connections(apic);
    if (apic->config_mutex)
      g_mutex_free(apic->config_mutex);
    if (apic->conf_objects)
    {
      for (i= 0; i <= apic->max_cluster_id; i++)
      {
        IC_CLUSTER_CONFIG *conf_obj= apic->conf_objects[i];
        if (!conf_obj)
          continue;
        g_assert(!conf_obj->node_ids);
        if (conf_obj->comm_hash)
          ic_hashtable_destroy(conf_obj->comm_hash);
      }
    }
    apic->mc_ptr->mc_ops.ic_mc_free(apic->mc_ptr);
  }
  return;
}

static IC_CLUSTER_CONFIG *get_cluster_config(IC_API_CONFIG_SERVER *apic,
                                             guint32 cluster_id)
{
  if (!apic->conf_objects ||
      cluster_id > apic->max_cluster_id)
    return NULL;
  return apic->conf_objects[cluster_id];
}

static gchar*
get_node_and_cluster_config(IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
                            guint32 node_id,
                            IC_CLUSTER_CONFIG **clu_conf)
{
  gchar *node_config;
  *clu_conf= get_cluster_config(apic, cluster_id);
  if (!clu_conf)
    return NULL;
  node_config= (*clu_conf)->node_config[node_id];
  if (!node_config)
    return NULL;
  return node_config;
}

static gchar*
get_node_object(IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
                guint32 node_id)
{
  IC_CLUSTER_CONFIG *clu_conf;
  return get_node_and_cluster_config(apic, cluster_id, node_id,
                                     &clu_conf);
}

static gchar*
get_typed_node_object(IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
                      guint32 node_id, IC_NODE_TYPES node_type)
{
  IC_CLUSTER_CONFIG *clu_conf= NULL;
  gchar *node_config= get_node_and_cluster_config(apic, cluster_id,
                                                  node_id,
                                                  &clu_conf);
  if (clu_conf &&
      clu_conf->node_types[node_id] == node_type)
    return node_config;
  return NULL;
}

static IC_SOCKET_LINK_CONFIG*
get_communication_object(IC_API_CONFIG_SERVER *apic, guint32 cluster_id,
                         guint32 first_node_id, guint32 second_node_id)
{
  IC_CLUSTER_CONFIG *clu_conf;
  IC_SOCKET_LINK_CONFIG test1;

  if (!(clu_conf= get_cluster_config(apic, cluster_id)))
    return NULL;
  test1.first_node_id= first_node_id;
  test1.second_node_id= second_node_id;
  return (IC_SOCKET_LINK_CONFIG*)ic_hashtable_search(clu_conf->comm_hash,
                                                     (void*)&test1);
}

static const gchar*
get_error_str(IC_API_CONFIG_SERVER *apic)
{
  return apic->err_str;
}

static gboolean
use_ic_cs(IC_API_CONFIG_SERVER *apic)
{
  return apic->use_ic_cs;
}

static gchar*
fill_error_buffer(IC_API_CONFIG_SERVER *apic,
                  int error_code,
                  gchar *error_buffer)
{
  return ic_common_fill_error_buffer(apic->err_str,
                                     apic->err_line,
                                     error_code,
                                     error_buffer);
}

static void
set_error_line(IC_API_CONFIG_SERVER *apic, guint32 error_line)
{
  apic->err_line= error_line;
}

/*
  This routine initialises the data structures needed for a configuration client.
  It sets the proper routines to get the configuration and to free the
  configuration.
*/
IC_API_CONFIG_SERVER*
ic_create_api_cluster(IC_API_CLUSTER_CONNECTION *cluster_conn,
                      gboolean use_ic_cs_var)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_API_CONFIG_SERVER *apic= NULL;
  guint32 num_cluster_servers= cluster_conn->num_cluster_servers;
  DEBUG_ENTRY("ic_create_api_cluster");


  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
    DEBUG_RETURN(NULL);
  /*
    The idea with this method is that the user can set-up his desired usage
    of the clusters using stack variables. Then we copy those variables to
    heap storage and maintain this data hereafter.
    Thus we can in many cases relieve the user from the burden of error
    handling of memory allocation failures.

    We will also ensure that the supplied data is validated.
  */
  if (!(apic= (IC_API_CONFIG_SERVER*)
      mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_API_CONFIG_SERVER))))
  {
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
    DEBUG_RETURN(NULL);
  }
  apic->mc_ptr= mc_ptr;
  apic->cluster_conn.num_cluster_servers= num_cluster_servers;

  apic->use_ic_cs= use_ic_cs_var;
  apic->api_op.ic_use_iclaustron_cluster_server= use_ic_cs;
  apic->api_op.ic_set_error_line= set_error_line;
  apic->api_op.ic_fill_error_buffer= fill_error_buffer;
  apic->api_op.ic_get_error_str= get_error_str;
  apic->api_op.ic_get_config= get_cs_config;
  apic->api_op.ic_get_cluster_ids= get_cluster_ids;
  apic->api_op.ic_get_info_config_channels= get_info_config_channels;
  apic->api_op.ic_get_cluster_config= get_cluster_config;
  apic->api_op.ic_get_node_object= get_node_object;
  apic->api_op.ic_get_communication_object= get_communication_object;
  apic->api_op.ic_get_typed_node_object= get_typed_node_object;
  apic->api_op.ic_free_config= free_cs_config;

  if (!(apic->cluster_conn.cluster_server_ips= (gchar**)
        mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, num_cluster_servers *
                                  sizeof(gchar*))) ||
      !(apic->cluster_conn.cluster_server_ports= (gchar**)
        mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, num_cluster_servers *
                                  sizeof(gchar*))) ||
      !(apic->cluster_conn.cluster_srv_conns= (IC_CONNECTION**)
        mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, num_cluster_servers *
                                   sizeof(IC_CONNECTION*))) ||
      !(apic->config_mutex= g_mutex_new()))
    goto error;

  memcpy((gchar*)apic->cluster_conn.cluster_server_ips,
         (gchar*)cluster_conn->cluster_server_ips,
         num_cluster_servers * sizeof(gchar*));
  memcpy((gchar*)apic->cluster_conn.cluster_server_ports,
         (gchar*)cluster_conn->cluster_server_ports,
         num_cluster_servers * sizeof(gchar*));

  DEBUG_RETURN(apic);
error:
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  DEBUG_RETURN(NULL);
}

/*
  The module below implements translation of a IC_CLUSTER_CONFIG object to a
  an array of key-value pairs divided in sections according to cluster
  server protocol. It also uses the ic_base64_encode routine to convert this
  into a base64 representation which is how it is sent in the cluster server
  protocol.
*/

static guint32
ndb_mgm_str_word_len(guint32 str_len)
{
  guint32 word_len;
  str_len++; /* To accomodate for final NULL */
  str_len++; /* For bug compatability with NDB MGM Protocol */
  word_len= (str_len+3)/4;
  return word_len;
}

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
          len+= ndb_mgm_str_word_len(str_len);
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
          len= ndb_mgm_str_word_len(str_len);
          /* We don't need to copy null byte since we initialised to 0 */
          if (str_len)
            memcpy((gchar*)&assign_array[2],
                   *charptr,
                   str_len);
          DEBUG_PRINT(CONFIG_LEVEL,
                      ("String value = %s, str_len= %u",
                       (gchar*)&assign_array[2], str_len));
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
      DEBUG_PRINT(CONFIG_LEVEL,
                  ("sectid = %u, data_type = %x, config_id = %u, value = %x",
                   sect_id, data_type, config_id, value));
      loc_key_value_array_len+= 2;
    }
  }
  /* Add node type for all sections */
  assign_array= &key_value_array[loc_key_value_array_len];
  config_id= IC_NODE_TYPE;
  key= (IC_CL_INT32_TYPE << IC_CL_KEY_SHIFT) +
       (sect_id << IC_CL_SECT_SHIFT) +
       config_id;
  switch (config_type)
  {
    case IC_COMM_TYPE:
      value= 0;
      break;
    case IC_DATA_SERVER_TYPE:
    case IC_CLUSTER_SERVER_TYPE:
    /* config_type already contains the correct value */
      value= config_type;
      break;
    default:
      if (!is_iclaustron_version(version_number))
        value= IC_CLIENT_TYPE;
      else
        value= config_type;
      break;
  }
  DEBUG_PRINT(CONFIG_LEVEL,
              ("sectid = %u, config_id = %u, value = %u",
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
  DEBUG_PRINT(CONFIG_LEVEL,
              ("sectid = %u, config_id = %u, value = %u",
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
  IC_DATA_SERVER_CONFIG *server_conf;
  IC_DATA_SERVER_CONFIG *client_conf;
  comm_section->first_node_id= node1;
  comm_section->second_node_id= node2;
  if ((local_comm_section= (IC_SOCKET_LINK_CONFIG*)
                           ic_hashtable_search(clu_conf->comm_hash,
                                               (void*)comm_section)))
    return local_comm_section;
  init_config_object((gchar*)comm_section, sizeof(IC_COMM_LINK_CONFIG),
                     IC_COMM_TYPE);
  comm_section->first_node_id= node1;
  comm_section->second_node_id= node2;
  if (clu_conf->node_types[node1] == IC_DATA_SERVER_NODE ||
      clu_conf->node_types[node2] != IC_DATA_SERVER_NODE)
  {
    comm_section->server_node_id= node1;
    server_conf= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[node1];
    client_conf= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[node2];
  }
  else
  {
    comm_section->server_node_id= node2;
    server_conf= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[node2];
    client_conf= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[node1];
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
  DEBUG_PRINT(CONFIG_LEVEL, ("1: len=%u", len));
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
      DEBUG_PRINT(CONFIG_LEVEL, ("2: len=%u", len));
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
          if ((!(clu_conf->node_types[i] == IC_DATA_SERVER_NODE ||
                clu_conf->node_types[j] == IC_DATA_SERVER_NODE)) ||
                ic_is_bit_set(version_number, IC_PROTOCOL_BIT))
            continue;
          /* We have found two nodes needing a comm section */
          comm_section= get_comm_section(clu_conf, &test1, i, j);
          len+= get_length_of_section(IC_COMM_TYPE, (gchar*)comm_section,
                                      version_number);
          num_comms++;
          DEBUG_PRINT(CONFIG_LEVEL, ("4: len=%u", len));
        }
      }
    }
  }
  /*
   * Add one key-value pair for each comm section
   *   - This is section 2 + num_nodes
   */
  len+= num_comms * 2;
  DEBUG_PRINT(CONFIG_LEVEL, ("3: len=%u", len));
  /* Add 1 word for checksum at the end */
  len+= 1;
  DEBUG_PRINT(CONFIG_LEVEL, ("5: len=%u", len));

  /*
     Allocate memory for key-value pairs, this memory is only temporary for
     this method and its caller, so memory will be freed soon again
  */
  if (!(loc_key_value_array= (guint32*)ic_calloc(4*len)))
    return IC_ERROR_MEM_ALLOC;
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
  loc_key_value_array[2]= (IC_SECTION_TYPE << IC_CL_KEY_SHIFT) +
                          (section_id << IC_CL_SECT_SHIFT) +
                          2000;
  loc_key_value_array[3]= node_desc_section << IC_CL_SECT_SHIFT;

  loc_key_value_array[4]= (IC_SECTION_TYPE << IC_CL_KEY_SHIFT) +
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
    loc_key_value_array[loc_key_value_array_len++]=
              (IC_UINT32 << IC_CL_KEY_SHIFT) +
              (section_id << IC_CL_SECT_SHIFT) +
                                              i;
    loc_key_value_array[loc_key_value_array_len++]= (2+i) << IC_CL_SECT_SHIFT;
  }
  DEBUG_PRINT(CONFIG_LEVEL,
    ("1: fill_len=%u", loc_key_value_array_len));
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
    DEBUG_PRINT(CONFIG_LEVEL, ("2: fill_len=%u", loc_key_value_array_len));
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
                                   (IC_UINT32 << IC_CL_KEY_SHIFT) +
                                   (comm_desc_section << IC_CL_SECT_SHIFT) +
                                   i);
    loc_key_value_array[loc_key_value_array_len++]= g_htonl(
                              (comm_desc_section+i+1) << IC_CL_SECT_SHIFT);
  }

  DEBUG_PRINT(CONFIG_LEVEL,
    ("3: fill_len=%u", loc_key_value_array_len));
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
          if ((!(clu_conf->node_types[i] == IC_DATA_SERVER_NODE ||
               clu_conf->node_types[j] == IC_DATA_SERVER_NODE)) ||
               ic_is_bit_set(version_number, IC_PROTOCOL_BIT))
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
          DEBUG_PRINT(CONFIG_LEVEL,
            ("4: fill_len=%u", loc_key_value_array_len));
        }
      }
    }
  }
  /* Calculate and fill out checksum */
  checksum= 0;
  for (i= 0; i < loc_key_value_array_len; i++)
    checksum^= g_ntohl(loc_key_value_array[i]);
  loc_key_value_array[loc_key_value_array_len++]= g_ntohl(checksum);
  DEBUG_PRINT(CONFIG_LEVEL,
    ("5: fill_len=%u", loc_key_value_array_len));
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
  DEBUG_PRINT_BUF(CONFIG_LEVEL, *(gchar**)base64_array);
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
free_run_cluster(IC_RUN_CLUSTER_SERVER *run_obj)
{
  if (run_obj)
    run_obj->run_conn->conn_op.ic_free_connection(run_obj->run_conn); 
  return;
}

static int
rec_get_nodeid_req(IC_CONNECTION *conn,
                   guint64 *node_number,
                   guint64 *version_number,
                   guint64 *node_type,
                   guint64 *cluster_id)
{
  gchar *read_buf;
  guint32 read_size;
  guint32 state= VERSION_REQ_STATE; /* get nodeid already received */
  int error;
  DEBUG_ENTRY("rec_get_nodeid_req");

  while (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    switch (state)
    {
      case VERSION_REQ_STATE:
        if (check_buf_with_int(read_buf, read_size, version_str,
                               VERSION_REQ_LEN, version_number))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in version request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= NODETYPE_REQ_STATE;
        break;
      case NODETYPE_REQ_STATE:
        if (check_buf_with_int(read_buf, read_size, nodetype_str,
                               NODETYPE_REQ_LEN, node_type))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in nodetype request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= NODEID_REQ_STATE;
        break;
      case NODEID_REQ_STATE:
        if (check_buf_with_int(read_buf, read_size, nodeid_str,
                               NODEID_LEN, node_number) ||
            (*node_number > IC_MAX_NODE_ID))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in nodeid request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= USER_REQ_STATE;
        break;
      case USER_REQ_STATE:
        if (check_buf(read_buf, read_size, user_str, USER_REQ_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in user request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= PASSWORD_REQ_STATE;
        break;
      case PASSWORD_REQ_STATE:
        if (check_buf(read_buf, read_size, password_str, PASSWORD_REQ_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in password request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= PUBLIC_KEY_REQ_STATE;
        break;
      case PUBLIC_KEY_REQ_STATE:
        if (check_buf(read_buf, read_size, public_key_str,
                      PUBLIC_KEY_REQ_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in public key request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= ENDIAN_REQ_STATE;
        break;
      case ENDIAN_REQ_STATE:
        if ((read_size < ENDIAN_REQ_LEN) ||
            (memcmp(read_buf, endian_str, ENDIAN_REQ_LEN) != 0))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in endian request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        if (!((read_size == ENDIAN_REQ_LEN + LITTLE_ENDIAN_LEN &&
              memcmp(read_buf+ENDIAN_REQ_LEN, little_endian_str,
                     LITTLE_ENDIAN_LEN) == 0) ||
             (read_size == ENDIAN_REQ_LEN + BIG_ENDIAN_LEN &&
              memcmp(read_buf+ENDIAN_REQ_LEN, big_endian_str,
                     BIG_ENDIAN_LEN) == 0)))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Failure in representation of what endian type"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= LOG_EVENT_REQ_STATE;
        break;
      case LOG_EVENT_REQ_STATE:
        if (check_buf(read_buf, read_size, log_event_str, LOG_EVENT_REQ_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in log_event request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        if (!ic_is_bit_set(*version_number, IC_PROTOCOL_BIT))
        {
          state= EMPTY_LINE_REQ_STATE;
          *cluster_id= 0;
        }
        else
          state= CLUSTER_ID_REQ_STATE;
        break;
      case CLUSTER_ID_REQ_STATE:
        if (check_buf_with_int(read_buf, read_size, cluster_id_str,
                               CLUSTER_ID_REQ_LEN, cluster_id))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in cluster id request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= EMPTY_LINE_REQ_STATE;
        break;
      case EMPTY_LINE_REQ_STATE:
        if (read_size == 0)
        {
          DEBUG_RETURN(0);
        }
        DEBUG_PRINT(CONFIG_LEVEL,
          ("Protocol error in empty line state"));
        PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        break;
      default:
        abort();
        break;
    }
  }
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Error in receiving get node id request, error = %d", error));
  DEBUG_RETURN(error);
}

static int
send_get_nodeid_reply(IC_CONNECTION *conn, guint32 node_id)
{
  gchar nodeid_buf[32];
  int error= 0;
  DEBUG_ENTRY("send_get_nodeid_reply");

  g_snprintf(nodeid_buf, 32, "%s%u", nodeid_str, node_id);
  if (ic_send_with_cr(conn, get_nodeid_reply_str) ||
      ic_send_with_cr(conn, nodeid_buf) ||
      ic_send_with_cr(conn, result_ok_str) ||
      ic_send_with_cr(conn, ic_empty_string))
    error= conn->conn_op.ic_get_error_code(conn);
  DEBUG_RETURN(error);
}

static int
rec_get_config_req(IC_CONNECTION *conn, guint64 version_number)
{
  gchar *read_buf;
  guint32 read_size;
  guint32 state= GET_CONFIG_REQ_STATE;
  guint64 read_version_num;
  int error;
  DEBUG_ENTRY("rec_get_config_req");

  while (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    switch(state)
    {
      case GET_CONFIG_REQ_STATE:
        if (check_buf(read_buf, read_size, get_config_str, GET_CONFIG_LEN))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in get config request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= VERSION_REQ_STATE;
        break;
      case VERSION_REQ_STATE:
        if (check_buf_with_int(read_buf, read_size, version_str,
                               VERSION_REQ_LEN, &read_version_num) ||
            (version_number != read_version_num))
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in version request state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        state= EMPTY_STATE;
        break;
      case EMPTY_STATE:
        if (read_size != 0)
        {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Protocol error in wait empty state"));
          PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
        }
        return 0;
      default:
        abort();
        break;
    }
  }
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Error in receiving get config request, error = %d", error));
  DEBUG_RETURN(error);
}

static int
send_config_reply(IC_CONNECTION *conn, gchar *config_base64_str,
                  guint32 config_len)
{
  gchar content_buf[64];
  int error= 0;
  DEBUG_ENTRY("send_config_reply");
 
  g_snprintf(content_buf, 64, "%s%u", content_len_str, config_len);
  if (ic_send_with_cr(conn, get_config_reply_str) ||
      ic_send_with_cr(conn, result_ok_str) ||
      ic_send_with_cr(conn, content_buf) ||
      ic_send_with_cr(conn, octet_stream_str) ||
      ic_send_with_cr(conn, content_encoding_str) ||
      ic_send_with_cr(conn, ic_empty_string) ||
      conn->conn_op.ic_write_connection(conn, (const void*)config_base64_str,
                                        config_len, 1) ||
      ic_send_with_cr(conn, ic_empty_string))
    error= conn->conn_op.ic_get_error_code(conn);
  DEBUG_RETURN(error);
}

static int
handle_get_cluster_list(IC_RUN_CLUSTER_SERVER *run_obj,
                        IC_CONNECTION *conn)
{
  gchar cluster_id_buf[32];
  gchar cluster_name_buf[128];
  guint32 i;
  int error;
  IC_CLUSTER_CONFIG *clu_conf;
  IC_STRING cluster_name;
  DEBUG_ENTRY("handle_get_cluster_list");

  if ((error= ic_send_with_cr(conn, get_cluster_list_reply_str)))
    goto error;
  for (i= 0; i <= run_obj->max_cluster_id; i++)
  {
    clu_conf= run_obj->conf_objects[i];
    if (!clu_conf)
      continue;
    g_snprintf(cluster_id_buf, 32, "%s%u", cluster_id_str, i);
    cluster_name_buf[0]= 0;
    IC_INIT_STRING(&cluster_name, cluster_name_buf, 0, TRUE);
    ic_add_string(&cluster_name, cluster_name_string);
    ic_add_ic_string(&cluster_name, &clu_conf->clu_info.cluster_name);
    if ((error= ic_send_with_cr(conn, cluster_name.str)) ||
        (error= ic_send_with_cr(conn, cluster_id_buf)))
      goto error;
  }
  if ((error= ic_send_with_cr(conn, end_get_cluster_list_str)))
    goto error;
  return 0;
error:
  DEBUG_PRINT(CONFIG_LEVEL,
              ("Protocol error in get cluster list"));
  PROTOCOL_CONN_CHECK_DEBUG_RETURN(FALSE);
}

static int
handle_get_mgmd_nodeid_request(IC_CONNECTION *conn, guint32 cs_nodeid)
{
  int error;
  gchar cs_nodeid_buf[32];
  g_snprintf(cs_nodeid_buf, 32, "%s%u", nodeid_str, cs_nodeid);
  DEBUG_ENTRY("handle_get_mgmd_nodeid_request");

  if ((error= rec_simple_str(conn, ic_empty_string)) ||
      (error= ic_send_with_cr(conn, get_mgmd_nodeid_reply_str)) ||
      (error= ic_send_with_cr(conn, cs_nodeid_buf)) ||
      (error= ic_send_with_cr(conn, ic_empty_string)))
  {
    DEBUG_PRINT(CONFIG_LEVEL,
                ("Protocol error in converting to transporter"));
    DEBUG_RETURN(error);
  }
  DEBUG_RETURN(0);
}

static int
handle_set_connection_parameter_request(IC_CONNECTION *conn,
                                        guint32 cs_nodeid,
                                        guint32 client_nodeid)
{
  int error;
  gchar cs_nodeid_buf[32];
  gchar client_nodeid_buf[32];
  DEBUG_ENTRY("handle_set_connection_parameter_request");

  g_snprintf(cs_nodeid_buf, 32, "%s%u", nodeid_str, cs_nodeid);
  g_snprintf(client_nodeid_buf, 32, "%s%u", nodeid_str, client_nodeid);
  if ((error= rec_simple_str(conn, client_nodeid_buf)) ||
      (error= rec_simple_str(conn, cs_nodeid_buf)) ||
      (error= ic_send_with_cr(conn, set_connection_parameter_reply_str)) ||
      (error= ic_send_with_cr(conn, result_ok_str)))
  {
    DEBUG_PRINT(CONFIG_LEVEL,
                ("Protocol error in converting to transporter"));
    DEBUG_RETURN(error);
  }
  DEBUG_RETURN(0);
}

static int
handle_convert_transporter_request(IC_CONNECTION *conn, guint32 client_nodeid)
{
  int error;
  int trp_type= IC_TCP_TRANSPORTER_TYPE;
  gchar client_buf[32], cs_buf[32];
  DEBUG_ENTRY("handle_convert_transporter_request");

  g_snprintf(client_buf, 64, "%d %d", client_nodeid, trp_type);
  g_snprintf(cs_buf, 64, "%d %d", client_nodeid, trp_type);
  if ((error= rec_simple_str(conn, ic_empty_string)) ||
      (error= rec_simple_str(conn, client_buf)) ||
      (error= ic_send_with_cr(conn, cs_buf)))
  {
    DEBUG_PRINT(CONFIG_LEVEL,
                ("Protocol error in converting to transporter"));
    DEBUG_RETURN(error);
  }
  DEBUG_RETURN(0);
}

/*
  report event
  ------------
  This protocol is used by the ndbd nodes to report shutdown of their process.
  The data contained in the protocol message is the same as the data sent in
  a EVENT_REP signal used by the NDB Protocol but instead a separate NDB MGM
  Protocol connection is opened up and used to report this special event.

  Data[0]:
  Bit 0-15 contains Event Type (always 27 in this case which means a shutdown
           has been completed).
  Bit 16-31 contains the node id of the node being shutdown.
  Data[1]:
  0:       Means it isn't restarting
  1:       Means restart and not initial restart
  2:       Means start from initial state
  4:       Another variant of initial restart
  Data[2]:
  OS Signal which caused shutdown (e.g. 11 for segmentation fault)

  If the shutdown was caused by an error there are three more words, for
  graceful shutdown only the above words are set.

  Data[4]:
  Error number
  Data[5]:
  Start phase when error occurred
  Data[6]:
  Always equal to 0
  TODO: Should direct output to file instead
*/
static int
handle_report_event(IC_CONNECTION *conn)
{
  guint64 num_array[32], length;
  gchar *read_buf;
  guint32 read_size;
  int error;
  guint32 report_node_id, os_signal_num, error_num= 0, start_phase= 0;
  DEBUG_ENTRY("handle_report_event");

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (check_buf_with_int(read_buf, read_size, length_str,
                          strlen(length_str), &length)) ||
      (error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
      (check_buf_with_many_int(read_buf, read_size, data_str,
                               strlen(data_str), (guint32)length,
                               &num_array[0])))
  {
    error= error ? error : PROTOCOL_ERROR;
    PROTOCOL_CONN_CHECK_ERROR_GOTO(error);
  }
  if ((error= rec_simple_str(conn, ic_empty_string)))
    PROTOCOL_CONN_CHECK_ERROR_GOTO(error);
  if ((error= ic_send_with_cr(conn, report_event_reply_str)) ||
      (error= ic_send_with_cr(conn, result_ok_str)) ||
      (error= ic_send_with_cr(conn, ic_empty_string)))
    goto error;
  report_node_id= num_array[0] >> 16;
  g_assert((num_array[0] & 0xFFFF) == 59);
  if (num_array[1] == 0)
    printf("Node %u has shutdown", report_node_id);
  else if (num_array[1] == 1)
    printf("Node %u has restarted", report_node_id);
  else
    printf("Node %u has performed initial restart", report_node_id);
  if (length == (guint64)3)
  {
    printf(" due to graceful shutdown\n");
  }
  else
  {
    g_assert(length == (guint64)6);
    g_assert(num_array[5] == 0);
    os_signal_num= num_array[2];
    error_num= num_array[3];
    start_phase= num_array[4];
    printf(" due to error %u, OS Signal %u in startphase %u\n",
           error_num, os_signal_num, start_phase);
  }
  DEBUG_RETURN(0);
error:
  DEBUG_RETURN(error);
}

struct ic_rc_param
{
  guint64 node_number;
  guint64 version_number;
  guint64 node_type;
  guint64 cluster_id;
  guint64 client_nodeid;
};
typedef struct ic_rc_param IC_RC_PARAM;

static int
handle_config_request(IC_RUN_CLUSTER_SERVER *run_obj,
                      IC_CONNECTION *conn,
                      IC_RC_PARAM *param)
{
  int ret_code;
  guint8 *config_base64_str;
  guint32 config_len;
  IC_RUN_CLUSTER_STATE *rcs_state= &run_obj->state;
  GMutex *state_mutex= rcs_state->protect_state;
  DEBUG_ENTRY("handle_config_request");

  if ((ret_code= rec_get_nodeid_req(conn,
                                    &param->node_number,
                                    &param->version_number,
                                    &param->node_type,
                                    &param->cluster_id)))
  {
    DEBUG_RETURN(ret_code);
  }
  g_mutex_lock(state_mutex);
  if (rcs_state->cs_started && rcs_state->cs_master)
  {
    ;
  }
  else if (rcs_state->cs_started && !rcs_state->cs_master)
  {
    /* Send an error message to indicate we're not master */
    ;
  }
  else
  {
    /* Send an error message to indicate we're still in start-up phase */
    ;
  }
  g_mutex_unlock(state_mutex);
  if (param->node_number == 0)
  {
    /* Here we need to discover which node id to use */
    param->client_nodeid= 1; /* Fake for now */
  }
  else
  {
    /* Here we ensure that the requested node id is correct */
    param->client_nodeid= 1;
  }
  if ((ret_code= send_get_nodeid_reply(conn, param->client_nodeid)) ||
      (ret_code= rec_get_config_req(conn, param->version_number)) ||
      (ret_code= ic_get_base64_config(run_obj->conf_objects[param->cluster_id],
                                      &config_base64_str,
                                      &config_len,
                                      param->version_number)))
  {
    DEBUG_RETURN(ret_code);
  }
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Converted configuration to a base64 representation"));
  ret_code= send_config_reply(conn, (gchar*)config_base64_str, config_len);
  ic_free((gchar*)config_base64_str);
  DEBUG_RETURN(ret_code);
}

static gpointer
run_handle_config_request(gpointer data)
{
  gchar *read_buf;
  guint32 read_size;
  IC_CONNECTION *conn= (IC_CONNECTION*)data;
  IC_RUN_CLUSTER_SERVER *run_obj;
  int error;
  int state= INITIAL_STATE;
  IC_RC_PARAM param;

  run_obj= (IC_RUN_CLUSTER_SERVER*)conn->conn_op.ic_get_param(conn);
  while (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    switch (state)
    {
      case INITIAL_STATE:
        if (!check_buf(read_buf, read_size, get_cluster_list_str,
                       strlen(get_cluster_list_str)))
        {
          if ((error= handle_get_cluster_list(run_obj, conn)))
          {
          DEBUG_PRINT(CONFIG_LEVEL,
            ("Error from handle_get_cluster_list, code = %u", error));
            goto error;
          }
          state= WAIT_GET_NODEID;
          break;
        }
        if (!check_buf(read_buf, read_size, get_nodeid_str,
                       strlen(get_nodeid_str)))
        {
          /* Handle a request to get configuration for a cluster */
          if ((error= handle_config_request(run_obj, conn, &param)))
          {
            DEBUG_PRINT(CONFIG_LEVEL,
              ("Error from handle_config_request, code = %u", error));
            goto error;
          }
          state= WAIT_GET_MGMD_NODEID;
          break;
        }
        if (!check_buf(read_buf, read_size, report_event_str,
                       strlen(report_event_str)))
        {
          /* Handle report event */
          if ((error= handle_report_event(conn)))
          {
            DEBUG_PRINT(CONFIG_LEVEL,
              ("Error from handle_report_event, code = %u", error));
          }
          break; /* The report event is always done in separate connection */
        }
        goto error;
      case WAIT_GET_NODEID:
        if (!check_buf(read_buf, read_size, get_nodeid_str,
                       strlen(get_nodeid_str)))
        {
          /* Handle a request to get configuration for a cluster */
          if ((error= handle_config_request(run_obj, conn, &param)))
          {
            DEBUG_PRINT(CONFIG_LEVEL,
              ("Error from handle_config_request, code = %u", error));
            goto error;
          }
          state= WAIT_GET_MGMD_NODEID;
          break;
        }
        goto error;
      case WAIT_GET_MGMD_NODEID:
        if (!check_buf(read_buf, read_size, get_mgmd_nodeid_str,
                       strlen(get_mgmd_nodeid_str)))
        {
          if ((error= handle_get_mgmd_nodeid_request(conn,
                                                     run_obj->cs_nodeid)))
          {
            DEBUG_PRINT(CONFIG_LEVEL,
            ("Error from handle_get_mgmd_nodeid_request, code = %u", error));
            goto error;
          }
          state= WAIT_SET_CONNECTION;
          break;
        }
        goto error;
      case WAIT_SET_CONNECTION:
        if (!check_buf(read_buf, read_size, set_connection_parameter_str,
                       strlen(set_connection_parameter_str)))
        {
          if ((error= handle_set_connection_parameter_request(
                            conn,
                            run_obj->cs_nodeid,
                            (guint32)param.client_nodeid)))
          {
            DEBUG_PRINT(CONFIG_LEVEL,
    ("Error from handle_set_connection_parameter_request, code = %u", error));
            goto error;
          }
          state= WAIT_CONVERT_TRANSPORTER;
          break;
        }
        /*
          Here it is ok to fall through, the WAIT_SET_CONNECTION is an
          optional state.
        */
      case WAIT_CONVERT_TRANSPORTER:
        if (!check_buf(read_buf, read_size, convert_transporter_str,
                       strlen(convert_transporter_str)))
        {
          if ((error= handle_convert_transporter_request(conn,
                                          param.client_nodeid)))
          {
            DEBUG_PRINT(CONFIG_LEVEL,
        ("Error from handle_convert_transporter_request, code = %u", error));
            goto error;
          }
          /* At this point the connection is turned into a NDB Protocol connection */
          state= 100; /* Need more work, TODO */
          break;
        }
        goto error;
      default:
        abort();
        break;
    }
  }
  DEBUG_PRINT(CONFIG_LEVEL, ("Connection closed by other side"));
  return NULL;
error:
  return NULL;
}

static int
start_handle_config_request(IC_RUN_CLUSTER_SERVER *run_obj,
                            IC_CONNECTION *conn)
{
  GError *error= NULL;
  int ret_code= 0;
  DEBUG_ENTRY("start_handle_config_request");

  conn->conn_op.ic_set_param(conn, (void*)run_obj);
  if (!g_thread_create_full(run_handle_config_request,
                            (gpointer)conn,
                            1024*16, /* 16 kByte stack size */
                            FALSE,   /* Not joinable        */
                            FALSE,   /* Not bound           */
                            G_THREAD_PRIORITY_NORMAL,
                            &error))
  {
    ret_code= 1;
  }
  DEBUG_RETURN(ret_code);
}


static int
run_cluster_server(IC_RUN_CLUSTER_SERVER *run_obj)
{
  int ret_code= 0;
  IC_CONNECTION *conn, *fork_conn;
  DEBUG_ENTRY("run_cluster_server");

  /*
    Before we start-up our server connection to listen to incoming events
    we first create some socket connections to connect to our fellow
    Cluster Servers. In the start-up phase this is necessary to handle
    synchronisation which Cluster Server becomes the master and who is to
    deliver the current configuration state to the other Cluster Servers.

    After the start-up phase these connections are maintained in an open
    state to ensure we can communicate to the other Cluster Servers at all
    times for changes of the configuration. If we don't manage to set-up
    connections to a certain Cluster Server in the start-up phase we'll
    close this client connection and wait for it to connect to our server
    connection.
  */
  conn= run_obj->run_conn;
  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);
  if (ret_code)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("Failed to set-up listening connection"));
    goto error;
  }
  do
  {
    if ((ret_code= conn->conn_op.ic_accept_connection(conn, NULL, NULL)))
    {
      DEBUG_PRINT(COMM_LEVEL,
        ("Failed to accept a new connection"));
      goto error;
    }
    DEBUG_PRINT(CONFIG_LEVEL,
("Run cluster server has set up connection and has received a connection"));
    if (!(fork_conn=
           conn->conn_op.ic_fork_accept_connection(conn,
                                          FALSE))) /* No mutex */
    {
      DEBUG_PRINT(COMM_LEVEL,
      ("Failed to fork a new connection from an accepted connection"));
      goto error;
    }
    if ((ret_code= start_handle_config_request(run_obj, fork_conn)))
    {
      DEBUG_PRINT(THREAD_LEVEL,
        ("Start new thread to handle configuration request failed"));
      goto error;
    }
    DEBUG_PRINT(CONFIG_LEVEL,
      ("Ready to accept a new connection"));
  } while (1);

error:
  DEBUG_RETURN(ret_code);
}

IC_RUN_CLUSTER_SERVER*
ic_create_run_cluster(IC_CLUSTER_CONFIG **clusters,
                      IC_MEMORY_CONTAINER *mc_ptr,
                      gchar *server_name,
                      gchar *server_port,
                      guint32 my_node_id)
{
  IC_RUN_CLUSTER_SERVER *run_obj;
  IC_CONNECTION *conn;
  guint32 max_cluster_id, cluster_id;
  IC_CLUSTER_CONFIG **cluster_iterator;
  IC_CLUSTER_CONFIG **cluster_array;
  DEBUG_ENTRY("ic_create_run_cluster");

  /*
    Iterate over the clusters to find the maximum cluster id, this is later
    used to build an index into the cluster configuration structs using
    cluster id.
  */
  max_cluster_id= 0;
  cluster_iterator= clusters;
  while (*cluster_iterator)
  {
    max_cluster_id= IC_MAX(max_cluster_id,
                           (*cluster_iterator)->clu_info.cluster_id);
    cluster_iterator++;
  }
  if (!(run_obj= (IC_RUN_CLUSTER_SERVER*)mc_ptr->mc_ops.ic_mc_calloc(
                mc_ptr, sizeof(IC_RUN_CLUSTER_SERVER))))
  {
    DEBUG_RETURN(NULL);
  }
  if (!(cluster_array= (IC_CLUSTER_CONFIG**)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr,
             sizeof(IC_CLUSTER_CONFIG*) * (max_cluster_id + 1))))
  {
    DEBUG_RETURN(NULL);
  }

  /*
    Initialise the Cluster Server state, the state is protected by a mutex to
    ensure when several connections receive requests only one at a time can
    change the Cluster Server state.
  */
  if (!(run_obj->state.protect_state= g_mutex_new()))
  {
    DEBUG_RETURN(NULL);
  }
  run_obj->state.cs_master= FALSE;
  run_obj->state.cs_started= FALSE;
  run_obj->state.bootstrap= FALSE;
  run_obj->state.config_dir= NULL;

  cluster_iterator= clusters;
  while (*cluster_iterator)
  {
    cluster_id= (*cluster_iterator)->clu_info.cluster_id;
    if (cluster_array[cluster_id])
    {
      printf("Configured two clusters with same id\n");
      DEBUG_RETURN(NULL);
    }
    cluster_array[cluster_id]= *cluster_iterator;
    cluster_iterator++;
  }
  run_obj->conf_objects= cluster_array;
  run_obj->max_cluster_id= max_cluster_id;
  run_obj->cs_nodeid= my_node_id;
  if (!(run_obj->run_conn= ic_create_socket_object(
                            FALSE, /* Server connection */
                            FALSE, /* Don't use mutex */
                            FALSE, /* Don't use connect thread */
                            CONFIG_READ_BUF_SIZE,
                            NULL,  /* Don't use authentication function */
                            NULL))) /* No authentication object */
    goto error;

  conn= run_obj->run_conn;
  conn->conn_op.ic_prepare_server_connection(conn,
                                             server_name,
                                             server_port,
                                             NULL,
                                             NULL,
                                             0,
                                             TRUE);
  run_obj->run_op.ic_run_cluster_server= run_cluster_server;
  run_obj->run_op.ic_free_run_cluster= free_run_cluster;
  DEBUG_RETURN(run_obj);

error:
  DEBUG_RETURN(NULL);
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
    case IC_DATA_SERVER_TYPE:
    {
      IC_DATA_SERVER_CONFIG *data_server_conf=
             (IC_DATA_SERVER_CONFIG*)current_config;
      mandatory_bits= data_server_mandatory_bits;
      if (data_server_conf->mandatory_bits != data_server_mandatory_bits)
        goto mandatory_error;
      if (data_server_conf->filesystem_path == NULL)
        data_server_conf->filesystem_path= data_server_conf->node_data_path;
      if (data_server_conf->data_server_checkpoint_path == NULL)
        data_server_conf->data_server_checkpoint_path=
          data_server_conf->filesystem_path;
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
         ((IC_DATA_SERVER_CONFIG*)current_config)->mandatory_bits;
  {
    __attribute__ ((unused)) gchar buf[128];
    DEBUG_PRINT(CONFIG_LEVEL,
      ("mandatory bits %s",
      ic_guint64_hex_str(((IC_DATA_SERVER_CONFIG*)
                          current_config)->mandatory_bits,
                         (gchar*)&buf)));
    DEBUG_PRINT(CONFIG_LEVEL,
      ("missing bits %s",
      ic_guint64_hex_str(missing_bits,
                         (gchar*)&buf)));
  }
  g_assert(missing_bits);
  for (i= 0; i < 64; i++)
  {
    bit64= 1;
    bit64 <<= i;
    if (missing_bits & bit64)
    {
      if (!(conf_entry= get_config_entry_mandatory(i, conf_type)))
      {
        DEBUG_PRINT(CONFIG_LEVEL,
                    ("Didn't find mandatory entry after config error, i= %u"
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
  IC_CLUSTER_CONFIG *clu_conf_perm;
  IC_MEMORY_CONTAINER *temp_mc_ptr;
  IC_MEMORY_CONTAINER *perm_mc_ptr;
  guint32 max_node_id;
  guint32 size_structs= 0;
  DEBUG_ENTRY("conf_serv_init");
  perm_mc_ptr= conf->perm_mc_ptr;
  if (pass == INITIAL_PASS)
  {
    /*
      In this initial phase we allocate a temporary memory container and we
      set up the pointers for deallocation, the memory to be returned is
      allocated on a memory container supplied by the caller.
    */
    if (!(temp_mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
    {
      DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    }
    if (!(clu_conf= (IC_CLUSTER_CONFIG_LOAD*)temp_mc_ptr->mc_ops.ic_mc_calloc(
                    temp_mc_ptr, sizeof(IC_CLUSTER_CONFIG_LOAD))))
    {
      temp_mc_ptr->mc_ops.ic_mc_free(temp_mc_ptr);
      DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    }
    conf->config_ptr.clu_conf= (struct ic_cluster_config_load*)clu_conf;
    clu_conf->temp_mc_ptr= temp_mc_ptr;
    if (!(clu_conf_perm= (IC_CLUSTER_CONFIG*)perm_mc_ptr->mc_ops.ic_mc_calloc(
                         perm_mc_ptr, sizeof(IC_CLUSTER_CONFIG))))
    {
      DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    }
    clu_conf->conf= clu_conf_perm;
    clu_conf->current_node_config_type= IC_NO_CONFIG_TYPE;
    DEBUG_RETURN(0);
  }
  clu_conf= conf->config_ptr.clu_conf;
  max_node_id= clu_conf->conf->max_node_id;
  if (max_node_id == 0)
  {
    DEBUG_RETURN(IC_ERROR_NO_NODES_FOUND);
  }
  clu_conf->current_node_config_type= IC_NO_CONFIG_TYPE;
  /*
    Calculate size of all node struct's and allocate them in one chunk.
  */
  size_structs+= clu_conf->conf->num_data_servers *
                 sizeof(IC_DATA_SERVER_CONFIG);
  size_structs+= clu_conf->conf->num_clients * sizeof(IC_CLIENT_CONFIG);
  size_structs+= clu_conf->conf->num_cluster_servers *
                 sizeof(IC_CLUSTER_SERVER_CONFIG);
  size_structs+= clu_conf->conf->num_sql_servers *
                 sizeof(IC_SQL_SERVER_CONFIG);
  size_structs+= clu_conf->conf->num_rep_servers *
                 sizeof(IC_REP_SERVER_CONFIG);
  size_structs+= clu_conf->conf->num_file_servers *
                 sizeof(IC_FILE_SERVER_CONFIG);
  size_structs+= clu_conf->conf->num_restore_nodes *
                 sizeof(IC_RESTORE_CONFIG);
  size_structs+= clu_conf->conf->num_cluster_mgrs *
                   sizeof(IC_CLUSTER_MGR_CONFIG);
  size_structs+= clu_conf->conf->num_comms *
                 sizeof(IC_SOCKET_LINK_CONFIG);

  if (!(clu_conf->struct_memory= (gchar*)
        perm_mc_ptr->mc_ops.ic_mc_calloc(perm_mc_ptr, size_structs)))
  {
    DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
  }
  if (!(clu_conf->conf->node_config= (gchar**)
        perm_mc_ptr->mc_ops.ic_mc_calloc(perm_mc_ptr,
                                sizeof(gchar*)*(max_node_id + 1))))
  {
    DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
  }
  if (!(clu_conf->string_memory= (gchar*)
        perm_mc_ptr->mc_ops.ic_mc_calloc(perm_mc_ptr,
                                         clu_conf->size_string_memory)))
  {
    DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
  }
  if (!(clu_conf->conf->node_types= (IC_NODE_TYPES*)
        perm_mc_ptr->mc_ops.ic_mc_calloc(perm_mc_ptr,
                             sizeof(IC_NODE_TYPES)*(max_node_id+1))))
  {
    DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
  }

  init_config_object((gchar*)&clu_conf->default_data_server_config,
                     sizeof(clu_conf->default_data_server_config),
                     IC_DATA_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_client_config,
                     sizeof(clu_conf->default_client_config),
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_cluster_server_config,
                     sizeof(clu_conf->default_cluster_server_config),
                     IC_CLUSTER_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_sql_server_config.client_conf,
                     sizeof(clu_conf->default_sql_server_config.client_conf),
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_sql_server_config,
                     sizeof(clu_conf->default_sql_server_config),
                     IC_SQL_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_rep_server_config.client_conf,
                     sizeof(clu_conf->default_rep_server_config.client_conf),
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_rep_server_config,
                     sizeof(clu_conf->default_rep_server_config),
                     IC_REP_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_file_server_config.client_conf,
                     sizeof(clu_conf->default_file_server_config.client_conf),
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_file_server_config,
                     sizeof(clu_conf->default_file_server_config),
                     IC_FILE_SERVER_TYPE);
  init_config_object((gchar*)&clu_conf->default_restore_config.client_conf,
                     sizeof(clu_conf->default_restore_config.client_conf),
                     IC_CLIENT_TYPE);
  init_config_object((gchar*)&clu_conf->default_restore_config,
                     sizeof(clu_conf->default_restore_config),
                     IC_RESTORE_TYPE);
  init_config_object((gchar*)&clu_conf->default_cluster_mgr_config,
                     sizeof(clu_conf->default_cluster_mgr_config),
                     IC_CLUSTER_MGR_TYPE);
  init_config_object((gchar*)&clu_conf->default_socket_config,
                     sizeof(clu_conf->default_socket_config),
                     IC_COMM_TYPE);
  DEBUG_RETURN(0);
}

static void
init_node(IC_CLUSTER_CONFIG_LOAD *clu_conf, size_t size, void *config_struct)
{
  clu_conf->current_node_config= (void*)clu_conf->struct_memory;
  clu_conf->struct_memory+= size;
  memcpy(clu_conf->current_node_config, config_struct, size);
}

static int
conf_serv_add_section(void *ic_config,
                      __attribute__ ((unused)) guint32 section_number,
                      guint32 line_number,
                      IC_STRING *section_name,
                      guint32 pass)
{
  int error;
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_add_section");
  DEBUG_IC_STRING(CONFIG_LEVEL, section_name);

  if ((error= complete_section(ic_config, line_number, pass)))
    DEBUG_RETURN(error);
  clu_conf->default_section= FALSE;
  if (ic_cmp_null_term_str(data_server_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_DATA_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_nodes++;
      clu_conf->conf->num_data_servers++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_DATA_SERVER_CONFIG),
              (void*)&clu_conf->default_data_server_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found data server group"));
  }
  else if (ic_cmp_null_term_str(client_node_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_CLIENT_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_clients++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_CLIENT_CONFIG),
              (void*)&clu_conf->default_client_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found client group"));
  }
  else if (ic_cmp_null_term_str(cluster_server_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_CLUSTER_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_cluster_servers++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_CLUSTER_SERVER_CONFIG),
              (void*)&clu_conf->default_cluster_server_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found cluster server group"));
  }
  else if (ic_cmp_null_term_str(sql_server_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_SQL_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_sql_servers++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_SQL_SERVER_CONFIG),
              (void*)&clu_conf->default_sql_server_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found sql server group"));
  }
  else if (ic_cmp_null_term_str(rep_server_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_REP_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_rep_servers++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_REP_SERVER_CONFIG),
              (void*)&clu_conf->default_rep_server_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found replication server group"));
  }
  else if (ic_cmp_null_term_str(file_server_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_FILE_SERVER_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_file_servers++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_FILE_SERVER_CONFIG),
              (void*)&clu_conf->default_file_server_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found file server group"));
  }
  else if (ic_cmp_null_term_str(restore_node_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_RESTORE_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_restore_nodes++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_RESTORE_CONFIG),
              (void*)&clu_conf->default_restore_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found restore node group"));
  }
  else if (ic_cmp_null_term_str(cluster_mgr_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_CLUSTER_MGR_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_cluster_mgrs++;
      clu_conf->conf->num_nodes++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_CLUSTER_MGR_CONFIG),
              (void*)&clu_conf->default_cluster_mgr_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found cluster manager group"));
  }
  else if (ic_cmp_null_term_str(socket_str, section_name) == 0)
  {
    clu_conf->current_node_config_type= IC_COMM_TYPE;
    if (pass == INITIAL_PASS)
    {
      clu_conf->conf->num_comms++;
      DEBUG_RETURN(0);
    }
    init_node(clu_conf, sizeof(IC_SOCKET_LINK_CONFIG),
              (void*)&clu_conf->default_socket_config);
    DEBUG_PRINT(CONFIG_LEVEL, ("Found socket group"));
  }
  else
  {
    clu_conf->default_section= TRUE;
    if (ic_cmp_null_term_str(data_server_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_data_server_config;
      clu_conf->current_node_config_type= IC_DATA_SERVER_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found data server default group"));
    }
    else if (ic_cmp_null_term_str(client_node_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_client_config;
      clu_conf->current_node_config_type= IC_CLIENT_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found client default group"));
    }
    else if (ic_cmp_null_term_str(cluster_server_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_cluster_server_config;
      clu_conf->current_node_config_type= IC_CLUSTER_SERVER_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found cluster server default group"));
    }
    else if (ic_cmp_null_term_str(sql_server_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_sql_server_config;
      clu_conf->current_node_config_type= IC_SQL_SERVER_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found sql server default group"));
    }
    else if (ic_cmp_null_term_str(rep_server_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_rep_server_config;
      clu_conf->current_node_config_type= IC_REP_SERVER_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found replication server default group"));
    }
    else if (ic_cmp_null_term_str(file_server_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_file_server_config;
      clu_conf->current_node_config_type= IC_FILE_SERVER_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found file server default group"));
    }
    else if (ic_cmp_null_term_str(restore_node_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_restore_config;
      clu_conf->current_node_config_type= IC_RESTORE_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found restore default group"));
    }
    else if (ic_cmp_null_term_str(cluster_mgr_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_cluster_mgr_config;
      clu_conf->current_node_config_type= IC_CLUSTER_MGR_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found cluster_mgr default group"));
    }
    else if (ic_cmp_null_term_str(socket_def_str, section_name) == 0)
    {
      clu_conf->current_node_config= &clu_conf->default_socket_config;
      clu_conf->current_node_config_type= IC_COMM_TYPE;
      DEBUG_PRINT(CONFIG_LEVEL, ("Found socket default group"));
    }
    else
    {
      DEBUG_PRINT(CONFIG_LEVEL, ("No such config section"));
      DEBUG_RETURN(IC_ERROR_CONFIG_NO_SUCH_SECTION);
    }
  }
  DEBUG_RETURN(0);
}

static
int conf_serv_add_key(void *ic_config,
                      __attribute__ ((unused)) guint32 section_number,
                      __attribute__ ((unused)) guint32 line_number,
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
  __attribute__ ((unused)) gchar buf[128];
  DEBUG_ENTRY("conf_serv_add_key");
  DEBUG_IC_STRING(CONFIG_LEVEL, key_name);
  DEBUG_IC_STRING(CONFIG_LEVEL, data);
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Line: %d, Section: %d, Key-value pair", (int)line_number,
     (int)section_number));
  if (clu_conf->current_node_config_type == IC_NO_CONFIG_TYPE)
    DEBUG_RETURN(IC_ERROR_NO_SECTION_DEFINED_YET);
  if (!(conf_entry= (IC_CONFIG_ENTRY*)ic_hashtable_search(glob_conf_hash,
                                                          (void*)key_name)))
    DEBUG_RETURN(IC_ERROR_NO_SUCH_CONFIG_KEY);
  struct_ptr= (gchar*)clu_conf->current_node_config + conf_entry->offset;
  if (!(conf_entry->config_types & (1 << clu_conf->current_node_config_type)))
    DEBUG_RETURN(IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION);
  if (conf_entry->is_mandatory && (pass != INITIAL_PASS))
  {
    ((IC_DATA_SERVER_CONFIG*)clu_conf->current_node_config)->mandatory_bits|=
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
    DEBUG_RETURN(0);
  }
  if (ic_conv_config_str_to_int(&value, data))
    DEBUG_RETURN(IC_ERROR_WRONG_CONFIG_NUMBER);
  if (conf_entry->is_boolean && value > 1)
    DEBUG_RETURN(IC_ERROR_NO_BOOLEAN_VALUE);
  num32_check= 1;
  num32_check<<= 32;
  if (!ic_cmp_null_term_str(node_id_str, key_name))
  {
    /*
      We have found a node id
    */
    clu_conf->conf->max_node_id= MAX(value, clu_conf->conf->max_node_id);
    if (!clu_conf->default_section && pass != INITIAL_PASS)
    {
      if (clu_conf->conf->node_config[value])
      {
        DEBUG_PRINT(CONFIG_LEVEL,
          ("Trying to define node %u twice", (guint32)value));
        DEBUG_RETURN(IC_ERROR_NODE_ALREADY_DEFINED);
      }
      clu_conf->conf->node_config[value]= (gchar*)clu_conf->current_node_config;
      clu_conf->conf->node_types[value]= 
        clu_conf->current_node_config_type;
    }
  }
  if (conf_entry->is_min_value_defined && conf_entry->min_value > value)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("Parameter %s is smaller than min_value = %u",
       ic_get_ic_string(key_name, (gchar*)&buf), conf_entry->min_value));
    DEBUG_RETURN(IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS);
  }
  else if (conf_entry->is_max_value_defined && conf_entry->max_value < value)
  {
    DEBUG_PRINT(CONFIG_LEVEL,
      ("Parameter %s is larger than min_value = %u",
      ic_get_ic_string(key_name, (gchar*)&buf), conf_entry->max_value));
    DEBUG_RETURN(IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS);
  }
  else if ((conf_entry->data_type == IC_UINT16 && value > 65535) ||
           (conf_entry->data_type == IC_CHAR && value > 255) ||
           (conf_entry->data_type == IC_BOOLEAN && value > 1) ||
           (conf_entry->data_type == IC_UINT32 && value >= num32_check))
  {
    DEBUG_PRINT(CONFIG_LEVEL, ("Parameter %s is larger than its type",
           ic_get_ic_string(key_name, (gchar*)&buf)));
    DEBUG_RETURN(IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS);
  }
  if (pass == INITIAL_PASS)
    DEBUG_RETURN(0);
  /* Assign value of configuration variable according to its data type.  */
  if (conf_entry->data_type == IC_CHAR ||
      conf_entry->data_type == IC_BOOLEAN)
    *(guint8*)struct_ptr= (guint8)value;
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
    DEBUG_RETURN(1);
  }
  DEBUG_RETURN(0);
}

static
int conf_serv_add_comment(void *ic_config,
                          __attribute__ ((unused)) guint32 section_number,
                          __attribute__ ((unused)) guint32 line_number,
                          __attribute__ ((unused)) IC_STRING *comment,
                          guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_add_comment");
  DEBUG_PRINT(CONFIG_LEVEL,
              ("Line number %d in section %d was comment line",
               line_number, section_number));
  if (pass == INITIAL_PASS)
    clu_conf->comments.num_comments++;
  DEBUG_RETURN(0);
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

static void
conf_init_end(void *ic_conf)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_conf;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  if (clu_conf)
  {
    /*
      We are now ready to release the temporary data structures we needed
      to generate the configuration data structures. The permanent data
      structures are released by the caller of the building of the
      configuration data since many configuration files can use the same
      memory container.
    */
    if (clu_conf->temp_mc_ptr)
      clu_conf->temp_mc_ptr->mc_ops.ic_mc_free(clu_conf->temp_mc_ptr);
  }
  return;
}

static
void conf_serv_end(void *ic_conf)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_conf;
  IC_CLUSTER_CONFIG_LOAD *clu_conf= conf->config_ptr.clu_conf;
  DEBUG_ENTRY("conf_serv_end");
  if (clu_conf)
  {
    if (clu_conf->conf->comm_hash)
      ic_hashtable_destroy(clu_conf->conf->comm_hash);
  }
  DEBUG_RETURN_EMPTY;
}

static IC_CONFIG_OPERATIONS config_server_ops =
{
  .ic_config_init = conf_serv_init,
  .ic_add_section = conf_serv_add_section,
  .ic_add_key     = conf_serv_add_key,
  .ic_add_comment = conf_serv_add_comment,
  .ic_config_verify = conf_serv_verify_conf,
  .ic_init_end    = conf_init_end,
  .ic_config_end  = conf_serv_end,
};

IC_CLUSTER_CONFIG*
ic_load_config_server_from_files(gchar *config_file,
                                 IC_CONFIG_STRUCT *conf_server)
{
  gchar *conf_data_str;
  guint64 conf_data_len;
  IC_STRING conf_data;
  int ret_val;
  IC_CONFIG_ERROR err_obj;
  IC_CLUSTER_CONFIG *ret_ptr;
  DEBUG_ENTRY("ic_load_config_server_from_files");

  conf_server->clu_conf_ops= &config_server_ops;
  conf_server->config_ptr.clu_conf= NULL;
  DEBUG_PRINT(CONFIG_LEVEL, ("config_file = %s", config_file));
  if (ic_get_file_contents(config_file, &conf_data_str,
                           &conf_data_len))
    goto file_open_error;

  IC_INIT_STRING(&conf_data, conf_data_str, conf_data_len, TRUE);
  ret_val= ic_build_config_data(&conf_data, conf_server, &err_obj);
  ic_free(conf_data.str);
  if (ret_val == 1)
  {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Error at Line number %u:\n%s\n", err_obj.line_number,
          ic_get_error_message(err_obj.err_num));
    ret_ptr= NULL;
  }
  else
  {
    ret_ptr= conf_server->config_ptr.clu_conf->conf;
    if (build_hash_on_comms(ret_ptr))
    {
      conf_serv_end(conf_server);
      ret_ptr= NULL;
    }
  }
  config_server_ops.ic_init_end(conf_server);
  DEBUG_RETURN(ret_ptr);

file_open_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Couldn't open file %s\n", config_file);
  DEBUG_RETURN(NULL);
}

static void
init_cluster_params(IC_CLUSTER_CONFIG_TEMP *temp)
{
  memset(&temp->cluster_name, 0, sizeof(IC_STRING));
  memset(&temp->password, 0, sizeof(IC_STRING));
  temp->cluster_id= IC_MAX_UINT32;
}

static void
set_cluster_config_string(IC_STRING *dest_str, IC_STRING *in_str,
                          IC_CLUSTER_CONFIG_TEMP *temp)
{
  memcpy(dest_str, in_str, sizeof(IC_STRING));
  dest_str->str= temp->string_memory;
  dest_str->str[in_str->len]= 0;
  dest_str->is_null_terminated= TRUE;
  temp->string_memory+= (in_str->len + 1);
}

static void
set_cluster_config(IC_CLUSTER_CONNECT_INFO *clu_info,
                   IC_CLUSTER_CONFIG_TEMP *temp)
{
  memcpy(&clu_info->cluster_name,
         &temp->cluster_name, sizeof(IC_STRING));
  memcpy(&clu_info->password,
         &temp->password, sizeof(IC_STRING));
  clu_info->cluster_id= temp->cluster_id;
}

static int
cluster_config_init(void *ic_config, guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_TEMP *temp;
  IC_CLUSTER_CONNECT_INFO **clusters;
  IC_MEMORY_CONTAINER *mc_ptr= conf->perm_mc_ptr;
  guint32 i;
  DEBUG_ENTRY("cluster_config_init");

  if (pass == INITIAL_PASS)
  {
    if (!(temp= (IC_CLUSTER_CONFIG_TEMP*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr,
                      sizeof(IC_CLUSTER_CONFIG_TEMP))))
      DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    temp->string_memory= NULL;
    conf->config_ptr.cluster_conf= temp;
  }
  else
  {
    temp= conf->config_ptr.cluster_conf;
    if (!(clusters= (IC_CLUSTER_CONNECT_INFO**)
           mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
           (temp->num_clusters + 1) * sizeof(IC_CLUSTER_CONNECT_INFO*))))
      DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    temp->clu_info= clusters;
    for (i= 0; i < temp->num_clusters; i++)
    {
      if (!(clusters[i]= (IC_CLUSTER_CONNECT_INFO*)
          mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
            sizeof(IC_CLUSTER_CONNECT_INFO))))
        DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    }
    if (!(temp->string_memory= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr,
          temp->size_string_memory)))
      DEBUG_RETURN(IC_ERROR_MEM_ALLOC);
    clusters[temp->num_clusters]= NULL;
  }
  temp->num_clusters= 0;
  temp->init_state= TRUE;
  temp->size_string_memory= 0;
  init_cluster_params(temp);
  DEBUG_RETURN(0);
}

static int
cluster_config_add_section(void *ic_config,
                           __attribute ((unused)) guint32 section_number,
                           __attribute ((unused)) guint32 line_number,
                           IC_STRING *section_name,
                           guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_TEMP *temp;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  DEBUG_ENTRY("cluster_config_add_section");

  temp= conf->config_ptr.cluster_conf;
  if (ic_cmp_null_term_str(cluster_str, section_name) == 0)
  {
    /*
       Found a cluster section (the only allowed one in this type of
       configuration file.
    */
    if (pass != INITIAL_PASS && !temp->init_state)
    {
      /*
        We have completed a section and need to copy the data over to the
        IC_CLUSTER_CONFIG struct.
      */
      clu_info= temp->clu_info[temp->num_clusters-1];
      set_cluster_config(clu_info, temp);
    }
    init_cluster_params(temp);
    temp->num_clusters++;
    temp->init_state= FALSE;
  }
  else
  {
    DEBUG_PRINT(CONFIG_LEVEL, ("No such cluster config section"));
    DEBUG_RETURN(IC_ERROR_CONFIG_NO_SUCH_SECTION);
  }
  return 0;
}

static int
cluster_config_add_key(void *ic_config,
                       __attribute__ ((unused)) guint32 section_number,
                       __attribute__ ((unused)) guint32 line_number,
                       IC_STRING *key_name,
                       IC_STRING *data,
                       guint32 pass)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_TEMP *temp;
  guint64 value;
  DEBUG_ENTRY("cluster_config_add_key");

  DEBUG_IC_STRING(CONFIG_LEVEL, key_name);
  DEBUG_IC_STRING(CONFIG_LEVEL, data);
  DEBUG_PRINT(CONFIG_LEVEL,
    ("Line: %d, Section: %d, Key-value pair", (int)line_number,
     (int)section_number));

  temp= conf->config_ptr.cluster_conf;
  if (temp->init_state)
    DEBUG_RETURN(IC_ERROR_NO_SECTION_DEFINED_YET);
  if (pass == INITIAL_PASS)
    temp->size_string_memory+= (data->len + 1);
  else
    memcpy(temp->string_memory, data->str, data->len);
  if (ic_cmp_null_term_str(cluster_name_str, key_name) == 0)
  {
    if (pass != INITIAL_PASS)
      set_cluster_config_string(&temp->cluster_name, data, temp);
  }
  else if (ic_cmp_null_term_str(cluster_password_str, key_name) == 0)
  {
    if (pass != INITIAL_PASS)
      set_cluster_config_string(&temp->password, data, temp);
  }
  else if (ic_cmp_null_term_str(cluster_id_string, key_name) == 0)
  {
    if (pass != INITIAL_PASS)
    {
      if (ic_conv_config_str_to_int(&value, data))
        DEBUG_RETURN(IC_ERROR_WRONG_CONFIG_NUMBER);
      temp->cluster_id= (guint32)value;
      if (temp->cluster_id > IC_MAX_CLUSTER_ID)
        DEBUG_RETURN(IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS);
    }
  }
  else
    DEBUG_RETURN(IC_ERROR_NO_SUCH_CONFIG_KEY);
  return 0;
}

static int
cluster_config_add_comment(__attribute__ ((unused)) void *ic_config,
                           __attribute__ ((unused)) guint32 section_number,
                           __attribute__ ((unused)) guint32 line_number,
                           __attribute__ ((unused)) IC_STRING *comment,
                           __attribute__ ((unused)) guint32 pass)
{
  return 0;
}

static int
cluster_config_verify(__attribute__ ((unused)) void *ic_config)
{
  return 0;
}

static void
cluster_config_init_end(void *ic_config)
{
  IC_CONFIG_STRUCT *conf= (IC_CONFIG_STRUCT*)ic_config;
  IC_CLUSTER_CONFIG_TEMP *temp;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  DEBUG_ENTRY("cluster_config_init_end");

  temp= conf->config_ptr.cluster_conf;
  if (temp->num_clusters > 0)
  {
    clu_info= temp->clu_info[temp->num_clusters-1];
    set_cluster_config(clu_info, temp);
  }
  return;
}

static void
cluster_config_end(__attribute__ ((unused)) void *ic_config)
{
  return;
}

/*
  These methods are used to implement the configuration change algorithms
  whereby we can change a configuration in a coherent manner.
*/

static int
remove_config_files(IC_STRING *config_dir,
                    IC_CLUSTER_CONNECT_INFO **clu_infos,
                    guint32 config_version)
{
  int error;
  IC_STRING file_name_string;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];

  ic_create_config_file_name(&file_name_string, file_name,
                             config_dir, &ic_config_string,
                             config_version);
  if ((error= ic_delete_file((const gchar *)file_name)))
    return error;
  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
    IC_INIT_STRING(&file_name_string, file_name, 0, TRUE);
    ic_create_config_file_name(&file_name_string, file_name, config_dir,
                               &clu_info->cluster_name,
                               config_version);
    if ((error= ic_delete_file((const gchar *)file_name)))
      return error;
  }
  return 0;
}

static guint64
get_node_data(const gchar *struct_ptr, guint32 offset,
              IC_CONFIG_DATA_TYPE data_type)
{
  guint64 value= 0;
  switch (data_type)
  {
    case IC_BOOLEAN:
    case IC_CHAR:
      value= (guint64)*(guint8*)(struct_ptr+offset);
      break;
    case IC_UINT16:
      value= (guint64)*(guint16*)(struct_ptr+offset);
      break;
    case IC_UINT32:
      value= (guint64)*(guint32*)(struct_ptr+offset);
      break;
    case IC_UINT64:
      value= *(guint64*)(struct_ptr+offset);
      break;
    default:
      g_assert(FALSE);
  }
  return value;
}

static int
write_new_section_header(IC_DYNAMIC_ARRAY *dyn_array,
                         IC_DYNAMIC_ARRAY_OPS *da_ops,
                         gchar *buf,
                         const gchar *section_name,
                         gboolean first_call)
{
  int error= IC_ERROR_MEM_ALLOC;
  if (!first_call)
  {
    buf[0]= CARRIAGE_RETURN;
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
      goto error;
  }
  buf[0]= '[';
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
    goto error;
  if (da_ops->ic_insert_dynamic_array(dyn_array, section_name,
                                      (guint32)strlen(section_name)))
    goto error;
  buf[0]= ']';
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
    goto error;
  buf[0]= CARRIAGE_RETURN;
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
    goto error;
  error= 0;
error:
  return error;
}

static int
write_line_with_int_value(IC_DYNAMIC_ARRAY *dyn_array,
                          IC_DYNAMIC_ARRAY_OPS *da_ops,
                          gchar *buf,
                          IC_STRING *name_var,
                          guint64 value)
{
  int error= IC_ERROR_MEM_ALLOC;
  if (da_ops->ic_insert_dynamic_array(dyn_array, name_var->str, name_var->len))
    goto error;

  buf[0]= ':';
  buf[1]= ' ';
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)2))
    goto error;

  if (!ic_guint64_str(value, buf, NULL))
    goto error;
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, strlen(buf)))
    goto error;
  
  buf[0]= CARRIAGE_RETURN;
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
    goto error;
  error= 0;
error:
  return error;
}

static int
write_line_with_char_value(IC_DYNAMIC_ARRAY *dyn_array,
                           IC_DYNAMIC_ARRAY_OPS *da_ops,
                           gchar *buf,
                           IC_STRING *name_var,
                           gchar *val_str)
{
  int error= IC_ERROR_MEM_ALLOC;
  if (da_ops->ic_insert_dynamic_array(dyn_array, name_var->str, name_var->len))
    goto error;

  buf[0]= ':';
  buf[1]= ' ';
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)2))
    goto error;

  if (da_ops->ic_insert_dynamic_array(dyn_array, val_str, strlen(val_str)))
    goto error;

  buf[0]= CARRIAGE_RETURN;
  if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
    goto error;
  error= 0;
error:
  return error;
}

static int
write_cluster_config_file(IC_STRING *config_dir,
                          IC_CLUSTER_CONNECT_INFO **clu_infos,
                          guint32 config_version)
{
  IC_DYNAMIC_ARRAY *dyn_array;
  IC_CLUSTER_CONNECT_INFO *clu_info;
  IC_DYNAMIC_ARRAY_OPS *da_ops;
  IC_STRING file_name_str;
  int error= IC_ERROR_MEM_ALLOC;
  int file_ptr;
  gboolean first_call= TRUE;
  gchar buf[IC_MAX_FILE_NAME_SIZE];

  ic_create_config_file_name(&file_name_str,
                             buf,
                             config_dir,
                             &ic_config_string,
                             config_version);
  /* We are writing a new file here */
  file_ptr= g_creat((const gchar *)buf, S_IWUSR | S_IRUSR);
  if (file_ptr == (int)-1)
  {
    error= errno;
    goto file_error;
  }

  if (!(dyn_array= ic_create_simple_dynamic_array()))
    return IC_ERROR_MEM_ALLOC;
  da_ops= &dyn_array->da_ops;

  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
  /* Write [cluster]<CR> into the buffer */
  if ((error= write_new_section_header(dyn_array, da_ops, buf,
                                       cluster_str, first_call)))
    goto error;
  first_call= FALSE;
  /* Write cluster_name: __name__<CR> into the buffer */
    if (da_ops->ic_insert_dynamic_array(dyn_array, cluster_name_str,
                                        (guint32)strlen(cluster_name_str)))
      goto error;
    buf[0]= ':';
    buf[1]= ' ';
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)2))
      goto error;
    if (da_ops->ic_insert_dynamic_array(dyn_array,
                                        clu_info->cluster_name.str,
                                        clu_info->cluster_name.len))
      goto error;
    buf[0]= CARRIAGE_RETURN;
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
      goto error;

  /* Write cluster_id: __id__<CR> into the buffer */
    if (da_ops->ic_insert_dynamic_array(dyn_array, cluster_id_string,
                                        (guint32)strlen(cluster_id_string)))
      goto error;
    buf[0]= ':';
    buf[1]= ' ';
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)2))
      goto error;
    
    if (!ic_guint64_str((guint64)clu_info->cluster_id, buf, NULL))
      goto error;
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, strlen(buf)))
      goto error;
    buf[0]= CARRIAGE_RETURN;
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
      goto error;

  /* Write password: __password__<CR> into the buffer */
    if (da_ops->ic_insert_dynamic_array(dyn_array,
                                        cluster_password_str,
                                        (guint32)strlen(cluster_password_str)))
      goto error;
    buf[0]= ':';
    buf[1]= ' ';
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)2))
      goto error;
    if (da_ops->ic_insert_dynamic_array(dyn_array,
                                        clu_info->password.str,
                                        clu_info->password.len))
      goto error;
    buf[0]= CARRIAGE_RETURN;
    if (da_ops->ic_insert_dynamic_array(dyn_array, buf, (guint32)1))
      goto error;
  }
  if ((error= da_ops->ic_write_dynamic_array_to_disk(dyn_array,
                                                     file_ptr)))
    goto error;
  error= 0;
error:
  close(file_ptr);
  da_ops->ic_free_dynamic_array(dyn_array);
file_error:
  return error;
}

static int
check_str_value(gchar **val_str, gchar *str_ptr, gboolean *first)
{
  if (*first)
  {
    *val_str= str_ptr;
    *first= FALSE;
  }
  else
  {
    if (strcmp(str_ptr, *val_str) != 0)
      return 1;
  }
  return 0;
}

static int
check_if_all_same_value_charptr(IC_CLUSTER_CONFIG *clu_conf,
                                IC_CONFIG_TYPES section_type,
                                guint32 offset,
                                gchar **val_str)
{
  gchar **str_ptr;
  gboolean first= TRUE;
  guint32 max_node_id;
  guint32 i, num_comms;

  if (section_type != IC_COMM_TYPE)
  {
    max_node_id= clu_conf->max_node_id;
    for (i= 1; i <= max_node_id; i++)
    {
      if (clu_conf->node_types[i] == (IC_NODE_TYPES)section_type)
      {
        g_assert(clu_conf->node_config[i]);
        if (clu_conf->node_config[i] == NULL)
          return 2;
        str_ptr= (gchar**)(clu_conf->node_config[i] + offset);
        if (check_str_value(val_str, *str_ptr, &first))
          return 1;
      }
    }
  }
  else
  {
    num_comms= clu_conf->num_comms;
    for (i= 0; i < num_comms; i++)
    {
      if (clu_conf->comm_config[i] == NULL)
        continue;
      str_ptr= (gchar**)(clu_conf->comm_config[i] + offset);
      if (check_str_value(val_str, *str_ptr, &first))
        return 1;
    }
  }
  return 0;
}

static int
check_int_value(guint64 data, guint64 *value, gboolean *first)
{
  if (*first)
  {
    *value= data;
    first= FALSE;
  }
  else
  {
    if (data != (*value))
      return 1;
  }
  return 0;
}

static int
check_if_all_same_value_int(IC_CLUSTER_CONFIG *clu_conf,
                            IC_CONFIG_TYPES section_type,
                            guint32 offset,
                            guint64 *value,
                            IC_CONFIG_DATA_TYPE data_type)
{
  guint64 data;
  gboolean first= TRUE;
  guint32 max_node_id;
  guint32 i, num_comms;

  if (section_type != IC_COMM_TYPE)
  {
    max_node_id= clu_conf->max_node_id;
    for (i= 1; i <= max_node_id; i++)
    {
      if (clu_conf->node_types[i] == (IC_NODE_TYPES)section_type)
      {
        g_assert(clu_conf->node_config[i]);
        if (clu_conf->node_config[i] == NULL)
          return 2;
        data= get_node_data(clu_conf->node_config[i], offset, data_type);
        if (check_int_value(data, value, &first))
          return 1;
      }
    }
  }
  else
  {
    num_comms= clu_conf->num_comms;
    for (i= 0; i < num_comms; i++)
    {
      g_assert(clu_conf->comm_config[i]);
      if (clu_conf->comm_config[i] == NULL)
        return 2;
      data= get_node_data(clu_conf->comm_config[i], offset, data_type);
      if (check_int_value(data, value, &first))
        return 1;
    }
  }
  return 0;
}

static int
write_node_section(IC_DYNAMIC_ARRAY *dyn_array,
                   IC_DYNAMIC_ARRAY_OPS *da_ops,
                   IC_NODE_TYPES node_type,
                   gchar *buf,
                   gchar *struct_ptr,
                   gchar *default_struct_ptr)
{
  guint32 i, inx, offset;
  IC_CONFIG_ENTRY *conf_entry;
  IC_CONFIG_TYPES section_type= (IC_CONFIG_TYPES)node_type;
  guint64 value= 0, default_value= 0;
  IC_STRING *entry_name;
  IC_CONFIG_DATA_TYPE data_type;
  gchar *str_ptr;
  int error;

  for (i= 0; i < glob_max_config_id; i++)
  {
    if ((inx= map_inx_to_config_id[i]))
    {
      conf_entry= &glob_conf_entry[i];
      if (!(conf_entry->config_types & (1 << section_type)))
        continue;
      /* We found a configuration item of this section type, handle it */
      data_type= conf_entry->data_type;
      offset= conf_entry->offset;
      if (data_type != IC_CHARPTR)
      {
        value= get_node_data(struct_ptr, offset, data_type);
        default_value= get_node_data(default_struct_ptr, offset, data_type);
      }
      entry_name= &conf_entry->config_entry_name;
      /* Check if non-mandatory config variable is different from default */
      if (data_type != IC_CHARPTR)
      {
        if (!conf_entry->is_mandatory && value == default_value)
          continue;
        /* We need to write a configuration variable line */
        if (!(error= write_line_with_int_value(dyn_array, da_ops, buf,
                                               entry_name, value)))
          continue;
      }
      else
      {
        if (!conf_entry->is_mandatory &&
            (strcmp(struct_ptr + offset, default_struct_ptr + offset) == 0))
          continue;
        str_ptr= *(gchar**)(struct_ptr + offset);
        /* We need to write a configuration variable line */
        if (!(error= write_line_with_char_value(dyn_array, da_ops, buf,
                                                entry_name,
                                                str_ptr)))
          continue;
      }
      return error;
    }
  }
  return 0;
}

static int
write_default_section(IC_DYNAMIC_ARRAY *dyn_array,
                      IC_DYNAMIC_ARRAY_OPS *da_ops,
                      gchar *buf,
                      IC_CLUSTER_CONFIG *clu_conf,
                      IC_CLUSTER_CONFIG_LOAD *clu_def,
                      IC_CONFIG_TYPES section_type)
{
  gchar *default_struct_ptr;
  gchar *data_ptr;
  guint32 i, inx;
  guint64 value= 0;
  gchar *val_str= NULL;
  IC_CONFIG_ENTRY *conf_entry;
  int error;

  switch (section_type)
  {
    case IC_DATA_SERVER_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_data_server_config;
      break;
    case IC_CLIENT_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_client_config;
      break;
    case IC_CLUSTER_SERVER_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_cluster_server_config;
      break;
    case IC_SQL_SERVER_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_sql_server_config;
      break;
    case IC_REP_SERVER_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_rep_server_config;
      break;
    case IC_FILE_SERVER_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_file_server_config;
      break;
    case IC_RESTORE_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_restore_config;
      break;
    case IC_CLUSTER_MGR_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_cluster_mgr_config;
      break;
    case IC_COMM_TYPE:
      default_struct_ptr= (gchar*)&clu_def->default_socket_config;
      break;
    default:
      return 1;
  }
  for (i= 0; i < glob_max_config_id; i++)
  {
    if ((inx= map_inx_to_config_id[i]))
    {
      conf_entry= &glob_conf_entry[i];
      if ((!(conf_entry->config_types & (1 << section_type))) ||
          conf_entry->is_mandatory ||
          conf_entry->is_derived_default)
        continue;
      /* We found a configuration item of this section type, handle it */
      data_ptr= default_struct_ptr + conf_entry->offset;
      if (conf_entry->data_type != IC_CHARPTR)
      {
        if ((error= check_if_all_same_value_int(clu_conf, section_type,
                                                conf_entry->offset,
                                                &value,
                                                conf_entry->data_type)))
        {
          if (error == 2)
            return 1;
          /*
            There was differing values in the nodes and thus we need to
            the standard default value.
          */
          value= conf_entry->default_value;
        }
      }
      else
      {
        if ((error= check_if_all_same_value_charptr(clu_conf, section_type,
                                                    conf_entry->offset,
                                                    &val_str)))
        {
          if (error == 2)
            return 1;
          val_str= conf_entry->default_string;
        }
      }
      switch (conf_entry->data_type)
      {
        case IC_CHAR:
        case IC_BOOLEAN:
          *(guint8*)data_ptr= (guint8)value;
          break;
        case IC_UINT16:
          *(guint16*)data_ptr= (guint16)value;
          break;
        case IC_UINT32:
          *(guint32*)data_ptr= (guint32)value;
          break;
        case IC_UINT64:
          *(guint64*)data_ptr= value;
          break;
        case IC_CHARPTR:
          *(gchar**)data_ptr= val_str;
          break;
        default:
          g_assert(FALSE);
          return 1;
      }
      if (conf_entry->data_type != IC_CHARPTR)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
                                              &conf_entry->config_entry_name,
                                              value)))
          return error;
      }
      else
      {
        if ((error= write_line_with_char_value(dyn_array, da_ops, buf,
                                               &conf_entry->config_entry_name,
                                               val_str)))
          return error;
      }
    }
  }
  return 0;
}

int
write_default_sections(IC_DYNAMIC_ARRAY *dyn_array,
                       IC_DYNAMIC_ARRAY_OPS *da_ops,
                       gchar *buf,
                       IC_CLUSTER_CONFIG *clu_conf,
                       IC_CLUSTER_CONFIG_LOAD *clu_def)
{
  guint32 i;
  int error= 0;
  gboolean first_call= TRUE;
  gboolean any_node_of_type[IC_NUMBER_OF_CONFIG_TYPES];
  const gchar *node_type_str[IC_NUMBER_OF_CONFIG_TYPES];

  for (i= 0; i < IC_NUMBER_OF_CONFIG_TYPES; i++)
    any_node_of_type[i]= FALSE;
  node_type_str[IC_NOT_EXIST_NODE_TYPE]= NULL;
  node_type_str[IC_DATA_SERVER_TYPE]= data_server_def_str;
  node_type_str[IC_CLIENT_TYPE]= client_node_def_str;
  node_type_str[IC_CLUSTER_SERVER_TYPE]= cluster_server_def_str;
  node_type_str[IC_SQL_SERVER_TYPE]= sql_server_def_str;
  node_type_str[IC_REP_SERVER_TYPE]= rep_server_def_str;
  node_type_str[IC_FILE_SERVER_TYPE]= file_server_def_str;
  node_type_str[IC_RESTORE_TYPE]= restore_node_def_str;
  node_type_str[IC_CLUSTER_MGR_TYPE]= cluster_mgr_def_str;
  node_type_str[IC_COMM_TYPE]= socket_def_str;
  for (i= 1; i <= clu_conf->max_node_id; i++)
    any_node_of_type[clu_conf->node_types[i]]= TRUE;
  any_node_of_type[IC_NOT_EXIST_NODE_TYPE]= FALSE;
  for (i= 0; i < IC_NUMBER_OF_CONFIG_TYPES; i++)
  {
    /*
      First step is to write all the default sections and also to
      record all the defaults. We need to record all defaults to avoid issues
      if we at some point decide to change the default value of a certain
      configuration parameter in a later release of the iClaustron software.

      It isn't necessary to write defaults for a node type not existing in
      this cluster.
    */
    if (any_node_of_type[i])
    {
      /* Write e.g. [data server default] and its defaults into the buffer */
      if ((error= write_new_section_header(dyn_array, da_ops, buf,
                                           node_type_str[i], first_call)))
        goto error;
      first_call= FALSE;
      if ((error= write_default_section(dyn_array, da_ops, buf, clu_conf,
                                        clu_def, (IC_CONFIG_TYPES)i)))
        goto error;
    }
  }
error:
  return error;
}

static int
write_node_sections(IC_DYNAMIC_ARRAY *dyn_array,
                    IC_DYNAMIC_ARRAY_OPS *da_ops,
                    gchar *buf,
                    IC_CLUSTER_CONFIG *clu_conf,
                    IC_CLUSTER_CONFIG_LOAD *clu_def)
{
  IC_NODE_TYPES node_type;
  guint32 i;
  gchar *struct_ptr, *default_struct_ptr;
  const gchar *sect_name;
  int error= 0;

  /*
    Now it is time to write all node specific configuration items.
    This is performed node by node, only values that differs from
    default values are recorded in the node specific section.
  */
  for (i= 1; i <= clu_conf->max_node_id; i++)
  {
    struct_ptr= clu_conf->node_config[i];
    if (!struct_ptr)
      continue;
    node_type= clu_conf->node_types[i];
    switch (node_type)
    {
      case IC_DATA_SERVER_NODE:
        sect_name= data_server_str;
        default_struct_ptr= (gchar*)&clu_def->default_data_server_config;
        break;
      case IC_CLIENT_NODE:
        sect_name= client_node_str;
        default_struct_ptr= (gchar*)&clu_def->default_client_config;
        break;
      case IC_CLUSTER_SERVER_NODE:
        sect_name= cluster_server_str;
        default_struct_ptr= (gchar*)&clu_def->default_cluster_server_config;
        break;
      case IC_SQL_SERVER_NODE:
        sect_name= sql_server_str;
        default_struct_ptr= (gchar*)&clu_def->default_sql_server_config;
        break;
      case IC_REP_SERVER_NODE:
        sect_name= rep_server_str;
        default_struct_ptr= (gchar*)&clu_def->default_rep_server_config;
        break;
      case IC_FILE_SERVER_NODE:
        sect_name= file_server_str;
        default_struct_ptr= (gchar*)&clu_def->default_file_server_config;
        break;
      case IC_RESTORE_NODE:
        sect_name= restore_node_str;
        default_struct_ptr= (gchar*)&clu_def->default_restore_config;
        break;
      case IC_CLUSTER_MGR_NODE:
        sect_name= cluster_mgr_str;
        default_struct_ptr= (gchar*)&clu_def->default_cluster_mgr_config;
        break;
      default:
        g_assert(FALSE);
        return 1;
    }
    if ((error= write_new_section_header(dyn_array, da_ops,
                                         buf, sect_name, FALSE)))
      goto error;
    if ((error= write_node_section(dyn_array, da_ops, node_type, buf,
                                   struct_ptr, default_struct_ptr)))
      goto error;
  }
error:
  return error;
}

static int
write_comm_sections(IC_DYNAMIC_ARRAY *dyn_array,
                    IC_DYNAMIC_ARRAY_OPS *da_ops,
                    gchar *buf,
                    IC_CLUSTER_CONFIG *clu_conf,
                    IC_CLUSTER_CONFIG_LOAD *clu_def)
{
  guint32 i;
  guint32 node1_id, node2_id, node1_port, node2_port;
  gchar *first_hostname, *second_hostname;
  IC_SOCKET_LINK_CONFIG *comm_ptr, *default_comm_ptr;
  IC_DATA_SERVER_CONFIG *node1_ptr, *node2_ptr;
  int error= 0;

  for (i= 0; i < clu_conf->num_comms; i++)
  {
    /*
      To speed up execution a bit we check specific config items rather than
      go through all configuration items for each communication section.
      This is an optimisation that means that we have to add code here every
      time we add or remove a configuration parameter for communication
      sections.
    */
    default_comm_ptr= &clu_def->default_socket_config;
    comm_ptr= (IC_SOCKET_LINK_CONFIG*)clu_conf->comm_config[i];
    node1_id= comm_ptr->first_node_id;
    node2_id= comm_ptr->second_node_id;
    node1_ptr= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[node1_id];
    node2_ptr= (IC_DATA_SERVER_CONFIG*)clu_conf->node_config[node2_id];
    node1_port= node1_ptr->port_number;
    node2_port= node2_ptr->port_number;
    first_hostname= node1_ptr->hostname;
    second_hostname= node2_ptr->hostname;
    if ((strcmp(first_hostname, comm_ptr->first_hostname) != 0) ||
        (strcmp(second_hostname, comm_ptr->second_hostname) != 0) ||
        ((node1_id == comm_ptr->server_node_id) ?
         ((node1_port != comm_ptr->server_port_number) ||
          (node2_port != comm_ptr->client_port_number)) :
         ((node1_port != comm_ptr->client_port_number) ||
          (node2_port != comm_ptr->server_port_number))) ||
        (default_comm_ptr->socket_write_buffer_size !=
         comm_ptr->socket_write_buffer_size) ||
        (default_comm_ptr->socket_read_buffer_size !=
         comm_ptr->socket_read_buffer_size) ||
        (default_comm_ptr->socket_kernel_read_buffer_size !=
         comm_ptr->socket_kernel_read_buffer_size) ||
        (default_comm_ptr->socket_kernel_write_buffer_size !=
         comm_ptr->socket_kernel_write_buffer_size) ||
        (default_comm_ptr->socket_max_wait_in_nanos !=
         comm_ptr->socket_max_wait_in_nanos) ||
        (default_comm_ptr->socket_maxseg_size !=
         comm_ptr->socket_maxseg_size) ||
        (default_comm_ptr->use_message_id !=
         comm_ptr->use_message_id) ||
        (default_comm_ptr->use_checksum !=
         comm_ptr->use_checksum) ||
        (default_comm_ptr->socket_bind_address != 
         comm_ptr->socket_bind_address))
    {
      /* This section isn't a default section so we need to fill it in */
      if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_FIRST_NODE_ID].config_entry_name,
           (guint64)node1_id)))
        goto error;
      if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_SECOND_NODE_ID].config_entry_name,
           (guint64)node2_id)))
        goto error;
      if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_SERVER_NODE_ID].config_entry_name,
           (guint64)comm_ptr->server_node_id)))
        goto error;
      if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_CLIENT_PORT_NUMBER].config_entry_name,
           (guint64)comm_ptr->client_port_number)))
        goto error;
      if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_SERVER_PORT_NUMBER].config_entry_name,
           (guint64)comm_ptr->server_port_number)))
        goto error;
      if (strcmp(first_hostname, comm_ptr->first_hostname) != 0)
      {
        if ((error= write_line_with_char_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_FIRST_HOSTNAME].config_entry_name,
           comm_ptr->first_hostname)))
          goto error;
      }
      if (strcmp(second_hostname, comm_ptr->second_hostname) != 0)
      {
        if ((error= write_line_with_char_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_SECOND_HOSTNAME].config_entry_name,
           comm_ptr->second_hostname)))
          goto error;
      }
      if (comm_ptr->socket_write_buffer_size !=
          default_comm_ptr->socket_write_buffer_size)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_WRITE_BUFFER_SIZE].config_entry_name,
           comm_ptr->socket_write_buffer_size)))
          goto error;
      }
      if (comm_ptr->socket_read_buffer_size !=
          default_comm_ptr->socket_read_buffer_size)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_READ_BUFFER_SIZE].config_entry_name,
           comm_ptr->socket_read_buffer_size)))
          goto error;
      }
      if (comm_ptr->socket_kernel_write_buffer_size !=
          default_comm_ptr->socket_kernel_write_buffer_size)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_KERNEL_WRITE_BUFFER_SIZE].config_entry_name,
           comm_ptr->socket_kernel_write_buffer_size)))
          goto error;
      }
      if (comm_ptr->socket_kernel_read_buffer_size !=
          default_comm_ptr->socket_kernel_read_buffer_size)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
          &glob_conf_entry[SOCKET_KERNEL_READ_BUFFER_SIZE].config_entry_name,
          comm_ptr->socket_kernel_read_buffer_size)))
          goto error;
      }
      if (comm_ptr->socket_max_wait_in_nanos !=
          default_comm_ptr->socket_max_wait_in_nanos)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_MAXSEG_SIZE].config_entry_name,
           comm_ptr->socket_max_wait_in_nanos)))
          goto error;
      }
      if (comm_ptr->socket_maxseg_size !=
          default_comm_ptr->socket_maxseg_size)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_MAXSEG_SIZE].config_entry_name,
           comm_ptr->socket_maxseg_size)))
          goto error;
      }
      if (comm_ptr->use_message_id !=
          default_comm_ptr->use_message_id)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_USE_MESSAGE_ID].config_entry_name,
           comm_ptr->use_message_id)))
          goto error;
      }
      if (comm_ptr->use_checksum !=
          default_comm_ptr->use_checksum)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_USE_CHECKSUM].config_entry_name,
           comm_ptr->use_checksum)))
          goto error;
      }
      if (comm_ptr->socket_bind_address !=
          default_comm_ptr->socket_bind_address)
      {
        if ((error= write_line_with_int_value(dyn_array, da_ops, buf,
           &glob_conf_entry[SOCKET_BIND_ADDRESS].config_entry_name,
           comm_ptr->socket_bind_address)))
          goto error;
      }
    }
  }
error:
  return error;
}

static int
write_one_cluster_config_file(IC_STRING *config_dir,
                              IC_CLUSTER_CONFIG *clu_conf,
                              guint32 config_version)
{
  IC_DYNAMIC_ARRAY *dyn_array;
  IC_DYNAMIC_ARRAY_OPS *da_ops;
  IC_STRING file_name_str;
  int error= IC_ERROR_MEM_ALLOC;
  int file_ptr;
  IC_CLUSTER_CONFIG_LOAD clu_def;
  gchar buf[IC_MAX_FILE_NAME_SIZE];

  memset(&clu_def, 0, sizeof(IC_CLUSTER_CONFIG_LOAD));
  /*
     We start by creating the new configuration file and a dynamic array
     used to fill in the content of this new file before we write it.
  */
  ic_create_config_file_name(&file_name_str,
                             buf,
                             config_dir,
                             &clu_conf->clu_info.cluster_name,
                             config_version);
  /* We are writing a new file here */
  file_ptr= g_creat((const gchar *)buf, S_IWUSR | S_IRUSR);
  if (file_ptr == (int)-1)
  {
    error= errno;
    goto file_error;
  }

  if (!(dyn_array= ic_create_simple_dynamic_array()))
    return IC_ERROR_MEM_ALLOC;
  da_ops= &dyn_array->da_ops;

  if ((error= write_default_sections(dyn_array, da_ops, buf,
                                     clu_conf, &clu_def)))
    goto error;
 
  if ((error= write_node_sections(dyn_array, da_ops, buf,
                                  clu_conf, &clu_def)))
    goto error;

  if ((error= write_comm_sections(dyn_array, da_ops, buf,
                                  clu_conf, &clu_def)))
    goto error;

  if ((error= da_ops->ic_write_dynamic_array_to_disk(dyn_array,
                                                     file_ptr)))
    goto error;
  error= 0;
error:
  close(file_ptr);
  da_ops->ic_free_dynamic_array(dyn_array);
file_error:
  return error;
}

static int
write_config_files(IC_STRING *config_dir, IC_CLUSTER_CONNECT_INFO **clu_infos,
                   IC_CLUSTER_CONFIG **clusters, guint32 config_version)
{
  IC_CLUSTER_CONNECT_INFO *clu_info;
  IC_CLUSTER_CONFIG *clu_conf;
  int error;
  if ((error= write_cluster_config_file(config_dir,
                                        clu_infos,
                                        config_version)))
    return error;
  while (*clu_infos)
  {
    clu_info= *clu_infos;
    clu_infos++;
    clu_conf= clusters[clu_info->cluster_id];
    if ((error= write_one_cluster_config_file(config_dir,
                                              clu_conf, config_version)))
      return error;
  }
  return 0;
}

static int
cmp_config_version_line(IC_STRING *str, guint64 *len,
                        guint32 *value, gchar **ptr)
{
  guint64 loc_value;
  guint32 value_len;
  guint64 loc_len= *len;
  gchar *loc_ptr= *ptr;
  int error= 1;

  if (memcmp(loc_ptr, str->str, str->len) || loc_len < str->len)
    goto file_error;
  loc_len-= str->len;
  loc_ptr+= str->len;
  if (ic_conv_str_to_int(loc_ptr, &loc_value, &value_len))
    goto file_error;
  if ((loc_len < (value_len + 1)) ||
      (loc_value > (guint64)(((guint64)1) << 32)))
    goto file_error;
  *value= (guint32)loc_value;
  loc_ptr+= value_len;
  if (*loc_ptr != CARRIAGE_RETURN)
    goto file_error;
  loc_ptr++;
  loc_len-= (value_len + 1);
  *ptr= loc_ptr;
  *len= loc_len;
  return 0;
file_error:
  return error;
}

static int
read_config_version_file(IC_STRING *config_dir,
                         guint32 *config_version,
                         guint32 *state,
                         guint32 *pid)
{
  IC_STRING file_name_string;
  IC_STRING str;
  int error;
  guint64 len;
  gchar *ptr;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];

  ic_create_config_version_file_name(&file_name_string, file_name, config_dir);
  if ((error= ic_get_file_contents(file_name, &ptr, &len)))
  {
    if (error == ENOENT)
    {
      *config_version= 0;
      *state= 0;
      *pid= 0;
      return 0;
    }
    return error;
  }
  /*
    Read a string like
    version: <config_version><CR>
    state: <state><CR>
    pid: <pid><CR>
  */
  IC_INIT_STRING(&str, (gchar*)version_str, VERSION_REQ_LEN, TRUE);
  if ((error= cmp_config_version_line(&str, &len, config_version, &ptr)))
    goto file_error;
  IC_INIT_STRING(&str, (gchar*)state_str, STATE_STR_LEN, TRUE);
  if ((error= cmp_config_version_line(&str, &len, state, &ptr)))
    goto file_error;
  IC_INIT_STRING(&str, (gchar*)pid_str, PID_STR_LEN, TRUE);
  if ((error= cmp_config_version_line(&str, &len, pid, &ptr)))
    goto file_error;
  return 0;
file_error:
  ic_free(ptr);
  return error;
}

static void
insert_line_config_version_file(IC_STRING *str,
                                gchar **ptr,
                                guint32 value)
{
  guint32 str_len;
  guint64 long_value= (guint64)value;
  gchar *loc_ptr= *ptr;

  strcpy(loc_ptr, str->str);
  loc_ptr+= str->len;
  loc_ptr= ic_guint64_str(long_value, loc_ptr, &str_len);
  loc_ptr+= str_len;
  *loc_ptr= CARRIAGE_RETURN;
  loc_ptr++;
  *ptr= loc_ptr;
}

static int
write_cv_file(IC_STRING *config_dir,
              guint32 config_version,
              guint32 state,
              guint32 pid)
{
  int file_ptr, error;
  IC_STRING file_name_string, str;
  gchar buf[128];
  gchar *ptr= buf;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];
  DEBUG_ENTRY("write_cv_file");

  /*
    Create a string like
    version: <config_version><CR>
    state: <state><CR>
    pid: <pid><CR>

    The version is the current version of the configuration
    State is either idle if no one is currently running the
    cluster server using this configuration, it's busy when
    someone is running and isn't updating the configuration,
    finally it can be in a number of states which is used
    when the configuration is updated.
  */
  IC_INIT_STRING(&str, (gchar*)version_str, VERSION_REQ_LEN, TRUE);
  insert_line_config_version_file(&str, &ptr, config_version);
  IC_INIT_STRING(&str, (gchar*)state_str, STATE_STR_LEN, TRUE);
  insert_line_config_version_file(&str, &ptr, state);
  IC_INIT_STRING(&str, (gchar*)pid_str, PID_STR_LEN, TRUE);
  insert_line_config_version_file(&str, &ptr, pid);

  ic_create_config_version_file_name(&file_name_string, file_name, config_dir);
  if (config_version == (guint32)1)
  {
    /* This is the initial write of the file */
    file_ptr= g_creat((const gchar *)file_name, S_IRUSR | S_IWUSR);
    if (file_ptr == (int)-1)
    {
      error= errno;
      goto file_error;
    }
    close(file_ptr);
  }
  /* Open config.version for writing */
  file_ptr= g_open((const gchar *)file_name, O_WRONLY | O_SYNC, 0);
  if (file_ptr == (int)-1)
  {
    error= errno;
    goto file_error;
  }
  error= ic_write_file(file_ptr, (const gchar*)buf, (size_t)(ptr-buf));
  close(file_ptr);
  return error;

file_error:
  return error;
}

static int
write_config_version_file(IC_STRING *config_dir,
                          guint32 config_version,
                          guint32 state,
                          guint32 pid)
{
  guint32 read_pid, read_state, read_cv;
  int error;
  DEBUG_ENTRY("write_config_version_file");

  if ((error= write_cv_file(config_dir, config_version,
                            state, pid)))
    goto error;
  if ((error= read_config_version_file(config_dir,
                                       &read_cv,
                                       &read_state,
                                       &read_pid)))
    goto error;
  if ((read_cv != config_version) ||
      (read_state != state) ||
      (read_pid != pid))
  {
    /*
      We have written the config version file but after reading it,
      the values doesn't match. This means someone else have got
      there before us. In this case we need to stop this process
      and let the other process win.
    */
    error= 1;
  }
  /*
    We are now safe since our pid is written into the config version
    file and anyone trying to start using this config directory will
    have to check if our process is awake. So as long as we are alive
    we are safe.
    error == 0 in this path.
  */
error:
  return error;
}

int
ic_load_config_version(IC_STRING *config_dir,
                       gchar *process_name,
                       guint32 *config_version)
{
  guint32 state, pid;
  int error;
  gchar *err_msg;
  DEBUG_ENTRY("ic_load_config_version");

  if ((error= read_config_version_file(config_dir,
                                       config_version,
                                       &state,
                                       &pid)))
  {
    if (state == CONFIG_STATE_IDLE)
    {
      /*
        Configuration not in update process. It is also idle in the sense
        that no other process is active using this configuration. In this
        state we can trust the version number read and update the file
        such that the state is set to busy with our own process id.
      */
      return 0;
    }
    if ((error= ic_is_process_alive(pid, process_name, &err_msg)))
    {
      /*
        Another process is still running a Cluster Server on the same
        configuration files. We will double-check that the process is
        still running and that it hasn't died on us. If it's dead then
        we can change the state to CONFIG_STATE_BUSY and set our own
        pid instead of the previous owner. If the state is in an update
        state we need to clean up before changing the state.
      */
      printf("%s\n", err_msg);
      return 1;
    }
    if (state == CONFIG_STATE_BUSY)
    {
      if (ic_is_process_alive(pid, process_name, &err_msg))
      {
        printf("%s\n", err_msg);
        return 1;
      }
    }
    else if (state == CONFIG_STATE_UPDATE_CLUSTER_CONFIG)
    {
    }
  }
  return 0;
}

static int
remove_config_version_file(IC_STRING *config_dir)
{
  IC_STRING file_name_string;
  gchar buf[IC_MAX_FILE_NAME_SIZE];

  ic_create_config_version_file_name(&file_name_string, buf, config_dir);
  return ic_delete_file((const gchar*)buf);
}

int
ic_write_full_config_to_disk(IC_STRING *config_dir,
                             guint32 *old_config_version_number,
                             IC_CLUSTER_CONNECT_INFO **clu_infos,
                             IC_CLUSTER_CONFIG **clusters)
{
  int error= 0;
  guint32 old_version= *old_config_version_number;
  guint32 own_pid;
  DEBUG_ENTRY("ic_write_full_config_to_disk");
  /*
   The first step before writing anything is to ensure that the previous
   write was successfully completed. This is accomplished by removing any
   files related to the previous version which might still be left in the
   directory (this could potentially happen since writing the new config is
   a multi-step write.

   1) Write the new configuration files with the new configuration version
     This step means updating the cluster configuration file plus each
     configuration file of each cluster.
   2) Update the config.version file with the new config version number
   3) Remove the old config version files

   If a crash occurs after step 2) but before step 3) has completed the
   configuration change was still successful although it didn't finish the
   clean-up. To finish the clean-up we perform it always before updating to
   a new version since it is safe to remove config files that are no longer
   current. Thus we add a step 0:

   0) Remove the cluster configuration file and all configuration files for
      all clusters for old_config_version_number - 1. This is only applicable
      if old_config_version_number > 1. Otherwise no previous version can
      exist.

   When the old config version number is 0 it means that we're writing the
   first version and we can thus skip both step 0 and step 3, also step 2
   is a create rather than an update.

   When the old config version number is 1 we can skip step 0 but need to
   perform step 3.
  */
  own_pid= ic_get_own_pid();
  if (old_version > (guint32)1)
  {
    /* Step 0 */
    if ((error= remove_config_files(config_dir, clu_infos, old_version - 1)))
      return error;
  }
  /* Step 1 */
  if ((error= write_config_files(config_dir, clu_infos, clusters,
                                 old_version + 1)))
    goto error;
  /* Step 2 */
  if ((error= write_config_version_file(config_dir, old_version + 1, 0,
                                        own_pid)))
    goto error;
  if (old_version > 0)
  {
    /* Step 3 */
    if ((error= remove_config_files(config_dir, clu_infos, old_version)))
      return error;
  }
  return 0;
error:
  (void)remove_config_files(config_dir, clu_infos, old_version + 1);
  if (old_version == 0)
  {
    remove_config_version_file(config_dir);
  }
  return error;
}

static IC_CONFIG_OPERATIONS cluster_config_ops =
{
  .ic_config_init = cluster_config_init,
  .ic_add_section = cluster_config_add_section,
  .ic_add_key     = cluster_config_add_key,
  .ic_add_comment = cluster_config_add_comment,
  .ic_config_verify = cluster_config_verify,
  .ic_init_end    = cluster_config_init_end,
  .ic_config_end  = cluster_config_end,
};

IC_CLUSTER_CONNECT_INFO**
ic_load_cluster_config_from_file(IC_STRING *config_dir,
                                 guint32 config_version_number,
                                 IC_CONFIG_STRUCT *cluster_conf)
{
  gchar *conf_data_str;
  guint64 conf_data_len;
  IC_STRING conf_data;
  IC_STRING file_name_string;
  int ret_val;
  IC_CONFIG_ERROR err_obj;
  IC_CLUSTER_CONNECT_INFO **ret_ptr;
  gchar file_name[IC_MAX_FILE_NAME_SIZE];
  DEBUG_ENTRY("ic_load_cluster_config_from_file");

  ic_create_config_file_name(&file_name_string,
                             file_name,
                             config_dir,
                             &ic_config_string,
                             config_version_number);

  cluster_conf->clu_conf_ops= &cluster_config_ops;
  cluster_conf->config_ptr.cluster_conf= NULL;
  DEBUG_PRINT(CONFIG_LEVEL, ("cluster_config_file = %s",
                             file_name));
  if ((ret_val= ic_get_file_contents(file_name, &conf_data_str,
                                     &conf_data_len)))
    goto file_open_error;

  IC_INIT_STRING(&conf_data, conf_data_str, conf_data_len, TRUE);
  ret_val= ic_build_config_data(&conf_data, cluster_conf, &err_obj);
  ic_free(conf_data.str);
  if (ret_val == 1)
  {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
          "Error at Line number %u:\n%s\n", err_obj.line_number,
          ic_get_error_message(err_obj.err_num));
    ret_ptr= NULL;
    printf("%s Failed to load the cluster configuration file %s\n",
           ic_err_str, file_name);
  }
  else
  {
    ret_ptr= cluster_conf->config_ptr.cluster_conf->clu_info;
    cluster_config_ops.ic_init_end(cluster_conf);
  }
  DEBUG_RETURN(ret_ptr);

file_open_error:
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
        "Failed reading cluster config file %s\n", file_name);
  DEBUG_RETURN(NULL);
}

IC_API_CONFIG_SERVER*
ic_get_configuration(IC_API_CLUSTER_CONNECTION *api_cluster_conn,
                     IC_STRING *config_dir,
                     guint32 node_id,
                     gchar *cluster_server_ip,
                     gchar *cluster_server_port,
                     gboolean use_iclaustron_cluster_server,
                     int *error,
                     gchar **err_str)
{
  IC_CLUSTER_CONNECT_INFO **clu_infos;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_CONFIG_STRUCT clu_conf_struct;
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  int ret_code;
  gchar *save_err_str= *err_str;

  *err_str= NULL;
  ret_code= 0;
  if (!(mc_ptr= ic_create_memory_container(MC_DEFAULT_BASE_SIZE, 0)))
    goto end;
  clu_conf_struct.perm_mc_ptr= mc_ptr;

  if (!(clu_infos= ic_load_cluster_config_from_file(config_dir,
                                                    (guint32)0,
                                                    &clu_conf_struct)))
    goto end;

  api_cluster_conn->num_cluster_servers= 1;
  api_cluster_conn->cluster_server_ips= &cluster_server_ip;
  api_cluster_conn->cluster_server_ports= &cluster_server_port;
  if ((apic= ic_create_api_cluster(api_cluster_conn,
                                   use_iclaustron_cluster_server)))
  {
    if (!(ret_code= apic->api_op.ic_get_config(apic,
                                               clu_infos,
                                               node_id)))
      goto end;
    *err_str= save_err_str;
    *err_str= apic->api_op.ic_fill_error_buffer(apic, ret_code, *err_str);
    apic->api_op.ic_free_config(apic);
    apic= NULL;
  }
end:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  *error= ret_code;
  return apic;
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
  if (!g_thread_supported())
    g_thread_init(NULL);
  ic_init_error_messages();
  if ((ret_value= ic_init_config_parameters()))
  {
    ic_end();
    DEBUG_RETURN(ret_value);
  }
  if ((ret_value= ic_ssl_init()))
  {
    ic_end();
    DEBUG_RETURN(ret_value);
  }
  DEBUG_RETURN(0);
}

void ic_end()
{
  DEBUG_ENTRY("ic_end");
  if (glob_conf_hash)
    ic_hashtable_destroy(glob_conf_hash);
  glob_conf_entry_inited= FALSE;
  ic_ssl_end();
  DEBUG_CLOSE;
}
