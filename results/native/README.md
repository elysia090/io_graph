# Native Kernel Results

Do not paste userspace benchmark numbers here as kernel measurements.

Keep `current.md` honest about the latest overlay/build validation even when
the booted kernel cannot run the new map type yet.

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
