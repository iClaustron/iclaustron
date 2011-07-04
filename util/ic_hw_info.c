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

#include <ic_base_header.h>
#include <ic_port.h>
#include <ic_debug.h>
#include <ic_hw_info.h>
#include <ic_string.h>

/**
  Get information about CPUs on this machine

  @parameter num_cpus           OUT: Number of CPU threads
  @parameter num_numa_nodes     OUT: Number of NUMA nodes in machine
  @parameter num_cpu_cores      OUT: Number of CPU cores
  @parameter cpu_info           OUT: Array describing each CPU

  Note: This function allocates memory when num_cpus > 0, in this
  case it's the responsibility of the caller to call ic_free on
  the cpu_info pointer returned.

  If we fail to retrieve CPU information for some reason we will return
  num_cpus == 0 to indicate no CPU information is available.
*/
void
ic_get_cpu_info(guint32 *num_processors,
                guint32 *num_cpu_sockets,
                guint32 *num_cpu_cores,
                guint32 *num_numa_nodes,
                IC_CPU_INFO **cpu_info)
{
  int ret_code;
  gchar *file_str= NULL;
  gchar *loop_file_content;
  gchar *file_content= NULL;
  guint64 file_size;
  gchar buf[200];
  guint32 line_size;
  guint32 i;
  guint32 loc_num_processors;
  guint64 num_lines;
  IC_CPU_INFO *loc_cpu_info= NULL;
  DEBUG_ENTRY("ic_get_cpu_info");

  if (!(ret_code= ic_get_hw_info(IC_GET_CPU_INFO,
                                 NULL,
                                 &file_str)))
  {
    if ((ret_code= ic_get_file_contents((const gchar*)file_str,
                                        &file_content,
                                        &file_size)))
      goto no_info;
    num_lines= ic_count_lines(file_content, file_size);
    loop_file_content= file_content;
    /* Get first line which provides number of CPU threads */
    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf,
                          "Number of CPU processors: %u",
                          num_processors)) != 1)
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get number of CPU processors"));
      goto no_info;
    }
    loc_num_processors= *num_processors;
    if (num_lines != (guint64)(loc_num_processors + 4))
    {
      DEBUG_PRINT(PORT_LEVEL, ("Wrong number of lines in HW CPU info"));
      goto no_info;
    }
    if (!(loc_cpu_info= (IC_CPU_INFO*)ic_malloc(
             (*num_processors) * sizeof(IC_CPU_INFO))))
      goto no_info; /* Report no info available in this case */

    *cpu_info= loc_cpu_info;
    /* Get line which provides number of CPU sockets */
    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf,
                          "Number of CPU sockets: %u",
                          num_cpu_sockets)) != 1)
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get number of CPU sockets"));
      goto no_info;
    }
    /* Get first line which provides number of CPU threads */
    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf,
                          "Number of CPU cores: %u",
                          num_cpu_cores)) != 1)
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get number of CPU cores"));
      goto no_info;
    }
    /* Get line which provides number of NUMA nodes */
    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf,
                          "Number of NUMA nodes: %u",
                          num_numa_nodes) != 1))
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get number of NUMA nodes"));
      goto no_info;
    }
    for (i= 0; i < loc_num_processors; i++)
    {
      if ((ret_code= ic_get_next_line(&loop_file_content,
                                      &file_size,
                                      buf,
                                      sizeof(buf),
                                      &line_size)))
        goto no_info;
      if ((ret_code= sscanf(buf,
         "CPU processor id = %u, core id = %u, numa node id = %u, cpu id = %u",
         &loc_cpu_info[i].processor_id,
         &loc_cpu_info[i].core_id,
         &loc_cpu_info[i].numa_node_id,
         &loc_cpu_info[i].cpu_id)) != 4)
      {
        DEBUG_PRINT(PORT_LEVEL, ("Failed to get CPU info"));
        goto no_info;
      }
    }
  }
end:
  if (file_str)
  {
    ic_delete_file(file_str);
    ic_free(file_str);
  }
  if (file_content)
    ic_free(file_content);
  DEBUG_RETURN_EMPTY;

no_info:
  *num_processors= 0;
  *num_cpu_sockets= 0;
  *num_numa_nodes= 0;
  *num_cpu_cores= 0;
  *cpu_info= NULL;
  if (loc_cpu_info)
    ic_free(loc_cpu_info);
  goto end;
}

