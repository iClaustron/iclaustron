# Copyright (C) 2008-2010 iClaustron AB
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

project (iClaustron)

cmake_minimum_required (VERSION 2.6)

if (WIN32)
  set(WINDOWS 1)
else (WIN32)
  execute_process(COMMAND uname -s
                  OUTPUT_VARIABLE OS_NAME)
  string (REGEX MATCH "[L][i][n][u][x]"
          LINUX_NAME 
          "${OS_NAME}")
  if (LINUX_NAME STREQUAL Linux)
    message("Linux OS found")
    set(LINUX 1)
  endif (LINUX_NAME STREQUAL Linux)
  string (REGEX MATCH "[D][a][r][w][i][n]"
          MACOSX_NAME 
          "${OS_NAME}")
  if (MACOSX_NAME STREQUAL Darwin)
    message("MacOSX found")
    set(MACOSX 1)
  endif (MACOSX_NAME STREQUAL Darwin)
  string (REGEX MATCH "[S][o][l][a][r][i][s]"
          SOLARIS_NAME 
          "${OS_NAME}")
  if (SOLARIS_NAME STREQUAL Solaris)
    set(SOLARIS 1)
  endif (SOLARIS_NAME STREQUAL Solaris)
  string (REGEX MATCH "[B][S][D]"
          BSD_NAME 
          "${OS_NAME}")
  if (BSD_NAME STREQUAL BSD)
    set(FREEBSD 1)
  endif (BSD_NAME STREQUAL BSD)
  if (NOT LINUX AND NOT MACOSX AND NOT SOLARIS AND NOT FREEBSD)
    message(FATAL_ERROR OS isn't supported)
  endif (NOT LINUX AND NOT MACOSX AND NOT SOLARIS AND NOT FREEBSD)
endif (WIN32)
macro (find_bison)
  FIND_PROGRAM(BISON_EXECUTABLE
    NAMES bison
    PATHS ${system_path}
    DOC "Bison executable"
    NO_DEFAULT_PATH)
 if (BISON_EXECUTABLE)
   execute_process(COMMAND ${BISON_EXECUTABLE} --version
                   OUTPUT_VARIABLE _BISON_VERSION)
   string (REGEX MATCH "[0-9]+\\.[0-9]+"
           BISON_VERSION
           "${_BISON_VERSION}")
   string (REGEX MATCH "[0-9]+"
           BISON_MAJOR_VERSION
           "${_BISON_VERSION}")
   string (REGEX MATCH "\\.[0-9]+"
           _BISON_MINOR_VERSION
           "${_BISON_VERSION}")
   string (REGEX MATCH "[0-9]+"
           BISON_MINOR_VERSION
           "${_BISON_MINOR_VERSION}")
   if (BISON_MAJOR_VERSION GREATER 1 AND BISON_MINOR_VERSION GREATER 0)
     message("Found working Bison executable at " ${BISON_EXECUTABLE})
     message("Bison version is " ${BISON_VERSION})
   else (BISON_MAJOR_VERSION GREATER 1 AND BISON_MINOR_VERSION GREATER 0)
     set(BISON_EXECUTABLE NOTFOUND)
   endif (BISON_MAJOR_VERSION GREATER 1 AND BISON_MINOR_VERSION GREATER 0)
 endif (BISON_EXECUTABLE)
endmacro (find_bison)

set (iClaustron_VERSION_MAJOR 0)
set (iClaustron_VERSION_MINOR 0)
set (iClaustron_VERSION_PATCH 1)
set (ICLAUSTRON_PACKAGE_NAME "iclaustron")
set (ICLAUSTRON_VERSION "0.0.1")
set (MYSQL_VERSION "mysql-5.1.35-ndb-7.1.0")

SET (ICLAUSTRON_LIBS "")
SET (SYSTEM_BINARY_PATH
     /usr/local/bin /opt/local/bin /bin /sbin /usr/bin /usr/sbin
     /opt/bin)
SET (SYSTEM_LIB_PATH
     /usr/local/lib /opt/local/lib /usr/lib /usr/lib
     /opt/lib)

set(BISON_EXECUTABLE NOTFOUND)
if (NOT WIN32)
  foreach (system_path
           /bin
           /sbin
           /usr/bin
           /usr/sbin
           /opt/bin
           /usr/local/bin
           /opt/local/bin
           )
    if (NOT BISON_EXECUTABLE)
      find_bison()
    endif (NOT BISON_EXECUTABLE)
  endforeach (system_path)
else (NOT WIN32)
  foreach (system_path
           "C:\GnuWin32\bin"
           )
    if (NOT BISON_EXECUTABLE)
      find_bison()
      set (BISON_PATH system_path)
    endif (NOT BISON_EXECUTABLE)
  endforeach (system_path)
endif (NOT WIN32)
if (NOT BISON_EXECUTABLE)
  message(FATAL_ERROR "Found no working Bison executable")
endif (NOT BISON_EXECUTABLE)

include_directories("${PROJECT_BINARY_DIR}/include")
include_directories("${PROJECT_SOURCE_DIR}/include")

configure_file("${PROJECT_SOURCE_DIR}/bootstrap.sh.in"
               "${PROJECT_BINARY_DIR}/bootstrap.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/setup.sh.in"
               "${PROJECT_BINARY_DIR}/setup.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/count_line.sh.in"
               "${PROJECT_BINARY_DIR}/count_line.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/scripts/check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/check_process.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/scripts/linux_get_hw_info.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/linux_get_hw_info.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/scripts/windows_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/windows_check_process.sh"
               @ONLY)

option(WITH_UNIT_TEST "Build unit test programs" OFF)
option(WITH_PRODUCTION "Build a production version" OFF)
option(WITH_DEBUG "Build a debug version" OFF)
option(WITH_READLINE "Use readline package" ON)
option(WITH_DTRACE "Use DTrace probes" OFF)
option(WITH_OPENSSL "Use OpenSSL package" OFF)
option(WITH_CYASSL "Use CYassl package" OFF)

#We need PkgConfig to discover glib and other packages we need
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR})
find_package(GLIB2 REQUIRED)
include_directories(${GLIB2_INCLUDE_DIRS})
add_definitions(${GLIB2_DEFINITIONS})
#Not necessary to do link_directories since libraries are full path
set (CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${GLIB2_LIBRARIES})

