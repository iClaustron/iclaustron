/* Copyright (C) 2007 iClaustron AB

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

static int glob_test_type= 0;
static GOptionEntry entries[] = 
{
  { "test_type", 0, 0, G_OPTION_ARG_INT, &glob_test_type,
    "Set test type", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

#define SIMPLE_BUF_SIZE 16*1024*1024
#define ORDERED_BUF_SIZE 128*1024*1024
static int
do_write_dyn_array(IC_DYNAMIC_ARRAY *dyn_array,
                   gchar *compare_buf,
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
    ret_code= dyn_array->da_ops.ic_write_dynamic_array(dyn_array,
                                                       start,
                                                       rand_size,
                                                       compare_buf+start);
    if (ret_code != 0)
      return ret_code;
    start+= rand_size;
  }
  return 0;
}

static int
do_insert_dyn_array(IC_DYNAMIC_ARRAY *dyn_array,
                    gchar *compare_buf,
                    int buf_size,
                    GRand *random)
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
                                random);
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
                               random);
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
  dyn_array->da_ops.ic_free_dynamic_array(dyn_array);
  return ret_code;

mem_alloc_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  if (compare_buf)
    ic_free(compare_buf);
  if (read_buf)
    ic_free(read_buf);
  if (random)
    g_rand_free(random);
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

int main(int argc, char *argv[])
{
  int ret_code= 1;

  if ((ret_code= ic_start_program(argc, argv, entries,
           "- Unit test program")))
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
      printf("Executing Unit test of Simple Dynamic Array\n");
      ret_code= unit_test_ordered_dynamic_array();
      break;
    default:
      break;
   }
   ic_end();
   printf("iClaustron Test return code: %d\n", ret_code);
   return ret_code;
}

