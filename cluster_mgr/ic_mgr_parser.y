/* Copyright (C) 2007, 2015 iClaustron AB

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

%{
#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_lex_support.h>
#include <../cluster_mgr/ic_clmgr_int.h>

int ic_mgr_lex(void *parse_data, void *scanner);

#define YYMALLOC ic_malloc
#define YYFREE ic_free

#define yylex ic_mgr_lex
#define yyerror ic_mgr_parse_error
%}
%union{
  guint64 int_val;
  IC_STRING *ic_str;
}

%token AS_SYM
%token ALL_SYM
%token BACKUP_SYM
%token CLUSTER_SYM
%token CLUSTERS_SYM
%token CLUSTER_LOG_SYM
%token CONFIG_SYM
%token CONNECTIONS_SYM
%token COOKIES_SYM
%token COUNT_SYM
%token DATA_SYM
%token DIE_SYM
%token DISPLAY_SYM
%token EXIT_SYM
%token FILE_SYM
%token FROM_SYM
%token GROUP_SYM
%token ICLAUSTRON_SYM
%token INITIAL_SYM
%token KILL_SYM
%token LIST_SYM
%token LISTEN_SYM
%token MANAGER_SYM
%token MANAGERS_SYM
%token MEMORY_SYM
%token MOVE_SYM
%token NDB_SYM
%token NODE_SYM
%token NODEGROUP_SYM
%token NODEGROUPS_SYM
%token PERFORM_SYM
%token QUIT_SYM
%token REPLICATION_SYM
%token RESTART_SYM
%token RESTORE_SYM
%token ROLLING_SYM
%token SEEN_SYM
%token SERVER_SYM
%token SERVERS_SYM
%token SET_SYM
%token SHOW_SYM
%token SQL_SYM
%token START_SYM
%token STATUS_SYM
%token STATS_SYM
%token STATVARS_SYM
%token STAT_LEVEL_SYM
%token STOP_SYM
%token TO_SYM
%token TOP_SYM
%token UPGRADE_SYM
%token USE_SYM
%token VARIABLE_SYM
%token VERSION_SYM

%token END_SYM
%token IDENTIFIER_SYM
%token INTEGER_SYM
%token VERSION_IDENTIFIER_SYM

%pure_parser
%parse-param { IC_PARSE_DATA *parse_data }
%lex-param   { IC_PARSE_DATA *parse_data }

%type <ic_str> IDENTIFIER_SYM VERSION_IDENTIFIER_SYM cluster_name node_name
%type <int_val> INTEGER_SYM cluster_id node_id

%%

command:
    any_command
    ;

any_command:
    action_command
    | statistics_command
    | set_state_command
    | status_command
    | stop_client_command
    | cookie_command
    ;

cookie_command:
    count_cookies_command
    ;

stop_client_command:
    exit_command
    | quit_command
    ;

count_cookies_command:
    COUNT_SYM COOKIES_SYM
    { PARSE_DATA->command= IC_COUNT_COOKIES_CMD; }

exit_command:
    EXIT_SYM
    { PARSE_DATA->command= IC_STOP_CLIENT_CMD; }

quit_command:
    QUIT_SYM
    { PARSE_DATA->command= IC_STOP_CLIENT_CMD; }

/**
 * Action commands, commands to start and stop nodes in various ways and
 * perform backups, perform rolling upgrades and move clusters.
 */

action_command:
    start_command
    | restart_command
    | stop_command
    | die_command
    | kill_command
    | move_command
    | perform_command
    ;

start_command:
    START_SYM target_specifier opt_initial
    { PARSE_DATA->command= IC_START_CMD; }

restart_command:
    RESTART_SYM target_specifier
    { PARSE_DATA->command= IC_RESTART_CMD; }
    ;

stop_command:
    STOP_SYM target_specifier
    { PARSE_DATA->command= IC_STOP_CMD; }
    ;

die_command:
    DIE_SYM target_specifier
    { PARSE_DATA->command= IC_DIE_CMD; }
    ;

kill_command:
    KILL_SYM target_specifier
    { PARSE_DATA->command= IC_DIE_CMD; }
    ;

