/* Copyright (C) 2007, 2020 iClaustron AB

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

#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_debug.h>
#include <ic_string.h>
#include <ic_err.h>

/*
  MODULE: iClaustron Error Handling
  ---------------------------------
  Module for printing error codes and retrieving error messages
  given the error number.

  To add a new error number do the following:
  1) Change IC_LAST_ERROR in ic_err.h
  2) Add the new error code in ic_err.h
  3) Add a new entry in ic_init_error_messages
*/

static gchar* ic_error_str[IC_MAX_ERRORS];
static gchar *no_such_error_str= "No such error";

int ic_translate_error_string(gchar *error_str)
{
  guint32 i;

  for (i= 0; i < IC_MAX_ERRORS; i++)
  {
    if (ic_error_str[i] &&
        strcmp(ic_error_str[i], error_str) == 0)
    {
      return (i + IC_FIRST_ERROR);
    }
  }
  return IC_ERROR_NO_SUCH_ERROR;
}

void
ic_print_error(int error_number)
{
  gchar buf[IC_MAX_ERROR_STRING_SIZE];

  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
  {
    ic_printf("OS Error number = %d", error_number);
    if (ic_get_strerror(error_number, buf, sizeof(buf)))
    {
      ic_printf("OS Error: %s", buf);
    }
  }
  else
  {
    ic_printf("%s", ic_error_str[error_number - IC_FIRST_ERROR]);
  }
  if (ic_get_last_error())
  {
    perror("Last reported OS Error:");
  }
}

gchar *ic_get_error_message(int error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
  {
    return no_such_error_str;
  }
  else
  {
    return ic_error_str[error_number - IC_FIRST_ERROR];
  }
}

gchar*
ic_common_fill_error_buffer(const gchar *extra_error_message,
                            guint32 error_line,
                            int error_code,
                            gchar *error_buffer)
{
  guint32 err_msg_len, err_str_len, err_buf_index;
  guint32 line_err_str_len, line_buf_len;
  gchar *line_err_msg= NULL;
  gchar *protocol_err_msg= "Protocol error on line: ";
  gchar *line_number_msg= "Error on line: ";
  gchar *err_msg, *line_buf_ptr;
  gchar line_buf[128];

  if (!error_buffer)
  {
    return NULL;
  }
  if (error_code == IC_PROTOCOL_ERROR)
  {
    line_err_msg= protocol_err_msg;
  }
  else if (error_code != 0 && error_line != 0)
  {
    line_err_msg= line_number_msg;
  }
  err_msg= ic_get_error_message(error_code);
  err_msg_len= strlen(err_msg);
  memcpy(error_buffer, err_msg, err_msg_len);
  error_buffer[err_msg_len]= CARRIAGE_RETURN;
  err_buf_index= err_msg_len;
  if (extra_error_message)
  {
    err_str_len= strlen(extra_error_message);
    memcpy(&error_buffer[err_buf_index + 1], extra_error_message,
           err_str_len);
    err_buf_index= err_buf_index + 1 + err_str_len;
    error_buffer[err_buf_index]= CARRIAGE_RETURN;
  }
  if (line_err_msg)
  {
    line_err_str_len= strlen(line_err_msg);
    memcpy(&error_buffer[err_buf_index + 1], line_err_msg,
           line_err_str_len);
    err_buf_index= err_buf_index + 1 + line_err_str_len;
    line_buf_ptr= ic_guint64_str((guint64)error_line, line_buf,
                                 &line_buf_len);
    if (line_buf_ptr)
    {
      memcpy(&error_buffer[err_buf_index], line_buf_ptr,
             line_buf_len);
      err_buf_index= err_buf_index + line_buf_len;
    }
    error_buffer[err_buf_index]= CARRIAGE_RETURN;
  }
  error_buffer[err_buf_index + 1]= 0; /* Null-terminated string */
  return error_buffer;  
}

