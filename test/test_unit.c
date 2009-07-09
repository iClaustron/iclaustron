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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_dyn_array.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_bitmap.h>

static const gchar *glob_process_name= "test_unit";
static int glob_test_type= 0;
static GOptionEntry entries[] = 
{
  { "test_type", 0, 0, G_OPTION_ARG_INT, &glob_test_type,
    "Set test type", NULL},
#ifdef DEBUG_BUILD
  { "error_inject", 0, 0, G_OPTION_ARG_INT, &error_inject,
    "Set error_inject", NULL},
#endif
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

#define SIMPLE_BUF_SIZE 16*1024*1024
#define ORDERED_BUF_SIZE 128*1024*1024

static int
do_write_dyn_array(IC_DYNAMIC_ARRAY *dyn_array,
                   gchar *compare_buf,
                   int buf_size,
                   GRand *random,
                   gchar *read_buf)
{
  guint32 start, rand_size, remaining_size;
  int ret_code;

  start= 0;
  while ((int)start < buf_size)
  {
    rand_size= g_rand_int_range(random,1,4000);
    remaining_size= buf_size - start;
    rand_size= IC_MIN(rand_size, remaining_size);
    ret_code= dyn_array->da_ops.ic_write_dynamic_array(dyn_array,
                                                       start,
                                                       rand_size,
                                                       compare_buf+start);
    if (ret_code != 0)
      return ret_code;
    ret_code= dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                                      start,
                                                      rand_size,
                                                      read_buf);
    if (ret_code != 0)
      return ret_code;
    if (memcmp(compare_buf+start, read_buf, rand_size))
      return 1;
    start+= rand_size;
  }
  return 0;
}

static int
do_insert_dyn_array(IC_DYNAMIC_ARRAY *dyn_array,
                    gchar *compare_buf,
                    int buf_size,
                    GRand *random,
                    gchar *read_buf)
{
  guint32 start;
  guint64 curr_size;
  gint32 rand_size, remaining_size;
  int ret_code;

  start= 0;
  while ((int)start < buf_size)
  {
    curr_size= dyn_array->da_ops.ic_get_current_size(dyn_array);
    if (curr_size != (guint64)start)
      return 2;
    rand_size= g_rand_int_range(random,1,4000);
    remaining_size= buf_size - start;
    rand_size= IC_MIN(rand_size, remaining_size);
    ret_code= dyn_array->da_ops.ic_insert_dynamic_array(dyn_array,
                                                        compare_buf+start,
                                                        rand_size);
    if (ret_code != 0)
      return ret_code;
    ret_code= dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                                      start,
                                                      rand_size,
                                                      read_buf);
    if (ret_code != 0)
      return ret_code;
    if (memcmp(compare_buf+start, read_buf, rand_size))
      return 1;
    start+= rand_size;
  }
  return 0;
}

static int
check_read_dyn_array(IC_DYNAMIC_ARRAY *dyn_array,
                     gchar *compare_buf,
                     gchar *read_buf,
                     int buf_size,
                     GRand *random)
{
  guint32 start;
  gint32 rand_size, remaining_size;
  int ret_code;

  start= 0;
  while ((int)start < buf_size)
  {
    rand_size= g_rand_int_range(random,1,4000);
    remaining_size= buf_size - start;
    rand_size= IC_MIN(rand_size, remaining_size);
    ret_code= dyn_array->da_ops.ic_read_dynamic_array(dyn_array,
                                                      start,
                                                      rand_size,
                                                      read_buf);
    if (ret_code != 0)
      return ret_code;
    if (memcmp(compare_buf+start, read_buf, rand_size))
      return 1;
    start+= rand_size;
  }
  return 0;
}

static int
test_dynamic_array(IC_DYNAMIC_ARRAY *dyn_array, int buf_size)
{
  int i;
  int ret_code;
  gchar *compare_buf= ic_malloc(buf_size);
  gchar *read_buf= ic_malloc(buf_size);
  GRand *random= g_rand_new();

  if (random == NULL)
    goto mem_alloc_error;
  if (compare_buf == NULL)
    goto mem_alloc_error;
  if (read_buf == NULL)
    goto mem_alloc_error;
  for (i= 0; i < buf_size; i++)
    ((guint8*)compare_buf)[i]= (guint8)(i & 255);

  ret_code= do_insert_dyn_array(dyn_array,
                                compare_buf,
                                buf_size,
                                random,
                                read_buf);
  if (ret_code)
    goto error;

  ret_code= check_read_dyn_array(dyn_array,
                                 compare_buf,
                                 read_buf,
                                 buf_size,
                                 random);
  if (ret_code)
    goto error;

  for (i= 0; i < buf_size; i++)
    ((guint8*)compare_buf)[i]= (guint8)((((guint8)compare_buf[i]) + 3) & 255);

  ret_code= do_write_dyn_array(dyn_array,
                               compare_buf,
                               buf_size,
                               random,
                               read_buf);
  if (ret_code)
    goto error;

  ret_code= check_read_dyn_array(dyn_array,
                                 compare_buf,
                                 read_buf,
                                 buf_size,
                                 random);
  if (ret_code)
    goto error;

  ret_code= 0;
end:
  if (compare_buf)
    ic_free(compare_buf);
  if (read_buf)
    ic_free(read_buf);
  if (random)
    g_rand_free(random);
  dyn_array->da_ops.ic_free_dynamic_array(dyn_array);
  return ret_code;

mem_alloc_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  goto end;
}

static int
unit_test_ordered_dynamic_array()
{
  IC_DYNAMIC_ARRAY *dyn_array;
  int buf_size= ORDERED_BUF_SIZE;

  dyn_array= ic_create_ordered_dynamic_array();
  if (dyn_array == NULL)
    return IC_ERROR_MEM_ALLOC;
  return test_dynamic_array(dyn_array, buf_size);
}

