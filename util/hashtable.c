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

/* Copyright (C) 2004 Christopher Clark <firstname.lastname@cl.cam.ac.uk> */

#include <hashtable.h>
#include <hashtable_private.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ic_common.h>
/*
Credit for primes table: Aaron Krowne
 http://br.endernet.org/~akrowne/
 http://planetmath.org/encyclopedia/GoodHashTablePrimes.html
*/
static const unsigned int primes[] = {
53, 97, 193, 389,
769, 1543, 3079, 6151,
12289, 24593, 49157, 98317,
196613, 393241, 786433, 1572869,
3145739, 6291469, 12582917, 25165843,
50331653, 100663319, 201326611, 402653189,
805306457, 1610612741
};
const unsigned int prime_table_length = sizeof(primes)/sizeof(primes[0]);
const float max_load_factor = 0.65;

int
ic_keys_equal_str(void *ptr1, void *ptr2)
{
  IC_STRING *str1= (IC_STRING*)ptr1;
  IC_STRING *str2= (IC_STRING*)ptr2;
  if (str1->len != str2->len)
    return 0;
  return strncmp((const char*)str1->str,
                 (const char*)str2->str,
                 str1->len) ? 0 : 1;
}

unsigned int
ic_hash_str(void *ptr)
{
  unsigned int hash= 0;
  IC_STRING *str= (IC_STRING*)ptr;
  unsigned char *char_ptr= (unsigned char*)str->str;
  unsigned len= str->len;
  unsigned int i;
  for (i= 0; i < len; i++)
  {
    hash= *char_ptr + (147*hash) + 5;
    char_ptr++;
  }
  return hash;
}

int
ic_keys_equal_ptr(void *ptr1, void *ptr2)
{
  return (ptr1 == ptr2) ? 0 : 1;
}

unsigned int
ic_hash_ptr(void *ptr)
{
  unsigned int len= sizeof(guint32);
  unsigned int char_val, i;
  unsigned int hash= 0;
  guint32 val= (guint32)ptr;
  for (i= 0; i < len; i++)
  {
    char_val= val & 255;
    val= val >> 8;
    hash= char_val + (147*hash) + 5;
  }
  return hash;
}

/*****************************************************************************/
struct ic_hashtable *
ic_create_hashtable(unsigned int minsize,
                    unsigned int (*hashf) (void*),
                    int (*eqf) (void*,void*))
{
    struct ic_hashtable *h;
    unsigned int pindex, size = primes[0];
    /* Check requested ic_hashtable isn't too large */
    if (minsize > (1u << 30)) return NULL;
    /* Enforce size as prime */
    for (pindex=0; pindex < prime_table_length; pindex++) {
        if (primes[pindex] > minsize) { size = primes[pindex]; break; }
    }
    h = (struct ic_hashtable *)malloc(sizeof(struct ic_hashtable));
    if (NULL == h) return NULL; /*oom*/
    h->table = (struct entry **)malloc(sizeof(struct entry*) * size);
    if (NULL == h->table) { free(h); return NULL; } /*oom*/
    memset(h->table, 0, size * sizeof(struct entry *));
    h->tablelength  = size;
    h->primeindex   = pindex;
    h->entrycount   = 0;
    h->hashfn       = hashf;
    h->eqfn         = eqf;
    h->loadlimit    = (unsigned int) ceil(size * max_load_factor);
    return h;
}

/*****************************************************************************/
unsigned int
hash(struct ic_hashtable *h, void *k)
{
    /* Aim to protect against poor hash functions by adding logic here
     * - logic taken from java 1.4 ic_hashtable source */
    unsigned int i = h->hashfn(k);
    i += ~(i << 9);
    i ^=  ((i >> 14) | (i << 18)); /* >>> */
    i +=  (i << 4);
    i ^=  ((i >> 10) | (i << 22)); /* >>> */
    return i;
}

