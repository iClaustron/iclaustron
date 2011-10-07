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
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_dyn_array.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_bitmap.h>
#include <ic_hashtable.h>
#include <ic_parse_connectstring.h>
#include <ic_sock_buf.h>

static const gchar *glob_process_name= "test_unit";
static int glob_test_type= 0;
static GOptionEntry entries[] = 
{
  { "test-type", 0, 0, G_OPTION_ARG_INT, &glob_test_type,
    "Set test type", NULL},
#ifdef DEBUG_BUILD
  { "error-inject", 0, 0, G_OPTION_ARG_INT, &error_inject,
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

struct ic_test_dyn_ptr_array
{
  int object;
  gboolean in_dyn_ptr;
  guint64 index;
};
typedef struct ic_test_dyn_ptr_array IC_TEST_DYN_PTR_ARRAY;

static int
test_ptr_array(IC_DYNAMIC_PTR_ARRAY *dyn_ptr,
               IC_TEST_DYN_PTR_ARRAY *test_dyn_ptr,
               guint32 num_inserts,
               guint32 num_removes)
{
  guint32 i;
  int ret_code;
  void *ret_object;

  for (i= 0; i < num_inserts; i++) 
  {
    if ((ret_code= dyn_ptr->dpa_ops.ic_insert_ptr(dyn_ptr,
                                            &test_dyn_ptr[i].index,
                                            (void*)&test_dyn_ptr[i].object)))
      goto error;
    test_dyn_ptr[i].in_dyn_ptr= TRUE;
  }
  for (i= 0; i < num_removes; i++)
  {
     if ((ret_code= dyn_ptr->dpa_ops.ic_remove_ptr(dyn_ptr,
                                            test_dyn_ptr[i].index,
                                            (void*)&test_dyn_ptr[i].object)))
       goto error;
     test_dyn_ptr[i].in_dyn_ptr= FALSE;
  }
  for (i= num_removes; i < num_inserts; i++)
  {
    if ((ret_code= dyn_ptr->dpa_ops.ic_get_ptr(dyn_ptr,
                                               test_dyn_ptr[i].index,
                                               (void**)&ret_object)))
      goto error;
    if (ret_object != (void*)&test_dyn_ptr[i].object)
      return 1;
  }
  for (i= 0; i < num_removes; i++)
  {
    if (!(ret_code= dyn_ptr->dpa_ops.ic_get_ptr(dyn_ptr,
                                                test_dyn_ptr[i].index,
                                                (void**)&ret_object)))
    {
      ret_code= 1;
      goto error;
    }
  }
  return 0;
error:
return ret_code;
}

static void
init_test_dyn_ptr(IC_TEST_DYN_PTR_ARRAY *test_dyn_ptr,
                  guint32 num_inserts)
{
  guint32 i;

  for (i= 0; i < num_inserts; i++)
    test_dyn_ptr[i].object= i;
}

static int
test_dynamic_ptr_array(guint32 num_inserts, guint32 num_removes)
{
  int ret_code;
  IC_DYNAMIC_PTR_ARRAY *dyn_ptr;
  IC_TEST_DYN_PTR_ARRAY *test_dyn_ptr= (IC_TEST_DYN_PTR_ARRAY*)
    ic_calloc(sizeof(IC_TEST_DYN_PTR_ARRAY)*num_inserts);

  ic_printf("Testing with %u number of inserts and %u number of removes",
            num_inserts, num_removes);
  dyn_ptr= ic_create_dynamic_ptr_array();
  if (dyn_ptr == NULL)
  {
    ic_free(test_dyn_ptr);
    return IC_ERROR_MEM_ALLOC;
  }
  if (!test_dyn_ptr)
    abort();
  init_test_dyn_ptr(test_dyn_ptr, num_inserts);
  if ((ret_code= test_ptr_array(dyn_ptr,
                                test_dyn_ptr,
                                num_inserts,
                                num_removes)))
    goto error;
error:
  dyn_ptr->dpa_ops.ic_free_dynamic_ptr_array(dyn_ptr);
  ic_free(test_dyn_ptr);
  return ret_code;
}

static int
unit_test_dynamic_ptr_array()
{
  int ret_code;

  if ((ret_code= test_dynamic_ptr_array(128, 2)))
    goto error;
  if ((ret_code= test_dynamic_ptr_array(129, 2)))
    goto error;
  if ((ret_code= test_dynamic_ptr_array(128*128, 2)))
    goto error;
  if ((ret_code= test_dynamic_ptr_array(128*128+1, 2)))
    goto error;
  if ((ret_code= test_dynamic_ptr_array(128*128*128, 2)))
    goto error;
  if ((ret_code= test_dynamic_ptr_array(128*128*128+1, 2)))
    goto error;
  if ((ret_code= test_dynamic_ptr_array(128*128*128+1, 128*128*128+1)))
    goto error;
  return 0;
error:
  return ret_code;
}

static int
unit_test_mc(gboolean use_mutex, gboolean use_large)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  guint32 i, j, k, max_alloc_size, no_allocs;
  gboolean large;

  for (i= 1; i < 1000; i++)
  {
    srandom(i);
    mc_ptr= ic_create_memory_container(313*i, 0, use_mutex);
    no_allocs= random() & 255;
    for (j= 0; j < 4; j++)
    {
      for (k= 0; k < no_allocs; k++)
      {
        large= ((random() & 3) == 1); /* 25% of allocations are large */
        if (large && use_large)
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

struct ic_test_hashtable
{
  guint64 object;
  gboolean in_hashtable;
};

typedef struct ic_test_hashtable IC_TEST_HASHTABLE;

static int
test_hashtable_one_test(IC_HASHTABLE *hashtable,
                        IC_TEST_HASHTABLE *test_hashtable,
                        guint32 num_inserts,
                        guint32 num_removes)
{
  guint32 i;
  int ret_code= 0;
  void *ret_object;

  for (i= 0; i < num_inserts; i++) 
  {
    if ((ret_code= ic_hashtable_insert(hashtable,
                                       (void*)&test_hashtable[i].object,
                                       (void*)&test_hashtable[i].object)))
      goto error;
    test_hashtable[i].in_hashtable= TRUE;
  }
  for (i= 0; i < num_removes; i++)
  {
     if (!(ret_object= ic_hashtable_remove(hashtable,
                                           (void*)&test_hashtable[i].object)))
       goto error;
     test_hashtable[i].in_hashtable= FALSE;
  }
  for (i= num_removes; i < num_inserts; i++)
  {
    if (!(ret_object= ic_hashtable_search(hashtable, 
                                         (void*)&test_hashtable[i].object)))
      goto error;
  }
  for (i= 0; i < num_removes; i++)
  {
    if ((ret_object= ic_hashtable_search(hashtable,
                                         (void*)&test_hashtable[i].object)))
    {
      ret_code= 1;
      goto error;
    }
  }
  return 0;
error:
  return ret_code;
}

static void
init_test_hashtable(IC_TEST_HASHTABLE *test_hashtable,
                    guint32 num_inserts)
{
  guint32 i;

  for (i= 0; i < num_inserts; i++)
    test_hashtable[i].object= i;
}

static int
test_hashtable(guint32 num_inserts,
               guint32 num_removes,
               gboolean use_ptr)
{
  int ret_code;
  IC_HASHTABLE *hashtable;
  IC_TEST_HASHTABLE *test_hashtable= (IC_TEST_HASHTABLE*)
    ic_calloc(sizeof(IC_TEST_HASHTABLE)*num_inserts);

  ic_printf("Testing with %u number of inserts and %u number of removes",
            num_inserts, num_removes);
  if (use_ptr)
  {
    hashtable= ic_create_hashtable(4096,
                                   ic_hash_uint64,
                                   ic_keys_equal_uint64,
                                   FALSE);
  }
  else
  {
    hashtable= ic_create_hashtable(4096,
                                   ic_hash_ptr,
                                   ic_keys_equal_ptr,
                                   FALSE);
  }

  if (hashtable == NULL)
  {
    ic_free(test_hashtable);
    return IC_ERROR_MEM_ALLOC;
  }
  if (!test_hashtable)
    abort();
  init_test_hashtable(test_hashtable, num_inserts);
  if ((ret_code= test_hashtable_one_test(hashtable,
                                         test_hashtable,
                                         num_inserts,
                                         num_removes)))
    goto error;
error:
  ic_hashtable_destroy(hashtable, FALSE);
  ic_free(test_hashtable);
  return ret_code;
}

static int
unit_test_hashtable()
{
  int ret_code;
  guint32 i;
  gboolean test_case;

  for (i= 0; i < 2; i++)
  {
    test_case= (gboolean)i;
    if ((ret_code= test_hashtable(128, 2, test_case)))
      goto error;
    if ((ret_code= test_hashtable(129, 2, test_case)))
      goto error;
    if ((ret_code= test_hashtable(128*128, 2, test_case)))
      goto error;
    if ((ret_code= test_hashtable(128*128+1, 2, test_case)))
      goto error;
    if ((ret_code= test_hashtable(128*128*128, 2, test_case)))
      goto error;
    if ((ret_code= test_hashtable(128*128*128+1, 2, test_case)))
      goto error;
    if ((ret_code= test_hashtable(128*128*128+1, 128*128*128+1, test_case)))
      goto error;
  }
  return 0;
error:
  return ret_code;
}

/*
  myhost1:1200
  myhost1:1200, Fel
  myhost1,myhost2
  myhost1:port1,myhost2 Fel
  myhost1:1200,myhost2
  , Fel
  : Fel
  :port1 Fel
  myhost1,myhost2,myhost3:port3 Fel
  myhost1,myhost2,myhost3:1199
*/
static int
test_parse_connectstring(gchar *connect_string,
                         guint32 num_hosts,
                         gchar **hosts,
                         gchar **ports)
{
  int ret_code;
  guint32 i;
  IC_CONNECT_STRING conn_str;

  ret_code= ic_parse_connectstring(connect_string,
                                   &conn_str,
                                   ic_empty_string,
                                   ic_empty_string);
  if (ret_code)
    return ret_code;
  if (conn_str.num_cs_servers != num_hosts)
    goto error;
  for (i= 0; i < num_hosts; i++)
  {
    if (strcmp(conn_str.cs_hosts[i], hosts[i]) != 0)
      goto error;
    if (strcmp(conn_str.cs_ports[i], ports[i]) != 0)
      goto error;
  }
  conn_str.mc_ptr->mc_ops.ic_mc_free(conn_str.mc_ptr);
  return 0;
error:
  conn_str.mc_ptr->mc_ops.ic_mc_free(conn_str.mc_ptr);
  return 1;
}

static int
unit_test_parse_connectstring()
{
  gchar *hosts[8];
  gchar *ports[8];

  hosts[0]= "myhost1"; ports[0]= "1186";
  hosts[1]= "myhost2"; ports[1]= "1186";
  hosts[2]= "myhost3"; ports[2]= "1186";
  hosts[3]= "myhost4"; ports[3]= "1186";
  hosts[4]= "myhost5"; ports[4]= "1186";
  if (test_parse_connectstring("myhost1,myhost2,myhost3,myhost4,myhost5",
            5, (gchar**)hosts, (gchar**)ports) != IC_ERROR_TOO_MANY_CS_HOSTS)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  if (test_parse_connectstring("myhost1,",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  if (test_parse_connectstring("myhost1:",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  hosts[1]= "myhost2"; ports[1]= "1222";
  if (test_parse_connectstring("myhost1,port1:myhost,port0",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1112";
  if (test_parse_connectstring(":port1112",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  hosts[1]= "myhost2"; ports[1]= "1222";
  if (test_parse_connectstring("1111:myhost1,1222:myhost2",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  hosts[1]= "myhost2"; ports[1]= "1222";
  if (test_parse_connectstring("myhost1:myhost2:port1",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  if (test_parse_connectstring(":1111",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1111";
  hosts[1]= "myhost2"; ports[1]= "1234";
  if (test_parse_connectstring("myhost1:1111:1234",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring(":",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring(",",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("",
            2, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost1,,myhost3",
            3, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost(1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost/1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost¤1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost%1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost&1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost)1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost=1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost@1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost!1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost?1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost+1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost^1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost~1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost*1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost'1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost>1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost<1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost|1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost[1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost]1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost{1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost}1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost\1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost`1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost'1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost§1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost£1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost$1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  if (test_parse_connectstring("myhost;1",
            1, (gchar**)hosts, (gchar**)ports) != IC_ERROR_PARSE_CONNECTSTRING)
    return 1;
  hosts[0]= "a"; ports[0]= "1186";
  if (test_parse_connectstring("a",
            1, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "a"; ports[0]= "1";
  if (test_parse_connectstring("a:1",
            1, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "a_-"; ports[0]= "1";
  if (test_parse_connectstring("a_-:1",
            1, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "192.168.0.1"; ports[0]= "1";
  if (test_parse_connectstring("192.168.0.1:1",
            1, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost1"; ports[0]= "1186";
  if (test_parse_connectstring("myhost1",
            1, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost2"; ports[0]= "1100";
  if (test_parse_connectstring("myhost2:1100",
           1, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  if (test_parse_connectstring("myhost4,myhost5",
            2, (gchar**)hosts, (gchar**)ports))
    return 1; 
  hosts[0]= "myhost4"; ports[0]= "1300";
  hosts[1]= "myhost5"; ports[1]= "1200";
  if (test_parse_connectstring("myhost4:1300,myhost5:1200",
            2, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1500";
  if (test_parse_connectstring("myhost4,myhost5:1500",
            2, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1555";
  hosts[1]= "myhost5"; ports[1]= "1186";
  if (test_parse_connectstring("myhost4:1555,myhost5",
            2, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1575";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1186";
  if (test_parse_connectstring("myhost4:1575,myhost5,myhost6",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1575";
  hosts[2]= "myhost6"; ports[2]= "1186";
  if (test_parse_connectstring("myhost4,myhost5:1575,myhost6",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1575";
  if (test_parse_connectstring("myhost4,myhost5,myhost6:1575",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1575";
  hosts[1]= "myhost5"; ports[1]= "1555";
  hosts[2]= "myhost6"; ports[2]= "1186";
  if (test_parse_connectstring("myhost4:1575,myhost5:1555,myhost6",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1575";
  hosts[2]= "myhost6"; ports[2]= "1555";
  if (test_parse_connectstring("myhost4,myhost5:1575,myhost6:1555",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1555";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1575";
  if (test_parse_connectstring("myhost4:1555,myhost5,myhost6:1575",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1555";
  hosts[1]= "myhost5"; ports[1]= "1575";
  hosts[2]= "myhost6"; ports[2]= "1757";
  if (test_parse_connectstring("myhost4:1555,myhost5:1575,myhost6:1757",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1186";
  if (test_parse_connectstring("myhost4,myhost5,myhost6",
            3, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4,myhost5,myhost6,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1555";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4:1555,myhost5,myhost6,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1757";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4,myhost5:1757,myhost6,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1256";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4,myhost5,myhost6:1256,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1556";
  if (test_parse_connectstring("myhost4,myhost5,myhost6,myhost7:1556",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1144";
  hosts[1]= "myhost5"; ports[1]= "1145";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4:1144,myhost5:1145,myhost6,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1250";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1280";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4:1250,myhost5,myhost6:1280,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1249";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1965";
  if (test_parse_connectstring("myhost4:1249,myhost5,myhost6,myhost7:1965",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1971";
  hosts[2]= "myhost6"; ports[2]= "1813";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4,myhost5:1971,myhost6:1813,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1800";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1850";
  if (test_parse_connectstring("myhost4,myhost5:1800,myhost6,myhost7:1850",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1600";
  hosts[3]= "myhost7"; ports[3]= "1700";
  if (test_parse_connectstring("myhost4,myhost5,myhost6:1600,myhost7:1700",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1276";
  hosts[1]= "myhost5"; ports[1]= "1566";
  hosts[2]= "myhost6"; ports[2]= "1766";
  hosts[3]= "myhost7"; ports[3]= "1186";
  if (test_parse_connectstring("myhost4:1276,myhost5:1566,myhost6:1766,myhost7",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1559";
  hosts[1]= "myhost5"; ports[1]= "1669";
  hosts[2]= "myhost6"; ports[2]= "1186";
  hosts[3]= "myhost7"; ports[3]= "1779";
  if (test_parse_connectstring("myhost4:1559,myhost5:1669,myhost6,myhost7:1779",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1906";
  hosts[1]= "myhost5"; ports[1]= "1186";
  hosts[2]= "myhost6"; ports[2]= "1966";
  hosts[3]= "myhost7"; ports[3]= "1926";
  if (test_parse_connectstring("myhost4:1906,myhost5,myhost6:1966,myhost7:1926",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1186";
  hosts[1]= "myhost5"; ports[1]= "1201";
  hosts[2]= "myhost6"; ports[2]= "1202";
  hosts[3]= "myhost7"; ports[3]= "1203";
  if (test_parse_connectstring("myhost4,myhost5:1201,myhost6:1202,myhost7:1203",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  hosts[0]= "myhost4"; ports[0]= "1222";
  hosts[1]= "myhost5"; ports[1]= "1223";
  hosts[2]= "myhost6"; ports[2]= "1224";
  hosts[3]= "myhost7"; ports[3]= "1225";
  if (test_parse_connectstring("myhost4:1222,myhost5:1223,myhost6:1224,myhost7:1225",
            4, (gchar**)hosts, (gchar**)ports))
    return 1;
  return 0;
}

static IC_SOCK_BUF_PAGE*
allocate_all_from_sock_buf(IC_SOCK_BUF *sock_buf,
                           guint32 size,
                           guint32 alloc_page_size,
                           guint32 preallocate_size,
                           IC_SOCK_BUF_PAGE **loc_free_pages)
{
  guint32 i;
  IC_SOCK_BUF_PAGE *sock_buf_page, *first_sock_buf_page, *prev_sock_buf_page;

  first_sock_buf_page= NULL;
  prev_sock_buf_page= NULL;
  for (i= 0; i < size; i++)
  {
    /* These should all work fine */
    if (!(sock_buf_page= sock_buf->sock_buf_ops.ic_get_sock_buf_page(
              sock_buf,
              (guint32)alloc_page_size,
              loc_free_pages,
              preallocate_size)))
      return NULL;
    /* Put the allocated buffers in a linked list */
    if (!first_sock_buf_page)
      first_sock_buf_page= sock_buf_page;
    else
      prev_sock_buf_page->next_sock_buf_page= sock_buf_page;
    prev_sock_buf_page= sock_buf_page;
  }
  return first_sock_buf_page;
}

static int
verify_sock_buf_failure(IC_SOCK_BUF *sock_buf,
                        IC_SOCK_BUF_PAGE **loc_free_pages,
                        guint32 preallocate_size)
{
  if (sock_buf->sock_buf_ops.ic_get_sock_buf_page(sock_buf,
                                                  (guint32)0,
                                                  loc_free_pages,
                                                  preallocate_size))
    return 1;
  return 0;
}

static void
verify_return_sock_buf_all(IC_SOCK_BUF *sock_buf,
                           IC_SOCK_BUF_PAGE *sock_buf_page)
{
  sock_buf->sock_buf_ops.ic_return_sock_buf_page(sock_buf,
                                                 sock_buf_page);
}

static int
test_sock_buf(guint32 size,
              guint32 page_size,
              guint32 preallocate_size)
{
  IC_SOCK_BUF *sock_buf;
  guint32 i;
  guint32 used_prealloc_size;
  guint32 alloc_page_size= 0;
  IC_SOCK_BUF_PAGE *loc_free_pages= NULL;
  IC_SOCK_BUF_PAGE *first_sock_buf_page;
  IC_SOCK_BUF_PAGE **prealloc_pages;
  int ret_code= 1;
  guint32 i_mod_4;
  guint32 used_size;
  guint32 run_loops= (page_size == 0) ? 16 : 4;
  guint32 extra_alloc_size= 100;
  guint32 num_extra_pages;

  for (i= 0; i < run_loops; i++)
  {
    if (!(sock_buf= ic_create_sock_buf(page_size, size)))
      return 1;
    /* 
      First four runs with normal page size, then four runs > 0 without
      extra allocation and four runs with extra allocation and finally
      four runs with no extra allocation due to larger size.
    */
    if (i < 4)
      alloc_page_size= 0;
    else if (i < 8)
      alloc_page_size= 64;
    else if (i < 12)
      alloc_page_size= 124;
    else
      alloc_page_size= 256;

    i_mod_4= i % 4;
    /* Runs 0 and 2 use preallocate_size, 1 and 3 use it set to 1 */
    if (i_mod_4 == 0 || i_mod_4 == 1)
      used_prealloc_size= preallocate_size;
    else
      used_prealloc_size= 1;

    /* All even runs use a free page list, odd runs don't */
    if (i_mod_4 == 0 || i_mod_4 == 2)
      prealloc_pages= NULL;
    else
      prealloc_pages= &loc_free_pages;

    if (alloc_page_size == 124)
    {
      used_size= size / 2; /* We use 2 buffers for each allocated buffer */
      num_extra_pages= extra_alloc_size / 2;
    }
    else
    {
      used_size= size;
      num_extra_pages= extra_alloc_size;
    }

    if (!(first_sock_buf_page= allocate_all_from_sock_buf(sock_buf,
                                                          used_size,
                                                          alloc_page_size,
                                                          used_prealloc_size,
                                                          prealloc_pages)))
      goto error;
    if (verify_sock_buf_failure(sock_buf, prealloc_pages, preallocate_size))
      goto error;
    verify_return_sock_buf_all(sock_buf, first_sock_buf_page);
    if (!(first_sock_buf_page= allocate_all_from_sock_buf(sock_buf,
                                                          used_size / 2,
                                                          alloc_page_size,
                                                          used_prealloc_size,
                                                          prealloc_pages)))
      goto error;
    if (sock_buf->sock_buf_ops.ic_inc_sock_buf(sock_buf, extra_alloc_size))
      goto error;
    if (!(first_sock_buf_page= allocate_all_from_sock_buf(sock_buf,
                                                          num_extra_pages,
                                                          alloc_page_size,
                                                          used_prealloc_size,
                                                          prealloc_pages)))
      goto error;
    if (!(first_sock_buf_page= allocate_all_from_sock_buf(sock_buf,
                                                          used_size / 2,
                                                          alloc_page_size,
                                                          used_prealloc_size,
                                                          prealloc_pages)))
      goto error;
    if (verify_sock_buf_failure(sock_buf, prealloc_pages, preallocate_size))
      goto error;
    if (sock_buf->sock_buf_ops.ic_inc_sock_buf(sock_buf, extra_alloc_size))
      goto error;
    if (!(first_sock_buf_page= allocate_all_from_sock_buf(sock_buf,
                                                          num_extra_pages,
                                                          alloc_page_size,
                                                          used_prealloc_size,
                                                          prealloc_pages)))
    if (verify_sock_buf_failure(sock_buf, prealloc_pages, preallocate_size))
      goto error;
    verify_return_sock_buf_all(sock_buf, first_sock_buf_page);
    ic_require(prealloc_pages == NULL &&
               loc_free_pages == NULL &&
               prealloc_pages ? (prealloc_pages == &loc_free_pages) : TRUE);
    sock_buf->sock_buf_ops.ic_free_sock_buf(sock_buf);
  }
  return 0;
error:
  sock_buf->sock_buf_ops.ic_free_sock_buf(sock_buf);
  return ret_code;
}

static int
unit_test_sock_buf()
{
  if (test_sock_buf(1000, 0, 10))
    return 1;
  if (test_sock_buf(100, 0, 1000))
    return 1;
  if (test_sock_buf(1000, 100, 100))
    return 1;
  return 0;
}
                         
static int
run_test(guint32 test_type)
{
  int ret_code;

  switch (test_type)
  {
    case 1:
      ic_printf("Test 1: Executing Unit test of Memory Container");
      ret_code= unit_test_mc(FALSE, FALSE);
      ret_code= unit_test_mc(FALSE, TRUE);
      ret_code= unit_test_mc(TRUE, FALSE);
      ret_code= unit_test_mc(TRUE, TRUE);
      break;

    case 2:
      ic_printf("Test 2: Executing Unit test of Simple Dynamic Array");
      ret_code= unit_test_simple_dynamic_array();
      break;
    case 3:
      ic_printf("Test 3: Executing Unit test of Ordered Dynamic Array");
      ret_code= unit_test_ordered_dynamic_array();
      break;
    case 4:
      ic_printf("Test 4: Executing Unit test of Dynamic Ptr Array");
      ret_code= unit_test_dynamic_ptr_array();
      break;
    case 5:
      ic_printf("Test 5: Executing Unit test of Bitmap");
      ret_code= unit_test_bitmap();
      break;      
    case 6:
      ic_printf("Test 6: Executing Unit test of Hashtable");
      ret_code= unit_test_hashtable();
      break;
    case 7:
      ic_printf("Test 7: Executing unit test of Parse Connectstring");
      ret_code= unit_test_parse_connectstring();
      break;
    case 8:
      ic_printf("Test 8: Executing unit test of Socket Buffer");
      ret_code= unit_test_sock_buf();
      break;
    default:
      ret_code= 0;
      ic_require(FALSE);
      break;
  }
  return ret_code;
}

int main(int argc, char *argv[])
{
  int ret_code;
  guint32 i;

  if ((ret_code= ic_start_program(argc, argv, entries, NULL,
                                  glob_process_name,
           "- Unit test program", FALSE)))
    return ret_code;
  if (glob_test_type == 0)
  {
    for (i= 1; i < 9; i++)
    {
      if ((ret_code= run_test(i)))
        break;
    }
  }
  else
    ret_code= run_test(glob_test_type);
  ic_end();
  ic_printf("iClaustron Test return code: %d", ret_code);
  return ret_code;
}
