# io_graph

`io_graph` is a BPF map type prototype for compact verified graph policy data.
The userspace proof is complete; active work is the kernel v0 path in
`kernel/`:

- `BPF_MAP_TYPE_IOGRAPH` map allocation and graph update;
- blob verification before publication;
- immutable RCU graph replacement;
- interpreter kfuncs matching `SPEC.md`;
- pre-ringbuf BPF demo and selftests.

The userspace benchmark and kernel map publication path both build a
runtime-only compact graph that folds verified single-child byte chains into
literal-run edges. Literal edges store only tail bytes after the dispatch byte,
and terminal final-action leaves can be returned directly from the incoming
compact edge. The source blob remains the verified byte-trie artifact;
`run_action()` uses the compact runtime graph for the pre-emission hot path.
High-fanout compact nodes can use a derived byte dispatch table, and
`run_action_idx()` gives loader-known entry indexes a direct entry selection
path.

Run:

```sh
make
make test
```

Native Linux validation entrypoint:

```sh
make validate-native
```

Linux overlay edit/build loop:

```sh
sh scripts/build_linux_overlay_minimal.sh /path/to/linux /path/to/build
```

Start with [SPEC.md](SPEC.md), [kernel/README.md](kernel/README.md), and
[docs/RESULTS.md](docs/RESULTS.md). Benchmark methodology is in
[docs/BENCHMARKING.md](docs/BENCHMARKING.md); source-code precedents and kernel
design notes are in [docs/DESIGN_NOTES.md](docs/DESIGN_NOTES.md). The frozen
userspace proof snapshot is kept under `results/userspace/`.
