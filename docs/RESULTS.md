# Results

The first proof milestone is complete. Its frozen userspace evidence now lives
in `results/userspace/current.md`.

The active result track is the kernel prototype:

| Track | Current status | Evidence |
|:---|:---|:---|
| map type | source split into map and kfunc units with RCU whole-graph replacement | `kernel/bpf/` |
| blob verifier | map update rejects bad layout, bounds, default chains, count caps, and blob-size caps | `kernel/bpf/iograph_map.c` |
| interpreter | required non-JIT execution path with sparse edge walk | `kernel/bpf/iograph_kfunc.c` |
| kfunc API | action-only prefilter kfunc plus run/step observation paths, verifier annotations, tracing-hook registration | `kernel/bpf/iograph_kfunc.c` |
| selftest | bad blob update rejection plus DROP-before-ringbuf-reserve path | `kernel/selftests/bpf/` |
| bench | Linux selftests bench source accepts compiled blobs and raw selectors | `kernel/selftests/bpf/benchs/bench_iograph.c` |
| pre-ringbuf path | BPF program calls `bpf_iograph_run_action()` before reserve | `bpf/prefilter_demo.bpf.c` |

## Kernel Measurements

Kernel numbers are intentionally not copied from the userspace proof. The
current kernel-backed rows come from a patched WSL kernel with
`BPF_MAP_TYPE_IOGRAPH` and the action-only kfunc path enabled:

| prefixes | selftests bench case | operations_M/s | throughput_ns/op | ringbuf on reject |
|---:|:---|---:|---:|:---|
| floor | empty same-hook BPF row | 8.351 +/- 0.126 | 119.7 | n/a |
| 100 | early-miss action decision | 7.513 +/- 0.089 | 133.1 | n/a |
| 100 | matched prefix DROP | 2.853 +/- 0.009 | 350.5 | 0 B |
| 1000 | early-miss action decision | 7.465 +/- 0.072 | 134.0 | n/a |
| 1000 | matched prefix DROP | 2.846 +/- 0.007 | 351.4 | 0 B |

These rows include syscall trigger, tracepoint dispatch, BPF program execution,
and graph rows include the map/kfunc walk. The same-hook floor shows why an
early miss is dominated by attach/trigger/BPF dispatch rather than traversal.
The detailed run shape, userspace comparison, and same-bench LPM trie rows live
in `results/native/current.md`. The userspace primitive now emits batch
p95/p99/p999 rows and 10000-prefix memory/update rows in
`results/userspace/current.md`.

Current kernel evidence:

| Validation | Current status | Evidence |
|:---|:---|:---|
| Linux source overlay | map, kfunc, UAPI, selftest, and bench slices copy into Linux source layout | `scripts/apply_linux_overlay.sh` |
| kernel object build | `kernel/bpf/iograph_map.o` and `kernel/bpf/iograph_kfunc.o` compile in a Linux tree with the integration patch applied | `results/native/current.md` |
| selftests bench path | patched Linux selftests bench loads the same-hook floor, 100/1000 io_graph decision and DROP rows, and 100/1000 LPM decision comparisons | `results/native/current.md` |
| PMU gating | the observed WSL runtime exposes no CPU PMU device, so branch/cache counters must be collected on a PMU-visible native host or VM | `results/native/current.md` |

Required kernel matrix:

- map create/update/delete verification results;
- invalid blob rejection from `BPF_MAP_UPDATE_ELEM`;
- `bpf_iograph_run` interpreter latency from a BPF benchmark program;
- branch, cache, L1, and LLC miss counters when `perf_event_open` or
  `perf stat` can observe the native PMU;
- BPF verifier load time and BPF JIT time for baselines;
- update latency without BPF program reload;
- ringbuf bytes emitted and reserve/drop counts for the prefilter demo;
- `perf stat` or `perf_event_open` branch/cache counters when the native PMU
  exposes them.

The template and invocation notes belong in `results/native/README.md`.
