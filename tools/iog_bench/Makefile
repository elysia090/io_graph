CC ?= cc

ROOT := ../..

CPPFLAGS += -I$(ROOT)/include -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
LDLIBS ?=

LIB_SRCS := \
	$(ROOT)/src/iog_blob.c \
	$(ROOT)/src/iog_compile.c \
	$(ROOT)/src/iog_graph.c \
	$(ROOT)/src/iog_interp.c \
	$(ROOT)/src/iog_map.c \
	$(ROOT)/src/iog_verify.c
LIB_OBJS := $(notdir $(LIB_SRCS:.c=.o))
OBJS := bench.o baselines.o datasets.o pre_ringbuf_model.o $(LIB_OBJS)

.PHONY: all clean validate-native

all: iog_bench

iog_bench: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(LIB_OBJS): %.o: $(ROOT)/src/%.c $(ROOT)/src/iog_internal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

bench.o: bench.c bench_internal.h $(ROOT)/include/iog/bench.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

baselines.o datasets.o pre_ringbuf_model.o: %.o: %.c bench_internal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f iog_bench $(OBJS) *.d

validate-native:
	sh ./validate_native.sh

-include $(OBJS:.o=.d)