include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckSymbolExists)
include(CheckLibraryExists)
include(CheckTypeSize)
check_function_exists(bzero HAVE_BZERO)
check_function_exists(memset HAVE_MEMSET)
check_function_exists(break HAVE_BREAK)
check_function_exists(gethrtime HAVE_GETHRTIME)
check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
check_function_exists(socket HAVE_SOCKET)
check_function_exists(bind HAVE_BIND)
check_function_exists(connect HAVE_CONNECT)
check_function_exists(listen HAVE_LISTEN)
check_function_exists(accept HAVE_ACCEPT)
check_function_exists(getaddrinfo HAVE_GETADDRINFO)
check_function_exists(freeaddrinfo HAVE_FREEADDRINFO)
check_function_exists(inet_ntop HAVE_INET_NTOP)
check_function_exists(sendmsg HAVE_SENDMSG)
check_function_exists(poll HAVE_POLL)
check_function_exists(epoll_create HAVE_EPOLL_CREATE)
check_function_exists(port_create HAVE_PORT_CREATE)
check_function_exists(kqueue HAVE_KQUEUE)
check_include_files(arpa/inet.h HAVE_ARPA_INET_H)
check_include_files(netinet/in.h HAVE_NETINET_IN_H)
check_include_files(netinet/tcp.h HAVE_NETINET_TCP_H)
check_include_files(sys/poll.h HAVE_SYS_POLL_H)
check_include_files(sys/select.h HAVE_SYS_SELECT_H)
check_include_files(sys/socket.h HAVE_SYS_SOCKET_H)
check_include_files(sys/times.h HAVE_SYS_TIMES_H)
check_include_files(sys/types.h HAVE_SYS_TYPES_H)
check_include_files(sys/wait.h HAVE_SYS_WAIT_H)
check_include_files(fcntl.h HAVE_FCNTL_H)
check_include_files(netdb.h HAVE_NETDB_H)
check_include_files(poll.h HAVE_POLL_H)
check_include_files(signal.h HAVE_SIGNAL_H)
check_include_files(time.h HAVE_TIME_H)
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(sys/uio.h HAVE_SYS_UIO_H)

message("Check for clock_gettime function")
find_library(RT_LIB
             NAMES rt
             PATHS ${SYSTEM_LIB_PATH})
if (RT_LIB)
  set (CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${RT_LIB})
  check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)
endif (RT_LIB)
if (HAVE_CLOCK_GETTIME)
  message("Found clock_gettime function")
else (HAVE_CLOCK_GETTIME)
  message("No clock_gettime function")
endif (HAVE_CLOCK_GETTIME)

if (NOT WIN32)
  find_library(MATH_LIB
               NAMES m
               PATHS /usr/local/lib /usr/lib)
  set (CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${MATH_LIB})
endif (NOT WIN32)


