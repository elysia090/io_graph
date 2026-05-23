# Results

The first proof milestone is complete. Its frozen userspace evidence now lives
in `results/userspace/current.md`.

The active result track is the kernel prototype:

| Track | Current status | Evidence |
|:---|:---|:---|
| map type | source split into map and kfunc units with RCU whole-graph replacement | `kernel/bpf/` |
| blob verifier | map update rejects bad layout, bounds, unknown flags, count caps, and blob-size caps | `kernel/bpf/iograph_map.c` |
| interpreter | required non-JIT execution path; `run_action()` uses the compact runtime graph, `run()`/`step()` keep the byte-trie diagnostic paths | `kernel/bpf/iograph_kfunc.c` |
| kfunc API | action-only prefilter kfunc plus run/step observation paths; kfuncs hold short internal RCU read sections and are available to the raw-tracepoint bench path | `kernel/bpf/iograph_kfunc.c` |
| selftest | bad blob update rejection plus DROP-before-ringbuf-reserve path | `kernel/selftests/bpf/` |
| bench | Linux selftests bench source accepts compiled blobs and raw selectors; compact aliases make the current `run_action()` runtime explicit | `kernel/selftests/bpf/benchs/bench_iograph.c` |
| pre-ringbuf path | BPF program calls `bpf_iograph_run_action()` before reserve | `bpf/prefilter_demo.bpf.c` |
| compact runtime | userspace and kernel map-publication single-child chain compression; refreshed kernel rows now beat the same-hook LPM baseline on matched prefix paths | `src/iog_compact.c`, `kernel/bpf/iograph_map.c`, `results/native/compact-2026-05-23.md` |

## Kernel Measurements

Kernel numbers are intentionally not copied from the userspace proof. The
current kernel-backed rows below are from a patched WSL kernel with
`BPF_MAP_TYPE_IOGRAPH` enabled and `bpf_iograph_run_action()` routed through
the publication-time compact runtime graph:

| prefixes | selftests bench case | operations_M/s | throughput_ns/op | ringbuf on reject |
|---:|:---|---:|---:|:---|
| floor | empty same-hook raw tracepoint BPF row | 9.364 +/- 0.144 | 106.79 | n/a |
| 100 | compact matched decision | 6.426 +/- 0.014 | 155.62 | n/a |
| 1000 | compact early-miss decision | 8.091 +/- 0.181 | 123.59 | n/a |
| 1000 | compact matched decision | 5.910 +/- 0.060 | 169.20 | n/a |
| 1000 | compact matched DROP | 5.956 +/- 0.029 | 167.90 | 0 B |
| 1000 | shared-prefix compact matched decision | 6.256 +/- 0.035 | 159.85 | n/a |
| 1000 | long-path compact matched decision | 6.210 +/- 0.044 | 161.03 | n/a |

These rows include syscall trigger, tracepoint dispatch, BPF program execution,
and graph rows include the map/kfunc walk. The same-hook floor shows why an
early miss is dominated by attach/trigger/BPF dispatch rather than traversal.
The detailed run shape, environment metadata, and same-bench LPM trie rows live
in `results/native/current.md` and `results/native/compact-2026-05-23.md`. The
userspace primitive now emits batch
p95/p99/p999 rows, 10000-prefix memory/update rows, and compact-chain runtime
rows in `results/userspace/current.md`.
The current compact-runtime follow-up for the kernel publication shape is in
`results/userspace/compact-runtime-2026-05-23.md`; after aligning the
userspace BPF-shaped shim with the published compact graph, it shows typical
100-prefix hits at 35.35 ns and typical 1000-prefix hits at 59.84 ns in the
same WSL userspace environment. The older 33.32 ns / 45.44 ns rows in that
file are preserved as direct compact-primitive tail snapshots and bypass the
published map-object boundary.

Current kernel evidence:

| Validation | Current status | Evidence |
|:---|:---|:---|
| Linux source overlay | map, kfunc, UAPI, selftest, and bench slices copy into Linux source layout | `scripts/apply_linux_overlay.sh` |
| kernel object build | `kernel/bpf/iograph_map.o` and `kernel/bpf/iograph_kfunc.o` compile in a Linux tree with the integration patch applied | `results/native/current.md` |
| selftests bench path | patched Linux selftests bench loads the same-hook floor, 100/1000 io_graph decision and DROP rows, and 100/1000 LPM decision comparisons | `results/native/current.md` |
| PMU gating | the observed WSL runtime exposes no CPU PMU device, so branch/cache counters must be collected on a PMU-visible native host or VM | `results/native/current.md` |

Same-hook LPM comparison from the compact kernel run:

| case | compact ns/op | LPM ns/op | compact speedup |
|:---|---:|---:|---:|
| 100 typical hit | 155.62 | 228.99 | 1.47x |
| 1000 typical hit | 169.20 | 287.44 | 1.70x |
| 1000 typical prefilter DROP | 167.90 | 286.37 | 1.71x |
| 1000 shared-prefix hit | 159.85 | 476.64 | 2.98x |
| 1000 long-path hit | 161.03 | 791.77 | 4.92x |

Required kernel matrix:

- map create/update/delete verification results;
- invalid blob rejection from `BPF_MAP_UPDATE_ELEM`;
- `bpf_iograph_run` interpreter latency from a BPF benchmark program;
- POST-side payload materialization rows for 300 B, 800 B, and 2 KiB events;
- branch, cache, L1, and LLC miss counters when `perf_event_open` or
  `perf stat` can observe the native PMU;
- BPF verifier load time and BPF JIT time for baselines;
- update latency without BPF program reload;
- ringbuf bytes emitted and reserve/drop counts for the prefilter demo;
- `perf stat` or `perf_event_open` branch/cache counters when the native PMU
  exposes them.

The template and invocation notes belong in `results/native/README.md`.
