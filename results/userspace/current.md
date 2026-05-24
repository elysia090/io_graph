# Userspace Proof Snapshot

Local validation:

```sh
nix shell nixpkgs#clang nixpkgs#gnumake -c make all CC=clang
nix shell nixpkgs#clang nixpkgs#gnumake -c make test CC=clang
nix shell nixpkgs#clang nixpkgs#gnumake -c ./tools/iog_bench/iog_bench --counts 100,1000 --dataset typical --iters 100000
nix shell nixpkgs#clang nixpkgs#gnumake -c ./tools/iog_bench/iog_bench --counts 1000 --dataset shared-prefix --iters 100000
nix shell nixpkgs#clang nixpkgs#gnumake -c ./tools/iog_bench/iog_bench --counts 1000 --dataset long-path --iters 100000
```

Environment for this snapshot: NixOS WSL2, Linux
6.6.114.1-microsoft-standard-WSL2, x86-64. Compiler environment was provided by
`nix shell nixpkgs#clang nixpkgs#gnumake`.

Hardware PMU counters were not exposed in this WSL run:

```text
perf_counters=unavailable(branch=No such file or directory cache=No such file or directory l1d=No such file or directory llc=No such file or directory)
```

The validation entrypoint reported `perf_event_paranoid=1` and
`pmu_cpu_type=unavailable`, so branch/L1/LLC miss columns report `na`.
`rdtsc` cycle estimates were available. Native bare-metal Linux should fill the
counter columns when the CPU PMU exposes those events.

## IO-Aware Object Accounting

The benchmark makes the avoided intermediate object explicit. The naive
pipeline builds a ringbuf record and then a decoded userspace event before
dropping. The `io_graph` path keeps only `state,cursor,action_code` and
materializes only POST events.

| payload_B | event_record_B | bpf_ringbuf_record_B | decoded_event_B | postdrop_intermediate_B | iog_run_state_B | drop_path_ringbuf_B |
|---:|---:|---:|---:|---:|---:|---:|
| 300 | 336 | 344 | 112 | 456 | 12 | 0 |
| 800 | 832 | 840 | 112 | 952 | 12 | 0 |
| 2048 | 2080 | 2088 | 112 | 2200 | 12 | 0 |

## Artifact Sizes

| prefixes | avg_len | states | edges | iog_blob_B | dense_table_B | gen_chain_src_B | gen_chain_bpf_est_B | list_payload_B | dense/iog | gen_bpf/iog |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 37.5 | 1,590 | 1,589 | 44,592 | 1,634,520 | 9,027 | 93,320 | 4,555 | 36.7 | 2.1 |
| 1000 | 37.5 | 12,364 | 12,363 | 346,264 | 12,710,192 | 89,572 | 932,000 | 45,500 | 36.7 | 2.7 |
| 10000 | 37.5 | 31,050 | 31,049 | 869,472 | 31,919,400 | 895,072 | 9,320,000 | 455,000 | 36.7 | 10.7 |

## Runtime-Only Chain Compression

The next optimization now exists as a userspace runtime object:

```text
verified byte-trie blob -> compact run_action graph
```

The blob format is unchanged. The builder keeps branch, entry, accepting,
flagged, else-transition, and shared-continuation states, then folds
single-child byte chains into literal-run edges. This file is the stable source
of truth for those rows; parallel work-note snapshots are intentionally not
kept.

PMU counters were still unavailable in this WSL run, so branch/L1/LLC columns
remain `na`; rdtsc cycles and p95/p99/p999 batch timings were recorded.
The current publication-shaped compact shim and update/memory split are folded
into this file. The BPF-shaped userspace map update now verifies the byte-trie
blob, inlines accept IDs into action codes, builds the compact runtime graph,
and routes `run_action()` through that published compact graph.

The latest hot-path implementation stores only literal tail bytes, lets
terminal `IOG_NODE_F_FINAL_ACTION` leaves return from incoming compact edges,
and prunes non-entry terminal final-action leaves from the compact node array.
IO-aware pre-emission accounting is also folded here: selector acquisition
bounds, action-only active memory, update scratch/peak bytes,
drop-before-reserve versus discard-after-reserve, and dirty-cacheline
accounting are part of the stable result entrypoint rather than separate work
notes.

### Compact Runtime Size

This table reflects literal tail-only storage, final-action edge returns, and
terminal final-action leaf pruning. The verified byte-trie blob is unchanged;
only the publication-time compact runtime object changes.