message("Checking for MSG_NOSIGNAL in this platform")
try_compile(USE_MSG_NOSIGNAL
            ${PROJECT_BINARY_DIR}/tmp
            ${PROJECT_SOURCE_DIR}/build_test/test_msg_no_signal.c)
if (USE_MSG_NOSIGNAL)
  message("Found MSG_NOSIGNAL support")
  add_definitions(-DUSE_MSG_NOSIGNAL)
else (USE_MSG_NOSIGNAL)
  message("No MSG_NOSIGNAL support")
endif (USE_MSG_NOSIGNAL)

if (NOT WIN32 AND WITH_DTRACE)
  message("Checking for DTrace")
  FIND_PROGRAM(DTRACE_EXECUTABLE
    NAMES dtrace 
    PATHS ${SYSTEM_BINARY_PATH}
    DOC "DTrace executable")
  if (DTRACE_EXECUTABLE)
    message("Found DTrace")
    set (HAVE_DTRACE TRUE)
  else (DTRACE_EXECUTABLE)
    message("Couldn't find DTrace")
  endif (DTRACE_EXECUTABLE)
endif (NOT WIN32 AND WITH_DTRACE)

if (WITH_READLINE)
  message("Checking for readline")
  find_path(INCLUDE_READLINE_DIR readline/readline.h)
  find_library(READLINE_LIB
               NAMES readline
               PATHS /usr/local/lib /usr/lib)
  find_library(NCURSES_LIB
               NAMES ncurses
               PATHS /usr/local/lib /usr/lib)
  if (INCLUDE_READLINE_DIR AND READLINE_LIB AND NCURSES_LIB)
    message("Found readline")
    set (HAVE_READLINE TRUE)
    set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${READLINE_LIB} ${NCURSES_LIB})
    include_directories(${INCLUDE_READLINE_DIR})
  else (INCLUDE_READLINE_DIR AND READLINE_LIB AND NCURSES_LIB)
    if (NOT INCLUDE_READLINE_DIR)
      message ("Could not find readline/readline.h")
    endif (NOT INCLUDE_READLINE_DIR)
    message ("Unsucessful in including readline")
  endif (INCLUDE_READLINE_DIR AND READLINE_LIB AND NCURSES_LIB)
endif(WITH_READLINE)

if (WITH_OPENSSL)
  message("Checking for OpenSSL")
  include(FindOpenSSL)
  if (OPENSSL_INCLUDE_DIR)
    message("Found OpenSSL")
    set (HAVE_OPENSSL TRUE)
    include_directories(${OPENSSL_INCLUDE_DIR})
    set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${OPENSSL_LIBRARIES})
  else (OPENSSL_INCLUDE_DIR)
    message ("Unsucessful in including openssl")
  endif(OPENSSL_INCLUDE_DIR)
endif(WITH_OPENSSL)

if (NOT HAVE_OPENSSL AND WITH_CYASSL)
  message("Checking for cyassl")
  find_path(INCLUDE_OPENSSL_DIR openssl/ssl.h)
  find_library(CYASSL_LIB
               NAMES cyassl
               PATHS /usr/local/lib /usr/lib)
  if (INCLUDE_OPENSSL_DIR AND CYASSL_LIB)
    message("Found cyassl")
    set (HAVE_CYASSL TRUE)
    set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${CYASSL_LIB})
    include_directories(${INCLUDE_SSL_DIR})
  else (INCLUDE_OPENSSL_DIR AND CYASSL_LIB)
    if (NOT INCLUDE_OPENSSL_DIR)
      message ("Could not find openssl/ssl.h")
    endif (NOT INCLUDE_OPENSSL_DIR)
    message ("Unsucessful in including cyassl")
  endif (INCLUDE_OPENSSL_DIR AND CYASSL_LIB)
endif(NOT HAVE_OPENSSL AND WITH_CYASSL)
if (HAVE_OPENSSL OR HAVE_CYASSL)
  set (HAVE_SSL TRUE)
endif (HAVE_OPENSSL OR HAVE_CYASSL)

if (WITH_UNIT_TEST)
  add_definitions(-DWITH_UNIT_TEST)
endif (WITH_UNIT_TEST)

if (WITH_DEBUG)
  add_definitions(-DDEBUG_BUILD)
  if (NOT WIN32)
    add_definitions(-g)
  endif (NOT WIN32)
endif (WITH_DEBUG)

if (NOT WIN32)
  if (WITH_PRODUCTION)
    add_definitions(-O2)
    add_definitions(-W)
  else (WITH_PRODUCTION)
    add_definitions(-W -Wall)
  endif (WITH_PRODUCTION)
endif (NOT WIN32)

set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${CMAKE_REQUIRED_LIBRARIES})

