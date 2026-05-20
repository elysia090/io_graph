io_graph Specification

Status: Design Draft
Maturity: Experimental
Target v0: Out-of-tree BPF map type prototype
Primary Shape: BPF_MAP_TYPE_IOGRAPH-like cyclic FSM map + step/run kfunc + minimal x86-64 self-loop JIT
Primary Goal: Preserve minimized cyclic graph artifacts as kernel/BPF-side runtime objects
Primary Non-Goal: Do not build a general regex engine, a general parser VM, or a new standalone kernel subsystem

1. Summary

io_graph is a BPF-side runtime artifact for minimized cyclic finite-state graphs.

Its purpose is to keep the cyclic canonical form produced by a userspace oracle alive across the userspace/kernel boundary, rather than flattening it into:

dense transition tables
generated BPF programs
large regex scratch regions
match/capture lists
decoded event objects
pointer-heavy kernel data structures

The v0 target is a small BPF map type prototype:

BPF_MAP_TYPE_IOGRAPH
  immutable cyclic graph blob
  load-time verifier
  interpreter walker
  experimental step/run kfuncs
  minimal x86-64 JIT focused on self-loop SCCs
  RCU whole-graph hot swap

The core idea is not that FSM compression is new. It is not.
The core idea is that the minimized cyclic artifact itself should become the runtime object in kernel/BPF space.

2. Core Thesis

The thesis is:

If the final answer can be computed by maintaining a small invariant over a
minimized cyclic graph, then io_graph should avoid constructing the large
intermediate object that conventional implementations materialize.

The small invariant is usually:

current_state
input cursor
accept code
optional bounded accumulator

The avoided intermediate object may be:

#states × alphabet transition table
schema_count × generated BPF programs
regex scratch region
match/capture list
decoded event object
mutable pointer tree

The project succeeds only if preserving the cyclic form at runtime gives a measurable benefit over flattening.

3. What io_graph Is Not

io_graph is not:

a general regex engine
a PCRE implementation
a Hyperscan replacement
a general I/O parser
a general BPF replacement
a new standalone kernel subsystem in v0
a new BTF semantics proposal in v0
a kernel-side minimizer
a general-purpose JIT VM

v0 explicitly excludes:

backreferences
lookaround
capture groups
general match reporting
general I/O compaction opcodes
emit-record ISA
input/output cursor VM
helper calls from graph code
dynamic allocation during run
FPU/SIMD requirements
large per-CPU scratch buffers
kernel-side DFA minimization
multi-architecture JIT
producer hooks
/dev/iograph public UAPI
certificate model

4. Why This Is Not Just DFA Compression

DFA compression is a mature area. Dense tables, sparse tables, default transitions, byte classes, minimization, and acceleration are all well-known.

io_graph does not claim:

better general DFA compression than D2FA/A-DFA/ClusterFA
better regex throughput than Hyperscan
better PCRE expressiveness than full regex engines

Instead, io_graph claims a narrower runtime property:

The minimized cyclic graph should remain the executable kernel/BPF artifact.

Existing regex/DFA implementations often build large runtime structures. For example, Hyperscan’s scratch allocation builds a large composite region containing queues, state buffers, fatbit arrays, deduper logs, SOM storage, delay slots, and other runtime structures.

regex-automata documents the classic dense DFA table shape as:

#states * 256 * sizeof(StateID)

and byte classes reduce this to:

#states * k * sizeof(StateID)

where k is the number of equivalence classes.  ￼

io_graph targets cases where even this kind of table should not be materialized at runtime.

5. IO-aware Execution Principle

For every supported workload, io_graph must answer five questions:

1. What large intermediate object does the naive implementation build?
2. What final answer is actually needed?
3. What invariant is sufficient to compute that answer?
4. What execution order avoids materializing the intermediate object?
5. Why does the cyclic graph artifact make this cheaper than flattening?

A workload belongs in v0 only if this table can be filled clearly.

Example:

workload:
  sparse policy FSM
naive intermediate:
  dense transition table or generated BPF FSM
answer:
  allow/deny/class_id
