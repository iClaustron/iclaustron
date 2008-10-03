  SET(ClusterManagerSource ic_clmgr.c)
#Create target for the building of the parser of the Cluster Manager
ADD_CUSTOM_TARGET(ClusterManagerParser echo "Creating ic_parser.c")

ADD_CUSTOM_COMMAND(
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ic_lex.l
  COMMAND ${FLEX_EXECUTABLE}
  ARGS -o${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ic_lex.l
  TARGET ClusterManagerParser
  OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c)

ADD_CUSTOM_COMMAND(
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ic_parser.y
  COMMAND ${BISON_EXECUTABLE}
  ARGS -d
       -y ${CMAKE_CURRENT_SOURCE_DIR}/ic_parser.y
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
  SET_SOURCE_FILE_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_parser.c
                              GENERATED)
  SET_SOURCE_FILE_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_lex.c
                              GENERATED)
  SET_SOURCE_FILE_PROPERTIES(${CMAKE_CURRENT_BINARY_DIR}/ic_parser.h
                              GENERATED)
#Include binary directory to ensure ic_lex.c is included as well
  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

  add_executable(ic_clmgrd ${ClusterManagerSource})
  target_link_libraries(ic_clmgrd comm ic_util ic_port ic_apic ic_apid)
