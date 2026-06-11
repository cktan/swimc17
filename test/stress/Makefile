CC  ?= cc
CXX ?= c++

SAN      := -fsanitize=address,undefined
CFLAGS   := -std=c17   -Wall -Wextra -g $(SAN) -I../../src -pthread -MMD
CXXFLAGS := -std=c++17 -Wall -Wextra -g $(SAN) -I../../src -I../../ext/include -pthread -MMD

LIBSWIMC17 := ../../src/libswimc17.a

OBJ := scale.o

all: stress_test

stress_test: $(OBJ) $(LIBSWIMC17)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm -pthread

scale.o: scale.cpp
	$(CXX) $(CXXFLAGS) -DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN -c $<

-include $(OBJ:.o=.d)

test: stress_test
	./stress_test --duration

clean:
	rm -f *.o *.d stress_test

format:
	clang-format -i *.cpp

.PHONY: all test clean format