invariant:
  current_state + accept_code
execution order:
  stream symbol by symbol, update state, return accept_code
why cyclic graph:
  shared continuations and default paths remain compact

6. Winning Conditions

io_graph is useful only if it wins in at least one of these ways.

6.1 Artifact size

The cyclic graph blob should be materially smaller than:

dense transition table
generated BPF FSM object
schema_count × parser programs

The claim is not “best compression ever.”
The claim is:

the minimized cyclic artifact is the runtime artifact

6.2 Update cost

Graph updates should be whole-object RCU swaps:

copy new blob
verify
build immutable graph object
optionally build JIT image
rcu_assign_pointer()
call_rcu(old, free)

No BPF program reload should be necessary for a pure graph update.

Target headline:

sub-microsecond atomic graph hot-reload, if graph size permits

6.3 Hot self-loop JIT

A single-state self-loop SCC should be lowered into a native tight loop.

This is the clearest reason to preserve the cyclic artifact through runtime.

cyclic graph survives to runtime
  -> SCC is visible
  -> JIT can emit scanner-like loop

6.4 Runtime memory discipline

Run path must be:

allocation-free
lock-free
FPU-free
per-CPU scratch-free
helper-free inside graph execution

7. Failure Conditions

The hypothesis fails if:

cyclic graph blob is not materially smaller than flat tables
graph update is not materially cheaper than BPF program reload
self-loop JIT does not beat generated/hand-written BPF FSM on scanner workloads
JIT image size cancels graph-size savings
branch misses dominate sparse graph walking
real workloads require PCRE/Hyperscan-level expressiveness
run path needs scratch, allocation, captures, or helper calls

If these happen, the correct fallback is:

userspace oracle/minimizer
  -> flat table
  -> generated BPF

not a kernel runtime artifact.

8. Relationship to Existing Kernel/BPF Code

8.1 LPM_TRIE precedent

BPF_MAP_TYPE_LPM_TRIE is a useful precedent: a BPF map type containing a kernel-side data structure with custom allocation, lookup, update, delete, BTF checking, and memory accounting. The implementation embeds struct bpf_map in struct lpm_trie, uses an RCU root pointer, and installs map operations through trie_map_ops.

io_graph should follow this shape:

BPF map type
custom map_alloc/free/update
custom memory accounting
BTF ID for introspection
BPF-callable step/run interface

8.2 Difference from LPM_TRIE

LPM_TRIE lookup traverses pointer-linked trie nodes using RCU child pointers.  ￼

io_graph should avoid pointer chasing in the hot path:

LPM_TRIE:
  root pointer -> child pointer -> child pointer -> ...
io_graph:
  graph pointer once
  nodes[state]
  edges[node.edge_start + i]

The hot path should be contiguous index-based graph walking.

8.3 Ringbuf precedent

BPF ringbuf is also a map-shaped kernel object, but its generic map lookup/update/delete operations return -ENOTSUPP. It exposes behavior through specialized helpers and map operations instead.  ￼

io_graph should do the same if normal key/value semantics do not fit.

map_update_elem:
  replace graph blob
map_lookup_elem:
  unsupported or metadata-only
map_delete_elem:
  clear graph or unsupported in v0
get_next_key:
  unsupported

8.4 Ringbuf is not the target

BPF ringbuf allocates and maps data pages, including a double-mapping trick for wraparound data.  ￼

io_graph should not compete with ringbuf.

Ringbuf is an output transport.
io_graph reduces what needs to be materialized before output.

9. Relationship to v2 / Oracle

btf-dedup-v2 or another userspace oracle is responsible for:

minimization
canonicalization
equivalence checking
witness minimization
explain/diff/debug output
graph corpus generation

The kernel must not run the oracle.

Kernel responsibilities are limited to:

parse blob
verify well-formedness
build immutable graph object
execute interpreter
optionally JIT selected structures
RCU-swap graph objects

The kernel does not verify:

minimality
canonicality
language equivalence
v2 correctness
source regex correctness
source BTF correctness

10. v0 Scope

