# Native Kernel Current State

This file records the latest kernel-backed measurement rows. It is the stable
native result entrypoint; date-stamped run notes are folded here instead of kept
as parallel sources of truth.

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
- POST payload rows: included separately with one producer and one ringbuf
  consumer, covering 300 B, 800 B, and 2048 B payload copies.
- PMU: WSL did not expose a CPU PMU device, so branch/cache/L1/LLC counters are
  unavailable for this run. `perf_event_paranoid` was `1`.

The throughput-derived `ns/op` rows include syscall trigger, raw tracepoint
dispatch, BPF program execution, and graph rows include map/kfunc work. They
are not a pure in-kernel kfunc microbenchmark.

## Compact Runtime Rows

`iograph-hook-floor` keeps the same attach type and syscall trigger but returns
from an empty BPF program. `floor_delta_ns` subtracts that empty-hook row.

| case | operations_M/s | throughput_ns/op | floor_delta_ns |
|:---|---:|---:|---:|
| empty same-hook BPF row | 8.618 +/- 0.185 | 116.04 | 0.00 |
| 100 typical compact hit | 6.086 +/- 0.050 | 164.31 | 48.27 |
| 1000 typical compact early miss | 7.683 +/- 0.127 | 130.16 | 14.12 |
| 1000 typical compact hit | 5.695 +/- 0.131 | 175.59 | 59.55 |
| 1000 typical compact prefilter DROP | 5.772 +/- 0.028 | 173.25 | 57.21 |
| 1000 shared-prefix compact hit | 5.725 +/- 0.060 | 174.67 | 58.63 |
| 1000 long-path compact hit | 5.724 +/- 0.074 | 174.70 | 58.66 |

The previous byte-trie kernel snapshot had matched decisions around
352-356 ns/op. The compact runtime brings matched decisions down to
164-176 ns/op on the same raw-tracepoint measurement shape.

## Same-Bench LPM Trie

`iograph-lpm-decision` runs the same selector and attach/trigger path through
`BPF_MAP_TYPE_LPM_TRIE`. LPM needs a `prefixlen,data` lookup key, so the full-key
row copies the full 256 B selector cap before the LPM helper call. The
bounded-copy row copies only `selector_len` bytes.

| case | compact ns/op | LPM full-key ns/op | LPM bounded-copy ns/op | compact vs bounded |
|:---|---:|---:|---:|---:|
| 100 typical hit | 164.31 | 212.18 | 184.95 | 1.13x |
| 1000 typical hit | 175.59 | 250.69 | 212.40 | 1.21x |
| 1000 typical prefilter DROP | 173.25 | 254.65 | 209.29 | 1.21x |
| 1000 shared-prefix hit | 174.67 | 319.69 | 304.14 | 1.74x |
| 1000 long-path hit | 174.70 | 516.00 | 505.31 | 2.89x |

Bounded-copy LPM materially improves the short-selector rows, but compact
`io_graph` still beats the same-hook LPM baseline for this prefix workload.

## POST Payload Rows

These rows include ringbuf reservation, payload copy, and one ringbuf consumer.
They show the cost paid for POST events; DROP rows above return before reserve.

| payload_B | ringbuf_record_B | compact POST M/s | compact POST ns/op | always POST M/s | always POST ns/op |
|---:|---:|---:|---:|---:|---:|
| 300 | 344 | 2.164 +/- 0.070 | 462.11 | 2.581 +/- 0.107 | 387.45 |
| 800 | 840 | 1.658 +/- 0.017 | 603.14 | 1.807 +/- 0.018 | 553.40 |
| 2048 | 2088 | 0.987 +/- 0.013 | 1013.17 | 1.024 +/- 0.030 | 976.56 |

## Validation Notes

- `bench iograph-hook-floor` loaded the same skeleton and attached the empty
  raw tracepoint BPF floor row.
- `bench iograph-compact-decision` and `bench iograph-compact-prefilter`
  loaded `BPF_MAP_TYPE_IOGRAPH`, updated it with compiled graph blobs, and ran
  100/1000-prefix typical policies plus 1000-prefix shared/long-path policies.
- `bench iograph-lpm-decision` and `bench iograph-lpm-prefilter` loaded the
  corresponding prefix text files into an LPM trie baseline on the same raw
  tracepoint path.
- `bench iograph-lpm-bounded-decision` and
  `bench iograph-lpm-bounded-prefilter` measured selector-length-only LPM key
  materialization.
- `bench iograph-compact-post-payload` and `bench iograph-ringbuf-always-post`
  measured POST-side payload materialization with a ringbuf consumer.
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
