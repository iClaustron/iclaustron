#
# This is the template to produce Makefile.in for this directory
# This directory is used to build the iClaustron Replication Server
#

bin_PROGRAMS = ic_repserv
ic_repserv_SOURCES = ic_repserv.c
ic_repserv_LDADD = $(LDADD) ../api_dataserver/libic_apid.la \
                            ../api_clusterserver/libic_apic.la
