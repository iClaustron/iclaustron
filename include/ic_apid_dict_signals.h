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
  also be fragmented, containing more table information than
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

static const guint32 IC_SIZE_UINT32= 4;
static const guint32 IC_UINT32_VALUE_TYPE= 0;
static const guint32 IC_STRING_VALUE_TYPE= 1;
static const guint32 IC_BINARY_VALUE_TYPE= 2;
static const guint32 IC_INVALID_VALUE_TYPE= 3;

/* Mandatory */
static const guint32 IC_TABLE_NAME_KEY= 1;

/* Mandatory in getting table info from kernel */
static const guint32 IC_TABLE_ID_KEY= 2;

/* Mandatory in getting table info from kernel */
static const guint32 IC_TABLE_VERSION_KEY= 3;

static const guint32 IC_TABLE_LOGGED_KEY= 4;
static const guint32 IC_TABLE_LOGGED_DEFAULT= 1;

/* Mandatory */
static const guint32 IC_NUM_PKEY_COL_KEY= 5;
static const guint32 IC_NUM_PKEY_COL_DEFAULT= 0;

/* Mandatory */
static const guint32 IC_NUM_COL_KEY= 6;
static const guint32 IC_NUM_COL_DEFAULT= 0;

/* Mandatory */
static const guint32 IC_NUM_NULL_COL_KEY= 7;
static const guint32 IC_NUM_NULL_COL_DEFAULT= 0;

/* Always 0, never changed */
static const guint32 IC_NUM_VARIABLE_COL_KEY= 8;
static const guint32 IC_NUM_VARIABLE_COL_DEFAULT= 0;

/* Always 6, never changed */
static const guint32 IC_TABLE_K_VALUE_KEY= 9;
static const guint32 IC_TABLE_K_VALUE_DEFAULT= 6;

static const guint32 IC_MIN_LOAD_FACTOR_KEY= 10;
static const guint32 IC_MIN_LOAD_FACTOR_DEFAULT= 78;

static const guint32 IC_MAX_LOAD_FACTOR_KEY= 11;
static const guint32 IC_MAX_LOAD_FACTOR_DEFAULT= 80;
static const guint32 IC_MAX_LOAD_FACTOR_OUR_DEFAULT= 90;

static const guint32 IC_PKEY_LEN_KEY= 12;
static const guint32 IC_PKEY_LEN_DEFAULT= 0;

static const guint32 IC_FRAGMENT_TYPE_KEY= 13;
/* Hashmap partition */
static const guint32 IC_FRAGMENT_TYPE_DEFAULT= 9;

static const guint32 IC_TABLE_TYPE_KEY= 18;
/* User Table */
static const guint32 IC_TABLE_TYPE_DEFAULT= 2;

/* Only used for indexes, defaults to empty string */
static const guint32 IC_PRIMARY_TABLE_KEY= 19;
/* Only used for indexes, defaults to RNIL */
static const guint32 IC_PRIMARY_TABLE_ID_KEY= 20;

/* IndexState defaults to -1 */
static const guint32 IC_INDEX_STATE_KEY= 21;

/* Trigger ids defaults to RNIL */
static const guint32 IC_INSERT_TRIGGER_ID_KEY= 22;
static const guint32 IC_UPDATE_TRIGGER_ID_KEY= 23;
static const guint32 IC_DELETE_TRIGGER_ID_KEY= 24;
static const guint32 IC_CUSTOM_TRIGGER_ID_KEY= 25;

static const guint32 IC_FRM_LEN_KEY= 26;
static const guint32 IC_FRM_DATA_KEY= 27;

static const guint32 IC_TABLE_TEMPORARY_KEY= 28;
static const guint32 IC_TABLE_TEMPORARY_DEFAULT= 0;

static const guint32 IC_FORCE_VAR_PART_KEY= 29; /* Always 0, never changed */

static const guint32 IC_FRAGMENT_COUNT_KEY= 128;

static const guint32 IC_FRAGMENT_DATA_LEN_KEY= 129;
static const guint32 IC_FRAGMENT_DATA_KEY= 130;

/* Tablespace id defaults to RNIL */
static const guint32 IC_TABLESPACE_ID_KEY= 131;
/* Tablespace version defaults to minus one */
static const guint32 IC_TABLESPACE_VERSION_KEY= 132;

static const guint32 IC_TABLESPACE_DATA_LEN_KEY= 133;
static const guint32 IC_TABLESPACE_DATA_KEY= 134;

static const guint32 IC_RANGE_LIST_DATA_LEN_KEY= 135;
static const guint32 IC_RANGE_LIST_DATA_KEY= 136;

static const guint32 IC_REPLICA_LIST_DATA_LEN_KEY= 137;
static const guint32 IC_REPLICA_LIST_DATA_KEY= 138;

/* Default max rows is 0 */
static const guint32 IC_MAX_ROWS_LOW_KEY= 139;
static const guint32 IC_MAX_ROWS_HIGH_KEY= 140;

