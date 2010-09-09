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
#include <ic_mc.h>
#include <ic_string.h>
#include <ic_parse_connectstring.h>
#include <ic_threadpool.h>
#include <ic_connection.h>
#include <ic_apic.h>
#include <ic_apid.h>

static const gchar *glob_process_name= "test_api_basic";

static int
run_api_thread(IC_APID_CONNECTION *apid_conn,
               IC_THREAD_STATE *thread_state)
{
  IC_APID_GLOBAL *apid_global;
  DEBUG_ENTRY("run_api_thread");

  (void)thread_state;
  apid_global= apid_conn->apid_conn_ops->ic_get_apid_global(apid_conn);
  DEBUG_RETURN_INT(0);
}

int main(int argc, char *argv[])
{
  int ret_code;
  IC_API_CONFIG_SERVER *apic= NULL;
  IC_APID_GLOBAL *apid_global= NULL;
  gchar error_str[ERROR_MESSAGE_SIZE];
  gchar *err_str= NULL;
  IC_THREADPOOL_STATE *tp_state= NULL;

  if ((ret_code= ic_start_program(argc, argv, ic_apid_entries, NULL,
                                  glob_process_name,
            "- iClaustron API Basic Test", TRUE)))
    goto end;
  if ((ret_code= ic_start_apid_program(&tp_state,
                                       &err_str,
                                       error_str,
                                       &apid_global,
                                       &apic,
                                       FALSE)))
    goto end;
  /*
    ic_run_apid_program will start glob_num_threads and in each thread it will
    create an IC_APID_CONNECTION object and deliver it to the run_api_thread
    method. The method won't return until all threads have stopped. All the
    actions in the program is happening in the threads.
  */
  ret_code= ic_run_apid_program(apid_global,
                                tp_state,
                                run_api_thread,
                                &err_str);
end:
  ic_stop_apid_program(ret_code,
                       err_str,
                       apid_global,
                       apic,
                       tp_state);
  return ret_code;
}
