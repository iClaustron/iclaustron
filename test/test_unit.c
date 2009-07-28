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

static int
test_translation_object(IC_DYNAMIC_TRANSLATION *dyn_trans,
                        IC_TEST_DYN_TRANS *test_dyn_trans,
                        guint32 num_inserts,
                        guint32 num_removes)
{
  guint32 i;
  int ret_code;
  void *ret_object;

  for (i= 0; i < num_inserts; i++) 
  {
    if ((ret_code= dyn_trans->dt_ops.ic_insert_translation_object(
                       dyn_trans,
                       &test_dyn_trans[i].index,
                       (void*)&test_dyn_trans[i].object)))
      goto error;
    test_dyn_trans[i].in_dyn_trans= TRUE;
  }
  for (i= 0; i < num_removes; i++)
  {
     if ((ret_code= dyn_trans->dt_ops.ic_remove_translation_object(
                       dyn_trans,
                       test_dyn_trans[i].index,
                       (void*)&test_dyn_trans[i].object)))
       goto error;
     test_dyn_trans[i].in_dyn_trans= FALSE;
  }
  for (i= num_removes; i < num_inserts; i++)
  {
    if ((ret_code= dyn_trans->dt_ops.ic_get_translation_object(
                       dyn_trans,
                       test_dyn_trans[i].index,
                       (void**)&ret_object)))
      goto error;
    if (ret_object != (void*)&test_dyn_trans[i].object)
      return 1;
  }
  for (i= 0; i < num_removes; i++)
  {
    if (!(ret_code= dyn_trans->dt_ops.ic_get_translation_object(
                       dyn_trans,
                       test_dyn_trans[i].index,
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
init_test_dyn_trans(IC_TEST_DYN_TRANS *test_dyn_trans,
                    guint32 num_inserts)
{
  guint32 i;

  for (i= 0; i < num_inserts; i++)
    test_dyn_trans[i].object= i;
}

static int
test_dynamic_translation(guint32 num_inserts, guint32 num_removes)
{
  int ret_code;
  IC_DYNAMIC_TRANSLATION *dyn_trans;
  IC_TEST_DYN_TRANS *test_dyn_trans= (IC_TEST_DYN_TRANS*)
    ic_calloc(sizeof(IC_TEST_DYN_TRANS)*num_inserts);

  ic_printf("Testing with %u number of inserts and %u number of removes",
            num_inserts, num_removes);
  dyn_trans= ic_create_dynamic_translation();
  if (dyn_trans == NULL)
  {
    ic_free(test_dyn_trans);
    return IC_ERROR_MEM_ALLOC;
  }
  if (!test_dyn_trans)
    abort();
  init_test_dyn_trans(test_dyn_trans, num_inserts);
  if ((ret_code= test_translation_object(dyn_trans,
                                         test_dyn_trans,
                                         num_inserts,
                                         num_removes)))
    goto error;
error:
  dyn_trans->dt_ops.ic_free_dynamic_translation(dyn_trans);
  ic_free(test_dyn_trans);
  return ret_code;
}

static int
unit_test_dynamic_translation()
{
  int ret_code;
  if ((ret_code= test_dynamic_translation(128, 2)))
    goto error;
  if ((ret_code= test_dynamic_translation(129, 2)))
    goto error;
  if ((ret_code= test_dynamic_translation(128*128, 2)))
    goto error;
  if ((ret_code= test_dynamic_translation(128*128+1, 2)))
    goto error;
  if ((ret_code= test_dynamic_translation(128*128*128, 2)))
    goto error;
  if ((ret_code= test_dynamic_translation(128*128*128+1, 2)))
    goto error;
  if ((ret_code= test_dynamic_translation(128*128*128+1, 128*128*128+1)))
    goto error;
  return 0;
error:
  return ret_code;
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

static unsigned int
hash_function(void *key)
{
  gchar *val_str= (gchar*)key;
  unsigned int hash_value= 23;
  guint32 i;

  for (i= 0; i < 8; i++)
    hash_value= ((147*hash_value) + val_str[i]);
  return hash_value;
}

static int
equal_function(void *key1, void* key2)
{
  guint64 *val_ptr1= (guint64*)key1;
  guint64 *val_ptr2= (guint64*)key2;
  guint64 val1= *val_ptr1;
  guint64 val2= *val_ptr2;

  if (val1 == val2)
    return 1;
  return 0;
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
test_hashtable(guint32 num_inserts, guint32 num_removes)
{
  int ret_code;
  IC_HASHTABLE *hashtable;
  IC_TEST_HASHTABLE *test_hashtable= (IC_TEST_HASHTABLE*)
    ic_calloc(sizeof(IC_TEST_HASHTABLE)*num_inserts);

  ic_printf("Testing with %u number of inserts and %u number of removes",
            num_inserts, num_removes);
  hashtable= ic_create_hashtable(4096, hash_function, equal_function);
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
  ic_hashtable_destroy(hashtable);
  ic_free(test_hashtable);
  return ret_code;
}

static int
unit_test_hashtable()
{
  int ret_code;
  if ((ret_code= test_hashtable(128, 2)))
    goto error;
  if ((ret_code= test_hashtable(129, 2)))
    goto error;
  if ((ret_code= test_hashtable(128*128, 2)))
    goto error;
  if ((ret_code= test_hashtable(128*128+1, 2)))
    goto error;
  if ((ret_code= test_hashtable(128*128*128, 2)))
    goto error;
  if ((ret_code= test_hashtable(128*128*128+1, 2)))
    goto error;
  if ((ret_code= test_hashtable(128*128*128+1, 128*128*128+1)))
    goto error;
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

                         
int main(int argc, char *argv[])
{
  int ret_code;

  if ((ret_code= ic_start_program(argc, argv, entries, NULL,
                                  glob_process_name,
           "- Unit test program", FALSE)))
    return ret_code;
  switch (glob_test_type)
  {
    case 0:
      ic_printf("Executing Unit test of Memory Container");
      ret_code= unit_test_mc();
      break;

    case 1:
      ic_printf("Executing Unit test of Simple Dynamic Array");
      ret_code= unit_test_simple_dynamic_array();
      break;
    case 2:
      ic_printf("Executing Unit test of Ordered Dynamic Array");
      ret_code= unit_test_ordered_dynamic_array();
      break;
    case 3:
      ic_printf("Executing Unit test of Dynamic translation");
      ret_code= unit_test_dynamic_translation();
      break;
    case 4:
      ic_printf("Executing Unit test of Bitmap");
      ret_code= unit_test_bitmap();
      break;      
    case 5:
      ic_printf("Executing Unit test of Hashtable");
      ret_code= unit_test_hashtable();
      break;
    case 6:
      ic_printf("Executing unit test of Parse Connectstring");
      ret_code= unit_test_parse_connectstring();
      break;
    default:
      break;
   }
   ic_end();
   ic_printf("iClaustron Test return code: %d", ret_code);
   return ret_code;
}
