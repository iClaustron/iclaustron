project (iClaustron)

cmake_minimum_required (VERSION 2.6)

set (iClaustron_VERSION_MAJOR 0)
set (iClaustron_VERSION_MINOR 0)
set (iClaustron_VERSION_PATCH 1)

set (ICLAUSTRON_LIBS "")
set (READLINE_LIBS "")
SET (SYSTEM_BINARY_PATH
     /usr/bin /bin /sbin /usr/sbin /usr/local/bin /usr/sbin
     /opt/bin /opt/local/bin)
FIND_PROGRAM(FLEX_EXECUTABLE
  NAMES flex
  PATHS ${SYSTEM_BINARY_PATH}
  DOC "flex executable")
FIND_PROGRAM(BISON_EXECUTABLE
  NAMES bison byacc
  PATHS ${SYSTEM_BINARY_PATH}
  DOC "Bison executable")

configure_file (
  "${PROJECT_SOURCE_DIR}/include/cmake_config.h.in"
  "${PROJECT_BINARY_DIR}/include/config.h"
  )

include_directories("${PROJECT_BINARY_DIR}")
include_directories("${PROJECT_SOURCE_DIR}/include")

configure_file("${PROJECT_SOURCE_DIR}/bootstrap.sh.in"
               "${PROJECT_BINARY_DIR}/bootstrap.sh")
configure_file("${PROJECT_SOURCE_DIR}/setup.sh.in"
               "${PROJECT_BINARY_DIR}/setup.sh")
configure_file("${PROJECT_SOURCE_DIR}/count_line.sh.in"
               "${PROJECT_BINARY_DIR}/count_line.sh")
configure_file("${PROJECT_SOURCE_DIR}/scripts/linux_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/linux_check_process.sh")
configure_file("${PROJECT_SOURCE_DIR}/scripts/macosx_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/macosx_check_process.sh")
configure_file("${PROJECT_SOURCE_DIR}/scripts/solaris_check_process.sh.in"
               "${PROJECT_BINARY_DIR}/scripts/solaris_check_process.sh")

option(WITH_PRODUCTION "Build a production version" OFF)
option(WITH_DEBUG "Build a debug version" OFF)
option(WITH_READLINE "Use readline package" OFF)
option(WITH_DTRACE "Use DTrace probes" OFF)
option(WITH_OPENSSL "Use OpenSSL package" OFF)
option(WITH_CYASSL "Use CYassl package" OFF)

#We need PkgConfig to discover glib and other packages we need
find_package(PkgConfig REQUIRED)
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR})
find_package(GLIB2 REQUIRED)
include_directories(${GLIB2_INCLUDE_DIRS})
link_directories(${GLIB2_LIBRARIES})
add_definitions(${GLIB2_DEFINITIONS})
set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${GLIB2_LIBRARIES})
message ("iClaustron libraries are ${ICLAUSTRON_LIBS}")
message ("GLIB2 headers are ${GLIB2_INCLUDE_DIRS}")

if (WITH_OPENSSL)
  include(FindOpenSSL)
  if (OPENSSL_INCLUDE_DIR)
    include_directories(${OPENSSL_INCLUDE_DIR})
    set (ICLAUSTRON_LIBS ${ICLAUSTRON_LIBS} ${OPENSSL_LIBRARIES})
  endif(OPENSSL_INCLUDE_DIR)
endif(WITH_OPENSSL)
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
#check_library_exists(rt HAVE_RT)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(netinet/in.h HAVE_NETINET_IN_H)
check_include_file(poll.h HAVE_POLL_H)
check_include_file(sys/poll.h HAVE_SYS_POLL_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(sys/socket.h HAVE_SYS_SOCKET_H)
check_include_file(netdb.h HAVE_NETDB_H)
check_include_file(arpa/inet.h HAVE_ARPA_INET_H)
check_include_file(glib.h HAVE_GLIB_H)

if (WITH_READLINE)
  find_library(READLINE_LIB
               NAMES readline
               PATHS /usr/local/lib /usr/lib)
  find_library(NCURSES_LIB
               NAMES ncurses
               PATHS /usr/local/lib /usr/lib)
  check_include_file(readline/readline.h HAVE_READLINE_H)
  set (READLINE_LIBS ${READLINE_LIB} ${NCURSES_LIB})
endif(WITH_READLINE)
if (WITH_OPENSSL)
endif(WITH_OPENSSL)
if (WITH_CYASSL)
endif(WITH_CYASSL)

add_subdirectory (port)
add_subdirectory (comm)
add_subdirectory (util)
add_subdirectory (api_clusterserver)
add_subdirectory (api_dataserver)
add_subdirectory (cluster_client)
add_subdirectory (cluster_mgr)
add_subdirectory (cluster_server)
#add_subdirectory (doc) Nothing to build here yet
#add_subdirectory (examples) Nothing to build here yet
add_subdirectory (fileserver)
add_subdirectory (include)
add_subdirectory (pcntrl)
add_subdirectory (repserver)
#add_subdirectory (scripts)
add_subdirectory (test)

#Install scripts into bin directory
install (PROGRAMS scripts/linux_check_process.sh.in DESTINATION bin
         RENAME scripts/linux_check_process.sh)
install (PROGRAMS scripts/macosx_check_process.sh.in DESTINATION bin
         RENAME scripts/macosx_check_process.sh)
install (PROGRAMS scripts/solaris_check_process.sh.in DESTINATION bin
         RENAME scripts/solaris_check_process.sh)
#Install examples directory into examples directory
install (DIRECTORY examples DESTINATION examples)
#Install doc directory into doc directory
install (DIRECTORY doc DESTINATION doc)
install (FILES README README-CONFIG AUTHORS NEWS INSTALL COPYING AUTHORS
         DESTINATION doc)
