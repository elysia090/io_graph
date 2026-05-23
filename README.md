# io_graph

`io_graph` is a BPF map type prototype for compact verified graph policy data.
The userspace proof is complete; active work is the kernel v0 path in
`kernel/`:

- `BPF_MAP_TYPE_IOGRAPH` map allocation and graph update;
- blob verification before publication;
- immutable RCU graph replacement;
- interpreter kfuncs matching `SPEC.md`;
- pre-ringbuf BPF demo and selftests.

Run:

```sh
make
make test
```

Native Linux validation entrypoint:

```sh
make validate-native
```

Start with [SPEC.md](SPEC.md), [kernel/README.md](kernel/README.md), and
[docs/RESULTS.md](docs/RESULTS.md). Benchmark methodology is in
[docs/BENCHMARKING.md](docs/BENCHMARKING.md); source-code precedents and kernel
design notes are in [docs/DESIGN_NOTES.md](docs/DESIGN_NOTES.md). The frozen
userspace proof snapshot is kept under `results/userspace/`.
