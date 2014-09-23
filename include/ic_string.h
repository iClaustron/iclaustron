/* Copyright (C) 2007, 2014 iClaustron AB

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

#ifndef IC_STRING_H
#define IC_STRING_H

IC_STRING ic_glob_config_dir;
IC_STRING ic_glob_data_dir;
IC_STRING ic_glob_base_dir;
IC_STRING ic_glob_working_dir;
IC_STRING ic_glob_stdout_file;
IC_STRING ic_glob_pid_file;
IC_STRING ic_glob_binary_dir;
gchar *ic_glob_process_name;

/*
  HEADER MODULE: iClaustron String handling
  -----------------------------------------
  This is a standard string type, it is declared as whether it is
  null_terminated or not.
*/
struct ic_string
{
  gchar *str;
  guint32 len;
  gboolean is_null_terminated;
};

/* Macro to quickly initialise an IC_STRING */
#define IC_INIT_STRING(obj, a, b, c) \
  (obj)->str= (a); \
  (obj)->len= (b); \
  (obj)->is_null_terminated= (c);

/* Macro to copy from one IC_STRING to another */
#define IC_COPY_STRING(dest_obj, src_obj) \
  (dest_obj)->str= (src_obj)->str; \
  (dest_obj)->len= (src_obj)->len; \
  (dest_obj)->is_null_terminated= (src_obj)->is_null_terminated;

/*
  A few functions to set default directory references to iClaustron
  directories.
 
  The calculated directory name will be placed in the first IC_STRING
  parameter.
*/
void ic_set_current_dir(IC_STRING *dir);
int ic_set_base_dir(IC_STRING *base_dir);
int ic_set_data_dir(IC_STRING *data_dir, gboolean debug);
int ic_set_working_dir(IC_STRING *working_dir,
                       guint32 node_id,
                       gboolean is_node_process);
int ic_set_out_file(IC_STRING *stdout_file,
                    guint32 node_id,
                    gchar *program_name,
                    gboolean is_node_process,
                    gboolean is_log_file);
int ic_set_binary_dir(IC_STRING *binary_dir, gchar *version);
int ic_set_config_dir(IC_STRING *config_dir,
                      gboolean is_cluster_server,
                      guint32 node_id);

void ic_set_number_ending_string(gchar *buf, guint64 number);
void ic_create_config_file_name(IC_STRING *file_name,
                                gchar *buf,
                                IC_STRING *config_dir,
                                IC_STRING *name,
                                IC_CONF_VERSION_TYPE config_version_number);
void ic_create_config_version_file_name(IC_STRING *file_name,
                                        gchar *buf,
                                        IC_STRING *config_dir);
void ic_reverse_str(gchar *in_buf, gchar *out_buf, gchar end_char);

/*
  ic_get_ic_string
    Convert IC_STRING to normal NULL-terminated string

  ic_add_string
    This function adds input_str to dest_str, input_str is always a normal
    NULL-terminated string. If the input string doesn't exist it returns
    an empty string.

  ic_add_dup_string
    This function will first allocate a new string to contain the destination
    string and the added string, then copy it to the new string and finally
    also release the storage of the old string. Thus it is a prerequisite for
    this function that the string is allocated using normal malloc.

  ic_add_ic_string
    Adds input_str to dest_str

  ic_mc_add_ic_string
    Adds in_str and dest_str into dest_str, dest_str string is allocated from
    memory container.

  ic_str_find_first
    This function finds the first occurrence of the searched_char in the
    string. If it doesn't find any occurrence it reports the length of
    the string.

  ic_print_ic_string
    This function prints the IC_STRING to stdout

  ic_cmp_null_term_str
    This function compares a NULL-terminated string with an IC_STRING

  ic_strdup, ic_mc_strdup
    Create a copy of the input IC_STR and allocate memory for the string.
    Assumes someone else allocated memory for new IC_STRING.
    ic_mc_strdup does the same thing but allocates memory from a memory
    container instead.

  ic_mc_char_to_strdup
    Create a copy of the input str with its given length. This copy
    is returned in an IC_STRING object where both the IC_STRING object
    and the string is allocated using the provided memory container.

  ic_chardup, ic_mc_chardup
    Create a copy of a NULL terminated string

  ic_conv_config_str_to_int
    Converts an IC_STRING containing a number from a configuration file
    to a number. A configuration file number can contain k, K, m, M, g and G
    to represent 1024, 1024*1024 and 1024*1024*1024.

  ic_convert_file_to_dir
    Converts a filename string to a directory name. Searches for the last
    '\' in the filename ('/' on Windows).

  ic_convert_to_uppercase
    Converts a C string to all UPPERCASE.
*/
gchar *ic_get_ic_string(IC_STRING *str, gchar *buf_ptr);
void ic_add_string(IC_STRING *dest_str, const gchar *input_str);
int ic_add_dup_string(IC_STRING *dest_str, const gchar *add_str);
void ic_add_ic_string(IC_STRING *dest_str, IC_STRING *input_str);
int ic_mc_add_ic_string(IC_MEMORY_CONTAINER *mc_ptr,
                        IC_STRING *dest_str,
                        IC_STRING *in_str);
