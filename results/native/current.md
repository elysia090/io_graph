# Native Kernel Current State

This file records the latest kernel-backed measurement rows. It is the stable
native result entrypoint; date-stamped run notes are folded here instead of kept
as parallel sources of truth.

## Measurement Shape

- Runtime: WSL2 x86-64 with a rebuilt patched
  `6.18.26.1-microsoft-standard-WSL2+` kernel that includes the current
  io_graph overlay and indexed action kfunc.
- Kernel build shape: stock Microsoft WSL kernel configuration plus the
  io_graph overlay, built from a disposable Linux worktree.
- Source validation shape: normal edit/build cycles use the incremental
  overlay gate that builds only `kernel/bpf/iograph_map.o` and
  `kernel/bpf/iograph_kfunc.o` through the patched Linux build system.
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

### Minimal Reboot Refresh

The patched 6.18 WSL kernel was rebuilt and booted again so the BTF visible to
the BPF loader includes `bpf_iograph_run_action_idx()`. The table below is a
minimal same-hook refresh for the 1000-prefix typical policy. It intentionally
does not replace the broader matrix above because the 100-prefix,
shared-prefix, long-path, and POST-payload rows were not rerun in this pass.

| case | operations_M/s | throughput_ns/op | floor_delta_ns |
|:---|---:|---:|---:|
| empty same-hook BPF row | 8.757 +/- 0.020 | 114.19 | 0.00 |
| 1000 typical compact hit | 5.953 +/- 0.017 | 167.98 | 53.79 |
| 1000 typical compact indexed hit | 5.830 +/- 0.078 | 171.53 | 57.34 |
| 1000 typical compact prefilter DROP | 5.789 +/- 0.160 | 172.74 | 58.55 |
| 1000 typical LPM bounded-copy hit | 4.209 +/- 0.009 | 237.59 | 123.40 |
| 1000 typical LPM bounded-copy prefilter DROP | 4.204 +/- 0.028 | 237.87 | 123.68 |

The indexed row validates the new kfunc load and execution path. This policy has
one entry, so the normal `run_action()` path already uses the single-entry fast
path; a multi-entry policy is needed to show the intended entry-lookup benefit.
In this minimal refresh, compact `io_graph` remains faster than bounded-copy LPM
by 1.41x on the hit row and 1.38x on the DROP row.

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

Weighted producer-side cost uses the 1000-prefix compact DROP row
(173.25 ns/op) and the compact POST payload rows above. Ringbuf emission is
only on the POST fraction.

| drop_pct | payload_B | compact prefilter avg ns/op | always POST ns/op | speedup | emitted ringbuf_B/op | ringbuf reduction |
|---:|---:|---:|---:|---:|---:|---:|
| 50 | 300 | 317.68 | 387.45 | 1.22x | 172.00 | 2.00x |
| 50 | 800 | 388.19 | 553.40 | 1.43x | 420.00 | 2.00x |
| 50 | 2048 | 593.21 | 976.56 | 1.65x | 1044.00 | 2.00x |
| 80 | 300 | 231.02 | 387.45 | 1.68x | 68.80 | 5.00x |
| 80 | 800 | 259.23 | 553.40 | 2.13x | 168.00 | 5.00x |
| 80 | 2048 | 341.23 | 976.56 | 2.86x | 417.60 | 5.00x |
| 90 | 300 | 202.14 | 387.45 | 1.92x | 34.40 | 10.00x |
| 90 | 800 | 216.24 | 553.40 | 2.56x | 84.00 | 10.00x |
| 90 | 2048 | 257.24 | 976.56 | 3.80x | 208.80 | 10.00x |
| 95 | 300 | 187.69 | 387.45 | 2.06x | 17.20 | 20.00x |
| 95 | 800 | 194.74 | 553.40 | 2.84x | 42.00 | 20.00x |
| 95 | 2048 | 215.25 | 976.56 | 4.54x | 104.40 | 20.00x |
| 99 | 300 | 176.14 | 387.45 | 2.20x | 3.44 | 100.00x |
| 99 | 800 | 177.55 | 553.40 | 3.12x | 8.40 | 100.00x |
| 99 | 2048 | 181.65 | 976.56 | 5.38x | 20.88 | 100.00x |

Break-even DROP rate is low because POST rows pay normal reserve/copy work
while DROP rows return before reserve:

| payload_B | break-even DROP rate |
|---:|---:|
| 300 | 25.8% |
| 800 | 11.6% |
| 2048 | 4.4% |

## Validation Notes

- The latest source overlay is validated with the incremental object gate:
  `scripts/build_linux_overlay_minimal.sh` builds `kernel/bpf/iograph_map.o`
  and `kernel/bpf/iograph_kfunc.o` through the patched Linux build system
  without a full kernel rebuild.
- A refreshed selftests bench run requires booting a patched 6.18 WSL kernel.
  The old boot artifact could load the compact rows, but lacked the indexed
  action kfunc in kernel BTF, so the boot `bzImage` was relinked and replaced.
- The selftests bench object build uses explicit volatile byte-copy loops in
  the BPF program so bounded selector copies are not lowered into unsupported
  BPF `memcpy` calls by the compiler.
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
- `bench iograph-compact-acquire-decision`,
  `bench iograph-lpm-bounded-acquire-decision`, and
  `bench iograph-discard-after-reserve` are now in the bench source for the
  next kernel run; the table above does not include those unrefreshed rows yet.
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
