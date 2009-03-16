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
#ifndef DEBUG_BUILD
int debug_dummy; /* To avoid silly linker errors */
#endif

#ifdef DEBUG_BUILD
guint32
ic_get_debug()
{
  return glob_debug;
}

void ic_set_debug(guint32 val)
{
  glob_debug= val;
}

static FILE *ic_fptr;
void
ic_debug_entry(const char *entry_point)
{
  if (glob_debug_screen)
    printf("Entry into: %s\n", entry_point);
  fprintf(ic_fptr, "Entry into: %s\n", entry_point);
}

int ic_debug_open()
{
  ic_fptr= fopen(glob_debug_file, "w");
  if (ic_fptr == NULL)
  {
    printf("Failed to open %s\n", glob_debug_file);
    return 1;
  }
  fprintf(ic_fptr, "Entry into: \n");
  return 0;
}

void
ic_debug_print_char_buf(gchar *buf)
{
  if (glob_debug_screen)
    printf("%s\n", buf);
  fprintf(ic_fptr, "%s\n", buf);
}

void
ic_debug_print_rec_buf(char *buf, guint32 size)
{
  char p_buf[2049];
  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  ic_debug_printf("Receive buffer, size %u:\n%s", size, p_buf);
}

void
ic_debug_printf(const char *format,...)
{
  va_list args;
  char buf[2049];

  va_start(args, format);
  vsprintf(buf, format, args);
  if (glob_debug_screen)
  {
    printf("%s\n", buf);
  }
  fprintf(ic_fptr, "%s\n", buf);
  va_end(args);
}

void
ic_debug_close()
{
  fclose(ic_fptr);
}
#endif
