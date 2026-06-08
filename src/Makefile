HFILES := swim_timer.h swim_membership.h swim_gossip_queue.h swim_codec.h swim_udp.h swim.h swim_protocol_internal.h swim_feed.h
CFILES := swim_timer.c swim_errno.c swim_node_id.c swim_membership.c swim_gossip_queue.c swim_codec.c swim_udp.c swim_main.c swim_feed.c
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
CFLAGS := -std=c17 -Wmissing-declarations -Wall -Wextra -MMD -pthread

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

libswimc17.a: $(OBJ)
	ar -rcs $@ $^

$(LIB_SHARED): $(OBJ)
	$(CC) -shared -pthread -o $@ $^ -lm

-include $(OBJ:%.o=%.d) $(EXEC:%=%.d)

test: all

clean:
	rm -f *.o *.d *.a *.so.* $(EXEC)

distclean: clean

format:
	clang-format -i *.[ch] 


.PHONY: all clean distclean format

