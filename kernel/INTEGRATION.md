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

The overlay target must be a disposable clean Linux worktree, not a pristine
base clone. The script refuses a dirty tree, checks the integration patch
before copying files, applies the patch after copying the overlay slices, and
runs `git diff --check` at the end.

The reproducible overlay command is:

```sh
./scripts/apply_linux_overlay.sh /path/to/linux
```

Recommended layout for WSL kernel measurements:

```text
~/src/io_graph              io_graph repo
~/src/wsl2-linux-base       pristine Microsoft WSL2-Linux-Kernel clone
~/src/wsl2-linux-iograph    disposable overlay worktree
~/build/wsl2-iograph        optional O= build output
/mnt/c/.../wsl-kernels/io_graph  bzImage, modules.vhdx, config snippets
```

Commit the overlay inside the disposable Linux worktree as a measurement
checkpoint before building. That commit is for reproducibility, not an upstream
submission.

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

## WSL Custom Kernel Notes

On WSL2, `.wslconfig` is a global VM configuration. A custom kernel configured
there affects all WSL2 distributions, including NixOS and Ubuntu. Do not have
repo scripts rewrite `%UserProfile%\.wslconfig` automatically. Generate or
store snippets, copy them explicitly for a measurement run, then restore stock
WSL configuration afterward.

The `kernel=` and `kernelModules=` values in `.wslconfig` are Windows absolute
paths, not `/mnt/c/...` paths:

```ini
[wsl2]
kernel=C:\\path\\to\\wsl-kernels\\io_graph\\bzImage
kernelModules=C:\\path\\to\\wsl-kernels\\io_graph\\modules.vhdx
memory=8GB
processors=4
```

After changing `.wslconfig`, restart the WSL VM with `wsl --shutdown`, then
verify the booted kernel with `uname -a` and `/proc/version` before recording
any benchmark row.

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
+extern const struct bench bench_iograph_compact_prefilter;
+extern const struct bench bench_iograph_lpm_prefilter;
+extern const struct bench bench_iograph_decision;
+extern const struct bench bench_iograph_compact_decision;
+extern const struct bench bench_iograph_hook_floor;
+extern const struct bench bench_iograph_lpm_decision;
@@
 	&bench_strncmp_helper,
+	&bench_iograph_prefilter,
+	&bench_iograph_compact_prefilter,
+	&bench_iograph_lpm_prefilter,
+	&bench_iograph_decision,
+	&bench_iograph_compact_decision,
+	&bench_iograph_hook_floor,
+	&bench_iograph_lpm_decision,
```

Example after building the selftests bench binary:

```sh
./bench -w 1 -d 5 iograph-prefilter --blob policy.iog \
	--selector /drop/event --drop-action 1
./bench -w 1 -d 5 iograph-compact-prefilter --blob policy.iog \
	--selector /drop/event --drop-action 1
./bench -w 1 -d 5 iograph-lpm-prefilter --prefixes prefixes.txt \
	--selector /drop/event --drop-action 1
./bench -w 1 -d 5 iograph-decision --blob policy.iog \
	--selector /drop/event
./bench -w 1 -d 5 iograph-compact-decision --blob policy.iog \
	--selector /drop/event
./bench -w 1 -d 5 iograph-hook-floor --selector /drop/event
./bench -w 1 -d 5 iograph-lpm-decision --prefixes prefixes.txt \
	--selector /drop/event
```

After compact publication, `iograph-decision` and
`iograph-compact-decision` both call the current `bpf_iograph_run_action()`
runtime. The compact-named rows are aliases that make the current runtime
explicit in result tables; they are not a same-build byte-trie versus compact
A/B comparison. The old byte-trie matched rows are historical snapshots unless
a separate debug kfunc or map flag is added for benchmarking.