v0 is intentionally small.

Included

BPF_MAP_TYPE_IOGRAPH-like prototype
canonical cyclic graph blob
load-time verifier
interpreter walker
experimental bpf_iograph_step kfunc
experimental bpf_iograph_run kfunc
x86-64 self-loop-focused JIT
RCU whole-graph update
selftests
benchmarks

Excluded

kernel-side minimization
general regex
captures
I/O emit-record opcodes
general parser VM
BTF frontend dependency
certificate model
producer hooks
stable UAPI promise
multi-architecture JIT

11. Graph Blob Format

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

11.1 Design notes

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

12. Verifier

The v0 verifier checks well-formedness and safety only.

12.1 Header checks

magic valid
version supported
total_size sane
all section offsets inside blob
offsets aligned
reserved == 0
flags known

12.2 Limit checks

node_cnt <= max_nodes
edge_cnt <= max_edges
entry_cnt <= max_entries
accept_cnt <= max_accepts
alphabet_size <= max_alphabet

12.3 Node checks

edge_start + edge_cnt <= total edge count
default_dst == IOG_NO_STATE or default_dst < node_cnt
accept_id == 0 or accept_id < accept_cnt
flags known

12.4 Edge checks

sym_lo <= sym_hi
sym_hi < alphabet_size
dst < node_cnt

12.5 Per-node edge ordering

For every node:

edges sorted by sym_lo
edges do not overlap

This lets runtime avoid overlap handling.

12.6 Entry checks

entry state < node_cnt
initial_state < node_cnt

12.7 Default chain checks

Default transitions are allowed, but bounded.

default chain depth <= IOG_MAX_DEFAULT_DEPTH
no unbounded default-only cycle

Suggested v0 value:

IOG_MAX_DEFAULT_DEPTH = 8

12.8 What verifier does not check

The verifier does not check:

minimality
canonicality
source equivalence
oracle correctness

13. Runtime Semantics

13.1 Step

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

13.2 Run

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

14. BPF API

v0 uses experimental kfuncs.

BPF kfuncs are not stable ABI in the same way as helpers; kernel documentation explicitly treats them as kernel-version-dependent extension points.  ￼

14.1 Step kfunc

__bpf_kfunc __u32 bpf_iograph_step(struct bpf_map *map,
                                   __u32 state,
                                   __u32 sym);

14.2 Run kfunc

__bpf_kfunc int bpf_iograph_run(struct bpf_map *map,
                                const __u8 *buf,
                                __u32 len,
                                __u32 entry,
                                __u32 *final_state);

14.3 Step vs run

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

15. Map Type Shape

struct bpf_iograph_map {
    struct bpf_map map;
    struct iog_graph __rcu *graph;
    struct mutex update_lock;
};

15.1 Map create

Suggested constraints:

key_size = sizeof(__u32)
max_entries = 1
value_size = maximum blob size, or fixed upper bound

v0 accepts only key 0.

15.2 Map update

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

15.3 Map lookup

v0 may return -ENOTSUPP.

Alternative:

lookup key 0 returns metadata only

15.4 Map delete

v0 may either:

clear current graph

or:

return -ENOTSUPP

The ringbuf precedent supports specialized maps that do not implement generic lookup/update/delete semantics fully.  ￼

15.5 Memory accounting

map_mem_usage must include:

sizeof(struct bpf_iograph_map)
current graph object
nodes
edges
entries
accepts
JIT image

LPM_TRIE and ringbuf both implement memory accounting in their map ops.

16. In-kernel Graph Object

The kernel may normalize the blob into an immutable object.

struct iog_graph {
    struct rcu_head rcu;
    u32 node_cnt;
    u32 edge_cnt;
    u32 entry_cnt;
    u32 accept_cnt;
    u32 alphabet_size;
    struct iog_node *nodes;
    struct iog_edge *edges;
    struct iog_entry *entries;
    struct iog_accept *accepts;
    struct iog_jit_image *jit;
};

Run path must not allocate.

allowed:
  rcu_dereference graph
  read nodes/edges
  update local state
  return
