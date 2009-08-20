dnl A configure.ac file needs to start by defining tree name and version
dnl of the tree.
dnl After that invoking automake (AM_INIT_AUTOMAKE) and then creating the
dnl portability header file config.h

AC_PREREQ(2.59)
AC_INIT
AC_CONFIG_AUX_DIR(config)
AC_CANONICAL_SYSTEM
AM_INIT_AUTOMAKE(iclaustron, 0.0.1)
AM_CONFIG_HEADER(include/config.h)
AC_LANG(C)

m4_ifdef([LT_INIT],
         [LT_INIT],
         [AC_PROG_LIBTOOL])

dnl Next step is to define all the ./configure options. This usually starts
dnl with a AC_ARG_ENABLE followed by a number of tests of various sorts to
dnl handle dependencies among different options.

AC_ARG_WITH(unit-test,
         [  --with-unit-test     Build unit test programs],
         [with_unit_testn=$withval],
         [with_unit_test=no])

AC_ARG_WITH(production,
         [  --with-production     Build a production version],
         [with_production=$withval],
         [with_production=no])

AC_ARG_WITH(debug,
         [  --with-debug      Build a debug version],
         [with_debug=$withval],
         [with_debug=no])

AC_ARG_WITH(readline,
         [  --with-readline   Build with readline],
         [with_readline=$withval],
         [with_readline=no])

AC_ARG_WITH(dtrace,
         [  --with-dtrace     Build with DTrace support],
         [with_dtrace=$withval],
         [with_dtrace=no])

AC_ARG_WITH(openssl,
         AC_HELP_STRING([--with-openssl=DIR],
                        [Use openssl, if DIR is provided it is the
                         installation directory of openssl.]),
         [openssl_dir=$withval],
         [openssl_dir=no])

AC_ARG_WITH(cyassl,
         AC_HELP_STRING([--with-cyassl=DIR],
                        [Use cyassl, if DIR is provided it is the
                         installation directory of cyassl.]),
         [cyassl_dir=$withval],
         [cyassl_dir=no])

dnl Next step is to check for all the programs needed by the configure process.
dnl These include usage of macros such as AC_CHECK_PROG and AC_PATH_TOOL.

AC_PROG_CC
AC_PROG_YACC
AM_PROG_LEX
AC_PROG_LIBTOOL
AC_CONFIG_FILES([Makefile scripts/Makefile include/Makefile port/Makefile util/Makefile comm/Makefile api/Makefile cluster_server/Makefile pcntrl/Makefile cluster_client/Makefile cluster_mgr/Makefile repserver/Makefile fileserver/Makefile test/Makefile])

AC_CONFIG_FILES([bootstrap.sh], [chmod +x bootstrap.sh])
AC_CONFIG_FILES([setup.sh], [chmod +x setup.sh])
AC_CONFIG_FILES([count_line.sh], [chmod +x count_line.sh])
AC_CONFIG_FILES([scripts/check_process.sh], [chmod +x scripts/check_process.sh])
AC_CONFIG_FILES([scripts/windows_check_process.sh], [chmod +x scripts/windows_check_process.sh])

AC_PATH_PROG(PKG_CONFIG, pkg-config, no)

dnl We check for all specific functions to be used by the tree.
dnl Set up operating system

AC_CHECK_FUNCS(bzero memset break gethrtime gettimeofday epoll_create)
AC_CHECK_FUNCS(port_create kqueue poll)
AC_CHECK_LIB(rt, clock_gettime,
  [LIBS="-lrt $LIBS"
   AC_DEFINE(HAVE_CLOCK_GETTIME, 1,
             [Define HAVE_CLOCK_GETTIME if clock_gettime function exists])])

