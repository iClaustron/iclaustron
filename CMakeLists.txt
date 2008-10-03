project (iClaustron [C])
cmake_minimum_required (VERSION 2.4)
set (iClaustron_VERSION_MAJOR 0)
set (iClaustron_VERSION_MINOR 0)
set (iClaustron_VERSION_BUILD 1)

configure_file (
  "${PROJECT_SOURCE_DIR}/include/cmake_config.h.in"
  "${PROJECT_BINARY_DIR}/include/config.h"
  )
include_directories("${PROJECT_BINARY_DIR}")
add_subdirectory (port)
add_subdirectory (comm)
add_subdirectory (api_clusterserver)
add_subdirectory (api_dataserver)
add_subdirectory (cluster_client)
add_subdirectory (cluster_mgr)
add_subdirectory (cluster_server)
add_subdirectory (doc)
add_subdirectory (examples)
add_subdirectory (fileserver)
add_subdirectory (include)
add_subdirectory (pcntrl)
add_subdirectory (repserver)
add_subdirectory (scripts)
add_subdirectory (test)
add_subdirectory (util)

