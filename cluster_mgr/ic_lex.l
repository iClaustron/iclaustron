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
#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include "ic_clmgr_int.h"
#include "ic_parser.h"

int line_no= 1;
#define LEX_DATA ((IC_PARSE_DATA*)yyget_extra(yyscanner))

#define YY_INPUT(buffer, res, max_size) \
{ \
  if (LEX_DATA->parse_inx >= LEX_DATA->parse_str_len) \
    res= YY_NULL; \
  else \
  { \
    res= LEX_DATA->parse_str_len - LEX_DATA->parse_inx; \
    if (res > max_size) res= max_size; \
    memcpy(buffer, LEX_DATA->parse_buf+LEX_DATA->parse_inx, res); \
    LEX_DATA->parse_inx= res; \
  } \
}

%}

%e 1200
%option reentrant noyywrap bison-bridge nounput
%option noyyalloc noyyrealloc noyyfree

%%

AS                      { return AS_SYM;           }
ALL                     { return ALL_SYM;          }
BACKUP                  { return BACKUP_SYM;       }
CLUSTER                 { return CLUSTER_SYM;      }
CLUSTERS                { return CLUSTERS_SYM;     }
CLUSTER_LOG             { return CLUSTER_LOG_SYM;  }
CONFIG                  { return CONFIG_SYM;       }
CONNECTIONS             { return CONNECTIONS_SYM;  }
DATA                    { return DATA_SYM;         }
DIE                     { return DIE_SYM;          }
DISPLAY                 { return DISPLAY_SYM;      }
FILE                    { return FILE_SYM;         }
FROM                    { return FROM_SYM;         }
GROUP                   { return GROUP_SYM;        }
INITIAL                 { return INITIAL_SYM;      }
KILL                    { return KILL_SYM;         }
LIST                    { return LIST_SYM;         }
LISTEN                  { return LISTEN_SYM;       }
MANAGER                 { return MANAGER_SYM;      }
MEMORY                  { return MEMORY_SYM;       }
MOVE                    { return MOVE_SYM;         }
NODE                    { return NODE_SYM;         }
NODEGROUP               { return NODEGROUP_SYM;    }
NODEGROUPS              { return NODEGROUPS_SYM;   }
RESTORE                 { return RESTORE_SYM;      }
PERFORM                 { return PERFORM_SYM;      }
REPLICATION             { return REPLICATION_SYM;  }
RESTART                 { return RESTART_SYM;      }
ROLLING                 { return ROLLING_SYM;      }
SEEN                    { return SEEN_SYM;         }
SERVER                  { return SERVER_SYM;       }
SET                     { return SET_SYM;          }
SHOW                    { return SHOW_SYM;         }
SQL                     { return SQL_SYM;          }
START                   { return START_SYM;        }
STATUS                  { return STATUS_SYM;       }
STATS                   { return STATS_SYM;        }
STATVARS                { return STATVARS_SYM;     }
STAT_LEVEL              { return STAT_LEVEL_SYM;   }
STOP                    { return STOP_SYM;         }
TO                      { return TO_SYM;           }
TOP                     { return TOP_SYM;          }
UPGRADE                 { return UPGRADE_SYM;      }
USE                     { return USE_SYM;          }
VARIABLE                { return VARIABLE_SYM;     }
VERSION                 { return VERSION_SYM;      }


";" { return END; }

[a-z][a-z0-9_]* {
/*[a-z][a-z0-9_-.]* { */
  IC_STRING *ic_str_ptr;
  IC_PARSE_DATA *ic_parse_data= (void*)LEX_DATA;
  IC_MEMORY_CONTAINER *mc_ptr= ic_parse_data->mc_ptr;
  gchar *buf_ptr;

  if (!(ic_str_ptr=
        (IC_STRING*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, sizeof(IC_STRING))) ||
      !(buf_ptr= mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, yyleng)))
  {
    yyerror((void*)ic_parse_data, yyscanner, "Memory allocation failure");
    return 0;
  }
  memcpy(buf_ptr, yytext, yyleng);
  IC_INIT_STRING(ic_str_ptr, buf_ptr, yyleng, FALSE);
  yylval->ic_str= ic_str_ptr;
  return VERSION_IDENTIFIER;
}

[A-Za-z][A-Za-z0-9_]* {
  IC_STRING *ic_str_ptr;
  IC_PARSE_DATA *ic_parse_data= (void*)LEX_DATA;
  IC_MEMORY_CONTAINER *mc_ptr= ic_parse_data->mc_ptr;
  gchar *buf_ptr;

  if (!(ic_str_ptr=
        (IC_STRING*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, sizeof(IC_STRING))) ||
      !(buf_ptr= mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, yyleng)))
  {
    yyerror((void*)ic_parse_data, yyscanner, "Memory allocation failure");
    return 0;
  }
  memcpy(buf_ptr, yytext, yyleng);
  IC_INIT_STRING(ic_str_ptr, buf_ptr, yyleng, FALSE);
  yylval->ic_str= ic_str_ptr;
  return IDENTIFIER;
}

[0-9]+  {
  IC_STRING int_str;
  guint64 int_val;
  int error;

  IC_INIT_STRING(&int_str, yytext, yyleng, FALSE);
  if ((error= ic_conv_config_str_to_int(&int_val, &int_str)))
  {
    yyerror((void*)LEX_DATA, yyscanner, "Integer Overflow");
    return error;
  }
  yylval->int_val= int_val;
  return INTEGER;
}

%%

void* yyalloc(yy_size_t size,
              __attribute__ ((unused)) void *scanner)
{
  return (void*) ic_malloc((size_t)size);
}

void* yyrealloc(void *ptr,
                yy_size_t size,
                __attribute__ ((unused)) void *scanner)
{
  return (void*) ic_realloc((gchar*)ptr, (size_t)size);
}

void yyfree(void *ptr,
            __attribute__ ((unused)) void *scanner)
{
  ic_free(ptr);
}

void
yyerror(void *parse_data,
        __attribute__ ((unused)) void *scanner,
        char *s)
{
  IC_CONNECTION *conn= PARSE_DATA->conn;
  gchar buf[1024];

  g_snprintf(buf, 1024, "Error: %s", s);
  if (ic_send_with_cr(conn, buf) ||
      ic_send_with_cr(conn, ic_empty_string))
    PARSE_DATA->exit_flag= TRUE;
}


void
ic_call_parser(gchar *parse_string, int str_len, void *parse_data)
{
  IC_PARSE_DATA *loc_parse_data= (IC_PARSE_DATA*)parse_data;
  yylex_init(&loc_parse_data->yyscanner);
  yyset_extra(parse_data, loc_parse_data->yyscanner);

  PARSE_DATA->parse_buf= parse_string;
  PARSE_DATA->parse_inx= 0;
  PARSE_DATA->parse_str_len= str_len;

  yyparse(loc_parse_data, loc_parse_data->yyscanner);
  yylex_destroy(loc_parse_data->yyscanner);
}
