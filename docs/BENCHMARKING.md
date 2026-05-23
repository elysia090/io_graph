# Benchmarking

This benchmark answers the second thesis:

> Rejected events should become in-kernel decisions, not ringbuf records.

It does not claim enforcement semantics and does not include Falco/Tetragon
integration or JIT. It measures the hot decision and accounts for the I/O
avoided when that decision happens before event emission.

## IO-aware shape

The naive pipeline creates a large intermediate object before it knows whether
the event will be dropped:

1. copy the event payload into a ring buffer record;
2. deliver the record to userspace;
3. decode it into a userspace event object;
4. run policy over path/cmdline/arg bytes;
5. drop most events.

The `io_graph` path changes the execution order:

1. read only the raw selector bytes already available at the hook;
2. maintain the small invariant `state, cursor, action_code`;
3. return DROP/POST;
4. materialize a ringbuf record only for POST.

For DROP, the benchmark reports 0 ringbuf bytes and no decoded userspace event.
The avoided intermediate object is `ringbuf_record(payload) + decoded_event`.
The benchmark runs the decision through the BPF map operations harness. The
userspace shim now mirrors kernel publication: map update verifies and copies
the byte-trie blob, inlines accept codes, builds the compact runtime graph, and
`bpf_iograph_run_action` semantics use that published compact graph. The
`bpf event path` table executes the order directly: negative rows run the
compact graph and skip record construction, while positive rows copy one
aligned BPF ringbuf record including its 8-byte header.

## Implemented paths

The C benchmark compares:

- `io_graph` byte-trie interpreter primitive before payload materialization;
- `io_graph_compact_chain`, the published compact `run_action()` primitive;
- ringbuf-then-userspace-drop, accounted as record materialization plus decode
  for every event, followed by userspace list-prefix evaluation;
- generated string-compare chain, implemented as repeated byte compares with a BPF
  code-size lower-bound estimate;
- list-backed prefix loop.

The event-path rows execute the order required by the kernel prototype:

1. evaluate raw selector bytes with `bpf_iograph_run_action` semantics;
2. return immediately for DROP;
3. reserve and copy one ringbuf record only for POST.

Negative rows should report `0` emitted ringbuf bytes. Positive rows include
the aligned ringbuf record plus the modeled 8-byte ringbuf header.

## Measurement Boundaries

The userspace decision table is a policy-evaluation primitive. Prefixes and
selector byte buffers are prebuilt before the timed loop. Path acquisition,
path canonicalization, and path-string generation are intentionally not folded
into that number.

The Linux selftests bench is hook-adjacent rather than a pure kfunc
microbenchmark:

| Row | Attach/trigger | Policy bytes | Payload |
|:---|:---|:---|:---|
| `decision cost` | direct C call | prebuilt selector samples | none |
| `bpf event path` | userspace BPF-shaped model | prebuilt selector samples | compact `run_action()` first; POST copies 300 B, 800 B, or 2 KiB |
| `iograph-hook-floor` | `raw_tp/sys_enter`, triggered by `getpgid` | none | no graph lookup, batched host trigger counter |
| `iograph-decision` / `iograph-compact-decision` | `raw_tp/sys_enter`, triggered by `getpgid` | preloaded writable BPF global | action only; both names use the current compact `run_action()` path after publication |
| `iograph-prefilter` / `iograph-compact-prefilter` | `raw_tp/sys_enter`, triggered by `getpgid` | preloaded writable BPF global | DROP-before-reserve path; both names use the current compact runtime, and the compact name is the explicit current row |
| `iograph-lpm-decision` | same `raw_tp/sys_enter` bench | same selector plus LPM key scratch | action only, batched host trigger counter |
| `iograph-lpm-prefilter` | same `raw_tp/sys_enter` bench | same selector plus LPM key scratch | current kernel bench event is small |

The DROP path reaches the action answer before ringbuf reservation. The POST
copy rows stay separate because event materialization is the large intermediate
object being avoided for rejected events.

The kernel DROP rows count triggers in the producer thread in 1024-call
batches. They do not add a BPF global atomic increment after the action says
DROP, and they do not add a userspace atomic to every syscall trigger; either
would measure benchmark bookkeeping on the rejected hot path rather than the
pre-emission decision itself. `iograph-hook-floor` keeps the same syscall,
attach, and empty BPF dispatch path so decision rows can be read against the
fixed hook floor.

## Workload dimensions

The default run covers:

- 100, 1000, and 10000 prefixes;
- 100k, 500k, and 1M events/sec;
- 90%, 95%, 97%, and 99% negative/drop distributions;
- 300 B, 800 B, and 2 KiB event payloads;
- early mismatch, late mismatch, and positive match decision cases.

The default dataset is `typical`. Stress the graph side separately with:

```sh
tools/iog_bench/iog_bench --counts 1000 --dataset shared-prefix
tools/iog_bench/iog_bench --counts 1000 --dataset long-path
```

`shared-prefix` pushes many policy rows through a long common path before they
diverge. `long-path` makes that shared walk longer while retaining late-miss and
hit samples near the end of the prefix.

## Metrics

Reported directly:

