#!/bin/bash
# Copyright (C) 2007 iClaustron AB
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
PWD=""
IC_PROMPT=""
INPUT_VAR=""
DEFAULT_ANSWER=""
LOG_FILE=""
CREATE_DIR=""
SRC_INSTALL_DIR=""
BIN_INSTALL_DIR=""
MYSQL_INSTALL_DIR=""
ICLAUSTRON_INSTALL_DIR=""
WITH_DEBUG=""
LOCAL_ONLY=""
LOCAL_INSTALL=""
REMOTE_INSTALL=""
NODE_LIST=""
BUILD_NODE_LIST=""
LOCAL_DEP_NODE_LIST=""
DEPENDENT_NODE_LIST=""
DEP_ARRAY_NODE_LIST=""
BUILD_NODE="0"
NODE=""
BIN_NODE=""

#
# These variables contain the version of the MySQL and iClaustron
# These are constants, the scripts are generated anew for each
# new version of iClaustron
MYSQL_VERSION="mysql-5.1.22-ndb-6.3.5"
ICLAUSTRON_VERSION="iclaustron-0.0.1"

# This variable defines whether this script is used to install binaries
# or whether it is a pure source build. SOURCE_BUILD="n" for binary
# distributions. A source build script can generate a binary installation
# archive and will then install this using this generated archive and
# by invoking this script with --install it will be converted to a binary
# installation script.
SOURCE_BUILD="y"

msg_to_log()
{
  echo "$*" >> $LOG_FILE
}

output_msg()
{
  echo "$*"
  msg_to_log $*
}

create_dir()
{
  if ! test -d $CREATE_DIR ; then
    mkdir -p $CREATE_DIR
    if ! test -d $CREATE_DIR ; then
      output_msg "Failed to create $CREATE_DIR"
      exit 1
    fi
    output_msg "Successfully created $CREATE_DIR"
  fi
}

exec_command()
{
  output_msg "Executing $*"
#  eval $* >> $LOG_FILE
  if test "x$?" != "x0" ; then
    output_msg "Failed command $*"
    exit 1
  fi
}

#
# Parameter1: Prompt ($*)
# Parameter2: Default answer when only return used (DEFAULT_VAR)
#
# Output parameter: INPUT_VAR
#
read_input()
{
  read -p "$*" INPUT_VAR
  if test -z "$INPUT_VAR" ; then
    INPUT_VAR=$DEFAULT_ANSWER
  fi
}

#
# Parameter1: Prompt ($*)
# Parameter2: 1 => Use Y as default, otherwise => Use N as default
# Sent in DEFAULT_ANSWER
#
# Output parameter: INPUT_VAR
#
read_yn()
{
  IC_PROMPT="$*"

  if test "x$DEFAULT_ANSWER" = "xy" ; then
    IC_PROMPT="$IC_PROMPT [Y/n]:"
  else
    IC_PROMPT="$IC_PROMPT [y/N]:"
  fi
  while [ 0 ] ; do
    read_input $IC_PROMPT
    INPUT_VAR=`echo $INPUT_VAR | sed 's/Y/y/g'`
    INPUT_VAR=`echo $INPUT_VAR | sed 's/N/n/g'`
    if test "x$INPUT_VAR" = "xy" ; then
      break
    fi
    if test "x$INPUT_VAR" = "xn" ; then
      break
    fi
  done
  msg_to_log "$IC_PROMPT $INPUT_VAR"
  if test "x$INPUT_VAR" = "xy" ; then
    return 1
  else
    return 0
  fi
}

check_node()
{
  ping -c 1 -t 5 $1 >> /dev/null
  if test "x$?" != "x0" ; then
    output_msg "Host $1 isn't available, error code $?"
    return 1
  fi
  return 0
}

get_debug_install()
{
  if test "x$SOURCE_BUILD" = "xy" ; then
    DEFAULT_ANSWER="n"
    read_yn "Would you like to build a debug version"
    if test "x$?" = "x1" ; then
      output_msg "Building debug version"
      WITH_DEBUG="--with-debug"
    else
      output_msg "Building production version"
    fi
  fi
  return 0
}

