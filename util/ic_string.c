/* Copyright (C) 2007-2009 iClaustron AB

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
  MODULE:
  iClaustron STRING ROUTINES
  --------------------------
  Some routines to handle iClaustron strings
*/

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

void ic_reverse_str(gchar *in_buf, gchar *out_buf, gchar end_char)
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

guint32
ic_count_characters(gchar *str, guint32 max_chars)
{
  gchar *ptr= str;
  guint32 num_chars= 0;

  while ((num_chars < max_chars) &&
         (!(*ptr == ' ' || *ptr == '\n' || *ptr == 0)))
  {
    num_chars++;
    ptr++;
  }
  return num_chars;
}

gboolean
ic_convert_str_to_int_fixed_size(gchar *str, guint32 num_chars,
                                 guint64 *ret_number)
{
  char *end_ptr;
  if (num_chars == 0)
    return TRUE;
  *ret_number= g_ascii_strtoull(str, &end_ptr, (guint)10);
  if (end_ptr == (str + num_chars))
  {
    if (*ret_number || (num_chars == 1 && (*str == '0')))
      return FALSE;
  }
  return TRUE;
}
