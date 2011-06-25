/* Copyright (C) 2011 iClaustron AB

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

#ifndef HW_INFO_H
#define HW_INFO_H

/* Interfaces to get HW information */
struct ic_cpu_info
{
  guint32 processor_id;
  guint32 core_id;
  guint32 numa_node_id;
  guint32 cpu_id;
};
typedef struct ic_cpu_info IC_CPU_INFO;

void ic_get_cpu_info(guint32 *num_processors,
                     guint32 *num_cpu_sockets,
                     guint32 *num_cpu_cores,
                     guint32 *num_numa_nodes,
                     IC_CPU_INFO **cpu_info);

struct ic_mem_info
{
  guint32 numa_node_id;
  guint32 memory_size;
};
typedef struct ic_mem_info IC_MEM_INFO;

void ic_get_mem_info(guint32 *num_numa_nodes,
                     guint32 *total_memory_size,
                     IC_MEM_INFO **mem_info);

void ic_get_disk_info(gchar *directory_name,
                      guint32 *disk_space);
#endif
