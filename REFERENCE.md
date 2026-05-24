# Reference Trail

This file records the external history that motivated the current io_graph
prototype. It is intentionally separate from `SPEC.md`: the spec defines the
design, while this file explains why the design is shaped around pre-emission
action decisions, compact graph data, direct selector-buffer traversal, and
DROP-before-reserve ordering.

## Public Name

Issue #6 proposes renaming the public surface from `iograph` to
`BPF_MAP_TYPE_PREFIX_POLICY`. The repo can keep `io_graph` / `iograph` as the
internal prototype name for C symbols, file names, benchmark rows, and the
experimental kernel overlay while the implementation is still moving. Public
documents should nevertheless make the rename direction clear:

- public-facing name: `BPF_MAP_TYPE_PREFIX_POLICY`;
- internal/prototype implementation name: `iograph`;
- current repository name: `io_graph`.

The important caveat is semantic. `PREFIX_POLICY` names the first upstreamable
workload shape: bounded string/byte-prefix selector policy that returns a small
action code before event emission. It should not be read as a general security
policy engine, path canonicalizer, regex engine, parser VM, or authorization
boundary. The object still evaluates raw bounded selector bytes and leaves
object identity, path resolution, and enforcement-sensitive semantics to the
integrating subsystem.

This split keeps the implementation stable while aligning the public wording
with the requested map-type name. If the public UAPI is later renamed
mechanically, the docs should preserve this boundary: `PREFIX_POLICY` means
compact verified prefix-policy graph data and a small pre-emission action
decision, not arbitrary policy execution in the kernel.

## Main Claim

The external evidence does not show that `BPF_MAP_TYPE_LPM_TRIE` is unable to
hold six thousand to ten thousand prefixes. That would be the wrong claim.
LPM_TRIE can hold many prefixes and remains the correct baseline for pure
longest-prefix lookup.

The stronger and safer claim is different:

- Existing Falco/Tetragon-style selector machinery is not designed to carry
  thousands of raw string or file-prefix values in BPF program logic.
- LPM_TRIE can store many prefixes, but raw path/string selectors still need a
  BPF hot-path `prefixlen+data` key before the program can decide whether to
  skip event materialization.
- The current kernel LPM trie has known locality and lifecycle tradeoffs from
  binary-node traversal and scattered node allocation. Those tradeoffs matter
  more as prefix sets and update/delete/free paths grow.
- The missing primitive is therefore not generic prefix lookup. It is
  high-cardinality raw-byte prefix/action lookup before event materialization.

This is the niche that `BPF_MAP_TYPE_PREFIX_POLICY` should name.

## 1. Falco libs #1557

