HFILES := swim_timer.h
CFILES := swim_timer.c
OBJ := $(CFILES:.c=.o)

# set up LIB_SHARED
LIB_VERSION := 1.0
LIB := libswimc17.a
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    LIB_SHARED =
else ifeq ($(OS), Windows_NT)
    LIB_SHARED =
else
    LIB_SHARED = libswimc17.so.$(LIB_VERSION)
endif
EXEC =

# set up CFLAGS / CXXFLAGS
CFLAGS := -std=c17 -Wmissing-declarations -Wall -Wextra -MMD

ifdef DEBUG
    CFLAGS += -O0 -g -fsanitize=address,undefined
else
    CFLAGS += -O3 -DNDEBUG
endif
# Add -fpic only for non-Windows platforms
ifneq ($(OS), Windows_NT)
    CFLAGS += -fpic
endif


all: $(EXEC) $(LIB) $(LIB_SHARED) 

*.o: $(HFILES)

libswimc17.a: swim_timer.o
	ar -rcs $@ $^

$(LIB_SHARED): swim_timer.o
	$(CC) -shared -o $@ $^

-include $(OBJ:%.o=%.d) $(EXEC:%=%.d)

test: all

clean:
	rm -f *.o *.d *.a *.so.* $(EXEC)

distclean: clean

format:
	clang-format -i *.[ch] *.hpp


.PHONY: all clean distclean format

