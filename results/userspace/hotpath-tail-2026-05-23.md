# Compact Hot-Path Tail Follow-Up, 2026-05-23

This run covers the next compact-runtime constant-factor pass:

- literal-run edges store only tail bytes after the first dispatch byte;
- terminal `IOG_NODE_F_FINAL_ACTION` leaves are represented as final-action
  edges in the compact action graph;
- non-entry, non-default-target terminal final-action leaves are pruned from
  the compact node array.

The verified byte-trie blob is unchanged. Diagnostic `run()` and `step()` keep
the byte-trie graph; the action-only compact graph is the pre-emission hot path.

Raw logs:

- `results/userspace/raw/hotpath-tail-typical-2026-05-23.md`
- `results/userspace/raw/hotpath-tail-shared-prefix-2026-05-23.md`
- `results/userspace/raw/hotpath-tail-long-path-2026-05-23.md`

PMU counters were not exposed in this WSL run, so branch/L1/LLC columns remain
`na`. Batch p95/p99/p999 rows are still recorded.

## Runtime Size

| dataset | prefixes | iog_blob_B | compact_runtime_B | compact_nodes | compact_edges | literal_edges | literal_tail_B | mean_tail_len | max_tail_len | blob/compact |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | 44,592 | 4,735 | 51 | 150 | 137 | 1,439 | 10.50 | 16 | 9.42 |
| typical | 1000 | 346,264 | 39,177 | 379 | 1,378 | 1,329 | 10,985 | 8.27 | 16 | 8.84 |
| typical | 10000 | 869,472 | 164,799 | 1,835 | 6,954 | 6,757 | 24,095 | 3.57 | 16 | 5.28 |
| shared-prefix | 1000 | 201,756 | 25,739 | 112 | 1,111 | 1,001 | 6,091 | 6.08 | 91 | 7.84 |
| long-path | 1000 | 64,472 | 20,836 | 112 | 1,111 | 1,001 | 1,188 | 1.19 | 188 | 3.09 |

Compared with the previous compact runtime, this removes one literal byte per
literal edge and prunes terminal final-action cnode leaves. The 1000-prefix
typical active compact graph drops from 56,506 B in the first compact-chain
run to 39,177 B here.

## Decision Cost

| dataset | prefixes | case | byte_trie_ns | compact_ns | compact_p95 | compact_p99 | compact_p999 | speedup |
|:---|---:|:---|---:|---:|---:|---:|---:|---:|
| typical | 100 | hit | 173.24 | 36.54 | 38.76 | 76.58 | 126.24 | 4.74 |
| typical | 1000 | hit | 189.67 | 47.12 | 46.47 | 90.90 | 348.33 | 4.03 |
| typical | 10000 | hit | 204.17 | 61.02 | 60.12 | 68.62 | 287.19 | 3.35 |
| shared-prefix | 1000 | hit | 526.04 | 34.15 | 32.48 | 110.34 | 213.45 | 15.40 |
| long-path | 1000 | hit | 1,035.19 | 34.05 | 33.56 | 106.92 | 305.86 | 30.40 |

The latency change is intentionally treated as a small constant-factor result,
not another algorithmic jump. The main win here is hot working-set reduction:
the compact action path no longer materializes terminal leaf nodes or compares
the first literal byte twice.

## Matched Path

| dataset | prefixes | case | matcher | input_B/op | compact_transitions/op | mean_ns/op | ns/input_B | ns/transition | p95 | p99 | p999 |
|:---|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | exact_short_match | compact | 37.47 | 5.44 | 36.10 | 0.963 | 6.639 | 37.93 | 62.24 | 121.80 |
| typical | 1000 | exact_short_match | compact | 37.05 | 6.52 | 46.89 | 1.266 | 7.196 | 47.87 | 89.02 | 158.23 |
| typical | 10000 | exact_short_match | compact | 37.62 | 7.70 | 61.76 | 1.641 | 8.017 | 60.12 | 130.72 | 687.28 |
| shared-prefix | 1000 | exact_short_match | compact | 101.00 | 4.00 | 32.90 | 0.326 | 8.225 | 33.35 | 46.43 | 147.82 |
| long-path | 1000 | exact_long_match | compact | 193.00 | 4.00 | 32.73 | 0.170 | 8.184 | 32.09 | 67.87 | 265.34 |

## Update And Active Memory

| dataset | prefixes | verify_us | compact_build_us | map_update_us | active_blob_B | active_compact_B | active_total_B |
|:---|---:|---:|---:|---:|---:|---:|---:|
| typical | 100 | 7.76 | 34.09 | 54.66 | 44,592 | 4,735 | 49,463 |
| typical | 1000 | 49.74 | 197.12 | 360.51 | 346,264 | 39,177 | 385,577 |
| typical | 10000 | 102.79 | 359.72 | 575.68 | 869,472 | 164,799 | 1,034,407 |
| shared-prefix | 1000 | 44.79 | 183.85 | 361.72 | 201,756 | 25,739 | 227,631 |
| long-path | 1000 | 8.48 | 42.75 | 72.07 | 64,472 | 20,836 | 85,444 |

The active object still keeps the verified source blob for diagnostic
byte-trie `run()` and `step()`. An action-only map mode that discards the source
blob after compact publication remains a separate memory experiment.
