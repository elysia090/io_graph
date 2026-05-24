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
| empty same-hook BPF row | 8.770 +/- 0.076 | 114.03 | 0.00 |
| 100 typical compact hit | 6.144 +/- 0.038 | 162.76 | 48.73 |
| 1000 typical compact early miss | 7.627 +/- 0.084 | 131.11 | 17.08 |
| 1000 typical compact hit | 5.780 +/- 0.041 | 173.01 | 58.98 |
| 1000 typical compact indexed hit | 5.679 +/- 0.035 | 176.09 | 62.06 |
| 1000 typical compact prefilter DROP | 5.822 +/- 0.019 | 171.76 | 57.73 |
| 10000 typical compact early miss | 7.621 +/- 0.073 | 131.22 | 17.19 |
| 10000 typical compact hit | 5.601 +/- 0.037 | 178.54 | 64.51 |
| 10000 typical compact prefilter DROP | 5.577 +/- 0.040 | 179.31 | 65.28 |
| 1000 shared-prefix compact hit | 6.096 +/- 0.045 | 164.04 | 50.01 |
| 10000 shared-prefix compact hit | 5.702 +/- 0.049 | 175.38 | 61.35 |
| 1000 long-path compact hit | 5.939 +/- 0.040 | 168.38 | 54.35 |
| 10000 long-path compact hit | 5.680 +/- 0.051 | 176.06 | 62.03 |

The previous byte-trie kernel snapshot had matched decisions around
352-356 ns/op. The compact runtime brings matched decisions down to
162-180 ns/op on the same raw-tracepoint measurement shape.

`BPF_MAP_TYPE_LPM_TRIE` is the same-hook prefix baseline. The full-key row
copies the full 256 B selector cap into the LPM scratch key. The bounded-copy
row copies only `selector_len` bytes.

| case | compact ns/op | LPM full-key ns/op | LPM bounded-copy ns/op | compact vs bounded |
|:---|---:|---:|---:|---:|
| 100 typical hit | 162.76 | 236.24 | 199.56 | 1.23x |
| 100 typical prefilter DROP | 163.69 | 244.62 | 202.14 | 1.23x |
| 1000 typical hit | 173.01 | 285.55 | 236.07 | 1.36x |
| 1000 typical prefilter DROP | 171.76 | 288.52 | 239.12 | 1.39x |
| 10000 typical hit | 178.54 | 308.45 | 261.51 | 1.46x |
| 10000 typical prefilter DROP | 179.31 | 303.40 | 283.05 | 1.58x |
| 1000 shared-prefix hit | 164.04 | 470.59 | 448.03 | 2.73x |
| 10000 shared-prefix hit | 175.38 | 565.61 | 536.19 | 3.06x |
| 1000 long-path hit | 168.38 | 764.53 | 762.78 | 4.53x |
| 10000 long-path hit | 176.06 | 919.96 | 919.96 | 5.23x |

The full-key LPM rows are intentionally retained for shared-prefix and
long-path cases in this comprehensive run. They confirm that the long-selector
gap is not caused only by bounded-copy key construction: the LPM trie walk
itself dominates those rows.

For this string/path-prefix action-policy workload, compact io_graph beats the
same-hook LPM_TRIE rows that include bounded key materialization. This is not a
claim that io_graph is generally faster than LPM_TRIE for every prefix map use.

This confirms the expected large-policy direction without overstating it:
LPM_TRIE accepts the 10000 generated prefix rows, but its same-hook bounded
typical hit row slows from 236.07 ns/op at 1000 prefixes to 261.51 ns/op at
10000 prefixes, while compact io_graph moves from 173.01 ns/op to
178.54 ns/op. The 10000-prefix compact decision remains 1.46x faster than the
bounded-copy LPM row, and acquisition widens the gap because LPM still builds a
lookup key after selector acquisition.

## Selector Acquisition Rows

These rows copy bounded selector bytes into a stack buffer before lookup. For
the typical policies, `max_probe_len` is 44 B.

