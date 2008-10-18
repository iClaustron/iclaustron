project (iClaustron)

cmake_minimum_required (VERSION 2.6)

set (iClaustron_VERSION_MAJOR 0)
set (iClaustron_VERSION_MINOR 0)
set (iClaustron_VERSION_PATCH 1)
set (ICLAUSTRON_PACKAGE_NAME "iclaustron")
set (ICLAUSTRON_VERSION "0.0.1")
set (MYSQL_VERSION "mysql-5.1.28-ndb-6.3.19")

SET (ICLAUSTRON_LIBS "")
SET (MANUAL_GLIB_LIB_DIR C:/glib/bin)
SET (MANUAL_GLIB_INCLUDE_DIR C:/glib/include)
SET (SYSTEM_BINARY_PATH
     /usr/local/bin /opt/local/bin /bin /sbin /usr/bin /usr/sbin
     /opt/bin)
SET (SYSTEM_LIB_PATH
     /usr/local/lib /opt/local/lib /usr/lib /usr/lib
     /opt/lib)

if (WIN32)
  FIND_PROGRAM(FLEX_EXECUTABLE
    NAMES flex
    PATHS ${SYSTEM_BINARY_PATH}
    DOC "flex executable")
  FIND_PROGRAM(BISON_EXECUTABLE
    NAMES bison byacc
    PATHS ${SYSTEM_BINARY_PATH}
    DOC "Bison executable")
else (WIN32)
  FIND_PROGRAM(FLEX_EXECUTABLE
    NAMES flex
    PATHS C:/Program Files/Flex
    DOC "flex executable")
  FIND_PROGRAM(BISON_EXECUTABLE
    NAMES bison byacc
    PATHS C:/Program Files/Bison C:/Program Files/Byacc
    DOC "Bison executable")
endif (WIN32)

configure_file (
  "${PROJECT_SOURCE_DIR}/include/cmake_config.h.in"
  "${PROJECT_BINARY_DIR}/include/config.h"
  )

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
configure_file("${PROJECT_SOURCE_DIR}/scripts/linux_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/linux_check_process.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/scripts/macosx_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/macosx_check_process.sh"
               @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/scripts/solaris_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/solaris_check_process.sh"
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
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(netinet/in.h HAVE_NETINET_IN_H)
check_include_files(poll.h HAVE_POLL_H)
check_include_files(sys/poll.h HAVE_SYS_POLL_H)
check_include_files(sys/types.h HAVE_SYS_TYPES_H)
check_include_files(sys/socket.h HAVE_SYS_SOCKET_H)
check_include_files(netdb.h HAVE_NETDB_H)
check_include_files(arpa/inet.h HAVE_ARPA_INET_H)

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

if (WIN32)
  find_library(MATH_LIB
               NAMES m
               PATHS C:/Program Files/Math)
else (WIN32)
  find_library(MATH_LIB
               NAMES m
               PATHS /usr/local/lib /usr/lib)
endif (WIN32)
set (CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${MATH_LIB})

message("Checking for MSG_NOSIGNAL in this platform")
try_compile(USE_MSG_NO_SIGNAL
            ${PROJECT_BINARY_DIR}/tmp
            ${PROJECT_SOURCE_DIR}/build_test/test_msg_no_signal.c)
if (USE_MSG_NO_SIGNAL)
  message("Found MSG_NO_SIGNAL support")
  add_definitions(-DUSE_MSG_NOSIGNAL)
else (USE_MSG_NO_SIGNAL)
  message("No MSG_NO_SIGNAL support")
endif (USE_MSG_NO_SIGNAL)

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
    endif (NOT_INCLUDE_READLINE_DIR)
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
  add_definitions(-g)
endif (WITH_DEBUG)

if (WITH_PRODUCTION)
  add_definitions(-O2)
  add_definitions(-W)
else (WITH_PRODUCTION)
  add_definitions(-W -Wall)
endif (WITH_PRODUCTION)

set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${CMAKE_REQUIRED_LIBRARIES})
#Build library for iClaustron portability layer
add_library(ic_port STATIC port/ic_port.c)

#Build library for iClaustron utility functions
add_library(ic_util STATIC util/hashtable.c util/hashtable_itr.c util/ic_util.c)
target_link_libraries(ic_util ic_port)

#Build library for communication utilities
add_library(comm STATIC comm/socket_conn.c comm/socket_membuf.c comm/comm_supp.c)
target_link_libraries(comm ic_port ic_util)

#Build library to Cluster Server API
add_library(ic_apic STATIC api_clusterserver/ic_apic.c)
target_link_libraries(ic_apic ic_port ic_util comm)