| dataset | prefixes | iog_blob_B | compact_runtime_B | compact_nodes | compact_edges | literal_edges | dispatch_tables | literal_tail_B | mean_tail_len | max_tail_len | blob/compact |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | 44,592 | 4,947 | 51 | 150 | 137 | 0 | 1,439 | 10.50 | 16 | 9.01 |
| typical | 1000 | 346,264 | 40,701 | 379 | 1,378 | 1,329 | 0 | 10,985 | 8.27 | 16 | 8.51 |
| typical | 10000 | 869,472 | 172,147 | 1,835 | 6,954 | 6,757 | 0 | 24,095 | 3.57 | 16 | 5.05 |
| shared-prefix | 1000 | 201,756 | 26,195 | 112 | 1,111 | 1,001 | 0 | 6,091 | 6.08 | 91 | 7.70 |
| long-path | 1000 | 64,472 | 21,292 | 112 | 1,111 | 1,001 | 0 | 1,188 | 1.19 | 188 | 3.03 |

### Compact Decision Cost

These rows are the compact action runtime after tail-only literal storage and
final-action edge returns. The latency shift is a constant-factor pass; the
main win is the smaller hot working set above.

| dataset | prefixes | case | byte_trie_ns | compact_ns | compact_p95 | compact_p99 | compact_p999 | speedup |
|:---|---:|:---|---:|---:|---:|---:|---:|---:|
| typical | 100 | hit | 228.11 | 29.47 | 29.38 | 30.30 | 128.04 | 7.74 |
| typical | 1000 | hit | 239.75 | 41.33 | 39.71 | 54.18 | 413.89 | 5.80 |
| typical | 10000 | hit | 204.17 | 61.02 | 60.12 | 68.62 | 287.19 | 3.35 |
| shared-prefix | 1000 | hit | 741.63 | 29.36 | 29.44 | 32.68 | 228.29 | 25.26 |
| long-path | 1000 | hit | 1,475.96 | 28.46 | 28.47 | 30.87 | 217.64 | 51.86 |

### Matched Path Cost

`matched_transitions/op` is compact edge advances. Literal-run edges can
consume many input bytes in one transition, so both `ns/input_B` and
`ns/transition` are reported.

| dataset | prefixes | case | input_B/op | matched_transitions/op | mean_ns/op | ns/input_B | ns/transition | p95 | p99 | p999 |
|:---|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | exact_short_match | 37.47 | 5.44 | 29.24 | 0.780 | 5.375 | 29.38 | 30.30 | 128.04 |
| typical | 1000 | exact_short_match | 37.05 | 6.52 | 40.50 | 1.093 | 6.212 | 39.71 | 54.18 | 413.89 |
| typical | 10000 | exact_short_match | 37.62 | 7.70 | 60.73 | 1.614 | 7.884 | 64.63 | 75.42 | 126.05 |
| shared-prefix | 1000 | exact_short_match | 101.00 | 4.00 | 29.63 | 0.293 | 7.408 | 29.44 | 32.68 | 228.29 |
| long-path | 1000 | exact_long_match | 193.00 | 4.00 | 28.62 | 0.148 | 7.155 | 28.47 | 30.87 | 217.64 |

## Memory Overhead

The active publication object retains the verified source blob for diagnostic
byte-trie `run()` and `step()` unless action-only mode is requested. The
current split is:

```text
source blob bytes
+ compact runtime bytes
+ graph/map container overhead
```

| dataset | prefixes | active_blob_B | active_compact_B | active_total_B | action_only_mem_B |
|:---|---:|---:|---:|---:|---:|
| typical | 100 | 44,592 | 4,947 | 49,675 | 5,099 |
| typical | 1000 | 346,264 | 40,701 | 387,101 | 40,853 |
| typical | 10000 | 869,472 | 172,147 | 1,041,755 | 172,299 |
| shared-prefix | 1000 | 201,756 | 26,195 | 228,087 | 26,347 |
| long-path | 1000 | 64,472 | 21,292 | 85,900 | 21,444 |

## Verifier And Layout

Each compiled blob passed 8 negative verifier selftests after intentional
corruption.

| prefixes | negative_cases | result |
|---:|---:|:---|
| 100 | 8 | ok |
| 1000 | 8 | ok |
| 10000 | 8 | ok |

| prefixes | zero_edge_nodes | single_edge_nodes | small_fanout_nodes | binary_fanout_nodes | max_fanout |
|---:|---:|---:|---:|---:|---:|
| 100 | 100 | 1,440 | 50 | 0 | 4 |
| 1000 | 1,000 | 10,986 | 346 | 32 | 8 |
| 10000 | 5,120 | 24,096 | 1,674 | 160 | 8 |

Most nodes are single-edge trie nodes, which supports the interpreter's
single-edge fast path before any JIT work.

## BPF Map Update

