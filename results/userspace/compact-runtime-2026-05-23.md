# Compact Runtime Follow-Up

Date: 2026-05-23

This run validates the runtime shape now mirrored by the kernel map
publication path: the verified byte-trie blob is copied and verified, then a
runtime-only compact graph folds non-accepting single-child byte chains into
literal-run edges. `run_action()` uses the compact graph; diagnostic `run()`
and `step()` remain byte-trie paths.

Environment: WSL2 x86-64. PMU branch/cache counters were unavailable:

```text
perf_counters=unavailable(branch=No such file or directory cache=No such file or directory l1d=No such file or directory llc=No such file or directory)
```

Command:

```sh
./tools/iog_bench/iog_bench --counts 100,1000 --iters 100000
```

## Compact Runtime Stats

| prefixes | iog_blob_B | compact_runtime_B | compact_nodes | compact_edges | literal_edges | literal_bytes | mean_literal_len | max_literal_len | max_fanout | max_compact_depth | blob/compact |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 44,592 | 6,472 | 151 | 150 | 137 | 1,576 | 11.50 | 17 | 4 | 6 | 6.89 |
| 1000 | 346,264 | 56,506 | 1,379 | 1,378 | 1,329 | 12,314 | 9.27 | 17 | 8 | 8 | 6.13 |

## Decision Cost

| prefixes | case | byte_trie_ns | compact_ns | compact_p95 | compact_p99 | compact_p999 | speedup |
|---:|:---|---:|---:|---:|---:|---:|---:|
| 100 | early_miss | 11.08 | 10.84 | 13.98 | 16.35 | 28.30 | 1.02 |
| 100 | late_miss | 143.75 | 33.83 | 34.42 | 36.98 | 128.46 | 4.25 |
| 100 | hit | 169.66 | 33.32 | 33.19 | 40.15 | 161.08 | 5.09 |
| 1000 | early_miss | 9.48 | 11.12 | 11.87 | 14.73 | 176.65 | 0.85 |
| 1000 | late_miss | 147.65 | 43.97 | 44.54 | 45.81 | 140.26 | 3.36 |
| 1000 | hit | 185.36 | 45.44 | 46.21 | 50.95 | 140.78 | 4.08 |

## Matched Path Cost

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
change: compact runtime graph traversal cuts typical 100-prefix hit cost by
about 5.1x and typical 1000-prefix hit cost by about 4.1x in this run.
