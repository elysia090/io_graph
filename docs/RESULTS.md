# Results

The first proof milestone is complete. Its frozen userspace evidence now lives
in `results/userspace/current.md`.

The active result track is the kernel prototype:

| Track | Current status | Evidence |
|:---|:---|:---|
| map type | source split into map and kfunc units with RCU whole-graph replacement | `kernel/bpf/` |
| blob verifier | map update rejects bad layout, bounds, unknown flags, count caps, and blob-size caps | `kernel/bpf/iograph_map.c` |
| interpreter | required non-JIT execution path; `run_action()` uses the compact runtime graph, `run()`/`step()` keep the byte-trie diagnostic paths | `kernel/bpf/iograph_kfunc.c` |
| kfunc API | action-only prefilter kfunc plus indexed action variant and run/step observation paths; kfuncs hold short internal RCU read sections and are available to the raw-tracepoint bench path | `kernel/bpf/iograph_kfunc.c` |
| selftest | bad blob update rejection plus DROP-before-ringbuf-reserve path | `kernel/selftests/bpf/` |
| bench | Linux selftests bench source accepts compiled blobs and raw selectors; compact aliases make the current `run_action()` runtime explicit; the indexed row measures direct entry selection; LPM has full-key, bounded-copy, and bounded-acquisition rows | `kernel/selftests/bpf/benchs/bench_iograph.c` |
| pre-ringbuf path | BPF program calls `bpf_iograph_run_action()` before reserve | `bpf/prefilter_demo.bpf.c` |
| compact runtime | userspace and kernel map-publication single-child chain compression; refreshed kernel rows now beat the same-hook LPM baseline on matched prefix paths | `src/iog_compact.c`, `kernel/bpf/iograph_map.c`, `results/native/current.md` |

## Kernel Measurements

Kernel numbers are intentionally not copied from the userspace proof. The
current kernel-backed rows below are from a patched WSL kernel with
`BPF_MAP_TYPE_IOGRAPH` enabled and `bpf_iograph_run_action()` routed through
the publication-time compact runtime graph:

| prefixes | selftests bench case | operations_M/s | throughput_ns/op | ringbuf on reject |
|---:|:---|---:|---:|:---|
| floor | empty same-hook raw tracepoint BPF row | 8.770 +/- 0.076 | 114.03 | n/a |
| 100 | compact matched decision | 6.144 +/- 0.038 | 162.76 | n/a |
| 1000 | compact early-miss decision | 7.627 +/- 0.084 | 131.11 | n/a |
| 1000 | compact matched decision | 5.780 +/- 0.041 | 173.01 | n/a |
| 1000 | compact indexed matched decision | 5.679 +/- 0.035 | 176.09 | n/a |
| 1000 | compact matched DROP | 5.822 +/- 0.019 | 171.76 | 0 B |
| 10000 | compact matched decision | 5.601 +/- 0.037 | 178.54 | n/a |
| 10000 | compact matched DROP | 5.577 +/- 0.040 | 179.31 | 0 B |
| 1000 | shared-prefix compact matched decision | 6.096 +/- 0.045 | 164.04 | n/a |
| 10000 | shared-prefix compact matched decision | 5.702 +/- 0.049 | 175.38 | n/a |
| 1000 | long-path compact matched decision | 5.939 +/- 0.040 | 168.38 | n/a |
| 10000 | long-path compact matched decision | 5.680 +/- 0.051 | 176.06 | n/a |

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
It also has an optional high-fanout byte dispatch table and an indexed action
kfunc for loader-known entries. The existing measured datasets do not exceed
the dispatch threshold, so no dispatch tables are allocated in those rows; the
compact node metadata still raises typical 1000-prefix compact runtime memory
to 40,701 B and typical 10000-prefix compact runtime memory to 172,147 B. The
kernel table above is the latest booted-kernel measurement from the patched WSL
kernel, including the indexed kfunc row. The current booted kernel still has a
64-entry graph-publication cap, so the 128/256 entry-index rows require a
rebooted overlay refresh; the 64-entry row below shows the intended benefit.

IO-aware pre-emission accounting is now folded into the userspace current
snapshot. It records `max_probe_len`, selector-copy-plus-decision rows,
discard-after-reserve baselines, action-only map memory, update scratch/peak
bytes, and dirty-cacheline accounting. Typical 1000-prefix active memory is
387,101 B with the retained blob and 40,853 B in action-only mode, while a
300 B early reject reserves 0 B with drop-before-reserve versus 344 B with
discard-after-reserve.

Current kernel evidence:

