include ../../Makefile.global

CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -g $(SAN) -I../../src -pthread -MMD
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I$(HOME)/.local/include -pthread -MMD

ifdef DEBUG
    CFLAGS += -O0 -g -fsanitize=address,undefined
    CXXFLAGS += -O0 -g -fsanitize=address,undefined
else
    CFLAGS += -O3 -DNDEBUG
    CXXFLAGS += -O3 -DNDEBUG
endif

LIBSWIMC17 := ../../src/libswimc17.a

OBJ := scale.o

ifdef DEBUG

all: stress_test

stress_test: $(OBJ) $(LIBSWIMC17)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm -pthread

scale.o: scale.cpp
	$(CXX) $(CXXFLAGS) -DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN -c $<

-include $(OBJ:.o=.d)

test: stress_test
	./stress_test --duration

else

all:
	@echo "stress test skipped (DEBUG not set)"

test:
	@echo "stress test skipped (DEBUG not set)"

endif

clean:
	rm -f *.o *.d stress_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
