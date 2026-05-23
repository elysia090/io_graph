ROOT := ../..
CC ?= cc
CPPFLAGS += -I$(ROOT)/include -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror

.PHONY: all clean

LIB_SRCS := \
	$(ROOT)/src/iog_blob.c \
	$(ROOT)/src/iog_compact.c \
	$(ROOT)/src/iog_compile.c \
	$(ROOT)/src/iog_graph.c \
	$(ROOT)/src/iog_interp.c \
	$(ROOT)/src/iog_map.c \
	$(ROOT)/src/iog_verify.c

all: iogc

iogc: iogc.o $(LIB_SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ iogc.o $(LIB_SRCS)

iogc.o: iogc.c $(ROOT)/include/iog/compile.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f iogc iogc.o
