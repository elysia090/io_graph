ROOT := ..
CC ?= cc
CPPFLAGS += -I$(ROOT)/include -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror

TESTS := test_verify test_interp test_map test_negative_blobs
LIB_SRCS := \
	$(ROOT)/src/iog_blob.c \
	$(ROOT)/src/iog_compile.c \
	$(ROOT)/src/iog_graph.c \
	$(ROOT)/src/iog_interp.c \
	$(ROOT)/src/iog_map.c \
	$(ROOT)/src/iog_verify.c

.PHONY: all run clean

all: $(TESTS)

$(TESTS): %: %.c $(LIB_SRCS) $(ROOT)/include/iog/bench.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB_SRCS)

run: all
	./test_verify
	./test_interp
	./test_map
	./test_negative_blobs

clean:
	rm -f $(TESTS)
