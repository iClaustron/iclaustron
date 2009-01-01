/* Copyright (C) 2007, 2008 iClaustron AB

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
#include <ic_apic.h>

IC_STRING ic_version_file_string=
{(gchar*)"_version", (guint32)8, (gboolean)TRUE};
IC_STRING ic_config_string=
{(gchar*)"config", (guint32)6, (gboolean)TRUE};
IC_STRING ic_config_ending_string=
{(gchar*)".ini", (guint32)4, (gboolean)TRUE};

/*
  A couple of functions to set references to directories in the iClaustron
  installation.
*/

void
ic_create_config_version_file_name(IC_STRING *file_name,
                                   gchar *buf,
                                   IC_STRING *config_dir)
{
  buf[0]= 0;
  IC_INIT_STRING(file_name, buf, 0, TRUE);
  ic_add_ic_string(file_name, config_dir);
  ic_add_ic_string(file_name, &ic_config_string);
  ic_add_ic_string(file_name, &ic_version_file_string);
  ic_add_ic_string(file_name, &ic_config_ending_string);
}

/*
  Create a file name like $CONFIG_PATH/config.ini for initial version
  and $CONFIG_PATH/config.ini.3 if version number is 3.
*/
void
ic_create_config_file_name(IC_STRING *file_name,
                           gchar *buf,
                           IC_STRING *config_dir,
                           IC_STRING *name,
                           guint32 config_version_number)
{
  gchar int_buf[IC_MAX_INT_STRING];
  IC_STRING ending_string;

  buf[0]= 0;
  IC_INIT_STRING(file_name, buf, 0, TRUE);
  ic_add_ic_string(file_name, config_dir);
  ic_add_ic_string(file_name, name); 
  ic_add_ic_string(file_name, &ic_config_ending_string);
  if (config_version_number)
  {
    ic_set_number_ending_string(int_buf, (guint64)config_version_number);
    IC_INIT_STRING(&ending_string, int_buf, strlen(int_buf), TRUE);
    ic_add_ic_string(file_name, &ending_string);
  }
}

/* Create a string like ".3" if number is 3 */
void
ic_set_number_ending_string(gchar *buf, guint64 number)
{
  gchar *ignore_ptr;
  buf[0]= '.';
  ignore_ptr= ic_guint64_str(number,
                             &buf[1],
                             NULL);
}

static int
add_dir_slash(IC_STRING *dir)
{
#ifdef WINDOWS
  if (ic_add_dup_string(dir, "\\"))
#else
  if (ic_add_dup_string(dir, "/"))
#endif
    return IC_ERROR_MEM_ALLOC;
  return 0;
}

static int
set_default_dir(const gchar *default_dir, IC_STRING *dir,
                const gchar *input_dir)
{
  int error= IC_ERROR_MEM_ALLOC;
  gchar *c_str;

  IC_INIT_STRING(dir, NULL, 0, TRUE);
  if (input_dir == NULL)
  {
    /*
      The user specified no base/data directory himself, in this case we'll
      use $HOME/iclaustron_install as the base directory unless the user is
      the root user, in this case we'll instead use
      /var/lib/iclaustron_install as the default directory. This is also how
      the iClaustron will install the software by default. (iclaustron_data
      for data dir).
    */
    const gchar *user_name= g_get_user_name();
    if (strcmp(user_name, "root") == 0)
    {
#ifdef WINDOWS
      if (ic_add_dup_string(dir, "\\var\\lib\\"))
#else
      if (ic_add_dup_string(dir, "/var/lib/"))
#endif
        goto end;
    }
    else
    {
      const gchar *home_var= g_getenv("HOME");
      if (ic_add_dup_string(dir, home_var))
        goto end;
      if (add_dir_slash(dir))
        goto end;
    }
    if (ic_add_dup_string(dir, default_dir))
      goto end;
    if (add_dir_slash(dir))
      goto end;
  }
  else
  {
    if (ic_add_dup_string(dir, input_dir))
      goto end;
#ifdef WINDOWS
    c_str= "\\";
#else
    c_str= "/";
#endif
    if (dir->str[dir->len - 1] != c_str[0])
    {
      if (ic_add_dup_string(dir, c_str))
        goto end;
    }
  }
  error= 0;
end:
  return error;
}

