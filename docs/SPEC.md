io_graph Specification

Status: Design Draft
Maturity: Experimental
Target v0: Out-of-tree BPF map type prototype
Primary Shape: BPF_MAP_TYPE_IOGRAPH-like cyclic FSM map + step/run kfunc + interpreter + minimal x86-64 self-loop JIT
Primary Goal: Preserve compact cyclic graph artifacts as kernel/BPF-side runtime objects when doing so avoids large intermediate objects
Primary v0 Workload: High-cardinality pre-ringbuf filtering
Primary Non-Goal: Do not build a general regex engine, a general parser VM, or a new standalone kernel subsystem

⸻

1. Summary

io_graph is a BPF-side runtime artifact for compact cyclic finite-state graphs.

Its purpose is to keep a compact cyclic graph alive across the userspace/kernel boundary, rather than flattening it into:

* dense transition tables,
* generated BPF programs,
* regex scratch regions,
* match/capture lists,
* decoded event objects,
* ring buffer event streams,
* pointer-heavy kernel data structures,
* userspace rule-evaluation contexts.

The first production-shaped workload is high-cardinality pre-ringbuf filtering: drop, post, or classify high-volume events before ring buffer allocation.

The v0 target is a small BPF map type prototype:

BPF_MAP_TYPE_IOGRAPH
  immutable cyclic graph blob
  load-time verifier
  interpreter walker
  experimental step/run kfuncs
  minimal x86-64 JIT focused on self-loop SCCs
  RCU whole-graph hot swap

The core idea is not that FSM compression is new. It is not.

The core idea is that, when the final decision only needs a small invariant over a compact cyclic graph, the graph itself should become the runtime object.

⸻

2. Production Problem

The first production-shaped target is pre-ringbuf filtering for high-volume observability and security agents.

Falco users have requested conditional kernel-side event filtering because highly utilized systems can generate large volumes of benign, nearly identical events that still consume resources in the userspace rule-evaluation pipeline. The requested direction is to drop events in kernel space before they are allocated on the ring buffer. Falco maintainers also identified high event rates, file-open-heavy workloads, and aggressive kernel-side file path prefix filtering as a possible direction, while warning that kernel-side work runs in application context and must remain cheap.  ￼

Tetragon already exposes per-hook in-kernel BPF selectors and actions. Its high-load performance discussion points to two options when ring buffers overflow: produce fewer events or optimize userspace consumption. The first option is the space targeted by io_graph.  ￼

Therefore v0 uses pre-ringbuf high-cardinality path/prefix filtering as the first benchmark workload.

⸻

3. Core Thesis

The thesis is:

If the final answer can be computed by maintaining a small invariant over a
compact cyclic graph, then io_graph should avoid constructing the large
intermediate object that conventional implementations materialize.

The small invariant is usually:

current_state
input cursor
accept/action code
optional bounded accumulator

The avoided intermediate object may be:

states × alphabet transition table
generated BPF program
regex scratch region
match/capture list
decoded event object
ringbuf record
userspace rule-evaluation context
mutable pointer tree

The project succeeds only if preserving the cyclic form at runtime gives a measurable benefit over flattening.

⸻

4. IO-aware Execution Principle

io_graph is not primarily an automaton compression project.

For every supported workload, io_graph starts by identifying the large intermediate object that the naive pipeline builds before reaching a small decision.

It then asks:

1. What large intermediate object does the naive implementation build?
2. What final answer is actually needed?
3. What invariant is sufficient to compute that answer?
4. What execution order reaches the answer before materialization?
5. Why does preserving the cyclic graph artifact make this cheaper than flattening?

A workload belongs in v0 only if this table can be filled clearly.

Example:

workload:
  Falco-style file-open filtering
naive intermediate:
  event parameter buffer + ringbuf record + userspace event object +
  userspace rule-engine context
answer:
  drop / post / action_id
invariant:
  current_state + best_accept_code over raw path bytes
execution order:
  read raw path argument, run graph, return before buffer write if DROP is final
why cyclic graph:
  shared prefixes, shared continuations, default paths, and self-loop scanner
  states remain compact and executable

⸻

5. What io_graph Is Not

io_graph is not:

* a general regex engine,
* a PCRE implementation,
* a Hyperscan replacement,
* a general I/O parser,
* a general BPF replacement,
* a new standalone kernel subsystem in v0,
* a new BTF semantics proposal in v0,
* a kernel-side minimizer,
* a general-purpose JIT VM,
* an authorization boundary.

