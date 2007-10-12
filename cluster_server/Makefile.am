##
## This is the template to produce Makefile.in for this directory
## This directory is used to build the iClaustron Configuration Server
##
INCLUDES = -DG_LOG_DOMAIN=\"iClaustron_Config_Server\"
bin_PROGRAMS = ic_conf
ic_conf_SOURCES = ic_conf.c
ic_conf_LDADD = $(LDADD) ../comm/libic_comm.la \
                          ../port/libic_port.la \
                          ../util/libic_util.la \
                          ../api_clusterserver/libic_apic.la

