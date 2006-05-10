#
# A configure.ac file needs to start by defining tree name and version
# of the tree.
# After that invoking automake (AM_INIT_AUTOMAKE) and then creating the
# portability header file config.h
#
AC_PREREQ(2.59)
AC_INIT
AC_CONFIG_AUX_DIR(config)
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE(iclaustron, 0.0.1)
AM_CONFIG_HEADER(include/config.h)

m4_ifdef([LT_INIT],
         [LT_INIT],
         [AC_PROG_LIBTOOL])
#
# Next step is to define all the ./configure options. This usually starts
# with a AC_ARG_ENABLE followed by a number of tests of various sorts to
# handle dependencies among different options.
#

#
# Next step is to check for all the programs needed by the configure process.
# These include usage of macros such as AC_CHECK_PROG and AC_PATH_TOOL.
#

AC_PROG_CC
AC_PROG_YACC
AC_PROG_LEX
AC_PROG_LIBTOOL
AC_CONFIG_FILES([Makefile port/Makefile comm/Makefile api_dataserver/Makefile api_clusterserver/Makefile control/Makefile cclient/Makefile clusterserver/Makefile repserver/Makefile fileserver/Makefile test/Makefile])

#
# Next step is to check for all libraries such that the resulting programs
# can be linked properly due to external dependencies.
#

AC_CHECK_FUNCS(bzero memset, break)
AC_CHECK_FUNCS(socket bind connect listen accept)

#
# Next step is to check the existence of external header files.
#
AC_CHECK_HEADER([unistd.h netinet/in.h sys/types.h sys/socket.h])
#
# Next step is to check for specific typedefs and structures found in the
# external headers.
#

#
# We check for all specific functions to be used by the tree.
#

#
# Produce the output of the configure process.
#
AC_OUTPUT
