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

#include <ic_common.h>
/*
  MODULE: GENERIC_CONFIG_READER
  This module is a generic reader of configuration files.
  It has one external routine:
  build_config_data
  This routine will call into the supplied ic_config_operations
  methods. Those methods will build the actual data structures
  generated by a specific configuration file.
*/

static void
ic_reverse_str(gchar *in_buf, gchar *out_buf)
{
  guint32 i= 0;
  guint32 j= 0;
  DEBUG_ENTRY("ic_reverse_str");

  while (in_buf[i])
    i++;
  out_buf[i]= in_buf[i]; /* Copy NULL byte */
  while (i)
    out_buf[j++]= in_buf[--i];
}

gchar *ic_guint64_str(guint64 val, gchar *ptr)
{
  guint32 i= 0;
  gchar buf[128];
  if (val == 0)
  {
    buf[0]= '0';
    i++;
  }
  while (val != 0)
  {
    guint tmp= val/10;
    tmp*= 10;
    tmp= val - tmp;
    buf[i]= (((gchar)'0') + (gchar)tmp);
    val/= 10;
    i++;
  }
  buf[i]= 0;
  ic_reverse_str((gchar*)&buf, ptr);
  return ptr;
}

gchar *ic_guint64_hex_str(guint64 val, gchar *ptr)
{
  guint32 i= 0;
  gchar buf[128];
  if (val == 0)
  {
    buf[0]= '0';
    i++;
  }
  while (val != 0)
  {
    guint tmp= val/16;
    tmp*= 16;
    tmp= val - tmp;
    if (tmp < 10)
      buf[i]= (((gchar)'0') + (gchar)tmp);
    else
      buf[i]= (((gchar)'A') + (gchar)(tmp-10));
    val/= 16;
    i++;
  }
  buf[i]= 0;
  ic_reverse_str((gchar*)&buf, ptr);
  return ptr;
}

#ifdef DEBUG_BUILD
static FILE *ic_fptr;
void
ic_debug_entry(const char *entry_point)
{
  printf("Entry into: %s\n", entry_point);
  if (fprintf(ic_fptr, "Entry into: %s\n", entry_point) <= 0)
    printf("error %d\n", errno);
}

int ic_debug_open()
{
  ic_fptr= fopen("debug.log", "w");
  if (ic_fptr == NULL)
  {
    printf("Failed to open debug.log");
    return 1;
  }
  if (fprintf(ic_fptr, "Entry into: \n") <= 0)
    printf("error %d\n", errno);
  return 0;
}

void
ic_debug_close()
{
  fclose(ic_fptr);
}
#endif

const guint32 MAX_LINE_LEN = 120;

const gchar *false_str= "false";
const gchar *true_str= "true";

static 
gboolean ic_check_digit(gchar c)
{
  if (c < '0' || c > '9')
    return FALSE;
  return TRUE;
}

int
ic_conv_config_str_to_int(guint64 *value, IC_STRING *ic_str)
{
  guint32 i;
  guint64 number= 0;
  gboolean no_digit_found= FALSE;

  if (!ic_cmp_null_term_str(false_str, ic_str))
    return 0;
  else if (!ic_cmp_null_term_str(true_str, ic_str))
  {
    number= 1;
    return 0;
  }
  for (i= 0; i < ic_str->len; i++)
  {
    if (no_digit_found)
      return 1;
    if (ic_check_digit(ic_str->str[i]))
    {
      number*= 10;
      number+= (ic_str->str[i] - '0');
    }
    else
    {
      no_digit_found= TRUE;
      if ((ic_str->str[i] == 'k') ||
          (ic_str->str[i] == 'K'))
        number*= 1024;
      else if ((ic_str->str[i] == 'm') ||
               (ic_str->str[i] == 'M'))
        number*= (1024*1024);
      else if ((ic_str->str[i] == 'g') ||
               (ic_str->str[i] == 'G'))
        number*= (1024*1024*1024);
      else
        return 1;
    }
  }
  *value= number;
  return 0;
}

int
ic_conv_str_to_int(gchar *str, guint64 *number)
{
  gchar reverse_str[64];
  gchar *rev_str= reverse_str;
  *number= 0;
  int str_len= strlen(str);
  DEBUG_ENTRY("ic_conv_str_to_int");

  if (str_len > 60 || str_len == 0)
    return 1;
  ic_reverse_str(str, rev_str);
  while (*rev_str)
  {
    if (*rev_str < '0' || *rev_str > '9')
      return 1;
    *number= (*number * 10) + (*rev_str - '0');
    rev_str++;
  }
  return 0;
}

static
guint32 read_cr_line(gchar *iter_data)
{
  gsize len= 1;
  do
  {
    gchar iter_char= *iter_data;
    if (iter_char == CARRIAGE_RETURN ||
        iter_char == 0)
      return len;
    len++;
    iter_data++;
  } while (len < MAX_LINE_LEN);
  return 0;
}