case "$target_os" in
  *Linux* | *linux* )
    AC_DEFINE(LINUX, 1,
              [Define LINUX when OS is Linux])
    ;;
  *sunos* | *solaris* )
    AC_DEFINE(SOLARIS, 1,
              [Define SOLARIS when OS is Solaris])
    ;;
  *freebsd* )
    AC_DEFINE(FREEBSD, 1,
              [Define FREEBSD when OS is FreeBSD])
    ;;
  *darwin* )
    AC_DEFINE(MACOSX, 1,
              [Define MACOSX when OS is Mac OS X])
    ;;
  *)
    AC_MSG_ERROR([OS ${target_os} isn't supported])
    ;;
esac

case "$host" in
  *solaris*)
    AC_CHECK_LIB(nsl, gethostbyname,
      [LIBS="-lnsl $LIBS"
      AC_DEFINE(HAVE_SOCKET, 1,
             [Define HAVE_SOCKET if socket function exists])])
    AC_CHECK_LIB(socket, socket,
      [LIBS="-lsocket $LIBS"
      AC_DEFINE(HAVE_SOCKET, 1,
             [Define HAVE_SOCKET if socket function exists])])
    AC_SEARCH_LIBS(bind, socket)
    AC_SEARCH_LIBS(connect, socket)
    AC_SEARCH_LIBS(listen, socket)
    AC_SEARCH_LIBS(accept, socket)
    AC_SEARCH_LIBS(getaddrinfo, socket)
    AC_SEARCH_LIBS(freeaddrinfo, socket)
    AC_SEARCH_LIBS(inet_ntop, socket)
    AC_SEARCH_LIBS(sendmsg, socket)
    ;;
  *)
    AC_CHECK_FUNCS(socket bind connect listen accept getaddrinfo freeaddrinfo)
    AC_CHECK_FUNCS(inet_ntop sendmsg)
    ;;
esac

dnl Set up library and includes for GLib
dnl Check for certain libraries such as readline

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
CFLAGS="$GLIB_INCLUDE"

AC_CHECK_LIB(glib-2.0, g_list_append,,
 [AC_MSG_ERROR([No glib library installed])])

AS_IF([test "x$with_readline" != "xno"],
  [AC_CHECK_HEADERS([readline/readline.h],,
    [AC_MSG_ERROR([Missing readline/readline.h])])])
