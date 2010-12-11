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

#include <ic_base_header.h>
#include <ic_err.h>
#include <ic_debug.h>
#include <ic_port.h>
#include <ic_string.h>
#include <ic_connection.h>
#include <ic_threadpool.h>
#include <ic_hashtable.h>
#include <ic_dyn_array.h>
#include <ic_protocol_support.h>
#include <ic_apic.h>
#include <ic_apid.h>


/* Messages for copy cluster server files protocol */
const gchar *ic_copy_cluster_server_files_str= "copy cluster server files";
const gchar *ic_cluster_server_node_id_str= "cluster server node id: ";
const gchar *ic_number_of_clusters_str= "number of clusters: ";
const gchar *ic_receive_config_ini_str= "receive config.ini";
const gchar *ic_number_of_lines_str= "number of lines: ";
const gchar *ic_receive_grid_common_ini_str= "receive grid_common.ini";
const gchar *ic_receive_cluster_name_ini_str= "receive ";
const gchar *ic_installed_cluster_server_files=
  "installed cluster server files";
const gchar *ic_end_str= "end";
const gchar *ic_receive_config_file_ok_str= "receive config file ok";

/* Messages for the Get CPU info protocol */
const gchar *ic_get_cpu_info_str= "get cpu info";
const gchar *ic_number_of_cpus_str= "number of cpus: ";
const gchar *ic_number_of_numa_nodes_str= "number of NUMA nodes: ";
const gchar *ic_number_of_cpus_per_core_str= "number of cpus per core: ";
const gchar *ic_cpu_str= "cpu ";
const gchar *ic_cpu_node_str= ", node: ";
const gchar *ic_core_str= ", core: ";
const gchar *ic_no_cpu_info_available_str= "no cpu info available";

/* Messages for the Get Memory Information Protocol */
const gchar *ic_get_memory_info_str= "get memory info";
const gchar *ic_number_of_mbyte_user_memory_str=
  "number of MByte user memory: ";
const gchar *ic_mem_node_str= "node: ";
const gchar *ic_mb_user_memory_str= ", MB user memory: ";
const gchar *ic_no_mem_info_available_str= "no memory info available";

/* Messages for Get Disk Information Protocol */
const gchar *ic_get_disk_info_str= "get disk info";
const gchar *ic_dir_str= "dir: ";
const gchar *ic_disk_space_str= "disk space: ";
const gchar *ic_no_disk_info_available_str= "no disk info available";

/*
  This program is also used to gather information from local log files as 
  part of any process to gather information about mishaps in the cluster(s).

  This process listens to connections on a server port, this is either a
  default port or one assigned when starting program. One can also set the
  IP address to use when listening to connections. By default the process
  will discover its IP address by itself.

  The program by default uses a configuration placed in /etc/ic_cntrl.conf
  but this can be changed by a parameter to the program.

  This program can be started and stopped by services in the Linux
  environment.
  service ic_cntrl start
  service ic_cntrl stop
  service ic_cntrl status
  service ic_cntrl restart
  The normal usage of ic_cntrl is to place it such that it is automatically
  started when the OS starts. Then use the service commands to control its
  operation. Start will start if the service isn't active already. Stop will
  stop its operation. It will not stop any of the processes that the program
  has spawned off. Status is used to check its current status. Finally
  restart will stop and start the program. The most common use of these
  operations are restart which can be used to force the program to reread
  the configuration file.
*/

/*
  TODO: Need to write basic tests for the functions that can be used in this
        program.
        1) Create a connection locally in basic test for client and server
        2) Provide the server part to this function
        3) Start writing to the interface
        4) Use error inject to test errors in all sorts of places
        5) Put the basic test program at the end of this file and build it
           only when unit tests are to built
        6) Call unit test program when built for unit tests
*/

#define REC_PROG_NAME 0
#define REC_PARAM 1
#define REC_FINAL_CR 2

/* Configurable variables */
static gchar *glob_server_name= "127.0.0.1";
static gchar *glob_server_port= IC_DEF_PCNTRL_PORT_STR;
static guint32 glob_daemonize= 1;

/* Global variables */
static const gchar *glob_process_name= "ic_pcntrld";
static IC_MUTEX *pc_hash_mutex= NULL;
static IC_HASHTABLE *glob_pc_hash= NULL;
static guint64 glob_start_id= 1;
static IC_DYNAMIC_PTR_ARRAY *glob_pc_array= NULL;

