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

/* Start schema transaction messages */
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
  sizeof(IC_SCHEMA_TRANS_BEGIN_REQ)/sizeof(guint32);

typedef struct ic_schema_trans_begin_conf IC_SCHEMA_TRANS_BEGIN_CONF;
struct ic_schema_trans_begin_conf
{
  guint32 ndb_reference;
  guint32 my_transaction_id;
  guint32 ndb_transaction_id;
};
static const int SCHEMA_TRANS_BEGIN_CONF_LEN= 
  sizeof(IC_SCHEMA_TRANS_BEGIN_CONF)/sizeof(guint32);

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
static const int SCHEMA_TRANS_BEGIN_REF_LEN= 
  sizeof(IC_SCHEMA_TRANS_BEGIN_REF)/sizeof(guint32);

/* Commit/Abort schema transaction messages */
static const int SCHEMA_TRANS_END_REQ_GSN = 734;
static const int SCHEMA_TRANS_END_CONF_GSN = 735;
static const int SCHEMA_TRANS_END_REF_GSN = 736;
static const int SCHEMA_TRANS_END_REP_GSN = 768;

typedef struct ic_schema_trans_end_req IC_SCHEMA_TRANS_END_REQ;
struct ic_schema_trans_end_req
{
  guint32 my_reference;
  guint32 my_transaction_id;
  guint32 request_info;
  guint32 ndb_transaction_id;
  guint32 flags;
};
static const int SCHEMA_TRANS_END_REQ_LEN= 
  sizeof(IC_SCHEMA_TRANS_END_REQ)/sizeof(guint32);

/* Constants for flags */
static const int SCHEMA_TRANS_END_ABORT= 1;
static const int SCHEMA_TRANS_END_BACKGROUND= 2;
static const int SCHEMA_TRANS_END_PREPARE= 4;

typedef struct ic_schema_trans_end_conf IC_SCHEMA_TRANS_END_CONF;
struct ic_schema_trans_end_conf
{
  guint32 ndb_reference;
  guint32 my_transaction_id;
};
static const int SCHEMA_TRANS_END_CONF_LEN= 
  sizeof(IC_SCHEMA_TRANS_END_CONF)/sizeof(guint32);

/*
  Same signal content in SCHEMA_TRANS_END_REF as in SCHEMA_TRANS_BEGIN_REF.
  So no need to define yet another struct.
*/
typedef struct ic_schema_trans_begin_ref IC_SCHEMA_TRANS_END_REF;
static const int SCHEMA_TRANS_END_REF_LEN= 
  sizeof(IC_SCHEMA_TRANS_END_REF)/sizeof(guint32);

/* Create hashmap messages */
static const int CREATE_HASH_MAP_REQ_GSN = 297;
static const int CREATE_HASH_MAP_CONF_GSN = 299;
static const int CREATE_HASH_MAP_REF_GSN = 298;

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
static const int CREATE_HASH_MAP_REQ_LEN= 
  sizeof(IC_CREATE_HASH_MAP_REQ)/sizeof(guint32);

/* Flag constants */
static const int CREATE_HASH_MAP_IF_NOT_EXISTS= 1;
static const int CREATE_HASH_MAP_DEFAULT= 2;
static const int CREATE_HASH_MAP_REORG= 4;

typedef struct ic_create_hash_map_conf IC_CREATE_HASH_MAP_CONF;
struct ic_create_hash_map_conf
{
  guint32 ndb_reference;
  guint32 ndb_data;
  guint32 ndb_object_id;
  guint32 ndb_object_version;
  guint32 my_transaction_id;
};
static const int CREATE_HASH_MAP_CONF_LEN= 
  sizeof(IC_CREATE_HASH_MAP_CONF)/sizeof(guint32);

static const int CREATE_TABLE_REQ_GSN = 587;
static const int CREATE_TABLE_CONF_GSN = 589;
static const int CREATE_TABLE_REF_GSN = 588;

typedef struct ic_create_table_req IC_CREATE_TABLE_REQ;
struct ic_create_table_req
{
  guint32 my_reference;
  guint32 my_data;
  guint32 flags;
  guint32 my_transaction_id;
  guint32 ndb_transaction_id;
};
static const int CREATE_TABLE_REQ_LEN= 
  sizeof(IC_CREATE_TABLE_REQ)/sizeof(guint32);

