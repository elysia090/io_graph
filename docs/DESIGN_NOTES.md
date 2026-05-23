# Design Notes

The current implementation was checked against upstream Linux BPF code and
kernel documentation.

## Ringbuf

The BPF ring buffer documentation describes `BPF_MAP_TYPE_RINGBUF` as a BPF map
object that deliberately does not expose normal key/value semantics. It exposes
reserve/commit/discard helpers instead, and the verifier tracks reserved
records so a program cannot reserve and then forget to submit or discard. The
same document describes `bpf_ringbuf_reserve()` as avoiding an extra copy by
returning a pointer directly into ring buffer memory.

Source references:

- [BPF ring buffer documentation](https://www.kernel.org/doc/html/latest/bpf/ringbuf.html)
- [kernel/bpf/ringbuf.c](https://github.com/torvalds/linux/blob/master/kernel/bpf/ringbuf.c)

Design consequences for `io_graph`:

- `map_lookup_elem`, `map_update_elem`, `map_delete_elem`, and
  `map_get_next_key` do not have to be normal key/value operations when the map
  is a specialized container object.
- The event path must run `io_graph` before ringbuf reserve. Discard after
  reserve is not the target because it still allocates ringbuf space and writes
  metadata.
- Ringbuf record accounting includes the BPF ringbuf record header, not only the
  event payload struct.

## LPM_TRIE

The LPM trie documentation and source show a custom BPF map with `struct
bpf_map` embedded in the implementation object, custom allocation/update/delete,
BTF checking, memory accounting, and RCU-protected trie nodes.

Source references:

- [BPF_MAP_TYPE_LPM_TRIE documentation](https://docs.kernel.org/bpf/map_lpm_trie.html)
- [kernel/bpf/lpm_trie.c](https://github.com/torvalds/linux/blob/master/kernel/bpf/lpm_trie.c)

Design consequences for `io_graph`:

- The graph object is immutable after publication.
- Updates are whole-object replacement.
- Old graph lifetime is protected by RCU.
- Memory accounting is explicit and must include the active graph object and
  blob bytes.

## Current Kernel Candidate

`kernel/bpf/iograph_map.c` and `kernel/bpf/iograph_kfunc.c` apply those
precedents directly:

- embeds `struct bpf_map` in `struct bpf_iograph_map`;
- stores `struct bpf_iograph_graph __rcu *graph`;
- verifies and copies the blob on update;
- publishes with `rcu_assign_pointer`;
- frees old graphs with `call_rcu`;
- reports map memory usage;
- exposes action-only `bpf_iograph_run_action` for pre-emission filtering plus
  state-observing `bpf_iograph_run` and `bpf_iograph_step` entrypoints.

The action walker is intentionally not layered as `step()` followed by
`accept(next_state)` for every byte. That composition reloads the next node for
accept handling and then reloads it again as the current node for the next
transition. The kernel fast path keeps the current node pointer, consumes the
next byte from that node, then reuses the transitioned node on the next
iteration. It is also a separate loop from the terminal-state-observing
`run()` path, so the pre-emission kfunc does not carry a `final_state` output
through the fast walk. The published graph object caches the single-entry state
and input bound that the verified blob already fixed at update time.

The BPF-callable kfunc path also consumes the RCU read-side protection already
held by BPF execution instead of nesting one more `rcu_read_lock()` around
every io_graph call. This mirrors the map helper shape: Linux BPF lookup helpers
check that BPF execution already holds an RCU flavor before entering map
`lookup_elem`, and LPM trie lookup dereferences its published trie root under
that caller-side protection.

The hook-adjacent benchmark follows the same execution-order rule for its own
measurement state. Negative prefilter rows count syscall triggers in the
producer thread instead of incrementing a BPF global DROP counter after the
decision. The same bench exports an empty `raw_tp/sys_enter` hook floor row, so
syscall trigger, attach, and BPF dispatch cost remain visible without becoming
part of the policy decision primitive.