AS_IF([test "x$with_readline" != "xno"],
  [AC_CHECK_LIB([readline], [main],
    [AC_SUBST([LIBREADLINE], ["-lreadline -lncurses"])
     AC_DEFINE([HAVE_LIBREADLINE], [1], [Define if you have libreadline])
    ],
    [AC_MSG_FAILURE(
       [readline wasn't available, remove --with-readline option])],
    [-lcurses])])

LIBS="$LIBS $LIBREADLINE"

dnl Next step is to check the existence of external header files.

AC_HEADER_STDC
AC_CHECK_HEADERS([arpa/inet.h netinet/in.h netinet/tcp.h sys/poll.h sys/select.h sys/socket.h sys/times.h sys/types.h sys/wait.h fcntl.h netdb.h poll.h signal.h time.h unistd.h],,
  [AC_MSG_ERROR([Common headers missing, e.g. socket.h])])
AC_CHECK_HEADER(glib.h,,
  [AC_MSG_ERROR([GLib headers missing])])

dnl Next step is to check for specific typedefs and structures found in the
dnl external headers.

dnl Set compiler flags according to the user choice

DEBUG_FLAG="-g"
OPTIMIZER_FLAG="-O2"
WALL_FLAGS="-W -Wall"
WARNING_FLAG="-W"

if test "x$with_production" = "xyes"; then
  if test "x$with_debug" = "xyes"; then
    CFLAGS="$CFLAGS -DDEBUG_BUILD $DEBUG_FLAG $OPTIMIZER_FLAG $WARNING_FLAG"
  else
    CFLAGS="$CFLAGS $WARNING_FLAG $OPTIMIZER_FLAG"
  fi
else
  if test "x$with_debug" = "xyes"; then
    CFLAGS="$CFLAGS -DDEBUG_BUILD $DEBUG_FLAG $WALL_FLAGS"
  else
    CFLAGS="$CFLAGS $WALL_FLAGS $DEBUG_FLAG $OPTIMIZER_FLAG"
  fi
fi
if test "x$with_unit_test" = "xyes" ; then
  CFLAGS="$CFLAGS -DWITH_UNIT_TEST"
fi
LIBS="$LIBS -lm"

dnl Check how to implement MSG_NOSIGNAL
dnl Linux supports it but not Mac OS X and Solaris
dnl Mac OS X instead supports SO_NOSIGPIPE

AC_MSG_CHECKING(whether we have MSG_NOSIGNAL in this platform)
AC_TRY_COMPILE([#include <sys/socket.h>],
[
int main()
{
 int f=MSG_NOSIGNAL;
}
],
dnl It was supported
[ AC_MSG_RESULT(yes)
 CFLAGS="$CFLAGS -DUSE_MSG_NOSIGNAL"
],
dnl Don't use it
AC_MSG_RESULT(no)
)

dnl Checking for DTrace support

if test "x$with_dtrace" != "xno" ; then
  AC_CHECK_PROGS(DTRACE, dtrace)
  if test ! -z "$DTRACE" ; then
    AC_DEFINE([HAVE_DTRACE], [1], [Set DTrace enabled flag])
    AC_SUBST(DTRACEFLAGS)
    AC_SUBST(HAVE_DTRACE)
    AC_MSG_RESULT(yes)
  else
    with_dtrace=no
  fi
fi
AM_CONDITIONAL([HAVE_DTRACE],
               [ test "x$with_dtrace" != "xno" ])

dnl Checking for SSL support, we can use any of openssl or
dnl cyassl. We prefer openssl first, then cyassl

have_ssl="no"

case "$openssl_dir" in
  "no")
    have_openssl="no"
    AC_MSG_RESULT([not including openssl])
    ;;
  * )
    have_openssl="yes"
    if test "x$openssl_dir" = "xyes" ; then
      openssl_dir="/usr/local/ssl"
    fi
    have_ssl="yes"
    ssl_dir="$openssl_dir"
    SSL_LIB="-lssl -lcrypto"
    AC_MSG_RESULT([including openssl from $openssl_dir])
    ;;
esac

case "$cyassl_dir" in
  "no")
    have_cyassl="no"
    AC_MSG_RESULT([not including cyassl])
    ;;
  * )
    have_cyassl="yes"
    if test "x$cyassl_dir" = "xyes" ; then
      cyassl_dir="/usr/local/cyassl"
    fi
    if test "x$have_ssl" = "xno" ; then
      have_ssl="yes"
      ssl_dir="$cyassl_dir"
      SSL_LIB="-lcyassl"
      AC_MSG_RESULT([including cyassl from $cyassl_dir])
    else
      have_cyassl="no"
      AC_MSG_RESULT([ignoring cyassl option since SSL support already defined])
    fi
    ;;
esac

dnl Next step is to check for all libraries such that the resulting programs
dnl can be linked properly due to external dependencies for SSL.

if test "x$have_ssl" = "xyes" ; then
  save_LIBS="$LIBS"
  save_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -I$ssl_dir/include"
  LIBS="$LIBS $SSL_LIB"
  LIBS="$LIBS -L$ssl_dir/lib"
  AC_TRY_LINK([#include <openssl/ssl.h>],
             [return SSL_library_init();],
             [have_ssl="yes"],
             [have_ssl="no"])
  if test "x$have_ssl" = "xno" ; then
    AC_MSG_RESULT([Unsuccessful attempt to link SSL library, removing SSL])
    LIBS="$save_LIBS"
    CFLAGS="$save_CFLAGS"
  else
    AC_DEFINE([HAVE_SSL], [1], [Defined by configure, using SSL.])
  fi
fi

AS_IF([test "x$have_openssl" = "xyes"],
    [AC_DEFINE([HAVE_OPENSSL], [1],
     [Defined by configure, using openssl for SSL.])])
AS_IF([test "x$have_cyassl" = "xyes"],
    [AC_DEFINE([HAVE_CYASSL], [1],
     [Defined by configure, using cyassl for SSL.])])

dnl Produce the output of the configure process.
AC_OUTPUT
