# Userspace Proof Snapshot

Local validation:

```sh
cd tools/iog_bench
make validate-native
```

Environment for this snapshot: NixOS WSL2, Linux
6.6.114.1-microsoft-standard-WSL2, x86-64. Compiler environment was provided by
`nix shell nixpkgs#clang nixpkgs#gnumake`.

Hardware PMU counters were not exposed in this WSL run:

```text
perf_counters=unavailable(branch=No such file or directory cache=No such file or directory l1d=No such file or directory llc=No such file or directory)
```

The validation entrypoint reported `perf_event_paranoid=0` and
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
single-child byte chains into literal-run edges. Raw result files:

- `results/userspace/compact-chain-current.md`
- `results/userspace/compact-chain-10000.md`
- `results/userspace/compact-chain-shared-prefix-1000.md`
- `results/userspace/compact-chain-long-path-1000.md`

PMU counters were still unavailable in this WSL run, so branch/L1/LLC columns
remain `na`; rdtsc cycles and p95/p99/p999 batch timings were recorded.

### Compact Runtime Size

| dataset | prefixes | iog_blob_B | compact_runtime_B | compact_nodes | compact_edges | literal_edges | literal_bytes | max_literal_len | blob/compact |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | 44,592 | 6,472 | 151 | 150 | 137 | 1,576 | 17 | 6.89 |
| typical | 1000 | 346,264 | 56,506 | 1,379 | 1,378 | 1,329 | 12,314 | 17 | 6.13 |
| typical | 10000 | 869,472 | 253,476 | 6,955 | 6,954 | 6,757 | 30,852 | 17 | 3.43 |
| shared-prefix | 1000 | 201,756 | 42,740 | 1,112 | 1,111 | 1,001 | 7,092 | 92 | 4.72 |
| long-path | 1000 | 64,472 | 37,837 | 1,112 | 1,111 | 1,001 | 2,189 | 189 | 1.70 |

### Compact Decision Cost

| dataset | prefixes | case | byte-trie_ns | compact_ns | compact_p95 | compact_p99 | compact_p999 | speedup |
|:---|---:|:---|---:|---:|---:|---:|---:|---:|
| typical | 100 | late_miss | 145.51 | 36.97 | 50.14 | 57.46 | 271.24 | 3.94 |
| typical | 100 | hit | 177.31 | 34.16 | 36.03 | 47.32 | 227.26 | 5.19 |
| typical | 1000 | late_miss | 149.42 | 43.05 | 44.92 | 63.23 | 122.50 | 3.47 |
| typical | 1000 | hit | 200.97 | 49.64 | 61.78 | 85.57 | 277.31 | 4.05 |
| typical | 10000 | late_miss | 170.87 | 60.14 | 70.40 | 88.80 | 180.50 | 2.84 |
| typical | 10000 | hit | 235.38 | 62.59 | 64.53 | 111.60 | 511.70 | 3.76 |
| shared-prefix | 1000 | hit | 611.81 | 31.07 | 30.90 | 33.91 | 151.68 | 19.69 |
| long-path | 1000 | hit | 1047.64 | 31.91 | 31.92 | 34.43 | 187.91 | 32.83 |

### Matched Path Cost

`matched_transitions/op` is byte-trie state advances for byte-trie rows and
compact edge advances for compact rows.

