CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -Wmissing-declarations -g $(SAN) -I../../src -pthread
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I../../ext/include -pthread

all: unit_test

unit_test: swim_timer_test.o swim_membership_test.o swim_gossip_queue_test.o swim_codec_test.o swim_udp_test.o swim_protocol_test.o swim_feed_test.o swim_timer.o swim_errno.o swim_node_id.o swim_membership.o swim_gossip_queue.o swim_codec.o swim_udp.o swim_protocol.o swim_feed.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm -pthread

swim_timer_test.o: swim_timer_test.cpp ../../ext/include/doctest.h ../../src/swim_timer.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_membership_test.o: swim_membership_test.cpp ../../ext/include/doctest.h ../../src/swim_membership.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_gossip_queue_test.o: swim_gossip_queue_test.cpp ../../ext/include/doctest.h ../../src/swim_gossip_queue.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_codec_test.o: swim_codec_test.cpp ../../ext/include/doctest.h ../../src/swim_codec.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_udp_test.o: swim_udp_test.cpp ../../ext/include/doctest.h ../../src/swim_udp.h ../../src/swim_errno.h ../../src/swim_node_id.h
	$(CXX) $(CXXFLAGS) -c $<

swim_protocol_test.o: swim_protocol_test.cpp ../../ext/include/doctest.h ../../src/swim_protocol.h ../../src/swim_protocol_internal.h ../../src/swim_errno.h ../../src/swim_node_id.h
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

swim_udp.o: ../../src/swim_udp.c ../../src/swim_udp.h
	$(CC) $(CFLAGS) -c ../../src/swim_udp.c

swim_protocol.o: ../../src/swim_protocol.c ../../src/swim_protocol.h ../../src/swim_protocol_internal.h
	$(CC) $(CFLAGS) -c ../../src/swim_protocol.c

swim_feed_test.o: swim_feed_test.cpp ../../ext/include/doctest.h ../../src/swim_feed.h ../../src/swim_errno.h
	$(CXX) $(CXXFLAGS) -c $<

swim_feed.o: ../../src/swim_feed.c ../../src/swim_feed.h
	$(CC) $(CFLAGS) -c ../../src/swim_feed.c


test: unit_test
	./unit_test

clean:
	rm -f *.o unit_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