The map update path verifies the byte-trie blob, inlines accept codes, builds
the compact runtime graph, and then publishes the new object. Reclaim remains
reported separately to keep post-RCU-grace-period freeing out of policy
activation latency. Peak estimates include update scratch and the new object;
`peak_with_retired_B` additionally includes old graphs waiting for reclamation.

| dataset | prefixes | verify_us | compact_build_us | map_update_us | active_total_B | update_scratch_B | peak_new_update_B |
|:---|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | 6.14 | 25.15 | 61.87 | 49,675 | 14,310 | 63,985 |
| typical | 1000 | 41.34 | 177.19 | 339.05 | 387,101 | 111,276 | 498,377 |
| typical | 10000 | 102.79 | 359.72 | 575.68 | 1,041,755 | 279,450 | 1,321,205 |
| shared-prefix | 1000 | 21.42 | 87.80 | 245.24 | 228,087 | 64,827 | 292,914 |
| long-path | 1000 | 7.41 | 36.23 | 76.25 | 85,900 | 20,700 | 106,600 |

## BPF Map Ops

The map operations harness exercises alloc/update/delete/mem_usage and confirms
that lookup/get_next_key return `-ENOTSUP`, matching the specialized-container
map precedent used by ringbuf.

Smoke result after adding map ops:

| prefixes | key_size | value_size | max_entries | update_ret | lookup_ret | get_next_key_ret | delete_ret | mem_usage_B | update_seq | retired_after_delete | reclaimed_graphs |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 4 | 44,592 | 1 | 0 | -95 | -95 | 0 | 44,720 | 1 | 1 | 1 |

Action-only mode verifies the same source blob and publishes the compact
runtime graph, then discards the retained source blob from the active object.
`run_action()` remains available; byte-trie `run()` and active byte-graph
inspection return no graph.

| dataset | prefixes | retained_mem_B | action_only_mem_B | run_action | diagnostic_run |
|:---|---:|---:|---:|---:|:---|
| typical | 100 | 49,675 | 5,099 | ok | no graph |
| typical | 1000 | 387,101 | 40,853 | ok | no graph |
| typical | 10000 | 1,041,755 | 172,299 | ok | no graph |
| shared-prefix | 1000 | 228,087 | 26,347 | ok | no graph |
| long-path | 1000 | 85,900 | 21,444 | ok | no graph |

## Materialization Plus Decode

| prefixes | payload_B | record_B | materialize_ns/op | materialize_cycles/op |
|---:|---:|---:|---:|---:|
| 100 | 300 | 344 | 22.39 | 60.21 |
| 100 | 800 | 840 | 38.04 | 102.29 |
| 100 | 2048 | 2088 | 74.44 | 200.17 |
| 1000 | 300 | 344 | 23.18 | na |
| 1000 | 800 | 840 | 39.38 | na |
| 1000 | 2048 | 2088 | 84.03 | na |

## BPF Event Path

The table printed by `tools/iog_bench/iog_bench` is named
`bpf event path (compact run_action shim)`: it runs the published compact graph
before materialization and only copies a record for POST. DROP rows emit
0 ringbuf bytes per event.

| dataset | prefixes | case | payload_B | ns/op | emitted_ringbuf_B/op | reserve_fail/op | run_allocs |
|:---|---:|:---|---:|---:|---:|---:|---:|
| typical | 100 | early_miss | 300 | 7.51 | 0.00 | 0.000000 | 0 |
| typical | 100 | late_miss | 300 | 29.40 | 0.00 | 0.000000 | 0 |
| typical | 100 | hit | 300 | 36.87 | 344.00 | 0.000000 | 0 |
| typical | 100 | hit | 800 | 46.67 | 840.00 | 0.000000 | 0 |
| typical | 100 | hit | 2048 | 56.60 | 2088.00 | 0.000000 | 0 |
| typical | 1000 | early_miss | 300 | 8.62 | 0.00 | 0.000000 | 0 |
| typical | 1000 | late_miss | 300 | 45.20 | 0.00 | 0.000000 | 0 |
| typical | 1000 | hit | 300 | 51.90 | 344.00 | 0.000000 | 0 |
| typical | 1000 | hit | 800 | 54.67 | 840.00 | 0.000000 | 0 |
| typical | 1000 | hit | 2048 | 67.49 | 2088.00 | 0.000000 | 0 |
| shared-prefix | 1000 | hit | 300 | 39.02 | 344.00 | 0.000000 | 0 |
| shared-prefix | 1000 | hit | 800 | 53.89 | 840.00 | 0.000000 | 0 |
| shared-prefix | 1000 | hit | 2048 | 54.10 | 2088.00 | 0.000000 | 0 |
| long-path | 1000 | hit | 300 | 36.54 | 344.00 | 0.000000 | 0 |
| long-path | 1000 | hit | 800 | 41.25 | 840.00 | 0.000000 | 0 |
| long-path | 1000 | hit | 2048 | 54.84 | 2088.00 | 0.000000 | 0 |

