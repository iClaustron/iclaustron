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
AC_LANG(C)

m4_ifdef([LT_INIT],
         [LT_INIT],
         [AC_PROG_LIBTOOL])
#
# Next step is to define all the ./configure options. This usually starts
# with a AC_ARG_ENABLE followed by a number of tests of various sorts to
# handle dependencies among different options.
#
AC_ARG_WITH(production,
         [  --with-production     Build a production version],
         [with_production=$withval],
         [with_production=no])

AC_ARG_WITH(debug,
         [  --with-debug      Build a debug version],
         [with_debug=$withval],
         [with_debug=no])

#
# Next step is to check for all the programs needed by the configure process.
# These include usage of macros such as AC_CHECK_PROG and AC_PATH_TOOL.
#

AC_PROG_CC
AC_PROG_YACC
AM_PROG_LEX
AC_PROG_LIBTOOL
AC_CONFIG_FILES([Makefile include/Makefile port/Makefile util/Makefile comm/Makefile api_dataserver/Makefile api_clusterserver/Makefile cluster_server/Makefile pcntrl/Makefile cluster_client/Makefile cluster_mgr/Makefile repserver/Makefile fileserver/Makefile test/Makefile])
AC_PATH_PROG(PKG_CONFIG, pkg-config, no)

#
# Set compiler flags according to the user choice
#

AC_CHECK_FUNCS(bzero memset, break)
AC_CHECK_FUNCS(socket bind connect listen accept getaddrinfo freeaddrinfo inet_ntop)
GLIB_PACKAGE="glib-2.0"
GTHREAD_PACKAGE="gthread-2.0"
GLIB_VERSION="2.10.2"
if test "$PKG_CONFIG" = "no"; then
  AC_MSG_ERROR([Error: You need to have pkg-config installed])
fi
GLIB_INCLUDE="`$PKG_CONFIG --cflags $GLIB_PACKAGE`"
GLIB_LIB="`$PKG_CONFIG --libs $GLIB_PACKAGE`"
GTHREAD_LIB="`$PKG_CONFIG --libs $GTHREAD_PACKAGE`"

LIBS="$LIBS $GLIB_LIB"
LIBS="$LIBS $GTHREAD_LIB"
IDIR="$IDIR $GLIB_INCLUDE"
CFLAGS="$CFLAGS $GLIB_INCLUDE"
#CPPFLAGS="$CPPFLAGS $GLIB_INCLUDE"

AC_CHECK_LIB(glib-2.0, g_list_append,,
 [AC_MSG_ERROR([No glib library installed])])

#
# Next step is to check the existence of external header files.
#
AC_HEADER_STDC
AC_CHECK_HEADERS([unistd.h netinet/in.h sys/types.h sys/socket.h netdb.h arpa/inet.h],,
  [AC_MSG_ERROR([Common headers missing, e.g. socket.h])])
AC_CHECK_HEADER(glib.h,,
  [AC_MSG_ERROR([GLib headers missing])])

#
# Next step is to check for specific typedefs and structures found in the
# external headers.
#

#
# We check for all specific functions to be used by the tree.
#

DEBUG_FLAG="-g"
OPTIMIZER_FLAG="-O2"
WALL_FLAGS="-W -Wall"
WARNING_FLAG="-W"

if test "x$with_production" = "xyes"; then
  if test "x$with_debug" = "xyes"; then
    CFLAGS="-DDEBUG_BUILD $DEBUG_FLAG $OPTIMIZER_FLAG"
  else
    CFLAGS="$WARNING_FLAG $OPTIMIZER_FLAG"
  fi
else
  if test "x$with_debug" = "xyes"; then
    CFLAGS="-DDEBUG_BUILD $DEBUG_FLAG $WALL_FLAGS"
  else
    CFLAGS="$WALL_FLAGS $DEBUG_FLAG $OPTIMIZER_FLAG"
  fi
fi
CFLAGS="$CFLAGS $GLIB_INCLUDE"
LIBS="$LIBS -lm"

#
# Check how to implement MSG_NOSIGNAL
# Linux supports it but not Mac OS X and Solaris
# Mac OS X instead supports SO_NOSIGPIPE
#
AC_MSG_CHECKING(whether we have MSG_NOSIGNAL in this platform)
AC_TRY_COMPILE([#include <sys/socket.h>],
[
int main()
{
 int f=MSG_NOSIGNAL;
}
],
# It was supported
[ AC_MSG_RESULT(yes)
 CFLAGS="$CFLAGS -DUSE_MSG_NOSIGNAL"
],
# Don't use it
AC_MSG_RESULT(no)
)

#
# Next step is to check for all libraries such that the resulting programs
# can be linked properly due to external dependencies.
#
#
# Produce the output of the configure process.
#
AC_OUTPUT
