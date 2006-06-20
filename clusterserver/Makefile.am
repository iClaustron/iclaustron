##
## This is the template to produce Makefile.in for this directory
## This directory is used to build the iClaustron Cluster Server
##

BUILT_SOURCES = ic_parser.h
AM_YFLAGS = -d
bin_PROGRAMS = ic_cserv
ic_cserv_SOURCES = ic_cserv.c ic_parser.y ic_lex.l
ic_cserv_LDADD = $(LDADD) ../comm/libic_comm.la \
                          ../port/libic_port.la

