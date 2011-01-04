/* Copyright (C) 2007-2011 iClaustron AB

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
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_bitmap.h>

/*
  Bitmap routines
  ---------------
  This is a set of bitmap routines. There are methods available to
  allocate the bitmap from the stack, using malloc and using the
  IC_MEMORY_CONTAINER class. Independent of method used to allocate
  the bitmap the routines used to read and manipulate it are the
  same.
*/
IC_BITMAP*
ic_create_bitmap(IC_BITMAP *bitmap, guint32 num_bits)
{
  IC_BITMAP *loc_bitmap= bitmap;
  guint32 size= IC_BITMAP_SIZE(num_bits);

  if (!loc_bitmap)
  {
    /* Bitmap wasn't allocated on stack so we need to allocate it. */
    if (!(loc_bitmap= (IC_BITMAP*)ic_calloc(sizeof(IC_BITMAP))))
      return NULL;
    loc_bitmap->alloced_bitmap= TRUE;
  }
  if (!loc_bitmap->bitmap_area)
  {
    if (!(loc_bitmap->bitmap_area= (guchar*)ic_calloc(size)))
    {
      if (loc_bitmap->alloced_bitmap)
        ic_free(loc_bitmap);
      return NULL;
    }
    loc_bitmap->alloced_bitmap_area= TRUE;
  }
  loc_bitmap->num_bits= num_bits;
  return loc_bitmap;
}

void
ic_free_bitmap(IC_BITMAP *bitmap)
{
  if (bitmap->alloced_bitmap_area)
    ic_free(bitmap->bitmap_area);
  if (bitmap->alloced_bitmap)
    ic_free(bitmap);
}

IC_BITMAP*
ic_mc_create_bitmap(IC_MEMORY_CONTAINER *mc, guint32 num_bits)
{
  IC_BITMAP *loc_bitmap;
  guint32 size= IC_BITMAP_SIZE(num_bits);

  if (!(loc_bitmap= (IC_BITMAP*)
        mc->mc_ops.ic_mc_calloc(mc, sizeof(IC_BITMAP))))
    goto end;
  if ((loc_bitmap->bitmap_area= (guchar*)mc->mc_ops.ic_mc_calloc(mc, size)))
    return loc_bitmap;
  loc_bitmap= NULL;
end:
  return loc_bitmap;
}

#ifdef DEBUG_BUILD
void
ic_bitmap_set_bit(IC_BITMAP *bitmap, guint32 bit_number)
{
  ic_assert(bit_number < bitmap->num_bits);
  macro_ic_bitmap_set_bit(bitmap, bit_number);
}

gboolean
ic_is_bitmap_set(IC_BITMAP *bitmap, guint32 bit_number)
{
  ic_assert(bit_number < bitmap->num_bits);
  return macro_ic_is_bitmap_set(bitmap, bit_number);
}
#endif
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
