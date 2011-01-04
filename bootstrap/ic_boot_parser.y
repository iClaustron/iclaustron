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
%token CS_INTERNAL_PORT_SYM
%token EQUAL_SYM
%token FILES_SYM
%token HOST_SYM
%token MANAGERS_SYM
%token NODE_ID_SYM
%token PCNTRL_HOST_SYM
%token PCNTRL_PORT_SYM
%token PREPARE_SYM
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

%type <ic_str> IDENTIFIER_SYM hostname
%type <int_val> INTEGER_SYM number

%%

command:
    any_command END_SYM
    ;

any_command:
    prepare_cluster_server_command
    | send_command
    | start_cluster_servers_command
    | verify_command
    | start_cluster_managers_command
    ;

prepare_cluster_server_command:
    PREPARE_SYM CLUSTER_SYM SERVER_SYM
    {
      PARSE_DATA->command= IC_PREPARE_CLUSTER_SERVER_CMD;
      PARSE_DATA->cs_index= PARSE_DATA->next_cs_index;
      PARSE_DATA->next_cs_index++;
    }
    host opt_pcntrl_host opt_pcntrl_port opt_cs_int_host opt_node
    {}
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
    HOST_SYM opt_equal hostname
    {
    }

opt_pcntrl_host:
    /* empty */
    {}
    | PCNTRL_HOST_SYM opt_equal hostname
    {}
    ;

opt_pcntrl_port:
    /* empty */
    {}
    | PCNTRL_PORT_SYM opt_equal number
    {}
    ;

opt_cs_int_host:
    /* empty */
    {}
    | CS_INTERNAL_PORT_SYM opt_equal number
    {}
    ;

opt_node:
    /* empty */
    {}
    | NODE_ID_SYM opt_equal number
    {}
    ;

number:
    INTEGER_SYM { $$= $1; }
    ;

hostname:
    IDENTIFIER_SYM { $$= $1; }
    ;

opt_equal:
    /* empty */
    {}
    | EQUAL_SYM
    {}
    ;
%%