configure_file (
  "${PROJECT_SOURCE_DIR}/include/cmake_config.h.in"
  "${PROJECT_BINARY_DIR}/include/config.h"
  )

#Build library for iClaustron portability layer
add_library(ic_port STATIC
            port/ic_port.c)

#Build library for iClaustron utility functions
add_library(ic_util STATIC
            util/ic_hashtable.c
            util/ic_hashtable_itr.c
            util/ic_util.c
            util/ic_mc.c
            util/ic_dyn_array.c
            util/ic_mc_int.h
            util/ic_dyn_array_int.h
            util/ic_threadpool_int.h
            util/ic_threadpool.c
            util/ic_debug.c
            util/ic_err.c
            util/ic_hw_info.c
            util/ic_string.c
            util/ic_bitmap.c
            util/ic_config_reader.c
            util/ic_parse_connectstring.c
            util/ic_lex_support.c
            util/ic_readline.c
            include/ic_hashtable_itr.h
            include/ic_hashtable_private.h
            include/ic_base64.h
            include/ic_config_reader.h
            include/ic_sock_buf.h
            include/ic_lex_support.h
            include/ic_readline.h)
target_link_libraries(ic_util ic_port)

#Build library for communication utilities
add_library(ic_comm STATIC
            comm/ic_connection.c
            comm/ic_sock_buf.c
            comm/ic_poll_set.c
            comm/ic_poll_set_int.h)
target_link_libraries(ic_comm ic_port ic_util)

#Build library for protocol support utilities
add_library(ic_proto STATIC
            protocol/ic_protocol_support.c
            protocol/ic_base64.c
            protocol/ic_pcntrl_proto.c
            protocol/ic_proto_str.c
            include/ic_proto_str.h
            include/ic_pcntrl_proto.h
            include/ic_protocol_support.h
            include/ic_base64.h)
target_link_libraries(ic_proto ic_port ic_util ic_comm)

#Build library to Cluster Server API and Data Server API
add_library(ic_api STATIC
            api/ic_apic_impl.c
            api/ic_apid_impl.c
            api/ic_apid_global.ic
            api/ic_apid_common.ic
            api/ic_apid_heartbeat.ic
            api/ic_apid_adaptive_send.ic
            api/ic_apid_send_message.ic
            api/ic_apid_send_thread.ic
            api/ic_apid_handle_messages.ic
            api/ic_apid_exec_message.ic
            api/ic_apid_rec_thread.ic
            api/ic_apid_start.ic
            api/ic_apid_range.ic
            api/ic_apid_where.ic
            api/ic_apid_cond_assign.ic
            api/ic_apid_op.ic
            api/ic_apid_error.ic
            api/ic_apid_conn.ic
            api/ic_apid_table.ic
            api/ic_apid_tablespace.ic
            api/ic_apic.ic
            api/ic_apic_conf_param.ic
            api/ic_apic_conf_read_transl.ic
            api/ic_apic_proto_supp.ic
            api/ic_apic_conf_read_proto.ic
            api/ic_apic_conf_reader.ic
            api/ic_apic_conf_writer.ic
            api/ic_apic_grid_conf_reader.ic
            api/ic_apic_if.ic
            api/ic_apic_run_cs.ic
            api/ic_apic_conf_net_read.ic
            api/ic_apic_int.h
            api/ic_apid_int.h
            api/ic_apid_impl.h
            api/ic_apid_static.h)
target_link_libraries(ic_api ic_port ic_util ic_comm ic_proto)

#install(TARGETS ic_api
#        RUNTIME DESTINATION bin
#        LIBRARY DESTINATION lib
#        ARCHIVE DESTINATION lib/iClaustron)

#Build test binaries
add_executable(test_comm test/test_comm.c)
target_link_libraries(test_comm ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS test_comm DESTINATION bin)

add_executable(test_api_basic test/test_api_basic.c)
target_link_libraries(test_api_basic ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS test_api_basic DESTINATION bin)

#Build Process Control Binary
add_executable(ic_pcntrld pcntrl/ic_pcntrl.c)
target_link_libraries(ic_pcntrld ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_pcntrld DESTINATION bin)

#Build Cluster client binary
add_executable(ic_cclient cluster_client/ic_cclient.c)
target_link_libraries(ic_cclient ic_port ic_util ic_comm ic_proto ic_api
                      ${READLINE_LIBS} ${ICLAUSTRON_LIBS})
install (TARGETS ic_cclient DESTINATION bin)

#Build Cluster Server binary
add_executable(ic_csd cluster_server/ic_cs.c)
target_link_libraries(ic_csd ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_csd DESTINATION bin)