void
ic_init_error_messages()
{
  guint32 i;

  for (i= 0; i < IC_MAX_ERRORS; i++)
    ic_error_str[i]= NULL;
  ic_error_str[IC_ERROR_LINE_TOO_LONG -IC_FIRST_ERROR]=
    "Line was too long";
  ic_error_str[IC_ERROR_CONFIG_BRACKET - IC_FIRST_ERROR]=
    "Missing ] after initial [";
  ic_error_str[IC_ERROR_CONFIG_INCORRECT_GROUP_ID - IC_FIRST_ERROR]=
    "Found incorect group id";
  ic_error_str[IC_ERROR_CONFIG_IMPROPER_KEY_VALUE - IC_FIRST_ERROR]=
    "Improper key-value pair";
  ic_error_str[IC_ERROR_CONFIG_NO_SUCH_SECTION - IC_FIRST_ERROR]=
    "Section name doesn't exist in this type of configuration file";
  ic_error_str[IC_ERROR_MEM_ALLOC - IC_FIRST_ERROR]=
    "Memory allocation failure";
  ic_error_str[IC_ERROR_NO_SECTION_DEFINED_YET - IC_FIRST_ERROR]=
    "Tried to define key value before first section defined";
  ic_error_str[IC_ERROR_NO_SUCH_CONFIG_KEY - IC_FIRST_ERROR]=
    "No such configuration key exists";
  ic_error_str[IC_ERROR_DEFAULT_VALUE_FOR_MANDATORY - IC_FIRST_ERROR]=
    "Trying to assign default value to a mandatory config entry";
  ic_error_str[IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION - IC_FIRST_ERROR]=
    "Assigning correct config entry in wrong section";
  ic_error_str[IC_ERROR_NO_NODES_FOUND - IC_FIRST_ERROR]=
    "No nodes found in the configuration file";
  ic_error_str[IC_ERROR_WRONG_CONFIG_NUMBER - IC_FIRST_ERROR]=
    "Number expected in config file, true, false and endings with k, m, g also allowed";
  ic_error_str[IC_ERROR_NO_BOOLEAN_VALUE - IC_FIRST_ERROR]=
    "Boolean value expected, got number larger than 1";
  ic_error_str[IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS - IC_FIRST_ERROR]=
    "Configuration value is out of bounds, check data type and min, max values";
  ic_error_str[IC_ERROR_NO_SERVER_NAME - IC_FIRST_ERROR]=
    "Server name must be provided in all connections";
  ic_error_str[IC_ERROR_NO_SERVER_PORT - IC_FIRST_ERROR]=
    "Server port must be provided in all connections";
  ic_error_str[IC_ERROR_GETADDRINFO - IC_FIRST_ERROR]=
    "Provided client/server name/port not found by getaddrinfo";
  ic_error_str[IC_ERROR_ILLEGAL_SERVER_PORT - IC_FIRST_ERROR]=
    "Provided server port isn't a legal port number";
  ic_error_str[IC_ERROR_DIFFERENT_IP_VERSIONS - IC_FIRST_ERROR]=
    "Trying to use IPv4 and IPv6 simultaneously on server/client part not supported";
  ic_error_str[IC_ERROR_ILLEGAL_CLIENT_PORT - IC_FIRST_ERROR]=
    "Provided client port isn't a legal port number";
  ic_error_str[IC_ERROR_INCONSISTENT_DATA - IC_FIRST_ERROR]=
    "Internal data structure error";
  ic_error_str[IC_ERROR_NODE_DOWN - IC_FIRST_ERROR]=
    "Node failure occurred";
  ic_error_str[IC_ERROR_NO_SUCH_CLUSTER - IC_FIRST_ERROR]=
    "No such cluster";
  ic_error_str[IC_ERROR_NO_SUCH_NODE - IC_FIRST_ERROR]=
    "No such node exists in this cluster";
  ic_error_str[IC_ERROR_MESSAGE_CHECKSUM - IC_FIRST_ERROR]=
    "Message received with wrong checksum";
  ic_error_str[IC_ERROR_ACCEPT_TIMEOUT - IC_FIRST_ERROR]=
    "Timeout when waiting for connection to accept";
  ic_error_str[IC_ERROR_POLL_SET_FULL - IC_FIRST_ERROR]=
    "Poll set is full, need to use another poll set";
  ic_error_str[IC_ERROR_NOT_FOUND_IN_POLL_SET - IC_FIRST_ERROR]=
    "The file descriptor wasn't found in this poll set";
  ic_error_str[IC_ERROR_NODE_ALREADY_DEFINED - IC_FIRST_ERROR]=
    "Can't use the same node id twice in a cluster";
  ic_error_str[IC_ERROR_PROCESS_NOT_ALIVE - IC_FIRST_ERROR]=
    "The process is not alive";
  ic_error_str[IC_ERROR_COULD_NOT_LOCK_CONFIGURATION - IC_FIRST_ERROR]=
    "The Cluster Server failed to lock the configuration, other Cluster Server already running";
  ic_error_str[IC_ERROR_CHECK_PROCESS_SCRIPT - IC_FIRST_ERROR]=
    "The check process script failed";
  ic_error_str[IC_ERROR_BOOTSTRAP_ALREADY_PERFORMED - IC_FIRST_ERROR]=
    "Bootstrap on Cluster Server already performed";
  ic_error_str[IC_ERROR_CONFLICTING_CLUSTER_IDS - IC_FIRST_ERROR]=
    "Cluster ids must be unique in configuration";
  ic_error_str[IC_ERROR_FAILED_TO_OPEN_FILE - IC_FIRST_ERROR]=
    "Failed to open file";
  ic_error_str[IC_ERROR_BOOTSTRAP_NEEDED - IC_FIRST_ERROR]=
    "Starting Cluster Server for the first time without bootstrap flag set";
  ic_error_str[IC_ERROR_CONNECT_THREAD_STOPPED - IC_FIRST_ERROR]=
    "Connect thread stopped";
  ic_error_str[IC_ERROR_THREADPOOL_FULL - IC_FIRST_ERROR]=
    "Threadpool is full";
  ic_error_str[IC_ERROR_START_THREAD_FAILED - IC_FIRST_ERROR]=
    "Start thread failed";
  ic_error_str[IC_ERROR_RECEIVE_TIMEOUT - IC_FIRST_ERROR]=
    "Timeout in receiving line in NDB Management Protocol";
  ic_error_str[IC_ERROR_CONNECT_TIMEOUT - IC_FIRST_ERROR]=
    "Timeout when waiting for connect on client side";
  ic_error_str[IC_ERROR_STOP_ORDERED - IC_FIRST_ERROR]=
    "Stop ordered in send thread";
  ic_error_str[IC_ACCEPT_ERROR - IC_FIRST_ERROR]=
    "Accept error on socket";
  ic_error_str[IC_END_OF_FILE - IC_FIRST_ERROR]=
    "Unexpected end of file";
  ic_error_str[IC_PROTOCOL_ERROR - IC_FIRST_ERROR]=
    "Protocol error in NDB MGM Protocol";
  ic_error_str[IC_AUTHENTICATE_ERROR - IC_FIRST_ERROR]=
    "Authentication error at connection setup";
  ic_error_str[IC_SSL_ERROR - IC_FIRST_ERROR]=
    "SSL error on socket";
  ic_error_str[IC_ERROR_PC_START_ALREADY_ONGOING - IC_FIRST_ERROR]=
    "An attempt to start this process is already ongoing";
  ic_error_str[IC_ERROR_PC_PROCESS_ALREADY_RUNNING - IC_FIRST_ERROR]=
    "This process is already running";
  ic_error_str[IC_ERROR_PROCESS_STUCK_IN_START_PHASE - IC_FIRST_ERROR]=
    "Process is stuck in start phase";
  ic_error_str[IC_ERROR_FAILED_TO_STOP_PROCESS - IC_FIRST_ERROR]=
    "Failed to stop/kill process";
  ic_error_str[IC_ERROR_PROCESS_STUCK_IN_START_PHASE - IC_FIRST_ERROR]=
    "Failed to stop/kill process, process stuck in start phase";
  ic_error_str[IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_PARAM - IC_FIRST_ERROR]=
    "Set/Get connection parameter only supports set/get Server Port Parameter";
  ic_error_str[IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_NODES - IC_FIRST_ERROR]=
    "Set/Get connection parameter only supports client node as server side";
  ic_error_str[IC_ERROR_INDEX_ZERO_NOT_ALLOWED - IC_FIRST_ERROR]=
    "Index 0 isn't allowed in dynamic translations";
  ic_error_str[IC_ERROR_PTR_ARRAY_INDEX_OUT_OF_BOUND - IC_FIRST_ERROR]=
    "Index out of bound in dynamic pointer array";
  ic_error_str[IC_ERROR_PTR_ARRAY_INDEX_ERROR - IC_FIRST_ERROR]=
    "Trying to read non-existent entry in dynamic pointer array";
  ic_error_str[IC_ERROR_PARSE_CONNECTSTRING - IC_FIRST_ERROR]=
    "Error when parsing connect string";
  ic_error_str[IC_ERROR_TOO_MANY_CS_HOSTS - IC_FIRST_ERROR]=
    "Too many hosts in connectstring";
  ic_error_str[IC_ERROR_FAILED_TO_DAEMONIZE - IC_FIRST_ERROR]=
    "Failed to daemonize process";
  ic_error_str[IC_ERROR_APPLICATION_STOPPED - IC_FIRST_ERROR]=
    "Application has been stopped";
  ic_error_str[IC_ERROR_GET_CONFIG_BY_CLUSTER_SERVER - IC_FIRST_ERROR]=
    "Trying to get configuration from network using cluster server";
  ic_error_str[IC_ERROR_BUFFER_MISSING_CREATE_APID_OP - IC_FIRST_ERROR]=
    "Need to have a valid buffers when creating APID operation object";
  ic_error_str[IC_ERROR_TOO_MANY_FIELDS - IC_FIRST_ERROR]=
    "Defining more fields than table contains isn't valid";
  ic_error_str[IC_ERROR_DUPLICATE_FIELD_IDS - IC_FIRST_ERROR]=
    "Trying to define the same field twice";
  ic_error_str[IC_ERROR_FIELD_ALREADY_DEFINED - IC_FIRST_ERROR]=
    "Trying to define the same field twice";
  ic_error_str[IC_ERROR_FIELD_NOT_DEFINED - IC_FIRST_ERROR]=
    "Trying to define a characteristic on a field not defined";
  ic_error_str[IC_ERROR_NOT_A_CHARSET_FIELD - IC_FIRST_ERROR]=
    "Trying to define character set on a field not using it";
  ic_error_str[IC_ERROR_NOT_A_DECIMAL_FIELD - IC_FIRST_ERROR]=
    "Trying to define decimal characteristics on a field not decimal";
  ic_error_str[IC_ERROR_NOT_A_SIGNABLE_FIELD - IC_FIRST_ERROR]=
    "Trying to define signed or not on a field not a number field";
  ic_error_str[IC_ERROR_INDEX_ALREADY_DEFINED - IC_FIRST_ERROR]=
    "Trying to define the same index twice";
  ic_error_str[IC_ERROR_INDEX_NOT_DEFINED - IC_FIRST_ERROR]=
    "Trying to operate on a non-existent index";
  ic_error_str[IC_ERROR_MULTIPLE_METADATA_OPS - IC_FIRST_ERROR]=
"Currently only supported with one metadata table/tablespace per transaction";
  ic_error_str[IC_ERROR_CREATE_TABLE_NO_FIELDS - IC_FIRST_ERROR]=
    "Trying to create a table without fields";
  ic_error_str[IC_ERROR_MD_COMMIT_NO_OPERATION - IC_FIRST_ERROR]=
    "Trying to commit metadata transaction without any operations";
  ic_error_str[IC_ERROR_ILLEGAL_MD_OPERATION_DROP_RENAME - IC_FIRST_ERROR]=
    "Trying to perform operation not allowed with drop/rename table";
  ic_error_str[IC_ERROR_ILLEGAL_MD_OPERATION_CREATE - IC_FIRST_ERROR]=
    "Trying to perform operation not allowed with create table";
  ic_error_str[IC_ERROR_TABLE_MUST_HAVE_PRIMARY_KEY - IC_FIRST_ERROR]=
    "Tables defined in NDB must have a primary key";
  ic_error_str[IC_ERROR_RECORD_SIZE_TOO_BIG - IC_FIRST_ERROR]=
    "Record size is limited to 8052 bytes currently";
  ic_error_str[IC_ERROR_SET_CONNECTION_NO_DYNAMIC - IC_FIRST_ERROR]=
    "Trying set dynamic port number when fixed port number is used";
  ic_error_str[IC_ERROR_NO_SUCH_ERROR - IC_FIRST_ERROR]=
    "No such error";
  ic_error_str[IC_ERROR_MASTER_INDEX_VIEW_DIFFERS - IC_FIRST_ERROR]=
    "Cluster Servers have different views on master index";
  ic_error_str[IC_ERROR_WRONG_NODE_ID - IC_FIRST_ERROR]=
    "Wrong node id";
  ic_error_str[IC_ERROR_CHANGE_VIEW - IC_FIRST_ERROR]=
    "Cluster Server changed view on master node order";
  ic_error_str[IC_ERROR_NO_NODEID - IC_FIRST_ERROR]=
    "Node id needs to be provided for Cluster Server";
  ic_error_str[IC_ERROR_NO_SUCH_CLUSTER_SERVER_NODEID - IC_FIRST_ERROR]=
    "Node id provided for Cluster Server didn't exist in config";
  ic_error_str[IC_ERROR_FAILED_TO_OPEN_COMMON_GRID_CONFIG -
               IC_FIRST_ERROR]=
    "Failed to open common grid configuration file grid_common.ini";
  ic_error_str[IC_ERROR_FAILED_TO_OPEN_CLUSTER_CONFIG - IC_FIRST_ERROR]=
    "Failed to open configuration file of a cluster";
  ic_error_str[IC_ERROR_FAILED_TO_OPEN_CLUSTER_LIST - IC_FIRST_ERROR]=
    "Failed to open configuration file of clusters in the grid, config.ini";
  ic_error_str[IC_ERROR_FAILED_TO_CREATE_CONFIG_VERSION - IC_FIRST_ERROR]=
    "Failed to create config_version.ini, need to create "
    "iclaustron_data/config/nodeX directory";
  ic_error_str[IC_ERROR_TWO_NODES_USING_SAME_HOST_PORT_PAIR - IC_FIRST_ERROR]=
    "Configuration has two nodes using the same host + port pair";
  ic_error_str[IC_ERROR_CS_STARTED_WITH_WRONG_NODEID - IC_FIRST_ERROR]=
    "The Cluster Server was started with the wrong nodeid";
  ic_error_str[IC_ERROR_COMMAND_TOO_LONG - IC_FIRST_ERROR]=
    "Command was too long";
  ic_error_str[IC_ERROR_NOT_USED_1 - IC_FIRST_ERROR]=
    "";
  ic_error_str[IC_ERROR_NO_FINAL_COMMAND - IC_FIRST_ERROR]=
    "Last command in command file must end with ;";
  ic_error_str[IC_ERROR_INCONSISTENT_CONTENT_IN_CONFIG_VERSION_FILE -
               IC_FIRST_ERROR]=
    "Inconsistent content in config_version.ini file";
  ic_error_str[IC_ERROR_SYNTAX_ERROR_IN_CONFIG_VERSION_FILE - IC_FIRST_ERROR]=
    "Syntax error in config_version.ini file";
  ic_error_str[IC_ERROR_PROCESS_ALREADY_BEING_KILLED - IC_FIRST_ERROR]=
    "The process is already in the process of being killed";
  ic_error_str[IC_ERROR_TOO_LARGE_ERROR_MESSAGE - IC_FIRST_ERROR]=
    "Error message received is too large";
  ic_error_str[IC_ERROR_FAILED_TO_START_PROCESS - IC_FIRST_ERROR]=
    "Failed to start process";
  ic_error_str[IC_ERROR_PROGRAM_NOT_SUPPORTED - IC_FIRST_ERROR]=
    "Program not supported";
  ic_error_str[IC_ERROR_FILE_ALREADY_EXISTS - IC_FIRST_ERROR]=
    "File already exists";
  ic_error_str[IC_ERROR_STARTUP - IC_FIRST_ERROR]=
    "Startup error";
  ic_error_str[IC_ERROR_WRONG_IP_FAMILY - IC_FIRST_ERROR]=
    "Wrong IP family";
  ic_error_str[IC_ERROR_MISSING_SCHEMA_NAME - IC_FIRST_ERROR]=
    "Missing schema name in metadata operation";
  ic_error_str[IC_ERROR_MISSING_DATABASE_NAME - IC_FIRST_ERROR]=
    "Missing database name in metadata operation";
  ic_error_str[IC_ERROR_MISSING_TABLE_NAME - IC_FIRST_ERROR]=
    "Missing table name in metadata operation";
  ic_error_str[IC_ERROR_TIMEOUT_WAITING_FOR_NODES - IC_FIRST_ERROR]=
    "Timeout waiting for API to succeed in setting up node connections"
    " to cluster";
  ic_error_str[IC_ERROR_FOUND_NO_CONNECTED_NODES - IC_FIRST_ERROR]=
    "Found no master node since API had no connected nodes for the cluster";
  ic_error_str[IC_ERROR_NO_SUCH_DATA_TYPE - IC_FIRST_ERROR]=
    "No such data type exists";
  ic_error_str[IC_ERROR_MISSING_COLUMN_NAME - IC_FIRST_ERROR]=
    "Missing column name in metadata operation";
  ic_error_str[IC_ERROR_TOO_LONG_TABLE_NAME - IC_FIRST_ERROR]=
    "Table name too long";
  ic_error_str[IC_ERROR_ALREADY_CREATED_METADATA_OBJECT - IC_FIRST_ERROR]=
    "Metadata object already created on API Data Connection";
  ic_error_str[IC_ERROR_FAILED_TO_CHANGE_DIR - IC_FIRST_ERROR]=
    "ic_daemonize failed to change directory";
  ic_error_str[IC_ERROR_FAILED_OPEN_STDOUT - IC_FIRST_ERROR]=
    "Failed to open stdout file after daemonize";
  ic_error_str[IC_ERROR_WRONG_PID_FILE_CONTENT - IC_FIRST_ERROR]=
    "Wrong content in pid file found when reading pid file";
  ic_error_str[IC_ERROR_PROGRAM_NOT_CERTIFIED_FOR_START - IC_FIRST_ERROR]=
    "Attempt to start uncertified program in process controller";
  ic_error_str[IC_ERROR_NO_PROPER_NODE_ID_FOR_PROGRAM - IC_FIRST_ERROR]=
    "Attempt to start program without proper node id set";
  ic_error_str[IC_ERROR_FAILED_TO_SPAWN_PROGRAM - IC_FIRST_ERROR]=
    "Failed to spawn program from process controller";
  ic_error_str[IC_ERROR_CONFIGURATION_ERROR - IC_FIRST_ERROR]=
    "Error found in configuration file(s)";
  ic_error_str[IC_ERROR_MALFORMED_CLIENT_STRING - IC_FIRST_ERROR]=
    "Commands in client must end with ;, any other place for ; is an error";
  ic_error_str[IC_ERROR_TOO_MANY_CLUSTER_MANAGERS - IC_FIRST_ERROR]=
    "Too many cluster managers in configuration";
  ic_error_str[IC_ERROR_NO_CONF_ENTRY_FOUND - IC_FIRST_ERROR]=
    "no configuration entry found in configuration";
  ic_error_str[IC_ERROR_NO_DEF_NODE_SECT_FOUND - IC_FIRST_ERROR]=
    "no default node section found in configuration";
  ic_error_str[IC_ERROR_NO_SUCH_NODE_TYPE - IC_FIRST_ERROR]=
    "incorrect node type given";
#ifdef DEBUG
  /* Verify we have set an error message for all error codes */
  for (i= IC_FIRST_ERROR; i <= IC_LAST_ERROR; i++)
  {
    if (!ic_error_str[i - IC_FIRST_ERROR])
      abort();
  }
#endif
  return;
}