/**
  Get information about memory size and about the NUMA nodes in the
  machine how much memory each such node has.

  @parameter num_numa_nodes      OUT: Number of NUMA nodes
  @parameter total_memory_size   OUT: Total memory size in MBytes
  @parameter mem_info            OUT: Array describing each NUMA node

  Note: When memory information is available we allocate the mem_info
  data structure, this needs to be freed by caller of this function.

  When no memory information is available or we fail somehow to retrieve
  memory information we will return total_memory_size == 0 to indicate
  no memory information is available.
*/
void ic_get_mem_info(guint32 *num_numa_nodes,
                     guint32 *total_memory_size,
                     IC_MEM_INFO **mem_info)
{
  int ret_code;
  guint32 num_lines;
  guint32 line_size;
  guint32 i;
  gchar *file_str= NULL;
  IC_MEM_INFO *loc_mem_info= NULL;
  gchar *file_content= NULL;
  gchar *loop_file_content;
  guint64 file_size;
  gchar buf[200];
  DEBUG_ENTRY("ic_get_mem_info");

  if (!(ret_code= ic_get_hw_info(IC_GET_MEM_INFO,
                                 NULL,
                                 &file_str)))
  {
    if ((ret_code= ic_get_file_contents((const gchar*)file_str,
                                        &file_content,
                                        &file_size)))
      goto no_info;
    num_lines= ic_count_lines(file_content, file_size);
    loop_file_content= file_content;

    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf,
                          "Number of NUMA nodes: %u",
                          num_numa_nodes)) != 1)
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get number of NUMA nodes"));
      goto no_info;
    }

    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf,
                          "Memory size = %u MBytes",
                          total_memory_size)) != 1)
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get memory size"));
      goto no_info;
    }
    if (!(loc_mem_info= (IC_MEM_INFO*)ic_malloc(
          sizeof(IC_MEM_INFO) * (*num_numa_nodes))))
      goto no_info; /* Report no info available in this case */

    *mem_info= loc_mem_info;
    for (i= 0; i < (*num_numa_nodes); i++)
    {
      if ((ret_code= ic_get_next_line(&loop_file_content,
                                      &file_size,
                                      buf,
                                      sizeof(buf),
                                      &line_size)))
        goto no_info;
      if ((ret_code= sscanf(buf,
                            "Numa node id = %u, Memory size = %u MBytes",
                            &loc_mem_info[i].numa_node_id,
                            &loc_mem_info[i].memory_size)) != 2)
      {
        DEBUG_PRINT(PORT_LEVEL, ("Failed to get number of CPU cores"));
        goto no_info;
      }
    }
  }
end:
  if (file_str)
  {
    ic_delete_file(file_str);
    ic_free(file_str);
  }
  if (file_content)
    ic_free(file_content);
  DEBUG_RETURN_EMPTY;

no_info:
  *num_numa_nodes= 0;
  *total_memory_size= 0;
  *mem_info= 0;
  if (loc_mem_info)
    ic_free((gchar*)loc_mem_info);
  goto end;
}

/**
  Get information about the disk space available below a certain directory
  name.

  @dir_name                      IN: The directory name
  @disk_space                    OUT: The disk space in GBytes
*/
void ic_get_disk_info(gchar *dir_name,
                      guint32 *disk_space)
{

  int ret_code;
  guint32 line_size;
  guint32 num_lines;
  gchar *file_content= NULL;
  gchar *loop_file_content;
  guint64 file_size;
  gchar *file_str;
  gchar buf[200];
  DEBUG_ENTRY("ic_get_disk_info");

  if (!(ret_code= ic_get_hw_info(IC_GET_DISK_INFO,
                                 dir_name,
                                 &file_str)))
  {
    if ((ret_code= ic_get_file_contents((const gchar*)file_str,
                                        &file_content,
                                        &file_size)))
      goto no_info;
    loop_file_content= file_content;
    num_lines= ic_count_lines(file_content, file_size);
    if (num_lines != 1)
      goto no_info;
    if ((ret_code= ic_get_next_line(&loop_file_content,
                                    &file_size,
                                    buf,
                                    sizeof(buf),
                                    &line_size)))
      goto no_info;
    if ((ret_code= sscanf(buf, "Disk space = %u GBytes", disk_space)) != 1)
    {
      DEBUG_PRINT(PORT_LEVEL, ("Failed to get disk space"));
      goto no_info;
    }
  }

end:
  if (file_str)
  {
    ic_delete_file(file_str);
    ic_free(file_str);
  }
  if (file_content)
    ic_free(file_content);
  DEBUG_RETURN_EMPTY;

no_info:
  *disk_space= 0;
  goto end;
}
