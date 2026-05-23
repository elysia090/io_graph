# Compact Runtime Follow-Up

Date: 2026-05-23

This file tracks the compact-runtime follow-up. The current userspace
publication path now mirrors the kernel map publication path: the verified
byte-trie blob is copied and verified, accept IDs are inlined into action
codes, then a runtime-only compact graph folds non-accepting single-child byte
chains into literal-run edges. `run_action()` uses the published compact graph;
diagnostic `run()` and `step()` remain byte-trie paths.

Environment: WSL2 x86-64. PMU branch/cache counters were unavailable:

```text
perf_counters=unavailable(branch=No such file or directory cache=No such file or directory l1d=No such file or directory llc=No such file or directory)
```

Current publication-shim command:

```sh
./tools/iog_bench/iog_bench --counts 100,1000 --iters 100000
```

## Compact Runtime Stats

| prefixes | iog_blob_B | compact_runtime_B | compact_nodes | compact_edges | literal_edges | literal_bytes | mean_literal_len | max_literal_len | max_fanout | max_compact_depth | blob/compact |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 44,592 | 6,472 | 151 | 150 | 137 | 1,576 | 11.50 | 17 | 4 | 6 | 6.89 |
| 1000 | 346,264 | 56,506 | 1,379 | 1,378 | 1,329 | 12,314 | 9.27 | 17 | 8 | 8 | 6.13 |

## Publication Cost And Memory

The map update path now includes compact graph construction. Active memory is
split into the verified source blob and the published compact runtime object.
The active total also includes the userspace graph container overhead.

| prefixes | verify_us | compact_build_us | map_update_us | update_iters | active_blob_B | active_compact_B | active_total_B | retired_graphs | retired_mem_B | total_mem_B | reclaim_us | reclaimed_graphs | update_seq |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 5.60 | 20.49 | 66.27 | 100 | 44,592 | 6,472 | 51,200 | 99 | 5,065,632 | 5,116,832 | 41.31 | 99 | 100 |
| 1000 | 45.60 | 154.44 | 337.83 | 20 | 346,264 | 56,506 | 402,906 | 19 | 7,654,606 | 8,057,512 | 12.92 | 19 | 20 |

## Publication-Shim Decision Cost

These rows are the current kernel-shaped userspace shim: `io_graph_byte_trie`
calls the verified byte-trie interpreter directly for comparison, while
`io_graph_compact_chain` goes through the published map object and its compact
`run_action()` path. The previous sidecar-direct compact primitive was a little
faster; the current rows include the same object boundary used by the BPF-shaped
event path.

| prefixes | case | byte_trie_ns | compact_shim_ns | speedup |
|---:|:---|---:|---:|---:|
| 100 | early_miss | 12.67 | 10.69 | 1.19 |
| 100 | late_miss | 154.09 | 38.47 | 4.01 |
| 100 | hit | 179.06 | 35.35 | 5.07 |
| 1000 | early_miss | 12.12 | 9.77 | 1.24 |
| 1000 | late_miss | 174.20 | 60.04 | 2.90 |
| 1000 | hit | 202.24 | 59.84 | 3.38 |

## Publication-Shim Matched Path Cost

| prefixes | case | byte_trie_ns | compact_shim_ns | input_B/op | compact_transitions/op | compact_ns/input_B | compact_ns/transition |
|---:|:---|---:|---:|---:|---:|---:|---:|
| 100 | exact_short_match | 178.61 | 39.42 | 37.47 | 5.44 | 1.052 | 7.246 |
| 100 | prefix_longest_match | 179.06 | 35.35 | 46.47 | 5.44 | 0.761 | 6.498 |
| 1000 | exact_short_match | 183.64 | 47.06 | 37.05 | 6.52 | 1.270 | 7.218 |
| 1000 | prefix_longest_match | 202.24 | 59.84 | 46.05 | 6.52 | 1.300 | 9.178 |

## Direct Compact Primitive Tail Snapshot

The following percentile rows are from the earlier direct compact primitive
snapshot in the same WSL environment. They remain useful for tail shape, but
the publication-shim rows above are the current BPF-shaped userspace model.

| prefixes | case | byte_trie_ns | compact_ns | compact_p95 | compact_p99 | compact_p999 | speedup |
|---:|:---|---:|---:|---:|---:|---:|---:|
| 100 | early_miss | 11.08 | 10.84 | 13.98 | 16.35 | 28.30 | 1.02 |
| 100 | late_miss | 143.75 | 33.83 | 34.42 | 36.98 | 128.46 | 4.25 |
| 100 | hit | 169.66 | 33.32 | 33.19 | 40.15 | 161.08 | 5.09 |
| 1000 | early_miss | 9.48 | 11.12 | 11.87 | 14.73 | 176.65 | 0.85 |
| 1000 | late_miss | 147.65 | 43.97 | 44.54 | 45.81 | 140.26 | 3.36 |
| 1000 | hit | 185.36 | 45.44 | 46.21 | 50.95 | 140.78 | 4.08 |

## Direct Compact Primitive Matched Path Tail Snapshot

| prefixes | case | matcher | input_B/op | matched_transitions/op | mean_ns/op | ns/input_B | ns/transition | p95 | p99 | p999 |
|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | exact_short_match | byte-trie | 37.47 | 37.47 | 169.07 | 4.512 | 4.512 | 168.37 | 248.71 | 349.55 |
| 100 | exact_short_match | compact | 37.47 | 5.44 | 33.83 | 0.903 | 6.221 | 35.01 | 38.07 | 129.98 |
| 100 | prefix_longest_match | byte-trie | 46.47 | 37.47 | 169.66 | 3.651 | 4.528 | 169.05 | 262.22 | 549.51 |
| 100 | prefix_longest_match | compact | 46.47 | 5.44 | 33.32 | 0.717 | 6.128 | 33.19 | 40.15 | 161.08 |
| 1000 | exact_short_match | byte-trie | 37.05 | 37.05 | 181.82 | 4.908 | 4.908 | 182.57 | 263.51 | 412.07 |
| 1000 | exact_short_match | compact | 37.05 | 6.52 | 45.59 | 1.231 | 6.997 | 45.82 | 52.95 | 158.12 |
| 1000 | prefix_longest_match | byte-trie | 46.05 | 37.05 | 185.36 | 4.026 | 5.004 | 184.91 | 332.55 | 835.34 |
| 1000 | prefix_longest_match | compact | 46.05 | 6.52 | 45.44 | 0.987 | 6.975 | 46.21 | 50.95 | 140.78 |

The early miss remains close to the floor. The matched path is the important
change: the current publication-shim run cuts typical 100-prefix hit cost by
about 5.1x and typical 1000-prefix hit cost by about 3.4x. The direct compact
primitive tail snapshot remains slightly faster because it bypasses the
published map-object boundary.