v0 explicitly excludes:

* backreferences,
* lookaround,
* capture groups,
* general match reporting,
* general I/O compaction opcodes,
* emit-record ISA,
* input/output cursor VM,
* helper calls from graph code,
* dynamic allocation during run,
* FPU/SIMD requirements,
* large per-CPU scratch buffers,
* kernel-side DFA minimization,
* multi-architecture JIT,
* producer hooks,
* /dev/iograph public UAPI,
* certificate model,
* path canonicalization.

⸻

6. Why This Is Not Just DFA Compression

DFA compression is a mature area. Dense tables, sparse tables, default transitions, byte classes, minimization, and acceleration are all well-known.

io_graph does not claim:

* better general DFA compression than D2FA/A-DFA/ClusterFA,
* better regex throughput than Hyperscan,
* better PCRE expressiveness than full regex engines.

Instead, io_graph claims a narrower runtime property:

The compact cyclic graph should remain the executable kernel/BPF artifact.

Existing regex/DFA implementations often build large runtime structures. io_graph targets cases where those structures are the wrong runtime artifact.

For pre-ringbuf filtering, the largest avoided object may not be a DFA table. It may be the event object itself:

event parameters
  -> ringbuf record
  -> userspace event object
  -> state-engine update
  -> userspace rule evaluation
  -> drop

io_graph attempts to move the small decision before those objects are built.

Dense DFA implementations optimize the table shape, but they still materialize a transition table. regex-automata documents the dense DFA size as #states * 256 * sizeof(StateID), reduced by byte classes to #states * k * sizeof(StateID) where k is the number of equivalence classes. io_graph targets cases where even that table is not the object that should exist in the kernel hot path.  ￼

⸻

7. Winning Conditions

io_graph is useful only if it wins in at least one of these ways.

7.1 Artifact size

The cyclic graph blob should be materially smaller than:

* dense transition table,
* generated BPF FSM object,
* schema_count × parser programs,
* per-pattern branch chains.

The claim is not “best compression ever.”

The claim is:

the compact cyclic graph artifact is the runtime artifact

7.2 Update cost

Graph updates should be whole-object RCU swaps:

copy new blob
verify
build immutable graph object
optionally build JIT image
rcu_assign_pointer()
call_rcu(old, free)

No BPF program reload should be necessary for a pure graph update.

Important distinction:

policy activation:
  new graph becomes visible after pointer publication
old graph reclamation:
  deferred until an RCU grace period completes

RCU grace period latency is not counted as policy activation latency.

7.3 Pre-ringbuf I/O reduction

For negative-heavy workloads, io_graph should reduce:

* ringbuf bytes/sec,
* ringbuf drops,
* userspace event objects,
* userspace rule-evaluation work.

The useful comparison is:

cost(io_graph check)
  versus
cost(event materialization + ringbuf write + userspace read + rule evaluation)

A matcher that is slower than a hand-written single string compare can still win if it avoids much larger I/O.

7.4 Hot self-loop JIT

A single-state self-loop SCC should be lowered into a native tight loop.

This is the clearest reason to preserve cyclic structure through runtime.

cyclic graph survives to runtime
  -> SCC is visible
  -> JIT can emit scanner-like loop

7.5 Runtime memory discipline

Run path must be:

* allocation-free,
* lock-free,
* FPU-free,
* per-CPU scratch-free,
* helper-free inside graph execution.

⸻

8. Failure Conditions

The hypothesis fails if:

* cyclic graph blob is not materially smaller than flat tables,
* graph update is not materially cheaper than BPF program reload,
* pre-ringbuf filtering does not reduce real I/O,
* self-loop JIT does not beat generated/hand-written BPF FSM on scanner workloads,
* JIT image size cancels graph-size savings,
* branch misses dominate sparse graph walking,
* real workloads require PCRE/Hyperscan-level expressiveness,
* run path needs scratch, allocation, captures, or helper calls.

If these happen, the correct fallback is:

userspace compiler/minimizer
  -> flat table
  -> generated BPF

not a kernel runtime artifact.

⸻

9. Path Canonicalization

io_graph operates on the raw byte sequence supplied by the caller.

It does not normalize paths. It does not resolve:

* ..,
* repeated /,
* symlinks,
* mount namespace-relative paths,
* /proc-relative paths,
* userspace-derived absolute fd.name semantics.

Callers using io_graph for path-prefix filtering must understand that raw string prefix filtering can be bypassed by semantically equivalent path spellings.

