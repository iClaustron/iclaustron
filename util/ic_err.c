/* Copyright (C) 2007-2009 iClaustron AB

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
#include <ic_debug.h>
#include <ic_string.h>
#include <ic_err.h>

/*
  MODULE: iClaustron Error Handling
  ---------------------------------
  Module for printing error codes and retrieving error messages
  given the error number.

  To add a new error number do the following:
  1) Change IC_LAST_ERROR
  2) Add a new entry in ic_init_error_messages
  3) Add the new error code in ic_err.h
*/

#define IC_FIRST_ERROR 7000
#define IC_LAST_ERROR 7070
#define IC_MAX_ERRORS 200

static gchar* ic_error_str[IC_MAX_ERRORS];
static gchar *no_such_error_str= "No such error";

void
ic_init_error_messages()
{
  guint32 i;
  DEBUG_ENTRY("ic_init_error_messages");
  for (i= 0; i < IC_MAX_ERRORS; i++)
    ic_error_str[i]= NULL;
  ic_error_str[IC_ERROR_CONFIG_LINE_TOO_LONG -IC_FIRST_ERROR]=
    "Line was longer than 120 characters";
  ic_error_str[IC_ERROR_CONFIG_BRACKET - IC_FIRST_ERROR]=
    "Missing ] after initial [";
  ic_error_str[IC_ERROR_CONFIG_INCORRECT_GROUP_ID - IC_FIRST_ERROR]=
    "Found incorrect group id";
  ic_error_str[IC_ERROR_CONFIG_IMPROPER_KEY_VALUE - IC_FIRST_ERROR]=
    "Improper key-value pair";
  ic_error_str[IC_ERROR_CONFIG_NO_SUCH_SECTION - IC_FIRST_ERROR]=
    "Section name doesn't exist in this configuration";
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
  ic_error_str[IC_ERROR_NODE_DOWN - IC_FIRST_ERROR]=
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
    "The Cluster Server failed to lock the configuration";
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
  ic_error_str[IC_ERROR_ACCEPT_TIMEOUT - IC_FIRST_ERROR]=
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
  ic_error_str[IC_ERROR_TRANSLATION_INDEX_OUT_OF_BOUND - IC_FIRST_ERROR]=
    "Index out of bound in dynamic translation";
  ic_error_str[IC_ERROR_TRANSLATION_INDEX_ERROR - IC_FIRST_ERROR]=
    "Trying to read non-existent entry in dynamic translation";
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
}

void
ic_print_error(int error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
  {
    ic_printf("%d: %s", error_number, no_such_error_str);
    if (sys_errlist[error_number])
      perror(sys_errlist[error_number]);
  }
  else
    ic_printf("%s", ic_error_str[error_number - IC_FIRST_ERROR]);
}

gchar *ic_get_error_message(int error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    return no_such_error_str;
  else
    return ic_error_str[error_number - IC_FIRST_ERROR];
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
    return NULL;
  if (error_code == IC_PROTOCOL_ERROR)
    line_err_msg= protocol_err_msg;
  else if (error_code != 0 && error_line != 0)
    line_err_msg= line_number_msg;
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
