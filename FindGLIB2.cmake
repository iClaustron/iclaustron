# - Try to find GLib2
# Once done this will define
#
#  GLIB2_FOUND - system has GLib2
#  GLIB2_INCLUDE_DIRS - the GLib2 include directory
#  GLIB2_LIBRARIES - Link these to use GLib2
#  GLIB2_DEFINITIONS - Compiler switches required for using GLib2
#
#  Copyright (c) 2006 Andreas Schneider <mail@cynapses.org>
#  Copyright (c) 2006 Philippe Bernery <philippe.bernery@gmail.com>
#  Copyright (c) 2007 Daniel Gollub <dgollub@suse.de>
#  Copyright (c) 2008-2010 Mikael Ronstrom <mikael.ronstrom@gmail.com>
#   Updated to use FindPkgConfig and fit it for iClaustron needs
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#
if (GLIB2_LIBRARIES AND GLIB2_INCLUDE_DIRS)
  # in cache already
  message("glib in cache already")
  set(GLIB2_FOUND TRUE)
else (GLIB2_LIBRARIES AND GLIB2_INCLUDE_DIRS)
  # use pkg-config to get the directories and then use these values
  # in the FIND_PATH() and FIND_LIBRARY() calls
  set (GLIB2_DEFINITIONS "")
  include(FindPkgConfig)

  set (WINDOWS_GLIB_INCLUDE "C:\\Program Files\\GTK\\include\\glib-2.0")
  set (WINDOWS_GLIB_INCLUDE ${WINDOWS_GLIB_INCLUDE} "C:\\Program Files (x86)\\GTK\\include\\glib-2.0")
  set (WINDOWS_GLIB_INCLUDE ${WINDOWS_GLIB_INCLUDE} "C:\\Program Files\\GTK\\lib\\glib-2.0\\include")
  set (WINDOWS_GLIB_INCLUDE ${WINDOWS_GLIB_INCLUDE} "C:\\Program Files (x86)\\GTK\\lib\\glib-2.0\\include")
  
  set (WINDOWS_GLIB_LIB "C:/Program Files (x86)/GTK/lib")
  set (WINDOWS_GLIB_LIB ${WINDOWS_GLIB_LIB} "C:\\Program Files\\GTK\\lib")

  set (UNIX_GLIB_LIB /opt/gnome/lib)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /opt/gnome/lib64)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /opt/lib)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /opt/local/lib)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /sw/lib)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /usr/lib64)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /usr/lib)
  set (UNIX_GLIB_LIB ${UNIX_GLIB_LIB} /usr/local/lib)

  set (UNIX_GLIB_INCLUDE /opt/gnome/include/glib-2.0)
  set (UNIX_GLIB_INCLUDE ${UNIX_GLIB_INCLUDE} /opt/include/glib-2.0)
  set (UNIX_GLIB_INCLUDE ${UNIX_GLIB_INCLUDE} /opt/local/include/glib-2.0)
  set (UNIX_GLIB_INCLUDE ${UNIX_GLIB_INCLUDE} /sw/include/glib-2.0)
  set (UNIX_GLIB_INCLUDE ${UNIX_GLIB_INCLUDE} /usr/include/glib-2.0)
  set (UNIX_GLIB_INCLUDE ${UNIX_GLIB_INCLUDE} /usr/local/include/glib-2.0)
  ## Glib
  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  message("Looking for glib-2.0")
  pkg_search_module(_GLIB2 glib-2.0>=2.10.2)
  if (_GLIB2)
    message("glib-2.0 found by pkg-config")
    set(GLIB2_DEFINITIONS ${_GLIB2_CFLAGS})
    find_library(GLIB2_LIBRARY
      NAMES
        glib-2.0
      PATHS
        ${_GLIB2_LIBRARY_DIRS}
      NO_DEFAULT_PATH) 
    find_path(GLIBCONFIG_INCLUDE_DIR
      NAMES
        glibconfig.h
      PATHS
        ${_GLIB2_INCLUDE_DIRS}
        ${_GLIB2_INCLUDE_DIRS}/include
      NO_DEFAULT_PATH)
    find_path(GLIB2_INCLUDE_DIR
      NAMES
        glib.h
      PATHS
        ${_GLIB2_INCLUDE_DIRS}
      NO_DEFAULT_PATH)
  else (_GLIB2)
    message("glib-2.0 not found by pkg-config")
  endif (_GLIB2)

  if (NOT GLIB2_LIBRARY)
    if (WIN32)
      find_library(GLIB2_LIBRARY
        NAMES glib-2.0
        PATHS "C:\\Program Files (x86)\\GTK\\lib")
    else (WIN32)
      find_library(GLIB2_LIBRARY
        NAMES glib-2.0
        PATHS ${UNIX_GLIB_LIB}
        NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GLIB2_LIBRARY)
      message("Didn't find glib in standard places, trying default")
      find_library(GLIB2_LIBRARY
                   NAMES glib-2.0)
    else (NOT GLIB2_LIBRARY)
      message("Found glib-2.0 in standard place")
    endif (NOT GLIB2_LIBRARY)
    if (NOT GLIB2_LIBRARY)
      message(FATAL_ERROR "Need a glib-2.0 library, fatal error")
    else (NOT GLIB2_LIBRARY)
      message("Found glib in default place")
    endif (NOT GLIB2_LIBRARY)
  else (NOT GLIB2_LIBRARY)
    message("glib-2.0 found in cache")
  endif (NOT GLIB2_LIBRARY)

  if (NOT GLIBCONFIG_INCLUDE_DIR)
    if (NOT WIN32)
      find_path(GLIBCONFIG_INCLUDE_DIR
        NAMES
          glibconfig.h
        PATHS
          /opt/gnome/lib64/glib-2.0/include
          /opt/gnome/lib/glib-2.0/include
          /opt/lib/glib-2.0/include
          /opt/local/lib/glib-2.0/include
          /sw/lib/glib-2.0/include
          /usr/lib64/glib-2.0/include
          /usr/lib/glib-2.0/include
          /usr/local/lib/glib-2.0/include
          /usr/lib/x86_64-linux-gnu/glib-2.0/include
        NO_DEFAULT_PATH)
    else(NOT WIN32)
      find_path(GLIBCONFIG_INCLUDE_DIR
        NAMES
          glibconfig.h
        PATHS
          ${WINDOWS_GLIB_INCLUDE}
        NO_DEFAULT_PATH)
    endif(NOT WIN32)
    if (NOT GLIBCONFIG_INCLUDE_DIR)
      message("glibconfig.h not found in standard places")
      find_path(GLIBCONFIG_INCLUDE_DIR
                NAMES glibconfig.h)
    else (NOT GLIBCONFIG_INCLUDE_DIR)
      message("glibconfig.h found in standard places")
    endif (NOT GLIBCONFIG_INCLUDE_DIR)
    if (NOT GLIBCONFIG_INCLUDE_DIR)
      message(FATAL_ERROR "glibconfig.h not found in default places, fatal error")
    else (NOT GLIBCONFIG_INCLUDE_DIR)
      message("glibconfig.h found in default places")
    endif (NOT GLIBCONFIG_INCLUDE_DIR)
  else (NOT GLIBCONFIG_INCLUDE_DIR)
    message("glibconfig.h found in cache")
  endif (NOT GLIBCONFIG_INCLUDE_DIR)

  if (NOT GLIB2_INCLUDE_DIR)
    if (WIN32)
      find_path(GLIB2_INCLUDE_DIR
        NAMES
          glib.h
        PATHS
          ${WINDOWS_GLIB_INCLUDE}
        NO_DEFAULT_PATH)
    else (WIN32)
      find_path(GLIB2_INCLUDE_DIR
        NAMES
          glib.h
        PATHS
          ${UNIX_GLIB_INCLUDE}
        NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GLIB2_INCLUDE_DIR)
      message("glib.h not found in standard places")
      find_path(GLIB2_INCLUDE_DIR
                NAMES glib.h)
    else (NOT GLIB2_INCLUDE_DIR)
      message("glib.h found in standard places")
    endif (NOT GLIB2_INCLUDE_DIR)
    if (NOT GLIB2_INCLUDE_DIR)
      message(FATAL_ERROR "glib.h not found in default places, fatal error")
    else (NOT GLIB2_INCLUDE_DIR)
      message("glib.h found in default places")
    endif (NOT GLIB2_INCLUDE_DIR)
  else (NOT GLIB2_INCLUDE_DIR)
    message("glib.h found in cache")
  endif (NOT GLIB2_INCLUDE_DIR)

  if (GLIB2_LIBRARY AND GLIB2_INCLUDE_DIR AND GLIBCONFIG_INCLUDE_DIR)
    message("Found glib library and includes")
    set(GLIB2_FOUND TRUE)
  endif (GLIB2_LIBRARY AND GLIB2_INCLUDE_DIR AND GLIBCONFIG_INCLUDE_DIR)

  ## GThread
  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  message("Looking for gthread-2.0")
  pkg_search_module(_GTHREAD2 gthread-2.0)
  if (_GTHREAD2)
    message("gthread-2.0 found by pkg-config")
    set(GLIB2_DEFINITIONS ${GLIB2_DEFINITIONS}${_GTHREAD2_CFLAGS})
    find_library(GTHREAD2_LIBRARY
      NAMES
        gthread-2.0
      PATHS
        ${_GTHREAD2_LIBRARY_DIRS}
      NO_DEFAULT_PATH) 
    find_path(GTHREAD2_INCLUDE_DIR
      NAMES
        gthread.h
      PATHS
        ${_GTHREAD2_INCLUDE_DIRS}
      PATH_SUFFIXES
        glib
      NO_DEFAULT_PATH)
  else (_GTHREAD2)
    message("gthread-2.0 not found by pkg-config")
  endif (_GTHREAD2)

  if (NOT GTHREAD2_LIBRARY)
    if (WIN32)
      find_library(GTHREAD2_LIBRARY
        NAMES gthread-2.0
        PATHS 
          ${WINDOWS_GLIB_LIB}
        NO_DEFAULT_PATH)
    else (WIN32)
      find_library(GTHREAD2_LIBRARY
        NAMES
          gthread-2.0
        PATHS
          ${UNIX_GLIB_LIB}
        NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GTHREAD2_LIBRARY)
      find_library(GTHREAD2_LIBRARY
                   NAMES gthread-2.0)
    endif (NOT GTHREAD2_LIBRARY)
  endif (NOT GTHREAD2_LIBRARY)

  if (NOT GTHREAD2_INCLUDE_DIR)
    if (WIN32)
      find_path(GTHREAD2_INCLUDE_DIR
        NAMES
          gthread.h
        PATHS
          ${WINDOWS_GLIB_INCLUDE}
        PATH_SUFFIXES
          glib
        NO_DEFAULT_PATH)
    else (WIN32)
      find_path(GTHREAD2_INCLUDE_DIR
        NAMES
          gthread.h
        PATHS
          ${UNIX_GLIB_INCLUDE}
        PATH_SUFFIXES
          glib
        NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GTHREAD2_INCLUDE_DIR)
      find_path(GTHREAD2_INCLUDE_DIR
                NAMES gthread.h)
    endif (NOT GTHREAD2_INCLUDE_DIR)
  endif (NOT GTHREAD2_INCLUDE_DIR)

  if (GTHREAD2_LIBRARY AND GTHREAD2_INCLUDE_DIR)
    set(GTHREAD2_FOUND TRUE)
    message("Found gthread library and includes")
  endif (GTHREAD2_LIBRARY AND GTHREAD2_INCLUDE_DIR)

  ## GModule
  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  message("Looking for gmodule-2.0")
  pkg_search_module(_GMODULE2 gmodule-2.0)
  if (_GMODULE2)
    message("gmodule-2.0 found by pkg-config")
    set(GLIB2_DEFINITIONS ${GLIB2_DEFINITIONS}${_GMODULE2_CFLAGS})
    find_library(GMODULE2_LIBRARY
      NAMES
        gmodule-2.0
      PATHS
        ${_GMODULE2_LIBRARY_DIRS}
      NO_DEFAULT_PATH) 
    find_path(GMODULE2_INCLUDE_DIR
      NAMES
        gmodule.h
      PATHS
        ${_GMODULE2_INCLUDE_DIRS}
      NO_DEFAULT_PATH)
  else (_GMODULE2)
    message("gmodule-2.0 not found by pkg-config")
  endif (_GMODULE2)

  if (NOT GMODULE2_LIBRARY)
    if (WIN32)
      find_library(GMODULE2_LIBRARY
        NAMES gmodule-2.0
        PATHS 
          ${WINDOWS_GLIB_LIB}
          NO_DEFAULT_PATH)
    else (WIN32)
      find_library(GMODULE2_LIBRARY
        NAMES
          gmodule-2.0
        PATHS
          ${UNIX_GLIB_LIB}
          NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GMODULE2_LIBRARY)
      find_library(GMODULE2_LIBRARY
                   NAMES gmodule-2.0)
    endif (NOT GMODULE2_LIBRARY)
  endif (NOT GMODULE2_LIBRARY)

  if (NOT GMODULE2_INCLUDE_DIR)
    if (WIN32)
      find_path(GMODULE2_INCLUDE_DIR
        NAMES
          gmodule.h
        PATHS
          ${WINDOWS_GLIB_INCLUDE}
          NO_DEFAULT_PATH)
    else (WIN32)
      find_path(GMODULE2_INCLUDE_DIR
        NAMES
          gmodule.h
        PATHS
          ${UNIX_GLIB_INCLUDE}
          NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GMODULE2_INCLUDE_DIR)
      find_path(GMODULE2_INCLUDE_DIR
                NAMES gmodule.h)
    endif (NOT GMODULE2_INCLUDE_DIR)
  endif (NOT GMODULE2_INCLUDE_DIR)

  if (GMODULE2_LIBRARY AND GMODULE2_INCLUDE_DIR)
    message("Found gmodule library and includes")
    set(GMODULE2_FOUND TRUE)
  endif (GMODULE2_LIBRARY AND GMODULE2_INCLUDE_DIR)

  ## GObject
  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  message("Looking for gobject-2.0")
  pkg_search_module(_GOBJECT2 gobject-2.0)
  if (_GOBJECT2)
    message("gobject-2.0 found by pkg-config")
    set(GLIB2_DEFINITIONS ${GLIB2_DEFINITIONS}${_GOBJECT2_CFLAGS})
    find_library(GOBJECT2_LIBRARY
      NAMES
        gobject-2.0
      PATHS
        ${_GOBJECT2_LIBRARY_DIRS}
      NO_DEFAULT_PATH) 
    find_path(GOBJECT2_INCLUDE_DIR
      NAMES
        gobject.h
      PATHS
        ${_GOBJECT2_INCLUDE_DIRS}
      PATH_SUFFIXES
        gobject
      NO_DEFAULT_PATH)
  else (_GOBJECT2)
    message("gobject-2.0 not found by pkg-config")
  endif (_GOBJECT2)

  if (NOT GOBJECT2_LIBRARY)
    if (WIN32)
      find_library(GOBJECT2_LIBRARY
        NAMES gobject-2.0
        PATHS 
          ${WINDOWS_GLIB_LIB}
          NO_DEFAULT_PATH)
    else (WIN32)
      find_library(GOBJECT2_LIBRARY
        NAMES
          gobject-2.0
        PATHS
          ${UNIX_GLIB_LIB}
          NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GOBJECT2_LIBRARY)
      find_library(GOBJECT2_LIBRARY
                   NAMES gobject-2.0)
    endif (NOT GOBJECT2_LIBRARY)
  endif (NOT GOBJECT2_LIBRARY)

  if (NOT GOBJECT2_INCLUDE_DIR)
    if (WIN32)
      find_path(GOBJECT2_INCLUDE_DIR
        NAMES
          gobject.h
        PATHS
          ${WINDOWS_GLIB_INCLUDE}
        PATH_SUFFIXES
          gobject
        NO_DEFAULT_PATH)
    else (WIN32)
      find_path(GOBJECT2_INCLUDE_DIR
        NAMES
          gobject.h
        PATHS
          ${UNIX_GLIB_INCLUDE}
        PATH_SUFFIXES
          gobject
        NO_DEFAULT_PATH)
    endif (WIN32)
    if (NOT GOBJECT2_INCLUDE_DIR)
      find_path(GOBJECT2_INCLUDE_DIR
                NAMES gobject.h)
    endif (NOT GOBJECT2_INCLUDE_DIR)
  endif (NOT GOBJECT2_INCLUDE_DIR)

  if (GOBJECT2_LIBRARY AND GOBJECT2_INCLUDE_DIR)
    message("Found gobject library and includes")
    set(GOBJECT2_FOUND TRUE)
  endif (GOBJECT2_LIBRARY AND GOBJECT2_INCLUDE_DIR)

  ## libintl
  find_path(LIBINTL_INCLUDE_DIR
    NAMES
      libintl.h
    NO_DEFAULT_PATH
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /opt/local/include
      /opt/include
      /sw/include
      /usr/include/glib-2.0
      /usr/local/include/glib-2.0
  )

  find_library(LIBINTL_LIBRARY
    NAMES
      intl
    NO_DEFAULT_PATH
    PATHS
      /opt/gnome/lib
      /opt/local/lib
      /opt/lib
      /sw/lib
      /usr/lib
      /usr/lib64
      /usr/local/lib
  )

  if (LIBINTL_LIBRARY AND LIBINTL_INCLUDE_DIR)
    set(LIBINTL_FOUND TRUE)
  endif (LIBINTL_LIBRARY AND LIBINTL_INCLUDE_DIR)
  ##

  ## libiconv
  find_path(LIBICONV_INCLUDE_DIR
    NAMES
      iconv.h
    NO_DEFAULT_PATH
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /opt/local/include
      /opt/include
      /sw/include
      /sw/include/glib-2.0
      /usr/local/include/glib-2.0
      /usr/include/glib-2.0
  )

  find_library(LIBICONV_LIBRARY
    NAMES
      iconv
    NO_DEFAULT_PATH
    PATHS
      /opt/gnome/lib
      /opt/gnome/lib64
      /opt/local/lib
      /opt/lib
      /sw/lib
      /usr/lib
      /usr/lib64
      /usr/local/lib
  )

  if (LIBICONV_LIBRARY AND LIBICONV_INCLUDE_DIR)
    set(LIBICONV_FOUND TRUE)
  endif (LIBICONV_LIBRARY AND LIBICONV_INCLUDE_DIR)
  ##

  if (NOT GLIB2_FOUND)
    message(FATAL_ERROR "Need a glib-2.0 library, fatal error")
  endif (NOT GLIB2_FOUND)
  if (NOT GTHREAD2_FOUND)
    message(FATAL_ERROR "Need a gthread-2.0 library, fatal error")
  endif (NOT GTHREAD2_FOUND)

  set(GLIB2_INCLUDE_DIRS ${GLIB2_INCLUDE_DIR} ${GLIBCONFIG_INCLUDE_DIR})
  set (GLIB2_LIBRARIES ${GLIB2_LIBRARY})

  if (GMODULE2_FOUND)
    set(GLIB2_LIBRARIES ${GLIB2_LIBRARIES} ${GMODULE2_LIBRARY})
    set(GLIB2_INCLUDE_DIRS ${GLIB2_INCLUDE_DIRS} ${GMODULE2_INCLUDE_DIR})
  endif (GMODULE2_FOUND)

  if (GTHREAD2_FOUND)
    set(GLIB2_LIBRARIES ${GLIB2_LIBRARIES} ${GTHREAD2_LIBRARY})
    set(GLIB2_INCLUDE_DIRS ${GLIB2_INCLUDE_DIRS} ${GTHREAD2_INCLUDE_DIR})
  endif (GTHREAD2_FOUND)

  if (GOBJECT2_FOUND)
    set(GLIB2_LIBRARIES ${GLIB2_LIBRARIES} ${GOBJECT2_LIBRARY})
    set(GLIB2_INCLUDE_DIRS ${GLIB2_INCLUDE_DIRS} ${GOBJECT2_INCLUDE_DIR})
  endif (GOBJECT2_FOUND)

  if (LIBINTL_FOUND)
    set(GLIB2_LIBRARIES ${GLIB2_LIBRARIES} ${LIBINTL_LIBRARY})
    set(GLIB2_INCLUDE_DIRS ${GLIB2_INCLUDE_DIRS} ${LIBINTL_INCLUDE_DIR})
  endif (LIBINTL_FOUND)

  if (LIBICONV_FOUND)
    set(GLIB2_LIBRARIES ${GLIB2_LIBRARIES} ${LIBICONV_LIBRARY})
    set(GLIB2_INCLUDE_DIRS ${GLIB2_INCLUDE_DIRS} ${LIBICONV_INCLUDE_DIR})
  endif (LIBICONV_FOUND)

  # show the GLIB2_INCLUDE_DIRS and GLIB2_LIBRARIES variables only in the advanced view
  mark_as_advanced(GLIB2_INCLUDE_DIRS GLIB2_LIBRARIES)

  # same for all other variables
  mark_as_advanced(GLIB2_INCLUDE_DIR GLIB2_LIBRARY GLIBCONFIG_INCLUDE_DIR)
  mark_as_advanced(GMODULE2_INCLUDE_DIR GMODULE2_LIBRARY)
  mark_as_advanced(GOBJECT2_INCLUDE_DIR GOBJECT2_LIBRARY)
  mark_as_advanced(GTHREAD2_INCLUDE_DIR GTHREAD2_LIBRARY)
  mark_as_advanced(LIBICONV_INCLUDE_DIR LIBICONV_LIBRARY)
  mark_as_advanced(LIBINTL_INCLUDE_DIR LIBINTL_LIBRARY)

endif (GLIB2_LIBRARIES AND GLIB2_INCLUDE_DIRS)