For enforcement-sensitive use cases, callers must combine io_graph with proper object identity, namespace, capability, or LSM checks.

This limitation is shared with kernel-side BPF path filters in general. It is not unique to io_graph.

v0 benchmarks should use raw path arguments first. Resolved path semantics are a later integration problem.

⸻

10. v0 Scope

v0 is intentionally small.

Included:

* BPF_MAP_TYPE_IOGRAPH-like prototype,
* compact cyclic graph blob,
* load-time verifier,
* interpreter walker,
* experimental bpf_iograph_step() kfunc,
* experimental bpf_iograph_run() kfunc,
* minimal x86-64 self-loop-focused JIT,
* RCU whole-graph update,
* selftests,
* benchmarks.

Excluded:

* kernel-side minimization,
* general regex,
* captures,
* I/O emit-record opcodes,
* general parser VM,
* BTF frontend dependency,
* certificate model,
* producer hooks,
* stable UAPI promise,
* multi-architecture JIT.

The first production-shaped workload is:

high-cardinality exact/prefix path filtering before ringbuf emission

Suffix, contains, captures, and streaming/resumable state are v0.5 or later.

⸻

11. Expected Performance Envelope

These are v0 engineering targets, not ABI guarantees.

11.1 Run-path cost

For small cache-resident prefix graphs:

fixed kfunc/run overhead:
  target 50–150 ns
interpreted byte processing:
  target 1–3 ns/byte for L1-resident graph walks
typical 100-byte path:
  target 150–500 ns/event
large graph / L2-resident / branchy path:
  acceptable 500 ns–2 µs/event envelope
self-loop JIT hot path:
  target 50–200 ns/event for scanner-like workloads

The target is not to beat every dense DFA transition table.

The target is to beat the full cost of event materialization, ringbuf emission, userspace delivery, and userspace rule evaluation when the event can be rejected early.

11.2 Memory footprint

For high-cardinality path/prefix policies:

100 prefixes, average 50 bytes:
  target 15–70 KiB graph blob
1000 prefixes, average 50 bytes:
  target 150–700 KiB graph blob
hard v0 graph blob cap:
  8 MiB

The goal is for common 100-prefix policies to fit in L1/L2 cache and for 1000-prefix policies to remain L2-resident on typical systems.

11.3 Update cost

Graph updates should avoid BPF program regeneration, verifier reprocessing, JIT recompilation, and attach-time churn.

Activation path:

copy blob
verify blob
build immutable object
publish with rcu_assign_pointer()

Old graph reclamation is deferred to an RCU grace period and is not counted as policy activation latency.

Expected update advantage over BPF program reload:

small/medium graph:
  10x–1000x lower activation cost depending on baseline

11.4 Compared baselines

The v0 target should be in the same order of magnitude as existing BPF map lookups for supported workloads, while solving a class of string/prefix matching that LPM_TRIE does not naturally solve.

Expected comparison:

BPF LPM_TRIE:
  good precedent for map-backed lookup object
  IP/binary prefix-oriented
  pointer-linked traversal
io_graph:
  string/prefix byte-sequence graph
  contiguous index-based traversal
  map-backed policy object

11.5 Expected wins

Minimum expected win:

100+ prefixes:
  BPF program instruction count remains nearly constant
negative-heavy event stream:
  ringbuf bytes reduced by 2x+
policy update:
  no BPF program reload for graph-only changes

Strong expected win:

100+ prefixes:
  10x+ smaller than generated BPF or dense table baseline
high-drop workload:
  10x–100x ringbuf I/O reduction
self-loop scanner:
  1.5x–3x faster than hand-written BPF loop

⸻

12. Target Workloads

v0 targets four workload families.

12.1 Falco-style pre-ringbuf path filter

Examples:

* drop noisy file-open events by raw path prefix,
* drop known benign executable path prefixes,
* classify raw command-line prefixes before event emission.

Naive intermediate:

* event parameter buffer,
* ringbuf record,
* userspace event object,
* userspace rule-evaluation context.

Invariant:

* current graph state,
* best action code,
* consumed cursor.

Win metric:

* ringbuf bytes/sec,
* events dropped before buffer allocation,
* userspace CPU avoided,
* total drop rate improvement.

12.2 Tetragon-style large-N selector

Tetragon already models selectors as in-kernel BPF filters and actions. Each selector contains filters, filters are combined with AND semantics, and multiple selectors are evaluated as first-match OR. Tetragon documentation describes bounded selector structure and value limits for some filters.  ￼