int
ic_set_data_dir(IC_STRING *data_dir, const gchar *input_data_dir)
{
  return set_default_dir("iclaustron_data", data_dir,
                         input_data_dir);
}

int
ic_set_base_dir(IC_STRING *base_dir, const gchar *input_base_dir)
{
  return set_default_dir("iclaustron_install", base_dir,
                         input_base_dir);
}

void ic_make_iclaustron_version_string(IC_STRING *res_str, gchar *buf)
{
  buf[0]= 0;
  IC_INIT_STRING(res_str, buf, 0, TRUE);
  ic_add_string(res_str, (const gchar *)PACKAGE);
  ic_add_string(res_str, (const gchar *)"-");
  ic_add_string(res_str, (const gchar *)VERSION);
}

void ic_make_mysql_version_string(IC_STRING *res_str, gchar *buf)
{
  buf[0]= 0;
  IC_INIT_STRING(res_str, buf, 0, TRUE);
  ic_add_string(res_str, (const gchar *)MYSQL_VERSION_STRING);
}

void ic_set_relative_dir(IC_STRING *res_str, IC_STRING *dir,
                         gchar *buf, const gchar *dir_name)
{
  IC_INIT_STRING(res_str, buf, 0, TRUE);
  ic_add_ic_string(res_str, dir);
  ic_add_string(res_str, dir_name);
#ifdef WINDOWS
  ic_add_string(res_str, "\\");
#else
  ic_add_string(res_str, "/");
#endif
}

/* The default configuration directory is ICLAUSTRON_DATA_DIR/config */
int
ic_set_config_path(IC_STRING *config_dir,
                   gchar *config_path,
                   gchar *buf)
{
  int error;
  IC_STRING data_dir;
  if ((error= ic_set_data_dir(&data_dir, config_path)))
    return error;
  ic_set_relative_dir(config_dir, &data_dir, buf,
                      ic_config_string.str);
  DEBUG_PRINT(CONFIG_LEVEL, ("Config dir: %s", config_dir->str));
  return 0;
}

/*
  Return highest bit set in a 32-bit integer, bit 0 is reported as 1 and
  no bit set is reported 0, thus we report one more than the bit index
  of the highest bit set
*/

guint32
ic_count_highest_bit(guint32 bit_var)
{
  guint32 i;
  guint32 bit_inx= 0;
  for (i= 0; i < 32; i++)
  {
    if (bit_var | (1 << i))
      bit_inx= i+1;
  }
  return bit_inx;
}

static void
ic_reverse_str(gchar *in_buf, gchar *out_buf, gchar end_char)
{
  guint32 i= 0;
  guint32 j= 0;
  DEBUG_ENTRY("ic_reverse_str");

  while (in_buf[i] != end_char)
    i++;
  out_buf[i]= in_buf[i]; /* Copy end_char byte */
  while (i)
    out_buf[j++]= in_buf[--i];
}

gchar *ic_guint64_str(guint64 val, gchar *ptr, guint32 *len)
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
  ic_reverse_str((gchar*)&buf, ptr, 0);
  if (len)
    *len= i;
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
  ic_reverse_str((gchar*)&buf, ptr, 0);
  return ptr;
}

/*
  MODULE: GENERIC_CONFIG_READER
  This module is a generic reader of configuration files.
  It has one external routine:
  build_config_data
  This routine will call into the supplied ic_config_operations
  methods. Those methods will build the actual data structures
  generated by a specific configuration file.
*/

const guint32 MAX_LINE_LEN = 120;

const gchar *false_str= "false";
const gchar *true_str= "true";

static gboolean
ic_check_digit(gchar c)
{
  if (c < '0' || c > '9')
    return FALSE;
  return TRUE;
}