/*****************************************************************************/
static int
ic_hashtable_expand(struct ic_hashtable *h)
{
  /* Double the size of the table to accomodate more entries */
  struct entry **newtable;
  struct entry *e;
  struct entry **pE;
  unsigned int newsize, i, index;
  /* Check we're not hitting max capacity */
  if (h->primeindex == (prime_table_length - 1))
    return 0;
  newsize = primes[++(h->primeindex)];

  newtable = (struct entry **)malloc(sizeof(struct entry*) * newsize);
  if (NULL != newtable)
  {
    memset(newtable, 0, newsize * sizeof(struct entry *));
    /* This algorithm is not 'stable'. ie. it reverses the list
     * when it transfers entries between the tables */
    for (i = 0; i < h->tablelength; i++)
    {
      while (NULL != (e = h->table[i]))
      {
        h->table[i] = e->next;
        index = indexFor(newsize,e->h);
        e->next = newtable[index];
        newtable[index] = e;
      }
    }
    free(h->table);
    h->table = newtable;
  }
  /* Plan B: realloc instead */
  else 
  {
    newtable = (struct entry **)
               realloc(h->table, newsize * sizeof(struct entry *));
    if (NULL == newtable)
    {
      (h->primeindex)--;
      return 0;
    }
    h->table = newtable;
    memset(newtable[h->tablelength], 0, newsize - h->tablelength);
    for (i = 0; i < h->tablelength; i++)
    {
      for (pE = &(newtable[i]), e = *pE; e != NULL; e = *pE)
      {
        index = indexFor(newsize,e->h);
        if (index == i)
        {
          pE = &(e->next);
        }
        else
        {
          *pE = e->next;
          e->next = newtable[index];
          newtable[index] = e;
        }
      }
    }
  }
  h->tablelength = newsize;
  h->loadlimit   = (unsigned int) ceil(newsize * max_load_factor);
  return -1;
}

/*****************************************************************************/
unsigned int
ic_hashtable_count(struct ic_hashtable *h)
{
  return h->entrycount;
}

/*****************************************************************************/
int
ic_hashtable_insert(struct ic_hashtable *h, void *k, void *v)
{
  /* This method allows duplicate keys - but they shouldn't be used */
  unsigned int index;
  struct entry *e;
  if (++(h->entrycount) > h->loadlimit)
  {
    /* Ignore the return value. If expand fails, we should
     * still try cramming just this value into the existing table
     * -- we may not have memory for a larger table, but one more
     * element may be ok. Next time we insert, we'll try expanding again.*/
    ic_hashtable_expand(h);
  }
  e = (struct entry *)malloc(sizeof(struct entry));
  if (NULL == e) { --(h->entrycount); return 1; } /*oom*/
  e->h = hash(h,k);
  index = indexFor(h->tablelength,e->h);
  e->k = k;
  e->v = v;
  e->next = h->table[index];
  h->table[index] = e;
  return 0;
}

/*****************************************************************************/
void * /* returns value associated with key */
ic_hashtable_search(struct ic_hashtable *h, void *k)
{
  struct entry *e;
  unsigned int hashvalue, index;
  hashvalue = hash(h,k);
  index = indexFor(h->tablelength,hashvalue);
  e = h->table[index];
  while (NULL != e)
  {
    /* Check hash value to short circuit heavier comparison */
    if ((hashvalue == e->h) && (h->eqfn(k, e->k)))
      return e->v;
    e = e->next;
  }
  return NULL;
}

/*****************************************************************************/
void * /* returns value associated with key */
ic_hashtable_remove(struct ic_hashtable *h, void *k)
{
  /* TODO: consider compacting the table when the load factor drops enough,
   *       or provide a 'compact' method. */

  struct entry *e;
  struct entry **pE;
  void *v;
  unsigned int hashvalue, index;

  hashvalue = hash(h,k);
  index = indexFor(h->tablelength,hash(h,k));
  pE = &(h->table[index]);
  e = *pE;
  while (NULL != e)
  {
    /* Check hash value to short circuit heavier comparison */
    if ((hashvalue == e->h) && (h->eqfn(k, e->k)))
    {
      *pE = e->next;
      h->entrycount--;
      v = e->v;
      free(e);
      return v;
    }
    pE = &(e->next);
    e = e->next;
  }
  return NULL;
}

/*****************************************************************************/
/* destroy */
void
ic_hashtable_destroy(struct ic_hashtable *h)
{
  unsigned int i;
  struct entry *e, *f;
  struct entry **table = h->table;
  for (i = 0; i < h->tablelength; i++)
  {
    e = table[i];
    while (NULL != e)
    {
      f = e;
      e = e->next;
      free(f);
    }
  }
  free(h->table);
  free(h);
}

/*
 * Copyright (c) 2002, Christopher Clark
 * Copyright (c) 2007, iClaustron AB
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

