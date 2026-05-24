# Native Kernel Current State

This file is the stable kernel-backed result entrypoint. Here, "native" means
the Linux kernel overlay and BPF selftests path rather than the userspace proof;
the current rows were measured on a patched WSL2 kernel, not on a PMU-visible
bare-metal host.

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
- Selector input: preloaded writable BPF global unless the row says
  `acquire+decision`.
- Path generation and path canonicalization: excluded.
- DROP payload generation: excluded; DROP-before-reserve rows return before
  `bpf_ringbuf_reserve()`.
- POST payload rows: included separately with one producer and one ringbuf
  consumer, covering 300 B, 800 B, and 2048 B payload copies.
- PMU: WSL did not expose a CPU PMU device, so branch/cache/L1/LLC counters are
  unavailable for this run. `perf_event_paranoid` was `1`.

The throughput-derived `ns/op` rows include syscall trigger, raw tracepoint
dispatch, BPF program execution, and graph rows include map/kfunc work. They
are not pure in-kernel kfunc microbenchmarks.

`iograph-hook-floor` keeps the same attach type and syscall trigger but returns
from an empty BPF program. `floor_delta_ns` subtracts that empty-hook row.

## Compact And LPM Rows

| case | operations_M/s | throughput_ns/op | floor_delta_ns |
|:---|---:|---:|---:|
| empty same-hook BPF row | 8.785 +/- 0.069 | 113.83 | 0.00 |
| 100 typical compact hit | 6.141 +/- 0.097 | 162.84 | 49.01 |
| 1000 typical compact early miss | 7.564 +/- 0.047 | 132.21 | 18.37 |
| 1000 typical compact hit | 5.739 +/- 0.071 | 174.25 | 60.42 |
| 1000 typical compact indexed hit | 5.689 +/- 0.060 | 175.78 | 61.95 |
| 1000 typical compact prefilter DROP | 5.787 +/- 0.054 | 172.80 | 58.97 |
| 1000 shared-prefix compact hit | 6.025 +/- 0.045 | 165.98 | 52.14 |
| 1000 long-path compact hit | 5.914 +/- 0.050 | 169.09 | 55.26 |

The previous byte-trie kernel snapshot had matched decisions around
352-356 ns/op. The compact runtime brings matched decisions down to
163-176 ns/op on the same raw-tracepoint measurement shape.

`BPF_MAP_TYPE_LPM_TRIE` is the same-hook prefix baseline. The full-key row
copies the full 256 B selector cap into the LPM scratch key. The bounded-copy
row copies only `selector_len` bytes.

| case | compact ns/op | LPM full-key ns/op | LPM bounded-copy ns/op | compact vs bounded |
|:---|---:|---:|---:|---:|
| 100 typical hit | 162.84 | 232.83 | 203.29 | 1.25x |
| 1000 typical hit | 174.25 | 292.74 | 246.37 | 1.41x |
| 1000 typical prefilter DROP | 172.80 | 293.86 | 245.28 | 1.42x |
| 1000 shared-prefix hit | 165.98 | n/a | 453.31 | 2.73x |
| 1000 long-path hit | 169.09 | n/a | 768.05 | 4.54x |

For this string/path-prefix action-policy workload, compact io_graph beats the
same-hook LPM_TRIE rows that include bounded key materialization. This is not a
claim that io_graph is generally faster than LPM_TRIE for every prefix map use.

## Selector Acquisition Rows

These rows copy bounded selector bytes into a stack buffer before lookup. For
the 1000-prefix typical policy, `max_probe_len` is 44 B.

| case | direct ns/op | acquire+decision ns/op | acquisition_delta_ns |
|:---|---:|---:|---:|
| compact 1000 early miss | 132.21 | 139.72 | 7.51 |
| compact 1000 hit | 174.25 | 184.54 | 10.29 |
| LPM bounded 1000 early miss | 129.55 | 139.96 | 10.41 |
| LPM bounded 1000 hit | 246.37 | 274.57 | 28.20 |

The acquisition rows still use a preloaded BPF global as the source. They model
bounded selector copy cost, not real path lookup, canonicalization, or argv
walking.

