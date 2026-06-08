CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -Wmissing-declarations -g $(SAN) -I../../src -pthread -MMD
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I../../ext/include -pthread -MMD

OBJ := swim_timer_test.o swim_membership_test.o swim_gossip_queue_test.o \
       swim_codec_test.o swim_udp_test.o swim_protocol_test.o swim_feed_test.o \
       swim_timer.o swim_errno.o swim_node_id.o swim_membership.o \
       swim_gossip_queue.o swim_codec.o swim_udp.o swim_protocol.o swim_feed.o

all: unit_test

unit_test: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm -pthread

swim_timer_test.o: swim_timer_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_membership_test.o: swim_membership_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_gossip_queue_test.o: swim_gossip_queue_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_codec_test.o: swim_codec_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_udp_test.o: swim_udp_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_protocol_test.o: swim_protocol_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_feed_test.o: swim_feed_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

swim_timer.o: ../../src/swim_timer.c
	$(CC) $(CFLAGS) -c $<

swim_errno.o: ../../src/swim_errno.c
	$(CC) $(CFLAGS) -c $<

swim_node_id.o: ../../src/swim_node_id.c
	$(CC) $(CFLAGS) -c $<

swim_membership.o: ../../src/swim_membership.c
	$(CC) $(CFLAGS) -c $<

swim_gossip_queue.o: ../../src/swim_gossip_queue.c
	$(CC) $(CFLAGS) -c $<

swim_codec.o: ../../src/swim_codec.c
	$(CC) $(CFLAGS) -c $<

swim_udp.o: ../../src/swim_udp.c
	$(CC) $(CFLAGS) -c $<

swim_protocol.o: ../../src/swim_protocol.c
	$(CC) $(CFLAGS) -c $<

swim_feed.o: ../../src/swim_feed.c
	$(CC) $(CFLAGS) -c $<

-include $(OBJ:%.o=%.d)

test: unit_test
	./unit_test

clean:
	rm -f *.o *.d unit_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