| case | direct ns/op | acquire+decision ns/op | acquisition_delta_ns |
|:---|---:|---:|---:|
| compact 1000 early miss | 131.11 | 139.47 | 8.36 |
| compact 10000 early miss | 131.22 | 137.87 | 6.65 |
| compact 100 hit | 162.76 | 168.83 | 6.07 |
| compact 1000 hit | 173.01 | 182.78 | 9.77 |
| compact 10000 hit | 178.54 | 187.69 | 9.15 |
| shared-prefix compact 1000 hit | 164.04 | 183.76 | 19.72 |
| shared-prefix compact 10000 hit | 175.38 | 195.39 | 20.01 |
| long-path compact 1000 hit | 168.38 | 203.25 | 34.87 |
| long-path compact 10000 hit | 176.06 | 213.31 | 37.25 |
| LPM bounded 1000 early miss | 133.89 | 141.24 | 7.35 |
| LPM bounded 10000 early miss | 133.78 | 143.08 | 9.30 |
| LPM bounded 100 hit | 199.56 | 208.07 | 8.51 |
| LPM bounded 1000 hit | 236.07 | 254.91 | 18.84 |
| LPM bounded 10000 hit | 261.51 | 275.41 | 13.90 |
| shared-prefix LPM bounded 1000 hit | 448.03 | 488.52 | 40.49 |
| shared-prefix LPM bounded 10000 hit | 536.19 | 562.11 | 25.92 |
| long-path LPM bounded 1000 hit | 762.78 | 791.77 | 28.99 |
| long-path LPM bounded 10000 hit | 919.96 | 956.02 | 36.06 |

The acquisition rows still use a preloaded BPF global as the source. They model
bounded selector copy cost, not real path lookup, canonicalization, or argv
walking.

## Drop Order Rows

| case | operations_M/s | throughput_ns/op | floor_delta_ns | reserved ringbuf bytes | emitted ringbuf bytes |
|:---|---:|---:|---:|---:|---:|
| 1000 DROP-before-reserve | 5.822 +/- 0.019 | 171.76 | 57.73 | 0 B | 0 B |
| 1000 discard-after-reserve | 5.224 +/- 0.017 | 191.42 | 77.39 | 8 B | 0 B |
| 10000 DROP-before-reserve | 5.577 +/- 0.040 | 179.31 | 65.28 | 0 B | 0 B |
| 10000 discard-after-reserve | 4.991 +/- 0.025 | 200.36 | 86.33 | 8 B | 0 B |

The discard row reserves only the small benchmark event header and immediately
discards it. It isolates the reserve/discard ordering cost; payload-copy POST
cost is measured separately below.

## Multi-Entry Indexed Rows

The multi-entry blobs reuse the same entry state for each entry and vary only
the entry table size. The `entry_id` row searches for the last id; the
`entry_idx` row directly indexes the same table slot.

| entries | entry_id ns/op | entry_idx ns/op | saved_ns | speedup |
|---:|---:|---:|---:|---:|
| 1 | 171.32 | 176.09 | -4.77 | 0.97x |
| 16 | 175.75 | 173.31 | 2.44 | 1.01x |
| 64 | 197.12 | 174.03 | 23.09 | 1.13x |

The single-entry row intentionally shows no gain because `run_action()` already
has a single-entry fast path. The 64-entry row shows the intended direct
entry-selection benefit. The running patched kernel used for this matrix still
has a 64-entry publication cap, so 128/256-entry rows fail update with
`-E2BIG`; those rows require a rebooted kernel after refreshing the overlay cap
to 256.

## POST Payload Rows

These rows include ringbuf reservation, payload copy, and one ringbuf consumer.
They show the cost paid for POST events; DROP rows above return before reserve.

