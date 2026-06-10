CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -g $(SAN) -I../../src -pthread -MMD
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I../../ext/include -pthread -MMD

OBJ := scale.o \
       swim_timer.o swim_errno.o swim_node_id.o swim_membership.o \
       swim_gossip_queue.o swim_codec.o swim_udp.o swim_main.o swim_feed.o

all: stress_test

stress_test: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm -pthread

scale.o: scale.cpp
	$(CXX) $(CXXFLAGS) -DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN -c $<

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

swim_main.o: ../../src/swim_main.c
	$(CC) $(CFLAGS) -c $<

swim_feed.o: ../../src/swim_feed.c
	$(CC) $(CFLAGS) -c $<

-include $(OBJ:.o=.d)

test: stress_test
	./stress_test

clean:
	rm -f *.o *.d stress_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
