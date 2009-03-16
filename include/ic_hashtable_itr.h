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

/* Copyright (C) 2002, 2004 Christopher Clark <firstname.lastname@cl.cam.ac.uk> */

#ifndef __HASHTABLE_ITR_CWC22__
#define __HASHTABLE_ITR_CWC22__
#include "ic_hashtable.h"
#include "ic_hashtable_private.h" /* needed to enable inlining */

/*****************************************************************************/
/* This struct is only concrete here to allow the inlining of two of the
 * accessor functions. */
struct ic_hashtable_itr
{
    struct ic_hashtable *h;
    struct entry *e;
    struct entry *parent;
    unsigned int index;
};


/*****************************************************************************/
/* ic_hashtable_iterator
 */

struct ic_hashtable_itr *
ic_hashtable_iterator(struct ic_hashtable *h);

/*****************************************************************************/
/* ic_hashtable_iterator_key
 * - return the value of the (key,value) pair at the current position */

extern inline void *
ic_hashtable_iterator_key(struct ic_hashtable_itr *i)
{
    return i->e->k;
}

/*****************************************************************************/
/* value - return the value of the (key,value) pair at the current position */

extern inline void *
ic_hashtable_iterator_value(struct ic_hashtable_itr *i)
{
    return i->e->v;
}

/*****************************************************************************/
/* advance - advance the iterator to the next element
 *           returns zero if advanced to end of table */

int
ic_hashtable_iterator_advance(struct ic_hashtable_itr *itr);

/*****************************************************************************/
/* remove - remove current element and advance the iterator to the next element
 *          NB: if you need the value to free it, read it before
 *          removing. ie: beware memory leaks!
 *          returns zero if advanced to end of table */

int
ic_hashtable_iterator_remove(struct ic_hashtable_itr *itr);

/*****************************************************************************/
/* search - overwrite the supplied iterator, to point to the entry
 *          matching the supplied key.
            h points to the ic_hashtable to be searched.
 *          returns zero if not found. */
int
ic_hashtable_iterator_search(struct ic_hashtable_itr *itr,
                             struct ic_hashtable *h, void *k);

#define DEFINE_HASHTABLE_ITERATOR_SEARCH(fnname, keytype) \
int fnname (struct ic_hashtable_itr *i, struct ic_hashtable *h, keytype *k) \
{ \
    return (ic_hashtable_iterator_search(i,h,k)); \
}



#endif /* __HASHTABLE_ITR_CWC22__*/

/*
 * Copyright (c) 2002, 2004, Christopher Clark
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

