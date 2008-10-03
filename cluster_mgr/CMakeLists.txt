  SET(ClusterManagerSource ic_clmgr.c)
#Create target for the building of the parser of the Cluster Manager
ADD_CUSTOM_TARGET(ClusterManagerParser echo "Creating ic_parser.c")

ADD_CUSTOM_COMMAND(
  SOURCE ic_lex.l
  COMMAND $(FLEX_EXECUTABLE)
  ARGS -o${iClaustron_BINARY_DIR}/src/ic_lex.c
        ${iClaustron_SOURCE_DIR}/cluster_mgr/ic_lex.l
  TARGET ClusterManagerParser
  OUTPUTS ${iClaustron_BINARY_DIR}/src/ic_lex.c)
ADD_CUSTOM_COMMAND(
  SOURCE ${iClaustron_SOURCE_DIR}/cluster_mgr/ic_parser.y
  COMMAND ${BISON_EXECUTABLE}
  ARGS -y ${iClaustron_SOURCE_DIR}/cluster_mgr/ic_parser.y
       -o ${iClaustron_BINARY_DIR}/src/ic_parser.c
  TARGET ClusterManagerParser
  DEPENDS ${iClaustron_BINARY_DIR}/src/ic_lex.c
  OUTPUTS ${iClaustron_BINARY_DIR}/src/ic_parser.c
  OUTPUTS ${iClaustron_BINARY_DIR}/src/ic_parser.h)

#Add ic_parser.c to source files
  SET(ClusterManagerSource ${ClusterManagerSource}
      ${iClaustron_BINARY_DIR}/src/ic_parser.c)
#Mark ic_parser.c as a generated file
  SET_SOURCE_FILE_PROPERTIES(${iClaustron_BINARY_DIR}/src/ic_parser.c
                             GENERATED)
#Include binary directory to ensure ic_lex.c is included as well
  INCLUDE_DIRECTORIES(${iClaustron_BINARY_DIR}/src)