gchar*
ic_convert_file_to_dir(gchar *buf, gchar *file_name)
{
  gchar *buf_start= buf;
  gchar *last_separator= NULL;
  gchar separator;

#ifndef WINDOWS
  separator= '\\';
#else
  separator= '/';
#endif
  while (*file_name)
  {
    *buf= *file_name;
    if (*buf == separator)
      last_separator= buf+1;
    buf++;
    file_name++;
  }
  if (last_separator)
  {
    /*
      We found a separator, thus filename contains a directory part.
      We insert a 0 after this separator to remove filename
      part from the full pathname.
    */
    *last_separator= 0;
  }
  else
  {
    /*
       We found no separator, thus the directory part is empty and we return
       an empty string.
    */
    *buf_start= 0;
  }
  return buf_start;
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
ic_conv_str_to_int(gchar *str, guint64 *number, guint32 *len)
{
  gchar *rev_str;
  guint32 str_len;
  guint64 base= 1;
  guint64 this_number;
  gchar end_char;
  gchar reverse_str[64];
  DEBUG_ENTRY("ic_conv_str_to_int");
  rev_str= reverse_str;
  *number= 0;

  for (str_len= 0; str_len < 62; str_len++)
  {
    if (!ic_check_digit(str[str_len]))
      break;
  }
  if (str_len > 60 || str_len == 0)
    return 1;
  end_char= str[str_len];
  if (len)
    *len= str_len;
  ic_reverse_str(str, rev_str, end_char);
  while (*rev_str != end_char)
  {
    this_number= (*rev_str - '0');
    *number= *number + (this_number * base);
    base *= 10;
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
    DEBUG_PRINT(CONFIG_LEVEL,
      ("Line number %d in section %d is an empty line", line_number,
       *section_num));
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
/*
  This method is used to build the configuration data structure from reading
  the configuration file. It's designed to be reusable for any type of
  configuration file. The configuration file is always read in two passes,
  the first usually gathers some information about size of various parts.
  
  PARAMETERS:
  conf_data      This parameter contains the configuration file as a long string
  ic_conf_op     This contains the function pointers for this specific config
                 file type
  ic_config      This contains the pointer to the data structure where the
                 configuration data is placed, this is done through a union
                 of pointers to one struct per configuration file type
  err_obj        Any error information is returned in this object
*/

int ic_build_config_data(IC_STRING *conf_data,
                         IC_CONFIG_STRUCT *ic_config,
                         IC_CONFIG_ERROR *err_obj)
{
  guint32 line_number;
  guint32 line_length, pass;
  int error;
  guint32 section_num;
  IC_STRING line_data;
  IC_CONFIG_OPERATIONS *ic_conf_op= ic_config->clu_conf_ops;
  DEBUG_ENTRY("ic_build_config_data");

  if (conf_data->len == 0)
  {
    err_obj->err_num= 1;
    err_obj->line_number= 0;
    return 1;
  }
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
        DEBUG_PRINT(CONFIG_LEVEL, ("Special case"));
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

guint32
ic_str_find_first(IC_STRING *ic_str, gchar searched_char)
{
  guint32 i;
  for (i= 0; i < ic_str->len; i++)
  {
    if (ic_str->str[i] == searched_char)
      return i;
  }
  return ic_str->len;
}

void
ic_add_string(IC_STRING *dest_str, const gchar *input_str)
{
  IC_STRING input_ic_str;

  IC_INIT_STRING(&input_ic_str, (gchar*)input_str, strlen(input_str), TRUE);
  ic_add_ic_string(dest_str, &input_ic_str);
}

int
ic_add_dup_string(IC_STRING *dest_str, const gchar *add_str)
{
  guint32 add_len= strlen(add_str);
  guint32 orig_len= dest_str->len;
  guint32 new_len= add_len + orig_len;
  gchar *new_str= ic_malloc(new_len + 1);
  if (new_str == NULL)
    return IC_ERROR_MEM_ALLOC;
  if (orig_len > 0)
    memcpy(new_str, dest_str->str, orig_len);
  memcpy(new_str + orig_len, add_str, add_len);
  if (dest_str->is_null_terminated)
    new_str[new_len]= 0;
  if (dest_str->str)
    ic_free(dest_str->str);
  dest_str->str= new_str;
  dest_str->len= new_len;
  return 0;
}

void
ic_add_ic_string(IC_STRING *dest_str, IC_STRING *input_str)
{
  gchar *start_ptr= dest_str->str+dest_str->len;
  gchar *end_ptr;
  memcpy(start_ptr, input_str->str, input_str->len);
  if (dest_str->is_null_terminated)
  {
    end_ptr= start_ptr + input_str->len;
    *end_ptr= 0;
  }
  dest_str->len+= input_str->len;
}

gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr)
{
  guint32 i;
  buf_ptr[0]= 0;
  if (str && str->str)
  {
    for (i= 0; i < str->len; i++)
      buf_ptr[i]= str->str[i];
    buf_ptr[str->len]= 0;
  }
  return buf_ptr;
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

int ic_mc_strdup(IC_MEMORY_CONTAINER *mc_ptr, IC_STRING *out_str,
                 IC_STRING *in_str)
{
  gchar *str;
  IC_INIT_STRING(out_str, NULL, 0, FALSE);
  if (in_str->len == 0)
    return 0;
  if (mc_ptr)
  {
    if (!(str= mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, in_str->len+1)))
      return 1;
  }
  else
  {
    if (!(str= ic_malloc(in_str->len+1)))
      return 1;
  }
  IC_COPY_STRING(out_str, in_str);
  out_str->str= str;
  memcpy(out_str->str, in_str->str, in_str->len);
  if (out_str->is_null_terminated)
    out_str->str[out_str->len]= NULL_BYTE;
  return 0;
}

int
ic_strdup(IC_STRING *out_str, IC_STRING *in_str)
{
  return ic_mc_strdup(NULL, out_str, in_str);
}

/*
  MODULE: iClaustron Error Handling
  ---------------------------------
  Module for printing error codes and retrieving error messages
  given the error number.

  To add a new error number do the following:
  1) Change IC_LAST_ERROR
  2) Add a new entry in ic_init_error_messages
  3) Add the new error code in ic_err.h
*/

#define IC_FIRST_ERROR 7000
#define IC_LAST_ERROR 7028
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
  ic_error_str[IC_ERROR_NODE_DOWN - IC_FIRST_ERROR]=
    "Node failure occurred";
  ic_error_str[IC_ERROR_NO_SUCH_CLUSTER - IC_FIRST_ERROR]=
    "No such cluster";
  ic_error_str[IC_ERROR_NODE_DOWN - IC_FIRST_ERROR]=
    "No such node exists in this cluster";
  ic_error_str[IC_ERROR_MESSAGE_CHECKSUM - IC_FIRST_ERROR]=
    "Message received with wrong checksum";
  ic_error_str[IC_ERROR_ACCEPT_TIMEOUT - IC_FIRST_ERROR]=
    "Timeout when waiting for connection to accept";
  ic_error_str[IC_ERROR_POLL_SET_FULL - IC_FIRST_ERROR]=
    "Poll set is full, need to use another poll set";
  ic_error_str[IC_ERROR_NOT_FOUND_IN_POLL_SET - IC_FIRST_ERROR]=
    "The file descriptor wasn't found in this poll set";
  ic_error_str[IC_ERROR_NODE_ALREADY_DEFINED - IC_FIRST_ERROR]=
    "Can't use the same node id twice in a cluster";
}

void
ic_print_error(int error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    printf("%d: %s\n", error_number, no_such_error_str);
  else
    printf("%s\n", ic_error_str[error_number - IC_FIRST_ERROR]);
}

gchar *ic_get_error_message(int error_number)
{
  if (error_number < IC_FIRST_ERROR ||
      error_number > IC_LAST_ERROR ||
      !ic_error_str[error_number - IC_FIRST_ERROR])
    return no_such_error_str;
  else
    return ic_error_str[error_number - IC_FIRST_ERROR];
}

gchar*
ic_common_fill_error_buffer(const gchar *extra_error_message,
                            guint32 error_line,
                            int error_code,
                            gchar *error_buffer)
{
  guint32 err_msg_len, err_str_len, err_buf_index;
  guint32 protocol_err_str_len, line_buf_len;
  gchar *protocol_err_msg= "Protocol error on line: ";
  gchar *err_msg, *line_buf_ptr;
  gchar line_buf[128];

  err_msg= ic_get_error_message(error_code);
  err_msg_len= strlen(err_msg);
  memcpy(error_buffer, err_msg, err_msg_len);
  error_buffer[err_msg_len]= CARRIAGE_RETURN;
  err_buf_index= err_msg_len;
  if (extra_error_message)
  {
    err_str_len= strlen(extra_error_message);
    memcpy(&error_buffer[err_buf_index + 1], extra_error_message,
           err_str_len);
    err_buf_index= err_buf_index + 1 + err_str_len;
    error_buffer[err_buf_index]= CARRIAGE_RETURN;
  }
  if (error_code == PROTOCOL_ERROR)
  {
    protocol_err_str_len= strlen(protocol_err_msg);
    memcpy(&error_buffer[err_buf_index + 1], protocol_err_msg,
           protocol_err_str_len);
    err_buf_index= err_buf_index + 1 + protocol_err_str_len;
    line_buf_ptr= ic_guint64_str((guint64)error_line, line_buf,
                                 &line_buf_len);
    if (line_buf_ptr)
    {
      memcpy(&error_buffer[err_buf_index], line_buf_ptr,
             line_buf_len);
      err_buf_index= err_buf_index + line_buf_len;
    }
    error_buffer[err_buf_index]= CARRIAGE_RETURN;
  }
  error_buffer[err_buf_index + 1]= 0; /* Null-terminated string */
  return error_buffer;  
}

static guint32 glob_debug= 0;
static gchar *glob_debug_file= "debug.log";
static guint32 glob_debug_screen= 0;

static GOptionEntry debug_entries[] = 
{
  { "debug_level", 0, 0, G_OPTION_ARG_INT, &glob_debug,
    "Set Debug Level", NULL},
  { "debug_file", 0, 0, G_OPTION_ARG_STRING, &glob_debug_file,
    "Set Debug File", NULL},
  { "debug_screen", 0, 0, G_OPTION_ARG_INT, &glob_debug_screen,
    "Flag whether to print debug info to stdout", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static gchar *help_debug= "Group of flags to set-up debugging";
static gchar *debug_description= "\
Group of flags to set-up debugging level and where to pipe debug output \n\
Debug level is actually several levels, one can decide to debug a certain\n\
area at a time. Each area has a bit in debug level. So if one wants to\n\
debug the communication area one sets debug_level to 1 (set bit 0). By\n\
setting several bits it is possible to debug several areas at once.\n\
\n\
Current levels defined: \n\
  Level 0 (= 1): Communication debugging\n\
  Level 1 (= 2): Entry into functions debugging\n\
  Level 2 (= 4): Configuration debugging\n\
  Level 3 (= 8): Debugging specific to the currently executing program\n\
  Level 4 (=16): Debugging of threads\n\
";

/*
  MODULE: iClaustron Initialise and End Functions
  -----------------------------------------------
  Every iClaustron program should start by calling ic_start_program
  before using any of the iClaustron functionality and after
  completing using the iClaustron API one should call ic_end().

  ic_start_program will define automatically a set of debug options
  for the program which are common to all iClaustron programs. The
  program can also supply a set of options unique to this particular
  program. In addition a start text should be provided.

  So for most iClaustron programs this means calling these functions
  at the start of the main function and at the end of the main
  function.
*/
static int ic_init();

int
ic_start_program(int argc, gchar *argv[], GOptionEntry entries[],
                 gchar *start_text)
{
  int ret_code= 1;
  GError *error= NULL;
  GOptionGroup *debug_group;
  GOptionContext *context;

  context= g_option_context_new(start_text);
  if (!context)
    goto mem_error;
  /* Read command options */
  g_option_context_add_main_entries(context, entries, NULL);
  debug_group= g_option_group_new("debug", debug_description,
                                  help_debug, NULL, NULL);
  if (!debug_group)
    goto mem_error;
  g_option_group_add_entries(debug_group, debug_entries);
  g_option_context_add_group(context, debug_group);
  if (!g_option_context_parse(context, &argc, &argv, &error))
    goto parse_error;
  g_option_context_free(context);
  if ((ret_code= ic_init()))
    return ret_code;
  return 0;

parse_error:
  printf("No such program option: %s\n", error->message);
  goto error;
mem_error:
  printf("Memory allocation error\n");
  goto error;
error:
  return ret_code;
}

static int
ic_init()
{
  int ret_value;
  DEBUG_OPEN;
  DEBUG_ENTRY("ic_init");
  if (!g_thread_supported())
    g_thread_init(NULL);
  ic_init_error_messages();
  if ((ret_value= ic_init_config_parameters()))
  {
    ic_end();
    DEBUG_RETURN(ret_value);
  }
  if ((ret_value= ic_ssl_init()))
  {
    ic_end();
    DEBUG_RETURN(ret_value);
  }
  DEBUG_RETURN(0);
}

void ic_end()
{
  DEBUG_ENTRY("ic_end");
  if (glob_conf_hash)
    ic_hashtable_destroy(glob_conf_hash);
  glob_conf_entry_inited= FALSE;
  ic_ssl_end();
  DEBUG_CLOSE;
}
