/* Copyright (C) 2009-2012 iClaustron AB

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

#define IC_INSERT_SLL(parent_object, object, name) \
  if ((parent_object)->first_##name) \
  { \
    (parent_object)->first_##name = (object); \
    (parent_object)->last_##name = (object); \
  } \
  else \
  { \
    (parent_object)->last_##name->next_##name = (object); \
  } \
  object->next_##name = NULL;

#define IC_REMOVE_FIRST_SLL(parent_object, name) \
  if ((parent_object->first_##name)) \
  { \
    (parent_object)->first_##name = \
      (parent_object)->first_##name->next_##name; \
    if (!(parent_object)->first_##name) \
      (parent_object)->last_##name= NULL; \
  } \
  else \
  { \
    ic_require(FALSE); \
  }

#define IC_GET_FIRST_SLL(parent_object, name) \
  ((parent_object)->first_##name)

#define IC_GET_NEXT_SLL(object, name) \
  ((object)->next_##name)

#define IC_INSERT_DLL(parent_object, object, name) \
  if ((parent_object)->first_##name) \
  { \
    (parent_object)->first_##name = (object); \
    (parent_object)->last_##name = (object); \
    (object)->prev_##name = NULL; \
  } \
  else \
  { \
    (parent_object)->last_##name->next_##name = (object); \
    (object)->prev_##name = (parent_object)->last_##name; \
    (parent_object)->last_##name = (object); \
  } \
  (object)->next_##name = NULL;

#define IC_REMOVE_DLL(parent_object, object, name) \
  if ((object)->next_##name) \
  { \
    if ((object)->prev_##name) \
    { \
      (object)->prev_##name->next_##name = (object)->next_##name; \
      (object)->next_##name->prev_##name = (object)->prev_##name; \
    } \
    else /* We are the first object */ \
    { \
      ic_assert((parent_object)->first_##name == (object)); \
      (parent_object)->first_##name = (object)->next_##name; \
    } \
  } \
  else /* We are the last object */ \
  { \
    ic_assert((parent_object)->last_##name == (object)); \
    (parent_object)->last_##name = (object)->prev_##name; \
    if ((object)->prev_##name) \
    { \
      (object)->prev_##name->next_##name = NULL; \
    } \
    else /* We are the only object */ \
    { \
      ic_assert((parent_object)->first_##name == (object)); \
      (parent_object)->first_##name = NULL; \
    } \
  } \
  (object)->next_##name = NULL; \
  (object)->prev_##name = NULL;

#define IC_GET_FIRST_DLL(parent_object, name) \
  ((parent_object)->first_##name)

#define IC_GET_NEXT_DLL(object, name) \
  ((object)->next_##name)

#define IC_GET_PREV_DLL(object, name) \
  ((object)->prev_##name)