int ic_mc_add_string(IC_MEMORY_CONTAINER *mc_ptr,
                     IC_STRING *dest_str,
                     const gchar *in_str);
guint32 ic_str_find_first(IC_STRING *ic_str, gchar searched_char);
void ic_print_ic_string(IC_STRING *str);
int ic_cmp_null_term_str(const gchar *null_term_str, const IC_STRING *cmp_str);
int ic_cmp_str(const IC_STRING *first_str, const IC_STRING *second_str);
int ic_chardup(gchar **out_str, gchar *in_str);
int ic_mc_chardup(IC_MEMORY_CONTAINER *mc_ptr, gchar **out_str,
                  gchar *in_str);
int ic_strdup(IC_STRING *out_str, IC_STRING *in_str);
int ic_mc_strdup(IC_MEMORY_CONTAINER *mc_ptr,
                 IC_STRING *out_str, IC_STRING *in_str);
int ic_mc_char_to_strdup(IC_MEMORY_CONTAINER *mc_ptr,
                         IC_STRING **out_str,
                         gchar *str,
                         guint32 str_len);
int ic_conv_config_str_to_int(guint64 *value, IC_STRING *ic_str);
gchar *ic_convert_file_to_dir(gchar *buf, gchar *file_name);
void ic_convert_to_uppercase(gchar *out_str, gchar *in_str, guint32 str_len);

/*
  ic_set_up_ic_string()

  Input:
    str                 String to set up
    len                 Maximum length
    is_null_terminated  Is string null terminated

  Output:
    str                 String that was set-up
    len                 Real length
    is_null_terminated  Normally true, except when no NULL was found in string
*/
void ic_set_up_ic_string(IC_STRING *in_out_str);

/*
  Conversion routines from string to number and vice versa.
*/
gchar *ic_guint64_str(guint64 val, gchar *ptr, guint32 *len);
gchar *ic_guint64_hex_str(guint64 val, gchar *ptr);
int ic_conv_str_to_int(gchar *str, guint64 *number, guint32 *len);

/*
  HEADER MODULE: iClaustron Predefined Strings
  -------------------------------------
  A number of predefined strings used by iClaustron
*/
extern gchar *ic_empty_string;
extern gchar *ic_space_str;
extern gchar *ic_semi_colon_str;
extern gchar *ic_colon_str;
extern gchar *ic_err_str;

/*
  Pointers to strings that contain the standard names of configuration
  files and configuration version files.
*/
extern IC_STRING ic_version_file_str;
extern IC_STRING ic_config_string;
extern IC_STRING ic_grid_common_config_string;
extern IC_STRING ic_config_ending_string;

/* Methods to handle conversion to integers from strings */
guint32 ic_count_characters(gchar *str, guint32 max_chars);
int ic_convert_str_to_int_fixed_size(gchar *str,
                                     guint32 num_chars,
                                     guint64 *ret_number);
/* Method to calculate number of lines in a character array */
guint64 ic_count_lines(gchar *str, guint64 str_size);
int ic_get_next_line(gchar **str,
                     guint64 *str_size,
                     gchar *buf,
                     guint32 buf_size,
                     guint32 *line_size);
#endif
