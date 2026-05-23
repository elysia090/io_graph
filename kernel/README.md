# Kernel

The active implementation lives here.

- `include/uapi/linux/bpf_iograph.h`: blob ABI header copied into a Linux tree.
- `bpf/iograph_map.c`: map allocation, update, verifier, RCU graph
  lifetime, memory accounting.
- `bpf/iograph_kfunc.c`: interpreter and SPEC v0 kfunc API.
- `selftests/bpf/progs/` and `selftests/bpf/prog_tests/`: BPF selftests.
- `selftests/bpf/benchs/`: Linux BPF bench sources for compiled blobs.

The source follows Linux BPF conventions from LPM trie map lifetime and ringbuf
container operations. It is not yet an upstream patch series; integration notes
are kept in `INTEGRATION.md`.

Current kernel-side v0 coverage:

- map allocation accepts one fixed-size graph slot and caps blobs at 8 MiB;
- map update verifies the immutable blob before RCU publication;
- `bpf_iograph_run_action()` returns the prefilter action without a BPF-side
  result object;
- `bpf_iograph_run_action()` is fail-open for prefilter use: missing graph,
  invalid entry, too-long input, and no match all return action `0`; the
  diagnostic `run()` path returns typed errors instead;
- the action-only kfunc uses a dedicated walk loop rather than the
  terminal-state-observing `run()` loop;
- the action walker carries the current runtime node across input bytes instead
  of re-resolving the node for both transition and accept handling;
- graph publication rewrites accept IDs in the copied runtime object into
  action codes, so accepting nodes do not need a second `accepts[]` lookup on
  the run path;
- graph publication also builds a runtime-only compact graph from the verified
  byte-trie blob by folding single-child byte chains into literal-run edges;
- `bpf_iograph_run_action()` uses the compact runtime graph, while
  `bpf_iograph_run()` and `bpf_iograph_step()` keep the byte-trie path for
  validation, final-state observation, and single-step diagnostics;
- accepting leaf nodes carry `IOG_NODE_F_FINAL_ACTION`, letting `run_action`
  return at a final prefix without changing longest-match semantics for
  accepting nodes that still have outgoing edges;
- `default_dst` is v0's consuming else transition, not a non-consuming fallback
  chain;
- BPF kfunc reads reuse the BPF program's existing RCU read-side protection
  instead of nesting another RCU section per graph decision;
- kfunc registrations are marked `KF_RCU_PROTECTED` so callers must already be
  in an RCU-protected BPF execution context;
- hook-adjacent DROP benches count producer triggers outside BPF in batches so
  the rejected path does not pay a benchmark-only global counter update or a
  per-trigger userspace atomic;
- the selftests bench exposes an empty same-hook floor row beside decision and
  prefilter rows to isolate attach, syscall trigger, and BPF dispatch cost;
- the selftests bench also exposes `iograph-compact-decision` and
  `iograph-compact-prefilter` aliases so refreshed kernel results can name the
  current compact `run_action()` runtime explicitly;
- map update caches single-entry state and the input bound in the published
  graph object for the common prefilter entry path;
- map memory accounting includes the copied blob plus compact entries, compact
  nodes, compact edges, and literal bytes;
- `bpf_iograph_run()` keeps final-state output for validation and debugging;
- `bpf_iograph_step()` exposes one verified transition;
- kfunc registration covers the tracing selftest path and the raw tracepoint
  path used by the low-overhead prefilter bench;
- selftests include invalid blob updates and the DROP-before-reserve path.
