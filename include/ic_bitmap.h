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

#ifndef IC_BITMAP_H
#define IC_BITMAP_H
/* Bit manipulation routines */
guint32 ic_count_highest_bit(guint32 bit_var);
#define ic_is_bit_set(value, bit_number) \
  (((value) >> (bit_number)) & 1)
#define ic_set_bit(value, bit_number) \
  (value)|= (1 << (bit_number))

/* Bitmap routines */
struct ic_bitmap
{
  guchar *bitmap_area;
  guint32 num_bits;
  gboolean alloced_bitmap;
  gboolean alloced_bitmap_area;
};

#define IC_BITMAP_SIZE(num_bits) ((4*((num_bits)/32) + 4))
#define IC_BITMAP_BIT(bit_number) ((bit_number) & 7)
#define IC_BITMAP_BYTE(bit_number) ((bit_number) / 8)
/*
  If we send in a bitmap which is NULL, we will allocate a
  bitmap and a bitmap area. If we send in a bitmap we could
  also set bitmap_area to zero if we want to allocate the
  bitmap area, otherwise it is assumed the area is already
  allocated.
*/
IC_BITMAP* ic_create_bitmap(IC_BITMAP* bitmap, guint32 num_bits);
IC_BITMAP* ic_mc_create_bitmap(IC_MEMORY_CONTAINER *mc, guint32 num_bits);
void ic_free_bitmap(IC_BITMAP* bitmap);
#define macro_ic_bitmap_set_bit(bitmap, bit_number) \
  (ic_set_bit(((bitmap)->bitmap_area[(IC_BITMAP_BYTE((bit_number)))]), \
               (IC_BITMAP_BIT((bit_number)))))
#define macro_ic_is_bitmap_set(bitmap, bit_number) \
  (ic_is_bit_set(((bitmap)->bitmap_area[IC_BITMAP_BYTE((bit_number))]), \
                 (IC_BITMAP_BIT((bit_number)))))
#define ic_bitmap_get_num_bits(bitmap) (bitmap->num_bits)
#define ic_bitmap_copy(dest_bitmap, src_bitmap) \
{ \
  ic_assert(IC_BITMAP_SIZE((dest_bitmap)->num_bits) == \
           IC_BITMAP_SIZE((src_bitmap)->num_bits)); \
  memcpy((dest_bitmap)->bitmap_area, (src_bitmap)->bitmap_area, \
         IC_BITMAP_SIZE((src_bitmap)->num_bits)); \
}
#ifndef DEBUG_BUILD
#define ic_is_bitmap_set(bitmap, bit_number) \
  macro_ic_is_bitmap_set(bitmap, bit_number)
#define ic_bitmap_set_bit(bitmap, bit_number) \
  macro_ic_bitmap_set_bit(bitmap, bit_number)
#else
void ic_bitmap_set_bit(IC_BITMAP *bitmap, guint32 bit_number);
gboolean ic_is_bitmap_set(IC_BITMAP *bitmap, guint32 bit_number);
#endif
#endif