Examples:

* large-N file path prefix clauses,
* executable path prefix clauses,
* command-line prefix clauses,
* argument string prefix classifiers.

Naive intermediate:

* generated selector branch code,
* repeated value arrays,
* multiple duplicated selectors,
* verifier-visible per-pattern logic.

Invariant:

* current prefix equivalence class,
* matched selector/action ID.

Win metric:

* maximum policy size before verifier/code-size pressure,
* instruction count,
* action latency,
* update latency.

12.3 Self-loop scanner

Examples:

* scan until delimiter,
* scan until non-class byte,
* scan until marker byte.

Naive intermediate:

* regex scratch,
* match object,
* generated BPF loop,
* dense table.

Invariant:

* state,
* cursor,
* accept/action code.

Win metric:

* cycles/byte,
* branch misses,
* JIT image size,
* throughput vs BPF FSM.

12.4 Sparse FSM classifier

Examples:

* policy classifier,
* protocol state classifier,
* byte stream classifier,
* many-schema shared continuation.

Naive intermediate:

* dense transition table,
* schema_count × generated BPF programs,
* schema_count × transition tables.

Invariant:

* entry state,
* current state,
* accept/action code.

Win metric:

* artifact size,
* transition/sec,
* d-cache misses,
* update latency,
* BPF verifier/JIT reload avoided.

⸻

13. Relationship to Existing Kernel/BPF Code

13.1 LPM_TRIE precedent

BPF_MAP_TYPE_LPM_TRIE is a useful precedent: a BPF map type containing a kernel-side data structure with custom allocation, lookup, update, delete, BTF checking, and memory accounting.

The implementation embeds struct bpf_map and uses RCU child pointers in a trie-shaped internal object.  ￼

io_graph should follow the same general map-object shape:

BPF map type
custom map_alloc/free/update
custom memory accounting
BPF-callable step/run interface

13.2 Difference from LPM_TRIE

LPM_TRIE lookup traverses pointer-linked trie nodes using RCU child pointers.

io_graph should avoid pointer chasing in the hot path:

LPM_TRIE:
  root pointer -> child pointer -> child pointer -> ...
io_graph:
  graph pointer once
  nodes[state]
  edges[node.edge_start + i]

The hot path should be contiguous index-based graph walking.

13.3 Ringbuf precedent

BPF ringbuf is also a map-shaped kernel object, but it behaves more like a specialized container object than a normal key/value map.

In current kernel code, ringbuf map lookup/update/delete/get_next_key return -ENOTSUPP, while behavior is exposed through specialized operations.  ￼

io_graph should do the same if normal key/value semantics do not fit.

map_update_elem:
  replace graph blob
map_lookup_elem:
  unsupported or metadata-only
map_delete_elem:
  clear graph or unsupported in v0
get_next_key:
  unsupported

13.4 Ringbuf is not the target

BPF ringbuf is an output transport. It has pages, producer/consumer positions, wait queues, irq work, and synchronization structures.

io_graph reduces what needs to be materialized before output.

⸻

14. Relationship to Regex Engines

io_graph is not trying to beat Hyperscan.

Hyperscan is broader and more powerful. Its scratch object contains queues, active queue arrays, block/transient/full state, delay slots, anchored literal logs, SOM storage, deduper logs, and callback context.

io_graph wins only by rejecting that expressiveness.

The intended v0 run state is:

state
cursor
action

No captures.
No SOM.
No match callback.
No queue.
No scratch region.
No per-call allocation.

The first competitor is not Hyperscan.

The first competitors are:

* ringbuf-then-userspace-drop,
* generated BPF string-compare chains,
* bounded selector arrays,
* exact-match maps used as a workaround,
* dense tables for workloads where sparse cyclic form is smaller.

⸻

15. Userspace Compiler / Oracle Boundary

The userspace toolchain that produces graph blobs is out of scope for the kernel ABI.

A producer may use:

* direct prefix trie compiler,
* Hopcroft-style minimizer,
* hash-consing minimizer,
* D2FA-like compressor,
* project-specific policy compiler,
* reference oracle.

The kernel does not trust the producer.

Kernel responsibilities are limited to:

* parse blob,
* verify well-formedness,
* build immutable graph object,
* execute interpreter,
* optionally JIT selected structures,
* RCU-swap graph objects.

The kernel does not verify:

