all:
	$(MAKE) -C timer all

test: all
	$(MAKE) -C timer test

clean:
	$(MAKE) -C timer clean

distclean: clean

format:
	clang-format -i timer/*.[ch] timer/*.cpp

.PHONY: all clean distclean format test