check_port()
{
  return 0
}

get_host_user_and_port()
{
  DEFAULT_ANSWER="no_host_given"
  read_input "Provide hostname: "
  if test "x$INPUT_VAR" != "no_host_given" ; then
    check_node $INPUT_VAR
    if test "x$?" = "x0" ; then
      NODE=$INPUT_VAR
      DEFAULT_ANSWER=$USER
      read_input "What user should you use on this host ($USER): "
      NODE="${INPUT_VAR}@${NODE}"
      DEFAULT_ANSWER=22
      read_input "What SSH port should be used [22: Default SSH port] :"
      check_port $INPUT_VAR
      if test "x$?" = "x0" ; then
        NODE="${NODE}:${INPUT_VAR}"
        output_msg "Added host $NODE"
        return 0
      fi
    fi
  fi
  return 1
}

get_dependent_node_install()
{
  DEPENDENT_NODE_LIST=""
  if test "x$SOURCE_BUILD" = "xy" ; then
    output_msg "Add set of nodes where the binaries built on the previous node"
    output_msg "will be distributed to"
    while [ 0 ] ; do
      DEFAULT_ANSWER="y"
      read_yn "Would you like to distribute the binaries to more nodes"
      if test "x$INPUT_VAR" = "xy" ; then
        get_host_user_and_port
        if test "x$?" = "x0" ; then
          DEPENDENT_NODE_LIST="$DEPENDENT_NODE_LIST $NODE"
        fi
      else
        break
      fi
    done
  fi
  return 0
}

get_node_install()
{
  DEFAULT_ANSWER="y"
  if test "x$SOURCE_BUILD" = "xy" ; then

    READ_YN_LOCAL_MACHINE="Would you like to build source on this machine"
    READ_YN_REMOTE_MACHINE="Would you like to build on another node"
  else
    READ_YN_LOCAL_MACHINE="Would you like to install on this machine"
    READ_YN_REMOTE_MACHINE="Would you like to add another node to install at"
  fi
  read_yn $READ_YN_LOCAL_MACHINE
  if test "x$INPUT_VAR" = "xy" ; then
    if test "x$SOURCE_BUILD" = "xy" ; then
      get_dependent_node_install
      LOCAL_DEP_NODE_LIST="$DEPENDENT_NODE_LIST"
    fi
    LOCAL_INSTALL="y"
  fi
  while [ 0 ] ; do
    DEFAULT_ANSWER="y"
    read_yn $READ_YN_REMOTE_MACHINE
    if test "x$INPUT_VAR" = "xy" ; then
      get_host_user_and_port
      if test "x$?" = "x0" ; then
        REMOTE_INSTALL="y"
        NODE_LIST="$NODE_LIST $NODE"
        if test "x$SOURCE_BUILD" = "xy" ; then
          BUILD_NODE_LIST="$BUILD_NODE_LIST $INPUT_VAR"
          get_dependent_node_install
          DEP_ARRAY_NODE_LIST[BUILD_NODE]=$DEPENDENT_NODE_LIST
          ((BUILD_NODE+=1))
        fi
      fi
    else
      break
    fi
  done
}

build_local()
{
  CREATE_DIR=$BIN_INSTALL_DIR
  create_dir

  CREATE_DIR=${BIN_INSTALL_DIR}/${MYSQL_VERSION}
  MYSQL_INSTALL_DIR=$CREATE_DIR
  create_dir

  CREATE_DIR=${BIN_INSTALL_DIR}/${ICLAUSTRON_VERSION}
  ICLAUSTRON_INSTALL_DIR=$CREATE_DIR
  create_dir

  output_msg "Building iClaustron"
  exec_command cd $ICLAUSTRON_VERSION
  exec_command ./bootstrap.sh
  exec_command ./configure --with-production \
               --with-readline --prefix=$ICLAUSTRON_INSTALL_DIR $WITH_DEBUG
  exec_command make
  exec_command make install
  exec_command cd ..

  output_msg "Building MySQL"
  exec_command cd $MYSQL_VERSION
  exec_command BUILD/build_mccge.sh --prefix=$MYSQL_INSTALL_DIR $WITH_DEBUG
  exec_command make install
  exec_command cd ..

  output_msg "Successfully built and installed $ICLAUSTRON_VERSION"
}

