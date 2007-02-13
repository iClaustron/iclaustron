#ifndef IC_ERR_H
#define IC_ERR_H

struct ic_config_err
{
  guint32 err_num;
  guint32 line_number;
};
typedef struct ic_config_err IC_CONFIG_ERROR;

#define IC_ERROR_CONFIG_LINE_TOO_LONG 7000
#define IC_ERROR_CONFIG_BRACKET 7001
#define IC_ERROR_CONFIG_INCORRECT_GROUP_ID 7002
#define IC_ERROR_CONFIG_IMPROPER_KEY_VALUE 7003
#define IC_ERROR_CONFIG_NO_SUCH_SECTION 7004
#define IC_ERROR_MEM_ALLOC 7005
#endif