static const guint32 IC_DEFAULT_NUM_PARTITIONS_FLAG_KEY= 141;
static const guint32 IC_DEFAULT_NUM_PARTITIONS_FLAG_DEFAULT= 1;
static const guint32 IC_LINEAR_HASH_FLAG_KEY= 142;
static const guint32 IC_LINEAR_HASH_FLAG_DEFAULT= 1;

/* Default min rows is 0 */
static const guint32 IC_MIN_ROWS_LOW_KEY= 143;
static const guint32 IC_MIN_ROWS_HIGH_KEY= 144;

static const guint32 IC_ROW_GCI_FLAG_KEY= 150;
static const guint32 IC_ROW_GCI_FLAG_DEFAULT= 1;

static const guint32 IC_ROW_CHECKSUM_FLAG_KEY= 151;
static const guint32 IC_ROW_CHECKSUM_FLAG_DEFAULT= 1;

static const guint32 IC_SINGLE_USER_MODE_KEY= 152;
static const guint32 IC_SINGLE_USER_MODE_DEFAULT= 0;

/* Defaults to RNIL */
static const guint32 IC_HASH_MAP_ID_KEY= 153;
/* Defaults to -1 */
static const guint32 IC_HASH_MAP_VERSION_KEY= 154;

/* Defaults to main memory storage */
static const guint32 IC_TABLE_STORAGE_TYPE_KEY= 155;
static const guint32 IC_TABLE_STORAGE_TYPE_DEFAULT= 0;

/* Defaults to 0 */
static const guint32 IC_EXTRA_ROW_GCI_BITS_KEY= 156;

/* Defaults to 0 */
static const guint32 IC_EXTRA_ROW_AUTHOR_BITS_KEY= 157;

static const guint32 IC_TABLE_END_KEY= 999;
static const guint32 IC_TABLE_END_DEFAULT= 1;

/* Mandatory to have a name */
static const guint32 IC_COLUMN_NAME_KEY= 1000;

/* Id defaults to -1, set by kernel */
static const guint32 IC_COLUMN_ID_KEY= 1001;

/* Type deprecated, always set to -1 */
static const guint32 IC_COLUMN_TYPE_KEY= 1002;

static const guint32 IC_COLUMN_SIZE_KEY= 1003;
static const guint32 IC_COLUMN_SIZE_DEFAULT= 5;

static const guint32 IC_COLUMN_ARRAY_SIZE_KEY= 1005;
static const guint32 IC_COLUMN_ARRAY_SIZE_DEFAULT= 1;

static const guint32 IC_COLUMN_PKEY_FLAG_KEY= 1006;
static const guint32 IC_COLUMN_PKEY_FLAG_DEFAULT= 0;

static const guint32 IC_COLUMN_STORAGE_TYPE_KEY= 1007;
static const guint32 IC_COLUMN_STORAGE_TYPE_DEFAULT= 0;

static const guint32 IC_COLUMN_NULL_FLAG_KEY= 1008;
static const guint32 IC_COLUMN_NULL_FLAG_DEFAULT= 0;

static const guint32 IC_COLUMN_DYNAMIC_FLAG_KEY= 1009;
static const guint32 IC_COLUMN_DYNAMIC_FLAG_DEFAULT= 0;

static const guint32 IC_COLUMN_DISTRIBUTION_KEY_FLAG_KEY= 1010;
static const guint32 IC_COLUMN_DISTRIBUTION_KEY_FLAG_DEFAULT= 0;

static const guint32 IC_COLUMN_EXT_TYPE_KEY= 1013;
static const guint32 IC_COLUMN_EXT_TYPE_DEFAULT= 8;

/* The rest of these types defaults to 0 */
static const guint32 IC_COLUMN_EXT_PRECISION_KEY= 1014;
static const guint32 IC_COLUMN_EXT_SCALE_KEY= 1015;
static const guint32 IC_COLUMN_EXT_LEN_KEY= 1016;

static const guint32 IC_COLUMN_AUTO_INCREMENT_FLAG_KEY= 1017;
static const guint32 IC_COLUMN_AUTO_INCREMENT_FLAG_DEFAULT= 0;

static const guint32 IC_COLUMN_ARRAY_TYPE_KEY= 1019;
static const guint32 IC_COLUMN_ARRAY_TYPE_DEFAULT= 0;


/* Defaults to 0 length and empty value */
static const guint32 IC_COLUMN_DEFAULT_VALUE_LEN_KEY= 1020;
static const guint32 IC_COLUMN_DEFAULT_VALUE_LEN_DEFAULT= 0;
static const guint32 IC_COLUMN_DEFAULT_VALUE_KEY= 1021;

static const guint32 IC_COLUMN_END_KEY= 1999;
static const guint32 IC_COLUMN_END_DEFAULT= 1;