move_command:
    MOVE_SYM opt_cluster
    { PARSE_DATA->command= IC_MOVE_CMD; }
    ;

perform_command:
    PERFORM_SYM BACKUP_SYM opt_cluster
    { PARSE_DATA->command= IC_PERFORM_BACKUP_CMD; }
    | PERFORM_SYM ROLLING_SYM UPGRADE_SYM opt_cluster
    { PARSE_DATA->command= IC_PERFORM_ROLLING_UPGRADE_CMD; }
    | PERFORM_SYM ROLLING_SYM UPGRADE_SYM CLUSTER_SYM SERVERS_SYM
    { PARSE_DATA->command= IC_PERFORM_ROLLING_UPGRADE_CLUSTER_SERVERS_CMD; }
    | PERFORM_SYM ROLLING_SYM UPGRADE_SYM CLUSTER_SYM MANAGERS_SYM
    { PARSE_DATA->command= IC_PERFORM_ROLLING_UPGRADE_CLUSTER_MANAGERS_CMD; }
    ;

opt_initial:
    /* empty */
    {
      PARSE_DATA->initial_flag= FALSE;
      PARSE_DATA->restart_flag= FALSE;
    }
    | INITIAL_SYM
    {
      PARSE_DATA->initial_flag= TRUE;
      PARSE_DATA->restart_flag= FALSE;
    }
    | RESTART_SYM
    {
      PARSE_DATA->initial_flag= FALSE;
      PARSE_DATA->restart_flag= TRUE;
    }
    | INITIAL_SYM RESTART_SYM
    {
      PARSE_DATA->initial_flag= TRUE;
      PARSE_DATA->restart_flag= TRUE;
    }
    | RESTART_SYM INITIAL_SYM
    {
      PARSE_DATA->initial_flag= TRUE;
      PARSE_DATA->restart_flag= TRUE;
    }
    ;

/**
 * Commands to show statistics generated by cluster operations.
 */
statistics_command:
    display_command
    | top_command
    ;

display_command:
    DISPLAY_SYM STATS_SYM opt_cluster node opt_group opt_variable
    { PARSE_DATA->command= IC_DISPLAY_STATS_CMD; }
    ;

top_command:
    TOP_SYM opt_cluster { PARSE_DATA->command= IC_TOP_CMD; }
    ;

/**
 * Commands to set the level of statistics generated, to set the current
 * cluster, to set the version of the NDB nodes and the version of the
 * iClaustron nodes (used when starting new nodes).
 */
set_state_command:
    set_command
    | use_command
    ;

set_command:
    SET_SYM STAT_LEVEL_SYM opt_cluster node opt_group opt_variable
    { PARSE_DATA->command= IC_SET_STAT_LEVEL_CMD; }
    ;

use_command:
    use_cluster_command
    | use_version_ndb_command
    | use_version_iclaustron_command
    ;

use_cluster_command:
    USE_SYM CLUSTER_SYM one_cluster_reference
    { PARSE_DATA->command= IC_USE_CLUSTER_CMD; }
    ;

use_version_ndb_command:
    USE_SYM VERSION_SYM NDB_SYM VERSION_IDENTIFIER_SYM
    {
      memcpy(&PARSE_DATA->ndb_version_name,&$4,sizeof(IC_STRING));
      PARSE_DATA->command= IC_USE_VERSION_NDB_CMD;
    }
    ;

use_version_iclaustron_command:
    USE_SYM VERSION_SYM ICLAUSTRON_SYM VERSION_IDENTIFIER_SYM
    {
      memcpy(&PARSE_DATA->iclaustron_version_name,&$4,sizeof(IC_STRING));
      PARSE_DATA->command= IC_USE_VERSION_ICLAUSTRON_CMD;
    }
    ;

/**
 * Many different commands to show the status of the nodes, the configuration,
 * the memory used, the logs written, the clusters in use, the connections
 * that are set up and to show the statistics level and the statistical
 * variables available.
 */
status_command:
    list_command
    | listen_command
    | show_command
    ;

list_command:
    LIST_SYM CLUSTERS_SYM
    { PARSE_DATA->command= IC_LIST_CMD; }
    ;

