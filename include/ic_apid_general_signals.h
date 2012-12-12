/* Copyright (C) 2012-2012 iClaustron AB

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

typedef enum ic_ndb_start_state IC_NDB_START_STATE;
enum ic_ndb_start_state
{
  IC_NDB_NOT_STARTED= 0,
  IC_NDB_CMVMI_STARTED= 1,
  IC_NDB_STARTING= 2,
  IC_NDB_STARTED= 3,
  IC_NDB_SINGLE_USER= 4,
  IC_NDB_STOP_NEW_CONNECTIONS= 5,
  IC_NDB_STOP_NEW_TRANSACTIONS= 6,
  IC_NDB_STOP_NEW_NODES= 7,
  IC_NDB_STOP_LAST= 8
};

typedef enum ic_ndb_start_type IC_NDB_START_TYPE;
enum ic_ndb_start_type
{
  IC_NDB_INITIAL_START= 0,
  IC_NDB_CLUSTER_RESTART= 1,
  IC_NDB_NODE_RESTART= 2,
  IC_NDB_INITIAL_NODE_RESTART= 3
};

typedef struct ic_ndb_node_state IC_NDB_NODE_STATE;
struct ic_ndb_node_state
{
  guint32 ndb_start_state; /* Type IC_NDB_START_STATE */
  guint32 ndb_node_group; /* Valid during IC_NDB_STARTING */
  guint32 ndb_dynamic_id; /* Valid during IC_NDB_STARTING */
  union
  {
    struct ndb_starting
    {
      guint32 start_phase;
      /* Type IC_NDB_START_TYPE, only used during IC_NDB_STARTING */
      guint32 ndb_start_type;
    };
    struct ndb_stopping
    {
      guint32 shutdown_flag;
      guint32 time_out;
      guint32 alarm_time;
    };
  };
  guint32 single_user_mode_flag;
  guint32 single_user_node;
  guint32 connected_node_bitmap[IC_MAX_NDB_DATA_NODES/IC_SIZE_UINT32];
};

static const int API_REGREQ_GSN= 3;
static const int API_REGCONF_GSN= 1;
static const int API_REGREF_GSN= 2;

typedef struct ic_api_regreq IC_API_REGREQ;
struct ic_api_regreq
{
  guint32 my_reference;
  guint32 my_ndb_version;
  guint32 my_mysql_version;
};
static const int API_REGREQ_LEN= 
  sizeof(IC_API_REGREQ)/sizeof(guint32);

typedef struct ic_api_regconf IC_API_REGCONF;
struct ic_api_regconf
{
  guint32 ndb_reference;
  guint32 ndb_version;
  guint32 hb_frequency;
  guint32 mysql_version;
  guint32 min_version;
  IC_NDB_NODE_STATE node_state;
};
static const int API_REGCONF_LEN= 
  sizeof(IC_API_REGCONF)/sizeof(guint32);

typedef struct ic_api_regref IC_API_REGREF;
struct ic_api_regref
{
  guint32 ndb_reference;
  guint32 ndb_version;
  guint32 error_code;
  guint32 mysql_version;
};
static const int API_REGREF_LEN= 
  sizeof(IC_API_REGREF)/sizeof(guint32);

static const int IC_API_REGREF_WRONG_TYPE= 1;
static const int IC_API_REGREF_UNSUPPORTED_VERSION= 2;
