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

#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_err.h>
#include <ic_debug.h>

/* Decode map from ASCII(43) = '+' to ASCII(122) = 'z' */
static guint8
decode_table[80]= {62, 65, 65, 65, 63, 52, 53, 54, 55, 56,
                   57, 58, 59, 60, 61, 65, 65, 65, 64, 65,
                   65, 65,  0,  1,  2,  3,  4,  5,  6,  7,
                    8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
                   18, 19, 20, 21, 22, 23, 24, 25, 65, 65,
                   65, 65, 65, 65, 26, 27, 28, 29, 30, 31,
                   32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                   42, 43, 44, 45, 46, 47, 48, 49, 50, 51};

static guint8
encode_table[64]= {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                   0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50,
                   0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
                   0x59, 0x5A, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
                   0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
                   0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
                   0x77, 0x78, 0x79, 0x7A, 0x30, 0x31, 0x32, 0x33,
                   0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x2B, 0x2F};

int
ic_base64_decode(guint8 *dest, guint32 *dest_len,
                 const guint8 *src, guint32 src_len)
{
  guint32 decode_len, conv_len;
  guint32 tot_conv_len= 0;
  guint32 inx, i;
  gboolean last= FALSE;
  guint8 decode_char[4], conv_char;
  for (inx= 0, decode_len= 0;
       inx < *dest_len && decode_len < src_len;
       decode_len+= 4, inx+= 3)
  {
    if (last)
      return 1;
    conv_len= 3;
    for (i= 0; i < 4; i++)
    {
      guint8 dec_char= src[decode_len + i];
      if (dec_char < '+' || dec_char > 'z')
        return 1;
      else
        conv_char= decode_table[dec_char - '+'];
      if (conv_char == 65)
        return 1;
      if (conv_char == 64)
      {
        conv_len--;
        if (conv_len == 0)
          return 1;
        last= TRUE;
      }
      decode_char[i]= conv_char & 63;
    }
    dest[inx]= (decode_char[0] << 2) + (decode_char[1] >> 4); 
    dest[inx + 1]= ((decode_char[1] & 0xF) << 4) + (decode_char[2] >> 2);
    dest[inx + 2]= ((decode_char[2] & 3) << 6) + decode_char[3]; 
    tot_conv_len+= conv_len;
  }
  if (decode_len != src_len)
    return 1;
  *dest_len= tot_conv_len;
  return 0;
}

int
ic_base64_encode(guint8 **dest,
                 guint32 *dest_len,
                 const guint8 *src,
                 guint32 src_len)
{
  guint32 std_quads= src_len/3;
  guint32 left_overs= src_len - 3*std_quads;
  guint32 cr_count= 0, i;
  guint32 c1, c2, c3, c4;
  guint32 real_quads= (src_len+2)/3;
  guint32 no_crs= (real_quads+18)/19;
  guint8 *dst, *end_dst;
  guint32 dst_len;
  DEBUG_ENTRY("base64_encode");

  dst_len= 4*real_quads+no_crs;
  dst= (guint8*)ic_calloc(dst_len + 1);
  if (!dst)
  {
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  }
  *dest= dst;
  *dest_len= dst_len;
  end_dst= dst + dst_len + 1;
  for (i= 0; i < std_quads; i++, src+= 3, dst+= 4)
  {
    c1= src[0] >> 2;
    c2= ((src[0] & 0x3) << 4) + (src[1] >> 4);
    c3= ((src[1] & 0xF) << 2) + (src[2] >> 6);
    c4= src[2] & 0x3F;
    dst[0]= encode_table[c1];
    dst[1]= encode_table[c2];
    dst[2]= encode_table[c3];
    dst[3]= encode_table[c4];
    if (++cr_count == 19)
    {
      dst[4]= CARRIAGE_RETURN;
      dst++;
      cr_count= 0;
    }
  }
  if (left_overs == 1)
  {
    c1= src[0] >> 2;
    c2= (src[0] & 0x3) << 4;
    dst[0]= encode_table[c1];
    dst[1]= encode_table[c2];
    dst[2]= '=';
    dst[3]= '=';
    dst+= 4;
    cr_count++;
  }
  else if (left_overs == 2)
  {
    c1= src[0] >> 2;
    c2= ((src[0] & 0x3) << 4) + (src[1] >> 4);
    c3= (src[1] & 0xF) << 2;
    dst[0]= encode_table[c1];
    dst[1]= encode_table[c2];
    dst[2]= encode_table[c3];
    dst[3]= '=';
    dst+= 4;
    cr_count++;
  }
  if (cr_count != 0)
  {
    *dst= CARRIAGE_RETURN;
    dst++;
  }
  *dst= 0;
  dst++;
  ic_assert(dst == end_dst);
  DEBUG_RETURN_INT(0);
}