listen_command:
    LISTEN_SYM CLUSTER_LOG_SYM
    { PARSE_DATA->command= IC_LISTEN_CMD; }
    ;

show_command:
    show_cluster_command
    | show_cluster_status_command
    | show_connections_command
    | show_config_command
    | show_memory_command
    | show_statvars_command
    ;

show_cluster_command:
    SHOW_SYM opt_cluster_reference
    { PARSE_DATA->command= IC_SHOW_CLUSTER_CMD; }
    ;

show_cluster_status_command:
    SHOW_SYM opt_cluster_reference STATUS_SYM opt_seen_from
    { PARSE_DATA->command= IC_SHOW_CLUSTER_STATUS_CMD; }
    ;

show_connections_command:
    SHOW_SYM CONNECTIONS_SYM target_specifier
    { PARSE_DATA->command= IC_SHOW_CONNECTIONS_CMD; }
    ;

show_config_command:
    SHOW_SYM CONFIG_SYM target_specifier
    { PARSE_DATA->command= IC_SHOW_CONFIG_CMD; }
    ;

show_memory_command:
    SHOW_SYM MEMORY_SYM target_specifier
    { PARSE_DATA->command= IC_SHOW_MEMORY_CMD; }
    ;

show_statvars_command:
    SHOW_SYM STATVARS_SYM opt_cluster node opt_group
    { PARSE_DATA->command= IC_SHOW_STATVARS_CMD; }
    ;

opt_variable:
    /* empty */
    | VARIABLE_SYM IDENTIFIER_SYM
    ;

opt_group:
    /* empty */
    | group_reference
    ;

group_reference:
    GROUP_SYM ALL_SYM
    | GROUP_SYM IDENTIFIER_SYM
    ;

opt_seen_from:
    /* empty */
    | SEEN_SYM FROM_SYM node
    ;

opt_cluster_reference:
    CLUSTER_SYM
    { PARSE_DATA->default_cluster= TRUE; }
    | one_cluster_reference
    ;

opt_cluster:
    /* empty */
    { PARSE_DATA->default_cluster= TRUE; }
    | CLUSTER_SYM one_cluster_reference
    ;

target_specifier:
    binary_type node_target_specifier
    { PARSE_DATA->binary_type_flag= TRUE; }
    | node_target_specifier
    { PARSE_DATA->binary_type_flag= FALSE; }
    ;

node_target_specifier:
    cluster node
    | node
    { PARSE_DATA->default_cluster= TRUE; }
    | cluster_all
    | node_all
    { PARSE_DATA->default_cluster= TRUE; }
    ;

binary_type:
    CLUSTER_SYM MANAGER_SYM
    { PARSE_DATA->binary_type= IC_CLUSTER_MANAGER_NODE; }
    | CLUSTER_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_CLUSTER_SERVER_NODE; }
    | DATA_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_DATA_SERVER_NODE; }
    | SQL_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_SQL_SERVER_NODE; }
    | REPLICATION_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_REP_SERVER_NODE; }
    | FILE_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_FILE_SERVER_NODE; }
    | RESTORE_SYM
    { PARSE_DATA->binary_type= IC_RESTORE_NODE; }
    ;
 
node:
    NODE_SYM node_reference
    ;

node_reference:
    node_id { PARSE_DATA->node_id= $1; }
    | node_name {memcpy(&PARSE_DATA->node_name,&$1,sizeof(IC_STRING));}
    ;

node_id:
    INTEGER_SYM { $$= $1; }
    ;

node_name:
    IDENTIFIER_SYM { $$= $1; }
    ;

cluster:
    CLUSTER_SYM one_cluster_reference
    ;

one_cluster_reference:
    cluster_id { PARSE_DATA->cluster_id= $1; }
    | cluster_name
    { memcpy(&PARSE_DATA->cluster_name, $1, sizeof(IC_STRING)); }
    ;

cluster_id:
    INTEGER_SYM { $$= $1; }
    ;

cluster_name:
    IDENTIFIER_SYM { $$= $1; }
    ;

node_all:
    NODE_SYM ALL_SYM { PARSE_DATA->node_all= TRUE; }

cluster_all:
    ALL_SYM { PARSE_DATA->cluster_all= TRUE; }

%%