Source: [falcosecurity/libs#1557](https://github.com/falcosecurity/libs/issues/1557)

Falco libs issue #1557 asks for conditional kernel-side event filtering. The
reported workload is a large, highly utilized deployment where benign,
repetitive events still traverse Falco's userspace rule-evaluation path and
contribute to drops. The requested shape is directly relevant to io_graph:
ignore events such as `exec` by `proc.cmdline` or file-open events by `fd.name`
before the event is allocated on the ring buffer.

The follow-up discussion is as important as the original request. Maintainers
agreed that pushing every event to userspace does not scale, but warned that
kernel-driver work runs in application context and can slow the workload being
observed. They also called out driver maintenance across a wide kernel matrix
and eBPF verifier failures as practical costs. That maps directly to io_graph's
scope limits: verified graph data, bounded interpreter execution, no regex VM,
no helper calls from graph execution, no allocation on the run path, and no
claim that this is a full Falco rule engine in kernel space.

The issue also explains why the first practical selector class should be file
or path prefix data rather than arbitrary event semantics. A maintainer
suggested starting with `fd.name` prefix filtering and warned against casually
dropping fork/exec events because Falco's process-cache and state engine depend
on those events. Later comments describe event rates above twenty million per
second, file-open-heavy workloads, and the possibility that very aggressive IP
or file-path prefix filtering in kernel space may be needed. That history is
the closest external match for the io_graph thesis: rejected events should be
decided before ringbuf materialization, but the decision primitive must remain
small enough to justify running in the producer context.

Design consequences for io_graph:

- Treat Falco-style integration as a pre-ringbuf action oracle, not a kernel
  copy of Falco's userspace rules.
- Keep DROP-before-reserve as a primary measurement, because the requested win
  is avoiding the ringbuf record entirely.
- Keep selector acquisition explicit. A cheap graph walk is not enough if the
  integration has already built the full event parameter buffer.
- Keep process-state-sensitive events as an integration concern, not something
  the generic graph primitive decides.

## 2. Tetragon #1278

Source: [cilium/tetragon#1278](https://github.com/cilium/tetragon/issues/1278)

Tetragon issue #1278 requests a `Prefix` operator for `matchBinaries`. The
example use case is forbidding network activity for every binary under a
specific directory without listing each executable. That is the same product
shape as high-cardinality prefix policy: users want a compact selector rather
than generated lists of exact strings.

The comments show the implementation tension. Existing `matchBinaries`
machinery stores binary identity from the exec stage and later answers whether
the current process matched; it does not necessarily carry the full string into
every later kprobe selector. A maintainer explained that an older prefix
implementation parsed potential strings byte by byte and was slow, while a
newer implementation used `LPM_TRIE`, which is optimized for prefix lookup and
is populated from userspace at load time. Prefix and postfix support would
therefore require carrying complete binary paths into the persisted exec map so
that a later kprobe can perform the prefix lookup.

This is a useful contrast for io_graph. Tetragon's LPM route is a good
baseline for pure prefix lookup, but it needs selector materialization into a
lookup key or persisted map value. io_graph's benchmarked fast path instead
walks the original bounded selector buffer directly and returns an action code
before event emission. The distinction matters: LPM_TRIE can be excellent for
plain prefix lookup, but the pipeline still pays for whichever intermediate
object is required to construct the LPM key or preserve the selector string.

The current Tetragon BPF-side selector machinery also shows why the thousands
of prefixes case is outside the existing selector shape. In
`bpf/process/types/basic.h`, ordinary match values are capped at 4, string
match values are capped at 2, and file/fd match values are capped at 2 or 8
depending on the large-BPF-program build. The nearby source comment explicitly
ties string parsing to BPF instruction cost. Tetragon does expose prefix-style
operators, but the BPF selector value layout is tuned for a handful of
conditions, not six thousand to ten thousand file/string prefixes.

Design consequences for io_graph:

- Always compare against LPM_TRIE, including bounded-copy key construction.
- Report direct-buffer traversal separately from any selector acquisition row.
- Keep `PREFIX_POLICY` scoped to bounded raw selector bytes to action code, not
  a general Tetragon selector replacement.

## 3. Tetragon #4323

Source: [cilium/tetragon#4323](https://github.com/cilium/tetragon/issues/4323)

Tetragon issue #4323 asks for reusable selectors across many hooks. The
motivating example repeats `matchBinaries NotPrefix` across multiple calls, or
uses a common binary-prefix exclusion while each syscall has different argument
positions for a pathname. This is not just a syntax problem; it shows that
real policies often repeat the same negative-heavy selector across many event
sources.

The naming discussion is directly relevant to io_graph's rename issue. The
thread starts with `globalSelectors`, but reviewers point out that Tetragon
already has `podSelector`, where selector means target selection rather than
event filtering. Reusing selector-like naming risks making two different
intentions look the same. The discussion moves toward explicit named macros,
with a preference for explicit use over implicit application to every selector.

Design consequences for io_graph:

- Public names should avoid overloading terms that already imply target
  selection, enforcement, or Kubernetes object selection.
- The graph object should remain an action-decision primitive; policy-language
  macros and reusable selector syntax belong in producers/loaders.
- Multi-entry support and `run_action_idx()` are important because a single
  compiled graph may serve several hook-specific selector entries.

## 4. Tetragon #4821

Source: [cilium/tetragon#4821](https://github.com/cilium/tetragon/issues/4821)

Tetragon issue #4821 reports high-load performance problems with more than ten
TracingPolicies and metrics for missed ring-buffer and userspace queue events.
The reporter tried larger ring buffer settings, but the result was not a
simple fix. A maintainer summarized the core tradeoff: if userspace is
consuming too slowly, larger buffers may absorb temporary bursts but will not
fix sustained overproduction; the choices are to produce fewer events or make
userspace consume faster.

A later comment gives the exact kind of workload io_graph is intended to
study: heavy `security_file_permission` and network-connect policies, many
policies, hard-to-filter versioned binary paths, and a note that a userspace
export denylist helps less than kernel-level filtering would. Increasing
ring-buffer capacity improved drops, but that is still a buffering mitigation,
not a change in event materialization order.

Design consequences for io_graph:

- The benchmark must show bytes not emitted, not only nanoseconds per match.
- POST-side payload rows are necessary because prefiltering adds decision cost
  to events that are still emitted.
- DROP-before-reserve must be compared with reserve-then-discard and
  userspace-drop models to show why event production order matters.

## 5. Falco #1743

Source: [falcosecurity/falco#1743](https://github.com/falcosecurity/falco/issues/1743)

Falco issue #1743 is an older concrete drop report. A Debian deployment of
Falco 0.29.1 generated many syscall event drop alerts under moderate host load,
even after the user followed the dropped-events checklist. The issue includes
second-by-second drop counts in the hundreds of thousands.

This issue is not about prefix filtering specifically, but it anchors the
problem historically: syscall event drops were visible to users years before
the current io_graph prototype. It reinforces that the goal is not a synthetic
microbenchmark. The relevant external pain is sustained producer/consumer
pressure between the kernel driver and userspace consumer.

Design consequences for io_graph:

- Results should keep ringbuf bytes/sec and rejected-event bytes at the same
  level of importance as ns/op.
- Claims should stay pipeline-focused: avoid materializing known rejects,
  reduce userspace decode/rule work, and keep emitted events for policy cases
  that require userspace.

## 6. Falco #1403

Source: [falcosecurity/falco#1403](https://github.com/falcosecurity/falco/issues/1403)

Falco issue #1403 is the dropped-events umbrella. It frames Falco's event path
as a producer driver, a userspace consumer, and a buffer between them. When the
consumer cannot keep up, events drop. The issue also explains the different
drop counters and identifies buffer drops as the frequent performance-related
case.

The umbrella lists several causes: CPU limits, large or complex rulesets,
metadata fetching that blocks on external systems, blocking outputs, and buffer
configuration. This is useful because it prevents a too-narrow interpretation
of io_graph. A faster string matcher alone cannot solve all dropped-event
causes. What io_graph can test is one specific lever: when a selector action
can be decided from bounded bytes, do that before event construction and avoid
ringbuf/user-decode work for rejects.

Design consequences for io_graph:

- Keep explicit non-goals for full Falco rule evaluation, metadata fetching,
  output backpressure, and state-engine correctness.
- Report prefilter savings as avoided intermediate objects: event parameter
  buffers, ringbuf records, userspace event objects, and rule-engine work.
- Maintain "fail-open action 0" language so the prototype is not mistaken for
  an authorization boundary.

## 7. Falco #1204

Source: [falcosecurity/falco#1204](https://github.com/falcosecurity/falco/issues/1204)

Falco issue #1204 proposed a Falco profiler and named two performance areas:
rules parsing/matching and syscall drops. It describes Falco's syscall path as
adding an observation round trip between kernel and userspace, with the shared
buffer existing because of that extra path.

The profiler request matters to io_graph because it separates two things that
are easy to conflate. One axis is making userspace rule matching more visible
and optimizable. The other is reducing dropped syscalls caused by the
kernel/userspace event path. io_graph deliberately works on the second axis by
moving a small, bounded action decision before ringbuf materialization. It does
not replace profiling or optimization of the full userspace rules engine.

Design consequences for io_graph:

- Keep perf counters, p95/p99/p999, and provenance in result files.
- Distinguish primitive decision cost from pipeline cost.
- Make clear that io_graph complements, rather than replaces, userspace
  profiling and rule-engine work.

## 8. Tetragon PR #686

Source: [cilium/tetragon#686](https://github.com/cilium/tetragon/pull/686)

Tetragon PR #686 improved `matchBinaries`, kept the original exact-match
operator path efficient, removed a four-value limit, and added `NotIn`. It
predates the later prefix-operator request, but it shows the same evolutionary
pressure: binary selectors started with exact inclusion/exclusion, then users
needed larger value sets and richer operators.

The patch is also a useful scale marker. It moves `matchBinaries` toward map
backing by using a global `names_map` and a per-sensor `sel_names_map`, and it
raises the global names map to 256 entries for binary names across selectors.
It also keeps binary path handling in 256-byte pathname buffers. This is a real
improvement over four inline values, but it is not a thousands-prefix backend
and it remains exact-name machinery rather than a high-cardinality prefix
runtime.

That history is a reminder that generated comparisons and ad hoc per-operator
logic tend to grow. io_graph's representation thesis is that high-cardinality
selector policy should grow as verified graph data and compact runtime graph
data, not as BPF program control flow or a sequence of userspace drop rules.

Design consequences for io_graph:

- Keep artifact-size comparisons against generated string-compare chains.
- Keep update-cost measurements, because richer selector policy is only useful
  if whole-policy replacement remains practical.
- Treat exact-match, prefix-match, and shared-selector cases as policy shapes
  compiled into the same bounded graph mechanism.

## 9. Cloudflare LPM Trie Performance Study

Source:
[A deep dive into BPF LPM trie performance and optimization](https://blog.cloudflare.com/a-deep-dive-into-bpf-lpm-trie-performance-and-optimization/)

Cloudflare's LPM trie study is an important baseline and caution. It describes
BPF LPM trie maps as fundamental for IP and IP+port matching, but also shows
that the implementation can suffer from lookup, update, delete, and free-path
costs at scale. The article explains why tries benefit from shared prefixes,
how path compression helps sparse chains, why level compression can matter for
dense upper levels, and why BPF LPM's two-child internal node structure can
increase traversal height. It also ties large tries to L1 data-cache and dTLB
miss behavior because dynamically allocated trie nodes may live at unrelated
addresses.

For io_graph this is not an argument that LPM_TRIE cannot handle ten thousand
prefixes. The same article includes a ten-thousand-entry lookup benchmark at
134.710 ns/op. The point is that LPM is only the lookup structure. For raw
path/string pre-emission filtering, the BPF program still has to acquire
selector bytes and materialize a `prefixlen+data` key before lookup. And as the
prefix set grows, LPM's pointer-heavy binary-node shape, cache/TLB behavior,
and update/delete/free paths become part of the operational cost. Cloudflare
documents production-scale bottlenecks at much larger entry counts, including
very slow lookups and long map-free times.

The current measurement discipline follows from that nuance: compare against
LPM, include full-key and bounded-copy key materialization, account for
cache/TLB behavior, and avoid claiming that one map type is universally faster.
io_graph's compact runtime is deliberately contiguous, publication-derived
graph data for bounded selector action decisions; its advantage should be
stated only for this workload shape.

Design consequences for io_graph:

- Keep full-key and bounded-copy LPM rows.
- Measure PMU counters on a host or VM that exposes them.
- Preserve compact runtime stats: nodes, edges, literal-tail bytes, dispatch
  tables, active memory, and update scratch/peak bytes.
- Use "same-hook, same-selector workload" language when comparing with LPM.

## 10. DeepWiki Cilium BPF Maps And State Management

Source:
[BPF Maps and State Management](https://deepwiki.com/cilium/cilium/3.3-bpf-maps-and-state-management)

DeepWiki's Cilium map overview is a secondary source, but it is useful context
for how BPF systems are organized in practice. It describes BPF maps as central
state shared between userspace and BPF programs, notes map registry and pinned
map replacement patterns, and summarizes dynamic map sizing and allocation.

It also shows where LPM is already a good fit. Cilium's IPCache uses an LPM
trie for IP identities and CIDR-style matching, while policy maps handle
structured network-policy keys such as source identity, destination port, and
protocol. That is a different workload from raw path/cmdline/argv prefix
classification before event materialization. The Cilium precedent supports
"map-backed policy/state is normal"; it does not make LPM a complete answer
for raw string selector action policy.

io_graph fits that ecosystem better as a map-backed, RCU-published policy
object than as generated BPF code. The loader uploads verified graph data; map
publication builds the executable compact runtime; BPF programs call a small
kfunc to get an action. This keeps the BPF program shape stable while policy
size grows as data and while whole-policy replacement can remain an update
operation.

Design consequences for io_graph:

- Keep policy as map data, not generated program control flow.
- Keep publication and replacement semantics explicit: verify, build compact
  runtime, publish, and retire the old graph via RCU.
- Keep active-memory accounting and update latency alongside run-path cost.

## Summary

The external trail is consistent:

- Falco users report dropped syscall events and ask for kernel-side filtering
  before ringbuf allocation.
- Falco maintainers warn that kernel-side work must remain cheap and bounded.
- Tetragon users ask for prefix/postfix and reusable selectors, but existing
  designs often require carrying selector strings or materializing lookup keys.
- Tetragon's current BPF selector machinery is sized for small string/file
  value counts, and the `matchBinaries` map improvement is a 256-name
  exact-match structure, not a thousands-prefix backend.
- LPM_TRIE is the right baseline for pure prefix lookup and can store many
  prefixes, but raw string/path pre-emission filtering still pays key
  materialization and inherits LPM's locality/update/delete/free tradeoffs.
- Large BPF systems already manage policy/state through map-backed objects, so
  a verified graph-data object with RCU replacement is a natural shape.

That history supports the current `BPF_MAP_TYPE_PREFIX_POLICY` direction:
verified source blob, publication-time compact action graph, direct bounded
selector traversal, small action code, DROP-before-reserve, and no claim to be
a regex engine, Falco/Tetragon rule engine, path canonicalizer, or security
boundary.
