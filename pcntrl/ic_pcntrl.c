/* Copyright (C) 2007-2009 iClaustron AB

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


#define REC_PROG_NAME 0
#define REC_PARAM 1
#define REC_FINAL_CR 2

/* Protocol strings */
static const gchar *ok_str= "ok";
static const gchar *start_str= "start";
static const gchar *stop_str= "stop";
static const gchar *kill_str= "kill";
static const gchar *list_str= "list";
static const gchar *list_full_str= "list full";
static const gchar *list_next_str= "list next";
static const gchar *list_node_str= "list node";
static const gchar *list_stop_str= "list stop";
static const gchar *auto_restart_true_str= "autorestart: true";
static const gchar *auto_restart_false_str= "autorestart: false";
static const gchar *num_parameters_str= "num parameters: ";

/* Configurable variables */
static gchar *glob_ip= NULL;
static gchar *glob_port= "10002";
static gchar *glob_base_dir= NULL;

/* Global variables */
static const gchar *glob_process_name= "ic_pcntrld";
static guint32 glob_stop_flag= FALSE;
static IC_STRING glob_base_dir_string;
static IC_STRING glob_ic_base_dir_string;
static IC_STRING glob_mysql_base_dir_string;
static GMutex *pc_hash_mutex= NULL;
static IC_HASHTABLE *glob_pc_hash= NULL;
static IC_THREADPOOL_STATE *glob_tp_state;
static guint64 glob_start_id= 1;
static IC_DYNAMIC_TRANSLATION *glob_dyn_trans= NULL;

