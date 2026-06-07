CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -Wmissing-declarations -g $(SAN) -I../../src
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I../../ext/include

all: timer_test

timer_test: swim_timer_test.o swim_membership_test.o swim_gossip_queue_test.o swim_codec_test.o swim_timer.o swim_errno.o swim_node_id.o swim_membership.o swim_gossip_queue.o swim_codec.o
	$(CXX) $(CXXFLAGS) -o $@ $^

swim_timer_test.o: swim_timer_test.cpp ../../ext/include/doctest.h ../../src/swim_timer.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_membership_test.o: swim_membership_test.cpp ../../ext/include/doctest.h ../../src/swim_membership.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_gossip_queue_test.o: swim_gossip_queue_test.cpp ../../ext/include/doctest.h ../../src/swim_gossip_queue.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_codec_test.o: swim_codec_test.cpp ../../ext/include/doctest.h ../../src/swim_codec.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_timer.o: ../../src/swim_timer.c ../../src/swim_timer.h
	$(CC) $(CFLAGS) -c ../../src/swim_timer.c

swim_errno.o: ../../src/swim_errno.c ../../src/swim_errno.h
	$(CC) $(CFLAGS) -c ../../src/swim_errno.c

swim_node_id.o: ../../src/swim_node_id.c ../../src/swim_node_id.h
	$(CC) $(CFLAGS) -c ../../src/swim_node_id.c

swim_membership.o: ../../src/swim_membership.c ../../src/swim_membership.h
	$(CC) $(CFLAGS) -c ../../src/swim_membership.c

swim_gossip_queue.o: ../../src/swim_gossip_queue.c ../../src/swim_gossip_queue.h
	$(CC) $(CFLAGS) -c ../../src/swim_gossip_queue.c

swim_codec.o: ../../src/swim_codec.c ../../src/swim_codec.h
	$(CC) $(CFLAGS) -c ../../src/swim_codec.c

test: timer_test
	./timer_test

clean:
	rm -f *.o timer_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