static
gchar *conv_group_id(gchar *group_id, guint32 len)
{
  guint32 iter_len= 0;
  gchar *save_group_id= group_id;
  while (iter_len < len)
  {
    gchar c= *group_id;
    if (g_ascii_isalpha(c))
    {
      c= g_ascii_tolower(c);
      *group_id= c;
    }
    else if (c != ' ')
      return NULL;
    group_id++;
    iter_len++;
  }
  return save_group_id;    
}

static
gchar *conv_key_id(gchar *key_id, guint32 *len)
{
  gchar *save_key_id= key_id;
  guint32 iter_len= 0;
  while (iter_len < *len)
  {
    gchar c= *key_id;
    if (!g_ascii_isalpha(c) && c != '_')
    {
      *len= iter_len;
      return save_key_id;
    }
    *key_id= g_ascii_tolower(c);
    iter_len++;
    key_id++;
  }
  return NULL;
}

static
gchar *rm_space(gchar *val_str, guint32 *iter_len, guint32 len)
{
  for ( ; 
       (g_ascii_isspace(val_str[0]) ||
        val_str[0] == '\t')
       && *iter_len < len;
       val_str++, (*iter_len)++)
    ;
  return val_str;
}

static
gchar *conv_key_value(gchar *val_str, guint32 *len)
{
  gboolean found= FALSE;
  guint32 iter_len= 0;
  gchar *save_val_str;
  gchar *first_val_str= val_str;

  val_str= rm_space(val_str, &iter_len, *len);
  if (*len == iter_len || (val_str[0] != '=' && val_str[0] != ':'))
    return NULL;
  val_str++;
  iter_len++;
  val_str= rm_space(val_str, &iter_len, *len);
  save_val_str= val_str;
  while (iter_len < *len)
  {
    gchar c= *val_str;
    if (!(g_ascii_isalpha(c) || c == '/' || c == '\\' ||
        g_ascii_isdigit(c) || '.' ||
        c == '_' || g_ascii_isspace(c)))
      return NULL;
    val_str++;
    iter_len++;
    found= TRUE;
  }
  if (!found)
    return NULL;
  *len= (guint32)(((first_val_str + *len) - save_val_str) - 1);
  return save_val_str;
}

static
guint32 read_config_line(IC_CONFIG_OPERATIONS *conf_ops,
                         IC_CONFIG_STRUCT *conf_obj,
                         IC_STRING *line_data,
                         guint32 line_number,
                         guint32 *section_num,
                         guint32 pass)
{
  gchar *iter_data= line_data->str;
  guint32 line_len= line_data->len;
  DEBUG_ENTRY("read_config_line");

  if (line_len == 1)
  {
    printf("Line number %d in section %d is an empty line\n", line_number,
                                                             *section_num);
    return 0;
  }
  else if (*iter_data == '#')
  {
    /*
      We have found a correct comment line, report it to the configuration
      object.
    */
    IC_STRING comment_str;
    IC_INIT_STRING(&comment_str, iter_data, line_len, FALSE);
    return conf_ops->ic_add_comment(conf_obj, line_number, *section_num,
                                    &comment_str, pass);
  }
  else if (*iter_data == '[')
  {
    if (iter_data[line_len - 2] != ']')
      return IC_ERROR_CONFIG_BRACKET;
    else
    {
      IC_STRING section_name;
      guint32 group_id_len= line_len - 3;
      gchar *group_id= conv_group_id(&iter_data[1], group_id_len);
      if (!group_id)
        return IC_ERROR_CONFIG_INCORRECT_GROUP_ID;
      /*
        We have found a correct group id, now tell the configuration
        object about this and let him decide if this group id is
        correct according to the definition of this configuration
        object.
      */
      IC_INIT_STRING(&section_name, group_id, group_id_len, FALSE);
      (*section_num)++;
      return conf_ops->ic_add_section(conf_obj, *section_num, line_number,
                                      &section_name, pass);
    }
  }
  else
  {
    IC_STRING key_name, data_str;
    guint32 id_len= line_len;
    guint32 val_len= 0;
    guint32 space_len= 0;
    gchar *key_id= NULL;
    gchar *val_str= NULL;

    rm_space(iter_data, &space_len, line_len);
    id_len-= space_len;
    if (!(key_id= conv_key_id(&iter_data[space_len], &id_len)) ||
        ((val_len= line_len - (id_len + space_len)), FALSE) ||
        (!(val_str= conv_key_value(&iter_data[id_len+space_len], &val_len))))
      return IC_ERROR_CONFIG_IMPROPER_KEY_VALUE;
    /*
      We have found a correct key-value pair. Now let the configuration object
      decide whether this key and value makes sense and if so insert it into
      the configuration.
    */
    IC_INIT_STRING(&key_name, key_id, id_len, FALSE);
    IC_INIT_STRING(&data_str, val_str, val_len, FALSE);
    return conf_ops->ic_add_key(conf_obj, *section_num, line_number,
                                &key_name, &data_str, pass);
  }
}

