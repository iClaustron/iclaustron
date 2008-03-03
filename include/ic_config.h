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

#ifndef IC_CONFIG_H
#define IC_CONFIG_H
#include <ic_common_header.h>
/*
  This struct contains generic operations for reading a configuration file.
  Configuration files can contain:
  1) Sections, e.g. [data server]
  2) Keys, e.g. Key = 1, always a key-value pair
     Arrays are handled by listing the same key with many different values.
     Each value will become an index in an array.
  3) Comments, e.g. #Whatever comment is desirable

  There is also a call before calling the first call to initialise the config
  object. There is also a call at the end indicating that the config file has
  been read to its end.
*/
#define INITIAL_PASS 0
#define BUILD_PASS   1
struct ic_cluster_config_temp;
struct ic_cluster_config_load;
struct ic_config_struct
{
  union {
    struct ic_cluster_config_load *clu_conf;
    struct ic_cluster_config_temp *cluster_conf;
  } config_ptr;
  struct ic_config_operations *clu_conf_ops;
  struct ic_memory_container *perm_mc_ptr;
};
typedef struct ic_config_struct IC_CONFIG_STRUCT;

struct ic_config_operations
{
  int (*ic_config_init)(void *ic_config, guint32 pass);
  int (*ic_add_section)(void *ic_config,
                        guint32 section_number,
                        guint32 line_number,
                        IC_STRING *section_name,
                        guint32 pass);
  int (*ic_add_key)(void *ic_config,
                    guint32 section_number,
                    guint32 line_number,
                    IC_STRING *key_name,
                    IC_STRING *data,
                    guint32 pass);
  int (*ic_add_comment)(void *ic_config,
                        guint32 section_number,
                        guint32 line_number,
                        IC_STRING *comment,
                        guint32 pass);
  int (*ic_config_verify)(void *ic_config);
  void (*ic_init_end)(void *ic_config);
  void (*ic_config_end)(void *ic_config);
};
typedef struct ic_config_operations IC_CONFIG_OPERATIONS;

/*
  This is a generic call to bootstrap a configuration file. It will read the
  configuration data and will call operations on the supplied
  ic_config_operations object.
  It needs a call to read the contents of the configuration file into a
  variable before being called. The contents of the file is sent in the
  conf_data variable.
*/
int ic_build_config_data(IC_STRING *conf_data,
                         IC_CONFIG_STRUCT *ic_conf_obj,
                         IC_CONFIG_ERROR *err_obj);

/*
  Routine to convert configuration value for unsigned integer values 
  Can be of the type:
  true, TRUE => Value = 1
  false, FALSE => Value = 0
  Normal number using 1234567890
  Number followed by k,K,m,M,g,G where k multiplies number by 1024, m
  multiplies by 1024*1024 and g muiltiplies by 1024*1024*1024.
*/
int conv_config_str_to_int(guint64 *value, IC_STRING *ic_str);
#endif
