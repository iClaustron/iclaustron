/* Copyright (C) 2007-2013 iClaustron AB

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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ctype.h>

gchar *ic_empty_string= "";
gchar *ic_semi_colon_str= ";";
gchar *ic_space_str= " ";
gchar *ic_colon_str= ":";
gchar *ic_err_str= "Error:";

IC_STRING ic_glob_config_dir= { NULL, 0, TRUE};
IC_STRING ic_glob_data_dir= { NULL, 0, TRUE};
IC_STRING ic_glob_base_dir= { NULL, 0, TRUE};
IC_STRING ic_glob_binary_dir= { NULL, 0, TRUE};
gchar *ic_glob_process_name= NULL;

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
IC_STRING ic_grid_common_config_string=
{(gchar*)"grid_common", (guint32)11, (gboolean)TRUE};
IC_STRING ic_config_ending_string=
{(gchar*)".ini", (guint32)4, (gboolean)TRUE};
IC_STRING ic_binary_string=
{(gchar*)"bin", (guint32)3, (gboolean)TRUE};

/**
  A couple of functions to set references to directories in the iClaustron
  installation.
*/

/**
  Create a string with the file name:
  CONFIG_DIR/config_version.ini

  @parameter file_name         OUT: String placeholder to place created name
  @parameter buf               IN:  Buffer used by file_name string
  @parameter config_dir        IN:  Config directory
*/
void
ic_create_config_version_file_name(IC_STRING *file_name,
                                   gchar *buf,
                                   IC_STRING *config_dir)
{
  DEBUG_ENTRY("ic_create_config_version_file_name");
  buf[0]= 0;
  IC_INIT_STRING(file_name, buf, 0, TRUE);
  ic_add_ic_string(file_name, config_dir);
  ic_add_ic_string(file_name, &ic_config_string);
  ic_add_ic_string(file_name, &ic_version_file_string);
  ic_add_ic_string(file_name, &ic_config_ending_string);
  DEBUG_RETURN_EMPTY;
}

/**
  Create a file name like $CONFIG_PATH/config.ini for initial version
  and $CONFIG_PATH/config.ini.3 if version number is 3.

  @parameter file_name         OUT: String placeholder to place created name
  @parameter buf               IN:  Buffer used by file_name string
  @parameter config_dir        IN:  Config directory
  @parameter name              IN:  Name of config file
                                    config, grid_common or cluster name
  @parameter config_version_number IN: Version number of config
*/
void
ic_create_config_file_name(IC_STRING *file_name,
                           gchar *buf,
                           IC_STRING *config_dir,
                           IC_STRING *name,
                           IC_CONF_VERSION_TYPE config_version_number)
{
  gchar int_buf[IC_MAX_INT_STRING];
  IC_STRING ending_string;
  DEBUG_ENTRY("ic_create_config_file_name");

  buf[0]= 0;
  IC_INIT_STRING(file_name, buf, 0, TRUE);
  ic_add_ic_string(file_name, config_dir);
  ic_add_ic_string(file_name, name); 
  ic_add_ic_string(file_name, &ic_config_ending_string);
  if (config_version_number)
  {
    ic_set_number_ending_string(int_buf, config_version_number);
    IC_INIT_STRING(&ending_string, int_buf, strlen(int_buf), TRUE);
    ic_add_ic_string(file_name, &ending_string);
  }
  DEBUG_RETURN_EMPTY;
}

/**
  Create a string like ".3" if number is 3

  @parameter buf             IN/OUT: Buffer to place the resulting string
  @parameter number          IN:     Number to stringify
*/
void
ic_set_number_ending_string(gchar *buf, guint64 number)
{
  buf[0]= '.';
  ic_guint64_str(number,
                 &buf[1],
                 NULL);
}

/**
  Add a directory separator in a portable manner, / on non-Windows
  and \ on Windows.

  @parameter dir             IN/OUT: String which is updated
*/
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

