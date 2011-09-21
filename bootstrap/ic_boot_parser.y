/* Copyright (C) 2010-2011 iClaustron AB

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
#include <../bootstrap/ic_boot_int.h>

int ic_boot_lex(void *parse_data, void *scanner);

#define YYMALLOC ic_malloc
#define YYFREE ic_free

#define yylex ic_boot_lex
#define yyerror ic_boot_parse_error
%}
%union{
  guint64 int_val;
  IC_STRING *ic_str;
}

%token CLUSTER_SYM
%token EQUAL_SYM
%token EXIT_SYM
%token FILES_SYM
%token HOST_SYM
%token MANAGER_SYM
%token MANAGERS_SYM
%token NODE_ID_SYM
%token PCNTRL_HOST_SYM
%token PCNTRL_PORT_SYM
%token PREPARE_SYM
%token QUIT_SYM
%token SEND_SYM
%token SERVER_SYM
%token SERVERS_SYM
%token START_SYM
%token VERIFY_SYM

%token END_SYM
%token IDENTIFIER_SYM
%token INTEGER_SYM

%pure_parser
%parse-param { IC_PARSE_DATA *parse_data }
%lex-param   { IC_PARSE_DATA *parse_data }

%type <ic_str> IDENTIFIER_SYM name opt_pcntrl_host host
%type <int_val> INTEGER_SYM number node opt_pcntrl_port

%%

command:
    any_command
    ;

any_command:
    prepare_command
    | send_command
    | start_cluster_servers_command
    | verify_command
    | start_cluster_managers_command
    | quit_command
    | exit_command
    ;

quit_command:
    QUIT_SYM { PARSE_DATA->command= IC_EXIT_CMD; }
    ;

exit_command:
    EXIT_SYM { PARSE_DATA->command= IC_EXIT_CMD; }
    ;

prepare_command:
     PREPARE_SYM CLUSTER_SYM prep_cmd

prep_cmd:
    server_cmd {}
    | mgr_cmd {}
    ;

server_cmd:
    SERVER_SYM host opt_pcntrl_host opt_pcntrl_port node
    {
      IC_CLUSTER_SERVER_DATA *cs_data;
      guint32 cs_index;
 
      PARSE_DATA->command= IC_PREPARE_CLUSTER_SERVER_CMD;
      cs_index= PARSE_DATA->next_cs_index;
      PARSE_DATA->next_cs_index++;
      if (PARSE_DATA->next_cs_index <= IC_MAX_CLUSTER_SERVERS)
      {
        cs_data= &PARSE_DATA->cs_data[cs_index];
        cs_data->hostname= $2->str;
        cs_data->pcntrl_hostname= $3 ? $3->str : $2->str;
        cs_data->pcntrl_port= $4;
        cs_data->node_id= $5;
      }
    }
    ;

mgr_cmd:
    MANAGER_SYM host opt_pcntrl_host opt_pcntrl_port node
    {
      IC_CLUSTER_MANAGER_DATA *mgr_data;
      guint32 mgr_index;

      PARSE_DATA->command= IC_PREPARE_CLUSTER_MANAGER_CMD;
      mgr_index= PARSE_DATA->next_mgr_index;
      PARSE_DATA->next_mgr_index++;
      if (PARSE_DATA->next_mgr_index <= IC_MAX_CLUSTER_MANAGERS)
      {
        mgr_data= &PARSE_DATA->mgr_data[mgr_index];
        mgr_data->hostname= $2->str;
        mgr_data->pcntrl_hostname= $3 ? $3->str : $2->str;
        mgr_data->pcntrl_port= $4;
        mgr_data->node_id= $5;
      }
    }
    ;

send_command:
    SEND_SYM FILES_SYM
    {
      PARSE_DATA->command= IC_SEND_FILES_CMD;
    }

start_cluster_servers_command:
    START_SYM CLUSTER_SYM SERVERS_SYM
    {
      PARSE_DATA->command= IC_START_CLUSTER_SERVERS_CMD;
    }

verify_command:
    VERIFY_SYM CLUSTER_SYM SERVERS_SYM
    {
      PARSE_DATA->command= IC_VERIFY_CLUSTER_SERVERS_CMD;
    }

start_cluster_managers_command:
    START_SYM CLUSTER_SYM MANAGERS_SYM
    {
      PARSE_DATA->command= IC_START_CLUSTER_MANAGERS_CMD;
    }

host:
    HOST_SYM opt_equal name
    { $$= $3; }

opt_pcntrl_host:
    /* empty */
    { $$= NULL; }
    | PCNTRL_HOST_SYM opt_equal name
    { $$= $3; }
    ;

opt_pcntrl_port:
    /* empty */
    { $$= IC_DEF_PCNTRL_PORT; }
    | PCNTRL_PORT_SYM opt_equal number
    { $$= $3; }
    ;

node:
    NODE_ID_SYM opt_equal number
    { $$= $3;}
    ;

number:
    INTEGER_SYM
    { $$= $1; }
    ;

name:
    IDENTIFIER_SYM
    { $$= $1; }
    ;

opt_equal:
    /* empty */
    {}
    | EQUAL_SYM
    {}
    ;
%%
