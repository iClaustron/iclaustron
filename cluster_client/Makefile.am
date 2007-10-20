#
# This is the template to produce Makefile.in for this directory
# This directory is used to build the iClaustron Command Client
#

bin_PROGRAMS = ic_cclient
ic_cclient_SOURCES = ic_cclient.c
ic_cclient_LDADD = $(LDADD) ../comm/libic_comm.la \
                            ../util/libic_util.la \
                            ../port/libic_port.la \
                            ../api_clusterserver/libic_apic.la
