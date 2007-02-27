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

struct ic_cluster_config_load;
struct ic_config_struct
{
  union {
    struct ic_cluster_config_load *clu_conf;
  } config_ptr;
};
typedef struct ic_config_struct IC_CONFIG_STRUCT;


struct ic_config_operations
{
  int (*ic_config_init)(IC_CONFIG_STRUCT *ic_config);
  int (*ic_add_section)(IC_CONFIG_STRUCT *ic_config,
                        guint32 section_number,
                        guint32 line_number,
                        IC_STRING *section_name,
                        guint32 pass);
  int (*ic_add_key)(IC_CONFIG_STRUCT *ic_config,
                    guint32 section_number,
                    guint32 line_number,
                    IC_STRING *key_name,
                    IC_STRING *data,
                    guint32 pass);
  int (*ic_add_comment)(IC_CONFIG_STRUCT *ic_config,
                        guint32 line_number,
                        guint32 section_number,
                        IC_STRING *comment,
                        guint32 pass);
  void (*ic_config_end)(IC_CONFIG_STRUCT *ic_config);
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
                         IC_CONFIG_OPERATIONS *ic_conf_op,
                         IC_CONFIG_STRUCT *ic_conf_obj,
                         IC_CONFIG_ERROR *err_obj);

#endif
