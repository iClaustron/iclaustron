# The C examples. Against an installed libiclaustron (cargo xtask
# install), found by pkg-config:
#
#   make
#
# or against the tree's own build (cargo build --release -p ic_capi):
#
#   make IC_TREE=../..
#
# One program alone is one line, e.g.
#
#   cc -o describe describe.c $(pkg-config --cflags --libs iclaustron)

PROGRAMS = describe key_ops pipeline unique retry \
  scan range_scan scan_update scan_executor \
  conditional_update auto_increment

CFLAGS ?= -O2
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra
# scan_executor's threads.
CFLAGS += -pthread

ifdef IC_TREE
TREE := $(abspath $(IC_TREE))
CFLAGS += -I$(TREE)/include
LDLIBS += -L$(TREE)/target/release -liclaustron
LDLIBS += -Wl,-rpath,$(TREE)/target/release
else
CFLAGS += $(shell pkg-config --cflags iclaustron)
LDLIBS += $(shell pkg-config --libs iclaustron)
LDLIBS += -Wl,-rpath,$(shell pkg-config --variable=libdir iclaustron)
endif

all: $(PROGRAMS)

$(PROGRAMS): %: %.c t1.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(PROGRAMS)

.PHONY: all clean