## Selector Acquisition And Drop Order

Prefix policy does not need full path or command-line acquisition on the reject
path. The compiler reports `max_prefix_len` and `max_probe_len`, where
`max_probe_len = min(max_input_len, max_prefix_len + 1)`, so a caller can copy
only the bytes needed to distinguish a longer override before running the
graph.

| dataset | prefixes | max_prefix_len | max_probe_len |
|:---|---:|---:|---:|
| typical | 1000 | 43 | 44 |
| shared-prefix | 1000 | 101 | 102 |
| long-path | 1000 | 193 | 194 |

Selector acquisition rows copy bytes into a bounded local buffer before
`run_action()`. Full-copy and bounded-probe rows let the report distinguish
policy evaluation from input acquisition cost.

Drop order rows model the difference between deciding before reserve and
reserving a ringbuf record that is later discarded.

| dataset | prefixes | case | payload_B | drop_before_reserve_ns | discard_after_reserve_ns | drop_before_reserve_B | discard_after_reserve_B |
|:---|---:|:---|---:|---:|---:|---:|---:|
| typical | 1000 | early reject | 300 | 7.67 | 15.53 | 0 | 344 |
| typical | 1000 | late reject | 300 | 41.17 | 49.81 | 0 | 344 |
| shared-prefix | 1000 | early reject | 300 | 4.72 | 14.35 | 0 | 344 |
| shared-prefix | 1000 | late reject | 300 | 29.55 | 40.62 | 0 | 344 |
| long-path | 1000 | early reject | 300 | 5.23 | 14.48 | 0 | 344 |
| long-path | 1000 | late reject | 300 | 34.89 | 40.07 | 0 | 344 |

At 1M events/sec and 95% reject, 0-byte rejects avoid the following producer
ringbuf traffic and dirty cacheline writes:

| payload_B | ringbuf_record_B | record_cachelines | avoided_dirty_cachelines_per_sec |
|---:|---:|---:|---:|
| 300 | 344 | 6 | 5.70M |
| 800 | 840 | 14 | 13.30M |
| 2048 | 2088 | 33 | 31.35M |

## Pre-Ringbuf Summary

Traffic reduction is exactly `1 / post_fraction`: 90% drop gives 10x, 95% gives
20x, 97% gives 33.3x, and 99% gives 100x. Rejected events contribute 0 ringbuf
bytes in the `io_graph` prefilter path.

At 1M events/sec the traffic side is deterministic once payload size and
drop rate are chosen:

| drop_pct | post_fraction | traffic_reduction | 800B_before_MB/s | 800B_after_MB/s | 2048B_before_MB/s | 2048B_after_MB/s |
|---:|---:|---:|---:|---:|---:|---:|
| 90 | 0.10 | 10.0x | 840.00 | 84.00 | 2088.00 | 208.80 |
| 95 | 0.05 | 20.0x | 840.00 | 42.00 | 2088.00 | 104.40 |
| 97 | 0.03 | 33.3x | 840.00 | 25.20 | 2088.00 | 62.64 |
| 99 | 0.01 | 100.0x | 840.00 | 8.40 | 2088.00 | 20.88 |

## Reading

The representation result remains intact: 100 prefixes fit in 44.6 KiB, 1000
prefixes fit in 346.3 KiB, and this generated 10000-prefix set fits in
869.5 KiB. Dense table size is 36.7x larger for this workload, while the
generated-chain BPF lower-bound estimate is 2.1x larger at 100 prefixes, 2.7x
larger at 1000 prefixes, and 10.7x larger at 10000 prefixes.

The pre-emission decision remains well below the target: 100-prefix decisions
are below 500 ns/op even for late mismatch and hit paths, and 1000-prefix
decisions are below 1 us/op. In contrast, the 1000-prefix generated-chain and
list-loop late mismatch baselines are 4.41 us/op and 2.19 us/op.

The second thesis is also supported: when the graph runs before materialization,
rejected events become 0 ringbuf bytes. With BPF ringbuf header accounting, at
1M events/sec with 95% drop and 2 KiB payloads, ringbuf traffic falls from
2088 MB/s to 104.4 MB/s. The current userspace harness now also reports the
bounded selector acquisition row, discard-after-reserve baseline, action-only
active memory, and update scratch/peak bytes needed to explain where the I/O
win comes from.
