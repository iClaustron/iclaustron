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

#include <ic_common.h>
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
#define IC_LAST_ERROR 7034
#define IC_MAX_ERRORS 100

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
}

void
ic_print_error(int error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    printf("%d: %s\n", error_number, no_such_error_str);
  else
    printf("%s\n", ic_error_str[error_number - IC_FIRST_ERROR]);
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

  if (error_code == PROTOCOL_ERROR)
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