#Build Bootstrap binary
#Create target for the building of the parser of the bootstrapper
SET(BootstrapSource bootstrap/ic_bootstrap.c bootstrap/ic_boot_lex.c)

if (WIN32)
  ADD_CUSTOM_COMMAND(
    COMMAND copy ${BISON_PATH}/m4.exe ${CMAKE_CURRENT_BINARY_DIR}
    OUTPUT ${BISON_PATH}/m4.exe)
endif (WIN32)

ADD_CUSTOM_COMMAND(
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/ic_boot_parser.y
  COMMAND ${BISON_EXECUTABLE}
  ARGS -d
       -y ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/ic_boot_parser.y
       -o ${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.c
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.c
          ${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.h)

#Add ic_boot_parser.c to source files
SET(BootstrapSource ${BootstrapSource}
    ${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.c
    ${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/ic_boot_int.h)
#Mark ic_boot_parser.c as a generated file
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.c
                            GENERATED)
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_boot_parser.h
                            GENERATED)
#Include binary directory to ensure ic_boot_parser.c is included as well
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

add_executable(ic_bootstrap ${BootstrapSource})
target_link_libraries(ic_bootstrap ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install (TARGETS ic_bootstrap DESTINATION bin)

#Build Configurator binary
add_executable(ic_configurator cfg/ic_configurator.c)
target_link_libraries(ic_configurator ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_configurator DESTINATION bin)

#Build File Server binary
add_executable(ic_fsd fileserver/ic_fileserv.c)
target_link_libraries(ic_fsd ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_fsd DESTINATION bin)

#Build Replication Server binary
add_executable(ic_repd repserver/ic_repserv.c)
target_link_libraries(ic_repd ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_repd DESTINATION bin)

#Create target for the building of the parser of the Cluster Manager
SET(ClusterManagerSource cluster_mgr/ic_clmgr.c cluster_mgr/ic_mgr_lex.c)

ADD_CUSTOM_COMMAND(
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_mgr_parser.y
  COMMAND ${BISON_EXECUTABLE}
  ARGS -d
       -y ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_mgr_parser.y
       -o ${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.c
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.c
          ${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.h)

#Add ic_mgr_parser.c to source files
SET(ClusterManagerSource ${ClusterManagerSource}
    ${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.c
    ${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_clmgr_int.h)
#Mark ic_mgr_parser.c as a generated file
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.c
                            GENERATED)
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_mgr_parser.h
                            GENERATED)
#Include binary directory to ensure ic_mgr_parser.c is included as well
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

add_executable(ic_clmgrd ${ClusterManagerSource})
target_link_libraries(ic_clmgrd ic_port ic_util ic_comm ic_proto ic_api
                      ${ICLAUSTRON_LIBS})
install (TARGETS ic_clmgrd DESTINATION bin)

#Install header files
install (FILES
         include/ic_apic.h
         include/ic_apic_data.h
         include/ic_apic_load_cfg.h
         include/ic_apic_acs.h
         include/ic_apic_rcs.h
         include/ic_apid.h
         include/ic_apid_datatypes.h
         include/ic_apid_hidden.h
         include/ic_apid_inline.h
         include/ic_base_header.h
         include/ic_connection.h
         include/ic_common.h
         include/ic_common_header.h
         include/ic_debug.h
         include/ic_dyn_array.h
         include/ic_err.h
         include/ic_hashtable.h
         include/ic_hw_info.h
         include/ic_mc.h
         include/ic_parse_connectstring.h
         include/ic_poll_set.h
         include/ic_bitmap.h
         include/ic_port.h
         include/ic_ssl.h
         include/ic_string.h
         include/ic_threadpool.h
         DESTINATION include)

#Install scripts into bin directory
install (PROGRAMS scripts/check_process.sh.in DESTINATION bin
         RENAME check_process.sh)
install (PROGRAMS scripts/linux_get_hw_info.sh.in DESTINATION bin
         RENAME linux_get_hw_info.sh)
install (PROGRAMS scripts/windows_check_process.sh.in DESTINATION bin
         RENAME windows_check_process.sh)

#Install examples directory into examples directory
install (DIRECTORY examples DESTINATION ${CMAKE_INSTALL_PREFIX})
install (FILES
         examples/config.ini
         examples/grid_common.ini
         examples/jocke.ini
         examples/kalle.ini
         DESTINATION examples)


#Install doc directory into doc directory
install (DIRECTORY doc DESTINATION ${CMAKE_INSTALL_PREFIX})
install (FILES
         COPYING
         INSTALL
         ChangeLog
         README
         README-CONFIG
         AUTHORS
         NEWS
         DESTINATION doc)
