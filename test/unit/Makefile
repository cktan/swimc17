include ../../Makefile.global

CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -Wmissing-declarations -g $(SAN) -I../../src -pthread -MMD
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I$(HOME)/.local/include -pthread -MMD

LIBSWIMC17 := ../../src/libswimc17.a

OBJ := swim_timer_test.o swim_membership_test.o swim_gossip_queue_test.o \
       swim_codec_test.o swim_udp_test.o swim_protocol_test.o swim_feed_test.o \
       swim_node_id_test.o

all: $(DOCTEST_H) unit_test

unit_test: $(OBJ) $(LIBSWIMC17)
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

swim_node_id_test.o: swim_node_id_test.cpp
	$(CXX) $(CXXFLAGS) -c $<

-include $(OBJ:%.o=%.d)

test: unit_test
	./unit_test --duration

clean:
	rm -f *.o *.d unit_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
