#
# This is the template to produce Makefile.in for this directory
# This directory is used to build the iClaustron Control Server
#

bin_PROGRAMS = ic_pcntrl
ic_pcntrl_SOURCES = ic_pcntrl.c
ic_pcntrl_LDADD = $(LDADD) \
                  ../api_dataserver/libic_apid.la \
                  ../api_clusterserver/libic_apic.la \
                  ../util/libic_util.la \
                  ../port/libic_port.la \
                  ../comm/libic_comm.la
