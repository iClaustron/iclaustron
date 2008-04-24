## This file is used to produce the Makefile.in for this directory
## This directory is used to build iClaustron Communication Functions
##

DTRACE= @DTRACE@
DTRACEFLAGS= @DTRACEFLAGS@
DTRACEFILES= socket_conn.o

lib_LTLIBRARIES = libic_comm.la

libic_comm_la_SOURCES = socket_conn.c socket_membuf.c comm_supp.c

if HAVE_DTRACE
  libic_comm_la_SOURCES += probes.d
endif

probes.h: probes.d
	$(DTRACE) $(DTRACEFLAGS) -s probe.d

SUFFIXES : .d

.d.o : $(DTRACEFILES)
	$(DTRACE) $(DTRACEFLAGS) -G -s $< $(DTRACEFILES)
