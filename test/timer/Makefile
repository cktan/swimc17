CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -Wmissing-declarations -g $(SAN) -I../../src
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I../../ext/include

all: timer_test

timer_test: swim_timer_test.o swim_timer.o swim_errno.o swim_node_id.o
	$(CXX) $(CXXFLAGS) -o $@ $^

swim_timer_test.o: swim_timer_test.cpp ../../ext/include/doctest.h ../../src/swim_timer.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_timer.o: ../../src/swim_timer.c ../../src/swim_timer.h
	$(CC) $(CFLAGS) -c ../../src/swim_timer.c

swim_errno.o: ../../src/swim_errno.c ../../src/swim_errno.h
	$(CC) $(CFLAGS) -c ../../src/swim_errno.c

swim_node_id.o: ../../src/swim_node_id.c ../../src/swim_node_id.h
	$(CC) $(CFLAGS) -c ../../src/swim_node_id.c

test: timer_test
	./timer_test

clean:
	rm -f *.o timer_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
