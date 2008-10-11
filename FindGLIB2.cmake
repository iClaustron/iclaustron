# - Try to find GLib2
# Once done this will define
#
#  GLIB2_FOUND - system has GLib2
#  GLIB2_INCLUDE_DIRS - the GLib2 include directory
#  GLIB2_LIBRARIES - Link these to use GLib2
#  GLIB2_DEFINITIONS - Compiler switches required for using GLib2
#  GLIB2_LINK_FLAGS - Link flags
#
#  Copyright (c) 2006 Andreas Schneider <mail@cynapses.org>
#  Copyright (c) 2006 Philippe Bernery <philippe.bernery@gmail.com>
#  Copyright (c) 2007 Daniel Gollub <dgollub@suse.de>
#  Copyright (c) 2008 Mikael Ronstrom <mikael.ronstrom@gmail.com>
#   Updated to use FindPkgConfig and fit it for iClaustron needs
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#


if (GLIB2_LIBRARIES AND GLIB2_INCLUDE_DIRS AND GLIB2_PUBLIC_LINK_FLAGS)
  # in cache already
  set(GLIB2_FOUND TRUE)
else (GLIB2_LIBRARIES AND GLIB2_INCLUDE_DIRS AND GLIB2_PUBLIC_LINK_FLAGS)
  # use pkg-config to get the directories and then use these values
  # in the FIND_PATH() and FIND_LIBRARY() calls
  include(FindPkgConfig)

  ## Glib
  pkg_search_module(_GLIB2 REQUIRED glib-2.0>=2.10.2)

  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  find_path(GLIBCONFIG_INCLUDE_DIR
    NAMES
      glibconfig.h
    PATHS
      ${_GLIB2_INCLUDE_DIRS}
      ${_GLIB2_INCLUDE_DIRS}/include
    NO_DEFAULT_PATH
  )

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
  )

  set(GLIB2_DEFINITIONS ${_GLIB2_CFLAGS})

  find_path(GLIB2_INCLUDE_DIR
    NAMES
      glib.h
    PATHS
      ${_GLIB2_INCLUDE_DIRS}
    NO_DEFAULT_PATH
  )

  find_path(GLIB2_INCLUDE_DIR
    NAMES
      glib.h
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /sw/include/glib-2.0
      /usr/include/glib-2.0
      /usr/local/include/glib-2.0
  )

  find_library(GLIB2_LIBRARY
    NAMES
      glib-2.0
    PATHS
      ${_GLIB2_LIBRARY_DIRS}
    NO_DEFAULT_PATH 
  )

  find_library(GLIB2_LIBRARY
    NAMES
      glib-2.0
    PATHS
      /opt/gnome/lib
      /opt/local/lib
      /sw/lib
      /usr/lib
      /usr/local/lib
  )
  ##

  ## GModule
  pkg_search_module(_GMODULE2 REQUIRED gmodule-2.0)

  set(GMODULE2_DEFINITIONS ${_GMODULE2_CFLAGS})

  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  find_path(GMODULE2_INCLUDE_DIR
    NAMES
      gmodule.h
    PATHS
      ${_GMODULE2_INCLUDE_DIRS}
    NO_DEFAULT_PATH 
  )

  find_path(GMODULE2_INCLUDE_DIR
    NAMES
      gmodule.h
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /sw/include/glib-2.0
      /usr/include/glib-2.0
      /usr/local/include/glib-2.0
  )

  find_library(GMODULE2_LIBRARY
    NAMES
      gmodule-2.0
    PATHS
      ${_GMODULE2_LIBRARY_DIRS}
    NO_DEFAULT_PATH 
  )

  find_library(GMODULE2_LIBRARY
    NAMES
      gmodule-2.0
    PATHS
      /opt/gnome/lib
      /opt/local/lib
      /sw/lib
      /usr/lib
      /usr/local/lib
  )
  if (GMODULE2_LIBRARY AND GMODULE2_INCLUDE_DIR)
    set(GMODULE2_FOUND TRUE)
  endif (GMODULE2_LIBRARY AND GMODULE2_INCLUDE_DIR)
  ##

  ## GThread
  pkg_search_module(_GTHREAD2 REQUIRED gthread-2.0)

  set(GTHREAD2_DEFINITIONS ${_GTHREAD2_CFLAGS})

  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  find_path(GTHREAD2_INCLUDE_DIR
    NAMES
      gthread.h
    PATHS
      ${_GTHREAD2_INCLUDE_DIRS}
    PATH_SUFFIXES
      glib
    NO_DEFAULT_PATH
  )

  find_path(GTHREAD2_INCLUDE_DIR
    NAMES
      gthread.h
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /sw/include/glib-2.0
      /usr/include/glib-2.0
      /usr/local/include/glib-2.0
    PATH_SUFFIXES
      glib
  )

  find_library(GTHREAD2_LIBRARY
    NAMES
      gthread-2.0
    PATHS
      ${_GTHREAD2_LIBRARY_DIRS}
    NO_DEFAULT_PATH 
  )

  find_library(GTHREAD2_LIBRARY
    NAMES
      gthread-2.0
    PATHS
      /opt/gnome/lib
      /opt/local/lib
      /sw/lib
      /usr/lib
      /usr/local/lib
  )

  if (GTHREAD2_LIBRARY AND GTHREAD2_INCLUDE_DIR)
    set(GTHREAD2_FOUND TRUE)
  endif (GTHREAD2_LIBRARY AND GTHREAD2_INCLUDE_DIR)
  ##

  ## GObject
  pkg_search_module(_GOBJECT2 REQUIRED gobject-2.0)

  set(GOBJECT2_DEFINITIONS ${_GOBJECT2_CFLAGS})

  # Prefer pkg-config results for custom builds found in PKG_CONFIG_PATH
  find_path(GOBJECT2_INCLUDE_DIR
    NAMES
      gobject.h
    PATHS
      ${_GOBJECT2_INCLUDE_DIRS}
    PATH_SUFFIXES
      gobject
    NO_DEFAULT_PATH 
  )

  find_path(GOBJECT2_INCLUDE_DIR
    NAMES
      gobject.h
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /sw/include/glib-2.0
      /usr/include/glib-2.0
      /usr/local/include/glib-2.0
    PATH_SUFFIXES
      gobject
  )

  find_library(GOBJECT2_LIBRARY
    NAMES
      gobject-2.0
    PATHS
      ${_GOBJECT2_LIBRARY_DIRS}
    NO_DEFAULT_PATH 
  )

  find_library(GOBJECT2_LIBRARY
    NAMES
      gobject-2.0
    PATHS
      /opt/gnome/lib
      /opt/local/lib
      /sw/lib
      /usr/lib
      /usr/local/lib
  )

  if (GOBJECT2_LIBRARY AND GOBJECT2_INCLUDE_DIR)
    set(GOBJECT2_FOUND TRUE)
  endif (GOBJECT2_LIBRARY AND GOBJECT2_INCLUDE_DIR)
  ##

  ## libintl
  find_path(LIBINTL_INCLUDE_DIR
    NAMES
      libintl.h
    NO_DEFAULT_PATH
    PATHS
      /opt/gnome/include/glib-2.0
      /opt/local/include/glib-2.0
      /opt/local/include
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
      /sw/lib
      /usr/local/lib
      /usr/lib
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
      /opt/local/lib
      /sw/lib
      /usr/lib
      /usr/local/lib
  )

  if (LIBICONV_LIBRARY AND LIBICONV_INCLUDE_DIR)
    set(LIBICONV_FOUND TRUE)
  endif (LIBICONV_LIBRARY AND LIBICONV_INCLUDE_DIR)
  ##

  set(GLIB2_INCLUDE_DIRS
    ${GLIB2_INCLUDE_DIR}
    ${GLIBCONFIG_INCLUDE_DIR}
  )
  set(GLIB2_LIBRARIES
    ${GLIB2_LIBRARY}
  )

  set(GLIB2_PUBLIC_LINK_FLAGS
    ${_GLIB2_LDFLAGS} ${_GMODULE2_LDFLAGS} ${_GTHREAD2_LDFLAGS}
  )

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

  if (GLIB2_INCLUDE_DIRS AND GLIB2_LIBRARIES)
    set(GLIB2_FOUND TRUE)
  endif (GLIB2_INCLUDE_DIRS AND GLIB2_LIBRARIES)

  if (GLIB2_FOUND)
    if (NOT GLIB2_FIND_QUIETLY)
      message(STATUS "Found GLib2: ${GLIB2_LIBRARIES}")
    endif (NOT GLIB2_FIND_QUIETLY)
  else (GLIB2_FOUND)
    if (GLIB2_FIND_REQUIRED)
      message(FATAL_ERROR "Could not find GLib2")
    endif (GLIB2_FIND_REQUIRED)
  endif (GLIB2_FOUND)

  # show the GLIB2_INCLUDE_DIRS and GLIB2_LIBRARIES variables only in the advanced view
  mark_as_advanced(GLIB2_INCLUDE_DIRS GLIB2_LIBRARIES GLIB2_PUBLIC_LINK_FLAGS)

  # same for all other variables
  mark_as_advanced(GLIB2_INCLUDE_DIR GLIB2_LIBRARY GLIBCONFIG_INCLUDE_DIR)
  mark_as_advanced(GMODULE2_INCLUDE_DIR GMODULE2_LIBRARY)
  mark_as_advanced(GOBJECT2_INCLUDE_DIR GOBJECT2_LIBRARY)
  mark_as_advanced(GTHREAD2_INCLUDE_DIR GTHREAD2_LIBRARY)
  mark_as_advanced(LIBICONV_INCLUDE_DIR LIBICONV_LIBRARY)
  mark_as_advanced(LIBINTL_INCLUDE_DIR LIBINTL_LIBRARY)

endif (GLIB2_LIBRARIES AND GLIB2_INCLUDE_DIRS AND GLIB2_PUBLIC_LINK_FLAGS)
