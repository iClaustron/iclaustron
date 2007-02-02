#ifndef IC_CONFIG_H
#define IC_CONFIG_H
#include <ic_common.h>
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
struct ic_config_struct
{
  int a;
};

struct ic_config_operations
{
  int (*ic_config_init)(struct ic_config_struct *ic_config);
  int (*ic_add_section)(struct ic_config_struct *ic_config,
                        guint32 section_number, guint32 line_number,
                        IC_STRING *section_name);
  int (*ic_add_key)(struct ic_config_struct *ic_config,
                    guint32 section_number, guint32 line_number,
                    IC_STRING *key_name, IC_STRING *data);
  int (*ic_add_comment)(struct ic_config_struct *ic_config,
                        guint32 line_number, IC_STRING *comment);
  int (*ic_config_end)(struct ic_config_struct *ic_config,
                       guint32 line_number);
};

/*
  This is a generic call to bootstrap a configuration file. It will read the
  configuration data and will call operations on the supplied
  ic_config_operations object.
  It needs a call to read the contents of the configuration file into a
  variable before being called.
*/
int ic_build_config_data(IC_STRING *conf_data,
                         struct ic_config_operations ic_conf_op);

#endif
