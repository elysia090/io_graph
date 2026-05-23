# Native Kernel Current State

Date: 2026-05-21

This file records the current kernel-backed measurement rows. They are separate
from the frozen userspace proof in `results/userspace/current.md`.

The measured rows below are the last typed-tracepoint snapshot. The bench source
now uses `raw_tp/sys_enter` and batches producer trigger accounting to remove
per-trigger bookkeeping from the fixed-cost path; refresh these rows after the
updated kernel/selftests slice is loaded.

## Measurement Shape

- Runtime: WSL2 x86-64 with a booted patched
  `6.18.26.1-microsoft-standard-WSL2+` kernel.
- Kernel build shape: stock Microsoft WSL kernel configuration plus the
  io_graph overlay. The integration changes to existing Linux files stay at the
  map-type plumbing, privileged map-create allow-list, object list, tools UAPI,
  and selftests bench registration points.
- Benchmark path: Linux `tools/testing/selftests/bpf` `bench`
  `iograph-hook-floor`, `iograph-decision`, `iograph-prefilter`, and LPM
  decision rows, all attached at `tp_btf/sys_enter` and triggered by repeated
  `getpgid` syscalls.
- Policy input: compiled 100-prefix and 1000-prefix `iog_blob` artifacts from
  the existing prefix generator and `iogc`.
- Selector input: a preloaded writable BPF global. Path acquisition and path
  string generation are excluded from these rows.

The throughput-derived `ns/op` rows below include the syscall trigger,
tracepoint dispatch, and BPF program. Graph rows also include map/kfunc graph
work. They are not a pure in-kernel kfunc microbenchmark.

The DROP rows count completed producer triggers outside BPF. The BPF rejected
path does not increment a global DROP counter after it already has the answer.

## Fixed-Cost Split

`iograph-hook-floor` keeps the same attach type and syscall trigger but returns
from an empty BPF program. It measures the hook-adjacent floor before policy
lookup:

| row | operations_M/s | throughput_ns/op |
|:---|---:|---:|
| empty `tp_btf/sys_enter` BPF hook | 8.351 +/- 0.126 | 119.7 |

Action-only graph rows use `iograph-decision`. `floor_delta_ns` subtracts the
119.7 ns empty-hook row to show the work above attach, syscall trigger, and
empty BPF dispatch:

| prefixes | selector case | operations_M/s | throughput_ns/op | floor_delta_ns |
|---:|:---|---:|---:|---:|
| 100 | early miss `/z` | 7.513 +/- 0.089 | 133.1 | 13.4 |
| 100 | matched prefix | 2.839 +/- 0.019 | 352.2 | 232.5 |
| 1000 | early miss `/z` | 7.465 +/- 0.072 | 134.0 | 14.3 |
| 1000 | matched prefix | 2.810 +/- 0.047 | 355.9 | 236.2 |

The early-miss row is only a few tens of nanoseconds above the same-hook floor.
That is the visible fixed-cost boundary: the direct C primitive still runs in
single-digit or low-teens ns for early mismatch, while this tracing row must
pay syscall trigger, tracepoint dispatch, BPF entry, and kfunc plumbing before
the graph can return.

## Prefilter DROP Rows

`iograph-prefilter` runs the same action first and reaches DROP before ringbuf
reservation:

| prefixes | selector case | action path | consumer | operations_M/s | throughput_ns/op |
|---:|:---|:---|:---|---:|---:|
| 100 | matched prefix | action-only DROP before reserve | no | 2.853 +/- 0.009 | 350.5 |
| 1000 | matched prefix | action-only DROP before reserve | no | 2.846 +/- 0.007 | 351.4 |

The matched DROP rows returned from the BPF program before
`bpf_ringbuf_reserve()`, so rejected events contribute 0 ringbuf bytes in this
kernel-backed path. POST-side payload copy rows stay in the userspace event
path matrix until the kernel bench grows a payload materialization mode.

The prefilter rows now sit on the action decision rows rather than paying a
benchmark-only BPF DROP counter after the decision. Prefix-count growth from
100 to 1000 still does not move the matched graph row materially on this
generated policy set.

## Userspace Comparison

The userspace primitive has no syscall trigger, attach dispatch, or kfunc
boundary. The latest direct action rows are recorded in
`results/userspace/current.md`:

| prefixes | selector case | direct C ns/op | kernel hook-adjacent ns/op |
|---:|:---|---:|---:|
| 100 | early miss | 11.24 | 133.1 |
| 100 | matched prefix / hit | 223.79 | 352.2 |
| 1000 | early miss | 7.17 | 134.0 |
| 1000 | matched prefix / hit | 234.00 | 355.9 |

The comparison says where to optimize next. Graph traversal explains most of
the matched-prefix delta above the 119.7 ns hook floor. It does not explain the
early-miss tracing row; that row is already fixed-cost dominated.

## Same-Bench LPM Trie

`iograph-lpm-decision` runs the same selector and attach/trigger path through
`BPF_MAP_TYPE_LPM_TRIE`. LPM needs a `prefixlen,data` lookup key, so this bench
uses one per-CPU scratch key object before the LPM helper call.

| prefixes | matched decision matcher | operations_M/s | throughput_ns/op | io_graph_ns / lpm_ns |
|---:|:---|---:|---:|---:|
| 100 | io_graph action kfunc | 2.839 +/- 0.019 | 352.2 | 1.46 |
| 100 | LPM trie | 4.155 +/- 0.030 | 240.7 | 1.00 |
| 1000 | io_graph action kfunc | 2.810 +/- 0.047 | 355.9 | 1.22 |
| 1000 | LPM trie | 3.438 +/- 0.015 | 290.9 | 1.00 |

For plain bounded path-prefix matching, this LPM baseline is currently faster
in the hook-adjacent WSL bench. The io_graph case here is representation and
execution-order evidence, not a claim that the first graph interpreter beats
the existing LPM helper on its home workload.

## Validation

- `bench iograph-hook-floor` loaded the same skeleton and attached the empty
  tracing BPF floor row.
- `bench iograph-decision` and `bench iograph-prefilter` loaded the
  `BPF_MAP_TYPE_IOGRAPH` policy map, updated it with compiled graph blobs,
  attached tracing BPF programs, and ran 100-prefix and 1000-prefix policies.
- `bench iograph-lpm-decision` loaded the same 100-prefix and 1000-prefix text
  prefix sets into an LPM trie baseline on the same tracing path.
- The current tree also compiles the kfunc objects after removing the nested
  per-decision RCU lock; the rows above were collected on the already booted
  patched WSL image before a relink/reboot of that kernel object change.
- Repository C tests passed with `make test`.
- Building the full Linux `test_progs` runner stopped before the io_graph test
  at an unrelated `bpf_qdisc_fail__incompl_ops` BPF compile failure in this WSL
  toolchain path. The io_graph selftest remains present, but this run does not
  claim a `test_progs -t iograph` result.

## PMU Status

The measurement kernel had `perf_event_paranoid=2`, but WSL did not expose
`/sys/bus/event_source/devices/cpu/type`. Branch/cache/L1/LLC
`perf_event_open` rows therefore remain unavailable for this kernel-backed run.
A native Linux host or PMU-visible VM is still needed for those counters.