int ic_build_config_data(IC_STRING *conf_data,
                         IC_CONFIG_OPERATIONS *ic_conf_op,
                         IC_CONFIG_STRUCT *ic_config,
                         IC_CONFIG_ERROR *err_obj)
{
  guint32 line_number;
  guint32 line_length, pass;
  int error;
  guint32 section_num;
  IC_STRING line_data;
  DEBUG_ENTRY("ic_build_config_data");

  for (pass= 0; pass < 2; pass++)
  {
    gchar *iter_data= conf_data->str;
    gsize iter_data_len= 0;
    line_number= 1;
    section_num= 0;
    if ((error= ic_conf_op->ic_config_init(ic_config, pass)))
      goto config_error;
    while (iter_data_len < conf_data->len)
    {
      if (*iter_data == LINE_FEED)
      {
        /* Special handling of Windows Line Feeds after Carriage Return */
        DEBUG(printf("Special case\n"));
        iter_data_len++;
        iter_data++;
        continue;
      }
      error= IC_ERROR_CONFIG_LINE_TOO_LONG;
      if (!(line_length= read_cr_line(iter_data)))
        goto config_error;
      IC_INIT_STRING(&line_data, iter_data, line_length, FALSE);
      if ((error= read_config_line(ic_conf_op, ic_config,
                                   &line_data, line_number, &section_num,
                                   pass)))
        goto config_error;
      iter_data+= line_length;
      iter_data_len+= line_length;
      line_number++;
    }
  }
  if ((error= ic_conf_op->ic_config_verify(ic_config)))
    goto config_error;
  return 0;
config_error:
  ic_conf_op->ic_config_end(ic_config);
  err_obj->err_num= error;
  err_obj->line_number= line_number;
  return 1;
}

/*
  MODULE:
  iClaustron STRING ROUTINES
  --------------------------
  Some routines to handle iClaustron strings
*/

gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr)
{
  guint32 i;
  if (str && str->str)
  {
    for (i= 0; i < str->len; i++)
      buf_ptr[i]= str->str[i];
    return buf_ptr;
  }
  return NULL;
}

void ic_print_ic_string(IC_STRING *str)
{
  if (str->is_null_terminated)
    printf("%s\n", str->str);
  else
  {
    guint32 str_len= 0;
    while (str_len < str->len)
    {
      putchar(str->str[str_len]);
      str_len++;
    }
    putchar('\n');
  }
}

int ic_cmp_null_term_str(const gchar *null_term_str, IC_STRING *cmp_str)
{
  guint32 iter_len= 0;
  gchar *cmp_char= cmp_str->str;
  guint32 str_len= cmp_str->len;
  if (cmp_str->is_null_terminated)
    return (strcmp(null_term_str, cmp_str->str) == 0) ? 1 : 0;
  while (iter_len < str_len)
  {
    if (*null_term_str != *cmp_char)
      return 1;
    null_term_str++;
    cmp_char++;
    iter_len++;
  }
  if (*null_term_str == 0)
    return 0;
  return 1;
}

void
ic_set_up_ic_string(IC_STRING *in_out_str)
{
  guint32 i;
  if (!in_out_str->str)
  {
    in_out_str->len= 0;
    in_out_str->is_null_terminated= FALSE;
    return;
  }
  for (i= 0; i < in_out_str->len; i++)
  {
    if (in_out_str->str[i])
      continue;
    else
    {
      in_out_str->len= i;
      in_out_str->is_null_terminated= TRUE;
      return;
    }
  }
  in_out_str->is_null_terminated= FALSE;
}

int
ic_strdup(IC_STRING *out_str, IC_STRING *in_str)
{
  gchar *str;
  IC_INIT_STRING(out_str, NULL, 0, FALSE);
  if (in_str->len == 0)
    return 0;
  if (!(str= ic_malloc(in_str->len+1)))
    return 1;
  IC_COPY_STRING(out_str, in_str);
  out_str->str= str;
  memcpy(out_str->str, in_str->str, in_str->len);
  if (out_str->is_null_terminated)
    out_str->str[out_str->len]= NULL_BYTE;
  return 0;
}


/*
  Module for printing error codes and retrieving error messages
  given the error number.
  To add a new error number do the following:
  1) Change IC_LAST_ERROR
  2) Add a new entry in ic_init_error_messages
  3) Add the new error code in ic_err.h
*/

