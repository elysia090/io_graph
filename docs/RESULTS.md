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
| bench | Linux selftests bench source accepts compiled blobs and raw selectors; compact aliases make the current `run_action()` runtime explicit; LPM has full-key and bounded-copy rows | `kernel/selftests/bpf/benchs/bench_iograph.c` |
| pre-ringbuf path | BPF program calls `bpf_iograph_run_action()` before reserve | `bpf/prefilter_demo.bpf.c` |
| compact runtime | userspace and kernel map-publication single-child chain compression; refreshed kernel rows now beat the same-hook LPM baseline on matched prefix paths | `src/iog_compact.c`, `kernel/bpf/iograph_map.c`, `results/native/current.md` |

## Kernel Measurements

Kernel numbers are intentionally not copied from the userspace proof. The
current kernel-backed rows below are from a patched WSL kernel with
`BPF_MAP_TYPE_IOGRAPH` enabled and `bpf_iograph_run_action()` routed through
the publication-time compact runtime graph:

| prefixes | selftests bench case | operations_M/s | throughput_ns/op | ringbuf on reject |
|---:|:---|---:|---:|:---|
| floor | empty same-hook raw tracepoint BPF row | 8.618 +/- 0.185 | 116.04 | n/a |
| 100 | compact matched decision | 6.086 +/- 0.050 | 164.31 | n/a |
| 1000 | compact early-miss decision | 7.683 +/- 0.127 | 130.16 | n/a |
| 1000 | compact matched decision | 5.695 +/- 0.131 | 175.59 | n/a |
| 1000 | compact matched DROP | 5.772 +/- 0.028 | 173.25 | 0 B |
| 1000 | shared-prefix compact matched decision | 5.725 +/- 0.060 | 174.67 | n/a |
| 1000 | long-path compact matched decision | 5.724 +/- 0.074 | 174.70 | n/a |

These rows include syscall trigger, tracepoint dispatch, BPF program execution,
and graph rows include the map/kfunc walk. The same-hook floor shows why an
early miss is dominated by attach/trigger/BPF dispatch rather than traversal.
The detailed run shape, environment metadata, and same-bench LPM trie rows live
in `results/native/current.md`. The userspace primitive now emits batch
p95/p99/p999 rows, 10000-prefix memory/update rows, compact runtime rows, and
IO-aware pre-emission accounting in `results/userspace/current.md`.

The userspace BPF-shaped shim now follows the kernel publication shape: map
update verifies the source blob, inlines action codes, builds the compact
runtime graph, and `run_action()` walks compact data. In that shape, typical
100-prefix hits measured at 29.47 ns and typical 1000-prefix hits at 41.33 ns
in the same WSL userspace environment. `current.md` is the stable result
entrypoint for these rows.

The latest compact hot-path implementation stores only literal tail bytes,
allows final-action terminal leaves to return from the incoming compact edge,
and prunes non-entry terminal final-action leaves from the compact node array.
That reduces typical 1000-prefix compact runtime memory to 39,177 B and
typical 10000-prefix compact runtime memory to 164,799 B. The kernel table
above is the latest booted-kernel measurement from the patched WSL kernel.

IO-aware pre-emission accounting is now folded into the userspace current
snapshot. It records `max_probe_len`, selector-copy-plus-decision rows,
discard-after-reserve baselines, action-only map memory, update scratch/peak
bytes, and dirty-cacheline accounting. Typical 1000-prefix active memory is
385,577 B with the retained blob and 39,329 B in action-only mode, while a
300 B early reject reserves 0 B with drop-before-reserve versus 344 B with
discard-after-reserve.

Current kernel evidence:

| Validation | Current status | Evidence |
|:---|:---|:---|
| Linux source overlay | map, kfunc, UAPI, selftest, and bench slices copy into Linux source layout | `scripts/apply_linux_overlay.sh` |
| kernel object build | `kernel/bpf/iograph_map.o` and `kernel/bpf/iograph_kfunc.o` compile in a Linux tree with the integration patch applied | `results/native/current.md` |
| selftests bench path | patched Linux selftests bench loads the same-hook floor, 100/1000 io_graph decision and DROP rows, full-key and bounded-copy LPM rows, and POST payload rows | `results/native/current.md` |
| PMU gating | the observed WSL runtime exposes no CPU PMU device, so branch/cache counters must be collected on a PMU-visible native host or VM | `results/native/current.md` |

Same-hook LPM comparison from the compact kernel run:

| case | compact ns/op | LPM full-key ns/op | LPM bounded-copy ns/op | compact vs bounded |
|:---|---:|---:|---:|---:|
| 100 typical hit | 164.31 | 212.18 | 184.95 | 1.13x |
| 1000 typical hit | 175.59 | 250.69 | 212.40 | 1.21x |
| 1000 typical prefilter DROP | 173.25 | 254.65 | 209.29 | 1.21x |
| 1000 shared-prefix hit | 174.67 | 319.69 | 304.14 | 1.74x |
| 1000 long-path hit | 174.70 | 516.00 | 505.31 | 2.89x |

POST payload rows from the same patched kernel with one producer and one
ringbuf consumer:

| payload_B | compact POST ns/op | always POST ns/op |
|---:|---:|---:|
| 300 | 462.11 | 387.45 |
| 800 | 603.14 | 553.40 |
| 2048 | 1013.17 | 976.56 |

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