/**
  Create a string containing a reference to the default directory, this
  could either be the installation directory of binaries and libraries
  or it could be the data directory.

  @parameter default_dir          IN:  iclaustron_install/iclaustron_data
  @parameter dir                  OUT: String output
*/
static int
set_default_dir(const gchar *default_dir,
                IC_STRING *dir)
{
  int error= IC_ERROR_MEM_ALLOC;
  DEBUG_ENTRY("set_default_dir");

  IC_INIT_STRING(dir, NULL, 0, TRUE);
  /*
    The user specified no base/data directory himself, in this case we'll
    use $HOME/iclaustron_install as the base directory unless the user is
    the root user, in this case we'll instead use
    /var/lib/iclaustron_install as the default directory. This is also how
    the iClaustron will install the software by default. (iclaustron_data
    for data dir).
  */
  const gchar *user_name= g_get_user_name();
#ifndef WINDOWS
  if (strcmp(user_name, "root") == 0)
  {
    if (ic_add_dup_string(dir, "/var/lib/"))
#else
  if f(strcmp(user_name, "root") == 0)
  {
    if (ic_add_dup_string(dir, "C:\\Program Files\\lib\\"))
#endif
      goto error;
  }
  else
  {
    const gchar *home_var= g_getenv("HOME");
    if (ic_add_dup_string(dir, home_var) ||
        add_dir_slash(dir))
      goto error;
  }
  if (ic_add_dup_string(dir, default_dir) ||
      add_dir_slash(dir))
    goto error;
  DEBUG_RETURN_INT(0);

error:
  if (dir->str)
  {
    ic_free(dir->str);
    IC_INIT_STRING(dir, NULL, 0, TRUE);
  }
  DEBUG_RETURN_INT(error);
}

/**
  Create a string referring to the iClaustron data directory

  @parameter data_dir            OUT: String referring to data directory
*/
int
ic_set_data_dir(IC_STRING *data_dir)
{
  int error;
  DEBUG_ENTRY("ic_set_data_dir");

  error= set_default_dir("iclaustron_data", data_dir);
  if (!error)
    DEBUG_PRINT(PROGRAM_LEVEL, ("Data dir: %s", data_dir->str));
  DEBUG_RETURN_INT(error);
}

/**
  Create a string referring to the iClaustron base directory

  @parameter base_dir            OUT: String referring to base directory
*/
int
ic_set_base_dir(IC_STRING *base_dir)
{
  int error;
  DEBUG_ENTRY("ic_set_base_dir");

  error= set_default_dir("iclaustron_install", base_dir);
  if (!error)
    DEBUG_PRINT(PROGRAM_LEVEL, ("Base dir: %s", base_dir->str));
  DEBUG_RETURN_INT(error);
}

/**
  Create the string ./ in a portable referring to the current directory

  @parameter dir             OUT: String to place resulting string
*/
void
ic_set_current_dir(IC_STRING *dir)
{
#ifdef WINDOWS
  IC_INIT_STRING(dir, ".\\", 2, TRUE);
#else
  IC_INIT_STRING(dir, "./", 2, TRUE);
#endif
}

/**
  Add the string dir_name/ to the input string

  @parameter dir               IN/OUT: The string to add to
  @parameter dir_name          IN:     The directory name to add
*/
static int
ic_add_dir(IC_STRING *dir,
           const gchar *dir_name)
{
  int error;
  DEBUG_ENTRY("ic_add_dir");

  if ((error= ic_add_dup_string(dir, dir_name)))
    goto error;
  if ((error= add_dir_slash(dir)))
    goto error;
  DEBUG_RETURN_INT(0);
error:
  if (dir->str)
    ic_free(dir->str);
  IC_INIT_STRING(dir, NULL, 0, TRUE);
  DEBUG_RETURN_INT(error);
}

/*
  The default binary directory is ICLAUSTRON_BASE_DIR/ICLAUSTRON_VERSION/bin
  As an example this could be:
  /home/mikael/iclaustron_install/iclaustron-0.0.1/bin

  @parameter binary_dir            OUT: The place to put the resulting string
  @parameter version               IN:  String containing iClaustron version
*/
int
ic_set_binary_dir(IC_STRING *binary_dir,
                  gchar *version)
{
  int error;
  DEBUG_ENTRY("ic_set_binary_dir");

  if ((error= ic_set_base_dir(binary_dir)))
    goto error;
  if ((error= ic_add_dir(binary_dir, version)))
    return error;
  if ((error= ic_add_dir(binary_dir, ic_binary_string.str)))
    goto error;
  DEBUG_PRINT(CONFIG_LEVEL, ("Binary dir: %s", binary_dir->str));
  DEBUG_RETURN_INT(0);

error:
  DEBUG_RETURN_INT(error);
}

/*
  The default configuration directory is ICLAUSTRON_DATA_DIR/config/node1
  for Cluster Servers (1 is nodeid), for other nodes it is
  ICLAUSTRON_DATA_DIR/config.
  So as an example a cluster server config directory could be:
  /home/mikael/iclaustron_data/config/node1
  and an example of another node directory could be:
  /home/mikael/iclaustron_data/node1

  @parameter config_dir         OUT: The place to put the resulting string
  @parameter is_cluster_server  IN:  Is it the directory of the cluster server
  @parameter my_node_id         IN:  My node id
*/
int
ic_set_config_dir(IC_STRING *config_dir,
                  gboolean is_cluster_server,
                  guint32 my_node_id)
{
  int error;
  gchar node_str_buf[64];
  DEBUG_ENTRY("ic_set_config_dir");

  if ((error= ic_set_data_dir(config_dir)))
    goto error;
  if ((error= ic_add_dir(config_dir, ic_config_string.str)))
    goto error;
  if (is_cluster_server)
  {
    memcpy(node_str_buf, "node", 4);
    ic_guint64_str((guint64)my_node_id,
                   &node_str_buf[4],
                   NULL);
    if ((error= ic_add_dir(config_dir, &node_str_buf[0])))
      goto error;
  }
  DEBUG_PRINT(CONFIG_LEVEL, ("Config dir: %s", config_dir->str));
  DEBUG_RETURN_INT(0);
error:
  DEBUG_RETURN_INT(error);
}

/**
  Reverse a string with a given final character

  @parameter in_buf         IN:  The input string
  @parameter out_buf        OUT: The output buffer containing reversed string
  @parameter end_char       IN:  The end character
*/
void ic_reverse_str(gchar *in_buf, gchar *out_buf, gchar end_char)
{
  guint32 i= 0;
  guint32 j= 0;

  while (in_buf[i] != end_char)
    i++;
  out_buf[i]= in_buf[i]; /* Copy end_char byte */
  while (i)
    out_buf[j++]= in_buf[--i];
}

/**
  Create a string given a 64-bit integer

  @parameter val          IN:  The 64-bit integer
  @parameter ptr          OUT: The buffer to place the resulting string
  @parameter len          OUT: The length of the resulting string
*/
gchar *ic_guint64_str(guint64 val, gchar *ptr, guint32 *len)
{
  guint32 i= 0;
  gchar buf[128];
  guint64 tmp;

  if (val == 0)
  {
    buf[0]= '0';
    i++;
  }
  while (val != 0)
  {
    tmp= val/10;
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

/**
  Create a string given a 64-bit integer in hexadecimal format, thus on
  the form 0x0A8

  @parameter val          IN:  The 64-bit integer
  @parameter ptr          OUT: The buffer to place the resulting string
  @parameter len          OUT: The length of the resulting string
*/
gchar *ic_guint64_hex_str(guint64 val, gchar *ptr)
{
  guint32 i= 0;
  gchar buf[IC_MAX_INT_STRING];
  guint64 tmp;

  if (val == 0)
  {
    buf[i]= '0';
    i++;
  }
  while (val != 0)
  {
    tmp= val/16;
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
  ic_reverse_str((gchar*)&buf, &ptr[2], 0);
  ptr[0]= '0';
  ptr[1]= 'x';
  return ptr;
}

/**
  Search for given character in string and return index of its first
  occurrence in the string

  @parameter ic_str           IN: Input string
  @parameter searched_char    IN: Character to search for

  @retval index of found character, if character not found one returns the
          string length instead
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

/**
  Add dest_str and input_str and put result in dest_str
  dest_str + input_str = dest_str

  @parameter dest_str         IN/OUT: Destination string
  @parameter input_str        IN:     Added string

  @note
    The function assumes that the destination string has sufficient
    memory to contain the sum of the two strings
*/
void
ic_add_ic_string(IC_STRING *dest_str, IC_STRING *input_str)
{
  gchar *start_ptr= dest_str->str+dest_str->len;
  gchar *end_ptr;

  if (!input_str)
    return;
  memcpy(start_ptr, input_str->str, input_str->len);
  if (dest_str->is_null_terminated)
  {
    end_ptr= start_ptr + input_str->len;
    *end_ptr= 0;
  }
  dest_str->len+= input_str->len;
}

/**
  Add dest_str and input_str and put result in dest_str
  Type of dest_str is IC_STRING and type of input_str is
  gchar*.
  dest_str + input_str = dest_str

  @parameter dest_str         IN/OUT: Destination string
  @parameter input_str        IN:     Added string

  @note
    The function assumes that the destination string has sufficient
    memory to contain the sum of the two strings
*/
void
ic_add_string(IC_STRING *dest_str, const gchar *input_str)
{
  IC_STRING input_ic_str;

  IC_INIT_STRING(&input_ic_str, (gchar*)input_str, strlen(input_str), TRUE);
  ic_add_ic_string(dest_str, &input_ic_str);
}

/**
  Add dest_str and add_str and place result in dest_str
  dest_str + add_str = dest_str
  In this case the dest_str is of type IC_STRING and add_str is of type
  gchar*. The resulting string is allocated using malloc. The old string
  is always free'd, even in case we fail to allocate the new string.

  @parameter dest_str                IN/OUT: The input and output string
  @parameter add_str                 IN:     The string added
*/
int
ic_add_dup_string(IC_STRING *dest_str, const gchar *add_str)
{
  guint32 add_len, orig_len, new_len;
  gchar *new_str;

  if (!add_str)
    return 0;
  add_len= strlen(add_str);
  orig_len= dest_str->len;
  new_len= add_len + orig_len;
  new_str= (gchar*)ic_malloc(new_len + 1);
  if (new_str == NULL)
  {
    if (dest_str->str)
      ic_free(dest_str->str);
    IC_INIT_STRING(dest_str, NULL, 0, FALSE);
    return IC_ERROR_MEM_ALLOC;
  }
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

/**
  Add dest_str and in_str and put resulting string in dest_str
  dest_str + in_str = dest_str
  Both input strings are of type IC_STRING. The new string is allocated
  from the provided memory container.

  @parameter mc_ptr               IN:     The memory container to use
  @parameter dest_str             IN/OUT: The input and output string
  @parameter in_str               IN:     The added string
*/
int
ic_mc_add_ic_string(IC_MEMORY_CONTAINER *mc_ptr,
                    IC_STRING *dest_str,
                    IC_STRING *in_str)
{
  gchar *new_str;
  guint32 new_len;

  if (!in_str)
    return 0;
  new_len= dest_str->len + in_str->len;
  if (!(new_str= (gchar*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, new_len + 1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(new_str, dest_str->str, dest_str->len);
  memcpy(&new_str[dest_str->len], in_str->str, in_str->len);
  if (dest_str->is_null_terminated)
    new_str[new_len]= 0;
  dest_str->str= new_str;
  dest_str->len= new_len;
  return 0;
}

/**
  strcpy variant, copy str to buf_ptr
  Copy str to buf_ptr, make resulting string a NULL-terminated string

  @parameter str                IN: String to copy
  @parameter buf_ptr            IN: Buffer to place result in

  @retval Pointer to buf_ptr where resulting string is placed
*/
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

/**
  Print str to stdout with a final <CR>

  @parameter str         IN: String to print
*/
void ic_print_ic_string(IC_STRING *str)
{
  if (str->is_null_terminated)
    ic_printf("%s", str->str);
  else
  {
    guint32 str_len= 0;
    while (str_len < str->len)
    {
      putchar(str->str[str_len]);
      str_len++;
    }
    putchar('\n');
    fflush(stdout);
  }
}

/**
  strcmp using two IC_STRING objects

  @parameter first_str           IN: First string to compare
  @parameter second_str          IN: Second string to compare

  @retval return 0 if equal, otherwise non-zero is returned
*/
int ic_cmp_str(const IC_STRING *first_str, const IC_STRING *second_str)
{
  guint32 first_len= first_str->len;
  guint32 second_len= second_str->len;
  gchar *first_char= first_str->str;
  gchar *second_char= second_str->str;

  if (first_len != second_len)
    return 1;
  return (memcmp(first_char, second_char, first_len));
}

/**
  strcmp using IC_STRING object and null-terminated string

  @parameter null_term_str          IN: Null-terminated string to compare
  @parameter cmp_str                IN: String to compare to

  @retval 0 if equal, otherwise 1
*/
int ic_cmp_null_term_str(const gchar *null_term_str, const IC_STRING *cmp_str)
{
  guint32 iter_len= 0;
  gchar *cmp_char= cmp_str->str;
  guint32 str_len= cmp_str->len;

  if (cmp_str->is_null_terminated)
    return (strcmp(null_term_str, cmp_str->str) == 0) ? 0 : 1;
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

/**
  Set up a complete IC_STRING object where only the str field have been
  initialised with a null-terminated string and length is the buffer length.
  Search for a NULL that terminates the string to get length, if no
  terminating NULL is found the resulting string is simply the provided
  string.

  @parameter in_out_str         IN/OUT: Resulting string
*/
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

/**
  strdup variant which allocates memory from memory container or from
  malloc (when mc_ptr == NULL). Input is IC_STRING and so is output.

  @parameter mc_ptr        IN:  Memory container to allocate from
  @parameter out_str       OUT: Output string
  @parameter in_str        OUT: Input string
*/
int ic_mc_strdup(IC_MEMORY_CONTAINER *mc_ptr,
                 IC_STRING *out_str,
                 IC_STRING *in_str)
{
  gchar *str;

  IC_INIT_STRING(out_str, NULL, 0, FALSE);
  if (mc_ptr)
  {
    if (!(str= mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, in_str->len+1)))
      return IC_ERROR_MEM_ALLOC;
  }
  else
  {
    if (!(str= ic_malloc(in_str->len+1)))
      return IC_ERROR_MEM_ALLOC;
  }
  IC_COPY_STRING(out_str, in_str);
  out_str->str= str;
  memcpy(out_str->str, in_str->str, in_str->len);
  if (out_str->is_null_terminated)
    out_str->str[out_str->len]= NULL_BYTE;
  return 0;
}

/*
  Deliver a string where both IC_STRING object and string itself
  is allocated from memory container. The returned string is
  always NULL terminated. The input string is of type gchar* and
  need not be NULL-terminated since we also supply the length of
  the string.

   @parameter mc_ptr           The memory container to allocate from
   @parameter out_str          The output string
   @parameter str              The input string
   @parameter str_len          The length of the input string
*/
int ic_mc_char_to_strdup(IC_MEMORY_CONTAINER *mc_ptr,
                         IC_STRING **out_str,
                         gchar *str,
                         guint32 str_len)
{
  IC_STRING *loc_ic_str_ptr;
  IC_STRING loc_str;

  ic_assert(mc_ptr);
  if (!(loc_ic_str_ptr=
        (IC_STRING*)mc_ptr->mc_ops.ic_mc_alloc(mc_ptr,
                                               sizeof(IC_STRING))))
    return IC_ERROR_MEM_ALLOC;
  IC_INIT_STRING(&loc_str, str, str_len, TRUE);
  if (ic_mc_strdup(mc_ptr, loc_ic_str_ptr, &loc_str))
    return IC_ERROR_MEM_ALLOC;
  *out_str= loc_ic_str_ptr;
  return 0;
}

/**
  strdup variant that uses either memory container or malloc to allocate
  new string. Both input and output strings are of type gchar* and thus
  must be NULL-terminated strings.

  @parameter mc_ptr        IN:  Memory container to allocate from
  @parameter out_str       OUT: Output string
  @parameter in_str        OUT: Input string
*/
int ic_mc_chardup(IC_MEMORY_CONTAINER *mc_ptr,
                  gchar **out_str,
                  gchar *in_str)
{
  IC_STRING dest_str;
  IC_STRING input_str;
  int ret_code;

  IC_INIT_STRING(&input_str, in_str, strlen(in_str), TRUE);
  if ((ret_code= ic_mc_strdup(mc_ptr, &dest_str, &input_str)))
    return ret_code;
  *out_str= dest_str.str;
  return 0;
}

/**
  strdup variant that allocates using malloc, input and output string
  are of the type IC_STRING.
 
  @parameter out_str       OUT: Output string
  @parameter in_str        IN:  Input string
*/
int
ic_strdup(IC_STRING *out_str, IC_STRING *in_str)
{
  return ic_mc_strdup(NULL, out_str, in_str);
}

/**
  strdup variant that allocates using malloc, input and output string
  are of the type gchar*.
 
  @parameter out_str       OUT: Output string
  @parameter in_str        IN:  Input string
*/
int
ic_chardup(gchar **out_str, gchar *in_str)
{
  return ic_mc_chardup(NULL, out_str, in_str);
}

/**
  Count number of characters before separator is found, separator is
  SPACE, NULL-byte or <CR>.

  @parameter str             String
  @parameter max_chars       String length
*/
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

/**
  Convert a string to unsigned 64-bit integer. The input string need not be
  NULL-terminated since we supply the length of the string.

  @parameter str              The number string
  @parameter num_chars        The length of the number string
  @parameter ret_number       OUT: The 64-bit unsigned integer converted to
*/
int
ic_convert_str_to_int_fixed_size(gchar *str,
                                 guint32 num_chars,
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

/**
  Copy and convert string. Copy in_str to out_str, convert all lowercase
  characters to uppercase characters.
  It is assumed that the output string has sufficient space to fit the
  entire input string.

  @parameter out_str           OUT: The uppercased output string
  @parameter in_str            IN:  The input string
  @parameter str_len           IN:  The lenght of the input string
*/
void
ic_convert_to_uppercase(gchar *out_str, gchar *in_str, guint32 str_len)
{
  guint32 i;

  for (i= 0; i < str_len; i++)
  {
    if (islower((int)in_str[i]))
      out_str[i]= in_str[i] - ('a' - 'A');
    else
      out_str[i]= in_str[i];
  }
}

/**
  Count the number of lines in a character array.

  @parameter str               IN: The character array
  @parameter str_size          IN: The size of the character array

  @return value                OUT: The number of lines found

  This function can be used to count the number of lines in a file
  by first using ic_get_file_contents and then using this function
  on the character array returned from this function.
*/
guint64
ic_count_lines(gchar *str, guint64 str_size)
{
  guint64 i;
  guint64 lines= 0;
  guint64 last_carriage_return;
  for (i= 0; i < str_size; i++)
  {
    if (str[i] == CARRIAGE_RETURN)
    {
      last_carriage_return= i;
      lines++;
    }
  }
  if ((last_carriage_return + 1) != str_size)
    lines++; /* Count also last line which isn't ended by a carriage return */
  return lines;
}

/**
  Get the next line in a character array, move the string pointer forward
  and decrement the remaining string size and return the line size, put the
  resulting string into the provided buffer. Ensure the line isn't longer
  than the buffer can contain.

  @parameter str                    IN/OUT: String, return new position
  @parameter str_size               IN/OUT: String size, return remaining size
  @parameter buf                    IN: Buffer to put line into
  @parameter buf_size               IN: Buffer size
  @parameter line_size              OUT: Line size

  Note:
    This function can be used in conjunction with ic_get_file_contents to
    get the content of a file, line by line.

  Note:
    The buffer must be big enough to also contain the final NULL byte.
*/
int
ic_get_next_line(gchar **str,
                 guint64 *str_size,
                 gchar *buf,
                 guint32 buf_size,
                 guint32 *line_size)
{
  gchar *loc_str= *str;
  guint64 loc_str_size= *str_size;
  guint64 i;
  guint64 max_size= IC_MAX(loc_str_size, (buf_size - 1));
  DEBUG_ENTRY("ic_get_next_line");

  *line_size= 0;
  for (i= 0; i < max_size; i++)
  {
    if (loc_str[i] == CARRIAGE_RETURN)
      goto end;
    buf[i]= loc_str[i];
  }
  if (i == loc_str_size)
  {
    /*
      It is ok that the last line isn't terminated by a CARRIAGE_RETURN.
      It must however still not be longer than the maximum buffer size.
    */
    goto end;
  }
  /*
    If we come here the line was too long, we couldn't fit the line in the
    buffer that was provided for this purpose.
  */
  DEBUG_RETURN_INT(IC_ERROR_LINE_TOO_LONG);

end:
  buf[i]= (gchar)0;
  *str_size= (loc_str_size - (i + 1));
  *str= (loc_str + i + 1);
  *line_size= i;
  DEBUG_RETURN_INT(0);
}
