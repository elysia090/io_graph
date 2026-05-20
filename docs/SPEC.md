io_graph SPEC.md

Summary

io_graph is a BPF map-backed finite-automaton object for high-cardinality kernel-side filtering.

It is designed for observability and security agents that need to drop, classify, or route events before ring buffer allocation and userspace rule evaluation.

The initial target is scalable string, prefix, suffix, and small-pattern matching over event fields such as file paths, executable paths, command lines, and selected metadata.

io_graph is not a replacement for BPF programs. It is a compact, hot-swappable data object used by BPF programs.

Problem Statement

Production BPF observability and security agents frequently evaluate simple but high-cardinality filters over high-volume event streams.

Examples include:

1. file path prefix filters,
2. executable path filters,
3. command-line filters,
4. event argument string filters,
5. metadata classifiers,
6. first-match policy actions.

The common goal is to avoid sending uninteresting events to userspace.

Existing approaches are insufficient:

1. generated BPF branch code grows with the number of patterns,
2. bounded selector arrays hit verifier and code-size limits,
3. exact-match BPF maps do not solve prefix or small-pattern matching,
4. dense transition tables can be too large,
5. userspace filtering happens after ring buffer allocation and does not reduce kernel-to-userspace I/O,
6. regex-like engines are too heavy for the regular subset needed by many production filters.

io_graph provides a reusable BPF object for this gap.

Production Evidence

The motivating workload is documented in existing production projects.

Falco users have requested conditional kernel-side event filtering because highly utilized systems can generate large volumes of benign, nearly identical events that still consume resources in the userspace rule-evaluation pipeline. The requested direction is to drop events in kernel space before they are allocated on the ring buffer.

Falco maintainers identified very high event rates and file-open-heavy workloads as a major bottleneck, and suggested aggressive kernel-side IP and file path prefix filtering as a possible direction. They also warned that kernel-side work runs in application context and must remain cheap.

Tetragon already exposes per-hook in-kernel BPF selectors and actions. Its high-load performance discussion similarly points to two options when ring buffers overflow: produce fewer events or optimize userspace consumption. The first option is the space targeted by io_graph.

io_graph therefore starts from a narrow production-shaped problem:

evaluate high-cardinality path / prefix / pattern filters in kernel
before ring buffer emission
without growing BPF program size linearly with the number of patterns

Motivation

The primary motivating workload is pre-ringbuf filtering.

A BPF program often has enough context to know that an event is uninteresting before allocating a ring buffer record. However, expressing large string or prefix filter sets directly in BPF code increases verifier pressure, instruction count, and update cost.

io_graph moves the high-cardinality matcher into a map-backed graph object:

event field
  -> io_graph matcher
  -> action id
  -> drop / post / classify

This allows the BPF program to remain small and stable while the policy graph is updated as data.

Design Goals

io_graph should:

1. reduce kernel-to-userspace event I/O for negative-heavy workloads,
2. avoid BPF program reload for policy updates,
3. represent large prefix or pattern sets compactly,
4. keep BPF programs small and verifier-friendly,
5. run without allocation, locks, FPU, SIMD, or large scratch memory in v0,
6. provide clear verifier rejection reasons for malformed graph blobs,
7. allow downstream projects to adopt it incrementally.

Non-goals

io_graph v0 does not aim to:

1. replace BPF programs,
2. replace userspace policy engines,
3. implement PCRE,
4. support captures, backreferences, lookaround, or full regex semantics,
5. compete with Hyperscan,
6. provide a general parser VM,
7. call BPF helpers from graph nodes,
8. allocate memory on the run path,
9. use FPU or SIMD in the initial kernel implementation,
10. introduce a standalone /dev/iograph subsystem,
11. support multi-architecture JIT in v0,
12. solve arbitrary graph canonicalization.

The first useful version should be small: a map-backed matcher for scalable kernel-side filtering before event emission.

v0 Scope

v0 focuses on high-cardinality prefix and small-pattern filters for negative-heavy event streams.

Supported in v0:

1. BPF map-backed immutable graph blob,
2. whole-graph RCU replacement on update,
3. load/update-time verifier for graph well-formedness,
4. interpreter execution path,
5. bpf_iograph_run() kfunc,
6. small action result: drop, post, class ID, policy ID, or action ID,
7. benchmarks against generated BPF, bounded selector arrays, exact-match maps, and userspace filtering.

Initial operators:

1. exact string match,
2. prefix match,
3. suffix match if cheap enough,
4. byte-class transition,
5. default transition,
6. accept/action state.

Initial target fields:

1. file path,
2. executable path,
3. command line,
4. selected string arguments,
5. small metadata strings.

Proposed BPF Shape

io_graph should be proposed as a BPF map type.

Conceptual userspace flow:

graph_fd = bpf_map_create(BPF_MAP_TYPE_IOGRAPH, ...);
bpf_map_update_elem(graph_fd, &key, graph_blob, BPF_ANY);