install_local()
{
  return 0
}

remote_copy_source()
{
  output_msg "Copying self-extracting archive of source to $1"
  return 0
}

remote_copy_binary()
{
  output_msg "Copying self-extracting archive of binaries to  $1"
  return 0
}

remote_copy_back_binary()
{
  output_msg "Copying binaries from  $1 to local tmp directory"
  return 0
}

remote_build_source()
{
  output_msg "Build binaries at $1"
  return 0
}

remote_install_binary()
{
  output_msg "Install binaries at $1"
  return 0
}

while test $# -gt 0
do
  case $1 in
  --local )
    LOCAL_ONLY="1"
    ;;
  --install )
    SOURCE_BUILD="n"
    ;;
  --with-debug )
    WITH_DEBUG="--with-debug"
    ;;
  -* )
    exit 1
    ;;
  * )
    exit 1
    ;;
  esac
  shift
done

PWD=`pwd`
DATE=`date`

if test "x$USER" = "xroot" ; then
  BIN_INSTALL_DIR="/var/lib/iclaustron"
  SRC_INSTALL_DIR="/usr/local/iclaustron"
  if test "x$SOURCE_BUILD" = "xy" ; then
    LOG_FILE="${SRC_INSTALL_DIR}"
  else
    LOG_FILE="${BIN_INSTALL_DIR}"
  fi
  LOG_FILE="${LOG_FILE}/iclaustron_installation.log"
else
  BIN_INSTALL_DIR="$HOME/iclaustron_install"
  LOG_FILE="${PWD}/iclaustron_installation.log"
  if test "x$LOCAL_ONLY" ; then
    SRC_INSTALL_DIR="${HOME}/icl_src_installation"
    LOG_FILE="${SRC_INSTALL_DIR}/iclaustron_installation.log"
  else
    SRC_INSTALL_DIR="$PWD"
    LOG_FILE="${PWD}/iclaustron_installation.log"
  fi
fi
echo "Message log created for iclaustron installation on $DATE" > $LOG_FILE
if test "x$SOURCE_BUILD" = "xy" ; then
  output_msg "Source will be installed at $SRC_INSTALL_DIR"
fi
output_msg "Binaries will be installed at $BIN_INSTALL_DIR"
output_msg "Log file is stored in $LOG_FILE"

if test "x$LOCAL_ONLY" = "x" ; then
  get_node_install
  get_debug_install
else
  LOCAL_INSTALL="y"
  REMOTE_INSTALL=""
fi

output_msg "LOCAL_INSTALL = " $LOCAL_INSTALL
output_msg "REMOTE_INSTALL = " $REMOTE_INSTALL

if test "x$LOCAL_INSTALL" = "xy" ; then
  if test "x$SOURCE_BUILD" = "xy" ; then
    build_local
    for NODE in $LOCAL_DEP_NODE_LIST
    do
      remote_install_binary $NODE
    done
  else
    install_local
  fi
fi

if test "x$REMOTE_INSTALL" = "xy" ; then
# Copy installation archive to all nodes listed and execute
# self-extracting archive on all local nodes
  output_msg "Remote Install started"
  for NODE in $NODE_LIST
  do
    if test "x$SOURCE_BUILD" = "xy" ; then
      remote_copy_source $NODE
      remote_build_source $NODE
      remote_copy_back_binary $NODE
      BUILD_NODE_LIST=$DEP_ARRAY_NODE_LIST[BUILD_NODE]
      for BIN_NODE in $BUILD_NODE_LIST
      do
        remote_install_binary $BIN_NODE
      done
      ((BUILD_NODE+=1))
    else
      remote_install_binary $NODE
    fi
  done
fi
