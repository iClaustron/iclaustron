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

/* Common error codes for schema transactions */
static const int SCHEMA_ERROR_BUSY= 701;
static const int SCHEMA_ERROR_NOT_MASTER= 702;
static const int SCHEMA_ERROR_NODE_RESTART= 711;

static const int SCHEMA_TRANS_END_REQ_GSN = 734;
static const int SCHEMA_TRANS_END_CONF_GSN = 735;
static const int SCHEMA_TRANS_END_REF_GSN = 736;
static const int SCHEMA_TRANS_END_REP_GSN = 768;

/* Start schema transaction */
static const int SCHEMA_TRANS_BEGIN_REQ_GSN = 731;
static const int SCHEMA_TRANS_BEGIN_CONF_GSN = 732;
static const int SCHEMA_TRANS_BEGIN_REF_GSN = 733;

typedef struct ic_schema_trans_begin_req IC_SCHEMA_TRANS_BEGIN_REQ;
struct ic_schema_trans_begin_req
{
  guint32 my_reference;
  guint32 my_transaction_id;
  guint32 flags;
};
static const int SCHEMA_TRANS_BEGIN_REQ_LEN=
  sizeof(IC_SCHEMA_TRANS_BEGIN_REQ)/4;

typedef struct ic_schema_trans_begin_conf IC_SCHEMA_TRANS_BEGIN_CONF;
struct ic_schema_trans_begin_conf
{
  guint32 ndb_reference;
  guint32 my_transaction_id;
  guint32 ndb_transaction_id;
};
static const int SCHEMA_TRANS_BEGIN_CONF_LEN= 3;

typedef struct ic_schema_trans_begin_ref IC_SCHEMA_TRANS_BEGIN_REF;
struct ic_schema_trans_begin_ref
{
  guint32 ndb_reference;
  guint32 my_transaction_id;
  guint32 error_code;
  guint32 error_line;
  guint32 error_node_id;
  guint32 master_node_id;
};
static const int SCHEMA_TRANS_BEGIN_REF_LEN= 6;

static const int CREATE_HASH_MAP_REQ_GSN = 297;
static const int CREATE_HASH_MAP_CONF_GSN = 299;
static const int CREATE_HASHMAP_REF_GSN = 298;

typedef struct ic_create_hash_map_req IC_CREATE_HASH_MAP_REQ;
struct ic_create_hash_map_req
{
  guint32 my_reference;
  guint32 my_data;
  guint32 ndb_transaction_id;
  guint32 my_transaction_id;
  guint32 flags;
  guint32 num_buckets;
  guint32 num_partitions;
};
static const int CREATE_HASH_MAP_REQ_LEN= 7;
static const int CREATE_HASH_MAP_IF_NOT_EXISTS= 1;
static const int CREATE_HASH_MAP_DEFAULT= 2;
static const int CREATE_HASH_MAP_REORG= 4;

static const int CREATE_TABLE_REQ_GSN = 587;
static const int CREATE_TABLE_CONF_GSN = 589;
static const int CREATE_TABLE_REF_GSN = 588;