static int
unit_test_simple_dynamic_array()
{
  IC_DYNAMIC_ARRAY *dyn_array;
  int buf_size= SIMPLE_BUF_SIZE;

  dyn_array= ic_create_simple_dynamic_array();
  if (dyn_array == NULL)
    return IC_ERROR_MEM_ALLOC;
  return test_dynamic_array(dyn_array, buf_size);
}

struct ic_test_dyn_trans
{
  int object;
  gboolean in_dyn_trans;
  guint64 index;
};
typedef struct ic_test_dyn_trans IC_TEST_DYN_TRANS;
#define IC_NUM_TEST_DYN_TRANS_OBJECTS 4000000

static int
test_dynamic_translation(IC_DYNAMIC_TRANSLATION *dyn_trans)
{
  void *ret_object;
  int ret_code;
  guint32 i;
  guint64 max_index;
  IC_TEST_DYN_TRANS *test_dyn_trans= (IC_TEST_DYN_TRANS*)
    ic_calloc(sizeof(IC_TEST_DYN_TRANS)*IC_NUM_TEST_DYN_TRANS_OBJECTS);

  if (!test_dyn_trans)
    abort();
  for (i= 0; i < IC_NUM_TEST_DYN_TRANS_OBJECTS; i++)
  {
    test_dyn_trans[i].object= i;
  }

  for (i= 0; i < 4; i++) 
  {
    if ((ret_code= dyn_trans->dt_ops.ic_insert_translation_object(
                       dyn_trans,
                       &test_dyn_trans[i].index,
                       (void*)&test_dyn_trans[i].object)))
      goto error;
    test_dyn_trans[i].in_dyn_trans= TRUE;
  }
  for (i= 0; i < 2; i++)
  {
     if ((ret_code= dyn_trans->dt_ops.ic_remove_translation_object(
                       dyn_trans,
                       test_dyn_trans[i].index,
                       (void*)&test_dyn_trans[i].object)))
       goto error;
     test_dyn_trans[i].in_dyn_trans= FALSE;
  }
  for (i= 2; i < 4; i++)
  {
    if ((ret_code= dyn_trans->dt_ops.ic_get_translation_object(
                       dyn_trans,
                       test_dyn_trans[i].index,
                       (void**)&ret_object)))
      goto error;
    if (ret_object != (void*)&test_dyn_trans[i].object)
      return 1;
    max_index= dyn_trans->dt_ops.ic_get_max_index(dyn_trans);
  }
  dyn_trans->dt_ops.ic_free_dynamic_translation(dyn_trans);
  ic_free(test_dyn_trans);
  return 0;
error:
  return ret_code;
  
}

static int
unit_test_dynamic_translation()
{
  IC_DYNAMIC_TRANSLATION *dyn_trans;

  dyn_trans= ic_create_dynamic_translation();
  if (dyn_trans == NULL)
    return IC_ERROR_MEM_ALLOC;
  return test_dynamic_translation(dyn_trans);
}

static int
unit_test_mc()
{
  IC_MEMORY_CONTAINER *mc_ptr;
  guint32 i, j, k, max_alloc_size, no_allocs;
  gboolean large;

  for (i= 1; i < 1000; i++)
  {
    srandom(i);
    mc_ptr= ic_create_memory_container(313*i, 0);
    no_allocs= random() & 255;
    for (j= 0; j < 4; j++)
    {
      for (k= 0; k < no_allocs; k++)
      {
        large= ((random() & 3) == 1); /* 25% of allocations are large */
        if (large)
          max_alloc_size= 32767;
        else
          max_alloc_size= 511;
        mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, random() & max_alloc_size);
      }
      mc_ptr->mc_ops.ic_mc_reset(mc_ptr);
    }
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  }
  return 0;
}

static int
unit_test_bitmap()
{
  int i;
  guint32 bitmap_size;
  IC_BITMAP *bitmap_ptr;
  guint32 bit_set;
  guint32 j;

  for (i= 0; i < 15; i++)
  {
    srandom(i);
    bitmap_size= random() % 5000;
    bitmap_ptr= ic_create_bitmap(NULL, bitmap_size);
    
    for (j= 0; j < bitmap_size; j++)
    {
      bit_set= random() % 1;
      if (bit_set != 0)
      {
        ic_bitmap_set_bit(bitmap_ptr, j);
        if (!ic_is_bitmap_set(bitmap_ptr, j))
          goto error;
      }
      else
        if (ic_is_bitmap_set(bitmap_ptr, j))
          goto error;
    }
    ic_free_bitmap(bitmap_ptr);
  }
  return 0;
error:
  return 1;
}

int main(int argc, char *argv[])
{
  int ret_code= 1;

  if ((ret_code= ic_start_program(argc, argv, entries, glob_process_name,
           "- Unit test program", FALSE)))
    return ret_code;
  switch (glob_test_type)
  {
    case 0:
      printf("Executing Unit test of Memory Container\n");
      ret_code= unit_test_mc();
      break;
    case 1:
      printf("Executing Unit test of Simple Dynamic Array\n");
      ret_code= unit_test_simple_dynamic_array();
      break;
    case 2:
      printf("Executing Unit test of Ordered Dynamic Array\n");
      ret_code= unit_test_ordered_dynamic_array();
      break;
    case 3:
      printf("Executing Unit test of Dynamic translation\n");
      ret_code= unit_test_dynamic_translation();
      break;
    case 4:
      printf("Executing Unit test of Bitmap\n");
      ret_code= unit_test_bitmap();
      break;      
    default:
      break;
   }
   ic_end();
   printf("iClaustron Test return code: %d\n", ret_code);
   return ret_code;
}