| prefixes | payload_B | ringbuf_record_B | compact POST M/s | compact POST ns/op | always POST M/s | always POST ns/op |
|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 300 | 344 | 2.141 +/- 0.021 | 467.07 | 2.460 +/- 0.018 | 406.50 |
| 1000 | 800 | 840 | 1.529 +/- 0.007 | 654.02 | 1.643 +/- 0.006 | 608.64 |
| 1000 | 2048 | 2088 | 0.921 +/- 0.002 | 1085.78 | 0.941 +/- 0.006 | 1062.70 |
| 10000 | 300 | 344 | 1.983 +/- 0.002 | 504.29 | 2.467 +/- 0.023 | 405.35 |
| 10000 | 800 | 840 | 1.519 +/- 0.006 | 658.33 | 1.640 +/- 0.004 | 609.76 |
| 10000 | 2048 | 2088 | 0.913 +/- 0.003 | 1095.29 | 0.956 +/- 0.010 | 1046.03 |

Weighted producer-side cost uses the 1000-prefix compact DROP row
(171.76 ns/op) and the compact POST payload rows above. Ringbuf emission is
only on the POST fraction.

| drop_pct | payload_B | compact prefilter avg ns/op | always POST ns/op | speedup | emitted ringbuf_B/op | ringbuf reduction |
|---:|---:|---:|---:|---:|---:|---:|
| 50 | 300 | 319.42 | 406.50 | 1.27x | 172.00 | 2.00x |
| 50 | 800 | 412.89 | 608.64 | 1.47x | 420.00 | 2.00x |
| 50 | 2048 | 628.77 | 1062.70 | 1.69x | 1044.00 | 2.00x |
| 80 | 300 | 230.82 | 406.50 | 1.76x | 68.80 | 5.00x |
| 80 | 800 | 268.21 | 608.64 | 2.27x | 168.00 | 5.00x |
| 80 | 2048 | 354.56 | 1062.70 | 3.00x | 417.60 | 5.00x |
| 90 | 300 | 201.29 | 406.50 | 2.02x | 34.40 | 10.00x |
| 90 | 800 | 220.99 | 608.64 | 2.75x | 84.00 | 10.00x |
| 90 | 2048 | 263.16 | 1062.70 | 4.04x | 208.80 | 10.00x |
| 95 | 300 | 186.53 | 406.50 | 2.18x | 17.20 | 20.00x |
| 95 | 800 | 195.87 | 608.64 | 3.11x | 42.00 | 20.00x |
| 95 | 2048 | 217.46 | 1062.70 | 4.89x | 104.40 | 20.00x |
| 99 | 300 | 174.71 | 406.50 | 2.33x | 3.44 | 100.00x |
| 99 | 800 | 176.58 | 608.64 | 3.45x | 8.40 | 100.00x |
| 99 | 2048 | 180.90 | 1062.70 | 5.87x | 20.88 | 100.00x |

Break-even DROP rate is low because POST rows pay normal reserve/copy work
while DROP rows return before reserve:

| payload_B | break-even DROP rate |
|---:|---:|
| 300 | 20.5% |
| 800 | 9.4% |
| 2048 | 2.5% |

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
- The comprehensive matrix was run with `scripts/run_kernel_bench_matrix.sh`;
  the WSL run wrote per-row logs and `matrix.tsv` under
  `/tmp/iograph-fullbench/matrix-full-v2`.
- The same run attempted 128/256-entry expanded blobs, but the booted overlay
  still rejected them at publication with `-E2BIG`; this is an overlay refresh
  and reboot issue, not a `run_action_idx()` load-path failure.
- The WSL environment exposes no CPU PMU device. Branch/cache/L1/LLC
  `perf_event_open` rows remain PMU-visible host work.

Representative command shape:

```sh
sh scripts/run_kernel_bench_matrix.sh /path/to/bench /tmp/iograph-fullbench /tmp/iograph-fullbench/matrix-full
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
- PMU-visible provenance fields for external runs: Linux overlay commit, bench
  binary build ID, and kernel config hash.

## Historical Byte-Trie Snapshot

The pre-compact kernel snapshot is retained in git history and summarized here
only for comparison:

| prefixes | selector case | byte-trie ns/op | LPM ns/op |
|---:|:---|---:|---:|
| 100 | matched prefix | 352.2 | 240.7 |
| 1000 | matched prefix | 355.9 | 290.9 |

Those rows are no longer the current `run_action()` path.