* minimality,
* canonicality,
* language equivalence,
* source regex correctness,
* source policy correctness,
* producer correctness.

⸻

16. Graph Blob Format

The v0 blob is a compact cyclic FSM.

#define IOG_MAGIC    0x494f4752 /* "IOGR" */
#define IOG_NO_STATE 0xffffffffu
struct iog_blob_hdr {
    __u32 magic;
    __u16 version;
    __u16 flags;
    __u32 node_cnt;
    __u32 edge_cnt;
    __u32 entry_cnt;
    __u32 accept_cnt;
    __u32 alphabet_size;
    __u32 initial_state;
    __u32 nodes_off;
    __u32 edges_off;
    __u32 entries_off;
    __u32 accepts_off;
    __u32 total_size;
    __u32 max_input_len;
    __u32 reserved;
};
struct iog_node {
    __u32 edge_start;
    __u16 edge_cnt;
    __u16 flags;
    __u32 default_dst;
    __u32 accept_id;
};
struct iog_edge {
    __u32 sym_lo;
    __u32 sym_hi;
    __u32 dst;
};
struct iog_entry {
    __u32 id;
    __u32 state;
};
struct iog_accept {
    __u32 id;
    __u32 code;
};

16.1 Design notes

iog_node is intentionally small.

edge_start:
  index into edges[]
edge_cnt:
  number of outgoing explicit edges
default_dst:
  fallback state, or IOG_NO_STATE
accept_id:
  classifier/match/action id

iog_edge uses ranges instead of single symbols:

sym_lo..sym_hi -> dst

This supports byte ranges, character classes, and scanner-like states without expanding to 256 entries.

⸻

17. Verifier

The v0 verifier checks well-formedness and safety only.

17.1 Header checks

* magic valid,
* version supported,
* total_size sane,
* all section offsets inside blob,
* offsets aligned,
* reserved fields are zero,
* flags known.

17.2 Limit checks

* node_cnt <= max_nodes,
* edge_cnt <= max_edges,
* entry_cnt <= max_entries,
* accept_cnt <= max_accepts,
* alphabet_size <= max_alphabet,
* max_input_len <= max_input_len_limit.

17.3 Node checks

* edge_start + edge_cnt <= total edge count,
* default_dst == IOG_NO_STATE or default_dst < node_cnt,
* accept_id == 0 or accept_id < accept_cnt,
* flags known.

17.4 Edge checks

* sym_lo <= sym_hi,
* sym_hi < alphabet_size,
* dst < node_cnt.

17.5 Per-node edge ordering

For every node:

* edges sorted by sym_lo,
* edges do not overlap.

This lets runtime avoid overlap handling.

17.6 Entry checks

* entry state < node_cnt,
* initial_state < node_cnt.

17.7 Default chain checks

Default transitions are allowed, but bounded.

default chain depth <= IOG_MAX_DEFAULT_DEPTH
no unbounded default-only cycle

Suggested v0 value:

IOG_MAX_DEFAULT_DEPTH = 8

17.8 What verifier does not check

The verifier does not check:

* minimality,
* canonicality,
* source equivalence,
* compiler correctness,
* oracle correctness.

⸻

18. Runtime Semantics

18.1 Step

Given:

graph
state
symbol

Return:

next_state

Algorithm:

node = nodes[state]
for edge in node.explicit_edges:
  if edge.sym_lo <= symbol <= edge.sym_hi:
    return edge.dst
if node.default_dst != IOG_NO_STATE:
  return node.default_dst
return IOG_NO_STATE

The implementation may binary-search, linear-scan, or JIT depending on edge count and node shape.

18.2 Run

run() repeatedly applies step() over an input buffer.

State:

current_state
cursor
last_accept_code

Output:

final_state
last_accept_code
status

v0 does not produce captures or variable-sized match lists.

⸻

19. BPF API

v0 uses experimental kfuncs.

19.1 Step kfunc

__bpf_kfunc __u32 bpf_iograph_step(struct bpf_map *map,
                                   __u32 state,
                                   __u32 sym);

19.2 Run kfunc

__bpf_kfunc int bpf_iograph_run(struct bpf_map *map,
                                const __u8 *buf,
                                __u32 len,
                                __u32 entry,
                                __u32 *final_state,
                                __u32 *action_code);

19.3 Step vs run

step() exists for integration and control.

run() is the performance path.

step:
  simple
  explicit
  useful for debugging
run:
  amortizes kfunc call overhead
  enables self-loop JIT
  benchmark target

