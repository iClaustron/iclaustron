project (iClaustron)
cmake_minimum_required (VERSION 2.6)
set (iClaustron_VERSION_MAJOR 0)
set (iClaustron_VERSION_MINOR 0)
set (iClaustron_VERSION_BUILD 1)

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