static GOptionEntry entries[] = 
{
  { "ip", 0, 0, G_OPTION_ARG_STRING, &glob_ip,
    "Set IP address, default is IP address of computer", NULL},
  { "port", 0, 0, G_OPTION_ARG_STRING, &glob_port, "Set Port, default = 10002", NULL},
  { "basedir", 0, 0, G_OPTION_ARG_STRING, &glob_base_dir,
    "Sets path to binaries controlled by this program", NULL},
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static int
send_error_reply(IC_CONNECTION *conn, const gchar *error_message)
{
  int error;
  if ((error= ic_send_with_cr(conn, "error")) ||
      (error= ic_send_with_cr(conn, error_message)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_ok_reply(IC_CONNECTION *conn)
{
  int error;
  if ((error= ic_send_with_cr(conn, ok_str)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_ok_pid_reply(IC_CONNECTION *conn, GPid pid)
{
  int error;

  if ((error= ic_send_with_cr(conn, ok_str)) ||
      (error= ic_send_pid(conn, pid)) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
send_list_entry(IC_CONNECTION *conn,
                IC_PC_START *pc_start,
                gboolean list_full_flag)
{
  gchar start_time_buf[64];
  gchar process_id_buf[64];
  gchar num_param_buf[64];
  gchar *start_time_ptr, *process_id_ptr, *num_param_ptr;
  guint32 start_time_len, process_id_len, num_param_len;
  guint32 i;
  int error;

  start_time_ptr= ic_guint64_str(pc_start->start_id,
                                 start_time_buf,
                                 &start_time_len);
  process_id_ptr= ic_guint64_str((guint64)pc_start->pid,
                                 process_id_buf,
                                 &process_id_len);
  num_param_ptr= ic_guint64_str((guint64)pc_start->num_parameters,
                                num_param_buf,
                                &num_param_len);

  if ((error= ic_send_with_cr(conn, list_node_str)) ||
      (error= ic_send_with_cr(conn, pc_start->key.grid_name.str)) ||
      (error= ic_send_with_cr(conn, pc_start->key.cluster_name.str)) ||
      (error= ic_send_with_cr(conn, pc_start->key.node_name.str)) ||
      (error= ic_send_with_cr(conn, pc_start->program_name.str)) ||
      (error= ic_send_with_cr(conn, pc_start->version_string.str)) ||
      (error= ic_send_with_cr(conn, start_time_ptr)) ||
      (error= ic_send_with_cr(conn, process_id_ptr)) ||
      (error= ic_send_with_cr(conn, num_param_ptr)))
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
  if ((error= ic_send_with_cr(conn, "list stop")) ||
      (error= ic_send_empty_line(conn)))
    return error;
  return 0;
}

static int
get_optional_str(IC_CONNECTION *conn,
                 IC_MEMORY_CONTAINER *mc_ptr,
                 IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *str_ptr;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == 0)
    return 0;
  if (!(str_ptr= mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, read_size+1)))
    return IC_ERROR_MEM_ALLOC;
  memcpy(str_ptr, read_buf, read_size);
  IC_INIT_STRING(str, str_ptr, read_size, TRUE);
  return 0;
}

static int
get_str(IC_CONNECTION *conn,
        IC_MEMORY_CONTAINER *mc_ptr,
        IC_STRING *str)
{
  int error;
  guint32 read_size;
  gchar *str_ptr;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == 0)
    return IC_PROTOCOL_ERROR;
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

  if ((error= get_str(conn, mc_ptr, &pc_key->grid_name)) ||
      (error= get_str(conn, mc_ptr, &pc_key->cluster_name)) ||
      (error= get_str(conn, mc_ptr, &pc_key->node_name)))
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

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (read_size == strlen(auto_restart_true_str) &&
      !memcmp(read_buf, auto_restart_true_str, read_size))
  {
    *auto_restart= 1;
    return 0;
  }
  else if (read_size == strlen(auto_restart_false_str) &&
           !memcmp(read_buf, auto_restart_false_str, read_size))
  {
    *auto_restart= 0;
    return 0;
  }
  else
    return IC_PROTOCOL_ERROR;
}

static int
get_num_parameters(IC_CONNECTION *conn,
                   guint32 *num_parameters)
{
  int error;
  guint32 read_size;
  guint64 loc_num_params;
  guint32 num_param_len= strlen(num_parameters_str);
  guint32 len;
  gchar *read_buf;

  if ((error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    return error;
  if (memcmp(read_buf, num_parameters_str, read_size))
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
  if ((mc_ptr= ic_create_memory_container((guint32)1024, (guint32) 0)))
    return IC_ERROR_MEM_ALLOC;
  if ((*pc_start= (IC_PC_START*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_PC_START))))
    goto mem_error;
  loc_pc_start= *pc_start;
  loc_pc_start->mc_ptr= mc_ptr;

  if ((error= get_str(conn, mc_ptr, &loc_pc_start->version_string)) ||
      (error= get_key(conn, mc_ptr, &loc_pc_start->key)) ||
      (error= get_str(conn, mc_ptr, &loc_pc_start->program_name)) ||
      (error= get_autorestart(conn, &loc_pc_start->autorestart)) ||
      (error= get_num_parameters(conn, &loc_pc_start->num_parameters)))
    goto error;
  if ((loc_pc_start->parameters= (IC_STRING*)
       mc_ptr->mc_ops.ic_mc_calloc(mc_ptr, sizeof(IC_STRING)*
         loc_pc_start->num_parameters)))
    goto mem_error;
  for (i= 0; i < loc_pc_start->num_parameters; i++)
  {
    if ((error= get_str(conn, mc_ptr, &loc_pc_start->parameters[i])))
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
  if (((*mc_ptr)= ic_create_memory_container((guint32)1024, (guint32) 0)))
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
  if ((error= get_optional_str(conn, mc_ptr, &(*pc_find)->key.grid_name)))
    goto error;
  if ((*pc_find)->key.grid_name.len == 0)
    return 0; /* No grid name provided, list all programs */
  if ((error= get_optional_str(conn, mc_ptr, &(*pc_find)->key.cluster_name)))
    goto error;
  if ((*pc_find)->key.cluster_name.len == 0)
    return 0; /* No cluster name provided, list all programs in grid */
  if ((error= get_optional_str(conn, mc_ptr, &(*pc_find)->key.node_name)))
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
  g_assert(pc_start_check == pc_start);
  glob_dyn_trans->dt_ops.ic_remove_translation_object(glob_dyn_trans,
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
  if ((ret_code= glob_dyn_trans->dt_ops.ic_insert_translation_object(
                                                       glob_dyn_trans,
                                                       &index,
                                                       (void*)pc_start)))
    return ret_code;
  pc_start->dyn_trans_index= index;
  if ((ret_code= ic_hashtable_insert(glob_pc_hash,
                                     (void*)pc_start, (void*)pc_start)))
  {
    glob_dyn_trans->dt_ops.ic_remove_translation_object(glob_dyn_trans,
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
  g_mutex_lock(pc_hash_mutex);
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
      g_mutex_unlock(pc_hash_mutex);
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
        g_assert(ret_code == IC_ERROR_PROCESS_NOT_ALIVE);
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
  g_mutex_unlock(pc_hash_mutex);
  return ret_code;
}

static int
handle_start(IC_CONNECTION *conn)
{
  gchar **arg_vector;
  int ret_code;
  guint32 i;
  GPid pid;
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
  if ((ret_code= ic_set_base_path(&working_dir,
                                  glob_base_dir,
                                  pc_start->version_string.str,
                                  NULL)))
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
  g_mutex_lock(pc_hash_mutex);
  /*
    pc_start struct is protected by mutex, as soon as the pid is nonzero
    anyone can remove it from hash table and release the memory. Thus
    need to be careful with updating the value from zero.
    We also update the start_id at this point to ensure that we can
    distinguish entries of the same key from each other.
  */
  pc_start->pid= pid;
  pc_start->start_id= glob_start_id++;
  g_mutex_unlock(pc_hash_mutex);
  return send_ok_pid_reply(conn, pc_start->pid);

late_error:
  g_mutex_lock(pc_hash_mutex);
  pc_start_check= ic_hashtable_remove(glob_pc_hash,
                                      (void*)pc_start);
  g_assert(pc_start_check == pc_start);
  g_mutex_unlock(pc_hash_mutex);
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
  GPid pid;
  guint64 start_id= 0;
  int loop_count= 0;
  gchar *program_name;

  if ((error= rec_key_message(conn, &pc_find)))
    goto error;

try_again:
  g_mutex_lock(pc_hash_mutex);
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
      g_mutex_unlock(pc_hash_mutex);
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
    g_mutex_unlock(pc_hash_mutex);
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
      g_assert(error == IC_ERROR_PROCESS_NOT_ALIVE);
      /*
        We were successful in stopping the process, now we need to
        remove the entry from the hash.
      */
      g_mutex_lock(pc_hash_mutex);
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
      g_mutex_unlock(pc_hash_mutex);
    }
  }
  else
  {
    /*
      We couldn't find any such process in the hash, this means that the
      process doesn't exist and we have succeeded since our aim was to
      ensure the process was stopped.
    */
    g_mutex_unlock(pc_hash_mutex);
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

  if (!(mc_ptr= ic_create_memory_container(32768, 0)))
    goto mem_error;
  if ((error= rec_optional_key_message(conn, &pc_find)))
    goto error;
  
  g_mutex_lock(pc_hash_mutex);
  max_index= glob_dyn_trans->dt_ops.ic_get_max_index(glob_dyn_trans);
  for (current_index= 0; current_index < max_index; current_index++)
  {
    if ((error= glob_dyn_trans->dt_ops.ic_get_translation_object(
                              glob_dyn_trans,
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
  g_mutex_unlock(pc_hash_mutex);
  loop_pc_start= first_pc_start;
  while (loop_pc_start)
  {
    if ((error= send_list_entry(conn, loop_pc_start, list_full_flag)))
      goto error;
    if (!(error= ic_rec_with_cr(conn, &read_buf, &read_size)))
    {
      if (ic_check_buf(read_buf, read_size, list_stop_str,
                       strlen(list_stop_str)))
        break;
      if (!(ic_check_buf(read_buf, read_size, list_next_str,
                         strlen(list_next_str))))
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
  The command handler is a thread that is executed on behalf of one of the
  Cluster Managers in a Grid.

  The Cluster Manager can ask the iClaustron Process Controller for 4 tasks.
  1) It can ask to get a node started
  2) It can ask to stop a node gracefully (kill)
  3) It can ask to stop a node in a hard manner (kill -9)
  4) It can also ask for a list of the programs currently running in the Grid

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
  Line 2: Version string (either an iClaustron version or a MySQL version string)
  Line 3: Grid Name
  Line 4: Cluster Name
  Line 5: Node name
  Line 6: Program Name
  Line 7: autorestart true OR autorestart false
  Line 8: Number of parameters
  Line 9 - Line x: Program Parameters on the format:
    Parameter name, 1 space, Type of parameter, 1 space, Parameter Value
  Line x+1: Empty line to indicate end of message

  Example: start\nversion: iclaustron-0.0.1\ngrid: my_grid\n
           cluster: my_cluster\nnode: my_node\nprogram: ic_fsd\n
           autorestart: true\nnum parameters: 2\n
           parameter: data_dir string /home/mikael/iclaustron\n\n

  The successful response is:
  Line 1: ok
  Line 2: Process Id
  Line 3: Empty line

  Example: ok\npid: 1234\n\n

  The unsuccessful response is:
  Line 1: error
  Line 2: message

  Example: error:\nerror: Failed to start process\n\n

  STOP/KILL PROTOCOL
  ------------------
  The protocol for stopping/killing a node is:
  Line 1: stop/kill
  Line 2: Grid Name
  Line 3: Cluster Name
  Line 4: Node name
  Line 5: Empty line

  The successful response is:
  Line 1: ok
  Line 2: Empty Line

  The unsuccessful response is:
  Line 1: error
  Line 2: message

  LIST PROTOCOL
  -------------
  The protocol for listing nodes is:
  Line 1: list [full]
  Line 2: Grid Name (optional)
  Line 3: Cluster Name (optional)
  Line 4: Node Name (optional)
  Line 5: Empty Line

  Only Grid Name is required, if Cluster Name is provided, only programs in this
  cluster will be provided, if Node Name is also provided then only a program
  running this node will be provided.

  The response is a set of program descriptions as:
  NOTE: It is very likely this list will be expanded with more data on the
  state of the process, such as use of CPU, use of memory, disk and so forth.
  Line 1: list node
  Line 2: Grid Name
  Line 3: Cluster Name
  Line 4: Node Name
  Line 5: Program Name
  Line 6: Version string
  Line 7: Start Time
  Line 8: Process Id
  Line 9: Number of parameters
  Line 10 - Line x: Parameter in same format as when starting (only if list full)
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
*/

static gpointer
run_command_handler(gpointer data)
{
  gchar *read_buf;
  guint32 read_size;
  IC_THREAD_STATE *thread_state= (IC_THREAD_STATE*)data;
  IC_CONNECTION *conn= (IC_CONNECTION*)
    thread_state->ts_ops.ic_thread_get_object(thread_state);
  int ret_code;

  thread_state->ts_ops.ic_thread_started(thread_state);
  thread_state= conn->conn_op.ic_get_param(conn);

  while (!(ret_code= ic_rec_with_cr(conn, &read_buf, &read_size)))
  {
    if (ic_check_buf(read_buf, read_size, start_str,
                     strlen(start_str)))
    {
      if ((ret_code= handle_start(conn)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, stop_str,
                          strlen(stop_str)))
    {
      if ((ret_code= handle_stop(conn, FALSE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, kill_str,
                          strlen(kill_str)))
    {
      if ((ret_code= handle_stop(conn, TRUE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, list_str,
                          strlen(list_str)))
    {
      if ((ret_code= handle_list(conn, FALSE)))
        break;
    }
    else if (ic_check_buf(read_buf, read_size, list_full_str,
                          strlen(list_full_str)))
    {
      if ((ret_code= handle_list(conn, TRUE)))
        break;
    }
    else if (read_size)
    {
      read_buf[read_size]= 0;
      printf("Received a message not expected: %s\n", read_buf);
      ret_code= IC_PROTOCOL_ERROR;
      /* Closing connection */
      break;
    }
  }
  if (ret_code)
    ic_print_error(ret_code);
  conn->conn_op.ic_free_connection(conn);
  thread_state->ts_ops.ic_thread_stops(thread_state);
  return NULL;
}

int start_connection_loop()
{
  int ret_code;
  guint32 thread_id;
  IC_CONNECTION *conn= NULL;
  IC_CONNECTION *fork_conn;

  conn->conn_op.ic_prepare_server_connection(conn,
                                             glob_ip,
                                             glob_port,
                                             NULL,
                                             NULL,
                                             0,
                                             TRUE);
  ret_code= conn->conn_op.ic_set_up_connection(conn, NULL, NULL);
  if (ret_code)
    return ret_code;
  do
  {
    do
    {
      glob_tp_state->tp_ops.ic_threadpool_check_threads(glob_tp_state);
      /* Wait for someone to connect to us. */
      if ((ret_code= conn->conn_op.ic_accept_connection(conn, NULL, NULL)))
      {
        printf("Error %d received in accept connection\n", ret_code);
        break;
      }
      if (!(fork_conn= 
            conn->conn_op.ic_fork_accept_connection(conn,
                                        FALSE)))  /* No mutex */
      {
        printf("Error %d received in fork accepted connection\n", ret_code);
        break;
      }
      /*
        We have an active connection, we'll handle the connection in a
        separate thread.
      */
      if (glob_tp_state->tp_ops.ic_threadpool_start_thread(
                 glob_tp_state,
                 &thread_id,
                 run_command_handler,
                 fork_conn,
                 IC_SMALL_STACK_SIZE,
                 FALSE))
      {
        printf("Failed to create thread after forking accept connection\n");
        fork_conn->conn_op.ic_free_connection(fork_conn);
        break;
      }
    } while (0);
  } while (!glob_stop_flag);
  return 0;
}

int main(int argc, char *argv[])
{
  int ret_code= 0;
  gchar tmp_buf[IC_MAX_FILE_NAME_SIZE];
  gchar iclaustron_buf[IC_MAX_FILE_NAME_SIZE];
  gchar mysql_buf[IC_MAX_FILE_NAME_SIZE];
 
  if ((ret_code= ic_start_program(argc, argv, entries, glob_process_name,
           "- iClaustron Control Server", TRUE)))
    return ret_code;
  if (!(glob_tp_state=
        ic_create_threadpool(IC_DEFAULT_MAX_THREADPOOL_SIZE)))
    return IC_ERROR_MEM_ALLOC;
  if (!(glob_pc_hash= create_pc_hash()))
    goto error;
  if (!(pc_hash_mutex= g_mutex_new()))
    goto error;
  if (!(glob_dyn_trans= ic_create_dynamic_translation()))
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
    glob_base_dir_string. We set up the default strings for the
    version we've compiled, the protocol to the process controller
    enables the Cluster Manager to specify which version to use.

    This program will have it's state in memory but will also checkpoint this
    state to a file. This is to ensure that we are able to connect to processes
    again even after we had to restart this program. This file is stored in
    the iclaustron_data placed beside the iclaustron_install directory. The file
    is called pcntrl_state.
  */
  if ((ret_code= ic_set_base_dir(&glob_base_dir_string, glob_base_dir)))
    goto error;
  ic_make_iclaustron_version_string(&glob_ic_base_dir_string, tmp_buf);
  ic_set_relative_dir(&glob_ic_base_dir_string, &glob_base_dir_string,
                      iclaustron_buf, glob_ic_base_dir_string.str);
  ic_make_mysql_version_string(&glob_mysql_base_dir_string, tmp_buf);
  ic_set_relative_dir(&glob_mysql_base_dir_string, &glob_base_dir_string,
                      mysql_buf, glob_mysql_base_dir_string.str);
  DEBUG_PRINT(PROGRAM_LEVEL, ("Base directory: %s",
                              glob_base_dir_string.str));
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
  ret_code= start_connection_loop();
error:
  glob_tp_state->tp_ops.ic_threadpool_stop(glob_tp_state);
  if (glob_pc_hash)
    ic_hashtable_destroy(glob_pc_hash);
  if (pc_hash_mutex)
    g_mutex_free(pc_hash_mutex);
  if (glob_base_dir_string.str)
    ic_free(glob_base_dir_string.str);
  if (glob_dyn_trans)
    glob_dyn_trans->dt_ops.ic_free_dynamic_translation(glob_dyn_trans);
  return ret_code;
}
