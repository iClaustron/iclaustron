/* Copyright (C) 2020, 2020 iClaustron AB

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
#define IC_DEF_HASH 1000
struct ic_comm_search
{
  guint32 node_id1;
  guint32 node_id2;
  guint32 index;
};

typedef struct ic_comm_search IC_COMM_SEARCH;
struct ic_int_cluster_config
{
  IC_CLUSTER_CONFIG clu_conf_ops;
  guint32 *key_value;
  guint32 len;
  guint32 cluster_id;
  guint32 section_def_dn_index;
  guint32 section_def_api_index;
  guint32 section_def_mgm_index;
  guint32 section_def_tcp_index;
  guint32 section_def_shm_index;
  guint32 section_def_system_index;
  guint32 node_section_ptrs[IC_MAX_NODE_ID + 1];
  guint8 node_type_array[IC_MAX_NODE_ID + 1];
  IC_COMM_SEARCH *comm_array;

};

typedef struct ic_int_cluster_config IC_INT_CLUSTER_CONFIG;