static int
send_error_reply(IC_CONNECTION *conn, const gchar *error_message)
{
  int error;
  if ((error= ic_send_with_cr(conn, ic_error_str)) ||
      (error= ic_send_with_cr(conn, error_message)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_ok_reply(IC_CONNECTION *conn)
{
  int error;
  if ((error= ic_send_with_cr(conn, ic_ok_str)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_ok_pid_reply(IC_CONNECTION *conn, IC_PID_TYPE pid)
{
  int error;

  if ((error= ic_send_with_cr(conn, ic_ok_str)) ||
      (error= ic_send_with_cr_with_num(conn, ic_pid_str, pid)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_list_entry(IC_CONNECTION *conn,
                IC_PC_START *pc_start,
                gboolean list_full_flag)
{
  guint32 i;
  int error;

  if ((error= ic_send_with_cr(conn, ic_list_node_str)) ||
      (error= ic_send_with_cr_two_strings(conn,
                                          ic_grid_str,
                                          pc_start->key.grid_name.str)) ||
      (error= ic_send_with_cr_two_strings(conn,
                                          ic_cluster_str,
                                          pc_start->key.cluster_name.str)) ||
      (error= ic_send_with_cr_two_strings(conn,
                                          ic_node_str,
                                          pc_start->key.node_name.str)) ||
      (error= ic_send_with_cr_two_strings(conn,
                                          ic_program_str,
                                          pc_start->program_name.str)) ||
      (error= ic_send_with_cr_two_strings(conn,
                                          ic_version_str,
                                          pc_start->version_string.str)) ||
      (error= ic_send_with_cr_with_num(conn,
                                       ic_start_time_str,
                                       pc_start->start_id)) ||
      (error= ic_send_with_cr_with_num(conn,
                                       ic_pid_str,
                                       (guint64)pc_start->pid)) ||
      (error= ic_send_with_cr_with_num(conn,
                                       ic_num_parameters_str,
                                       (guint64)pc_start->num_parameters)))
    return error;
  if (list_full_flag)
  {
    for (i= 0; i < pc_start->num_parameters; i++)
    {
      if ((error= ic_send_with_cr(conn, pc_start->parameters[i].str)))
        return error;
    }
  }
  if ((error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_list_stop_reply(IC_CONNECTION *conn)
{
  int error;

  if ((error= ic_send_with_cr(conn, ic_list_stop_str)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
get_optional_str(IC_CONNECTION *conn,
                 IC_MEMORY_CONTAINER *mc_ptr,
                 const gchar *head_str,
                 IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *str_ptr;
  gchar *read_buf;
  guint32 head_len= 0;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == 0)
  {
    IC_INIT_STRING(str, NULL, 0, FALSE);
    return 0;
  }
  if (head_str)
    head_len= strlen(head_str);
  if (read_size <= head_len)
    return IC_PROTOCOL_ERROR;
  if (head_len && memcmp(read_buf, head_str, head_len))
    return IC_PROTOCOL_ERROR;
  read_buf+= head_len;
  read_size-= head_len;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(str_ptr, read_buf, read_size);
  IC_INIT_STRING(str, str_ptr, read_size, TRUE);
  return 0;
}

static int
get_str(IC_CONNECTION *conn,
        IC_MEMORY_CONTAINER *mc_ptr,
        const gchar *head_str,
        IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *str_ptr;
  gchar *read_buf;
  guint32 head_len= 0;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (head_str)
    head_len= strlen(head_str);
  if (read_size <= head_len)
    return IC_PROTOCOL_ERROR;
  if (head_len && memcmp(read_buf, head_str, head_len))
    return IC_PROTOCOL_ERROR;
  read_buf+= head_len;
  read_size-= head_len;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(str_ptr, read_buf, read_size);
  IC_INIT_STRING(str, str_ptr, read_size, TRUE);
  return 0;
}

static int
get_key(IC_CONNECTION *conn,
        IC_MEMORY_CONTAINER *mc_ptr,
        IC_PC_KEY *pc_key)
{
  int error;

  if ((error= get_str(conn, mc_ptr, ic_grid_str, &pc_key->grid_name)) ||
      (error= get_str(conn, mc_ptr, ic_cluster_str, &pc_key->cluster_name)) ||
      (error= get_str(conn, mc_ptr, ic_node_str, &pc_key->node_name)))
    return error;
  return 0;
}

static int
get_autorestart(IC_CONNECTION *conn,
                gboolean *auto_restart)
{
  int error;
  guint32 read_size;
  gchar *read_buf;
  guint32 auto_restart_size= strlen(ic_auto_restart_str);

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size <= auto_restart_size)
    goto protocol_error;
  if (!memcmp(read_buf, ic_auto_restart_str, auto_restart_size))
    goto protocol_error;
  read_size-= auto_restart_size;
  read_buf+= auto_restart_size;
  if (read_size == strlen(ic_true_str) &&
      !memcmp(read_buf, ic_true_str, read_size))
    *auto_restart= 1;
  else if (read_size == strlen(ic_false_str) &&
           !memcmp(read_buf, ic_false_str, read_size))
    *auto_restart= 0;
  else
    goto protocol_error;
  return 0;
protocol_error:
  return IC_PROTOCOL_ERROR;
}

static int
get_num_parameters(IC_CONNECTION *conn,
                   guint32 *num_parameters)
{
  int error;
  guint32 read_size;
  guint64 loc_num_params;
  guint32 num_param_len= strlen(ic_num_parameters_str);
  guint32 len;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size <= num_param_len ||
      memcmp(read_buf, ic_num_parameters_str, read_size))
    return IC_PROTOCOL_ERROR;
  if ((read_size <= num_param_len) ||
      (ic_conv_str_to_int(read_buf+num_param_len, &loc_num_params, &len)) ||
      (loc_num_params >= IC_MAX_UINT32))
    return IC_PROTOCOL_ERROR;
  *num_parameters= (guint32)loc_num_params;
  return 0;
}

static int
rec_start_message(IC_CONNECTION *conn,
                  IC_PC_START **pc_start)
{
  IC_MEMORY_CONTAINER *mc_ptr;
  IC_PC_START *loc_pc_start;
  guint32 i;
  int error;

  /*
    When we come here we have already received the start string, we now
    receive the rest of the start package.
    We put the data into an IC_PC_START struct which is self-contained
    and also contains a memory container for all the memory allocated.
  */
  if ((mc_ptr= ic_create_memory_container((guint32)1024, (guint32) 0, FALSE)))
    return IC_ERROR_MEM_ALLOC;
  if ((*pc_start= (IC_PC_START*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_PC_START))))
    goto mem_error;
  loc_pc_start= *pc_start;
  loc_pc_start->mc_ptr= mc_ptr;

  if ((error= get_str(conn, mc_ptr, ic_program_str,
                      &loc_pc_start->program_name)) ||
      (error= get_str(conn, mc_ptr, ic_version_str,
                      &loc_pc_start->version_string)) ||
      (error= get_key(conn, mc_ptr, &loc_pc_start->key)) ||
      (error= get_autorestart(conn, &loc_pc_start->autorestart)) ||
      (error= get_num_parameters(conn, &loc_pc_start->num_parameters)))
    goto error;
  if ((loc_pc_start->parameters= (IC_STRING*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_STRING)*
         loc_pc_start->num_parameters)))
    goto mem_error;
  for (i= 0; i < loc_pc_start->num_parameters; i++)
  {
    if ((error= get_str(conn, mc_ptr, NULL, &loc_pc_start->parameters[i])))
      goto error;
  }
  if ((error= ic_rec_simple_str(conn, ic_empty_string)))
    goto error;
  return 0;
mem_error:
  error= IC_ERROR_MEM_ALLOC;
error:
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return error;
}

static int
init_pc_find(IC_MEMORY_CONTAINER **mc_ptr,
             IC_PC_FIND **pc_find)
{
  if (((*mc_ptr)= ic_create_memory_container((guint32)1024,
                                             (guint32)0, FALSE)))
    return IC_ERROR_MEM_ALLOC;
  if (((*pc_find)= (IC_PC_FIND*)
       (*mc_ptr)->mc_ops.ic_mc_calloc((*mc_ptr), sizeof(IC_PC_FIND))))
    return IC_ERROR_MEM_ALLOC;
  (*pc_find)->mc_ptr= (*mc_ptr);
  return 0;
}

static int
rec_key_message(IC_CONNECTION *conn,
                 IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  int error;

  /*
    When we come here we already received the stop/kill string and we only
    need to fill in the key parameters (grid, cluster and node name).
  */
  if ((error= init_pc_find(&mc_ptr, pc_find)))
    goto error;
  if ((error= get_key(conn, mc_ptr, &(*pc_find)->key)))
    goto error;
  if ((error= ic_rec_simple_str(conn, ic_empty_string)))
    goto error;
  (*pc_find)->mc_ptr= mc_ptr;
  return 0;
error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return error;
}

static int
rec_optional_key_message(IC_CONNECTION *conn,
                         IC_PC_FIND **pc_find)
{
  IC_MEMORY_CONTAINER *mc_ptr= NULL;
  int error;

  /*
    When we come here we already received the list [full] string and we
    only need to fill in key parameters. All key parameters are optional
    here.
  */
  if ((error= init_pc_find(&mc_ptr, pc_find)))
    goto error;
  if ((error= get_optional_str(conn, mc_ptr, ic_grid_str,
                               &(*pc_find)->key.grid_name)))
    goto error;
  if ((*pc_find)->key.grid_name.len == 0)
    return 0; /* No grid name provided, list all programs */
  if ((error= get_optional_str(conn, mc_ptr, ic_cluster_str,
                               &(*pc_find)->key.cluster_name)))
    goto error;
  if ((*pc_find)->key.cluster_name.len == 0)
    return 0; /* No cluster name provided, list all programs in grid */
  if ((error= get_optional_str(conn, mc_ptr, ic_node_str,
                               &(*pc_find)->key.node_name)))
    goto error;
  if ((*pc_find)->key.node_name.len == 0)
    return 0; /* No node name provided, list all programs in cluster */
  if ((error= ic_rec_simple_str(conn, ic_empty_string)))
    goto error;
  return 0;
error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return error;
}

static unsigned int
ic_pc_hash_key(void *ptr)
{
  IC_PC_KEY *pc_key= ptr;
  IC_STRING *str;
  unsigned int hash1, hash2, hash3;

  str= &pc_key->grid_name;
  hash1= ic_hash_str((void*)str);
  str= &pc_key->cluster_name;
  hash2= ic_hash_str((void*)str);
  str= &pc_key->node_name;
  hash3= ic_hash_str((void*)str);
  return (hash1 ^ hash2 ^ hash3);
}

static int
ic_pc_key_equal(void *ptr1, void *ptr2)
{
  IC_PC_KEY *pc_key1= ptr1;
  IC_PC_KEY *pc_key2= ptr2;

  if ((ic_cmp_str(&pc_key1->grid_name, &pc_key2->grid_name)) ||
      (ic_cmp_str(&pc_key1->cluster_name, &pc_key2->cluster_name)) ||
      (ic_cmp_str(&pc_key1->node_name, &pc_key2->node_name)))
    return 1;
  return 0;
}

IC_HASHTABLE*
create_pc_hash()
{
  return ic_create_hashtable(4096, ic_pc_hash_key, ic_pc_key_equal);
}

static void
remove_pc_entry(IC_PC_START *pc_start)
{
  IC_PC_START *pc_start_check;
  pc_start_check= ic_hashtable_remove(glob_pc_hash,
                                      (void*)pc_start);
  ic_assert(pc_start_check == pc_start);
  glob_pc_array->dpa_ops.ic_remove_ptr(glob_pc_array,
                                       pc_start->dyn_trans_index,
                                       (void*)pc_start);
  /* Release memory associated with the process */
  pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
}

static int
insert_pc_entry(IC_PC_START *pc_start)
{
  int ret_code= 0;
  guint64 index;
  if ((ret_code= glob_pc_array->dpa_ops.ic_insert_ptr(glob_pc_array,
                                                      &index,
                                                      (void*)pc_start)))
    return ret_code;
  pc_start->dyn_trans_index= index;
  if ((ret_code= ic_hashtable_insert(glob_pc_hash,
                                     (void*)pc_start, (void*)pc_start)))
  {
    glob_pc_array->dpa_ops.ic_remove_ptr(glob_pc_array,
                                         pc_start->dyn_trans_index,
                                         (void*)pc_start);
  }
  return ret_code;
}

static int
book_process(IC_PC_START *pc_start)
{
  IC_PC_START *pc_start_found;
  guint64 prev_start_id= 0;
  int ret_code= 0;

  /*
    Insert the memory describing the started process in the hash table
    of all programs we have started, the key is the grid name, the cluster
    name and the node name. We can also iterate over this hashtable to
    check all entries. Since there can be multiple threads accessing this
    hash table concurrently we have to protect access by a mutex.
    At this point we need to verify that there isn't already a process
    running with this key, if such a process is already running we
    need to verify it is still running. Only if no process is running
    with this key will we continue to process the start of the program.
  */
try_again:
  ic_mutex_lock(pc_hash_mutex);
  if ((pc_start_found= (IC_PC_START*)
       ic_hashtable_search(glob_pc_hash, (void*)pc_start)))
  {
    /*
      We found a process with the same key, it could be a process which
      has already stopped so we first verify that the process is still
      running.
    */
    if (!pc_start_found->pid)
    {
      /* Someone is already trying to start, we won't continue or attempt */
      ret_code= IC_ERROR_PC_START_ALREADY_ONGOING;
    }
    else
    {
      if (pc_start_found->start_id == prev_start_id)
      {
        /*
          We come here the second time around and have verified that it is
          still the same process in the hash, thus we have verified that
          the process is dead and that it is the same process which is
          in the hash, thus it is now safe to remove it from the hash.
        */
        remove_pc_entry(pc_start_found);
        ret_code= insert_pc_entry(pc_start);
        goto end;
      }
      prev_start_id= pc_start_found->start_id;
      ic_mutex_unlock(pc_hash_mutex);
      /*
        Verify that the process is still alive, we have released the mutex
        to avoid making the mutex a hotspot at the price of slightly more
        complex logic.
      */
      if ((ret_code= ic_is_process_alive(pc_start_found->pid,
                                         pc_start_found->program_name.str)))
      {
        if (ret_code == IC_ERROR_CHECK_PROCESS_SCRIPT)
          return ret_code;
        ic_assert(ret_code == IC_ERROR_PROCESS_NOT_ALIVE);
        /*
          Process is dead, we need to acquire the mutex again and verify that
          no one else have inserted a new process into the hash again before
          removing the old copy and inserting ourselves.
          We have remembered the start_id of the instance we found to be dead
          and thus if we can verify that it is still in the hash then we are
          safe to remove it. Otherwise someone got there before us and we
          retry a check of the process being alive.
        */
        goto try_again;
      }
      else
      {
        /* Process is still alive, so no use for us to continue */
        return IC_ERROR_PC_PROCESS_ALREADY_RUNNING;
      }
    }
  }
  else
  {
    /*
      No similar process found, we'll insert it into the hash at this point
      to make sure no one else is attempting to start the process while we
      are trying.
    */
    ret_code= insert_pc_entry(pc_start);
  }
end:
  ic_mutex_unlock(pc_hash_mutex);
  return ret_code;
}

static int
handle_start(IC_CONNECTION *conn)
{
  gchar **arg_vector;
  int ret_code;
  guint32 i;
  IC_PID_TYPE pid;
  IC_STRING working_dir;
  IC_PC_START *pc_start, *pc_start_check;

  IC_INIT_STRING(&working_dir, NULL, 0, FALSE);
  if ((ret_code= rec_start_message(conn, &pc_start)))
    return ret_code;
  if ((arg_vector= (gchar**)pc_start->mc_ptr->mc_ops.ic_mc_calloc(
                   pc_start->mc_ptr,
                   (pc_start->num_parameters+2) * sizeof(gchar*))))
    goto mem_error;
  /*
    Prepare the argument vector, first program name and then all the
    parameters passed to the program.
  */
  arg_vector[0]= pc_start->program_name.str;
  for (i= 0; i < pc_start->num_parameters; i++)
  {
    arg_vector[i+1]= pc_start->parameters[i].str;
  }
  /* Book the process in the hash table */
  if ((ret_code= book_process(pc_start)))
    goto error;
  /*
    Create the working directory which is the base directory
    (iclaustron_install) with the version number appended and
    the bin directory where the binaries are placed.
  */
  if ((ret_code= ic_set_binary_dir(&working_dir,
                                   pc_start->version_string.str)))
    goto late_error;
  /*
    Start the actual using the program name, its arguments and the binary
    placed in the working bin directory, return the PID of the started
    process.
  */
  if ((ret_code= ic_start_process(&arg_vector[0],
                                  working_dir.str,
                                  &pid)))
    goto late_error;
  /*
    Verify that a process with this PID still exists and that it's using
    the correct program name.
  */
  if ((ret_code= ic_is_process_alive(pid,
                                     pc_start->program_name.str)))
    goto late_error;
  if (working_dir.str)
    ic_free(working_dir.str);
  ic_mutex_lock(pc_hash_mutex);
  /*
    pc_start struct is protected by mutex, as soon as the pid is nonzero
    anyone can remove it from hash table and release the memory. Thus
    need to be careful with updating the value from zero.
    We also update the start_id at this point to ensure that we can
    distinguish entries of the same key from each other.
  */
  pc_start->pid= pid;
  pc_start->start_id= glob_start_id++;
  ic_mutex_unlock(pc_hash_mutex);
  return send_ok_pid_reply(conn, pc_start->pid);

late_error:
  ic_mutex_lock(pc_hash_mutex);
  pc_start_check= ic_hashtable_remove(glob_pc_hash,
                                      (void*)pc_start);
  ic_assert(pc_start_check == pc_start);
  ic_mutex_unlock(pc_hash_mutex);
  goto error;
mem_error:
  ret_code= IC_ERROR_MEM_ALLOC;
error:
  if (working_dir.str)
    ic_free(working_dir.str);
  pc_start->mc_ptr->mc_ops.ic_mc_free(pc_start->mc_ptr);
  return send_error_reply(conn, 
                          ic_get_error_message(ret_code));
}

static int
handle_stop(IC_CONNECTION *conn, gboolean kill_flag)
{
  IC_PC_FIND *pc_find= NULL;
  IC_PC_START *pc_start_found;
  int error;
  IC_PID_TYPE pid;
  guint64 start_id= 0;
  int loop_count= 0;
  gchar *program_name;

  if ((error= rec_key_message(conn, &pc_find)))
    goto error;

try_again:
  ic_mutex_lock(pc_hash_mutex);
  if ((pc_start_found= (IC_PC_START*)ic_hashtable_search(glob_pc_hash,
                                                         (void*)pc_find)))
  {
    /*
      We found a process, now it's time to try to stop the process.
      We'll check if the process is still in startup, in that case we
      need to wait for the starting process to complete before we
      stop the process.
    */
    if (pc_start_found->pid == 0)
    {
      /* The process is still starting up, too early to stop it. */
      ic_mutex_unlock(pc_hash_mutex);
      if (++loop_count == 10)
      {
        error= IC_ERROR_PROCESS_STUCK_IN_START_PHASE;
        goto error;
      }
      ic_sleep(1);
      goto try_again;
    }
    /*
      We release the mutex on the hash, try to stop the process and
      then check if we were successful in stopping it after waiting
      for a few seconds.
    */
    start_id= pc_start_found->start_id;
    pid= pc_start_found->pid;
    program_name= pc_start_found->program_name.str;
    ic_mutex_unlock(pc_hash_mutex);
    ic_kill_process(pc_start_found->pid, kill_flag);
    ic_sleep(3); /* Sleep 3 seconds */
    error= ic_is_process_alive(pid, program_name);
    if (error == 0)
    {
      error= IC_ERROR_FAILED_TO_STOP_PROCESS;
      goto error;
    }
    else if (error == IC_ERROR_CHECK_PROCESS_SCRIPT)
      goto error;
    else
    {
      ic_assert(error == IC_ERROR_PROCESS_NOT_ALIVE);
      /*
        We were successful in stopping the process, now we need to
        remove the entry from the hash.
      */
      ic_mutex_lock(pc_hash_mutex);
      pc_start_found= (IC_PC_START*)ic_hashtable_search(glob_pc_hash,
                                                        (void*)pc_find);
      if (pc_start_found &&
          pc_start_found->pid == pid &&
          pc_start_found->start_id == start_id)
      {
        /*
          The process we set out to stop is still in the hash so we remove it.
        */
        remove_pc_entry(pc_start_found);
      }
      ic_mutex_unlock(pc_hash_mutex);
    }
  }
  else
  {
    /*
      We couldn't find any such process in the hash, this means that the
      process doesn't exist and we have succeeded since our aim was to
      ensure the process was stopped.
    */
    ic_mutex_unlock(pc_hash_mutex);
  }
  pc_find->mc_ptr->mc_ops.ic_mc_free(pc_find->mc_ptr); 
  return send_ok_reply(conn);
error:
  if (pc_find && pc_find->mc_ptr)
    pc_find->mc_ptr->mc_ops.ic_mc_free(pc_find->mc_ptr); 
  return send_error_reply(conn,
                          ic_get_error_message(error));
}

static gboolean
is_list_match(IC_PC_START *pc_start, IC_PC_FIND *pc_find)
{
  if (pc_find->key.grid_name.str == NULL)
    return TRUE;
  if (pc_find->key.grid_name.len != pc_start->key.grid_name.len ||
      memcmp(pc_find->key.grid_name.str,
             pc_start->key.grid_name.str,
             pc_find->key.grid_name.len) != 0)
    return FALSE;
  /* Grid names were equal */
  if (pc_find->key.cluster_name.str == NULL)
    return TRUE;
  if (pc_find->key.cluster_name.len != pc_start->key.cluster_name.len ||
      memcmp(pc_find->key.cluster_name.str,
             pc_start->key.cluster_name.str,
             pc_find->key.cluster_name.len) != 0)
    return FALSE;
  /* Cluster names were equal */
  if (pc_find->key.node_name.str == NULL)
    return TRUE;
  if (pc_find->key.node_name.len != pc_start->key.node_name.len ||
      memcmp(pc_find->key.node_name.str,
             pc_start->key.node_name.str,
             pc_find->key.node_name.len) != 0)
    return FALSE;
  /* Node names were equal */
  return TRUE;
}

static IC_PC_START*
copy_pc_start(IC_PC_START *pc_start,
              IC_MEMORY_CONTAINER *mc_ptr,
              gboolean list_full_flag)
{
  IC_PC_START *new_pc_start;
  guint32 i;

  if ((new_pc_start= (IC_PC_START*)
        mc_ptr->mc_ops.ic_mc_alloc(mc_ptr, sizeof(IC_PC_START))))
    goto mem_error;
  memcpy(new_pc_start, pc_start, sizeof(IC_PC_START));
  if ((new_pc_start->key.grid_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->key.grid_name.len + 1)) ||
      (new_pc_start->key.cluster_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->key.cluster_name.len + 1)) ||
      (new_pc_start->key.node_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->key.node_name.len + 1)) ||
      (new_pc_start->version_string.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->version_string.len + 1)) ||
      (new_pc_start->program_name.str= (gchar*)mc_ptr->mc_ops.ic_mc_calloc(
       mc_ptr, new_pc_start->program_name.len + 1)))
    goto mem_error;
  memcpy(new_pc_start->key.grid_name.str,
         pc_start->key.grid_name.str,
         pc_start->key.grid_name.len);
  new_pc_start->key.grid_name.is_null_terminated= TRUE;

  memcpy(new_pc_start->key.cluster_name.str,
         pc_start->key.cluster_name.str,
         pc_start->key.cluster_name.len);
  new_pc_start->key.cluster_name.is_null_terminated= TRUE;

  memcpy(new_pc_start->key.node_name.str,
         pc_start->key.node_name.str,
         pc_start->key.node_name.len);
  new_pc_start->key.node_name.is_null_terminated= TRUE;

  memcpy(new_pc_start->version_string.str,
         pc_start->version_string.str,
         pc_start->version_string.len);
  new_pc_start->version_string.is_null_terminated= TRUE;

  memcpy(new_pc_start->program_name.str,
         pc_start->program_name.str,
         pc_start->program_name.len);
  new_pc_start->program_name.is_null_terminated= TRUE;

  if (list_full_flag)
  {
    if ((new_pc_start->parameters= (IC_STRING*)mc_ptr->mc_ops.ic_mc_alloc(
         mc_ptr, pc_start->num_parameters * sizeof(IC_STRING))))
      goto mem_error;
    for (i= 0; i < pc_start->num_parameters; i++)
    {
      if ((new_pc_start->parameters[i].str= mc_ptr->mc_ops.ic_mc_calloc(
           mc_ptr, pc_start->parameters[i].len + 1)))
        goto mem_error;
      memcpy(new_pc_start->parameters[i].str,
             pc_start->parameters[i].str,
             pc_start->parameters[i].len);
      new_pc_start->parameters[i].is_null_terminated= TRUE;
    }
  }
  return new_pc_start;

mem_error:
  return NULL;
}

static int
handle_list(IC_CONNECTION *conn, gboolean list_full_flag)
{
  IC_PC_FIND *pc_find= NULL;
  int error;
  IC_PC_START *first_pc_start= NULL;
  IC_PC_START *last_pc_start= NULL;
  IC_PC_START *pc_start, *loop_pc_start, *new_pc_start;
  guint64 current_index, max_index;
  IC_MEMORY_CONTAINER *mc_ptr;
  void *void_pc_start;
  guint32 read_size;
  gchar *read_buf;

  if (!(mc_ptr= ic_create_memory_container(32768, 0, FALSE)))
    goto mem_error;
  if ((error= rec_optional_key_message(conn, &pc_find)))
    goto error;
  
  ic_mutex_lock(pc_hash_mutex);
  max_index= glob_pc_array->dpa_ops.ic_get_max_index(glob_pc_array);
  for (current_index= 0; current_index < max_index; current_index++)
  {
    if ((error= glob_pc_array->dpa_ops.ic_get_ptr(glob_pc_array,
                                                  current_index,
                                                  &void_pc_start)))
      goto error;
    pc_start= (IC_PC_START*)void_pc_start;
    if (pc_start && is_list_match(pc_start, pc_find))
    {
      if (!(new_pc_start= copy_pc_start(pc_start, mc_ptr, list_full_flag)))
        goto mem_error;
      if (!first_pc_start)
        first_pc_start= new_pc_start;
      else
        last_pc_start->next_pc_start= new_pc_start;
      last_pc_start= new_pc_start;
    }
  }
  ic_mutex_unlock(pc_hash_mutex);
  loop_pc_start= first_pc_start;
  while (loop_pc_start)
  {
    if ((error= send_list_entry(conn, loop_pc_start, list_full_flag)))
      goto error;
    if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    {
      if (ic_check_buf(read_buf, read_size, ic_list_stop_str,
                       strlen(ic_list_stop_str)))
        break;
      if (!(ic_check_buf(read_buf, read_size, ic_list_next_str,
                         strlen(ic_list_next_str))))
        goto protocol_error;
    }
    loop_pc_start= loop_pc_start->next_pc_start;
  }
  if (!(ic_rec_simple_str(conn, ic_empty_string)))
    goto protocol_error;
  mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return send_list_stop_reply(conn);

protocol_error:
  error= IC_PROTOCOL_ERROR;
  goto error;
mem_error:
  error= IC_ERROR_MEM_ALLOC;
error:
  if (mc_ptr)
    mc_ptr->mc_ops.ic_mc_free(mc_ptr);
  return send_error_reply(conn,
                          ic_get_error_message(error));
}

/*
  Handle the receive file and write the file to the provided file name.
  For security reasons this function can only write files into the
  config directory. We don't want the client to be able to specify the
  directory since that could be used for too many malicious reasons.
*/
static int
handle_receive_file(IC_CONNECTION *conn,
                    IC_DYNAMIC_PTR_ARRAY *file_name_array,
                    gchar *file_name,
                    guint64 *num_files,
                    guint32 node_id,
                    guint32 number_of_lines)
{
  gchar *read_buf;
  guint32 read_size;
  guint32 i;
  int error;
  IC_FILE_HANDLE file_ptr;
  IC_STRING file_str;

  if ((error= ic_set_config_dir(&file_str,
                                TRUE,
                                node_id)))
    return error;
  if ((error= ic_add_dup_string(&file_str, file_name)))
    return error;

  /* Record the file name to be able to clean up after an error */
  if ((error= file_name_array->dpa_ops.ic_insert_ptr(file_name_array,
                                                     num_files,
                                                     (void*)file_str.str)))
  {
    ic_free(file_str.str);
    return error;
  }
  if ((error= ic_create_file(&file_ptr,
                             file_str.str)))
  {
    /*
      Clean up since error handling will attempt to delete file. We don't
      want the file deleted since the error could have been that the file
      already existed.
    */
    ic_free(file_str.str);
    (*num_files)--;
    return error;
  }
  for (i= 0; i < number_of_lines; i++)
  {
    /* Receive line from connection and write line received to file */
    if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)) ||
        (error= ic_write_file(file_ptr, read_buf, read_size)))
      goto error;
  }
  if ((error= ic_rec_empty_line(conn)) ||
      (error= ic_close_file(file_ptr)))
    goto error;
  return 0;
error:
  (void)ic_close_file(file_ptr); /* Ignore error here */
  return error;
}

/*
  This method receives a number of files with their content from the client
  and installs those as configuration files in the proper directory where
  iClaustron expects to find the configuration files.

  This method is atomic, it will either write and create all files sent and
  received or it will remove all the files it created and wrote if anything
  goes wrong before the entire file transfer is completed.

  To aid in this we use a dynamic pointer array.
*/
static int
handle_copy_cluster_server_files(IC_CONNECTION *conn)
{
  int error;
  void *mem_alloc_object;
  guint32 num_lines;
  guint32 num_clusters;
  guint32 i;
  guint32 node_id;
  guint64 num_files= 0;
  IC_STRING file_name;
  gchar file_name_buf[IC_MAX_FILE_NAME_SIZE];
  IC_DYNAMIC_PTR_ARRAY *file_name_array;

  if (!(file_name_array= ic_create_dynamic_ptr_array()))
    return IC_ERROR_MEM_ALLOC;

  /* Receive config.ini */
  if ((error= ic_rec_number(conn, ic_cluster_server_node_id_str,
                            &node_id)) ||
      (error= ic_rec_number(conn, ic_number_of_clusters_str,
                            &num_clusters)) ||
      (error= ic_rec_simple_str(conn, ic_receive_config_ini_str)) ||
      (error= ic_rec_number(conn, ic_number_of_lines_str,
                            &num_lines)))
    goto error_delete_files;

  /* Create the config.ini file name */
  ic_create_config_file_name(&file_name,
                             file_name_buf,
                             NULL,
                             &ic_config_string,
                             0);
  if ((error= handle_receive_file(conn,
                                  file_name_array,
                                  file_name.str,
                                  &num_files,
                                  node_id,
                                  num_lines)))
    goto error_delete_files;
  if ((error= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
      (error= ic_send_empty_line(conn)))
    goto error_delete_files;

  /* Receive grid_common.ini */
  if ((error= ic_rec_simple_str(conn, ic_receive_grid_common_ini_str)) ||
      (error= ic_rec_number(conn, ic_number_of_lines_str,
                            &num_lines)))
    goto error_delete_files;

  /* Create the grid_common.ini file name */
  ic_create_config_file_name(&file_name,
                             file_name_buf,
                             NULL,
                             &ic_grid_common_config_string,
                             0);
  if ((error= handle_receive_file(conn,
                                  file_name_array,
                                  file_name.str,
                                  &num_files,
                                  node_id,
                                  num_lines)))
    goto error_delete_files;
  if ((error= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
      (error= ic_send_empty_line(conn)))
    goto error_delete_files;

  /* Receive all cluster config files */
  for (i= 0; i < num_clusters; i++)
  {
    if ((error= ic_rec_string(conn,
                              ic_receive_cluster_name_ini_str,
                              file_name_buf)) ||
        (error= ic_rec_number(conn, ic_number_of_lines_str,
                              &num_lines)))
      goto error_delete_files;
    if ((error= handle_receive_file(conn,
                                    file_name_array,
                                    file_name_buf,
                                    &num_files,
                                    node_id,
                                    num_lines)))
      goto error_delete_files;
    if ((error= ic_send_with_cr(conn, ic_receive_config_file_ok_str)) ||
        (error= ic_send_empty_line(conn)))
      goto error_delete_files;
  }
  /* Protocol complete and worked ok, clean up dynamic pointer array */
  file_name_array->dpa_ops.ic_free_dynamic_ptr_array(file_name_array);
  return 0;

error_delete_files:
  for (i= 0; i < num_files; i++)
  {
    ic_require(file_name_array->dpa_ops.ic_get_ptr(file_name_array,
                                                   (guint64)i,
                                                   &mem_alloc_object));
    (void)ic_delete_file((gchar*)mem_alloc_object); /* Ignore error */
    ic_free((gchar*)mem_alloc_object);
  }
  file_name_array->dpa_ops.ic_free_dynamic_ptr_array(file_name_array);
  return error;
}

static int
handle_get_cpu_info(IC_CONNECTION *conn)
{
  int error;
  guint32 num_cpus;
  guint32 num_numa_nodes;
  guint32 cpus_per_core;
  guint32 len;
  guint32 i;
  IC_CPU_INFO *cpu_info;
  const gchar *buf_array[6];
  gchar cpu_num_str[IC_NUMBER_SIZE];
  gchar numa_node_str[IC_NUMBER_SIZE];
  gchar core_num_str[IC_NUMBER_SIZE];

  if ((error= ic_rec_empty_line(conn)))
    return error;

  if ((error= ic_get_cpu_info(&num_cpus,
                              &num_numa_nodes,
                              &cpus_per_core,
                              &cpu_info)) ||
      num_cpus == 0)
  {
    if ((error= ic_send_with_cr(conn, ic_no_cpu_info_available_str)) ||
        (error= ic_send_empty_line(conn)))
      return error;
    return 0;
  }

  if ((error= ic_send_with_cr_with_num(conn, ic_number_of_cpus_str,
                                       (guint64)num_cpus)) ||
      (error= ic_send_with_cr_with_num(conn, ic_number_of_numa_nodes_str,
                                       (guint64)num_numa_nodes)) ||
      (error= ic_send_with_cr_with_num(conn, ic_number_of_cpus_per_core_str,
                                       (guint64)cpus_per_core)))
    goto error;
  for (i= 0; i < num_cpus; i++)
  {
    buf_array[0]= ic_cpu_str;
    buf_array[1]= ic_guint64_str((guint64)cpu_info[i].cpu_id,
                                 cpu_num_str,
                                 &len);
    buf_array[2]= ic_cpu_node_str;
    buf_array[3]= ic_guint64_str((guint64)cpu_info[i].numa_node_id,
                                 numa_node_str,
                                 &len);
    buf_array[4]= ic_core_str;
    buf_array[5]= ic_guint64_str((guint64)cpu_info[i].core_id,
                                 core_num_str,
                                 &len);
    if ((error= ic_send_with_cr_composed(conn, buf_array, (guint32)6)))
      goto error;
  }
  error= ic_send_empty_line(conn);

error:
  ic_free((gchar*)cpu_info);
  return error;
}

static int
handle_get_memory_info(IC_CONNECTION *conn)
{
  int error;
  guint32 num_numa_nodes;
  guint32 i;
  guint32 len;
  guint64 total_memory_size;
  IC_MEM_INFO *mem_info;
  const gchar *buf_array[4];
  gchar numa_node_str[IC_NUMBER_SIZE];
  gchar memory_size_str[IC_NUMBER_SIZE];

  if ((error= ic_rec_empty_line(conn)))
    return error;

  if ((error= ic_get_mem_info(&num_numa_nodes,
                              &total_memory_size,
                              &mem_info)) ||
      num_numa_nodes == 0)
  {
    if ((error= ic_send_with_cr(conn, ic_no_mem_info_available_str)) ||
        (error= ic_send_empty_line(conn)))
      return error;
    return 0;
  }
  if ((error= ic_send_with_cr_with_num(conn,
                                       ic_number_of_mbyte_user_memory_str,
                                       (guint64)(total_memory_size/(1024 * 1024)))) ||
      (error= ic_send_with_cr_with_num(conn, ic_number_of_numa_nodes_str,
                                       (guint64)num_numa_nodes)))
    goto error;
  for (i= 0; i < num_numa_nodes; i++)
  {
    buf_array[0]= ic_mem_node_str;
    buf_array[1]= ic_guint64_str((guint64)mem_info[i].numa_node_id,
                                 numa_node_str,
                                 &len);
    buf_array[2]= ic_mb_user_memory_str;
    buf_array[3]= ic_guint64_str(mem_info[i].memory_size,
                                 memory_size_str,
                                 &len);
    if ((error= ic_send_with_cr_composed(conn, buf_array, (guint32)4)))
      goto error;
  }
  error= ic_send_empty_line(conn);

error:
  ic_free((gchar*)mem_info);
  return error;
}

static int
handle_get_disk_info(IC_CONNECTION *conn)
{
  int error;
  guint64 disk_space;
  gchar dir_name_buf[IC_MAX_FILE_NAME_SIZE];

  /* Get directory which we want to analyze how much disk space can be used */
  if ((error= ic_rec_string(conn, ic_dir_str,
                            dir_name_buf)) ||
      (error= ic_rec_empty_line(conn)))
    return error;

  /* Check disk space now */
  ic_get_disk_info(dir_name_buf, &disk_space);
  if (disk_space == 0)
  {
    /*
      Report that info isn't accessible, we don't report any info on why for
      security reasons. We don't want a user to be able to check the existence
      of a directory if he doesn't have permission to it.
    */
    if ((error= ic_send_with_cr(conn, ic_no_disk_info_available_str)))
      return error;
  }
  else
  {
    /* Report disk space found available in this directory */
    if ((error= ic_send_with_cr_two_strings(conn,
                                            ic_dir_str,
                                            dir_name_buf)) ||
        (error= ic_send_with_cr_with_num(conn,
                                         ic_disk_space_str,
                                         disk_space)))
      return error;
  }
  error= ic_send_empty_line(conn);
  return error;
}

/*
  The command handler is a thread that is executed on behalf of one of the
  Cluster Managers in a Grid.

  The Cluster Manager can ask the iClaustron Process Controller for 5 tasks.
  1) It can ask to get a node started
  2) It can ask to stop a node gracefully (kill)
  3) It can ask to stop a node in a hard manner (kill -9)
  4) It can also ask for a list of the programs currently running in the Grid
  5) As part of the bootstrap it can be asked to install the configuration
     files of the Cluster Servers.

  GENERIC PROTOCOL
  ----------------
  The protocol between the iClaustron Process Controller and the Cluster
  Manager is entirely half-duplex and it is always the Cluster Manager
  asking for services from the Process Controller. The only action the
  Process Controller performs automatically is to restart a program if
  it discovers it has died when the autorestart flag is set to true in
  the start command.

  Cluster Servers use the Cluster Name * to indicate that they are part
  of all clusters inside a grid. The same goes for Cluster Managers.

  START PROTOCOL
  --------------
  The protocol for starting node is:
  Line 1: start
  Line 2: program: Program Name
  Line 3: version: Version string
          (either an iClaustron version or a MySQL version string)
  Line 4: grid: Grid Name
  Line 5: cluster: Cluster Name
  Line 6: node: Node name
  Line 7: autorestart true OR autorestart false
  Line 8: num parameters: Number of parameters
  Line 9 - Line x: Program Parameters on the format:
    Parameter name, 1 space, Type of parameter, 1 space, Parameter Value
  Line x+1: Empty line to indicate end of message

  Example: start\nprogram: ic_fsd?\nversion: iclaustron-0.0.1\n
           grid: my_grid\ncluster: my_cluster\nnode: my_node\n
           autorestart: true\nnum parameters: 2\n
           parameter: data_dir string /home/mikael/iclaustron\n\n

  The successful response is:
  Line 1: Ok
  Line 2: pid: Process Id
  Line 3: Empty line

  Example: ok\npid: 1234\n\n

  The unsuccessful response is:
  Line 1: Error
  Line 2: error: message
  Line 3: empty line

  Example: error:\nerror: Failed to start process\n\n

  STOP/KILL PROTOCOL
  ------------------
  The protocol for stopping/killing a node is:
  Line 1: stop/kill
  Line 2: grid: Grid Name
  Line 3: cluster: Cluster Name
  Line 4: node: Node name
  Line 5: Empty line

  The successful response is:
  Line 1: ok
  Line 2: Empty Line

  The unsuccessful response is:
  Line 1: error
  Line 2: error: message

  LIST PROTOCOL
  -------------
  The protocol for listing nodes is:
  Line 1: list [full]
  Line 2: grid: Grid Name (optional)
  Line 3: cluster: Cluster Name (optional)
  Line 4: node: Node Name (optional)
  Line 5: Empty Line

  Only Grid Name is required, if Cluster Name is provided, only programs in
  this cluster will be provided, if Node Name is also provided then only a
  program running this node will be provided.

  The response is a set of program descriptions as:
  NOTE: It is very likely this list will be expanded with more data on the
  state of the process, such as use of CPU, use of memory, disk and so forth.
  Line 1: list node
  Line 2: program: Program Name
  Line 3: version: Version string
  Line 4: grid: Grid Name
  Line 5: cluster: Cluster Name
  Line 6: node: Node Name
  Line 7: start time: Start Time
  Line 8: pid: Process Id
  Line 9: Number of parameters
  Line 10 - Line x: Parameter in same format as when starting (only if list
    full)
  The protocol to check status is:
  Line x+1: Empty Line

  After receiving one such line the Cluster Manager responds with either
  list next or list stop to stop the listing as:
  Line 1: list next
  Line 2: Empty Line
  or
  Line 1: list stop
  Line 2: Empty Line

  After list next a new node will be sent, when there are no more nodes to
  send the Process Controller will respond with:
  Line 1: list stop
  Line 2: Empty Line

  INSTALL CONFIGURATION FILES PROTOCOL
  ------------------------------------
  Line 1: copy cluster server files
  Line 2: cluster server node id: #node
  Line 3: number of clusters: #clusters
  Line 4: receive config.ini
  Line 5: number of lines: #lines
  Line 6 - Line x: Contains the data from the config.ini file

  Response:
  Line 1: receive config file ok

  As usual in the iClaustron Protocol an empty lines indicates end of
  the communication from one side.

  Then the next file is copied with the same protocol but replacing
  config.ini by grid_common.ini.

  Then the protocol is repeated once per cluster to install where the
  filename is cluster_name.ini, so e.g. kalle.ini if the name of the
  cluster is kalle.

  After receiving the last file the server waits for other protocol
  messages or that the client closes the connection.

  GET CPU INFO PROTOCOL
  ---------------------
  This protocol asks the iClaustron Process Controller about the number
  of CPUs, how many cores, how many threads and how many NUMA nodes there
  are in the computer. If the process runs on a virtual machine it is the
  resources this virtual machine have access to which is asked for.

  The information provided here is 3 levels of CPU information, it is
  assumed that CPUs consists of NUMA nodes which usually is equal to
  CPU chips and often also a CPU socket. Each NUMA node have a set of
  CPU cores. Finally each CPU core can have multiple CPU threads. We
  call the lowest level cpu here, there are different names used for it
  in different products.

  We report the number of cpus, number of NUMA nodes and number of cpus
  per core. Then the mapping is provided by specifying cpu id together
  with information onto which NUMA node and CPU core this is related.

  The receiver of this information can use this information to discover
  how to bind iClaustron processes to the correct set of CPUs. It can
  also bind memory allocation to a certain set of NUMA nodes for the
  iClaustron programs.

  This information is valuable for any configurator program that tries to
  assist the user in providing a reasonable configuration based on the HW
  the user have provided.

  Line 1: get cpu info

  Response:
  Line 1: number of cpus: #cpus
  Line 2: number of NUMA nodes: #nodes
  Line 3: number of cpus per core: #cpus_per_core
  Line 4 - (3 + #cpus): cpu: 0, node: 0, core: 0

  GET MEMORY INFORMATION PROTOCOL
  -------------------------------
  This protocol provides the ability to ask the iClaustron Process Controller
  about how much user memory is available to the iClaustron programs on the
  computer, it also lists this information per NUMA node.

  Line 1: get memory info

  Response:
  Line 1: number of MByte user memory: #MB_user_memory
  Line 2: number of NUMA nodes: #nodes
  Line 3 - (2 + #nodes): node: 0, MB user memory: #MB_user_memory_in_node

  GET DISK INFORMATION PROTOCOL
  -----------------------------
  This protocol provides the ability to ask the iClaustron Process Controller
  about 
  Line 1: get disk info
  Line 2: dir: @directory

  Response:
  Line 1: dir: @directory
  Line 2: disk space: #GBytes

  Or:
  Line 1: dir: @directory
  Line 2: error string: @error_string
*/

static gpointer
run_command_handler(gpointer data)
{
  gchar *read_buf;
  guint32 read_size;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_THREADPOOL_STATE *tp_state;
  IC_CONNECTION *conn;
  int ret_code;
  DEBUG_THREAD_ENTRY("run_command_handler");
  tp_state= thread_state->ic_get_threadpool(thread_state);
  conn= (IC_CONNECTION*)
    tp_state->ts_ops.ic_thread_get_object(thread_state);

  tp_state->ts_ops.ic_thread_started(thread_state);

  while (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (ic_check_buf(read_buf, read_size, ic_start_str,
                     strlen(ic_start_str)))
    {
      if ((ret_code= handle_start(conn)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, ic_stop_str,
                          strlen(ic_stop_str)))
    {
      if ((ret_code= handle_stop(conn, FALSE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, ic_kill_str,
                          strlen(ic_kill_str)))
    {
      if ((ret_code= handle_stop(conn, TRUE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, ic_list_str,
                          strlen(ic_list_str)))
    {
      if ((ret_code= handle_list(conn, FALSE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, ic_list_full_str,
                          strlen(ic_list_full_str)))
    {
      if ((ret_code= handle_list(conn, TRUE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size,
                          ic_copy_cluster_server_files_str,
                          strlen(ic_copy_cluster_server_files_str)))
    {
      if ((ret_code= handle_copy_cluster_server_files(conn)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size,
                          ic_get_cpu_info_str,
                          strlen(ic_get_cpu_info_str)))
    {
      if ((ret_code= handle_get_cpu_info(conn)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size,
                          ic_get_memory_info_str,
                          strlen(ic_get_memory_info_str)))
    {
      if ((ret_code= handle_get_memory_info(conn)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size,
                          ic_get_disk_info_str,
                          strlen(ic_get_disk_info_str)))
    {
      if ((ret_code= handle_get_disk_info(conn)))
        break;
    }
    else if (read_size)
    {
      read_buf[read_size]= 0;
      ic_printf("Received a message not expected: %s", read_buf);
      ret_code= IC_PROTOCOL_ERROR;
      /* Closing connection */
      break;
    }
  }
  if (ret_code)
    ic_print_error(ret_code);
  conn->conn_op.ic_free_connection(conn);
  tp_state->ts_ops.ic_thread_stops(thread_state);
  DEBUG_THREAD_RETURN;
}

int start_connection_loop(IC_THREADPOOL_STATE *tp_state)
{
  int ret_code;
  guint32 thread_id;
  IC_CONNECTION *conn= NULL;
  IC_CONNECTION *fork_conn;

  DEBUG_ENTRY("start_connection_loop");
  if (!(conn= ic_create_socket_object(FALSE, FALSE, FALSE,
                                       CONFIG_READ_BUF_SIZE,
                                       NULL, NULL)))
    DEBUG_RETURN_INT(IC_ERROR_MEM_ALLOC);
  conn->conn_op.ic_prepare_server_connection(conn,
                                             glob_server_name,
                                             glob_server_port,
                                             NULL,
                                             NULL,
                                             0,
                                             TRUE);
  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);
  if (ret_code)
    DEBUG_RETURN_INT(ret_code);
  do
  {
    do
    {
      tp_state->tp_ops.ic_threadpool_check_threads(tp_state);
      /* Wait for someone to connect to us. */
      if ((ret_code= conn->conn_op.ic_accept_connection(conn)))
      {
        ic_printf("Error %d received in accept connection", ret_code);
        break;
      }
      if (!(fork_conn= 
            conn->conn_op.ic_fork_accept_connection(conn,
                                        FALSE)))  /* No mutex */
      {
        ic_printf("Error %d received in fork accepted connection", ret_code);
        break;
      }
      /*
        We have an active connection, we'll handle the connection in a
        separate thread.
      */
      if (tp_state->tp_ops.ic_threadpool_start_thread(tp_state,
                                                      &thread_id,
                                                      run_command_handler,
                                                      fork_conn,
                                                      IC_SMALL_STACK_SIZE,
                                                      FALSE))
      {
        ic_printf("Failed to create thread after forking accept connection");
        fork_conn->conn_op.ic_free_connection(fork_conn);
        break;
      }
    } while (0);
  } while (!ic_get_stop_flag());
  ic_printf("iClaustron Process Controller was stopped");
  DEBUG_RETURN_INT(0);
}

static GOptionEntry entries[] = 
{
  { "server_name", 0, 0, G_OPTION_ARG_STRING,
    &glob_server_name,
    "Set server address of process controller", NULL},
  { "server_port", 0, 0, G_OPTION_ARG_STRING,
    &glob_server_port,
    "Set server port, default = 11860", NULL},
  { "iclaustron_version", 0, 0, G_OPTION_ARG_STRING,
    &ic_glob_version_path,
    "Version string to find iClaustron binaries used by this program, default "
    IC_VERSION_STR, NULL},
  { "daemonize", 0, 0, G_OPTION_ARG_INT,
     &glob_daemonize,
    "Daemonize program", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int main(int argc, char *argv[])
{
  int ret_code= 0;
  IC_THREADPOOL_STATE *tp_state= NULL;
  IC_STRING log_file;
 
  IC_INIT_STRING(&log_file, NULL, 0, FALSE);
  if ((ret_code= ic_start_program(argc, argv, entries, NULL,
                                  glob_process_name,
           "- iClaustron Control Server", TRUE)))
    return ret_code;
  if (glob_daemonize)
  {
    if ((ret_code= ic_add_dup_string(&log_file, ic_glob_data_dir.str)) ||
        (ret_code= ic_add_dup_string(&log_file, "ic_pcntrld.log")))
      goto error;
    if ((ret_code= ic_daemonize(log_file.str)))
      goto error;
  }
  ic_set_die_handler(NULL, NULL);
  ic_set_sig_error_handler(NULL, NULL);
  DEBUG_PRINT(PROGRAM_LEVEL, ("Base directory: %s",
                              ic_glob_base_dir.str));
  DEBUG_PRINT(PROGRAM_LEVEL, ("Data directory: %s",
                              ic_glob_data_dir.str));
  if (!(tp_state= ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE, TRUE)))
    return IC_ERROR_MEM_ALLOC;
  if (!(glob_pc_hash= create_pc_hash()))
    goto error;
  if (!(pc_hash_mutex= ic_mutex_create()))
    goto error;
  if (!(glob_pc_array= ic_create_dynamic_ptr_array()))
    goto error;
  /*
    First step is to set-up path to where the binaries reside. All binaries
    we control will reside under this directory. Under this directory the
    binaries will be placed in either
    MYSQL_VERSION/bin
    or
    ICLAUSTRON_VERSION/bin
    and libraries will be in either
    MYSQL_VERSION/lib/mysql
    or
    ICLAUSTRON_VERSION/lib

    The default directory is $HOME/iclaustron_install if we're not running this
    as root, if we're running as root, then the default directory is
    /var/lib/iclaustron_install.

    The resulting base directory is stored in the global variable
    ic_glob_base_dir. We set up the default strings for the
    version we've compiled, the protocol to the process controller
    enables the Cluster Manager to specify which version to use.

    This program will have it's state in memory but will also checkpoint this
    state to a file. This is to ensure that we are able to connect to processes
    again even after we had to restart this program. This file is stored in
    the iclaustron_data placed beside the iclaustron_install directory. The
    file is called pcntrl_state.
  */
  /*
    Next step is to wait for Cluster Managers to connect to us, after they
    have connected they can request action from us as well. Any server can
    connect as a Cluster Manager, but they have to provide the proper
    password in order to get connected.

    We will always be ready to receive new connections.

    The manner to stop this program is by performing a kill -15 operation
    such that the program receives a SIGKILL signal. This program cannot
    be started and stopped from a Cluster Manager for security reasons.
  */
  ret_code= start_connection_loop(tp_state);
error:
  if (tp_state)
    tp_state->tp_ops.ic_threadpool_stop(tp_state);
  if (glob_pc_hash)
    ic_hashtable_destroy(glob_pc_hash);
  if (pc_hash_mutex)
    ic_mutex_destroy(pc_hash_mutex);
  if (log_file.str)
    ic_free(log_file.str);
  if (glob_pc_array)
    glob_pc_array->dpa_ops.ic_free_dynamic_ptr_array(glob_pc_array);
  ic_end();
  return ret_code;
}