#define IC_FIRST_ERROR 7000
#define IC_LAST_ERROR 7020
#define IC_MAX_ERRORS 100
static gchar* ic_error_str[IC_MAX_ERRORS];
static gchar *no_such_error_str= "No such error";
void
ic_init_error_messages()
{
  guint32 i;
  DEBUG_ENTRY("ic_init_error_messages");
  for (i= 0; i < IC_MAX_ERRORS; i++)
    ic_error_str[i]= NULL;
  ic_error_str[IC_ERROR_CONFIG_LINE_TOO_LONG -IC_FIRST_ERROR]=
    "Line was longer than 120 characters";
  ic_error_str[IC_ERROR_CONFIG_BRACKET - IC_FIRST_ERROR]=
    "Missing ] after initial [";
  ic_error_str[IC_ERROR_CONFIG_INCORRECT_GROUP_ID - IC_FIRST_ERROR]=
    "Found incorrect group id";
  ic_error_str[IC_ERROR_CONFIG_IMPROPER_KEY_VALUE - IC_FIRST_ERROR]=
    "Improper key-value pair";
  ic_error_str[IC_ERROR_CONFIG_NO_SUCH_SECTION - IC_FIRST_ERROR]=
    "Section name doesn't exist in this configuration";
  ic_error_str[IC_ERROR_MEM_ALLOC - IC_FIRST_ERROR]=
    "Memory allocation failure";
  ic_error_str[IC_ERROR_NO_SECTION_DEFINED_YET - IC_FIRST_ERROR]=
    "Tried to define key value before first section defined";
  ic_error_str[IC_ERROR_NO_SUCH_CONFIG_KEY - IC_FIRST_ERROR]=
    "No such configuration key exists";
  ic_error_str[IC_ERROR_DEFAULT_VALUE_FOR_MANDATORY - IC_FIRST_ERROR]=
    "Trying to assign default value to a mandatory config entry";
  ic_error_str[IC_ERROR_CORRECT_CONFIG_IN_WRONG_SECTION - IC_FIRST_ERROR]=
    "Assigning correct config entry in wrong section";
  ic_error_str[IC_ERROR_NO_NODES_FOUND - IC_FIRST_ERROR]=
    "No nodes found in the configuration file";
  ic_error_str[IC_ERROR_WRONG_CONFIG_NUMBER - IC_FIRST_ERROR]=
    "Number expected in config file, true, false and endings with k, m, g also allowed";
  ic_error_str[IC_ERROR_NO_BOOLEAN_VALUE - IC_FIRST_ERROR]=
    "Boolean value expected, got number larger than 1";
  ic_error_str[IC_ERROR_CONFIG_VALUE_OUT_OF_BOUNDS - IC_FIRST_ERROR]=
    "Configuration value is out of bounds, check data type and min, max values";
  ic_error_str[IC_ERROR_NO_SERVER_NAME - IC_FIRST_ERROR]=
    "Server name must be provided in all connections";
  ic_error_str[IC_ERROR_NO_SERVER_PORT - IC_FIRST_ERROR]=
    "Server port must be provided in all connections";
  ic_error_str[IC_ERROR_GETADDRINFO - IC_FIRST_ERROR]=
    "Provided client/server name/port not found by getaddrinfo";
  ic_error_str[IC_ERROR_ILLEGAL_SERVER_PORT - IC_FIRST_ERROR]=
    "Provided server port isn't a legal port number";
  ic_error_str[IC_ERROR_DIFFERENT_IP_VERSIONS - IC_FIRST_ERROR]=
    "Trying to use IPv4 and IPv6 simultaneously on server/client part not supported";
  ic_error_str[IC_ERROR_ILLEGAL_CLIENT_PORT - IC_FIRST_ERROR]=
    "Provided client port isn't a legal port number";
  ic_error_str[IC_ERROR_INCONSISTENT_DATA - IC_FIRST_ERROR]=
    "Internal data structure error";
}

void
ic_print_error(guint32 error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    printf("%s\n", no_such_error_str);
  else
    printf("%s\n", ic_error_str[error_number - IC_FIRST_ERROR]);
}

gchar *ic_get_error_message(guint32 error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    return no_such_error_str;
  else
    return ic_error_str[error_number - IC_FIRST_ERROR];
}

int
ic_start_program(int argc, gchar *argv[], GOptionEntry entries[],
                 gchar *start_text)
{
  int ret_code= 1;
  GError *error= NULL;
  GOptionContext *context;

  context= g_option_context_new(start_text);
  if (!context)
    goto error;
  /* Read command options */
  g_option_context_add_main_entries(context, entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);
  if ((ret_code= ic_init()))
    return ret_code;
  return 0;

parse_error:
  printf("No such program option\n");
error:
  return ret_code;
}
  
