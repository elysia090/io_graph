# Native Kernel Results

Do not paste userspace benchmark numbers here as kernel measurements.

Keep `current.md` honest about the latest overlay/build validation even when
the booted kernel cannot run the new map type yet.

Use `TEMPLATE.md` for new compact-runtime kernel runs. Keep old byte-trie
kernel rows as historical snapshots and write refreshed compact rows to a new
dated file, for example `compact-2026-05-23.md`.

## Measurement Tree Discipline

Keep the io_graph repository, Linux source tree, build output, and boot
artifacts separate:

```text
~/src/io_graph
~/src/wsl2-linux-base
~/src/wsl2-linux-iograph
~/build/wsl2-iograph
/mnt/c/.../wsl-kernels/io_graph
```

The base Linux clone must remain pristine. Apply `scripts/apply_linux_overlay.sh`
only to a disposable clean worktree or disposable clone. The script refuses a
dirty tree, checks `kernel/linux.integration.patch` before copying overlay
files, applies the patch, and then runs `git diff --check`.

WSL `.wslconfig` is a global WSL2 VM setting. A custom kernel configured there
affects every WSL2 distribution, not only the shell used to build or run the
benchmark. Store boot artifacts and config snippets separately, switch them
explicitly for measurement, and restore the stock config after the run.

For each kernel run record:

- kernel commit or patch stack;
- compiler and config fragment;
- CPU and PMU availability;
- map verifier rejection tests;
- selftest result;
- `bpf_iograph_run` latency and cycles;
- BPF baseline verifier/JIT/load timing;
- ringbuf bytes/sec, reserve failures, drops, and userspace CPU for the
  prefilter demo.
- `perf_event_open` branch/cache/L1/LLC counters when the host exposes the PMU.

Keep raw logs next to each run and summarize the matrix in `docs/RESULTS.md`.

References:

- https://learn.microsoft.com/en-us/windows/wsl/wsl-config
- https://github.com/microsoft/WSL2-Linux-Kernel
- https://man7.org/linux/man-pages/man2/perf_event_open.2.html