- ns/op;
- matched-path mean ns/input byte and ns/successful graph transition;
- batch p95/p99/p999 ns/op for the userspace decision primitive;
- cycles/op on x86 via `rdtsc`;
- best-effort branch/cache misses through `perf_event_open`;
- artifact size;
- graph verify, policy update, reclaim, and active-memory cost;
- materialization plus decode cost;
- BPF event-path cost and emitted ringbuf bytes per event;
- run-path allocations.

Reported by estimate:

- ringbuf bytes/sec before and after prefiltering;
- traffic reduction;
- avoided intermediate-object bytes/sec;
- CPU cores spent on filtering;
- downstream CPU avoided versus ringbuf-then-userspace-drop;
- speedup versus list-backed prefiltering;
- artifact-size ratio versus dense table and generated-chain estimates.

If perf counters are unavailable, the branch/cache columns print `na`; the
benchmark still reports time, cycles, size, bytes/sec, and CPU-core estimates.

The userspace percentile columns are per-batch ns/op distributions, with up to
1024 timed batches in the decision loop. They avoid reporting
`clock_gettime()`-dominated single-call pseudo-latencies for sub-microsecond
matchers.

## Matched Path Matrix

The matched-path table separates raw input bytes from successful graph state
advances. Exact rows consume one successful transition per input byte. Prefix
rows keep the suffix bytes in `input_B/op`, while
`matched_transitions/op` stays at the accepted prefix length. That makes the
remaining matched-path work visible as mean ns/input byte and mean
ns/successful transition alongside batch p95/p99/p999 ns/op.

The copied userspace and kernel graph objects inline accept codes after blob
verification by rewriting each runtime node's accepted ID into the action code.
The verified source blob format still carries `accept_id -> accept_code`, but
the run path does not need an `accepts[]` lookup on every accepting node.
Userspace map publication and kernel map publication then derive a compact
runtime graph for the action-only prefilter path: single-child byte chains
become literal-run edges, while range edges, accepting nodes, final-action
nodes, entry states, and consuming else transitions remain explicit graph
nodes. The byte-trie matcher remains in the benchmark as
`io_graph_byte_trie`; it is a primitive comparison row, not the current
BPF-shaped event path.
Accepting leaf nodes are emitted with `IOG_NODE_F_FINAL_ACTION`, which lets the
last-accept action walker return early without changing longest-match
semantics for nodes that still have outgoing override edges.

`io_graph_accept_inline_first_final_action` returns at the first non-zero
action. That path is only the right semantics when the caller knows the
accepted action is final, such as a prefix DROP policy with no longer override.
Longest-match policies stay on `io_graph_accept_inline_last_accept`.

`default_dst` is treated as a consuming else transition in v0: explicit edge
miss moves to `default_dst` and consumes one input byte. It is not a
non-consuming fallback chain.

The current C rows are:

| requested case | current row shape |
|:---|:---|
| `io_graph 100 exact short match` | `--counts 100 --dataset typical`, exact input equals policy prefix |
| `io_graph 100 exact long match` | `--counts 100 --dataset long-path`, exact input equals policy prefix |
| `io_graph 100 prefix accept-early-return` | hit input with suffix, first-final-action walker |
| `io_graph 100 prefix longest-match` | hit input with suffix, last-accept walker |
| `io_graph 1000 with accept_code inline` | same table, `io_graph_accept_inline_*` matcher rows |
| `io_graph 1000 with single-child chain compression` | `io_graph_compact_chain`, a runtime-only graph built from the verified byte-trie blob; kernel `run_action()` uses the same compact shape after map publication |

The remaining optimization rows must stay separate instead of borrowing
interpreter numbers:

- `io_graph 1000 JIT chain compare`;
- `io_graph 1000 JIT self-loop`.

The compact row reports both byte input cost and compact edge transition cost.
For compact rows, `matched_transitions/op` is the number of literal/range graph
edge advances, not the byte-trie transition count. JIT rows still require the
JIT image being measured. The table schema is normalized for all of them: mean
ns/op, mean ns/input byte, mean ns/successful transition, and batch
p95/p99/p999 ns/op.

The compact runtime graph stats table also reports compact nodes/edges,
literal edge count, literal pool bytes, mean and max literal length, max
compact fanout, and max compact depth when the runtime graph is acyclic in
publication order. Cyclic graphs keep the depth column explicit as `cyclic`
instead of pretending there is a finite longest path.

## Reject Mix

The model keeps 90%, 95%, 97%, and 99% reject rows. The 95% row is not a claim
about every deployment: it is a negative-heavy operating point near public
pre-filtering evidence. Falco documents high syscall-event pressure and points
to conditional kernel-side event filtering; Datadog describes a production
approver/discarder design that filters up to 94% of events in kernel before
userspace processing.

References:

- https://falco.org/docs/troubleshooting/dropping/
- https://www.datadoghq.com/blog/engineering/workload-protection-ebpf-fim/

## Native Linux

Run the userspace proof on a native Linux host with:

```sh
make validate-native
```

`validate_native.sh` prints the kernel identity, `perf_event_paranoid`, CPU PMU
exposure when readable, and the benchmark output. Hardware branch/cache/L1/LLC
counters come from `perf_event_open`; PMU policy or virtualization can leave
those columns as `na` without invalidating the size and timing rows.

Kernel measurements are separate. Record them under `results/native/` only
after loading a kernel that carries the map type and kfunc prototype.