Conceptual BPF flow:

u32 action = 0;
ret = bpf_iograph_run(&policy_graph, buf, len, entry, &action);
if (ret < 0)
    return 0;
if (action == IOGRAPH_ACTION_DROP)
    return 0;
return submit_event(ctx);

The run path must be:

1. allocation-free,
2. lock-free or RCU-read-only,
3. bounded by verified graph metadata,
4. independent of userspace compiler trust,
5. safe under graph replacement.

Initial Downstream Integration Shape

Falco-shaped integration

The first downstream integration target is a Falco-style pre-ringbuf path filter.

A modern BPF event filler or tail-called event-specific program can evaluate a raw path-like argument before pushing event parameters into the buffer:

event-specific filler
  -> read raw path / executable / command field
  -> run io_graph matcher
  -> if action == DROP: return early
  -> otherwise push event to ring buffer

This target intentionally avoids full userspace fd.name semantics in v0. The kernel often sees raw path arguments rather than fully resolved absolute paths. v0 should benchmark raw path and prefix filtering first and treat userspace path resolution as a later integration problem.

Tetragon-shaped integration

Tetragon already models selectors as in-kernel BPF filters and actions.

io_graph can serve as a backend for selector components that currently scale poorly with the number of string or prefix values:

TracingPolicy selector
  -> compile high-cardinality string/prefix clauses into io_graph blob
  -> BPF program calls bpf_iograph_run()
  -> graph returns action_id / class_id / selector match

The initial integration should preserve existing bounded selector behavior and only offload the high-cardinality prefix/pattern subproblem.

Run-path Constraints

io_graph runs in the application context of the instrumented hook. It must not turn kernel-side filtering into a new application slowdown source.

Therefore the run path must be:

1. allocation-free,
2. bounded,
3. lock-free or RCU-read-only,
4. free of FPU/SIMD in v0,
5. small enough to beat the cost of pushing and processing unwanted events,
6. measurable with both positive-match and negative-match distributions.

Graph Blob Model

The graph blob is an immutable data object stored in a BPF map.

A minimal v0 blob should contain:

1. header,
2. node table,
3. edge table,
4. accept/action table,
5. entry state,
6. verifier metadata,
7. optional layout/version fields.

Nodes represent states.

Edges represent transitions over input bytes or byte classes.

Accept states produce small action IDs.

The blob should be size-tagged and versioned.

Unknown flags must be rejected.

All offsets and counts must be bounds-checked during map update.

Verifier Requirements

The io_graph verifier should check:

1. header size and version,
2. node count limit,
3. edge count limit,
4. action count limit,
5. all node ranges are in bounds,
6. all edge targets are in bounds,
7. all action IDs are in bounds,
8. entry state is valid,
9. transition encoding is supported,
10. no integer overflow in offset/size computation,
11. maximum run bounds are known,
12. graph replacement is safe under RCU.

The verifier should not prove arbitrary program behavior. It only verifies a restricted graph data format.

Execution Semantics

bpf_iograph_run() evaluates a graph against an input buffer.

Conceptually:

state = entry
for each byte in input:
    edge = transition(state, byte)
    if no edge:
        break
    state = edge.target
    if state has accept action:
        action = state.action
return final action/state

v0 should support a bounded maximum input length supplied by the caller or verifier-approved callsite constraints.

v0 does not support captures, substring extraction, helper calls, or arbitrary callbacks.

Update Semantics

Graph updates should be whole-object replacements.

On update:

1. userspace builds a new graph blob,
2. bpf_map_update_elem() submits the blob,
3. kernel verifier checks it,
4. if valid, the map swaps the active graph pointer,
5. old graph memory is released after an RCU grace period.

Updates must not mutate graphs in place.

Readers must see either the old graph or the new graph, never a partially updated graph.

JIT Policy

v0 must have an interpreter.

JIT is optional, but if implemented in v0 it should be narrow.

The initial JIT should focus on:

1. hot self-loop states,
2. prefix scanner loops,
3. byte-class skip loops,
4. small direct transitions.

The JIT must not become a general-purpose VM backend.

If JIT is enabled, interpreter/JIT equivalence tests are mandatory.

Benchmark Workloads

v0 must be evaluated on production-shaped workloads.

Workload A: Falco-style file-open workload

Generate or replay high-rate open/openat-like events with raw path arguments.

Pattern sets:

1. 6 prefixes,
2. 32 prefixes,
3. 100 prefixes,
4. 1000 prefixes.

Compare:

1. no kernel-side filter,
2. hard-coded BPF prefix checks,
3. generated BPF branch chain,
4. exact-match map where applicable,
5. io_graph interpreter,
6. io_graph JIT if enabled.

Metrics:

