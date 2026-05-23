# Kernel-Shaped Harness

This harness exercises the same policy object contract as the kernel sources in
`kernel/bpf/`:

1. `map_alloc`: accept one graph slot with `key_size = sizeof(u32)`,
   `max_entries = 1`, and fixed `value_size = blob_len`;
2. `map_update_elem`: copy the value blob, verify it, build an immutable graph,
   publish it, and retire the old graph;
3. `map_delete_elem`: clear the active graph and retire it;
4. `map_lookup_elem` and `map_get_next_key`: return unsupported, matching the
   container-object precedent used by BPF ringbuf;
5. `map_mem_usage`: include the map object plus active and retired graph bytes;
6. `bpf_iograph_run`: return a small action code without run-path allocation.

The userspace harness is not a substitute for kernel selftests. It exists to
pin down the object lifetime, update semantics, and benchmark accounting before
the Linux tree integration.

## Verifier Selftests

Each compiled blob is cloned and corrupted before timing begins. The negative
cases currently cover:

- bad magic;
- non-canonical section order;
- out-of-bounds node edge range;
- out-of-bounds edge destination;
- out-of-bounds accept id;
- consuming-else self-loop;
- unsorted/overlapping edges;
- input length above verifier limits.

These tests are intentionally aligned with the verifier in
`kernel/bpf/iograph_map.c`.

## Interpreter

The interpreter is the required pre-JIT execution path:

- single-edge fast path for the common trie node case;
- small linear scan for low fanout nodes;
- binary search for larger sorted edge sets.

The graph stays compact verified data. No dense transition table is built on
the run path.