⸻

20. Action Model

v0 returns an action code.

It does not return resumable graph state as part of the public fast path.

The pre-ringbuf filtering workload needs a small decision value, not streaming automaton state. Returning resumable state would expand the scope into streaming match and resumable execution, which is intentionally out of v0.

Recommended reserved action values:

0:
  no match / default
1:
  drop / no-post
2:
  post
3+:
  caller-defined class ID, policy ID, or selector ID

Negative-match policies should be represented through action IDs, not through a separate execution mechanism.

Examples:

deny-list:
  default action = POST
  matching denied prefix -> DROP
allow-list:
  default action = DROP
  matching allowed prefix -> POST

⸻

21. Map Type Shape

struct bpf_iograph_map {
    struct bpf_map map;
    struct iog_graph __rcu *graph;
    struct mutex update_lock;
};

21.1 Map create

Suggested constraints:

key_size = sizeof(__u32)
max_entries = 1
value_size = maximum blob size, or fixed upper bound

v0 accepts only key 0.

21.2 Map update

BPF_MAP_UPDATE_ELEM(key=0, value=blob)

Update path:

copy blob from userspace
verify blob
build immutable graph object
optionally build JIT image
lock update_lock
swap graph pointer with rcu_assign_pointer()
unlock
call_rcu(old_graph, free)

21.3 Map lookup

v0 may return -ENOTSUPP.

Alternative:

lookup key 0 returns metadata only

21.4 Map delete

v0 may either:

clear current graph

or:

return -ENOTSUPP

21.5 Memory accounting

map_mem_usage must include:

* sizeof(struct bpf_iograph_map),
* current graph object,
* nodes,
* edges,
* entries,
* accepts,
* JIT image.

⸻

22. In-kernel Graph Object

The kernel may normalize the blob into an immutable object.

struct iog_graph {
    struct rcu_head rcu;
    u32 node_cnt;
    u32 edge_cnt;
    u32 entry_cnt;
    u32 accept_cnt;
    u32 alphabet_size;
    u32 max_input_len;
    struct iog_node *nodes;
    struct iog_edge *edges;
    struct iog_entry *entries;
    struct iog_accept *accepts;
    struct iog_jit_image *jit;
};

Run path must not allocate.

Allowed:

rcu_dereference graph
read nodes/edges
update local state
return

Forbidden:

kmalloc
mutex
spinlock
FPU
per-CPU scratch allocation
helper call from graph execution

⸻

23. Concurrency and Update Model

Graph object is immutable after publication.

Update is whole-object replacement.

old graph remains valid during RCU read-side critical sections
new graph becomes visible atomically
old graph freed after grace period

This avoids incremental mutation complexity.

Compared with pointer-linked data structures that mutate subtrees, io_graph uses coarser but simpler whole-graph replacement.

⸻

24. JIT v0

v0 JIT is not a general graph JIT.

It supports only structures needed to test the thesis.

24.1 Required lowering

single-state self-loop SCC -> tight loop
sparse outgoing edges -> cmp chain
accept/reject return

24.2 Optional lowering

dense outgoing edges -> jump table

This may be v0.5 if code size grows.

24.3 Self-loop target

Pattern:

state S:
  edge class C -> S
  exit edges -> other states

Lowering:

while cursor < end:
  sym = *cursor
  if sym in C:
    cursor++
    continue
  break_to_exit_dispatch

This is the primary v0 JIT performance claim.

24.4 JIT lifetime

JIT image lifetime is tied to struct iog_graph.

new graph:
  build JIT image before publish
update:
  publish new graph with rcu_assign_pointer
old graph:
  call_rcu
  free JIT image after grace period

⸻

25. Interpreter

Interpreter is required.

It is used for:

* correctness reference,
* JIT differential testing,
* fallback,
* debugging,
* fuzzing.

The interpreter must produce the same trace as the JIT for:

* state sequence,
* accept sequence,
* final state,
* status.

⸻

26. Differential Testing

Every JIT-supported graph must be tested against the interpreter.

Test matrix:

* random sparse graphs,
* random self-loop graphs,
* random default chains,
* random input buffers,
* random entry states,
* edge ordering edge cases,
* invalid blob rejection,
* RCU update during execution.

Required property:

interpreter_trace == jit_trace

⸻

27. Security Model

v0 keeps the attack surface small by design.

27.1 Trusted

* kernel verifier,
* kernel interpreter,
* kernel JIT after verification,
* RCU lifetime rules.