## Drop Order Rows

| case | operations_M/s | throughput_ns/op | floor_delta_ns | reserved ringbuf bytes | emitted ringbuf bytes |
|:---|---:|---:|---:|---:|---:|
| DROP-before-reserve | 5.787 +/- 0.054 | 172.80 | 58.97 | 0 B | 0 B |
| discard-after-reserve | 5.232 +/- 0.047 | 191.13 | 77.30 | 8 B | 0 B |

The discard row reserves only the small benchmark event header and immediately
discards it. It isolates the reserve/discard ordering cost; payload-copy POST
cost is measured separately below.

## Multi-Entry Indexed Rows

The multi-entry blobs reuse the same entry state for each entry and vary only
the entry table size. The `entry_id` row searches for the last id; the
`entry_idx` row directly indexes the same table slot.

| entries | entry_id ns/op | entry_idx ns/op | saved_ns | speedup |
|---:|---:|---:|---:|---:|
| 1 | 174.25 | 175.78 | -1.53 | 0.99x |
| 16 | 178.92 | 176.12 | 2.80 | 1.02x |
| 64 | 204.92 | 176.68 | 28.24 | 1.16x |

The single-entry row intentionally shows no gain because `run_action()` already
has a single-entry fast path. The 64-entry row shows the intended direct
entry-selection benefit.

## POST Payload Rows

These rows include ringbuf reservation, payload copy, and one ringbuf consumer.
They show the cost paid for POST events; DROP rows above return before reserve.

| payload_B | ringbuf_record_B | compact POST M/s | compact POST ns/op | always POST M/s | always POST ns/op |
|---:|---:|---:|---:|---:|---:|
| 300 | 344 | 2.136 +/- 0.008 | 468.16 | 2.450 +/- 0.011 | 408.16 |
| 800 | 840 | 1.544 +/- 0.010 | 647.67 | 1.637 +/- 0.009 | 610.87 |
| 2048 | 2088 | 0.899 +/- 0.004 | 1112.35 | 0.938 +/- 0.004 | 1066.10 |

Weighted producer-side cost uses the 1000-prefix compact DROP row
(172.80 ns/op) and the compact POST payload rows above. Ringbuf emission is
only on the POST fraction.

| drop_pct | payload_B | compact prefilter avg ns/op | always POST ns/op | speedup | emitted ringbuf_B/op | ringbuf reduction |
|---:|---:|---:|---:|---:|---:|---:|
| 50 | 300 | 320.48 | 408.16 | 1.27x | 172.00 | 2.00x |
| 50 | 800 | 410.23 | 610.87 | 1.49x | 420.00 | 2.00x |
| 50 | 2048 | 642.57 | 1066.10 | 1.66x | 1044.00 | 2.00x |
| 80 | 300 | 231.87 | 408.16 | 1.76x | 68.80 | 5.00x |
| 80 | 800 | 267.77 | 610.87 | 2.28x | 168.00 | 5.00x |
| 80 | 2048 | 360.71 | 1066.10 | 2.96x | 417.60 | 5.00x |
| 90 | 300 | 202.34 | 408.16 | 2.02x | 34.40 | 10.00x |
| 90 | 800 | 220.29 | 610.87 | 2.77x | 84.00 | 10.00x |
| 90 | 2048 | 266.76 | 1066.10 | 4.00x | 208.80 | 10.00x |
| 95 | 300 | 187.57 | 408.16 | 2.18x | 17.20 | 20.00x |
| 95 | 800 | 196.54 | 610.87 | 3.11x | 42.00 | 20.00x |
| 95 | 2048 | 219.78 | 1066.10 | 4.85x | 104.40 | 20.00x |
| 99 | 300 | 175.75 | 408.16 | 2.32x | 3.44 | 100.00x |
| 99 | 800 | 177.55 | 610.87 | 3.44x | 8.40 | 100.00x |
| 99 | 2048 | 182.20 | 1066.10 | 5.85x | 20.88 | 100.00x |

Break-even DROP rate is low because POST rows pay normal reserve/copy work
while DROP rows return before reserve:

