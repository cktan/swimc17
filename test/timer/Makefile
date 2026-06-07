CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -Wmissing-declarations -g $(SAN) -I../../src
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src

all: timer_test

timer_test: swim_timer_test.o swim_timer.o
	$(CXX) $(CXXFLAGS) -o $@ $^

swim_timer_test.o: swim_timer_test.cpp doctest.h ../../src/swim_timer.h
	$(CXX) $(CXXFLAGS) -c $<

swim_timer.o: ../../src/swim_timer.c ../../src/swim_timer.h
	$(CC) $(CFLAGS) -c ../../src/swim_timer.c

test: timer_test
	./timer_test

clean:
	rm -f *.o timer_test

.PHONY: all test clean
