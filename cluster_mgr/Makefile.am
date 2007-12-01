##
## This is the template to produce Makefile.in for this directory
## This directory is used to build the iClaustron Cluster Manager
##

BUILT_SOURCES          = ic_parser.h
bin_PROGRAMS           = ic_clmgr
ic_clmgr_SOURCES       = ic_clmgr.c ic_parser.y ic_lex.l
ic_clmgr_LDADD = $(LDADD) ../comm/libic_comm.la \
                          ../util/libic_util.la \
                          ../port/libic_port.la \
                          ../api_clusterserver/libic_apic.la
AM_YFLAGS              = -d --verbose