27.2 Untrusted

* userspace graph blob,
* graph producer,
* canonicality claims,
* minimality claims,
* BPF program inputs.

27.3 Run path restrictions

* no dynamic allocation,
* no sleeping,
* no helper calls from graph,
* no arbitrary pointer access,
* no kernel pointer exposure,
* no FPU/SIMD in v0,
* no capture buffers,
* no path canonicalization.

⸻

28. Benchmarks

28.1 Baselines

A. hand-written C FSM
Upper bound.

B. hand-written BPF FSM, JITed
Realistic BPF competitor.

C. generated BPF flat-table FSM, JITed
Flattening baseline.

D. flat transition table walker
Data-only flattening baseline.

E. LPM_TRIE-style acyclic map lookup
Map-type precedent baseline.

F. userspace filtering after ringbuf
Current fallback for many observability agents.

G. io_graph interpreter
Reference.

H. io_graph JIT
Target.

If a regex-kfunc baseline is available and reproducible, include it separately.

28.2 Required metrics

* artifact bytes,
* flat table bytes,
* generated BPF object bytes,
* BPF verifier time,
* BPF JIT time,
* io_graph verify time,
* io_graph JIT time,
* update latency,
* lost invocations during update,
* transitions/sec,
* cycles/byte,
* branch misses,
* i-cache misses,
* d-cache misses,
* ringbuf bytes/sec,
* ringbuf drops,
* userspace CPU avoided,
* run-path allocations,
* per-CPU memory footprint.

28.3 Required experiments

Self-loop scanner:

io_graph JIT must beat or match BPF FSM

Sparse FSM:

cyclic blob must be materially smaller than flat table

Hot reload:

graph update must be materially cheaper than BPF reload

Pre-ringbuf filtering:

io_graph must reduce ringbuf bytes and/or drops in negative-heavy workloads

Many-schema/shared-continuation:

shared continuation must reduce total artifact size

⸻

29. Quantitative Success Criteria

v0 is successful only if at least one production-shaped workload demonstrates:

1. 100-prefix policy evaluated in under 500 ns/event on a cache-resident graph.
2. 100-prefix graph blob below 70 KiB.
3. 1000-prefix graph blob below 700 KiB.
4. BPF instruction count remains approximately constant as pattern count grows.
5. Graph-only policy update avoids BPF program reload.
6. Negative-heavy pre-ringbuf benchmark reduces ringbuf bytes by at least 2x.
7. Strong benchmark reduces ringbuf bytes by 10x or more.
8. Self-loop JIT beats or matches hand-written BPF scanner loop on at least one workload.

⸻

30. Implementation Budget

Target kernel LOC:

map type + loader + verifier:
  ~700 lines
interpreter:
  ~300 lines
step/run kfuncs:
  ~200 lines
x86-64 self-loop JIT:
  ~800-1200 lines
selftests + benchmark:
  ~600-900 lines

Target total kernel-side v0:

2500-3500 lines

If kernel-side code exceeds roughly 4000 lines before benchmarks validate the thesis, scope should be reduced.

Recommended initial limits:

graph blob max size:
  8 MiB
node count max:
  1,000,000
edge count max:
  4,000,000
action count max:
  65,536
max input length per run:
  65,536 bytes
default chain depth:
  8
typical run-path target:
  under 1 microsecond for common negative-heavy prefix checks
absolute run-path guardrail:
  under 10 microseconds for configured v0 benchmarks

These are engineering budgets, not ABI guarantees.

⸻

31. Roadmap

v0: Minimal cyclic FSM map

* BPF_MAP_TYPE_IOGRAPH-like map,
* graph blob verifier,
* interpreter,
* step/run kfuncs,
* self-loop x86-64 JIT,
* RCU whole-graph update,
* Falco-style pre-ringbuf benchmark,
* Tetragon-style selector benchmark.

v0.5: Layout and JIT refinement

* small-edge inline nodes, if benchmark proves useful,
* dense edge jump table,
* hot SCC-only JIT,
* better graph memory layout,
* suffix matching if justified.

v1: Upstreamable BPF map type

* clean map API,
* bpftool introspection,
* libbpf support,
* selftests,
* documentation,
* downstream design partner signal.

v2: Producer/tooling integrations

* project-specific policy compilers,
* optional canonical graph producers,
* explain/diff/debug tooling,
* production integration branches.

v3: Additional producer-side use cases

