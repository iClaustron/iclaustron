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

#ifndef IC_ERR_H
#define IC_ERR_H
/* Basic system error handling header file */
#include <errno.h>
/*
  HEADER MODULE: iClaustron Error Handling
  ----------------------------------------
  Error handling interface
*/
void ic_init_error_messages();
void ic_print_error(int error_number);
gchar *ic_get_error_message(int error_number);
gchar *ic_common_fill_error_buffer(const gchar *error_message,
                                   guint32 error_line,
                                   int error_code,
                                   gchar *error_buffer);
gchar *ic_get_strerror(int error_number, gchar *buf, guint32 buf_len);

struct ic_config_error
{
  int err_num;
  guint32 line_number;
};
typedef struct ic_config_error IC_CONFIG_ERROR;

#define IC_ERROR_CONFIG_LINE_TOO_LONG 7000
#define IC_ERROR_CONFIG_BRACKET 7001
#define IC_ERROR_CONFIG_INCORRECT_GROUP_ID 7002
#define IC_ERROR_CONFIG_IMPROPER_KEY_VALUE 7003
#define IC_ERROR_CONFIG_NO_SUCH_SECTION 7004
#define IC_ERROR_MEM_ALLOC 7005
#define IC_ERROR_NO_SECTION_DEFINED_YET 7006
#define IC_ERROR_NO_SUCH_CONFIG_KEY 7007
#define IC_ERROR_DEFAULT_VALUE_FOR_MANDATORY 7008
#define IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION 7009
#define IC_ERROR_NO_NODES_FOUND 7010
#define IC_ERROR_WRONG_CONFIG_NUMBER 7011
#define IC_ERROR_NO_BOOLEAN_VALUE 7012
#define IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS 7013
#define IC_ERROR_NO_SERVER_NAME 7014
#define IC_ERROR_NO_SERVER_PORT 7015
#define IC_ERROR_GETADDRINFO 7016
#define IC_ERROR_ILLEGAL_SERVER_PORT 7017
#define IC_ERROR_DIFFERENT_IP_VERSIONS 7018
#define IC_ERROR_ILLEGAL_CLIENT_PORT 7019
#define IC_ERROR_INCONSISTENT_DATA 7020
#define IC_ERROR_NODE_DOWN 7021
#define IC_ERROR_NO_SUCH_CLUSTER 7022
#define IC_ERROR_NO_SUCH_NODE 7023
#define IC_ERROR_MESSAGE_CHECKSUM 7024
#define IC_ERROR_ACCEPT_TIMEOUT 7025
#define IC_ERROR_POLL_SET_FULL 7026
#define IC_ERROR_NOT_FOUND_IN_POLL_SET 7027
#define IC_ERROR_NODE_ALREADY_DEFINED 7028
#define IC_ERROR_PROCESS_NOT_ALIVE 7029
#define IC_ERROR_COULD_NOT_LOCK_CONFIGURATION 7030
#define IC_ERROR_CHECK_PROCESS_SCRIPT 7031
#define IC_ERROR_BOOTSTRAP_ALREADY_PERFORMED 7032
#define IC_ERROR_CONFLICTING_CLUSTER_IDS 7033
#define IC_ERROR_FAILED_TO_OPEN_FILE 7034
#define IC_ERROR_BOOTSTRAP_NEEDED 7035
#define IC_ERROR_CONNECT_THREAD_STOPPED 7036
#define IC_ERROR_THREADPOOL_FULL 7037
#define IC_ERROR_START_THREAD_FAILED 7038
#define IC_ERROR_RECEIVE_TIMEOUT 7039
#define IC_ERROR_CONNECT_TIMEOUT 7040
#define IC_ERROR_STOP_ORDERED 7041
#define IC_ACCEPT_ERROR 7042
#define IC_END_OF_FILE 7043
#define IC_PROTOCOL_ERROR 7044
#define IC_AUTHENTICATE_ERROR 7045
#define IC_SSL_ERROR 7046
#define IC_ERROR_PC_START_ALREADY_ONGOING 7047
#define IC_ERROR_PC_PROCESS_ALREADY_RUNNING 7048
#define IC_ERROR_PROCESS_STUCK_IN_START_PHASE 7049
#define IC_ERROR_FAILED_TO_STOP_PROCESS 7050
#define IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_PARAM 7051
#define IC_ERROR_SET_CONNECTION_PARAMETER_WRONG_NODES 7052
#define IC_ERROR_INDEX_ZERO_NOT_ALLOWED 7053
#define IC_ERROR_TRANSLATION_INDEX_OUT_OF_BOUND 7054
#define IC_ERROR_TRANSLATION_INDEX_ERROR 7055
#define IC_ERROR_PARSE_CONNECTSTRING 7056
#define IC_ERROR_TOO_MANY_CS_HOSTS 7057
#define IC_ERROR_FAILED_TO_DAEMONIZE 7058
#endif
