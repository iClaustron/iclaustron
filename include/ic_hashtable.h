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

/* Copyright (C) 2002 Christopher Clark <firstname.lastname@cl.cam.ac.uk> */

#ifndef __HASHTABLE_CWC22_H__
#define __HASHTABLE_CWC22_H__

#include <ic_base_header.h>
/* Example of use:
 *
 *      IC_HASHTABLE  *h;
 *      struct some_key      *k;
 *      struct some_value    *v;
 *
 *      static unsigned int         hash_from_key_fn( void *k );
 *      static int                  keys_equal_fn ( void *key1, void *key2 );
 *
 *      h = ic_create_hashtable(16, hash_from_key_fn, keys_equal_fn, FALSE);
 *      k = (struct some_key *)     malloc(sizeof(struct some_key));
 *      v = (struct some_value *)   malloc(sizeof(struct some_value));
 *
 *      (initialise k and v to suitable values)
 * 
 *      if (! ic_hashtable_insert(h,k,v) )
 *      {     exit(-1);               }
 *
 *      if (NULL == (found = ic_hashtable_search(h,k) ))
 *      {    printf("not found!");                  }
 *
 *      if (NULL == (found = ic_hashtable_remove(h,k) ))
 *      {    printf("Not found\n");                 }
 *
 */

/* Macros may be used to define type-safe(r) hashtable access functions, with
 * methods specialized to take known key and value types as parameters.
 * 
 * Example:
 *
 * Insert this at the start of your file:
 *
 * DEFINE_HASHTABLE_INSERT(insert_some, struct some_key, struct some_value);
 * DEFINE_HASHTABLE_SEARCH(search_some, struct some_key, struct some_value);
 * DEFINE_HASHTABLE_REMOVE(remove_some, struct some_key, struct some_value);
 *
 * This defines the functions 'insert_some', 'search_some' and 'remove_some'.
 * These operate just like ic_hashtable_insert etc., with the same parameters,
 * but their function signatures have 'struct some_key *' rather than
 * 'void *', and hence can generate compile time errors if your program is
 * supplying incorrect data as a key (and similarly for value).
 *
 * Note that the hash and key equality functions passed to ic_create_hashtable
 * still take 'void *' parameters instead of 'some key *'. This shouldn't be
 * a difficult issue as they're only defined and passed once, and the other
 * functions will ensure that only valid keys are supplied to them.
 *
 * The cost for this checking is increased code size and runtime overhead
 * - if performance is important, it may be worth switching back to the
 * unsafe methods once your program has been debugged with the safe methods.
 * This just requires switching to some simple alternative defines - eg:
 * #define insert_some ic_hashtable_insert
 *
 */

unsigned int ic_hash_str(void *ptr);
unsigned int ic_hash_ptr(void *ptr);
unsigned int ic_hash_uint64(void *key);

int ic_keys_equal_str(void *ptr1, void *ptr2);
int ic_keys_equal_ptr(void *ptr1, void *ptr2);
int ic_keys_equal_uint64(void *key1, void *key2);

/*****************************************************************************
 * ic_create_hashtable
   
 * @name                    ic_create_hashtable
 * @param   minsize         minimum initial size of ic_hashtable
 * @param   hashfunction    function for hashing keys
 * @param   key_eq_fn       function for determining key equality
 * @param   initial         always set to FALSE except from ic_port_init/end
 * @return                  newly created ic_hashtable or NULL on failure
 */

IC_HASHTABLE*
ic_create_hashtable(unsigned int minsize,
                    unsigned int (*hashfunction) (void*),
                    int (*key_eq_fn) (void*,void*),
                    gboolean initial);

/*****************************************************************************
 * ic_hashtable_insert
   
 * @name        ic_hashtable_insert
 * @param   h   the ic_hashtable to insert into
 * @param   k   the key - ic_hashtable claims ownership and will free on removal
 * @param   v   the value - does not claim ownership
 * @return      zero for successful insertion
 *
 * This function will cause the table to expand if the insertion would take
 * the ratio of entries to table size over the maximum load factor.
 *
 * This function does not check for repeated insertions with a duplicate key.
 * The value returned when using a duplicate key is undefined -- when
 * the ic_hashtable changes size, the order of retrieval of duplicate key
 * entries is reversed.
 * If in doubt, remove before insert.
 */

int 
ic_hashtable_insert(IC_HASHTABLE *h, void *k, void *v);

#define DEFINE_HASHTABLE_INSERT(fnname, keytype, valuetype) \
int fnname (IC_HASHTABLE *h, keytype *k, valuetype *v) \
{ \
    return ic_hashtable_insert(h,k,v); \
}

/*****************************************************************************
 * ic_hashtable_search
   
 * @name        ic_hashtable_search
 * @param   h   the ic_hashtable to search
 * @param   k   the key to search for  - does not claim ownership
 * @return      the value associated with the key, or NULL if none found
 */

void *
ic_hashtable_search(IC_HASHTABLE *h, void *k);

#define DEFINE_HASHTABLE_SEARCH(fnname, keytype, valuetype) \
valuetype * fnname (IC_HASHTABLE *h, keytype *k) \
{ \
    return (valuetype *) (ic_hashtable_search(h,k)); \
}

/*****************************************************************************
 * ic_hashtable_remove
   
 * @name        ic_hashtable_remove
 * @param   h   the ic_hashtable to remove the item from
 * @param   k   the key to search for  - does not claim ownership
 * @return      the value associated with the key, or NULL if none found
 */

void * /* returns value */
ic_hashtable_remove(IC_HASHTABLE *h, void *k);

#define DEFINE_HASHTABLE_REMOVE(fnname, keytype, valuetype) \
valuetype * fnname (IC_HASHTABLE *h, keytype *k) \
{ \
    return (valuetype *) (ic_hashtable_remove(h,k)); \
}


/*****************************************************************************
 * ic_hashtable_count
   
 * @name        ic_hashtable_count
 * @param   h   the ic_hashtable
 * @return      the number of items stored in the ic_hashtable
 */
unsigned int
ic_hashtable_count(IC_HASHTABLE *h);


/*****************************************************************************
 * ic_hashtable_destroy
   
 * @name               ic_hashtable_destroy
 * @param   h          the ic_hashtable
 * @param   initial    Always set to FALSE except by ic_port_init/ic_port_end
 */

void
ic_hashtable_destroy(IC_HASHTABLE *h,
                     gboolean initial);

#endif /* __HASHTABLE_CWC22_H__ */

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
