/* Copyright (C) 2007-2010 iClaustron AB

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

/* Copyright (C) 2002, 2004 Christopher Clark  <firstname.lastname@cl.cam.ac.uk> */

#include <ic_hashtable.h>
#include <ic_hashtable_private.h>
#include <ic_hashtable_itr.h>
#include <stdlib.h> /* defines NULL */

/*****************************************************************************/
/* ic_hashtable_iterator    - iterator constructor */

struct ic_hashtable_itr *
ic_hashtable_iterator(struct ic_hashtable *h)
{
    unsigned int i, tablelength;
    struct ic_hashtable_itr *itr = (struct ic_hashtable_itr *)
        malloc(sizeof(struct ic_hashtable_itr));
    if (NULL == itr) return NULL;
    itr->h = h;
    itr->e = NULL;
    itr->parent = NULL;
    tablelength = h->tablelength;
    itr->index = tablelength;
    if (0 == h->entrycount) return itr;

    for (i = 0; i < tablelength; i++)
    {
        if (NULL != h->table[i])
        {
            itr->e = h->table[i];
            itr->index = i;
            break;
        }
    }
    return itr;
}

/*****************************************************************************/
/* key      - return the key of the (key,value) pair at the current position */
/* value    - return the value of the (key,value) pair at the current position */


/*****************************************************************************/
/* advance - advance the iterator to the next element
 *           returns zero if advanced to end of table */

int
ic_hashtable_iterator_advance(struct ic_hashtable_itr *itr)
{
    unsigned int j,tablelength;
    struct entry **table;
    struct entry *next;
    if (NULL == itr->e) return 0; /* stupidity check */

    next = itr->e->next;
    if (NULL != next)
    {
        itr->parent = itr->e;
        itr->e = next;
        return -1;
    }
    tablelength = itr->h->tablelength;
    itr->parent = NULL;
    if (tablelength <= (j = ++(itr->index)))
    {
        itr->e = NULL;
        return 0;
    }
    table = itr->h->table;
    while (NULL == (next = table[j]))
    {
        if (++j >= tablelength)
        {
            itr->index = tablelength;
            itr->e = NULL;
            return 0;
        }
    }
    itr->index = j;
    itr->e = next;
    return -1;
}

/*****************************************************************************/
/* remove - remove the entry at the current iterator position
 *          and advance the iterator, if there is a successive
 *          element.
 *          If you want the value, read it before you remove:
 *          beware memory leaks if you don't.
 *          Returns zero if end of iteration. */

int
ic_hashtable_iterator_remove(struct ic_hashtable_itr *itr)
{
    struct entry *remember_e, *remember_parent;
    int ret;

    /* Do the removal */
    if (NULL == (itr->parent))
    {
        /* element is head of a chain */
        itr->h->table[itr->index] = itr->e->next;
    } else {
        /* element is mid-chain */
        itr->parent->next = itr->e->next;
    }
    /* itr->e is now outside the ic_hashtable */
    remember_e = itr->e;
    itr->h->entrycount--;
    freekey(remember_e->k);

    /* Advance the iterator, correcting the parent */
    remember_parent = itr->parent;
    ret = ic_hashtable_iterator_advance(itr);
    if (itr->parent == remember_e) { itr->parent = remember_parent; }
    free(remember_e);
    return ret;
}

/*****************************************************************************/
int /* returns zero if not found */
ic_hashtable_iterator_search(struct ic_hashtable_itr *itr,
                             struct ic_hashtable *h, void *k)
{
    struct entry *e, *parent;
    unsigned int hashvalue, index;

    hashvalue = hash(h,k);
    index = indexFor(h->tablelength,hashvalue);

    e = h->table[index];
    parent = NULL;
    while (NULL != e)
    {
        /* Check hash value to short circuit heavier comparison */
        if ((hashvalue == e->h) && (h->eqfn(k, e->k)))
        {
            itr->index = index;
            itr->e = e;
            itr->parent = parent;
            itr->h = h;
            return -1;
        }
        parent = e;
        e = e->next;
    }
    return 0;
}


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

