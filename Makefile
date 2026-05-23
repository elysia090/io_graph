CC ?= cc

.PHONY: all bench iogc test clean validate-native

all: bench iogc

bench:
	$(MAKE) -C tools/iog_bench all

iogc:
	$(MAKE) -C tools/iogc all

validate-native:
	$(MAKE) -C tools/iog_bench validate-native

test:
	$(MAKE) -C tests all run

clean:
	$(MAKE) -C tools/iog_bench clean
	$(MAKE) -C tools/iogc clean
	$(MAKE) -C tests clean
