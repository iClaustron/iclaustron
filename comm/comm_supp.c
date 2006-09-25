#include <ic_common.h>

void
ic_print_buf(char *buf, guint32 size)
{
  char p_buf[2049];
  memcpy(p_buf, buf, size);
  p_buf[size]= NULL_BYTE;
  printf("Receive buffer, size %u:\n%s\n", size, p_buf);
}

/* Decode map from ASCII(43) = '+' to ASCII(122) = 'z' */
static char
decode_table[80]= {62, 65, 65, 65, 63, 52, 53, 54, 55, 56,
                   57, 58, 59, 60, 61, 65, 65, 65, 64, 65,
                   65, 65,  0,  1,  2,  3,  4,  5,  6,  7,
                    8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
                   18, 19, 20, 21, 22, 23, 24, 25, 65, 65,
                   65, 65, 65, 65, 26, 27, 28, 29, 30, 31,
                   32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                   42, 43, 44, 45, 46, 47, 48, 49, 50, 51};

int
base64_decode(char *dest, guint32 *dest_len,
              const char *src, guint32 src_len)
{
  guint32 decode_len, conv_len, tot_conv_len;
  guint32 inx, i;
  gboolean last= FALSE;
  char decode_char[4], conv_char;
  for (inx= 0, decode_len= 0;
       inx < *dest_len && decode_len < src_len;
       decode_len+= 4, inx+= 3)
  {
    if (last)
      return 1;
    conv_len= 3;
    for (i= 0; i < 4; i++)
    {
      char dec_char= src[decode_len + i];
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
base64_encode(__attribute__ ((unused)) char *dest,
              __attribute__ ((unused)) guint32 *dest_len,
              __attribute__ ((unused)) const char *src,
              __attribute__ ((unused)) guint32 src_len)
{
  return 0;
}

gboolean
convert_str_to_int_fixed_size(char *str, guint32 num_chars,
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
