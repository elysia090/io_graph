# Native Kernel Current State

Date: 2026-05-23

This file records the latest kernel-backed measurement rows. Detailed raw output
and environment notes are in `results/native/compact-2026-05-23.md`.
The rows below predate the later compact tail/pruning userspace follow-up
recorded in `results/userspace/hotpath-tail-2026-05-23.md`; rerun this kernel
matrix after booting a kernel built from that source if exact post-pruning
kernel numbers are needed.

## Measurement Shape

- Runtime: WSL2 x86-64 with a booted patched
  `6.18.26.1-microsoft-standard-WSL2+ #2` kernel.
- Kernel build shape: stock Microsoft WSL kernel configuration plus the
  io_graph overlay, built from a disposable Linux worktree.
- Benchmark path: Linux `tools/testing/selftests/bpf` `bench`, attached at
  `raw_tp/sys_enter` and triggered by repeated `getpgid` syscalls.
- Policy input: compiled `iog_blob` artifacts from `iogc`.
- Selector input: preloaded writable BPF global. Path acquisition and path
  string generation are excluded from these rows.
- Payload generation: excluded. DROP rows return before ringbuf reservation.
- PMU: WSL did not expose a CPU PMU device, so branch/cache/L1/LLC counters are
  unavailable for this run.

The throughput-derived `ns/op` rows include syscall trigger, raw tracepoint
dispatch, BPF program execution, and graph rows include map/kfunc work. They
are not a pure in-kernel kfunc microbenchmark.

## Compact Runtime Rows

`iograph-hook-floor` keeps the same attach type and syscall trigger but returns
from an empty BPF program. `floor_delta_ns` subtracts that empty-hook row.

| case | operations_M/s | throughput_ns/op | floor_delta_ns |
|:---|---:|---:|---:|
| empty same-hook BPF row | 9.364 +/- 0.144 | 106.79 | 0.00 |
| 100 typical compact hit | 6.426 +/- 0.014 | 155.62 | 48.83 |
| 1000 typical compact hit | 5.910 +/- 0.060 | 169.20 | 62.41 |
| 1000 typical compact early miss | 8.091 +/- 0.181 | 123.59 | 16.80 |
| 1000 typical compact late miss | 5.867 +/- 0.068 | 170.44 | 63.65 |
| 1000 typical compact prefilter DROP | 5.956 +/- 0.029 | 167.90 | 61.11 |
| 1000 shared-prefix compact hit | 6.256 +/- 0.035 | 159.85 | 53.05 |
| 1000 long-path compact hit | 6.210 +/- 0.044 | 161.03 | 54.24 |

The previous byte-trie kernel snapshot had matched decisions around
352-356 ns/op. The compact runtime brings matched decisions down to
156-170 ns/op on the same raw-tracepoint measurement shape.

## Same-Bench LPM Trie

`iograph-lpm-decision` runs the same selector and attach/trigger path through
`BPF_MAP_TYPE_LPM_TRIE`. LPM needs a `prefixlen,data` lookup key, so this
historical row uses one per-CPU scratch key object and copies the full 256 B
selector cap before the LPM helper call. The source now also contains
`iograph-lpm-bounded-decision` and `iograph-lpm-bounded-prefilter` rows that
copy only `selector_len` bytes; those rows require a refreshed booted-kernel
run.

| case | compact ns/op | LPM ns/op | compact speedup |
|:---|---:|---:|---:|
| 100 typical hit | 155.62 | 228.99 | 1.47x |
| 1000 typical hit | 169.20 | 287.44 | 1.70x |
| 1000 typical prefilter DROP | 167.90 | 286.37 | 1.71x |
| 1000 shared-prefix hit | 159.85 | 476.64 | 2.98x |
| 1000 long-path hit | 161.03 | 791.77 | 4.92x |

For the plain prefix rows that previously favored LPM, runtime-only chain
compression now closes the matched-path gap and beats the same-hook LPM baseline
in this WSL kernel run.

## Validation Notes

- `bench iograph-hook-floor` loaded the same skeleton and attached the empty
  raw tracepoint BPF floor row.
- `bench iograph-compact-decision` and `bench iograph-compact-prefilter`
  loaded `BPF_MAP_TYPE_IOGRAPH`, updated it with compiled graph blobs, and ran
  100/1000-prefix typical policies plus 1000-prefix shared/long-path policies.
- `bench iograph-lpm-decision` and `bench iograph-lpm-prefilter` loaded the
  corresponding prefix text files into an LPM trie baseline on the same raw
  tracepoint path.
- The source now has bounded-copy LPM bench variants, but this dated kernel run
  predates those rows.
- The compact prefilter DROP row returned before `bpf_ringbuf_reserve()`, so
  rejected events contributed 0 ringbuf bytes in this bench path.
- The WSL environment exposes no CPU PMU device. Branch/cache/L1/LLC
  `perf_event_open` rows remain native-host work.

## Historical Byte-Trie Snapshot

The pre-compact kernel snapshot is retained in git history and summarized here
only for comparison:

| prefixes | selector case | byte-trie ns/op | LPM ns/op |
|---:|:---|---:|---:|
| 100 | matched prefix | 352.2 | 240.7 |
| 1000 | matched prefix | 355.9 | 290.9 |

Those rows are no longer the current `run_action()` path.