1. ringbuf bytes/sec,
2. ringbuf drops,
3. events dropped before buffer allocation,
4. kernel CPU,
5. userspace CPU,
6. cycles per checked byte,
7. policy update latency,
8. BPF instruction count,
9. graph/code/table size.

Workload B: Tetragon-style selector workload

Generate selector-like policies with first-match action semantics.

Measure:

1. maximum policy size accepted before verifier/code-size pressure,
2. action latency,
3. drop rate,
4. update latency,
5. instruction count,
6. map memory footprint.

Workload C: self-loop scanner

Input:

1. scan until delimiter,
2. skip byte class,
3. path prefix walk,
4. marker search.

Measure:

1. cycles/byte,
2. bytes/sec,
3. instruction count,
4. branch miss rate if available,
5. cache miss rate if available.

Success Criteria

Minimum success:

1. 10x smaller artifact than dense table or generated-BPF representation for high-cardinality prefix filters.
2. 10x faster policy update than BPF program regeneration/reload.
3. 2x reduction in kernel-to-userspace event bytes for a negative-heavy filtering workload.
4. No allocation, no locks, no FPU, and no large scratch memory on the run path.
5. Clear verifier rejection reasons for malformed graph blobs.

Strong success:

1. 50x or greater reduction in ringbuf bytes for a high-drop workload.
2. 50x or greater reduction in policy artifact size versus dense table.
3. 1.5x or greater runtime improvement over hand-written BPF for self-loop scanner patterns.
4. Ability to express 100+ path prefixes without expanding BPF program size linearly.
5. Prototype integration with at least one production-shaped Falco, Tetragon, Tracee, or similar workload.

Upstream Strategy

io_graph should be proposed incrementally.

Stage 0: out-of-tree prototype

Implement:

1. graph blob format,
2. verifier,
3. interpreter,
4. update path,
5. bpf_iograph_run() prototype,
6. benchmark harness.

Stage 1: production-shaped benchmark

Show reduction in:

1. ringbuf pressure,
2. BPF program size,
3. policy update cost,
4. userspace filtering cost.

Stage 2: BPF RFC

Send as a BPF map type proposal, not as a new kernel subsystem.

Initial claim:

BPF lacks a reusable map-backed finite-automaton object for high-cardinality
kernel-side prefix/pattern filters.

Stage 3: design partner

Validate the workload with at least one observability or security project before a mainline RFC.

Candidate projects:

1. Falco,
2. Tetragon,
3. Tracee,
4. Inspektor Gadget,
5. similar BPF-based runtime security or observability agents.

Stage 4: mainline candidate

Mainline should be considered only after:

1. the API surface is small,
2. verifier behavior is understandable,
3. update lifetime is RCU-safe,
4. benchmarks show clear wins,
5. a downstream user can explain the production need.

Design Partner Questions

For Falco/Tetragon-like users, the initial design needs the following data:

1. Which fields need scalable kernel-side filtering?
    * file path
    * executable path
    * command line
    * container metadata
    * namespace metadata
    * event arguments
2. Which operators are required for the first useful version?
    * exact
    * prefix
    * suffix
    * contains
    * small pattern
    * negative match
3. How many patterns are needed?
    * 6
    * 32
    * 100
    * 1000+
4. What is the event rate?
5. What percentage of events should be dropped before ring buffer emission?
6. How often does the policy update?
7. Is deterministic policy fingerprinting required for rollout safety?
8. Is explainability required for why an event was dropped?
9. Is action output enough, or is capture/extraction required?
10. Which current workaround is used?
    * larger ring buffer
    * syscall selection
    * bounded BPF selectors
    * userspace filtering
    * custom downstream patch

Open Questions

1. Should v0 support suffix matching, or should it start with exact/prefix only?
2. Should the first kernel implementation include JIT, or interpreter only?
3. Should io_graph be a new BPF map type or an extension of an existing map type?
4. Should bpf_iograph_run() return only action ID, or both action ID and final state?
5. How should graph update errors be reported to userspace?
6. How should graph memory be accounted?
7. What is the maximum acceptable graph size for v0?
8. What is the maximum acceptable per-event runtime cost?
9. How should negative-match policies be represented?
10. Which downstream project should be the first design partner?

Initial v0 Recommendation

Start with the smallest useful shape:

BPF_MAP_TYPE_IOGRAPH
  immutable prefix/pattern graph blob
  RCU whole-graph replacement
  interpreter
  bpf_iograph_run()
  action_id result
  exact + prefix match
  Falco-style pre-ringbuf benchmark

Do not start with:

general regex
captures
PCRE
full parser VM
multi-arch JIT
standalone subsystem
userspace policy engine replacement

Core Claim

io_graph is a bounded BPF map-backed prefix/pattern matcher for dropping or classifying high-volume events before ring buffer emission.

It is useful only if it reduces real I/O, update cost, or verifier pressure compared with existing BPF representations.