forbidden:
  kmalloc
  mutex
  spinlock
  FPU
  per-CPU scratch allocation

17. JIT v0

v0 JIT is not a general graph JIT.

It supports only structures needed to test the thesis.

17.1 Required lowering

single-state self-loop SCC -> tight loop
sparse outgoing edges -> cmp chain
accept/reject return

17.2 Optional lowering

dense outgoing edges -> jump table

This may be v0.5 if code size grows.

17.3 Self-loop target

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

This is the primary v0 performance claim.

17.4 JIT lifetime

JIT image lifetime is tied to struct iog_graph.

new graph:
  build JIT image before publish
update:
  publish new graph with rcu_assign_pointer
old graph:
  call_rcu
  free JIT image after grace period

18. Interpreter

Interpreter is required.

It is used for:

correctness reference
JIT differential testing
fallback
debugging
fuzzing

The interpreter must produce the same trace as the JIT for:

state sequence
accept sequence
final state
status

19. Differential Testing

Every JIT-supported graph must be tested against the interpreter.

Test matrix:

random sparse graphs
random self-loop graphs
random default chains
random input buffers
random entry states
edge ordering edge cases
invalid blob rejection
RCU update during execution

Required property:

interpreter_trace == jit_trace

20. Target Workloads

v0 targets exactly three workload families.

20.1 Self-loop scanner

Examples:

scan until delimiter
scan until non-class byte
scan until marker byte

Naive intermediate:

regex scratch
match object
generated BPF loop
dense table

Invariant:

state + cursor + accept_code

Win metric:

cycles/byte
branch misses
JIT image size
throughput vs BPF FSM

20.2 Sparse FSM classifier

Examples:

policy classifier
protocol state classifier
byte stream classifier

Naive intermediate:

dense transition table

Invariant:

current_state + accept_code

Win metric:

artifact size
transition/sec
d-cache misses
update latency

20.3 Many-schema shared continuation

Examples:

many event schemas
many policy patterns
shared error/skip/accept paths

Naive intermediate:

schema_count × generated BPF programs
schema_count × transition tables

Invariant:

entry_state + current_state + accept_code

Win metric:

total artifact size
graph update latency
BPF verifier/JIT reload avoided

21. Benchmarks

21.1 Baselines

A. hand-written C FSM
   upper bound
B. hand-written BPF FSM, JITed
   realistic BPF competitor
C. generated BPF flat-table FSM, JITed
   flattening baseline
D. flat transition table walker
   data-only flattening baseline
E. LPM_TRIE-style acyclic map lookup
   map-type precedent baseline
F. io_graph interpreter
   reference
G. io_graph JIT
   target

If a regex-kfunc baseline is available and reproducible, include it separately.

21.2 Required metrics

artifact bytes
flat table bytes
generated BPF object bytes
BPF verifier time
BPF JIT time
io_graph verify time
io_graph JIT time
update latency
lost invocations during update
transitions/sec
cycles/byte
branch misses
i-cache misses
d-cache misses
run-path allocations
per-CPU memory footprint

21.3 Required experiments

self-loop scanner:
  io_graph JIT must beat or match BPF FSM
sparse FSM:
  cyclic blob must be materially smaller than flat table
hot reload:
  graph update must be materially cheaper than BPF reload
many-schema:
  shared continuation must reduce total artifact size

22. Security Model

v0 keeps the attack surface small by design.

22.1 Trusted

kernel verifier
kernel interpreter
kernel JIT after verification
RCU lifetime rules

22.2 Untrusted

userspace graph blob
v2/oracle
canonicality claims
minimality claims
BPF program inputs

22.3 Run path restrictions

no dynamic allocation
no sleeping
no helper calls from graph
no arbitrary pointer access
no kernel pointer exposure
no FPU/SIMD in v0
no capture buffers

23. Concurrency and Update Model

Graph object is immutable after publication.

Update is whole-object replacement.

old graph remains valid during RCU read-side critical sections
new graph becomes visible atomically
old graph freed after grace period

This avoids incremental mutation complexity.