* trace/event policy classifiers,
* kernel-side scanners,
* ringbuf prefiltering,
* LSM policy FSMs.

⸻

32. Upstream Strategy

Do not pitch v0 as:

* new kernel subsystem,
* new regex engine,
* new BTF runtime,
* new parser VM,
* userspace policy engine replacement.

Pitch v0 as:

BPF map type for compact cyclic FSM artifacts,
with allocation-free interpreter, RCU graph replacement,
and optional self-loop JIT.

Patch structure:

1. bpf: add BPF_MAP_TYPE_IOGRAPH skeleton and UAPI
2. bpf: iograph cyclic graph blob format and verifier
3. bpf: iograph interpreter walker
4. bpf: iograph step/run kfuncs
5. bpf: iograph x86-64 self-loop JIT
6. selftests/bpf: verifier and interpreter/JIT equivalence
7. selftests/bpf: benchmarks against BPF FSM, flat table, LPM_TRIE-style lookup
8. selftests/bpf: pre-ringbuf filtering benchmark

Patches 1–4 must be useful without JIT.

Patch 5 carries the cyclic-runtime thesis.

The RFC should ideally include one of:

* downstream benchmark results,
* downstream maintainer comment,
* downstream branch or prototype,
* Reviewed-by / Tested-by / Co-developed-by signal from an affected project.

⸻

33. Design Partner Questions

For Falco/Tetragon-like users, the initial design needs the following data:

1. Which fields need scalable kernel-side filtering?
    * file path,
    * executable path,
    * command line,
    * container metadata,
    * namespace metadata,
    * event arguments.
2. Which operators are required for the first useful version?
    * exact,
    * prefix,
    * negative prefix,
    * suffix,
    * contains,
    * small pattern.
3. How many patterns are needed?
    * 6,
    * 32,
    * 100,
    * 1000+.
4. What is the event rate?
5. What percentage of events should be dropped before ring buffer emission?
6. How often does the policy update?
7. Is deterministic policy fingerprinting required for rollout safety?
8. Is explainability required for why an event was dropped?
9. Is action output enough, or is capture/extraction required?
10. Which current workaround is used?
    * larger ring buffer,
    * syscall selection,
    * bounded BPF selectors,
    * userspace filtering,
    * custom downstream patch.
11. Are raw path semantics acceptable, or is canonical path resolution required?
12. Is this filtering for performance only, or for security enforcement?

⸻

34. Open Questions

The remaining open questions are intentionally narrow.

1. Should map_update_elem() require fixed value_size == max_blob_size?
2. Should variable-size graph blobs use a custom update path?
3. Should run() be required in v0, or is step() enough for the first RFC?
4. Should dense jump tables be v0 or v0.5?
5. Should JIT be opt-in per map?
6. How should bpftool display graph size and JIT size?
7. Should accept_code be opaque u32 or structured?
8. Should delete clear the graph or be unsupported?
9. Which downstream project should be the first design partner?
10. Can self-loop JIT beat BPF FSM on real workloads?

⸻

35. Maintainer-facing Summary

io_graph is a BPF-map-shaped runtime artifact for compact cyclic FSMs.

It is designed for narrow workloads where existing implementations materialize large intermediate objects, but the final answer only requires a small invariant:

state
cursor
accept/action code
optional bounded accumulator

It does not compete with general regex engines or Hyperscan.
It does not introduce a standalone kernel subsystem.
It does not run minimization in kernel.

The initial implementation is a small BPF map type prototype:

* immutable cyclic graph blob,
* load-time verifier,
* allocation-free walker,
* experimental step/run kfuncs,
* RCU graph replacement,
* minimal self-loop JIT.

The first production-shaped benchmark is pre-ringbuf filtering for high-volume observability/security agents.

Expected v0 envelope:

100 prefixes:
  15–70 KiB blob
  150–500 ns/event interpreted
1000 prefixes:
  150–700 KiB blob
  L2-resident target
update:
  pointer-publish activation
  no BPF reload
strong workload:
  10x–100x ringbuf I/O reduction

The hypothesis is falsifiable:

If cyclic graph artifacts are not smaller,
if updates are not cheaper,
if pre-ringbuf filtering does not reduce real I/O,
or if self-loop JIT does not beat BPF FSM baselines,
then io_graph should not become an upstream feature.

The reason to try it is equally clear:

When the compact cyclic form is the natural answer,
flattening it into tables, generated BPF, event objects, or userspace streams
is the intermediate object.
io_graph avoids constructing that object.