| dataset | prefixes | case | matcher | input_B/op | matched_transitions/op | mean_ns/op | ns/input_B | ns/transition | p95 | p99 | p999 |
|:---|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | exact_short_match | byte-trie | 37.47 | 37.47 | 179.22 | 4.783 | 4.783 | 206.95 | 358.34 | 750.83 |
| typical | 100 | exact_short_match | compact | 37.47 | 5.44 | 34.93 | 0.932 | 6.424 | 36.19 | 53.98 | 237.28 |
| typical | 1000 | exact_short_match | byte-trie | 37.05 | 37.05 | 195.87 | 5.287 | 5.287 | 221.57 | 360.20 | 560.48 |
| typical | 1000 | exact_short_match | compact | 37.05 | 6.52 | 48.22 | 1.302 | 7.400 | 60.55 | 100.34 | 274.44 |
| typical | 10000 | exact_short_match | byte-trie | 37.62 | 37.62 | 215.84 | 5.737 | 5.737 | 221.16 | 469.40 | 2139.85 |
| typical | 10000 | exact_short_match | compact | 37.62 | 7.70 | 60.73 | 1.614 | 7.884 | 64.63 | 75.42 | 126.05 |
| shared-prefix | 1000 | exact_short_match | byte-trie | 101.00 | 101.00 | 538.50 | 5.332 | 5.332 | 653.37 | 988.20 | 1473.31 |
| shared-prefix | 1000 | exact_short_match | compact | 101.00 | 4.00 | 35.02 | 0.347 | 8.755 | 44.50 | 54.12 | 469.48 |
| long-path | 1000 | exact_long_match | byte-trie | 193.00 | 193.00 | 1131.54 | 5.863 | 5.863 | 1532.81 | 1926.99 | 4542.57 |
| long-path | 1000 | exact_long_match | compact | 193.00 | 4.00 | 33.22 | 0.172 | 8.306 | 31.80 | 47.21 | 1004.39 |

## Memory Overhead

The map update rows report active graph object memory, not only artifact bytes.

| prefixes | iog_blob_B | active_mem_B | active_over_blob_B |
|---:|---:|---:|---:|
| 100 | 44,592 | 44,720 | 128 |
| 1000 | 346,264 | 346,392 | 128 |
| 10000 | 869,472 | 869,600 | 128 |

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

The measured run path uses `iog_map_run_action()`. The update path copies the
blob, verifies it, builds an immutable graph object, publishes it, and retires
the old object. Reclaim is reported separately to keep post-RCU-grace-period
freeing out of policy activation latency.

| prefixes | verify_us | map_update_us | update_iters | active_mem_B | retired_graphs | retired_mem_B | total_mem_B | reclaim_us | reclaimed_graphs | update_seq |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 5.70 | 22.85 | 100 | 44,704 | 99 | 4,422,528 | 4,467,232 | 12.61 | 99 | 100 |
| 1000 | 43.41 | 58.96 | 20 | 346,376 | 19 | 6,580,536 | 6,926,912 | 2.22 | 19 | 20 |
| 10000 | 89.77 | 169.97 | 5 | 869,584 | 4 | 3,478,208 | 4,347,792 | 0.97 | 4 | 5 |

## BPF Map Ops

The map operations harness exercises alloc/update/delete/mem_usage and confirms
that lookup/get_next_key return `-ENOTSUP`, matching the specialized-container
map precedent used by ringbuf.

Smoke result after adding map ops:

| prefixes | key_size | value_size | max_entries | update_ret | lookup_ret | get_next_key_ret | delete_ret | mem_usage_B | update_seq | retired_after_delete | reclaimed_graphs |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 4 | 44,592 | 1 | 0 | -95 | -95 | 0 | 44,720 | 1 | 1 | 1 |

## Decision Cost

| prefixes | case | matcher | ns/op | cycles/op | run_allocs |
|---:|:---|:---|---:|---:|---:|
| 100 | early_miss | io_graph | 11.24 | needs rerun | 0 |
| 100 | late_miss | io_graph | 183.77 | needs rerun | 0 |
| 100 | hit | io_graph | 223.79 | needs rerun | 0 |
| 100 | late_miss | gen_chain | 445.95 | 1,198.72 | 0 |
| 100 | late_miss | list_loop | 232.66 | 625.40 | 0 |
| 1000 | early_miss | io_graph | 7.17 | needs rerun | 0 |
| 1000 | late_miss | io_graph | 192.63 | needs rerun | 0 |
| 1000 | hit | io_graph | 234.00 | needs rerun | 0 |
| 1000 | late_miss | gen_chain | 4,408.77 | 11,850.78 | 0 |
| 1000 | late_miss | list_loop | 2,188.21 | 5,881.93 | 0 |