Compared to LPM_TRIE, which mutates a pointer-linked tree under lock and frees nodes through RCU, io_graph should use coarser but simpler whole-graph replacement.  ￼

24. Prior Art Boundary

24.1 DFA compression

DFA compression and minimization are not new.

io_graph does not compete on general compression ratio.
It competes on runtime preservation of the minimized cyclic artifact.

24.2 Regex engines

Hyperscan-style engines are broader and more powerful.

They also maintain rich runtime state and scratch space.

io_graph wins only by rejecting that expressiveness.

24.3 regex-automata dense DFA

regex-automata explicitly documents dense DFA table size and byte-class reduction.  ￼

io_graph targets cases where table materialization is the wrong runtime artifact.

24.4 BPF map precedents

LPM_TRIE shows BPF map type as a kernel-side lookup data structure.

Ringbuf shows BPF map type can act as a specialized container object rather than a normal key/value map.

io_graph should follow these precedents.

25. Upstream Strategy

Do not pitch v0 as:

new kernel subsystem
new regex engine
new BTF runtime
new parser VM

Pitch v0 as:

BPF map type for minimized cyclic FSM artifacts,
with optional self-loop JIT and RCU graph replacement.

Patch structure:

1. bpf: add BPF_MAP_TYPE_IOGRAPH skeleton and UAPI
2. bpf: iograph cyclic graph blob format and verifier
3. bpf: iograph interpreter walker
4. bpf: iograph step/run kfuncs
5. bpf: iograph x86-64 self-loop JIT
6. selftests/bpf: verifier and interpreter/JIT equivalence
7. selftests/bpf: benchmarks against BPF FSM, flat table, LPM_TRIE-style lookup

Patches 1–4 must be useful without JIT.

Patch 5 carries the thesis.

26. Implementation Budget

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

27. Roadmap

v0: Minimal cyclic FSM map

BPF_MAP_TYPE_IOGRAPH-like map
graph blob verifier
interpreter
step/run kfuncs
self-loop x86-64 JIT
RCU whole-graph update
benchmarks

v0.5: Layout and JIT refinement

small-edge inline nodes, if benchmark proves useful
dense edge jump table
hot SCC-only JIT
better graph memory layout

v1: Upstreamable BPF map type

clean map API
bpftool introspection
libbpf support
selftests
documentation

v2: BTF/v2 integration

optional BTF/reference-kind graph frontend
v2 oracle diff/explain integration
canonicality metadata for userspace only

v3: Producer-side use cases

trace/event policy classifiers
kernel-side scanners
ringbuf prefiltering
LSM policy FSMs

28. Open Questions

Should map_update_elem require fixed value_size == max_blob_size?
Should variable-size graph blobs use a custom update path?
Should run() be required in v0, or is step() enough?
Should dense jump tables be v0 or v0.5?
What is the default chain depth cap?
Should JIT be opt-in per map?
How should bpftool display graph size and JIT size?
Should accept_code be opaque u32 or structured?
Should delete clear the graph or be unsupported?
Can self-loop JIT beat BPF FSM on real workloads?

29. Maintainer-facing Summary

io_graph is a BPF-map-shaped runtime artifact for minimized cyclic FSMs.

It is designed for narrow workloads where existing implementations materialize large intermediate objects, but the final answer only requires a small invariant:

state
cursor
accept code
optional bounded accumulator

It does not compete with general regex engines or Hyperscan.
It does not introduce a standalone kernel subsystem.
It does not run minimization in kernel.

The initial implementation is a small BPF map type prototype:

immutable cyclic graph blob
load-time verifier
allocation-free walker
experimental step/run kfuncs
RCU graph replacement
minimal self-loop JIT

The hypothesis is falsifiable:

If cyclic graph artifacts are not smaller,
if updates are not cheaper,
or if self-loop JIT does not beat BPF FSM baselines,
then io_graph should not become an upstream feature.

The reason to try it is equally clear:

When the minimized cyclic form is the natural answer,
flattening it into tables or generated BPF is the intermediate object.
io_graph avoids constructing that object.