| Validation | Current status | Evidence |
|:---|:---|:---|
| Linux source overlay | map, kfunc, UAPI, selftest, and bench slices copy into Linux source layout | `scripts/apply_linux_overlay.sh` |
| incremental kernel object build | `kernel/bpf/iograph_map.o` and `kernel/bpf/iograph_kfunc.o` compile through the patched Linux build system without a full kernel rebuild | `scripts/build_linux_overlay_minimal.sh`, `results/native/current.md` |
| selftests bench path | patched Linux selftests bench loads the same-hook floor, 100/1000 io_graph decision and DROP rows, full-key and bounded-copy LPM rows, and POST payload rows | `results/native/current.md` |
| PMU gating | the observed WSL runtime exposes no CPU PMU device, so branch/cache counters must be collected on a PMU-visible native host or VM | `results/native/current.md` |

Same-hook LPM comparison from the compact kernel run:

| case | compact ns/op | LPM full-key ns/op | LPM bounded-copy ns/op | compact vs bounded |
|:---|---:|---:|---:|---:|
| 100 typical hit | 162.76 | 236.24 | 199.56 | 1.23x |
| 1000 typical hit | 173.01 | 285.55 | 236.07 | 1.36x |
| 10000 typical hit | 178.54 | 308.45 | 261.51 | 1.46x |
| 1000 shared-prefix hit | 164.04 | 470.59 | 448.03 | 2.73x |
| 10000 shared-prefix hit | 175.38 | 565.61 | 536.19 | 3.06x |
| 1000 long-path hit | 168.38 | 764.53 | 762.78 | 4.53x |
| 10000 long-path hit | 176.06 | 919.96 | 919.96 | 5.23x |

The comprehensive run includes full-key LPM rows for shared-prefix and
long-path cases. It confirms the large-policy direction without claiming that
LPM_TRIE cannot hold the policy: LPM accepted the 10000-prefix generated rows,
but bounded-copy LPM slowed to 261.51 ns/op on typical, 536.19 ns/op on
shared-prefix, and 919.96 ns/op on long-path, while compact io_graph stayed at
178.54 ns/op, 175.38 ns/op, and 176.06 ns/op respectively.

Selector-acquisition rows now copy bounded selector bytes before lookup. For
the 1000-prefix typical policy, copying 44 B adds 9.77 ns to compact hit rows
and 18.84 ns to bounded-copy LPM hit rows. For the 10000-prefix typical
policy, the same bounded acquisition gives 187.69 ns/op for compact and
275.41 ns/op for bounded-copy LPM.

Reserve/discard is also measured in the same kernel bench path:

| order | ns/op | reserved ringbuf bytes | emitted ringbuf bytes |
|:---|---:|---:|---:|
| 1000 DROP-before-reserve | 171.76 | 0 B | 0 B |
| 1000 discard-after-reserve | 191.42 | 8 B | 0 B |
| 10000 DROP-before-reserve | 179.31 | 0 B | 0 B |
| 10000 discard-after-reserve | 200.36 | 8 B | 0 B |

Multi-entry entry selection:

| entries | entry_id ns/op | entry_idx ns/op | saved_ns |
|---:|---:|---:|---:|
| 1 | 171.32 | 176.09 | -4.77 |
| 16 | 175.75 | 173.31 | 2.44 |
| 64 | 197.12 | 174.03 | 23.09 |

POST payload rows from the same patched kernel with one producer and one
ringbuf consumer:

| payload_B | compact POST ns/op | always POST ns/op |
|---:|---:|---:|
| 300 | 467.07 | 406.50 |
| 800 | 654.02 | 608.64 |
| 2048 | 1085.78 | 1062.70 |

Weighted with the 1000-prefix compact DROP row, the same kernel producer path
breaks even at 20.5% DROP for 300 B records, 9.4% DROP for 800 B records, and
2.5% DROP for 2048 B records. At 95% DROP the producer-side averages are
186.53 ns/op, 195.87 ns/op, and 217.46 ns/op respectively, while emitted
ringbuf bytes fall by 20x.

Required kernel matrix:

- map create/update/delete verification results;
- invalid blob rejection from `BPF_MAP_UPDATE_ELEM`;
- `bpf_iograph_run` interpreter latency from a BPF benchmark program;
- branch, cache, L1, and LLC miss counters when `perf_event_open` or
  `perf stat` can observe the native PMU;
- BPF verifier load time and BPF JIT time for baselines;
- update latency without BPF program reload;
- ringbuf bytes emitted and reserve/drop counts for the prefilter demo;
- real hook acquisition rows that use path/cmdline/argv acquisition helpers
  rather than a bounded copy from a preloaded BPF global;
- repeated-run variance with alternating compact/LPM order;
- `perf stat` or `perf_event_open` branch/cache counters when the native PMU
  exposes them.

The template and invocation notes belong in `results/native/README.md`.
