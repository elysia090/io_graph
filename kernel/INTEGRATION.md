# Linux Tree Integration Notes

The repository mirror follows Linux source layout slices. Copy into a Linux
tree:

- `kernel/include/uapi/linux/bpf_iograph.h` to
  `include/uapi/linux/bpf_iograph.h`;
- `kernel/include/uapi/linux/bpf_iograph.h` to
  `tools/include/uapi/linux/bpf_iograph.h` for selftests BPF builds;
- `kernel/bpf/iograph_map.c` to `kernel/bpf/iograph_map.c`;
- `kernel/bpf/iograph_kfunc.c` to `kernel/bpf/iograph_kfunc.c`;
- `kernel/bpf/iograph_internal.h` to `kernel/bpf/iograph_internal.h`;
- `kernel/selftests/bpf/progs/*` to `tools/testing/selftests/bpf/progs/`;
- `kernel/selftests/bpf/prog_tests/*` to
  `tools/testing/selftests/bpf/prog_tests/`.
- `kernel/selftests/bpf/benchs/*` to `tools/testing/selftests/bpf/benchs/`.

The reproducible overlay command is:

```sh
./scripts/apply_linux_overlay.sh /path/to/linux
```

Plumb the map type through:

- `include/uapi/linux/bpf.h` enum `bpf_map_type`;
- `include/linux/bpf_types.h` with
  `BPF_MAP_TYPE(BPF_MAP_TYPE_IOGRAPH, iograph_map_ops)`;
- `kernel/bpf/Makefile` for map and kfunc objects;
- `kernel/bpf/syscall.c` privileged map-create allow-list.

Minimal integration diff:

```diff
--- a/include/uapi/linux/bpf.h
+++ b/include/uapi/linux/bpf.h
@@
 	BPF_MAP_TYPE_ARENA,
+	BPF_MAP_TYPE_IOGRAPH,
 	__MAX_BPF_MAP_TYPE
 };
--- a/include/linux/bpf_types.h
+++ b/include/linux/bpf_types.h
@@
 BPF_MAP_TYPE(BPF_MAP_TYPE_RINGBUF, ringbuf_map_ops)
+BPF_MAP_TYPE(BPF_MAP_TYPE_IOGRAPH, iograph_map_ops)
--- a/kernel/bpf/Makefile
+++ b/kernel/bpf/Makefile
@@
 obj-$(CONFIG_BPF_SYSCALL) += lpm_trie.o
+obj-$(CONFIG_BPF_SYSCALL) += iograph_map.o iograph_kfunc.o
--- a/kernel/bpf/syscall.c
+++ b/kernel/bpf/syscall.c
@@
 	case BPF_MAP_TYPE_ARENA:
+	case BPF_MAP_TYPE_IOGRAPH:
 		if (!bpf_token_capable(token, CAP_BPF))
```

`bpf_iograph_run_action()` uses the kfunc memory-size annotation and is the
action-only prefilter fast path. `bpf_iograph_run()` adds the
uninitialized-result annotation for final-state observation. Keep those
prototypes in sync with the BPF declarations when the prototype is copied into
a Linux tree.

The selftests bench keeps its raw selector bytes in a writable BPF global. The
current verifier path rejects the same `__sz` kfunc memory pair when that input
is sourced from BPF `.rodata`.

The selftest exercises the first pre-ringbuf invariant: a DROP action returns
before `bpf_ringbuf_reserve()`.

## Bench Registration

Linux BPF bench sources live in `tools/testing/selftests/bpf/benchs/`, their
BPF programs live in `progs/`, and `bench.c` registers exported bench structs.
Add the io_graph pieces after copying the overlay:

```diff
--- a/tools/testing/selftests/bpf/Makefile
+++ b/tools/testing/selftests/bpf/Makefile
@@
 $(OUTPUT)/bench_strncmp.o: $(OUTPUT)/strncmp_bench.skel.h
+$(OUTPUT)/bench_iograph.o: $(OUTPUT)/iograph_bench.skel.h
@@
 		 $(OUTPUT)/bench_strncmp.o \
+		 $(OUTPUT)/bench_iograph.o \
--- a/tools/testing/selftests/bpf/bench.c
+++ b/tools/testing/selftests/bpf/bench.c
@@
 extern struct argp bench_strncmp_argp;
+extern struct argp bench_iograph_argp;
@@
 	{ &bench_strncmp_argp, 0, "bpf_strncmp helper benchmark", 0 },
+	{ &bench_iograph_argp, 0, "io_graph prefilter benchmark", 0 },
@@
extern const struct bench bench_strncmp_helper;
+extern const struct bench bench_iograph_prefilter;
+extern const struct bench bench_iograph_lpm_prefilter;
+extern const struct bench bench_iograph_decision;
+extern const struct bench bench_iograph_hook_floor;
+extern const struct bench bench_iograph_lpm_decision;
@@
 	&bench_strncmp_helper,
+	&bench_iograph_prefilter,
+	&bench_iograph_lpm_prefilter,
+	&bench_iograph_decision,
+	&bench_iograph_hook_floor,
+	&bench_iograph_lpm_decision,
```

Example after building the selftests bench binary:

```sh
./bench -w 1 -d 5 iograph-prefilter --blob policy.iog \
	--selector /drop/event --drop-action 1
./bench -w 1 -d 5 iograph-lpm-prefilter --prefixes prefixes.txt \
	--selector /drop/event --drop-action 1
./bench -w 1 -d 5 iograph-decision --blob policy.iog \
	--selector /drop/event
./bench -w 1 -d 5 iograph-hook-floor --selector /drop/event
./bench -w 1 -d 5 iograph-lpm-decision --prefixes prefixes.txt \
	--selector /drop/event
```