#Build library for Data Server API
add_library(ic_apid STATIC api_dataserver/ic_apid.c)
target_link_libraries(ic_apid ic_port ic_util comm ic_apic)
#install(TARGETS ic_apid
#        RUNTIME DESTINATION bin
#        LIBRARY DESTINATION lib
#        ARCHIVE DESTINATION lib/iClaustron)

#Build test binary
add_executable(test_comm test/test_comm.c)
target_link_libraries(test_comm ic_port ic_util ic_apic ic_apid
                      ${ICLAUSTRON_LIBS})
install(TARGETS test_comm DESTINATION bin)

#Build Process Control Binary
add_executable(ic_pcntrld pcntrl/ic_pcntrl.c)
target_link_libraries(ic_pcntrld ic_port ic_util ic_apic ic_apid
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_pcntrld DESTINATION bin)

#Build Cluster client binary
add_executable(ic_cclient cluster_client/ic_cclient.c)
target_link_libraries(ic_cclient comm ic_util ic_port ic_apic
                      ${READLINE_LIBS} ${ICLAUSTRON_LIBS})
install (TARGETS ic_cclient DESTINATION bin)

#Build Cluster Server binary
add_executable(ic_csd cluster_server/ic_cs.c)
target_link_libraries(ic_csd ic_port ic_util ic_apic ic_apid
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_csd DESTINATION bin)

#Build File Server binary
add_executable(ic_fsd fileserver/ic_fileserv.c)
target_link_libraries(ic_fsd ic_port ic_util ic_apic ic_apid
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_fsd DESTINATION bin)

#Build Replication Server binary
add_executable(ic_repd repserver/ic_repserv.c)
target_link_libraries(ic_repd ic_port ic_util ic_apic ic_apid
                      ${ICLAUSTRON_LIBS})
install(TARGETS ic_repd DESTINATION bin)

#Create target for the building of the parser of the Cluster Manager
SET(ClusterManagerSource cluster_mgr/ic_clmgr.c)
ADD_CUSTOM_TARGET(ClusterManagerParser echo "Creating ic_parser.c")

ADD_CUSTOM_COMMAND(
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_lex.l
  COMMAND ${FLEX_EXECUTABLE}
  ARGS -o${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c
        ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_lex.l
  TARGET ClusterManagerParser
  OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c)

ADD_CUSTOM_COMMAND(
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_parser.y
  COMMAND ${BISON_EXECUTABLE}
  ARGS -d
       -y ${CMAKE_CURRENT_SOURCE_DIR}/cluster_mgr/ic_parser.y
       -o ${CMAKE_CURRENT_BINARY_DIR}/ic_parser.c
  TARGET ClusterManagerParser
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c
  OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/ic_parser.c
          ${CMAKE_CURRENT_BINARY_DIR}/ic_parser.h)

#Add ic_parser.c to source files
SET(ClusterManagerSource ${ClusterManagerSource}
    ${CMAKE_CURRENT_BINARY_DIR}/ic_parser.c
    ${CMAKE_CURRENT_BINARY_DIR}/ic_parser.h
    ${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c)
#Mark ic_parser.c and ic_lex.c as a generated file
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_parser.c
                            GENERATED)
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c
                            GENERATED)
SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_parser.h
                            GENERATED)
#Include binary directory to ensure ic_lex.c is included as well
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

add_executable(ic_clmgrd ${ClusterManagerSource})
target_link_libraries(ic_clmgrd ic_port ic_util comm ic_apic ic_apid
                      ${ICLAUSTRON_LIBS})
install (TARGETS ic_clmgrd DESTINATION bin)

#Install header files
install (FILES
         include/hashtable.h
         include/hashtable_itr.h
         include/hashtable_private.h
         include/ic_apic.h
         include/ic_apid.h
         include/ic_clmgr.h
         include/ic_comm.h
         include/ic_common.h
         include/ic_common_header.h
         include/ic_err.h
         include/ic_port.h
         include/ic_ssl.h
         include/ic_config.h
         DESTINATION include)

#Install scripts into bin directory
install (PROGRAMS scripts/linux_check_process.sh.in DESTINATION bin
         RENAME linux_check_process.sh)
install (PROGRAMS scripts/macosx_check_process.sh.in DESTINATION bin
         RENAME macosx_check_process.sh)
install (PROGRAMS scripts/solaris_check_process.sh.in DESTINATION bin
         RENAME solaris_check_process.sh)
#Install examples directory into examples directory
install (DIRECTORY examples DESTINATION ${CMAKE_INSTALL_PREFIX})
#Install doc directory into doc directory
install (DIRECTORY doc DESTINATION ${CMAKE_INSTALL_PREFIX})
install (FILES README README-CONFIG AUTHORS NEWS INSTALL COPYING AUTHORS
         DESTINATION doc)