## Decision Tail Batches

The expanded benchmark prints batch distributions for the policy primitive.
Selector strings are prebuilt before timing, and up to 1024 timed batches feed
the p95/p99/p999 columns.

| prefixes | case | matcher | mean_ns/op | batch_p95_ns/op | batch_p99_ns/op | batch_p999_ns/op |
|---:|:---|:---|---:|---:|---:|---:|
| 100 | late_miss | io_graph | 152.76 | 164.89 | 208.11 | 256.65 |
| 1000 | late_miss | io_graph | 163.10 | 188.72 | 285.03 | 458.47 |
| 10000 | early_miss | io_graph | 11.75 | 10.97 | 11.00 | 11.50 |
| 10000 | late_miss | io_graph | 164.42 | 163.78 | 184.72 | 462.69 |
| 10000 | hit | io_graph | 218.65 | 217.38 | 264.75 | 1,227.94 |

## Materialization Plus Decode

| prefixes | payload_B | record_B | materialize_ns/op | materialize_cycles/op |
|---:|---:|---:|---:|---:|
| 100 | 300 | 344 | 22.39 | 60.21 |
| 100 | 800 | 840 | 38.04 | 102.29 |
| 100 | 2048 | 2088 | 74.44 | 200.17 |
| 1000 | 300 | 344 | needs rerun | needs rerun |
| 1000 | 800 | 840 | needs rerun | needs rerun |
| 1000 | 2048 | 2088 | needs rerun | needs rerun |

## BPF Event Path

This table runs the graph before materialization and only copies a record for
POST. DROP rows emit 0 ringbuf bytes per event.

| prefixes | case | payload_B | ns/op | cycles/op | emitted_ringbuf_B/op | reserve_fail/op | run_allocs |
|---:|:---|---:|---:|---:|---:|---:|---:|
| 100 | early_miss | 800 | 7.98 | 21.46 | 0.00 | 0.000000 | 0 |
| 100 | late_miss | 800 | 182.36 | 490.19 | 0.00 | 0.000000 | 0 |
| 100 | hit | 800 | 256.65 | 689.88 | 840.00 | 0.000000 | 0 |

## Pre-Ringbuf Summary

Traffic reduction is exactly `1 / post_fraction`: 90% drop gives 10x, 95% gives
20x, 97% gives 33.3x, and 99% gives 100x. Rejected events contribute 0 ringbuf
bytes in the `io_graph` prefilter path.

Representative 1M events/sec rows:

| prefixes | neg_case | drop_pct | payload_B | before_ringbuf_MB/s | after_ringbuf_MB/s | avoided_intermediate_MB/s | iog_prefilter_ns | iog_cores | postdrop_cores | cores_saved_vs_postdrop | speedup_vs_list_prefilter |
|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | early_miss | 90 | 800 | 840.00 | 84.00 | 856.80 | 35.7 | 0.036 | 0.100 | 0.065 | 1.85 |
| 100 | early_miss | 95 | 800 | 840.00 | 42.00 | 904.40 | 22.8 | 0.023 | 0.097 | 0.074 | 2.66 |
| 100 | late_miss | 95 | 2048 | 2088.00 | 104.40 | 2090.00 | 187.2 | 0.187 | 0.324 | 0.137 | 1.35 |
| 1000 | early_miss | 95 | 800 | 840.00 | 42.00 | 904.40 | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun |
| 1000 | late_miss | 95 | 2048 | 2088.00 | 104.40 | 2090.00 | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun |
| 1000 | late_miss | 99 | 2048 | 2088.00 | 20.88 | 2178.00 | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun | previous full run needs rerun |

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
2088 MB/s to 104.4 MB/s. The latest full 1000-prefix matrix should be rerun
after the BPF map ops and event-path changes; the last full decision-cost run
still shows 1000-prefix decisions below 1 us/op.
