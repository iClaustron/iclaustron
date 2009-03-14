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
#include <ic_apic.h>

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

guint32 glob_debug= 0;
gchar *glob_debug_file= "debug.log";
guint32 glob_debug_screen= 0;

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
                 const gchar *program_name,
                 gchar *start_text)
{
  int ret_code= 1;
  GError *error= NULL;
  GOptionGroup *debug_group;
  GOptionContext *context;

  printf("Starting %s program\n", program_name);
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
  ic_destroy_conf_hash();
  ic_ssl_end();
  DEBUG_CLOSE;
}
