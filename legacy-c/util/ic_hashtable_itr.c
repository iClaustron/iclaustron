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

/*
  Copyright (C) 2002, 2004 Christopher Clark <firstname.lastname@cl.cam.ac.uk>
*/

#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_hashtable.h>
#include <ic_hashtable_private.h>
#include <ic_hashtable_itr.h>

/*****************************************************************************/
/* ic_hashtable_iterator    - iterator constructor */

IC_HASHTABLE_ITR*
ic_hashtable_iterator(IC_HASHTABLE *h,
                      IC_HASHTABLE_ITR *itr,
                      gboolean from_start)
{
  itr->h = h;
  itr->e = NULL;
  if (from_start)
    itr->index= 0;
  return itr;
}

/*****************************************************************************/
/* advance - advance the iterator to the next element
 *           returns zero if advanced to end of table */

int
ic_hashtable_iterator_advance(IC_HASHTABLE_ITR *itr)
{
  unsigned int i, j,tablelength;
  IC_HASH_ENTRY **table;
  IC_HASH_ENTRY *next;
  IC_HASHTABLE *h= itr->h;

  if (NULL == itr->e)
  {
    /* First call to advance */
    tablelength = h->tablelength;
    for (i = itr->index; i < tablelength; i++)
    {
      if (NULL != h->table[i])
      {
        itr->e = h->table[i];
        itr->index = i;
        return -1;
      }
    }
    ic_require(h->entrycount == 0);
    return 0;
  }

  next = itr->e->next;
  if (NULL != next)
  {
    itr->e = next;
    return -1;
  }
  tablelength = itr->h->tablelength;
  j= ++itr->index;
  if (j > tablelength)
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

/*
 * Copyright (c) 2002, 2004, Christopher Clark
 * Copyright (c) 2007-2011 iClaustron AB
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
