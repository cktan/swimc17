include ../Makefile.global

all:
	$(MAKE) -C unit all
	$(MAKE) -C stress all

test: all
	$(MAKE) -C unit test
	$(MAKE) -C stress test

clean:
	$(MAKE) -C unit clean
	$(MAKE) -C stress clean

distclean: clean

format:
	$(MAKE) -C unit format
	$(MAKE) -C stress format

.PHONY: all clean distclean format test

