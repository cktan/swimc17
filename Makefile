.NOTPARALLEL:

prefix ?= /usr/local
# remove trailing /
override prefix := $(prefix:%/=%)
DIRS := src test

BUILDDIRS := $(DIRS:%=build-%)
CLEANDIRS := $(DIRS:%=clean-%)
FORMATDIRS := $(DIRS:%=format-%)
TESTDIRS := $(DIRS:%=test-%)

###################################

# Define PCFILE content based on prefix
define PCFILE
prefix=${prefix}
Name: libswimc17
URL: https://github.com/cktan/swimc17/
Description: SWIM C library in c17.
Version: v1.0
Libs: -L$${prefix}/lib -lswimc17
Cflags: -I$${prefix}/include
endef

# Make it available to subshells
export PCFILE

#################################

all: $(BUILDDIRS)

$(BUILDDIRS):
	$(MAKE) -C $(@:build-%=%)

install: all
	install -d $(DESTDIR)${prefix}/include
	install -d $(DESTDIR)${prefix}/lib
	install -d $(DESTDIR)${prefix}/lib/pkgconfig
	install -m 0644 src/swim_timer.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/swim_errno.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/swim_node_id.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/swim_membership.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/swim_gossip_queue.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/swim_codec.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/swim_udp.h $(DESTDIR)${prefix}/include/
	install -m 0644 src/libswimc17.a $(DESTDIR)${prefix}/lib/
	@echo "$$PCFILE" >> $(DESTDIR)${prefix}/lib/pkgconfig/libswimc17.pc

test: $(TESTDIRS)

format: $(FORMATDIRS)

clean: $(CLEANDIRS)

$(TESTDIRS):
	$(MAKE) -C $(@:test-%=%) test

$(CLEANDIRS):
	$(MAKE) -C $(@:clean-%=%) clean

$(FORMATDIRS):
	$(MAKE) -C $(@:format-%=%) format

.PHONY: $(DIRS) $(BUILDDIRS) $(TESTDIRS) $(CLEANDIRS) $(FORMATDIRS)
.PHONY: all install test format clean
