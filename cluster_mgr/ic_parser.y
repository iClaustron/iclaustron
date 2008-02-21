/* Copyright (C) 2007 iClaustron AB

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
#include <ic_common.h>
#include <ic_clmgr.h>
int yylex(void *parse_data, void *scanner);
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
%token DIE_SYM
%token DISPLAY_SYM
%token FILE_SYM
%token FROM_SYM
%token GROUP_SYM
%token IC_KERNEL_SYM
%token KILL_SYM
%token LIST_SYM
%token LISTEN_SYM
%token MANAGER_SYM
%token MEMORY_SYM
%token MOVE_SYM
%token MYSQLD_SYM
%token MYSQLD_SAFE_SYM
%token NODE_SYM
%token NODEGROUP_SYM
%token NODEGROUPS_SYM
%token PERFORM_SYM
%token REPLICATION_SYM
%token RESTART_SYM
%token RESTORE_SYM
%token ROLLING_SYM
%token SEEN_SYM
%token SERVER_SYM
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

%token END
%token IDENTIFIER
%token INTEGER
%token VERSION_IDENTIFIER

%pure_parser
%parse-param { IC_PARSE_DATA *parse_data }
%parse-param { void *scanner }
%lex-param   { yyscan_t *scanner }

%type <ic_str> IDENTIFIER VERSION_IDENTIFIER cluster_name node_name
%type <int_val> INTEGER cluster_id node_id

%%

command:
    any_command END
    ;

any_command:
    action_command
    | statistics_command
    | set_state_command
    | status_command
    ;

action_command:
    die_command
    | kill_command
    | move_command
    | perform_command
    | restart_command
    | start_command
    | stop_command
    ;

status_command:
    list_command
    | listen_command
    | show_command
    ;

set_state_command:
    set_command
    | use_command
    ;

statistics_command:
    display_command
    | top_command
    ;

die_command:
    DIE_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_DIE_CMD; }
    ;

kill_command:
    KILL_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_DIE_CMD; }
    ;

move_command:
    MOVE_SYM opt_cluster
    { PARSE_DATA->command= IC_MOVE_CMD; }
    ;

perform_command:
    PERFORM_SYM BACKUP_SYM
    { PARSE_DATA->command= IC_PERFORM_BACKUP_CMD; }
    | PERFORM_SYM opt_cluster ROLLING_SYM UPGRADE_SYM
    { PARSE_DATA->command= IC_PERFORM_ROLLING_UPGRADE_CMD; }
    ;

restart_command:
    RESTART_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_RESTART_CMD; }
    ;

start_command:
    START_SYM start_params
    { PARSE_DATA->command= IC_START_CMD; }

start_params:
    binary_type opt_cluster node_or_all
    | opt_cluster opt_node opt_all
    ;

binary_type:
    IC_KERNEL_SYM
    { PARSE_DATA->binary_type= IC_KERNEL_NODE; }
    | CLUSTER_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_CLUSTER_SERVER_NODE; }
    | SQL_SYM
    { PARSE_DATA->binary_type= IC_SQL_SERVER_NODE; }
    | REPLICATION_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_REP_SERVER_NODE; }
    | FILE_SYM SERVER_SYM
    { PARSE_DATA->binary_type= IC_FILE_SERVER_NODE; }
    | RESTORE_SYM
    { PARSE_DATA->binary_type= IC_RESTORE_NODE; }
    | CLUSTER_SYM MANAGER_SYM
    { PARSE_DATA->binary_type= IC_CLUSTER_MANAGER_NODE; }
    ;

node_or_all:
    node
    | all
    ;

stop_command:
    STOP_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_STOP_CMD; }
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
    | show_stat_command
    ;

show_cluster_command:
    SHOW_SYM CLUSTER_SYM opt_cluster
    { PARSE_DATA->command= IC_SHOW_CLUSTER_CMD; }
    ;

show_cluster_status_command:
    SHOW_SYM CLUSTER_SYM STATUS_SYM opt_cluster opt_seen_from
    { PARSE_DATA->command= IC_SHOW_CLUSTER_STATUS_CMD; }
    ;

show_connections_command:
    SHOW_SYM CONNECTIONS_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_SHOW_CONNECTIONS_CMD; }
    ;

show_config_command:
    SHOW_SYM CONFIG_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_SHOW_CONFIG_CMD; }
    ;

show_memory_command:
    SHOW_SYM MEMORY_SYM opt_cluster opt_node opt_all
    { PARSE_DATA->command= IC_SHOW_MEMORY_CMD; }
    ;

show_statvars_command:
    SHOW_SYM STATVARS_SYM opt_cluster opt_node opt_group
    { PARSE_DATA->command= IC_SHOW_STATVARS_CMD; }
    ;

show_stat_command:
    SHOW_SYM STATS_SYM opt_group
    { PARSE_DATA->command= IC_SHOW_STATS_CMD; }
    ;

set_command:
    SET_SYM STAT_LEVEL_SYM opt_cluster opt_node opt_group opt_variable
    { PARSE_DATA->command= IC_SET_STAT_LEVEL_CMD; }
    ;

use_command:
    USE_SYM CLUSTER_SYM one_cluster_reference
    { PARSE_DATA->command= IC_USE_CLUSTER_CMD; }
    ;

display_command:
    DISPLAY_SYM STATS_SYM opt_cluster opt_node opt_group opt_variable
    { PARSE_DATA->command= IC_DISPLAY_STATS_CMD; }
    ;

top_command:
    TOP_SYM opt_cluster { PARSE_DATA->command= IC_TOP_CMD; }
    ;

opt_variable:
    /* empty */
    | VARIABLE_SYM IDENTIFIER
    ;

opt_group:
    /* empty */
    | group_reference
    ;

group_reference:
    GROUP_SYM ALL_SYM
    | GROUP_SYM IDENTIFIER
    ;

opt_seen_from:
    /* empty */
    | SEEN_SYM FROM_SYM opt_node
    ;

opt_cluster:
    /* empty */
    { PARSE_DATA->default_cluster= TRUE; }
    | CLUSTER_SYM cluster_reference
    ;

one_cluster_reference:
    cluster_id { PARSE_DATA->cluster_id= $1; }
    | cluster_name
    { memcpy(&PARSE_DATA->cluster_name, $1,sizeof(IC_STRING)); }
    ;

cluster_reference:
    cluster_id { PARSE_DATA->cluster_id= $1; }
    | cluster_name
    { memcpy(&PARSE_DATA->cluster_name,$1,sizeof(IC_STRING)); }
    | ALL_SYM { PARSE_DATA->cluster_all= TRUE; }
    ;

cluster_id:
    INTEGER { $$= $1; }
    ;

cluster_name:
    IDENTIFIER { $$= $1; }
    ;

opt_node:
    /* empty */
    { PARSE_DATA->default_node= TRUE; }
    | node
    ;

node:
    NODE_SYM node_reference
    ;

node_reference:
    node_id { PARSE_DATA->node_id= $1; }
    | node_name {memcpy(&PARSE_DATA->node_name,&$1,sizeof(IC_STRING));}
    | ALL_SYM { PARSE_DATA->node_all= TRUE; }
    ;

node_id:
    INTEGER { $$= $1; }
    ;

node_name:
    IDENTIFIER { $$= $1; }
    ;

opt_all:
    /* empty */
    | all
    ;

all:
    ALL_SYM { PARSE_DATA->all= TRUE; }
%%