/**
  CREATE_TABLE_REQ also contains one segment containing
  the information about the table to be created. The signal could
  also be a fragmented containing more table information than
  what fits in one signal.
*/

typedef struct ic_create_table_conf IC_CREATE_TABLE_CONF;
struct ic_create_table_conf
{
  guint32 ndb_reference;
  guint32 ndb_data;
  guint32 my_transaction_id;
  guint32 table_id;
  guint32 table_version;
};
static const int CREATE_TABLE_CONF_LEN= 
  sizeof(IC_CREATE_TABLE_CONF)/sizeof(guint32);

typedef struct ic_create_table_ref IC_CREATE_TABLE_REF;
struct ic_create_table_ref
{
  guint32 ndb_reference;
  guint32 ndb_data;
  guint32 my_transaction_id;
  guint32 error_code;
  guint32 error_line;
  guint32 error_node_id;
  guint32 master_node_id;
  guint32 error_status;
  guint32 error_key;
};
static const int CREATE_TABLE_REF_LEN= 
  sizeof(IC_CREATE_TABLE_REF)/sizeof(guint32);

static const int GET_TABINFO_REQ_GSN= 24;
static const int GET_TABINFO_CONF_GSN= 190;
static const int GET_TABINFO_REF_GSN= 23;

typedef struct ic_get_tabinfo_req IC_GET_TABINFO_REQ;
struct ic_get_tabinfo_req
{
  guint32 my_reference;
  guint32 my_data;
  guint32 request_type;
  union {
    guint32 table_name_len; /* if request by name */
    guint32 table_id;       /* if request by id */
  };
  /*
    Through setting my_transaction_id we are able to see our own
    changes before they are committed.
  */
  guint32 my_transaction_id;
};
static const int GET_TABINFO_REQ_LEN=
  sizeof(IC_GET_TABINFO_REQ)/sizeof(guint32);

static const int GET_TABINFO_BY_NAME= 1;
static const int GET_TABINFO_BY_ID= 2;
static const int GET_TABINFO_LONG_SIGNAL_CONF= 4;

/**
  If GET_TABINFO_BY_NAME is set then GET_TABINFO_REQ contains one segment
  that contains the table name.
*/

typedef struct ic_get_tabinfo_conf IC_GET_TABINFO_CONF;
struct ic_get_tabinfo_conf
{
  guint32 ndb_data;
  guint32 table_id;
  guint32 table_create_gci;
  guint32 total_len_of_info;
  guint32 table_type;
  guint32 ndb_reference;
};
static const int GET_TABINFO_CONF_LEN=
  sizeof(IC_GET_TABINFO_CONF)/sizeof(guint32);

/**
  In addition GET_TABINFO_CONF also contains a segment with all the
  table info. This segment can also be sent as a fragmented message
  in multiple signals.
*/

typedef struct ic_get_tabinfo_ref IC_GET_TABINFO_REF;
struct ic_get_tabinfo_ref
{
  guint32 ndb_data;
  guint32 ndb_reference;
  guint32 request_type; /* Same as for GET_TABINFO_REQ */
  union {
    guint32 table_name_len; /* if request by name */
    guint32 table_id;       /* if request by id */
  };
  guint32 my_transaction_id;
  guint32 error_code;
  guint32 error_line;
};
static const int GET_TABINFO_REF_LEN=
  sizeof(IC_GET_TABINFO_REF)/sizeof(guint32);

static const int LIST_TABLES_REQ_GSN= 193;
static const int LIST_TABLES_CONF_GSN= 194;

typedef struct ic_list_tables_req IC_LIST_TABLES_REQ;
struct ic_list_tables_req
{
  guint32 my_data;
  guint32 my_reference;
  guint32 flags;
  guint32 table_id;
  guint32 table_type;
};
static const int LIST_TABLES_REQ_LEN=
  sizeof(IC_LIST_TABLES_REQ)/sizeof(guint32);

typedef struct ic_list_tables_conf IC_LIST_TABLES_CONF;
struct ic_list_tables_conf
{
  guint32 my_data;
  guint32 num_tables;
};
static const int LIST_TABLES_CONF_LEN=
  sizeof(IC_LIST_TABLES_CONF)/sizeof(guint32);

/**
  The LIST_TABLES_CONF has two segments, the first contains table data
  and the second segment contains a list of table names.
*/
