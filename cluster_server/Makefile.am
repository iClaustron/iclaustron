##
## This is the template to produce Makefile.in for this directory
## This directory is used to build the iClaustron Cluster Server
##
INCLUDES = -DG_LOG_DOMAIN=\"iClaustron_Cluster_Server\"
bin_PROGRAMS = ic_cs
ic_cs_SOURCES = ic_cs.c
ic_cs_LDADD = $(LDADD) ../comm/libic_comm.la \
                          ../port/libic_port.la \
                          ../util/libic_util.la \
                          ../api_clusterserver/libic_apic.la