| payload_B | break-even DROP rate |
|---:|---:|
| 300 | 20.3% |
| 800 | 7.7% |
| 2048 | 4.9% |

The weighted average is:

```text
avg_prefilter =
  drop_fraction * compact_drop_ns +
  post_fraction * compact_post_ns
```

Break-even is when `avg_prefilter < always_post_ns`.

## Validation Notes

- The latest source overlay is validated with the incremental object gate:
  `scripts/build_linux_overlay_minimal.sh` builds `kernel/bpf/iograph_map.o`
  and `kernel/bpf/iograph_kfunc.o` through the patched Linux build system
  without a full kernel rebuild.
- The running kernel already exposed the current map type and kfuncs. This run
  rebuilt only the Linux selftests `bench` binary after adding bench rodata for
  `entry_id` and `entry_idx`.
- The selftests bench object build uses explicit volatile byte-copy loops in
  the BPF program so bounded selector copies are not lowered into unsupported
  BPF `memcpy` calls by the compiler.
- `bench iograph-hook-floor` loaded the same skeleton and attached the empty
  raw tracepoint BPF floor row.
- `bench iograph-compact-decision`, `bench iograph-compact-idx-decision`, and
  `bench iograph-compact-prefilter` loaded `BPF_MAP_TYPE_IOGRAPH`, updated it
  with compiled graph blobs, and ran 100/1000-prefix typical policies plus
  1000-prefix shared/long-path policies.
- `bench iograph-lpm-decision`, `bench iograph-lpm-prefilter`,
  `bench iograph-lpm-bounded-decision`, and
  `bench iograph-lpm-bounded-prefilter` measured same-hook LPM baselines.
- `bench iograph-compact-acquire-decision` and
  `bench iograph-lpm-bounded-acquire-decision` measured bounded selector copy
  before the decision.
- `bench iograph-discard-after-reserve` measured reserve/discard after a DROP
  decision.
- `bench iograph-compact-post-payload` and `bench iograph-ringbuf-always-post`
  measured POST-side payload materialization with a ringbuf consumer.
- The compact prefilter DROP row returned before `bpf_ringbuf_reserve()`, so
  rejected events contributed 0 ringbuf bytes in this bench path.
- The WSL environment exposes no CPU PMU device. Branch/cache/L1/LLC
  `perf_event_open` rows remain PMU-visible host work.

Representative command shape:

```sh
./bench -w 1 -d 5 iograph-hook-floor --selector /x/miss/early-00/not-matched
./bench -w 1 -d 5 iograph-compact-decision --blob policy.iog --selector "$hit"
./bench -w 1 -d 5 iograph-compact-prefilter --blob policy.iog --selector "$hit" --drop-action 1
./bench -w 1 -d 5 iograph-lpm-bounded-decision --prefixes prefixes.txt --selector "$hit"
./bench -w 1 -d 5 iograph-compact-acquire-decision --blob policy.iog --selector "$hit" --probe-len 44
./bench -w 1 -d 5 iograph-discard-after-reserve --blob policy.iog --selector "$hit" --drop-action 1
./bench -w 1 -d 5 -c 1 iograph-compact-post-payload --blob policy.iog --selector "$hit" --drop-action 2 --payload-size 800
./bench -w 1 -d 5 iograph-compact-idx-decision --blob policy_entries64.iog --selector "$hit" --entry-idx 63
```

## Remaining Evidence

- PMU-visible host or VM counters: cycles, instructions, branch misses, L1D
  misses, LLC misses, and i-cache misses where available.
- Real hook acquisition rows that use actual path/cmdline/argv acquisition
  helpers rather than copying from a preloaded BPF global.
- Repeated-run variance study with alternating compact/LPM order.
- Kernel map update latency and memory accounting from `BPF_MAP_UPDATE_ELEM`.

## Historical Byte-Trie Snapshot

The pre-compact kernel snapshot is retained in git history and summarized here
only for comparison:

| prefixes | selector case | byte-trie ns/op | LPM ns/op |
|---:|:---|---:|---:|
| 100 | matched prefix | 352.2 | 240.7 |
| 1000 | matched prefix | 355.9 | 290.9 |

Those rows are no longer the current `run_action()` path.
